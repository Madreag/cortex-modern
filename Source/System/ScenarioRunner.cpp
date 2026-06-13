#include "ScenarioRunner.h"

#include "Constants.h"
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
#include <sstream>
#include <string>

namespace RTE {

	namespace {
		bool s_Active = false;
		ScenarioRunner::Args s_Args;

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
		if (a == "-tick-hashes") {
			// Turn on per-tick hash trace recording. Boolean flag — no value.
			s_Args.tickHashes = true;
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

		return currentRun.passed ? 0 : 1;
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
