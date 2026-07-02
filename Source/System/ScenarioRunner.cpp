#include "ScenarioRunner.h"

#include "Constants.h"
#include "ControllerLog.h"
#include "FrameMan.h"
#include "MetricsCollector.h"
#include "MovableMan.h"
#include "SettingsMan.h"
#include "TimerMan.h"
#include "WindowMan.h"

#include "GUI.h"
#include "AllegroBitmap.h"
#include "RenderTarget.h"

#include <SDL3/SDL.h>

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
		uint64_t s_LockstepPollNowMs = 0;
		std::vector<NetGameCommand> s_PendingLocalGameCommands;
		bool s_LockstepStallOverlayEnabled = false;

		// Presentation only: the sim thread is blocked waiting on the peer, so the normal render path
		// can't run. Keep the window pumped and show the last frame replaced by a plain wait screen.
		void DrawLockstepStallOverlay(uint32_t stallMs, uint32_t graceMs) {
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
			largeFont->DrawAligned(&drawBitmap, centerX, centerY - 12, "Waiting for the other player... " + std::to_string(stallMs / 1000) + "s", GUIFont::Centre);
			if (graceMs > stallMs) {
				smallFont->DrawAligned(&drawBitmap, centerX, centerY + 8, "The match ends in " + std::to_string((graceMs - stallMs + 999) / 1000) + "s if they do not return", GUIFont::Centre);
			}
			g_WindowMan.ClearBackbuffer(false);
			g_WindowMan.GetScreenBuffer()->Begin();
			g_WindowMan.UploadFrame();
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
		s_LockstepPollNowMs = 0;
	}

	bool ScenarioRunner::IsLockstepControllerSyncActive() {
		return s_LockstepCoordinator && s_LockstepCoordinator->IsRunning();
	}

	bool ScenarioRunner::IsLockstepLocalActor(int64_t actorUniqueID, int actorTeam, bool cpuControlled) {
		return !s_LockstepCoordinator || s_LockstepCoordinator->IsLocalActor(actorUniqueID, actorTeam, cpuControlled);
	}

	uint8_t ScenarioRunner::ResolveTeamCommandAuthority(int team) {
		return s_LockstepCoordinator ? s_LockstepCoordinator->ResolveTeamCommandAuthority(team) : 0;
	}

	bool ScenarioRunner::SubmitLockstepChecksum(uint64_t tick, const std::array<uint8_t, 32>& hash) {
		return s_LockstepCoordinator && s_LockstepCoordinator->SubmitLocalChecksum(tick, hash);
	}

	uint16_t ScenarioRunner::GetLockstepInputDelayFrames() {
		return s_LockstepCoordinator ? s_LockstepCoordinator->GetConfig().inputDelayFrames : 0;
	}

	bool ScenarioRunner::QueueLockstepLocalControllerFrames(uint64_t tick, std::vector<ControllerFrame> frames, std::string* error) {
		if (!s_LockstepCoordinator) {
			if (error) *error = "lockstep coordinator is not active";
			return false;
		}
		return s_LockstepCoordinator->QueueLocalInput(tick, frames, DrainLocalGameCommands(), error);
	}

	void ScenarioRunner::EnqueueLocalGameCommand(const NetGameCommand& command) {
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

	bool ScenarioRunner::WaitForLockstepControllerFrame(uint64_t tick, NetLockstepReadyFrame& outFrame, std::string* error) {
		if (!s_LockstepCoordinator) {
			if (error) *error = "lockstep coordinator is not active";
			return false;
		}

		const uint32_t timeoutMs = s_LockstepCoordinator->GetConfig().timeoutMs;
		const uint32_t maxPolls = timeoutMs > 0 ? timeoutMs + 50 : 500;
		const auto waitStart = std::chrono::steady_clock::now();
		// A sub-second wait is a normal frame exchange; only a real stall gets the marker + overlay.
		uint32_t nextOverlayMs = 1500;
		bool stalled = false;
		for (uint32_t poll = 0; poll <= maxPolls; ++poll) {
			s_LockstepCoordinator->Tick(s_LockstepPollNowMs++);
			NetLockstepReadyFrame ready;
			while (s_LockstepCoordinator->PopReadyFrame(ready)) {
				if (ready.frame == tick) {
					if (stalled) {
						const auto stallMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count();
						std::cout << "[net-match] peer stall recovered after " << stallMs << "ms (tick " << tick << ")" << std::endl;
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
				if (!stalled) {
					stalled = true;
					std::cout << "[net-match] waiting on peer frames (tick " << tick << ")" << std::endl;
				}
				if (s_LockstepStallOverlayEnabled) {
					DrawLockstepStallOverlay(stallMs, timeoutMs);
				}
				nextOverlayMs = stallMs + 200;
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
		return config;
	}

} // namespace RTE
