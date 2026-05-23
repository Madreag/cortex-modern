# cccp-ctl

Standalone CLI for testing + benchmarking the Cortex Command sim. Implements
**T-VALIDATION** (ADR-017) — the first-class test surface every MP milestone
extends with its own subcommand. See the [engine.html T-VALIDATION card](../../../cccp/modernization-docs/engine.html#tracks)
for the full plan + V1.x changelog.

## Quick start

**Build (Windows, MSBuild — primary):**
```
msbuild Source\CLI\cccp-ctl.vcxproj /p:Configuration=Release /p:Platform=x64
```
Output: `Source/CLI/_BinTests/x64/Release/cccp-ctl.exe`

**Build (Linux/macOS/WASM, Meson):**
```
meson setup builddir
meson compile -C builddir cccp-ctl
```
Output: `builddir/cccp-ctl`

**Run:**
```
cccp-ctl --help
cccp-ctl info game-bin                                      # First-stop sanity check
cccp-ctl test selftest                                      # Verify the harness itself (EC3)
cccp-ctl test all --quick                                   # CI smoke run, ~minutes
cccp-ctl test scenario --scenario M1Baseline --seed 42
cccp-ctl test replay-determinism --scenario M1Baseline --runs 100
cccp-ctl test thread-matrix --scenario M4ThreadStress --threads 1,2,4,8,16
cccp-ctl test cross-platform-checksum --traces win.json,lin.json,mac.json --labels win,linux,macos
cccp-ctl trace inspect M1Baseline-trace.json --ticks 1,100,599
cccp-ctl bench replay --scenario M3TerrainStress --runs 5
```

## Subcommand surface

| Command | What it does |
|---|---|
| `test all` | Aggregate M1–M4 suite runner (the CI gate). `--quick` for fast smoke. |
| `test selftest` | EC3 positive control — runs scenario without then with `--selftest-perturb`, verifies harness catches injected non-determinism. First test to run on any new machine. |
| `test scenario` | Run a single scenario, report pass/fail + metrics. |
| `test replay-determinism` | Run scenario N times, diff per-tick hashes. Wraps `<bin> -determinism-check`. |
| `test thread-matrix` | Run scenario across Lua-state counts (M4 FFF-415 acid test). |
| `test cross-platform-checksum` | Diff trace JSONs from multiple platforms (M5 acceptance test). |
| `bench replay` | Run scenario N times, report `__sim_compute_accum` throughput (min/median/max + spread%). |
| `trace inspect <path>` | Pretty-print a trace JSON. `--full`, `--ticks N1,N2`, `--subsystem name` filters. |
| `info scenarios` / `info subsystems` / `info game-bin` | Discovery / introspection. |
| `test sync-drift` / `latency-injection` / `rollback-burst` | Stubs — exit 64. Need M6 / M9. |

Run `cccp-ctl <command> --help` for every option — the help is comprehensive
and is the canonical reference. This README is a pointer.

## Common options (every subcommand)

- `--game-bin <path>` — override auto-detect
- `--json` — emit JSON to stdout (default: human-readable text)
- `--quiet` — suppress `[cccp-ctl] $ <cmd>` echo lines on stderr
- `--out-dir <path>` — direct temp / output dirs here (default: system temp)
- `-h, --help` — subcommand-specific help
- `-V, --version` — print version

## Exit codes

| Code | Meaning |
|---|---|
| 0 | success / MATCH |
| 1 | test failed / DIVERGED |
| 2 | usage error |
| 64 | subcommand known but not yet implemented (stub) |

## Architecture

- **Standalone binary, no engine dependencies.** Links only stdlib + the vendored
  `nlohmann/json` header at `external/sources/nlohmann_json-3.12.0/`. Mirrors
  `Source/CI/DeterminismCheck.cpp`'s no-engine-deps pattern + the
  `Source/System/FixedPointTests.vcxproj` standalone-executable precedent.
- **Subprocess-orchestrates the game binary** via the existing `-scenario` and
  `-determinism-check` modes. Doesn't link any engine code.
- **`CwdGuard`** chdirs to `gameBin.parent_path()` before `std::system` / `popen`
  and restores after. The game binary references data files as `Data/...`
  relative paths, so its CWD must be the worktree root or it crashes with
  "RTE Assert! Failed to open data file 'Data/Tests.rte/Index.ini'!".
- **Game-binary auto-detect** walks up to 8 parent dirs from `cccp-ctl`'s
  location and at each level tries `{Cortex Command,CortexCommand,cccp}.exe` +
  `_Bin/x64/{Final,Release,Debug,Debug Minimal,Debug Full}` + `_BinTests/x64/*`
  + `build/` + `builddir/`. Stops early at repo root (detected by `RTEA.sln`
  / `.git` / `external/` markers — NOT `meson.build`, which exists at every
  subdir of the recursive Meson build).
- **Echo to stderr.** `[cccp-ctl] $ <cmd>` echo lines go to stderr so stdout
  stays clean for `--json` consumers downstream.
- **Aggregate runners read JSON from `--output` files**, not from polluted
  child stdout (which contains echoes + game-binary messages). The child
  writes its divergence report to `--output`; the parent reads that file.
- **POSIX exit-code decoding.** `pclose` / `std::system` return wait-status on
  POSIX, not exit code; `DecodeExitStatus()` calls `WEXITSTATUS()` to extract
  the actual code. Critical for `test selftest` which checks `rc == 1` to
  verify divergence is signalled correctly.

## Build setup

`cccp-ctl.vcxproj` is **deliberately separate from `RTEA.sln`** — same pattern
as `Source/System/FixedPointTests.vcxproj`. Adding it to the solution would
force every game-build CI step to compile it for no gain.

The Meson target is defined at the repo root `meson.build` alongside
`FixedPointTests` as a standalone `executable()` call with the no-engine-deps
preprocessor flags.

Required external: `nlohmann/json.hpp` at
`external/sources/nlohmann_json-3.12.0/`. Already vendored.

## Gotchas

- **AI-NN scenarios are trust scenarios.** Stock AI does NOT perform the named
  behaviour, so `cccp-ctl test scenario --scenario AI-XX` will exit 1 (FAIL).
  Pass `--trust-selftest` for the test script to drive the behaviour and verify
  the grading harness itself. Per `MILESTONES_COMPLETION.md` MC3 all 12 are
  "normal=FAIL, selftest=PASS" by design.
- **`test scenario` writes tick hashes by default** (V1.2 change). Use
  `--no-tick-hashes` for smaller traces if you don't need per-tick analysis.
  `trace inspect` and `cross-platform-checksum` need them.
- **The game binary opens a visible window** — there's no headless mode (per
  ScenarioRunner.h: the previous hidden-window architecture was reverted
  because the user wants to *see* the AI play). Scenarios self-terminate on
  activity end with a ~1.5s settle wait. Minimum runtime ≈ startup (~3s) +
  sim ticks + 1.5s.
- **`info game-bin` is your first stop** if anything fails — it shows where
  `cccp-ctl` looked + whether the binary was found, with a helpful search-path
  hint on miss.
- **Setting `CCCP_CTL_DEBUG_SEARCH=1`** in the environment makes
  `AutoDetectGameBin` print each path it tries on stderr. Useful when the
  auto-detect refuses to find a binary you can see is there.

## Where to read more

- **`engine.html` T-VALIDATION card** — the full design + V1.x changelog +
  deferred V2 items.
- **`tracker.html` TR-VALID entry** — status summary.
- **`Source/CI/DeterminismCheck.cpp`** — the orchestrator pattern `cccp-ctl`
  wraps (its `-determinism-check` mode is the workhorse).
- **`Source/System/ScenarioRunner.{h,cpp}`** — what `<bin> -scenario X` does:
  parses CLI args, runs the activity, writes the JSON report.
- **`MILESTONES_COMPLETION.md` MC1–MC10** — what the M0–M4 verification stack
  looks like and what passes today.

## Deferred to V2

Explicit non-goals for V1.x, recorded for future:

- `--parallel N` (replay-determinism + thread-matrix oversubscription mode).
- `--timeout <sec>` (needs platform-specific spawn; `std::system` can't kill).
- `test mod-corpus` (needs a standalone mod-corpus runner that doesn't exist).
- `trace bisect` (minimal reproducer — halve ticks until divergence localised).
- `ci baseline` + `ci compare-pr` (regression detection vs. stored baseline).
- `watch determinism` (file-watcher-driven continuous run).
- Slack / Discord webhook hooks for nightly failure alerts.
- Read the scenario list from `Data/Tests.rte` at runtime instead of the
  hardcoded `kScenarios` table (needs an INI parser).
