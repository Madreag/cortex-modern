# Cortex-Modern — Project Instructions

Policy for every agent working on the **cortex-modern** fork (`D:\Projects\*` worktrees, the Mac). This file and
`D:\Projects\AGENTS.md` are byte-identical twins; `D:\Projects\cccp\AGENTS.md` is a pointer to them. **Read
`D:\Projects\RESUME.md` first** — it is the single live resume document (state, next actions, lessons, path index);
this file is policy only. Rewritten 2026-09-12 18:10 MST from the layered 2026-09-08 → 09-12 versions (verbatim
pre-rewrite copy: `_archive/docs_archive_20260912/CLAUDE_pre-rewrite-20260912.md`); every rule below is still
binding, the superseded narrative is gone.

## 0. The operating model (2026-09-12)

- **The lead** is a Claude Code session (Fable 5.1, effort max). Its weekly budget is the scarcest resource: it
  spends its own tokens on reading diffs, re-deriving evidence and writing verdicts and documents, and delegates
  builds, batteries, log digging and mechanical implementation.
- **Worker routes** (details and exact commands: RESUME.md §0.5 and §6.3b): Opus 5 at effort max through the Agent
  tool's project agents `cortex-opus-engineer` / `cortex-opus-verifier` / `cortex-opus-visual-reviewer` (engineering
  and design-heavy lanes, deep verification, EVERY screenshot or visual question); Grok 4.6 Extra High Fast through the
  Cursor CLI only (`grok-workers/win_cli/win_job.py`, on the Mac `grok-workers/mac_cli/mac_job.py`; batteries, triage,
  reviews, mechanical implementation from a lead-fixed design); SWE-2 Max through the Devin CLI
  (`grok-workers/devin_cli/devin_job.py`, free; bounded UI/feature work with named seams; at most two sessions, kept
  busy by `devin_queue.py`); Claude Code Opus 5 on the Mac (`grok-workers/mac_cli/mac_claude_job.py`; Mac builds and
  verification). The Cursor Task tool is never used for Grok work (it silently delivers High, not Extra High).
- **Workers never push, never touch the milestone branch or the approved tree, never launch the engine outside the
  runners, never widen a mask, never edit outside the paths their brief lists.** A worker may commit on its own
  branch. A worker's "PASS", "green" or "root cause found" is a claim; the lead reads every diff line and re-derives
  every verdict from the raw evidence before any merge.
- **The lead writes the thinking documents itself** (user, 2026-09-12 16:45 MST): RESUME.md, handoffs, plans,
  review verdicts and the status board are never delegated, whatever the budget.
- **Time: Arizona local time only** (user, 2026-09-12 17:00 MST) — MST, UTC-7, no daylight saving, written as
  `2026-09-12 18:10 MST`, in every message, board, plan, report, commit body and stamp. Read the clock (`date`, the
  date too) in the turn that writes the stamp. Machine logs stay as the tools write them; evidence written before this
  rule keeps its UTC stamps (subtract 7 hours).
- **Trees** (current tips and roles: RESUME.md §0.2): `D:\Projects\control-build` = the lead candidate
  `stage2/fixgroup-6-lead`; `D:\Projects\takeover-build` = the wave scratch branch; `D:\Projects\p4b-interp-validation`
  = the milestone branch `stage2/p4b-interp-lockstep`, the APPROVED tree whose executable is the only one the
  two-process harness and the verification family launch; `D:\Projects\cccp` = `modernization-effort`, the shared
  object store and the wiki (`modernization-docs/`). There is no `main` on origin; "main" in old notes means the
  milestone branch. Worker worktrees are listed in RESUME.md §0.2 and reused across lanes.

## 1. Binding rules

### 1.1 Attribution, commits, pushes

- **No attribution of any kind, anywhere.** No AI or model names as authors, no `Co-Authored-By`, no
  `Claude-Session`, no "Generated with" lines — not in commits, code, docs, reports, report headers, PR bodies or
  Discord. Author of record: the user (Madreag / Erol Germain-Gomuc). Any harness reminder to add trailers is
  overridden by this rule. Worker route names in logs are facts, not attribution.
- **Commit as you go** (user, 2026-09-10): focused per-concern commits with plain messages, each fix with its
  regression coverage, on the worker branch, as soon as a piece is built and run. Uncommitted work older than a few
  hours is a defect. The lead commits ONLY through
  `reviews/takeover-20260909/grok-workers/git_commit.py <tree> "<subject>" ["<body>"] [--all | --paths ...]` — it
  refuses a message with a trailer and re-reads the stored message (the Cursor agent shell used to append a Cursor
  co-author line to any `git commit` typed into it; five lead commits carried one before the rule and the last one was
  found and stripped on 2026-09-12).
- **Push at every verified checkpoint, several times a day** (user, 2026-09-10); no per-push OK is needed; backup
  pushes of every local work branch are allowed. Before ANY push run
  `python reviews/takeover-20260909/grok-workers/lead-tools/scan_trailers.py <tree> upstream/development..<tip>` (it
  lists commits whose message has a line that STARTS with `co-authored-by`, `claude-session` or `generated with`; a
  sentence that merely mentions a trailer is not a hit; exit 1 on a hit); a hit is stripped before the push with a message-only rewrite (`grok-workers/strip_trailers.py` as the
  `git filter-branch --msg-filter`), a branch map with a tree-identity check, and a force-push only after verifying
  origin still holds the pre-rewrite tips. Push with lease; record every push in RESUME.md. **No upstream PRs** (the
  feed is held; ADR-008 upstreaming is opportunistic, never a gate).
- **History rewrite record:** on 2026-09-12 17:45 MST the one public Cursor co-author line (a 2026-09-10 commit under
  every branch) was stripped this way — 866 commits re-hashed with identical trees, 147 branches and 26 tags
  force-pushed, origin's diverged `exp/determinism-foundation` stripped separately. Maps:
  `reviews/takeover-20260909/trailer-strip-20260912/` (`map.txt` branch tips, `commit-map.txt` old → new). Evidence
  written before that moment quotes pre-strip shas; resolve them through the map. A clone or worktree made before it
  must `git fetch --all` and reset each branch to `origin/<branch>`. Upstream's own 2022-2025 human co-author lines
  are not ours and stay.

### 1.2 The verification standard

- **Verification means BUILT and RUN** on the claimed configuration, with the real output recorded. "Covered by
  logic", "should be fine", "X implies Y" is not a result. Unfinished work is never labelled complete; partial
  coverage is a checkpoint.
- **Inventory-first, never whack-a-mole** (user, 2026-09-11): the whole battery runs ONCE on a changed tree; every
  failure is root-caused in parallel (one read-only worker per failure); the fixes land as ONE group; then ONE battery.
- **The lead verifies personally**, for every failure, that it is a TRUE failure (the engine did the wrong thing, per
  evidence the lead looked at — not an oracle reading a wrong path, a fixture ending the game, a timing artefact or a
  worker's guess), and for every fix, that it is WELL IMPLEMENTED (the lead reads the whole diff; the detecting test
  is red on the exact defect and green after, re-run by the lead on the rebuilt binary; the named mechanism is what
  the code changes; nothing masked, widened or patched around). Every HIGH-severity fix also gets an independent
  Opus verifier pass that re-derives RED/GREEN from the raw logs and quotes file:line; the lead spot-checks the
  flagged lines on the source.
- **RED first, on the exact defect.** A fix without a detecting test that was red is argued, not verified. Grep
  every worker diff for production branches only a test can reach and for failure messages that state the expectation
  instead of the failure.
- **Never widen a comparison mask, tolerance or exclusion; never serialise local AI to get a pass; never patch a mod,
  fixture or test to hide an engine regression.** Every comparison exclusion needs field-specific evidence and an
  independent review. Every oracle, checker or driver change between a failing and a passing run is reported with
  both runs; an undisclosed one is treated as a masked failure.
- **Any lane that changes a wire or version constant runs `tools/run_selftests.py` 11/11, and the lead's read checks
  that it did.** After editing a golden or a version constant the lead re-runs the detecting test on the rebuilt
  binary (the F14 → F14b half-fix).
- Preserve Controller-sync, the fixed timestep, mod compatibility (§3), closures, shared references, coroutine
  continuations, async pathfinding and async mixing. The `GetAudibleVolume` authority policy is settled (shared
  gameplay reads the controlling player's actual reading; activity-wide and unowned objects read the host; local
  presentation keeps its local reading).

### 1.3 Merges, waves, the milestone

- Reviewed worker branches merge into the wave or candidate branch one merge per branch; keep-both conflict
  resolutions edit the conflicted WORKING file (never a copy rebuilt from a stage — auto-merged hunks live only
  there); resolve, marker-check and commit are separate steps; every merge commit is checked with
  `grok-workers/lead-tools/verify_merge_commit.py <tree> <merge> <other>` (per file, the +/- line multiset of
  `merge^1..merge` must equal `merge-base..other`; only both-sides-identical lines may differ). Read two lanes' diffs
  together when they touch the same seam.
- A wave is prepared on a scratch branch (`stage2/fixgroup-6-lead-wave-a` in `takeover-build`) while a battery pins
  the candidate's executable; the candidate is promoted by fast-forward afterwards, rebuilt alone, then verify6, then
  ONE battery, then the family.
- **Nothing merges into the milestone branch while a verification family runs on the approved tree** (its
  `binary_matches_source` / `source_unchanged` verdicts compare the tree against the pinned executable). Reviewed tips
  wait for the family's end and become the next family. No rewrite or squash of history except the message-only
  trailer strips of §1.1; historical messages and hashes otherwise stay.

### 1.4 Builds and engine launches

- **Build cap: at most TWO engine-building lanes at once, each `CL=/MP6`; NONE while the lead's candidate or family
  build runs (`/MP12`, alone).** Judge the cap on `cl.exe`/`link.exe` from other working directories — idle
  `MSBuild.exe` node-reuse processes linger for an hour and mean nothing. Never rebuild a tree while an engine of that
  tree runs (LNK1104) or while a battery pins its executable.
- **Every engine launch goes through `tools/win32_test_runner.py`** (via `tools/run_sim_test.py` `make_run` from
  Python or `tools/isolated_launch.py` from PowerShell; `tools/posix_test_runner.py` on the Mac). Never create the
  engine process directly (`Start-Process`, `subprocess.Popen`, typing the exe at a shell): a direct launch makes a GL
  context and raises a window on the user's desktop. Never run `-net-port-map-probe` or a bare `-net-p2p-selftest`
  connect mode.
- **Nothing reaches the user's desktop from unattended work**: `CCCP_HEADLESS=1` in every worker environment (the
  `*-selftest` flags imply headless in the engine); private hidden runtimes with muted settings; no GUI tool of any
  kind in a lane (no WinDbg, browser, editor window, headed game). A visible engine is killed by PID and the lane is
  resumed with a corrective prompt.
- **Firewall.** Windows prompts for any executable path with no per-program allow rule the first time it binds a
  socket (a GNS client binds an ephemeral UDP port, so a port-range rule cannot cover it). The moment the lead creates
  a worktree or a control build it runs `reviews/takeover-20260909/grok-workers/firewall_allow_all_exes.ps1` ELEVATED
  (it adds per-program Allow rules for every `Cortex Command*.exe` under `D:\Projects`; nothing else is touched);
  `tools/win32_test_runner.py` refuses a `-net*` launch whose executable has no inbound rule
  (`firewall_allow_rule_present`). Workers cannot add rules: they report and stop. New lanes reuse the directory of a
  merged worktree by checking out a fresh branch there. No watchers, scheduled tasks or firewall changes for other
  projects, ever.

### 1.5 Disk, junctions, scratch, cleanup

- **`D:\mx` junction rule.** Every harness run directory carries a `runtime\Data` junction into an engine tree and the
  matrix roots junction into the approved tree. `robocopy /MOVE`, `Move-Item`, `shutil.move` and `rmtree` follow
  junctions and have emptied the repository's `Data` twice. Workers never move, copy or remove a directory tree under
  `D:\mx` or any run root; a lane out of disk stops and reports. Only the lead frees space, with a walker that never
  enters a reparse point (unlink junctions, never follow them) and checks the engine trees' `Data` counts afterwards.
  `git worktree remove` follows junctions too: list every reparse point inside a worktree
  (`Get-ChildItem -Recurse -Attributes ReparsePoint`) and unlink each first; workers never create junctions from an
  engine tree into their worktrees.
- **Scratch footprint.** A lane keeps under its run root only what its verdict needs: at most one control build
  besides the tip, no extra worktrees or tree copies, no copies of executables beyond the pinned control and tip, only
  the final run's restoration and fuzz outputs. Over 5 GB on this PC or 12 GB on the Mac: stop and say so. Never
  redirect a runner's stderr into a lane directory unbounded (six 1.2 GB dumps were found on 2026-09-12).
- **Cleanup** (user, 2026-09-10 and 2026-09-12): delete only what the lead has personally verified as redundant;
  log every deletion under `reviews/` (`reviews/cleanup-20260912.md` is the model); never touch live trees, a running
  family's evidence or a pinned executable. Retention: the compact reproduction package of every important failure
  and fix (source and patches, fixtures and seeds, configuration, necessary raw state, exact binaries with symbols,
  manifests, logs, verdicts); passing jobs' bulk raw state may be deleted once its review has closed. What was deleted
  and what was kept on 2026-09-12: RESUME.md §4.7.

### 1.6 The Mac

- The Mac Mini is a second worker host, never idle by policy: Grok through the Cursor CLI
  (`/Users/erol/.local/bin/cursor-agent`, logged in as the user) and Claude Code Opus 5, both as launchd jobs
  bootstrapped into the logged-in GUI session `gui/501` — a plain SSH session (`ssh -o BatchMode=yes Erol-Mac`) has a
  locked login keychain and even `--help` fails. Lanes live under `/Users/erol/cortex-workers/<lane>` (fresh clones;
  `cli-home` is the CLI profile and stays); `/Users/erol/Documents/Codex/` is retained evidence, read-only. The Grok
  model gate reads `agent-output-<lane>.jsonl` (the dash matters). Evidence comes back with `tar` over ssh excluding
  `runtime`/`external`/`Data`/`build`/`.git` (`scp -r` follows the `Data` symlink); a lane is deleted once its evidence
  is fetched. Runbook: `reviews\claude-review-2026-09-08\MAC_CLAUDE_SSH_RUNBOOK.md`; on the Mac call `/usr/bin/python3`.

### 1.7 Boards, logs, pointer surfaces

- **`D:\Projects\STATUS.md` is the live status board** (user, 2026-09-11): every status request updates the board
  first, then answers with one line per item 1-8 (percentage + delta since the previous ask) and exactly what is being
  worked on now (task and worker); each request appends a history line. Every task, worker or Mac lane must move a
  number; one that lands without moving a number is reported as a NEGATIVE delta with the reason, and the lead
  re-evaluates before spawning the next.
- `grok-workers/SPAWN_LOG.md` gets a row per spawn (written by the runners, with the model evidence line) and per
  landing (written by the lead, with the verdict); `reviews/takeover-20260909/LEAD-REVIEW-overnight.md` is the
  review ledger (findings register, acceptances, NEGATIVEs). A report that exists only in an agent's notification is
  saved to the lane's `REPORT-*.md` in the same turn.
- Pointer surfaces kept current at meaningful checkpoints: RESUME.md (always first), `STATUS.md`, `LEAD_PLAN.md`
  (the 2026-09-11 plan; its still-current parts are named in RESUME.md §0.1), `PRs\PR_ROADMAP.html`,
  `STAGE2_H4_RECONNECT_PLAN.md`, `reviews\recovery-2026-09-07\contract-audit\CONTRACTS.md` and
  `mac-peer-20260907\MAC_RESUME.md`, `reviews\claude-review-2026-09-08\INTEGRATION_33_RUNBOOK.md` §9. Stale pointers
  have cost full re-investigations twice.

## 2. Worker protocol

- **Every brief** names the worktree and branch at a sha (re-read from the tree — a stale base wasted W94), the exact
  editable paths, the RED line and the GREEN runs that define acceptance (a brief that says "root-cause and fix"
  produces a partial), the report path, the mod-compatibility rule, the no-attribution rule, `CL=/MP6` and the
  two-lane cap, the runner-only launch rule with `CCCP_HEADLESS=1`, the `D:\mx` rule, lane-unique ports and scratch,
  a time cap that fits the work, "final message ≤ 25 lines", and it points at `grok-workers/WORKER_RULES.md`
  (template: `LEAD_PLAN.md` Appendix B). Workers get bounded, mechanical or parallelisable work from a design the lead
  has fixed — never judgement calls, never "decide what is green".
- **Model verification is from the runner's evidence line, never from a slug**: `win_job.py` / `mac_job.py` read the
  CLI's first stream-json line and kill the job unless it names `Cursor Grok 4.6 Extra High Fast` (exit 4 = REFUSED);
  `mac_claude_job.py` reads the transcript's first line for `claude-opus-5`; `devin_job.py` runs a pre-flight smoke
  whose export must name `SWE-2 Max`. Everything is bounded by a hard `--timeout`; nothing polls forever; a
  rate-limited Devin turn resumes the SAME session. Work produced by the earlier High-level Grok generation (W12-W38)
  stands only on the lead's line-by-line review and the built-and-run gates it passed.
- **Lead duties per landing**: extract the final message; read the report's "not done / deviations" first; read
  every diff line; re-derive at least the RED and GREEN lines (and two numbers of every report) from the raw files it
  cites; re-run the detecting test yourself when the lane's own run is the only evidence; `verify_merge_commit.py`
  after the merge; the SPAWN_LOG row, the LEAD-REVIEW entry, a STATUS number moved or a negative delta. When a worker
  stalls or drifts, the lead redirects or replaces it rather than accepting a weaker result.
- Step-by-step spawn instructions per route, exit codes, resume mechanics and the failure modes seen so far:
  RESUME.md §6.3b.

## 3. Mod compatibility (binding, 2026-09-08)

Existing mods must keep working unchanged. Breaking Lua or native API behaviour to solve determinism, checkpointing,
ownership or copy-on-write problems is not authorized: fix the engine instead, and never make mod authors rewrite
working scripts. Return-by-reference properties stay live aliases (`sound.Pos.X = value`,
`local p = sound.Pos; p.X = value` keep their established behaviour), Vector operations, native argument conversions,
identity/alias, ownership and garbage-collection behaviour stay as they were, and these semantics must hold through
local/shared audio scopes, copy-on-write transitions, ownership transfer and restored object graphs. Compatibility is
proven with unchanged Lua fixtures run on a retained pre-change reference executable, on the broken build and on the
repaired build (the fixtures must detect the break); matching host/client hashes cannot prove compatibility when both
peers run the same broken semantics, and the `lua_state` tick hash is not an adequate oracle. No opt-in compatibility
flags, no patched mods or fixtures to hide engine regressions, no widened masks or tolerances. A genuine
mod-compatibility break is a product decision escalated to the user (ADR-004). Steering source:
`reviews/claude-review-2026-09-08/MOD_COMPATIBILITY_STEERING_PROMPT.md`.

## 4. "No compromises" — the work standard

When a hard problem offers an easy way out — serialize instead of fixing the race, disable instead of debugging, defer
instead of solving, narrow the goal instead of hitting it — the easy way out is not the answer. This is not
risk-aversion: choosing the low-effort or guaranteed-but-worse option because it is safer is itself the compromise
being rejected. "This is hard" is not a reason to stop. A fallback is a last resort, used only when the full solution
is proven impossible — proven, not assumed because a first attempt met resistance. Root-cause resistance, research how
the problem class is solved elsewhere, build the no-give-up solution, iterate until it is genuinely there. Genuine
product decisions (a mod-compatibility break, a fork in design intent) still escalate to the user; difficulty and risk
do not.

## 5. Upstream: Causeless's directions, the held feed, rejected architecture, comment style

**Never send Causeless a broken PR.** A PR is never opened upstream, or on the fork for his review, until we have
verified it fully green ourselves: every cumulative state a one-at-a-time merge creates is built AND booted end to end
(`base+#1`, `base+#1+#2`, …); the all-merged build passing is necessary but not sufficient; upstream CI and his review
are not our backstop. **The feed is HELD** (user, 2026-09-05): no PRs to CCCP until #283 merges; do not open, re-cut or
propose upstream PRs. `pr/*` branches, when the feed reopens, are cut from `upstream/development`, never from the local
stack.

**His directions (2026-05-25 review):** (1) Controller-sync MP architecture, never deterministic-AI — each machine runs
its own AI with async pathfinding; only Controllers cross the network; (2) FPU + libm standardization for
cross-platform determinism, never expanded fixed-point (`/fp:precise`, `-ffp-contract=off`, deterministic polys at the
primitive level); (3) smaller, focused, per-concern PRs and commits — one purpose per PR statable in one sentence,
~300 lines target (~600 for unavoidable engine changes), stacked series over mega-bundles, a "fix + nearby refactor +
unrelated cleanup" change is three PRs.

**Architecturally rejected (never bring forward, never re-litigate):** M3 Q40.24 fixed-point math; the pathfinder
serial-epilogue (M4A); the Race A consumption-side pathfinder serialization; the `detmath` vendor / FP-env-as-baseline /
scoped-libm; M5.5 mega-bundling; multi-platform race-investigation iteration commits (only the final fixes come
forward as clean commits). Do not re-apply the reverted off-wire fixes (SpatialPartitionGrid MO-query sort,
controller release-timer sim-time, alarm-event sort, PathFinder node sort, Lua AI-aim `^`). Record: ADR-019 and the
wiki decision log `cccp\modernization-docs\decisions.html`.

**Code comments — Causeless's style, on every commit:** short, almost always one line; brief present-tense block
labels are idiomatic (`// Run seeing rays for all actors`); non-obvious WHY stated plainly; TODOs name the constraint
in one line; no fix-narration ("this used to do X"), no milestone or ticket tags in code; `///` Doxygen on header
declarations; delete the stale workaround comment when you fix its root cause. If Causeless wouldn't have written it,
delete it — over-commenting is a credibility tax on the whole fork.

## 6. Cross-platform determinism — the working approach

The lean fixes are what work: the keep-set (portable mt19937-state hash, explicit RNG-distribution mapping, Allegro
integer `fixmul`/`fixdiv`), the deterministic timestep config-lock (a per-machine `Settings.ini` timestep is poison),
controller sim-time (`IsPastSimMS`, never wall-clock in a sim tick), a deterministic transcendental poly routed at the
PRIMITIVE level (`DeterministicSinCos`, `DeterministicAtan2`; never a heavy libm vendor), and the eval-order fix:
C++ argument evaluation order is unsequenced, so `Vector(-RandomNum(x), -RandomNum(y))` draws in opposite order on
MSVC/GCC versus Apple clang — sequence RNG-consuming calls into named locals. No off-wire carve-outs: the foundation
is bit-identical for every subsystem, every tick. Pin residuals by MEASUREMENT (per-tick hashes, raw dumps,
byte-diffs across platforms), never by inspection. Anything a render-frame path writes that sim code later reads is a
desync channel; per-machine settings that steer sim decisions are pinned or identity-gated. Agent topology: the Mac
Mini (arm64) is the cross-arch reference and a worker host; the WSL2 leg on this PC contends with Windows builds and
is stale since 2026-07-10 (re-sync before trusting).

## 7. Branches and worktrees

- **Stage convention:** Stage 0 `modernization-effort` (frozen legacy reference; the fork's default branch; the wiki
  lives on it); Stage 1 `stage/cumulative-verify` → `determinism-revamp` (the determinism foundation, done, shipped as
  the PR 3-10 feed); Stage 2 the stacked chain `stage2/p1-controllerframe-synctest` → `p2d-gns-transport` →
  `p3-controller-lockstep` → `p4a-multiplayer-ui-alpha` → **`stage2/p4b-interp-lockstep`** (the milestone branch,
  worktree `p4b-interp-validation`), with the fix-group candidates `stage2/fixgroup-N` → `stage2/fixgroup-6-lead` and
  the wave scratch `stage2/fixgroup-6-lead-wave-a` on top (RESUME.md §3.2).
- **Naming:** `stage2/<phase-or-lane>` for this work · `flagship/<milestone>` · `feature/<name>` ·
  `experiment/<name>` (throwaway by default) · `fix/<name>` · `stage/<name>` (integration) · `xref/<name>`
  (cross-platform measurement) · `pr/<name>` cut from `upstream/development`.
- **Worktrees:** one branch per worktree; all share the object store `D:\Projects\cccp\.git`; cut a new branch from
  the active tip and `git -C D:\Projects\cccp worktree add D:\Projects\<dir> <branch>`; never work in `cccp` itself
  for engine changes; lanes reuse existing (firewall-ruled) worktree directories; removal only after the reparse-point
  check (§1.5), and the branch stays on origin. Keep all remote branches (no pruning): since the worktree deletions
  GitHub is the only copy of many. The reflog keeps deleted local refs for ~30 days.
- **Push policy:** work branches are pushed at checkpoints and as backups (§1.1); only a genuine throwaway experiment
  stays local. CI on the fork runs only on pushes to `development` and on PRs.
- `development` (local) is a clean mirror of `upstream/development` — never commit to it. ADR-008: fork-first. The
  wiki (`cccp\modernization-docs\`) is the planning artifact; ADRs are append-only.

## 8. Build and test commands

```powershell
$env:GNS_ROOT     = "D:\Projects\stage2_p2\gns_spike\install-win-vcpkg-release"
$env:GNS_DEP_ROOT = "D:\Projects\stage2_p2\gns_spike\build-win-vcpkg-release\vcpkg_installed\x64-windows"
$env:CL           = "/MP6"     # worker lanes; the lead's candidate/family build uses /MP12 and runs ALONE
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"   # LITERAL path
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
& $msbuild "D:\Projects\<tree>\RTEA.sln" /t:RTEA /p:Configuration="Final" /p:Platform=x64 /m /nologo /v:minimal
```

Configurations: `Debug Full | Debug Minimal | Debug Release | Final` (there is NO plain "Release"; "Final ASan" exists
for the ASan hunts). `/t:RTEA` incremental, `/t:RTEA:Rebuild` clean; a `:Build` target does not exist. Gates in order:
`python <tree>/tools/run_selftests.py --repo <tree> --out <dir> --timeout 300` (11/11), verify6
(`grok-workers/lead-tools/verify_lead6b.py`, 8/8), ONE battery (the current brief under `grok-workers/`), the family
(`reviews/recovery-2026-09-07/contract-audit/run_family.py` on the approved tree with nothing else running). Every
engine launch through the runners (§1.4). Running the game by hand is for a human at the desktop only: it is
GUI-subsystem, so PIPE its output (`2>&1 | Out-Host`); a `>` redirect detaches it. Kill prior instances with
`Get-Process | Where-Object { $_.ProcessName -like "Cortex Command*" } | Stop-Process -Force`. On this machine use
`python` (not `python3`), `pwsh -NoProfile`, never `taskkill`/`tasklist` in Git Bash (MSYS2 mangles `/flags`) — kill
by PID from PowerShell; write scripts containing backslashes with the Write tool, not a Bash heredoc.
