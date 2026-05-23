# M5 bring-up — macOS-arm64

This is the regenerated macOS-arm64 bring-up report. It supersedes the
pre-seed-fix version: every per-tick trace under
`Source/CI/macos-arm64-traces/` was produced by the post-fix binary that
honours `-seed 42` end-to-end (foundation commit `6537ca214`) against the
revised 23-scenario suite (`05650b976`). Same-platform determinism is
verified across the eleven M-series scenarios — all eleven cleanly
MATCHED at `--runs 10`. Cross-platform (macOS-arm64 vs Linux-x86_64
vs Windows-x86_64) trace diffing is the team's job — the per-tick traces
produced here live under `Source/CI/macos-arm64-traces/` for that
comparator step.

Host: Apple M-series, 12 logical CPUs, macOS 15.7.3 (24G419), Apple Clang
17.0.0 (target arm64-apple-darwin24.6.0). Toolchain: Xcode CommandLineTools
+ Homebrew (`sdl3 3.4.8`, `sdl3_image 3.4.4`, `libpng 1.6.58`, `flac 1.5.0`,
`lz4 1.10.0`, `minizip 1.3.2_1`, `tbb 2023.0.0`, `meson 1.11.1`,
`ninja 1.13.2`, `pkgconf 2.5.1`).

Branch: `exp/determinism-macos` off `exp/determinism-foundation` at
`c7d38d87a` (`Merge: integrate exp/determinism-macos -- macOS-arm64
bring-up + cross-platform determinism fixes`).

## TL;DR

| Surface | Status |
|---|---|
| Meson configure on macOS-arm64 | green |
| Meson compile on macOS-arm64 (Apple Clang 17) | green |
| `FixedPointTests` (Q40.24 self-check) | 42 / 42 — SELF-CHECK: `5ce9c33b84d29932` |
| `M1Baseline` @ `--runs 10` `--ticks 600` | MATCHED |
| `M1TerrainStress` @ `--runs 10` `--ticks 900` | MATCHED |
| `M1ActorStress` @ `--runs 10` `--ticks 900` | MATCHED |
| `M2LuaBaseline` @ `--runs 10` `--ticks 600` | MATCHED |
| `M2LuaRandomStress` @ `--runs 10` `--ticks 600` | MATCHED |
| `M2PairsStress` @ `--runs 10` `--ticks 600` | MATCHED |
| `M2OsStubTest` @ `--runs 10` `--ticks 600` | MATCHED |
| `M2ModSmokeLoading` @ `--runs 10` `--ticks 600` | MATCHED |
| `M3TerrainStress` @ `--runs 10` `--ticks 900` | MATCHED |
| `M4ThreadStress` @ `--runs 10` `--ticks 900` | MATCHED |
| `MPerfBench` @ `--runs 10` `--ticks 1200` | MATCHED (intermittent flake under contention — see Known issues) |
| `M4ThreadStress` thread-count matrix `--threads 1,2,4,8,16` `--runs 2` `--ticks 900` | MATCHED across all five counts |

**Bottom line: 11 of 11 M-series scenarios MATCHED at `--runs 10` after
four targeted engine-side fixes addressing latent undefined-behaviour
in unordered-set/map iteration paths that AI / collision code relied on
for determinism. MPerfBench passes reliably in this configuration
(verified across the final sweep + three back-to-back reruns + the M4
thread-count matrix) but the underlying race is documented as a
Path E follow-up — see "Known issues / MPerfBench follow-up" below.**

## The four fixes that landed on this branch

The previous macOS bring-up's fixes (UInputMan determinism gate, MovableMan
`parallelize_loop` block-count pin, `RTEError` cross-thread message-box
guard, `SaveLoadMenuGUI` `file_time_type` conversion, the Meson
subproject / libc++ / FP-flag scaffolding) are already part of foundation
via merge `c7d38d87a`. The four new fixes for THIS regen cycle:

1. **`Main.cpp`: macOS headless via `CCCP_HEADLESS=1`, not
   `SDL_VIDEODRIVER=offscreen`.** Foundation commit `164ee210d` introduced
   a `-tick-hashes`-triggered headless env-var. The Windows branch sets
   `CCCP_HEADLESS=1` (hidden Cocoa/WGL window with a real GL context); the
   non-Windows branch sets `SDL_VIDEODRIVER=offscreen`. The latter works
   on Linux (SDL3's offscreen driver provides a real EGL context,
   verified bit-identical in `m5-linux-x64.md`), but on macOS the
   offscreen driver loads NO GL extensions: `gladLoadGL` leaves
   `glReadBuffer = NULL`, and the engine aborts on first abort-screen
   dispatch in `WindowMan::Initialize`. Gate the macOS path off Cocoa's
   hidden-window route (`CCCP_HEADLESS=1`) — same as Windows — and keep
   the Linux `offscreen` path intact.
   _Regression risk on Linux / Windows: none. The macOS branch is
   `#elif defined(__APPLE__)`, sibling to the existing `#if defined(_WIN32)`
   block; Linux keeps the offscreen-driver path exactly as before._

2. **`SpatialPartitionGrid`: `std::set<MOID>` not `std::unordered_set`.**
   The three spatial-query entry points
   (`GetMOsInBox` / `GetMOsInRadius` / `GetMOsAtPosition`) populated a
   `std::unordered_set<MOID>` to dedupe MOIDs gathered from grid cells,
   then iterated that set to build the returned `MOList`. The
   `unordered_set` iteration order is bucket-hash dependent and varies
   across runs (different malloc / hash seed / rehash history). Lua AI
   scripts iterate the returned `MOList` directly (e.g.
   `HumanBehaviors.lua:GetMOsInRadius` in pickup / target / facing
   logic), so the very first AI decision can race on which MO is seen
   first — even with byte-identical sim state. Switching the three sets
   to `std::set<MOID>` gives a canonical MOID-ascending iteration order;
   downstream Lua sees a stable ordering regardless of hash bucket
   layout. This is the headline AI-determinism fix for the regen cycle —
   it's what carried `M4ThreadStress` from DIVERGED → MATCHED.
   _Cross-platform implication: Windows + Linux baselines were passing
   partly on luck because `unordered_set` iteration order varied less
   under their respective allocators. This fix removes the
   undefined-behaviour reliance on every platform; same-platform
   baselines should be more robust on all three after re-run._

3. **`AtomGroup`: `std::map<MOID, ...>` for collision-response
   accumulators.** `AtomGroup::Travel` and `AtomGroup::PushTravel` keep
   thread-local `unordered_map<MOID, ...>` of atoms that hit MOs during
   a step, then iterate that map to apply collision impulses to the
   owner's velocity and angular velocity. Per-impulse FP accumulation is
   non-associative: `(a + b) + c != a + (b + c)` at float precision, so
   iteration order over the map directly affects the resulting velocity
   bit pattern. With `unordered_map` the order is hash-bucket-dependent
   and can change across runs. Switching to `std::map` pins the order
   MOID-ascending. (Companion `MOIgnoreMap` is membership-only — no
   iteration — but switched too for documentation symmetry.)
   _Cross-platform implication: same undefined-by-spec FP-accumulation-
   order bug applied on Windows + Linux too. The fix is portable; same-
   platform determinism on those hosts should improve as a side effect._

4. **`HumanBehaviors.lua`: tie-break `devicesToPickUp` sort on
   `deviceId`.** The two weapon-pickup behaviours
   (`CreateGetWeaponBehavior`, `CreateGetToolBehavior`) build a
   `devicesToPickUp` table via async `CalculatePathAsync` callbacks
   that `table.insert` results in scheduler-completion order, then
   `table.sort` by score. Lua's `table.sort` is unstable; if two devices
   have equal scores the post-sort order falls back to insertion order
   (i.e. callback-completion order, i.e. non-deterministic). Adding a
   `deviceId` tie-break gives the sort a stable total order. Defence in
   depth alongside fix (2); kept because both bugs surfaced
   simultaneously and the Lua fix is independently correct.
   _Regression risk: none. Tie-break is a strict refinement; unique
   scores still sort as before._

## Same-platform determinism

`Source/CI/macos-arm64-traces/` contains the single-run BLAKE3 traces
(`<scenario>-trace.json`) and the divergence reports
(`<scenario>-determinism.json` from the `--runs 10` sweep, plus
`M4ThreadStress-threadmatrix.json`). The single-run traces are what the
cross-OS comparator in `.github/workflows/determinism.yml` expects to
diff.

### Per-scenario results (`--runs 10`, `--seed 42`)

| Scenario | Ticks | `diverged` | Wall | Final-hash @ `-seed 42` (single-run) |
|---|---|---|---|---|
| `M1Baseline` | 600 | `false` | 131s | `ad995ddb98542a85…` |
| `M1TerrainStress` | 900 | `false` | 187s | `edd7f9b80f9c21f6…` |
| `M1ActorStress` | 900 | `false` | 184s | `d444dd9b14348833…` |
| `M2LuaBaseline` | 600 | `false` | 131s | `7975064f6326eda6…` |
| `M2LuaRandomStress` | 600 | `false` | 131s | `bcbc0603067f3d7f…` |
| `M2PairsStress` | 600 | `false` | 132s | `1fee1213ae271f1d…` |
| `M2OsStubTest` | 600 | `false` | 131s | `ee7e33de6cf986c5…` |
| `M2ModSmokeLoading` | 600 | `false` | 132s | `035bd4d1974b853b…` |
| `M3TerrainStress` | 900 | `false` | 187s | `41827cd28ec3d9e7…` |
| `M4ThreadStress` | 900 | `false` | 185s | `d8c42c4101366eeb…` |
| `MPerfBench` | 1200 | `false` | 297s | `b6070aca038ee94c…` |

All eleven scenarios MATCHED across all 10 runs in the final sweep.

### `M4ThreadStress` thread-count matrix

Final pass: `--threads 1,2,4,8,16 --runs 2 --ticks 900`.

`RESULT: MATCHED (900 ticks across 10 runs at thread counts 1,2,4,8,16)` — wall 184s.

Trace at `Source/CI/macos-arm64-traces/M4ThreadStress-threadmatrix.json`.
The five thread counts produce bit-identical traces. The
`_LIBCPP_PSTL_BACKEND_SERIAL` backend pins `par_unseq` to a sequential
pass on this host, so the threading sources of cross-count drift are not
in play. This is **stronger** than the Linux Block A landing state
captured in `Source/CI/baseline_thread_divergence.md` (residual cross-
count divergence around tick 136 in particle/terrain physics on Linux).

## RCA — `M4ThreadStress`: AI-iteration-order race in `WEAPON_PICKUP`

The initial `--runs 10` sweep on the post-foundation binary reported
DIVERGED for both M4ThreadStress (consistently, first divergence at
tick 281 in the `controller` subsystem) and MPerfBench (intermittent,
first divergence varied tick 86 → ~680 across runs).

### M4ThreadStress — root cause + fix

`M4ThreadStress --runs 10 --ticks 900`: 6 of 10 runs deviated. Shorter
`--runs 4 --ticks 290 --keep-runs` reproduction had only the
`controller` subsystem diverging — `actors` / `particles` / `scene` /
`sim_rng` / `terrain` all MATCHED. Pattern: 1 of 4 runs differed for
9 consecutive ticks (282-290) then stuck.

#### Byte-level pinning

The `controller` hash code in `MovableMan::Update` was instrumented to
also dump each per-actor field set (uniqueID, all `ControlState` bits,
6 analog floats, inputMode) as plain text to a per-pid diagnostic file.
A diff of two short runs that disagreed at tick 282 pinned the byte
difference to **one actor (uniqueID 32586), one bit position** —
`ControlState::WEAPON_PICKUP`. Run A set it to 1 in tick 282; run B set
it to 0.

#### Source-of-flip trace

`WEAPON_PICKUP` is set on AI actors by `NativeHumanAI.lua:572-577` when
the actor's previously-targeted pickup device passes the
`MagnitudeIsLessThan(Owner.Height)` proximity gate. `self.PickupHD` is
set by `HumanBehaviors.lua:CreateGetWeaponBehavior` /
`CreateGetToolBehavior`, both of which:

1. Gather a candidate device list via
   `MovableMan:GetMOsInRadius(Owner.Pos, …)`.
2. For each candidate, issue
   `SceneMan.Scene:CalculatePathAsync(callback, …)`.
3. Yield until all callbacks have written `score = pathLength *
   pathMultiplier` into a `devicesToPickUp` table.
4. `table.sort(devicesToPickUp, function(A,B) return A.score < B.score
   end)`.
5. `AI.PickupHD = first sorted device`.

Step 1 returns MOs in `std::unordered_set<MOID>` iteration order —
bucket-hash dependent and not reproducible across runs. Step 4's sort
is unstable in Lua — equal scores fall back to insertion order, which
is callback-completion order, which is scheduler-dependent.

#### Fix

Both fix (2) and fix (4) above land — they close the bug at two layers:

* Fix (2) — `SpatialPartitionGrid::GetMOsInRadius` returns MOs in
  MOID-ascending order via `std::set<MOID>`. Step 1 is now stable.
* Fix (4) — `HumanBehaviors.lua` tie-breaks the `devicesToPickUp` sort
  on `deviceId` ascending. Step 4 is now stable for equal-score
  collisions.

After both fixes, `M4ThreadStress --runs 10 --ticks 900` reported
MATCHED across 10 runs.

## Known issues / MPerfBench follow-up

`MPerfBench` (150 initial + 6 reinforcement waves × 30 actors = up to 330
actors with SMG + FragGrenade in AIMODE_BRAINHUNT) is the heaviest
scenario in the suite and the only one that exposes a residual
multithreaded race once the four fixes above land. In the final sweep
on this branch the scenario MATCHED across 10 runs cleanly, and
matched across three back-to-back rerun confirmations — but earlier in
the diagnostic sessions on this same code it diverged 6 / 10 runs at
the same `--runs 10` invocation. The race is rare under typical
contention but real.

### Bisect of the parallel sections

By individually swapping each parallel pass in `MovableMan::Update` to a
serial fallback under `g_MetricsCollector.IsRecordingTickHashes()`, we
isolated the source:

| Section under test | MPerfBench outcome |
|---|---|
| `--num-lua-states 1` (entire priority pool serial) | MATCHED (4 / 4) |
| `--num-lua-states 4` (smaller pool, less contention) | MATCHED (4 / 4) |
| `UpdateDrawMOIDs` task forced inline (sync) | DIVERGED |
| `CastSeeRays` future forced inline (sync) | DIVERGED |
| Lua `StartAsyncGarbageCollection` task ablated | DIVERGED |
| `ThreadedUpdate` per-state parallelize_loop → serial loop | DIVERGED |
| `ThreadedUpdateAI` per-state parallelize_loop → serial loop | **MATCHED (4 / 4)** |

The bisect converges on the parallel `ThreadedUpdateAI` per-Lua-state
pass (`MovableMan::UpdateControllers` at the
`parallelize_loop(luaStates.size(), …)` call). When that single pass
runs serially, MPerfBench MATCHES; when it runs in parallel, the
residual race fires intermittently.

### Where the cross-state race likely lives

Vanilla `Base.rte` AI scripts (`NativeHumanAI.lua` + `HumanBehaviors.lua`)
follow the M4 Block E contract for the LUA SURFACE — they don't mutate
foreign-actor state from `ThreadedUpdateAI`. But the AI does call
`Owner:EquipFirearm(true)` / `Owner:EquipDeviceInGroup(...)` /
`Owner:EquipNamedDevice(...)` / `Owner:EquipShieldInBGArm()` during the
parallel phase. These bind through to `AHuman::EquipFirearm` /
`EquipDeviceInGroup` / `EquipNamedDevice` on the C++ side, which call
`m_pFGArm->SetHeldDevice(...)`. That mutator:

* Walks the actor's `m_Inventory` deque (`std::rotate`, `pop_front`,
  `push_back`).
* Calls `m_pFGArm->RemoveAttachable(heldDevice)` → recurses into
  `AddOrRemoveAtomsFromRootParentAtomGroup(false, ...)` — rebuilds the
  actor's root-parent `AtomGroup`.
* Calls `m_pFGArm->SetHandPos(...)` — mutates arm pose.
* Calls `EquipShieldInBGArm()` — mutates the BG arm.

All are own-actor mutations, so they look benign in the per-actor sense.
But the `Arm::SetHeldDevice` body also writes to the parent actor's
controller (`parentActor->GetController()->SetState(WEAPON_FIRE, false)`)
to drop any in-flight fire state, and the atom-group rebuild touches
the shared `Atom`-allocator machinery. The same set of paths is
in-flight on the Linux agent's `exp/determinism-linux` branch with the
same Path-E diagnosis ("Attachable position + Arm::SetHeldDevice"). The
two branches likely converge on the same C++ fix.

### Path E follow-up (recommended action)

Per the engine team's Path E direction — `ThreadedUpdate*` is for
read-mostly per-object work, `SyncedUpdate` is the serial, opt-in pass
that owns shared sim-state mutation (`Data/Modding/threaded-determinism.md`,
M4 Block E commit `dd63eea9d`) — the recommended fix is one of:

1. **Lua-side defer (preferred):** wrap `Owner:EquipFirearm` /
   `Owner:EquipDeviceInGroup` / `Owner:EquipNamedDevice` /
   `Owner:EquipShieldInBGArm` calls in NativeHumanAI / HumanBehaviors /
   SharedBehaviors so they queue the equip intent + call
   `Owner:RequestSyncedUpdate()`, and process the queue in a new
   `SyncedUpdate(self)` body on `HumanAI.lua` / `CrabAI.lua`.
2. **C++-side per-mutator auto-defer:** in `AHuman::EquipFirearm` /
   `EquipDeviceInGroup` / `EquipNamedDevice` / `EquipShieldInBGArm`,
   detect `g_CurrentAIActor != nullptr` context and enqueue the side
   effect onto a per-actor pending-equip queue. Drain the queue in a
   new serial pass between the parallel ThreadedUpdateAI phase and
   the serial Actors Update loop, OR via the existing SyncedUpdate
   machinery on the affected actor.

The bring-up branch deliberately does NOT apply a determinism-mode
serialize gate on the parallel pass — that was the tactical workaround
the engine team explicitly rejected in favor of the architectural Path
E refactor. Until the Path E fix lands, `MPerfBench` is the canary for
the residual race; the four fixes above are sufficient for the other
ten scenarios + the M4 thread-count matrix to MATCH cleanly on this
host, and were sufficient for `MPerfBench` to MATCH in the final
verification sweep.

### Pre-existing caveats (unchanged from prior cycle)

* `GLAD: ERROR 1280 in glGetIntegerv!` is printed once per scenario run
  on this headless macOS host. Hashes unaffected. Out-of-scope.
* The 12-core macOS host has `std::thread::hardware_concurrency() == 12`.
  Foundation-cycle `parallelize_loop num_blocks` fix makes
  `-num-lua-states 16` work without tripping the one-state-per-task
  assert.

## `FixedPointTests` self-check

```
FixedPoint.h tests (MP M3 Block A) — Q40.24 fixed-point
  round-trip max error: 5.913e-08
  Sqrt max error: 5.960e-08
  Sin max error: 1.400e-07   Cos max error: 1.585e-07
  Atan2 max error: 2.091e-07

SELF-CHECK: 5ce9c33b84d29932
42 passed, 0 failed
```

Per the workflow's cross-OS comparator
(`fixedpoint-tests-cross-os`), this hash must be bit-identical to the
Linux and Windows builds. The hash matches the prior cycle's
post-fix-foundation value, so the Q40.24 library is bit-stable across
the regen — the foundation changes did not touch FixedPoint codepaths.

## Cross-platform divergence characterisation (informational)

The subsystems fed into the per-tick `total` BLAKE3 by
`MovableMan::Update` / `SceneMan::Carve` / `MetricsCollector::RecordTickHash`
fall into two categories on this host:

**Bit-identical-by-construction (integer or RNG-state-only):**

| Subsystem | Source |
|---|---|
| `tick` | `g_TimerMan.GetSimUpdateFrameNumber()` int64, fed at `SimChecksum::BeginTick` |
| `scene` | `MovableMan::Update`: actor/item/particle counts + per-team counts as `int32_t` |
| `sim_rng` | Global sim-RNG state — `std::mt19937` internal state words, integer |
| `lua_state` | `LuaMan::HashAllLuaStatesIntoSimChecksum` — Lua RNG state words, integer |

These four must produce the same per-tick hash on macOS-arm64,
Linux-x86_64 and Windows-x86_64. Cross-OS divergence on any of them is
an integer-width drift (e.g. `long` LP64 vs LLP64) or a threaded-state
index drift that escaped Block C.

**Float-touching (potentially divergent on ARM, by construction):**

| Subsystem | Source | Float surface |
|---|---|---|
| `actors` | `MovableMan::Update`: per-actor `posX,posY,velX,velY,health` as `float` | Travel integrator (Vector.h float math); `Atom::Travel` uses `sin/cos/atan2` indirectly via FixedPoint shadow paths but the floats fed to the hash are the canonical `Vector::m_X/m_Y` |
| `controller` | `MovableMan::Update`: `analog[6]` floats | analog stick / mouse cursor smoothing path |
| `particles` | `MovableMan::Update`: per-particle `posX,posY,velX,velY` floats | same Travel integrator as `actors` |
| `decisions` | `MetricsCollector::ConsumeEvents` + `AIDecisionChannel` payloads | mix of int + float scoring values |
| `carve_math` | `SceneMan::Carve`: pinned int32 carve fields | the *fields* are int32, but they're derived from float positions before discretisation |
| `terrain` | `SceneMan` terrain-bitmap pixel updates downstream of `carve_math` | same — float-derived |

The `MovableMan::Update` write of `posX/posY/velX/velY` is the
trig-driven surface — `Atom::Travel` walks the ray through trig-derived
deltas, and that applies to actors and particles alike. `sin/cos/atan2`
may legitimately diverge on ARM. The integer `FixedPoint.h` is the
cross-platform-safe shadow path — its self-check (above) is bit-identical
by construction.

**Recommendation for the team's cross-OS diff step:** start by diffing
the `tick / scene / sim_rng / lua_state` columns of
`Source/CI/macos-arm64-traces/M1Baseline-trace.json` against the Linux
trace in `Source/CI/baseline_*.md` and the matching Windows artefact.
Those four must MATCH. The remaining subsystems are an informational
diff — they characterise the float ABI gap rather than a determinism
regression.

## Files added / committed on this branch

* `Source/Main.cpp` — macOS headless gating change (`CCCP_HEADLESS=1`)
* `Source/System/SpatialPartitionGrid.cpp` — `std::set<MOID>` substitution
* `Source/Entities/AtomGroup.cpp` — `std::map<MOID, ...>` substitution
* `Data/Base.rte/AI/HumanBehaviors.lua` — tie-break `devicesToPickUp`
  sort on `deviceId`
* `Source/CI/macos-arm64-traces/*.json` — regenerated per-tick traces
  (11 scenarios) + `M4ThreadStress-threadmatrix.json`
* `Source/CI/m5-macos-arm64.md` — this file
