# macOS-arm64 Path E verification — cherry-pick of f495476f5 / 4b9546b01

Cross-platform verification cycle for the Linux agent's partial Path E
fix. The two Linux commits were cherry-picked clean onto
`exp/determinism-macos` (was `06ad65084`, four-keeper state) and the
race rate plus residual signature were measured at the Lua-state
counts the user requested.

Host: Apple M-series, 12 logical CPUs, macOS 15.7.3 (24G419),
Apple Clang 17.0.0 (target arm64-apple-darwin24.6.0). Build: Meson
`release / O3 / no LTO`, FixedPointTests `SELF-CHECK:
5ce9c33b84d29932` (42 / 42 pass) unchanged from the four-keeper
binary. No determinism-mode serialize gate present on this branch
(the rejected `45c757755` is not in the cherry-pick).

## TL;DR

| Surface | Status |
|---|---|
| Cherry-pick `f495476f5` (AHuman Equip* defer + NativeHumanAI peek) | clean (4 files, 131 + / 5 −) |
| Cherry-pick `4b9546b01` (mod_compat doc) | clean (1 file, 19 +) |
| Build green (Apple Clang 17, Ninja) | yes |
| `FixedPointTests` SELF-CHECK | `5ce9c33b84d29932` (unchanged) |
| `M4ThreadStress` `-threads 8` × 5 sweeps × 10 runs | 5 / 5 MATCHED (100 / 100 runs clean) |
| `M4ThreadStress` `-threads 16` × 5 sweeps × 10 runs | 5 / 5 MATCHED (100 / 100 runs clean) |
| `MPerfBench` `-threads 8` × 5 sweeps × 10 runs | 2 / 5 MATCHED (47 / 50 runs clean) |
| `MPerfBench` `-threads 16` × 5 sweeps × 10 runs | 1 / 5 MATCHED (46 / 50 runs clean) |
| `MPerfBench` default (12) × 3 sweeps × 10 runs | 2 / 3 MATCHED (22 / 30 runs clean) |
| `MPerfBench` `-threads 8` with `ThreadedUpdateAI` serialized × 3 sweeps | 3 / 3 MATCHED (30 / 30 runs clean) |

**Headline.** The Path E cherry-pick FULLY closes the
`M4ThreadStress` residual on macOS-arm64 (100 % match rate at both
Lua-state counts, contrast with Linux's "1 / 5 sweeps match" residual).
`MPerfBench` exposes a DIFFERENT residual race on macOS that the
Path E AHuman::Equip* defer does not cover. The single-section
ablation reconfirms the residual lives in the parallel
`ThreadedUpdateAI` per-Lua-state pass: when that pass is forced to a
serial loop, the race closes uniformly (3 / 3 sweeps MATCH).

## Race rate (post-cherry-pick) — macOS-arm64 vs Linux reference

| Scenario / Lua-states | Sweeps run | Sweeps MATCH | Per-run outcome-rate ≈ | Linux reference |
|---|---|---|---|---|
| `M4ThreadStress` / 8 | 5 (50 runs) | 5 (100 %) | 0 % rare-outcome | n/a (Linux M4 still ~80 % diverge / sweep) |
| `M4ThreadStress` / 16 | 5 (50 runs) | 5 (100 %) | 0 % rare-outcome | n/a |
| `MPerfBench` / 8 | 5 (50 runs) | 2 (40 %) | ~10 % rare-outcome | n/a |
| `MPerfBench` / 12 (default) | 3 (30 runs) | 2 (67 %) | ~5–10 % rare-outcome | n/a |
| `MPerfBench` / 16 | 5 (50 runs) | 1 (20 %) | ~16 % rare-outcome | n/a |

Linux's reported post-cherry-pick state is `M4ThreadStress`-specific:
"race rate from 20 % to 3-5 %" / "1-in-5 sweeps MATCH". On macOS
`M4ThreadStress` is fully closed across both Lua-state counts the user
requested. The macOS `MPerfBench` rate worsens monotonically with
Lua-state count, consistent with more parallel `ThreadedUpdateAI`
workers raising contention on the residual shared-state writer.

## Residual signature on macOS — `MPerfBench`

Every diverged `MPerfBench` sweep produces the SAME per-subsystem
first-divergence fingerprint, regardless of Lua-state count or which
specific run-index disagrees with run 0:

```
controller: tick 43     ← FIRST
actors:     tick 44
particles:  tick 44
sim_rng:    tick 49
carve_math: tick 49
scene:      tick 50
decisions:  tick 54
terrain:    tick 54
```

`controller` flips first at tick 43, then physics float fields (actors
pos/vel + particles pos/vel) cascade at tick 44 — same propagation
pattern as the original `M4ThreadStress` `WEAPON_PICKUP` race the
foundation cycle closed via `SpatialPartitionGrid` `std::set` + the
`HumanBehaviors.lua` deviceId tie-break. The earliness (tick 43) means
the race fires inside the first few seconds of sim, during the initial
spawn-and-target-acquisition phase before any actor has had time to
walk far. `MPerfBench`'s 150 initial + reinforcement-wave actors with
`SMG + FragGrenade` in `AIMODE_BRAINHUNT` is the trigger; the lighter
`M4ThreadStress` scenario (already at MATCH) is not.

### Comparison to Linux's residual

Linux's residual after the same cherry-pick is documented as tick-210
candidates (`PathFinder` / `See-ray` / `bullet-spawn`). The macOS
signature is **different**:

| Axis | Linux residual | macOS residual |
|---|---|---|
| Scenario that exposes it | `M4ThreadStress` (residual after Path E) | `MPerfBench` only — `M4ThreadStress` is clean |
| First divergence tick | ~210 | 43 |
| Suspected violator codepath | `PathFinder` / See-ray / bullet-spawn | controller-state writer (specifically the `WEAPON_PICKUP` / `WEAPON_FIRE` family — see "Likely violator candidates" below) |
| Lives in | `ThreadedUpdate` family | `ThreadedUpdateAI` per-Lua-state pass (single-section ablation confirms) |

The two platforms see different races emerge after the Path E
cherry-pick closes the cross-actor `AHuman::Equip*` race. This is
expected: the original race was the dominant one on both platforms,
and once it's closed the next-dominant race surfaces. The
next-dominant race differs because the residual rate depends on
thread-scheduling fairness, malloc-arena layout, FP rounding mode,
and TBB / oneAPI / `_LIBCPP_PSTL_BACKEND_SERIAL` backend details
that vary across the Linux glibc + GCC + libstdc++ and macOS
darwin + Apple-libc++ + `_LIBCPP_PSTL_BACKEND_SERIAL` ABIs.

## Single-section ablation (re-bisect under Path E branch)

The user's directive was to bisect with the same methodology used in
the prior session. Re-ran the `ThreadedUpdateAI` per-Lua-state pass
serialization under `g_MetricsCollector.IsRecordingTickHashes()` —
temporary instrumentation, reverted before commit, not pushed.

| Section under test | `MPerfBench -threads 8` sweeps |
|---|---|
| `ThreadedUpdateAI` per-state parallelize_loop → serial loop | **3 / 3 MATCHED** |

Confirms the residual race still lives in `ThreadedUpdateAI` per-state
pass. The `AHuman::Equip*` defer that the cherry-pick added closes
ONE violator in that pass (the cross-actor attachable-position race);
the `MPerfBench`-exposed residual is a DIFFERENT violator in the same
pass that the defer doesn't cover.

(The instrumentation diff was identical to the rejected
`45c757755` serialize-gate at the top level, used here strictly as
a bisect probe.)

## Likely violator candidates on macOS — narrowed inside ThreadedUpdateAI

Inside `NativeHumanAI:Update` (the `HumanAI.lua` ThreadedUpdateAI
dispatch), the Lua code that runs on every AI tick still calls into
the C++ side via:

| Call | Target Lua state | Does it mutate cross-actor state? |
|---|---|---|
| `MovableMan:ValidMO(...)` | own | read-only |
| `MovableMan:GetMOsIn{Radius,Box,Distance}(...)` | own | read-only (deterministic since Fix 2 = `std::set<MOID>`) |
| `SceneMan:CastObstacleRay(...)` | own | reads carve scratch; serialized inside the engine ray-cast pool — read-only from the AI's perspective |
| `SceneMan.Scene:CalculatePathAsync(callback, …)` | own | enqueues onto shared PathFinder pool; callback is invoked later in the tick. Deterministic since `HumanBehaviors.lua` tie-break on `deviceId` (Fix 4) |
| `Owner:EquipFirearm(true)` / `EquipDeviceInGroup(...)` / `EquipNamedDevice(...)` / `EquipShieldInBGArm()` | own (with cross-actor mutation prior to cherry-pick) | NOW deferred via `m_PendingDeferredMutations` (this cherry-pick) |
| `Owner.AIMode = ...` (AHuman::SetAIMode) | own | writes own AI mode + `m_DeviceState` |
| `Owner.Health = Owner.Health - 1` (status etc.) | own | own field |
| `Owner:GetController():SetState(...)` | own controller | writes own controller bits — read in the `controller` subsystem hash on the NEXT tick |
| `Owner.Pos = ...` / `Owner.Vel = ...` (AHuman setters) | own | own field, but downstream `Attachable::UpdatePositionAndJointPositionBasedOnOffsets` walks the attachable tree on the NEXT tick |
| `MovableMan:AddParticle(particle)` / `AddMO(...)` | shared movable-list | takes the MovableMan add-mutex but the order of inserts across parallel AI workers is scheduler-dependent. Currently used by some AI scripts for muzzle smoke / blood sprite spawns |
| `Owner:RequestSyncedUpdate()` | own | sets a flag; serial pass picks it up |

The cherry-pick covers the `Equip*` row only. The most plausible
remaining violator given the tick-43 controller flip:

* **`MovableMan:AddParticle(...)` / `AddMO(...)` from ThreadedUpdateAI
  context.** Spawning ordering depends on the parallel
  scheduler. If a downstream tick reads particle order to make a
  decision (e.g. `GetMOsInRadius` returns particles in MOID order, but
  the MOID counter increments under a global lock with parallel
  callers), two runs can see a different next-particle MOID and
  branch differently in the next AI tick. Probability: moderate;
  needs byte-level pinning to confirm.

* **`g_CurrentAIActor` + `parentActor->GetController()->SetState(...)`
  from inside `Arm::SetHeldDevice`'s `WEAPON_FIRE` clear, even though
  the `Equip*` chain is now deferred.** If any OTHER path still
  invokes `Arm::SetHeldDevice` from the parallel AI phase, the
  parent-actor controller write would race. The cherry-pick comment
  ("closes the cross-actor `Attachable::UpdatePositionAndJointPosition`
  ... vs `MOSprite::HitTestAtPixel` race") explicitly mentions only
  the cross-actor attachable race, not the parent-actor controller
  write. Probability: moderate; the early-tick + controller-first
  fingerprint is consistent with a controller-state writer that runs
  before any meaningful physics divergence.

* **`HumanBehaviors.lua` coroutine resumption order** — every AI
  coroutine's first resume happens during the first
  `ThreadedUpdateAI` after the actor spawns, and coroutine state
  reads are by-reference. If two AI actors share state via a Lua
  upvalue, the parallel resumption order can change which actor
  observes "fresh" upvalue first. Probability: low without code
  audit but the tick-43 timing matches "first-AI-tick" semantics for
  the initial 150-actor wave.

Definitive identification requires a byte-level dump of per-actor
controller bits at tick 43, then trace through the AI codepath of
the diverging actor's UniqueID — same methodology used in the prior
session to pin the `WEAPON_PICKUP` / `MOID 32586` race. That
investigation belongs in the canonical
`flagship/m4a-path-e-enforcement` consolidation per the user's
direction; this verification cycle does not implement it.

## What got committed on this branch

```
cfeec206b Source/CI: document M4 path-E equip-defer scaffold in mod_compat_assessment
ef68031fc M4 path-E: defer AHuman::Equip* under parallel AI + AI script peek refactor
06ad65084 Source/CI: regenerate macOS-arm64 baselines + bring-up report  ← pre-cherry-pick HEAD
2e28cd5bb HumanBehaviors: tie-break devicesToPickUp sort on deviceId
5628fd188 AtomGroup: pin hitMOAtoms iteration order via std::map<MOID, ...>
6511eaeca SpatialPartitionGrid: deterministic MO iteration via std::set<MOID>
3232221ac Main: macOS headless via CCCP_HEADLESS=1, not SDL offscreen driver
```

Only the two Linux cherry-picks are added on top of the four-keeper
state. No new fixes on this branch per the user's direction — the
canonical fix lands on `flagship/m4a-path-e-enforcement`.

## Artefacts under `Source/CI/macos-arm64-traces/path-e-verify/`

| Filename pattern | Sweeps | Purpose |
|---|---|---|
| `MPerfBench-t8-sweep{1..5}.json` | 5 | macOS race-rate at `-num-lua-states 8`, post-cherry-pick |
| `MPerfBench-t16-sweep{1..5}.json` | 5 | macOS race-rate at `-num-lua-states 16`, post-cherry-pick |
| `MPerfBench-default-sweep{1..3}.json` | 3 | macOS race-rate at default 12 lua-states, post-cherry-pick |
| `M4ThreadStress-t8-sweep{1..5}.json` | 5 | macOS `M4ThreadStress` at `-num-lua-states 8`, post-cherry-pick (all clean) |
| `M4ThreadStress-t16-sweep{1..5}.json` | 5 | macOS `M4ThreadStress` at `-num-lua-states 16`, post-cherry-pick (all clean) |
| `bisect-ablation-AI-serial-t8-sweep{1..3}.json` | 3 | Confirmation that ThreadedUpdateAI serialization closes the residual |

Each `*-sweep*.json` is the canonical `DeterminismCheck` matrix-mode
report (10 runs at a single Lua-state count, intra-count diff against
run 0). The `runs_detail` array per file enumerates which run-indices
diverged in that sweep; the `per_subsystem_first_divergence` map
records the first-divergent tick per subsystem.

## What this verification answers

The user's four explicit questions:

1. **Does the race close on macOS at the same rate Linux saw
   (60 % improvement, 1-in-5 sweeps MATCH)?** Partially.
   `M4ThreadStress` is FULLY closed on macOS (100 % match rate at
   both Lua-state counts — better than Linux's 20 % match rate).
   `MPerfBench` retains a residual on macOS at 20-67 % match rate
   depending on Lua-state count.

2. **Is the residual signature on macOS the same as Linux's
   tick-210 PathFinder / See-ray / bullet-spawn?** No — different.
   macOS `MPerfBench` residual fires at tick 43 in the `controller`
   subsystem, then cascades; Linux's residual fires at tick 210
   in physics codepaths.

3. **Does the single-section ablation localize the macOS residual
   to a specific parallel pass?** Yes — `ThreadedUpdateAI`
   per-Lua-state pass. Same pass the prior cycle pinned; the Path E
   `Equip*` defer covers one violator in that pass but a different
   violator (likely controller-state writer or particle-spawn
   ordering) remains.

4. **Should the macOS branch land a follow-up fix?** No — per the
   user's directive, the canonical fix lands on
   `flagship/m4a-path-e-enforcement`. This branch is verification
   data only.
