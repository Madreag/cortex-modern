# CORTEX-MODERN — RESUME (the single live doc)

**Rewritten 2026-09-12 16:40 MST.** This file is the resume point: read it first, every session.
Older detail: `_archive/docs_archive_20260912/RESUME_pre-rewrite-20260912.md` (the verbatim pre-rewrite copy — nothing
was lost, only compressed); `HANDOFF*.md` and the 15 phase plans in `_archive/docs_archive_20260905/`.
Policy long-form lives in `D:\Projects\CLAUDE.md` (== `AGENTS.md`); the live board is `D:\Projects\STATUS.md`;
the live order of work is `D:\Projects\LEAD_PLAN.md`. This file does not repeat them — it points at them.

Sections: §0 resume in 10 minutes · §1 rules · §2 goal + the eight items · §3 what happened 09-11 → 09-12 ·
§4 current state · §5 what is left · §6 lessons learned · §7 path index · appendices A-F (project, architecture,
build/verify, quirks, history, upstream).

**HANDOFF NOTE (the lead, 2026-09-12 16:55 MST).** The user ended the overnight run at ~16:20 MST ("wrap up; the
lead's weekly Fable budget is at 5%; use Opus 5 for help; no new effort; fix what the reviews found"). What was done
in the wrap-up: wave A finished on the scratch branch with the three lead fix-ups and W71-4 (§4.2); every local
`stage2/*` and `xref/*` branch backup-pushed to origin (89 branches, 16:29 MST, lease-protected, none rejected);
W136 read and REJECTED with a precise re-spin (§3.6); the lead's merge tools copied into `grok-workers/lead-tools/`;
three memory notes for the next session; this file rewritten by the lead (a first draft was delegated and the user
rejected that — **the handoff document, review verdicts and plans are the lead's own work, never delegated**).
**CLEAN STOP at 2026-09-12 17:30 MST (user's instruction: nothing running).** Every agent, lane, build, engine and
queue runner was stopped and checked: no `cl.exe`/`link.exe`/`Cortex Command.exe`/`cursor-agent`/runner process on this
PC, no launchd lane on the Mac, the Devin queue runner (`devin_queue.py`) killed — **there is no Grok, SWE-2, Devin
or Mac session to resume.** Every branch with work is on origin (§4.5). The next lead starts at §5.1 and spawns its
own agents. Two things were cut short by the stop and are recorded as such: the independent verifier pass on W136-2
(the lead read the codec diff itself; the merge is on the wave, §4.3) and the promotion of `control-build` (still at
`5c2c65c2ed`; the gates already run on the wave exe are in §4.1).

---

# §0. RESUME IN 10 MINUTES

## 0.1 Read order (30 min of reading buys a day)

1. This file, §0-§5.
2. `D:\Projects\STATUS.md` — the board: one line per item 1-8 with its %, the delta since the last ask, what runs now.
3. `D:\Projects\LEAD_PLAN.md` — waves, worker routes, acceptance rules, the headed §11 review script (Appendix A).
4. `D:\Projects\reviews\takeover-20260909\LEAD-REVIEW-overnight.md` — the lead's line-by-line review log: per-file
   verdicts, the findings register F1-F20, every lane acceptance, every NEGATIVE. 1205 lines; the last 400 are today.
5. `D:\Projects\reviews\takeover-20260909\grok-workers\SPAWN_LOG.md` — every lane spawn and landing with its model
   evidence line and report path. The last ~150 rows are 2026-09-11/12.
6. `D:\Projects\CLAUDE.md` — binding policy (delegation, firewall, junctions, scratch, commits, mod compatibility).
7. Only if you need it: `STAGE2_H4_RECONNECT_PLAN.md` (H4 design + pins P1-P32), `RECOMMENDED.md`,
   `NEXTENHANCEMENTS.md`, `reviews\takeover-20260909\grok-workers\WORKER_RULES.md` (travels with every brief).

## 0.2 The trees (verified 2026-09-12 16:30 MST)

| Path | Branch @ tip | What it is |
|---|---|---|
| `D:\Projects\control-build` | `stage2/fixgroup-6-lead` @ `5c2c65c2ed` | **LEAD CANDIDATE.** Built exe `98f73d74aaed` (from `7d3666aa15`, 14:52 MST). The FG6B battery pins it. Dirty only in the vendor `allegro-release.lib`. |
| `D:\Projects\takeover-build` | `stage2/fixgroup-6-lead-wave-a` @ `9484ff6106` | **WAVE SCRATCH.** Wave A prepared here while the battery pinned control-build: eleven merges + three lead fix-ups over `5c2c65c2ed`. Uncommitted trim in `Actor.cpp/.h` + `NetLockstepSelfTest.cpp` (§4.4). A build was linking here at 16:30 MST. |
| `D:\Projects\p4b-interp-validation` | `stage2/p4b-interp-lockstep` @ `f83cc75099` | **THE MILESTONE BRANCH / APPROVED TREE.** The only executable the two-process harness and the verification family launch (exe `ff6a44ac46c8`, 2026-09-11 16:44 MST). `origin` matches. Workers may never write here. Docs say "main" — there is no `main` ref; this branch is it. |
| `D:\Projects\cccp` | `modernization-effort` @ `0064b7e542` | Legacy reference **and the shared object store** for every worktree. |

Worker worktrees (one branch each, reused across lanes so the firewall rules keep covering the path):

| Worktree | Branch @ tip | Lane |
|---|---|---|
| `alias-walk` | `stage2/coroutine-stack-fit` @ `7a689008fb` | W136 (rejected) + W136-2 (F20 fix) — merged on the wave; verifier pass still owed (§4.3) |
| `item4-simspeed` | `stage2/preview-substitute-links` @ `03a7add7be` | W133 + W133-2 (F15) — merged on the wave; F15b/F15c debt (§3.4) |
| `item5-lifecycle` | `stage2/cross-actor-waypoints` @ `b0e33ba3c7` | W71-3 + W71-4 (F10) — merged on the wave |
| `value-observations` | `stage2/preview-reference-writes` @ `f1fb71f401` | W127 (F8) — merged |
| `h4-secondary` | `stage2/sound-identity-pin` @ `f605a8330a` | W131 (F11) — merged |
| `hold-pause` | `stage2/relaunch-slots-fg6b` @ `49b7249b78` | W124-2 (F13) — merged |
| `fencing-warm` | `stage2/stop-leave-piemenu-pin` @ `9a37f3a246` | W135 (F17+F18) — merged |
| `item7-chat` | `stage2/tools-oracle-fixes` @ `0bbfaedb10` | W134 (F19 tools) — merged |
| `item4-feel` | `stage2/preview-scripts` @ `6ae2d3c6f4` | W129 (preview sound scope + glow oracle) — merged |
| `item7-ux` | `stage2/port-map-toggle` @ `d3fd36c60c` | W125 (SWE-2 lobby toggle) — merged |
| `item8-directory` | `stage2/s4-wiring` @ `86b4f82d6c` | W123 (ICE/mux wiring) — merged |
| `item8-dedicated` | `stage2/preview-projectiles` @ `3b8035ddc9` | W104b — merged |
| `item8-discovery` | `stage2/audit-3-harness` @ `9027d7e874` | audit-3 harness — merged |
| `audio-owner-registry` | `stage2/join-list-mod-label` @ `c3ffc8bd1d` | W94-2 — merged |
| `item3-fixtures`, `lane-a`, `lane-b` | `stage2/e2e-harness-flags` `ae1b48b562`, `stage2/rematch-roster-2-mac` `1dae3de1c5`, `stage2/item3b-harness` `1836ffb80b` | merged/parked |
| `fixgroup-1..4`, `takeover-fixes`, `xarch-combat` | `stage2/fixgroup-1` `7050cb259d` … `stage2/fixgroup-4` `bf2301a075`, `stage2/a7-journal-wip` `fa24014635`, `stage2/p4a-multiplayer-ui-alpha` `f072d35dad` | superseded integration trees; A7 journal WIP parked; P4A reference |

## 0.3 Build (Windows, primary)

```powershell
$env:GNS_ROOT     = "D:\Projects\stage2_p2\gns_spike\install-win-vcpkg-release"
$env:GNS_DEP_ROOT = "D:\Projects\stage2_p2\gns_spike\build-win-vcpkg-release\vcpkg_installed\x64-windows"
$env:CL           = "/MP6"     # worker lanes; the lead's own candidate build uses /MP12 and runs ALONE
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"   # LITERAL path
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
& $msbuild "D:\Projects\control-build\RTEA.sln" /t:RTEA /p:Configuration="Final" /p:Platform=x64 /m /nologo /v:minimal
```

Configs: `Debug Full | Debug Minimal | Debug Release | Final`. **No plain "Release".** `/t:RTEA` incremental,
`/t:RTEA:Rebuild` clean (needed when luabind exports change, e.g. `get_main_thread`); a `:Build` target does not exist.
Never rebuild while an engine of that tree runs (LNK1104) or while a battery pins its exe.

## 0.4 Verify (in the order you run them)

| Gate | Command | Bar |
|---|---|---|
| Socket-free suite | `python <tree>/tools/run_selftests.py --repo <tree> --out <dir> --timeout 300` | `"passed": 11` — controller-frame, net-protocol, net-identity, net-session, net-lockstep, net-match, net-auth, net-admission, net-reconnect, net-reconnect-session, camera-null-scene. Exit 0 with no PASS token is FAIL. |
| **verify6** (quick candidate set) | `python reviews/takeover-20260909/grok-workers/lead-tools/verify_lead6b.py` (edit `REPO`/`OUT` at the top) | 8/8: the suite, `-net-port-map-selftest`, `-net-directory-selftest`, `-script-graph-selftest -num-lua-states 4`, `tools/test_global_callbacks.py`, preview-event-D7, lpinv-100, W102 arm-1 switch-control e2e. |
| ONE battery | the `fg6b-battery` brief re-run on the new tip through a Grok run-only lane; scratch `D:\mx\fg6bat<N>`, lane-unique ports (47651-47656 / 8457). Brief + drivers: `reviews/takeover-20260909/grok-workers/fg6b-battery/` | every engine gate green or a lead-classified non-engine artefact; ~20 steps (selftests, H4 gates, B1 gates, lobby, heal e2e, gameplay fixtures, dedicated, lifecycle packs, drop3_switch, preview_event_d7, lpinv_100, portmap_e2e, controller_log). |
| The family (acceptance for a changed approved tree) | from `reviews/recovery-2026-09-07/contract-audit`: `python run_family.py --source 44 --breadth-out D:\mx\s44b1 --matrix-out D:\mx\s44-1 --mac-attempt 1` (the `family-source44.json` must not exist; the Mac attempt dir must be new) | chain + remaining + breadth 81/81 accounted + matrix 106 reviewed + the Mac attempt green. Runs ONLY on `p4b-interp-validation`, with no MSBuild, engine or worker lane active. |
| Every engine launch | `python <tree>/tools/run_sim_test.py --repo <tree> --out <dir> --timeout <s> -- <engine flags>` (or `make_run` from Python, `tools/isolated_launch.py` from PowerShell) | never the executable directly — a direct launch puts a window on the user's desktop. |

## 0.5 Spawning workers (routes are facts; the model gate is not optional)

| Route | Command | Gate / evidence |
|---|---|---|
| Grok 4.6 Extra High Fast, Windows | `python reviews/takeover-20260909/grok-workers/win_cli/win_job.py <name> <worktree> <prompt.txt> --task "..." [--timeout SEC]` | first stream-json line must contain `Extra High Fast`; `cli_runs/<name>/agent-output.jsonl:1`. Exit 4 = REFUSED. The runner appends the SPAWN_LOG row itself. |
| Grok 4.6 Extra High Fast, Mac | `python .../mac_cli/mac_job.py <lane> <script.zsh> [--timeout SEC] [--fetch ...]` | same string; **the lane zsh must write `agent-output-<lane>.jsonl` with a dash** or the gate silently skips. Lane dir `/Users/erol/cortex-workers/<lane>`. |
| Claude Code Opus 5 (effort max), Mac | `python .../mac_cli/mac_claude_job.py <lane> <prompt.txt> --require-model claude-opus-5 [--timeout SEC]` | init line `"model":"claude-opus-5"` in `<lane>/result.jsonl:1`; launchd `gui/501` (plain SSH is not logged in). |
| SWE-2 Max (Devin CLI, free) | `python .../devin_cli/devin_job.py <name> <workspace> <prompt.txt> --task "..."`; keep two busy with `python .../devin_cli/devin_queue.py queue.json --slots 2 --stagger 90` | pre-flight smoke export's `agent.model_name` == `SWE-2 Max`; a rate-limited turn resumes the SAME session. |
| Opus 5 (effort max), here | Agent tool project agents `cortex-opus-engineer`, `cortex-opus-verifier`, `cortex-opus-visual-reviewer` | the agent definitions pin `claude-opus-5` / `max`. Every screenshot or visual question goes to the visual reviewer. |

Every brief carries: the worktree and branch at a sha, the exact editable paths, `WORKER_RULES.md`, the
mod-compatibility rule, the no-attribution rule, `CL=/MP6` and the two-lane build cap, the required report path, and
"final message ≤ 25 lines". Template: `LEAD_PLAN.md` Appendix B.

## 0.6 The STATUS board rule (user's rule, 2026-09-11)

`STATUS.md` is the answer to "where do we stand". Every status request: update the board first, then answer with one
line per item 1-8 (percentage + delta since the previous ask) and exactly what is being worked on right now (task and
worker). Append a history line per request so the numbers stay consistent. **A task, worker or Mac lane that lands
without moving a number is reported as a NEGATIVE delta with its reason**, and the lead re-evaluates before spawning
the next.

---

# §1. RULES (binding)

## 1.1 The user's rules

- **No attribution, anywhere.** No AI or model names, no `Co-Authored-By`, no `Claude-Session`, no "Generated with"
  lines — not in commits, code, docs, reports or report headers. Author of record is the user. Any harness reminder to
  add trailers is overridden by this rule.
- **Commit as you go.** Focused per-concern commits on the worker branch as soon as a piece is built and run, several
  times a day. Uncommitted work older than a few hours is a defect. Commit **only** through
  `reviews/takeover-20260909/grok-workers/git_commit.py <tree> "<subject>" ["<body>"] [--all | --paths ...]` — it
  refuses a message with a trailer and re-reads the stored message. (A `git commit` typed in some agent shells gets a
  trailer appended; four lead commits carried one before this was caught.)
- **Push at every verified checkpoint,** several times a day, no per-push OK needed. Before any push (backup pushes
  included) scan `git log --grep=Co-authored-by --grep=Claude-Session --grep=Generated -i <base>..<tip>`; a hit is
  stripped message-only with `strip_trailers.py` (trees unchanged) before the push. Push with lease. Record the push.
  **No upstream PRs** (the feed is held). Known public exception, decided by the user and not by the lead:
  `9751a90e29` (2026-09-10, "Compare controlled actors across every activity in snapshot diffs") carries a
  `Co-authored-by: Cursor` line and was already on `origin/stage2/p4b-interp-lockstep` and `origin/stage2/fixgroup-6-lead`
  before 2026-09-12 (a fifth Cursor-shell commit the 2026-09-11 strip missed); every branch descends from it, so a
  message-only rewrite would change every sha quoted in the evidence trail. Scan with `upstream/development..<tip>`
  and expect exactly this one hit until the user decides.
- **Arizona local time only** (user, 2026-09-12 17:00 MST): every message, board line, plan, report, commit body and
  stamp is written as `2026-09-12 17:05 MST` (MST, UTC-7, no daylight saving); never UTC. Read the clock (`date`) in
  the turn that writes the stamp. Evidence logs written before this rule (SPAWN_LOG.md, LEAD-REVIEW-overnight.md, lane
  reports) keep their UTC stamps — subtract 7 hours; machine logs stay as the tools write them. This file, STATUS.md
  and CLAUDE.md were converted on 2026-09-12 17:03 MST (pre-conversion copies in `_archive/docs_archive_20260912/`).
- **The lead writes the thinking documents itself** (user, 2026-09-12 16:45 MST): RESUME.md, handoffs, plans, review
  verdicts and status boards are never delegated to a subagent, whatever the budget. Subagents get bounded engineering
  and verification with an exact spec; the lead reads and re-derives their output.
- **Inventory-first, never whack-a-mole.** The whole battery runs ONCE on a changed tree; every failure is root-caused
  in parallel (one read-only worker per failure); the fixes land as ONE group; then ONE battery.
- **The lead verifies personally.** For every failure: that it is a TRUE failure (the engine did the wrong thing, per
  evidence the lead looked at — not an oracle reading a wrong path, a fixture ending the game, a timing artefact or a
  worker's guess). For every fix: that it is WELL IMPLEMENTED (the lead reads the whole diff; the detecting test is red
  on the exact defect and green after, re-run by the lead; the named mechanism is what the code changes; nothing
  masked, widened or patched around). A worker's "PASS" / "green" / "root cause found" is a claim to verify.
- **Verification = BUILT and RUN** on the claimed configuration. "Covered by logic" / "should be fine" / "X implies Y"
  is not a result. Partial coverage is a checkpoint, never "achieved".
- **Never widen a comparison mask, tolerance or exclusion; never serialise local AI to get a pass; never patch a mod,
  fixture or test to hide an engine regression.** Every comparison exclusion needs field-specific evidence and an
  independent review. Report EVERY oracle/checker/driver change between a failing and a passing run, with both runs.
- **Workers never push, never touch `main`/the approved tree, never launch the engine outside the runners.** A worker
  may commit on its own branch. The lead reads every diff line before any merge.
- **Build cap: two Windows MSBuild lanes at `CL=/MP6`; none while the lead's candidate/family build runs (`/MP12`,
  alone).** Judge the cap on `cl.exe`/`link.exe` from other working directories — idle MSBuild node-reuse processes
  linger for an hour and mean nothing.
- **Nothing reaches the user's desktop from unattended work**: no game window, focus change, sound or firewall prompt.
  `CCCP_HEADLESS=1` in every worker environment; `*-selftest` flags imply headless in the engine; no GUI tool of any
  kind in a lane (no WinDbg, browser, editor window, headed game).
- **Firewall.** Windows prompts for any executable path with no per-program allow rule the first time it binds a socket
  (a GNS client binds an ephemeral UDP port, so a port-range rule cannot cover it). The moment a worktree or control
  build is created, the lead runs `grok-workers/firewall_allow_all_exes.ps1` **elevated**;
  `tools/win32_test_runner.py` refuses a `-net*` launch whose exe has no inbound rule (`firewall_allow_rule_present`).
  Workers cannot add rules: they stop and report. Until the next elevated run, NEW lanes reuse an existing worktree
  directory with a fresh branch, never a new directory.
- **`D:\mx` junction rule.** Every run root carries junctions into engine trees. Never move, copy, rename or remove a
  directory under `D:\mx` or any run root (`robocopy /MOVE`, `Move-Item`, `shutil.move`, `rmtree` all follow junctions
  and have emptied the repository's `Data` twice). `git worktree remove` follows junctions too: list and unlink every
  reparse point first. A lane out of disk stops and reports; only the lead frees space.
- **Scratch footprint.** A lane keeps under its run root only what its verdict needs: at most one control build besides
  the tip, no extra worktrees or tree copies, only the final run's restoration/fuzz outputs. Over 5 GB (12 GB on the
  Mac): stop and say so.
- **Mac.** Lanes under `/Users/erol/cortex-workers/<lane>`; `/Users/erol/Documents/Codex/` is retained evidence and
  read-only. Every CLI run is a launchd job in the logged-in GUI session (`gui/501`) — a plain SSH session has no
  unlocked keychain. Evidence comes back with `tar` over ssh excluding `runtime` (scp -r follows the `Data` symlink).
- **Merges.** Keep-both conflict resolutions edit the conflicted **working file** (never a stage copy — auto-merged
  hunks live only there), then every merge commit is checked with
  `grok-workers/lead-tools/verify_merge_commit.py <tree> <merge> <other>`: per file, the +/- line multiset of
  `merge^1..merge` must equal `merge-base..other`, 0 differing files.
- **Any lane that changes a wire or version constant runs `tools/run_selftests.py` 11/11, and the lead's read checks
  that it did** (rule added 2026-09-12 14:56 MST after W126's gap became F14).
- **Cleanup** is authorised only for items the lead has personally verified as redundant; log every deletion under
  `reviews/`; never touch live trees, a running family's evidence or a pinned executable.
- Momentum over ceremony; honest pushback welcome; do big writing tasks yourself; never ask the user to run what you
  can run.

## 1.2 Causeless's directions (upstream lead — architecture and conduct)

1. **Controller-sync MP, never deterministic-AI.** AI runs per-machine off-wire (async pathfinding allowed); only each
   actor's Controller crosses the wire. Discrete player actions cross as owner-issued, tick-stamped game commands whose
   RESULT must be bit-identical.
2. **FPU + libm standardization, never fixed-point.** Deterministic trig/exp polys in `RTETools.h`, `/fp:precise`,
   `-ffp-contract=off`. Q40.24 is rejected and not re-litigated.
3. **Small, focused, per-concern PRs and commits.** One sentence of purpose; ~300 lines target, ~600 for unavoidable
   engine changes; stacked series over mega-bundles.
4. **Terse comments.** One short line, almost never two; plain block labels; non-obvious WHY only; no fix-narration; no
   milestone or ticket tags in code; `///` Doxygen on header declarations; delete the stale workaround comment when you
   fix its root cause. If Causeless wouldn't write it, delete it.
5. **Never send a broken PR.** Every cumulative state a one-at-a-time merge creates is built and booted before
   submission; upstream CI and his review are not our backstop.

## 1.3 Mod compatibility (binding; ADR-004 steering, `reviews/claude-review-2026-09-08/MOD_COMPATIBILITY_STEERING_PROMPT.md`)

Existing mods keep working unchanged. Breaking Lua or native API behaviour to solve determinism, checkpointing,
ownership or copy-on-write problems is **not authorized** — fix the engine instead. Return-by-reference properties stay
live aliases (`sound.Pos.X = value`, `local p = sound.Pos; p.X = value`); Vector operations, native argument
conversions, identity/alias, ownership and garbage-collection behaviour stay as they were, through local/shared audio
scopes, copy-on-write transitions, ownership transfer and restored object graphs. Proof = **unchanged** Lua fixtures
run on a retained pre-change reference executable, on the broken build and on the repaired build, and the fixtures must
detect the break. Matching host/client hashes cannot prove compatibility when both peers run the same broken semantics;
the `lua_state` tick hash is not an adequate oracle. No opt-in compatibility flags. Escalate a genuine
mod-compatibility break to the user as a product decision; never decide it yourself.

---

# §2. GOAL AND THE EIGHT ITEMS

**End goal (the user's words, binding):** perfect-feel multiplayer at **100-200 ms ping** — no lag, no warping, clean
prediction. **Flawless normal MP + UX (join / leave / rematch / reconnect) comes strictly first.** Measurements pick
the endgame technique (bounded rollback and/or prediction), not *whether* it is pursued. The assignment is the whole
live roadmap, not a milestone.

Percentages are from `STATUS.md` at 2026-09-12 16:10 MST; overall **78%** (mean 77.5). They are the lead's judgement
against named evidence, not a burn-down.

| # | Item | % | "Done" means |
|---|---|---|---|
| 1 | Controller boundary + wire/replay compatibility | 98 | ControllerFrame v6 / replay v3 / controller-log v2 stable, old peers rejected at hello, legacy recordings replay bit-identical, and the Source44 family green on the promoted tree. Moves only with the family. |
| 2 | Restore / identity / state inventory | 98 | Restoration + native + identity gates green in the family on Windows AND a clean arm64 clone; the state inventory complete (no undeclared transient); snapshot/restore faithful under every scope. |
| 3 | Gameplay fixtures + minimizer + matrix | 96 | 3b door/crab/craft under one script, the UI path of an AI order, the seat-side pie close; all gameplay fixtures cross-peer identical with **no exclusions**; 3e (the actor-switch ownership product call) decided with the user. |
| 4 | Presentation + performance (100-200 ms feel) | 40 | 4a numbers (pinned 2026-09-06, §C.4 below) met or reported as honest misses; 4b preview-event ledger + optimistic projectiles + Activity UI following the preview; 4c headed two-window measurements at ~100/200 ms and two render rates; 4d sim-speed program and the rollback verdict from measurements. |
| 5 | Breadth + verification debt | 97 | 5b 3/4-peer packs, co-op, PvPvE and the full leave/drop/rejoin/resync/replay/rematch/pause lifecycle on Windows and the Mac; 5c the WSL2 leg (never yet run) in a window with no Windows builds. |
| 6 | H4 reconnect + host moderation | 95 | Phase A §9a gates all green, Phase B substitution (B1) + the moderation GUI (B2), and the **headed §11 reconnect review with the user at the desktop** (LEAD_PLAN Appendix A). |
| 7 | Lobby / session UX + robustness | 38 | 7a chat (wire exists, no UI/routing yet) · 7b overlay/toasts/delay display · 7c mod-mismatch UX · 7d compression, beacon, delta frames, address re-resolve · 7e replayable post-resync rounds, periodic snapshots, telemetry bundle, crash-rejoin prompt · 7f replay browser + post-match report. |
| 8 | Discovery / internet play + roadmap | 58 | 8b dedicated headless host (the persistent server/world on the Mac — the end game) · 8a session directory + NAT rendezvous self-hosted on the Mac (preferred) or this PC, with a **real ICE connect proven end to end** · 8c lobby v2 · 8d adaptive delay, audio smoothing, bounded rollback per the item-4 measurements. |

Grouped: flawless normal MP (1, 2, 3, 5, 6) ≈ 96.8%; measured feel (4) 40%; UX + discovery (7, 8) ≈ 48%.
Estimate last given (2026-09-12 01:33 MST): 5-8 days of sessions like this session for the whole roadmap.

**Hosting decision (user, 2026-09-11 13:45 MST, settled — do not re-propose):** players self-host; anyone can host and
anyone can join. The session directory and the NAT rendezvous run on the Mac Mini (preferred) or this PC, never a
third-party service. The same Mac runs `-net-dedicated` as a persistent server/world once direct connection is solid.
Order inside item 8: dedicated host → directory + NAT → lobby v2.

**Reclaim hold = PAUSE THE MATCH (user, 2026-09-10 22:55 MST, settled):** while a dropped player's seat is held, the
simulation does not advance for anyone; reclaim or substitution resumes from the held frame; hold expiry drops the seat
and resumes with the ledgered consequences. Announced (clean) leaves still close the seat immediately with no pause.

---

# §3. WHAT HAPPENED, 2026-09-11 → 2026-09-12

## 3.1 The takeover

The Cursor lead burned 88% of the Cursor budget on orchestration while Grok had 77% left and only one board item moved
in a five-hour window. On 2026-09-11 14:20 MST the user moved the lead to **Claude Code (Fable 5.1, effort max)** with
four delegate routes (§0.5) and two binding corrections: inventory-first, and the lead personally verifies every
failure and every fix. A second correction landed the same evening: **every Grok worker spawned through the Cursor
Task tool ran at HIGH FAST, not Extra High Fast** (including W36/W38, created with the `cursor-grok-4.6-xhigh-fast`
slug and labelled "High Fast" in the UI). The Task tool is never used for Grok work again; the CLI is, and its init
line is the only accepted proof. Wave 0 that night: firewall rules for 126 executable paths, a trailer strip of 15
branches (message-only, trees byte-identical, `trailer-strip-20260911/map.txt`), and smoke-verified runners for
SWE-2 and Claude-on-Mac.

## 3.2 The fix groups and the promotion chain

`fixgroup-1` (`7050cb259d`) → `fixgroup-2` → `fixgroup-3` (`0e7e5b6094`) → `fixgroup-4` (`bf2301a075`) →
`fixgroup-5` (`fb4c6e9053`) → **wave 1 promoted to the milestone `f83cc75099`** (commit dated 2026-09-12 08:14 MST,
pushed with lease, trailer scan of the new range empty) → `fixgroup-6` (`8c7d5b75c6`) → **`stage2/fixgroup-6-lead`** (the lead candidate, in
`control-build`) → **`stage2/fixgroup-6-lead-wave-a`** (the scratch wave, in `takeover-build`).

The Source43 family on the milestone ended NOT GREEN with five reds root-caused as one group (R1 stale `Actor*` in the
contiguous index → W105; R2 borrowed owner-ref restore → W106/W106-2; R3 a Mac oracle artefact; R4 a breadth lane
killed by stray engines; R5 an intermittent host crash after a resync relaunch). All were fixed; R5 became the ASan
hunt that produced W114/W118/W124/W124-2 and, indirectly, F20.

## 3.3 The overnight waves and the lead's own review

At the user's request the lead read the **whole** delta itself, hunk by hunk, and logged every verdict:

- **Overnight delta** `d81478d2ee..f83cc75099` (111 non-merge commits, Source 79 files +11306/-270, tools 10 files
  +2808/-3) — complete 11:59 MST. Verdict: no determinism defect, no mod-compatibility break; findings F1-F6.
- **Part A**, the takeover window `be217add64..d81478d2ee` (103 commits, Source 99 files +19697/-2211) — complete
  14:21 MST. No new finding beyond F10's enqueue half.
- **Part B**, the P4B phase `db9ec184be..be217add64` (542 commits, 394 files, +65419/-2327) — started 15:23 MST.
  Done by 16:10 MST: all of `Source/Network` production code (54 files, 13,685 diff lines), all Managers
  (MovableMan 2,939 lines; LuaMan 5,107; LuaThreadCodec 596; AudioMan, ActivityMan, SceneMan, FrameMan, UInputMan,
  MusicMan, PrimitiveMan, PostProcessMan, LocalPrediction, …), and half of System (ScenarioRunner, Controller, Atom,
  CheckpointArchive, PathFinder, Entity). **Still to read: the rest of System, `Main.cpp`, the network selftests.**
  Five read-only Grok audit lanes (`pb-audit-1..5`) covered Entities, tools, GUI/Menus/Data and second-read the
  network and manager/system hunks; every finding was re-derived by the lead against the **candidate**, not the P4B
  tip, which closed three of them and produced F17, F18, F19.

## 3.4 The findings register F1-F20

| # | Severity | What | Status |
|---|---|---|---|
| F1 | LOW (test gap) | `RunContiguousActorIndexSelfTest`'s orphan case used uid 0, refused by the `<= 0` check before the cohort check ever ran; the R1 shape (a nonzero stale uid) was unexercised | **fixed + merged** `d6a0e0f123` (driver-verified: `refused=1`) |
| F2 | LOW (presentation leak) | `AudioMan::RetirePredictedVoice` cleared `predicted` on a finishing one-shot, so the audio checkpoint captured it as an unowned voice; a peer restoring that snapshot replays it | **fixed + merged** `a731ac1326` with two selftest checks |
| F3 | LATENT (constraint) | the W62 fenced-peer waiver's content safety relies on `frameLane == ControlReliable` (ordered). Any lane switching the frame lane must also erase the waived peer's stored frames ≥ F and drop its later arrivals | **open as a standing constraint** — carried into every frame-lane brief |
| F4 | fixture | the A7 `coop_hand_back` arm ran the armed P4 Alpha Duel (team 0 dead by tick 272 at seed 42), so the reclaim always found the activity Over | **closed** — preset `Net Lifecycle Test` in every driver copy; V-A7-2 PASS on both pinned exes |
| F5 | MEDIUM (item 8) | Windows pinned-certificate mode set only `IGNORE_UNKNOWN_CA`, so the README's own self-signed `CN=cortex-directory` cert reached by IP was refused on Windows while the Mac accepted the pinned leaf | **fixed + merged** (W117 `865a739b45`), red/green through the real HTTPS probe |
| F6 | LATENT (scale) | `GET /v1/sessions` returned every row while the engine refuses a body over 128 KiB — past ~200 sessions the join list shows "unreachable" | **fixed + merged** (W120 `6062f464bd`: limit/cursor/total paging, client follows 5 pages) |
| F7 | LOW (was MEDIUM) | a claimed CPU actor kept wire mode PLAYER after the purge; corrected on W119's evidence — the seeded owner is consulted first, so it was never orphaned, only one tick late | **fixed + merged** (W119 `0d5c318367`: explicit hand-back in AI mode at expiry) |
| F8 | **HIGH** | a preview hook writing through an entity reference held in `self` reached the **canonical** world (lpinv 6/8 at tick 153, `shadows=0`); six vanilla Base.rte scripts hold such references. A desync hazard introduced by W89-2's unfrozen edge hooks | **fixed + merged on the wave** (W127 `f1fb71f401`: remap every MovableObject userdata in the copied self at bind; anything else freezes the clone's hooks) |
| F9 | MEDIUM | the candidate did not build on macOS: `PreviewScriptSelfTest.cpp` was in the vcxproj only, and `NetPortMap.h` built a default argument from a nested struct with default member initializers (clang 17 refuses, MSVC accepts) | **fixed** `229f12826c`; Windows binary byte-identical before/after |
| F10 | MEDIUM | AI-generated queue ops were authorized by **team command authority**, not ownership, at BOTH the apply gate and the enqueue gate; under host-cpu-remote-human the owner's AI output for a CPU actor on a human's team was dropped and its waypoint queue never consumed | **fixed + merged** (W71-3 `435924a207`: `writerUID` on `NetGameAIOrder` codec v21 with v20 decode kept; owner-gated producers; widened apply+enqueue gates. W71-4 `b0e33ba3c7` closes the last gap: keep the path request armed while the add is in flight — read by the lead and ACCEPTED WITH CORRECTIONS, §3.6; **CLOSED 2026-09-12 17:24 MST**: the trimmed arm PASSes in the suite and the craft_cargo pulse on the wave exe is `[316, 317, 318, 319]` on both peers, `D:\mx\wave-b\craft_cargo\pulse_gameplay_wave.txt`) |
| F11 | **HIGH** | the checkpoint **sound identity cursor** is a process-global counter with no pin at match start, while sound identities cross the wire in `NetGameSoundOp`. Two peers whose processes created a different number of containers (a rematch, a prior skirmish, or Windows vs arm64 — measured 9212 vs 9200 at the same tick) hand out different identities for the same container. Every existing two-peer test starts two fresh identical processes, which is why none saw it | **fixed + merged** (W131 `f605a8330a`: pin to `1<<40` beside the uid pin; unit arm 17-identity delta red→green; relaunch order verified) |
| F12 | LOW | a directory probe under a blocked address left an unprunable rate-limit bucket | **fixed + merged** on the W120 branch, red-first, service suite 29/29 |
| F13 | MEDIUM | `m_LockstepRelaunchInProgress` was set at relaunch and cleared only in `ActivityMan::Clear()`, so the end-of-Update rebind re-imposed the snapshot's UIDs on brain / controlled / controller / marked slots **every tick for the rest of the process**; a later legitimate switch was reverted at the end of its tick (and later checkpoints wrote stale ids) | **fixed + merged** (W124-2 `49b7249b78`: `EndLockstepRelaunch` after the first `MovableMan::Update`; dead ChecksLeft code removed). RED `tick 460: owner=2 after hand-back`, GREEN 10/10 |
| F14 | LOW (test pin) | the reclaim transcript golden hard-coded protocol version 1; W126 bumped `c_Version` to 2 | **fixed** `7d3666aa15` (golden derives the version bytes) |
| F14b | LOW | **the F14 fix was a half-fix** — `TestKnownAnswers`' three HMAC vectors were computed over the v1 transcript while `MakeTranscript` read the live version, so `-net-reconnect-selftest` stayed red (FG6B, W124-2, W133, W135 all saw it) | **fixed** `5c2c65c2ed` (known-answer transcripts pin `protocolVersion = 1`; the layout test keeps covering the live version). NEGATIVE against the lead |
| F15 | **HIGH** | the preview-HUD crash: a render substitute's faithful link (`m_pItemInReach`) resolved through the speculating lookup onto a preview clone that `EndSpeculation` frees, read by the HUD on the next frame (ASan heap-use-after-free, `AHuman.cpp:3672`, 10/10 runs) | **fixed + merged, with follow-ups.** W133 `8b3c6590ee` (merged `c13966d02f`) remaps external links to residents; W133-2 (merged `8eaa54d923`, 2026-09-12 17:09 MST) closes the two remaining heads: `6e3698b463` — `EndPreviewScripts` walks recorded (uid, state) bindings (RED ASan UAF at `LuaMan.cpp:7914`, `D:\mx\w133b\asan\red\report.32984`, clean after), `03a7add7be` — `OverlaySurvivorOf` returns nullptr for every retiring overlay object (spawns, spawnMeta keys, added-since-mark), correct by reading only (no red was ever produced for that head). GREEN on the ASan build: HUD 5/5 no report, ledger, lpinv, suite 11/11. **F15b (open, test debt):** the two unit arms the verifier specified — a harvested speculative spawn linked from a clone's `m_pItemInReach`/`m_pMOToNotHit` must be null after `EndSpeculation` (RED on `6e3698b463`, GREEN on `03a7add7be`); an in-world shadow link must map to the resident, not null. **F15c (open, hardening):** `RetiringOverlayObjects` lists top-level objects only, so a link naming a part of a retiring object or a shadow part falls through (latent: `MovableObjectReference` links self-expire and the raw links store roots); `RemapExternalLinks` does not recurse wounds; a stateless clone part that gains a Lua state during the preview keeps its `#preview` slot |
| F16 | LOW-MEDIUM (fidelity) | `RemapPreviewUserdata` tested the live world before the part map, so a self-held reference to the original's equipped gun mapped to a duplicate shadow of the whole original rather than the clone's own part | **fixed by the lead** `74072b3d08` on the wave |
| F17 | MEDIUM (robustness) | on the relay host, a **client's** `ProtocolError`/`InternalError`/`MissingFrameTimeout`/`PeerDisconnected` stop set the coordinator to Failed, ending an N-peer match that the same client's socket merely closing would not (a griefer packet ends everyone's match) | **fixed + merged** (W135 `2b9a8ca1ff`: the stop becomes that peer's `ApplyPeerLeave` at `FirstFrameWithout`, relayed as PeerLeft; the host's own stop still fails clients) |
| F18 | MEDIUM (determinism) | `SubPieMenuHoverOpenDelay` is a per-machine setting driving a **sim** timer inside `Actor::Update`; two peers with different values open the sub-menu on different ticks and a release in that window resolves a different slice on each | **fixed + merged** (W135 `9a37f3a246`: pinned in `ApplyDeterministicConfig` like the gold/crab settings, re-read by `PieMenu::UpdateSliceActivation`, handed back after; 900→1000→900 selftest) |
| F19 | LOW-MEDIUM (oracle validity) | contract-audit fixtures gated on the literal uid `1048577` and `run_audit`'s gate never failed on an empty or mismatching check list — a drifted uid made the whole audit vacuous | **fixed + merged** (W134: fixtures claim `_ContractAuditOwner` on the first Create/Update and print an ARMED line; the gate fails on `fixture_armed`, `<family>_checks` and every named mismatch; unit tests red→12/12) |
| F20 | **HIGH (memory safety)** | a restored coroutine's stack is sized by its **saved top** (`lua_checkstack(co, top + 16)`), but LuaJIT requires `base + framesize <= maxstack` for every live frame (checked at call time, never on resume). A yielded frame with many locals is restored into a stack that cannot hold it, and the first register write past the allocation corrupts the heap. Every script-graph restore that rebuilds a suspended coroutine is exposed: relaunch, resync heal, rollback probe, preview codec paths | **W136 `b380ac89bb` REJECTED (2026-09-12 16:48 MST), W136-2 re-spin in progress.** The RED/GREEN evidence is genuine (RED `D:\mx\w136\sg-red4\stdout.log:537` `FAIL coroutine_big_frame_stack_fits 285/131` then a 0xC0000005; GREEN `sg-green2` 177 PASS, suite 11/11), but the sizing walk only sizes frame links that carry `pcslot` and skips the `ftsz` links (FRAME_CP — every coroutine's own main function —, FRAME_VARG, FRAME_CONT, FRAME_PCALL), so a big frame BELOW the yield is never sized; the safety net gates on `frame_islua` and skips the same frames; the `_ScriptGraphThreadStackFits` oracle mirrors the formula instead of the invariant. The lead confirmed on the source (`LuaThreadCodec.cpp` ~345-362 and ~482). W136-2 (Opus engineer, same branch): a second RED-first arm with the big frame in the main function yielding inside a helper/metamethod, then size EVERY link from the function in its own slot (`slots[i-1]`, `needed = max(needed, i + 1 + framesize)`), drop the `frame_islua` gate, make the query check the invariant. Evidence of the defect: `D:\mx\w124b\asan\report.52732` |

## 3.5 Every NEGATIVE of the window, with its lesson

| NEGATIVE | Lesson now in force |
|---|---|
| **W130 accepted on "sound by construction, unproven"** — its 5-line MOID-join fix was not the crash mechanism at all; W132's ASan measurement proved the pre-fix and post-fix trees crash identically (5/5 and 5/5) | When you cannot tie the faulting line to the fix **by reading**, do not accept it — measure first. W130 stays merged as a correct ordering change; the crash became F15. |
| **W126 accepted without the full suite** — its lane ran seven selftests, not `-net-reconnect-selftest`, and the lead did not ask | A lane that touches a wire or version constant runs `tools/run_selftests.py` **11/11**, and the lead's read checks that it did. |
| **F14 half-fixed by the lead** — the golden was derived from `c_Version` but the three HMAC known answers were not; the lead rebuilt and did not re-run the reconnect selftest on the rebuilt exe | The 11/11 rule binds the lead too. Re-run the detecting test **on the rebuilt binary**, not on the reasoning. |
| **The lead's first W131 keep-both resolution** rebuilt the file from the pre-merge stage `:2:` and silently dropped two hunks git had already auto-merged (a Run-list entry and an include). It would have compiled with the new arm never running | Keep-both edits the conflicted **working file**. `verify_merge_commit.py` after every merge commit; it caught this one. |
| **FG6-BATTERY's last five steps waited 65 minutes** for an engine-idle window that W124's continuous e2e loop never gave, then the runner died; the battery was stopped as superseded | The wait-for-idle rule needs a **lock the loops honour** (`D:\mx\LEAD_BATTERY.lock` exists; the loops must take it). Do not schedule an idle-gated battery beside a continuous e2e lane. |
| **Two orphan MSBuild node-reuse processes** (no parent, no children) were counted as active builds by the lanes' slot waits and stalled W120/W130 after their reports | Judge the build cap on `cl.exe`/`link.exe` by working directory, never on `MSBuild.exe` process count. |
| **W113 (SWE-2) mapped for 3 h and landed nothing**; W113-2 landed decisions 1-2 only; decision 3 went to an Opus lane (W123) | SWE-2 gets bounded UI/feature work with named seams. Engine wiring with design judgement goes to Opus. |
| **W89 ended partial after 86 min** (two commits, a red oracle, a 0xC0000005 crash, no report) | Give a lane a crash control on the known-good exe **first**; W89-2 did that and root-caused the crash in one pass. |
| **W94 blocked at its gate on a stale base** (the lead's staging error) — a wasted spawn | Re-check the base sha in the brief against the tree the worker will get. |
| **A SWE-2 worker typed the executable at a shell** for "quick selftests" and put a game window on the user's desktop three times | `CCCP_HEADLESS=1` in every worker process environment; selftest flags imply headless in the engine; kill the visible one by PID, end the session, resume it with a corrective prompt. Instructions alone were not enough. |
| **W45-2 opened WinDbg on the user's desktop**; **W58 was a zombie for 2 h** after a rate limit | No GUI tool in a lane, ever (`cdb -c` or a dbghelp script for dumps). The runner ends a lingering process after a minute and resumes the same session. |
| **Devin billed $72.34** for WIKI generation on repo connect (CLI usage was $0.01) | The user revoked Devin's GitHub access; keep GitHub disconnected from Devin. CLI SWE-2 Max stays free (0 ACU). |
| **The lead's timestamps drifted up to 90 minutes ahead** over one hour of estimated stamps | Run `date -u` in the same turn as any board/log timestamp. When a stamp is later found wrong, append a correction note; never silently rewrite history. |

## 3.6 The W-lane index for this window (one line each)

| Lane | Route / tree | What it produced |
|---|---|---|
| **W124-2** | Grok, `hold-pause`, `stage2/relaunch-slots-fg6b` `49b7249b78` | F13 fix: the relaunch window ends after the first `MovableMan::Update` (`EndLockstepRelaunch` clears the flag and the kept checkpoint ids; dead ChecksLeft code deleted). RED `tick 460: owner=2 after hand-back` → GREEN 10/10 switch-returner arms, drop3 9/10 (the tenth wrote the ASan report that opened F20). ACCEPTED, merged `f87026a765`. |
| **W127** | Grok, `value-observations`, `f1fb71f401` | F8 fix: at `BeginPreviewScripts`, every MovableObject userdata in the clone's copied `self` is remapped (original→clone, live world→overlay shadow, clone parts by uid); anything else freezes the clone's hooks, counted. Write fixture 6/8→8/8 with `shadows=2`, fl100 violations=0, det traces identical. ACCEPTED; opened F16. |
| **W129** | Grok, `item4-feel`, `6ae2d3c6f4` | Preview clones skip `Create` and are deleted under `FaithfulCloneScope(false)`, so no checkpoint sound identity is allocated; a `SoundContainer` in a copied self becomes a preview copy whose `m_PreviewOrigin` points at the canonical container; the glow oracle is keyed to the firearm subtree. Cursor holds at depths 1/4/7 (was +2 per preview); AK lpinv 4/4, pickup 8/8. ACCEPTED. |
| **W131** | Grok, `h4-secondary`, `f605a8330a` | F11 fix: the checkpoint sound identity cursor pinned to `1<<40` at every deterministic match start; `-selftest-preallocate-sound-identities` gives the two-process form. ACCEPTED; the lead dropped its misleading "impossible in practice" warning in `2dca901fbd`. |
| **W132** | Grok, `takeover-build`, measurement only | Two Final+ASan builds (W103 tip `3f27189784` pre, W130 tip `b944ce84dc` post) × 5 AK-47 HUD runs each + 2 ledgers: **all 12 halt on the same heap-use-after-free**. Proved W130 was not the mechanism and gave F15 its exact stack. Nothing to merge. |
| **W133** | Grok, `item4-simspeed`, `8b3c6590ee` | F15 part 1: `RemapExternalLinks` over `m_pMOToNotHit`, `m_pItemInReach`, `m_pMOMoveTarget`, waypoints, a craft's incoming MO, attachables and inventory; `ResidentForRetiringShadow`. Correct, **incomplete** (spawns, the add-queue tail, and `EndPreviewScripts` dereferencing freed clones). Merged as the branch `c13966d02f`. |
| **W133-2** | Grok, same tree/branch, `03a7add7be` (+`6e3698b463`) | F15 completion to the lead's design: `OverlaySurvivorOf` returning nullptr for members of `RetiringOverlayObjects()` (spawns, spawnMeta keys, added-since-mark), and `EndPreviewScripts` iterating recorded uid/state pairs instead of clone pointers. ACCEPTED WITH FOLLOW-UPS (Opus verifier every line, evidence re-derived from `D:\mx\w133b`; the lead spot-checked the three functions), merged `8eaa54d923`. GREEN bar met on the ASan build: HUD 5/5 exit 0 no report, ledger `PASS press tick 153`, lpinv 8/8, suite 11/11. Debt: F15b arms, F15c hardening (§3.4). |
| **W134** | Grok, `item7-chat`, `0bbfaedb10` (tools only) | F19 fix (see the table). B-2-3 closed by the lead's reading instead of a change: `compare_sim_traces` already diffs over the **union** of both traces' subsystem keys, so a subsystem one side drops is already a divergence. ACCEPTED, merged `c72c8c05d2`. |
| **W135** | Grok, `fencing-warm`, `9a37f3a246` | F17 + F18 (see the table), each red-first with its FAIL line quoted on exe `b13b1a6e9e`. ACCEPTED, merged `123d292dca`. Not done by the lane: a 2-peer stop arm. |
| **W136** | Grok, `alias-walk`, `b380ac89bb` | F20 attempt, base `5c2c65c2ed`. **REJECTED** (§3.4 F20): real fix for the covered shape (the big frame as the yield's immediate caller), genuine RED/GREEN, suite 11/11 — but frames carried as `ftsz` links are never sized and the safety net cannot see them. The worker disclosed the deviation in its report ("REPORT.md:99-101") — read a report's deviations section first. **W136-2** (Opus engineer, same branch, in progress) closes it with a second RED-first arm and per-slot sizing. |
| **W71-3** | Grok, `item5-lifecycle`, `435924a207` | F10's ownership gate: `writerUID` under codec v21 (v20 decode kept for replays), owner-only producers, `IsLockstepAIOrderAuthorized` (team authority OR sender-owns-target OR a writerUID naming a same-team actor the sender owns). Three red arms. ACCEPTED, merged `ba0cd02d34`. |
| **W71-4** | Grok, same tree, `b0e33ba3c7` | F10's last gap: under lockstep `UpdateMovePath` keeps its request armed while this actor has in-flight waypoint writes and nothing to load, so the applied add is loaded next tick and the owner sends the pop. Merged `9484ff6106`. ACCEPTED WITH CORRECTIONS after the lead's line-by-line read (evidence re-derived: RED `D:\mx\w71e\red_lockstep3\stdout.log` L7, GREEN `green_lockstep\stdout.log` L7, pulse 316..319 both peers): the trim in §4.4 is being committed on the wave by an Opus engineer; F10 counts closed only when the trimmed arm and the pulse are green on the wave build. |
| **FG6B battery** | Grok, run-only, lane `fg6b-battery` | The ONE acceptance battery on `7d3666aa15` / exe `98f73d74aaed`; scratch `D:\mx\fg6bat2`, ports 47651-47656 / 8457, 7 h cap. 19 selftests: 18 PASS + the F14b reconnect red. Still writing its post-battery steps at 16:30 MST; `battery_exit.txt` = 0. Known non-engine reds: `binary_matches_source` on every gameplay fixture (the lead's F14b source edit at 15:49 MST is newer than the 21:52 exe) while every **semantic** check passes (peers identical over the whole range on all seven fixtures); the old-driver fencing shape; the `heal_mod_semantics script_continued` artefact. |

Also in this window and already merged into the candidate: W90 (AK-47 recording + Lua-fire ledger arm), W104b
(preview projectiles as ledger events + presentation ghosts), W119 (F7), W120 (F6+F12), W123 (S4 ICE/mux wiring,
1391 lines — **no ICE connect exercised yet**), W125 (SWE-2 host port-map toggle + lobby line), W126 (module digests,
chat wire, protocol v2), W130 (MOID-join ordering), W-MAC-RUNNER (`tools/posix_test_runner.py`, so the repo's own
drivers run on macOS), W-MAC-FG6B (the candidate green on arm64: 14 selftests + 5 new cases, settings gate, A7 5/5).

---

# §4. CURRENT STATE (2026-09-12 23:30-16:40 MST, all values read from the trees)

## 4.1 Trees, tips, binaries

| Tree | Branch | Tip | Exe |
|---|---|---|---|
| `control-build` | `stage2/fixgroup-6-lead` | `5c2c65c2ed` | `98f73d74aaed385710ac077d9c0e7e331549ba23a19059b044c6b5f1b6dbe631` — built 14:52 MST from `7d3666aa15`; **does not contain `5c2c65c2ed`** (the F14b selftest fix), which is why `binary_matches_source` is red in the battery's fixture steps |
| `takeover-build` | `stage2/fixgroup-6-lead-wave-a` | `1fa9b540f4` (= origin, pushed 17:26 MST; = `8eaa54d923` + the W136-2 merge) | `cf2e611bf36d575aef5fe856a431704dc1ae2532e58b9b3e95556bcfc58c70eb`, built 17:17 MST from `1fa9b540f4` (Final, `/MP6`). **Gates already green on it** (an Opus engineer, stopped before the promotion; logs under `D:\mx\wave-b\`): `run_selftests.py` 11/11 (`selftests\result.json`); `-script-graph-selftest -num-lua-states 4` **181 PASS, 0 FAIL** (`script-graph\stdout.log`, the six `coroutine_*` arms included); AK-47 HUD replay ×3 `PASS press tick 500` on this plain Final build (`ak47hud\run1..3`); `ak-lpinv176` depths 1/4/7 as before, depth 12 `spawned=8 [None,Tracer Ronin AK-47,Casing,None×5]` (still unexplained, §5.1); **W71-4's craft_cargo pulse `host_wp_nonzero [316, 317, 318, 319]` = `client_wp_nonzero` (`craft_cargo\pulse_gameplay_wave.txt`) — F10 CLOSED on the trimmed code.** Not run on it: verify6, the battery. Earlier gates on the previous wave exe `959fc25e…` (W127 fixtures 8/8 ×3, W129 AK arms) are under `D:\mx\wave-a\`. **Verified on it** (Opus engineer, all through the runners, logs under `D:\mx\wave-a\`): `run_selftests.py` 11/11 (`selftests\result.json`; the new arm `PASS a_path_update_stays_armed_while_the_waypoint_add_is_in_flight` at `net-lockstep-selftest\stdout.log:12`; `net-reconnect-selftest` PASS — F14b confirmed on a build); W127's held-ref fixtures `lpinv-write` / `lpinv-control` / `lpinv-fallback` all `PASS tick 153: 8/8`, `preview_codec_fallback=0` on write/control and the by-design Activity freezes only on fallback (same three uids as W127's run) — this is the proof of the lead's fix-up `c676483586` (without it every sound-holding preview froze); W129's AK arms `ak-lpinv176` 4/4, `ak-lpinv150` 4/4, `pickup-lpinv100` 8/8, `preview_codec_fallback=0`, `violations=0`, canonical state byte-identical. Open observation: at depth 12 the AK arm lists `spawned=8` (five unnamed extra entries) where W129's tip listed 3; depths 1/4/7 identical, canonical untouched. The lead's hypothesis is F16 (`74072b3d08`: the preview now fires the clone's OWN gun instead of a duplicate shadow, so its flash/casing particles appear in the preview's spawn list); confirm by re-running the arm on `c676483586` (pre-F16) vs `74072b3d08` before the battery. |
| `p4b-interp-validation` | `stage2/p4b-interp-lockstep` | `f83cc75099` (= `origin`) | `ff6a44ac46c8a48c3757dcd888ea419e9e88b7b5070ea88a8c15d8629a30b3f6`, 2026-09-11 16:44 MST |
| `item4-simspeed` | `stage2/preview-substitute-links` | `03a7add7be` | `c8278244ba23…` (the lane's own) |
| `alias-walk` | `stage2/coroutine-stack-fit` | `b380ac89bb` | `9f851b0f7430…` (the lane's own) |

Working-tree dirt that is expected and must not be committed: the two rebuilt vendor libs
(`external/sources/allegro 4.4.3.1-custom/_Bin/allegro-release.lib`, `external/sources/luabind-0.7.1/_Bin/luabind-release.lib`)
and `p4b-interp-validation/AbortCode.txt` (untracked).

## 4.2 Wave A, in merge order (all on `stage2/fixgroup-6-lead-wave-a`)

`5c2c65c2ed` (base) → `f6d70fb596` W127 `f1fb71f401` (clean) → `2f9e7dabd0` W131 `f605a8330a`
(`NetLockstepSelfTest.cpp` keep-both of the W119 and W131 arms) → `2a46169dfb` W129 `6ae2d3c6f4` (`Main.cpp` include
keep-both; `LuaMan.cpp` W127's remap block then W129's `DropPreviewSoundCopies`) → `ba0cd02d34` W71-3 `435924a207` →
`f87026a765` W124-2 `49b7249b78` → `c13966d02f` W133 `8b3c6590ee` → `123d292dca` W135 `9a37f3a246` →
`c72c8c05d2` W134 `0bbfaedb10` → **three lead fix-ups** `c676483586` (let preview `SoundContainer` copies pass the
hold remap — the W127/W129 semantic clash found while merging: W127 freezes a preview whose hold carries any
non-MovableObject Entity, and W129 had just started putting SoundContainer copies in that hold, which would have
frozen every preview that holds a sound and made W129's AK-47 fix moot), `74072b3d08` (F16: consult the part map
before the live-world branch), `2dca901fbd` (drop the W131 rematch warning) → `9484ff6106` W71-4 `b0e33ba3c7`.

→ `a44e14132f` (the lead's W71-4 trim) → `8eaa54d923` W133-2 `03a7add7be` (17:09 MST; LuaMan.cpp keep-both) →
`1fa9b540f4` W136-2 `7a689008fb` (17:13 MST, merged clean and verified 0 differing files; **local only until the
Opus verifier's verdict on W136-2 — push it only on ACCEPT, otherwise `git reset --hard 8eaa54d923`**).

Every merge commit was verified with `verify_merge_commit.py` (0 differing files; the W133-2 merge flagged only
the F14b cherry-pick lines, identical on both sides). Trailer scan `f83cc75099..1fa9b540f4` = **0 hits**. The
battery for this tip is briefed at `grok-workers/fg6c-battery/prompt.txt` (placeholders `TIP_SHA`/`EXE_SHA`/
`BUILD_TIME` to fill from the control-build promotion): scratch `D:\mx\fg6bat3`, ports 47661-47666 / 8458, baseline
fg6b, expected flips listed, N6 = the W71-4 pulse.

## 4.3 What is merged and what is not

- **Merged on the wave, read and accepted:** W127, W131, W129, W71-3, W124-2, W133 (as the branch), W135, W134,
  W71-4, the three lead fix-ups, and the W71-4 trim `a44e14132f` (§4.4).
- **Merged 2026-09-12 17:09 MST:** W133-2 `03a7add7be` as `8eaa54d923` (LuaMan.cpp keep-both: W127's map clears plus
  W133-2's bindings clear at the top of `BeginPreviewScripts`; the verifier flagged only the F14b cherry-pick lines,
  identical on both sides). The wave exe `959fc25e…` predates this merge: **rebuild before any gate or battery.**
- **Merged 2026-09-12 17:13 MST, pushed 17:26 MST:** W136-2 `7a689008fb` as `1fa9b540f4` (clean, 0 differing files).
  Basis: the lead read the whole codec diff (every link sized from `slots[i-1]`, no `pcslot` gate, no caller fallback,
  no `base + framesize` term, no `frame_islua` gate in the net, the fits query on the invariant) and the lane's quoted
  evidence (RED `D:\mx\w136b\red\run\stdout.log:539-540` — the helper arm's false fit `110/137` then 0xC0000005 on the
  unfixed codec; GREEN `green\run\stdout.log:683-688` six coroutine PASS lines, 181/0, suite 11/11, gcb pass). **The
  independent Opus verifier pass was stopped by the clean-stop order before it reported** — the next lead runs it
  (brief: the W136 rejection findings 1-4 as the checklist) before the battery. Known and disclosed: the metamethod arm
  is regression coverage only (it passed on the unfixed codec by over-allocation, `D:\mx\w136b\red-meta`).
- **W136** `b380ac89bb` — REJECTED (§3.4 F20), superseded by W136-2 on the same branch.
- **Nothing is in the milestone yet.** `stage2/p4b-interp-lockstep` stays at `f83cc75099` until the wave is built,
  verified, battered and the family is green.

## 4.4 The W71-4 trim (committed `a44e14132f`, 16:3x MST, on the wave)

`Source/Entities/Actor.cpp` (-13), `Source/Entities/Actor.h` (-3), `Source/Network/NetLockstepSelfTest.cpp`
(+4/-22). It removes, from W71-4's merged commit:

- a **test-only branch in production code**: W71-4 put a whole load-and-pop emulation (set `m_pMOMoveTarget`, enqueue
  a `PopWaypoint` command, advance `m_WaypointCursor`) inside `if (g_SceneMan.GetScene() == nullptr)` in
  `Actor::UpdateMovePath` — a path that only executes when there is no scene, i.e. only in the unit selftest;
- `Actor::GetWaypointCursor()`, an accessor added only for that arm;
- the arm's own `install_allegro` + `SceneMan::Construct()` (moved into the suite's setup, where the other managers
  are constructed);
- and it corrects **two inverted FAIL messages** the worker wrote ("the path update stayed armed while the waypoint
  add was in flight" printed on the failure where it had *disarmed*).

Committed through `git_commit.py`; the wave build and the gates in §4.1 were run on the trimmed tree.

## 4.5 Pushed vs not (verified by `git ls-remote origin`)

| Ref | origin |
|---|---|
| `stage2/p4b-interp-lockstep` (the milestone) | `f83cc75099` — **up to date** |
| `stage2/fixgroup-6-lead` (the candidate) | `5c2c65c2ed` — **up to date** |
| `stage2/fixgroup-6-lead-wave-a` (the wave) | `1fa9b540f4` — **up to date** (pushed 17:26 MST after 0-hit scans; earlier pushes at 16:53 and 17:09 MST) |
| `stage2/coroutine-stack-fit` (W136 + W136-2) | `7a689008fb` — **up to date** (pushed 17:26 MST) |
| **Backup push, 2026-09-12 16:29 MST** | every local `stage2/*` and `xref/*` branch that was new or ahead: 89 branches, `--force-with-lease`, 84 created + 5 fast-forwarded (`actor-switch-tests`, `fixgroup-6-lead`, `item3b-harness`, `resync-boundary`, `session-directory`), none rejected. CI runs only on `development` pushes and PRs, so nothing was triggered. Trailer scan: the single pre-existing public hit `9751a90e29` (§1.1), nothing new. |
| `stage2/coroutine-stack-fit` (W136) | `b380ac89bb` · `stage2/preview-substitute-links` (W133-2) `03a7add7be` · `stage2/posix-test-runner` `586fcfce02` · `stage2/fixgroup-6` `8c7d5b75c6` · `stage2/fixgroup-6-preview` `f83cc75099` |
| `main` | **does not exist on origin.** The fork's default branch is still `modernization-effort` (`0064b7e542`). Where old notes say "main", they mean the milestone branch. |

## 4.6 Lanes live at the time of writing

- **FG6B battery** (Grok run-only lane `fg6b-battery`, `control-build` exe `98f73d74aaed` from `7d3666aa15`):
  DONE (exit 0 at ~16:55 MST 2026-09-12; `fg6b-battery/REPORT.md`, 508 lines, scratch `D:\mx\fg6bat2`). The lead's
  verdict: **no true engine failure.** Reds, all classified non-engine: the reconnect KAT `56527b38…` (F14b — fixed
  at `5c2c65c2ed`, PASS on the wave build's suite); the `fencing_two_transports` FAILs (`fenced=0`) come from the
  h4gates OLD driver copy while the current driver passed `fencing_1..5` 5/5; `heal_wp` `script_continued` (known
  heal artefact); `binary_matches_source` on fixtures / crab / arm1 (the lead's F14b source edit at 15:49 MST
  postdates the 21:52 exe) with every semantic check PASS on all seven fixtures and switch_control; the crab driver's
  `_crab.ps1:84` argument error (harness); the bare `global-callback` flag REFUSE (the driver row `gcb` passes);
  `present_identity` compared_ticks=0 (known); `controller_log` has no fixture. Green: all other selftests, every H4
  and B1 gate, lobby, heal, reclaim, dedicated, lifecycle, directory (29/29 + e2e), fl100, mixed_lua, heal_global,
  and N1-N5 (AK-47 HUD 3/3 with no dump on this pre-W133 exe, expire3, the loopback session-id e2e — not an ICE
  connect —, portmap on/off, drop3_switch/lpinv_100/preview_event_d7). `control-build` is free to fast-forward.
- **W136-2** landed 17:12 MST (`7a689008fb`, §4.3); its report is in SPAWN_LOG (17:3x MST row) and the evidence under
  `D:\mx\w136b`.
- **Nothing is running** (17:30 MST). No worker session of any route needs resuming.
- **W133-2** (Grok, `item4-simspeed`): landed 16:55 MST with two commits over the cherry-pick `8bff413243`:
  `6e3698b463` (EndPreviewScripts iterates recorded uid/state, RED ASan UAF `LuaMan.cpp:7914` in
  `D:\mx\w133b\asan\red\report.32984` → no report after it) and `03a7add7be` (`OverlaySurvivorOf` redirects survivor
  links off objects the overlay deletes — implemented to the lead's design with NO red of its own: the worker did not
  observe the DrawHUD head after commit 1). GREEN exe `c8278244ba…`: HUD 5/5 exit 0 no ASan, `PASS press tick 500`,
  `compare_sim_traces` 600 ticks identical, ledger `PASS press tick 153`, lpinv PASS, suite 11/11. Under an Opus
  verifier's read; the lead merges after spot-checking the flagged lines. The missing red for the second head is a
  unit arm to add (spawn speculatively, point a resident's link at the spawn, EndSpeculation, assert the link is null
  or a resident) — record it as a follow-up if not done before the battery.
- The Mac is idle: the next Mac rows are the ICE-only e2e (W123's gap), the AK-47 oracles re-run after W129/W127, and
  the Windows-host/Mac-client lockstep e2e after W131.

---

# §5. WHAT IS LEFT, IN ORDER

## 5.1 Close the wave (hours)

1. (done 2026-09-12 16:58 MST) The FG6B report is read and classified (§4.6): no true engine failure on the
   candidate at `7d3666aa15`; recorded in LEAD-REVIEW and SPAWN_LOG.
2. W136-2 is merged (§4.3) but its independent verifier pass never reported: spawn an Opus verifier on `7a689008fb`
   with the W136 rejection findings 1-4 as the checklist (re-derive RED `D:\mx\w136b\red`, GREEN `green`, the
   frame-walk arithmetic against `lj_frame.h`); on a reject, revert the merge on the wave (`git revert -m 1
   1fa9b540f4`) and reopen F20. Also answer the depth-12 spawn question (§4.1) before the battery.
3. (done 2026-09-12 17:09 MST) W133-2 reviewed, accepted with follow-ups, merged `8eaa54d923` (§3.4 F15). Open from
   it: the F15b unit arms (a bounded Opus-engineer lane: add both arms to the `[lpinv]` case loop in `Main.cpp`
   ~1999-2077 as the verifier specified — RED on `6e3698b463`'s exe, GREEN on the tip) and the F15c hardening (walk
   parts of retiring objects and shadow parts into the retiring set; recurse wounds in `RemapExternalLinks`; record
   `{uid, nullptr}` bindings for stateless parts). Neither blocks the battery; both are test/hardening debt.
4. (done 17:17-17:24 MST on the wave exe `cf2e611b…`, §4.1) suite 11/11, script-graph 181/0, AK-47 HUD 3/3 on a
   plain Final build, the craft_cargo pulse closed at 316..319 on both peers.
5. **Promote:** in `control-build` (on `stage2/fixgroup-6-lead` at `5c2c65c2ed`; restore the two dirty vendor libs
   with `git checkout --` first) run `git merge --ff-only stage2/fixgroup-6-lead-wave-a` (expect `1fa9b540f4`),
   rebuild Final ALONE with `CL=/MP12`, record the exe sha256, then verify6 8/8 (`lead-tools/verify_lead6b.py`, edit
   `REPO`/`OUT` in a copy).
6. **ONE battery** on the promoted tip: fill `grok-workers/fg6c-battery/prompt.txt` (replace `TIP_SHA` with
   `1fa9b540f4`, `TIP_SHA_FULL` with `git rev-parse HEAD`, `EXE_SHA` with the control-build sha256, `BUILD_TIME`),
   spawn it run-only with `python win_cli/win_job.py fg6c-battery D:/Projects/control-build fg6c-battery/prompt.txt --task "FG6C battery" --timeout 25200`
   (7 h; the runner logs the model evidence line). Every red inventoried, root-caused in parallel, classified true/non-engine by
   the lead. Do not fix one gate at a time.
7. Mac clean clone of the merged tip (build + selftests + settings gate + A7 5 arms) — and check the two
   cross-platform traps first: every new `.cpp` in its `meson.build`, no default arguments of nested-class type.

## 5.2 Promote (a day)

8. Merge the candidate into `stage2/p4b-interp-lockstep` in `p4b-interp-validation` (`--no-ff`), tree clean apart from
   the vendor libs, trailer scan `f83cc75099..HEAD` = 0, push with lease, record the push here and in `STATUS.md`
   with the remaining failures.
9. **The Source44 family** on the approved tree (§0.4), nothing else running. Verdict = chain + remaining + breadth
   81/81 accounted + matrix 106 reviewed + the Mac attempt green. Then items 1, 2 and 5 can move.

## 5.3 Mac follow-ups (parallel, the Mac is never idle)

10. **ICE-only end-to-end row** for W123 — the honest gap in the S4 wiring: the loopback e2e took the IP half, no ICE
    connect has ever been exercised, and rematch/resync over ICE is refused. This is the first real internet-play proof.
11. **AK-47 oracles re-run on the Mac** after W129 and W127 (the +2-per-preview cursor drift and the held-reference
    remap must behave identically on arm64).
12. **Windows-host / Mac-client lockstep e2e** after W131 — no cross-platform two-peer match exists in the records;
    every Mac proof is Mac-vs-Mac. Compare the sound observations and the 600-tick traces.

## 5.4 Finish the review (a session)

13. Part B's remainder: the rest of `Source/System`, `Main.cpp`, and the network selftests. F3 stays an open
    constraint until a lane touches the frame lane.

## 5.5 Then the roadmap (LEAD_PLAN §6, in value ÷ effort order with the user's decisions applied)

- **Item 6 close-out:** the headed §11 reconnect review with the user at the desktop (LEAD_PLAN Appendix A: a
  `headed_review.ps1` under `reviews/takeover-20260909/headed-review/`, `steps.md`, screenshots into `screens/`,
  judged by the Opus visual reviewer), then the B2 moderation GUI probe on Windows.
- **Item 4:** the 4c headed measurements at 100/200 ms and two render rates on the promoted build, then optimistic
  projectiles travelling, the Activity UI following the preview, then 4d (sim-speed program, rollback verdict).
- **Item 7:** chat UI + routing on top of W126's wire → 7b overlay/toasts/delay display → 7c mod-mismatch UX → 7d →
  7e → 7f.
- **Item 8:** ICE connect proven → rematch/resync over ICE → the dedicated headless host as the persistent Mac
  server/world → lobby v2 → 8d.
- **Item 3:** 3b fixtures under one script, the UI path of an AI order, the seat-side pie close; escalate 3e to the
  user with options when the design fork appears.
- **Item 5:** 5b 3/4-peer packs, co-op, PvPvE, full lifecycle on both platforms; 5c the WSL2 leg in a window with no
  Windows builds.
- Roadmap notes carried from accepted lanes (not blockers): a failed port-map lease renewal is never retried; the /24
  gate refuses a larger-LAN gateway for UPnP; no GUI toggle for `NetworkPortMapEnable`; the preview projectile ghost
  is static until adoption; W135's 2-peer stop arm; F16's preferred form (register each preview clone as its
  original's overlay shadow).

---

# §6. LESSONS LEARNED

## 6.1 Optimal subagent usage

- **Route by kind of judgement, not by size.** Opus 5 for engine surgery, design-heavy lanes, independent verification
  and **every** screenshot or visual question. Grok 4.6 Extra High Fast for batteries (run-only), triage, read-only
  audits, reviews and mechanical implementation **from a design the lead has already fixed**. SWE-2 for bounded UI and
  feature work with named seams. The one time engine wiring with real design judgement went to SWE-2 (W113), three
  hours produced nothing and an Opus lane (W123) did it.
- **A worker's "green" is a claim.** W130's acceptance ("sound by construction, unproven") cost a whole item-4 lane
  and was only undone by an ASan measurement. Read every diff line, re-run every detecting test, re-derive at least
  two numbers of every report from the raw files it cites.
- **Read two lanes' diffs together when they touch the same seam.** The W127/W129 semantic clash (W127 freezes a
  preview whose hold carries a non-MovableObject Entity; W129 puts SoundContainer copies into that hold) was invisible
  in either diff alone and would have silently disabled W129's fix on the merged tree.
- **Read-only audit lanes are cheap and they work — if every finding is re-derived against the candidate.** Five
  part-B lanes at 6-10 minutes each produced 43 findings; three survived as F17/F18/F19, three were already closed by
  later work, the rest were notes. Checking against the P4B tip instead of the candidate would have produced three
  false fix lanes.
- **Brief with the design fixed and the acceptance stated.** Every brief that said "RED = <this exact line>, GREEN =
  <these exact runs>" produced a usable lane; the ones that said "root-cause and fix" produced partials.
- **The lead's own tokens are the scarcest resource; spend them on verdicts, not on watching.** The lead's weekly
  budget ran out at the end of 2026-09-12 while Opus 5 had 18% and Grok/SWE-2 were unlimited. What worked in the
  last hours: an Opus verifier does the deep read of a HIGH-severity lane and quotes file:line for every claim; the
  lead reads only the flagged lines on the source (25 lines confirmed the W136 reject) and every "not done /
  deviations" paragraph of the worker's report first; Opus engineers own builds, gates and mechanical corrections
  from an exact spec; the lead never `cat`s a log — `grep`/`tail -c`/`head -c` with a cut. What did NOT work and is
  now a rule: delegating the handoff document (§1.1).
- **A verifier per HIGH fix pays for itself.** W136's narrow-shape fix passed its own RED/GREEN and the suite; only
  a reviewer reading LuaJIT's frame model found that every frame carried as an `ftsz` link was skipped. Ask the
  verifier for the minimal correction and the RED arm that proves the uncovered shape, then re-spin from that.

## 6.2 Parallel work

- **Two build lanes at `/MP6`, none while the lead builds.** Full uncapped builds exhaust the machine's memory.
- **Reuse merged worktree directories** for new lanes (`alias-walk`, `hold-pause`, `fencing-warm`, `h4-secondary`,
  `value-observations`, `item*`) — the firewall rules are per executable path, and a new directory means a prompt on
  the user's desktop unless the elevated script is re-run.
- **Prepare the next wave on a scratch branch while a battery pins the candidate.** That is exactly what
  `stage2/fixgroup-6-lead-wave-a` in `takeover-build` is for: nine merges and three fix-ups landed while FG6B kept
  `control-build`'s exe frozen; control-build fast-forwards when the battery ends.
- **The Mac is never idle.** It is a second worker host (Grok via the Cursor CLI, and Claude Code Opus 5), not a
  reference you consult once a day. It found F9 (the two cross-platform build breaks) and F11 (the identity cursor
  difference) inside one window.
- **An idle-gated step and a continuous e2e loop cannot share a machine.** 65 minutes of a battery's life were spent
  waiting for an engine-idle window that never came.

## 6.3 Grok quirks

- **Extra High Fast exists only through the CLI**, and only the init line proves it (`"model":"Cursor Grok 4.6 Extra
  High Fast"`). The Task tool returns no model evidence and silently delivers High.
- **Everything is bounded or it never ends.** The runner reads the init line once, then waits for the process under a
  hard timeout — no polling loops. One earlier lane spun an exe-hash comparison loop that never finished.
- **Workers write inverted test messages.** W71-4's arm printed "the path update stayed armed…" on the failure where
  it had *disarmed*. Read every assertion's message against its condition.
- **Workers add test-only branches to production code** to make a unit arm reachable (W71-4's whole load-and-pop
  emulation under `if (GetScene() == nullptr)`). Grep every diff for conditions that can only be true in a test.
- **Workers launch the engine directly** unless the environment stops them. Instructions were not enough;
  `CCCP_HEADLESS=1` in the spawned environment plus headless-implying selftest flags are.
- **Workers leave vendor `_Bin` libs dirty** after a rebuild. Restore them to HEAD before checking out a wave branch.
- **Workers do not run the suite unless told**, and they report what they ran, not what they skipped (W126 → F14).
- Undisclosed oracle corrections happen (W58-2): the rule is now that a lane reports every checker/driver change with
  both run directories, and an undisclosed edit is treated as a masked failure even when the new check is stricter.

## 6.4 SWE-2 / Devin quirks

- The CLI's sessions are **durable**: a rate-limited turn resumes the same session after the stated reset instead of
  restarting; the runner records the session id at launch, kills the lingering process and resumes.
- A rate-limited process **lingers without exiting** (W58 was a zombie for two hours). The runner ends it after a
  minute.
- The model gate must be a **pre-flight** (a one-line smoke prompt whose export names `SWE-2 Max`) because the real
  export only names the model after a turn completes.
- Two sessions at a time, staggered ~90 s (simultaneous starts trip the account-wide message rate limit); keep the
  queue (`devin_cli/queue.json`) at least two deep.
- Connecting the repo triggered **$72.34 of WIKI generation**; GitHub stays disconnected from Devin. CLI usage is free
  (0 ACU).

## 6.5 Mac usage

- Every CLI run is a **launchd job in `gui/501`** — a plain SSH session has a locked login keychain and even `--help`
  fails.
- The Grok model gate reads `agent-output-*.jsonl`: a lane script that writes `agent-output.jsonl` (no dash) skips the
  gate **silently**, leaving the spawn unverifiable.
- Fetch evidence with `tar` over ssh excluding `runtime` (and `gns-src`, `scratch`): `scp -r` follows the `Data`
  symlink and copied 512 MB of game data in three minutes while the real evidence was under 1 MB.
- A Mac lane keeps under 12 GB; `/Users/erol/Documents/Codex/` is read-only retained evidence.
- The Mac catches what no Windows gate can: a missing `meson.build` entry, a clang-rejected default argument built
  from a nested struct, and platform-dependent counters (the sound identity cursor).

## 6.6 Bug minimization and proof

- **RED first, on the exact defect.** Every accepted fix in this window quotes its failing line on a named exe and its
  passing line after. A fix without a detecting test that was red is not verified, it is argued.
- **Measure, don't infer.** W130 (inferred mechanism, wrong) vs W132 (ASan, 12 verbatim reports, right). The same
  lesson produced Stage 1's `particles@29` closer: the hunt chased physics → trig → threads before an eval-order bug
  was measured.
- **When a fix's mechanism cannot be tied to the faulting line by reading, do not accept it — instrument it.**
- **A half-fix is a failure mode of its own** (F14 → F14b). After editing a version constant or a golden, re-run the
  detecting test **on the rebuilt binary**.
- **Keep-both merges edit the conflicted working file**, never a stage copy; `verify_merge_commit.py` after every
  merge commit catches what reading does not (it caught two silently dropped auto-merged hunks).
- **Never combine an assertion and a commit in one shell line.** Doing so once committed conflict markers; separate
  the check from the commit.
- **Read the clock before every stamp** (`date -u`); estimated stamps drifted 90 minutes in an hour.
- **Classify every red before fixing anything.** The FG6B battery's reds were: one lead-caused stale-binary artefact
  (`binary_matches_source`, from a source edit made after the build), one known old-driver fencing shape, one known
  `script_continued` heal artefact, and one harness aggregation bug — while every semantic check passed. Fixing them
  as engine defects would have been pure waste.
- **Sim-consistent bugs need state-census gates, not divergence gates** (the resync double-spawn was invisible to
  every hash gate because both peers did it identically). Census asserts must BOUND state, not pin outcomes.
- **Cross-platform build misses are a standing class:** new `.cpp` in `meson.build`; no default arguments of
  nested-class type in headers.
- **The state inventory is a real oracle** (`ContractAudit.h` + `StateInventory.csv`): every new field is declared as
  carried or transient, and F19 showed that an oracle that cannot fail is worse than no oracle — `run_audit` now fails
  on an empty check list.

## 6.7 Process

- `STATUS.md` deltas make waste visible. A lane that lands without moving a number is a negative delta with a reason,
  and the lead re-evaluates before spawning the next. That rule is what surfaced W113, W89 and W130.
- `SPAWN_LOG.md` rows carry the model evidence line and the report path — spawn rows are written by the runners, and
  landing rows by the lead with the verdict. A landing with no row is unverifiable a day later.
- **A report that exists only in a notification is lost.** When a subagent cannot write its own report, save the
  notification text verbatim to the lane's `REPORT.md` in the same turn.
- One live doc (this file), one board (`STATUS.md`), one order of work (`LEAD_PLAN.md`), one review ledger
  (`LEAD-REVIEW-overnight.md`). Everything else is evidence under `reviews/`.
- Stale pointers cost a full re-investigation (proven twice). When a milestone lands, update the pointer surfaces in
  the same session.

---

# §7. PATH INDEX

## 7.1 Local — trees and docs

| Purpose | Path |
|---|---|
| This doc / board / order of work | `D:\Projects\RESUME.md` · `STATUS.md` · `LEAD_PLAN.md` |
| Project instructions (identical twins) | `D:\Projects\CLAUDE.md` **==** `D:\Projects\AGENTS.md` |
| Pre-rewrite RESUME (verbatim) | `D:\Projects\_archive\docs_archive_20260912\RESUME_pre-rewrite-20260912.md` |
| HANDOFF, HANDOFF2, 15 phase plans, 4.5k reports | `D:\Projects\_archive\docs_archive_20260905\` |
| The lead's review ledger (F1-F20, every acceptance, every NEGATIVE) | `reviews\takeover-20260909\LEAD-REVIEW-overnight.md` |
| Every lane spawn and landing | `reviews\takeover-20260909\grok-workers\SPAWN_LOG.md` |
| Worker rules (travels with every brief) | `reviews\takeover-20260909\grok-workers\WORKER_RULES.md` |
| Trailer-rewrite hash map (2026-09-11) | `reviews\takeover-20260909\trailer-strip-20260911\map.txt` |
| H4 design + pins P1-P32 | `D:\Projects\STAGE2_H4_RECONNECT_PLAN.md` |
| Backlogs | `RECOMMENDED.md` (near-term) · `NEXTENHANCEMENTS.md` (long-range) · `MULTIPLAYER_BEST_PLAN.md` · `STAGE2_P8_PLAN.md` |
| Contract audit + the family runner | `reviews\recovery-2026-09-07\contract-audit\` (`CONTRACTS.md`, `run_family.py`, `mac-peer-20260907\MAC_RESUME.md`) |
| Mod-compatibility steering | `reviews\claude-review-2026-09-08\MOD_COMPATIBILITY_STEERING_PROMPT.md` · family runbook `INTEGRATION_33_RUNBOOK.md` |
| ADRs (through ADR-024) / public wiki | `cccp\modernization-docs\decisions.html` · https://madreag.github.io/cortex-modern/ |
| Run roots (junctions inside — never move or delete) | `D:\mx\` (`lead-fg6` the candidate's gates, `fg6bat2` the FG6B battery, `w124b` the ASan reports incl. F20's, `w132` the preview-HUD ASan, `w133b` W133-2's ASan red/red2/green, `w136` W136's red/green, `w136b` W136-2, `w71e` W71-4's red/green/pulse, `wave-a` the wave's gates, `s43b2` R5's evidence) |
| The lead's cross-session memory (harness) | `C:\Users\egerm\.claude\projects\D--Projects\memory\` — one fact per file, indexed by `MEMORY.md`; the 2026-09-12 notes cover merge verification, separate resolve/check/commit steps, worker test-only code, heredoc backslashes, Mac evidence copies, the model-gate output name, cross-platform build misses |
| The lead's session scratchpad (scripts, partB diffs, checkpoint loggers) | `C:\Users\egerm\AppData\Local\Temp\claude\D--Projects\d1ccc20c-5de9-4ed0-bdc3-fc51ecd35f88\scratchpad\` — session-specific; the reusable tools were copied to `grok-workers\lead-tools\` |
| Fixtures and recordings | `D:\Projects\stage2_p4\fixtures\` (`pickup_fire.ccreplay`, `pickup_fire.txt`), `stage2_p4\rb_replay_20260905\`, `rb_replay_buy\` |

## 7.2 Local — the scripts you will actually run

| Script | What |
|---|---|
| `reviews\takeover-20260909\grok-workers\git_commit.py` | the **only** way the lead commits (refuses trailers, re-reads the stored message) |
| `…\strip_trailers.py` | message-only trailer strip (trees unchanged) |
| `…\win_cli\win_job.py` · `…\mac_cli\mac_job.py` · `…\mac_cli\mac_claude_job.py` · `…\devin_cli\devin_job.py` · `…\devin_cli\devin_queue.py` | the four worker routes and the SWE-2 queue (§0.5) |
| `…\firewall_allow_all_exes.ps1` (elevated) · `firewall_planned_worktrees.txt` | per-executable inbound/outbound allow rules for every `Cortex Command*.exe` under `D:\Projects` |
| **`…\grok-workers\lead-tools\`** (copied out of the lead's scratchpad 2026-09-12 — use these, not a new copy) | `verify_merge_commit.py` (per-file +/- multiset check of a merge commit) · `resolve_keep_both.py` (keep-both on the conflicted **working** file) · `apply_hunk.py` (re-apply one hunk of a branch diff by content) · `check_merge.py` (pre-commit form of the merge check) · `extract_final.py` (save a Grok lane's final message as `REPORT-final-message.md`) · `verify_lead6b.py` (**verify6**; edit `REPO`/`OUT` at the top) |
| `<tree>\tools\run_selftests.py` | the 11 socket-free selftests, scored from PASS tokens |
| `<tree>\tools\win32_test_runner.py` · `run_sim_test.py` · `isolated_launch.py` · `posix_test_runner.py` | the only sanctioned engine launchers (private hidden runtime, muted settings, firewall-rule check; the POSIX one is the macOS twin) |
| `<tree>\tools\test_global_callbacks.py` | the global-callback driver — it installs the Checkpoint Global fixture; without it `RunGlobalCallbacksSelfTest` returns false and the bare flag is refused |
| `<tree>\tools\compare_sim_traces.py` · `compare_snapshots.py` · `check_switch_control.py` · `h4_gate_evidence.py` · `snapshot_runtime.py` · `check_state_inventory.py` · `wait_engine_idle.py` | the comparison and gate oracles |
| `reviews\recovery-2026-09-07\contract-audit\run_family.py` | the verification family (chain → Mac chain → remaining gates → breadth → matrix) |

## 7.3 GitHub

- `origin` = `git@github.com:Madreag/cortex-modern.git` (public fork, staying a fork). `upstream` =
  `https://github.com/cortex-command-community/Cortex-Command-Community-Project.git`.
- Default branch on the fork is still `modernization-effort`. There is **no `main`**.
- Branch naming: `stage2/<phase-or-lane>` for this work · `flagship/<milestone>` · `feature/` · `experiment/` ·
  `fix/` · `stage/` (integration) · `xref/` (cross-platform measurement, throwaway) · `pr/<name>` **cut from
  `upstream/development`**, never from the local stack.
- **Keep all remote branches. No pruning** (settled): since the worktree deletions, GitHub is the only copy of many.
- GitHub is the safety net: all worktrees share one object store (`D:\Projects\cccp\.git`); deleting a worktree cannot
  lose commits; restore any branch with
  `git -C D:\Projects\cccp worktree add D:\Projects\<dir> <branch>` (fetch it first if the ref is missing).
- The inherited nightly workflow is disabled (all three platform builds succeeded; only "Publish Release" 403'd on a
  fork — re-enabling needs only a `permissions:` block).

## 7.4 Mac

- Lanes: `/Users/erol/cortex-workers/<lane>` · retained read-only evidence: `/Users/erol/Documents/Codex/`
- Cursor CLI: `/Users/erol/.local/bin/cursor-agent` (logged in as the user), profile `cortex-workers/cli-home`,
  attribution off. Claude Code runs through launchd in the same GUI session.
- Reached as `ssh -o BatchMode=yes Erol-Mac`; runbook
  `reviews\claude-review-2026-09-08\MAC_CLAUDE_SSH_RUNBOOK.md`. On the Mac call `/usr/bin/python3`.

---

# APPENDIX A — WHAT THIS PROJECT IS

Cortex Command Community Project (CCCP) is the open-source continuation of the 2012 physics action game *Cortex
Command* — 2D destructible terrain, per-pixel physics, AGPL-3.0, C++, with essentially **no working multiplayer**.
**`cortex-modern` is a fork that adds real multiplayer:** deterministic lockstep netcode over GameNetworkingSockets,
2-4 players, PvP / co-op PvE / PvPvE, with desync self-heal, mid-match rejoin, replays, LAN discovery and internet
play; over time it also becomes a broader engine-modernization effort. Author of record: Erol Germain-Gomuc (Madreag).

**The work standard — "no compromises":** when a hard problem offers an easy way out (serialize instead of fixing the
race, disable instead of debugging, defer instead of solving, narrow the goal instead of hitting it), the easy way out
is not the answer. This is not risk-aversion — choosing the low-effort option *because* it is safer is itself the
compromise being rejected. A fallback is a last resort, used only when the full solution is **proven** impossible.
Genuine product decisions (a mod-compatibility break, a fork in design intent) still escalate to the user; difficulty
and risk do not.

# APPENDIX B — ARCHITECTURE IN BRIEF

- **Deterministic lockstep, Controller-sync** (ADR-020/024). Stage 1 made the replicated sim bit-identical across
  Win / Linux / arm64 given identical Controllers. The keep-set: portable mt19937-state hash, explicit RNG-distribution
  mapping, Allegro integer `fixmul`/`fixdiv`, the dt config-lock, deterministic trig/exp polys **at the primitive
  level**, eval-order fixes, Lua-state seed fix, PieMenu listener ordering, per-MO RNG, sim/render RNG split, Path E.
  The AI/controller is **off-wire and advisory** — excluded from every MP gate.
- **The wire:** per-tick `ControllerFrame`s per actor over the `NetLockstep` coordinator on GNS (default builds are
  GNS-free behind `CCCP_WITH_GNS`). Ownership is the `TeamOwner` policy plus an explicit **per-actor owner map**
  (W102); remote actors get injected wire frames in `MovableMan::UpdateControllers`. Host-star relay; the coordinator
  merges every remote's frames, commands and observations in **ascending peer order** (the one N-peer ordering point).
- **Input delay is per-sender** (codec v8 → v21 today); auto-pick `D_i = ceil(RTT(i,host)/tick) + 1` on the own round
  trip only. The 33.3 ms divisor deliberately errs toward lower own-latency — **do not "fix" it** to 16.7 ms.
- **Discrete actions are `NetGameCommand`s** (SetTeamFunds, SpawnActor, DeliverCargo incl. real queued purchases,
  ScuttleCraft, InventoryOp, PauseMatch, SwitchControl, PlayerBindings, AIOrder). They ride the frame, are
  sender-stamped, apply at the same frame after a stable sort by sender, and are authority-gated per team — except
  AI-generated orders, which are gated by **ownership** since F10.
- **Local prediction:** a speculative overlay (`BeginSpeculation`/`ShadowOf`/`EndSpeculation`) clones the local actor
  subtree, steps it D ticks and restores; preview scripts run edge hooks on shadow selves; a **preview-event ledger**
  makes a sound or effect play once, at the preview tick, and suppresses its canonical repeat; named speculative
  spawns become presentation-only projectile ghosts adopted at commit. The HUD and inventory draw from the clone
  through render substitutes.
- **Safety nets:** sim-gated desync hash every 30 ticks → `Desync` stop on both peers · the join gate (`NetIdentity`)
  hashes build/codec versions, dt bits, module manifest, sim-steering settings and enabled global scripts, rejecting
  with a rich reason on both peers · a 20 s missing-frame grace with an overlay · leave/rematch lifecycle.
- **Recovery stack:** desync → HEAL (host snapshots, streams it through the lobby round, every peer reloads the
  identical file) · mid-match REJOIN with an authenticated H4 admission plane (transaction handshake, single-active
  incarnation fencing, durable ticket store, drop-time ownership ledger, system-authored reseat) · REPLAY
  (`.ccreplay`) · LAN discovery beacon + browser · session directory + GNS ICE/STUN signalling for internet play.
- **Hold pause:** while a dropped seat is held for reclaim, commits stop for everyone; resolutions
  (Reclaimed/Substituted/Expired) are accepted only from the round authority.
- **Topology limit:** star relay through the host; a host drop ends the match. Host migration is open (§5.5).

# APPENDIX C — BUILD / RUN / VERIFY DETAIL

**C.1 GNS prefix.** `GNS_ROOT` / `GNS_DEP_ROOT` point into `D:\Projects\stage2_p2\gns_spike\`, which is build output,
**not** in git. GNS links statically, so a built exe survives its loss — only recompilation breaks. Rebuild recipe
(full vcpkg clone — `--depth 1` breaks the manifest baseline; GNS pinned to **v1.6.0**; install into the original
paths) is in the archived RESUME §D and reproduced by the same cmake three-step. *Treat any env-var-referenced path as
a dependency, not as scratch.*

**C.2 Running the game exe by hand (only when a human is at the desktop).** It is GUI-subsystem: **pipe** output
(`2>&1 | Out-Host`); a `>` redirect detaches it — empty exit code, no output, looks like a crash. Kill prior
instances with `Get-Process | Where-Object { $_.ProcessName -like "Cortex Command*" } | Stop-Process -Force`.

**C.3 Forensic tooling (this is how every desync was actually found).**
`CC_SIM_DUMP=<from>:<to>` per-tick per-MO exact-bit dump beside the trace · `CC_TERRAIN_EVENTS=<from>:<to>` +
`CC_TRACK_UID=<uid>` in-memory event trace with per-tick bit-exact rows and in-tick phase stamps (`phA`..`phD`) ·
`CC_TERRAIN_DUMP=<tick>` raw layers · `-controller-debug-dump` + `-controller-debug-ticks` ·
`-menu-script <file>` GUI automation (rebuilt menus only) · `-net-replay-out` / `-net-replay` ·
`-net-fake-lag` (per-process, both legs) · the rollback fidelity probe `-rollback-fidelity-probe T:K` and fuzz
`-rollback-fidelity-fuzz seed:count:window` with `rb_diffsim.py`. Analyzers: `compare_evt_run.py`, `compare_trk.py`.
The localization playbook that worked, in order: sim-gated compare → `CC_SIM_DUMP` boundary diff → raw terrain-layer
pixel diff → enriched dump fields → `[gib-cause]` tags.

**C.4 The item-4 acceptance numbers (pinned 2026-09-06 before any measurement; a miss is reported, never relaxed).**
`pace.wall_tps ≥ 59.0` and `sim_ms_per_tick ≤ 8` with prediction on · auto input delay D = 4 at 100 ms, D = 7 at
200 ms · input-to-photon ≤ 1 sim tick + 1 frame (≤ 34 ms at 60 Hz, ≤ 24 ms at 144 Hz) regardless of D ·
`local_prediction.ms_total / previews ≤ 2 ms` at D ≤ 7 · `violations = 0` always, and a > 4 px correction of the local
actor only on a remote-caused event, ≤ 1 per 10 s · firing/audio response once, at the preview tick, ≤ 34 ms after the
press · p99 draw ≤ 1.5× the local single-player baseline, no frame > 50 ms · whole cost ≤ 15% CPU over the D=0 match.

**C.5 Other legs.** WSL2 at `/home/erol/xarch-combat-p4` is **not** a git clone (sync by `git archive` tar) and
**contends** with Windows builds — stale since 2026-07-10, re-sync before trusting. macOS runs natively through the
Mac worker routes (§0.5) and now has `tools/posix_test_runner.py`, so the repo's own drivers work there.

# APPENDIX D — QUIRKS THAT BITE

**Engine / sim.** Every capped e2e reports `Over` after its own teardown — `winner_team` is the discriminator ·
e2e arms fire on `GetSimUpdateCount()` (match starts at ~2) · the delivery queue pops on **sim time** (buy arrival ≈
order + 351 ticks; buy/pause/full-loop cases need 900-tick caps) · the `Timer`/`TimerMan` sim family freezes under the
synced pause, the wall-clock family does not · **selftests run engine paths before manager construction** — never
touch manager singletons unconditionally in code reachable from a selftest (a 0xC0000005 with zero output) · wire
structs use positional aggregate init in arms, so **append** new fields · clones do not reliably carry module
identity — serialize preset triples from the PRESET pointers · `Players::NoPlayer = -1` and stock display code indexes
player arrays unguarded · MO UniqueIDs are pinned at every deterministic launch (`1<<20`), and since W131 so is the
checkpoint sound identity cursor (`1<<40`) · ESC in a lockstep match is the LEAVE flow; **P** is the synced pause.

**Per-machine settings that steer sim decisions must be pinned or identity-gated.** Pinned: timestep,
`AutomaticGoldDeposit`, crab bombs, `SubPieMenuHoverOpenDelay` (F18). Identity-gated: enabled global scripts, codec
version, module manifest, dt bits, AI update interval, pathfinder node size, MOID count, settling/subtraction. The Lua
state count was **removed** from the identity (W85).

**Render/sim boundary.** Anything a render-frame path WRITES that sim code later READS is a desync channel (five
caught instances: settle draws, trail bakes, `Exit::m_Clear`, the MOID spatial grid, Lua AI `GibThis()`). No
`UInputMan.Update()` inside sim ticks — sim-rate edges come from `KeyPressedSim`/`ElementPressedSim`, cleared once per
tick by `EndSimUpdate()`. The render section of `RunGameLoop` must stay wrapped in the render-RNG override and
`SetRenderDrawContext(true)`.

**Harness.** The ps1 `$failures` list throws before the trace compare; mismatch/rematch/perturb cases exit through
their own pass paths with distinct strings · **PowerShell switch params cannot be passed as strings from an array** ·
a dash-leading parameter value needs the colon form `-Param:"-x y"` · `Compare-Object` on dump lines puts an MO whose
FIELDS differ on **both** sides · a loopback service a lane starts takes a lane-unique port (Python's reuse-address
sockets let two lanes bind the same port) · the interp build renders every frame, so two-instance e2e paces near
vsync and long cases can outlive harness waits.

**This machine.** Use `python` (not `python3`); `pwsh -NoProfile`; never `taskkill`/`tasklist` in Bash (MSYS2 rewrites
`/f` into a path) — kill by PID from a prior query, and never filter `Stop-Process` on a CommandLine pattern (it
matches the Bash tool's own wrapper and kills the shell mid-script) · write any script containing backslashes with the
Write tool, not through a Bash heredoc (the PreToolUse hook rewrites backslash sequences).

# APPENDIX E — PROJECT HISTORY IN BRIEF

**Stage 0** `modernization-effort` (frozen, still the fork's default branch). **Stage 1** cross-platform deterministic
sim — DONE (`determinism-cs`): SimBaseline bit-identical across x86 Win / x86 Linux / arm64 macOS for all 600 ticks at
nls 1/2/4/8. The lessons: the lean fixes are what work (a deterministic POLY, never a heavy vendor — `detmath` ended
up inert); route the poly at the PRIMITIVE level; a per-machine `Settings.ini` timestep is poison; unsequenced
evaluation order of RNG-consuming calls is a determinism bug class (MSVC/GCC right-to-left, Apple clang left-to-right
— this, not FP, closed `particles@29`); no off-wire carve-outs at the foundation; pin residuals by measurement.

**Stage 2 (the multiplayer build).** P1/P1B ControllerFrame replay + synctest · P2 A-D protocol → identity → loopback
session → **GNS transport** · P3 headless playable Controller-sync lockstep (full green, 3-platform) · P4A
player-facing MP UI + LAN alpha (3-platform green) · P4B render interpolation + nonzero input delay (matrix 16/16) ·
P4C N-peer 2→4 and modes (PvP, co-op PvE, PvPvE) · P5 snapshot / desync-heal / rejoin / replay · P6 LAN discovery ·
P7/P8 per-sender delay, honest pace, true 60 tps, local prediction · **H4 authenticated reconnect + host moderation
(active)** · the 2026-09-05/06/07 recovery milestone (restore fidelity, the Lua/native checkpoint contract, the script
graph, the contract audit) · the 2026-09-09→12 takeover (fix groups 1-6, the batteries, the families).

**Engine bugs this work flushed out** (they name the failure classes): `IsHumanTeam` as a per-peer sim mutation ·
missing join-tick controller quarantine · missing AHuman disabled-controller gates · native-AI `ReloadFirearms()` as
an off-wire sim mutation across 15 Lua sites · async `UpdateDrawMOIDs` racing render-frame MO draws through shared
static scratch bitmaps · a MicroPather infinite loop from a cost saturating to FLT_MAX · `RocketAI.lua`'s owner-side
`GibThis()` hidden by a `Vel.Largest > 3` short-circuit · resync/rejoin **double-spawning every actor**
sim-consistently (invisible to every hash gate; caught only by an actor census) · a leaver's stale socket killing the
survivors · `Activity::Create` dropping `SceneName`.

**Key measured facts** (they overturned earlier estimates): re-sim is **5.1 ms/tick ≈ 196 tps** (the old
"16.8 ms / 60 tps" was an MSPSU-clamp artefact, 3.3× off) → small local rollback is viable · live D=0 tick 12.4 ms, of
which 7.3 ms is the synchronous frame wait (~0 at D≥1) · matches run a true 60.0 tps · wire cost ≈ 184 B/tick/peer ·
snapshot 225 ms / 1.31 MB, sim-pure, byte-identical across peers.

# APPENDIX F — UPSTREAM AND SETTLED DECISIONS

**Upstream is HELD (user decision 2026-09-05):** no further PRs to CCCP until #283 merges. Do not open, re-cut or
propose upstream PRs; this is a standing instruction, not a pending task. `upstream/development` has had zero commits
since 2026-05-28 (the merge of our own #280). #283 is APPROVED + MERGEABLE and unmerged since June. PRs 5, 6, 7, 9, 10
are built and verified but held; PR 8 additionally needs a human vanilla-AI playtest; #279/#280 merged; #282 closed by
design. Per ADR-008 upstreaming is opportunistic, never a gate — the MP work does not depend on any of it.

**Architecturally rejected — do not bring forward, do not re-litigate:** M3 Q40.24 fixed-point math · the pathfinder
serial-epilogue (M4A) · the Race A consumption-side pathfinder serialization · the `detmath` musl vendor / FP-env as
baseline / scoped-libm · M5.5 mega-bundling.

**Do not re-apply the reverted off-wire fixes:** SpatialPartitionGrid MO-query sort, controller release-timer
sim-time, alarm-event sort, PathFinder node sort, Lua AI-aim `^`. Record: `xarch_diff\cs\NIGHT_LOG.md`,
`PRs\DETERMINISM_CS_KEEPSET_PACKAGE.md`.

**People.** Causeless — upstream lead; candid, high bar; his three directions are §1.2 rules 1-3. HeliumAnt —
reviewed and approved #283. getcetc (ooaaaoaoao) — FoW co-author. The user relays everything to and from Discord and
is the only outside channel.
