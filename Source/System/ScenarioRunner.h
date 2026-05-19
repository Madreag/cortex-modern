#pragma once

#include <cstdint>
#include <string>

namespace RTE {

	/// CLI scenario direct-launch mode.
	///
	/// Activated when the binary is invoked with `-scenario <PresetName>`. Skips the menu loop,
	/// starts the named activity directly via `g_ActivityMan.StartActivity`, runs the normal
	/// `RunGameLoop` (visible window, real GL context — the user *watches* the scenario play),
	/// and exits cleanly when the activity sets `ActivityState::Over`. The `MetricsCollector`'s
	/// JSON report is written if `-out <path>` was supplied.
	///
	/// Used both for solo developer "run AI-04 and watch it" workflows and for batch-style
	/// runs from a shell script that loops over scenarios. There is no "headless" mode any more;
	/// the previous hidden-window architecture was reverted because the user wants to *see*
	/// the AI play.
	class ScenarioRunner {
	public:
		struct Args {
			std::string scenario;     // single PresetName-suffix, e.g. "AI-01". One scenario per
			                          // invocation — batching all 12 is a shell-script concern
			                          // (FMOD/module-reload across activities in one process is messy).
			std::string outPath;      // JSON output path; empty = no report
			uint64_t    seed = 0;     // deterministic seed; 0 = use SeedRNG()'s default
			uint64_t    maxTicks = 0; // 0 = scenario-default safety cap (1800 ticks / 30 sim seconds)
			bool        tickHashes = false; // -tick-hashes: emit per-tick hash trace into the JSON
			                                 // report. Block A (M1) — read by cccp-determinism-check
			                                 // to diff multiple runs of the same scenario+seed.
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

		/// Build the full preset name from the `-scenario` arg (currently prefixes with "Trust ").
		/// Returns "Trust AI-01" for arg "AI-01", or just the raw string if no transformation applies.
		static std::string ResolvePresetName(const std::string& shorthand);

		/// After the activity ends and RunGameLoop returns, write the metrics report (if -out set).
		/// Returns exit code: 0 if scenario passed (or no result was set), 1 otherwise.
		static int FinalizeAndGetExitCode();
	};

} // namespace RTE
