#pragma once

#include "ControllerFrame.h"
#include "NetLockstep.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace RTE {
	/// CLI scenario direct-launch mode.
	///
	/// Activated when the binary is invoked with `-scenario <PresetName>`. Skips the menu loop,
	/// starts the named activity directly via `g_ActivityMan.StartActivity`, runs the normal
	/// `RunGameLoop` — visible by default so the user can *watch*, or hidden under `-headless` /
	/// `-tick-hashes` for an automated run (`-headed` forces a window back on) — and exits when
	/// the activity ends or the `-max-ticks` cap is hit. The `MetricsCollector`'s JSON report is
	/// written if `-out <path>` was supplied.
	///
	/// Used both for "run a scenario and watch it" workflows and for batch determinism runs
	/// driven headless by the `-determinism-check` orchestrator.
	class ScenarioRunner {
	public:
		struct Args {
			std::string scenario;     // single PresetName-suffix, e.g. "SimBaseline". One scenario per
			                          // invocation — batching is a shell-script concern
			                          // (FMOD/module-reload across activities in one process is messy).
			std::string outPath;      // JSON output path; empty = no report
			uint64_t    seed = 0;     // deterministic seed; 0 = use SeedRNG()'s default
			uint64_t    maxTicks = 0; // 0 = scenario-default safety cap (1800 ticks / 30 sim seconds)
			int         numLuaStates = -1; // -num-lua-states override, if supplied
			std::string controllerLogOutPath; // -controller-log-out path
			std::string controllerLogInPath; // -controller-log-in path
			bool        controllerLogCanonicalize = true; // record sim uses encoded->decoded frames
			bool        controllerReplayStrict = false; // strict replay skips local Controller/AI production
			bool        controllerLogAllowNlsMismatch = false; // cross-nls replay stress only
			std::string controllerDebugDumpPath; // -controller-debug-dump JSONL path for replay diagnostics
			std::vector<std::pair<uint64_t, uint64_t>> controllerDebugDumpTicks; // optional inclusive tick ranges
			bool        tickHashes = false; // -tick-hashes: emit per-tick hash trace into the JSON
			                                 // report, read by the determinism check to diff
			                                 // multiple runs of the same scenario+seed.
			bool        selftestPerturb = false; // -determinism-selftest-perturb: inject one genuine
			                                 // non-determinism at a fixed tick (the determinism
			                                 // check's positive control).
			bool        selftestFundsCommand = false; // -net-match-e2e-funds-command: host-issued funds command at tick 50.
			bool        selftestSpawnCommand = false; // -net-match-e2e-spawn-command: host-issued spawn command at tick 50.
			bool        selftestDeliverCommand = false; // -net-match-e2e-deliver-command: host-issued delivery command at tick 50.
		};

		/// True if `-scenario` was supplied on the command line.
		static bool IsActive();

		/// Set by ParseArgs. Reset for tests.
		static void SetActive(bool active);

		/// Parsed args. Only valid when IsActive() is true.
		static const Args& GetArgs();

		/// Parse a single CLI flag starting at startIndex. Returns the number of argv items
		/// consumed (0 / 1 / 2). Sets active = true when `-scenario` is seen.
		static int ParseArgs(int argCount, char** argValue, int startIndex);

		/// Build the full preset name from the `-scenario` arg (prefixes with "Determinism ").
		/// Returns "Determinism SimBaseline" for arg "SimBaseline", or the raw string if already prefixed.
		static std::string ResolvePresetName(const std::string& shorthand);

		/// After the activity ends and RunGameLoop returns, write the metrics report (if -out set).
		/// Returns exit code: 0 if scenario passed (or no result was set), 1 otherwise.
		static int FinalizeAndGetExitCode();

		/// Prepare ControllerFrame record/replay state after deterministic config is pinned.
		static bool PrepareControllerLog(std::string* error = nullptr);

		static bool IsControllerLogRecording();
		static bool IsControllerLogReplaying();
		static bool ShouldCanonicalizeControllerLog();
		static bool IsControllerReplayStrict();
		static bool IsControllerDebugDumpEnabled();
		static bool ShouldControllerDebugDumpTick(uint64_t tick);
		static const std::string& GetControllerDebugDumpPath();
		static void RecordControllerFrames(uint64_t tick, std::vector<ControllerFrame> frames);
		static bool GetReplayControllerFrames(uint64_t tick, std::vector<ControllerFrame>& outFrames, std::string* error = nullptr);
		static void SetControllerReplayError(const std::string& error);
		static void ClearControllerReplayError();
		static bool HasControllerReplayError();
		static const std::string& GetControllerReplayError();

		static void SetLockstepCoordinator(NetLockstepCoordinator* coordinator);
		static bool IsLockstepControllerSyncActive();
		static bool IsLockstepLocalActor(int64_t actorUniqueID, int actorTeam, bool cpuControlled);
		static uint16_t GetLockstepInputDelayFrames();
		static bool QueueLockstepLocalControllerFrames(uint64_t tick, std::vector<ControllerFrame> frames, std::string* error = nullptr);
		static bool WaitForLockstepControllerFrame(uint64_t tick, NetLockstepReadyFrame& outFrame, std::string* error = nullptr);

		/// Enqueue an owner-issued game command to ride the next local lockstep frame; the coordinator stamps
		/// the sender and both peers apply it at the synced frame.
		static void EnqueueLocalGameCommand(const NetGameCommand& command);
		static std::vector<NetGameCommand> DrainLocalGameCommands();

		/// Pin the sim-affecting config to canonical values for a deterministic run, so the sim is
		/// bit-identical across machines regardless of per-machine Settings.ini. Call after settings
		/// load, before the sim starts.
		static void ApplyDeterministicConfig();

		/// Snapshot the effective sim-affecting config as key=value strings, for the trace fingerprint.
		static std::map<std::string, std::string> GatherSimConfig();
	};

} // namespace RTE
