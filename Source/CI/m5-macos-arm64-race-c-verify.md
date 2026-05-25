# macOS-arm64 Race C cross-verify — WSL2 cc1c4cd81 cherry-pick (NO REGRESSION)

WSL2 round-5 Race C fix cherry-picked onto `exp/determinism-macos`
and verified end-to-end. **No regression on Race B closure** — the
byte-level fingerprint re-check produces the identical md5
(`a0964d8952376633e95fecf744af69db`) as the round-5 baseline. Full
matrix 175 / 175 runs clean. FixedPoint SELF-CHECK
`5ce9c33b84d29932` unchanged.

Branch tip: `ec2775d0e` plus this report (`exp/determinism-macos`).

## Cherry-pick

| On branch | Source SHA | Source branch | Summary |
|---|---|---|---|
| `ec2775d0e` | `cc1c4cd81` | `origin/exp/determinism-wsl2` | MovableMan: rebuild `m_ContiguousActorIDs` synchronously at `UpdateControllers` entry. Moves the rebuild out of the late-tick `UpdateDrawMOIDs` (which observed `m_Actors` after in-tick Lua-callback / Travel / death modifications and could land in different orders across runs even when SimChecksum hashes match through the prior tick) into a serial pre-AI step so `ShouldUpdateAIThisFrame`'s `GetContiguousActorID` gate is computed off a stable, just-built mapping. |

Cherry-pick required a one-file conflict resolution: WSL2's
`cc1c4cd81` modified `Source/CI/m5-wsl2-x86_64.md` which is
WSL2-only and deleted in macOS HEAD. Dropped the WSL2 report file
plus the WSL2 round-5 trace folder (`Source/CI/wsl2-r5-traces/`) so
the macOS branch stays platform-cleansed. Only the
`Source/Managers/MovableMan.cpp` source change rode through (10
lines, +7 / -3). Build green; FixedPointTests SELF-CHECK
`5ce9c33b84d29932`.

## Verification matrix

All reports under `Source/CI/macos-arm64-traces/round6-verify/`.
Every `diverged` flag is `false`; every `compared_ticks` matches the
requested `--ticks` value.

### Race B surface — `MPerfBench` at all three Lua-state counts

| Scenario / threads | Sweeps × runs | Compared ticks | Result |
|---|---|---|---|
| `MPerfBench -threads 8 -runs 5` | 1 × 5 | 1200 | MATCHED 5 / 5 |
| `MPerfBench -threads 12 -runs 5` | 1 × 5 | 1200 | MATCHED 5 / 5 |
| `MPerfBench -threads 16 -runs 5` | 1 × 5 | 1200 | MATCHED 5 / 5 |
| `MPerfBench -threads 16 -runs 10` confirm 1-5 | 5 × 10 | 1200 each | MATCHED 50 / 50 |

**Total at the Race B surface: 65 / 65 clean across 8 sweeps**
(matches round-5 exactly).

### Race A surface — `M4ThreadStress` at both Lua-state counts

| Scenario / threads | Runs | Compared ticks | Result |
|---|---|---|---|
| `M4ThreadStress -threads 8 -runs 10` | 10 | 900 | MATCHED 10 / 10 |
| `M4ThreadStress -threads 16 -runs 10` | 10 | 900 | MATCHED 10 / 10 |

**Total: 20 / 20 clean** (matches round-5 exactly).

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

**Total: 90 / 90 clean** (matches round-5 exactly).

### Grand total

**175 / 175 runs MATCHED across the round-6 matrix**, identical run
count and outcome to round-5. Zero divergences observed.

### `FixedPointTests` SELF-CHECK

```
SELF-CHECK: 5ce9c33b84d29932
42 passed, 0 failed
```

Unchanged from round-5 (and every prior round). The Race C cherry-
pick does not touch Q40.24 codepaths.

## Byte-level pin re-check (per directive: confirm no shift)

Re-added the per-actor controller dump (reverted before commit), ran
`MPerfBench -seed 42 -ticks 60 -runs 10 -threads 16`, md5'd the 10
per-pid sim-tick-44 dumps:

```
Sim tick 44 dumps (10 runs at -threads 16):
all 10 files md5 → a0964d8952376633e95fecf744af69db  (1 unique hash)

Round-5 baseline md5  → a0964d8952376633e95fecf744af69db  (identical)

Actor uid=18218 entry at sim tick 44 (identical across all 10 runs):
uid=18218 bits=000000000100000000000000000000000000000000000000000000000
          mv=0x0p+0,0x0p+0 aim=0x0p+0,0x0p+0 cur=0x0p+0,0x0p+0
          mode=2 px=0x1.d1p+9 py=0x1.351d7p+8 vx=0x0p+0 vy=0x1.caacfap+3
          h=0x1.9p+6 am=4
                                  ^ bit 9 = BODY_JUMPSTART (set, as before)
                       ^ bit 4 = MOVE_LEFT (CLEAR — matches reference)
```

**md5 is byte-identical to round-5 baseline.** The Race C fix does
NOT change which AI tick fires for actor 18218 (it could have shifted
the `ShouldUpdateAIThisFrame` gate result, since that's exactly what
Race C affects — but on this seed / scenario the post-fix
contiguous-ID mapping happens to produce the same gate-true tick
schedule for actor 18218 at tick 44). The byte-level fingerprint
closure from round-5 is preserved exactly.

## Verdict

No regression. The Race C cherry-pick is safe to merge into the
canonical `flagship/m4a-path-e-enforcement` consolidation as
delivered — verified across:

- All MPerfBench thread-count configs (8, 12, 16) and sweep depths
  (5-run + 10-run × 5 confirm)
- Both M4ThreadStress thread counts (8, 16)
- All eleven M-series scenarios at `-runs 10`
- Byte-level controller fingerprint at the tick-44 / actor-18218
  surface where round-5 confirmed Race B closed

## Branch state at the verification commit

```
$ git log --oneline -8 exp/determinism-macos
<this commit>  Source/CI: round-6 verification — WSL2 Race C cherry-pick clean, no Race B regression
ec2775d0e      MovableMan: rebuild m_ContiguousActorIDs synchronously at UpdateControllers entry   ← WSL2 cc1c4cd81
cf254cad1      Source/CI: round-5 verification — Race B closed by three Linux cherry-picks
6c0f63e87      PathFinder Path E: move UpdatePathFinding to post-MovableMan epilogue              ← Linux 724ed3a94
6796f50e2      LuaMan: sort script-callback queue by caller key to drain async-race order         ← Linux 69532738b
844866e74      Close two parallel-AI races feeding the M4ThreadStress divergence                  ← Linux 9bf7329bf
419916717      Source/CI: Race B round-4 audit on macOS — HALT before fix
252fdb8b7      PathFinder: drain prev-tick deferred + sort node list + serial UpdateNodeList      ← Linux e56261ed0
```
