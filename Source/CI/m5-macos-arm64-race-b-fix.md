# macOS-arm64 Race B fix — round-5 verification (CALLBACK-ID HYPOTHESIS CONFIRMED)

Cherry-picked three Linux fixes onto `exp/determinism-macos` per
round-5 directive and verified Race B is closed across the full
test matrix. **Callback-id hypothesis CONFIRMED**: the macOS Race B
tick-44 / actor uid=18218 / bit-4 MOVE_LEFT fingerprint is CLOSED
(not just masked), and the full eleven-scenario M-series sanity
matrix is 175 / 175 runs clean.

Branch tip: `6c0f63e87` plus this report (`exp/determinism-macos`).
FixedPoint SELF-CHECK `5ce9c33b84d29932` unchanged.

## Cherry-picks (in order)

| Order | SHA on branch | Linux SHA | One-liner | Conflicts |
|---|---|---|---|---|
| 1 | `844866e74` | `9bf7329bf` | Close two parallel-AI races feeding M4ThreadStress (atomic callback id + sound gate + TSan LuaJIT unblock) | clean auto-merge in `Source/Lua/LuaAdapters.cpp` |
| 2 | `6796f50e2` | `69532738b` | LuaMan: sort script-callback queue by caller key to drain async-race order | clean auto-merge in `Source/Lua/LuaAdapters.cpp` |
| 3 | `6c0f63e87` | `724ed3a94` | PathFinder Path E: move UpdatePathFinding to post-MovableMan epilogue | clean auto-merge in `Source/Main.cpp` |

All three picked cleanly; no manual conflict resolution. Build green
on Apple Clang 17 / Ninja; FixedPointTests SELF-CHECK
`5ce9c33b84d29932` (matches every prior round).

## Verification matrix

All reports under `Source/CI/macos-arm64-traces/round5-verify/`.
Every `diverged` flag is `false`; every `compared_ticks` matches the
requested `--ticks` value (no early termination).

### Race B surface — `MPerfBench` at all three Lua-state counts (the
fingerprint that was firing 80 % / sweep before round-5)

| Scenario / threads | Sweeps × runs | Compared ticks | Result |
|---|---|---|---|
| `MPerfBench -threads 8 -runs 5` | 1 × 5 | 1200 | MATCHED 5 / 5 |
| `MPerfBench -threads 12 -runs 5` | 1 × 5 | 1200 | MATCHED 5 / 5 |
| `MPerfBench -threads 16 -runs 5` | 1 × 5 | 1200 | MATCHED 5 / 5 |
| `MPerfBench -threads 16 -runs 10` confirm 1-5 | 5 × 10 | 1200 each | MATCHED 50 / 50 |

**Total at the Race B surface: 65 / 65 runs across 8 sweeps clean.**
Pre-round-5 baseline at `-threads 16` was 1 / 5 sweeps MATCHED (i.e.
80 % diverge per sweep) — the cherry-pick stack flips that to 0 %
diverge across 8 independent sweeps.

### Race A surface — `M4ThreadStress` at both Lua-state counts

| Scenario / threads | Runs | Compared ticks | Result |
|---|---|---|---|
| `M4ThreadStress -threads 8 -runs 10` | 10 | 900 | MATCHED 10 / 10 |
| `M4ThreadStress -threads 16 -runs 10` | 10 | 900 | MATCHED 10 / 10 |

**Total: 20 / 20 runs clean.** Race A's residual is closed by Linux's
`724ed3a94` architectural Path E (post-MovableMan epilogue);
`e56261ed0`'s narrow par_unseq → seq + sort is superseded by the
epilogue's serial-by-construction property but kept in tree.

### M1 / M2 / M3 sanity sweeps

| Scenario | Compared ticks | Runs | Result |
|---|---|---|---|
| `M1Baseline` | 600 | 10 | MATCHED |
| `M1TerrainStress` | 900 | 10 | MATCHED |
| `M1ActorStress` | 900 | 10 | MATCHED |
| `M2LuaBaseline` | 600 | 10 | MATCHED |
| `M2LuaRandomStress` | 600 | 10 | MATCHED |
| `M2PairsStress` | 600 | 10 | MATCHED |
| `M2OsStubTest` | 600 | 10 | MATCHED |
| `M2ModSmokeLoading` | 600 | 10 | MATCHED |
| `M3TerrainStress` | 900 | 10 | MATCHED |

**Total: 90 / 90 runs clean.** No regression from the cherry-picks.

### Grand total

**175 / 175 runs MATCHED** across all scenarios, thread counts, and
sweep configurations. Zero divergences observed.

### `FixedPointTests` SELF-CHECK

```
SELF-CHECK: 5ce9c33b84d29932
42 passed, 0 failed
```

Unchanged from every prior round — Q40.24 codepaths bit-stable
across all five rounds of macOS-arm64 work.

## Byte-level pin re-check (per directive: "not just masked")

Temporarily re-added the per-actor controller / pos / vel / health
dump that produced the round-3 / round-4 byte-level pin, ran
`MPerfBench -seed 42 -ticks 60 -runs 10 -threads 16` (the smallest
config that fully exercises the tick-44 race surface), captured
per-actor dumps at every tick in the 40-48 window, and md5'd them
across the 10 parallel runs.

```
Sim tick 44 dumps (10 runs at -threads 16):
all 10 files md5 → a0964d8952376633e95fecf744af69db  (1 unique hash)

Actor uid=18218 entry at sim tick 44 (identical across all 10 runs):
uid=18218 bits=000000000100000000000000000000000000000000000000000000000
          mv=0x0p+0,0x0p+0 aim=0x0p+0,0x0p+0 cur=0x0p+0,0x0p+0
          mode=2 px=0x1.d1p+9 py=0x1.351d7p+8 vx=0x0p+0 vy=0x1.caacfap+3
          h=0x1.9p+6 am=4
                                  ^ bit 9 = BODY_JUMPSTART (set, as before)
                       ^ bit 4 = MOVE_LEFT (CLEAR — matches reference run 0 from round 3)
```

This is the **reference** bit pattern (run 0 from rounds 3 / 4). The
divergent pattern (`bits=000010000100000000000000000000000000000000000000000000000`
with bit 4 = MOVE_LEFT SET) does NOT appear in any of the 10 runs.
The fingerprint is closed, not masked — every run takes the same
AI-decision branch, every Lua per-MO `_AsyncPathCallbacks` slot
resolves to the same path callback, every `lateralMoveState`
assignment is byte-stable at sim tick 44 and every tick that
precedes it.

Instrumentation reverted before the verification commit; tree clean
against HEAD; final rebuild + FixedPoint sanity verified after
revert.

## Why each cherry-pick contributes

* **`9bf7329bf` (atomic callback id)** — the headline fix. The
  static `int currentCallbackId` in `LuaAdaptersScene::CalculatePathAsync`
  was being incremented from multiple parallel `ThreadedUpdateAI`
  worker threads. Two simultaneous AI actors calling
  `SceneMan.Scene:CalculatePathAsync` could be handed the same id;
  the second's callback then overwrote the first's slot in the Lua
  `_AsyncPathCallbacks` table. When the path completion callbacks
  fired, one actor consumed the wrong actor's path data, took a
  different `lateralMoveState` branch in `GoProneToTarget` or
  `LookForTargets`, and at some later AI tick wrote
  `Controller.MOVE_LEFT` divergently — the exact MPerfBench Race B
  fingerprint. The `std::atomic<int>` with
  `fetch_add(memory_order_relaxed)` makes the id allocation race-free
  and the per-actor callback slots stay isolated. The companion
  `AudioMan::PlaySoundContainer` early-return is a separate
  hardening for the `g_RenderRNG` + AudioMan unordered_map race
  triggered by AI equip-sound playback; both rode through cleanly.

* **`69532738b` (LuaMan callback queue sort)** — sibling fix that
  closes the second half of the same machinery. Even with a unique
  callback id, two `CalculatePathAsync` completions arriving on
  different worker threads in the same tick landed in
  `m_ScriptCallbacks` in non-deterministic completion order; the
  vector was then drained in that order on the main thread, hitting
  Lua `table.insert` in different orders across runs.
  `stable_sort` on a splitmix64-derived key from `startPos +
  targetPos` makes the drain order deterministic per-tick.

* **`724ed3a94` (PathFinder Path E)** — architectural close for
  Race A. Moves `Scene::UpdatePathFinding` out of the
  inside-`MovableMan::Update` call site and into a serial epilogue
  that runs after `MovableMan::Update` finishes all parallel
  phases. AI path Solves in the same tick now see the previous
  tick's grid (deterministic snapshot, same input every run);
  UpdatePathFinding writes the next-tick grid serially. Eliminates
  the read-while-write race surface by phase ordering, not by
  locking or snapshotting. Cherry-picked into Race B verification so
  any Race A residual doesn't mask Race B's tick-44 fingerprint —
  cleanly achieved (M4ThreadStress 20 / 20).

## Final verdict

**Callback-id hypothesis CONFIRMED.** Race B on macOS-arm64 closes
fully under the three Linux cherry-picks. No fix authored on this
branch — only the three Linux cherry-picks plus this verification
report.

| Question | Answer |
|---|---|
| Was Race B the callback-id violator? | **YES** — `9bf7329bf` + `69532738b` together close the MPerfBench tick-44 fingerprint |
| Is Race A also closed by the same cherry-pick stack? | **YES** — `724ed3a94`'s architectural Path E gives M4ThreadStress 20 / 20 at both `-threads 8` and `16` |
| Any regression on the M1 / M2 / M3 sanity matrix? | **NO** — 90 / 90 runs clean across nine scenarios |
| Was the fingerprint truly closed or just masked? | **CLOSED** — byte-level re-check across 10 parallel runs at the same surface produces a single md5 hash for tick-44; actor 18218 bit 4 is uniformly clear, matching the reference behaviour pre-Race-B exposure |

## Recommended next step

None for macOS — this branch is verification-complete for the
`m5-macos-arm64` series. The three Linux cherry-picks should land
in the canonical `flagship/m4a-path-e-enforcement` consolidation
exactly as-is. Round-4's tertiary candidate
(`DeterministicMORNGScope` seed-mix) is therefore moot for macOS
and should not be pursued unless WSL2 or other platforms surface
a different residual.

## Branch state at the verification commit

```
$ git log --oneline -8 exp/determinism-macos
<this commit>  Source/CI: round-5 verification — Race B closed by three Linux cherry-picks
6c0f63e87      PathFinder Path E: move UpdatePathFinding to post-MovableMan epilogue          ← Linux 724ed3a94
6796f50e2      LuaMan: sort script-callback queue by caller key to drain async-race order     ← Linux 69532738b
844866e74      Close two parallel-AI races feeding the M4ThreadStress divergence              ← Linux 9bf7329bf
419916717      Source/CI: Race B round-4 audit on macOS — HALT before fix                    ← round-4 audit (HALT)
252fdb8b7      PathFinder: drain prev-tick deferred + sort node list + serial UpdateNodeList  ← Linux e56261ed0
0a49cc79f      Source/CI: secondary candidate disproven — CastMORay GetRootMOID is race-free on macOS
5a1dd3b58      Source/CI: audit-pass bisect of MPerfBench tick-43 residual on macOS-arm64
```
