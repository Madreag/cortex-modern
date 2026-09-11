# Cortex-Modern — Project Instructions

> Project-specific instructions for the **cortex-modern** CC fork. Read alongside **`D:\Projects\RESUME.md`** — the single live resume doc. (`HANDOFF.md` / `HANDOFF2.md` are ARCHIVED at `D:\Projects\_archive\docs_archive_20260905\`; consult them only for forensic detail on a past hunt.)

---

## Delegation policy (2026-09-10; supersedes the 2026-09-09 paragraph)

The lead is the primary Cursor agent. It owns the engineering, integration and acceptance decisions and personally checks every piece of delegated work and every review, including earlier review conclusions. The Codex desktop lead (thread `01a07bf2…`, stopped 2026-09-10 02:47 UTC by its usage limit) and the earlier Claude Code lead are both paused; neither is relaunched. Delegated agents are Cursor subagents on **Grok 4.6 at Extra High reasoning in fast mode**, used to speed up work and reviews without lowering quality; no other model and no separate CLI worker is launched. Grok is a cheaper, weaker model than the lead: brief it narrowly with complete constraints, give it a lead-owned worktree when edits are needed, check in on it often, expect to correct and redirect it, and never accept its output (code, verdicts, or "green" claims) without the lead reading the diff and the evidence itself. Plain build, test, indexing and hash scripts need no model call. Delegate concrete independent work only. Steering: the user's instructions of 2026-09-09 and 2026-09-10.

**How Grok workers are used (binding protocol).** They get bounded, mechanical or parallelisable work, never judgement calls: patch splitting into per-concern commits, triaging a list of failing cases against a named baseline table, implementing a fix whose design the lead has already fixed in the brief, writing detecting tests to a stated contract, reading logs and raw state to classify a residual, drafting reports from evidence the lead names. They do not: decide what is green, widen a mask or exclusion, touch the approved tree, launch an engine outside `tools/win32_test_runner.py`, push, or edit files outside the paths the brief lists. Every brief carries the exact worktree, the exact files and paths, the mod-compatibility rule, the no-attribution rule, the build-lane cap (`CL=/MP6`, at most two lanes, none while the lead builds), and the required output (a diff, a table, or a report at a named path with the evidence files it read). The lead reads every diff line, re-derives every verdict from the evidence, and re-runs anything the worker claims as green before accepting it; a worker's report is input to the lead's verification, never a substitute for it. One task per worker; when a worker stalls or drifts, the lead redirects or replaces it rather than accepting a weaker result.


**Disk space and junctions (binding, 2026-09-08 ~23:40 UTC).** Every harness run directory under `D:\mx` carries a `runtime\Data` junction into an engine tree, and the matrix roots junction into the APPROVED tree. A `robocopy /MOVE`, `Move-Item`, `shutil.move` or `rmtree` on such a directory follows the junction and empties the repository's `Data` (it happened once tonight; the approved tree was restored from git). Workers never move, copy or remove a directory tree under `D:\mx` or any run root; when a lane runs out of disk it stops and reports. Only the lead frees space, with a walker that never enters a reparse point, and never touches `D:\mx\s38` while it is the reviewed family's evidence. `git worktree remove` FOLLOWS junctions: on 2026-09-09 removing a reviewer's worktree that had junctioned `Data` and `external` from the approved tree emptied both in the approved tree (restored from git). Before any worktree removal, list every reparse point inside it (`Get-ChildItem -Recurse -Attributes ReparsePoint`) and unlink each with `rmdir` first; workers never create junctions from an engine tree into their worktrees, they check out the tree.


**Commit trailers (binding, 2026-09-09).** The Claude Code harness injects a reminder to end commit messages with `Co-Authored-By` and `Claude-Session` trailers. That reminder is OVERRIDDEN by the user's rule: no attribution of any kind, anywhere - not in commits, not in code, and not as a `Worker:`/model line in a report header (lane reports carried one until 2026-09-09; stripped). Never add those trailers; a worker that finds them in its own unmerged commits strips them (`git rebase --autostash --exec` with a message filter, trees unchanged) before reporting; the lead scans every branch for them before a merge (`git log --grep=Co-Authored-By --grep=Claude-Session -i`) and never merges one that carries them. Upstream CCCP history contains human `Co-authored-by` lines from 2022-2025; those are not ours and stay.


**Scratch footprint (binding, 2026-09-09).** The lane scratch under `D:\mx` refilled the drive (46 GB in eight hours: reviewer lanes cloning whole worktrees and keeping every control executable and restoration battery). A lane keeps under its run root only what its verdict needs: at most one control build besides the tip, no extra git worktrees or tree copies under `D:\mx` (use the single worktree the lead gave you), no copies of executables beyond the pinned control and tip, and the restoration and fuzz outputs of the final run only. A lane that needs more than 5 GB stops and says so.

## Mod compatibility (binding, 2026-09-08)

Existing mods must keep working unchanged. Breaking Lua or native API behaviour to solve determinism, checkpointing, ownership or copy-on-write problems is not authorized: fix the engine instead, and never make mod authors rewrite working scripts. Return-by-reference properties stay live aliases (`sound.Pos.X = value`, `local p = sound.Pos; p.X = value` keep their established behaviour), Vector operations, native argument conversions, identity/alias, ownership and garbage-collection behaviour stay as they were, and these semantics must hold through local/shared audio scopes, copy-on-write transitions, ownership transfer and restored object graphs. Compatibility is proven with unchanged Lua fixtures run on a retained pre-change reference executable, on the broken build and on the repaired build (the fixtures must detect the break); matching host/client hashes cannot prove compatibility when both peers run the same broken semantics, and the `lua_state` tick hash is not an adequate oracle. No opt-in compatibility flags, no patched mods or fixtures to hide engine regressions, no widened masks or tolerances; every comparison exclusion needs field-specific evidence and an independent review. Worker briefs carry this rule. Steering source: `reviews/claude-review-2026-09-08/MOD_COMPATIBILITY_STEERING_PROMPT.md`.

## Current mission and status: READ `D:\Projects\RESUME.md` FIRST

`D:\Projects\RESUME.md` is the single live resume document: current state, save points and checkpoints, the completion
checklist, build and verify commands, architecture, roadmap, quirks and the path index. Status is NOT duplicated here any
more (the former mission block of this file, with every checkpoint paragraph, lives in RESUME.md section B). What follows
is policy only.

**Binding operating rules (preserved from the former mission block; policy of 2026-09-09 where dated).**

- The milestone branch is `stage2/p4b-interp-lockstep` in `D:\Projects\p4b-interp-validation` (the approved tree; its
  executable at the approved path is the only one the two-process harness launches, with the firewall rule). Worker branches
  are for isolation, one worktree each, reused across follow-ups; reviewed worker branches merge into the milestone branch
  directly, one merge per branch; no more numbered integration branches per source iteration; no rewrite or squash of
  history; historical commit messages and hashes stay as they are.
- **Commit as you go (user's rule, 2026-09-10; supersedes every older "commit only when asked" clause anywhere).** Work is
  committed continuously, several times a day: focused per-concern commits with plain messages, each fix with its regression
  coverage, on the worker branch, as soon as a piece is verified (built and run). Uncommitted work older than a few hours is a
  defect. NO attribution of any kind anywhere: no AI names, no co-author trailers, no generated-with lines, in commits, docs,
  code or reports.
- **Push to GitHub at major milestones, several times a day (user's rule, 2026-09-10; supersedes the 2026-09-09 "after major
  fix groups" wording and every older "pushing is gated on the user's explicit OK" clause in this file and in RESUME.md).**
  Every verified checkpoint on the milestone branch is pushed to `origin` promptly, not every few days. Before a push, verify
  the exact source against the pinned build manifest and the evidence, record the push in RESUME.md, and state the remaining
  failures; a checkpoint push is not milestone completion. Pushing does not need the user's per-push OK. No upstream PRs (the
  feed is held).
- Verification means built and actually run on the claimed configuration; simulation changes need the two-peer gates;
  agent reports never replace built-and-run verification; unfinished work is never labelled complete; comparison masks are
  never widened and local AI is never serialised to get a pass; every comparison exclusion needs field-specific evidence
  and independent review; the `GetAudibleVolume` authority policy is settled (shared gameplay reads the controlling player's
  actual reading, activity-wide and unowned objects read the host, local presentation keeps its local reading).
- Preserve Controller-sync, the fixed timestep, mod compatibility (binding section above), closures, shared references,
  coroutine continuations and async pathfinding and mixing.
- Unattended runs use private hidden runtimes with muted settings; no game window, focus change, sound or firewall prompt
  reaches the desktop; only the approved executable path and the guarded runners run two-process tests; the Mac is reached
  through `ssh Erol-Mac` per `reviews\claude-review-2026-09-08\MAC_CLAUDE_SSH_RUNBOOK.md`.
- Nothing merges into the milestone branch while a verification family runs on the approved tree: the breadth's
  `binary_matches_source` and the matrix's `source_unchanged` verdicts compare the tree against the pinned executable,
  so a merge mid-family turns the rest of the family red for a reason that is not the engine. Reviewed tips wait for the
  family's end and become the next family.
- Every engine launch goes through `tools/win32_test_runner.py` (via `tools/run_sim_test.py` `make_run` from Python, or
  `tools/isolated_launch.py` from PowerShell). Never create the engine process directly (`Start-Process`,
  `System.Diagnostics.Process`, `& "...\Cortex Command.exe"`, `subprocess.Popen`): even with the window created hidden,
  a direct launch makes a GL context and raises its window on the user's desktop, which minimised the user's fullscreen
  game on 2026-09-08 (the loose harnesses in `D:\Projects\stage2_p4` did this; the ones the families use are patched).
- Cleanup (user's instruction 2026-09-10): the user wants test scratch cleaned up, on the condition that nothing of value
 is lost and every item is reviewed before deletion. The lead deletes only what it has personally verified as redundant
 (report or verified package exists elsewhere, branch merged, hashes match), logs every deletion under `reviews/`, and
 never touches the live trees, the current family's evidence or the pinned executables. Retention: keep the compact
 reproduction package of every important failure and fix (source and patches, fixtures and seeds, configuration, necessary
 raw state, exact binaries with symbols, manifests, logs, verdicts); passing jobs' bulk raw state may be compacted once the
 family's review has closed. See the disk-and-junctions rule above.
- Build concurrency: at most TWO engine-building lanes at once, each capped with `CL=/MP6`; NONE while the lead's chain builds the approved tree. The chain uses `/MP12` and runs alone. Full uncapped builds exhaust the machine's memory. The lead coordinates all builds and inspects every gate personally.
- Pointer surfaces kept current at meaningful checkpoints: RESUME.md (always first), `PRs\PR_ROADMAP.html`,
  `STAGE2_H4_RECONNECT_PLAN.md`, `reviews\recovery-2026-09-07\contract-audit\CONTRACTS.md`,
  `reviews\recovery-2026-09-07\contract-audit\mac-peer-20260907\MAC_RESUME.md`,
  `reviews\claude-review-2026-09-08\INTEGRATION_33_RUNBOOK.md` (family runbook, section 9).

## "No compromises" — the work standard

When a hard problem offers an easy way out — serialize instead of fixing the race, disable instead of debugging, defer instead of solving, narrow the goal instead of hitting it — **the easy way out is not the answer.** On this project, "no compromises" means: push through the resistance and engineer the result that gives up nothing — full correctness *and* full performance *and* the rest — never trading one goal away to make another easy.

- This is **not** risk-aversion. Choosing the low-effort or guaranteed-but-worse option *because* it is safer or simpler is itself the compromise being rejected. "This is hard" is not a reason to stop — the hard part is the work.
- A fallback — ship the partial result, take the slower-but-simpler design — is a last resort, used only when the full solution is *proven* genuinely impossible: proven, not assumed because a first attempt met resistance.
- When you hit resistance: root-cause it, research how the problem class is solved elsewhere, build the no-give-up solution, and iterate until it is genuinely there.
- Still escalate genuine **product decisions** to the user — a mod-compatibility break (ADR-004), a fork in design intent. Those are real decisions to surface. Difficulty is not one of them; neither is risk.

## Never send Causeless a broken PR — self-verify GREEN before submission

**Top priority. Non-negotiable.** A PR is NEVER opened upstream — or opened on the fork for Causeless's review — until *we* have verified it is fully green ourselves. We do **not** rely on upstream CI or Causeless's review to catch build or runtime breakage; catching that is *our* job, done before he ever sees it. His review is for design and architecture, not for finding breaks we should have caught. A broken PR wastes his time and is a direct credibility tax on the whole fork (same logic as the over-commenting tax).

**Verification means BUILT and BOOTED — actually run, never reasoned about.** An argument that a state "is covered" — because a dependency is inert, because two PRs are disjoint, because the all-merged build works — is **NOT** verification. If you have not run that exact configuration end to end, it is not verified. No exceptions, including for "trivial" or "vendor-only" PRs. If you catch yourself writing "covered by logic" / "should be fine" / "X implies Y" in place of a result, stop and run it.

**Verify every state the submission sequence passes through:**
1. Fix the submission order explicitly (e.g. `#1 → #2 → #5 → #3`).
2. For that order, **build AND boot every cumulative state upstream will actually receive**: `base+#1`, then `base+#1+#2`, then `base+#1+#2+#5`, then `base+#1+#2+#5+#3`. Each one, individually, end to end — clean build (≥ Windows MSBuild; meson/Linux where feasible) **and** a clean boot/run of the relevant path.
3. A PR is cleared to send only after *its* cumulative state is verified green.
4. The all-merged build passing is **necessary but not sufficient** — it proves nothing about the intermediate states a one-at-a-time merge creates. That gap is precisely where a broken PR slips to the maintainer.

If a configuration can't be verified, build the verification first; never submit on faith or on an argument. This gate is ours, owned up front — never a downstream backstop.

## Upstream alignment — Causeless's stated directions

From Causeless's 2026-05-25 review of the fork:

1. **Controller-sync MP architecture** (not deterministic-AI). The deterministic boundary is the Controller wire — each machine runs its own AI (with async pathfinding) per-machine; only Controllers cross the network. This lets expensive async operations (pathfinding requests) stay async. Rationale: "async operations are fundamentally opposed to determinism" — the Controller boundary solves it.
2. **FPU + libm standardization** for cross-platform determinism, not expanded fixed-point. If Lua float math is deterministic single-threaded (which it appears to be), then physics float math is too. Standardize libm choice + `-ffp-contract=off` everywhere. Reconsider only if FPU math hurts performance.
3. **Smaller, focused PRs.** AI tendency to create enormous bundled changesets makes review/merge/understanding hard. Per-concern decomposition is mandatory.

See **ADR-019** ("Clean stage trunks", Accepted 2026-05-25) in the wiki decision log (`modernization-docs/decisions.html`) for the full record.

## Architecturally rejected (do not bring forward to revamp trunks)

These approaches were tried on `modernization-effort` but are explicitly not part of the revamp foundation:

- **M3 Q40.24 fixed-point math.** Causeless's review: "really no benefit" given Lua-float cross-platform determinism. FPU + libm standardization is the chosen cross-platform path. Q40.24 code stays in `modernization-effort` only; never re-applied to revamp trunks.
- **Pathfinder serial-epilogue (M4A)** moving `Scene::UpdatePathFinding` after `MovableMan::Update`. Required by deterministic-AI approach; redundant under Controller-sync (each machine runs its own async pathfinder).
- **Race A architectural fix** (consumption-side pathfinder serialization). Same reason — Controller-sync eliminates the cross-machine sync requirement that motivated it.
- **Multi-platform race-investigation iteration commits.** The final FIXES (Race B atomic, Race C sync rebuild + re-sort, Race D when found) come forward as single clean commits; the investigation history stays in `modernization-effort`.
- **M5.5 mega-bundling.** The same code comes forward, but split into ~4-5 smaller commits per concern.

These rejections aren't regret — they're the result of empirical work that informed the right path. The work was useful; the *form* it took is the part that doesn't belong on the upstream-aligned trunks.

## Cross-platform determinism — the working approach + agent topology

Learnings from the Stage-1 cross-platform determinism work (x86 Win/Linux + arm64 Mac):

- **The lean fixes are what work — a deterministic POLY, never a heavy vendor.** SimBaseline (the idle-sentry baseline scenario) is now **bit-identical across x86 Win + x86 Linux + arm64 Mac, all 600 ticks** (`final 4468d355`). The foundation = the **keep-set** (portable mt19937-state hash + explicit RNG-distribution mapping + Allegro integer `fixmul`/`fixdiv`) + the **deterministic timestep config-lock** (pin one canonical `dt`; a per-machine `Settings.ini` timestep is poison) + the **controller sim-time fix** (`IsPastSimMS`, never wall-clock in a sim tick) + a **deterministic transcendental poly** for the trig that diverges cross-arch + the **eval-order draw-order fix** (next bullet). The trig poly fixes: **terrain** = Allegro sprite-rotation `_AL_SINCOS` libm sin/cos (3-platform verified) and **physics rotation** = `Vector::GetRadRotatedCopy` routed through `DeterministicSinCos` (range-reduce + Taylor, basic ops → bit-identical across MSVC/glibc/Apple because `/fp:precise`+`-ffp-contract=off` are set), routed at the **PRIMITIVE level** (per-site mislocalized twice), NEVER fixed-point (ADR-019 / Causeless #2). The poly == the x86 libm (no-op on x86, only the arm64/Apple outlier converges) but differs from glibc by 1 ULP at ~1.3% of angles → combat shifts values on ALL platforms to the poly consensus (cross-platform determinism still holds — the goal). `DeterministicAtan2` (high-accuracy minimax, NOT Taylor) is **next** — combat aiming (`GetAbsRadAngle`) is the in-progress residual (the ActorStress combat grind). **The `detmath` vendor is INERT/unused — the lean poly is the fix, not a libm vendor.** ⚠️ The `particles@29` residual was **NOT trig** — it was the eval-order bug below; the hunt chased physics→trig→threads first, a lesson in measure-don't-infer.
- **Unsequenced eval-order of RNG-consuming calls is a determinism bug class.** C++ leaves function-argument evaluation order unsequenced; MSVC/GCC draw right-to-left, Apple clang left-to-right. So `Vector(-RandomNum(x), -RandomNum(y))` consumes the two `g_SimRNG` draws in opposite order cross-compiler, swapping which feeds each axis → cross-platform desync. This — not any FP/trig divergence — was the SimBaseline `particles@29` closer (`SceneMan::TryPenetrate` debris spawn; Win==Linux==`442f8cf9` vs arm64==`b807703e`). Fix: sequence the draws into named locals so every compiler draws in source order. Audit ALL multi-`RandomNum`/`RandomNormalNum`/side-effecting-call expressions (task #104).
- **No off-wire carve-outs.** The determinism foundation must be **fully bit-identical** cross-platform — every subsystem, every tick. "This part is off-wire (terrain gen, AI) so it can diverge" is a compromise, not a fix: a divergence anywhere can cascade into the replicated sim. The bar is the *full trace* bit-identical.
- **Pin residuals by MEASUREMENT, not inspection.** Cross-platform bit-identity is a multi-residual grind. Don't reason about which op diverges — instrument a cross-platform dump (raw bytes / per-tick hashes), build on each machine, byte-diff, localize, fix, re-verify the full trace, repeat.
- **Agent topology + contention.** The **macOS agent** = a separate Mac Mini (native arm64) — the cross-*arch* reference; no build contention with this PC. The **WSL2 agent** = Linux x86 **on THIS PC** — the cross-*OS* reference; its builds **contend with Windows builds**, so while it runs, stay analysis-only on Windows. Both are relayed by the user (draft the prompt → user relays → report comes back); comparison artifacts ride `xref/*` branches.

## Branching & worktree convention

For any substantial work (flagship milestones, multi-file features, experiments), **use the worktree pattern** so each effort is isolated, parallel-workable, and individually deletable. This is already in use for the FoW work (worktrees at `D:\Projects\dynamic-fow-negative-distance-test` and `D:\Projects\pr-fow-character-centric-2stage`).

### Stage convention — trunk evolves across project phases

The project's trunk evolves through named stages as the codebase matures toward upstream-mergeable state. Each stage is a clean branch built on top of the previous; old stages stay as legacy reference.

| Stage | Branch | Status | Purpose |
|---|---|---|---|
| 0 (legacy) | `modernization-effort` | frozen for new milestone work | Reference / source-of-truth for code being selectively brought forward. In-flight stragglers (e.g., Race D Windows round-3) can finish here. |
| 1 (done — proven outcome branch: `determinism-cs`) | `stage/cumulative-verify` (integration) → `determinism-revamp` (clean target trunk) | shipped as the PR 3–10 feed | The integrated determinism foundation (PRs 3–10) lives on **`stage/cumulative-verify`** (`066af5922`) — that's the build/work tip, and the worktree `D:\Projects\determinism-revamp` is checked out on it. **`determinism-revamp`** is the *clean trunk those PRs merge into* (currently at the upstream base `20dfb3ea5` — it does NOT yet hold the foundation work). Foundation = cccp-ctl harness, TSan CI, Path E, Race B/C fixes, M1/M2 scenarios. Cross-arch completeness is in progress on `xref/*` branches. Drops M3 Q40.24, pathfinder serial-epilogue, Race A. |
| 2 (active) | `stage2/p1-controllerframe-synctest` → `stage2/p2d-gns-transport` → `stage2/p3-controller-lockstep` → **`stage2/p4b-interp-lockstep`** (current milestone tip, worktree `D:\Projects\p4b-interp-validation`; `stage2/p4a-multiplayer-ui-alpha` in `D:\Projects\xarch-combat` is the previous phase) | active | Stage-2 lockstep MP as a stacked chain, each phase cut from the previous full-green head (P1B→P2D→P3→P4A). Controller-sync from day one; P4B next after the P4A gate. The old planned name `multiplayer-revamp` was never used. |

Per Causeless's 2026-05-25 review feedback, large bundled commits + zig-zag direction changes make upstream merge effectively impossible. Stage trunks give each phase a clean, focused, reviewable narrative that could in principle be packaged for upstream PRs incrementally.

### The pattern

```bash
# 1. Identify the active integrated trunk tip
#    Stage 1: stage/cumulative-verify (integrated foundation; determinism-revamp = clean merge target)
#    Stage 2: stage2/p4a-multiplayer-ui-alpha  (each new phase cuts from the previous full-green stage2/* head)

# 2. Sync with latest upstream/development
git -C D:/Projects/cccp fetch upstream
git -C D:/Projects/cccp checkout stage/cumulative-verify   # the integrated Stage-1 tip
git -C D:/Projects/cccp merge upstream/development         # or rebase, per ADR-008

# 3. Cut branch + worktree for the new work FROM the active tip
git -C D:/Projects/cccp branch <branch-name> stage/cumulative-verify
git -C D:/Projects/cccp worktree add D:/Projects/<dir-name> <branch-name>

# Work happens in D:/Projects/<dir-name>/
# Main repo at D:/Projects/cccp stays on modernization-effort (legacy reference), untouched.
```

### Cleanup when work doesn't pan out

```bash
git -C D:/Projects/cccp worktree remove D:/Projects/<dir-name>
git -C D:/Projects/cccp branch -D <branch-name>
```

Zero residue. Main repo unaffected. Reflog preserves SHAs for ~30 days if you want to recover.

### Branch naming convention

| Prefix | Use | Cuts from |
|---|---|---|
| `flagship/<milestone>` | Substantial flagship-milestone work (e.g., `flagship/mp-m7-lan-lockstep`) | active stage trunk (`determinism-revamp` → `multiplayer-revamp`) |
| `feature/<name>` | Smaller features not tied to a flagship milestone (e.g., `feature/gamepad-rumble`) | active stage trunk |
| `experiment/<name>` | Throwaway-by-default branches for trying approaches. The `experiment/` prefix is the "I might scrap this" signal. | active stage trunk or a flagship branch |
| `fix/<name>` | Bug-fix branches | active stage trunk |
| `pr/<name>` | Branches cut specifically for upstream PR submission. **MUST be cut from `upstream/<target-branch>` directly**, not from the active trunk — this keeps the PR diff clean. Cherry-pick the relevant commits onto it. | `upstream/development` (or specific upstream branch like `upstream/dynamic-fow-negative-distance-test`) |
| `stage/<name>` | Integration / cumulative trunks — the carrier itself, not per-concern work. `stage/cumulative-verify` = the PR-3–10 stack for CI + the eventual trunk merge; `stage/cum-base-3-7` = a stacked base. | the integration point |
| `xref/<name>` | Cross-platform **reference / measurement** branches — built on another machine (the macOS or WSL2 agent) to dump or compare determinism artifacts (e.g. `xref/keepset-dtlock`, `xref/arm64-terraindump`). Throwaway by default. | the tree being compared |

### PR decomposition — small, focused, per-concern

Per Causeless's 2026-05-25 review feedback: **one logical concern per PR**, not bundled multi-effort branches.

- **Bad**: a single branch that bundles test infrastructure + TSan CI + race fixes + cross-actor mutation enforcement + universal UB fixes.
- **Good**: separate PRs for each piece — `pr/cccp-ctl-scaffolding`, `pr/cccp-ctl-snapshot-restore`, `pr/tsan-ci`, `pr/race-b-callback-id-atomic`, `pr/path-e-enforcement`, etc.

Rules:
- Each PR has one purpose statable in one sentence.
- If two changes share a file but serve different purposes, two PRs (sequenced if dependent).
- A PR doing "small fix + nearby refactor + unrelated cleanup" is three PRs.
- Reviewable in one sitting (< ~300 lines diff target; up to ~600 acceptable for unavoidable engine changes).
- Multi-step features land as a stacked series of small PRs, not a single mega-PR.

### Keep experimental branches local until ready

**Critical for the "easy to scrap" goal**: don't push experimental branches to `origin` (Madreag/cortex-modern) until you actively want others (or CI/bots) involved. Local branches can be deleted with zero externally-visible footprint. Once pushed, the branch exists on GitHub's history and triggers bots.

**Push when:**
- Want CI to run (msbuild Windows passes; Meson fails on a pre-existing `GUIRect` issue regardless — that's documented in the FoW PR triage)
- Want bot review (BugBot, Devin, Codex)
- Want to share with Causeless / upstream
- Want backup or cross-machine sync

**Don't push when:**
- Trying an approach you might scrap
- Mid-iteration and the branch is in a broken state
- Just want personal-fork practice work

### Worktree hygiene

- **Don't work in the main `D:\Projects\cccp` worktree** for substantive new work. The main worktree stays on `modernization-effort` as the legacy reference. Active development uses dedicated worktrees on the active stage trunk (`determinism-revamp`, then `multiplayer-revamp`).
- **One branch per worktree.** If you need to try two competing approaches to the same problem, create two worktrees.
- **Periodically clean up** stale worktrees with `git worktree list` then `git worktree remove`.
- **The reflog is your safety net** if you delete something you wanted — `git reflog show <branch-name>` reveals SHAs within ~30 days.

### Stacked-branch workflow for milestone sequences

For flagship milestone work (AI M0/M1/..., MP M0/M1/..., Campaign M0/M1/...) **each new milestone is built on the previous milestone's branch**, forming a linear stack. This lets you keep working without blocking on PR reviews of the previous milestone.

```
upstream/development
  └─ determinism-revamp  (Stage 1 — foundation rebuild)
       └─ multiplayer-revamp  (Stage 2 — cut from determinism-revamp tip when Stage 1 complete)
            └─ flagship/mp-m7-lan-lockstep          (push when ready, open PR, then IMMEDIATELY...)
                 └─ flagship/mp-m8-online-rollback-coop  (...cut next milestone from M7's tip)
                      └─ flagship/mp-m9-online-rollback-pvp       ← shippable PvP target
                           └─ flagship/mp-m10-public-hosting
```

**Cadence per milestone:**
1. Build locally on the milestone branch
2. When ready, push and open PR (cortex-modern fork PR, or upstream `pr/*` if upstreaming)
3. **Immediately cut next milestone branch from the current tip** — don't wait for review
4. When current milestone lands in the active stage trunk, rebase the next branch onto the updated trunk
5. Address review feedback in the original worktree, force-push, rebase descendants if needed

**Steady state:** 1 active dev branch + 1-2 branches in review + `modernization-effort` (base). At most 2-3 active worktrees at any time.

**For sub-experiments within a milestone:** cut `experiment/<name>` branches off the current milestone branch, delete freely if they don't pan out.

**For upstream PRs:** cut `pr/<milestone>` branches off `upstream/development` (NOT off the local stack). Cherry-pick the squashed commits for that milestone. Each upstream PR is independent — they all share the same `upstream/development` base.

## Pre-existing conventions worth knowing

Captured here for reference; see `D:\Projects\HANDOFF.md` for full context.

- **Trunk evolution**: `modernization-effort` (cut from `upstream/development` @ `41e6b7010`) was the working branch through M4A. Now **legacy** — preserved as source-of-truth for code being selectively brought forward; in-flight stragglers (e.g., Race D Windows round-3) can finish there. New working tip is **`stage/cumulative-verify`** (Stage 1 — the integrated foundation; **`determinism-revamp`** is the clean trunk it merges into). When the determinism foundation is complete + merged, fork to **`multiplayer-revamp`** (Stage 2).
- **`development` (local)** is a clean mirror of `upstream/development` — never commit directly.
- **ADR-008**: fork-first; upstreaming is opportunistic, never a gate. Work in cortex-modern first, optionally upstream via `pr/*` branches later.
- **Commit attribution**: ALL commits, PRs, branches, and other authored artifacts are attributed solely to the user (Madreag / Erol Germain-Gomuc). **NEVER add `Co-Authored-By: Claude` trailers** or any other AI-attribution. Even when Claude does the work, the author of record is the user. Same rule for PR bodies, Discord messages, and other public communications — write in the user's voice without "🤖 Generated with Claude Code" footers or similar.
- **Commit as you go; push at every verified checkpoint, several times a day** (user's rule 2026-09-10; see the binding operating rules above). The old "commit/push only when the user asks" convention is retired.
- **The wiki at `D:\Projects\cccp\modernization-docs\`** is the project's planning artifact; ADRs are append-only.

## Code comments — match Causeless's style

Upstream lead **Causeless** flagged this reviewing the FoW PR (2026-05-19):

> "AI comments WAY too much. It very much confused and muddies the code when the AI feels the need to add several paragraphs explaining things as mundane as a null dereference fix or other minor adjustments. These things are self-explanatory and if anything, the in-depth comments actually confuse the situation further (especially when it's unclear if the comment is referring to an old problem or an extant one)."

**Copy his actual in-tree style.** A scan of Causeless-authored engine code shows a consistent, terse pattern — match it on every commit to this repo (C++, Lua, shaders, build files):

- **Short — almost always one line.** The longest comments he writes run to two lines and are rare. Never a paragraph, never a multi-line block.
- **Brief block labels are fine and idiomatic** — this is his most common comment. A one-line, present-tense label above a block: `// Run seeing rays for all actors`, `// Get all the particles emitted this frame`, `// Add ourselves to the last passenger's inventory`. Short, plain, says what the next few lines do.
- **Non-obvious WHY, stated plainly and casually:** `// This is a bit funky but consistent with the code that applies the impulse`, `// We do async GC, but we still keep the normal GC on so it can catch any big spikes`. One line, conversational register.
- **TODOs name the constraint, terse:** `// TODO- RemoveActor removes the actor directly, so actorsSeeFuture must finish immediately`. One line, maybe a second for "this isn't ideal because…". No essay.
- **No fix-narration.** His crash-fix commits add *zero* comments — he changes the code and lets the commit message carry the story. Never write "this used to do X" / "fixes the crash where…" in source. When he fixes a root cause he *deletes* the stale workaround comment rather than leaving it to rot.
- **No milestone/ticket tags in code.** No "M1 Block C", no issue numbers — commit message and PR body only.
- **Header declarations** use `///` Doxygen (`/// Gets whether…` + `/// @return …`) — the codebase standard.

Rule of thumb: if Causeless wouldn't have written it, delete it — a comment is one short line or it doesn't ship. Holds for fork-internal commits as much as upstream PRs; he reviews both, and over-commenting is a credibility tax on the whole fork.

## Build & test commands

See `D:\Projects\HANDOFF.md` §5.3 and §16.7 for full build instructions. Quick reference:

```bash
# Windows MSBuild (the primary path - run via PowerShell, not Git Bash)
# Configurations: Debug Full | Debug Minimal | Debug Release | Final  (there is NO plain "Release" config)
# vswhere: use the LITERAL path — the ${env:ProgramFiles(x86)} form can fail to expand in some PS contexts.
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
# Build the ACTIVE worktree's sln (e.g. the determinism trunk), not always cccp's:
& $msbuild "D:\Projects\determinism-revamp\RTEA.sln" /t:RTEA /p:Configuration="Final" /p:Platform=x64 /m /nologo /v:minimal
# /t:RTEA = incremental · /t:RTEA:Rebuild = clean. The ':Build' target does NOT exist.
```

Before launching the game, kill any prior instance:
```powershell
Get-Process | Where-Object { $_.ProcessName -like "Cortex Command*" } | Stop-Process -Force
```

**Running the headless determinism harness on Windows** — the game exe is GUI-subsystem, so the invocation matters:
```powershell
# A PIPE makes PowerShell WAIT for the run. A redirect (*> file / *> $null) DETACHES it —
# empty $LASTEXITCODE, no trace written — which looks like a crash but isn't. Always pipe:
& ".\Cortex Command.exe" -scenario SimBaseline -seed 42 -max-ticks 600 -tick-hashes -num-lua-states 4 -out trace.json 2>&1 | Out-Host
```
