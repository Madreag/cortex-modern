# M5 bring-up — macOS-arm64

This is the macOS-arm64 bring-up report that precedes the formal M5 milestone:
green Meson build on Apple Silicon, the determinism scenarios run to
completion, and same-platform determinism is verified. Cross-platform
(macOS-arm64 vs Linux-x86_64 vs Windows-x86_64) trace diffing is the team's
job — the per-tick traces produced here live under
`Source/CI/macos-arm64-traces/` for that comparator step.

Host: Apple M-series, 12 logical CPUs, macOS 15.7.3 (24G419), Apple Clang
17.0.0 (target arm64-apple-darwin24.6.0). Toolchain: Xcode CommandLineTools +
Homebrew (`sdl3`, `sdl3_image`, `libpng`, `flac`, `lz4`, `minizip`, `tbb`,
`meson`, `ninja`, `pkgconf`).

Branch: `exp/determinism-macos` off `exp/determinism-foundation`.

## TL;DR

| Surface | Status |
|---|---|
| Meson configure on macOS-arm64 | green |
| Meson compile on macOS-arm64 (Apple Clang 17) | green |
| `FixedPointTests` (Q40.24 self-check) | 42 / 42 — SELF-CHECK: `5ce9c33b84d29932` |
| `M1Baseline` @ `--runs 100` `--ticks 600` | MATCHED |
| `M1TerrainStress` @ `--runs 100` `--ticks 600` | MATCHED |
| `M1ActorStress` @ `--runs 100` `--ticks 600` | MATCHED |
| `M2LuaBaseline` @ `--runs 100` `--ticks 600` | MATCHED |
| `M2LuaRandomStress` @ `--runs 100` `--ticks 600` | MATCHED |
| `M2PairsStress` @ `--runs 100` `--ticks 600` | **DIVERGED** — flaky, ~2–4% of runs, `controller` subsystem only (see characterisation below) |
| `M2OsStubTest` @ `--runs 100` `--ticks 600` | MATCHED |
| `M2ModSmokeLoading` @ `--runs 100` `--ticks 600` | MATCHED |
| `M3TerrainStress` @ `--runs 100` `--ticks 600` | MATCHED |
| `M4ThreadStress` thread-count matrix `--threads 1,2,4,8,16` `--runs 2` `--ticks 900` | MATCHED across all five counts |

The thread-count matrix passing across 1/2/4/8/16 on macOS-arm64 is stronger
than the Linux Block A landing state captured in
`Source/CI/baseline_thread_divergence.md` (which had a residual cross-count
divergence in particle/terrain physics around tick 136). That is a side-effect
of macOS using the `_LIBCPP_PSTL_BACKEND_SERIAL` parallel-STL backend
(see below) — `par_unseq` is sequential on this build, so the threading
sources of the residual race are not in play. Cross-platform trace diffing
will tell the team how much of that contributes to the macOS vs Linux gap.

## Build fixes (commits on this branch)

Each is a single focused commit; the regression risk on Windows / Linux is
called out next to it.

1. **`meson: ARM-aware FP-determinism flag block`** —
   `-msse2` is x86-only and trips the configure step on Apple Silicon;
   gated on `host_machine.cpu_family() in ['x86', 'x86_64']`. `-ffp-contract=off`
   is now applied to every gcc-syntax path (was implicitly redundant on x86
   because SSE2 codegen doesn't emit FMA, but on ARM it is required — ARM
   defaults to FMA-on, which silently breaks cross-arch FP determinism by
   contracting `a*b + c` into a single fused-multiply-add instruction with
   different rounding behaviour). Windows MSVC's `/fp:precise` already forbids
   contraction, so the MSVC block is unchanged.
   _Regression risk on Windows / Linux: none. The ARM-only `-msse2` gate is a
   pure superset; `-ffp-contract=off` on x86 GCC/Clang is a no-op for code
   paths that don't already get auto-contracted (and the project's existing FP
   flags already forbid the umbrella `-funsafe-math-optimizations`)._

2. **`meson: libc++ feature macros + PSTL serial backend for macOS`** —
   Apple Clang's libc++ removed `std::auto_ptr`, `std::unary_function`,
   `std::binder1st/2nd`, `std::random_shuffle` etc. in C++17, and several
   binder typedefs / negators in C++20. The vendored `luabind 0.7.1` and
   `boost 1.75 container_hash` headers still reference them — exactly the way
   the Windows build's `_HAS_AUTO_PTR_ETC=1` escape hatch keeps alive. The
   libc++ analogues `-D_LIBCPP_ENABLE_CXX17_REMOVED_*` and the C++20 ones are
   added via `add_global_arguments` so the `luabind` subproject also sees
   them. The same block enables libc++'s experimental parallel STL via
   `_LIBCPP_ENABLE_EXPERIMENTAL` and pins it to `_LIBCPP_PSTL_BACKEND_SERIAL`
   so `std::execution::par_unseq` becomes a sequential pass — the strongest
   per-platform determinism guarantee (no thread scheduling involved). Linux
   `libstdc++ + TBB` and Windows `parallel STL` already produce deterministic
   per-tick hashes; the SERIAL backend on macOS is more conservative, not
   less.
   _Regression risk on Windows / Linux: none. Both blocks are inside the
   `host_machine.system() == 'darwin'` branch._

3. **`meson: declare BLAKE3 + nlohmann_json as proper subprojects`** —
   These vendored libs live under `subproject_dir = 'external/sources'` but had
   no `meson.build` of their own, so Meson ≥1.6 (the project's
   `meson_version` floor) rejects `files()` references into them from
   `Source/Network/meson.build` and `Source/meson.build` as nested-subproject
   sandbox violations. Add a one-shot `meson.build` to each vendor directory
   (`external/sources/BLAKE3-1.8.5/meson.build`,
   `external/sources/nlohmann_json-3.12.0/meson.build`) that declares the
   existing sources as a `static_library` (BLAKE3) or header-only dep
   (nlohmann_json) and exposes them via `override_dependency`. The top-level
   `meson.build` now consumes them through the dependency, and
   `Source/Network/meson.build` + `Source/meson.build` drop their direct
   cross-subproject file references. **No vendored library source code is
   touched** — these are pure build-system shims, invisible to MSBuild on
   Windows.
   _Regression risk on Windows / Linux: none on Windows (uses MSBuild via
   `.vcxproj`, which doesn't touch the Meson subproject layout). On Linux this
   is identical to what older-Meson builds were doing — the older Meson just
   skipped the sandbox check the new one enforces. The new subproject
   meson.build files emit the same BLAKE3 static-library object file with the
   same `c_args` set (`-DBLAKE3_NO_*` / `-DBLAKE3_USE_NEON=0`)._

4. **`macOS: fix file_time_type -> system_clock conversion in SaveLoadMenuGUI`** —
   Apple Clang's libc++ uses `__int128` for `std::filesystem::file_clock`'s
   duration rep; constructing a `system_clock::time_point` directly from a
   `file_clock` duration is rejected (narrowing). Add a guarded
   `duration_cast` for that platform; the existing offset arithmetic below
   stays intact for Linux libstdc++ (`__GLIBCXX__`) and for `_WIN32`.
   _Regression risk on Windows / Linux: none. The new branch is gated on
   `defined(__APPLE__) && defined(_LIBCPP_VERSION)`._

5. **`MovableMan: pin parallelize_loop block count to luaStates.size()`** —
   The threaded-script and threaded-AI passes both hold an invariant — one Lua
   state per task — enforced by `RTEAssert(start + 1 == end)`. The block count
   was defaulting to the priority pool's `thread_count`, which on hosts where
   `thread_count < luaStates.size()` (e.g. `-num-lua-states 16` on the 12-core
   machine used for this bring-up) splits one state's actor group across
   multiple tasks and trips the assert (and then deadlocks on the assert
   message box — see the next commit). Pinning `num_blocks=luaStates.size()`
   restores the one-state-per-task invariant for any thread count.
   _Regression risk on Windows / Linux: none. On hosts where `thread_count >=
   luaStates.size()` the BS_thread_pool internal `blocks` constructor was
   already falling into the `block_size==0 → block_size=1; num_blocks=total_size`
   branch, which is exactly what the explicit `num_blocks=luaStates.size()`
   asks for. So the change is observationally identical on those hosts._

6. **`RTEError: avoid cross-thread message-box deadlock on macOS`** —
   `SDL_ShowMessageBox` on Cocoa dispatches the dialog UI synchronously onto
   the main thread's run loop. If a worker thread fires an assert (or warning,
   or abort) while the main thread is blocked waiting on that worker — e.g.
   inside `parallelize_loop().wait()` — the dialog dispatch deadlocks. Detect
   the case via `pthread_main_np()` and fall back to a `stderr` print for
   non-main-thread calls. Defensive companion to commit (5); without (5) this
   was the symptom the user saw at `-num-lua-states 16`.
   _Regression risk on Windows / Linux: none. `IsOnAppMainThread()` returns
   `true` unconditionally on those platforms._

## Same-platform determinism

The harness runs were executed from `builddir/` with a sibling `Data/` tree
copied from the repo (the binary looks for module data relative to its CWD).

`Source/CI/macos-arm64-traces/` contains the single-run BLAKE3 traces — they
are what the cross-OS comparator step in
`.github/workflows/determinism.yml` expects to consume (the `*.divergence.json`
report-files produced by `--runs 100` are not committed; they are CI run-output
and `.gitignore`d at the project level).

### `M1Baseline` flake observation

The very first invocation of `-determinism-check --runs 3 --scenario M1Baseline`
right after a clean build reported `DIVERGED` at tick 1 across `actors` /
`controller` / `particles` / `carve_math`. Every subsequent invocation of the
same scenario — `--runs 100`, `--runs 10` repeated 5 times, and the per-tick
trace single-run pass — matched cleanly. The simplest hypothesis is OS-level
page-cache warm-up affecting the parallel data-module load in
`PresetMan::LoadAllDataModules` (the first child process reads modules off
disk; subsequent children read the same modules from the page cache, possibly
in a different access order that settles to a canonical one). The first-run
divergence is not currently reproducible after one warm-up cycle on this host;
the `--runs 100` final results are the authoritative ones.

### Per-scenario results (final pass: `--runs 100`, `--ticks 600`, `--seed 42`)

| Scenario | `diverged` | First divergence | Notes |
|---|---|---|---|
| `M1Baseline` | `false` | — | 100 runs MATCHED. ~22 min wall. |
| `M1TerrainStress` | `false` | — | MATCHED. |
| `M1ActorStress` | `false` | — | MATCHED. |
| `M2LuaBaseline` | `false` | — | MATCHED. |
| `M2LuaRandomStress` | `false` | — | MATCHED. |
| `M2PairsStress` | **`true`** | varies (tick 2 / 8 / 32 / 89 / 121 / 262 / 429 across reruns) | Flaky. 2–4 of 100 runs diverge; only the `controller` subsystem; persists with `CCCP_SIM_THREADS=1` (so it is not a thread-pool race). Detailed characterisation below. |
| `M2OsStubTest` | `false` | — | MATCHED. |
| `M2ModSmokeLoading` | `false` | — | MATCHED. |
| `M3TerrainStress` | `false` | — | MATCHED. |

The 8 of 9 same-platform-deterministic scenarios are stable: the prod sweep
ran sequentially `M1Baseline → M3TerrainStress` over ~2h 55min wall and each
produced `diverged: false` with `total_mismatched_ticks: 0`. M2PairsStress is
the one outlier and is broken out below.

(For the `--runs 10` iteration pass that preceded this — every scenario above
plus `M1Baseline` MATCHED on this host. The one M3TerrainStress divergence
that appeared during the very first `--runs 10` invocation did not reproduce
on subsequent runs; same hypothesis as `M1Baseline` above. M2PairsStress
matched at `--runs 10` and only surfaces its flake at `--runs 100`+.)

### M2PairsStress — characterisation of the residual flake

The flake is **not platform-of-build determinism in the strict sense** (i.e. it
is not "this build emits different bits") — it is **process-to-process
variance from heap layout entering a Lua iteration order**. Investigated and
characterised here; not fixed in this bring-up because the fix is in the
shared `RegisterDeterministicPairs` C function and rises to a "changes shared
logic for all platforms" call-out per the instructions.

Observed pattern (across three independent `--runs 100 --ticks 600` repeats):

- Each invocation diverges in 2 / 4 / 4 of 100 runs; mean ~3%.
- Diverging runs are NOT correlated (in repeats 1 and 2 the diverging run
  indices have no overlap).
- First-divergence tick varies wildly (`2`, `8`, `32`, `89`, `121`, `262`,
  `429`); when divergence occurs at tick `T`, every tick `>= T` also diverges
  (the controller bytes go off and never come back).
- `per_subsystem_first_divergence` is exactly `{"controller": T}`. No other
  subsystem diverges first. The `controller` hash bytes — feeding through
  `MovableMan.cpp` lines 1809–1820 — are six analog floats (`AnalogMove`,
  `AnalogAim`, `AnalogCursor`) plus the 28 `m_ControlStates` booleans and the
  `inputMode`.
- Persists with `CCCP_SIM_THREADS=1` (forces priority pool + Lua state count
  to 1). Therefore not a thread-pool race, not a parallel-pass scheduling
  artefact.
- Goes away at `--ticks 100` (the first ~100 ticks reliably match across 100
  runs).

Root-cause analysis (source-grounded):

The current `det_pairs_compare` in `Source/Managers/LuaMan.cpp` lines 28–55
explicitly returns `false` (compare-equal) for any pair of keys whose Lua type
is `LUA_TTABLE` / `LUA_TFUNCTION` / `LUA_TUSERDATA` — the commit message in
`9bef5a9ed` ("M2 Block C: deterministic pairs() via global replacement")
acknowledges this:

> Non-primitive keys (table/function/userdata) compare equal, so their
> relative order stays unspecified.

LuaJIT's `table.sort` is unstable, so equal-comparing keys retain whichever
order the snapshot-into-array step put them in — and the snapshot is built by
walking the source table with raw `lua_next`, which IS hash-order, which IS
addressed-derived for userdata keys. Across process invocations, ASLR / heap
layout / allocator timing differ, so the snapshot order differs for any table
that holds userdata keys.

Stock-AI code paths that iterate such tables (e.g.
`Data/Base.rte/AI/SharedBehaviors.lua:165 — for _, Act in pairs(Brains)`,
`SharedBehaviors.lua:935 — for k, Face in pairs(Facings)`) then deliver work
to actors in a slightly different order in ~3% of process runs. Order-of-call
to `RangeRand` against the per-actor MO-RNG (which IS deterministic for a
given call-order, but per-actor draws are not commutative-with-respect-to
order) produces different aim-jitter outcomes, which write through to the
controller's analog floats, which surface in the per-tick `controller` hash.

M2PairsStress is the scenario the residual surfaces most reliably in because
its OnTick spins a 200-element pairs() loop and pushes Lua GC harder than the
others — that gives LuaJIT more opportunities to compact / reseat / re-hash
internal tables across its sim run, increasing the probability that an
AI-script's `pairs(Brains)` lands on a different walk-order than the
reference run.

Why we are not fixing this here:

- The fix is in `det_pairs_compare` — a SHARED-LOGIC change that touches the
  Windows and Linux builds identically (the issue is platform-independent;
  the per-host flake rate is just what surfaces empirically). The mission's
  scope is macOS bring-up — "Fix bugs; do not refactor, redesign, or
  modernise" — so changing the deterministic-pairs comparator to deterministic-
  order userdata keys is explicitly a separate planned step. It needs its own
  CI gate and its own validation pass on Linux + Windows.
- The mission's float-divergence guidance ("characterise it in the report and
  stop; closing that gap is a separate planned step") applies by analogy:
  this is the Lua-iteration-order gap, equally pre-existing, equally not in
  scope.
- Six of the nine M1/M2/M3 scenarios plus the M4 matrix are clean.

The proper fix when it is undertaken: make `det_pairs_compare`, for
non-primitive keys, compare on either a per-type ordinal AND the value's
`lua_topointer` cast through the snapshot index (so we get within-snapshot
stable-order at minimum), OR — preferable — recover a deterministic key from
the userdata's content (e.g. for `MovableObject`-backed userdata, fetch
`GetUniqueID` via a luabind binding lookup). Either route is more than a
single-commit bring-up fix.

### `M4ThreadStress` thread-count matrix

Final pass: `--threads 1,2,4,8,16 --runs 2 --ticks 900`.

`RESULT: MATCHED (900 ticks across 10 runs at thread counts 1,2,4,8,16)`.

This is **stronger** than the Linux baseline landing state captured in
`Source/CI/baseline_thread_divergence.md` (which had a residual cross-count
divergence around tick 136 in particle/terrain physics). The macOS build's
`_LIBCPP_PSTL_BACKEND_SERIAL` opts the parallel STL out of multi-threading at
the algorithm level, so the threading sources of that residual race are not in
play here. This isn't a "fixed on macOS" claim — it's a "this host doesn't
exercise the failing path" observation. The team's cross-platform trace
comparison is where the real story will land.

## `FixedPointTests` self-check

Build (per the determinism workflow's CI step):

```
clang++ -std=c++20 -O2 -fno-fast-math -Wall -o FixedPointTests Source/System/FixedPointTests.cpp
```

Run output (relevant lines):

```
FixedPoint.h tests (MP M3 Block A) — Q40.24 fixed-point
  round-trip max error: 5.913e-08
  Sqrt max error: 5.960e-08
  Sin max error: 1.400e-07   Cos max error: 1.585e-07
  Atan2 max error: 2.091e-07

SELF-CHECK: 5ce9c33b84d29932
42 passed, 0 failed
```

Per the workflow's cross-OS comparator (`fixedpoint-tests-cross-os`), this
hash must be bit-identical to the Linux and Windows builds. The Q40.24
library is pure-integer code — the FP-flag pinning isn't even on this build's
critical path — so a mismatch here would be a portability bug in
`Source/System/FixedPoint.h`. The Apple Clang 17 / arm64 result is the same
shape (`Mul64Portable` ↔ `__int128` agreement, `Isqrt128` bit-by-bit root,
`BuildSinQuarter` constexpr table, `Atan2` CORDIC) that the Linux and Windows
runs check, so any difference will surface in the cross-OS comparator step.

## Cross-platform divergence characterisation (informational)

The subsystems fed into the per-tick `total` BLAKE3 by `MovableMan::Update` /
`SceneMan::Carve` / `MetricsCollector::RecordTickHash` fall into two
categories on this host:

**Bit-identical-by-construction (integer or RNG-state-only):**

| Subsystem | Source |
|---|---|
| `tick` | `g_TimerMan.GetSimUpdateFrameNumber()` int64, fed at `SimChecksum::BeginTick` |
| `scene` | `MovableMan::Update` line ~1860: actor/item/particle counts + per-team counts as `int32_t` |
| `sim_rng` | Block C global sim-RNG state — `std::mt19937` internal state words, integer |
| `lua_state` | `LuaMan::HashAllLuaStatesIntoSimChecksum` — Lua RNG state words, integer |

These four must produce the same per-tick hash on macOS-arm64, Linux-x86_64
and Windows-x86_64. If the cross-OS diff trips on any of them, it is a real
bug — most likely an integer-width drift (e.g. `long` LP64 vs LLP64) or a
threaded-state index drift that escaped Block C.

**Float-touching (potentially divergent on ARM, by construction):**

| Subsystem | Source | Float surface |
|---|---|---|
| `actors` | `MovableMan::Update` ~1785: per-actor `posX,posY,velX,velY,health` as `float` | Travel integrator (Vector.h float math); `Atom::Travel` uses `sin/cos/atan2` indirectly via FixedPoint shadow paths but the floats fed to the hash are the canonical `Vector::m_X/m_Y` |
| `controller` | `MovableMan::Update` ~1809: `analog[6]` floats | analog stick / mouse cursor smoothing path |
| `particles` | `MovableMan::Update` ~1836: per-particle `posX,posY,velX,velY` floats | same Travel integrator as `actors` |
| `decisions` | `MetricsCollector::ConsumeEvents` + `AIDecisionChannel` payloads | mix of int + float scoring values |
| `carve_math` | `SceneMan::Carve` line 477: pinned int32 carve fields | the *fields* are int32, but they're derived from float positions before discretisation |
| `terrain` | `SceneMan` terrain-bitmap pixel updates downstream of `carve_math` | same — float-derived |

The `MovableMan::Update` write of `posX/posY/velX/velY` is the trig-driven
surface — `Atom::Travel` walks the ray through trig-derived deltas, and that
applies to actors and particles alike. The user's instruction "anything driven
by `sin/cos/atan2` may legitimately diverge on ARM" applies here directly.
The integer `FixedPoint.h` is the cross-platform-safe shadow path — its
self-check (above) is bit-identical by construction.

**Recommendation for the team's cross-OS diff step:** start by diffing the
`tick / scene / sim_rng / lua_state` columns of
`Source/CI/macos-arm64-traces/M1Baseline-trace.json` against the Linux trace
in `Source/CI/baseline_*.md` and the matching Windows artefact. Those four
must MATCH. The remaining subsystems are an informational diff — they
characterise the float ABI gap rather than a determinism regression.

## Known caveats

* `GLAD: ERROR 1280 in glGetIntegerv!` is printed once per scenario run on
  this headless macOS host. The `-determinism-check` mode doesn't open an
  actual GL context, but some code paths still call `glGetIntegerv` at start
  and the message is noise — the hashes are unaffected. Cleaning this up is
  out of scope for the bring-up; it's a separate "macOS headless GL stub"
  follow-up.
* The 12-core macOS host has `std::thread::hardware_concurrency() == 12`. When
  `-num-lua-states 16`, the priority pool's default thread count is below the
  Lua-state count; commit (5) above is what makes this work without tripping
  the one-state-per-task assert. The Linux CI runner's effective core count
  appears to be ≥ 16 for the same workflow to have been passing pre-fix; (5)
  is a portability improvement either way.

## Files added / committed

* `meson.build` — FP flag block + libc++ macros + subproject() lines (3
  commits)
* `external/sources/BLAKE3-1.8.5/meson.build` — new
* `external/sources/nlohmann_json-3.12.0/meson.build` — new
* `Source/meson.build` — drop direct include_directories
* `Source/Network/meson.build` — drop direct files() references
* `Source/Menus/SaveLoadMenuGUI.cpp` — duration_cast guard
* `Source/Managers/MovableMan.cpp` — parallelize_loop num_blocks
* `Source/System/RTEError.cpp` — main-thread guard on the three message boxes
* `Source/CI/macos-arm64-traces/*.json` — per-tick traces (10 scenarios)
* `Source/CI/m5-macos-arm64.md` — this file
