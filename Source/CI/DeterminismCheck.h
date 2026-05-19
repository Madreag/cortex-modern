#pragma once

// Block A (M1) — CI determinism test scaffold.
//
// Provides the `-determinism-check` mode of the main game binary. When that flag is
// detected by `Main.cpp` (before any of the heavy engine init runs), execution is
// handed to `DeterminismCheck::Run` and the binary acts as a thin orchestrator:
// it re-invokes itself N times (as a child process) with `-scenario <name>
// -seed <S> -max-ticks <T> -tick-hashes -out runs/<i>.json` and then diffs the
// per-tick BLAKE3 hash traces from each run.
//
// Output is one JSON report at `--output <path>` describing whether all runs
// matched and — when they didn't — which tick and which subsystem first diverged.
//
// CLI:
//   <bin> -determinism-check --scenario M1Baseline --ticks 600 --seed 42 --runs 10 \
//                            --output divergence.json [--game-bin <path>] [--keep-runs]
//
// Single-dash forms (`-scenario`, `-runs`, …) are also accepted for parity with the
// rest of the engine's CLI conventions.
//
// Exit codes:
//   0 — all runs matched on every tick (the deterministic-by-construction goal).
//   1 — at least one tick diverged across runs (expected at the start of M1; the
//       Blocks B-F work drives this to zero).
//   2 — argument/usage error or child-process failure.
//
// This module deliberately has *no* dependency on engine headers (no Singleton,
// no SimChecksum, no nlohmann include from Source) — only stdlib + nlohmann/json
// via the pre-vendored header at `external/sources/nlohmann_json-3.12.0/`. This
// makes the entry point safe to invoke before InitializeManagers + module load.

#include <string>

namespace RTE {

	class DeterminismCheck {
	public:
		/// `true` iff `-determinism-check` appeared in argv. Cheap pre-scan called from
		/// `main()` so we never touch InitializeManagers or PresetMan::LoadAllDataModules
		/// when the user just wants to run the comparator.
		static bool IsRequested(int argc, char** argv);

		/// Run the determinism-check orchestrator. Parses the rest of argv, spawns child
		/// processes, diffs their JSON outputs, writes a divergence report, returns the
		/// process exit code (0/1/2 — see file header).
		static int Run(int argc, char** argv);
	};

} // namespace RTE
