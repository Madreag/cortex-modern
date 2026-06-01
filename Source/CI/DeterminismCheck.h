#pragma once

// CI determinism test scaffold.
//
// Provides the `-determinism-check` mode of the main game binary. When that flag is
// detected by `Main.cpp` (before any of the heavy engine init runs), execution is
// handed to `DeterminismCheck::Run` and the binary acts as a thin orchestrator:
// it re-invokes itself N times (as a child process) with `-scenario <name>
// -seed <S> -max-ticks <T> -tick-hashes -out runs/<i>.json` and then diffs the
// per-tick hash traces from each run.
//
// Output is one JSON report at `--output <path>` describing whether all runs
// matched and — when they didn't — which tick and which subsystem first diverged.
//
// CLI:
//   <bin> -determinism-check --scenario SimBaseline --ticks 600 --seed 42 --runs 10 \
//                            --output divergence.json [--game-bin <path>] [--keep-runs]
//
// The thread-count matrix mode adds cross-thread-count diffing. With `--threads 1,2,4,8,16` the
// orchestrator runs the scenario at each Lua-state count (via `-num-lua-states`)
// and diffs the per-tick traces ACROSS counts — the acceptance test for the
// "bit-identical regardless of thread count" contract:
//   <bin> -determinism-check --scenario ThreadStress --ticks 900 --seed 42 \
//                            --threads 1,2,4,8,16 --runs 2 --output matrix.json
//
// Single-dash forms (`-scenario`, `-runs`, …) are also accepted for parity with the
// rest of the engine's CLI conventions.
//
// Exit codes:
//   0 — all runs matched on every tick (the deterministic-by-construction goal).
//   1 — at least one tick diverged across runs.
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
