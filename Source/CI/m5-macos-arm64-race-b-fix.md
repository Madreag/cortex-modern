# macOS-arm64 Race B investigation — round 4 audit (HALT before fix)

Per follow-up directive: pin Race B's actual root cause inside the
HumanBehaviors target-acquisition chain on macOS-arm64, then design
+ implement the Path E fix. Result: **HALT** — Phase 1 ablations
rule out all three anticipated candidates with identical
behaviour-degradation signatures, so the directive's "stop and
report" gate fires. No fix lands on this branch.

Branch tip: `252fdb8b7` (`exp/determinism-macos` after cherry-pick of
Linux `e56261ed0` "PathFinder: drain prev-tick deferred + sort node
list + serial UpdateNodeList"). Tree clean against HEAD; FixedPoint
SELF-CHECK `5ce9c33b84d29932` unchanged.

## Round-3 baseline still holds after PathFinder cherry-pick

Re-ran the byte-level controller dump after cherry-picking
`e56261ed0`. The MPerfBench tick-44 fingerprint is **byte-identical**
to round 3 — Linux's PathFinder narrow fix does not shift the macOS
Race B signature:

```
DIFF at sim tick 44 (harness array index 43):
< uid=18218 bits=000000000100000000000000000000000000000000000000000000000  ← run 0 (reference)
> uid=18218 bits=000010000100000000000000000000000000000000000000000000000  ← run 3 (diverging)
                  ^^^^                                                          ^^^^^^^^^^^
                  bit 4 = MOVE_LEFT set in run 3 but not run 0
                  bit 9 = BODY_JUMPSTART set in both runs (PREJUMP)
```

Both runs identical: `px=0x1.d1p+9 py=0x1.351d7p+8 vx=0x0p+0
vy=0x1.caacfap+3 h=0x1.9p+6 am=4 (BRAINHUNT)`. The MOVE_LEFT bit is
the ONLY divergent bit at the first divergent tick — pos / vel /
health / AIMode all bit-stable.

Race rate at `MPerfBench -threads 16 -runs 5`: 4 / 5 sweeps DIVERGED
(matches round-1 baseline of 1 / 5 sweeps MATCHED). Cherry-picking
`e56261ed0` did not measurably change the macOS rate.

## Phase 1 — ablation matrix (HumanBehaviors target-acquisition chain)

Per directive, ablated the three primary candidates that drive
`Controller.MOVE_LEFT` via `self.lateralMoveState`. Each ablation
patched the source tree with a `BISECT_INSTRUMENTATION_DO_NOT_COMMIT`
marker, was tested via `MPerfBench -threads 16 -runs 5` × 5 sweeps,
then reverted before the next ablation. None of the ablation patches
are present on the committed branch.

| # | Ablation site | Mechanism | Sweep result | Verdict |
|---|---|---|---|---|
| A | `MovableMan::GetMOsInBox` Lua binding (`Source/Managers/MovableMan.cpp:247`) under `g_CurrentAIActor != nullptr` | Return an empty `std::vector<MovableObject*>*` instead of querying the SpatialPartitionGrid | 3 / 5 MATCHED — signature unchanged when diverged (`controller:43, actors:44, particles:44, scene:50, sim_rng:49, terrain:54`) | **NOT the violator** |
| B | `HumanBehaviors.CheckEnemyLOS` (`Data/Base.rte/AI/HumanBehaviors.lua:23-101`) entirely | `if true then return nil end` at function entry | 3 / 5 MATCHED — signature unchanged | **NOT the violator** |
| C | `HumanBehaviors.GoProneToTarget` lateral-move chooser (`Data/Base.rte/AI/HumanBehaviors.lua:903-907`) | Comment out the `if Dist.X > 0 then AI.lateralMoveState = ... else AI.lateralMoveState = Actor.LAT_LEFT end` block | 3 / 5 MATCHED — signature unchanged | **NOT the violator** |

### Interpretation

All three ablations produce **identical 3 / 5 MATCHED** at -threads 16
-runs 5 vs baseline 1 / 5 MATCHED. The signature when diverged is
byte-identical across ablations and to the round-3 baseline. This is
the same partial-reduction pattern as round-3 E1 (skip `GetRootMOID`
under parallel AI) — explained by **behaviour degradation**, not race
closure. Removing any one major target-acquisition surface makes the
AI's chaotic actor-vs-actor brawl less divergent on average (fewer
LOS-and-shoot interactions → fewer divergent paths to follow), but
doesn't eliminate the underlying race.

The fact that **all three** distinct ablations give the same rate
reduction with the same signature is the strongest possible evidence
that none of them is the actual violator. If one were the root cause,
its ablation would have produced a markedly different outcome
(complete close, or signature shift, or a different rate envelope)
from the others.

## Why the violator is NOT in the anticipated chain

Mechanical analysis from round 3 + this round:

* At sim tick 44, all physics fields (pos, vel, health, AIMode, analog
  inputs) are bit-identical for actor 18218 between runs. Only bit 4
  (MOVE_LEFT) differs.
* `Controller::ResetCommandState` clears all bits when an AI tick fires
  (`Controller.cpp:170`). So bit 4 = 1 at end of tick 44 means the
  AI script for actor 18218 wrote `MOVE_LEFT = true` during tick 44's
  AI run.
* The only setter of `Controller.MOVE_LEFT` in any human AI script is
  `NativeHumanAI.lua:671`, gated by `self.lateralMoveState ==
  Actor.LAT_LEFT`.
* The setters of `self.lateralMoveState = Actor.LAT_LEFT`:
  - `NativeHumanAI.lua:528` — needs `Owner.Vel.X > 2`. At tick 44
    `Vel.X = 0x0p+0 = 0` in both runs, so this branch evaluates to
    `LAT_STILL`. RULED OUT by physics matching.
  - `HumanBehaviors.lua:907` (`GoProneToTarget`) — needs `Dist.X < 0`
    where `Dist` depends on `AI.Target`. ABLATED in C above and
    signature persisted. RULED OUT.
* The only other path is **`lateralMoveState` was set to `LAT_LEFT` at
  an earlier AI tick and persisted into tick 44**. For that to differ
  across runs, an earlier AI tick must have made a different choice
  while controller bits matched (because per the dump all bits match
  through tick 43).

That last condition means there must be an AI input that differs
across runs at an AI tick BEFORE 44, but whose effect doesn't reach
the controller until tick 44. The most plausible storage for that
hidden state is `self.lateralMoveState` itself — but to set it
differently across runs at the earlier tick, the AI had to read a
**non-physics input** that differed. Candidates ruled out so far:

- RNG state (per-MO seeded via `DeterministicMORNGScope` —
  deterministic per `(uid, tick, hash(funcName))` per round-3 audit).
- Cross-actor controller reads (`Controller::IsState` uses
  `m_FrozenControlStates` snapshot for foreign reads —
  `Source/System/Controller.h:178-184`).
- `FirearmIsReady` / `EquippedItem` (frozen via `FreezeStateForAIPhase`
  — `AHuman.cpp:1291-1295`).
- `m_RootMOID` reads in `CastMORay::GetRootMOID` (round-3 disproven:
  0 SetParent calls under parallel AI vs 1.04M GetRootMOID reads).
- PathFinder updates (`e56261ed0` now drains prev-tick + serializes;
  cherry-pick verified not to shift the signature).
- All three primary HumanBehaviors candidates A / B / C above.

This list of ruled-out sources is exhaustive against the violator
candidates the directive enumerated. **The remaining surface that has
NOT been audited on this branch is the script-callback queue +
per-tick callback id allocation in `LuaMan` / `LuaAdaptersScene`**.
See "Recommended next investigation" below.

## What the macOS branch ships vs what Linux already has

Linux `exp/determinism-linux` carries two source-changing fixes
post-foundation that the macOS branch does NOT have:

| Linux commit | One-liner | Why it's relevant to Race B on macOS |
|---|---|---|
| `9bf7329bf` "Close two parallel-AI races feeding M4ThreadStress" | (a) `AudioMan::PlaySoundContainer` early-return under `IsRecordingTickHashes()` — kills g_RenderRNG race + AudioMan unordered_map race triggered by `AI:Equip*` → device-switch sound. (b) `LuaAdaptersScene::CalculatePathAsync` `currentCallbackId` becomes `std::atomic<int>` with `fetch_add(memory_order_relaxed)` — fixes static-int increment race where two parallel ThreadedUpdateAI workers can hand the same id to two simultaneous path requests, losing one callback's Lua slot. | The atomic-callback-id fix directly addresses a race where two parallel AI actors making `SceneMan.Scene:CalculatePathAsync` requests in the same tick can land in the same `_AsyncPathCallbacks` slot, silently corrupting one actor's path result. If actor 18218 (or one of its targets) is one of those actors, downstream behaviour decisions (including `lateralMoveState` assignments in prior AI ticks) would diverge across runs while physics + controller bits remain identical right up to the AI tick that consumes the corrupted path. **Fingerprint match: yes** — racy-callback-id loses path data → AI takes a different branch → `lateralMoveState` set differently → MOVE_LEFT bit flips while physics matches. The sound suppression is a separate hardening; it also rides through. |
| `69532738b` "LuaMan: sort script-callback queue by caller key to drain async-race order" | Adds a deterministic sort key to `AddLuaScriptCallback`, `stable_sort`s the queue before draining, derives the key in `CalculatePathAsync` from `startPos + targetPos` (splitmix64 mix). Pins the order in which path callbacks land in their Lua tables when multiple async path requests complete on different worker threads in the same tick. | Sibling of the atomic-callback-id fix. Even with a unique callback id, if two callbacks for actor X resolve in different orders across runs, the `_AsyncPathCallbacks` table sees different `table.insert` orders, and `table.sort` is unstable on equal keys (e.g. `WeaponSearch / ToolSearch` score ties). The macOS HumanBehaviors.lua tie-break fix (`2e28cd5bb` `deviceId` ascending) addresses the score-tie half of this; the callback-order half is what `69532738b` covers. Both fixes are complementary. **Fingerprint match: yes** for the same chain as above. |

Both Linux fixes target the parallel ThreadedUpdateAI path-callback
machinery — exactly the surface that produces hidden non-physics
state divergence across runs while leaving physics matched until the
next AI tick reads it. Neither fix is on this macOS branch.

## Recommended next investigation (not implemented on this branch)

**Stop-and-report verdict per directive.** The violator is not in the
ablated HumanBehaviors candidates and the rate / signature pattern is
inconsistent with "expand to other parallel-AI cross-actor reads"
(option D in the directive) being a productive direction without
first ruling out the two Linux fixes above.

Recommended order of operations for round-5 (subject to user
direction):

1. **Cherry-pick `9bf7329bf` onto `exp/determinism-macos`** (sound
   suppression + atomic callback id). Rebuild. Re-run the MPerfBench
   tick-44 byte-level dump. Three outcomes:
   - **Race B closes** (signature disappears): atomic callback id was
     the root cause. No further macOS-side work needed; promote both
     Linux fixes into the canonical
     `flagship/m4a-path-e-enforcement` consolidation.
   - **Signature shifts** to a different tick / actor / bit: atomic
     callback id was a contributing cause but a deeper race remains.
     Re-bisect from the new pin.
   - **Signature unchanged**: atomic callback id is benign on macOS;
     proceed to step 2.

2. **Cherry-pick `69532738b` on top** (sort script-callback queue).
   Same three-outcome triage.

3. **Only if both Linux fixes fail to shift the macOS signature**,
   open option D from the directive — instrument the per-AI-tick
   `self.lateralMoveState` write log for actor 18218 across ticks
   1-44, identify the precise tick + assignment site where
   `lateralMoveState` first becomes `LAT_LEFT` in run 3 but
   `LAT_STILL` (or never-assigned) in run 0. That gives the upstream
   non-physics input that varies, which can then be ablated directly.

## Why this isn't a fix landing

Per directive: "If you find the violator is something we haven't
anticipated (e.g., not in the HumanBehaviors chain at all, or
interacts weirdly with Linux's PathFinder fix), stop and report —
don't expand scope to implement a fix for something we haven't
agreed on."

The Phase 1 result satisfies the "not in the HumanBehaviors chain"
clause (all three primary candidates ruled out with the same
behaviour-degradation fingerprint). No fix lands on this branch.
`exp/determinism-macos` carries only the Linux `e56261ed0`
cherry-pick on top of the prior tip; no ablation patches, no
instrumentation, no fix attempts.

## Verification of branch state at HALT

```
$ git log --oneline -5 exp/determinism-macos
252fdb8b7 PathFinder: drain prev-tick deferred + sort node list + serial UpdateNodeList   ← Linux e56261ed0 cherry-pick
0a49cc79f Source/CI: secondary candidate disproven — CastMORay GetRootMOID is race-free on macOS   ← prior round-3 audit
5a1dd3b58 Source/CI: audit-pass bisect of MPerfBench tick-43 residual on macOS-arm64   ← round-2 audit
eb4cfbfd7 Source/CI: macOS-arm64 verification of Path E cherry-pick   ← round-1 verification
cfeec206b Source/CI: document M4 path-E equip-defer scaffold in mod_compat_assessment   ← Linux 4b9546b01 cherry-pick

$ git diff --stat HEAD
(no output — tree clean against HEAD)

$ git diff --cached --stat
(no output — nothing staged)

$ ./builddir/FixedPointTests | tail -3
SELF-CHECK: 5ce9c33b84d29932
42 passed, 0 failed
```

This file (`Source/CI/m5-macos-arm64-race-b-fix.md`) is the only new
file created this round. Local commit only — not pushed to origin
per directive.
