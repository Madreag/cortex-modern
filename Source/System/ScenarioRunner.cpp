#include "ScenarioRunner.h"
#include "Actor.h"
#include "ActivityMan.h"
#include "AudioMan.h"
#include "LuaMan.h"

#include "Constants.h"
#include "ConsoleMan.h"
#include "ControllerLog.h"
#include "FrameMan.h"
#include "MetricsCollector.h"
#include "MovableMan.h"
#include "MovableObject.h"
#include "NetActorOwnership.h"
#include "NetMatchReplay.h"
#include "RTETools.h"
#include "SettingsMan.h"
#include "TimerMan.h"
#include "WindowMan.h"

#include "GUI.h"
#include "AllegroBitmap.h"
#include "RenderTarget.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace RTE {

	namespace {
		bool s_Active = false;
		ScenarioRunner::Args s_Args;
		std::unique_ptr<ControllerLog> s_ControllerRecordLog;
		std::unique_ptr<ControllerLog> s_ControllerReplayLog;
		std::string s_ControllerReplayError;
		NetLockstepCoordinator* s_LockstepCoordinator = nullptr;
		std::vector<NetGameCommand> s_PendingLocalGameCommands;
		std::map<int64_t, uint8_t> s_LockstepControlOverrides; //!< Synced per-actor control handoffs (co-op shared teams).
		bool s_LockstepStallOverlayEnabled = false;
		bool s_LockstepPaused = false;
		int s_LockstepResumeCountdown = -1;
		NetMatchReplayWriter s_ReplayWriter;
		NetMatchReplayReader s_ReplayReader;
		std::string s_ReplayRecordArmedPath;
		int s_ReplayRecordRound = 0;
		uint64_t s_ReplayRewindFrom = 0; //!< Fidelity gate: keep copies of records in [from, from+count).
		uint64_t s_ReplayRewindCount = 0;
		std::deque<NetLockstepFrame> s_ReplayRewindKeep; //!< Records kept during the first pass.
		std::deque<NetLockstepFrame> s_ReplayRewindBuffer; //!< The kept window, re-fed on the re-run.
		std::deque<NetLockstepFrame> s_ReplayLookahead; //!< Records read ahead for the local-actor preview, consumed in order.
		bool s_ReplayLookaheadFailed = false; //!< A read-ahead hit the stream's end or a bad record; the feed reports it when it gets there.
		bool s_ReplayLookaheadEof = false;
		std::string s_ReplayLookaheadError;
		ScenarioRunner::LockstepReplayOutcome s_ReplayOutcome = ScenarioRunner::LockstepReplayOutcome::None;
		uint64_t s_ReplayFramesConsumed = 0;
		uint64_t s_ReplayLastTick = 0;
		bool s_ReplayEndMarkerSeen = false;
		uint64_t s_ReplayRecordFrames = 0;
		bool s_ReplayRecordClosed = false;

		// Reads the next record: the read-ahead first, then the file. A read-ahead failure is replayed
		// here so the feed classifies it at the tick it belongs to.
		bool NextReplayRecord(NetLockstepFrame& outRecord, NetReplayReadStatus& outStatus, std::string* error) {
			if (!s_ReplayLookahead.empty()) {
				outRecord = std::move(s_ReplayLookahead.front());
				s_ReplayLookahead.pop_front();
				outStatus = NetReplayReadStatus::Frame;
				return true;
			}
			if (s_ReplayLookaheadFailed) {
				outStatus = s_ReplayLookaheadEof ? NetReplayReadStatus::CleanEnd : s_ReplayReader.GetLastReadStatus();
				if (error) *error = s_ReplayLookaheadError;
				return false;
			}
			bool eof = false;
			std::string readError;
			if (s_ReplayReader.ReadFrame(outRecord, eof, &readError)) {
				outStatus = NetReplayReadStatus::Frame;
				return true;
			}
			outStatus = s_ReplayReader.GetLastReadStatus();
			if (error) *error = readError;
			return false;
		}
		bool s_SimSettingsPinned = false;
		bool s_SavedAutomaticGoldDeposit = true;
		bool s_SavedCrabBombsEnabled = false;
		int s_SavedCrabBombThreshold = 42;

		// Presentation only: the sim thread is blocked waiting on the peer, so the normal render path
		// can't run. Keep the window pumped and show the last frame replaced by a plain wait screen.
		void DrawLockstepStallOverlay(uint32_t stallMs, uint32_t graceMs, const std::string& waitingOn) {
			SDL_PumpEvents();
			BITMAP* backbuffer = g_FrameMan.GetBackBuffer32();
			GUIFont* largeFont = g_FrameMan.GetLargeFont();
			GUIFont* smallFont = g_FrameMan.GetSmallFont();
			if (!backbuffer || !largeFont || !smallFont) {
				return;
			}
			clear_to_color(backbuffer, 0);
			AllegroBitmap drawBitmap(backbuffer);
			const int centerX = backbuffer->w / 2;
			const int centerY = backbuffer->h / 2;
			const std::string who = waitingOn.empty() ? "the other player" : waitingOn;
			largeFont->DrawAligned(&drawBitmap, centerX, centerY - 12, "Waiting for " + who + "... " + std::to_string(stallMs / 1000) + "s", GUIFont::Centre);
			if (graceMs > stallMs) {
				smallFont->DrawAligned(&drawBitmap, centerX, centerY + 8, "The match ends in " + std::to_string((graceMs - stallMs + 999) / 1000) + "s if they do not return", GUIFont::Centre);
			}
			g_WindowMan.ClearBackbuffer(false);
			g_WindowMan.GetScreenBuffer()->Begin();
			g_WindowMan.UploadFrame();
			static bool s_LoggedOnce = false;
			if (!s_LoggedOnce) {
				s_LoggedOnce = true;
				std::cout << "[net-match] stall overlay drawn" << std::endl;
			}
		}

		std::string FloatBitsHex(float value) {
			uint32_t bits;
			std::memcpy(&bits, &value, sizeof(bits));
			std::ostringstream oss;
			oss << "0x" << std::hex << std::setw(8) << std::setfill('0') << bits;
			return oss.str();
		}

		std::vector<std::pair<uint64_t, uint64_t>> ParseTickRanges(const std::string& spec) {
			std::vector<std::pair<uint64_t, uint64_t>> ranges;
			std::stringstream ss(spec);
			std::string token;
			while (std::getline(ss, token, ',')) {
				if (token.empty()) {
					continue;
				}
				const size_t dash = token.find('-');
				uint64_t first = 0;
				uint64_t last = 0;
				if (dash == std::string::npos) {
					first = last = static_cast<uint64_t>(std::strtoull(token.c_str(), nullptr, 10));
				} else {
					first = static_cast<uint64_t>(std::strtoull(token.substr(0, dash).c_str(), nullptr, 10));
					last = static_cast<uint64_t>(std::strtoull(token.substr(dash + 1).c_str(), nullptr, 10));
					if (last < first) {
						std::swap(first, last);
					}
				}
				ranges.emplace_back(first, last);
			}
			return ranges;
		}
	}

	bool ScenarioRunner::IsActive() { return s_Active; }
	void ScenarioRunner::SetActive(bool active) { s_Active = active; }
	const ScenarioRunner::Args& ScenarioRunner::GetArgs() { return s_Args; }

	int ScenarioRunner::ParseArgs(int argCount, char** argValue, int startIndex) {
		if (startIndex >= argCount) {
			return 0;
		}
		const std::string a = argValue[startIndex];
		const bool hasValue = (startIndex + 1) < argCount;
		if (a == "-scenario" && hasValue) {
			s_Args.scenario = argValue[startIndex + 1];
			s_Active = true;
			return 2;
		}
		if (a == "-test-script" && hasValue) {
			s_Args.testScript = argValue[startIndex + 1];
			return 2;
		}
		if (a == "-contract-audit-continue-through" && hasValue) {
			s_Args.contractAuditContinueThrough = static_cast<uint64_t>(std::strtoull(argValue[startIndex + 1], nullptr, 10));
			return 2;
		}
		if (a == "-contract-audit-seed-marker" && hasValue) {
			s_Args.contractAuditSeedMarker = std::strtoll(argValue[startIndex + 1], nullptr, 10);
			return 2;
		}
		if (a == "-contract-audit-continuation-perturb") {
			s_Args.contractAuditContinuationPerturb = true;
			return 1;
		}
		if (a == "-out" && hasValue) {
			s_Args.outPath = argValue[startIndex + 1];
			return 2;
		}
		if (a == "-seed" && hasValue) {
			s_Args.seed = static_cast<uint64_t>(std::strtoull(argValue[startIndex + 1], nullptr, 10));
			return 2;
		}
		if (a == "-max-ticks" && hasValue) {
			s_Args.maxTicks = static_cast<uint64_t>(std::strtoull(argValue[startIndex + 1], nullptr, 10));
			return 2;
		}
		if (a == "-num-lua-states" && hasValue) {
			s_Args.numLuaStates = static_cast<int>(std::strtol(argValue[startIndex + 1], nullptr, 10));
			return 2;
		}
		if (a == "-tick-hashes") {
			// Turn on per-tick hash trace recording. Boolean flag — no value.
			s_Args.tickHashes = true;
			return 1;
		}
		if (a == "-controller-log-out" && hasValue) {
			s_Args.controllerLogOutPath = argValue[startIndex + 1];
			return 2;
		}
		if (a == "-controller-log-in" && hasValue) {
			s_Args.controllerLogInPath = argValue[startIndex + 1];
			return 2;
		}
		if (a == "-controller-log-canonicalize") {
			s_Args.controllerLogCanonicalize = true;
			return 1;
		}
		if (a == "-controller-log-no-canonicalize") {
			s_Args.controllerLogCanonicalize = false;
			return 1;
		}
		if (a == "-controller-replay-strict") {
			s_Args.controllerReplayStrict = true;
			return 1;
		}
		if (a == "-controller-log-allow-nls-mismatch") {
			s_Args.controllerLogAllowNlsMismatch = true;
			return 1;
		}
		if (a == "-controller-debug-dump" && hasValue) {
			s_Args.controllerDebugDumpPath = argValue[startIndex + 1];
			return 2;
		}
		if (a == "-controller-debug-ticks" && hasValue) {
			s_Args.controllerDebugDumpTicks = ParseTickRanges(argValue[startIndex + 1]);
			return 2;
		}
		if (a == "-determinism-selftest-perturb") {
			// Positive-control: arm the one-shot perturbation. Boolean flag — no value.
			s_Args.selftestPerturb = true;
			return 1;
		}
		if (a == "-net-match-e2e-funds-command") {
			// Arm the host-issued funds command. Boolean flag.
			s_Args.selftestFundsCommand = true;
			return 1;
		}
		if (a == "-net-match-e2e-spawn-command") {
			// Arm the host-issued spawn command. Boolean flag.
			s_Args.selftestSpawnCommand = true;
			return 1;
		}
		if (a == "-net-match-e2e-deliver-command") {
			// Arm the host-issued delivery command. Boolean flag.
			s_Args.selftestDeliverCommand = true;
			return 1;
		}
		if (a == "-net-match-e2e-ai-order-command") {
			s_Args.selftestAIOrderCommand = true;
			return 1;
		}
		if (a == "-net-match-e2e-scuttle-command") {
			// Arm the host-issued scuttle command. Boolean flag.
			s_Args.selftestScuttleCommand = true;
			return 1;
		}
		if (a == "-net-match-e2e-brain-kill-command") {
			// Arm the host-issued brain-kill delivery. Boolean flag.
			s_Args.selftestBrainKillCommand = true;
			return 1;
		}
		if (a == "-net-match-e2e-stall") {
			// Arm the one-shot 8s frame stall. Boolean flag.
			s_Args.selftestStall = true;
			return 1;
		}
		if (a == "-free-run-sim") {
			s_Args.freeRunSim = true;
			return 1;
		}
		if (a == "-net-match-e2e-join-rejection") {
			s_Args.selftestJoinRejection = true;
			return 1;
		}
		if (a == "-script-graph-selftest") {
			s_Args.scriptGraphSelfTest = true;
			return 1;
		}
		if (a == "-net-match-e2e-rematch") {
			// Arm the return-to-lobby rematch ride-through. Boolean flag.
			s_Args.selftestRematch = true;
			return 1;
		}
		if (a == "-net-match-e2e-leave") {
			// Arm the one-shot quit-to-menu at tick 300. Boolean flag.
			s_Args.selftestLeave = true;
			return 1;
		}
		if (a == "-net-match-e2e-snapshot") {
			// Arm the full-game save at tick 300 (both peers pass it: same synced frame). Boolean flag.
			s_Args.selftestSnapshot = true;
			return 1;
		}
		if (a == "-net-match-e2e-inventory-command") {
			// Arm the host-issued inventory ops. Boolean flag.
			s_Args.selftestInventoryCommand = true;
			return 1;
		}
		if (a == "-net-match-e2e-buy-command") {
			// Arm the funds grant + real buy order through CreateDelivery. Boolean flag.
			s_Args.selftestBuyCommand = true;
			return 1;
		}
		if (a == "-net-match-e2e-brain-spawn-command") {
			// Arm the second-brain spawn for this peer's team. Boolean flag.
			s_Args.selftestBrainSpawnCommand = true;
			return 1;
		}
		if (a == "-net-match-e2e-pause-command") {
			// Arm the synced pause + unpause. Boolean flag.
			s_Args.selftestPauseCommand = true;
			return 1;
		}
		return 0;
	}

	std::string ScenarioRunner::ResolvePresetName(const std::string& shorthand) {
		// Convention: the test scenarios are registered as "Determinism <Name>". Accept either form
		// from the command line.
		if (shorthand.rfind("Determinism ", 0) == 0) {
			return shorthand;
		}
		return "Determinism " + shorthand;
	}

	bool ScenarioRunner::RunContractAuditLoad(const std::string& operation,
	    const std::function<void(const std::string&)>& observe, bool& prepared, bool& applied) {
		const auto tickBefore = g_TimerMan.GetSimUpdateCount();
		if (operation.starts_with("ordinary-load:")) {
			prepared = true;
			applied = g_ActivityMan.LoadAndLaunchGame(operation.substr(14));
			std::cout << "[contract-audit-load] entry=LoadAndLaunchGame accepted=" << applied
			          << " tick_before=" << tickBefore << " tick_after=" << g_TimerMan.GetSimUpdateCount() << std::endl;
			return true;
		}
		const bool reference = operation.starts_with("stage-reference:");
		const bool stageOnly = operation.starts_with("stage-reject:");
		const bool ordinary = operation.starts_with("stage-ordinary-reject:");
		if (!reference && !stageOnly && !ordinary) return false;
		const std::string arguments = operation.substr(reference ? 16 : stageOnly ? 13 : 22);
		const size_t separator = arguments.find(':');
		if ((!reference && (separator == std::string::npos || separator == 0 || separator + 1 == arguments.size())) || arguments.empty()) {
			prepared = applied = false;
			std::cout << "[contract-audit-stage] invalid_operation=1" << std::endl;
			return true;
		}
		const std::string validName = reference ? arguments : arguments.substr(0, separator);
		prepared = g_ActivityMan.LoadGameToRestart(validName);
		if (!prepared) {
			applied = false;
			std::cout << "[contract-audit-stage] valid_staged=0" << std::endl;
			return true;
		}
		observe("staged_before");
		bool replacementAccepted = false;
		if (!reference) {
			const std::string rejectedName = arguments.substr(separator + 1);
			replacementAccepted = stageOnly ? g_ActivityMan.LoadGameToRestart(rejectedName) : g_ActivityMan.LoadAndLaunchGame(rejectedName);
		}
		observe("staged_after");
		// A successful replacement is an unexpected negative control. Preserve its evidence
		// rather than repairing the candidate or pretending that a refusal was exercised.
		const bool retainedLaunched = !replacementAccepted && g_ActivityMan.RestartActivity();
		applied = retainedLaunched;
		std::cout << "[contract-audit-stage] valid_staged=1 replacement_attempted=" << !reference
		          << " replacement_accepted=" << replacementAccepted << " retained_launched=" << retainedLaunched
		          << " entry=" << (ordinary ? "LoadAndLaunchGame" : reference ? "reference" : "LoadGameToRestart")
		          << " tick_before=" << tickBefore << " tick_after=" << g_TimerMan.GetSimUpdateCount() << std::endl;
		return true;
	}

	void ScenarioRunner::SetContractAuditSeedMarker() {
		if (!s_Args.contractAuditSeedMarker) return;
		const std::string script = "_ContractAuditCandidateMarker = " + std::to_string(s_Args.contractAuditSeedMarker);
		for (size_t index = 0; index <= g_LuaMan.GetThreadedScriptStates().size(); ++index) {
			g_LuaMan.GetStateByIndex(static_cast<int>(index)).RunScriptString(script);
		}
		std::cout << "[contract-audit-marker] seeded=" << s_Args.contractAuditSeedMarker << std::endl;
	}

	void ScenarioRunner::PerturbContractAuditContinuation() {
		if (!s_Args.contractAuditContinuationPerturb) return;
		g_SimRNG.RandomNum<uint32_t>();
		std::cout << "[contract-audit-continuation] deliberate_rng_draw=1" << std::endl;
	}

	int ScenarioRunner::FinalizeAndGetExitCode() {
		if (!s_Active) {
			return 0;
		}

		const auto currentRun = g_MetricsCollector.GetCurrentRun();
		int exitCode = currentRun.passed ? 0 : 1;

		if (s_ControllerRecordLog && !s_Args.controllerLogOutPath.empty()) {
			std::string error;
			if (!s_ControllerRecordLog->Write(s_Args.controllerLogOutPath, &error)) {
				std::cerr << "[scenario] failed to write controller log: " << error << std::endl;
				exitCode = 1;
			} else {
				std::cout << "[scenario] wrote controller log: " << s_Args.controllerLogOutPath << std::endl;
			}
		}

		if (!s_Args.outPath.empty()) {
			if (!g_MetricsCollector.WriteReport(s_Args.outPath)) {
				std::cerr << "[scenario] failed to write report to " << s_Args.outPath << std::endl;
				return 1;
			}
			std::cout << "[scenario] wrote report: " << s_Args.outPath << std::endl;
		}

		std::cout << "[scenario] " << currentRun.scenario
		          << " passed=" << (currentRun.passed ? "yes" : "no")
		          << " ticks=" << currentRun.ticks << std::endl;

		if (HasControllerReplayError()) {
			exitCode = 1;
		}
		return exitCode;
	}

	bool ScenarioRunner::PrepareControllerLog(std::string* error) {
		s_ControllerReplayError.clear();
		s_ControllerRecordLog.reset();
		s_ControllerReplayLog.reset();

		if (!s_Args.controllerLogOutPath.empty() && !s_Args.controllerLogInPath.empty()) {
			if (error) *error = "cannot record and replay a ControllerLog in the same run.";
			return false;
		}

		if (!s_Args.controllerLogOutPath.empty()) {
			s_ControllerRecordLog = std::make_unique<ControllerLog>();
			s_ControllerRecordLog->metadata.scenario = ResolvePresetName(s_Args.scenario);
			s_ControllerRecordLog->metadata.commit = "unknown";
			s_ControllerRecordLog->metadata.seed = s_Args.seed;
			s_ControllerRecordLog->metadata.maxTicks = s_Args.maxTicks;
			s_ControllerRecordLog->metadata.numLuaStates = s_Args.numLuaStates;
			s_ControllerRecordLog->metadata.simConfig = GatherSimConfig();
		}

		if (!s_Args.controllerLogInPath.empty()) {
			s_ControllerReplayLog = std::make_unique<ControllerLog>();
			std::string readError;
			if (!s_ControllerReplayLog->Read(s_Args.controllerLogInPath, &readError)) {
				if (error) *error = readError;
				return false;
			}
			const std::string expectedScenario = ResolvePresetName(s_Args.scenario);
			if (!s_ControllerReplayLog->metadata.scenario.empty() && s_ControllerReplayLog->metadata.scenario != expectedScenario) {
				if (error) *error = "controller log scenario mismatch: expected " + expectedScenario + ", got " + s_ControllerReplayLog->metadata.scenario;
				return false;
			}
			if (s_ControllerReplayLog->metadata.seed != 0 && s_Args.seed != 0 && s_ControllerReplayLog->metadata.seed != s_Args.seed) {
				if (error) *error = "controller log seed metadata mismatch.";
				return false;
			}
			if (!s_Args.controllerLogAllowNlsMismatch &&
			    s_ControllerReplayLog->metadata.numLuaStates >= 0 &&
			    s_Args.numLuaStates >= 0 &&
			    s_ControllerReplayLog->metadata.numLuaStates != s_Args.numLuaStates) {
				if (error) *error = "controller log num-lua-states mismatch.";
				return false;
			}
			if (!s_ControllerReplayLog->metadata.simConfig.empty() && s_ControllerReplayLog->metadata.simConfig != GatherSimConfig()) {
				if (error) *error = "controller log sim config mismatch.";
				return false;
			}
		}
		return true;
	}

	bool ScenarioRunner::IsControllerLogRecording() {
		return s_ControllerRecordLog != nullptr;
	}

	bool ScenarioRunner::IsControllerLogReplaying() {
		return s_ControllerReplayLog != nullptr;
	}

	bool ScenarioRunner::ShouldCanonicalizeControllerLog() {
		return IsControllerLogRecording() && s_Args.controllerLogCanonicalize;
	}

	bool ScenarioRunner::IsControllerReplayStrict() {
		return IsControllerLogReplaying() && s_Args.controllerReplayStrict;
	}

	bool ScenarioRunner::IsControllerDebugDumpEnabled() {
		return !s_Args.controllerDebugDumpPath.empty();
	}

	bool ScenarioRunner::ShouldControllerDebugDumpTick(uint64_t tick) {
		if (!IsControllerDebugDumpEnabled()) {
			return false;
		}
		if (s_Args.controllerDebugDumpTicks.empty()) {
			return true;
		}
		for (const auto& [first, last]: s_Args.controllerDebugDumpTicks) {
			if (tick >= first && tick <= last) {
				return true;
			}
		}
		return false;
	}

	const std::string& ScenarioRunner::GetControllerDebugDumpPath() {
		return s_Args.controllerDebugDumpPath;
	}

	void ScenarioRunner::RecordControllerFrames(uint64_t tick, std::vector<ControllerFrame> frames) {
		if (s_ControllerRecordLog) {
			s_ControllerRecordLog->AddTick(tick, std::move(frames));
		}
	}

	bool ScenarioRunner::GetReplayControllerFrames(uint64_t tick, std::vector<ControllerFrame>& outFrames, std::string* error) {
		if (!s_ControllerReplayLog) {
			if (error) *error = "controller replay log is not loaded.";
			return false;
		}
		const ControllerLogTick* rec = s_ControllerReplayLog->FindTick(tick);
		if (!rec) {
			if (error) *error = "controller replay log has no frame for tick " + std::to_string(tick);
			return false;
		}
		outFrames = rec->frames;
		return true;
	}

	void ScenarioRunner::SetControllerReplayError(const std::string& error) {
		s_ControllerReplayError = error;
		g_MetricsCollector.RecordString("controller_replay_error", error);
		g_MetricsCollector.SetResult(false);
	}

	void ScenarioRunner::ClearControllerReplayError() {
		s_ControllerReplayError.clear();
	}

	bool ScenarioRunner::HasControllerReplayError() {
		return !s_ControllerReplayError.empty();
	}

	const std::string& ScenarioRunner::GetControllerReplayError() {
		return s_ControllerReplayError;
	}

	void ScenarioRunner::SetLockstepCoordinator(NetLockstepCoordinator* coordinator) {
		s_LockstepCoordinator = coordinator;
		s_LockstepControlOverrides.clear();
		// A coordinator handoff ends any synced pause; the next match must not inherit a frozen clock.
		// Touch the timer singleton only when actually frozen — selftests run this before manager init.
		if (s_LockstepPaused) {
			s_LockstepPaused = false;
			g_TimerMan.SetSimTimeFrozen(false);
		}
		s_LockstepResumeCountdown = -1;
		// A closing session hands the user's per-machine sim settings back.
		if (!coordinator && s_SimSettingsPinned) {
			s_SimSettingsPinned = false;
			g_SettingsMan.SetAutomaticGoldDeposit(s_SavedAutomaticGoldDeposit);
			g_SettingsMan.SetCrabBombsEnabled(s_SavedCrabBombsEnabled);
			g_SettingsMan.SetCrabBombThreshold(s_SavedCrabBombThreshold);
		}
		if (!coordinator) {
			CloseLockstepReplayRecord();
		}
	}

	bool ScenarioRunner::IsLockstepControllerSyncActive() {
		return s_LockstepCoordinator && s_LockstepCoordinator->IsRunning();
	}

	bool ScenarioRunner::FinishLockstepSimulationTick(uint64_t completedTick) {
		return s_LockstepCoordinator && s_LockstepCoordinator->FinishSimulationTick(completedTick);
	}

	bool ScenarioRunner::HasLockstepCoordinator() {
		return s_LockstepCoordinator != nullptr;
	}

	std::string ScenarioRunner::GetLockstepStopReason() {
		return s_LockstepCoordinator ? s_LockstepCoordinator->GetStats().timeoutReason : std::string();
	}

	bool ScenarioRunner::IsLockstepLocalActor(int64_t actorUniqueID, int actorTeam, bool cpuControlled) {
		// Playback owns no actor: the file drives everything through the remote apply.
		if (s_ReplayReader.IsOpen()) {
			return false;
		}
		if (!s_LockstepCoordinator) {
			return true;
		}
		// A synced control handoff overrides the per-team policy for that actor.
		const auto overrideIt = s_LockstepControlOverrides.find(actorUniqueID);
		if (overrideIt != s_LockstepControlOverrides.end()) {
			return overrideIt->second == s_LockstepCoordinator->GetConfig().localPeerId;
		}
		return s_LockstepCoordinator->IsLocalActor(actorUniqueID, actorTeam, cpuControlled);
	}

	bool ScenarioRunner::IsLockstepActorOwner(int64_t actorUniqueID, int actorTeam, bool cpuControlled, uint8_t peerId) {
		if (!s_LockstepCoordinator) {
			return true;
		}
		const auto overrideIt = s_LockstepControlOverrides.find(actorUniqueID);
		if (overrideIt != s_LockstepControlOverrides.end()) {
			return overrideIt->second == peerId;
		}
		return s_LockstepCoordinator->ResolveActorOwner(actorUniqueID, actorTeam, cpuControlled) == peerId;
	}

	uint8_t ScenarioRunner::GetLockstepActorOwner(int64_t actorUniqueID, int actorTeam, bool cpuControlled) {
		if (!s_LockstepCoordinator) {
			return 0;
		}
		const auto overrideIt = s_LockstepControlOverrides.find(actorUniqueID);
		if (overrideIt != s_LockstepControlOverrides.end()) {
			return overrideIt->second;
		}
		return s_LockstepCoordinator->ResolveActorOwner(actorUniqueID, actorTeam, cpuControlled);
	}

	uint8_t ScenarioRunner::GetLockstepHostPeerId() {
		return s_LockstepCoordinator ? s_LockstepCoordinator->GetConfig().matchConfig.hostPeerId : 0;
	}

	uint8_t ScenarioRunner::ResolveTeamCommandAuthority(int team) {
		return s_LockstepCoordinator ? s_LockstepCoordinator->ResolveTeamCommandAuthority(team) : 0;
	}

	void ScenarioRunner::SetLockstepControlOverride(int64_t actorUniqueID, uint8_t ownerPeerId) {
		s_LockstepControlOverrides[actorUniqueID] = ownerPeerId;
	}

	void ScenarioRunner::PurgeLockstepControlOverridesForGonePeers(uint64_t frame) {
		if (!s_LockstepCoordinator || s_LockstepControlOverrides.empty()) {
			return;
		}
		for (auto it = s_LockstepControlOverrides.begin(); it != s_LockstepControlOverrides.end();) {
			if (s_LockstepCoordinator->IsPeerGoneAtFrame(it->second, frame)) {
				it = s_LockstepControlOverrides.erase(it);
			} else {
				++it;
			}
		}
	}

	bool ScenarioRunner::IsLockstepTeamCommandSender(int team, uint8_t senderPeerId) {
		if (!s_LockstepCoordinator || team < 0) {
			return true;
		}
		return NetActorOwnership::IsTeamCommandAuthority(s_LockstepCoordinator->GetConfig().matchConfig, static_cast<uint8_t>(team), senderPeerId);
	}

	int ScenarioRunner::GetLockstepHumanSlotIndex(int team) {
		if (!s_LockstepCoordinator || team < 0) {
			return -1;
		}
		const uint8_t localPeerId = s_LockstepCoordinator->GetConfig().localPeerId;
		int slotIndex = 0;
		for (const NetMatchPlayerSlot& slot: s_LockstepCoordinator->GetConfig().matchConfig.players) {
			if (slot.cpu || static_cast<int>(slot.team) != team) {
				continue;
			}
			if (slot.peerId == localPeerId) {
				return slotIndex;
			}
			++slotIndex;
		}
		return -1;
	}

	bool ScenarioRunner::SubmitLockstepChecksum(uint64_t tick, const std::array<uint8_t, 32>& hash) {
		return s_LockstepCoordinator && s_LockstepCoordinator->SubmitLocalChecksum(tick, hash);
	}

	uint16_t ScenarioRunner::GetLockstepInputDelayFrames() {
		return s_LockstepCoordinator ? s_LockstepCoordinator->GetConfig().inputDelayFrames : 0;
	}

	uint8_t ScenarioRunner::GetLockstepLocalPeerId() {
		return s_LockstepCoordinator ? s_LockstepCoordinator->GetConfig().localPeerId : 0;
	}

	bool ScenarioRunner::IsLockstepPaused() {
		return s_LockstepPaused;
	}

	int ScenarioRunner::GetLockstepResumeCountdown() {
		return s_LockstepResumeCountdown;
	}

	void ScenarioRunner::ApplyLockstepPauseCommand(bool pause) {
		if (pause && !s_LockstepPaused) {
			s_LockstepPaused = true;
			s_LockstepResumeCountdown = -1;
			g_TimerMan.SetSimTimeFrozen(true);
			const std::string line = "match paused at tick " + std::to_string(g_TimerMan.GetSimUpdateCount()) + " sim ms " + std::to_string(g_TimerMan.GetSimTimeMS());
			g_ConsoleMan.PrintString("NETWORK: " + line);
			std::cout << "[net-match] " << line << std::endl;
		} else if (!pause && s_LockstepPaused && s_LockstepResumeCountdown < 0) {
			s_LockstepResumeCountdown = static_cast<int>(3.0F / c_DefaultDeltaTimeS + 0.5F);
			const std::string line = "match resuming in " + std::to_string(s_LockstepResumeCountdown) + " ticks";
			g_ConsoleMan.PrintString("NETWORK: " + line);
			std::cout << "[net-match] " << line << std::endl;
		}
	}

	void ScenarioRunner::AdvanceLockstepPausedTick() {
		if (!s_LockstepPaused || s_LockstepResumeCountdown <= 0) {
			return;
		}
		if (--s_LockstepResumeCountdown == 0) {
			s_LockstepPaused = false;
			s_LockstepResumeCountdown = -1;
			g_TimerMan.SetSimTimeFrozen(false);
			const std::string line = "match resumed at tick " + std::to_string(g_TimerMan.GetSimUpdateCount()) + " sim ms " + std::to_string(g_TimerMan.GetSimTimeMS());
			g_ConsoleMan.PrintString("NETWORK: " + line);
			std::cout << "[net-match] " << line << std::endl;
		}
	}

	bool ScenarioRunner::IsLockstepHumanTeam(int team) {
		if (!s_LockstepCoordinator) {
			return false;
		}
		for (const NetMatchPlayerSlot& slot: s_LockstepCoordinator->GetConfig().matchConfig.players) {
			if (static_cast<int>(slot.team) == team && !slot.cpu) {
				return true;
			}
		}
		return false;
	}

	bool ScenarioRunner::IsLockstepActiveTeam(int team) {
		if (!s_LockstepCoordinator) {
			return false;
		}
		for (const NetMatchPlayerSlot& slot: s_LockstepCoordinator->GetConfig().matchConfig.players) {
			if (static_cast<int>(slot.team) == team) {
				return true;
			}
		}
		return false;
	}

	bool ScenarioRunner::IsLockstepHoldingSeatForReclaim() {
		if (!s_LockstepCoordinator) {
			return false;
		}
		// A round that has committed nothing yet is still resuming: after a resync relaunch the ledgered
		// reseat rides its first committed frame, and the activity update runs before MovableMan applies
		// it, so the first evaluation a match may be judged on is the one after that frame lands.
		return s_LockstepCoordinator->IsHoldingSeatForReclaim() ||
		       (s_LockstepCoordinator->IsRunning() && !s_LockstepCoordinator->HasCommittedAFrame());
	}

	bool ScenarioRunner::IsLockstepActorOwnerGone(int64_t actorUniqueID, int actorTeam, bool cpuControlled, uint64_t frame) {
		if (!s_LockstepCoordinator) {
			return false;
		}
		// A control handoff resolves the owner just like production does; the purge normally erases
		// gone owners' handoffs first, so this branch mostly answers for live takers.
		const auto overrideIt = s_LockstepControlOverrides.find(actorUniqueID);
		if (overrideIt != s_LockstepControlOverrides.end()) {
			return s_LockstepCoordinator->IsPeerGoneAtFrame(overrideIt->second, frame);
		}
		return s_LockstepCoordinator->IsActorOwnerGone(actorUniqueID, actorTeam, cpuControlled, frame);
	}

	bool ScenarioRunner::QueueLockstepLocalControllerFrames(uint64_t tick, std::vector<ControllerFrame> frames, std::string* error) {
		if (!s_LockstepCoordinator) {
			if (error) *error = "lockstep coordinator is not active";
			return false;
		}
		// Playback: the sampled frames are discarded and the recording's committed tick applies
		// instead — sender ids preserved so command authority resolves as it did live.
		if (s_ReplayReader.IsOpen()) {
			(void)DrainLocalGameCommands();
			// A tick before the recording's first frame free-runs with empty input (the wait's
			// priming path), mirroring how the live match ran it.
			if (tick < s_ReplayReader.GetStartFrame()) {
				return true;
			}
			// The fidelity gate's re-run consumes the buffered window before the reader resumes.
			if (!s_ReplayRewindBuffer.empty() && s_ReplayRewindBuffer.front().targetFrame == tick) {
				NetLockstepFrame buffered = s_ReplayRewindBuffer.front();
				s_ReplayRewindBuffer.pop_front();
				return s_LockstepCoordinator->QueueReplayFrame(tick, std::move(buffered.frames), std::move(buffered.commands), error, std::move(buffered.observations));
			}
			NetLockstepFrame record;
			NetReplayReadStatus status = NetReplayReadStatus::None;
			std::string readError;
			if (!NextReplayRecord(record, status, &readError)) {
				if (status == NetReplayReadStatus::CleanEnd) {
					s_ReplayEndMarkerSeen = s_ReplayReader.GetVersion() >= 2;
					s_ReplayOutcome = LockstepReplayOutcome::Completed;
					s_LockstepCoordinator->Complete("replay ended");
					if (error) *error = s_LockstepCoordinator->GetStats().timeoutReason;
					return false;
				}
				s_ReplayOutcome = status == NetReplayReadStatus::Truncated ? LockstepReplayOutcome::Truncated : LockstepReplayOutcome::Corrupt;
				if (error) *error = std::string(status == NetReplayReadStatus::Truncated ? "replay truncated at tick " : "replay corrupt at tick ") + std::to_string(tick) + ": " + readError;
				return false;
			}
			if (record.targetFrame != tick) {
				s_ReplayOutcome = LockstepReplayOutcome::Corrupt;
				if (error) *error = "replay record frame " + std::to_string(record.targetFrame) + " does not match tick " + std::to_string(tick);
				return false;
			}
			++s_ReplayFramesConsumed;
			s_ReplayLastTick = record.targetFrame;
			if (record.targetFrame >= s_ReplayRewindFrom && record.targetFrame < s_ReplayRewindFrom + s_ReplayRewindCount) {
				s_ReplayRewindKeep.push_back(record);
			}
			return s_LockstepCoordinator->QueueReplayFrame(tick, std::move(record.frames), std::move(record.commands), error, std::move(record.observations));
		}
		// The input boundary also carries this peer's actual audibility of the shared sounds it answers for.
		return s_LockstepCoordinator->QueueLocalInput(tick, frames, DrainLocalGameCommands(), error, g_AudioMan.SampleSoundObservations());
	}

	bool ScenarioRunner::PeekLockstepLocalControllerFrames(uint64_t tick, std::vector<ControllerFrame>& outFrames) {
		if (!s_LockstepCoordinator || !s_LockstepCoordinator->IsRunning()) {
			return false;
		}
		if (!s_ReplayReader.IsOpen()) {
			return s_LockstepCoordinator->PeekLocalFrames(tick, outFrames);
		}
		// Playback: read ahead to the tick and hand back the recorded frames of the actors this
		// peer owns, so the preview runs the same inputs the canonical tick will.
		const NetLockstepFrame* found = nullptr;
		for (const NetLockstepFrame& buffered: s_ReplayRewindBuffer) {
			if (buffered.targetFrame == tick) {
				found = &buffered;
				break;
			}
		}
		while (!found && (s_ReplayLookahead.empty() || s_ReplayLookahead.back().targetFrame < tick)) {
			if (s_ReplayLookaheadFailed) {
				return false;
			}
			NetLockstepFrame record;
			bool eof = false;
			std::string readError;
			if (!s_ReplayReader.ReadFrame(record, eof, &readError)) {
				s_ReplayLookaheadFailed = true;
				s_ReplayLookaheadEof = eof;
				s_ReplayLookaheadError = readError;
				return false;
			}
			s_ReplayLookahead.push_back(std::move(record));
		}
		if (!found) {
			for (const NetLockstepFrame& record: s_ReplayLookahead) {
				if (record.targetFrame == tick) {
					found = &record;
					break;
				}
			}
		}
		if (!found) {
			return false;
		}
		// Every recorded frame rides; the preview applies only the ones addressed to its clones.
		outFrames = found->frames;
		return true;
	}

	uint16_t ScenarioRunner::GetLockstepLocalInputDelay() {
		return s_LockstepCoordinator && s_LockstepCoordinator->IsRunning() ? s_LockstepCoordinator->GetConfig().inputDelayFrames : 0;
	}

	void ScenarioRunner::EnqueueLocalGameCommand(const NetGameCommand& command) {
		if (g_MovableMan.IsSpeculative()) {
			g_MovableMan.ReportSpeculationViolation("queueing a wire command for", nullptr);
			return;
		}
		s_PendingLocalGameCommands.push_back(command);
	}

	std::vector<NetGameCommand> ScenarioRunner::DrainLocalGameCommands() {
		std::vector<NetGameCommand> drained = std::move(s_PendingLocalGameCommands);
		s_PendingLocalGameCommands.clear();
		return drained;
	}

	void ScenarioRunner::SetLockstepStallOverlayEnabled(bool enabled) {
		s_LockstepStallOverlayEnabled = enabled;
	}

	void ScenarioRunner::ArmReplayRewindBuffer(uint64_t fromFrame, uint64_t frameCount) {
		s_ReplayRewindFrom = fromFrame;
		s_ReplayRewindCount = frameCount;
		s_ReplayRewindKeep.clear();
		s_ReplayRewindBuffer.clear();
	}

	bool ScenarioRunner::RewindReplayForProbe(uint64_t firstFrame, std::string* error) {
		if (!s_LockstepCoordinator || !s_ReplayReader.IsOpen()) {
			if (error) *error = "no replay playback to rewind";
			return false;
		}
		if (s_ReplayRewindKeep.size() < s_ReplayRewindCount) {
			if (error) *error = "the rewind buffer is incomplete";
			return false;
		}
		s_ReplayRewindBuffer = std::move(s_ReplayRewindKeep);
		s_ReplayRewindKeep.clear();
		return s_LockstepCoordinator->RewindReplay(firstFrame, error);
	}

	void ScenarioRunner::ArmLockstepReplayRecord(const std::string& path) {
		s_ReplayRecordArmedPath = path;
		s_ReplayRecordRound = 0;
	}

	bool ScenarioRunner::BeginLockstepReplayRecord(const NetMatchConfig& config, std::string* error) {
		if (s_ReplayRecordArmedPath.empty()) {
			return false;
		}
		// Every launch gets its own file: a resync/rematch round must never truncate the previous
		// round's recording (a desync's recording IS the forensic evidence).
		std::string path = s_ReplayRecordArmedPath;
		if (s_ReplayRecordRound > 0) {
			path += ".r" + std::to_string(s_ReplayRecordRound);
		}
		++s_ReplayRecordRound;
		if (!s_ReplayWriter.Open(path, config, error)) {
			return false;
		}
		std::cout << "[net-match] recording the match to " << path << std::endl;
		return true;
	}

	bool ScenarioRunner::IsLockstepReplayRecording() {
		return s_ReplayWriter.IsOpen();
	}

	uint64_t ScenarioRunner::GetLockstepReplayFramesWritten() {
		return s_ReplayWriter.GetFramesWritten();
	}

	void ScenarioRunner::CloseLockstepReplayRecord() {
		if (s_ReplayWriter.IsOpen()) {
			s_ReplayRecordFrames = s_ReplayWriter.GetFramesWritten();
			s_ReplayRecordClosed = true;
			std::cout << "[net-match] replay recorded: " << s_ReplayRecordFrames << " frames" << std::endl;
		}
		s_ReplayWriter.Close();
	}

	uint64_t ScenarioRunner::GetLockstepReplayRecordFrames() {
		return s_ReplayWriter.IsOpen() ? s_ReplayWriter.GetFramesWritten() : s_ReplayRecordFrames;
	}

	bool ScenarioRunner::WasLockstepReplayRecordClosed() {
		return s_ReplayRecordClosed && !s_ReplayWriter.IsOpen();
	}

	ScenarioRunner::LockstepReplayOutcome ScenarioRunner::GetLockstepReplayOutcome() {
		return s_ReplayOutcome;
	}

	void ScenarioRunner::SetLockstepReplayOutcome(LockstepReplayOutcome outcome) {
		s_ReplayOutcome = outcome;
	}

	const char* ScenarioRunner::ReplayOutcomeName(LockstepReplayOutcome outcome) {
		switch (outcome) {
			case LockstepReplayOutcome::None: return "none";
			case LockstepReplayOutcome::Playing: return "playing";
			case LockstepReplayOutcome::Completed: return "completed";
			case LockstepReplayOutcome::TickCap: return "tick_cap";
			case LockstepReplayOutcome::Truncated: return "truncated";
			case LockstepReplayOutcome::Corrupt: return "corrupt";
			case LockstepReplayOutcome::SimFailure: return "sim_failure";
		}
		return "unknown";
	}

	uint64_t ScenarioRunner::GetLockstepReplayFramesConsumed() {
		return s_ReplayFramesConsumed;
	}

	uint64_t ScenarioRunner::GetLockstepReplayLastTick() {
		return s_ReplayLastTick;
	}

	bool ScenarioRunner::LockstepReplaySawEndMarker() {
		return s_ReplayEndMarkerSeen;
	}

	bool ScenarioRunner::SetLockstepReplaySource(const std::string& path, std::string* error) {
		s_ReplayRewindFrom = 0;
		s_ReplayRewindCount = 0;
		s_ReplayRewindKeep.clear();
		s_ReplayRewindBuffer.clear();
		s_ReplayLookahead.clear();
		s_ReplayLookaheadFailed = false;
		s_ReplayLookaheadEof = false;
		s_ReplayLookaheadError.clear();
		s_ReplayFramesConsumed = 0;
		s_ReplayLastTick = 0;
		s_ReplayEndMarkerSeen = false;
		const bool opened = s_ReplayReader.Open(path, error);
		s_ReplayOutcome = opened ? LockstepReplayOutcome::Playing : LockstepReplayOutcome::Corrupt;
		return opened;
	}

	bool ScenarioRunner::IsLockstepReplayPlayback() {
		return s_ReplayReader.IsOpen();
	}

	const NetMatchConfig& ScenarioRunner::GetLockstepReplayConfig() {
		return s_ReplayReader.GetConfig();
	}

	uint64_t ScenarioRunner::GetLockstepReplayStartFrame() {
		return s_ReplayReader.GetStartFrame();
	}

	namespace {
		long long s_LockstepWaitUs = 0;

		struct LockstepWaitTimer {
			std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
			~LockstepWaitTimer() { s_LockstepWaitUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count(); }
		};
	}

	long long ScenarioRunner::GetLockstepWaitUs() {
		return s_LockstepWaitUs;
	}

	void ScenarioRunner::ResetLockstepWaitUs() {
		s_LockstepWaitUs = 0;
	}

	bool ScenarioRunner::WaitForLockstepControllerFrame(uint64_t tick, NetLockstepReadyFrame& outFrame, std::string* error) {
		if (!s_LockstepCoordinator) {
			if (error) *error = "lockstep coordinator is not active";
			return false;
		}
		LockstepWaitTimer waitTimer;

		// Input-delay priming: with D>0 the first D frames have no committed input yet (the pipeline
		// is still filling), and the coordinator emits no ready frame before effectiveStartFrame. The
		// sim free-runs those ticks with empty input so both peers advance identically. No-op at D=0.
		if (tick < s_LockstepCoordinator->GetStats().effectiveStartFrame) {
			outFrame = NetLockstepReadyFrame{};
			outFrame.frame = tick;
			return true;
		}

		const uint32_t timeoutMs = s_LockstepCoordinator->GetConfig().timeoutMs;
		// The grace is a wall-clock budget. Feeding the coordinator a poll counter made it a count of
		// ~1ms sleeps instead, so a peer that stopped sending was waited on for far longer than the
		// configured milliseconds - long enough for the host's drop notice to arrive too late.
		const uint64_t giveUpMs = timeoutMs > 0 ? static_cast<uint64_t>(timeoutMs) + 50 : 500;
		const auto waitStart = std::chrono::steady_clock::now();
		// A sub-second wait is a normal frame exchange; only a real stall gets the marker + overlay.
		uint32_t nextOverlayMs = 1500;
		bool stalled = false;
		while (true) {
			s_LockstepCoordinator->Tick(NetLockstepNowMs());
			NetLockstepReadyFrame ready;
			while (s_LockstepCoordinator->PopReadyFrame(ready)) {
				if (ready.frame == tick) {
					if (stalled) {
						const auto stallMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count();
						std::cout << "[net-match] peer stall recovered after " << stallMs << "ms (tick " << tick << ")" << std::endl;
					}
					// The recorder captures every committed tick: all peers' frames and commands. The
					// codec wants one UID-sorted set; command order re-sorts by sender at apply.
					if (s_ReplayWriter.IsOpen()) {
						std::vector<ControllerFrame> allFrames = ready.localFrames;
						allFrames.insert(allFrames.end(), ready.remoteFrames.begin(), ready.remoteFrames.end());
						std::sort(allFrames.begin(), allFrames.end(), [](const ControllerFrame& lhs, const ControllerFrame& rhs) {
							return lhs.actorUniqueID < rhs.actorUniqueID;
						});
						std::vector<NetGameCommand> allCommands = ready.localCommands;
						allCommands.insert(allCommands.end(), ready.remoteCommands.begin(), ready.remoteCommands.end());
						std::vector<NetSoundObservation> allObservations = ready.localObservations;
						allObservations.insert(allObservations.end(), ready.remoteObservations.begin(), ready.remoteObservations.end());
						std::string writeError;
						if (!s_ReplayWriter.WriteFrame(tick, allFrames, allCommands, allObservations, &writeError)) {
							std::cout << "[net-match] replay recording stopped: " << writeError << std::endl;
							s_ReplayWriter.Close();
						}
					}
					outFrame = std::move(ready);
					return true;
				}
				if (ready.frame > tick) {
					if (error) *error = "lockstep produced future frame " + std::to_string(ready.frame) + " while waiting for " + std::to_string(tick);
					return false;
				}
			}
			if (s_LockstepCoordinator->IsFailed() || s_LockstepCoordinator->IsStopped()) {
				if (error) *error = s_LockstepCoordinator->GetStats().timeoutReason;
				return false;
			}
			const uint32_t stallMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count());
			if (stallMs >= nextOverlayMs) {
				const std::string missing = s_LockstepCoordinator->DescribeMissingPeers();
				if (!stalled) {
					stalled = true;
					std::cout << "[net-match] waiting on peer frames (tick " << tick << (missing.empty() ? "" : ", " + missing) << ")" << std::endl;
				}
				if (s_LockstepStallOverlayEnabled) {
					DrawLockstepStallOverlay(stallMs, timeoutMs, missing);
				}
				nextOverlayMs = stallMs + 200;
			}
			if (stallMs >= giveUpMs) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (error) *error = "timed out waiting for lockstep frame " + std::to_string(tick);
		return false;
	}

	void ScenarioRunner::ApplyDeterministicConfig() {
		// One canonical timestep on every machine. Settings.ini's DeltaTime is per-machine and, depending
		// on the SettingsMan/TimerMan init order, may not even apply — so pin it here, post-load, pre-sim.
		const std::string loaded = FloatBitsHex(g_TimerMan.GetDeltaTimeSecs());
		g_TimerMan.SetDeltaTimeSecs(c_DefaultDeltaTimeS);
		const std::string pinned = FloatBitsHex(g_TimerMan.GetDeltaTimeSecs());
		if (loaded != pinned) {
			std::cerr << "[scenario] pinned dt " << loaded << " -> " << pinned << std::endl;
		}
		// Pin the MO unique-ID counter too: UniqueIDs cross the wire (equip sync, scuttle commands), so
		// every deterministic match must hand out the same IDs on every peer — including a rematch, where
		// each process has created a different number of MOs by launch time. The base clears load-time IDs.
		MovableObject::PinUniqueIDCounter(1 << 20);
		// Remember the user's values so the closing session can hand them back.
		if (!s_SimSettingsPinned) {
			s_SimSettingsPinned = true;
			s_SavedAutomaticGoldDeposit = g_SettingsMan.GetAutomaticGoldDeposit();
			s_SavedCrabBombsEnabled = g_SettingsMan.CrabBombsEnabled();
			s_SavedCrabBombThreshold = g_SettingsMan.GetCrabBombThreshold();
		}
		// Gold pickups route to team funds or carried gold off this per-machine setting; pin it.
		g_SettingsMan.SetAutomaticGoldDeposit(true);
		// Crab bombs gib a craft's ejected crabs past a threshold; both are per-machine settings on a sim path.
		g_SettingsMan.SetCrabBombsEnabled(false);
		g_SettingsMan.SetCrabBombThreshold(42);
	}

	std::map<std::string, std::string> ScenarioRunner::GatherSimConfig() {
		// Sim-affecting settings that must match across machines in a deterministic run. Recorded into the
		// trace; the determinism check diffs this before the per-tick hashes, so a config mismatch reads as
		// one instead of masquerading as a sim divergence.
		std::map<std::string, std::string> config;
		config["delta_time_bits"] = FloatBitsHex(g_TimerMan.GetDeltaTimeSecs());
		config["ai_update_interval"] = std::to_string(g_SettingsMan.GetAIUpdateInterval());
		config["pathfinder_grid_node_size"] = std::to_string(g_SettingsMan.GetPathFinderGridNodeSize());
		config["recommended_moid_count"] = std::to_string(g_SettingsMan.RecommendedMOIDCount());
		config["particle_settling"] = g_MovableMan.IsParticleSettlingEnabled() ? "1" : "0";
		config["mo_subtraction"] = g_MovableMan.IsMOSubtractionEnabled() ? "1" : "0";
		config["automatic_gold_deposit"] = g_SettingsMan.GetAutomaticGoldDeposit() ? "1" : "0";
		config["crab_bombs"] = g_SettingsMan.CrabBombsEnabled() ? std::to_string(g_SettingsMan.GetCrabBombThreshold()) : "off";
		// Which sim-mutating global scripts run is per-machine Settings state; a mismatch must read
		// as a config difference, not a sim divergence.
		config["enabled_global_scripts"] = g_SettingsMan.GetEnabledGlobalScriptsCSV();
		return config;
	}

} // namespace RTE
