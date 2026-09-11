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
#include "NetReconnectUx.h"
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
#include <set>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace RTE {

	namespace {
		bool s_Active = false;
		ScenarioRunner::Args s_Args;
		std::unique_ptr<ControllerLog> s_ControllerRecordLog;
		std::unique_ptr<ControllerLog> s_ControllerReplayLog;
		std::string s_ControllerReplayError;
		NetLockstepCoordinator* s_LockstepCoordinator = nullptr;
		uint64_t s_LockstepAppliedFrame = 0;
		std::function<void()> s_SessionPump;
		const NetSeatPresence* s_SeatPresence = nullptr;
		std::vector<NetGameCommand> s_PendingLocalGameCommands;
		uint64_t s_NextLocalCommandSequence = 1;
		uint64_t s_CommandSessionId = 0;
		std::array<uint8_t, 16> s_CommandEpoch{};
		std::map<uint64_t, NetResyncPendingCommand> s_LocalCommandOutbox;
		std::map<uint64_t, std::vector<NetGameCommand>> s_RequeuedCommands;
		std::map<uint64_t, NetGamePlayerBindings> s_RequeuedPlayerBindings;
		std::map<uint64_t, NetLockstepFrame> s_RequeuedInputs;
		std::map<uint64_t, NetLockstepFrame> s_LocalInputHistory;
		std::vector<NetLockstepFrame> s_RecoveredInputs;
		std::vector<NetResyncPendingCommand> s_RecoveredCommands, s_RecoveredPlayerBindings;
		std::map<uint8_t, uint64_t> s_AppliedCommandSequences;
		std::map<uint8_t, NetResyncPlayerBindings> s_PeerPlayerBindings;
		std::vector<NetValueObservation> s_DroppedValueObservations;

		NetValueObservation ToValueObservation(const MovableObject::PendingValueOp& op) {
			NetValueObservation observation;
			observation.objectUID = op.objectUID;
			observation.tick = op.tick;
			observation.ordinal = op.ordinal;
			observation.mapKind = static_cast<uint8_t>(op.map);
			observation.key = op.key;
			observation.op = static_cast<uint8_t>(op.op);
			observation.numberValue = op.number;
			observation.stringValue = op.text;
			return observation;
		}

		std::vector<NetValueObservation> SampleValueObservations() {
			std::vector<NetValueObservation> observations = std::move(s_DroppedValueObservations);
			s_DroppedValueObservations.clear();
			for (const MovableObject::PendingValueOp& op: MovableObject::SamplePendingValueOps()) {
				observations.push_back(ToValueObservation(op));
			}
			return observations;
		}

		bool SameCommandBits(const NetGameCommand& left, const NetGameCommand& right) {
			NetLockstepFrame a, b;
			a.senderPeerId = left.senderPeerId; b.senderPeerId = right.senderPeerId;
			a.roundId = b.roundId = 1;
			a.commands.push_back(left); b.commands.push_back(right);
			std::vector<uint8_t> first, second;
			return NetLockstepCodec::EncodeRecoveryInput(a, first) && NetLockstepCodec::EncodeRecoveryInput(b, second) && first == second;
		}

		bool SameInputBits(NetLockstepFrame left, NetLockstepFrame right) {
			left.roundId = right.roundId = 1;
			std::vector<uint8_t> first, second;
			return NetLockstepCodec::EncodeRecoveryInput(left, first) && NetLockstepCodec::EncodeRecoveryInput(right, second) && first == second;
		}
		std::map<int64_t, uint8_t> s_LockstepControlOverrides; //!< Synced per-actor control handoffs (co-op shared teams).
		std::map<int64_t, uint8_t> s_LockstepDroppedControlOverrides;
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
		void DrawLockstepStallOverlay(uint32_t stallMs, uint32_t graceMs, const std::string& waitingOn, bool holdPause, const std::string& holdName, uint32_t holdSeconds) {
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
			if (holdPause) {
				const std::string who = holdName.empty() ? "a player" : holdName;
				largeFont->DrawAligned(&drawBitmap, centerX, centerY - 12, "Match paused: waiting for " + who + " to return (" + std::to_string(holdSeconds) + "s left)", GUIFont::Centre);
			} else {
				const std::string who = waitingOn.empty() ? "the other player" : waitingOn;
				largeFont->DrawAligned(&drawBitmap, centerX, centerY - 12, "Waiting for " + who + "... " + std::to_string(stallMs / 1000) + "s", GUIFont::Centre);
				if (graceMs > stallMs) {
					smallFont->DrawAligned(&drawBitmap, centerX, centerY + 8, "The match ends in " + std::to_string((graceMs - stallMs + 999) / 1000) + "s if they do not return", GUIFont::Centre);
				}
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

	void ScenarioRunner::SetSessionPump(std::function<void()> pump) {
		s_SessionPump = std::move(pump);
	}

	void ScenarioRunner::SetLockstepSeatPresence(const NetSeatPresence* presence) {
		s_SeatPresence = presence;
	}

	void ScenarioRunner::SetLockstepCoordinator(NetLockstepCoordinator* coordinator, bool preserveCommands) {
		if (!coordinator && s_LockstepCoordinator) {
			for (auto& input: s_LockstepCoordinator->CaptureLocalInputHistory()) s_LocalInputHistory[input.targetFrame] = std::move(input);
			if (!s_LocalInputHistory.empty()) {
				const uint64_t newest = s_LocalInputHistory.rbegin()->first;
				if (newest > NetLockstepCodec::c_MaxFutureFrameSkew) s_LocalInputHistory.erase(s_LocalInputHistory.begin(), s_LocalInputHistory.lower_bound(newest - NetLockstepCodec::c_MaxFutureFrameSkew));
			}
		}
		s_LockstepCoordinator = coordinator;
		if (!coordinator) {
			s_SeatPresence = nullptr;
		}
		if (coordinator && (!preserveCommands || s_CommandSessionId != coordinator->GetConfig().sessionId || s_CommandEpoch != coordinator->GetConfig().seatPresenceEpoch)) {
			s_PendingLocalGameCommands.clear();
			s_NextLocalCommandSequence = 1;
			s_LocalCommandOutbox.clear();
			s_RequeuedCommands.clear();
			s_RequeuedPlayerBindings.clear();
			s_RequeuedInputs.clear();
			s_LocalInputHistory.clear();
			s_RecoveredInputs.clear();
			s_RecoveredCommands.clear();
			s_RecoveredPlayerBindings.clear();
			s_AppliedCommandSequences.clear();
			s_PeerPlayerBindings.clear();
		}
		if (coordinator) { s_CommandSessionId = coordinator->GetConfig().sessionId; s_CommandEpoch = coordinator->GetConfig().seatPresenceEpoch; }
		s_LockstepAppliedFrame = 0;
		s_LockstepControlOverrides.clear();
		s_LockstepDroppedControlOverrides.clear();
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
		std::erase_if(s_RecoveredInputs, [&](const auto& input) { return input.targetFrame <= completedTick; });
		std::erase_if(s_RecoveredCommands, [&](const auto& command) { return command.frame <= completedTick; });
		std::erase_if(s_RecoveredPlayerBindings, [&](const auto& binding) { return binding.frame <= completedTick; });
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

	uint8_t ScenarioRunner::GetLockstepDropTimeActorOwner(int64_t actorUniqueID, int actorTeam, bool cpuControlled) {
		if (!s_LockstepCoordinator) {
			return 0;
		}
		const auto overrideIt = s_LockstepControlOverrides.find(actorUniqueID);
		if (overrideIt != s_LockstepControlOverrides.end()) {
			return overrideIt->second;
		}
		const auto droppedIt = s_LockstepDroppedControlOverrides.find(actorUniqueID);
		if (droppedIt != s_LockstepDroppedControlOverrides.end()) {
			return droppedIt->second;
		}
		return s_LockstepCoordinator->ResolveActorOwnerBeforeLeaves(actorUniqueID, actorTeam, cpuControlled);
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

	uint64_t ScenarioRunner::GetLockstepRoundId() {
		return s_LockstepCoordinator ? s_LockstepCoordinator->GetRoundId() : 0;
	}

	uint64_t ScenarioRunner::GetLockstepAppliedFrame() {
		return s_LockstepAppliedFrame;
	}

	void ScenarioRunner::SetLockstepAppliedFrame(uint64_t frame) {
		s_LockstepAppliedFrame = frame;
	}

	void ScenarioRunner::PurgeLockstepControlOverridesForGonePeers(uint64_t frame) {
		if (!s_LockstepCoordinator || s_LockstepControlOverrides.empty()) {
			return;
		}
		for (auto it = s_LockstepControlOverrides.begin(); it != s_LockstepControlOverrides.end();) {
			if (s_LockstepCoordinator->IsPeerGoneAtFrame(it->second, frame)) {
				// Admission can observe the drop after this frame has released its control handoffs.
				s_LockstepDroppedControlOverrides.insert_or_assign(it->first, it->second);
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
		return s_LockstepCoordinator && s_LockstepCoordinator->SubmitLocalChecksum(tick, hash, nullptr, s_AppliedCommandSequences);
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
		return s_LockstepCoordinator->IsSeatHeldForReclaimAtFrame(s_LockstepAppliedFrame) ||
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
				return s_LockstepCoordinator->QueueReplayFrame(tick, std::move(buffered.frames), std::move(buffered.commands), error, std::move(buffered.observations), std::move(buffered.valueObservations));
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
			return s_LockstepCoordinator->QueueReplayFrame(tick, std::move(record.frames), std::move(record.commands), error, std::move(record.observations), std::move(record.valueObservations));
		}
		const auto& config = s_LockstepCoordinator->GetConfig();
		if (s_LockstepCoordinator->NeedsResyncPriming()) {
			std::vector<NetLockstepFrame> batches(config.inputDelayFrames);
			for (size_t index = 0; index < batches.size(); ++index) {
				const uint64_t target = config.startFrame + index;
				auto& input = batches[index];
				const auto previous = s_RequeuedInputs.find(target);
				if (previous != s_RequeuedInputs.end()) input = previous->second;
				input.senderPeerId = config.localPeerId; input.targetFrame = target; input.roundId = s_LockstepCoordinator->GetRoundId();
				if (previous == s_RequeuedInputs.end()) {
					if (const auto commands = s_RequeuedCommands.find(target); commands != s_RequeuedCommands.end()) input.commands = commands->second;
					if (const auto bindings = s_RequeuedPlayerBindings.find(target); bindings != s_RequeuedPlayerBindings.end()) input.commands.push_back({config.localPeerId, bindings->second});
				}
			}
			if (!s_LockstepCoordinator->PrimeResyncInputs(batches, error)) return false;
			const uint64_t primedEnd = config.startFrame + config.inputDelayFrames;
			s_RequeuedCommands.erase(s_RequeuedCommands.begin(), s_RequeuedCommands.lower_bound(primedEnd));
			s_RequeuedPlayerBindings.erase(s_RequeuedPlayerBindings.begin(), s_RequeuedPlayerBindings.lower_bound(primedEnd));
			s_RequeuedInputs.erase(s_RequeuedInputs.begin(), s_RequeuedInputs.lower_bound(primedEnd));
		}
		const auto& acks = s_LockstepCoordinator->GetAuthoritativeCommandAcks();
		if (const auto ack = acks.find(config.localPeerId); ack != acks.end()) {
			s_LocalCommandOutbox.erase(s_LocalCommandOutbox.begin(), s_LocalCommandOutbox.upper_bound(ack->second));
		}
		std::vector<NetGameCommand> commands;
		const uint64_t targetFrame = tick + config.inputDelayFrames;
		if ((!s_RequeuedCommands.empty() && s_RequeuedCommands.begin()->first < targetFrame) ||
			(!s_RequeuedPlayerBindings.empty() && s_RequeuedPlayerBindings.begin()->first < targetFrame) ||
			(!s_RequeuedInputs.empty() && s_RequeuedInputs.begin()->first < targetFrame)) {
			if (error) *error = "a recovered input missed its target frame";
			return false;
		}
		if (const auto previous = s_RequeuedInputs.find(targetFrame); previous != s_RequeuedInputs.end()) {
			NetLockstepFrame input = previous->second;
			input.roundId = s_LockstepCoordinator->GetRoundId();
			if (!s_LockstepCoordinator->QueueRecoveredInput(input, error)) return false;
			s_RequeuedCommands.erase(targetFrame);
			s_RequeuedPlayerBindings.erase(targetFrame);
			s_RequeuedInputs.erase(previous);
			return true;
		}
		if (const auto recovered = s_RequeuedCommands.find(targetFrame); recovered != s_RequeuedCommands.end()) {
			commands = recovered->second;
		}
		const size_t recoveredCount = commands.size();
		size_t freshCount = 0;
		if (s_RequeuedCommands.upper_bound(targetFrame) == s_RequeuedCommands.end() && s_RequeuedInputs.upper_bound(targetFrame) == s_RequeuedInputs.end()) {
			freshCount = std::min(s_PendingLocalGameCommands.size(), NetLockstepCodec::c_MaxCommandsPerPacket - commands.size());
			commands.insert(commands.end(), s_PendingLocalGameCommands.begin(), s_PendingLocalGameCommands.begin() + static_cast<std::ptrdiff_t>(freshCount));
		}
		for (auto& command: commands) {
			command.senderPeerId = config.localPeerId;
			if (command.sequence == 0) {
				if (s_NextLocalCommandSequence == UINT64_MAX) {
					if (error) *error = "game command sequence exhausted";
					return false;
				}
				command.sequence = s_NextLocalCommandSequence++;
			}
			s_LocalCommandOutbox[command.sequence] = {targetFrame, command};
		}
		for (size_t index = 0; index < freshCount; ++index) s_PendingLocalGameCommands[index] = commands[recoveredCount + index];
		if (const auto historical = s_RequeuedPlayerBindings.find(targetFrame); historical != s_RequeuedPlayerBindings.end()) {
			commands.push_back({config.localPeerId, historical->second});
		} else if (const Activity* activity = g_ActivityMan.GetActivity()) {
			NetGamePlayerBindings bindings;
			activity->CaptureNetPlayerBindings(bindings);
			if (config.localPeerId == config.matchConfig.hostPeerId) bindings.appliedCommands = s_AppliedCommandSequences;
			commands.push_back({config.localPeerId, bindings});
		}
		const bool queued = s_LockstepCoordinator->QueueLocalInput(tick, frames, commands, error, g_AudioMan.SampleSoundObservations(), SampleValueObservations());
		if (queued) {
			s_RequeuedCommands.erase(targetFrame);
			s_RequeuedPlayerBindings.erase(targetFrame);
			s_RequeuedInputs.erase(targetFrame);
			s_PendingLocalGameCommands.erase(s_PendingLocalGameCommands.begin(), s_PendingLocalGameCommands.begin() + static_cast<std::ptrdiff_t>(freshCount));
		}
		// A reading the wire had to drop was still recorded as sent, so hand it back to be sampled afresh.
		g_AudioMan.ForgetSentAudibility(s_LockstepCoordinator->TakeDroppedObservations());
		s_DroppedValueObservations = s_LockstepCoordinator->TakeDroppedValueObservations();
		return queued;
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
		if (g_MovableMan.IsRestoringSnapshot()) {
			return;
		}
		if (g_MovableMan.IsSpeculative()) {
			g_MovableMan.ReportSpeculationViolation("queueing a wire command for", nullptr);
			return;
		}
		if (NetGameCommandTypeOf(command.payload) != NetGameCommandType::Reseat) {
			const uint8_t sender = command.senderPeerId != 0 ? command.senderPeerId : GetLockstepLocalPeerId();
			if (!IsLockstepTeamCommandSender(NetGameCommandTeam(command.payload), sender)) {
				return;
			}
		}
		s_PendingLocalGameCommands.push_back(command);
	}

	std::vector<NetGameCommand> ScenarioRunner::DrainLocalGameCommands() {
		std::vector<NetGameCommand> drained = std::move(s_PendingLocalGameCommands);
		s_PendingLocalGameCommands.clear();
		return drained;
	}

	void ScenarioRunner::ObserveLockstepPlayerBindings(uint8_t peer, uint64_t frame, const NetGamePlayerBindings& bindings) {
		if (peer == GetLockstepHostPeerId()) {
			if (const auto ack = bindings.appliedCommands.find(GetLockstepLocalPeerId()); ack != bindings.appliedCommands.end()) {
				s_LocalCommandOutbox.erase(s_LocalCommandOutbox.begin(), s_LocalCommandOutbox.upper_bound(ack->second));
			}
		}
		auto& current = s_PeerPlayerBindings[peer];
		if (frame >= current.frame) current = {frame, bindings};
	}

	bool ScenarioRunner::ConsumeLockstepGameCommand(const NetGameCommand& command) {
		if (command.sequence == 0 || s_ReplayReader.IsOpen()) return true;
		auto& applied = s_AppliedCommandSequences[command.senderPeerId];
		if (command.sequence <= applied) return false;
		applied = command.sequence;
		return true;
	}

	std::vector<NetResyncPendingCommand> ScenarioRunner::CaptureUnacknowledgedLocalCommands() {
		std::vector<NetResyncPendingCommand> commands;
		commands.reserve(s_LocalCommandOutbox.size());
		for (const auto& [sequence, command]: s_LocalCommandOutbox) commands.push_back(command);
		return commands;
	}

	uint64_t ScenarioRunner::ResyncResumeStartFrame(uint64_t dropFrame) {
		return dropFrame;
	}

	uint64_t ScenarioRunner::GetLockstepResumeFrame() {
		return s_LockstepCoordinator ? s_LockstepCoordinator->GetResumeFrame() : 0;
	}

	bool ScenarioRunner::ResolveResyncDropFrame(uint64_t resumeFrame, uint64_t simUpdateCount, uint64_t& outDropFrame, bool& outRewind, std::string* error) {
		const uint64_t lastApplied = resumeFrame > 0 ? resumeFrame - 1 : 0;
		if (simUpdateCount != lastApplied && simUpdateCount != resumeFrame) {
			if (error) *error = "resync sim tick " + std::to_string(simUpdateCount) + " is neither the applied tick " + std::to_string(lastApplied) + " nor the drop frame " + std::to_string(resumeFrame);
			return false;
		}
		outDropFrame = resumeFrame;
		// A failed frame wait leaves the counter on the unsimulated drop frame, a deferred stop on the applied tick.
		outRewind = simUpdateCount == resumeFrame && resumeFrame > 0;
		return true;
	}

	bool ScenarioRunner::CaptureNetResyncState(uint64_t savedTick, NetResyncState& state, std::string* error) {
		if (!s_LockstepCoordinator || savedTick == UINT64_MAX) return false;
		NetResyncState captured;
		captured.sessionId = s_LockstepCoordinator->GetConfig().sessionId;
		captured.sourceRound = s_LockstepCoordinator->GetRoundId();
		captured.savedTick = savedTick;
		captured.controlOwners = s_LockstepControlOverrides;
		captured.droppedControlOwners = s_LockstepDroppedControlOverrides;
		captured.playerBindings = s_PeerPlayerBindings;
		captured.appliedCommands = s_AppliedCommandSequences;
		if (const Activity* activity = g_ActivityMan.GetActivity()) {
			auto& own = captured.playerBindings[GetLockstepLocalPeerId()];
			own.frame = savedTick;
			activity->CaptureNetPlayerBindings(own.bindings);
		}
		std::map<std::pair<uint8_t, uint64_t>, NetLockstepFrame> inputs;
		auto futureInputs = s_RecoveredInputs;
		for (auto& input: s_LockstepCoordinator->CapturePendingInputs(savedTick)) futureInputs.push_back(std::move(input));
		for (const auto& [frame, input]: s_LocalInputHistory) futureInputs.push_back(input);
		for (auto& input: s_LockstepCoordinator->CaptureLocalInputHistory()) futureInputs.push_back(std::move(input));
		for (auto& input: futureInputs) {
			if (input.targetFrame <= savedTick) continue;
			input.roundId = captured.sourceRound;
			const auto [found, inserted] = inputs.emplace(std::make_pair(input.senderPeerId, input.targetFrame), input);
			if (!inserted && !SameInputBits(found->second, input)) { if (error) *error = "conflicting pending input"; return false; }
		}
		auto pending = s_RecoveredCommands;
		for (auto& command: s_LockstepCoordinator->CapturePendingCommands(savedTick)) pending.push_back(std::move(command));
		auto pendingBindings = s_RecoveredPlayerBindings;
		for (auto& binding: s_LockstepCoordinator->CapturePendingPlayerBindings(savedTick)) pendingBindings.push_back(std::move(binding));
		for (const auto& [key, input]: inputs) {
			captured.pendingInputs.push_back(input);
			for (const auto& command: input.commands) {
				if (std::holds_alternative<NetGamePlayerBindings>(command.payload)) pendingBindings.push_back({input.targetFrame, command});
				else if (command.sequence != 0) pending.push_back({input.targetFrame, command});
			}
		}
		for (const auto& [sequence, command]: s_LocalCommandOutbox) pending.push_back(command);
		std::map<std::pair<uint8_t, uint64_t>, NetResyncPendingCommand> unique;
		for (auto command: pending) {
			const auto& value = command.command;
			const auto applied = captured.appliedCommands.find(value.senderPeerId);
			if (value.sequence == 0 || (applied != captured.appliedCommands.end() && value.sequence <= applied->second)) continue;
			command.frame = std::max(command.frame, savedTick + 1);
			const auto [found, inserted] = unique.emplace(std::make_pair(value.senderPeerId, value.sequence), command);
			if (!inserted && (!SameCommandBits(found->second.command, value) || found->second.frame != command.frame)) {
				if (error) *error = "conflicting pending game command";
				return false;
			}
		}
		for (const auto& [key, command]: unique) captured.pendingCommands.push_back(command);
		unique.clear();
		for (const auto& binding: pendingBindings) {
			if (binding.frame <= savedTick) continue;
			const auto [found, inserted] = unique.emplace(std::make_pair(binding.command.senderPeerId, binding.frame), binding);
			if (!inserted && !SameCommandBits(found->second.command, binding.command)) { if (error) *error = "conflicting pending player binding"; return false; }
		}
		for (const auto& [key, binding]: unique) captured.pendingPlayerBindings.push_back(binding);
		for (const auto& command: s_PendingLocalGameCommands) {
			if (command.sequence == 0 && command.senderPeerId == GetLockstepHostPeerId() && std::holds_alternative<NetGameReseat>(command.payload)) captured.admittedReseats.push_back(command);
		}
		state = std::move(captured);
		return true;
	}

	bool ScenarioRunner::RestoreNetResyncState(const NetResyncState& state, std::string* error) {
		const auto fail = [&] { if (error) *error = "resync command or ownership state is inconsistent"; return false; };
		if (!s_LockstepCoordinator || state.sessionId != s_LockstepCoordinator->GetConfig().sessionId || state.savedTick == UINT64_MAX ||
			state.savedTick + 1 != s_LockstepCoordinator->GetConfig().startFrame) return fail();
		std::vector<uint8_t> validation;
		if (!NetResyncCodec::Encode(state, {0}, validation, error)) return false;
		const auto member = [&](uint8_t peer) { return peer > 0 && peer <= s_LockstepCoordinator->GetConfig().peerCount; };
		for (const auto& [uid, peer]: state.controlOwners) if (!member(peer)) return fail();
		for (const auto& [uid, peer]: state.droppedControlOwners) if (!member(peer)) return fail();
		for (const auto& [peer, binding]: state.playerBindings) if (!member(peer)) return fail();
		for (const auto& [peer, sequence]: state.appliedCommands) if (!member(peer)) return fail();
		for (const auto& command: state.pendingCommands) if (!member(command.command.senderPeerId)) return fail();
		for (const auto& binding: state.pendingPlayerBindings) if (!member(binding.command.senderPeerId)) return fail();
		for (const auto& input: state.pendingInputs) if (!member(input.senderPeerId)) return fail();
		const uint8_t local = GetLockstepLocalPeerId();
		const auto applied = state.appliedCommands.find(local);
		const uint64_t watermark = applied == state.appliedCommands.end() ? 0 : applied->second;
		const uint64_t round = s_LockstepCoordinator->GetRoundId();
		const auto future = [&](uint64_t frame) { return frame > state.savedTick && frame - state.savedTick - 1 <= NetLockstepCodec::c_MaxFutureFrameSkew; };
		std::vector<NetLockstepFrame> authoritative;
		std::map<std::pair<uint8_t, uint64_t>, NetLockstepFrame> inputs;
		for (auto input: state.pendingInputs) {
			input.roundId = round;
			inputs.emplace(std::make_pair(input.senderPeerId, input.targetFrame), input);
			authoritative.push_back(std::move(input));
		}
		auto history = s_LocalInputHistory;
		for (auto& input: s_LockstepCoordinator->CaptureLocalInputHistory()) history[input.targetFrame] = std::move(input);
		std::map<uint64_t, NetLockstepFrame> requeuedInputs;
		for (auto& [frame, input]: history) {
			if (frame <= state.savedTick) continue;
			input.roundId = round;
			if (!future(frame) || input.senderPeerId != local || frame != input.targetFrame || !NetLockstepCodec::EncodeRecoveryInput(input, validation)) return fail();
			const auto [found, inserted] = inputs.emplace(std::make_pair(local, frame), input);
			if (!inserted && !SameInputBits(found->second, input)) return fail();
		}
		std::map<std::pair<uint8_t, uint64_t>, NetResyncPendingCommand> commands, bindings;
		const auto addCommand = [&](const NetResyncPendingCommand& pending) {
			const auto& command = pending.command;
			const auto ack = state.appliedCommands.find(command.senderPeerId);
			if (command.sequence == 0 || (ack != state.appliedCommands.end() && command.sequence <= ack->second)) return true;
			const auto [found, inserted] = commands.emplace(std::make_pair(command.senderPeerId, command.sequence), pending);
			return inserted || (found->second.frame == pending.frame && SameCommandBits(found->second.command, command));
		};
		const auto addBinding = [&](const NetResyncPendingCommand& pending) {
			const auto [found, inserted] = bindings.emplace(std::make_pair(pending.command.senderPeerId, pending.frame), pending);
			return inserted || SameCommandBits(found->second.command, pending.command);
		};
		for (const auto& pending: state.pendingCommands) if (!addCommand(pending)) return fail();
		for (const auto& pending: state.pendingPlayerBindings) if (!addBinding(pending)) return fail();
		for (const auto& [key, input]: inputs) {
			for (const auto& command: input.commands) {
				if (std::holds_alternative<NetGamePlayerBindings>(command.payload)) {
					if (!addBinding({input.targetFrame, command})) return fail();
				} else if (!addCommand({input.targetFrame, command})) return fail();
			}
			if (input.senderPeerId == local) {
				requeuedInputs.emplace(input.targetFrame, input);
				history[input.targetFrame] = input;
			}
		}
		const auto agreesWithInput = [&](const NetResyncPendingCommand& pending) {
			const auto input = inputs.find({pending.command.senderPeerId, pending.frame});
			return input == inputs.end() || std::any_of(input->second.commands.begin(), input->second.commands.end(), [&](const auto& command) { return SameCommandBits(command, pending.command); });
		};
		std::pair<uint8_t, uint64_t> previous{};
		for (const auto& [key, pending]: commands) {
			if (!agreesWithInput(pending) || (previous.first == key.first && pending.frame < previous.second)) return fail();
			previous = {key.first, pending.frame};
		}
		for (const auto& [key, pending]: bindings) if (!agreesWithInput(pending)) return fail();
		auto outbox = s_LocalCommandOutbox;
		for (const auto& [key, pending]: commands) if (pending.command.senderPeerId == local) {
			const auto [found, inserted] = outbox.emplace(pending.command.sequence, pending);
			if (!inserted && (!SameCommandBits(found->second.command, pending.command) || found->second.frame != pending.frame)) return fail();
		}
		outbox.erase(outbox.begin(), outbox.upper_bound(watermark));
		auto owners = state.controlOwners;
		auto dropped = state.droppedControlOwners;
		for (const auto& command: state.admittedReseats) {
			const auto* reseat = std::get_if<NetGameReseat>(&command.payload);
			if (!reseat || command.senderPeerId != GetLockstepHostPeerId() || command.sequence != 0 || !member(reseat->newOwnerPeerId)) return fail();
			for (const auto uid: reseat->actorUIDs) {
				const Actor* actor = dynamic_cast<const Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long>(uid)));
				if (!actor || actor->GetTeam() == reseat->team) { owners[uid] = reseat->newOwnerPeerId; dropped.erase(uid); }
			}
		}
		auto pending = s_PendingLocalGameCommands;
		for (const auto& command: pending) {
			if (command.sequence == 0) continue;
			const auto found = outbox.find(command.sequence);
			if (command.senderPeerId != local || (command.sequence > watermark && (found == outbox.end() || !SameCommandBits(command, found->second.command)))) return fail();
		}
		std::erase_if(pending, [](const auto& command) { return command.sequence != 0; });
		std::erase_if(pending, [&](const auto& command) { return std::any_of(state.admittedReseats.begin(), state.admittedReseats.end(), [&](const auto& admitted) { return SameCommandBits(command, admitted); }); });
		std::map<uint64_t, std::vector<NetGameCommand>> requeued;
		std::map<uint64_t, NetGamePlayerBindings> requeuedBindings;
		std::set<uint64_t> acceptedSequences;
		for (const auto& [key, command]: commands) if (command.command.senderPeerId == local) acceptedSequences.insert(command.command.sequence);
		for (const auto& [key, binding]: bindings) if (binding.command.senderPeerId == local && !requeuedInputs.contains(binding.frame)) {
			const auto* value = std::get_if<NetGamePlayerBindings>(&binding.command.payload);
			if (!value || !future(binding.frame) || !requeuedBindings.emplace(binding.frame, *value).second) return fail();
		}
		uint64_t previousFrame = state.savedTick + 1;
		uint64_t nextSequence = std::max(s_NextLocalCommandSequence, watermark + 1);
		for (auto& [sequence, command]: outbox) {
			if (sequence == 0 || sequence == UINT64_MAX || command.command.sequence != sequence || command.command.senderPeerId != local ||
				std::holds_alternative<NetGamePlayerBindings>(command.command.payload)) return fail();
			if (acceptedSequences.contains(sequence) && command.frame < previousFrame) return fail();
			previousFrame = std::max(previousFrame, command.frame);
			if (const auto full = requeuedInputs.find(previousFrame); full != requeuedInputs.end() &&
				std::any_of(full->second.commands.begin(), full->second.commands.end(), [&](const auto& value) { return SameCommandBits(value, command.command); })) {
				nextSequence = std::max(nextSequence, sequence + 1);
				continue;
			}
			while (requeuedInputs.contains(previousFrame) || requeued[previousFrame].size() >= NetLockstepCodec::c_MaxCommandsPerPacket) {
				if (acceptedSequences.contains(sequence) || previousFrame == UINT64_MAX) return fail();
				++previousFrame;
			}
			if (!future(previousFrame)) return fail();
			command.frame = previousFrame;
			requeued[previousFrame].push_back(command.command);
			nextSequence = std::max(nextSequence, sequence + 1);
		}
		std::vector<NetResyncPendingCommand> recoveredCommands, recoveredBindings;
		for (const auto& [key, command]: commands) recoveredCommands.push_back(command);
		for (const auto& [key, binding]: bindings) recoveredBindings.push_back(binding);
		if (!history.empty() && history.rbegin()->first > NetLockstepCodec::c_MaxFutureFrameSkew) {
			history.erase(history.begin(), history.lower_bound(history.rbegin()->first - NetLockstepCodec::c_MaxFutureFrameSkew));
		}
		if (!s_LockstepCoordinator->InstallResyncInputs(authoritative, error)) return false;
		s_LockstepControlOverrides = std::move(owners);
		s_LockstepDroppedControlOverrides = std::move(dropped);
		s_PeerPlayerBindings = state.playerBindings;
		s_AppliedCommandSequences = state.appliedCommands;
		s_LocalCommandOutbox = std::move(outbox);
		s_PendingLocalGameCommands = std::move(pending);
		s_RequeuedCommands = std::move(requeued);
		s_RequeuedPlayerBindings = std::move(requeuedBindings);
		s_RequeuedInputs = std::move(requeuedInputs);
		s_LocalInputHistory = std::move(history);
		s_RecoveredInputs = std::move(authoritative);
		s_RecoveredCommands = std::move(recoveredCommands);
		s_RecoveredPlayerBindings = std::move(recoveredBindings);
		s_NextLocalCommandSequence = nextSequence;
		return true;
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

	bool ScenarioRunner::DrainLockstepRelay(uint32_t budgetMs, uint32_t lingerMs) {
		const auto start = std::chrono::steady_clock::now();
		auto elapsed = [&start] {
			return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
		};
		if (!s_LockstepCoordinator) {
			std::this_thread::sleep_for(std::chrono::milliseconds(lingerMs));
			return true;
		}
		while (s_LockstepCoordinator->HasPendingRelayWork() && elapsed() < budgetMs) {
			s_LockstepCoordinator->Tick(NetLockstepNowMs());
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		const bool drained = !s_LockstepCoordinator->HasPendingRelayWork();
		const uint32_t drainMs = elapsed();
		if (!drained) {
			std::cout << "[net-match] quit with " << s_LockstepCoordinator->GetStats().relayBacklogBytes
			          << " bytes still owed to peers after " << drainMs << "ms" << std::endl;
		} else if (drainMs > 0) {
			std::cout << "[net-match] relay drained in " << drainMs << "ms" << std::endl;
		}
		// Keep relaying through the linger rather than idling it away: a client finishing its own last
		// tick sends a frame its siblings still need, and we are the only route between them.
		for (const uint32_t until = drainMs + lingerMs; elapsed() < until;) {
			s_LockstepCoordinator->Tick(NetLockstepNowMs());
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return drained;
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
		auto giveUpOrigin = waitStart;
		bool heldThisWait = false;
		// A sub-second wait is a normal frame exchange; only a real stall gets the marker + overlay.
		uint32_t nextOverlayMs = 1500;
		bool stalled = false;
		uint32_t nextPumpMs = 0;
		while (true) {
			s_LockstepCoordinator->Tick(NetLockstepNowMs());
			// A stalled round must not stall the admission plane with it: the peer we are waiting on may
			// be waiting on an answer only this pump can send. Paced to the tick so the plane's own
			// clock does not run ahead of the wall clock while we spin.
			if (s_SessionPump) {
				const uint32_t sincePumpMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count());
				if (sincePumpMs >= nextPumpMs) {
					nextPumpMs = sincePumpMs + 15;
					s_SessionPump();
				}
			}
			if (!s_LockstepCoordinator) {
				if (error) *error = "lockstep coordinator is not active";
				return false;
			}
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
						std::vector<NetValueObservation> allValueObservations = ready.localValueObservations;
						allValueObservations.insert(allValueObservations.end(), ready.remoteValueObservations.begin(), ready.remoteValueObservations.end());
						std::string writeError;
						if (!s_ReplayWriter.WriteFrame(tick, allFrames, allCommands, allObservations, allValueObservations, &writeError)) {
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
			const bool holdPause = s_LockstepCoordinator->AnyDroppedSeatHeld();
			uint32_t holdSeconds = 0;
			std::string holdName;
			if (holdPause) {
				holdName = s_LockstepCoordinator->DescribeHeldPause(holdSeconds, NetLockstepNowMs());
				if (s_SeatPresence) {
					for (const auto& [peerId, seat]: s_SeatPresence->GetSeats()) {
						if (!seat.holdActive) {
							continue;
						}
						if (!seat.holderName.empty()) {
							holdName = seat.holderName;
						}
						holdSeconds = static_cast<uint32_t>(s_SeatPresence->HoldWallSecondsRemaining(peerId));
						break;
					}
				}
			}
			if (stallMs >= nextOverlayMs) {
				const std::string missing = s_LockstepCoordinator->DescribeMissingPeers();
				if (!stalled) {
					stalled = true;
					if (holdPause) {
						std::cout << "[net-match] match paused waiting for " << (holdName.empty() ? "a player" : holdName)
						          << " (" << holdSeconds << "s left, tick " << tick << ")" << std::endl;
					} else {
						std::cout << "[net-match] waiting on peer frames (tick " << tick << (missing.empty() ? "" : ", " + missing) << ")" << std::endl;
					}
				}
				if (s_LockstepStallOverlayEnabled) {
					DrawLockstepStallOverlay(stallMs, timeoutMs, missing, holdPause, holdName, holdSeconds);
				}
				nextOverlayMs = stallMs + 200;
			}
			if (holdPause) {
				heldThisWait = true;
			} else {
				if (heldThisWait) {
					giveUpOrigin = std::chrono::steady_clock::now();
					heldThisWait = false;
				}
				const uint32_t giveUpStallMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - giveUpOrigin).count());
				if (giveUpStallMs >= giveUpMs) {
					break;
				}
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
