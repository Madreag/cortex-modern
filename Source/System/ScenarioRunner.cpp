#include "ScenarioRunner.h"

#include "Constants.h"
#include "ControllerLog.h"
#include "MetricsCollector.h"
#include "MovableMan.h"
#include "SettingsMan.h"
#include "TimerMan.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace RTE {

	namespace {
		bool s_Active = false;
		ScenarioRunner::Args s_Args;
		std::unique_ptr<ControllerLog> s_ControllerRecordLog;
		std::unique_ptr<ControllerLog> s_ControllerReplayLog;
		std::string s_ControllerReplayError;

		std::string FloatBitsHex(float value) {
			uint32_t bits;
			std::memcpy(&bits, &value, sizeof(bits));
			std::ostringstream oss;
			oss << "0x" << std::hex << std::setw(8) << std::setfill('0') << bits;
			return oss.str();
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
		if (a == "-determinism-selftest-perturb") {
			// Positive-control: arm the one-shot perturbation. Boolean flag — no value.
			s_Args.selftestPerturb = true;
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
			if (s_ControllerReplayLog->metadata.numLuaStates >= 0 && s_Args.numLuaStates >= 0 && s_ControllerReplayLog->metadata.numLuaStates != s_Args.numLuaStates) {
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

	bool ScenarioRunner::HasControllerReplayError() {
		return !s_ControllerReplayError.empty();
	}

	const std::string& ScenarioRunner::GetControllerReplayError() {
		return s_ControllerReplayError;
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
