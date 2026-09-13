# LEAD PLAN — Claude Code lead, from 2026-09-11 14:20 MST

> The 2026-09-11 execution plan (see the status note below). `RESUME.md` is the state and the live order of work,
> `STATUS.md` is the board, `CLAUDE.md` (= `AGENTS.md`) is the policy.

> **STATUS OF THIS DOCUMENT (2026-09-12 18:01 MST):** this is the 2026-09-11 plan, kept for its still-current parts —
> §6 (the roadmap order, mirrored in RESUME.md §5.5), §7 (lead duties per landing), §8 (worktree and firewall policy;
> its branch table is superseded by RESUME.md §0.2), Appendix A (the headed §11 reconnect review) and Appendix B (the
> brief template). §1's standing table and waves 0-2 are history: the live state is RESUME.md §4, the live order of
> work is RESUME.md §5, the board is STATUS.md, the policy is CLAUDE.md (= AGENTS.md). Times were converted from UTC
> to Arizona time on 2026-09-12; shas quoted here predate the 2026-09-12 trailer strip (map:
> `reviews/takeover-20260909/trailer-strip-20260912/commit-map.txt`).

## 0. Goal and the two rules that shape everything

Finish the whole §B-1 roadmap as fast as possible without lowering quality. Two binding rules from the user:

1. **Inventory-first, never whack-a-mole.** The whole battery runs ONCE on a changed tree; every failure is root-caused in
   parallel (one read-only worker per failure); the fixes land as ONE group across worktrees; then ONE battery.
2. **The lead verifies personally**, for every failure, that it is a TRUE failure (the engine did the wrong thing, per evidence
   the lead looked at), and for every fix, that it is WELL IMPLEMENTED (the lead reads the whole diff; the detecting test is red
   on the exact defect and green after; the named mechanism is what the code changes; nothing masked, widened or patched
   around). Worker "PASS" and "root cause found" are claims to verify, never results.

## 1. Standing (lead's judgement, 2026-09-11 14:20 MST)

| # | Item (RESUME §B-1) | % | Δ vs 18:11 | Left before 100 |
|---|---|---|---|---|
| 1 | Controller boundary + wire/replay compat | 97 | 0 | Source42 attempt 4 green on the merged tree |
| 2 | Restore / identity / state inventory | 94 | 0 | Source42 attempt 4 restoration + native gates |
| 3 | Gameplay fixtures + minimizer + matrix | 85 | 0 | 3b door/crab/craft + UI-order fixtures, seat-side pie closes; 3e product call |
| 4 | Presentation + performance (100-200 ms feel) | 15 | 0 | 4b preview-event ledger, optimistic projectiles, UI follows the preview; 4c headed measurements; 4d sim-speed + rollback decision |
| 5 | Breadth + verification debt | 88 | -2 | 5b 3/4-peer packs, co-op, PvPvE, full lifecycle; 5c WSL2 leg (never run) |
| 6 | H4 reconnect + host moderation | 79 | 0 | battery 2 with resync-frame proof, fencing exercised, Source42 green, headed §11 review with the user |
| 7 | Lobby / session UX + robustness | 20 | 0 | 7a-7f (RECOMMENDED.md) |
| 8 | Discovery / internet play + roadmap | 10 | 0 | 8b dedicated host (Mac persistent server), 8a directory + NAT on the Mac, 8c lobby v2, 8d feel items |

Overall **61%** (mean). Grouped: flawless normal MP (1, 2, 3, 5, 6) ≈ 89%; measured feel (4) 15%; UX + discovery (7, 8) ≈ 15%.

**Status rule (user, 2026-09-11):** every status answer gives, per item and overall, the delta since the previous ask,
what moved it, and what is running now; a lane that lands without moving a number is a NEGATIVE delta with its reason.

## 2. Worker routes and runners (each verified by a run on 2026-09-11)

| Route | Model / level | Runner | Model gate (evidence) | Use for |
|---|---|---|---|---|
| Opus here | Claude Opus 5, effort max | Agent tool: `cortex-opus-engineer`, `cortex-opus-verifier`, `cortex-opus-visual-reviewer` | project agent definitions pin `claude-opus-5` / `max` | engine surgery, design-heavy lanes, independent verification, EVERY screenshot or visual question |
| Grok Windows | Cursor Grok 4.6 Extra High Fast | `grok-workers/win_cli/win_job.py <name> <worktree> <prompt.txt> --task "..."` | init line must say "Extra High Fast" (`cli_runs/<name>/agent-output.jsonl:1`) | batteries (run-only), triage, reviews, mechanical implementation from a lead-fixed design |
| Grok Mac | same | `grok-workers/mac_cli/mac_job.py <lane> <script.zsh>` | same gate on the lane's `agent-output-*.jsonl` | Mac clean-clone builds, selftests, A7 group |
| SWE-2 | SWE-2 Max (free) | `grok-workers/devin_cli/devin_job.py <name> <worktree> <prompt.txt> --task "..."` | export `agent.model_name` must be "SWE-2 Max" (`cli_runs/<name>/export.json`) | bounded UI / feature work with named seams |
| Claude Mac | Claude Code Opus 5, effort max | `grok-workers/mac_cli/mac_claude_job.py <lane> <prompt.txt> --task "..."` | init line `"model":"claude-opus-5"` (`<lane>/result.jsonl:1`); launchd gui/501, plain SSH is not logged in | Mac builds/verification of worker branches, extra implementation lanes |

Rules for every route: workers never push, never touch main or `D:\Projects\p4b-interp-validation`, never launch the engine
outside `tools/win32_test_runner.py`, never widen a mask, never write attribution; a worker may commit on its own branch with
plain messages; the lead reads every diff line before a merge; `WORKER_RULES.md` travels with every brief; every spawn lands in
`grok-workers/SPAWN_LOG.md` with its evidence line (the runners append it). Build cap: two Windows MSBuild lanes (`CL=/MP6`),
none while the Source42 chain builds (`/MP12`); the Mac builds independently. Windows worker builds wait on `Get-Process MSBuild`.

## 3. Wave 0 — preparation (DONE 2026-09-11 14:20 MST unless marked)

- [x] Firewall: every existing worktree executable path has inbound+outbound allow rules (elevated run 14:10 MST, 126 paths).
      The sidecar for planned worktrees (`grok-workers/firewall_planned_worktrees.txt`) was added to the script, but that
      elevated run did not execute (prompt missed) → **new lanes reuse merged worktree directories** (section 8) until the
      next elevated run, which the lead requests only when the user is at the PC.
- [x] Trailer strip: 15 branches rewritten message-only, trees byte-identical, 0 hits per branch, ten backup branches
      force-pushed with lease, `takeover-msvc` pushed new. Map: `reviews/takeover-20260909/trailer-strip-20260911/map.txt`.
      Main local `de9fa9e3de` stays unpushed until the checkpoint of wave 2 (origin main `79dc711958`).
- [x] Runners: `devin_cli/devin_job.py` (SWE-2 Max, smoke OK), `mac_cli/mac_claude_job.py` (Opus 5 max via launchd, smoke OK),
      `win_job.py` / `mac_job.py` gates already proven (xhigh accepted, high refused).
- [x] Mac Cursor profiles: attribution OFF in `cortex-workers/cli-home` and the user's `~/.cursor` (backup `.bak-20260911`).
- [x] Fresh manifest for battery 2: `grok-workers/w40-battery-2/build-manifest.json` (fixgroup-1 `67d2d70aad`, exe `21f8a6e6`).
- [ ] Wave-1 briefs written under `grok-workers/w4x-*/prompt.txt` (the lead writes them at launch).

## 4. Wave 1 — parallel lanes (all start together; more than one item moves per window)

| Lane | Item | Worktree / branch | Route | Deliverable | Acceptance (lead-verified) |
|---|---|---|---|---|---|
| W40 battery-2 | 6, 2 | run-only on `D:\Projects\fixgroup-1` exe `21f8a6e6` | Grok Windows | `w40-battery-2/REPORT.md`: 13 selftests, 8 H4 gates, 4 B1 gates, 2 lobby runs, heal e2e at delay 3; per resync: host log lines, every peer's lockstep frame fields, trace tick range, first gap/duplicate | every trace contiguous, no duplicate; resume == drop frame on drop paths; heal/rejoin resyncs lose and double nothing; the lead reads every frame table |
| W41 mac-fixgroup-1 | 6, 5 | Mac clean clone of `origin/stage2/fixgroup-1` @ `67d2d70aad` | Grok Mac | build (GNS), 13 selftests, native suite diag off/on, A7 group (5 arms) | PASS lines identical to W34 apart from paths; A7 arms 5/5 with the drop-frame semantics; binary sha recorded |
| W42 feel-measure | 4 | read-only on the fixgroup-1 exe | Grok Windows | `w42-feel-measure/REPORT.md`: 100/200 ms numbers vs the 4a table, PASS/MISS/NOT MEASURABLE, fields needed | numbers only, each with run dir + JSON path; the lead spot-checks two against the raw reports |
| W43 fencing-warm | 6 | `D:\Projects\fencing-warm` / `stage2/fencing-warm` | Opus here (engineer), build lane 1 | `-net-join-wait-for <path>` option + selftest, driver diff (`w43-fencing-warm/driver.diff`), three fencing gate runs | `host_fenced_the_old_transport` > 0 with the second client committed while Running, three runs; if still 0, the exact drop point of the synthesized `PeerDisconnected`; Grok review of the diff (W47) |
| W44 item4-ledger | 4 | `D:\Projects\item4-feel` / `stage2/item4-feel` | Opus here (engineer), build lane 2 | 4b preview-event ledger, first slice: a sound/effect emitted by the preview plays once at the preview tick and its canonical repeat is suppressed; ledger keyed by emitter uid + preset + tick, expiring after D ticks; detecting selftest + a fixture run at 100/200 ms fake lag | sound plays once and early (measured in the e2e report), no change at D=0, mod compatibility untouched, red-then-green test |
| W45 item7-ux | 7 | `D:\Projects\item7-ux` / `stage2/item7-ux` | SWE-2 (Devin), builds when a lane is free | 7b: "Resyncing…" overlay on `DrawLockstepStallOverlay`, in-match toasts for heal/left/rejoined/pause, auto-picked input delay shown in the lobby; then 7a lobby + in-match chat (lobby codec message, scrollback, relay rebroadcast) | menu-script / probe coverage per feature, selftests green, Mac build green (W49), screenshots judged by the Opus visual reviewer |
| W46 item8-dedicated | 8 | `D:\Projects\item8-discovery` / `stage2/item8-discovery` | SWE-2 (Devin), builds when a lane is free | 8b `-net-dedicated`: host role with no seated human (star relay, authoritative lobby, snapshot source), headless, selftest + a 2-client e2e through the dedicated host | e2e 600 ticks host==clients with the dedicated host never seating; runs headless on the Mac (the persistent-server end game) |
| W47 review-fixgroup | 6, 2 | read-only | Grok Windows | Extra High re-review of the High-generation diffs in fixgroup-1 (W18/W21 fixes) against `FIX_GROUP_1_DESIGN.md`; contradictions named with file:line | the lead reads every finding against the diff |
| W48 attempt3-inventory | 5 | read-only | Grok Windows | every one of the 14 attempt-3 non-pass cases mapped to its fix commit with the evidence lines; the fencing crash stack vs `c477a62955` | the lead re-derives two mappings from raw logs |
| W49 mac-verify | 7, 8, 5 | Mac lanes per branch | Claude Mac | builds each pushed worker branch from a clean clone, runs selftests + the relevant e2e, reports; later 5b 3/4-peer packs and lifecycle on the Mac | reports carry command, binary sha, PASS/FAIL lines verbatim |

Lane order of priority when a build slot is contended: W43 > W44 > W45 > W46.

## 5. Wave 2 — the merge, the checkpoint push, Source42 attempt 4

1. W40 + W41 green and read; W47/W48 findings closed; W43 diff read and reviewed.
2. `git -C D:\Projects\fixgroup-1 merge --no-ff stage2/fencing-warm` → build alone (`/MP12`, no worker builds) → ONE battery
   (W40's brief again, new manifest) + Mac clean clone (W41 again). If anything is red: inventory ALL reds, one read-only worker
   per red, lead rules true/false failure, fixes as ONE group, ONE battery. Never one gate at a time.
3. Merge `stage2/fixgroup-1` into main (`D:\Projects\p4b-interp-validation`, `--no-ff`), tree clean apart from the two vendor
   `.lib` files, trailer scan `79dc711958..HEAD` = 0, `git push origin stage2/p4b-interp-lockstep`, record the push and the
   remaining failures in RESUME.md and STATUS.md.
4. Source42 attempt 4, from `reviews\recovery-2026-09-07\contract-audit`, no MSBuild/engine/`claude -p` running:
   `python run_family.py --source 42 --breadth-out D:\mx\s42b3 --matrix-out D:\mx\s42-4 --mac-attempt 4`
   (`family-source42.json` must be absent in that directory; the Mac attempt dir `source42-attempt4` must be new).
   Chain ≈ 5 min, remaining ≈ 5 min, breadth ≈ 50 min, matrix hours; worker builds (two lanes) resume after the chain.
5. Verdict = chain + remaining + breadth 81/81 accounted (pass or explained residue with evidence) + matrix 106 reviewed +
   Mac attempt 4 green. Then STATUS: 1 → 100, 2 → 100, 5 → 95, 6 → 90; cleanup of the six merged worktrees (section 8).

## 6. Wave 3 — items 4, 7, 8, 3, 5 to 100

Same cycle for every slice: worktree → implementer → reviewer → ONE group merge into the next `fixgroup-N` → ONE battery →
main → push → STATUS. Slices, in the order value ÷ effort with the user's decisions applied:

- **Item 6 close-out:** the headed §11 reconnect review with the user (appendix A), B2 moderation GUI probe on Windows.
- **Item 7:** 7b overlay/toasts/delay display → 7a chat → 7c mod-mismatch UX → 7d compression, beacon, delta frames,
  address re-resolve → 7e replayable post-resync rounds, periodic snapshots, telemetry bundle, replay selftest, crash-rejoin
  prompt → 7f replay browser + post-match report.
- **Item 8 (hosting decision, user 2026-09-11):** players self-host; anyone can host and join. 8b the dedicated headless host
  first (it becomes the PERSISTENT SERVER/WORLD on the Mac once direct connection is solid — the end game), then 8a the session
  directory (register/heartbeat/list) hosted on the Mac (preferred) or this PC + NAT rendezvous through GNS ICE/STUN, then 8c
  lobby v2 (picker, team builder, loadout, ready-check, kick, password), host migration, spectators, peer cap, cheat-surface
  hardening, platform icons; 8d adaptive delay, audio smoothing, bounded rollback per the item-4 measurements.
- **Item 4:** 4b ledger (W44) → optimistic projectiles → Activity UI follows the preview → 4c headed measurements on the
  private desktop at 100/200 ms and two render rates (screenshots to the Opus visual reviewer) → 4d sim-speed program and the
  rollback verdict from measurements.
- **Item 3:** 3b door / crab / craft under the same script, the UI path of an AI order, the seat-side pie closes; 3e is a
  product call the lead escalates with options when the design fork appears.
- **Item 5:** 5b 3/4-peer packs, co-op, PvPvE, leave/drop/rejoin/resync/replay/rematch/pause lifecycle (Mac + Windows), 5c the
  WSL2 leg in a window with no Windows builds.

## 7. Lead duties per landing (binding, every lane)

1. Read the whole diff. Check the detecting test is red on the exact defect and green after (run it, do not trust the report).
2. Check the mechanism the worker names is the mechanism the code changes; no mask, tolerance, exclusion or fixture patch.
3. Re-derive at least two numbers of every report from the raw files it cites.
4. Record the landing: STATUS number moved (or a negative delta), SPAWN_LOG row present, evidence paths in RESUME.md.
5. Commit through `grok-workers/git_commit.py` only; scan for trailers before every push; push at every verified checkpoint.

## 8. Worktree and firewall policy

Executable paths with firewall rules (no prompt): every current worktree, incl. `fencing-warm`, `item4-feel`, `item7-ux`,
`item8-discovery`. New lanes REUSE a merged worktree's directory by checking out a fresh branch in it:

| Directory | Current branch (merged) | Reuse for |
|---|---|---|
| `D:\Projects\takeover-build` | `stage2/takeover-msvc` | next integration tree `fixgroup-2` |
| `D:\Projects\alias-walk` | `stage2/alias-walk` | item 3b fixtures |
| `D:\Projects\audio-owner-registry` | `stage2/audio-owner-registry` | item 7 chat (second UX lane) |
| `D:\Projects\value-observations` | `stage2/value-observations` | item 8 directory client |
| `D:\Projects\h4-secondary` | `stage2/h4-secondary-2` | item 4 sim-speed |
| `D:\Projects\hold-pause` | `stage2/hold-pause-2` | item 5 lifecycle packs |

Never move, copy or remove a directory under `D:\mx` or a run root (junctions); worktree removal only after listing and
unlinking every reparse point (AGENTS.md). `takeover-fixes` (`stage2/a7-journal-wip`) stays parked: the A7 journal/observer
instrumentation becomes a positive only after its own Mac-built selftest pass and a lead review.

## Appendix A — the headed §11 reconnect review (the user at the desktop, ~20 minutes)

When: after Source42 attempt 4 is green and main is pushed (the reviewed build must be the approved one); the lead announces
readiness in chat and finalizes the exact steps here first. Screenshots the user takes are judged by the Opus visual reviewer.

Preparation by the lead (before asking the user): a `headed_review.ps1` under `reviews/takeover-20260909/headed-review/` that
launches a windowed host and a windowed client of the approved executable on the user's desktop (two windows side by side,
P4 Alpha Duel, sound on), a `steps.md` with numbered steps and the expected on-screen result of each, and a `screens/` folder
the user drops screenshots into (Win+Shift+S, paste into the folder as `stepNN.png`).

The steps the review covers (final wording lands in `steps.md`):

1. Same-process loss: the client's link is cut (the lead provides the exact command or key); expected: a visible
   "reconnecting" status with a cancel and a manual-retry control, then automatic reconnection and play resuming from the
   held frame; the host shows the persistent roster line "Player X — disconnected / reconnecting" while the seat is held.
2. Crash and relaunch: the client is killed (the lead provides the command); on relaunch the startup offers "Rejoin the match
   in progress?" from the durable ticket; accepting reseats the same seat; refusing reports a clear stale/missing-ticket error.
3. Clean leave: the client leaves through the menu; expected: no pause, the seat closes at once, the old ticket cannot reclaim.
4. Moderation: the host opens the moderation panel (F6) during a hold: expected the disconnected seat, the wait/substitute/
   cancel controls, and the pause banner with the countdown on every window.

The user reports each step as PASS or FAIL in their own words with the screenshot name; the lead records the verdicts in
RESUME.md item 6b/6c and the evidence under `headed-review/`.

## Appendix B — brief template (every worker)

```
Read D:\Projects\reviews\takeover-20260909\grok-workers\WORKER_RULES.md and obey it.
Worktree: <dir>, branch <branch> at <sha>. You may edit only: <paths>. Everything else is read-only.
Task: <one paragraph; the design is FIXED as follows: ...>.
Verification you must run: <exact commands through tools/win32_test_runner.py or the named driver>; report every check verbatim.
Build: CL=/MP6; the two-lane cap is judged on compilers, not on MSBuild.exe processes: wait while cl.exe or link.exe from two other working directories run (idle MSBuild node-reuse processes linger for an hour after a build and mean nothing). No push. No attribution. Comments one short line or none.
Output: <report path> with the commands, the output paths, the failing lines verbatim; final message max 25 lines.
```

## Appendix C — hash map of the 2026-09-11 trailer rewrite

`reviews/takeover-20260909/trailer-strip-20260911/map.txt` (old → new per branch, TREE_SAME on every row),
`pre-tips.txt`, `post-scan.txt`, `origin-before-push.txt`, `origin-after-push.txt`. Older hashes cited in RESUME.md, STATUS.md
and worker reports (`84ce04b531`, `bcec2fcc70`, `184559b4ff`, `b629b407c1`, `be0a2672a9`, `6fa0fd6898`, `de6fcf8157`,
`ec89dd56fd`, …) name the pre-rewrite commits; the map resolves them.
