# Linux x86-64 determinism bring-up

Cross-platform bring-up of the `exp/determinism-foundation` work on Linux
x86-64, preceding the formal M5 milestone. The determinism foundation (M0–M4)
was verified on Windows x64 only; this report covers the first off-Windows
build and same-platform determinism run.

Branch: `exp/determinism-linux` (cut from `exp/determinism-foundation`).

## Environment

| | |
|---|---|
| OS | WSL2, Ubuntu 24.04, x86-64 |
| Compiler | GCC 13.3.0 |
| Build system | Meson 1.11.1 + Ninja 1.11.1 |
| Build type | `release` (matches the `determinism-linux` CI job) |
| Binary | `builddir/CortexCommand` — ELF 64-bit x86-64, ~22 MB |

**Meson version note.** The project requires `meson_version >= 1.6.0`. Ubuntu
24.04's `apt` ships Meson 1.3.2, which fails the version check outright — the
CI job's `apt-get install meson` step will not produce a usable Meson on a
current Ubuntu image. Meson 1.11.1 was installed via `pipx` for this bring-up
(matching the macOS agent's toolchain). This is a toolchain observation for
the team; no repo change was made for it.

## Build

```
meson setup builddir --buildtype=release
meson compile -C builddir
```

The full engine and the `FixedPointTests` target build and link cleanly. The
GCC-strictness breakage anticipated for the bring-up (missing `<cstdint>` /
`<cstring>`, integer-literal suffixes, template/constexpr strictness) did
**not** materialise — the release build has no hard errors, and the engine
also compiles under `debugoptimized` (used for the sanitizer build below).
The anticipated pre-existing `GUIRect` Meson break did not appear either.

### Build fix — BLAKE3 as a Meson subproject

*Commit `5e9452512` — `meson: build BLAKE3 as a subproject for the Linux build`.*

`meson setup` failed before any compilation:

```
Source/Network/meson.build:7:17: ERROR: Sandbox violation:
Tried to grab file blake3.c from a nested subproject.
```

`Source/Network/meson.build` compiled BLAKE3 by calling `files()` on
`../../external/sources/BLAKE3-1.8.5/*.c`. `external/sources` is the Meson
`subproject_dir`; Meson forbids the main project from grabbing source files
out of a subproject directory. This is a hard error on **both** Meson 1.6.0
(the project's declared minimum) and 1.11.1 — it is a genuine bug in the
determinism work's Meson plumbing, not version strictness. BLAKE3 was the
only compiled library under `external/sources/` without an in-tree
`meson.build`; every other one (LuaJIT, RakNet, SDL3, zlib-ng, libpng,
minizip-ng, luabind, tracy …) ships one and is loaded via `subproject()`.

Fix: added `external/sources/BLAKE3-1.8.5/meson.build` (a `project()` +
`static_library` + `declare_dependency`), loaded it via
`subproject('BLAKE3-1.8.5')` in the root `meson.build`, and linked it through
`blake3_dep`; removed the `files()` / `static_library` block from
`Source/Network/meson.build`. The `-DBLAKE3_NO_SSE2/SSE41/AVX2/AVX512`
and `-DBLAKE3_USE_NEON=0` defines are preserved verbatim, so BLAKE3 still
compiles its portable C path only and hashes bit-identically across targets.

**Regression risk: none.** Windows builds via `RTEA.vcxproj` / MSBuild, which
never invokes Meson — the Windows build is structurally untouched. For the
Linux/macOS Meson build the change is structural only: the BLAKE3 object code
and compile flags are unchanged, and no BLAKE3 source or header is modified.

## FixedPointTests (Q40.24 fixed-point self-check)

Built standalone exactly as the `fixedpoint-tests-linux` CI job does:

```
g++ -std=c++20 -O2 -msse2 -fno-fast-math -Wall -o FixedPointTests \
    Source/System/FixedPointTests.cpp
```

Clean compile (no errors, no warnings). Result:

```
SELF-CHECK: 5ce9c33b84d29932
42 passed, 0 failed
```

No `FixedPoint.h` portability bug found. The `SelfCheckHash()` path is pure
integer code (`Fixed`, `Sqrt`, `Sin`, `Cos`, `Atan2`, `FixedVector` — no
`libm`), so it is expected to hash identically on every platform; the
Windows↔Linux comparison itself is the CI `fixedpoint-tests-cross-os` job's
job. The build did not define `NDEBUG`, so `RTE_FIXED_CHECK` asserts were
live during the run and none tripped. All 42 correctness checks pass,
including the 200 000-iteration `Mul64` portable-vs-`__int128`-intrinsic
bit-agreement test and the `Isqrt128` exactness test.

## Same-platform determinism

Each scenario was run through the determinism harness on this machine:

```
./builddir/CortexCommand -determinism-check --scenario <S> \
    --ticks 600 --seed 42 --runs 100 --output <S>-divergence.json
```

`diverged: false` means the per-tick BLAKE3 trace is bit-identical run-to-run
on Linux x86-64. All runs were done headless with `SDL_VIDEODRIVER=offscreen`
(see *Notes*).

| Scenario            | Runs | Result    |
|---------------------|------|-----------|
| M1Baseline          | 300  | MATCHED   |
| M1TerrainStress     | 100  | MATCHED   |
| M1ActorStress       | 100  | MATCHED   |
| M2LuaBaseline       | 100  | MATCHED   |
| M2LuaRandomStress   | 100  | MATCHED   |
| M2PairsStress       | 100  | MATCHED   |
| M2OsStubTest        | 100  | MATCHED   |
| M2ModSmokeLoading   | 100  | MATCHED   |
| M3TerrainStress     | 100  | MATCHED   |

All nine scenarios are bit-identical run-to-run on Linux x86-64 after the
PathFinder fix below — `diverged: false` on every one. `M1Baseline` was run
300× rather than 100 because it is the scenario that exposed the bug
(pre-fix it diverged roughly 1 in 115 runs); 300 clean post-fix runs is the
fix's empirical confirmation, alongside the ThreadSanitizer evidence.

**Thread-count matrix.** `M4ThreadStress` through
`--threads 1,2,4,8,16 --runs 2` (900 ticks): **MATCHED** — bit-identical
across all five Lua-state counts, confirmed over 4 independent matrix runs.
The cross-thread-count residual `determinism.yml` documents at ~tick 136 did
not reproduce on Linux; see *Notes*.

## The determinism bug — async pathfinding request accounting

*Commit `679b53c84` — `PathFinder: count async path requests from enqueue, not dispatch`.*

### Symptom

On the **first** harness run, `M1Baseline` diverged: `RESULT: DIVERGED`,
`first_divergence_tick: 1`, 599/600 ticks mismatched, the `actors` subsystem
diverging first (tick 1, ahead of `sim_rng` — i.e. a non-RNG state path).
Subsequent runs (2 + 30 + 80 = 112 child runs across `M1Baseline` and
`M1ActorStress`) all matched. The divergence is a genuine but rare race —
roughly 1 in ~115 runs — that Windows' scheduler did not expose but Linux's
does. M4's "bit-identical run-to-run at a fixed thread count" held on Windows
and very nearly holds on Linux.

### Diagnosis

A ThreadSanitizer build (`-Db_sanitize=thread`, LuaJIT excluded from
instrumentation, ASLR disabled via `setarch -R` — see *Notes* below)
localised it. The decisive, TSan-trustworthy race — through the sim's own
fully-instrumented `BS::thread_pool` — was the main thread reading a
`PathRequest` in `Actor::PreControllerUpdate` (`Actor.cpp:1097`) while a
`BS::thread_pool` worker running `PathFinder::CalculatePathAsync`'s task
(`PathFinder.cpp:252`) wrote that same `PathRequest`.

Root cause: `PathFinder::CalculatePathAsync` (`PathFinder.cpp:230`) pushes the
solve onto the background thread pool, but `m_CurrentPathingRequests` — the
in-flight counter that `Scene::BlockUntilAllPathingRequestsComplete`
(`Scene.cpp:2378`), `Scene::UpdatePathFinding`'s early-out (`Scene.cpp:2393`)
and `PathFinder::RecalculateAllCosts` all gate on — was incremented only when
a worker actually entered `PathFinder::CalculatePath` (`PathFinder.cpp:154`).
A request still sitting in the pool's queue was therefore invisible to every
one of those gates. `BlockUntilAllPathingRequestsComplete` (called per frame
in determinism runs from `MovableMan::Update`) could return while a request
was still queued; the main thread then read a `PathRequest` whose worker had
not yet written its `path` / `status` / `complete` fields. The harness gives
`actors` the first divergence because a stale/incomplete path changes AI
movement directly. (TSan also flags `AdjacentCost` ↔ `UpdateNodeCosts` on the
node grid, but that one persists post-fix and is attributable to TSan's
blindness to the uninstrumented-`libtbb` `par_unseq` join — see *Notes*.)

### Fix

Increment `m_CurrentPathingRequests` at **enqueue** time in
`CalculatePathAsync` (before `push_task`) and release it at the end of the
task. The atomic counter now brackets the request's whole queued + running
lifetime, so the gates see queued requests and `BlockUntilAll...` returning
establishes a real happens-before edge with the worker's writes. Path
*results* are unchanged — only the counter's timing changes.

Verified two ways: the TSan build no longer reports the
`Actor::PreControllerUpdate ↔ PathRequest` race after the fix; and
`M1Baseline` is bit-identical across 300 post-fix runs (it diverged
~1/115 pre-fix). All nine scenarios and the thread-count matrix MATCHED —
see the determinism table above.

**Shared logic — flagged explicitly.** This changes `Source/System/PathFinder.cpp`,
which the Windows build compiles too. It does not alter path computation; it
makes the in-flight counter accurate so the existing gates wait correctly.
Windows runs the identical path and benefits identically — it simply hit the
window less often. Regression risk is low: the counter is balanced (+1 at
enqueue, −1 in the task, which always runs; `CalculatePath` keeps its own
balanced ±1), so there is no deadlock path. The only behavioural change is
that `Scene::UpdatePathFinding` now correctly defers a node-cost update by one
cycle while a path request is queued — its intended behaviour.

## Notes (observed, not changed)

* **The harness is not headless by default.** On a machine with a display
  (WSL2 provides one through WSLg), SDL creates a real OpenGL window for
  every determinism child. Running several determinism-checks concurrently
  then deadlocks the children in `poll()` on the X11/Wayland sockets — a
  WSLg display-contention issue, not an engine fault. Every run in this
  report used `SDL_VIDEODRIVER=offscreen`, SDL's windowless driver: it still
  provides a real (EGL) GL context, so the engine's GL init does not crash —
  unlike `SDL_VIDEODRIVER=dummy`, which has no GL context and segfaults on
  the first `glReadBuffer`. An `offscreen` run produces a per-tick trace
  bit-identical to a real-display run (verified, 0 of 600 ticks differ), so
  the video driver does not affect the sim hash. **Recommendation:** the
  determinism harness should force `SDL_VIDEODRIVER=offscreen` (or the engine
  should select it whenever `-determinism-check` / `-tick-hashes` is set) so
  the Linux CI job is reliably headless — the CI runner is display-less and
  the harness's "no X display needed" assumption is currently unverified.
* **M4ThreadStress cross-thread-count residual.** `determinism.yml` documents
  a residual cross-thread-count divergence in `M4ThreadStress` at ~tick 136,
  left to the M4 follow-up. On Linux x86-64 the `--threads 1,2,4,8,16` matrix
  MATCHED for the full 900 ticks across 4 independent runs — the residual did
  not reproduce here. The PathFinder async-counting bug fixed below is a
  plausible (partial) cause of that residual: different Lua-state counts give
  different pool scheduling, which resolves the queued-path-request window
  differently and would produce exactly a count-dependent, downstream
  (particle/terrain physics) divergence. This is suggestive, not proof — the
  matrix is only `--runs 2` per count and the residual was characterised on
  Windows. The team should re-run the Windows matrix with the PathFinder fix
  before closing the M4 follow-up; M5 itself is out of scope for this work.
* **Meson floating-point flag block.** `meson.build` adds the M1 Block E
  FP-determinism flags (`-msse2 -fno-fast-math -fno-associative-math
  -frounding-math …`) to `extra_args` for GCC (lines ~39–48), but the release
  branch at line ~80 does `extra_args = ['-w']` — an assignment, not an
  append — which **discards** those FP flags. A `--buildtype=release` build
  (the `determinism-linux` CI job, and this bring-up) therefore compiles
  *without* the explicit FP pinning and relies on GCC's defaults. On x86-64
  this is largely benign (SSE2 is the ABI baseline; GCC does not reassociate
  or contract FP by default), but the explicit pinning is absent. This block
  is owned by the macOS agent's restructure, so it was **not edited here** —
  flagged for that work.
* **TSan + this codebase.** System `libtbb` is not TSan-instrumented, so TSan
  cannot see the `std::execution::par_unseq` join synchronisation and emits a
  large volume of false-positive races in and around TBB (`partitioner.h`,
  `parallel_for.h`, and `AdjacentCost ↔ UpdateNodeCosts` once the parallel
  `UpdateNodeList` has already joined). The diagnosis above relies only on
  races through the sim's own `BS::thread_pool`, which is fully instrumented.
  Also: the TSan binary aborts at start-up with `unexpected memory mapping`
  unless run under `setarch -R` (ASLR off) — a known TSan/WSL2 interaction.
* **Cosmetic residual race (not determinism-affecting).** The deferred
  MOID-draw task (`MovableMan::UpdateDrawMOIDs`, submitted at
  `MovableMan.cpp:1899`) runs concurrently with the main-thread
  `MovableMan::Draw` (`MovableMan.cpp:1907`); both touch actor MOID state
  (`MovableObject::RegMOID` ↔ `MOSRotating::Draw`). The `actors` / `particles`
  / `scene` checksum subsystems are fed earlier in the tick, before the task
  is submitted, so this does not affect the sim checksum — but it is a real
  data race. Characterised here; not fixed (render-side, outside the
  determinism island).
* **Variable warmup length.** A scenario's *total* sim-tick count varies
  slightly run-to-run (e.g. `M1Baseline` 679/689/690) — a variable-length
  asset-loading/warmup phase. It does not affect the traced 600-tick window
  (the determinism-check compares a fixed 600-tick trace), so it is benign
  for determinism; noted for completeness.

## Per-tick traces

One `-tick-hashes` trace per scenario was produced with `-num-lua-states 4`
(pinned, so the threaded-Lua state count matches every OS) and committed under
`Source/CI/*-trace.json` (commit `cbcf392de`) for the team's cross-platform
per-tick hash diff against the Windows traces.

## Summary

**Builds.** The engine and `FixedPointTests` build clean on Linux x86-64
(GCC 13.3, Meson 1.11.1, `--buildtype=release`) after one Meson fix — BLAKE3
made a proper subproject. The anticipated GCC-strictness errors and `GUIRect`
Meson break did not occur.

**Green.**
* `FixedPointTests` — 42/42 pass, `SELF-CHECK: 5ce9c33b84d29932`.
* All nine determinism scenarios — bit-identical run-to-run on this machine
  (`diverged: false`); `M1Baseline` over 300 runs, the rest over 100.
* `M4ThreadStress` thread-count matrix (1/2/4/8/16) — MATCHED.
* Per-tick traces for all nine scenarios committed under `Source/CI/`.

**Fixed.** One same-platform non-determinism bug — the async pathfinder's
in-flight counter (`m_CurrentPathingRequests`) did not count queued requests,
so the main thread could read a `PathRequest` before its worker had finished
writing it. Linux's scheduler exposed it on ~1/115 `M1Baseline` runs; fixed
in `PathFinder::CalculatePathAsync` (commit `679b53c84`).

**For the team (not changed here).**
* The Meson FP-flag block reassigns `extra_args` in release builds, dropping
  the explicit FP-determinism flags — owned by the macOS agent's restructure;
  flagged, not edited.
* A cosmetic MOID-draw ↔ main-thread-`Draw` data race — render-side, does not
  reach the sim checksum — characterised, not fixed.
* The cross-OS trace diff and the FixedPoint cross-OS hash check are CI jobs;
  the Linux-side artifacts they consume are committed on this branch.

Net: the Linux x86-64 build is clean, and the simulation is deterministic
run-to-run on this platform across every CI determinism scenario.

## Block F revalidation — canonical M4A consolidation

Branch tip `366b9d073` (`exp/determinism-linux` after force-push from
`dfa7691a4`). Pre-rebase state preserved at
`backup/pre-block-f-linux-2026-05-24`.

Consolidated stack on this tip: Path E architecture (epilogue
UpdatePathFinding) + atomic callback-id + queue sort + PathFinder seq/sort
internals + `m_ContiguousActorIDs` sync rebuild + M5.5 V1.4 + M4A TSan +
macOS universal UB fixes.

### Verification — all green

| Scenario | Config | Result | Report |
|---|---|---|---|
| 9 × M1/M2/M3 (M1Baseline, M1TerrainStress, M1ActorStress, M2LuaBaseline, M2LuaRandomStress, M2PairsStress, M2OsStubTest, M2ModSmokeLoading, M3TerrainStress) | --runs 10 | **MATCH** (all) | `block-f-test-all.json` |
| M4ThreadStress thread-matrix | --threads 1,2,4,8,16 --runs 2 | **MATCH** (FFF-415 acid) | `block-f-m4-matrix.json` |
| MPerfBench | --threads 8 --ticks 1200 --runs 10 | **MATCH** | `block-f-mperfbench.json` |
| selftest:M1Baseline (EC3 positive control) | --runs 5 | **PASS** (harness catches injected divergence) | `block-f-test-all.json` |
| FixedPointTests SELF-CHECK | — | `5ce9c33b84d29932` | (matches the cross-platform proof point) |

### Notes

* `cccp-ctl test all` aggregates `--runs 10 --seed 42` over the 9
  M1/M2/M3 scenarios + an M4ThreadStress sub-thread-matrix (defaults to
  `--runs 1` inside the aggregate). The explicit `test thread-matrix`
  with `--runs 2` (above) covers the user-spec re-run.
* MPerfBench was driven through `test thread-matrix --threads 8` because
  `test scenario` / `test replay-determinism` do not surface a
  `--num-lua-states` knob; `thread-matrix` with a single value is the
  equivalent shape.
* Linux host: GCC 13.3, Meson 1.11.1, `--buildtype=release`. `Data/` was
  symlinked into `build/` (`build/Data → ../Data`) so the harness's
  `cwd=build/` invocation can find scenario assets.
* Race A (PathFinder consumption-side residual) and Race C
  (`m_ContiguousActorIDs` async-rebuild drift) remain closed on this
  platform under the consolidation; no regression observed.
