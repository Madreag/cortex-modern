# MP M4 — threaded-sim determinism: baseline + shared-state audit (Block A)

This is the **Block A landing-state report** for MP M4 (multi-threaded sim with
deterministic merge). It captures two things, before any threading code is
changed:

1. The **shared-state audit** — a complete enumerated list of every shared
   mutable state read-written from the sim's parallel passes, each item
   classified by the determinism hazard it represents. This list is the *spec*
   for Blocks B–E.
2. The **baseline thread-count divergence** — the `M4ThreadStress` scenario run
   through the new `--threads 1,2,4,8,16` determinism-check matrix, showing the
   starting state Blocks B–E drive to zero.

> Run locally: see [How to run](#how-to-run) below.

## What M4 is (and is not)

M4 makes the sim's **existing** threaded work deterministic — the
`ThreadedUpdate` / `ThreadedUpdateAI` AI-Lua passes and see-ray vision casting
that *already* run in parallel. It closes M1's documented residual within-OS
thread race and flips the determinism CI gate from informational to blocking.

M4 does **not** parallelize the physics integrator `MovableMan::Travel()` and
does **not** touch the synchronous Lua collision callbacks (`OnCollideWithMO` /
`OnCollideWithTerrain`). `Travel()` stays serial; the collision callbacks keep
firing serially within it. M4 is a *determinism* milestone, roughly
perf-neutral — not the "4–8× throughput" effort (that is a separate, post-M6
effort gated on its own ADR).

## The test: the thread-count matrix

M4's acceptance criterion is the Factorio FFF-415 acid test: **the per-tick
checksum must be bit-identical regardless of how many threads the sim runs on.**

`-determinism-check --threads 1,2,4,8,16` runs the chosen scenario once (×`--runs`)
at each Lua-state count — forced via `SettingsMan::SetNumberOfLuaStatesOverride`,
which sizes `m_ScriptStates` (`LuaMan::Initialize`) and therefore the
parallel-pass width — and diffs the per-tick BLAKE3 traces *across* the counts.
A residual race shows up there and nowhere else is authoritative.

`M4ThreadStress` (`Data/Tests.rte/Activities/M4ThreadStress.lua`) is the matrix
scenario: 40 brain-hunting Green Dummies on a deterministic grid, 900 ticks. The
count is well above the 16-state ceiling so every Lua state carries several
actors and the parallel passes are genuinely saturated at every thread count.

## The three parallel passes (source-verified)

The sim is not single-threaded. Three parallel passes run every tick, verified
in `Source/Managers/MovableMan.cpp`:

| Pass | Site | Unit of parallelism |
|---|---|---|
| `ThreadedUpdate` script hook | `MovableMan.cpp:1404` `parallelize_loop` over Lua states | one Lua state per chunk |
| `ThreadedUpdateAI` | `MovableMan.cpp:1950` `parallelize_loop` over Lua states | one Lua state per chunk |
| See-ray vision casting | `MovableMan.cpp:1832` `parallelize_loop` over `m_Actors` | index range of `m_Actors` |

`MovableMan::Travel()` (`:1879`) is three serial `for` loops — M4 leaves it
serial. The serial `SyncedUpdate` pass (`:1421`) is opt-in and ordered by Block E.

## The shared-state audit

Method: read the call graphs of `CastSeeRays` / `Look` / `AHuman::Look` /
`ACrab::Look`, the `ThreadedUpdate` / `ThreadedUpdateAI` dispatch, and the C++
bindings reachable from threaded AI Lua; grep `Data/Base.rte` for vanilla Lua
threaded hooks. Every shared mutable state touched from a parallel pass is below,
classified as **RNG**, **iteration order**, **cross-worker mutation**, or
**benign** (with the justification a benign classification requires).

### Class RNG — closed by Block C

| Item | Evidence | Note |
|---|---|---|
| See-ray look direction / ray jitter → `g_SimRNG` | `Actor.cpp:611,616`; `AHuman.cpp:1373,1386,1413`; `ACrab.cpp:692,727` (free `RandomNum`/`RandomNormalNum`) | The headline race — 7 sites consume the global sim RNG from see-ray workers. |
| Threaded AI/script → C++ binding → `g_SimRNG` | `Actor::DropAllInventory/DropAllGold` `Actor.cpp:774-848`; `MOSRotating::GibThis` `MOSRotating.cpp:894-1068`; `AEmitter::Update`/`PEmitter` emission `AEmitter.cpp:494-505`; `AHuman` throw/reach `AHuman.cpp:1892-2050` | Reached when a threaded hook calls a binding that draws (e.g. an AI script calling `:GibThis()`). The `thread_local` C++ RNG redirect catches all of them at the dispatch site. |
| Threaded-hook `math.random` → per-Lua-state `m_RandomGenerator` | `LuaStateWrapper::SelectRand/RangeRand/PosRand/NormalRand` `LuaMan.cpp:417-431`; vanilla users: `Flame.lua`, `GroundFlame.lua`, `RevolverCylinderReload.lua`, `ActorSpawner.lua` | The per-state RNG is seeded per state index; the actor→state assignment is machine-thread-count-dependent, so the same actor draws a different stream at different thread counts. Block C redirects threaded-hook `math.random` to a per-MO source. |

The `lua_state` SimChecksum subsystem currently hashes the master state RNG **plus
every threaded state's RNG** (`LuaMan::HashAllLuaStatesIntoSimChecksum`). The
number of threaded states *is* the thread count, so this subsystem cannot be
bit-identical across thread counts by construction. Block C resolves this: once
threaded per-MO Lua work is redirected off the per-state generators, those
generators no longer carry sim-observable state, and `lua_state` hashes the
**master state only** (count-invariant).

### Class iteration order — closed by Block B

| Item | Evidence | Note |
|---|---|---|
| `LuaStateWrapper::m_RegisteredMOs` iterated in pointer-hash order | `unordered_set<MovableObject*>` `LuaMan.h:257`; iterated at `MovableMan.cpp:1396` (ThreadedUpdate master), `:1410` (ThreadedUpdate parallel), `:1427` (SyncedUpdate master), `:1437` (SyncedUpdate serial) | Pointer-hash order depends on allocation addresses and insertion history — not reproducible. Two MOs on one state both drawing `math.random` interleave by this order. Block B gives the iteration a MOID order. M1 Block C sorted `MovableMan`'s deques but not these per-state sets. |
| Actor → Lua-state assignment is creation-order round-robin | `LuaMan::GetAndLockFreeScriptState` `LuaMan.cpp:520-526` (`m_LastAssignedLuaState`) | Deterministic *if* spawn order is deterministic (M1–M3 make it so). Block B makes the dependence self-evident. The `ThreadedUpdateAI` parallel loop already iterates `m_Actors` (MOID-sorted by M1 Block C), so only the `ThreadedUpdate`/`SyncedUpdate` `m_RegisteredMOs` iteration is unordered. |

### Class cross-worker mutation — closed by Block D

| Item | Evidence | Note |
|---|---|---|
| `m_AddedAlarmEvents` append order | `MovableMan::RegisterAlarmEvent` (mutex-guarded append); drained frame-start `MovableMan.cpp:1330-1333` into `m_AlarmEvents` | Alarm events are appended in worker-race order and the drain preserves that order. AI reads `m_AlarmEvents`. Block D sorts the added-events by a stable key before the drain — the same treatment M1 Block C gave `m_AddedActors`. |

This is the **only** genuine cross-worker mutation the audit surfaced — Block D
is correspondingly small (the plan anticipated this: "if Block A finds the only
races were RNG + iteration order, Block D is small or empty").

### Class benign — justified, no block needed

| Item | Evidence | Why benign |
|---|---|---|
| `m_AddedActors` / `m_AddedItems` / `m_AddedParticles` | mutex-guarded append `MovableMan.cpp` `AddActor`/`AddItem`/`AddParticle`; sorted by `m_UniqueID` before drain (M1 Block C) | The sort-before-drain makes the merged order independent of append race order. |
| `m_ValidActors` / `m_ValidItems` / `m_ValidParticles` | mutex-guarded `unordered_set` insert | Set membership is commutative — the final set is append-order-independent. |
| Fog-of-war unseen-layer reveal | `SceneMan::RevealUnseenBox` / `RestoreUnseenBox` `SceneMan.cpp:1071-1107` (`rectfill` pixel writes, unguarded) | A monotonic pixel fill — overlapping writes from multiple see-ray workers produce an order-independent result. FoW is not a checksummed subsystem; it feeds rendering/vision, and the see-ray RNG that *drives* the reveal pattern is itself made per-actor-deterministic by Block C. |
| AI decision channel | `AIDecisionChannel::Emit` mutex-guarded; `Drain` content-sorted; `FeedToChecksum` resolves `StringId`→content (M1 Block F) | Emit is thread-safe; the drain sorts by event content (not the racy `sequence` atomic), so the `decisions` subsystem is append-order-independent. |
| MOID-layer draw task (`UpdateDrawMOIDs`) | `submit` at `MovableMan.cpp:1844`, waited `:2091` | Feeds rendering and the next frame's MOID lookup; not a checksummed sim subsystem. Treated as outside the determinism island. |

### Out of M4 scope — documented residual

The synchronous Lua collision callbacks `OnCollideWithMO` / `OnCollideWithTerrain`
(`MovableObject.cpp:766,778`) fire from inside serial `Travel()`. They are
serial and deterministically ordered, so they are not a *threading* race. They
do, however, read `math.random` from the colliding MO's per-Lua-state RNG, whose
state index is thread-count-dependent — so a mod that draws RNG inside a
collision callback has a residual count-dependency. M4 deliberately does not
touch the `OnCollideWith*` path (it is the rock the previous multi-threaded-physics
prototype broke on); `M4ThreadStress` exercises the in-scope threaded paths, not
collision-callback RNG. This residual is owned by the future collision-callback
contract rework, not M4. Vanilla `Base.rte` collision callbacks that draw RNG
(`Flame.lua`) are confirmed by the Block F mod-corpus pass not to affect the
matrix in practice.

## Expected baseline divergence

At Block A — before Blocks B–E — the `M4ThreadStress` thread-count matrix is
**expected to diverge**. That is M1's documented residual within-OS thread race,
now measured directly at 2/4/8/16 threads. The CI matrix job is therefore
non-blocking (`continue-on-error: true`) at Block A; Block F flips it to blocking
once Blocks B–E have closed the race.

<!-- BLOCK-A-BASELINE-START -->
Measured 2026-05-20 — `M4ThreadStress`, 900 ticks, seed 42, `--threads 1,2,4,8,16 --runs 2`
(10 child processes, Windows / Final x64):

```text
[determinism-check] scenario: M4ThreadStress
[determinism-check] mode    : thread-count matrix
[determinism-check] threads : 1,2,4,8,16  (x2 runs each = 10 total)
[determinism-check] RESULT  : DIVERGED
    first_divergence_tick:  0
    total_mismatched_ticks: 904 / 904
    per-subsystem first divergence:
        lua_state:  tick 0
        sim_rng:    tick 1
        decisions:  tick 3
        carve_math: tick 5
        particles:  tick 7
        actors:     tick 10
        scene:      tick 10
        terrain:    tick 10
```

Every thread count diverges, and the two same-count runs diverge from each other
too — the within-OS scheduling race M1 documented, now measured directly. Reading
the per-subsystem order:

- **`lua_state` t0** — diverges immediately *by construction*: it hashes every
  threaded Lua state's RNG, and the number of threaded states *is* the thread
  count, so the hash spans a different object count at 1 vs 16. Block C resolves
  this — once threaded per-MO Lua work is redirected off the per-state RNGs, those
  RNGs stop carrying sim-observable state and `lua_state` hashes the master only.
- **`sim_rng` t1** — the headline race: `g_SimRNG` consumed from see-ray / threaded
  workers. Block C's per-MO RNG redirect closes it.
- **`decisions` / `carve_math` / `particles` / `actors` / `scene` / `terrain`** —
  all downstream of the RNG race; they fall back into line as Blocks C/D close it.
- **`tick`** does not appear → the sim frame number matches across every thread
  count, confirming the trace mechanism itself is sound.
<!-- BLOCK-A-BASELINE-END -->

The `tick` subsystem (sim frame number) must match across every thread count
already at Block A — it is the sanity check that the trace mechanism itself is
sound. If `tick` ever diverges, investigate the harness before chasing a real
race.

## The spec for Blocks B–E

| Block | Closes | Mechanism |
|---|---|---|
| **B** | iteration order | MOID-ordered iteration of `m_RegisteredMOs` in the threaded/synced passes; deterministic actor→state assignment. |
| **C** | RNG (the headline) | `thread_local` redirect of the C++ `g_SimRNG` free functions and the Lua per-state RNG to a per-MO generator, deterministically seeded from `(uniqueID, tick, phase)`. `lua_state` checksum narrowed to the master state. |
| **D** | cross-worker mutation | Sort `m_AddedAlarmEvents` by a stable key before the frame-start drain. |
| **E** | the `SyncedUpdate` boundary | MOID-order the serial `SyncedUpdate` pass; document it as the channel for script-driven sim-state mutation. |
| **F** | the gate | Flip the determinism CI gate (incl. this matrix) to blocking; docs; mod corpus. |

Anything that diverges at 2/4/8/16 threads after Block E and is *not* on this
list is an audit miss — the matrix is the backstop, and Block F does not close
until it is fully MATCHED.

## M4 outcome (Block F)

Blocks B–E and the render-RNG fix drove the matrix from "diverges at tick 0" to:

- **Same thread count — fully deterministic.** A fixed Lua-state count is now
  bit-identical run-to-run; the within-OS scheduling race M1 documented is closed.
- **Across thread counts — bit-identical for the first ~135 ticks** of
  `M4ThreadStress` (1/2/4/8/16).

What each block closed:

- **B** — MOID-ordered the threaded / synced registered-MO iteration.
- **C** — the headline: the per-MO RNG redirect took the threaded `g_SimRNG`
  race off worker threads; `lua_state` narrowed to the master state. First
  divergence moved tick 0 → tick 8.
- **render-RNG redirect** — closed a sim/render RNG-split gap (cosmetic `Draw`
  `RandomNum` draws were drifting `g_SimRNG` a draw-rate-dependent amount). First
  divergence tick 8 → ~136, and same-thread-count divergence closed entirely.
- **D** — alarm-event content-ordering + synchronous pathing in determinism
  traces.
- **E** — the `SyncedUpdate` boundary documented.

**The residual.** A cross-thread-count divergence emerges around tick 136: the
particle / terrain physics (`terrain` / `carve_math` / `particles`) diverges
between thread counts with identical MO counts — divergent per-particle state,
not a divergent spawn. It is a count-dependent effect in the threaded→serial-
physics interaction; the leading candidates are collision-callback per-Lua-state
RNG (`OnCollideWith*` is deliberately left on the per-state generator, out of M4
scope) and cross-actor AI coordination through per-Lua-state Lua globals. Per
`M4_PLAN.md` §7 this is the documented M4-follow-up; the determinism gate stays
informational until it is closed.

## How to run

### Windows

```pwsh
& ".\Cortex Command.exe" -determinism-check `
    --scenario M4ThreadStress `
    --ticks 900 `
    --seed 42 `
    --threads 1,2,4,8,16 `
    --runs 2 `
    --output M4ThreadStress-matrix.json
```

### Linux

```bash
./CortexCommand -determinism-check \
    --scenario M4ThreadStress \
    --ticks 900 \
    --seed 42 \
    --threads 1,2,4,8,16 \
    --runs 2 \
    --output M4ThreadStress-matrix.json
```

`--threads` runs the scenario at each listed Lua-state count and diffs the
per-tick traces across counts; `--runs` is the runs-per-count (≥2 also catches
same-count scheduling races). `--keep-runs` retains the per-run JSONs for a
deeper dive. Exit code 0 = MATCHED, 1 = DIVERGED, 2 = usage/spawn error.

## References

- `D:\Projects\M4_PLAN.md` — the M4 plan (Block A §4, the audit spec §3).
- `Source/CI/DeterminismCheck.{h,cpp}` — the `--threads` matrix orchestrator.
- `Source/CI/baseline_divergence.md` — the M1 Block A sibling report.
- `Data/Tests.rte/Activities/M4ThreadStress.lua` — the matrix scenario.
- `.github/workflows/determinism.yml` — the non-blocking matrix CI job.
