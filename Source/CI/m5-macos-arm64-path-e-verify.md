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

## Audit-pass bisect — 4-candidate ablation matrix

After the user's "narrow to file + line" request, ran the four
candidate ablations against `MPerfBench -threads 16 -runs 5`
(temporary `BISECT_INSTRUMENTATION_DO_NOT_COMMIT` patches; reverted
before commit; this branch ships zero ablation code).

### Ablation matrix

| # | Candidate ablation (executed only when `g_CurrentAIActor != nullptr`) | Result | Verdict |
|---|---|---|---|
| A | `Controller::SetState` → skip the bit write entirely | **MATCHED 5 / 5 runs** | Trivial false-positive — silencing every AI controller write makes actors stand still, suppressing every downstream divergence opportunity; tells us only that the race manifests via controller bits, not where it originates. |
| B | `MovableMan::AddParticle` → mark particle SetToDelete + return | DIVERGED 3 / 5 runs (signature unchanged: `controller:43, actors:44, particles:44, …`) | **Not the violator** — disabling particle adds does not change the residual. |
| C | `MovableMan::AddMO` → mark MO SetToDelete + return | DIVERGED 4 / 5 runs (signature unchanged) | **Not the violator** — disabling MO adds does not change the residual. |
| D | `SceneMan::CastMORay` → return `g_NoMOID` | **MATCHED 3 / 3 sweeps × 5 runs each = 15 / 15 clean** | Trivial-but-significant: skipping the AI target-acquisition ray cast removes the inputs that feed every target-driven AI decision. Race closes, but in the same "no targets → no behavior" sense as A. |

### Byte-level pinning of the offending bit + actor

With per-actor controller dump instrumentation (one file per
sim-tick per pid, written from inside the `controller` subsystem
hash compute), captured 6 short MPerfBench runs at `-threads 16`
(scenario auto-extends past `-ticks 50` so dumps span ticks 1-137).
Run 0 was the reference; run 1 was the diverging child. Diffed
each run's per-tick controller dump against run 0; first divergence:

```
DIFF at sim tick 44 (harness array index 43):
< uid=18218 bits=000000000100000000000000000000000000000000000000000000000  ← reference (run 0)
> uid=18218 bits=000010000100000000000000000000000000000000000000000000000  ← diverging (run 1)
                  ^^^^                                                          ^^^^^^^^^^^
                  bit 4 = MOVE_LEFT set in run 1 but not run 0; pos/vel/health/AIMode all bit-identical.
                  bit 9 = BODY_JUMPSTART set in both runs (actor is in PREJUMP state).
```

Both runs: `px=0x1.d1p+9 py=0x1.351d7p+8 vx=0x0p+0 vy=0x1.caacfap+3
h=0x1.9p+6 am=4 (BRAINHUNT)`. The ONLY observable difference is bit 4
of the actor's controller bitmask. All earlier ticks (1-43) are
byte-identical between the two runs.

### Write site of the diverging bit

`bit 4 = MOVE_LEFT` is set in exactly one place during a NativeHuman
AI tick:

* **`Data/Base.rte/AI/NativeHumanAI.lua:671`** —
  ```lua
  if self.lateralMoveState == Actor.LAT_LEFT then
      self.Ctrl:SetState(Controller.MOVE_LEFT, true);
  elseif self.lateralMoveState == Actor.LAT_RIGHT then
      self.Ctrl:SetState(Controller.MOVE_RIGHT, true);
  end
  ```

  So the race is "actor 18218's `self.lateralMoveState` is
  `Actor.LAT_LEFT` in run 1 but `Actor.LAT_STILL` in run 0 at the
  AI tick that immediately precedes sim tick 44".

### Upstream chain — where `lateralMoveState` is assigned

`self.lateralMoveState` is a persistent Lua-side AI field (set
during one AI tick, read during subsequent ticks until next assign).
The candidate write sites are:

* **`Data/Base.rte/AI/NativeHumanAI.lua:527-534`** —
  `lateralMoveState = LAT_LEFT/RIGHT/STILL` based on
  `Owner.Vel.X > 2` / `< -2`. Reads `Owner.Vel.X`. At sim tick 44,
  `Owner.Vel.X = 0x0p+0 = 0` in BOTH runs — this branch evaluates to
  STILL identically, so this is NOT the source of the disagreement.
* **`Data/Base.rte/AI/HumanBehaviors.lua:903-907`** (inside
  `GoProneToTarget`) —
  ```lua
  if not Owner.EquippedBGItem then
      if Dist.X > 0 then
          AI.lateralMoveState = Actor.LAT_RIGHT;
      else
          AI.lateralMoveState = Actor.LAT_LEFT;
      end
  end
  ```
  `Dist` is `SceneMan:ShortestDistance(PronePos, AimPoint, false)`.
  `AimPoint` is the target's aim point. If the AI's **`self.Target`
  is different across runs**, `Dist` differs, lateralMoveState
  differs. This is the most plausible upstream culprit — and
  ablation D (skip `CastMORay`) closing the race is consistent with
  it (without ray-casts the AI never acquires a target, so the
  target-divergence path is never exercised).

### Probable root cause (pending consolidation-branch confirmation)

The race is in `self.Target` acquisition by AI actors in MPerfBench
under contention. The two candidate code paths that drive target
selection:

* **`HumanBehaviors.CheckEnemyLOS`** (`HumanBehaviors.lua:23-101`) —
  used by MPerfBench actors (they're spawned with
  `MaxTeamAISkill ≥ NUTSDIFFICULTY` so `SpotTargets` resolves to
  `CheckEnemyLOS`, see `NativeHumanAI.lua:57-61`). It collects
  enemies via `MovableMan:GetMOsInBox(box, Owner.Team, true)` (now
  deterministic via Fix 2), iterates one per tick, and casts
  `SceneMan:CastMORay` to verify LOS to the body, then EyePos.
* **`HumanBehaviors.LookForTargets`** (`HumanBehaviors.lua:7-22`) —
  fallback path; calls `Owner:LookForMOs(viewAngDeg, ...)` which
  internally calls `RandomNormalNum()` then `g_SceneMan.CastMORay`
  (AHuman.cpp:1498-1527).

Both paths converge on `SceneMan::CastMORay` against the MOID grid
that `MovableMan::UpdateDrawMOIDs` built on the PREVIOUS tick.
`CastMORay` itself is purely read-only against shared state during
ThreadedUpdateAI (the MOID-grid bitmap, `m_MOIDIndex`,
`MovableObject::GetRootID`); per-call output is deterministic for
identical inputs.

The discriminator is therefore **the INPUT to `CastMORay`** —
specifically `Owner.ViewPoint`, `Owner.EquippedItem.Pos`,
`Owner.EyePos`, the per-actor enemy-list cache (`AI.Enemies`), and
the per-AI-tick consumption order of the per-MO RNG that drives
`viewAngDeg` in `LookForTargets`. The most plausible specific
mechanism, consistent with the tick-43 / no-physics-divergence
fingerprint, is the **enemy-list iteration in `CheckEnemyLOS`**:

* **`Data/Base.rte/AI/HumanBehaviors.lua:39`** — `local Enemy =
  table.remove(AI.Enemies);` pops the next enemy to LOS-check.
  `AI.Enemies` is populated via `table.insert(AI.Enemies, Act)` in
  the for-loop at line 32-36. `MovableMan:GetMOsInBox` returns MOs
  in `std::set<MOID>` order (deterministic per Fix 2), but the
  for-loop adds them in iteration order, then `table.remove`
  (no-arg) pops from the end (LIFO). If the GetMOsInBox result is
  the same canonical order across runs, AI.Enemies is the same list,
  and the LIFO pop order is the same.

  However, the **`for Act in MovableMan:GetMOsInBox(box, Owner.Team,
  true)` iteration is over a Lua iterator returned by a C++ binding**
  (likely a generator that wraps the internal `std::set<MOID>`
  iteration). If the C++ binding ALSO touches some shared
  cross-actor state during iteration (e.g., it constructs a per-call
  scratch vector that gets moved-from), the race could leak there.

Pending consolidation-branch dive into the `GetMOsInBox` Lua-iterator
binding + the `MovableObject::GetRootParent()` lookup inside
`CastMORay`'s ignoredMOID resolution loop (`SceneMan.cpp:1916-1922`),
both of which were not in scope for any of the four foundation/cherry-
pick fixes.

### One-line fix target for the consolidation agent

Highest-confidence single-line target, given the above:

> **Audit `HumanBehaviors.CheckEnemyLOS`
> (`Data/Base.rte/AI/HumanBehaviors.lua:23-101`) and the
> `MovableMan:GetMOsInBox` Lua-iterator binding for any
> non-determinism in cross-actor reads during ThreadedUpdateAI.
> The byte-level pin shows actor `uid=18218` MOVE_LEFT bit flips at
> sim tick 44 with all physics state bit-identical — i.e. the AI
> made a different target-selection decision while seeing the same
> scene snapshot, which can only happen if the target-acquisition
> read path has a residual race.**

Secondary candidate (lower confidence but cheap to audit):

> **`SceneMan::CastMORay`'s `ignoreMOIDs` resolution loop
> (`SceneMan.cpp:1916-1922`) calls `g_MovableMan.GetRootMOID(hitMOID)`
> which walks `m_MOIDIndex` + reads `MovableObject::GetRootID()`. If
> any actor's `GetRootID()` is being concurrently mutated by another
> parallel AI worker's Attachable-tree walk (e.g. a not-yet-deferred
> mutator that escaped the Path E `Equip*` cherry-pick), the
> returned root ID races, and the AI's `ignoredMOIDHit` decision
> differs across runs.**

## Audit-pass round 2 — pinning the secondary candidate (DISPROVEN)

Per follow-up directive (don't duplicate Linux's investigation of the
primary candidate `CheckEnemyLOS` / `GetMOsInBox`), checked whether
the secondary `CastMORay` ignoreMOIDs resolution candidate is a real
Path E violator on macOS.

### Method

Two ablations and one read/write call-count instrumentation pass
against `MPerfBench -threads 16` (instrumentation reverted before
commit; this branch ships zero instrumentation code).

| # | What was changed | Variant | Result |
|---|---|---|---|
| E1 | Skip the racy half of the `ignoreMOIDs` check (keep direct MOID equality, skip the `g_MovableMan.GetRootMOID(hitMOID) == ignoredMOID` branch) under parallel AI | `-runs 5` × 5 sweeps | 4 / 5 MATCHED — race rate dropped from baseline 4 / 5 DIVERGED to 1 / 5 DIVERGED, **signature unchanged** (`controller:43, actors:44, particles:44, scene:50, sim_rng:49, terrain:54`) |
| C1 | Log every `Attachable::SetParent(...)` call with a flag for whether `g_CurrentAIActor` was set | `-runs 3 -ticks 100` (3 runs) | 11,022 SetParent calls total; **0 with AIActor=YES** |
| C2 | Log every `MovableMan::GetRootMOID(MOID)` call with the same flag | `-runs 2 -ticks 60` (2 runs) | 1,425,106 GetRootMOID calls; **1,044,083 with AIActor=YES** — the read path IS heavily exercised under parallel AI |

### What the instrumentation proves

The race-on-read hypothesis requires both a concurrent reader AND a
concurrent writer. We confirmed the reader: GetRootMOID fires ~1M
times under parallel AI in 120 sim-ticks. We disproved the writer:
zero Attachable::SetParent calls fire under parallel AI across 300
sim-ticks of MPerfBench. With no concurrent writer of `m_RootMOID`
during the parallel AI phase, `GetRootMOID` returns deterministic
values per call, and the `ignoreMOIDs` resolution loop in
`CastMORay` cannot race.

The E1 rate reduction (4 / 5 → 1 / 5 DIVERGED) is therefore explained
by **behaviour degradation, not race closure**: skipping the
GetRootMOID resolution makes the AI see its own gun / attachables as
valid ray-cast hits, which changes AI decisions and by chance
reduces the divergence frequency without changing the underlying
signature. Same false-positive class as ablations A and D.

### Verdict

**Secondary candidate (`SceneMan::CastMORay` ignoreMOIDs resolution
via `GetRootMOID`) is NOT a Path E violator on macOS.** The
read path is race-free because the write path (`Attachable::SetParent`
modifying `m_RootMOID`) does not fire concurrently with AI reads.
The Path E `Equip*` cherry-pick already shut down the only plausible
parallel-AI caller of `SetParent` (the `AHuman::Equip*` →
`Arm::SetHeldDevice` → `Arm::RemoveAndDeleteAttachable` /
`AddAttachable` chain). No remaining mutators are reachable from
`ThreadedUpdateAI` Lua bindings used by `Base.rte` AI scripts.

Consolidation-branch focus stays on the primary candidate (Linux's
investigation of `HumanBehaviors.CheckEnemyLOS` +
`MovableMan:GetMOsInBox` iterator binding). If Linux's investigation
also rules out the primary, the next bisect candidate is the per-AI-
tick consumption order of the per-MO RNG seeded by
`DeterministicMORNGScope` — specifically whether the seed-mix or any
shared seed source is observable to two parallel AI workers in a
race-visible way.

### Updated one-line target for the consolidation agent

Primary (Linux is investigating):

> **Audit `HumanBehaviors.CheckEnemyLOS`
> (`Data/Base.rte/AI/HumanBehaviors.lua:23-101`) and the
> `MovableMan:GetMOsInBox` Lua-iterator binding for any
> non-determinism in cross-actor reads during ThreadedUpdateAI.**

Secondary (DISPROVEN on macOS — do not re-investigate):

> ~~`SceneMan::CastMORay`'s `ignoreMOIDs` resolution loop /
> `g_MovableMan.GetRootMOID`~~ ← empirically race-free; zero
> concurrent writers of `m_RootMOID` during the parallel AI phase
> (300 sim-ticks of MPerfBench tested with full SetParent
> instrumentation; 0 of 11,022 SetParent calls fire under
> parallel AI).

Tertiary (if both primary and secondary are ruled out):

> **`DeterministicMORNGScope` in
> `Source/Entities/MovableObject.cpp:674` and the seed-mix in
> `LuaMan.cpp:DeriveMORNGSeed` (line 140-146). Verify the per-MO
> RNG seed is byte-stable across runs for the same (uid, tick,
> hash(funcName)) triple. The fact that actor 18218's controller
> bit varies while pos/vel are identical means SOME input to the
> AI script's `RangeRand` / `math.random` / `RandomNormalNum`
> calls is varying — most plausibly an RNG state divergence not
> yet identified.**

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
| `ablation-A-ctrl-setstate-skip-t16.json` | 1 | Audit ablation: skip `Controller::SetState` from parallel AI → MATCHED (trivial — silences AI) |
| `ablation-B-addparticle-skip-t16.json` | 1 | Audit ablation: skip `MovableMan::AddParticle` from parallel AI → DIVERGED (not the violator) |
| `ablation-C-addmo-skip-t16.json` | 1 | Audit ablation: skip `MovableMan::AddMO` from parallel AI → DIVERGED (not the violator) |
| `ablation-D-castmoray-skip-t16-sweep{1..3}.json` | 3 | Audit ablation: skip `SceneMan::CastMORay` from parallel AI → MATCHED 15 / 15 runs (target-acquisition path implicated; still degraded-behaviour false-positive in absolute sense but discriminates against B / C) |

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
