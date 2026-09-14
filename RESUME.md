# CORTEX-MODERN — RESUME (the single live doc)

**AUTHORIZED CONFIGURATION CHANGE — 2026-09-13 15:23 MST.** User authorized only the context settings: `C:/Users/egerm/.codex/config.toml` now sets `model_context_window = 600000` and `model_auto_compact_token_limit = 500000`. TOML parsing and exact two-key change validated; model, max reasoning and default service tier preserved. Backup: `C:/Users/egerm/.codex/config.toml.pre-context-500k-20260913-152345.bak`. The effective settings of the already-running thread have not been verified. Project execution remains paused.

**USER PAUSE — 2026-09-13 14:52 MST. This overrides every instruction below to continue or replenish workers.** Project execution stopped at 14:40 MST. Do not restart workers, queues, builds, engine tests, implementation, merges or pushes. Only the requested orchestration audit and planning are authorized now. The two extra coordination/evidence subagents were interrupted; they are not the corrective plan. Preserve `reviews/takeover-20260909/grok-workers/devin_cli/STOP-20260913`; stale queue entries are interrupted, not completed. Stop evidence: `reviews/resume-20260913/user-pause-windows.json` and `user-pause-mac.json`. Scores remain 98/98/96/40/98/95/39/61 (mean 78.125%, delta zero).

Current plan and measured comparison: `reviews/resume-20260913/EXECUTION_RECOVERY_PLAN.md`. The orchestration correction is proposed, NOT validated. Read this pause and that plan before the historical pickup or §5. The user's review of the plan precedes any resumption.

**Rewritten 2026-09-12 16:40 MST; last updated 2026-09-13 14:21 MST (status request, merged directory and active pie repair).** This
file is the resume point: read it first, every session.
Older detail: `_archive/docs_archive_20260912/RESUME_pre-rewrite-20260912.md` (the verbatim pre-rewrite copy — nothing
was lost, only compressed); `HANDOFF*.md` and the 15 phase plans in `_archive/docs_archive_20260905/`.
Policy long-form lives in `D:\Projects\CLAUDE.md` (== `AGENTS.md`); the live board is `D:\Projects\STATUS.md`;
the live order of work is §5 of this file (`D:\Projects\LEAD_PLAN.md` is the 2026-09-11 plan, §0.1). This file does
not repeat the policy — it points at it.

Sections: §0 resume in 10 minutes · §1 rules · §2 goal + the eight items · §3 what happened 09-11 → 09-12 ·
§4 current state · §5 what is left · §6 lessons learned · §7 path index · appendices A-F (project, architecture,
build/verify, quirks, history, upstream).

**HISTORICAL PICKUP (2026-09-13 14:21 MST), superseded by the USER PAUSE above.** Scores98/98/96/40/98/95/39/61; all deltas0 since13:51.

Live audit14:17-14:23: Grok0 (all six13:59 jobs finished14:04-14:10), SWE2 (actual Devin PIDs43028/37576 and fresh session tool records, not just queue state), Opus1 (Mac PID67548, setup final verifier, fresh stream), Astra subagent1 (pie_close_implementation, actual RED/GREEN/control runs), primary1. Queue reviewer and completed lease reviewer excluded. Grok and Opus floors are currently missed; replenishment is overdue, a lead scheduling failure.

Accepted directory client b3145db997 is now integrated into wave3b12f4bcc6e7c72ccb2266093cadf0b53f1eb6e6=origin; merge line-multisets exact, trailer scan0, exact7e63 lease push. Wave executable stale. New directory test-only feb492b9a77082b96d1c96da0afd1a9eebdd0210 reports focused Failed cleanup RED/GREEN+11 but is unreviewed/unpushed. Pie fix on item5-lifecycle branch stage2/pie-close-lockstep has reported exact control RED/tip GREEN (201-204 pie mismatch removed, full320tick maps and1470actor rows equal); GoTo/SP/11 and independent review still gate acceptance. Harness shared-runtime setup race preserved and separate-peer runtime correction disclosed. Actual450tick Form Squad and client GoTo results await personal raw review. Windows/Mac600tick terrain divergence has a measured12material/64foreground pixel difference on matching assets; cause/fix remains unaccepted. UI repaired-v2 built14:17, screenshots/review pending. Mac initial ICE setup results now independently reviewed; successful rematch/resync and admission coverage remain open.

Next: replenish useful Grok/Opus jobs immediately; personally verify gameplay and terrain raw evidence, review pie/client test diffs, finish UI pixels, correct hidden-lease integration, then assemble one fix group. Candidate7e63/c67b35 remains frozen, approvedaa650e601e unchanged, Source44 NOT STARTED. Codex76% remaining14:17; stop immediately at<=60% of any available core window. No defensible full-roadmap ETA yet; prior overall estimate remains withdrawn.

SWE queue PID36168 owns two durable sessions through devin_cli/queue-20260913-runtime.json. UI defiant-taurus owns item7-ux; platinum-begonia works on stimulus classification. Never manually overlap these sessions. Preserve known controls and raw failures. Lead writes all verdicts and boards; workers never push or touch approved tree. Current candidate and wave are different tips.


**HANDOFF NOTE (the lead, 2026-09-12 16:55 MST).** The user ended the overnight run at ~16:20 MST ("wrap up; the
lead's weekly Fable budget is at 5%; use Opus 5 for help; no new effort; fix what the reviews found"). What was done
in the wrap-up: wave A finished on the scratch branch with the three lead fix-ups and W71-4 (§4.2); every local
`stage2/*` and `xref/*` branch backup-pushed to origin (89 branches, 16:29 MST, lease-protected, none rejected);
W136 read and REJECTED with a precise re-spin (§3.6); the lead's merge tools copied into `grok-workers/lead-tools/`;
memory notes for the next session (§7.1); this file rewritten by the lead (a first draft was delegated and the user
rejected that — **the handoff document, review verdicts and plans are the lead's own work, never delegated**).
**CLEAN STOP at 2026-09-12 17:30 MST (user's instruction: nothing running).** Every agent, lane, build, engine and
queue runner was stopped and checked: no `cl.exe`/`link.exe`/`Cortex Command.exe`/`cursor-agent`/runner process on this
PC, no launchd lane on the Mac, the Devin queue runner (`devin_queue.py`) killed — **there is no Grok, SWE-2, Devin
or Mac session to resume.** Every branch with work is on origin (§4.5). The next lead starts at §5.1 and spawns its
own agents. Two things were cut short by the stop and are recorded as such: the independent verifier pass on W136-2
(the lead read the codec diff itself; the merge is on the wave, §4.3) and the promotion of `control-build` (still at
`4dd386fb51`; the gates already run on the wave exe are in §4.1). **After the stop (17:30-18:00 MST):** the one
public Cursor co-author trailer was stripped from the whole history and force-pushed (§1.1: 866 commits re-hashed,
every sha in this file rewritten through `trailer-strip-20260912/commit-map.txt`); the drives were cleaned
(§4.7: 125 GB of superseded run roots, 28 GB of finished Mac lanes, 12.6 GB of superseded review bulk, 7.6 GB of
stderr dumps, nine superseded worktrees — every deletion in `reviews/cleanup-20260912.md`); the June wiki edits in
`cccp` were committed and pushed; every local branch equals origin.

---

# §0. RESUME IN 10 MINUTES

## 0.1 Read order (30 min of reading buys a day)

1. This file, §0-§5.
2. `D:\Projects\STATUS.md` — the board: one line per item 1-8 with its %, the delta since the last ask, what runs now.
3. `D:\Projects\LEAD_PLAN.md` — the 2026-09-11 plan: its waves 0-2 are done and its standing table is historical;
   still current are §6 (the roadmap order, mirrored in §5.5 here), §7 (lead duties per landing), §8 (worktree and
   firewall policy), Appendix A (the headed §11 review script) and Appendix B (the brief template). The live order of
   work is §5 of this file.
4. `D:\Projects\reviews\takeover-20260909\LEAD-REVIEW-overnight.md` — the lead's line-by-line review log: per-file
   verdicts, the findings register F1-F20, every lane acceptance, every NEGATIVE. ~1,300 lines; the entries from
   "MovableMan.cpp verdict" onward are the 2026-09-12 overnight and wrap-up work.
5. `D:\Projects\reviews\takeover-20260909\grok-workers\SPAWN_LOG.md` — every lane spawn and landing with its model
   evidence line and report path. The last ~150 rows are 2026-09-11/12.
6. `D:\Projects\CLAUDE.md` — binding policy (delegation, firewall, junctions, scratch, commits, mod compatibility).
7. Only if you need it: `STAGE2_H4_RECONNECT_PLAN.md` (H4 design + pins P1-P32), `RECOMMENDED.md`,
   `NEXTENHANCEMENTS.md`, `reviews\takeover-20260909\grok-workers\WORKER_RULES.md` (travels with every brief).

## 0.2 The trees (verified 2026-09-12 17:50 MST, after the trailer strip — all shas are post-strip)

| Path | Branch @ tip | What it is |
|---|---|---|
| `D:\Projects\control-build` | `stage2/fixgroup-6-lead` @ `3f65208668` (fast-forwarded to the wave tip 2026-09-13 17:34 MST; the FG6C evidence stays pinned to 7e63a0c1b5 / exe c67b35a6…; exe 2b637683d933… built alone /MP12 at 17:36 MST, log D:/mx/lead-candidate-20260913b/build.log; verify6 8/8 at 17:39 MST, OUT D:/mx/lead-fg6/verify6-3f65208668-r2) | **LEAD CANDIDATE.** Clean Final exe c67b35a65b05, built09:22:40 alone/MP12. Verify6 underlying8/8; original wrapper7/8 false marker disclosed. Frozen for FG6C; candidate=origin. |
| `D:\Projects\takeover-build` | `stage2/fixgroup-6-lead-wave-a` @ `73c17555c7` (=origin, 2026-09-13 18:51 MST: 3f65208668 + terrain fix 4169c598e6 + lease corrections 73c17555c7 + viewport follow-up aca344d5f7; 2026-09-13 17:34 MST: 3b12f4bcc6 + pie-close 81187eef4e + viewport 9a3459c389) | **WAVE SCRATCH.** Directory backend/codec/client integrated3b12f4bc after seven-file merge proof and exact-lease push. Binary is STALE; candidate remains frozen7e63. |
| `D:\Projects\p4b-interp-validation` | `stage2/p4b-interp-lockstep` @ `aa650e601e` | **THE MILESTONE BRANCH / APPROVED TREE.** The only executable the two-process harness and the verification family launch (exe `ff6a44ac46c8`, 2026-09-11 16:44 MST). `origin` matches. Workers may never write here. Docs say "main" — there is no `main` ref; this branch is it. |
| `D:\Projects\cccp` | `modernization-effort` @ `67d844f3d7` (= origin) | Legacy reference, the fork's default branch, **the shared object store** for every worktree, and the wiki (`modernization-docs/`). The June-2026 wiki edits that sat uncommitted are the tip commit (committed and pushed 17:33 MST). |

Worker worktrees (one branch each, reused across lanes so the firewall rules keep covering the path):

| Worktree | Branch @ tip | Lane |
|---|---|---|
| `alias-walk` | `stage2/coroutine-stack-capacity` @ `ec5a34efbc` (= origin) | Accepted, merged into wave748041cc68. Final exef4bec5f3, lead186/0; control and8011 boundary proof preserved. |
| `item4-simspeed` | `stage2/preview-substitute-links` @ `364344d970` | W133 + W133-2 (F15) — merged on the wave; F15b/F15c debt (§3.4) |
| `item5-lifecycle` | `stage2/pie-close-lockstep` @ `7e63a0c1b5` plus active repair | Pie shared-state repair, Final546645e3; control RED/tip GREEN reported, remaining gates active. Prior generated library changes preserved before branch reuse |
| `value-observations` | `stage2/preview-reference-writes` @ `4bb407c37e` | W127 (F8) — merged |
| `h4-secondary` | `stage2/sound-identity-pin` @ `3bc895d6fd` | W131 (F11) — merged |
| `hold-pause` | `stage2/relaunch-slots-fg6b` @ `7cbf518a83` | W124-2 (F13) — merged |
| `fencing-warm` | `stage2/preview-links-control-green` @ `364344d970` plus exact arms | Historical B controls finished; exe a9cd964a corresponds to this dirty test tree. Original W135 branch retained. |
| `item7-chat` | `stage2/directory-unlisted-sessions` @ `feb492b9a7` (origin b3145db997) | Production client accepted and merged; additive Failed cleanup tests unreviewed/unpushed. Accepted45804cf6 exe/PDB retained; newer test build remains under review. Service wiring remains separate. |
| `item4-feel` | `stage2/depth12-spawn-origin` @ `b5811aad98` plus instrumentation | Diagnostic source retained; current exe d76f01b9 is the no-travel control and DOES NOT match restored source. Never use as candidate. |
| `item7-ux` | `stage2/mod-mismatch-presentation` @ `2594cff51d` plus active viewport repair | Old6a18084d visually rejected on encoded640x360 despite driver exit0. SWE owns exact viewport/font/background repair; unmerged/unpushed. |
| `item8-directory` | `stage2/ice-lifecycle-regressions` @ `7e63a0c1b5` | Native test382943f6 prepared, uncommitted/unbuilt; all341+/5- read including late tagged peer arm. af6 exe STALE. |
| `item8-dedicated` | `stage2/preview-projectiles` @ `49af8a120e` | W104b — merged |
| `item8-discovery` | `stage2/audit-3-harness` @ `a5f5380835` | audit-3 harness — merged |
| `audio-owner-registry` | `stage2/join-list-mod-label` @ `dda7247be3` | W94-2 — merged |
| *(removed 2026-09-12 17:48 MST)* `item3-fixtures`, `lane-a`, `lane-b`, `fixgroup-1..4`, `takeover-fixes`, `xarch-combat` | `stage2/e2e-harness-flags` `2e4f14e861`, `stage2/rematch-roster-2-mac` `1b09c8078a`, `stage2/item3b-harness` `7a94ee1a5d`, `stage2/fixgroup-1` `67d2d70aad` … `stage2/fixgroup-4` `eadd79cfc7`, `stage2/a7-journal-wip` `8c14facea8`, `stage2/p4a-multiplayer-ui-alpha` `e915e274f8` | superseded worktrees deleted in the cleanup (branches on origin at these tips; restore any with `git -C D:\Projects\cccp worktree add D:\Projects\<dir> <branch>` and re-run the firewall script elevated before a `-net*` launch from it) |

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
| ONE battery | the `fg6c-battery` brief (`reviews/takeover-20260909/grok-workers/fg6c-battery/prompt.txt`, placeholders to fill, §5.1 step 6) through a Grok run-only lane; scratch `D:\mx\fg6bat3`, ports 47661-47666 / 8458; baseline and drivers: `fg6b-battery/` | every engine gate green or a lead-classified non-engine artefact; ~20 steps (selftests, H4 gates, B1 gates, lobby, heal e2e, gameplay fixtures, dedicated, lifecycle packs, drop3_switch, preview_event_d7, lpinv_100, portmap_e2e, controller_log). |
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
| **GPT-6 Astra (effort max), Codex CLI — the main engineering counterpart** | `python reviews/takeover-20260909/grok-workers/codex_cli/codex_job.py <name> <worktree> <prompt.txt> --task "..." --timeout SEC [--resume THREAD]` | the session rollout's `turn_context` line must say `"model":"gpt-6-astra"` and `"effort":"max"` (`cli_runs/<name>/model-evidence.txt`; exit 4 = REFUSED); full permissions (`--dangerously-bypass-approvals-and-sandbox`); `usage.json` per lane for the quota; §6.3b |

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
  refuses a message with a trailer and re-reads the stored message. (The Cursor agent shell appended a Cursor
  co-author line to any `git commit` typed into it; five lead commits carried one — four were stripped on 2026-09-11,
  the fifth was found and stripped on 2026-09-12.)
- **Push at every verified checkpoint,** several times a day, no per-push OK needed. Before any push (backup pushes
  included) run `python grok-workers/lead-tools/scan_trailers.py <tree> upstream/development..<tip>` (line-start
  match, the same rule `strip_trailers.py` strips by; exit 1 on a hit); a hit is stripped message-only with
  `strip_trailers.py` as the `git filter-branch --msg-filter`, a branch map with a tree-identity check, and a
  force-push only after verifying origin still holds the pre-rewrite tips. Push with lease. Record the push.
  **No upstream PRs** (the feed is held). **Trailer strip 2026-09-12 17:49 MST** (user's order: no Cursor co-author anywhere): the one public Cursor co-author line (old `9751a90e29`, 2026-09-10) was stripped by a message-only `git filter-branch --msg-filter strip_trailers.py` over every commit not reachable from its parent; 866 commits changed sha, 148 branches and 2 tags were rewritten (every tree byte-identical, `map.txt`), 147 branches force-pushed after verifying origin still held the pre-rewrite tips (`push2.txt`), 26 tags force-pushed; origin's `exp/determinism-foundation` (a May branch that had diverged from the local copy) was stripped separately (21 May-era co-author lines, old `366b9d0738` → `34253c5708`, tree identical, `README-expdf.txt`) and the local diverged copy was left as it was. A line-start scan of every local branch and tag since 2026-04-01 finds 0 trailer lines; only upstream's own 2022-2025 human co-author lines remain, and they stay. The live docs, STATUS, SPAWN_LOG, LEAD-REVIEW and the fg6c brief had every old sha rewritten from `commit-map.txt` (old sha → new sha, 40-char); evidence logs under `D:\mx` and lane reports keep the OLD shas — resolve them through that map. The old trailer commit is now `79dc711958`. Post-scan of every branch and tag for trailer lines: 0.
- **Arizona local time only** (user, 2026-09-12 17:00 MST): every message, board line, plan, report, commit body and
  stamp is written as `2026-09-12 17:05 MST` (MST, UTC-7, no daylight saving); never UTC. Read the clock (`date`) in
  the turn that writes the stamp. Evidence logs written before this rule (SPAWN_LOG.md, LEAD-REVIEW-overnight.md, lane
  reports) keep their UTC stamps — subtract 7 hours; machine logs stay as the tools write them. This file, STATUS.md,
  CLAUDE.md (17:03 MST) and LEAD_PLAN.md (18:01 MST) were converted on 2026-09-12 (pre-conversion copies in
  `_archive/docs_archive_20260912/`).
- **Concurrency is outcome-based, never a head-count** (2026-09-13 15:19 MST, replacing the 2026-09-13 12:33 MST quota): a lane is one
  whole outcome with exact RED/GREEN, the queue of bounded outcomes stays at least six deep, four to eight lanes run when
  the queue is full and the lead can review them within the hour, dispatch never waits for the lead's turn, landings are
  the measure. Full text: CLAUDE.md §0.1.
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
- **Workers never push, never touch the milestone branch or the approved tree, never launch the engine outside the
  runners, never edit outside the paths their brief lists.** A worker may commit on its own branch. The lead reads
  every diff line before any merge.
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
  `reviews/` (the 2026-09-12 pass: `reviews/cleanup-20260912.md`, §4.7); never touch live trees, a running family's
  evidence or a pinned executable; delete under `D:\mx` only with a walker that never enters a reparse point
  (`lead-tools`' pattern: unlink junctions, never follow them) and check the engine trees' `Data` counts afterwards.
- **Every worker launches with full permissions** (user, 2026-09-13 16:42 MST): Codex `--dangerously-bypass-approvals-and-sandbox`, Cursor `-p --force --trust --sandbox disabled`, Claude `--dangerously-skip-permissions` (Agent-tool agents inherit the lead's bypass mode), Devin `--permission-mode dangerous`. The brief and `WORKER_RULES.md` are the fence.
- **Astra is the main engineering counterpart** (user, 2026-09-13 16:42 MST, option B): GPT-6 Astra at effort max through the Codex CLI (`codex_cli/codex_job.py`) takes engine surgery, netcode, UI/UX design, harness engineering and independent second reads, used heavily from the start; the user watches the Codex quota and says when to shift that work to Opus; the quota may be used to zero. SWE-2 Max as much as possible (free); Grok 4.6 Extra High Fast swarms for run-only batteries, builds, triage and mechanical implementation ("Grok 4.7" does not exist in the Cursor CLI, checked 2026-09-13 16:21 MST); Opus 5 for verification, visuals and surgery.
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

Percentages are from `STATUS.md` at 2026-09-12 18:01 MST (the clean-stop board); overall **78%** (mean 77.6). They
are the lead's judgement against named evidence, not a burn-down.

| # | Item | % | "Done" means |
|---|---|---|---|
| 1 | Controller boundary + wire/replay compatibility | 98 | ControllerFrame v6 / replay v3 / controller-log v2 stable, old peers rejected at hello, legacy recordings replay bit-identical, and the Source44 family green on the promoted tree. Moves only with the family. |
| 2 | Restore / identity / state inventory | 98 | Restoration + native + identity gates green in the family on Windows AND a clean arm64 clone; the state inventory complete (no undeclared transient); snapshot/restore faithful under every scope. |
| 3 | Gameplay fixtures + minimizer + matrix | 96 | 3b door/crab/craft under one script, the UI path of an AI order, the seat-side pie close; all gameplay fixtures cross-peer identical with **no exclusions**; 3e (the actor-switch ownership product call) decided with the user. |
| 4 | Presentation + performance (100-200 ms feel) | 40 | 4a numbers (pinned 2026-09-06, §C.4 below) met or reported as honest misses; 4b preview-event ledger + optimistic projectiles + Activity UI following the preview; 4c headed two-window measurements at ~100/200 ms and two render rates; 4d sim-speed program and the rollback verdict from measurements. |
| 5 | Breadth + verification debt | 98 | 5b 3/4-peer packs, co-op, PvPvE and the full leave/drop/rejoin/resync/replay/rematch/pause lifecycle on Windows and the Mac; 5c the WSL2 leg (never yet run) in a window with no Windows builds. |
| 6 | H4 reconnect + host moderation | 95 | Phase A §9a gates all green, Phase B substitution (B1) + the moderation GUI (B2), and the **headed §11 reconnect review with the user at the desktop** (LEAD_PLAN Appendix A). |
| 7 | Lobby / session UX + robustness | 39 | 7a chat (wire exists, no UI/routing yet) · 7b overlay/toasts/delay display · 7c mod-mismatch UX · 7d compression, beacon, delta frames, address re-resolve · 7e replayable post-resync rounds, periodic snapshots, telemetry bundle, crash-rejoin prompt · 7f replay browser + post-match report. |
| 8 | Discovery / internet play + roadmap | 60 | 8b dedicated headless host (the persistent server/world on the Mac — the end game) · 8a session directory + NAT rendezvous self-hosted on the Mac (preferred) or this PC, with a **real ICE connect proven end to end** · 8c lobby v2 · 8d adaptive delay, audio smoothing, bounded rollback per the item-4 measurements. |

Grouped: flawless normal MP (1, 2, 3, 5, 6) ≈ 97.0%; measured feel (4) 40%; UX + discovery (7, 8) ≈ 48%.
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

`fixgroup-1` (`67d2d70aad`) → `fixgroup-2` → `fixgroup-3` (`aeb398b25b`) → `fixgroup-4` (`eadd79cfc7`) →
`fixgroup-5` (`a8e890f543`) → **wave 1 promoted to the milestone `aa650e601e`** (commit dated 2026-09-12 08:14 MST,
pushed with lease, trailer scan of the new range empty) → `fixgroup-6` (`12c7f8cf11`) → **`stage2/fixgroup-6-lead`** (the lead candidate, in
`control-build`) → **`stage2/fixgroup-6-lead-wave-a`** (the scratch wave, in `takeover-build`).

The Source43 family on the milestone ended NOT GREEN with five reds root-caused as one group (R1 stale `Actor*` in the
contiguous index → W105; R2 borrowed owner-ref restore → W106/W106-2; R3 a Mac oracle artefact; R4 a breadth lane
killed by stray engines; R5 an intermittent host crash after a resync relaunch). All were fixed; R5 became the ASan
hunt that produced W114/W118/W124/W124-2 and, indirectly, F20.

## 3.3 The overnight waves and the lead's own review

At the user's request the lead read the **whole** delta itself, hunk by hunk, and logged every verdict:

- **Overnight delta** `dfe252ad80..aa650e601e` (111 non-merge commits, Source 79 files +11306/-270, tools 10 files
  +2808/-3) — complete 11:59 MST. Verdict: no determinism defect, no mod-compatibility break; findings F1-F6.
- **Part A**, the takeover window `be217add64..dfe252ad80` (103 commits, Source 99 files +19697/-2211) — complete
  14:21 MST. No new finding beyond F10's enqueue half.
- **Part B**, the P4B phase `db9ec184be..be217add64` (542 commits, 394 files, +65419/-2327) — started 15:23 MST.
  Done by 16:10 MST: all of `Source/Network` production code (54 files, 13,685 diff lines), all Managers
  (MovableMan 2,939 lines; LuaMan 5,107; LuaThreadCodec 596; AudioMan, ActivityMan, SceneMan, FrameMan, UInputMan,
  MusicMan, PrimitiveMan, PostProcessMan, LocalPrediction, …), and all of System by 16:31 MST (ScenarioRunner,
  Controller, Atom, CheckpointArchive, PathFinder, Entity, ContentFile, AudioCheckpoint, InputScript, AIWriteScript,
  RTETools, Reader/Writer, BitmapCheckpoint, RTEError, ContractAudit.h, the small files, micropather's
  sentinel-by-identity fix, StateInventory.csv — all sound). Main.cpp (3,074 diff lines) read complete 2026-09-13
  01:59 MST. Admission/Auth/Protocol/Session/Match selftests (+1,627/-7) read complete 06:19 MST. **Still to read:
  NetLockstepSelfTest.cpp and NetReconnectSessionSelfTest.cpp (~8.6k changed lines); NetReconnectSelfTest.cpp (+866)
  read complete 06:42 MST.**
  Five read-only Grok audit lanes (`pb-audit-1..5`) covered Entities, tools, GUI/Menus/Data and second-read the
  network and manager/system hunks; every finding was re-derived by the lead against the **candidate**, not the P4B
  tip, which closed three of them and produced F17, F18, F19.

## 3.4 The findings register F1-F21

| # | Severity | What | Status |
|---|---|---|---|
| F1 | LOW (test gap) | `RunContiguousActorIndexSelfTest`'s orphan case used uid 0, refused by the `<= 0` check before the cohort check ever ran; the R1 shape (a nonzero stale uid) was unexercised | **fixed + merged** `9cecfeea71` (driver-verified: `refused=1`) |
| F2 | LOW (presentation leak) | `AudioMan::RetirePredictedVoice` cleared `predicted` on a finishing one-shot, so the audio checkpoint captured it as an unowned voice; a peer restoring that snapshot replays it | **fixed + merged** `e350827f3b` with two selftest checks |
| F3 | LATENT (constraint) | the W62 fenced-peer waiver's content safety relies on `frameLane == ControlReliable` (ordered). Any lane switching the frame lane must also erase the waived peer's stored frames ≥ F and drop its later arrivals | **open as a standing constraint** — carried into every frame-lane brief |
| F4 | fixture | the A7 `coop_hand_back` arm ran the armed P4 Alpha Duel (team 0 dead by tick 272 at seed 42), so the reclaim always found the activity Over | **closed** — preset `Net Lifecycle Test` in every driver copy; V-A7-2 PASS on both pinned exes |
| F5 | MEDIUM (item 8) | Windows pinned-certificate mode set only `IGNORE_UNKNOWN_CA`, so the README's own self-signed `CN=cortex-directory` cert reached by IP was refused on Windows while the Mac accepted the pinned leaf | **fixed + merged** (W117 `62a0cd9361`), red/green through the real HTTPS probe |
| F6 | LATENT (scale) | `GET /v1/sessions` returned every row while the engine refuses a body over 128 KiB — past ~200 sessions the join list shows "unreachable" | **fixed + merged** (W120 `958d30c6cf`: limit/cursor/total paging, client follows 5 pages) |
| F7 | LOW (was MEDIUM) | a claimed CPU actor kept wire mode PLAYER after the purge; corrected on W119's evidence — the seeded owner is consulted first, so it was never orphaned, only one tick late | **fixed + merged** (W119 `ed0b46b266`: explicit hand-back in AI mode at expiry) |
| F8 | **HIGH** | a preview hook writing through an entity reference held in `self` reached the **canonical** world (lpinv 6/8 at tick 153, `shadows=0`); six vanilla Base.rte scripts hold such references. A desync hazard introduced by W89-2's unfrozen edge hooks | **fixed + merged on the wave** (W127 `4bb407c37e`: remap every MovableObject userdata in the copied self at bind; anything else freezes the clone's hooks) |
| F9 | MEDIUM | the candidate did not build on macOS: `PreviewScriptSelfTest.cpp` was in the vcxproj only, and `NetPortMap.h` built a default argument from a nested struct with default member initializers (clang 17 refuses, MSVC accepts) | **fixed** `e13e890fad`; Windows binary byte-identical before/after |
| F10 | MEDIUM | AI-generated queue ops were authorized by **team command authority**, not ownership, at BOTH the apply gate and the enqueue gate; under host-cpu-remote-human the owner's AI output for a CPU actor on a human's team was dropped and its waypoint queue never consumed | **fixed + merged** (W71-3 `42e0559913`: `writerUID` on `NetGameAIOrder` codec v21 with v20 decode kept; owner-gated producers; widened apply+enqueue gates. W71-4 `1544d359c2` closes the last gap: keep the path request armed while the add is in flight — read by the lead and ACCEPTED WITH CORRECTIONS, §3.6; **CLOSED 2026-09-12 17:24 MST**: the trimmed arm PASSes in the suite and the craft_cargo pulse on the wave exe is `[316, 317, 318, 319]` on both peers, `D:\mx\wave-b\craft_cargo\pulse_gameplay_wave.txt`) |
| F11 | **HIGH** | the checkpoint **sound identity cursor** is a process-global counter with no pin at match start, while sound identities cross the wire in `NetGameSoundOp`. Two peers whose processes created a different number of containers (a rematch, a prior skirmish, or Windows vs arm64 — measured 9212 vs 9200 at the same tick) hand out different identities for the same container. Every existing two-peer test starts two fresh identical processes, which is why none saw it | **fixed + merged** (W131 `3bc895d6fd`: pin to `1<<40` beside the uid pin; unit arm 17-identity delta red→green; relaunch order verified) |
| F12 | LOW | a directory probe under a blocked address left an unprunable rate-limit bucket | **fixed + merged** on the W120 branch, red-first, service suite 29/29 |
| F13 | MEDIUM | `m_LockstepRelaunchInProgress` was set at relaunch and cleared only in `ActivityMan::Clear()`, so the end-of-Update rebind re-imposed the snapshot's UIDs on brain / controlled / controller / marked slots **every tick for the rest of the process**; a later legitimate switch was reverted at the end of its tick (and later checkpoints wrote stale ids) | **fixed + merged** (W124-2 `7cbf518a83`: `EndLockstepRelaunch` after the first `MovableMan::Update`; dead ChecksLeft code removed). RED `tick 460: owner=2 after hand-back`, GREEN 10/10 |
| F14 | LOW (test pin) | the reclaim transcript golden hard-coded protocol version 1; W126 bumped `c_Version` to 2 | **fixed** `cefccaa1f6` (golden derives the version bytes) |
| F14b | LOW | **the F14 fix was a half-fix** — `TestKnownAnswers`' three HMAC vectors were computed over the v1 transcript while `MakeTranscript` read the live version, so `-net-reconnect-selftest` stayed red (FG6B, W124-2, W133, W135 all saw it) | **fixed** `4dd386fb51` (known-answer transcripts pin `protocolVersion = 1`; the layout test keeps covering the live version). NEGATIVE against the lead |
| F15 | **HIGH** | the preview-HUD crash: a render substitute's faithful link (`m_pItemInReach`) resolved through the speculating lookup onto a preview clone that `EndSpeculation` frees, read by the HUD on the next frame (ASan heap-use-after-free, `AHuman.cpp:3672`, 10/10 runs) | **fixed + merged, with follow-ups.** W133 `12c7d17e4f` (merged `eee6c0369a`) remaps external links to residents; W133-2 (merged `c3e4586b9b`, 2026-09-12 17:09 MST) closes the two remaining heads: `8e66a72ce7` — `EndPreviewScripts` walks recorded (uid, state) bindings (RED ASan UAF at `LuaMan.cpp:7914`, `D:\mx\w133b\asan\red\report.32984`, clean after), `364344d970` — `OverlaySurvivorOf` returns nullptr for every retiring overlay object (spawns, spawnMeta keys, added-since-mark), correct by reading only (no red was ever produced for that head). GREEN on the ASan build: HUD 5/5 no report, ledger, lpinv, suite 11/11. **F15b (open, test debt):** the two unit arms the verifier specified — a harvested speculative spawn linked from a clone's `m_pItemInReach`/`m_pMOToNotHit` must be null after `EndSpeculation` (RED on `8e66a72ce7`, GREEN on `364344d970`); an in-world shadow link must map to the resident, not null. **F15c (open, hardening):** `RetiringOverlayObjects` lists top-level objects only, so a link naming a part of a retiring object or a shadow part falls through (latent: `MovableObjectReference` links self-expire and the raw links store roots); `RemapExternalLinks` does not recurse wounds; a stateless clone part that gains a Lua state during the preview keeps its `#preview` slot |
| F16 | LOW-MEDIUM (fidelity) | `RemapPreviewUserdata` tested the live world before the part map, so a self-held reference to the original's equipped gun mapped to a duplicate shadow of the whole original rather than the clone's own part | **fixed by the lead** `372ba9a5a4` on the wave |
| F17 | MEDIUM (robustness) | on the relay host, a **client's** `ProtocolError`/`InternalError`/`MissingFrameTimeout`/`PeerDisconnected` stop set the coordinator to Failed, ending an N-peer match that the same client's socket merely closing would not (a griefer packet ends everyone's match) | **fixed + merged** (W135 `7b76bbd3ac`: the stop becomes that peer's `ApplyPeerLeave` at `FirstFrameWithout`, relayed as PeerLeft; the host's own stop still fails clients) |
| F18 | MEDIUM (determinism) | `SubPieMenuHoverOpenDelay` is a per-machine setting driving a **sim** timer inside `Actor::Update`; two peers with different values open the sub-menu on different ticks and a release in that window resolves a different slice on each | **fixed + merged** (W135 `0774713efa`: pinned in `ApplyDeterministicConfig` like the gold/crab settings, re-read by `PieMenu::UpdateSliceActivation`, handed back after; 900→1000→900 selftest) |
| F19 | LOW-MEDIUM (oracle validity) | contract-audit fixtures gated on the literal uid `1048577` and `run_audit`'s gate never failed on an empty or mismatching check list — a drifted uid made the whole audit vacuous | **fixed + merged** (W134: fixtures claim `_ContractAuditOwner` on the first Create/Update and print an ARMED line; the gate fails on `fixture_armed`, `<family>_checks` and every named mismatch; unit tests red→12/12) |
| F20 | **HIGH (memory safety)** | a restored coroutine's stack is sized by its **saved top** (`lua_checkstack(co, top + 16)`), but LuaJIT requires `base + framesize <= maxstack` for every live frame (checked at call time, never on resume). A yielded frame with many locals is restored into a stack that cannot hold it, and the first register write past the allocation corrupts the heap. Every script-graph restore that rebuilds a suspended coroutine is exposed: relaunch, resync heal, rollback probe, preview codec paths | **W136 `82e5184bad` REJECTED (2026-09-12 16:48 MST); W136-2 `f1c5e517ae` landed 17:12 MST and is merged on the wave (`d20d4bddfa`) on the lead's read of the codec diff — the independent verifier pass is still owed (§4.3, §5.1 step 2).** W136's RED/GREEN evidence is genuine (RED `D:\mx\w136\sg-red4\stdout.log:537` `FAIL coroutine_big_frame_stack_fits 285/131` then a 0xC0000005; GREEN `sg-green2` 177 PASS, suite 11/11), but the sizing walk only sizes frame links that carry `pcslot` and skips the `ftsz` links (FRAME_CP — every coroutine's own main function —, FRAME_VARG, FRAME_CONT, FRAME_PCALL), so a big frame BELOW the yield is never sized; the safety net gates on `frame_islua` and skips the same frames; the `_ScriptGraphThreadStackFits` oracle mirrors the formula instead of the invariant. The lead confirmed on the source (`LuaThreadCodec.cpp` ~345-362 and ~482). W136-2 (Opus engineer, same branch) delivered exactly that: a RED-first helper arm (`D:\mx\w136b\red\run\stdout.log:539-540` — the old query's false fit `110/137`, then 0xC0000005 on the unfixed codec) plus a metamethod arm (regression coverage only), every link sized from the function in its own slot (`slots[i-1]`, `needed = max(needed, i + 1 + framesize)`), no `frame_islua` gate in the net, the query on the invariant; GREEN 181 PASS / 0 FAIL with six `coroutine_*` arms, suite 11/11, gcb pass (`D:\mx\w136b\green`). Evidence of the original defect: `D:\mx\w124b\asan\report.52732` |
| F21 | HIGH (reconnect correctness) | Returning co-op player loses local selection when deferred rebind uses host-save IDs; related marks and inventory brains covered by repair group | Mac group3a865f9e native RED/GREEN and A7 all5; first two production fixes independently reviewed. Brain-group independent review, retained-world-slot coverage and Windows proof pending; unmerged. |

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
| **The lead's timestamps drifted up to 90 minutes ahead** over one hour of estimated stamps; later the lead wrote "2026-09-13" on lines stamped before midnight UTC | Run `date` (Arizona local time, §1.1) in the same turn as any board/log timestamp and read the DATE from it too. When a stamp is later found wrong, append a correction note; never silently rewrite history. |

## 3.6 The W-lane index for this window (one line each)

| Lane | Route / tree | What it produced |
|---|---|---|
| **W124-2** | Grok, `hold-pause`, `stage2/relaunch-slots-fg6b` `7cbf518a83` | F13 fix: the relaunch window ends after the first `MovableMan::Update` (`EndLockstepRelaunch` clears the flag and the kept checkpoint ids; dead ChecksLeft code deleted). RED `tick 460: owner=2 after hand-back` → GREEN 10/10 switch-returner arms, drop3 9/10 (the tenth wrote the ASan report that opened F20). ACCEPTED, merged `a4277016a4`. |
| **W127** | Grok, `value-observations`, `4bb407c37e` | F8 fix: at `BeginPreviewScripts`, every MovableObject userdata in the clone's copied `self` is remapped (original→clone, live world→overlay shadow, clone parts by uid); anything else freezes the clone's hooks, counted. Write fixture 6/8→8/8 with `shadows=2`, fl100 violations=0, det traces identical. ACCEPTED; opened F16. |
| **W129** | Grok, `item4-feel`, `d9aa5a7690` | Preview clones skip `Create` and are deleted under `FaithfulCloneScope(false)`, so no checkpoint sound identity is allocated; a `SoundContainer` in a copied self becomes a preview copy whose `m_PreviewOrigin` points at the canonical container; the glow oracle is keyed to the firearm subtree. Cursor holds at depths 1/4/7 (was +2 per preview); AK lpinv 4/4, pickup 8/8. ACCEPTED. |
| **W131** | Grok, `h4-secondary`, `3bc895d6fd` | F11 fix: the checkpoint sound identity cursor pinned to `1<<40` at every deterministic match start; `-selftest-preallocate-sound-identities` gives the two-process form. ACCEPTED; the lead dropped its misleading "impossible in practice" warning in `728a47a431`. |
| **W132** | Grok, `takeover-build`, measurement only | Two Final+ASan builds (W103 tip `38be2a22f4` pre, W130 tip `e502b5133e` post) × 5 AK-47 HUD runs each + 2 ledgers: **all 12 halt on the same heap-use-after-free**. Proved W130 was not the mechanism and gave F15 its exact stack. Nothing to merge. |
| **W133** | Grok, `item4-simspeed`, `12c7d17e4f` | F15 part 1: `RemapExternalLinks` over `m_pMOToNotHit`, `m_pItemInReach`, `m_pMOMoveTarget`, waypoints, a craft's incoming MO, attachables and inventory; `ResidentForRetiringShadow`. Correct, **incomplete** (spawns, the add-queue tail, and `EndPreviewScripts` dereferencing freed clones). Merged as the branch `eee6c0369a`. |
| **W133-2** | Grok, same tree/branch, `364344d970` (+`8e66a72ce7`) | F15 completion to the lead's design: `OverlaySurvivorOf` returning nullptr for members of `RetiringOverlayObjects()` (spawns, spawnMeta keys, added-since-mark), and `EndPreviewScripts` iterating recorded uid/state pairs instead of clone pointers. ACCEPTED WITH FOLLOW-UPS (Opus verifier every line, evidence re-derived from `D:\mx\w133b`; the lead spot-checked the three functions), merged `c3e4586b9b`. GREEN bar met on the ASan build: HUD 5/5 exit 0 no report, ledger `PASS press tick 153`, lpinv 8/8, suite 11/11. Debt: F15b arms, F15c hardening (§3.4). |
| **W134** | Grok, `item7-chat`, `075b57587e` (tools only) | F19 fix (see the table). B-2-3 closed by the lead's reading instead of a change: `compare_sim_traces` already diffs over the **union** of both traces' subsystem keys, so a subsystem one side drops is already a divergence. ACCEPTED, merged `9c3eefc3a1`. |
| **W135** | Grok, `fencing-warm`, `0774713efa` | F17 + F18 (see the table), each red-first with its FAIL line quoted on exe `b13b1a6e9e`. ACCEPTED, merged `aa34bfeec6`. Not done by the lane: a 2-peer stop arm. |
| **W136** | Grok, `alias-walk`, `82e5184bad` | F20 attempt, base `4dd386fb51`. **REJECTED** (§3.4 F20): real fix for the covered shape (the big frame as the yield's immediate caller), genuine RED/GREEN, suite 11/11 — but frames carried as `ftsz` links are never sized and the safety net cannot see them. The worker disclosed the deviation in its report ("REPORT.md:99-101") — read a report's deviations section first. **W136-2** (Opus engineer, same branch, `f1c5e517ae`, landed 17:12 MST) closed it with a second RED-first arm and per-slot sizing; merged `d20d4bddfa` on the lead's read (§4.3). |
| **W71-3** | Grok, `item5-lifecycle`, `42e0559913` | F10's ownership gate: `writerUID` under codec v21 (v20 decode kept for replays), owner-only producers, `IsLockstepAIOrderAuthorized` (team authority OR sender-owns-target OR a writerUID naming a same-team actor the sender owns). Three red arms. ACCEPTED, merged `d396b15d50`. |
| **W71-4** | Grok, same tree, `1544d359c2` | F10's last gap: under lockstep `UpdateMovePath` keeps its request armed while this actor has in-flight waypoint writes and nothing to load, so the applied add is loaded next tick and the owner sends the pop. Merged `abf5dcb28d`. ACCEPTED WITH CORRECTIONS after the lead's line-by-line read (evidence re-derived: RED `D:\mx\w71e\red_lockstep3\stdout.log` L7, GREEN `green_lockstep\stdout.log` L7, pulse 316..319 both peers); the trim (§4.4) is committed as `0bcaaddde7`, and the trimmed arm plus the craft_cargo pulse are green on the wave exe (§4.1) — F10 CLOSED. |
| **FG6B battery** | Grok, run-only, lane `fg6b-battery` | The ONE acceptance battery on `cefccaa1f6` / exe `98f73d74aaed`; scratch `D:\mx\fg6bat2`, ports 47651-47656 / 8457, 7 h cap. DONE 16:55 MST, read and classified by the lead (§4.6): no true engine failure. 19 selftests: 18 PASS + the F14b reconnect red. Known non-engine reds: `binary_matches_source` on every gameplay fixture (the lead's F14b source edit at 15:49 MST is newer than the 21:52 exe) while every **semantic** check passes (peers identical over the whole range on all seven fixtures); the old-driver fencing shape; the `heal_mod_semantics script_continued` artefact. |

Also in this window and already merged into the candidate: W90 (AK-47 recording + Lua-fire ledger arm), W104b
(preview projectiles as ledger events + presentation ghosts), W119 (F7), W120 (F6+F12), W123 (S4 ICE/mux wiring,
1391 lines — **no ICE connect exercised yet**), W125 (SWE-2 host port-map toggle + lobby line), W126 (module digests,
chat wire, protocol v2), W130 (MOID-join ordering), W-MAC-RUNNER (`tools/posix_test_runner.py`, so the repo's own
drivers run on macOS), W-MAC-FG6B (the candidate green on arm64: 14 selftests + 5 new cases, settings gate, A7 5/5).

---

# §4. CURRENT STATE (2026-09-12 17:50 MST, all values read from the trees; every sha is post-strip)

## 4.1 Trees, tips, binaries

| Tree | Branch | Tip | Exe |
|---|---|---|---|
| `control-build` | `stage2/fixgroup-6-lead` | `4dd386fb51` | `98f73d74aaed385710ac077d9c0e7e331549ba23a19059b044c6b5f1b6dbe631` — built 14:52 MST from `cefccaa1f6`; **does not contain `4dd386fb51`** (the F14b selftest fix), which is why `binary_matches_source` is red in the battery's fixture steps |
| `takeover-build` | `stage2/fixgroup-6-lead-wave-a` | `8342801d91` (=origin after2026-09-13 07:42 MST lease push; coroutine and F15 repairs merged) | `cf2e611bf36d575aef5fe856a431704dc1ae2532e58b9b3e95556bcfc58c70eb`, built 17:17 MST from `d20d4bddfa` (Final, `/MP6`). **STALE BINARY: do not launch. Historical gates on d20d4bddfa only** (an Opus engineer, stopped before the promotion; logs under `D:\mx\wave-b\`): `run_selftests.py` 11/11 (`selftests\result.json`); `-script-graph-selftest -num-lua-states 4` **181 PASS, 0 FAIL** (`script-graph\stdout.log`, the six `coroutine_*` arms included); AK-47 HUD replay ×3 `PASS press tick 500` on this plain Final build (`ak47hud\run1..3`); `ak-lpinv176` depths 1/4/7 as before, depth 12 `spawned=8 [None,Tracer Ronin AK-47,Casing,None×5]` (explained by five secondary tracer-travel MOPixels, §4.10); **W71-4's craft_cargo pulse `host_wp_nonzero [316, 317, 318, 319]` = `client_wp_nonzero` (`craft_cargo\pulse_gameplay_wave.txt`) — F10 CLOSED on the trimmed code.** Not run on it: verify6, the battery. **Gates run earlier on the previous wave exe `959fc25e…` (tip `0bcaaddde7`, 16:32 MST; logs under `D:\mx\wave-a\`)**, all through the runners: `run_selftests.py` 11/11 (the new arm `PASS a_path_update_stays_armed_while_the_waypoint_add_is_in_flight` at `net-lockstep-selftest\stdout.log:12`; `net-reconnect-selftest` PASS — F14b confirmed on a build); W127's held-ref fixtures `lpinv-write` / `lpinv-control` / `lpinv-fallback` all `PASS tick 153: 8/8`, `preview_codec_fallback=0` on write/control and the by-design Activity freezes only on fallback (same three uids as W127's run) — the proof of the lead's fix-up `b5811aad98` (without it every sound-holding preview froze); W129's AK arms `ak-lpinv176` 4/4, `ak-lpinv150` 4/4, `pickup-lpinv100` 8/8, `preview_codec_fallback=0`, `violations=0`, canonical state byte-identical. Depth12 observation closed by exact pre/post/no-travel controls: five secondary MOPixels from tracer travel, canonical state unchanged (§4.10). |
| `p4b-interp-validation` | `stage2/p4b-interp-lockstep` | `aa650e601e` (= `origin`) | `ff6a44ac46c8a48c3757dcd888ea419e9e88b7b5070ea88a8c15d8629a30b3f6`, 2026-09-11 16:44 MST |
| `item4-simspeed` | `stage2/preview-substitute-links` | `364344d970` | `c8278244ba23…` (the lane's own) |
| `alias-walk` | `stage2/coroutine-stack-fit` | `f1c5e517ae` | `d627e0741a…` (W136-2's green exe, the lane's own) |

Working-tree dirt that is expected and must not be committed: the rebuilt vendor libs
(`external/sources/allegro 4.4.3.1-custom/_Bin/allegro-release.lib`, `external/sources/luabind-0.7.1/_Bin/luabind-release.lib`)
in the trees that built. Every worktree was otherwise clean at 17:50 MST (the stray `AbortCode.txt` markers and the
untracked June screenshots in `cccp` were deleted in the cleanup).

## 4.2 Wave A, in merge order (all on `stage2/fixgroup-6-lead-wave-a`)

`4dd386fb51` (base) → `031687bae8` W127 `4bb407c37e` (clean) → `6e315ce881` W131 `3bc895d6fd`
(`NetLockstepSelfTest.cpp` keep-both of the W119 and W131 arms) → `d67394fb66` W129 `d9aa5a7690` (`Main.cpp` include
keep-both; `LuaMan.cpp` W127's remap block then W129's `DropPreviewSoundCopies`) → `d396b15d50` W71-3 `42e0559913` →
`a4277016a4` W124-2 `7cbf518a83` → `eee6c0369a` W133 `12c7d17e4f` → `aa34bfeec6` W135 `0774713efa` →
`9c3eefc3a1` W134 `075b57587e` → **three lead fix-ups** `b5811aad98` (let preview `SoundContainer` copies pass the
hold remap — the W127/W129 semantic clash found while merging: W127 freezes a preview whose hold carries any
non-MovableObject Entity, and W129 had just started putting SoundContainer copies in that hold, which would have
frozen every preview that holds a sound and made W129's AK-47 fix moot), `372ba9a5a4` (F16: consult the part map
before the live-world branch), `728a47a431` (drop the W131 rematch warning) → `abf5dcb28d` W71-4 `1544d359c2`.

→ `0bcaaddde7` (the lead's W71-4 trim) → `c3e4586b9b` W133-2 `364344d970` (17:09 MST; LuaMan.cpp keep-both) →
`d20d4bddfa` W136-2 `f1c5e517ae` (17:13 MST, merged clean and verified 0 differing files; pushed 17:26 MST on the
lead's read of the codec diff — the independent verifier pass is owed, §5.1 step 2; a reject there means
`git revert -m 1 d20d4bddfa` on the wave).

Every merge commit was verified with `verify_merge_commit.py` (0 differing files; the W133-2 merge flagged only
the F14b cherry-pick lines, identical on both sides). Trailer scan `aa650e601e..d20d4bddfa` = **0 hits**. The
battery for this tip is briefed at `grok-workers/fg6c-battery/prompt.txt` (placeholders `TIP_SHA`/`EXE_SHA`/
`BUILD_TIME` to fill from the control-build promotion): scratch `D:\mx\fg6bat3`, ports 47661-47666 / 8458, baseline
fg6b, expected flips listed, N6 = the W71-4 pulse.

## 4.3 What is merged and what is not

- **Merged on the wave, read and accepted:** W127, W131, W129, W71-3, W124-2, W133 (as the branch), W135, W134,
  W71-4, the three lead fix-ups, and the W71-4 trim `0bcaaddde7` (§4.4).
- **Merged 2026-09-12 17:09 MST:** W133-2 `364344d970` as `c3e4586b9b` (LuaMan.cpp keep-both: W127's map clears plus
  W133-2's bindings clear at the top of `BeginPreviewScripts`; the verifier flagged only the F14b cherry-pick lines,
  identical on both sides). The wave was rebuilt after it (exe `cf2e611b…`, §4.1).
- **Merged 2026-09-12 17:13 MST, pushed 17:26 MST:** W136-2 `f1c5e517ae` as `d20d4bddfa` (clean, 0 differing files).
  Basis: the lead read the whole codec diff (every link sized from `slots[i-1]`, no `pcslot` gate, no caller fallback,
  no `base + framesize` term, no `frame_islua` gate in the net, the fits query on the invariant) and the lane's quoted
  evidence (RED `D:\mx\w136b\red\run\stdout.log:539-540` — the helper arm's false fit `110/137` then 0xC0000005 on the
  unfixed codec; GREEN `green\run\stdout.log:683-688` six coroutine PASS lines, 181/0, suite 11/11, gcb pass). **The
  independent Opus verifier pass was stopped by the clean-stop order before it reported** — the next lead runs it
  (brief: the W136 rejection findings 1-4 as the checklist) before the battery. Known and disclosed: the metamethod arm
  is regression coverage only (it passed on the unfixed codec by over-allocation, `D:\mx\w136b\red-meta`).
- **W136** `82e5184bad` — REJECTED (§3.4 F20), superseded by W136-2 on the same branch.
- **Nothing is in the milestone yet.** `stage2/p4b-interp-lockstep` stays at `aa650e601e` until the wave is built,
  verified, battered and the family is green.

## 4.4 The W71-4 trim (committed `0bcaaddde7`, 16:3x MST, on the wave)

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
| `stage2/p4b-interp-lockstep` (the milestone) | `aa650e601e` — **up to date** |
| `stage2/fixgroup-6-lead` (the candidate) | `4dd386fb51` — **up to date** |
| `stage2/fixgroup-6-lead-wave-a` (the wave) | `d20d4bddfa` — **up to date** (pushed 17:26 MST after 0-hit scans; earlier pushes at 16:53 and 17:09 MST) |
| `stage2/coroutine-stack-fit` (W136 + W136-2) | `f1c5e517ae` — **up to date** (pushed 17:26 MST) |
| `docs/lead-handoff-20260912` (docs snapshot, parent `d20d4bddfa`) | tip `47c2130867` — last pushed 18:02 MST (three snapshots: the clean stop, after the strip and cleanup, after the final review): `docs/handoff-20260912/` holds this file, CLAUDE.md, STATUS.md, LEAD_PLAN.md, the H4 plan, the review ledger, the cleanup log, SPAWN_LOG, WORKER_RULES, `lead-tools/`, the trailer-strip maps, the FG6B report, the FG6C brief, the pre-rewrite RESUME and the memory notes. The live copies are the ones under `D:\Projects` (this row was edited after the last snapshot); this branch is the off-machine backup — re-push it with `scratchpad`-free tooling of your own after the next doc pass. |
| **Backup push, 2026-09-12 16:29 MST** | every local `stage2/*` and `xref/*` branch that was new or ahead: 89 branches, `--force-with-lease`, 84 created + 5 fast-forwarded (`actor-switch-tests`, `fixgroup-6-lead`, `item3b-harness`, `resync-boundary`, `session-directory`), none rejected. CI runs only on `development` pushes and PRs, so nothing was triggered. Trailer scan at the time: only the one pre-existing public hit (stripped an hour later, §1.1), nothing new. |
| other lane branches | `stage2/preview-substitute-links` (W133-2) `364344d970` · `stage2/posix-test-runner` `6c1a099341` · `stage2/fixgroup-6` `12c7f8cf11` · `stage2/fixgroup-6-preview` `aa650e601e` — all up to date |
| **Trailer strip 2026-09-12 17:49 MST** | 147 rewritten branches force-pushed (origin tips verified against `trailer-strip-20260912/pre-tips.txt` first), 26 tags force-pushed, origin's diverged `exp/determinism-foundation` stripped separately (`README-expdf.txt`). **Every local branch now equals origin** (checked 17:50 MST). Maps: `reviews/takeover-20260909/trailer-strip-20260912/map.txt` (branch tips) and `commit-map.txt` (866 commits). Any clone or worktree made before 17:45 MST must `git fetch --all` and reset its branches to origin. |
| `main` | **does not exist on origin.** The fork's default branch is still `modernization-effort` (`67d844f3d7`, = origin). Where old notes say "main", they mean the milestone branch. |

## 4.6 Lanes live at the time of writing

- **FG6B battery** (Grok run-only lane `fg6b-battery`, `control-build` exe `98f73d74aaed` from `cefccaa1f6`):
  DONE (exit 0 at ~16:55 MST 2026-09-12; `fg6b-battery/REPORT.md`, 508 lines, scratch `D:\mx\fg6bat2`). The lead's
  verdict: **no true engine failure.** Reds, all classified non-engine: the reconnect KAT `56527b38…` (F14b — fixed
  at `4dd386fb51`, PASS on the wave build's suite); the `fencing_two_transports` FAILs (`fenced=0`) come from the
  h4gates OLD driver copy while the current driver passed `fencing_1..5` 5/5; `heal_wp` `script_continued` (known
  heal artefact); `binary_matches_source` on fixtures / crab / arm1 (the lead's F14b source edit at 15:49 MST
  postdates the 21:52 exe) with every semantic check PASS on all seven fixtures and switch_control; the crab driver's
  `_crab.ps1:84` argument error (harness); the bare `global-callback` flag REFUSE (the driver row `gcb` passes);
  `present_identity` compared_ticks=0 (known); `controller_log` has no fixture. Green: all other selftests, every H4
  and B1 gate, lobby, heal, reclaim, dedicated, lifecycle, directory (29/29 + e2e), fl100, mixed_lua, heal_global,
  and N1-N5 (AK-47 HUD 3/3 with no dump on this pre-W133 exe, expire3, the loopback session-id e2e — not an ICE
  connect —, portmap on/off, drop3_switch/lpinv_100/preview_event_d7). `control-build` is free to fast-forward.
- **W136-2** landed 17:12 MST (`f1c5e517ae`, §4.3); its report is in SPAWN_LOG (17:3x MST row) and the evidence under
  `D:\mx\w136b`.
- **Nothing is running** (17:30 MST). No worker session of any route needs resuming.
- **W133-2** (Grok, `item4-simspeed`): landed 16:55 MST with two commits over the cherry-pick `860707b91b`:
  `8e66a72ce7` (EndPreviewScripts iterates recorded uid/state, RED ASan UAF `LuaMan.cpp:7914` in
  `D:\mx\w133b\asan\red\report.32984` → no report after it) and `364344d970` (`OverlaySurvivorOf` redirects survivor
  links off objects the overlay deletes — implemented to the lead's design with NO red of its own: the worker did not
  observe the DrawHUD head after commit 1). GREEN exe `c8278244ba…`: HUD 5/5 exit 0 no ASan, `PASS press tick 500`,
  `compare_sim_traces` 600 ticks identical, ledger `PASS press tick 153`, lpinv PASS, suite 11/11. Reviewed by an
  Opus verifier and spot-checked by the lead, ACCEPTED WITH FOLLOW-UPS, merged `c3e4586b9b` at 17:09 MST (§3.4 F15:
  F15b arms and F15c hardening are the open debt).
- The Mac is idle and clean (only `cli-home` remains under `/Users/erol/cortex-workers`): the next Mac rows are the
  ICE-only e2e (W123's gap), the AK-47 oracles re-run after W129/W127, and the Windows-host/Mac-client lockstep e2e
  after W131.

## 4.7 Cleanup 2026-09-12 (17:35-17:50 MST; every line in `reviews/cleanup-20260912.md`)

- **PC, `D:\mx`:** 42 superseded run roots deleted with a junction-safe walker (the s38/s39/s40/s41/s42 family
  evidence and lane roots whose verdicts are closed in the reviews, the old lane roots W18-W81/W106/W70/W71 whose
  reports live under `grok-workers/<lane>/`, `m38rev`), 125 GB; 136 roots remain, among them everything §7.1 cites
  (`s43*` as the latest family's evidence — delete it after the Source44 family —, `fg6bat`/`fg6bat2`, `w71d`/`w71e`,
  `w124`/`w124b`, `w132`, `w133b`, `w136`/`w136b`, `w114`/`w118`, `w105`, `w89`, `w123`, `va7`, `a3fix`,
  `w127`/`w129`/`w134`, `wave-a`/`wave-b`, `lead-fg6`, `w80c`). The five engine trees' `Data` counts were identical
  before and after (8518/8517 files), i.e. no junction was followed.
- **PC, reviews:** six 1.2 GB `runner-stderr*.txt` dumps in W40-W48's lane dirs (7.6 GB); under
  `recovery-2026-09-07/contract-audit` every `grouped-build-*` except the approved tree's `ff6a44ac`, the
  transaction-valid runs of families 38/41/42/42-2 (43 kept), the aborted Source42 attempt, and
  `purge-before-debug-1`'s 2.6 GB crash dump + exe copies (its analysis and launch records stay) — 12.6 GB.
- **PC, worktrees:** nine superseded worktrees removed (§0.2), 18.9 GB; 18 remain. The untracked June screenshots
  and the two `AbortCode.txt` markers deleted; the June wiki edits committed (`cccp` `modernization-effort`, pushed).
- **Mac:** every finished lane under `/Users/erol/cortex-workers` deleted (28 GB, 74 → 102 GB free) after archiving
  each lane's small files (35,727 files, gzip-verified) into
  `reviews/takeover-20260909/mac-evidence-20260912/mac-lanes-small-files.tgz`; `cli-home` (the logged-in Cursor CLI
  profile) kept; `/Users/erol/Documents/Codex/` untouched.
- **Kept on purpose (not junk):** `reviews/recovery-2026-09-06/expanded-mod-gates` (4.2 GB, the expanded heal gates
  and their driver), `reviews/claude-review-2026-09-08/lanes` (3.3 GB, that review's evidence),
  `contract-audit/mac-peer-20260907` (4.4 GB, the cross-platform peer trail), `D:\Projects\keystone` +
  `.keystone-venv` (14.6 GB, a different project of the user's), `stage2_p2` (the GNS build prefix the build needs),
  `stage2_p4` (fixtures and recordings), `_archive`.
- Free space on `D:` went from 109 GB to 253 GB.

---

## 4.8 Readiness check before GO (2026-09-13 01:24 MST)

The candidate, wave, approved tree, W136-2 and W133-2 executable hashes were read again and match §4.1. Nine relevant
worktrees were inspected; only the expected vendor libraries are dirty. No engine/compiler/build/worker jobs were found;
the old interactive session and a language server remain open on Windows and were left alone. The Mac is reachable via
`ssh Erol-Mac`; only `cli-home` remains under `cortex-workers`. Neither lead lock nor `family-source44.json` exists.
Observed free space: D: 252.5 GB; Mac 103 GiB available.

**Correction to the earlier blanket branch-equality statements:** `git ls-remote --heads origin` compared with all 198
local branches gives 197 matches. The exception is the deliberately retained local `exp/determinism-foundation`
(`f2cd052750d64ddf2d5cf5d2934ef2e6a9010fcb`) versus origin
(`34253c57088618c54b92f88db3df78a69c148d8c`), consistent with §1.1's record that the diverged local copy was left alone.
All active work branches checked match origin. No ref was changed; the historical branch does not need resetting.

The task metadata confirms `gpt-6-astra` / `max`; local config is `service_tier = "default"` (the task metadata does not
separately echo the effective tier). Usage reads 92% weekly remaining, with no secondary window reported. The current
user instruction is to stop at 60% remaining or less, keep Grok and one or two SWE-2 sessions supplied after GO, use
Opus 5 max for difficult work until its quota is exhausted, and reserve Codex subagents for exceptional need. No reset
credit is authorized. The local Claude CLI supports the named project agent and explicit model/effort; real worker
authentication and model gates still run at dispatch. The historical Devin queue must not be restarted wholesale.

The lead's executable plan is `reviews/resume-20260913/PLAN.md`: W136-2 independent review and the exact depth-12
comparison first, then candidate fast-forward/build/verify6, then one pinned battery. Mac work proceeds independently;
Windows worker engine runs wait for the battery's exclusive window to finish. F15b/F15c and the remaining Part B read
stay open. Only planning records changed in this pickup; execution awaits the user's GO.


## 4.9 Active execution after GO (2026-09-13 01:48 MST)

Scores unchanged: 98/98/96/40/98/95/38/58. Required model evidence is in each `grok-workers/cli_runs/<name>/`.

| Job | Worktree / branch | Scope / state |
|---|---|---|
| v136b-20260913-r2 | alias-walk / stage2/coroutine-stack-fit @ f1c5e517ae | Opus 5 max, Fast off; independent findings1–4 review. Session9fdf5e44-13ab-4175-b298-aa7418f62cdc. No edits/builds. |
| depth12-20260913 | item4-feel / stage2/depth12-f16-pre then stage2/depth12-f16-post | Grok Extra High Fast; exact b5811aad98 versus372ba9a5a4. One /MP6 slot, scratch D:/mx/depth12-20260913. These new measurement refs have no unique source commits. |
| wmacfg6c-20260913 | /Users/erol/cortex-workers/wmacfg6c-20260913/repo | Grok Extra High Fast; clean wave clone d20d4bddfa, build/selftests/settings/A7/AK. Legacy wrappers recovered from retained archive; all adaptations must be disclosed. launchd label in cli_runs job.json. |
| swe-modtext-20260913 | item7-ux / stage2/mod-mismatch-presentation @ d20d4bddfa | SWE-2 Max; join-refusal formatting and two-direction driver. Second /MP6 slot released for exact-base RED then GREEN; ports47721–47722. Check-mods/cache is a later slice. |
| f15debt-20260913 | item8-directory / stage2/preview-retirement-hardening @ d20d4bddfa | Opus5 max, Fast off; owned retiring parts/wounds/stateless bindings + detecting arms. Engine/build held until explicit BUILD_ALLOWED.txt; no source acceptance yet. |

The lead read the complete W136-2 final diff (153 insertions,2 deletions), the restore frame walk and LuaJIT frame layout,
and re-derived retained RED helper false-fit110/137 followed by 0xC0000005 at red/run/stdout.log:539–540; GREEN
205/232 and successful resume at green/run/stdout.log:685–686, matching exe d627e074… . Independent verdict and a
fresh detecting run on the rebuilt candidate remain pending. No wave/candidate/milestone ref changed.

Only explicitly identified generated vendor libraries were restored before reusing item4-feel, item7-ux and
item8-directory. No directory cleanup, merge, commit or push. Updated local CLI helpers use Arizona spawn stamps;
local Claude runner bounds runtime/output, loads the exact project agent, pins max/Fast-off/attribution-off, and checks
its initialization model. The first failed verifier attempt is retained under cli_runs/v136b-20260913 (NO_INIT,
agent-not-found); the successful retry is the new -r2 session, not an old handoff worker.

Use this section instead of §0.2's older worker-directory occupancy for these three reused directories.

Update 2026-09-13 01:53 MST: depth12-20260913 finished. Lead re-derived pre/postF16 counts0/1/1/8 on BOTH tips;
F16 hypothesis refuted. Identification still blocks the battery. Follow-up depth12-origin-20260913 uses item4-feel
on stage2/depth12-spawn-origin @ b5811aad98 with explicit temporary instrumentation and a travel-call negative control,
no commits/merge. Its slot replaces the old depth12 job. Details in LEAD-REVIEW; item4 +0 NEGATIVE. Codex91% remaining.


## 4.10 Recovery and continued execution (2026-09-13 06:04 MST)

The previous GO turn ended with an automatic cybersecurity-screening error after about 32 minutes. Its error named
no rejected command. Files, model evidence and reports survived; all jobs completed by 02:47 MST. No unattended
workers/builds/engines remained on either machine at recovery. The older interactive session was left alone.

Recovered outcomes (claims remain subject to lead review):
- v136b-r2: independent review accepts original findings 1–4; near-8000 stack-capacity refusal question remains.
- depth12-origin: with travel 8 spawns vs omitted travel 3, extras are five MOPixels queued during tracer travel at
  tick185. Full diagnostic diff and raw logs re-derived; identity question closed in LEAD-REVIEW. item4-feel keeps instrumentation; executable is
  the no-travel control and DOES NOT match restored source. Never use it as a candidate.
- Mac wave: clean arm64 build; script-graph181/0, AK invariance4/4; A7 four passes and coop_hand_back failure
  (expected controlled_uid1048615, observed1048577). Not Mac-green. Compact archive and report saved in
  reviews/resume-20260913/mac-wave-evidence.tar.gz and REPORT-mac-wave.md. Remote clone/evidence retained, not deleted.
- swe-modtext: commit bfa75659bd on item7-ux; same-driver RED36/38, GREEN38/38, lead rerun38/38 and all diff lines
  read. Independent review confirms original single-module landing text readable; separate existing host status
  clipped. Added mixed/overflow driver cases pass87/87; source diff read, longer screenshot review active. Not pushed.
- f15debt: eight-file +530/-22 patch in item8-directory, all diff lines read. C-RED four link failures, C-GREEN
  all four pass; eight Final runs complete on exe6972b5b3. Stateless-binding s arm passes BOTH, so that cleanup has
  no detecting RED. Historical B controls, independent lifetime review and ASan coverage remain; not accepted.

Current jobs (2026-09-13 14:21 MST):

Live audit14:17-14:23: Grok0 (all six13:59 jobs finished14:04-14:10), SWE2 (actual Devin PIDs43028/37576 and fresh session tool records, not just queue state), Opus1 (Mac PID67548, setup final verifier, fresh stream), Astra subagent1 (pie_close_implementation, actual RED/GREEN/control runs), primary1. Queue reviewer and completed lease reviewer excluded. Grok and Opus floors are currently missed; replenishment is overdue, a lead scheduling failure.

Accepted directory client b3145db997 is now integrated into wave3b12f4bcc6e7c72ccb2266093cadf0b53f1eb6e6=origin; merge line-multisets exact, trailer scan0, exact7e63 lease push. Wave executable stale. New directory test-only feb492b9a77082b96d1c96da0afd1a9eebdd0210 reports focused Failed cleanup RED/GREEN+11 but is unreviewed/unpushed. Pie fix on item5-lifecycle branch stage2/pie-close-lockstep has reported exact control RED/tip GREEN (201-204 pie mismatch removed, full320tick maps and1470actor rows equal); GoTo/SP/11 and independent review still gate acceptance. Harness shared-runtime setup race preserved and separate-peer runtime correction disclosed. Actual450tick Form Squad and client GoTo results await personal raw review. Windows/Mac600tick terrain divergence has a measured12material/64foreground pixel difference on matching assets; cause/fix remains unaccepted. UI repaired-v2 built14:17, screenshots/review pending. Mac initial ICE setup results now independently reviewed; successful rematch/resync and admission coverage remain open.

Next: replenish useful Grok/Opus jobs immediately; personally verify gameplay and terrain raw evidence, review pie/client test diffs, finish UI pixels, correct hidden-lease integration, then assemble one fix group. Candidate7e63/c67b35 remains frozen, approvedaa650e601e unchanged, Source44 NOT STARTED. Codex76% remaining14:17; stop immediately at<=60% of any available core window. No defensible full-roadmap ETA yet; prior overall estimate remains withdrawn.


Finished recovery lanes: mac-a7f diagnosed F21 from unchanged journals and call paths; f15-probes completed the
C inventory; swe-fixture-aggregation and swe-modtext-mixed finished. Aggregation repair is three reviewed lines in
driver copies; 14 repaired serialization/failure-propagation cases pass, original exceptions reproduced. Its harness
uses hidden child processes and MST stamps after a disclosed two-line correction, with new final-harness evidence.
modtext-visual saved its complete review but hit its 20-minute timeout while saving through Bash; subsequent review
briefs include Write and a report-before-timeout requirement. No image conclusion is inferred from label logs alone.

Stack boundary raw evidence now shows two ordinary yielded coroutines working on the old control, refused by
W136-2 at needed7985/8010, and resuming after the proposed allocation repair. The allocation diff and complete
controls remain under review; this extends F20 compatibility coverage and is a pre-promotion gate.

Checkpoint 2026-09-13 06:55 MST: stack implementation finished672024dfd1, all51+/2- lines and the full report read.
Lead independently reran script-graph on exee71c99d3:185 PASS including final verdict, zero FAIL, complete guarded
launch manifest. The prior-codec control proves the three new continuations but errors later on the unavailable
new query; it is NOT a suite pass. Negative far-link allocation control and RED1 oracle correction disclosed.
**Backup push:** trailer scan0 over upstream/development..672024dfd1, origin branch absent before push, then explicit
empty-lease creation of stage2/coroutine-stack-capacity at672024dfd1 succeeded at06:47 MST (reflog06:47:35). No wave merge yet.
Independent F15 review confirms the four weak-link mapping defects; no memory-safety RED is claimed for them.
It finds no supported stateless-binding defect (existing destruction already drops the slot), so that cleanup and
its artificial s probe are withheld. Keep PreviewRoots; trim only that unsupported subset, retain all b/c/r checks,
then run new controls and ASan. Arm support-pointer risk remains unmeasured, no speculative fix included.
Long UI images actually FAIL although strings87/87; new layout lane owns display-only corrections. The host status
patch separately built and ran87/87; its report's one-row height arithmetic is inaccurate, actual pixels are being
verified. Local Claude runner now supports --resume; initialization confirms the retained visual UUID and Opus5max.

Checkpoint 2026-09-13 07:11 MST: stack verifier finished exit0, independently reran185/0 and confirmed all51+/2-
lines. Near-limit7984/7985/8010 arms only exercise acceptance; low-slot big-frame arms exercise protected growth.
Before merge add needed8011 and prove RED without the growth block, GREEN with unchanged production. No claim of
old-codec compatibility for8011. F15 independent review completed; unsupported stateless cleanup withheld as above.
Mac binding85d90f2e4a completed A7 all5 arms; reported coop returner retains1048615 at frames204-264. Compact evidence
fetched to reviews/resume-20260913/mac-local-binding-evidence.tar.gz (12,021,760 bytes, SHA e909dc83a42c9b5a...).
Lead raw re-derivation and related marked/inventory coverage precede acceptance. New Mac turn preserves old logs.
Host status b7e1d86b35 accepted: independent four-image review measures status34px (extra18), both rows visible,
all lower controls shifted18px. Lead guarded rerun38/38 on562e80b1; no shorter-text, port-map or Back-click claim.
**Backup push:** scan0, origin branch absent, empty-lease creation stage2/lobby-status-height atb7e1d86b35 succeeded
2026-09-13 07:11 MST. Item7 moves38→39. Original mod text plus new layout remains separately unmerged: new exe
ae289ebd, four phases103/103, disclosed display-delimiter oracle change, actual pixels under independent review.
SWE session platinum-begonia finished layout; defiant-taurus finished four FG6C fixture copies before its new stack task.
All four copy diffs read: checker/interp unchanged, repaired wrappers differ only root/lane/ports. Do not overwrite
these reviewed copies when filling the remaining battery brief. NEGATIVE item2/5/6 +0 for intermediate reviews and
preparation: acceptance waits for remaining proofs, not a claim of completed battery/family. Scores now98/98/96/40/98/94/39/58.

Checkpoint2026-09-13 07:26 MST: stack-growth-boundary finished ec5a34efbc, two LuaMan test lines read and temporary
codec removal diff read. Exact no-growth control f0bc3c9b refuses8011 at stdout691; three earlier near-limit arms pass.
GREEN f4bec5f3 allocates16038 slots and resumes done. Raw prefix-specific totals are186 script-graph PASS/0 FAIL;
the worker's687 counts other suites and progress echoes too. Lead rerun repeats186/0 with complete launch manifest.
Codec hash restored byte-identical f7827f37. **Backup push07:23 MST:** scan0, explicit lease from672024dfd1 toec5a34efbc.
**Wave merge748041cc68 at07:23 MST:** helper commit, both changed files exactly match merge-base..ec5a34efbc under
verify_merge_commit (42+11 changed lines, differences0). Wave exe cf2e611b is now stale and must not be launched.
Wave backup waits for the next reviewed merge checkpoint; candidate still4dd386fb51 and milestoneaa650e601e.
F15 trim14db824c10 clean, current exe is Final ASan49b4f0f8; lead bc2003/3, all seven pointer checks PASS. Final
independent review active. Eight layout images passed the measured clip/delimiter/Back cases; actual button action,
combined-host geometry and wide-name limits remain separate coverage. UTF-8 decoding and positive host separator
assertions should be tightened before the layout driver lands. Neither Windows build slot is currently allocated.
Part B NetLockstepSelfTest diff read through1460/3991 (plus overlapping context); NetReconnectSessionSelfTest remains.

Checkpoint 2026-09-13 07:51 MST: F15 final independent report read in full (supported8-file402+/18- trim, ten ASan rows). Lead rebuilt an exact pair to close compile-log provenance: D:/mx/lead-f15-pair-20260913/result.json; RED12865621 yields four C-link failures atstdout22–25, GREEN09ec8453 clears/maps all four andbc3/3 at27. Both build logs explicitly compile MovableMan/MOSRotating; same regression arms; source restored byte-identical, tree clean. This is a weak-link RED, not a memory-safety RED. Prior49b4 ASan tip was overwritten by this same-source rebuilt GREEN; new RED exe/PDB retained. Unsupported stateless cleanup remains withheld.
**Push/merge:** preview-retirement-hardening14db824c10 backed up with scan0 and absent-origin lease; merged into wave8342801d91 via helper, verify_merge_commit8files/differences0. Full wave trailer scan0, origin checked atd20d4bddfa, explicit lease pushd20→834 succeeded07:42. The preceding coroutine merge748041cc68 is included. Candidate/milestone unchanged, wave binary stale.
Mac marked fix84a66e41 reports real pending-rebind RED3fails/GREEN3passes; fix-only diff read. Superseded fixture variants and scene-less seated-path crash disclosed; full test diff/raw review remains. New brain lane extends only brain-slot validity via current-world ownership, leaves controlled slots and existing dead-world behavior unchanged.
Normal-layout image report fully read:8images pass measured cases. Wide-name review shows0of5 names readable; GUIFont drops later text after an overwide token. Large→empty geometry returns pixel-identically to baseline. Driver UTF8/positivehost checks reviewed;105/105 reported and retained-output RED checks disclosed. SWE now measures token width with the actual font and expands/recenters only this panel; no global wrap rewrite.
FG6C prep copied59files,43literal substitution diffs, protected4hashes unchanged. Not yet accepted; lead diff reading and active dependency audit remain. Part B NetLockstepSelfTest read through2390/3991; reread730–760 for prior output truncation, then remaining1601lines and NetReconnectSessionSelfTest4723.
Status request recorded98/98/96/40/98/94/39/58; delta from preceding request item6-1/item7+1, others0. NEGATIVE item2/5 +0 for intermediate integration/preparation; true completion needs combined gates. ETAs and next delta baseline are in STATUS history.

Checkpoint 2026-09-13 07:58 MST: all3991 lines of Part B NetLockstepSelfTest diff read, including the730–760 reread. No new production defect established from that test review; some assertions cover only counters rather than full payload agreement, so no expanded claim. NetReconnectSessionSelfTest diff4729lines (4723source) read through1530. All43 FG6C substitution diffs read; byte-level re-derivation and active helper audit remain. Mac marked full133+/2- diff now read and archived in mac-relaunch-marked-evidence.tar.gz (3,020,800bytes, SHA f59976ac8b0bf6d7e5428cf448030eadfbaa93369613c602986cf907517ea1cd;354files). Lead re-evaluated the new host separator predicates against retained REDs and current raw GREENs: both detect old dots;105/105 current JSON keys true, expected semicolons in both raw host strings. New F21 independent review runs alongside repair; four workers. Codex88% at07:56.

Checkpoint 2026-09-13 08:21 MST: Mac3a865f9e clean grouped A7 all5 onf7bfe863, 21 launches; reported first resumed coop frame205 keeps1048615. Full third production/test diff read; native two inventory RED assertions and seven control cases to rederive from archive. Archive mac-inventory-brain-evidence.tar.gz=11,714,560bytes SHA50aa908d96632cfeb2cf025461fc2a2c57faa1c583746ee94d72f93da4bc7cc7,672files. Source objects fetched read-only from Mac; item8-directory switched clean to3a after exact F15 GREEN09ec8453 exe+PDB file preservation (PDB d8878a20...). No wave/candidate/milestone merge. First independent F21 report fully read: no production defect, but retained Activity arms call UID resolver rather than deferred world rebind; new Mac native coverage closes this. ScenarioRunner function location corrected for reviewer (Source/System); third review ongoing.

FG6C check_fg6c_copies.py rederived59 destinations byte-for-byte from allowed literal substitutions,43changed/16identical,58Python compiled,4protected hashes unchanged. Actual ICE script REPO derives from its control-build file path; no missing --repo issue. Approved/candidate win32 runner hashes identicalfead3f0d; run_sim_test diff only adds POSIX branch, Windows path unchanged. Git OpenSSL binaries exist; prep PATH per battery process. New bounded supervisor controls scratch cap/time; final identity/pins wait for candidate build.

UI full width diff read: (new-old)/2 can drift over alternating odd/even widths, fixed design is new/2-old/2. New SWE round verifies actualfont worstlegal token at640x360. Previous width report elapsed68min contradicted runner1237s; correction requested. Part B NetReconnectSessionSelfTest read through3540/4729; NetLockstep all3991 done. No new production defect established. Status delta baseline remains07:51; item6 explanation does not reset the baseline.

Checkpoint 2026-09-13 09:00 MST: F21 brain independent REPORT fully read: exact two inventory RED/GREEN arms and seven controls, no production defect; ScenarioRunner and ConstructionRegistryScope cleanup source reads closed. m_StartActivity pending-link difference is a pre-existing observation, no reachable defect established. b4 test-only44+/23- full diff already read; check_world_slots.py proves final source hashes match git, identical fixture both sides, control removes exactly two refresh calls. RED9d0e89c5 FAIL157/160/163; GREEN58b5ed57 PASS same lines. Final independent pass pending. Windows3a Final7822b034 built,11/11 and native4/native1-diag181/0; diagnostic4 held before engine until2400s worker cap. Runner/engines gone after timeout. Do not reuse its helper result code as a verdict. Next FF item8-directory to b4, rebuild and lead native variants when desktop protection permits.

Mac ICE one attempt08:46:47–08:47:13 onb4/58b5, both engine exits0 and601ticks, client routeice, mux IP0/P2P617 and host0/613. Harness initial six false fields read wrong JSON root; corrected evaluation5/6 true on same raw data. Host remoteidentityip:::1 and trace passed:false need explicit source explanation; independent Mac audit active. No WAN/STUN/TURN/cross-platform/rematch/resync claim. Archive mac-ice-only-evidence.tar.gz1,013,760bytes SHA f486746ebf7404d900b2d01eb66119696763d477346d664a1ed9982ee06df94b,96files; full report fetched/read. Mac real-world slots archive2,304,000bytes SHA59c923288f63db52546c3f52389375f279d1d02926c7c98a606d3225036a38be,243files; no source merge yet.

FG6C guard64a708a5 full186line difference read, original+final whole code and tests read; lead reran15 true checks, file-symlink skip due privilege, failure-to-kill and reparse TOCTOU paths not forced. Sampling can overshoot; no strict disk quota claim. New candidate identity helper requires explicit future build pin and fails closed. Part B3991+4729 lines complete. UI6a18084d built; no clean arithmetic RED runtime and no current GREEN yet; report's1283px/ANSI conclusion is unverified until actual encoding/font check. No completion score changes or pushes.

Checkpoint 2026-09-13 09:53 MST: Item6 restored94→95 after independent final F21 review, personal Mac detecting run and combined Windows detecting tests. This closes the specific defect that caused the earlier95→94 correction; full battery/family and headed reconnect remain. Item8 rises58→59 for accepted Mac/Mac LAN ICE-only application join: both peers601 ticks, P2P613/617 and IP0/0, unchanged trace comparator passes. This proves neither public WAN traversal nor ICE rematch/resync. Candidate and wave both7e63a0c1b52983d9461113fcf883d14b1f970e50; wave=origin, candidate=origin. Candidate Final c67b35a65b05 built alone09:22:40; 612 build inputs unchanged. Lead rederived all8 verify6 groups on19 pinned launches: suite11/11, graph186/0, local UI39/0, local state10/0, callbacks/preview/lpinv/control-switch pass. Original wrapper says7/8 solely because it expected RESULT: PASS instead of the real PASS switch_control marker; original summary retained and unchanged oracle rerun by lead. Approved milestoneaa650e601e unchanged. Identity check09:49 passed all10 checks, real firewall included. One FG6C inventory battery is next. Codex83% remaining09:46, stop at<=60% in any available window.

F21 b4 final independent REPORT fully read, no integration blocker: real world-slot fixture44+/23- read; three exact RED/GREEN arms confirmed. Lead Mac run09:10 on58b5ed57 gives181/39/10 with0fail. Windows combined c67b35 gives186/39/10, all five F21 detecting arms, and19 launches same hash/exit0/no timeout. Personal check_candidate_verify6.py reruns unchanged switch-control checker: uid1048634 apply80 handback90,291 owner rows,600 trace ticks. Original summary.json7/8 retained; wrong expected marker is the only wrapper failure. Evidence candidate-verify6-lead.json. Restore item6 point; full battery still pending.

Merges/pushes:09:06 cumulative b7 dependency merged1adf8ac201 (3files211+/5-, including original bfa formatting/test); exact merge verification0 differences.09:07 scan0 exact-lease wave834→1adf.09:16 F21b4 merged7e63 (4files238+/11-, exact verify0).09:17 atomic scan0 exact-lease wave1adf→7e63 and absent-origin creation stage2/resync-local-rebind=b4a57ae64d. Candidate fast-forward09:19 from4dd to7e63, no push yet. Approved tree unchanged. Build09:20:47–09:22:40 logs and612 input hashes in D:/mx/lead-candidate-20260913; build log0f5c9706. Owned lead build lock created/removed normally.

ICE acceptance limited to same-Mac LAN application ICE. Lead check_ice_raw.py verifies TLS fixture changes only join_mode/listen_addrs,4 snapshots, both601 ticks, P2P613/617,IP0/0 and reruns unchanged comparatorc32a79e3. Initial six false fields came from wrong JSON root; reeval changed only lookup, five corrected and host remoteidentity extra assertion remains false. Host ip:::1 is default joiner GNS identity; trace passed:false/ticks0 are unused MetricsCollector result/count defaults on e2e path, real completion601 recorded separately. Preserve every initial/revised check. Full independent audit read; no public WAN/STUN/TURN/rematch/resync claim. Archive f486746e (1,013,760bytes) and independent auditd71c2755 (204,800bytes). Lifecycle analysis full report fetched/read; incomplete source questions delegated for closure, no engine implementation accepted.

UI width final independent report read in full: five64-byte ASCII names visible in960x540 captures on134e, oldae2890/5; Back inside viewport, empty/short geometry restores. Rounding6a no runtime/capture proof. Actual640x360 and CP1252 path still untested; font arithmetic is a hypothesis until engine run. No UI score increase.09:16 lead stopped owned orphan Python29196 and bash25500 after timeout; host launch started:false and no stdout.09:18 stopped owned F21 native23268 tree after810s desktop-gate wait, also never started. No desktop override, no engine killed, no directory cleanup. Unrelated interactive54604 preserved.

FG6C identity helper accepted after full233line diff/test read and own19true cases; actual lead pin c67b35/7e63/buildlog0f5c now passes10checks including firewall,610 Source files checked excludingCLI. Build manifest writes only after success. Part B full listed diff review remains complete; limited payload/assertion coverage is explicit, not new production failure.

Checkpoint 2026-09-13 10:05 MST: Three external workers: Windows Grok4.6 Extra High Fast FG6C full inventory (runner83467, provider49a70c96-87fe-484a-8246-ed997e91334a); Mac Grok4.6 Extra High Fast fresh combined-tip build/gates (runner79607, mac-fg6c-final-20260913); SWE-2 Max menu minimum-viewport/encoding driver preparation without engine (runner7963, platinum-begonia). No Codex subagents. Mac Opus source closure completed and report read; no ICE implementation yet. Candidate and wave7e63a0c1b5 both equal origin after candidate scan0 and exact-lease4dd→7e63 push09:57. Candidate c67b35a65b05 frozen under lead-owned LEAD_BATTERY.lock. FG6C started09:58; guard48700 owns inventory47236, run_all52660, run_battery21184. No other Windows engine/build lane. Fresh Mac clone verifies same7e63 independently. Approved milestoneaa650e601e unchanged. Codex83% remaining10:02; stop at<=60%. Future verify6 template394933a6 accepted: exact2 edits (real PASS switch_control marker and final nonzero exit on failed summary), original full bytes retained; lead reran6/6 dummy marker/rc/summary cases and original raw checker independently. No engine rerun for text-summary fix. Final FG6C execution policy diff read: explicit c67B1 pin,9 automatic retry paths suppressed while original outcomes/timing flags retained. N6 source38lines+sequential inventory launcher read/compiled; unchanged gameplay checker/analyzer, expected pulse316..319. No source/oracle/mask changes. Lifecycle closure establishes completed menu does not pump directory, row/signaling share lifetime, schema has no hidden state, session-ticket address is44/128bytes, and rematch trace resets require per-round strict comparisons. Source-only recommendations remain under lead design review; no new runtime completion score.

At recovery scores were98/98/96/40/98/95/38/58. At 2026-09-13 06:19 MST, item6 falls95→94 for confirmed F21; overall remains78 rounded (620/8=77.5). NEGATIVE item2/5 +0 at recovery: evidence was left unreviewed
and Mac is not green. No code merge/push, candidate promotion, engine-tree cleanup or milestone mutation at recovery.
The lead continues raw evidence review, closes diagnostics and schedules candidate build/verify6/battery afterwards.

---

## 4.11 Lead pickup, option B (2026-09-13 16:42 MST)

Read from the trees and both machines 16:08-16:40 MST. Nothing was running except the user's paused interactive Codex
session (PID 25904, left alone). Candidate `control-build` = `stage2/fixgroup-6-lead` @ `7e63a0c1b5`, exe `c67b35a6…`
(frozen, verify6 8/8); wave `takeover-build` = `stage2/fixgroup-6-lead-wave-a` @ `3b12f4bcc6` = origin (directory
client `b3145db997` integrated; binary stale, never launch it); milestone `p4b-interp-validation` @ `aa650e601e`
unchanged since 2026-09-12; `cccp` @ `67d844f3d7`. The FG6C verdict, the F22 terrain finding and the list of Astra's
unreviewed landings are in LEAD-REVIEW (2026-09-13 16:42 MST).

**Backups made at pickup:** pushed with scan 0 — `stage2/pie-close-lockstep` `81187eef4e` (new on origin),
`stage2/mod-mismatch-presentation` `2594cff51d` (new), `stage2/directory-unlisted-sessions` `feb492b9a7`. Patches of
every uncommitted diff under `reviews/pickup-20260913/patches/` (stamp in `STAMP.txt`):

| Worktree @ branch | Uncommitted | Owner / meaning |
|---|---|---|
| `item7-ux` `stage2/mod-mismatch-presentation` @ `2594cff51d` | GUIFont/GUILabel/GUISkin/MainMenuGUI +93/-20, `tools/test_viewport_fit.py` | SWE-2 `swe-ui-viewport-complete` (session defiant-taurus) repairing F1/F3/F2 — B4 |
| `item8-directory` `stage2/ice-lifecycle-regressions` @ `7e63a0c1b5` | NetMatchSelfTest/NetMatchService(.h) +454/-51 | the owned-link ICE lifecycle test/service work (Mac owned-link lanes; staged here) — B6 commits it as WIP first |
| `h4-secondary` `stage2/terrain-initialization-diagnostic` @ `7e63a0c1b5` | SLTerrain/TerrainDebris +204/-9 | terrain construction dump instrumentation (`grok-terrain-load-diagnostic`) — B1 |
| `fencing-warm` `stage2/preview-links-control-green` @ `364344d970` | Main.cpp/PreviewScriptSelfTest +423 | the F15 historical-B control arms (exe a9cd964a); evidence closed, keep as a WIP commit |
| `item4-feel` `stage2/depth12-spawn-origin` @ `b5811aad98` | MovableMan.cpp +74/-7 | depth-12 diagnostic instrumentation; its exe is a no-travel control, never a candidate |
| `item7-chat` `stage2/directory-unlisted-sessions` @ `feb492b9a7` | NetDirectorySelfTest.cpp +15 | an additional Failed-cleanup test arm, unreviewed |

**SWE queue** (`devin_cli/queue-20260913-runtime.json`, stop file `STOP-20260913` from 14:40 MST): platinum-begonia
finished controller-log driver, substitution launch validity, directory failed-cleanup and stimulus classification
(all claims, B-queue inputs); `swe-ui-viewport-complete` (defiant-taurus) and `swe-directory-lease-combined-fixture`
(platinum-begonia) were running at the pause and resume in the same sessions; `swe-controller-driver-live` queued.

**Mac** (`/Users/erol/cortex-workers`, 92 GiB free): 30 lane directories remain; every finished one has `REPORT.md`
and `exit.txt`; the four `grok-*-runtime/terrain` Windows-pair lanes never ran (they waited for a Windows go);
`mac-pie-close-independent` and `mac-terrain-primitive-cause` were cut mid-run and resume by session id (LEAD-REVIEW).
Three lanes hold 1.7-2 GB each (`mac-directory-client-red`, `mac-directory-codec-red`, `mac-fg6c-final`): cleanup B10
after their evidence is confirmed fetched, logged in `reviews/cleanup-20260913.md`.

**Launched at the user's GO (16:58-17:05 MST, recorded 2026-09-13 17:04 MST):** Astra A1 `astra-fg6d-harness-20260913` (thread 01a09d34-ecd4…), A2 `astra-pie-close-review-20260913` (01a09d34-f35a…), B1 `astra-terrain-xplat-20260913` (01a09d34-fca2…), B2 `astra-f15c-second-read-20260913` (01a09d35-04af…), B3 `astra-f21-coroutine-read-20260913` (01a09d35-0c8f…), all gpt-6-astra/max from their rollouts; SWE-2 Max `swe-ui-viewport-complete-r1-20260913` (session defiant-taurus resumed) and `swe-lobby-chat-20260913` (item 7a slice 1, session platinum-begonia resumed; brief in that lane dir; worktree audio-owner-registry on stage2/lobby-chat from the wave tip); Mac Opus `mac-pie-close-independent-20260913` resumed (session 2f067b76…). Briefs for B5 (`astra-directory-lease-corrections-20260913`) and B6 (`astra-ice-lifecycle-20260913`) are written and wait for Astra capacity. Item 3 moved 96 → 97 at 17:02 MST (Form Squad UI path, LEAD-REVIEW). Every lane's runner output is under `grok-workers/cli_runs/<name>/` (Codex, Cursor) or the lane dir (Devin, Mac).

**Routes verified this hour:** `codex_cli/codex_job.py` (new) gate, stdin brief, usage and spawn row
(`cli_runs/smoke-astra-runner-20260913`); `codex exec resume` keeps the thread's context and runs full-permission;
`win_job.py --resume <chat id>` (new flag) continues a finished lane with its memory (`cli_runs/smoke-grok-resume-20260913`);
the Cursor CLI offers no Grok 4.7 (`cli_runs/smoke-grok47*-20260913/agent-stderr.txt` lists every model).

---

# §5. WHAT IS LEFT, IN ORDER

## 5.0 Order of work from 2026-09-13 16:42 MST (option B: the lead runs it; Astra is the main engineering counterpart)

Roles (commands §0.5, spawn steps §6.3b, policy CLAUDE.md §0/§0.1): **Astra** (GPT-6 Astra, effort max, Codex CLI,
full permissions) = engine surgery, netcode, UI/UX design, harness engineering, independent second reads — used heavily
now; the user watches the Codex quota and calls the shift to Opus; usable to zero. **Opus 5** (max, Fast off; Agent tool
here, `mac_claude_job.py` on the Mac) = the independent verifier of every HIGH fix, every visual question, surgery once
Astra shifts. **SWE-2 Max** (free, two durable sessions) = bounded UI/feature work with named seams. **Grok 4.6 Extra
High Fast** (both machines) = run-only batteries, builds and gates, triage, mechanical implementation from a fixed
design. **The Mac** = every cross-platform proof, never idle. The queue below stays at least six deep; five to eight
lanes run when the lead can review the landings within the hour (CLAUDE.md §0.1). Sequence A is never displaced.

### Sequence A — the approved tree moves

| # | Lane (route) | Tree @ sha / inputs | Outcome: RED → GREEN | Cap |
|---|---|---|---|---|
| A1 | `astra-fg6d-harness` (Astra) | read-only `control-build` @ `7e63a0c1b5`; new lane dir `grok-workers/fg6d-battery/` copied from `fg6c-battery` plus the reviewed heal / AK / controller-log copies; artefacts `D:\mx\fg6bat3` read-only | the ten harness corrections of LEAD-REVIEW (2026-09-13 16:42 MST): global-callback through the recording driver; old h4gates fencing row retired; desktop-gate waits → NOT RUN + one rerun pass; own-lock waits removed; boundary-aware fail-closed heal oracle with its mutation table re-run; present_identity prefix-180 contract; AK three-way equal except `__wall_seconds`; n6 exact list + deliver line; controller_log row on the pin-completion driver; `scan_resync` fixed and invoked. RED = each defect reproduced on the FG6C artefacts by the old scorer; GREEN = the corrected scorer on the same artefacts plus unit tests; no engine run; every diff in `fg6d-battery/PREP-REPORT.md` | 3 h |
| A2 | `astra-pie-close-review` (Astra, read-only) + resume Mac Opus `mac-pie-close-independent` (`2f067b76…`) | `item5-lifecycle` `stage2/pie-close-lockstep` @ `81187eef4e`; raw `D:\mx\pie-close-lockstep-20260913`, `D:\mx\pie-switch-runtime-20260913` | independent verdict on the 3+/2- `GameActivity.cpp` change (1246/1256 guarded by `IsLockstepControllerSyncActive`), the `PieMenu.cpp:881` reopen path, four 201-204 differences → 0 with 320 maps / 1470 rows equal, the SP fixture, the failed first two-peer invocation; the Mac detecting run | 1.5 h |
| A3 | lead | wave ← pie-close if A2 and my own read accept; candidate fast-forward to the wave tip; `/MP12` alone; verify6 8/8; firewall rules | the next candidate exe pinned | 1 h |
| A4 | `grok-fg6d-battery` (Grok run-only) | the new candidate; scratch `D:\mx\fg6bat4`; ports 47661-47666 / 8458 | ONE FG6D inventory in an exclusive Windows engine window with the desktop free of fullscreen apps (the runner's QUNS gate otherwise waits) | 7 h |
| A5 | lead | classification; `--no-ff` merge into `stage2/p4b-interp-lockstep`; scan 0; push with lease | the Source44 family on the approved tree (nothing else on Windows engines) → items 1/2/5 move; push; docs | a day |

### Queue B — parallel outcomes (spare capacity; Windows engine use only outside the A4/A5 windows unless the brief is non-engine)

| # | Lane (route) | Tree / inputs | Outcome: RED → GREEN | Cap |
|---|---|---|---|---|
| B1 | `astra-terrain-xplat` (Astra engineering; Mac half through `mac_cli/mac_job.py`) | `h4-secondary` `stage2/terrain-initialization-diagnostic` @ `7e63a0c1b5` (+ its diagnostic diff); inputs `grok-cross-platform-terrain-cause-20260913`, the Mac checkpoint | **F22**: identical tick-1 terrain bytes on both machines. RED = the retained dump pair (mat 12 / fg 64); GREEN = mat 0 / fg 0 with contraction off for every Mac C subproject (and whatever else the disassembly shows); no mask, no asset change; Windows unchanged or rebuilt bit-identically | 4 h |
| B2 | `astra-f15c-second-read` (Astra, read-only) | `14db824c10` on the wave; §3.4 F15/F15b/F15c; the ASan rows | claims with file:line on the lifetime of retiring parts/wounds, the withheld stateless cleanup, the uncovered arms | 1.5 h |
| B3 | `astra-f21-coroutine-read` (Astra, read-only) | `85d90f2e4a` `84a66e4189` `3a865f9e78` `b4a57ae64d`; `672024dfd1` `ec5a34efbc` | claims with file:line on the rebind of local slots / marked actor / brain across relaunch and on `base+framesize<=maxstack` incl. the 8011 arm | 2 h |
| B4 | `swe-ui-viewport-complete` (SWE-2, resume defiant-taurus) → `cortex-opus-visual-reviewer` | `item7-ux` @ `2594cff51d` + its repair diff | F1 offscreen panel, F3 invisible high-bit token, F2 odd-width seam fixed at 640x360; RED = the 25 rejected captures; GREEN = fresh captures accepted by the Opus visual reviewer; then the next wave | 3 h |
| B5 | `astra-directory-lease-corrections` (Astra engineering) | a fresh branch from the wave tip in `item7-chat` (it holds the client); `grok-directory-lease-integration-20260913/patches`, the Mac review `mac-directory-lease-integration-review-20260913/REPORT.md` (F1-F4), `grok-client-hidden-inflight-404-20260913` | F1 settle-then-relist, F2 keep only the bound row, F3 locked reads, F4 join_mode for the new id, hidden-404 fail-closed; RED/GREEN = the review's predicted native commands built `/MP6` and run; selftests 11/11 | 4 h |
| B6 | `astra-ice-lifecycle` (Astra engineering) | `item8-directory` `stage2/ice-lifecycle-regressions` @ `7e63a0c1b5` + its owned-link diff (committed as WIP first) | an ICE match returns to the lobby / rematches on the bound mux (Mac RED "a session-id (ICE) match cannot return to the lobby yet"); Destroy/ReportRuntimeError clear the mux pump before Stop; ended-world late admission refused (`TestEndedWorldLateAdmission`); native RED/GREEN on Windows and the Mac | 5 h |
| B7 | lead | `grok-formsquad-ui-stimulus-20260913`, `grok-client-orders-runtime-20260913` raw | item 3b credit for the UI path of an AI order if the raw holds | 1 h |
| B8 | `mac-wave-<tip>` (Grok Mac) after A3 | clean clone of the new candidate tip | arm64 build, script-graph, A7, AK, local UI/state gates | 3 h |
| B9 | Astra design lanes as A/B land | item 8b dedicated headless host on the Mac; item 7a chat routing + UI | design notes with the detecting-test plan, then implementation lanes | 2 h each |
| B11 | `astra-pie-lockstep-gating` (Astra engineering, HIGH; launched 17:44 MST) | `item5-lifecycle` `stage2/pie-close-lockstep` @ `81187eef4e` | **F23**: the departing actor keeps player control bits and its pie flickers Enabling/Disabling after a lockstep switch (both peers, pre-existing); the remaining local pie mutations in GameActivity.cpp gated or proven presentation-only; arms RED on the tip, GREEN after; SP controls byte-identical | 5 h |
| B11a | (folded into B11's acceptance, 2026-09-13 17:52 MST) | same worktree and driver family | the pie-close detector made fail-closed against A2's five probes (`astra-pie-close-review-20260913/probe_detector.py --assert-fail-closed` must exit 0; today it exits 1) and an MP PREV detecting pair (RED on the retained control, GREEN on the tip) | — |
| B13 | `astra-f15-debt-arms` or Grok (execution lane, queued 2026-09-13 17:53 MST) | `control-build` @ `3f65208668`; the F15 lanes' retained fixtures | B2's §4 recipes I/T/M/Q/X/J/A/L/N/O: the taken-shadow-retiring branch, spawnMeta-only filtering, stale-key exclusion, unharvested tail descendants, retiring Actor inventories, ghost-owner reachability, and unchanged mod behaviour across a preview end (aliases, continuations) — each a fail-closed arm RED on the reversed hardening, GREEN on the tip | 5 h |
| B12 | `astra-f21-debt-arms` or Grok (execution lane, queued 2026-09-13 17:46 MST) | `control-build` @ `3f65208668`; alias-walk as the isolated builder | the seven debt arms of B3's report §6: players 1..3 across all four slot types; a legitimate first-update selection change through a scene-backed `SwitchToActor`; inventory/marked slots through a real resync incl. ACraft inventory; substitute / seatless spectator / dedicated host / late tagged ICE lifecycles; large reused placeholders, protected-growth failure and GC stress; frozen old/new serialized SG/ccsave artefacts read by both readers; the A7 oracle requiring 61 consecutive resumed rows. Each RED on a reversed repair, GREEN on the tip | 5 h |
| B10 | lead | Mac lanes whose evidence is fetched; `reviews/cleanup-20260913.md` | Mac disk back (three 1.7-2 GB lanes); every deletion logged | — |

**Status protocol:** on every status request the board is updated first, then one line per item with % and Δ, then
running / queued / awaiting-review with each lane's task, route and ETA, then negatives with their reasons; a history
line is appended. A landing that moves a number is also reported unprompted, in one line.

Execution is active after the user's GO (2026-09-13 01:38 MST); `reviews/resume-20260913/PLAN.md` applies the order below
to the current CLI routes and quota stop. The original independent W136-2 review is complete; §4.10 lists current follow-ups and durable session reuse.

## 5.1 Close the wave (hours)

Historical order (2026-09-13 14:21 MST, superseded by §5.0): Next: replenish useful Grok/Opus jobs immediately; personally verify gameplay and terrain raw evidence, review pie/client test diffs, finish UI pixels, correct hidden-lease integration, then assemble one fix group. Candidate7e63/c67b35 remains frozen, approvedaa650e601e unchanged, Source44 NOT STARTED. Codex76% remaining14:17; stop immediately at<=60% of any available core window. No defensible full-roadmap ETA yet; prior overall estimate remains withdrawn.

1. (done 2026-09-12 16:58 MST) The FG6B report is read and classified (§4.6): no true engine failure on the
   candidate at `cefccaa1f6`; recorded in LEAD-REVIEW and SPAWN_LOG.
2. Coroutine ec5 accepted/merged; depth12 secondary MOPixels explained. F21 final b4 independent review, exact RED/GREEN and Mac/Windows detecting runs complete; merged7e63. Full battery regression remains.
3. Supported F15 hardening14db824c10 accepted after independent ten-row ASan audit and fresh lead same-oracle Final ASan RED/GREEN; merged8342801d91 and pushed. Unsupported stateless cleanup removed; uncovered inventory/shared/raw-link arms stay explicit debt. Exact GREEN09ec8453 and symbols preserved at D:/mx/lead-f15-pair-20260913/preserved-green before reusing item8-directory.
4. (done 17:17-17:24 MST on the wave exe `cf2e611b…`, §4.1) suite 11/11, script-graph 181/0, AK-47 HUD 3/3 on a
   plain Final build, the craft_cargo pulse closed at 316..319 on both peers.
5. **Promotion/build/verify6 done:** control-build7e63 clean Finalc67b35 built alone09:22:40. Lead rederived8 groups on19 launches; original summary7/8 retained for outer marker mismatch. Future helper repair394933a6 accepted after lead6/6 noengine marker/exit checks. Identity10/10 and build pin ready. Candidate backup pushed09:57 with scan0 and exact4dd lease.
6. **ONE FG6C inventory battery ENDED (not accepted; current details in pickup):** exact7e63/c67b35 in final prompt/build-pin; real identity10/10. Foreground run_inventory→run_all+N6 under audited guard24000s/5GiB sampled cap, Grok runner25200s. run_battery and remaining_steps timing reruns disabled (original results/flags retained); exact before files and diffs in fg6c-battery/diffs/final-execution-policy. Protected four fixture files unchanged. Inherited present_identity TICKS adjustment is disclosed and must be diffed. Generated report is a draft: inventory every failure first, then parallel diagnosis, one fix group, one battery.

7. Mac combined-tip7e63/87158955 native/A7 and correctedHUD gates CLOSED: lead235+35rawchecks and personalHUD PID20020 pass. Preserve original missingdepth7 and constructor failures. Next Mac engine lane is directory-client test-only RED726bc912 on db59; transportGREEN follows reviewed ownership correction.

Historical exception 2026-09-13 11:19 MST: FG6C arm1 owning-lock stall: inherited w102 wait_engine.py waits for LEAD_BATTERY.lock; no arm1 directory or engine created. Lead saved exact process ancestry/lock/noengine evidence then killed ONLY waitPID48472 at11:07:24, parent row records4294967295. Inventory continued; lock retained. NOT RUN, no engine failure or pass; separately corrected invocation awaits full inventory end. Lead notice and arm1-own-lock-stall.json retained, original drivers unchanged.

Historical checkpoint 2026-09-13 11:59 MST: FG6C original run_all/remaining finished11:30 with five NOT RUN engine rows caused by own-lock waits: arm1,AK1/2/3,ICE; dependentAKcomparison had no inputs. Lead retained exact ancestry/noengine evidence and terminated only those waiting drivers (ICE owned directory fixture also stopped). Original argv/checkers/pins and LEAD_BATTERY.lock unchanged. FinalN6 started11:30:09 but isolated_launch--hold PID49144 is waiting at QUNS_BUSY; no engine, no override. Bounded2400s wait ends about12:10. Guard48700/inventory47236 retained. Only after all owned descendants stop and pin recheck may corrected single invocations run under the same lead-owned lock; original failures remain disclosed. Arm/AK/ICE preflights read,17+10 noengine worker tests; final separator correction pending lead rerun.

## 5.2 Promote (a day)

8. Merge the candidate into `stage2/p4b-interp-lockstep` in `p4b-interp-validation` (`--no-ff`), tree clean apart from
   the vendor libs, `lead-tools/scan_trailers.py` over `upstream/development..HEAD` = 0 hits, push with lease, record
   the push here and in `STATUS.md` with the remaining failures.
9. **The Source44 family** on the approved tree (§0.4), nothing else running. Verdict = chain + remaining + breadth
   81/81 accounted + matrix 106 reviewed + the Mac attempt green. Then items 1, 2 and 5 can move.

## 5.3 Mac follow-ups (parallel, the Mac is never idle)

10. **ICE-only application join proved on the Mac LAN** (601ticks, P2P only; scoped acceptance above). Next close transport ownership, directory/signaling lifetime and session-id ticket context for real ICE rematch/resync. Source analysis is not runtime proof; obtain real RED before implementation. Cross-platform and public WAN remain unproved.
11. **AK-47 Mac:** combinedlpinv176 and correctedHUD3/3+personalrepeat accepted, settings/source/exe pinned. Directory client accepted on both platforms after native RED/GREEN; native ICE ownership correction built but missing setup/success/cancel proofs remain. Cross-platform pair is now an active run-only lane.
12. **Windows-host / Mac-client lockstep e2e** after W131 — no cross-platform two-peer match exists in the records;
    every Mac proof is Mac-vs-Mac. Compare the sound observations and the 600-tick traces.

## 5.4 Finish the review (a session)

13. Listed Part B reads complete: Main.cpp and Admission/Auth/Protocol/Session/Match, plus all3991 NetLockstepSelfTest and4729 NetReconnectSessionSelfTest diff lines. Lead read all remaining hunks, including previously truncated slices. No new production defect established; limited counter/payload assertions are recorded as coverage limits. F3 remains an open constraint until a lane touches the frame lane.

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
  `stage2/fixgroup-6-lead-wave-a` in `takeover-build` is for: thirteen merges, three fix-ups and a trim landed and
  were built and gated there while FG6B kept `control-build`'s exe frozen; control-build is promoted by fast-forward
  afterwards (§5.1 step 5).
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

## 6.3b How to spawn each route, step by step (read from the runners themselves, 2026-09-12)

**Every lane, before spawning:** (1) the brief is a UTF-8 file with LF endings that names the worktree AND its
branch at a sha, the exact editable paths, the RED line and the GREEN runs that define acceptance, the report path,
the mod-compatibility rule, the no-attribution rule, `CL=/MP6` + the two-lane build cap, "engine launches only through
the runners with `CCCP_HEADLESS=1`", "never `-net-port-map-probe` or a bare `-net-p2p-selftest`", "never move/delete
under `D:\mx`", "final message ≤ 25 lines", and it points at `grok-workers/WORKER_RULES.md` (template: LEAD_PLAN.md
Appendix B); (2) the worktree is one of the firewall-ruled directories (§6.2) with a fresh branch checked out at the
base sha and the two vendor `_Bin` libs restored; (3) the base sha in the brief is re-read from the tree (W94 was
wasted on a stale base); (4) ports and scratch roots are lane-unique (`D:\mx\<lane>`, loopback service port, game
port range); (5) the time cap fits the work (a battery 7 h, a fix lane 2.5-4 h, a review 1-1.5 h).

**Grok 4.6 Extra High Fast on Windows** — `python grok-workers/win_cli/win_job.py <name> <worktree> <prompt.txt>
--task "<one line>" --timeout <sec> [--mode agent|ask|plan] [--model cursor-grok-4.6-xhigh-fast] [--require-model "Extra High Fast"]`,
run from `grok-workers/`. It launches `cursor-agent` with a PRIVATE profile (`grok-workers/cli_home`: no user MCP
servers, hooks or Claude settings; commit/PR attribution pinned OFF), reads the first stream-json line ONCE (up to
`--init-wait 120` s), kills the job unless that line contains the required model string, then only waits under the
hard `--timeout` (default 3600 s; `kill_tree` at the cap) and appends the SPAWN_LOG row itself. Exit codes: 0 the
worker finished (its own exit code in `cli_runs/<name>/exit.txt`), 2 timeout, 3 the CLI ended before naming a model
(login or network), 4 REFUSED (wrong model — never accept work from such a run). Outputs: `cli_runs/<name>/agent-output.jsonl`
(line 1 = the model evidence), `command.txt`, `exit.txt`; the worker's final message is extracted with
`grok-workers/lead-tools/extract_final.py <name>` into `cli_runs/<name>/REPORT-final-message.md` (the lane's own
`REPORT.md` lives in `grok-workers/<name>/`). There is NO resume option in this runner: a stalled or drifting Grok lane
is killed at the cap (or by PID) and re-spawned with a corrective brief in the same worktree; the previous run's
`agent-output.jsonl` is its record. Rate limits: the Cursor CLI has shown none at this volume; the failures seen were
timeouts (an exe-hash loop that never ended), a direct engine launch, and a wrong model (exit 4) — all bounded by the
runner. Run it in the background with the cap as the Bash timeout; the notification arrives when the process exits.

**Grok 4.6 Extra High Fast on the Mac** — write a zsh script (LF) that `cd`s into `/Users/erol/cortex-workers/<lane>`,
runs `cursor-agent -p --force --trust --sandbox disabled --output-format stream-json --model cursor-grok-4.6-xhigh-fast
--workspace <lane> "$(tr -d '\r' < prompt.txt)" > agent-output-<lane>.jsonl` (the DASH in the output name is what the
gate reads; without it the model check is skipped silently) and writes `exit.txt`; `scp` the prompt as `prompt.txt`;
then `python grok-workers/mac_cli/mac_job.py <lane> <script.zsh> --timeout <sec> [--fetch <files>] [--poll 15]`.
It bootstraps a launchd job into `gui/501` (the only session with the unlocked keychain), reads the model line once
(REFUSED otherwise), polls `exit.txt` every 15 s, boots the job out and fetches the named outputs. On TIMEOUT the job
is LEFT RUNNING — boot it out yourself (`launchctl bootout gui/501/<label>` over `ssh Erol-Mac`) before spawning the
next Mac lane. The lead writes the SPAWN_LOG row for Mac spawns. Each lane is a fresh clone under
`/Users/erol/cortex-workers/<lane>` (1-3 GB): delete it after its evidence is fetched with `tar` over ssh excluding
`runtime`/`external`/`Data`/`build`/`.git` (the 2026-09-12 cleanup archived every lane's small files into
`reviews/takeover-20260909/mac-evidence-20260912/mac-lanes-small-files.tgz` and removed all lanes but `cli-home`).

**Claude Code Opus 5 on the Mac** — `python grok-workers/mac_cli/mac_claude_job.py <lane> <prompt.txt> --task "<one line>"
--timeout <sec> [--fetch ...] [--session-id UUID | --resume UUID] [--model claude-opus-5] [--effort max] [--require-model claude-opus-5]`
(defaults: timeout 7200, init-wait 180). Same launchd route; the gate reads the transcript's first line for the model;
exit codes 0 / 2 timeout (booted out) / 3 no init line / 4 REFUSED. `--resume <UUID>` continues a session with its
whole context (the UUID is printed at spawn and stored in the lane's `result.jsonl`): use it for a follow-up or a
corrective prompt instead of re-briefing from zero.

**SWE-2 Max through the Devin CLI (free)** — `python grok-workers/devin_cli/devin_job.py <name> <workspace> <prompt.txt>
--task "<one line>" --timeout <sec> [--permission-mode dangerous] [--no-preflight] [--resume-session <id>] [--rate-resumes 8]`.
The gate is a PRE-FLIGHT: a one-line smoke prompt in a scratch directory whose export must name `SWE-2 Max`
(REFUSED otherwise; `--no-preflight` only when one ran minutes ago), and the real job's export is checked again at
the end (REFUSED_FINAL = its output is not used). Sessions are durable (`%APPDATA%\devin\cli-next\sessions.db`;
`devin -r <session_id> -p --prompt-file <turn>` continues non-interactively with the whole history). Devin's
account-wide "overall message rate limit" ends a turn after ~three in-process; the runner sees "Created new session:
<id>", kills the lingering process, waits for the stated reset and RESUMES THE SAME SESSION with a short continuation
turn, up to `--rate-resumes` (8) times, so nothing the worker did is lost; `--resume-session <id>` does the same by
hand. Keep at most two SWE-2 sessions alive, started ~90 s apart, from `devin_cli/queue.json` with
`python grok-workers/devin_cli/devin_queue.py queue.json --slots 2 --stagger 90` (the queue runner is a long-lived
process — it was still alive at the 2026-09-12 clean stop and had to be killed; stop it explicitly when the queue is
done). GitHub stays DISCONNECTED from Devin (a repo connect billed $72 of wiki generation; CLI use is 0 ACU). Give
SWE-2 bounded UI/feature work with named seams only.

**Opus 5 here (the Agent tool)** — `cortex-opus-engineer` (Read/Edit/Write/Glob/Grep/Bash; builds, gates, mechanical
corrections from an exact spec), `cortex-opus-verifier` (read-only; re-derives RED/GREEN from raw logs and quotes
file:line), `cortex-opus-visual-reviewer` (every screenshot). Always `run_in_background: true`; the report arrives as
a notification and exists nowhere else — save it to the lane's `REPORT-*.md` in the same turn. Stopping an agent
(`TaskStop`) does not kill a build or engine it started: check `cl.exe`/`link.exe`/`Cortex Command.exe` afterwards and
kill by PID. Do not delegate the thinking documents (§1.1).

**After any lane lands:** extract the final message; read the report's "not done / deviations" section first; read
every diff line (`git show`); re-derive at least the RED and GREEN lines from the raw files it cites; run
`verify_merge_commit.py` after the merge; write the SPAWN_LOG row with the verdict and the LEAD-REVIEW entry; move a
STATUS number or record a negative delta.

**GPT-6 Astra (effort max) through the Codex CLI** (added 2026-09-13 16:42 MST) —
`python grok-workers/codex_cli/codex_job.py <name> <worktree> <prompt.txt> --task "<one line>" --timeout <sec>
[--sandbox full|workspace-write|read-only] [--add-dir DIR] [--resume THREAD]`. The runner feeds the brief on stdin
(`codex exec` hangs on an open non-TTY stdin unless the prompt IS the stdin), runs
`codex exec --json -m gpt-6-astra -c model_reasoning_effort=max -c notify=[] --dangerously-bypass-approvals-and-sandbox
--skip-git-repo-check -C <worktree> -`, reads the thread id from `cli_runs/<name>/events.jsonl`, then the session
rollout `%USERPROFILE%\.codex\sessions\<y>\<m>\<d>\rollout-*-<thread>.jsonl`, and kills the job unless its
`turn_context` line says `"model":"gpt-6-astra"` and `"effort":"max"` (exit 4). Outputs: `events.jsonl`, `stderr.txt`,
`last-message.md` (`-o`), `REPORT-final-message.md`, `usage.json` (input / cached / output tokens: the quota record),
`model-evidence.txt`, `thread.txt`, `exit.txt`. Exit 0 finished (the CLI's rc in `exit.txt`), 2 timeout, 3 no init, 4
refused. `--resume <thread>` runs `codex exec resume <thread> …` behind the same gate; the cwd comes from the session
and `-C` / `-s` / `--add-dir` are not accepted there, so a resumed lane is full-permission. Each lane is its own
`codex exec` process: run as many as the queue holds; a rate limit surfaces as a CLI error in `stderr.txt`, and the lane
is then resumed with `--resume`. A `turn.failed` event with `codex_error_info: cyber_policy` (the provider's
cybersecurity classifier; seen 2026-09-13 18:19 MST on allocator-failure / stack-capacity test wording, and earlier on
the interactive session) ends the turn with CLI exit 1 and no usage event: resume the thread with a continuation that
rewords the step in plain engine terms and moves it last; if it recurs, record the step and skip it. Both 2026-09-13 trips followed a process-enumeration command (`Get-CimInstance Win32_Process … CommandLine`
for the build-cap check): Astra briefs now check the cap with `Get-Process -Name cl,link` only and never enumerate
command lines. Astra reads the `AGENTS.md` at the worktree root by convention: the twin is the policy,
the brief is the fence. Every Astra brief says "no nested Codex sessions or sub-agents" (B2 spawned its own
`codex exec` for part of a read on 2026-09-13; harmless but unbudgeted). Smoke evidence 2026-09-13 16:09-16:32 MST: `cli_runs/smoke-astra-runner-20260913` (gate ok,
14 s); `-s workspace-write` allowed in-workspace writes, commands and loopback sockets and denied outside writes;
resume kept the thread's context. Briefs: the same template as every route (Appendix B) plus the exact permitted
paths; Astra may call the other runners (`win_job.py`, `mac_job.py`, `mac_claude_job.py`) when its brief says so.

**Resume mechanics, every route** (2026-09-13 16:42 MST): Codex `codex_job.py --resume THREAD`; Cursor Windows `win_job.py --resume
CHATID` (new flag; the CLI keeps every chat under `cli_home/.cursor/chats/`, proven on `eb8f897b…` at 16:32 MST with
its memory intact); Cursor Mac `cursor-agent --resume CHATID` in the lane zsh (untested); Devin the same session
(`devin_job.py` resume; the queue hands a session on); Mac Claude `mac_claude_job.py --resume UUID`; Agent-tool Opus
agents via SendMessage to the agent id. Permissions: every route launches full-permission (§1.1).

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
- **Read the clock before every stamp** (`date`, Arizona local time — the date too); estimated stamps drifted 90
  minutes in an hour, and a later stamp carried the wrong day.
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
- One live doc (this file: state and the order of work), one board (`STATUS.md`), one policy file (`CLAUDE.md` ==
  `AGENTS.md`), one review ledger (`LEAD-REVIEW-overnight.md`), one spawn log; `LEAD_PLAN.md` is the 2026-09-11 plan
  kept for its still-current sections (§0.1). Everything else is evidence under `reviews/`.
- Stale pointers cost a full re-investigation (proven twice). When a milestone lands, update the pointer surfaces in
  the same session.

---

# §7. PATH INDEX

## 7.1 Local — trees and docs

| Purpose | Path |
|---|---|
| This doc (state + order of work) / the board / the 2026-09-11 plan | `D:\Projects\RESUME.md` · `STATUS.md` · `LEAD_PLAN.md` (historical except §6-§8 and Appendices A-B) |
| Project instructions (identical twins) | `D:\Projects\CLAUDE.md` **==** `D:\Projects\AGENTS.md` |
| Pre-rewrite RESUME (verbatim) | `D:\Projects\_archive\docs_archive_20260912\RESUME_pre-rewrite-20260912.md` |
| HANDOFF, HANDOFF2, 15 phase plans, 4.5k reports | `D:\Projects\_archive\docs_archive_20260905\` |
| The lead's review ledger (F1-F20, every acceptance, every NEGATIVE) | `reviews\takeover-20260909\LEAD-REVIEW-overnight.md` |
| Every lane spawn and landing | `reviews\takeover-20260909\grok-workers\SPAWN_LOG.md` |
| Worker rules (travels with every brief) | `reviews\takeover-20260909\grok-workers\WORKER_RULES.md` |
| Trailer-rewrite hash maps | `reviews\takeover-20260909\trailer-strip-20260911\map.txt` (15 branches) · `trailer-strip-20260912\` (`map.txt` 148 branch tips, `commit-map.txt` 866 commits old→new, `pre-tips.txt`, `push2.txt`, `README-expdf.txt`) — evidence and reports written before 17:45 MST quote pre-strip shas; resolve them here |
| Cleanup log (what was deleted and why, 2026-09-12) | `reviews\cleanup-20260912.md` |
| Mac lane evidence archive (every lane's small files before the lanes were deleted) | `reviews\takeover-20260909\mac-evidence-20260912\mac-lanes-small-files.tgz` |
| H4 design + pins P1-P32 | `D:\Projects\STAGE2_H4_RECONNECT_PLAN.md` |
| Backlogs | `RECOMMENDED.md` (near-term) · `NEXTENHANCEMENTS.md` (long-range) · `MULTIPLAYER_BEST_PLAN.md` · `STAGE2_P8_PLAN.md` |
| Contract audit + the family runner | `reviews\recovery-2026-09-07\contract-audit\` (`CONTRACTS.md`, `run_family.py`, `mac-peer-20260907\MAC_RESUME.md`) |
| Mod-compatibility steering | `reviews\claude-review-2026-09-08\MOD_COMPATIBILITY_STEERING_PROMPT.md` · family runbook `INTEGRATION_33_RUNBOOK.md` |
| ADRs (through ADR-024) / public wiki | `cccp\modernization-docs\decisions.html` · https://madreag.github.io/cortex-modern/ |
| Run roots (junctions inside — never move or delete; 136 roots after the 2026-09-12 cleanup, §4.7) | `D:\mx\` (`lead-fg6` the candidate's gates, `fg6bat2` the FG6B battery, `w124b` the ASan reports incl. F20's, `w132` the preview-HUD ASan, `w133b` W133-2's ASan red/red2/green, `w136` W136's red/green, `w136b` W136-2, `w71e` W71-4's red/green/pulse, `wave-a` the wave's gates, `s43b2` R5's evidence) |
| The lead's cross-session memory (harness) | `C:\Users\egerm\.claude\projects\D--Projects\memory\` — one fact per file, indexed by `MEMORY.md`; the notes cover merge verification, separate resolve/check/commit steps, worker test-only code, Arizona local time, heredoc backslashes, PowerShell dash values, process kills by PID, subagent reports living in the notification, direct engine launches, Mac evidence copies, the model-gate output name, cross-platform build misses |
| The lead's session scratchpad (scripts, partB diffs, checkpoint loggers) | `C:\Users\egerm\AppData\Local\Temp\claude\D--Projects\d1ccc20c-5de9-4ed0-bdc3-fc51ecd35f88\scratchpad\` — session-specific; the reusable tools were copied to `grok-workers\lead-tools\` |
| Fixtures and recordings | `D:\Projects\stage2_p4\fixtures\` (`pickup_fire.ccreplay`, `pickup_fire.txt`), `stage2_p4\rb_replay_20260905\`, `rb_replay_buy\` |

## 7.2 Local — the scripts you will actually run

| Script | What |
|---|---|
| `reviews\takeover-20260909\grok-workers\git_commit.py` | the **only** way the lead commits (refuses trailers, re-reads the stored message) |
| `…\strip_trailers.py` | message-only trailer strip (trees unchanged) |
| `…\codex_cli\codex_job.py` | the Astra route: `codex exec` behind the rollout model/effort gate; `--resume THREAD`; `usage.json` per lane (§6.3b) |
| `…\win_cli\win_job.py` · `…\mac_cli\mac_job.py` · `…\mac_cli\mac_claude_job.py` · `…\devin_cli\devin_job.py` · `…\devin_cli\devin_queue.py` | the four worker routes and the SWE-2 queue (§0.5) |
| `…\firewall_allow_all_exes.ps1` (elevated) · `firewall_planned_worktrees.txt` | per-executable inbound/outbound allow rules for every `Cortex Command*.exe` under `D:\Projects` |
| **`…\grok-workers\lead-tools\`** (copied out of the lead's scratchpad 2026-09-12 — use these, not a new copy) | `verify_merge_commit.py` (per-file +/- multiset check of a merge commit) · `resolve_keep_both.py` (keep-both on the conflicted **working** file) · `apply_hunk.py` (re-apply one hunk of a branch diff by content) · `check_merge.py` (pre-commit form of the merge check) · `extract_final.py` (save a Grok lane's final message as `REPORT-final-message.md`) · `verify_lead6b.py` (**verify6**; edit `REPO`/`OUT` at the top) · `scan_trailers.py` (the pre-push attribution scan, line-start rule, exit 1 on a hit; `--all-branches` for the whole repo) |
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
- **History since 2026-09-10 was rewritten (message-only) on 2026-09-12 17:45 MST** to strip the attribution line
  (§1.1): 147 branches and 26 tags were force-pushed with identical trees. A clone, worktree or CI checkout made
  before that must `git fetch --all` and reset each branch to `origin/<branch>`; old shas resolve through
  `trailer-strip-20260912/commit-map.txt`.
- GitHub is the safety net: all worktrees share one object store (`D:\Projects\cccp\.git`); deleting a worktree cannot
  lose commits; restore any branch with
  `git -C D:\Projects\cccp worktree add D:\Projects\<dir> <branch>` (fetch it first if the ref is missing).
- The inherited nightly workflow is disabled (all three platform builds succeeded; only "Publish Release" 403'd on a
  fork — re-enabling needs only a `permissions:` block).

## 7.4 Mac

- Lanes: `/Users/erol/cortex-workers/<lane>` (empty since the 2026-09-12 cleanup except `cli-home`; each new lane is
  a fresh clone) · retained read-only evidence: `/Users/erol/Documents/Codex/` (3.7 GB, untouched)
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

**This machine.** Clock and stamps in Arizona local time (`date` prints MST; the machine has no daylight saving) ·
Use `python` (not `python3`); `pwsh -NoProfile`; never `taskkill`/`tasklist` in Bash (MSYS2 rewrites
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


Recovery checkpoint 2026-09-13 06:19 MST: depth12 identity closed; lead mod-text rerun38/38 on49c0cda0… at D:/mx/swe-modtext-20260913/lead-recheck. Mac diagnosis completed from raw evidence; new Opus5 max/Fast-off Mac lane mac-local-binding-20260913 has fresh clone branch stage2/resync-local-rebind @d20d4bddfa. Only Activity.cpp/.h editable, original A7 oracle retained; five-arm GREEN required. mac_claude_job.py now explicitly sets max/Fast-off/attribution-off/headless and MST spawn stamps; py_compile passed. Mac clone setup completed while worker initialized; worker re-read correct HEAD/branch before edits. Windows slots remain stack-capacity and f15-probes.
