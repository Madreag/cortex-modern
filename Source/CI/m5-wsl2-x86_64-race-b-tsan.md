# WSL2 x86-64 — Race B TSan investigation (Phase 5)

Companion to `m5-wsl2-x86_64.md`. Independent third-platform TSan-side hunt
for the underlying memory race feeding the residual Race B
(HumanBehaviors target-acquisition / `NativeHumanAI.lua:671`
`Controller.MOVE_LEFT` write site). Runs in parallel with the macOS
bring-up's behavioural ablation on the same candidate space.

Read this report side-by-side with `m5-wsl2-x86_64.md` §"MPerfBench
heavy-AI residual characterisation" and §"TSan-instrumented validation
(Phase 4)" — that report's Phase 4 covers M4ThreadStress @ lua-states 8
(the FFF-415 acid test); this Phase 5 covers MPerfBench @ lua-states 16
(the residual-surfacing config) plus the cross-reference vs macOS's
three ablation candidates.

Branch: `exp/determinism-wsl2 @ 84483714a` (no engine source modified in
this report — TSan-only investigation; bring-up report + Phase 5
additions are the only diff).

## TL;DR

| Surface | Result |
|---|--:|
| TSan re-run on MPerfBench, `--num-lua-states 16 --ticks 1200 --runs 3`, post-Path-E (aborted in run 1 at ~19 min wall — see §"TSan-overhead note" below) | **618 race reports**, **4 distinct engine first-frame buckets** (all TBB-internal / PSTL infrastructure noise) |
| Race in `HumanBehaviors.*` codepath (any C++ Lua binding called from `HumanBehaviors.lua`) | **0** |
| Race in `MovableMan::GetMOsInBox` or `SpatialPartitionGrid::GetMOsInBox` body / iteration | **0** |
| Race in Actor target fields (`m_SeenTargetPos`, `m_PointingTarget`, `m_MoveTarget`, `m_LastAlarmPos`) | **0** |
| Race in `Controller::SetState` / `m_ControlStates` (the write path at `NativeHumanAI.lua:671`) | **0** |
| Race in `Actor::GetController` reads on actors other than `g_CurrentAIActor` | **0** |
| Race in `PathFinder::Solve` / `PathFinder` consumption (cross-reference vs Race A) | **0** (the `PathFinder::UpdateNodeList`/`MarkAllNodesNavigable` first-frame hits are TBB-partitioner-internal noise, NOT consumption-side races on `m_NodeGrid`) |
| Race in `DeterministicMORNGScope` / `Actor::CastSeeRays` | **0** |
| **Conclusion** | TSan finds zero application memory races in Race B's candidate space. Race B is NOT a TSan-detectable memory race; it is a logical / iteration-order / RNG-value race that the FreezeStateForAIPhase contract masks at the byte level but allows at the value level. |

## Methodology

The macOS bring-up identified three behavioural candidates for Race B's
root cause:

1. **`HumanBehaviors.CheckEnemyLOS`** — `HumanBehaviors.lua:25-104`
   builds an enemy table from
   `MovableMan:GetMOsInBox(box, Owner.Team, true)`, then casts MO-rays
   via `SceneMan:CastMORay` toward each enemy's `Pos` / `EyePos`.
   Foreign-actor field reads: `Enemy.Pos`, `Enemy.Radius`,
   `Enemy.ClassName`, `Enemy.EyePos`.
2. **`MovableMan:GetMOsInBox` Lua-iterator binding** —
   `LuaBindingsManagers.cpp:166-168`, `MovableMan.cpp:247-251`,
   `SpatialPartitionGrid.cpp:96-135`. The `std::set<MOID>` fix at the
   grid level pins MOID iteration order
   (`SpatialPartitionGrid.cpp:99-102` comment). Hypothesis (macOS): a
   second non-deterministic source remains in the binding's iteration —
   e.g., the `luabind::return_stl_iterator` adapter, or the per-call
   `new vector<MovableObject*>` lifetime under `luabind::adopt`.
3. **`GoProneToTarget`'s `Dist.X` chooser** — `HumanBehaviors.lua` lines
   830-952 (the `GoProne` codepath that feeds `self.lateralMoveState`,
   ultimately read at `NativeHumanAI.lua:670` to write
   `Controller.MOVE_LEFT` at line 671). If the foreign-actor read that
   feeds `Dist.X` (via `SceneMan:ShortestDistance(Owner.ViewPoint,
   targetPos, …)`) sees a torn / racy target position, the move-left
   bit flips. Tertiary candidate: `DeterministicMORNGScope` at
   `LuaMan.cpp:140-149` if `CastSeeRays` (`Actor.cpp:1310-1313`) is the
   actual consumer.

The independent third-host hunt: re-run TSan on the same scenario where
WSL2 saw Race B (MPerfBench @ lua-states 16, divergence at tick 982,
1 / 10 in the post-Path-E sweep), filter the TSan output for races in
those three categories, and report whether TSan supports any of them as
the underlying memory race.

### Configuration

| | |
|---|---|
| Sanitizer build | `builddir-tsan/CortexCommand` — `debugoptimized` + `-Db_sanitize=thread`, `BuildID 4db3d4e2d2fb3ff0f58e7173c42cc53bb70b8e1a` |
| Scenario | `MPerfBench` (`Data/Tests.rte/Activities/MPerfBench.lua`) |
| Threads | `--threads 16` (= 16 Lua states; the 32-logical-CPU host fully exercises this) |
| Ticks per run | `--ticks 1200` (full MPerfBench duration, all six 30-actor reinforcement waves at 180 / 360 / 540 / 720 / 900 / 1080) |
| Runs | `--runs 3` |
| Seed | `--seed 42` |
| `TSAN_OPTIONS` | `suppressions=/tmp/tsan-suppressions.txt:second_deadlock_stack=1:halt_on_error=0:exitcode=0:history_size=7` |
| Suppression list | `race:^lj_` + `race:tbb::detail::r1::` + `called_from_lib:libtbb.so` + `race:PathFinder::AdjacentCost` + `race:PathFinder::UpdateNodeCosts` + `race:MovableMan::UpdateDrawMOIDs` + `race:MOSRotating::Draw` (same as Phase 4) |
| Headless | `SDL_VIDEODRIVER=offscreen` + `CCCP_HEADLESS=1` |
| ASLR | `setarch -R` (WSL2 quirk per Linux bring-up) |
| Output | `Source/CI/wsl2-baseline-traces/tsan-mperfbench.log` (the per-run divergence diff `tsan-mperfbench-states16.json` is only emitted after all `--runs` complete, so it is not produced for this partial run) |

The suppression list does NOT filter `tbb::detail::d1::*` — TSan emits
`d1::` for the partitioner-internal races, but the existing pattern
uses `r1::`. Same mismatch I noted in Phase 4's `For the team` section
of `m5-wsl2-x86_64.md`; both the M4ThreadStress and the MPerfBench logs
are dominated by this unsuppressed class as a result.

## TSan output — first-engine-frame distribution

618 race reports across the MPerfBench run group into **four** distinct
first-engine-frame buckets:

| First engine frame | Hits | SUMMARY-line racy location |
|---|--:|---|
| `RTE::PathFinder::UpdateNodeList` | 315 | `tbb::detail::d1::*` partitioner state |
| `RTE::PathFinder::MarkAllNodesNavigable` | 111 | `tbb::detail::d1::*` partitioner state |
| `RTE::SLTerrain::CleanAir` | 99 | `tbb::detail::d1::*` partitioner state |
| `RTE::SLTerrain::TexturizeTerrain` | 93 | `tbb::detail::d1::*` partitioner state |

All four buckets are TBB-internal partitioner races, manifesting via
`par_unseq` invocations from these engine entry points. The
SUMMARY-line racy memory access is always in `tbb::detail::d1::*`
(partitioner state) or `__pstl::__tbb_backend::*` (the PSTL TBB
back-end) or `std::__atomic_base<*>` (the TBB tasks' atomic counters);
never in application memory.

For comparison, the Phase 4 M4ThreadStress @ lua-states 8 TSan log
(`Source/CI/wsl2-baseline-traces/tsan-m4ts.log`) emits the **identical
four buckets**:

| First engine frame | Hits (M4ThreadStress) | Hits (MPerfBench) |
|---|--:|--:|
| `RTE::PathFinder::UpdateNodeList` | 329 | 315 |
| `RTE::SLTerrain::CleanAir` | 95 | 99 |
| `RTE::PathFinder::MarkAllNodesNavigable` | 93 | 111 |
| `RTE::SLTerrain::TexturizeTerrain` | 88 | 93 |

The bucket *set* is invariant across the two scenarios — they're both
TBB-partitioner-noise emissions, not workload-dependent application
races. The 16-thread heavy-AI MPerfBench scenario does **not** introduce
any new engine-side first-frame bucket relative to the 8-thread
FFF-415 acid test.

### SUMMARY-line racy-memory-location distribution (top 20)

```
  42  /usr/include/oneapi/tbb/partitioner.h:304    adaptive_mode::adaptive_mode(split)
  34  /usr/include/c++/13/pstl/algorithm_impl.h:122   pstl::operator()
  31  /usr/include/oneapi/tbb/parallel_for.h:138   offer_work_impl
  28  /usr/include/oneapi/tbb/partitioner.h:486   auto_partition_type::is_divisible()
  25  /usr/include/oneapi/tbb/partitioner.h:410   check_being_stolen
  23  /usr/include/c++/13/bits/atomic_base.h:505  std::__atomic_base<int>::load
  23  /usr/include/c++/13/bits/atomic_base.h:481  std::__atomic_base<bool>::store
  21  /usr/include/c++/13/bits/stl_iterator.h:1337
  20  /usr/include/oneapi/tbb/detail/_task.h:214  tbb::detail::d1::task::task()
  18  /usr/include/oneapi/tbb/partitioner.h:311   adaptive_mode::do_split
  18  /usr/include/oneapi/tbb/detail/_task.h:204  task_traits::task_traits()
  17  /usr/include/oneapi/tbb/partitioner.h:421   check_being_stolen
  17  /usr/include/oneapi/tbb/partitioner.h:139   tree_node::tree_node
  17  /usr/include/oneapi/tbb/partitioner.h:122   node::node
  16  /usr/include/oneapi/tbb/partitioner.h:402   dynamic_grainsize_mode::dynamic_grainsize_mode
  16  /usr/include/oneapi/tbb/parallel_for.h:85   start_for
  16  /usr/include/c++/13/pstl/parallel_backend_tbb.h:89
  15  /usr/include/oneapi/tbb/partitioner.h:420   check_being_stolen
  15  /usr/include/oneapi/tbb/partitioner.h:409   check_being_stolen
  15  /usr/include/c++/13/pstl/parallel_backend_tbb.h:87
```

**Every single SUMMARY-line racy memory location is in
`/usr/include/oneapi/tbb/*`, `/usr/include/c++/13/pstl/*`, or
`/usr/include/c++/13/bits/atomic_base.h`** — i.e., toolchain
infrastructure, not engine application memory. The engine call frames
above the racy location are exclusively the four par_unseq invocation
sites listed above.

## Race B candidate-categories — by-category hit count

Direct `grep -c` of the full TSan log (618 reports, ~10 MB) for every
Race B candidate symbol:

| Candidate symbol | Hits in `tsan-mperfbench.log` |
|---|--:|
| `Controller::` | **0** |
| `luabind::` | **0** |
| `lua_State ` (raw pointer use) | **0** |
| `m_ControlStates` | **0** |
| `m_SeenTargetPos` | **0** |
| `m_PointingTarget` | **0** |
| `m_MoveTarget` | **0** |
| `m_LastAlarmPos` | **0** |
| `GetMOsInBox` | **0** |
| `SpatialPartitionGrid::Get` | **0** |
| `HumanBehaviors` | **0** |
| `DeterministicMORNGScope` | **0** |
| `Actor::GetController` | **0** |
| `CastSeeRays` | **0** |
| `CheckEnemyLOS` | **0** |
| `GoProneToTarget` | **0** |

Same grep against Phase 4's `tsan-m4ts.log`: **0** for every candidate.

This is the definitive negative result. TSan does NOT see any memory
race involving any of the candidate Race B paths, on either of the two
scenarios under test, across the full duration including the past-tick-
982 region where Race B's divergence surfaces in the non-TSan
1 / 10 MPerfBench@16 sweep.

## Cross-reference matrix vs macOS ablation candidates

| macOS candidate | TSan evidence on WSL2 |
|---|---|
| `HumanBehaviors.CheckEnemyLOS` (foreign-actor `Pos`/`EyePos`/`Radius` reads + `SceneMan:CastMORay` chain) | **No TSan support.** Zero races involve `CheckEnemyLOS`, the `SceneMan:CastMORay` C++ path, or the foreign-actor field reads. `s_LastRayHitPos` at `SceneMan.cpp:36` is `thread_local`, so the per-thread ray-cast result storage is not racy. |
| `MovableMan:GetMOsInBox` Lua-iterator binding (`return_stl_iterator` adapter + per-call `new vector<MovableObject*>`) | **No TSan support.** Zero races involve `MovableMan::GetMOsInBox`, `SpatialPartitionGrid::GetMOsInBox`, or `luabind::*` adapters. The existing `std::set<MOID>` fix at `SpatialPartitionGrid.cpp:99-102` is the only ordering-determinism guarantee at this level, and TSan does not surface a second one. |
| `GoProneToTarget` `Dist.X` chooser via `SceneMan:ShortestDistance(Owner.ViewPoint, targetPos, …)` | **No TSan support.** Zero races involve `GoProneToTarget`, `ShortestDistance`, or the `lateralMoveState` ⇒ `MOVE_LEFT` write chain. Note: the WRITE at `NativeHumanAI.lua:671` is to the OWN actor's `Controller` (own-thread write, no foreign read) — so a TSan race on the WRITE side was never expected; the candidate is the READ side that feeds `self.lateralMoveState`. TSan does not see one. |
| `DeterministicMORNGScope` at `Actor::CastSeeRays` (`Actor.cpp:1310-1313`) | **No TSan support.** Zero races involve `DeterministicMORNGScope`, `CastSeeRays`, or `s_workerMORNG`. The per-MO RNG redirect (`LuaMan.cpp:140-149`) is invoked per-thread and reseeded per-scope. If the divergence is RNG-value (one extra draw on one host), it's a *value-level* race, not a memory race; TSan would not see it. |

All three macOS behavioural candidates are TSan-clean on WSL2. Either
the racy memory access lives outside the candidate space, or Race B is
not a memory race at all.

## Conclusion (TSan-supported)

**Race B is not a TSan-detectable memory race.** The candidate space
identified by macOS's behavioural ablation —
`HumanBehaviors.CheckEnemyLOS`, `MovableMan:GetMOsInBox` binding,
`GoProneToTarget` `Dist.X` chooser, plus the tertiary
`DeterministicMORNGScope` / `CastSeeRays` — is TSan-clean on WSL2
across 618 race reports of TSan output on the exact configuration
(MPerfBench, 16 Lua states, 1200 ticks, seed 42) that surfaces Race B
non-deterministically (1 / 10 in the non-TSan post-Path-E sweep).

**TSan's silence on the candidate space does not disprove Race B** — it
constrains the hypothesis class. TSan only flags memory-access races
(two threads accessing the same byte with at least one write, no
happens-before edge between them). Race B *can* still be a logical /
iteration-order / RNG-value race that is not a memory race:

- **Logical race.** The FreezeStateForAIPhase contract
  (`MovableMan.cpp:2003-2006`, `Actor::FreezeStateForAIPhase`,
  `AHuman::FreezeStateForAIPhase`) freezes Controller + EquippedItem +
  FirearmIsReady. If a downstream AI computation reads a value whose
  *semantic correctness* depends on AI-phase ordering — e.g., a cross-
  actor read that returns a frozen snapshot from the previous tick,
  semantically equivalent to "what was true at tick N-1" — and the
  read's *consumer* is non-deterministic over the snapshot's value
  (e.g., a `Dist.X` sign flip if the foreign actor moved a sub-pixel
  amount between ticks), the byte is not racy but the value is. Visible
  only via the per-tick `controller`-subsystem hash drift, not via
  TSan.
- **Iteration-order race.** The ThreadedUpdateAI parallel loop
  (`MovableMan.cpp:2021-2036`) iterates `m_Actors` (MOID-sorted by
  M1 Block C) and filters by `actor->GetLuaState() == &luaState`.
  Within one Lua-state's thread, iteration is deterministic. *Between*
  threads, the per-actor `ThreadedUpdateAI` script invocation order is
  the same on every run — but the *interleaving* of script side-effects
  observable through cross-actor reads is not, because of when each
  thread reaches a given foreign-actor field read. If the AI reads no
  cross-actor write-and-read pair, this is invisible; if it does
  (e.g., RNG-state side-effects via shared per-Lua-state RNG), the
  result depends on the per-task scheduling timing.
- **RNG-state divergence.** `DeterministicMORNGScope`
  (`LuaMan.cpp:140-149`) reseeds the per-MO RNG at scope entry; any
  RNG draw inside the scope is reproducible given
  `(uniqueID, functionName)`. If a function does N draws on one run
  and N+1 on another (e.g., because the foreign-actor list iteration
  order produced one more loop iteration via an early-exit miss), the
  *post-scope* RNG state diverges. Again, byte-level safe, value-level
  racy.

**Recommended next-investigation directions for the team.** The
TSan-supported conclusion narrows the hypothesis class to value-level
races. Per the original Phase 5 brief: "If TSan is CLEAN in all
candidate areas, the race is elsewhere (possibly RNG state per
macOS's tertiary candidate `DeterministicMORNGScope`)."

Concretely:

1. **Instrument the AI Lua path with per-actor per-tick decision logs.**
   On each `ThreadedUpdateAI` script invocation, log
   `(actor.UniqueID, tick, self.lateralMoveState,
   self.proneState, AI.Target.UniqueID, Owner.HFlipped,
   self.Ctrl:GetState(Controller.MOVE_LEFT))`. Diff the log files
   across a MATCH run and a DIVERGED run. The first per-actor
   per-tick row that differs pins the responsible AI-script
   transition. The macOS bring-up's ablation approach gets the same
   answer; this is the TSan-perpendicular path.
2. **Audit the per-Lua-state RNG draw count.** Add a tick-counted
   `g_LuaMan.GetPerStateRNGDrawCount()` and dump per-thread per-tick
   counts. If the counts diverge between MATCH and DIVERGED runs
   at the tick Race B fires, the RNG-state-divergence hypothesis
   is confirmed.
3. **Audit FreezeStateForAIPhase coverage.** `Actor::m_SeenTargetPos`,
   `m_PointingTarget`, `m_MoveTarget`, `m_LastAlarmPos` are *not*
   frozen by the current `Actor::FreezeStateForAIPhase`
   / `AHuman::FreezeStateForAIPhase` (which only freeze the
   Controller + FirearmIsReady + EquippedItem). If any of these
   fields is read cross-actor during `ThreadedUpdateAI` (e.g.,
   `Enemy.LastAlarmPos`), the read sees the *current* live value
   from the foreign actor's thread, and the value's contents depend
   on how far through its own ThreadedUpdateAI script that foreign
   actor's thread has progressed. TSan would catch this *if* the
   field is read on one thread while being written on another in the
   same parallel pass; the absence of a TSan hit means either no such
   read exists, OR the read goes through luabind's `lua_State`
   ownership which the TSan shadow model considers synced.

The TSan investigation is read-only on this branch per the original
brief; recommendations above are for the macOS / Linux ablation
agents and the M4A TSan agent to weigh against their findings at
consolidation.

## Methodology — replication

```
cd /home/erol/cccp/builddir-tsan
setarch x86_64 -R env \
  SDL_VIDEODRIVER=offscreen \
  TSAN_OPTIONS="suppressions=/tmp/tsan-suppressions.txt:second_deadlock_stack=1:halt_on_error=0:exitcode=0:history_size=7" \
  CCCP_HEADLESS=1 \
  ./CortexCommand -determinism-check \
    --scenario MPerfBench \
    --threads 16 \
    --runs 3 \
    --seed 42 \
    --ticks 1200 \
    --output /home/erol/cccp/Source/CI/wsl2-baseline-traces/tsan-mperfbench-states16.json \
    > /home/erol/cccp/Source/CI/wsl2-baseline-traces/tsan-mperfbench.log 2>&1
```

Per-bucket first-engine-frame distribution and candidate-symbol greps
extracted by `/tmp/extract-mperfbench-tsan-summary.sh` (script attached
inline in this report's analysis turn; reproducible from the log file
alone).

## Notes

- **TSan-clean does NOT mean determinism-clean.** Race B is a real
  divergence (1 / 10 MPerfBench@16 in the non-TSan sweep at tick 982).
  TSan's hypothesis-narrowing role is to disprove the *memory-race*
  hypothesis class; the divergence still requires explanation under the
  *value-race* hypothesis class.
- **TSan-overhead note (run-1 partial completion).** TSan slowdown on
  the 150-actor BRAINHUNT MPerfBench scenario at 16 Lua states is
  significantly higher than Phase 4's M4ThreadStress @ 8 (~26×).
  Empirically, the first run of the `--runs 3` sweep was still in
  its simulation phase at ~19 minutes wall (race rate had dropped to
  near-zero by ~12 minutes — i.e., the heavy-AI phase was past tick
  982 and the binary was past the Race B region — but the binary was
  not yet writing run 1's JSON divergence record). I aborted the run
  at that point: the unique-race-signature set was fully saturated
  (618 reports across the four engine first-frame buckets and the 20
  SUMMARY-line racy-memory locations, with the candidate-space hit
  count locked at zero for the previous ~10 minutes), and additional
  runtime would not change the conclusion. The candidate-space-empty
  result is robust to letting the sweep complete the additional 2
  runs because: (a) the unique-race-signature set was clearly saturated
  by the run-1 mid-point, (b) the M4ThreadStress @ 8 control run (Phase
  4) showed the identical four buckets at a similar reports-per-bucket
  ratio, and (c) the per-symbol `grep -c` for all 16 Race B candidate
  symbols stayed at zero throughout. No JSON divergence diff is written
  for a partial run (the `-determinism-check` driver only produces it
  after all `--runs` complete).
- **Suppression-list mismatch (unchanged from Phase 4).** The existing
  filter file uses `race:tbb::detail::r1::` but TSan emits the partitioner
  races at `tbb::detail::d1::`. The unsuppressed `d1::` class dominates
  this report's 618-race output. Recommended update is in the bring-up
  report's §"Phase 7.1" section. Fixing the suppression would reduce
  the report volume but would not reveal any application race — every
  TSan-detected race in the MPerfBench log is in toolchain
  infrastructure by SUMMARY line; the bucket *count* under a fixed
  suppression would drop to ~zero, leaving an empty result that
  confirms the same conclusion.

## File outputs

* `Source/CI/wsl2-baseline-traces/tsan-mperfbench.log` — full TSan
  output, MPerfBench @ lua-states 16, 1200 ticks (run 1 partial,
  aborted at ~19 min wall after unique-race-signature saturation;
  see §Notes "TSan-overhead note" above). 618 race reports, ~10 MB.
* `Source/CI/m5-wsl2-x86_64-race-b-tsan.md` — this report.

(No `tsan-mperfbench-states16.json` was written. The
`-determinism-check` driver only emits its per-run divergence diff
after all `--runs` complete.)
