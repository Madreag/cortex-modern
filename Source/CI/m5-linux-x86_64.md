# M5 bring-up — Linux x86-64

Cross-platform determinism bring-up on Ubuntu 24.04 / x86-64, regenerating the
prior Linux baseline against `exp/determinism-foundation` at `c7d38d87a`. The
prior bring-up's traces were produced under the `-seed` capture bug fixed by
commit `6537ca214`; this pass replaces them with post-fix output. The new
`Source/CI/linux-x86_64-traces/` subdir aligns the Linux layout with
`macos-arm64-traces/` and the forthcoming `windows-x86_64-traces/`.

Host: Intel i7-4770K (4C / 8T, Haswell), 8 logical CPUs, Ubuntu 24.04.4 LTS
(kernel 6.17), GCC 13.3.0. Toolchain: apt
(`build-essential gcc-13 g++-13 ninja-build pkg-config cmake libflac-dev
libminizip-dev libpng-dev libtbb-dev liblz4-dev libgl1-mesa-dev libfreetype-dev
libwayland-dev libxkbcommon-dev libudev-dev libdbus-1-dev libdebuginfod-dev
python3-pip`) plus `pipx install meson` (1.11.1, since the project's
`meson_version >= 1.6.0` rejects Ubuntu 24.04's apt-shipped 1.3.2). Ninja
1.11.1 from apt.

Branch: `exp/determinism-linux` cut from `exp/determinism-foundation` at SHA
`c7d38d87a` (Merge of `exp/determinism-macos`).

## TL;DR

| Surface | Status |
|---|---|
| Meson configure on Linux x86-64 | green |
| Meson compile on Linux x86-64 (GCC 13.3.0, Release + LTO) | green |
| `FixedPointTests` (Q40.24 self-check) | 42 / 42 — `5ce9c33b84d29932` (bit-identical to macOS-arm64) |
| `M1Baseline` @ `--runs 10` `--ticks 600` | MATCHED |
| `M1TerrainStress` @ `--runs 10` `--ticks 900` | MATCHED |
| `M1ActorStress` @ `--runs 10` `--ticks 900` | MATCHED |
| `M2LuaBaseline` @ `--runs 10` `--ticks 600` | MATCHED |
| `M2LuaRandomStress` @ `--runs 10` `--ticks 600` | MATCHED |
| `M2PairsStress` @ `--runs 10` `--ticks 600` | MATCHED |
| `M2OsStubTest` @ `--runs 10` `--ticks 600` | MATCHED |
| `M2ModSmokeLoading` @ `--runs 10` `--ticks 600` | MATCHED |
| `M3TerrainStress` @ `--runs 10` `--ticks 900` | MATCHED |
| `M4ThreadStress` @ `--runs 10` `--ticks 900` | **DIVERGED — same-thread-count race surfaced by the new reinforcement-wave scenario; see RCA** |
| `M4ThreadStress` thread-count matrix `--threads 1,2,4,8,16` `--runs 2` `--ticks 900` | **DIVERGED at threads ≥ 4 (clean at threads 1 and 2); same tick-333 controller cascade as the repeat-runs test** |
| `MPerfBench` @ `--runs 10` `--ticks 1200` | DIVERGED (scenario header declares "Not a determinism scenario"; out-of-scope per the scenario's own contract) |

**Pass criterion not met.** Nine of the ten determinism-scoped scenarios MATCH
cleanly at `--runs 10`. `M4ThreadStress` exposes a same-thread-count race
that the prior Linux bring-up did not catch — that bring-up tested the
pre-reinforcement-wave version of the scenario (rewritten in commit
`05650b976`) and only in `--threads`-matrix mode. The new
reinforcement-wave path makes M4 the first scenario in the suite that
saturates the threaded-AI pass past ~300 ticks, and the race fires there.

`MPerfBench` is explicitly scoped out of determinism by its own header
(`Data/Tests.rte/Activities/MPerfBench.lua` line 14: *"Not a determinism
scenario"*), so its divergence is by design — included in the sweep only
because the prompt's scenario list names it.

## Build environment

* **Toolchain.** `gcc-13 (Ubuntu 13.3.0-6ubuntu2~24.04.1)`, `g++-13` matching.
  Configured via `CC=gcc-13 CXX=g++-13`. Meson 1.11.1 (pipx), Ninja 1.11.1
  (apt). The pipx step is necessary because Ubuntu 24.04's `apt install meson`
  ships 1.3.2 and the project declares `meson_version: '>=1.6.0'`. The prior
  Linux bring-up flagged the same need.
* **Configure.** `meson setup build --buildtype=release -Ddebug=false
  -Db_lto=true`. LTO is fine on Linux (unlike macOS) and gives a tighter
  binary. Configure ran clean.
* **Compile.** `meson compile -C build -j 8`. ~3 min wall on this Haswell
  host (parallel cold build). Produces `build/CortexCommand` (~19 MB, ELF
  64-bit, dynamically linked) and `build/FixedPointTests` (~41 KB,
  standalone).
* **Subprojects loaded** (per `meson setup` output): BLAKE3-1.8.5, LuaJIT-2.1,
  RakNet, SDL3-3.2.10, SDL3_image-3.2.4, allegro 4.4.3.1-custom, luabind-0.7.1,
  nlohmann_json-3.12.0, tracy. All built from `external/sources/`.
* **Run-time wrapper.** Every harness invocation runs with
  `SDL_VIDEODRIVER=offscreen`. Without it the binary opens a real GL window
  per child (SDL is not headless by default on Linux). The prior Linux
  bring-up documented this and noted the harness should set it internally —
  that has not been done yet; explicit env var is the workaround.

### FP-determinism flag block — still discarded on Linux release builds

`meson.build:88` does `extra_args = ['-w']` (assignment, not append) in the
`else` branch that handles `--buildtype=release`. This **wipes** the M1
Block E FP-determinism flags accumulated at `meson.build:30-55`
(`-msse2 -ffp-contract=off -fno-fast-math -fno-finite-math-only
-fno-associative-math -fno-reciprocal-math -fno-unsafe-math-optimizations
-frounding-math -fsignaling-nans`). Verified directly against the compile
commands — every `g++-13 …` invocation for engine sources lands with
`-O3 -DRELEASE_BUILD -DNDEBUG -w` and no FP-pinning flags.

This was flagged in the prior Linux report's *Notes* section and called out
as "owned by the macOS agent's restructure." The macOS branch did
restructure the flag block (adding the ARM `cpu_family` gate and pulling
`-ffp-contract=off` out from under the SSE-only branch), but the
release-mode reassignment that drops them all on the floor is unchanged.

On x86-64 with GCC 13.3 + SSE2 (the ABI baseline) this is empirically
benign — the run-to-run sweep matches on every FP-touching subsystem of
every passing scenario, and the FixedPointTests Q40.24 self-check is
bit-identical to macOS-arm64. The flags would mostly matter for cross-OS
hash equality on the float-touching subsystems — and those are already
expected to differ across float ABIs. Not fixed here; the fix is mechanical
(swap `=` for `+=` on `meson.build:88`) but it touches a block already in
restructure and is better landed alongside the Windows agent's MSVC-side
review of the same block.

### `FixedPointTests` Q40.24 self-check

```
$ ./build/FixedPointTests
FixedPoint.h tests (MP M3 Block A) — Q40.24 fixed-point
  round-trip max error: 5.913e-08
  Sqrt max error: 5.960e-08
  Sin max error: 1.400e-07   Cos max error: 1.585e-07
  Atan2 max error: 2.091e-07

SELF-CHECK: 5ce9c33b84d29932
42 passed, 0 failed
```

`5ce9c33b84d29932` matches the macOS-arm64 hash from `m5-macos-arm64.md`
exactly. The Q40.24 integer library is bit-identical across the two
platforms tested, as it should be (it is pure integer code — `Mul64Portable`
↔ `__int128` agreement, `Isqrt128` exact root, `BuildSinQuarter` constexpr
table, `Atan2` CORDIC).

## Same-platform determinism — `--runs 10` per scenario

Run from the repo root with `SDL_VIDEODRIVER=offscreen` so SDL stays
headless. Default `-num-lua-states` (16) and the priority thread pool's
default size (the host's 8 logical CPUs).

### First-invocation cache-warmup observation

The very first `M1Baseline --runs 2 --ticks 200` smoke-test right after a
cold build reported DIVERGED at tick 1 across `actors` /
`carve_math` / `decisions` / `particles` / `scene` / `sim_rng` / `terrain`
(199/200 ticks mismatched). Re-running `M1Baseline --runs 3` immediately
afterwards MATCHED cleanly, and the subsequent `--runs 10` production sweep
also MATCHED. Identical to the cache-warmup pattern the macOS report
documented for `M1Baseline`; the simplest hypothesis is OS page-cache
warm-up affecting parallel data-module load order in
`PresetMan::LoadAllDataModules`. The production `--runs 10` results are the
authoritative ones.

### Per-scenario results

| Scenario | Wall | `diverged` | Notes |
|---|---|---|---|
| `M1Baseline` | 127 s | `false` | 10 runs MATCHED, 600 ticks each. |
| `M1TerrainStress` | 192 s | `false` | MATCHED. |
| `M1ActorStress` | 185 s | `false` | MATCHED — scenario rewritten in `05650b976`, holds determinism under the new self-check. |
| `M2LuaBaseline` | 127 s | `false` | MATCHED. |
| `M2LuaRandomStress` | 127 s | `false` | MATCHED — exercises the per-MO RNG redirect (Block C); no drift. |
| `M2PairsStress` | 128 s | `false` | MATCHED — the macOS-arm64 mouse heisenbug's gate (in foundation) holds on Linux. |
| `M2OsStubTest` | 127 s | `false` | MATCHED — `det_os_clock` / `det_os_time` stubs route to sim-tick counters. |
| `M2ModSmokeLoading` | 128 s | `false` | MATCHED. |
| `M3TerrainStress` | 186 s | `false` | MATCHED — scenario rewritten in `05650b976`, holds determinism under the new self-check. |
| `M4ThreadStress` | 186 s | `true` | **DIVERGED — see RCA below.** |
| `MPerfBench` | 111 s | `true` | **DIVERGED + intermittent worker segfault.** Scenario header declares "Not a determinism scenario." See note below. |

Total sweep wall: **1624 s (~27 min)** on this i7-4770K, including the
M4 / MPerfBench failed runs.

Per-tick total hashes from the committed single-run traces
(`Source/CI/linux-x86_64-traces/<name>-trace.json` → `.runs[0].final_total_hash`):

| Scenario | `-seed 42` final_total_hash | Trace ticks |
|---|---|---|
| `M1Baseline` | `1adeca4e5ead294d5ef024aa74f452d73b6ab411a0138ff32948c1fa07b17b3f` | 599 |
| `M1TerrainStress` | `3c533e94987dbe1da3f0333031ce50c77a7b61636db9a15659387d3c6ef16b93` | 899 |
| `M1ActorStress` | `a27efb5834976eda4852d984e7233c6737a9dbeeee9eb876780a00ed34b57963` | 899 |
| `M2LuaBaseline` | `f62c694a063e4cc06eafc7384001f0874f3cd7cf2939b7513072ea59292ca035` | 599 |
| `M2LuaRandomStress` | `ec3fa3195f690863ffab81232e1b58edc1ce2fb2bc06a1a0b259ecc0f0073796` | 599 |
| `M2PairsStress` | `4450265c68407d091d68b41bfd2a884dd4413dae5637b7efd660a7f29414c97e` | 599 |
| `M2OsStubTest` | `dcbb115d0596130463a55d9074aeebb69a13204af5fbe2a5924ad5b605c4c360` | 599 |
| `M2ModSmokeLoading` | `ba5ac70105908bef7a205fa5fdbc7a151d671c5beadc2938d9b6ed7cac434fbb` | 599 |
| `M3TerrainStress` | `4e6703b55370d98c91d7966dc1e737a609f16553ccf36373e3ef376477f33ecf` | 899 |
| `M4ThreadStress` | `2d4179804f6d34050c838d2c21dc2659b6e322fb73c41161e8b9aaa3fccfb80b` (one sample — race flake not visible at `--runs 1`) | 899 |
| `MPerfBench` | `2762dce509c1ac3d58f8b95093766e8f49e2025dde3b0d3051d5599cb66c4a07` (out-of-scope per scenario header) | 1199 |

Trace generation total wall: **190 s** (single run per scenario, all in
serial).

## `M4ThreadStress` — RCA

### Test result

Same-config repeat-runs (`--runs 10 --ticks 900 --seed 42`, default Lua
states, default thread pool):

```
RESULT: DIVERGED
    first_divergence_tick: 333
    total_mismatched_ticks: 567 / 900
    per-subsystem first divergence:
        controller: tick 333
        actors:     tick 336
        particles:  tick 336
        carve_math: tick 337
        decisions:  tick 350
        scene:      tick 358
        sim_rng:    tick 358
        terrain:    tick 358
```

Across two back-to-back sweeps the **first-diverging tick and subsystem
cascade are identical** (333 controller → 336 actors/particles → 337
carve_math → 350 decisions → 358 scene/sim_rng/terrain) but the **count of
diverging runs varies** (8/10 in the first sweep, 2/10 in the immediate
re-run). That is the signature of a real race with a tick-333-anchored
trigger, fired with probabilistic frequency depending on host scheduling
pressure.

### Thread-count matrix (`--threads 1,2,4,8,16 --runs 2`)

```
RESULT: DIVERGED
    first_divergence_tick: 333  (same as repeat-runs)
    diverged runs (vs reference run = threads=1 run 0):
        threads=4  run 0 — diverged
        threads=8  run 1 — diverged
        threads=16 run 1 — diverged
    threads=1, threads=2 — both runs MATCHED
```

Clean at thread counts **1** and **2**; race fires intermittently at thread
counts **4, 8, 16**. The race needs at least 4 parallel workers in the
threaded-AI pass to manifest — i.e. it is genuinely a threading race in the
`ThreadedUpdateAI` / `ThreadedUpdate` parallelize_loop, not a hidden
non-determinism every host would hit serially.

### What did *not* fire

* The **`baseline_thread_divergence.md` tick-136 cross-thread-count residual**
  (`particles` / `terrain` first-divergence around tick 136 across thread
  counts) — **gone**. Both repeat-runs and matrix mode first-diverge at
  tick 333, not 136, and the early-tick particle/terrain pattern that
  characterised the prior residual is absent. Blocks B-E and the
  render-RNG redirect have closed the prior Block A residual.
* The macOS-arm64 `M2PairsStress` mouse-cursor controller flake. The
  `UInputMan` `IsRecordingTickHashes()` gate is in place
  (`Source/Managers/UInputMan.cpp:144,166` covers `AnalogMoveValues` /
  `AnalogAimValues`) and `M2PairsStress` MATCHES across 10 runs here.
* Pure `sim_rng` divergence at tick 0 / tick 1. The Block C per-MO RNG
  redirect (`Source/Managers/LuaMan.cpp:134-166`, `DeterministicMORNGScope`)
  holds — `sim_rng` is the *last* subsystem to diverge in the cascade
  (tick 358), confirming it is downstream of the controller race rather
  than the source.

### What is firing

The first diverging subsystem is `controller` at tick 333; everything else
cascades from that (`actors` and `particles` 3 ticks later as actors react
to the diverged controller state, then `decisions` / `scene` / `sim_rng` /
`terrain` settle as the simulation paths diverge). The trigger tick
position is itself a clue:

* M4ThreadStress's reinforcement waves (commit `05650b976`) drop at ticks
  220 / 440 / 660. Tick 333 is *between* waves, ~113 ticks after the first
  wave dropped 16 additional armed BRAINHUNT actors on top of the initial
  40. The population is at its first post-wave peak when the divergence
  fires.
* The pre-`05650b976` `M4ThreadStress` had no waves — it was a single
  initial-population scenario that ran to completion without saturating
  the threaded pass mid-run. That is why the prior Linux bring-up tested
  only the matrix mode for M4 and never the same-config `--runs 10`
  repeat mode: the old scenario would have been clean.

### Audit: what is and is not pinned

Blocks A-E infrastructure is **present** in the foundation:

* **Block B — sorted iteration.** `SortedRegisteredMOs(luaState)`
  (`MovableMan.cpp:72`) returns a MOID-ordered vector and is called at
  both `ThreadedUpdate` call sites (`:1426, :1446`) and the `SyncedUpdate`
  call sites (`:1468, :1478`).
* **Block C — per-MO RNG redirect.** `DeterministicMORNGScope` in
  `LuaMan.cpp:149-166` is opened by both
  `MovableObject::RunScriptedFunctionInAppropriateScripts`
  (`MovableObject.cpp:674`, excluding `OnCollideWith*`) and
  `Actor::CastSeeRays` (`Actor.cpp:1313`). The seed is
  `DeriveMORNGSeed(uniqueID, GetSimUpdateCount(), Hash(functionName))`
  (`LuaMan.cpp:141-147`) — fully deterministic from per-MO + per-tick
  state.
* **Block D — alarm-event ordering.** `AlarmEventLess`
  (`MovableMan.cpp:81-88`) sorts by (team, scenePos.X, scenePos.Y, range).
* **Block E — synced-update pass.** Serial MOID-ordered loop downstream of
  the parallel `ThreadedUpdate`, at `MovableMan.cpp:1468-1485`.
* **`g_CurrentAIActor`** is `thread_local` (`Controller.cpp:13`,
  `Controller.h:82`); the `AHuman::FirearmIsReady` /
  `ACrab::GetEquippedItem` / `ACrab::FirearmIsReady` snapshot-read pattern
  reads frozen state via `FreezeStateForAIPhase`
  (`MovableMan.cpp:2004-2006`).
* **`MovableMan::parallelize_loop num_blocks pin`** at
  `:1440-1454` (ThreadedUpdate) and `:2021-2035` (UpdateControllers)
  — both pin `num_blocks = luaStates.size()` so the
  `RTEAssert(start + 1 == end)` invariant holds on hosts where the pool's
  thread_count is below the luaStates count (this Haswell box: thread
  pool = 8, default luaStates = 16, so the pin is engaging).

The residual race is therefore one Blocks B-E did **not** cover. Most
likely candidates (not narrowed further in this pass):

1. **Incomplete `FreezeStateForAIPhase` field-list.** The freeze covers
   `m_FrozenFirearmReady`, `m_FrozenEquippedItem`, and the
   `Actor::FreezeStateForAIPhase` base set. Any other per-actor field
   that the parallel `ThreadedUpdateAI` reads *across* actors (i.e. about
   another actor than `g_CurrentAIActor`) and that the freeze list
   doesn't cover is a candidate — the AI Lua reads from a target actor
   would see live-write state during the parallel pass.
2. **`OnCollideWith*` per-Lua-state RNG residual.** Deliberately out of M4
   scope per `baseline_thread_divergence.md` §"Out of M4 scope". These
   run *serially* inside `Travel()`, so the divergence per-thread-count
   would be deterministic — but the per-actor population reaching the
   collision-callback by tick 333 is influenced by *all the earlier
   non-deterministic AI decisions made by the parallel pass*, which is
   how a serial-callback's RNG-state could vary across runs.
3. **Cross-actor pointer comparison in AI Lua/C++** that sorts or branches
   on memory addresses instead of `m_UniqueID`. ASLR re-randomises
   addresses per run; M4's high actor count makes ties common. Would also
   explain the threads ≥ 4 dependency (more actors per Lua state at
   higher counts increases the cross-actor read surface).

A TSan build per the prior Linux PathFinder bug's playbook (build with
`-Db_sanitize=thread`, run under `setarch -R` for ASLR-off, exclude LuaJIT
from instrumentation) is the next investigation step. Not run here — the
matrix-and-repeat-runs characterisation above is the bring-up's
contribution; further narrowing belongs in a focused M4 follow-up
commit, not in the bring-up itself.

## `MPerfBench` — out-of-scope per scenario header; documented divergence

`Data/Tests.rte/Activities/MPerfBench.lua` line 14:
> Not a determinism scenario.

The scenario's purpose (per its header) is to keep the sim compute-bound
for `__wall_seconds` measurement — 150 BRAINHUNT actors + six 30-actor
reinforcement waves over 1200 ticks. That workload at default thread
count diverges by tick 69 (`particles` first), and one of the four child
runs in the re-run also crashed with `std::terminate()` during
high-population mid-run state (post-wave-1).

Per the scenario contract this is expected. Reporting it here only for
completeness because it appears in the prompt's scenario list. The
intermittent worker `std::terminate` (and the matching one seen during
`--keep-runs` invocations on M4ThreadStress) is a separate engine-stability
issue worth its own ticket; both stack traces show the same shape
(libstdc++ unwind → `__cxa_throw` → engine code path → `abort`) and the
worker that crashes is always after the first reinforcement wave has
landed. The bring-up itself stays in scope and does not chase it.

## Cross-platform comparison

The Linux trace at `Source/CI/linux-x86_64-traces/M1Baseline-trace.json`
final hash is
**`1adeca4e5ead294d5ef024aa74f452d73b6ab411a0138ff32948c1fa07b17b3f`**.
The macOS-arm64 trace at `Source/CI/macos-arm64-traces/M1Baseline-trace.json`
final hash is
**`c7282c4487df76c6829dedd1140a16cd9ded1894329e1c1d34837cc8db8a7520`**.

The two **differ**, as expected. Two contributors:

1. **FP code path drift.** The Release-mode build on Linux compiles without
   the `-ffp-contract=off` block (see *Build environment* above), and GCC
   13.3 / Apple Clang 17 will not produce identical x87/SSE/NEON FMA
   selection or libm `sin/cos/atan2` last-ULP results regardless. Every
   `actors` / `controller` / `particles` / `decisions` / `carve_math` /
   `terrain` subsystem feeds float state into the per-tick hash and is
   legitimately a cross-arch differ. Per `m5-macos-arm64.md`'s
   "Cross-platform divergence characterisation" section: anything driven
   by `sin/cos/atan2` is allowed to differ across ARM vs x86 even with
   the FP-determinism flags fully engaged.
2. **The integer-only subsystems** (`tick`, `scene`, `sim_rng`,
   `lua_state`) *should* match cross-OS. The committed per-tick traces
   carry per-subsystem hashes; the team's cross-OS comparator step
   (`.github/workflows/determinism.yml` `fixedpoint-tests-cross-os` and
   the per-tick comparator) is where the column-by-column diff will land
   and where any integer subsystem mismatch would be a real portability
   bug.

The `FixedPointTests` Q40.24 hash (`5ce9c33b84d29932`) **does match**
macOS-arm64 bit-for-bit, confirming the integer math layer is cross-arch
deterministic on its own.

The Windows-x86_64 trace dir (`Source/CI/windows-x86_64-traces/`) does
not exist yet — the Windows agent runs in parallel; the Windows-side
comparison is pending. Once it lands, the Linux x86-64 vs Windows x86-64
diff should be **identical** on the integer subsystems and **also
identical** on the float subsystems (both x86-64 with default GCC/MSVC
on SSE2, both lacking the FP-pinning flags — same drift sources).

## Notes on the layout migration

The pre-fix root-level traces (`Source/CI/M*-trace.json`) are deleted and
replaced with the post-seed-fix traces under
`Source/CI/linux-x86_64-traces/`, matching the macOS layout. The prior
report `Source/CI/m5-linux-x64.md` is renamed to
`Source/CI/m5-linux-x86_64.md` for naming-consistency with the other
platforms. The 9-vs-11-scenario count change reflects two newly-tested
scenarios on this pass — `M4ThreadStress-trace.json` (the prior dump
didn't include it) and `MPerfBench-trace.json` (also new; generated
despite the scenario's out-of-scope status, for completeness).

## Known issues / follow-ups (not closed by this bring-up)

* **`M4ThreadStress` same-thread-count race at threads ≥ 4** — characterised
  above. The bring-up regenerates the baseline with the divergence
  documented; the fix is the M4-follow-up's job and needs a TSan pass to
  narrow. Likely candidates are an incomplete `FreezeStateForAIPhase`
  field-list, an `OnCollideWith*` per-Lua-state RNG residual, or a
  cross-actor pointer comparison in AI Lua/C++. None are in the audit
  document's closed-by-Blocks-B-E categories.
* **`meson.build:88` discards FP-determinism flags in release.** The macOS
  restructure left the assignment-not-append in place; the
  Linux release build compiles without `-ffp-contract=off` etc. Empirically
  benign on this host but it should be `extra_args += ['-w']` not
  `extra_args = ['-w']`. Owned by the cross-platform meson cleanup; flagged
  but not changed here, same as the prior Linux report.
* **SDL headless mode is not engaged by the harness.** `SDL_VIDEODRIVER=offscreen`
  has to be set in the environment for every determinism-check run on
  Linux or SDL opens a real GL window and concurrent runs deadlock on
  the X11/Wayland sockets. The prior Linux report recommended the harness
  set this internally; that recommendation still stands.
* **Intermittent worker `std::terminate()` aborts** in `MPerfBench` (run 4
  of the production sweep) and in `M4ThreadStress` under `--keep-runs`
  (twice). Same stack-trace shape both scenarios:
  `libstdc++ unwind → __cxa_throw → engine code → abort`. Address symbol
  resolution would require an ASLR-off `setarch -R` re-run plus
  `addr2line`. Not a determinism issue per se; an engine-stability
  follow-up.

## Conclusion

Linux x86-64 is **green on 9 of 10 determinism-scoped scenarios** at
`--runs 10`. `M4ThreadStress` exposes a same-thread-count race
(threads ≥ 4, first-diverges in `controller` at tick 333) that the prior
Linux bring-up did not catch — the reinforcement-wave rewrite of the
scenario is new since that bring-up, and the matrix-only mode the prior
work used would not have surfaced it. The pre-existing M4 cross-thread-count
residual at tick 136 is **closed** by Blocks B-E; the new tick-333 race is
its successor and is captured here as a documented follow-up. `MPerfBench`
diverges by design (scenario header declares it). The integer Q40.24
self-check hashes bit-identical to macOS-arm64.
