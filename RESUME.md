# CORTEX-MODERN — RESUME (the single live doc)

> **Created 2026-09-05**, replacing the 450 KB doc sprawl (`HANDOFF.md` 320 KB + `HANDOFF2.md`
> 134 KB + 15 phase plans). Everything still *live* is here. The originals are archived verbatim at
> **`D:\Projects\_archive\docs_archive_20260905\`** — consult them only for forensic detail on a specific
> past hunt.
>
> **This file is the resume point. Start here, every session.**
> Structure: §A what this project is · §B where we are + next action · §C the rules · §D build &
> verify · §E architecture · §F what's been done · §G what's next · §H quirks · §I paths · §J log.

---

# §A. WHAT THIS PROJECT IS

**Cortex Command Community Project** (CCCP) is an open-source continuation of the 2012 physics
action game *Cortex Command* — a 2D destructible-terrain game with per-pixel physics. It is
AGPL-3.0, C++, and has essentially **no working multiplayer**.

**This project — `cortex-modern` — is a fork that adds real multiplayer.** Deterministic lockstep
netcode over GameNetworkingSockets: 2-4 players, PvP / co-op PvE / PvPvE, with desync self-heal,
mid-match rejoin, replays, LAN discovery, and internet play. Over time it also becomes a broader
engine-modernization effort (performance, AI, rendering).

**Repo:** `git@github.com:Madreag/cortex-modern.git` — a **public GitHub fork** of
`cortex-command-community/Cortex-Command-Community-Project`. Author of record: Erol Germain-Gomuc
(Madreag).

### The end goal (binding, the user's words)

Perfect-feel multiplayer at **100-200 ms ping** — no lag, no warping, clean prediction.
**Flawless normal MP + UX (join / leave / rematch / reconnect) comes strictly first.**
Measurements pick the endgame technique (bounded rollback and/or prediction), not *whether* it is
pursued.

### The work standard — "no compromises"

When a hard problem offers an easy way out — serialize instead of fixing the race, disable instead
of debugging, defer instead of solving, narrow the goal instead of hitting it — **the easy way out
is not the answer.** Push through and build the result that gives up nothing. This is *not*
risk-aversion: choosing the low-effort option *because* it is safer is itself the compromise being
rejected. "This is hard" is not a reason to stop. A fallback is a last resort, used only when the
full solution is **proven** impossible — proven, not assumed after resistance. Genuine **product
decisions** (a mod-compatibility break, a fork in design intent) still escalate to the user;
difficulty and risk do not.

---

# §B. WHERE WE ARE — START HERE

**2026-09-11 00:29 UTC - NEXT ACTIONS (the resume plan; the lead is the Cursor agent, workers are Grok 4.6 subagents per AGENTS.md "Delegation policy").** State: main `c8f8188ae0` (Source41) unpushed 138 ahead; worker tree `takeover-fixes` holds the Mac-verified positive20 group plus ~650 lines of unbuilt journal/observer WIP (`handoff-20260910/post-positive20.patch`); the Source41 family is complete (matrix reviewed, breadth 81/81 with 13 unreviewed non-pass cases); nothing is running. Order of work:

1. **Commit the positive20 group** on `stage2/takeover-next` as per-concern commits (patches proven byte-exact against the tree before use; attribution scan; the journal/observer WIP stays out of these commits and is committed separately once it builds). Then push the branch to origin as a backup (a push of a worker branch is a backup, not a checkpoint).
2. **Triage the 13 Source41 breadth non-pass cases** against the Source38-40 verdict tables (`lanes/source33-lanes/report.md`): known residue vs Source41 regression vs harness/fixture defect. First: `fl200` auto-delay (Source40 gave 14) and the `Rejected a AIOrder command from a peer that does not control team 1` host exits (transport-authority commit `9b84ddc7df` suspect). Fix the harness/fixture defects (`heal` import path in the breadth wrapper, `compat_review_extra.lua` invalid `SoundSet:HasAnySounds`, the `compat_deferral` comparison's missing documented UID exclusion, if that is what it is).
3. **Land the tools-only `compare_snapshots.py` comparator repair** on main (commit + local merge), rerun `mp_snapshot_p5` and the blocked `peer_roundtrip` on the unchanged approved executable.
4. **`AI_StuckForTime` residual** (`heal_global_d0/d3`): field-specific causal investigation with a detecting control; no mask, no local-AI serialisation.
5. **Journal/observer WIP**: compile-check on the Mac (cheap); if it builds and the positive20 gates still pass it becomes positive21, else set it aside via the handoff patch and finish it as the next group.
6. **Windows candidate build of `takeover-fixes`** (Final, `/MP12`, alone), then the family: fast gates, native, healing, invariance, restoration, transaction, both fuzzes, breadth, matrix = Source42. Merge `stage2/takeover-next` into main when green, push the checkpoint, record here.
7. Then the long-red H4 gates (`clean_leave`, `reclaim_socket`, `fencing_two_transports`, `rejoin_after_resync`; "activity ended in state Over" residue), B1 substitution 2/4, then section B-1 items 5b, 7, 8 and the measured 100-200 ms feel.

Every engine launch through `tools/win32_test_runner.py`; at most two `/MP6` build lanes and none during the lead's build; commit as you go, push every verified checkpoint.**

**2026-09-10 23:49 UTC - RULES AND CLEANUP. The user retired the "commit/push only when asked" convention: commit as you go (focused per-concern commits as soon as a piece is built and run) and push every verified checkpoint to origin several times a day; only upstream PRs stay gated (AGENTS.md, CLAUDE.md, this file section C, and the untracked pointer files in every worktree are updated). Reviewed cleanup executed (reviews/takeover-20260909/cleanup-20260910/REPORT.md, plan.json, deletions.jsonl): 120 GB freed on D: (126 to 247 GB free). Removed: 14 worktree checkouts of merged/reviewed branches (all branches kept; the round-start-review checkout had 484 lines of uncommitted reviewer tests, preserved as cleanup-20260910/preserved/round-start-review-8f493e8ecf.patch), 27 concluded lane run roots under D:/mx, the heavy raw state of the per-job-verdict-green jobs of the Source38 and Source40 matrices (every FINDING/TRIAGED/FAIL/OPEN job kept whole, all verdicts/logs/traces kept), and uncited Sept 5-6 loose runs. Worktrees remaining: cccp, p4b-interp-validation, takeover-fixes, xarch-combat. D:/mx/s41 (current family evidence), s41b3, s41r2, the breadth baselines and the pinned executables are intact. On the Mac only the 13 evidence tarballs already hash-verified on Windows were removed; the Mac disk pressure (91% used) is not Cortex work (corefall 72 GB, moltdash-v3 27 GB, Library caches).**

**2026-09-10 22:07 UTC - RECOVERY HANDOFF after the Codex usage-limit stop (thread 01a07bf2-1ee0-7b63-86f8-21dc6ef8e3cc; root and its three subagents errored at 02:47:05 UTC with usage_limit_exceeded; nothing was recorded between 02:34 and the stop). Reconstructed from the thread's tool log and the disk. Main D:/Projects/p4b-interp-validation is unchanged at c8f8188ae0 (Source41, 138 ahead of origin, unpushed); approved executable bb3cf264b4e7 unchanged. The worker tree D:/Projects/takeover-fixes (stage2/takeover-next HEAD 60cb698146 = main + round-start 8f493e8ecf + B2 moderation 703e3f1202 + two composition commits) still carries the whole uncommitted group (72 modified/16 untracked files vs main, +7991/-345, never committed, never built on Windows); a non-destructive snapshot (patch, untracked tarball, hash manifest, README) is in reviews/takeover-20260909/handoff-20260910/. Its verified state is positive20 (Mac manifest 29938cd8ba99, GNS binary 238066fa01d9): ten network CLI selftests, controller-frame, native-graph with diagnostics off and on, the same on the no-GNS build (native-positive20-nogns-verified-0243, copied back today, 81 files hash-verified), and, finished at 02:56 UTC after the stop and unrecorded until now, THE FULL A7 GROUP PASSES ON POSITIVE20: stagger_seat_survives (902 matched frames), silent_socket_bound (12,002), leave_ack_dropped (300), slow_resync_save (12,002), coop_hand_back (12,002), source unchanged before and after every arm (a7-verified-mac-group-positive20-0256, 283 files hash-verified). The 02:34 red (craft-fixture duplicate-ID Console errors) is therefore closed by positive20's LuaMan.cpp fixture isolation (craft-fixture-isolation-20260910/fix.patch). On top of positive20 the tree holds about 650 lines of UNBUILT A7/B2 journal and observer instrumentation (new NetRecoveryJournal.h/.cpp, NetA7Journal helpers, hooks in NetLockstep.cpp, host observations in NetReconnectSession.*, NetSession.*, NetMatchService.cpp, NetReconnectTxCache.*, NetLobbySession.cpp, vcxproj/filters/meson entries) that root and the adversarial reviewer were mid-edit on when the limit hit (last write 02:47:29 UTC); it is isolated as handoff-20260910/post-positive20.patch and has never been compiled: a draft to review, not part of positive20. The Windows Source41 breadth v3 (D:/mx/s41b3, unchanged exe) finished on its own at 02:52 UTC: 81/81 collected, 68 pass, 13 non-pass, UNREVIEWED: fl200 (auto delay differs from 14 on both peers; Source40 gave 14), h4_clean_leave_1, h4_reclaim_socket_1, h4_rejoin_after_resync_1 (host exit 1 with "Rejected a AIOrder command from a peer that does not control team 1"), h4_fencing_two_transports_1 (client2 setup: connection closed by remote host), h4_peers_3_4_regression_3 (audio checkpoint restoration failure on the returner, the audio-flake class; the other four repetitions pass where Source40 was 0/5), b1_substitute_commit and b1_substitute_returner_wins (host and stayer exit 1, no seat drop evidenced), heal (harness: ModuleNotFoundError recovery_e2e in the breadth wrapper, not an engine result), compat_deferral_source22/approved (output differs from Source22, no UID exclusions applied), compat_extra_source22/approved (the fixture calls SoundSet:HasAnySounds and fails identically on both rungs: a fixture defect). The four H4 gates were already red with known residues on Source38-40 (lanes/source33-lanes/report.md, section Source40); whether the AIOrder rejection is a new Source41 symptom of the transport-authority commit 9b84ddc7df is open. Matrix: the continuation review (source41-matrix-review/continuation-20260910/review.md) passes 42/44 new jobs; heal_global_d0/d3 stay OPEN_SHARED_STATE_RESIDUAL (host-only AI_StuckForTime NumberValue on UID 1048653, written by SharedBehaviors.lua:547, consumed by the stock Automovers controller); mp_snapshot_p5 stays failed (the comparator handles only Activity1 and picks the brain, not the controlled actor; the narrow tools/compare_snapshots.py repair passes 61 controls and the retained pair but lives only in takeover-fixes); peer_roundtrip stays blocked. Other frozen but unexecuted subagent output: the B2 live-driver package (b2-live-group-20260910/CONTRACT_V2.md, continuity_oracle.py, live_group.py, recovery_codec.py) and the observer plan (b2-review/b2-native-observer-plan-20260910.md, lobby-transfer-hardening-20260910.md). No Windows or Mac engine, build or wrapper is running; the Mac has 39 GB free (91% used), D: has 126 GB free. No merge, commit, push, cleanup or milestone acceptance. Delegation policy changed by the user today: workers are Cursor subagents on Grok 4.6 (xhigh reasoning, fast mode), briefed narrowly, reviewed and corrected by the lead; see AGENTS.md.**

**2026-09-10 02:34 UTC: complete-input recovery group passes all network tests on Mac positive19 f393f7319954 (manifest 7014da270de8cba0); all 27 runtime groups pass, including repeated full restoration at delays 1/3 and 3/1. Native assertions now pass and the process exits zero, but strict native acceptance remains RED for duplicate-ID Console errors in the new craft fixture; its canonical source remains registered during ordinary second-copy loading. That fixture setup is under review; diagnostics are not suppressed. Positive18 f18d74c5d1c6d7d passes strict real A7 co-op recovery: original selected unit 1048615 returns, local player state is preserved, and all 12,002 resumed frames 282-12,283 match the declared checksum oracle. Its 54 retained files and the 159 files from native/build controls 16-18 are copied back and hash-verified. Positive17 retains the missing-test-brace build failure; positive18 retains the old ACK-oracle and private-registration failures. Source41 Windows breadth is still active, last observed 46/81; main c8f8188ae0 and approved bb3cf264 remain unchanged/unpushed. No integration or milestone acceptance. B2 live fault capabilities, normal MP, Windows candidate verification and 100-200 ms feel remain open.**

**2026-09-10 02:18 UTC: grouped complete-input recovery and native restoration review. Source41 main c8f8188ae0 and approved executable bb3cf264b4e7 remain unchanged and unpushed; the guarded 81-case Windows breadth continuation is running at D:/mx/s41b3. The retained snapshot comparator failure is independently reproduced and the narrow Activity/controlled-actor role correction passes all 61 controls and the exact retained pair; the original red result stays intact, and peer roundtrip remains pending. Mac positive14/15 pass ten network CLI tests plus controller-frame, but three native graph checks fail: missing borrowed owner reference, unresolved nested vector owner, and inventory loss during restored startup. All 284 regular files from attempts 11-15 are copied back and hash-verified. The complete-input recovery, transfer progress, local UI/alias and native startup repair group is frozen or finishing review in takeover-fixes, not yet built together. Full MP, Windows candidate, strict co-op/repeated recovery, B2 socket faults and 100-200 ms feel remain open. No integration, new accepted checkpoint, cleanup or milestone completion.**

**2026-09-10 01:54 UTC: Source41 continuation collected 44 additional jobs; mp_snapshot_p5 remains recorded red because the comparator handles only Activity1 and selects the brain instead of the controlled actor, and peer_roundtrip remains unrun/blocked. A focused isolated comparator repair passes 61 detecting tests and the retained pair; root review is pending. The continuation stopped before breadth; a fresh breadth-only guarded wrapper is being prepared. Main c8f8188ae0 and approved bb3cf264 remain unchanged/unpushed. Isolated Mac positive12 fe2c0bc84cff builds and passes nine of ten network CLI tests plus controller-frame; lockstep crashes on a fixture manager used before construction, and native graph fails on an invalid const-Actor Lua fixture call. Corrections are in progress, not verified. Review also found resync loses accepted controller/sound inputs and remote backlog across repeated heals; bounded complete-input transport, exact-target retention and detecting tests are being implemented together in takeover-fixes. No integration, new verified checkpoint, cleanup or full MP acceptance.**

**2026-09-10 01:25 UTC - work resumed after reboot; grouped resync repair in progress.** The user confirmed responsiveness and requested efficient grouped fixes and parallel reviews. Source41 main c8f8188ae0 and its approved bb3cf264b4e7 executable remain unchanged. The retained 60-job matrix portion has now been reviewed: no new named-oracle failure, with seven new audit jobs still requiring raw-state classification and earlier coverage gaps still open. The first continuation wrapper failed before launching an engine because its subprocess adapter omitted STDOUT; its inputs and failure remain intact. Reviewed v2 (six detecting guard checks) is collecting exactly the 46 unfinished/interrupted jobs under D:/mx/s41r2, then the original 81-case breadth under D:/mx/s41b2; continuation-run-v2.json records progress. Mac positive10 60570fa497cf now passes slow-save attempt4 (7273 ms save, 12002 matching resumed frames) and silent-socket attempt3 (eight expected expirations, successful reclaim, 12002 matching resumed frames). All 354 transferred regular files were independently hash-verified. Strict co-op selection remains red and preserved. In takeover-fixes HEAD 60cb698146, UNBUILT work groups codec18 player observations, acknowledged command sequences, a bounded resync envelope, ownership/drop-map restoration, exact-target resync priming, and peer-local player/UI restoration. Review caught pending selection loss, delayed-command restamping and GUI alias lifetime problems; those fixes and detecting native/Lua tests are being completed together. Ordinary mod semantics, shared references and canonical controller state remain binding. No candidate build, source export, merge, commit, push, cleanup or full MP acceptance is claimed. Current evidence: reviews/takeover-20260909/a7-positive10-postreboot-review.md and source41-matrix-review/followup-20260910-reboot/review.md. 

**2026-09-10 01:02 UTC - PAUSED FOR USER REBOOT.** User reported persistent desktop sluggishness and is rebooting. All owned Windows family/matrix/breadth wrappers and their verified descendants were stopped; no Mac engine remains running, and all subagents are idle. Source41 main c8f8188ae0 stays unchanged and unpushed. Matrix D:/mx/s41 has 60 NEEDS_REVIEW, 45 NOT_RUN, and the interrupted lobby_3_rejoin_100 still marked RUNNING in its original untouched matrix.json; it must be rerun in a fresh output, not counted as failed or passed. Breadth has not started. Pause/process measurements and all 47 modified/untracked non-vendor source files from takeover-fixes HEAD 60cb698146 are byte-verified under reviews/takeover-20260909/pc-sluggishness-20260910; wip-source-manifest.json records hashes. Mac positive10 60570fa497cf passed ten CLI checks; strict co-op now proves wrong local selection (1048615 expected, 1048577 observed). Slow-save attempt3 reached the journal's 16 MiB harness cap and is NOT a gameplay verdict. The repaired a7_support.py raises only journals to 64 MiB with bounded reads, passes 30 controls, and is NOT deployed to Mac yet. Root began codec18 PlayerBindings observation support and a snapshot-construction command guard; this is INCOMPLETE, UNBUILT WIP, with no source export after positive10. The new codec calls ReadVar but the existing helper is ReadVarOrFail: repair before building. Binding production/apply, peer-local restore after full checkpoint commit, canonical ownership envelope, pending reseats/future commands, and relevant regression gates remain to implement. Detailed restart order is pc-sluggishness-20260910/REBOOT_RESUME.md. No merge, commit, push, cleanup or full MP acceptance is claimed. Keep Windows tests paused until the user's PC is responsive.

**2026-09-10 00:43 UTC - co-op ledger and UI completion pass; player rebind remains RED.** Main/Source41 remain unchanged; Windows matrix has 31 completed jobs awaiting review, one running and 74 not run, with breadth queued. The fixed 26-job review found no new engine failure but explicit async/nullable/native coverage gaps (reviews/takeover-20260909/source41-matrix-review/review.md). Isolated Mac positive9 `147969bb776e` passes ten CLI selftests, the old co-op UID oracle (12,002 matching resumed frames), and actual UI inputs with both peers completing simulation tick 602. Co-op still steals the host brain during snapshot startup and copies host-local player/camera/input state onto the client; the exact paths are in wip-resync-ownership-review.md. Slow-save returner survives 7,335 ms and reloads, but the stock duel then ends. Silent8 with the unarmed test fixture exposes missing writable save-module initialization under -module. Positive10 `60570fa497cf` builds and passes ten CLI selftests: it initializes saved-game storage for selected modules, gives lobby session decisions the actual shared clock, and adds local selected-unit observations. Its stricter co-op control is running; player-rebind fixes are not implemented. Driver guards pass 25/25. All 348 prior-run files are hash-verified in a7-verified-mac-group-0044. No main merge or push; full MP/latency acceptance remains open.**

**2026-09-10 00:15 UTC - P21 leave fixed on Mac; additional A7 failures collected.** Main and Source41 remain unchanged, Windows matrix active (26 jobs awaiting review at the fixed review snapshot, failure loads then running; 81-case breadth remains queued). Isolated candidate positive7 `f1eba81f6db6` builds and passes ten CLI selftests plus the corrected real leave-ack test: 300 matching applied frames, seven retries, one unacknowledged/ambiguous leave, unchanged ticket. Positive6 fails the same corrected driver with missing settlement. The outer leave wait could end before the protocol evaluated its deadline; the repair waits for settlement and counts the ambiguous ending once. Driver guards pass 23/23. Further real A7 collection is RED: silent8 expires all eight sockets and accepts reclaim, but the stock duel has ended before the late resync and saving Over is refused; slow save exceeds seven seconds and the waiting returner times out; co-op restores but loses the original live unit from the drop ledger and assigns another unit. These are retained and not waived. UI inputs still pass, but the old terminal oracle confuses prefetched input counts with completed simulation; two direct report fields and a strict consumed-queue/completed-tick oracle are now added locally, unbuilt (five pure oracle tests pass). A scoped keepalive sender during host snapshot save is also locally implemented, unbuilt. Investigate ownership/ledger ordering and use a declared long-lived activity for lifecycle timing; retain the ended-match failure for normal UX review. Mac source remains positive7; no main merge or push. See reviews/takeover-20260909/b2-mac-verification.md and the retained A7 run roots.

**2026-09-09 23:53 UTC - B2 real SDL inputs pass; first instrumented A7 socket arm passes on Mac.** The Source41 Windows matrix and queued breadth remain unchanged and active; main stays c8f8188ae0. In takeover-fixes, the real host/client F6 probe exposed insufficient label height and an 8-bit roster font drawn onto the 32-bit GUI buffer. Those repairs are built on Mac candidate 811b2725aaf0: both peers complete all 30 keyboard/mouse steps, keep playing while the panel is open, close through Escape and the actual Close button, and finish the same 601-frame terminal interval. The 600-frame-only validator's false rejection is retained; the corrected oracle checks the declared cap, accepted-frame interval and equal final frames. UI-plane PNGs are visually reviewed, and all 168 retained files are hash-verified under reviews/takeover-20260909/b2-verified-mac-ui-2344. This is not a full composited gameplay screenshot or normal multiplayer sign-off. The reviewed A7 journal is now composed in the isolated source, with an early refusal before a report could race an active worker. Candidate ed4e02df1db8 builds and passes ten network CLI selftests. Its first real Mac A7 arm, stagger_seat_survives, passes with the original decision/checksum oracles and actual Mac process evidence. The leave-ack arm is running as a retained control; no P21 production repair has been made. Twenty Windows driver guards and three real POSIX lifetime controls pass. Further A7/B2 socket faults, populated/long-name/resync GUI checks, Windows candidate verification, normal MP and 100-200 ms feel remain open. No main merge or push.

**2026-09-09 23:16 UTC - B2 composition failure reproduced; repaired Mac configurations pass scoped tests.** Main remains c8f8188ae0 and the Windows Source41 family remains unchanged and running (19 matrix jobs awaiting review, references running, 86 not started; the separate 81-case breadth wrapper follows). In the isolated B2 worktree, the earlier composed code fails its real lockstep selftest: the surviving client never receives the substitution status. That control is preserved on Mac executable 5ef99d28d8dc; its only build repair was a missing iostream include. The working repairs replace the Stop-message notices with authenticated codec-17 full seat snapshots, retain current seat metadata, validate the displayed seat/applicant transaction on clicks, and add a live F6 panel plus persistent roster and cached resync display. The exact exported working source builds on Mac with GNS (217b03beeb0b) and without GNS (1bd3454cee0a); all eleven CLI selftests and the native checkpoint suite pass in both configurations. New three/four-peer protocol cases also exposed and fixed missing invalid-Stop encoding validation. Build attempts and intermediate failures remain retained. Evidence: reviews/takeover-20260909/b2-mac-verification.md. This source remains uncommitted in D:/Projects/takeover-fixes, HEAD 60cb698146; no Windows candidate build, main merge or push. Actual GUI inputs/screenshots, five B2 socket fault windows, five corrected A7 arms, normal MP acceptance and 100-200 ms feel remain open. The A7 driver has 20 passing pure guards; its journal instrumentation is still unapplied and no real A7 run is claimed.

**2026-09-09 22:05 UTC - Source41 Mac verified; Windows matrix running; breadth preflight repaired.** Main remains c8f8188ae0, unpushed and unchanged. Windows fast, native, healing, prediction invariance, restoration, transaction and both fuzz gates pass; the transaction raw classifier records 10 render-scratch differences and is not a complete fidelity claim. The 106-job matrix currently has 13 completed jobs awaiting review and one running. Original breadth launched zero cases: its pin rejected six unexported ImGui sample fonts. The corrected guard verifies those files against the pinned Git blobs; 47 guards and the actual Source41 pin pass. A separate supplemental wrapper waits for the matrix/family to release the engine, then runs the full 81-case breadth stage at D:/mx/s41lanes. Mac attempt 3 passes all five native/game gates and all ten CLI selftests on binary 219bca93e01f; full retained evidence/source/bundle verification passes. Attempts 1 and 2 preserve classifier false failures (MEASURE diagnostics, then suffix PASS messages); 46 repaired Mac guards pass against both actual full logs. Reports: reviews/takeover-20260909/mac-measure-repair/REPORT.md and breadth-font-repair.md. All five additional A7 arms still need corrected launch barriers and oracles; their independent review is b2-review/a7-arms-review/review.md. The next worktree, D:/Projects/takeover-fixes at 60cb698146, combines round-start and B2; root-owned moderation repairs are INCOMPLETE and UNBUILT (dedicated codec-17 seat snapshots and retained seat metadata started; tests, stable action targeting and live-game panel pending). No build or main merge while matrix/breadth run. Normal MP/visual acceptance and 100-200 ms feel remain open.

**2026-09-09 21:33 UTC - Source41 integrated; full verification running; no push.** Main is c8f8188ae0 (138 commits ahead of origin). The six-branch batch reproduced reviewed tree 3f2f2987c75d99934d8be173321a891f83983998, then five takeover fixes landed: 9b84ddc7df transport authority, 96b31b7f7f activity icons, 77cb1ec576 codec 16, b6b60109ef observed frame-40 departure, c8f8188ae0 reconnect evidence. The prior failing controls and fixed native/graph/socket results are retained; four real frame-40 runs pass, while the old oracle accepted frame 15. The Mac harness final review has no actionable blocker; 116 combined synthetic guards and the real POSIX lifetime controls pass, but the new Mac game family remains unverified. The full unchanged-source family now runs build/export/preserve, Windows fast/remaining gates, Mac 5+10, 81 breadth cases and the 106-job matrix at D:/mx/s41lanes and D:/mx/s41; family-source41.* records actual progress. See reviews/takeover-20260909/source41-integration-review.md for exact scope, provenance and open coverage. No merge or main source edits while it runs. The five additional A7 fault arms, seven nullable-turret settled observations, missing open-upvalue control, round-start 8f493e8ecf, the reviewed-but-unaccepted B2 moderation branch, normal MP/visual acceptance and 100-200 ms feel remain open. Cleanup remains unauthorized.

**2026-09-09 ~02:20 UTC WORKFLOW CHANGE AND FIRST CHECKPOINT PUSH.** The user, on an independent reviewer's recommendation, authorised routine pushes of verified development checkpoints to origin after major fix groups (still no PRs, no squash or rewrite, no attribution anywhere, and cleanup of folders, worktrees, run roots or evidence remains UNAUTHORISED: the retirement inventory in this session's transcript had errors - `stage2_p2\gns_spike` is the live `GNS_ROOT`/`GNS_DEP_ROOT` dependency, `testing-audit-engine` holds 35 uncommitted changes with untracked `LuaThreadCodec` files, `archive-loads-review` holds uncommitted reviewer code, `archive-loads-review-base` holds `c2a5c0506` on no branch, `D:\mx` holds current lane evidence and reviewer worktrees, and `C:\mx-archive` is the only copy of the moved matrices). Retention policy from now on: keep the compact reproduction package of every important failure and fix (source and patches, fixtures, seeds, configuration, necessary raw state, exact binaries with symbols, manifests, logs, verdicts); identify redundant bulk separately; Source38's matrix stays until replacement coverage with before/after controls is shown. Milestone branch: `stage2/p4b-interp-lockstep`; reviewed worker branches merge into it directly, no more numbered integration branches. FIRST PUSH DONE: `1ab7fc2e1..4b39e3028 stage2/p4b-interp-lockstep -> origin` (284 commits: Source38 `32312c71e` plus the two snapshot-schema commits; the pinned manifest head verified equal to `32312c71e`; no attribution trailers in any message, the only hits for the forbidden word are evidence paths naming the pre-existing folder `reviews/claude-review-2026-09-08`, left unchanged with the history). Four further findings from that reviewer are in flight: A (`NetMatchService.cpp:724` `PumpSessionEvents` adds 15 ms per call and is called twice a tick, halving the 20 s seat hold; use monotonic elapsed time) and B (`NetSession.cpp:165` `TickAdmissionPlane` never expires silent connections, so eight of them block reconnects) go to the H4 lane when a worker slot frees; C (`run_audit.py:397` accepted loads exempt from the errors gate) and D (`run_audit.py:86` clock projection reaching inside multi-line native strings) are in the harness lane's `followup-1.md`. The harness port into `tools/` is a separate later concern (tested port, no generated evidence). RESUME.md is the single live document; the mission block in CLAUDE.md/AGENTS.md is to be reduced to policy plus a pointer once every binding rule is preserved here. Running now under the three-lane cap: `mo-link-review` (resumed), `matrix-harness` (with C and D), `sound-wire` DONE (the cut session had already landed the two commits; the fresh session verified them: `lanes/sound-wire/report.md` `## Follow-up 2`): `b4226eb9b` (the observation tables carry their round id again, `NetLockstep.h:156-158`; a frame of another round is read past with no dictionary and `discard` set, `NetLockstep.cpp:1167-1172`, shape fully validated, nothing bound, the frame handed on so the sender stays alive and counts as a straggler) and `abbb745e7` (bindings staged in block order and applied with a restart's `Reset()` only once the whole block has read, `:684-686`, `:780-787`), tip `abbb745e7`; the reviewer's three probes committed, red on `b10c319c9` plus the checks with the reviewer's own digits and green on the tip in both configurations; pinned tip builds GNS `cce1acf668` and no-GNS `1767ef3b74`: eleven selftests, native 530/0, restoration 13/13, fuzzes, SimBaseline and the retained recordings identical, relay bytes unchanged at 445/1598/3134; counter guidance for the lead (F2.5): `unresolved_observation_packets` is 0 on every gate including heal and is the condemning number, `stale_round_packets` non-zero is expected on heal. The reviewer's final delta re-check (`lanes/sound-wire-review/report.md` `## Follow-up 2`, 1,110 lines) calls `abbb745e7` READY TO INTEGRATE with no failing case: read-past in discard mode still parses and validates every field while skipping resolution, staging, binding and the `reserve` (the allocation guard), the malicious shapes refuse; the round id is correct by construction (three `m_RoundId` assignments, `otherRound` the same expression `HandleFrame` drops a straggler with, so a frame can never be decoded empty and then accepted); a fenced incarnation cannot reach the tables; the last-byte cuts leave the table untouched; F2.5's counter guidance holds (`unresolved=0 stale_round_packets=3` on the resync shape; a receiver still at round 0 gap-refuses but ordered delivery makes that window unreachable); the reviewer withdraws its scratch-dictionary suggestion in favour of the lane's read-past; battery green on its builds, relay bytes unmoved. FINAL TIPS READY FOR THE MILESTONE BRANCH (reviewed): `stage2/h4-phase-a` A6 `fed9dc3bd` (A7 in progress on top), `stage2/lobby-grace` `3d8d285d2`, `stage2/h4-phase-b` `673b39743` (their composition is `68d0771c2` on `stage2/integration-39`), `stage2/registry-audit` `1edc4f8d4`, `stage2/archive-loads-fix` `52ccf2487`, `stage2/sound-observation-wire` `abbb745e7`, `stage2/mo-link-restore` `be9e8d72e`; pending review: `stage2/matrix-harness` `cf816ba86` (lane `matrix-harness-review` running), `stage2/round-start-fixes` (lane running), H4 slice A7 (lane running). DONE ~03:40 UTC: the lead merged the reviewed engine tips directly into `stage2/p4b-interp-lockstep` (all clean, no trailers): `20f4edfe5` (A6 + lobby + B1 via their composition `68d0771c2`), `1241bc96a` (registry `1edc4f8d4`), `c9bc81df4` (archive-loads `52ccf2487`), `f8f70665d` (sound-wire `abbb745e7`), `b6ca51406` (MO-link `be9e8d72e`); main is `b6ca51406`, unpushed since `4b39e3028`; Source39's first chain run was killed by the harness at the link step (~03:50 UTC, second memory kill: the full build plus three building lanes - `round-start`, `matrix-harness-review`, H4 A7 - all killed too; rule tightened in CLAUDE.md/AGENTS.md and memory: no worker lane while the chain builds) and re-run ALONE with `chain_build_gate.py --label source39-1 --source 39` and the full fast-gate list: GREEN ~04:20 UTC - **Source39 is pinned as `grouped-build-f15fcba3` (executable `f15fcba375909177…`, head `b6ca51406`)**, build, export `combined-source-39`, preserve, the eleven selftests, authority match and fault, wounds, rng_particles, ui_bus, ai_defer, ai_defer_alias and the fault-armed per_machine all exit 0 (`chain-source39-1.json`). Running on it now: the breadth lane follow-up 5 (`lanes/source33-lanes/followup-5.md`, runs `D:\mx\s39lanes`: gates 1-4 and 6, the eight H4 gates with A6's expected flips, B1's four substitution gates, the five lobby lanes with the relay counters, the sound gates, the heal gate's counters, the compatibility ladder) and the Mac chain (`chain_mac_39.zsh`); the remaining gates run after the harness branch merges (its review resumed); H4 A7 DONE (`lanes/h4-a4-semantics/report.md` `## Slice A7`): both reported defects reproduced on the base and fixed - `93c952da1` (one `NetAdmissionClock` per session shared by setup, play, stalls and every resync; the runner takes the same clock; the lobby round ticks the plane; `admission_clock_ms` in the match report; defect A was worse than reported: the pump ran twice a tick, hundreds of times in a stall and never in a pause, and started behind the setup clock) and `7e9bba08b` (silent handshakes expire through the live path, and only those; `CheckTimeouts` shares the implementation); the branch merged the milestone branch cleanly (`a3ba11aad`, carrying the family and the fullscreen guard); seat hold measured in elapsed milliseconds under single, double, stalled and paused pumping with the pump-counting discipline kept as a live negative control; eleven selftests both configurations, native 542/0, SimBaseline unchanged, scratch 1 MB; A7.D lists the gates (the `crash_relaunch_provisional` P2 arm may shift as the correction). The independent review (`lanes/h4-a7-review/report.md`) calls `a3ba11aad` NOT READY: defect B's commit is sound (reproduced on the base, cannot expire a seated or slow-but-alive peer, the sim never sees it), but A's commit is correct at the plane and wrong in the composition - the service now hands `NetMatchRunner` an absolute clock while `NetLobbySession.cpp:119` still adds the session base to it, so from the first lobby round the plane runs on clock plus clock: P2 measured 50 030 ms against the pinned 20 000, the plane clock frozen 30 s at a live handover, a 60 s jump at rematch/resync, a peer dropped at the first lobby tick (`timeout_ms: 5000 vs 30290`), the P21 leave ladder 0 resends, and any lobby round past 600 s of session failing at once; the gates cannot see it (the 1.5 s stagger keeps the inflation under the 5 s timeout) and `admission_clock_ms` reports the right clock while the deadlines run on `NetSession::m_NowMs`, which no report carries; two of A7.A's three motivating claims do not hold (the in-wait pump was already wall-paced; a pause does not stop the pump), the real defect (two pumps a tick, P2 in ~11 s) stands; B is incomplete against P14 (a `Handshake` peer heartbeating every 4 s holds the bound; `connectedAtMs` is stamped and read nowhere); the reviewer's two-line fix (round-relative `nowMs` for the lobby tick and budget, absolute only to `TickAdmissionPlane`) measured P2 at exactly 20 000. The H4 lane is resumed with `followup-4.md` to land the fix, the connection-age expiry, the clock in the report, the corrected claims and two stale plan anchors; the reviewer re-checks after. SOURCE39 BREADTH (`lanes/source33-lanes/report.md` `## Source39`, third start under `D:\mx\s39lanes3`, the two earlier attempts voided): gates 1-4 and 6 green and identical to Source38 (fake-lag fl100 auto-delay 7 instead of 8 from a 200 vs 201 ms RTT reading at a frame boundary, not a code change); all eight sound arms agree with the chain; the heal gate passes; the compatibility ladder is zero-difference; A6's three `clean_leave` checks flipped green; all three lobby controls 3/3 including `ctl_rejoin_4_100` and the new 3-peer drop control, and `drop_4_200` now runs a match with its replacement admitted; `largest_relay_packet_bytes` ~247 B (191 KB relayed in total), `unresolved_observation_packets` 0 everywhere. TWO NEW HOST-FATAL REGRESSIONS (`0xE06D7363`, exit 3): (1) a re-entrant `std::mutex` - `PumpSessionEvents` holds the coordinator's mutex and A6's `NetLockstep.cpp:2178` (`50c3b3309`) calls `IsHoldingSeatForReclaim()` -> `QuerySeatState` -> the same mutex on the same thread; deterministic on the no-survivor drop path, kills three H4 hosts and takes `reclaim_socket` and `rejoin_after_resync` BACKWARDS from Source38 (`peers_3_4_regression` 5/5 because its drop arm always has a survivor); (2) `a GUI entity reference has no persistent owner` thrown from `GUICheckpoint::SaveEntityReference` (`GUIControlManager.cpp:179`) via `InventoryMenuGUI::SaveCheckpoint` during the resync snapshot save, killing the two B1 gates that resync (the two that never resync are clean; `substitute_bounds` has an independent failure). EResult 25 is NOT gone: the host loses all three remotes at frame 151 with `pending reliable 8388583` of an 8 MB budget, `rate 2097152 B/s` (= SendRateMin), `unacked 799228`, `queue 3999ms`, ping 400 - the lobby lane's section-7 pacing row, not payload; `stale_round_packets` 0 on the heal gate (no round restarted, so the F2.5 guidance holds). Lanes briefed for the three: `h4-a6-fix` (worktree `D:\Projects\h4-a6-fix`, branch `stage2/h4-a6-mutex` from `1d567e365`), `h4-b1-substitution` follow-up 1 (fresh worktree `D:\Projects\h4-b1-fix`, branch `stage2/h4-b1-resync-fix`), `lobby-grace` follow-up 2 (fresh worktree `D:\Projects\lobby-pacing`, branch `stage2/lobby-pacing`); they launch after the cleanup. CLEANUP APPROVED by the user: preservation done into `C:\preserve-20260909` (11 patches of the dirty worktrees with an index and checksums, `orphans.bundle` verified with `refs/keep/stage2-p2-27db40d` and `refs/keep/archive-loads-review-base`, 72 unique executables 4 GB, `deletion-log.json`); the deletion of exactly the manifest's paths ran with the junction-safe deleter and `git worktree remove --force` for 35 registered worktrees: 529 paths deleted, 22 handled by git, D: 185.7 GB and C: 66.3 GB freed (D: 284 GB free, C: 228 GB free); the deliberately access-denied selftest fixtures (`cccp-path-case-*/denied/hidden.txt`) and two `native-source38-1` directories were left behind. TWO MISTAKES, both recorded: (a) the approved tree's `Data` (8,516 files) and `external` (7,513 files) were emptied during the run - the deleter never entered a junction, but one of the 35 removed worktrees carried junctions into the approved tree and `git worktree remove --force` followed them (the lane reports of archive-loads, archive-loads-review, matrix-harness and others describe junctioning `Data` and `external` from the approved tree to avoid full checkouts); the lead restored both from git at once (`git checkout -- Data external`: 8,516/8,516 and 7,513/7,513 on disk, `git status` clean, the executable and every pinned Source39 artefact hash unchanged, the eight runtime DLLs present, `D:\mx\s38` intact at 6.2 million files; the two vendor `.lib` build outputs are now the committed versions, regenerated by the next build); rule added below: before any worktree removal, list and unlink the reparse points inside it; (b) the manifest's `contract-audit` rule ('every directory except the pinned builds, the Mac exports and the Source38 transaction archive') also deleted the Source38 and Source39 fast-gate run directories (`selftests-source3N`, `sound-query-*-source3N-*`, `sound-volume-authority-*`, `native-source38-1`, `restore-source38-1`, `fuzz-*-source38-1`); their verdicts survive in `chain-source38-1.json`, `chain-source38-remaining.json` and `chain-source39-1.json`, the Source38 executable survives in `grouped-build-f29f69bc`, and the Source39 fast gates are re-run on the approved executable with the family so the raw evidence exists again. Preservation archive: `C:\preserve-20260909` (5.1 GB: `index.json`, 11 patches, `orphans.bundle` verified, 72 unique executables, `deletion-log.json`, `deletion-stdout.log`). A7 CORRECTIONS DONE (`## Slice A7, corrections`): four commits on top of `a3ba11aad`, tip `30b794dc9e` - `390bbf43ba` (`NetMatchRunner::ResolveRoundClocks`: the lobby round and its budget round-relative, only the plane session-elapsed), `be4fd80a34` (a `Handshake` peer expires on `connectedAtMs`), `04629125d1` (`session.clock_ms` in the report beside `admission_clock_ms`; A7.D and A7.A corrected), `30b794dc9e` (both cases measured through the real composition); both headline measurements reproduced on control builds first; tip builds `141ab9dd`/`d6dbaf2d`: eleven selftests, native 542/0, SimBaseline unchanged, scratch 2.1 MB; the reviewer's re-check (`lanes/h4-a7-review/report.md` `## Follow-up 1`, seven builds of its own) calls `30b794dc9e` READY: inflation at the first lobby tick 0 (control 30 030 ms), the seated peer Ready, the plane clock continuous across handover, rematch/resync step 0, a 700 s service clock survives, the composed P2 exactly 20 000 ms (control 50 030), P21's ladder back at 7 resends, P14 on connection age (eight heartbeating handshakes expire at 5 010 ms, a hello at 4 000 accepted, at 6 000 rejected, the seated holder untouched), `session.clock_ms` correct; battery green, no trailers. Three non-blocking notes for a later H4 follow-up: the prescribed gate assertion `session.clock_ms - admission_clock_ms == 0` cannot pass as written because the e2e report is built after the loop stops feeding the session (sample mid-match or report the maximum difference at pump time); a >5 s no-feed phase with a silent peer still drops it through the next round's first iterations (re-stamp on a clock step larger than `timeoutMs`; align `BeginLeave(GetClockMs())` with `Tick(AdmissionNowMs())` at `:399`/`:411`); two commits carry each other's described contents. MERGED into the milestone branch by the lead (`stage2/h4-phase-a` at `30b794dc9e`; the lead's merge commit follows `1d567e365`). `lobby-grace` follow-up 2 launched in the fresh worktree `D:\Projects\lobby-pacing` (branch `stage2/lobby-pacing` from the merged tip). DECISION: Source39's remaining gates are NOT run - the working tree moved past the pinned `f15fcba3` when A7 merged, so their `source_unchanged` checks would read false, and Source39 was never going to be the final combined verification once its two regressions appeared; the next family (Source40 = Source39 + A7 + the A6 mutex fix + the B1 snapshot fix + the lobby pacing fix + round-start, each reviewed) runs everything through the new generalized driver `contract-audit/run_family.py --source 40 --breadth-prompt <followup-6.md>` (preconditions including no worker lane alive, chain with the full fast-gate list which restores the raw fast-gate evidence, Mac chain, remaining gates, breadth lane, matrix from `D:\mx\s40`; `--from remaining|breadth|matrix` to resume). The section B-1 checklist boxes were reconciled (3a, 5a, 6a done; 5c, 6b, 6c partial with their exact state). `h4-a6-fix` DONE (`lanes/h4-a6-fix/report.md`): not a mutex bug but a lock-order inversion - the coordinator holds a callback back into the service (`m_SeatStateSource` -> `NetMatchService::QuerySeatState`) and the service calls into the coordinator with `m_Mutex` held on the live path `ScenarioRunner.cpp:1131` -> `PumpSessionEvents` -> `InjectEvent` -> `NotifyDisconnect` -> `RecordDrop` -> the census -> `NetLockstep.cpp:2178` -> `QuerySeatState`; `68faade562` resolves which left seats are held once per `Tick` and in `ApplyPeerLeave` into `m_LeftSeatsHeld` so `AnyLeftSeatHeld()` is a plain read (ownership resolution now tick-stable), `284e20e7c3` pins it with `TestSeatStateNeverReadUnderTheServiceLock` driving the production wait loop (control `57589ff2` fails with `resource deadlock would occur`, tip `6ffdd590` passes while asserting the held seat's units still fall to the relay host); eleven selftests on GNS, no-GNS and the final binary, native 542/0, SimBaseline 0/600 differing. Second confirmed defect left for a follow-up: `RecordDrop` filters the census for the leaver's id, which the leave fallback guarantees the census never reports, so the ledger is always empty and `IssueReseat` returns early - why `host_reseat_issued` has been red since Source36; the lane is resumed with `followup-1.md` to fix it, one review covers both. The lane recorded a rule breach (`rm -rf` on a run directory holding junctions); the lead re-verified the approved tree intact. `h4-b1-substitution` follow-up 1 DONE (`## Follow-up 1`, worktree `D:\Projects\h4-b1-fix`, branch `stage2/h4-b1-resync-fix`, tip `8875f81843`, three commits): the throw is the first `SaveEntityReference` at `InventoryMenuGUI.cpp:1425` on `m_InventoryActor`, an actor already destroyed (UniqueID 0) whose non-owning menu pointer is refreshed only on the next `Update`, while the resync save runs from `HandleControllerReplayFailure` (`Main.cpp:1983`) inside that window - not new in Source39 (those files are byte-identical between `de87ff198` and `1d567e365`; the four gates simply never ran on Source38); `f630469c50` saves a pointer MovableMan no longer knows as absent (`GUICheckpoint::LiveObject`), a genuinely orphaned reference is still refused, `08a72143e1` names the class and preset in the refusal, seven checks red on the base (225/7) and green on the tip (549/0); `8875f81843` fixes `substitute_bounds`' own harness (the drop landed at tick 977 of 1200, joiners timed out; now 3600 ticks and joiners started on the host's own drop line, with `host_adjudicated_the_drop`). NEW FINDING, not the lane's: the audio checkpoint flakes nondeterministically on this machine - the same binary alternates native 549/0 and 252/6, restoration failures move run to run, every failing log says `invalid AudioMan checkpoint`, and UNMODIFIED main flakes identically (1 native flake in 6; restoration 6/13 on one run); lane `audio-checkpoint-flake` (worktree `D:\Projects\audio-flake`, branch `stage2/audio-checkpoint-flake` from `1b5d1e92fa`, runs `D:\mx\acf`) reproduces it, names the refusing validation, and fixes the writer's consistency (the likely race: a playing voice whose sample is still loading is captured while the sample is skipped). `h4-a6-fix` follow-up 1 DONE (`## Follow-up 1`, tip `7393aac281`): the ledger asked who held the units while the census answered who plays them now - `RecordDrop` (`NetReconnectSession.cpp:591`) filtered for the leaver's id but `ResolveActorOwner`'s fallback (`NetLockstep.cpp:2176-2181`) never names the leaver, so `IssueReseat` (`:638`) returned on an empty record on every family since Source36 (not an A6 regression); `14b79d4592` splits `ResolveActorOwnerBeforeLeaves` out of `ResolveActorOwner`, reached through `GetLockstepDropTimeActorOwner` (control handoff first, then the policy with leaves not applied) and used only by the census the ledger reads, live ownership unchanged; `7393aac281` pins it with a real three-peer round in survivor and no-survivor arms (control fails in the survivor arm); eleven selftests on GNS, no-GNS and the final binary `220d57dc`, native 542/0, SimBaseline 0/600 differing, scratch 75 MB; gate expectations: `host_reseat_issued` and B1's `reseat_issued` flip, `host_saw_seat_drop_and_return` moves on the mutex fix, `peers_3_4_regression` stays 5/5 with `drop3`'s tick hashes legitimately changed, and the co-op hand-back has no gate yet (needs a `-net-match-mode coop` arm). One review lane, `h4-fixes-review` (runs `D:\mx\h4fr`), covers both `stage2/h4-a6-mutex` at `7393aac281` and `stage2/h4-b1-resync-fix` at `8875f81843`. `lobby-grace` follow-up 2 (worktree `D:\Projects\lobby-pacing`) measured the pacing cause and landed two commits so far, `11bf641396` (the transport tells a refused send that is a full queue, `k_EResultLimitExceeded`, apart from a lost peer) and `542d891cb6` (the relay holds a peer behind our own full queue instead of taking its seat, and no longer judges it silent for a send we never made), with a third change in progress (the match runs at the library's 256 KB/s default rate with the rate raised for the state stream alone): at the refusal the host had handed each connection ~110-120 KB while GNS reported 799 KB unacked and 8.39 MB pending, and `m_cbPendingReliable` counts data scheduled for retransmission; `rate 2097152 B/s` is exactly our `SendRateMin`, which in this library is a MANUAL rate with no estimator behind it (`steamnetworkingtypes.h:1233-1239`, default 256 K), so a 4 s ten-RTT retransmission queue built at a rate nothing backs off (`queue 3999ms` = pending / rate); the in-process four-peer 200 ms control passes and goes red with the production shape on a build with only the decision reverted; eleven selftests and SimBaseline identical so far; the resumed lane finished (`lanes/lobby-grace/report.md` `## Follow-up 2`, tip `30863f9686`): the third commit `30863f9686` runs the match at the library's 256 KB/s default with the rate raised for the lobby's state stream alone; each commit built and the eleven suites run on its own tree seven times in both configurations (GNS `bf97051d`, no-GNS `b13dc1f9`), the four-peer 200 ms in-process control keeps every seat and resumes the round (1,329 congested refusals, three holds) and goes red with the exact production shape when only the decision is reverted, SimBaseline 600/600 identical; the lobby lanes with their counters (F2.8) settle it on the next family. The branch awaits its independent review (`lobby-pacing-review`, queued for the next slot). `round-start` resumed (`followup-1.md`: merge the milestone branch first, then the two start defects). The Mac builds the three fix tips from exact exports (`combined-source-h4af`, `b1f`, `lgp`; `chain_mac_fixes3.zsh`, selftests follow). THIRD MEMORY KILL (~07:30 UTC, clock approximate): `h4-fixes-review`, `audio-checkpoint-flake` and `round-start` were all doing full builds of fresh or freshly merged worktrees and the harness killed all three; the cap is now TWO building lanes (instructions and memory updated); they resume in that order from their sessions. `h4-fixes-review` DONE (`lanes/h4-fixes-review/report.md`, 540 lines, seven builds of its own): BOTH BRANCHES READY - the three negative controls reproduce red on one control binary (the mutex check's `resource deadlock would occur`, the ledger check failing in the survivor arm, the production throw `a GUI entity reference has no persistent owner: Actor "None"`) and green on the tips; eleven selftests on all six good builds, native 542/0 and 549/0, SimBaseline identical for every tip, restoration 13/13 on the base and both tips (the audio flake did not hit there); after the mutex fix the seat-state source is reached only from `Tick` and `ApplyPeerLeave`, `m_LeftSeatsHeld` is one tick stale but host-only and exact within a tick; the B1 fix keeps every live object on the unchanged path and widens no recycled-address weakness. Five non-blocking findings for later: an off-tick second drop can overwrite a good ledger record with an empty one (pre-existing, reachable, needs a socket gate), the mutex check pins the resolver rather than the census path, the co-op hand-back has no socket gate, the host/client asymmetry inside a held-seat window is unmeasured, `inventory_live_reference_unchanged` is tautological. MERGED by the lead into the milestone branch (`stage2/h4-a6-mutex` `7393aac281`, `stage2/h4-b1-resync-fix` `8875f81843`; no trailers; main `9b599ce359`). FOURTH MEMORY KILL with only two lanes (`audio-checkpoint-flake`, `round-start`): the real cause is 32 logical CPUs times `MultiProcessorCompilation`, one uncapped full build alone ~32 GB; fixed at the source - `launch_worker.py` now launches every worker with `CL=/MP6` and `build_audit.ps1` sets `/MP12`; both lanes resumed - and a FIFTH kill followed, because the env `CL` options are read before the project's own `/MP` (from `MultiProcessorCompilation`) which overrides them; at rest the machine has 34 GB free and no hog. The variable the CL task honours in that case is `CL_MPCount`: the launcher now sets `CL_MPCount=6` (chain `12`), one lane ran alone (`audio-checkpoint-flake`) with a free-memory monitor: no low-memory event during its build, so the `CL_MPCount` cap holds and the second lane was added. AUDIO FLAKE ROOT CAUSE (`lanes/audio-checkpoint-flake/report.md`, tip `96ccf1ef0e`): NOT the FMOD async-sample race (instrumented, never fired) but an uninitialised member - `AudioMan::m_MuteAudioOnFocusLoss` is never set by `Clear()` and `SettingsMan.cpp:141` assigns it only when `Settings.ini` contains the property, so on a boot whose settings omit it the checkpoint writer serialises a raw heap byte (observed 100, 117, 140, 141, 108) and the reader correctly refuses an out-of-range bool as `invalid AudioMan checkpoint`; an upstream defect (Causeless's `200a289ad4`) harmless until the checkpoint work serialised the value; `7a9e735e76` names the refusing check and value in the diagnostic, `55c5f9d908` adds two checks, `96ccf1ef0e` initialises the member and the five FMOD handles with the same hole; base 4 of 6 runs red, tip 6 of 6 green (544/0), restoration 13/13 three times, ten selftests; the lane finished (tip `96ccf1ef0e`; restoration 13/13 five times, native 544/0 six times, ten selftests, fuzzes 17/17 and 10/10, SimBaseline identical to an unmodified-main control; its first 23 clean runs were on a worktree with a fully populated `Settings.ini`, which assigns the member and hides the bug - which is also why the approved path's earlier batteries passed). The lane flagged that lane reports carried a `Worker: <model>` header line; the lead stripped model names from every lane report and the no-attribution rule now names report headers. Its review rides with the pacing review (`lobby-pacing-review`, launched, two verdicts). THE REAL MEMORY-KILL CAUSE (kills 3, 4 and 5, and the monitor's low-memory events): a single `Cortex Command.exe -headless -net-lockstep-selftest` from the `round-start` worktree (`D:\mx\rs\control3\net-lockstep`) grew to 26 GB with zero compilers running, until the runner's 200 s timeout ended it (exit 124); the kills coincided with that lane being active. Fix at the harness: main commit `2447a23164` makes `win32_test_runner.py` cap every hidden launch's job at 8 GB of committed memory (`JOB_OBJECT_LIMIT_JOB_MEMORY`, `CC_RUNNER_JOB_MEMORY_GB` overrides, recorded as `job_memory_limit_bytes`; a normal selftest launch still passes), so a runaway fails its own allocation instead of the machine; the compiler caps stay (`CL_MPCount`). The round-start lane was stopped and restarted with `followup-2.md` and FINISHED (`lanes/round-start/report.md` `## Follow-up 1`, tip `bbe374e660`, four commits on the merged milestone base `2447a23164`, fast-forward): the bomb was the lane's own four-peer fixture on the PRE-EXISTING start-answer storm - every start a Running peer receives is answered with another start (`NetLockstep.cpp:2749`), doubling per 5 ms tick with three remotes, 26 GB in 200 s; `f3755f5bde` paces the answer to once per retransmit interval, `075ece8b1f` answers a repeated start with every start the round has targeted at the asking peer, `ee8efda497` never answers a peer that has already played the round, `bbe374e660` follows the host onto its new round while ours has committed nothing (`ReadoptRound` resets the sound-wire per-round tables and re-sends our production under the new tag); no wire change, `RelayToOtherRemotes` untouched; every fixture now stops on a 200-start budget; control `592e1ef8` red on all four cases, tip `53ea78e4` green; eleven selftests both configurations, native 549/0, restoration 13/13, fuzzes, SimBaseline and both replays 0 of 2100 ticks differing. Its independent review runs as lane `round-start-review` (runs `D:\mx\rsr`). DESKTOP INCIDENT RESOLVED (mechanism identified, fix landed, 2026-09-09): the game reached the desktop through the loose PowerShell harnesses in `D:\Projects\stage2_p4` (`run_interp_e2e.ps1`'s `Start-CortexCommand` and its two replay invocations, and `Start-Peer` in the desync scripts), which created the engine process themselves with `System.Diagnostics.Process` on the interactive desktop. The e2e arguments force `CCCP_HEADLESS`, so the window is created hidden (`WindowMan.cpp:192`), but a real window and GL context are still made and `SDL_RaiseWindow` (`WindowMan.cpp:284`) takes the foreground, which minimises a fullscreen exclusive game. The breadth lane's gates 1-4 (semantic battery, interp matrix, fake-lag duel, `rtdesync`) run through those harnesses; the third Source39 attempt ran the battery 20:26-20:31 and the matrix 20:32-20:38 on 2026-09-08, the window in which the user saw the game open; every other launcher goes through `run_sim_test.make_run` and was already on the private desktop, which is why the hidden-launch watcher probes saw nothing. Fix: main `60359be83e` adds `tools/isolated_launch.py` (one engine launch through `IsolatedRun`: private desktop, `SW_HIDE`, job kill-on-close, the fullscreen hold, the 8 GB cap; the engine's output echoed on the wrapper's stdout, its exit code passed through `ExitProcess`; `--hold` waits before a harness's own deadline starts), and all six harness copies launch through it now (`stage2_p4\run_interp_e2e.ps1`, `run_interp_desync.ps1`, `run_runtime_desync_test.ps1`, `lanes\semantic-battery\driver\run_interp_e2e.ps1`, `lanes\breadth-debt\driver\run_interp_e2e.ps1`, `lanes\source33-lanes\driver\run_interp_desync.ps1`; the loose scripts are not under git, so the patch is recorded here and the patcher is in the session scratchpad). Verified: the jetpack battery case through the patched battery driver on the approved executable `f15fcba3` gives a `jetpack.check.txt` byte-identical to the Source39 run (`D:\mx\dw\sem`), all four launches (host, client, replay, verify) record `input_desktop_before/after: Default` on private desktops, and the desktop watcher over the whole run recorded zero new windows and zero foreground changes over the 240 s covering all four launches (`D:\mx\dw\watch-jetpack.json`). Residual: `stage2_p4\test_test_trust.ps1`, `run_p4c_3peer_e2e.ps1`, `run_p4a_menu_service_e2e.ps1`, `loop_desync_probe.ps1`, `test_legacy_replays.ps1`, `run_interp_evt_battery.ps1`, `chain12.ps1`, the two `run_p*_windows_gns_gate.ps1`, `run_p4c_menu_3p.ps1`, `recovery_e2e.py` and `recovery_lobby_hang.py` still launch directly; none is part of a current family, and none may run unattended until routed through the shim. PACING/AUDIO REVIEW RETURNED (`lanes/lobby-pacing-review/report.md`, own builds): `stage2/audio-checkpoint-flake` `96ccf1ef0e` READY (the member and the five FMOD handles have no other initialisation path, the reader's range check as strict, a sweep of all 40 settings-fed members finds exactly one unset at the base and none at the tip, 3-of-6 red on the guard commit against 6-of-6 green on the fix) and MERGED into main as `56b73a9c08` (after the shim commit `60359be83e`). `stage2/lobby-pacing` `30863f9686` NOT READY: `11bf641396` (congestion told from a fault) and `30863f9686` (256 KB/s with the rate raised for the state stream) stand, but `542d891cb6`'s hold has no bound - four peers at 200 ms with one link metered to never drain now ends the round for everyone (`peers_left=0 failed=1 reason=MissingFrameTimeout`, 5 frames) where the old policy shed that peer and played on to frame 35, and after the overflow branch dropped 8991 forwards on a reliable in-order lane a fully healed link cannot rescue the round (`healed_from=5 frames_after_heal=5`); the 256 KB/s rate makes an 8 MB drain 32 s against the 20 s missing-frame grace; the bulk rate is lowered when the last chunk is handed to the socket rather than when it left, `m_BulkTransfer` survives `Stop()`, no behavioural coverage; the GNS rate claim is confirmed (`SNP_ClampSendRate` pins `nMin == nMax`, nothing lowers the estimate; 256 KB/s is ~11x headroom for the match) and nothing reaches the simulation (SimBaseline `94b13fc1` on base, lobby tip and audio tip). The lobby lane is resumed with `lanes/lobby-grace/followup-3.md` (bound the hold below the grace and take the seat; a reliable-lane overflow fatal to that seat, never the round; the three `30863f9686` notes; the reviewer's probe as red/green selftests; merge main first). ROUND-START REVIEW RETURNED (`lanes/round-start-review/report.md`, own builds): the storm is measured GONE in production, not only bounded by the fixture (four peers, one stray repeat: base 4372 starts in 30 ms, tip 2 starts in 1005 ms; at N>2 a client's `m_RemoteTransports` holds only the host, so `AnswerRepeatedStart` returns for every relayed repeat); the answer target and the 250 ms pacing are right (the ladder's own period, no starvation), the A-series stale rule is not reopened, no forged or stale start can move a client's round in the star, and 'already played' does not misjudge a rejoiner (the live rejoin is a host-side resync). `f3755f5bde`, `075ece8b1f`, `ee8efda497` READY and MERGED into main as `fff10dea85` (the reviewer's suggested split). `bbe374e660` (`ReadoptRound`) NOT READY, three failures reproduced: F1 it keeps `m_PeerLeaveFrames`/`m_LeftSeatsHeld` so the follower alone stops requiring a peer's frames (`A accepted=1 host accepted=0`, a divergent commit set in the round the rejoin resync creates; the lane's 'the host re-announces them' is wrong, the host reached the round through `Start()` which cleared its own map); F3 it keeps the deferred stops (`m_PendingRecoveryStop`, `m_PendingCompleteStop`, `m_LastCompletedSimulationTick`, `m_DeferStops`) so a held `Complete` stops the followed round at its first tick and a client goes deaf to later resync requests; F2 following the authority skips the stale-round rule that also shielded the field validation, bounded only by `startFrame` (constant across rounds), so a start disagreeing on any other field fails the round with `ProtocolError:lockstep start mismatch` where the base counted it stale (latent, green on control, red on tip); F4 the refused re-send lacks `QueueLocalInput`'s discipline, wrong `observationsCarried` delta, uncounted. Battery reproduced (11/11 both configurations, native 549/0, SimBaseline 600/600 `94b13fc1`). The round-start lane is resumed with `lanes/round-start/followup-3.md` (one shared reset-everything-a-round-owns helper for `Start`, `StartReplay` and `ReadoptRound`; validate before adopting and count stale; F4; the reviewer's three cases as red/green selftests; the F1.4 addition for the lead's drivers; merge main first). SOURCE40 COMPOSITION DECIDED: main after the audio fix, the launcher shim and the three round-start commits (A6, A7, B1, harness C/D, fullscreen hold, job cap already in); NOT in it: the pacing branch and `ReadoptRound`, both back in their lanes; the family starts when no lane is alive (the lobby and round-start lanes are the two running), and the H4-residual and set-aside-residual lanes launch during its breadth/matrix steps. ROUND-START FOLLOW-UP 3 DONE (`lanes/round-start/report.md` `## Follow-up 3`; tip `d678bc72ef`, three commits on `bbe374e660` with main `fff10dea85` merged in as `f91b616e07`): `d77726f2e0` one `ResetRoundState()` shared by `Start`/`StartReplay`/`ReadoptRound` (fourteen fields were carried out of the left round; two stated exceptions: the follower's re-sent production and the `m_DeferStops` mode armed only after `Start()`), `40764e3863` `StartMatchesConfig` checked before adopting (a disagreeing followed start is stale and logged), `d678bc72ef` ordered retry of the re-send with readings riding the next queued frame instead of a re-encode (stated deviation) and honest `observations_carried`/`frame_packets_sent`/new `start_answers`; control red 4 of 5 on the reviewer's cases, tip 11/11 both configurations, native 551/0, SimBaseline and both replays identical to main; trailer-clean, merges clean. Its re-review (`lanes/round-start-review/report.md` `## Follow-up 1`) says READY as `d678bc72ef`: all three findings fixed on the reviewer's builds, the lane's copies of the reviewer's fixtures byte-identical or strengthened; the `m_DeferStops` exception right in both directions (`ConsumeReadyToLaunch` `NetMatchService.cpp:687` is the only arm site and both new-round paths destroy the coordinator, so a follower keeps the mode; a new case pins that a follower which never deferred applies at once; the rematch sets `startFrame` to 1 and the resync to `simUpdateCount+1`, correcting the reviewer's own note); a mechanical field audit (`D:\mx\rsr\field_audit.py`) finds ten fields outside `ResetRoundState()`, all config/topology or the two stated groups, `m_PeerEffectiveStart` derived from a config the follow requires to match; `StartMatchesConfig` carries all nine fields checked before adoption with no partial state; the readings land together one frame later with honest counters. Three non-blocking notes for a later round-start follow-up: `FlushResendFrames` silently gives up on an owed frame the follower already committed locally (`NetLockstep.cpp:1793-1796`, reachable on GNS where a start fits the send buffer and a frame does not), the ladder reads `start_retransmits - start_answers - round_readoptions` (the runbook's two-term form is wrong), and the flush keeps retrying after a `Fail`. Battery 11/11 both configurations, native 552/0, SimBaseline 600/600 against `2447a23164`. MERGED into main as `269d2ee718`; SOURCE40 NOW INCLUDES the whole round-start repair (`followup-6.md` restored to the `round_readoptions` expectations plus the reviewer's `peer_leave_frames`-empty check). LOBBY FOLLOW-UP 3 STOPPED SHORT (01:17): the lane ended its session on a status message with the battery running and NOTHING COMMITTED (eight source files, +497/-25, uncommitted in `D:\Projects\lobby-pacing`; the branch holds only its merge of main `54b9dbb20b`; no engine process left behind); its fixtures pass on its tree (`dead_link_loses_only_its_own_seat` peers_left=1 with `relay_backlog_overflows=63` on the dead peer alone, `dead_link_healed_in_time_keeps_every_seat` peers_left=0, the lane's own `congested_relay_holds_every_peer` unchanged, new `bulk_rate_follows_the_socket` raised_at=130ms dropped_at=210ms). TWO NEW PRE-EXISTING DEFECTS it measured: (a) a HEALTHY four-peer in-process round exhausts the 8 GB job cap by frame 70 (`bad allocation` at 6050 ms simulated; no congestion, no refusals, no leaves) - the growth behind the 26 GB runaway is therefore not only the start storm, and its owner (loopback queues, relay backlog, observation tables, ready-frame store, or the fixture) is unattributed; (b) a round that sheds a peer stops ~30 frames later with `MissingFrameTimeout` (survivors produced 0-39, the host received only 0-34 from each), same on the reviewer's reverted-policy probe, so it predates the branch; the two-process `drop_4_200`/`ctl_drop_3_200` lanes finish 180/180 after a scripted drop, so whether it is loopback-only is open. The branch now CONFLICTS with main in `NetLockstep.cpp` and `NetLockstepSelfTest.cpp` (the round-start repair landed after its merge). Relaunched with `lanes/lobby-grace/followup-3b.md`: commit per concern (never the `.lib` artifacts), merge main `269d2ee718` keeping both behaviours, battery on the merged tip, attribute the two defects with numbers, append the section; 'do not stop at a status update'. SET-ASIDE FOLLOW-UP 4 DONE (02:17; `lanes/world-setaside-audit/report.md` `## Follow-up 4`; branch `stage2/setaside-residuals`, fresh checkout `D:\Projects\setaside-residuals`, tip `9d35f97f94`, three commits on `269d2ee718`, trailer-clean, merges clean): `c483b9f47d` both harness captures settle first through named captures (`Main.cpp:1401-1423`; red on the control with the review's numbers, green on the tip); `9d103c3aa3` the latched hold was WORSE than a latch - an abandoned refused hold left `m_HeldRegistries` and each state's held lists pointing into a dead record and the control build crashes at shutdown in `~MovableObject -> ForgetDestroyedObject`; the hold now belongs to its record (`m_WorldSetAside`, `~WorldSetAside` gives it back, `Clear()` drops it), pre-check untouched, `next_hold` 0 -> 1; `9d35f97f94` the sweep's lifetime half cannot be driven in-process (structural: the stash strongly holds every walked value until reinstate) and is pinned. Battery both configurations (native 413/0 on this tree - the reviewer is asked whether that is the whole suite), eleven selftests, restoration 13/13, fuzzes byte-identical, unchanged-Lua fixture findable, SimBaseline identical; matrix jobs at the two sites keep every verdict, and the audit's raw-difference count moves (`hold_5` `differences=0` against Source38's 3,665, byte-identical `after`) so two matrix baselines need re-recording when this merges. Re-review brief written (`lanes/setaside-review/followup-3.md`, session `a874a2c9-b293-445c-bd5a-4f1cf41d1875`), launched after the Source40 chain build. LOBBY FOLLOW-UP 3 DONE (03:19; `lanes/lobby-grace/report.md` `## Follow-up 3` from line 1022; tip `17f5cea4b4`, five commits: `62658501d2` the hold bounded at `timeoutMs * 3 / 4` (past the `timeoutMs/2` silence bound, a quarter of the grace short of the missing-frame timeout; `relay_longest_congestion_hold_ms` and per-peer hold/overflow/last-refusal in the report; three adjacent defects fixed with it), `53b9832df0` a dropped forward ends that peer's seat not the round, `7ee4891b7c` the bulk rate given back only when every connection reports nothing pending and nothing unacked and `Stop()` clears the latch, `fc1a7afd3e` the fixtures (dead link loses only its seat at 3000 ms; healed in time keeps every seat; healthy control to frame 120), `17f5cea4b4` the merge of the round re-adoption; battery 11/11 both configurations, native 4/4 x 550, SimBaseline `94b13fc1`; trailer-clean, merges clean). ATTRIBUTIONS: (a) the healthy four-peer 8 GB `bad allocation` was the START-ANSWER STORM (9210 retransmits allocating packets) and is GONE on the merged tip (every container flat, the fixture's own undrained ready-frame store 0.4 KB/frame) - closed by main's round-start discipline, so the 26 GB runaway and this were one mechanism; (b) the POST-LEAVE STALL SURVIVES: after a mid-round leave the round advances ~15 frames and freezes with the clients having produced frames the host never receives and nothing queued/in flight/dropped (a send-side stop); the two-process lanes cannot see it (`ctl_drop_3_200` drops in the lobby, `drop_4_200` sheds at frame 151 of 180) - owner: a new follow-up (`lanes/lobby-grace/followup-4.md`, written: root cause in process, fix, and a `--leave-at-frame` knob for `test_lobby_lifecycle.py` so a sixth lobby lane sheds a peer at frame 40). SOURCE40 FAMILY STARTED 03:20 local (`run_family.py --source 40 --breadth-prompt lanes\source33-lanes\followup-6.md`; preconditions passed at `269d2ee718`; the incremental chain build finished within ten minutes, exported and pinned as `grouped-build-ec9db6d0`; log `contract-audit\family-source40.log`, summary `family-source40.json`); two watchers wake the lead at the `remaining` and `breadth_lane` steps. QUEUE for the two slots: after `remaining` the pacing re-review (`lanes/lobby-pacing-review/followup-1.md`, session `bc2134e4-8a4a-42c0-a550-a5949516d7ae`) and the set-aside re-review; after the reviews, as slots free and nothing further until a review returns; the H4 lane's next follow-up carries the A7 reviewer's three notes and followup-5's open items 6 and 7. SOURCE40 FAST GATES (03:20-03:29): chain green on `grouped-build-ec9db6d0` (exe `ec9db6d0dcf1...`; build, export, preserve; the eleven selftests pass; sound authority match and fault-diverge (72 changed ticks); `wounds`, `rng_particles`, `ui_bus`, `ai_defer`, `ai_defer_alias` match; `per_machine` diverges under the fault; no gate failures), Mac chain launched, remaining gates: native 551/0, heal PASS (`source_unchanged`), invariance PASS, restoration PASS, fuzz buy 17/17 and pickup 10/10, and ONE FAILURE that is not the engine: the `transaction` gate (`run_remaining_gates.py` step `transaction`, the `valid` job of `transaction-controls-source20.json`) crashed in `run_audit.py:210` copying its thirteen control snapshots from `contract-audit\full-runtime-transaction-controls-20`, which the APPROVED CLEANUP DELETED (it is in `C:\preserve-20260909\deletion-log.json` and not in the preservation index; the manifest's contract-audit rule took it with the run directories). No engine process ran for that gate; its verdict is 'not run', not red; the controls (`bad_closure`, `bad_header_first/last`, `bad_vm_rng`, `invalid_coroutine_pc`, `invalid_iterator`, `invalid_native_state`, `missing_native_preset/reference`, `truncated_graph`, `unknown_closure_factory`, `unknown_native_class`, `valid_full`) must be regenerated from a seed save through `graph_controls.py` / `prepare_transaction_control_commands.py` before the next family; the matrix's `continuation_*` and audit jobs cover the same continuation machinery meanwhile. The breadth lane (`followup-6.md`) is running on the approved path; the pacing re-review (`lobby-pacing-review` follow-up 1) and the set-aside re-review (`setaside-review` follow-up 3) were launched at 03:31 in the two slots. SOURCE40 MAC CHAIN (03:25-03:27, `chain-40.out` on the Mac): the ten selftests pass, authority match, `wounds`, `rng_particles`, `ui_bus` match (`changed_ticks` 0), and the NATIVE SUITE FAILS ONE CHECK, `native_runtime_checkpoint_values`, on the audio lane's `unset_setting_byte_is_refused_and_named`: the archive is refused on the Mac too (correct), but the refusal quotes one raw byte 0x94 where Windows quotes `'100'`. Mechanism pinned with a probe of the writer's exact shape compiled on the Mac (Apple clang 17, `scratchpad\bool_byte_probe.cpp`): reading a `bool` whose storage byte is 100 is undefined behaviour and clang exploits it three ways - `-O0` masks to the low bit and writes `0` (the archive would be ACCEPTED as false), `-O2` writes `100`, and the engine's inlined build takes `to_chars`'s single-digit fast path and writes the byte `'0' + 100` = 0x94; MSVC writes `100`. Not a functional regression on either platform, but the writer's bool path is compiler-dependent, so the fix is a `CheckpointWriter::Value(const bool&)` that memcpy's the storage byte and writes it as a number on every compiler, plus a text-level check; queued as `lanes/audio-checkpoint-flake/followup-1.md` (session `b8d41f45-8a4e-4d41-835b-b8a0ea0c4a52`, worktree `D:\Projects\audio-flake`), the Mac verified by the next family's chain. PACING RE-REVIEW RETURNED (03:49; `lanes/lobby-pacing-review/report.md` `## Follow-up 1`): NOT READY as `17f5cea4b4`; four of five commits land on the reviewer's builds (`62658501d2` the bound takes one seat at `longest_hold_ms=3000` on a 4000 ms grace, four hang-then-break windows run past frame 100, a bounded hold and an A6 held seat can never disagree; `53b9832df0` restores base behaviour with attribution and the dropped forward is unrecoverable on `ControlReliable`; `fc1a7afd3e`; the merge intact both ways, 27 lockstep verdicts, SimBaseline `94b13fc1`, 11/11 both configurations, native 4/4 x 546); the BLOCKER is `7ee4891b7c`: the bulk-rate return condition `pending + unacked == 0` is unsatisfiable while the round sends (`m_cbSentUnackedReliable` stays non-zero a full RTT after every reliable send, one packet per peer every 33 ms), measured `bulk_round_traffic_200ms still_bulk=1` after 5 s, so a resync/rejoin round would run its whole life at 32 MB/s, worse than before; fix by dropping the `unacked` term, a one-RTT hold, or per-connection lowering. The reviewer also RE-ATTRIBUTED THE POST-LEAVE FREEZE: not a send-side stop and not the fixture (the clients keep sending after the host stops accepting, last send 4635 ms vs last accept 4430 ms; the host fails 170 ms after its last accepted frame on a 4000 ms grace; the freeze never occurs when the seat is taken by the silence path) - start at the missing-frame clock on `DropUnreachablePeers -> ApplyPeerLeave -> AdvanceReadyFrames` (the grace not re-based on a hold-path leave). Both go to the lobby lane in `lanes/lobby-grace/followup-5.md` (supersedes followup-4: the bulk-rate return, the freeze's clock, the `--leave-at-frame` knob), launched in the freed slot. SET-ASIDE RE-REVIEW RETURNED (04:24; `lanes/setaside-review/report.md` `## Follow-up 3`): READY as `9d35f97f94` on the reviewer's builds (the rule table right at all eight `SerializeScriptGraphs` sites; the ownership change covers every pointer into a record and every abandonment path, the control's shutdown crash reproduced frame-for-frame at `MovableObject.cpp:139` and gone on the tip; battery both configurations, restoration 13/13, fuzzes, SimBaseline identical), with notes to carry: the sweep pin's claim is too wide (an object the capture never walked is not pinned by the stash, `recorded=1 swept=1`; the destruction-keyed forget is what covers it, `still_named=0`) so F4.3's wording narrows; the settle now lives inside the audit's observation, so the two cases whose operation captures nothing settle anyway (`preview_5` reads `registry=3104` twice where the control reads 3107) and the `nullable_turret` baseline re-records across seven of fourteen cases (3665 -> 0, 3666 -> 1, 3742 -> 77, 3965 -> 300, 3665 -> 0: missing fields of swept objects with zero value changes); '413' is the anchored count of a 550-line suite. MERGE DEFERRED, NEW RULE: nothing merges into `stage2/p4b-interp-lockstep` while a family runs on the approved tree, because the breadth's `binary_matches_source` and the matrix's `source_unchanged` verdicts compare the tree against the pinned executable; every reviewed tip (`stage2/setaside-residuals` `9d35f97f94` now; the pacing and bool-byte tips when reviewed) merges when the Source40 family finishes, and Source41 is their family. The audio lane is resumed on `followup-1.md` (the bool-byte writer) in the freed slot. LOBBY FOLLOW-UP 5 STOPPED SHORT AGAIN (04:29): the lane committed `316feb39fc` (the bulk rate given back a ping after the socket drains, not when the link falls quiet), started `finish_f5.py` (commits 2 and 3, builds, battery, the reviewer's probes, the GNS-disabled configuration) as a background job, ended its session on 'I'll write the report section when it lands', and the job died with the session (its build log ends at 'Generating code'; no process left); the freeze fix is uncommitted (+22 lines in `NetLockstep.*`/`NetLockstepSelfTest.cpp`). Relaunched with `lanes/lobby-grace/followup-5b.md`: finish inside the session, a started job must be awaited and read before the turn ends. Worker failure mode to remember: a lane that delegates to a background script and ends its turn loses the script. SOURCE40 BREADTH RETURNED (04:31; `lanes/source33-lanes/report.md` `## Source40` from line 1938, runs `D:\mx\s40lanes`): gates 1-4 and 6 green and identical to Source39 (battery 10/10 with all six lines identical, matrix 16/16, invariance 8/8 with its control, discovery PASS) and the ROUND-START REPAIR MEASURED ON THE WIRE (fl200 start chatter 39 sent / 38 retransmits -> 2 / 1, resyncs 0, auto delays 8 and 14); ALL FIVE LOBBY LANES 15/15 AND ERESULT 25 GONE WITHOUT THE PACING BRANCH (six 4-peer 200 ms runs: `relay_send_failures` and `relay_resends` 0, every peer 177 frames, longest client stall 332-347 ms against Source39's 22-34 refusals, seats lost at frame 151 and stalls near 6 s) - the storm was the filler, so the pacing branch's rate change (`30863f9686`, `7ee4891b7c`, `316feb39fc`) needs re-weighing by its reviewer while its classification and bounded hold keep their own value; both Source39 host-fatals FIXED and ZERO AbortLog in the whole family (Source39 had five); the audio flake absent; `reclaim_socket` regains `returner_reclaimed`, `returner_committed`, `resync_round_survived`, `host_census_clean 0`; B1 `substitute_bounds` and `substitute_host_cancel` PASS; all eight sound arms, the heal gate (`unresolved_observation_packets` 0, `round_readoptions` 0) and the compat ladder (28/28, zero differences vs Source22) unchanged and green. TWO REGRESSIONS vs Source39, both deterministic: R1 `peers_3_4_regression` 0/5 on `census_refusals=1` on `drop3_host` with the new `resync failed: timed out waiting for the resync round` (the snapshot saves, the resync round never forms); R2 `clean_leave`'s announced leave adjudicated as `connection lost` (Source39 `Match left`), client `Leaving (ticket kept)`, no LeaveAck, `clean_leave_acked` 1 -> 0. Still open: `host_reseat_issued` red on both reclaim drivers, the `activity ended in state Over` residue, and B1's `substitute_commit`/`substitute_returner_wins` hosts now end gracefully with `resync failed: resync snapshot save failed` (the crash became a named refusal; the save still fails there). The matrix step is running (`D:\mx\s40`). BRIEFS WRITTEN: `h4-a4-semantics/followup-6.md` (R1, R2, the reseat, then the residuals; in-process reproduction and a code-level bisect across the five merges; sockets only on the next family) and `h4-b1-substitution/followup-2.md` (the named refusal in the resync save). AUDIO FOLLOW-UP 1 STOPPED SHORT (04:37, launched before the launcher's finish epilogue): two clean commits on `stage2/audio-checkpoint-flake` (`d1c29203f0` the writer copies a bool's storage byte; `9029cb3d64` the archive-text pin), trailer-clean, merges clean; reported 10/10 selftests, fuzzes 17/17 and 10/10, SimBaseline `94b13fc1` (archive-neutral), native 553/0 x5 with the four audio checks green, and a sweep of 1,799 identifiers reaching an archive call: 31 bool members without a declaration initialiser, all assigned at construction, `m_MuteAudioOnFocusLoss` the only one that ever reached the archive unassigned; still to run when it ended: restoration x5, the GNS-disabled configuration, the SimBaseline control; no report section. Relaunched with `followup-1b.md`. LAUNCHER CHANGE: `lanes/launch_worker.py` now appends a fixed finish rule to every brief (a started job is awaited inside the session; the lane is finished only when the commits and the report section exist) - three stop-shorts in one night were each a lane ending its turn on a status message. LOBBY FOLLOW-UP 5 DONE (04:43; `lanes/lobby-grace/report.md` `## Follow-up 5`; tip `1410fec3ac`, three commits on `17f5cea4b4`, trailer-clean, merges clean, both configurations green): `316feb39fc` the bulk rate given back one measured RTT after `pending` reaches zero (deadline armed once, `GnsTransport.cpp:328`; the reviewer's `bulk_round_traffic_200ms` now drops at 200 ms with the quiet and 5 ms links unchanged); `816f697897` THE FREEZE WAS NEITHER A SEND-SIDE STOP NOR A CLOCK: the evicted peer's uplink still worked (the jam is host-to-peer only), it ran out its own grace at 4400 ms on a round it had left and sent a `MissingFrameTimeout` the host adopted 200 ms later - a stop from a peer in `m_PeerLeaveFrames` is now ignored and counted (`stops_from_left_peers`, `NetLockstep.cpp:3037-3045`), red at `next_frame:20` before, green after; `1410fec3ac` `--leave-at-frame N` in `tools/test_lobby_lifecycle.py` for a sixth lobby lane. Its re-review brief `lanes/lobby-pacing-review/followup-2.md` also asks the reviewer to RE-WEIGH THE RATE CHANGE against Source40's wire (refusals gone without the branch) and say whether the three rate commits should land. THE H4 REGRESSION LANE LAUNCHED (04:50, `h4-a4-semantics/followup-6.md`) in the freed slot. AUDIO FOLLOW-UP 1 DONE (04:56; `lanes/audio-checkpoint-flake/report.md` `## Follow-up 1`; tip `9029cb3d64`, two commits on `269d2ee718`, trailer-clean, merges clean, both configurations green, native 553/0, SimBaseline `94b13fc1`): `d1c29203f0` `CheckpointWriter::Value(const bool&)` binds the reference and memcpy's the storage byte (no bool-typed load on the path; an assigned bool still writes `0 `/`1 `, so no archive changes), `9029cb3d64` pins the exact archive text for assigned bools and the `100 ` token for the unset byte; the sweep found 31 bool members without a declaration initialiser, all assigned at construction. SIDE FINDING for a later lane: SimBaseline's per-tick `controller` subsystem hash is NOT run-to-run stable at ticks 26-28 on unmodified main `269d2ee718` (two runs of the same binary give the same divergence signature; `final_total_hash` unaffected, the controller is off-wire) - any gate comparing per-tick hashes element by element reads a false divergence there; the pacing reviewer is asked to reproduce and name it. The pacing re-review (`lobby-pacing-review/followup-2.md`, now also covering the audio tip `9029cb3d64`) launches in the freed slot. MATRIX PACE: 14 of 106 jobs in the first 85 minutes (about 6 min a job), so the Source40 family ends around 14:00 local; the reviewed tips merge then. PACING+AUDIO RE-REVIEW RETURNED (05:18; `lanes/lobby-pacing-review/report.md` `## Follow-up 2`): `stage2/lobby-pacing` READY as `1410fec3ac` WITH A FOURTH COMMIT retiring the rate machinery (the one-RTT hold fixes the blocker on all three probes; the freeze fix is right, the reviewer's own clock inference withdrawn, the shape reachable on GNS since `TimeoutConnected` counts received packets; the stop filter cannot swallow anything the round needs, and `stops_from_left_peers` must be read; the rate change lost its premise - refusals gone on main at 2 MB/s, an 8 MB buffer drains in 4.0 s at 2 MB/s inside every bound and 32.0 s at 256 KB/s inside none, and the branch passes without the three rate commits). `stage2/audio-checkpoint-flake` READY as `9029cb3d64` (the overload wins on every path, `vector<bool>` right, the reviewer's own sweep of 419 uninitialised bool members finds none reaching an archive; the controller flicker is a documented advisory property no gate compares element by element, not reproduced in six runs). The lobby lane is resumed with `lanes/lobby-grace/followup-6.md` (restore the 2 MB/s and 32 MB/s rates, retire `SetBulkTransferMode` and the hold; keep the classification, the bound, the overflow rule and the stop filter; residual: nothing calls `Disconnect` on an evicted seat). MERGE BATCH AT THE FAMILY'S END: `stage2/setaside-residuals` `9d35f97f94`, `stage2/audio-checkpoint-flake` `9029cb3d64`, `stage2/lobby-pacing` `9309afa918`, `stage2/h4-phase-a` `bab95b50ed`, `stage2/transaction-controls` `12af44c0a8`, `stage2/h4-b1-resync-fix` `1eb6314055`. LOBBY FOLLOW-UP 6 DONE (05:31): `9309afa918` retires the send-rate machinery (7 files, +8/-380: 2 MB/s, 32 MB/s and 8 MB are main's again byte for byte; `SetBulkTransferMode`, the drain latch, `ApplySendRate`, the lobby's two call sites and the two `bulk_rate_*` fixtures gone; kept: congestion told from a fault, `m_BytesHandedOver`, `Stop()` clearing both caches, the metered loopback queue, and the throttled detailed status as the diagnostic that made the retirement provable), both batteries green, trailer-clean, merges clean; `stage2/lobby-pacing` at `9309afa918` is MERGE-READY per the reviewer's verdict. The B1 lane is launched on `followup-2.md` (the resync save's named refusal) in the freed slot. H4 FOLLOW-UP 6 DONE (05:38; `lanes/h4-a4-semantics/report.md` last sections; tip `769b3b0f4b`, seven commits on `269d2ee718`, trailer-clean, merges clean, 11/11 both configurations, native 546/0, SimBaseline identical, fuzz 4/4): R1 AND R2 ARE ONE DEFECT, A REGRESSION OF A7's ABSOLUTE CLOCK - the A7 reviewer's own F1.6 residual in production: the round owns the transport for the whole match, lobby/lockstep packets never stamp `m_LastReceiveMs` (`NetSession.cpp:380`) and nobody sends a session heartbeat, so with real elapsed time the first evaluation after the match (`NetLobbySession.cpp:123` / `NetMatchService.cpp:413`) measured the entire match and evicted everyone: the host waited for a round nobody could form and the leaving client killed its transport before the LeaveRequest flushed; both reproduced in process on `269d2ee718` with the drivers' signatures; `f14d9a5d67` makes `CheckTimeouts` treat a step longer than the budget as a resumption (control: a peer quiet under a ticking session is still evicted; P14 untouched). `063765f1a9` `host_reseat_issued` is NOT R1's twin: `reclaim_socket`'s ledger is right and its units are dead by the reclaim (`actors 2` of a peak 4 vs `drop3`'s 5 of 6), so the engine hands back nothing; two early returns get separate counters the drivers read. Residuals landed: `fb7dffb7fc` (good drop record kept), `be8f0b8fe3` (max clock divergence at pump time), `b4d40b1006` (BeginLeave on the admission clock), `b836ba8a88` (the census under the lock in the mutex check), `769b3b0f4b` (the live-reference check); open: followup-5 item 6 and 7(a,c,d,e,f). Re-review launched: `lanes/h4-a7-review/followup-2.md` (session `d7bd074a-5370-440b-8613-4e4db43023ee`). B1 FOLLOW-UP 2 DONE (05:51; `lanes/h4-b1-substitution/report.md` `## Follow-up 2`; `6fa71e01f5` on `stage2/h4-b1-resync-fix`, fast-forwarded onto `269d2ee718`, trailer-clean, merges clean): the resync save failure on `substitute_commit`/`substitute_returner_wins` is NOT a writer refusal (`no persistent owner` nowhere in the four Source40 runs) but `ActivityMan::SaveCurrentGame`'s state guard (`ActivityMan.cpp:141-143`) on an activity already `Over`: A5's game-over hold read `IsHoldingSeatForReclaim()` (every remote gone), so with a stayer still playing the hold never engaged, the duel's win check ended the match inside the dropped seat's reclaim window, and the reconnect had no running game to snapshot; fix: a separate `IsAnySeatHeldForReclaim()` for the activity gate only, ownership consumers unchanged, deferral bounded by P2 and only for a scripted outcome; `TestCoordinatorHeldSeatWithASurvivor` red on base, green on tip. Review launched: `lanes/h4-fixes-review/followup-1.md` (session `d8acb3d1-fd23-442b-beb4-47d343edd5a7`), with the crux question whether a host-only hold of a scripted outcome can diverge the sims (a two-peer tick-hash case across the hold window). H4 RE-REVIEW RETURNED (05:58; `lanes/h4-a7-review/report.md` `## Follow-up 2`): READY as `769b3b0f4b` - both regressions reproduced on the base with the lane's case, verdict lines byte-identical, the tip green on the whole battery; the resumption predicate is the right shape (it sits on the interval between evaluations, which only `NetSession::Tick` performs; exactly one resumption per transition; the blanket restart is the only meaningful choice; a peer that died mid-match is evicted 5 010 ms after the resumption with the round's 20 s missing-frame grace covering the match; P14 still expires on connection age); the reviewer's own F1.6 judgement was wrong in magnitude (the session is never handed a receive during a round at all, so no talking peer protects the transition), P2/P14/P21 hold, no pin value changes, the plan's text should say the budget is measured only over intervals the session was evaluated with P14 exempt. Non-blocking notes for the H4 lane's next follow-up: the predicate has no floor (twenty consecutive slow evaluations kept a never-speaking peer seated 100 s in a probe; no shipped caller ticks that slowly; `timeout_resumptions` shows it; the control should assert `timeout_resumptions == 1` and bound the delay); the reseat counters split the two early returns but cannot tell 'all ledgered units dead' from 'a live unit on the returner's team the ledger never named' - record the count of live-on-team actors the record does not name; the native figure is 157 named cases, not 546. `stage2/h4-phase-a` `769b3b0f4b` JOINS THE MERGE BATCH. The transaction-controls lane launched (fresh) in the freed slot. B1 HOLD REVIEW RETURNED (06:14; `lanes/h4-fixes-review/report.md` `## Follow-up 1`): `6fa71e01f5` NOT READY. The diagnosis stands (`ActivityMan::SaveCurrentGame`'s guard on an activity that ended before the resync), but the fix puts a HOST-ONLY answer behind a decision every peer makes inside the deterministic sim: the outcome is declared by Lua on every peer (`GAScripted.cpp:351/360/361`, `P4AlphaDuel.lua:126-134`), `AnyLeftSeatHeld()` is host-only (`QuerySeatState` returns the default unless `m_IsHost`, `NetMatchService.cpp:1125`), and the replaced predicate carried the very condition that made a host-only answer safe (`NetLockstep.h:459-461`: while it holds there is no other peer in the round). MEASURED in the lane's own scenario on both survivors: control `host_defers=0 stayer_defers=0`, tip `host_defers=1 stayer_defers=0`, up to 1200 ticks of divergence on `actors` and `lua_state` (the host re-running `KillAllEnemyActors` while the stayer's script has stopped) - the ordinary mid-match drop in the four gates. Fixes that would hold: carry the hold as a FRAME DEADLINE on the leave notice so every peer derives it, or repair the resync save instead of deferring the outcome; `peers_3_4_regression`'s `drop3_survivors_identical` is the socket gate that should have caught it. The B1 lane is resumed with `lanes/h4-b1-substitution/followup-3.md` (choose the pin-keeping option, prove agreement with the reviewer's crux fixture and tick hashes across the hold window). THE MERGE BATCH IS FOUR: `stage2/setaside-residuals` `9d35f97f94`, `stage2/audio-checkpoint-flake` `9029cb3d64`, `stage2/lobby-pacing` `9309afa918`, `stage2/h4-phase-a` `769b3b0f4b` (all ten pairs merge cleanly with each other); B1's hold waits for its rework and re-review. TRANSACTION CONTROLS REGENERATED (06:26; `lanes/transaction-controls/report.md`): `contract-audit\full-runtime-transaction-controls-20\` is back with all thirteen `.ccsave` controls and a `manifest.json`, cut by the unmodified `graph_controls.py` from a fresh tick-100 marked seed produced on the lane's own build of `269d2ee718`; all five VM graphs parse and re-encode byte-exactly (the SG3 writer unchanged since Source20), two generator runs give byte-identical contents, `prepare_transaction_control_commands.py`'s hash assertion passes; the `valid` job is green (17/17, `complete`, `source_unchanged`, `continuation_checks_passed`, marker 731 in all five VMs after the load and at tick 521, nine residual raw records all `RENDER_SCRATCH`); the `refusals` job ran GREEN FOR THE FIRST TIME EVER (every job in the artifact had been `NOT_RUN`): all 11 candidates refused, `raw_field_differences_total 0`, the live world, all five graphs, native identities and the raw observation bit-identical across each refusal, continuing to 521 against an independent process. Finding for the harness: `invalid_iterator` is refused at the EARLY parse stage, not the late stage the grouping predicts (a borrowed iterator's `last` cursor is read as a lower bound; the Source20 archive had the same node, so the grouping was never right for that case) - `prepare_transaction_control_commands.py`'s early/late lists move it when next touched. The `transaction` gate is runnable again for the next family. The H4 lane is resumed on `followup-7.md` (the A7 reviewer's three notes; followup-5 items 6 and 7) in the freed slot. The transaction lane also committed six harness drivers on `stage2/transaction-controls` `12af44c0a8` (`tools/contracts/run_txc_job.py`, `verify_txc_controls.py`, `make_txc_manifest.py`, `classify_txc_refusals.py`, `inspect_iterator_control.py`, `annotate_txc_manifest.py`; +607, trailer-clean, harness only), which merge cleanly with main and with all four batch branches: THE MERGE BATCH IS FIVE. Matrix timing corrected: the matrix step began at 05:25 (the driver waited for the breadth's last engine to exit), about three minutes a job, `references` running at 06:26, so the family ends around 11:00 local. B1 FOLLOW-UP 3 DONE (06:37; `lanes/h4-b1-substitution/report.md` `## Follow-up 3`; tip `1eb6314055`, two commits on `6fa71e01f5`, trailer-clean, merges clean): the crux reproduced on the lane's own control; option (b) closed by the settled decision that a held seat's player stays counted by the activity until the window expires (the Source36 red that created A5), so (a): `541a241d35` puts drop-versus-announced on the wire as `NetLockstepStopReason::PeerDropped` (a new value in the existing u16, no codec version), `1eb6314055` derives the hold as `leaveFrame + c_ReclaimHoldFrames` (1200 = P2's 20 s at the pinned timestep) from the frame the sim is applying on every peer, `IsAnySeatHeldForReclaim()` removed, ownership's predicate untouched; the reviewer's crux fixture extended to five frames and both survivors is red on the control and green on the tip (`host_defers == stayer_defers` at the drop, mid-window, the last held frame, the deadline and past it), a clean leave holds nothing. Re-review launched: `lanes/h4-fixes-review/followup-2.md` (the wire value, the frame across a resync, the expiry, frames against P2's wall clock). B1 HOLD RE-REVIEW RETURNED (06:56; `lanes/h4-fixes-review/report.md` `## Follow-up 2`): READY as `1eb6314055` - both inputs to the hold ride the leave notice, the answer is read at the frame the sim applies, both survivors agree at every boundary (at the drop, mid-window, the last held frame, the deadline, past it; `releases_exactly_at_deadline=1`, leave frame host=3 stayer=3) against `host_defers=1 stayer_defers=0` on `6fa71e01f5`; the wire value is carried unchanged everywhere (whitelist decoder, `PeerLeft || PeerDropped` handled together, one relay site host-guarded, `m_DroppedSeats` cleared in `ResetRoundState`); across a resync the hold ends with the round, which is right. Two non-blocking notes for the lane report (history not rewritten): the commit's reason for skipping a codec version is false (`buildId` is a literal and `gameVersion` a constant, so the identity would accept a mixed round; what protects it is the decoder failing closed with a named `ProtocolError`), and `541a241d35`'s frame bound in `RefreshLeftSeatHolds` changes ownership and round lifetime (a 1v1 round stops at `leaveFrame + 1200` while the plane still holds the seat; deliberate per the in-code comment, denied by the message). Observation for the H4 lane: the plane's 20 000 ms and the round's 1200 frames bound the same seat and disagree in both directions; the frame clock should own the plane's seat hold too. The tick hashes over a real socket are `peers_3_4_regression`'s `drop3_survivors_identical` on Source41. THE MERGE BATCH IS SIX: add `stage2/h4-b1-resync-fix` `1eb6314055`. The round-start lane is resumed on `followup-4.md` (its re-review's three notes: `FlushResendFrames` giving up on an owed frame, the ladder formula, retrying after a `Fail`) in the freed slot. H4 FOLLOW-UP 7 DONE (06:57; `lanes/h4-a4-semantics/report.md` `## Follow-up 7`; tip `bab95b50ed`, four commits on `769b3b0f4b`, trailer-clean, merges clean, battery green both configurations): `0a5d6878f9` a resumption bounded to one restart per silence per peer (the reviewer's probe reproduced first; a second consecutive claim refused; any decodable packet earns a fresh one; the control asserts `timeout_resumptions == 1` and the eviction one budget later), `8a6452e359` `IssueReseat` records the live-on-team actors the record does not name, `4e442a22f6` the held-seat asymmetry measured: `IsHoldingSeatForReclaim()` requires every remote gone, so a hold and a disagreeing peer are mutually exclusive (both peers agree at every sample of a 20 s window; taking the survivor away parts the answers exactly as described), `bab95b50ed` the three fault arms for the socket gates. INTEGRATION REVIEW LAUNCHED: `lanes/h4-a7-review/followup-3.md` re-checks the four commits ON A LOCAL MERGE OF THE SIX BATCH BRANCHES (a throwaway preview branch in the reviewer's worktree; never main) with both batteries, the H4 arms and B1's crux fixture together, and returns two verdicts: `bab95b50ed`, and the six-branch merge as Source41. ROUND-START FOLLOW-UP 4 DONE (07:11; `lanes/round-start/report.md` `## Follow-up 4`; tip `8f493e8ecf`, two commits on `d678bc72ef`, trailer-clean, merges clean): `a39211a7e7` `ReadoptRound` builds each owed frame once and keeps it in `m_ResendFrames` so a local commit cannot take it away, retry until it lands (failing would end a match on transient backpressure on the path that exists to keep one alive), `LoopbackTransport` gains `acceptedSendsBeforeRefusing` making the reviewer's GNS-only shape constructible; `8f493e8ecf` the flush stops once the round has failed; control red 2 of 2; the ladder text corrected in F1.4/F3.4 (RESUME already carried the three-term form). Re-review launched: `lanes/round-start-review/followup-2.md` (session `41009633-10ce-4565-a9a7-bcf754ca2de9`); NOT in this batch: `stage2/round-start-fixes` `8f493e8ecf` conflicts with `stage2/lobby-pacing` in `Source/Network/LoopbackTransport.cpp` (both extended the loopback transport: the metered queue and refusal classification against `acceptedSendsBeforeRefusing`), so after the batch merges the round-start lane merges main, resolves that file keeping both, and its tip lands in the family after Source41. INTEGRATION REVIEW RETURNED (07:20; `lanes/h4-a7-review/report.md` `## Follow-up 3`): TWO VERDICTS READY - `bab95b50ed` ready to integrate, and THE SIX-BRANCH MERGE READY TO BECOME SOURCE41: all six tips merged clean into the reviewer's local `review/source41-preview` (`fc435ec6fa`, tree `55308037293f`), both configurations build, every battery green on the merge (11/11 twice, native 161 named / 0 FAIL, SimBaseline bit-identical, fuzz 4/4 + 17/17 + 10/10, restoration 13/13), nothing red on the merge that is green on its branch (six files touched by two or more branches, `NetLockstepSelfTest.cpp` by three, all passing); the resumption floor reverses the reviewer's probe byte-for-byte (one restart, eviction one budget later; R1/R2 cannot re-open; a real match phase hands the session zero decodable messages); the held-seat question settled on the merged tree (host and surviving client agree at every sample and flip exactly at `leaveFrame + 1200`; the round's hold is `min(plane, frames)` as the pins want). Two notes, neither blocking, plus three plan lines, given to the H4 lane as `followup-8.md` (in the free slot; its commits land after Source41): `common.py`'s `check_reseat_issued` does not read the unnamed-survivor count; `IssueReseat` now calls the census on the `without_a_ledger` path (one line moves the count after the early return); the floor's precondition (two unwatched phases with nothing decodable between them evict); the two seat bounds are independent constants. THE BATCH IS FINAL (six): `stage2/setaside-residuals` `9d35f97f94`, `stage2/audio-checkpoint-flake` `9029cb3d64`, `stage2/lobby-pacing` `9309afa918`, `stage2/h4-phase-a` `bab95b50ed`, `stage2/transaction-controls` `12af44c0a8`, `stage2/h4-b1-resync-fix` `1eb6314055`; after the real merge the tree must equal the preview's. ROUND-START RESIDUALS RE-REVIEW RETURNED (07:26; `lanes/round-start-review/report.md` `## Follow-up 2`): READY as `8f493e8ecf` - the retry is bounded by the host's missing-frame grace (the host is the peer waiting; its stop reaches the follower over the receive direction a refused send does not block), the retained copy carries the controller frames and commands and no readings, consistent with the follower's own commit, and the reviewer added two cases of its own, both red on the control (`review_owed_frame_keeps_its_commands`; the stop guard covering `Stopped`); `acceptedSendsBeforeRefusing` is faithful in shape; battery 11/11 both configurations, native 552/0, SimBaseline identical. It lands AFTER the batch (the `LoopbackTransport.cpp` overlap with the lobby branch): the round-start lane merges the new main, resolves keeping both, adopts the reviewer's two cases, and its tip goes into the family after Source41. The lobby lane is resumed on `followup-7.md` (close an evicted seat's connection, the residual both re-reviews left open) in the free slot; its commits also land after Source41. H4 FOLLOW-UP 8 DONE (07:34; `047d761183` on `stage2/h4-phase-a` above the batch tip: the empty-record early return moved above everything that walks the world, the count computed off the single `actors` fetch, all four arms pinning the call profile 1/1/1/0; `common.py::check_reseat_issued` reads `host_reseat_live_on_team_not_named` and opens its verdict with the world; plan lines (b) and (c) written; trailer-clean; battery green both configurations). It lands with the round-start residuals in the family after Source41. SLICE B2 LAUNCHED (07:45; `lanes/h4-b1-substitution/followup-4.md`, the B1 lane's session; fresh checkout `D:\Projects\h4-b2` on `stage2/h4-b2-moderation` from `1eb6314055`, main merged after the batch lands): the host's moderation panel (disconnected seats, pending applicants, wait / substitute / cancel through the existing substitution API, e2e commands for headless gates), the section-11 persistent roster indication on every peer derived from wire facts, and the remaining section-9b fault-arm gates (initial-disconnect-before-ack, ack-lost, commit-result-lost, delayed-duplicate, substitute-disappears-before-ack), mod compatibility binding. LOBBY FOLLOW-UP 7 DONE (07:42; `1f8bf6632c` on `stage2/lobby-pacing` above the batch tip: the relay host closes an evicted seat's connection after the notice and the flush, counted as `connections_closed_on_eviction`, on the silence bound and the hold bound or a dropped forward; argued against P20 (a returner binds a new transport), P21 and P22 (the ticket cannot be cleared by a transport close); the evicted peer now fails with `PeerDisconnected` in the tick its seat went instead of spending its grace sending into a round that stopped listening; both batteries green, trailer-clean). Re-review launched: `lanes/lobby-pacing-review/followup-3.md`; it lands in the family after Source41. EVICTION CLOSE RE-REVIEW RETURNED (07:52; `lanes/lobby-pacing-review/report.md` `## Follow-up 3`): READY as `1f8bf6632c` - P20/P21/P22 read against the plan text and P25 backing them (the ticket record deleted only on LeaveAck, P22, or age), no window where the seat points at a closed connection (`ApplyPeerLeave` erases the entry before the close), the notice measured reaching both survivors one link latency after the close on both bounds, GNS `Disconnect` flushing sent data before `CloseConnection` with linger, the evicted peer failing in the tick its seat went with a reason naming the bound and its service starting an automatic ticket rejoin; the close also opens the reclaim window and bumps the seat generation at the moment of eviction; no gate moves; battery green both configurations. PREVIEW EXTENSION LAUNCHED (`lanes/h4-a7-review/followup-4.md`): the integration reviewer merges `047d761183` and `1f8bf6632c` into its preview and re-runs everything; if green, Source41's batch is the six branches at these advanced tips (`stage2/h4-phase-a` `047d761183`, `stage2/lobby-pacing` `1f8bf6632c`) and the merge script's expected tree moves to the new preview's. Matrix at 58 of 106 at 07:52 (the fast jobs). STOP POINT (lead paused here, 2026-09-09 ~08:00 local, low on tokens; work is orderly, nothing half-written on the approved tree). STATE OF THE TREE: main `stage2/p4b-interp-lockstep` at `269d2ee718` (Source40's pinned executable `ec9db6d0`), working tree clean except the two rebuilt vendor `.lib` files and the untracked `AbortCode.txt` (both expected). NOTHING from the current review round is merged into main yet, by the no-merge-during-family rule. SEVEN REVIEWED BRANCHES stand ready, all trailer-clean: `stage2/setaside-residuals` `9d35f97f94` (READY), `stage2/audio-checkpoint-flake` `9029cb3d64` (READY), `stage2/lobby-pacing` `1f8bf6632c` (READY; the bounded hold, the overflow rule, the stop filter, the rate machinery retired, and the evicted-seat connection close), `stage2/h4-phase-a` `047d761183` (READY; the session-clock regression fix, the resumption floor, the reseat counters, the reseat census-call fix), `stage2/transaction-controls` `12af44c0a8` (READY; harness drivers, the controls regenerated), `stage2/h4-b1-resync-fix` `1eb6314055` (READY; the deterministic scripted-outcome hold), `stage2/round-start-fixes` `8f493e8ecf` (READY but CONFLICTS with lobby-pacing in `LoopbackTransport.cpp`, lands AFTER the batch). TWO LANES WERE STILL RUNNING at the pause (let them finish on their own; read their results when you resume): `h4-a7-review` (follow-up 4 RETURNED READY after the pause: eight-commit preview `2b916e3201`, tree `3f2f2987...`) and `h4-b1-substitution` (session `b1d797bd-3b7e-4951-b6dd-36e6d2dd8e84`, slice B2: the moderation UI, the roster indication, the remaining section-9b fault gates; task `b7zrldms9`). The registry lists `pos-alias-repair` as running, but only two `claude -p` workers are actually alive (the two above); that entry is STALE, ignore it. The SOURCE40 FAMILY FINISHED 15:22 UTC (exit 2 = expected, from two known non-greens, no new engine crash): `remaining` exit 1 is the transaction gate, whose Source20 control snapshots the cleanup deleted (regenerated on `stage2/transaction-controls`, so the gate itself passes next family); `matrix` exit 2 is 106 jobs, 105 `NEEDS_REVIEW` and ONE `FAILED_OR_INVALID` = `mp_rematch` with `session timeout (timeout_ms: 5000 vs 5002)` on the rematch path - the H4 session-clock regression class that `stage2/h4-phase-a` `047d761183` (`f14d9a5d67`, the resumption rule) fixes, which is IN the Source41 batch but NOT on Source40's `269d2ee718`, so Source41 is expected to clear it (verify on the next family; `D:\mx\s40\mp_rematch`). The matrix wants a full review pass; every other job is `NEEDS_REVIEW` (the matrix's normal state, not a failure). The merge script's family-finished gate is now OPEN. TO RESUME: (1) read the two lanes' results and the matrix's completion; (2) DONE: `h4-a7-review` follow-up 4 passed - the extended eight-commit preview `review/source41-preview` `2b916e3201` (tree `3f2f2987c75d99934d8be173321a891f83983998`) is READY, nothing red on the merge that is green on a branch, the eviction close and the census fix both clean on it; the merge script's `PREVIEW_TREE` and the advanced tips are already set to match; (3) once the family is `finished`, run `scratchpad\merge_batch41.py` (it refuses while the family runs, an engine is alive or the tree is dirty, scans every message for attribution, merges the six in the preview's order, and proves the result tree equals the preview's), then push the checkpoint, then start the Source41 family with `lanes/source33-lanes/followup-7.md` (drafted); (4) after Source41, land round-start `8f493e8ecf` (merge main, resolve `LoopbackTransport.cpp` keeping both) and whatever slice B2 produced. SLICE B2 DONE 08:32 (`lanes/h4-b1-substitution/report.md` `## Slice B2`; branch `stage2/h4-b2-moderation`, tip `703e3f1202`, eight per-concern commits on `1eb6314055`, trailer-clean; battery green both configurations, native 551/0, restoration 13/13, SimBaseline `94b13fc1` zero element differences, the unchanged-Lua fixture found): the host moderation panel (a Seats sub-screen, a row per disconnected seat with wait / substitute / cancel through the existing `WaitForSeat`/`SubstituteApplicant`/`CancelSubstitution` API, driven headless through the panel's own path), the section-11 roster line derived on every peer from the round's seat notices (replacing the host-only source; both survivors produce identical text), and the five remaining section-9b failure windows as in-process and over-the-wire gates (`-net-h4-fault`). Codec note: three new `NetLockstepStopReason` values (`SeatReclaiming`/`Reclaimed`/`Substituted`) in the existing field, allowlisted, `c_Version` unchanged. IT IS NOT REVIEWED YET and NOT in the Source41 batch: it needs an independent adversarial review (it touches the wire and the menu path) and its base `1eb6314055` rides in the Source41 batch, so after Source41 merges it wants main merged, a review lane, and then a family after Source41. THE WHOLE WORKER FLEET IS NOW IDLE (zero `claude -p` workers alive); no lane is building or running, so the merge script's engine-idle and tree-clean gates hold. OLDER LOG BELOW. EARLIER, THE DESKTOP INCIDENT ~04:35 UTC (resolved above): the user reported a game window opening repeatedly on the desktop while working; the lead stopped all three lanes at once (no worker or game process left) and searched: every game launch of the previous four hours went through `win32_test_runner.py` onto a private desktop with the input desktop unchanged, and no worker transcript contains a direct launch of the executable, so the cause is unidentified (a window escaping the runner in a way the records do not show, or a launch from outside these lanes, such as the independent reviewing agent's sessions); the user then said resume, and the three lanes were resumed from their sessions. VERIFICATION AFTER THE INCIDENT (times in the notes above are about 1.5 h too late; the incident was ~02:30-02:56 UTC by the runners' own clocks): the approved tree is intact (git clean but the vendor `.lib` outputs, 8,516 tracked `Data` files on disk, every pinned Source39 artefact hash matches, only the merge and the chain build touched it); of the 182 game launches in the previous three hours every one ran on a private desktop with the input desktop `Default` before and after, i.e. the runner never switched the user's desktop, and no transcript holds a direct launch; the abnormal exits are all explained (the expected red controls with exit 1 or 5, the harness review's fourteen `nullable_turret` runs exiting `3221225781` = STATUS_DLL_NOT_FOUND from its own checkout, which it must redo, and the runs cut by the stop). The most likely thing the user saw is the Xbox Game Bar reacting to each new `Cortex Command.exe` process even on a hidden desktop (fourteen launches in a few seconds), which the user can settle by excluding the executable from Game Bar or turning its launch notification off; to be confirmed with the user. Because the breadth lane's timing-sensitive runs were interrupted, its Source39 attempt under `D:\mx\s39lanes` is VOID and restarted from scratch under `D:\mx\s39lanes2` (`followup-5b.md`); the harness review and A7 lanes resumed on their own sessions (their runs are deterministic single-process work whose partial results they redo). The user later said: Company of Heroes 2 fullscreen kept being minimised while `Cortex Command` launched and closed repeatedly; then it happened again with the user not gaming ('closes right away, you cannot catch it'). Measured with a desktop watcher (`scratch desktop_watch.py`, results `D:\mx\watch\watch-*.json`) during hidden launches on the approved tree: a selftest, a 60-tick and a 900-tick scenario, and a deliberate DLL-less launch (exit 3221225781 at once) - NO window of the game or of any other process appeared on the interactive desktop and the foreground changed only with the user's own Alt-Tab (an `AMDAutoUpdate.exe` console window was open on the desktop at the time); the process is created suspended on `winsta0\<private>` with `SW_HIDE`, `CREATE_NO_WINDOW` and a job forbidding desktop switches and display-settings changes. The mechanism is therefore STILL UNIDENTIFIED for the non-gaming sighting; for the gaming case the working hypothesis is graphics-driver-level loss of exclusive fullscreen when another process creates a rendering context, which no desktop isolation prevents. Mitigation landed as main commit `da9944387`: `win32_test_runner.py` holds every hidden launch while `SHQueryUserNotificationState` reports a fullscreen or presentation state (recorded as `waited_for_user_fullscreen_seconds`; `CC_RUNNER_IGNORE_FULLSCREEN=1` bypasses), and the lanes were told to merge or cherry-pick it before launching. A joint one-minute test with the user in a fullscreen game would settle the mechanism. DISK ~05:30 UTC (clock approximate): D: back to 40 GB free because lane scratch under `D:\mx` grew 46 GB in eight hours (`wsr2` 9.5, `swr` 7.4, `wsr` 6.1, `mlr` 5.9, `al` 4.2, `ml` 4.1, `mh` 3.4, `sw` 2.5, `wsr3` 2.5: reviewer worktree clones, control executables, restoration batteries) plus new worktrees; a binding 'Scratch footprint' rule now caps lanes; retirement of that scratch (all reviews concerned are written) is proposed to the user, unauthorised until they say so; the Source39 matrix needs ~55 GB free before it can start; the Mac Source39 chain finished green meanwhile: exact export `combined-source-39` built as `2bd9bd2e` with zero source mismatches, native pass, authority match, wounds/rng_particles/ui_bus 0 changed ticks, ten selftests 10/10 (mirrored under `mac-peer-20260907/results-source39/`); the three lanes resume from their sessions after the chain (`round-start` `aca12b13`, `matrix-harness-review` and `h4-a4-semantics` `8c31d315`, sessions in `lanes/registry.json`) (the harness branch merges after its review, before the remaining gates, because their classification step imports its modules); the breadth brief `lanes/source33-lanes/followup-5.md` and the Mac script `chain_mac_39.zsh` are written. The earlier plan text follows: the lead builds Source39 with `chain_build_gate.py --label source39-1 --source 39`, runs the remaining gates and a breadth follow-up (A6's flips, B1's substitution gates, the five lobby lanes with the relay counters, the sound gates identical, `unresolved_observation_packets` 0 and `stale_round_packets` non-zero only on heal), the Mac chain, and pushes that as an intermediate checkpoint; round-start and A7 (each reviewed) join before the final combined Source39 matrix. `matrix-harness` DONE (`lanes/matrix-harness/report.md`): in-repo commits on `stage2/matrix-harness`, tip `cf816ba86` after the lead stripped the harness-injected `Co-Authored-By`/`Claude-Session` trailers the worker had added (trees unchanged; a binding 'Commit trailers' rule now sits in CLAUDE.md/AGENTS.md and the lead scans every branch before a merge; the four pushed commits that match the scan are 2022-2025 upstream CCCP commits with human co-authors): F4 (the audit gate requires `applied` for the five applying transitions, no engine error outside a refusal, no graph problems; `nullable_turret` re-run now returns 1 with `hold_5` red, so F1 reproduces independently), F3 (`tools/cross_process_state.py` names each real-clock anchor with its writer; all 34 retained continuation cases re-classified, refusal cases 3,480 anchors in 39 families all real-clock, every residue RENDER_SCRATCH), C (the errors exemption follows the recorded outcome: over 406 retained cases it fails exactly `hold_5` and keeps the 44 refusal cases green; the pre-fix gate passed an accepted load with an injected error, traceback or assert), D (`tools/state_document.py` reads a dump as the records the writer wrote so no rule reaches inside a `std::quoted` string; real on the retained document, 22,233 records span lines; no engine change required, the writer change spelled out anyway); out of repo, in place with retained pre-edit copies under `lanes/matrix-harness/patches/`: F5 in `full_collected_matrix.py` (104 to 106 jobs with `invariance_*_300` green through the child path, 18 dimension strings corrected) and F6 in `classify_inactive_carriers.py` (matrix-wide 190 of 205 carriers prove inactive, the other 15 being F1's own sweep); unit tests 52+12+10+10+9 green. `stage2/matrix-harness` must be merged before the next matrix (the classifiers load the new modules from the tree they point at and fail loudly until then); F3's projection is a comparison exclusion and got its independent review (`lanes/matrix-harness-review/report.md`, 646 lines): READY TO MERGE as `cf816ba86` plus the two out-of-repo edits, no failing case - the exclusion is sound on stronger evidence than the lane's (30,376 of 30,378 matching fields are a `Timer`'s `m_StartRealTime` whose sim anchor and limits stay unmasked, `PersistedTimerAnchor` never masked, injected sim fields still reported, the projection reached only at the single `cross_process=True` site); F4/C fail exactly `hold_5` across 406 retained cases with the 44 refusals green and F1 reproducing on a third executable; D real (an independently written strict parser agrees on all 5,349,056 records, 22,233 multi-line, 153 with NULs); F5/F6 zero drift beyond the two tick-300 jobs and 18 strings, the tick-300 job green through the child path. Non-blocking notes: `APPLYING_OPERATIONS` is a membership list, the parity reader assumes no backslash outside quotes (measured 0), `graphs_serialized` vacuous on an empty list. MERGED into the milestone branch by the lead (tools only, no rebuild; the harness unit tests run on main) so the remaining gates' classification loads `tools/state_document.py` from the approved tree. Queued: `round-start` (`aca12b13`), the H4 slice for A and B, the reviewer re-checks after sound-wire and the audit residuals.

**2026-09-09 ~01:05 UTC SECOND SAVE POINT (usage pause of about an hour; read this before the first save point below).** Source38 is closed as the audio/compatibility family (matrix complete and reviewed, every red triaged, the P5 harness gap fixed on main). Main `D:\Projects\p4b-interp-validation` is at `4b39e3028` (Source38 `32312c71e` plus two tools-only commits: the `AudioRuntime3` and `BuyMenuGUI3` snapshot schemas), unpushed, no PR. Source39 is staged on `stage2/integration-39` at `3a49af31d` (A6 + lobby + B1 + the FIRST registry repair) and must be re-composed before it is built: merge the FINAL branches - `stage2/registry-audit` at `1edc4f8d4` (the reviewer's follow-up 2 verdict: READY TO INTEGRATE - control `38a22f03` 525/4 with `hold_5` `applied=false`, tip `db1cf6da` 529/0 with all 14 `applied=true`, settle-first shown state-neutral to the byte, 'recorded, not returned' total; two non-blocking residuals for a later audit follow-up: two other pre-settle capture sites, the rollback probe's comparison capture at `Main.cpp:1694` and the contract audit's `observe` at `:2467`, each one `CaptureRuntimeGlobals()` away, and a refusal before the world moves latching `m_HasWorldSetAside`), `stage2/sound-observation-wire` at the tip the lane's follow-up 2 produces on top of `b10c319c9` (the reviewer's verdict on `b10c319c9` is ready-to-integrate; the two closing shapes are in flight), `stage2/archive-loads-fix` at `52ccf2487` (review positive; main already carries its schema), `stage2/mo-link-restore` at `be9e8d72e` (review in flight) - by resuming lane `integration-39` (session in `lanes/registry.json`) with a follow-up naming those tips, then `run_source38_family.py` copied as a Source39 driver (edit `STAGED_TREE_COMMIT`, the `38` labels and roots, the breadth prompt `followup-5.md` to be written with A6's section A6.5 flips, B1's `tools/h4_substitution_gates.py` gates, the five lobby lanes with `largest_relay_packet_bytes`, the sound gates identical, `unresolved_observation_packets` read per the sound-wire report's corrected section 8), then the Mac chain, then the matrix from `D:\mx\s39` (D: has 63 GB free after the archive moves; a matrix needs ~55 GB).

Running at this pause, resumable with `python launch_worker.py <lane> --resume <session> --prompt lanes/resume-after-limit.md --cwd <worktree>` and never more than three at once: `setaside-review` follow-up 2 (`a874a2c9-b293-445c-bd5a-4f1cf41d1875`, cwd `D:\Projects`), `sound-wire` follow-up 2 (`b49a883e-85bf-4642-ac1f-f64838d92114`, cwd `D:\Projects\sound-wire`), `mo-link-review` (`4d25e2b9-e280-4833-9f8e-e49d540a1d03`, cwd `D:\Projects`). Queued, never started after the memory kill: `matrix-harness` (`b838155a-2250-481b-927f-7394f56b495e`, cwd `D:\Projects\harness-39`; F3-F6 tools fixes needed before the Source39 matrix) and `round-start` (`aca12b13-f82b-4486-92fb-526270d5f324`, cwd `D:\Projects\round-start`; two pre-existing lockstep start defects, next family after Source39). Also pending: the archive-loads reviewer's non-blocking probe (restored flags follow their module) as a later check; the Phase B moderation UI (slice B2); the headed section 11 review with the user's consent; the disk decision (which superseded evidence may be deleted rather than archived) is the user's.

**2026-09-08 ~18:20 UTC SAVE POINT (usage pause; read this first).** Engine: main `D:\Projects\p4b-interp-validation` at `32312c71e` (280 ahead of origin, unpushed, no PR; only the two vendor `.lib` build outputs are modified). The final family is **Source38 = `grouped-build-f29f69bc`** (approved executable `f29f69bc…`, the merge of the staged tree `de87ff198`): eleven selftests, authority match and fault, wounds, rng_particles, ui_bus, ai_defer, the alias-only two-peer arm (`ai_defer_alias`, second run after a fixture fix) and the fault-armed per_machine control green (`chain-source38-1.json`); native 526/0, heal, invariance, restoration, transaction (8 render-scratch differences) and both fuzzes green (`chain-source38-remaining.json`); the independent reviewer's mod-compatibility verdict is green. Mac: Source37 `c12fc401` green on the chain and selftests; the Source38 tree built as `30e06f16` (staged export i38, ten selftests 10/10) and the exact `combined-source-38` chain (`chain_mac_38.zsh`, results `chain-source38.json` and `selftests-38-result.json` on the Mac evidence directory) was running at the save point.

RESUMED 2026-09-08 ~20:05 UTC after the 1 pm (Phoenix) reset: the lobby-grace and Phase B lanes were cut at the limit (17 and 34 uncommitted files kept in their worktrees) and are resumed on their sessions with `resume-after-limit.md`; the breadth lane was cut at gate 1 and the family driver went on to the matrix, which is running from `D:\mx\s38` (19 of 104 jobs done at the resume) - the breadth lane follow-up 4 is resumed only after the matrix releases the executable; a new independent review of the set-aside repair runs as lane `setaside-review` (runs under `D:\mx\wsr`); Mac Source38 is green from the exact export (`combined-source-38`, binary `30e06f16`, the same bytes as the staged i38 build: native pass, authority match, wounds/rng_particles/ui_bus 0 changed ticks, ten selftests), mirrored under `mac-peer-20260907/results-source38/`. UPDATE ~21:05 UTC: the Source38 breadth (`lanes/source33-lanes/report.md` `## Source38`, runs `D:\mx\s38lanes`) holds: gates 1-4 and 6 green and identical to Source37 (battery 10/10, interp matrix 16/16, fake-lag 100/200 with 0 resyncs after one repeated single-run outlier, tick-300 invariance 8/8 with its control, discovery PASS); the eight H4 gates repeat the Source37 verdicts check for check with `peers_3_4_regression` 5/5 and residues message-for-message identical, all three belonging to slice A6 which is not in this build; `ai_defer_alias` passes and not vacuously (both owned actors hold, write and handler-write through the kept position, both peers read the identical sequence, alias traces md5-equal, 4808/4808 effects); the compatibility ladder is character-identical across all four fixtures with 28/28 deferral cases on both rungs (the `spawn_child uid` line varies run to run on the same executable, Source22 itself printed 1049494 then 1049517, so it carries no build signal); lobby lanes repeat the Source37 shape 3/3 identically (3-peer control green, the three 4-peer lanes red with EResult 25 and 22-34 host refusals, `largest_relay_packet_bytes` null because that counter is on the lobby branch); `per_machine` reads the committed authority value (histograms md5-equal), the chain's fault-armed run diverges. THE SOURCE38 MATRIX IS COMPLETE (~22:26 UTC, `D:\mx\s38\matrix.json`, status COLLECTED_REQUIRES_GATE_REVIEW): 88 of 104 jobs finished with return code 0 (NEEDS_REVIEW, to be read one by one), 15 FAILED_OR_INVALID, 1 blocked. Triage of the 15: (a) `archive_loads` - the staged seed's restart, lane running (below); (b) every 3- and 4-peer lobby lane at 0 and 100 ms plus `lobby_short_trace_control` (3-peer rejoin at 0 ms) - host `condition wait timed out: remoteready`, a client's session failed on a roster peer it had no seat for yet, the lobby lane's `2934952e1`; (c) `lobby_2_drop_200`, `lobby_3_drop_200`, `lobby_4_drop_200` - the heartbeat-timeout drop keeps the departed member, `7c6b9bea3`; (d) `lobby_4_rejoin_200` - every peer 177-178 of 180, the EResult 25 byte volume (`3d8d285d2` plus the sound-wire branch); (e) `mp_snapshot_p5` (`snapshots_sim_identical`, and `peer_roundtrip` blocked behind it) - a HARNESS GAP, not an engine divergence: all 600 tick hashes match, and the two peers' RuntimeGlobals blocks differ only inside the `audio` archive, which the deferral work bumped to `AudioRuntime3` (`AudioMan.cpp:1392`, AudioRuntime2 plus the two deferred sound-op counters) while `tools/snapshot_runtime.py` knew only AudioRuntime1/2, so the FMOD listener and player positions (local by design, masked for the earlier versions with the same field-specific evidence) could not be masked; with the schema and the same two masks added (main commit on top of `32312c71e`, `test_compare_snapshots.py` 52/52 including the new AudioRuntime3 case) the projected blocks are byte-identical including the deferred counters; `mp_snapshot_p5` re-ran GREEN under `D:\mx\s38r\mp_snapshot_p5b` on the unchanged executable (fresh tick-300 saves, `PASS: shared snapshot state matches`) and `peer_roundtrip` GREEN (its rerun wrote into `D:\mx\s38\peer_roundtrip`, the blocked job's empty slot, after the matrix, against the original P5 saves with the corrected comparer; `rerun_matrix_job.py` now also re-roots JSON-escaped child payload paths). The 88 NEEDS_REVIEW jobs were read one by one against their oracles by lane `matrix38-review` (`lanes/matrix38-review/report.md`, ~500 lines, scratch `D:\mx\m38rev`): 82 GREEN, 6 FINDING, 16 triaged by the lead; 1,109 recorded binary hashes all equal the pin; the retained classifiers were run over every residue (83,955 raw audit records resolved to 65,299 proven-inactive carriers plus named residues, no field masked); `continuation_normal`/`continuation_late` are 22/22 after being red on Source35/36, every MP lane compares every tick with no divergence, both positive controls fire. Findings and their lanes: F1 (ENGINE, scored green by the matrix) - `MovableMan::SetAsideWorld` (`:1397`) serialises the script graphs before `CaptureRuntimeGlobals` runs the checkpoint GC settle, the opposite of `CaptureWorld` (`:1233`), so at tick 5 the settle sweeps three script-owned objects the captured graph names, `ReinstateWorld` refuses and returns early past the stash release, the unique-ID pin, the cursor and terrain restores (`nullable_turret/hold_5`, `applied=0`) - to the audit lane as `followup-3.md` (ordering rule shared by every capture path, atomic refusal, red-then-green selftest); F2 (ENGINE) - one object per affected restore keeps an `m_pMOToNotHit` collision-ignore link the live world had cleared, 183/183 other links round-trip, the residue open since Source13 now measured - lane `mo-link-restore` (worktree `D:\Projects\mo-link`, branch `stage2/mo-link-restore` from `3a49af31d`, runs `D:\mx\ml`) DONE (`lanes/mo-link-restore/report.md`, 603 lines): `Scene::SaveSceneObject` (`Scene.cpp:1456`) wrote `MOToNotHitUniqueID` only when a live link existed and `MovableObject::ReadProperty` (`:541`) only ever added one, while `Create(reference)` always installs the preset's link (the Battle Rifle ignores its own discarded magazine), so absence meant 'say nothing'; `350a0a38f` makes a full-data save always state the answer (`= 0` for none) with the reader authoritative, `be9e8d72e` pins it with `restore_takes_only_the_recorded_ignore_link`; tip `be9e8d72e`; base `421afdff` reproduces both the archive and script-graph shapes as one writer/reader, control `5f0086d1` red on exactly the check, audit diffs 7,044 to 7,038 and 3,765 to 3,753 with the diff-of-diffs exactly the residue, both configurations green (eleven selftests, native 530/0, restoration 13/13, fuzzes, transaction `valid` RENDER_SCRATCH only, SimBaseline identical); the independent review (`lanes/mo-link-review/report.md`, 620 lines, five executables of its own) calls `be9e8d72e` READY TO INTEGRATE: the residue reproduced in both shapes and removed exactly, the control red on exactly the new check; old saves safe three ways (the reader's new line sits inside the property's own `MatchProperty`, the thirteen retained Source20-era archives with 69 property-less records each pass the transaction gate on the tip, a probe stripping the property reads identically on base and tip), no shipped INI carries the property, `Index.ini` byte-identical, `= 0` the engine's existing sentinel, no harness schema or mask needed; one bounded unreached difference (a live link the writer cannot name by uid is now recorded as none; an armed diagnostic printed 0 lines across a full `world_normal` job); peers in step (identical 95-pair link sequences across two processes; the comparer sensitive to the lines); family gates green on its tip build. Non-blocking notes: no fixture exercises a gameplay ignore link, `ResolveFaithfulLinks` can leave a stale non-zero uid (pre-existing), the INI and the borrowed row name the link by different uids. It also confirms that `classify_continuation_raw.py` now imports `tools/state_document.py` from `stage2/matrix-harness`, so the remaining-gates classification fails on the approved tree until that branch merges (merge order for Source39: harness first); F3, F4, F5, F6 (HARNESS) - the raw refusal comparison carries 3,480 cross-process real-clock anchors (39 field families, all anchors) that `a6aa570fb` masked only for the Lua comparison, `run_audit.py`'s gate never reads `applied`/errors/raw differences (why F1 returned 0), three job families claim more than they carry (`mp_resync_heal`/`mp_rematch` untraced, the leave lanes start no match, the invariance jobs probe tick 143), and the carrier classifier lacks `m_PersistedVelOscillations` (175 records) - lane `matrix-harness` (worktree `D:\Projects\harness-39`, branch `stage2/matrix-harness` from `2f686e2d2`, runs `D:\mx\mh`), which also adds tick-300 invariance jobs to the matrix. Gates: 2 met, 1 met on engine behaviour but blocked on F3, 7 not met (`audit_classification` now blocked by F1/F2 rather than absent evidence). INCIDENT ~23:30 UTC: six lanes building at once exhausted memory and the harness killed every background task; sessions persist, so the lanes are resumed in batches of three (rule saved in memory): batch 1 `sound-wire` (session `b49a883e-85bf-4642-ac1f-f64838d92114`), `archive-loads-review` (`832cf7aa-78f5-4141-a4ef-a216915a735e`), `world-setaside-audit` follow-up 3 (`178e6299-f2e4-4d6e-b56c-7e95b4b75aa1`); batch 2 as they finish: `mo-link-restore` (`ce02f1de-6216-469d-8b6b-52e9556052c2`), `matrix-harness` (`b838155a-2250-481b-927f-7394f56b495e`), `round-start` (`aca12b13-f82b-4486-92fb-526270d5f324`), each with `launch_worker.py <lane> --resume <session> --prompt lanes/resume-after-limit.md --cwd <worktree>`. Its first red (~21:45 UTC, 54 of 104 done): `archive_loads` FAILED_OR_INVALID, all 16 archive controls including `success` and `legacy_rgb` - the candidate handling is right (corrupt archives refused with the expected messages, loadable ones loaded) but the staged seed's restart fails on every case with `invalid activity UI checkpoint: BuyMenuGUI player=0` (`GameActivity.cpp:2576`, strict `LoadCheckpoint`) and `the saved activity runtime state did not restore` (`[load-selftest] FAIL … restarted=0`); the seed is the tick-100 `contract_audit.ccsave` the `failure_seed` job wrote on this same build; no earlier matrix ever reached this job, so it is new evidence, not yet a proven regression. Lane `archive-loads` (worktree `D:\Projects\archive-loads`, branch `stage2/archive-loads-fix` from `32312c71e`, runs `D:\mx\al`) root-causes it, bisects across the pinned executables (Source22/31/35/37), FINISHED (`lanes/archive-loads/report.md`, 537 lines): two pre-existing engine defects, neither a Source23-38 regression - `BuyMenuGUI.cpp:2502` refused any record whose per-module expansion flags did not number exactly the loading installation's modules (`Tests.rte` loads only under `-scenario`, so the seed job wrote 13 flags and the loader had 14; the seed is right, the validation was wrong; first failing on Source22 from `7ce256fdd`), and `Main.cpp:2609` returned exit 1 whenever a load left the saved game running past the scenario's finalizer, so the job could never have gone green; five commits `4a57684dc`, `9da1b621c`, `3688f0ea5` (the same class in `ObjectPickerGUI`, where a short vector would have thrown in `.at(moduleID)`), `8d3d68bbf`, `52ccf2487` on `stage2/archive-loads-fix`, tip `52ccf2487`: flags keyed by module name and sized to the installation, legacy records still load; on tip `2059a7ea` and the GNS-disabled `45619ed5` the 16 archive controls are green four times over (retained and regenerated seeds, both configurations), native 532/0, eleven selftests, restoration 13/13, fuzzes, transaction `valid` complete with 9 render-scratch, SimBaseline byte-identical, the control build `d9503ff2` red on exactly the four new checks; no Lua-facing behaviour moved (`SetModuleExpanded` is setter-only, no getter). Open for the lead, untouched: whether a replaced activity's Lua `EndActivity` should fire on a mid-game load. The independent review (`lanes/archive-loads-review/report.md`, 443 lines) reproduced every claim on its own builds (16/16 red on the unrepaired engine with the matrix's exact lines, 16/16 green on the tip against both seeds, a Source20-era archive loading on the tip, an old build refusing a `BuyMenuGUI3` record loudly, native 526 to 532, restoration, fuzzes, SimBaseline identical, the control build red on exactly four checks, nothing script-visible moved) and calls the engine repair correct, with one tools-side blocker: the record tag bump to `BuyMenuGUI3` has no `tools/snapshot_runtime.py` schema, so its four real-clock anchors (`blink/menu/repeat_start/repeat_timer.real_start`) stop being masked and the retained P5 peer pair flips from PASS to FAIL on `player_ui.0.buy.menu_timer.real_start` with zero simulation difference - the same gap `2f686e2d2` closed for `AudioRuntime3`; the lead landed `SCHEMAS["BuyMenuGUI3"] = SCHEMAS["BuyMenuGUI2"]` (the module-keyed flags ride in the tail like version 2's) with the harness test `test_buy_menu_runtime3_masks_only_its_timer_anchors` as main commit `4b39e3028` (53/53 harness tests); the picker has no schema at either version, so `ObjectPickerGUI3` changes nothing there. `stage2/archive-loads-fix` at `52ccf2487` is therefore ready for the Source39 integration. Non-blocking notes recorded: the reviewer's probe that restored flags follow the module they were saved for and the apply path installs them (7/7 on a probe build) is a missing check to add later; case-sensitive module names; the bare-picker round trip; forward compatibility for future bisects. Second red (~22:00 UTC, 57 done): `lobby_2_drop_200` FAILED_OR_INVALID while `lobby_2_drop_0` and `_100` and every 2-peer leave/rejoin lane pass - the host detects the drop by heartbeat timeout (`timeout_ms: 5000 vs 5006`) but its roster keeps `Departing(team1,remote,ping0)` as a member, so `wait_connected 2` for the replacement times out and no match starts (`no runner.lockstep block`); at 200 ms the heartbeat timeout wins the race against the peer-closed callback, so the host hangs up itself, which is exactly the path `7c6b9bea3` repairs (`GnsTransport::Disconnect` emitting the local `PeerDisconnected` and handing the peer to the admission plane); expected green on Source39, to be confirmed there, not fixed on Source38. New lane ~21:30 UTC: `sound-wire` (worktree `D:\Projects\sound-wire`, branch `stage2/sound-observation-wire` from the integrated tip `3a49af31d`, runs `D:\mx\sw`): a compact wire form for the per-frame sound observations under a lockstep codec bump 13 to 14 (per-sender slot dictionary, full key on first use, `{slot, value}` afterwards, version-13 decode kept), a deterministic overflow rule above the 512-per-packet cap, replay records kept readable, codec and four-peer loopback selftests with the relay byte counters at N = 64/256/512, the full socket-free family on both configurations; the committed-audibility contract, every value and the sampling cadence stay exactly as they are (the lead decided the byte volume is the lever because `GnsTransport.cpp:110-112` already allows 8 MB and 32 MB/s; the Source39 lobby measurement decides whether pacing is also needed). FINISHED ~22:30 UTC (`lanes/sound-wire/report.md`): five commits `f58a32dd1`, `507308ecf`, `cb9efbbb7`, `6f20aa97c`, `62aaf906f`, tip `62aaf906f`, lockstep codec 14 with 13 still decoded; a repeat observation is 5 bytes (8.8x), a first use 21 (2.1x), worst case 57; the four-peer loopback steady-state relayed frame falls 2940 to 444 B at N=64, 11388 to 1596 at N=256, 22652 to 3132 at N=512; the 512 refusal is replaced by a 4096 cap under a 24 KiB budget with the rest carried into the next frame (carried first, minus re-sampled keys, a bounded held set dropping the stalest on the sender, counted); nothing in the committed-audibility contract moved; build `577f8875` on both configurations: eleven selftests, native 529/0, restoration 13/13, fuzzes, SimBaseline and both retained replays identical, 454 `.ccreplay` files all at versions 8-11. Design flag to weigh: `RelayToOtherRemotes` moved ahead of the pre-start buffering and the late/skew drops so every receiver sees a sender's whole binding sequence (a single shared decode/relay table was wrong and the four-peer selftest caught it, `unresolved_observation_packets: 24`). The independent review (`lanes/sound-wire-review/report.md`, 538 lines) reproduced the whole battery and the byte counts to the digit on its own builds and calls the tip ready to integrate once one hardening item closes: after the 4096-slot table wraps, a receiver that misses the rebinding packet resolves the slot to the evicted key silently (`decode_ok=1`, no counter) - latent today because every path that loses a forward also stalls the round, but not to be relied on; the resync/reclaim case is safe by construction (`QueueLocalInput` refuses unless Running, both tables reset in `Start()` before the local start goes out; measured across a staggered restart); the relay reordering forwards five classes the host still refuses and every remote applies the identical rule; three corrections (a dropped carried reading never converges because `m_LastSentAudibility` already holds it, `unresolved_observation_packets` also counts benign fenced traffic, a stale-round frame no longer stamps its sender alive). The sound-wire lane is resumed (`followup-1.md`) for a per-slot binding generation carried by the repeat form, the convergence fix, the counter split and the liveness question, with the reviewer's probe as a red-then-green selftest - DONE (`lanes/sound-wire/report.md` `## Follow-up 1`): five commits `093f08200` (codec 15: the sender's binding count precedes every observation block, a hole is refused as `ObservationBindingGap`, zero means a round restart and resets the table), `e15f60722` (a stale-round frame counts against its sender again), `0bfb65cc1` (a dropped reading is offered again), `a13804f0d` (an unknown transport's refusal told from a member's hole), `b10c319c9` (the version-14 wrong-key reading kept beside the probe as a permanent control), tip `b10c319c9`, cost 1-2 bytes a frame, pinned builds `8009d5b2`/`31fdbb61` green on the whole battery; the reviewer's re-check (`lanes/sound-wire-review/report.md` `## Follow-up 1`) calls `b10c319c9` READY TO INTEGRATE: every loss pattern behaves as designed (probe `binding_sequence_loss_patterns`, the counts can disagree only once and re-agree only through a zero), convergence exact (12,187 dropped readings handed back under a 9,000-sound flood), nothing A6 or lobby-grace closed reopened, battery and byte counts reproduced; two remaining shapes, closed by the lane in `followup-2.md` before integration: a zero binding count from a stale-round frame resets a live table (a regression against codecs 13 and 14 held off only by the Running gate; fix: a round id on the per-sender tables, a mismatched block decoded against a scratch dictionary and the frame handed on without observations), a clean resync moving `unresolved_observation_packets` (the straggler frames; F1.8's 'condemning number' guidance corrected), and a mid-block refusal leaving the count advanced (stage binds, commit on success). DISK BREAKDOWN measured for the user ~00:50 UTC: D: is 985 GB with ~535 GB of the user's own ComfyUI/games/VR data; the project holds ~325 GB, almost all retained evidence (`D:\Projects\reviews` 154 GB, of which `full-matrix-source32-1` 53 GB and the 2026-09-06/07 recovery trees ~150 GB combined; `D:\mx` 112 GB with `s38` 55 GB; 17 worktrees at ~1.8 GB); nothing deleted, superseded runs are archived to `C:\mx-archive` (210 GB free) and permanent deletion is the user's call. DISK INCIDENT ~23:20 UTC: `D:` reached 0.1 GB free (retained matrix roots: `s38` 55 GB, `s35` 33 GB, `s36` 20 GB, `s37` 12 GB). The `mo-link-restore` lane moved `D:\mx\s35` to `C:\mx-archive\s35` with a `robocopy /MOVE` whose first invocation lacked `/XJ`, so it followed the runs' `runtime\Data` junctions into the APPROVED tree and emptied its `Data` (`C:\mx-archive\move-s35.log`); the audit lane found both its worktree's and the approved tree's `Data` empty in the same window (its own `robocopy /MOVE` of a failed run, `C:\mx-archive\wsa`, had the same shape) and restored both from git; the lead verified the approved tree afterwards: 8,516 tracked `Data` files on disk, `git status` clean but the two vendor `.lib` outputs, the executable `f29f69bc` and every pinned artefact hash of `grouped-build-f29f69bc/build.json` unchanged. A binding 'Disk space and junctions' rule now sits in CLAUDE.md/AGENTS.md above the mod-compatibility section: workers never move, copy or remove anything under `D:\mx`; the lead frees space with `robocopy /E /MOVE /XJ` into `C:\mx-archive` and never touches `D:\mx\s38`. The lead moved `s36` and `s37` that way (`move-s36.log`, `move-s37.log`); `s35` is retained on `C:` with an empty skeleton left on `D:`. Audit lane follow-up 3 DONE (`## Follow-up 3`): `1b2fe43fc` (the settle runs before a set-aside captures the script graphs, `MovableMan.cpp:1412`, every capture and hold path tabled against the one rule) and `1edc4f8d4` (judgeable failures refuse before the world moves, `MovableMan.cpp:1531-1536`; past `PurgeAllMOs` a failure is recorded, not returned, and every remaining step runs; the seven old early returns listed), tip `1edc4f8d4`; control `68c429bc` fails the three new checks with the engine's own F1 line, tip `857d3fef` and no-GNS `f4891bf8` 529/0, the `nullable_turret` matrix job re-run single-process reads `applied=true` on all 14 cases with the sweep identical to the retained run, restoration 13/13, fuzzes, SimBaseline identical; the reviewer re-checks (`lanes/setaside-review/followup-2.md`, runs `D:\mx\wsr3`). The two pre-existing round-start defects the reviewer found (a client restarting its round before the host adopts the old one rejects the new round forever; the host never re-relays a client's retransmitted start) go to lane `round-start` (worktree `D:\Projects\round-start`, branch `stage2/round-start-fixes` from `3a49af31d`, runs `D:\mx\rs`); its two-peer gates (the seven sound gates identical, the lobby counters smaller by the stated factor, `unresolved_observation_packets` 0) run on the approved path with the family. UPDATE ~20:45 UTC: the matrix is slow (20 of 104 jobs in the first three hours), so the Source38 breadth lane follow-up 4 was resumed beside it (its brief now says the four-peer counters are informational, the measured lobby re-run belongs to Source39). Source39 is staged: worktree `D:\Projects\integration-39` on `stage2/integration-39` from `32312c71e`, with lane `integration-39` composing `stage2/h4-phase-a` (A6), `stage2/lobby-grace` (conflicts with A6 in the lockstep round and relay, composed by the lane), `stage2/h4-phase-b` (B1) and `stage2/registry-audit` (provisional, pending the `setaside-review` verdict), DONE and green (`lanes/integration-39/report.md`): merges `b387bbc65` (A6), `546888d6c` (lobby), `68d0771c2` (B1), `3a49af31d` (registry audit), tip `3a49af31d`, 32 files +3716/-44; the only conflict was lobby-grace's, two hunks in `NetLockstep.h` (both new predicates kept) and `NetLockstepSelfTest.cpp` (all three new fixtures kept, 25 cases defined and dispatched), every added line of all four branches present and none of the removed lines back; the real interaction is in the auto-merged `NetLockstep.cpp`, where A6's refused host frame now rides lobby-grace's instrumented backlog (`relay_bytes_sent` includes retried host frames, section 3.1); measured on the worktree binaries `869c13db` (GNS) and `0d5634cb` (no GNS): eleven socket-free selftests 11/11 on both (`-camera-null-scene-selftest` in place of discovery, which needs the approved path's firewall rule), native 529/0 on both, restoration 13/13, fuzzes 17/17 and 10/10, SimBaseline 0 of 600 tick hashes differ against main over three runs and both configurations; the registry merge is WITHDRAWN from Source39: the independent review (`lanes/setaside-review/report.md`, 584 lines, `evidence/measurements.json`) confirms the hole and the diagnosis on its own builds but finds the repair's rule wrong and blocking - `UnregisterObject`/`UnregisterMO` are not destruction choke points, `ReleaseScriptOwnedTree` (`LuaMan.cpp:3455`) detaches live Lua-owned objects through them while the world is held, so the reinstated world loses live registrations (probe selftest 1/1 on the base, 0/0 on the tip; an unchanged Lua fixture reads `FindObjectByUniqueID` `findable=false` after a real rollback on the tip), which is script-visible and engages the binding compatibility rule; two smaller items (the withdraw at the top of `ReinstateWorld` instead of where the record is consumed, a throw after publication leaving a dead frame in `m_HeldRegistries`); `80f6d9095` and `352049777` stand; the reviewer's demonstrated correction `2079eaf3` (forget on destruction via `~MovableObject`, withdraw where consumed and on a throw) passes everything. The audit lane landed the correct rule (`lanes/world-setaside-audit/report.md` `## Follow-up 1`): four new commits `3d434f86a` (a held copy forgets an object when it is destroyed, from `~MovableObject` through `MovableMan::ForgetDestroyedObject` and `LuaMan::ForgetDestroyedRegisteredMO`, never on unregistration), `30c33c3a2` (withdraw where the record is consumed), `bb115d777` (withdraw on a throw), `30a87aac3` (`SwapAndHoldRegisteredMOs` closes the publication gap by construction); tip `30a87aac3`; the reviewer's probe is the native check `reinstate_keeps_a_detached_live_owner` and the rollback fixture is `tools/fixtures/mod_setaside_park.lua`, both red on `352049777` plus the checks (`2657acfb`) and green on the tip (`cffcbb8a`: native 525/0, `findable=true`), restoration 13/13, fuzzes, SimBaseline identical, both configurations; the natural sweep inside a hold stays an open fixture shape (F1.7). The reviewer's re-check (`lanes/setaside-review/report.md` `## Follow-up 1`, runs `D:\mx\wsr2`) calls `30a87aac3` READY TO INTEGRATE: every enumerated path answers on the reviewer's own builds (destroy during the hold, detach, re-identification, threaded states, destroy inside `ReinstateWorld`'s body flipping red on `352049777` to green, a throw after publication under an injected fault), the swap/hold gap closed by construction, the battery reproduced exactly (525/0 both configurations, ten selftests, restoration 13/13, fuzzes, SimBaseline identical), the fixture base true / broken false / tip true; one residual (an object that took a new identity during the hold keeps its old key in the held record, unreached everywhere and pre-existing) is closed by erasing by address, which the reviewer built and measured as `52287682`; the audit lane committed that closure as `7832dee37` (forget by address; the identity capture in `~MovableObject` became dead code and was dropped), shown red on `30a87aac3` plus the check (`83a214a8`: 524/2) and green on the tip (`54848449`: 526/0, GNS-disabled `c7149814` 526/0), ten selftests, restoration 13/13, fuzzes, SimBaseline identical, the fixture `findable=true` (committed blob LF `f93639da`, checkout CRLF under `core.autocrlf=input`); `stage2/registry-audit` is FINAL at `7832dee37` and merges into the Source39 integration together with the reviewed sound-wire tip; if both finish before the matrix releases the executable the integration lane re-merges it, otherwise Source39 is built from `68d0771c2` (A6 + lobby + B1, the tree before the registry merge) and the repair joins the family after; the Mac builds the exact export `combined-source-i39` after the A6 and B1 branch tips (`chain_mac_i39.zsh`); the Mac is building the A6 and B1 branch tips from their exact exports (`combined-source-h4a6`, `combined-source-h4b1`, `chain_mac_h4a6_h4b1.zsh`, selftests follow). After the matrix: read every NEEDS_REVIEW job, then `chain_build_gate.py --label source39-1 --source 39` on the integrated tree (merge `stage2/integration-39` into main first), `run_remaining_gates.py --source 39`, a breadth follow-up 5 with A6's flips (its section A6.5), B1's `tools/h4_substitution_gates.py` gates, the lobby lanes alone with `largest_relay_packet_bytes` (lobby report section 7) deciding the byte-rate question, the Mac chain, and the Phase B moderation UI (slice B2) plus the headed section 11 review when the user consents. Was running at the original save point (superseded by the lines above): (1) `run_source38_family.py` (contract-audit) at its breadth stage: the breadth lane follow-up 4 (`lanes/source33-lanes/followup-4.md`, session `948243b2-2969-4625-a20b-fb782ef300fb`, runs under `D:\mx\s38lanes`) and then the full matrix from `D:\mx\s38`; if the breadth lane died, resume it with `python launch_worker.py source33-lanes --resume 948243b2-2969-4625-a20b-fb782ef300fb --prompt lanes/resume-after-limit.md --cwd D:/Projects`, and if the matrix never started run `python run_source38_family.py --from matrix` (stages: `remaining`, `breadth`, `matrix`; `D:\mx\s38` must not exist or is moved aside; nothing else may use the approved executable while it runs). (2) Four Opus 5 max lanes on the Source38 tree, resumable with `python launch_worker.py <lane> --resume <session> --prompt lanes/resume-after-limit.md --cwd <worktree>`: `h4-a4-semantics` session `8c31d315-8430-4b22-b619-68642672a3df` in `D:\Projects\h4-phase-a` (slice A6; brief `followup-2.md`) - FINISHED at the save point: five per-concern commits `5701edc19`, `50c3b3309`, `5a46aee91`, `5f718834c`, `fed9dc3bd` on `stage2/h4-phase-a` (a resumed match is judged only once it has committed a frame, a held seat's units play instead of standing down, the admission plane is serviced while the round waits, a superseded incarnation's transport is dropped from the round, the match ends cleanly when the last peer announces its leave), worktree clean, eleven selftests on both configurations with `-net-lockstep-selftest` at twenty-four cases, SimBaseline unchanged; its report section A6.5 names the gate commands and the checks that must flip on the next family, and A6.6 records that the stand-down killing the brain is an inference the `reclaim_socket` re-run settles; `lobby-grace` session `46853b03-2aaa-41e5-a8d4-8019ed1dfcf3` in `D:\Projects\lobby-grace` (brief `followup-1.md`) - FINISHED after the resume: three per-concern commits `2934952e1` (a lobby ignores a roster peer it has no seat for yet: `NetLobbySession.cpp:627-629` failed a client's whole session on it, the cause of the 4-peer/100 ms control; control build `17bf9e9d` reproduces the production string), `7c6b9bea3` (`GnsTransport::Disconnect` now emits the local `PeerDisconnected` and hands the peer to the admission plane, which `LoopbackTransport` already did, the cause of the leaked seat in `lobby_4_drop_200`; control build `f222e1b4` reproduces it once loopback behaves like GNS), `3d8d285d2` (a relayed frame's cost is measured, `largest_relay_packet_bytes`, and the host hands its relay backlog over before quitting instead of a blind 1.5 s sleep); worktree clean, ten socket-free suites on the committed tree `7e1f4f25`, SimBaseline identical. EResult 25 is `k_EResultLimitExceeded` against a `SendBufferSize` already at 8 MB: the host genuinely queues ~8 MB to one client, dominated by the 44-byte sound observations (up to 512 per frame, 22.5 KB) that the committed-audibility contract sends every tick; no byte-rate change was made (the contract is the binding policy and needs the two-process gates), the lane's section 7 turns one measured run into the decision. Two corrections to the Source37 reading: the heartbeat-timeout line appears in passing lanes too, and `lobby_4_rejoin_200`'s peers reached tick 180 and were killed by the host's exit while it still held one peer's forwards; `h4-b1-substitution` session `b1d797bd-3b7e-4951-b6dd-36e6d2dd8e84` in `D:\Projects\h4-phase-b` (H4 Phase B slice B1; brief `prompt.md`) - FINISHED after the resume: six per-concern commits `2094d0fbd`, `59a7c4587`, `b20d710d2`, `02f549493`, `6f0ec73d4`, `673b39743` on `stage2/h4-phase-b` (the Phase-B admission messages on the unchanged wire, a seat credential reserved without being installed so the first COMMIT wins, precise rejection only after the superseded credential is proved, applications with the P15 bounds handed over as one transaction, the host's wait/substitute/cancel verbs and moderation view on `NetMatchService`, the four two-process gates in `tools/h4_substitution_gates.py`), worktree clean, ten selftests on both configurations, SimBaseline identical to A6 on the same base, nine negative-control builds; it also found and fixed a latent Phase-A defect (`NetProtocol::ReadRejectReason` never accepted `SessionEnded = 16`, so that `JoinRejected` was dropped by its own decoder; the selftest now round-trips every reason); pins P27, P30, P31, P32 are confirmed by the lead in the plan's second confirmation line (20:20 UTC); the gates (report section 7, ports 44500-44530) run on the next family; the moderation UI is slice B2; the Mac has not built this branch; `world-setaside-audit` session `178e6299-f2e4-4d6e-b56c-7e95b4b75aa1` in `D:\Projects\registry-audit` (brief `prompt.md`) - FINISHED at the save point with a real hole: a set-aside world's `knownObjects` copy and the swapped script update lists keep pointers to script-owned objects that nothing stops from being destroyed while the world is held, so `DiscardWorld` calls `DiscardScriptState()` on freed memory and `ReinstateWorld` puts the freed pointer back into the live registry; four commits `2651eb905`, `c96995740`, `80f6d9095`, `352049777` on `stage2/registry-audit` (registry copy and script update lists forget a destroyed object, the held sound registry's live-owner check pinned by a selftest, a script-state cursor guard), build `1439f89d` green on native 524/0 with two new checks, ten selftests in both configurations, restoration 13/13, both fuzzes, SimBaseline identical, and the control build red on exactly the new checks; the lane flags that it has no naturally occurring reproduction (its report section 8.1) and that its own second attempt passed every gate while wrong, so the repair gets an independent Opus review before it joins the next family. Each lane appends to its `report.md`; `lanes/registry.json` records launches and results.

Next in order: read the breadth lane's `## Source38` and the matrix (`D:\mx\s38\matrix.json`; finished jobs read NEEDS_REVIEW with their return code and must be reviewed one by one, never called green by count), mirror the Mac Source38 results into `mac-peer-20260907/results-source38/`, then integrate the lanes' commits (A6, lobby, B1, audit) as the next family through `chain_build_gate.py --label source39-1 --source 39` plus `run_remaining_gates.py --source 39`, a breadth follow-up and the Mac chain; the headed §11 review needs the user's consent for two visible game windows on the interactive desktop; then RESUME §B-1 items 5b, 6c Phase B, 7 session UX, 8 discovery, 4b-4d/8d measured feel. Rules unchanged: Opus 5 max for every worker, no push or PR without explicit approval, no attribution, never delete or kill, the binding mod-compatibility section applies to every change.

**2026-09-08 second checkpoint (~08:45 UTC, commits landed ~09:35 UTC): HEAD `bb9207773`, 200 ahead and unpushed. The Source32 tree is committed as eleven per-concern local commits split from the verified working source by the `reviews/claude-review-2026-09-08/lanes/commit-plan` lane (patches proven byte-exact against the tree before use): `bb728e006` carries the pre-existing Lua/native object graph checkpoint generation, `f5f0c15ed` the logical sound lifecycle, `6083dab12` the scoped-draw unsigned negation, `511a43e87` the selftest progress echo, `1581cb6e8` restore-side stitch collapsing, `b89614a3d` lockstep rounds and start retransmission, `0821d77d3` the audibility observation transport, `90498a1b2` local scopes kept out of the shared key sequence, `4a364ed87` the scope/audibility diagnostics, `96c352090` script initialization before the AI pass, `bb9207773` logical playback on every bus plus `Pos` by value; only the two rebuilt vendor `.lib` binaries stay uncommitted (build output), and the untracked parked WIP (`primitive_checkpoint_pending.patch`, `primitive_pending/`, `gui_checkpoint_selftest.cpp.txt`) moved out of the tree to `native-fidelity-work/parked-20260908-tools-contracts/`. Follow-ups recorded by that lane: the two `[audibility]` console prints in `4a364ed87` are ungated and the miss set is unbounded (gate them behind `CC_TRACE_SOUND_SCOPE_OBJECT` or a dedicated switch in the next source family, not on the frozen build), `bb728e006` is one large generation commit that must be decomposed before anything goes upstream, and `SoundContainer.Pos` by value is a mod-visible contract change (`sound.Pos.X = n` no longer mutates) that the user should see. Before the commits the tree carried the uncommitted Source23–Source32 audio/lockstep work (`AudioMan.*`, `SoundContainer.*`, `SoundSet.*`, `LogicalSound.h`, `SoundSimulation.h`, `MovableMan.cpp`, `MovableObject.cpp`, `ActivityMan.cpp`, `Main.cpp`, `NetLockstep.*`, `NetLockstepSelfTest.cpp`, `NetMatchReplay.*`, `NetMatchRunner.cpp`, `ScenarioRunner.*`, `LuaMan.cpp`, `LuaBindingsEntities.cpp`, `GUISound.cpp`, `MusicMan.cpp`) on top of the pre-existing uncommitted graph/native work.** Delegation now follows `reviews/claude-review-2026-09-08/CLAUDE_STEERING_PROMPT.md`: this Windows session is the only Fable lead; every worker is `claude-opus-5` at `max` (`lanes/launch_worker.py`, registry `lanes/registry.json`, definitions in `.claude/agents/`, env force + `Agent(fork)` deny in `.claude/settings.json`; the definitions load at the next session start, so this session launches explicit CLI workers). Evidence since the 07:00 paragraph, all under `reviews/recovery-2026-09-07/contract-audit/` unless noted: **Source25** (Mac `bdadc086`) collapses stitch frames on restore (`_ScriptGraph.canonicalThread`, refuses malformed continuations; `_ScriptGraphProgress` echo). **Source26** (Windows `77d9c536`, Mac `d8861d25`) fixes the heal race: lockstep rounds (`roundId` on Start/Frame/Checksum, codec v11), Start retransmission every 250 ms, pre-start frame buffering, stale-round filtering; heal gate `reviews/recovery-2026-09-06/expanded-mod-gates/20260908_065147_heal_7d27f107` PASS, `NetLockstepSelfTest` adds `TestCoordinatorFrameBeforeStart` and `TestCoordinatorIgnoresStaleRound`. **Source27** (Windows `e1d36d8a`, Mac `7771b764`): nine engine selftests `selftests-source27` pass, native suite `audibility-native-1` passes, but the two-peer actual-volume gate `sound-volume-authority-source27-match-1` was RED on both platforms (72 changed ticks from tick 61). **Source28/29** (Windows `42f842e9`, Mac `c6fa23b4`): local (AI) scopes no longer renumber their parent's shared sequence (`local_scopes_keep_shared_keys`), `[audibility]` first-reading/miss diagnostics, a stale Start for another frame is ignored before the round is known; the diagnostics proved both peers committed identical tables while the client's container for the host's actor carried a different key. **Source30** (`3bcf26aa`, `CC_TRACE_SOUND_SCOPE_OBJECT=<uid>` scope/script-call trace, `sound-volume-authority-source30-trace-1`) pinned the cause: the owning peer's threaded AI pass initialized the actor's scripts on a worker thread before its first Update, so `Create` and every later sibling scope sat in a different place on the two peers. **Source31** (`bd9da823`): AI passes skip actors whose scripts are not yet initialized (scripts now initialize in the first Update stage on every peer), `WhilePieMenuOpen` is a local hook; `sound-volume-authority-source31-match-1` shared trace matches 600/600 with identical readings on both peers following the settled policy (0.25 while the host controls its actor, 0.75 from the transfer at step 240, 0.25 again from 420) except at the transfer ticks themselves, where the new controller's reading had not been sent yet; the negative control `sound-volume-authority-source31-fault-1` (`CC_FAULT_INJECT=local_audibility`) diverges 72 ticks from tick 61 as required. **Source32 (Windows `65730f25`, `grouped-build-65730f25`, export `combined-source-32`):** every peer reports every live shared sound so the next controller's reading is already committed when control moves; UI-bus sounds played from simulation are logical (audit `reviews/claude-review-2026-09-08/lanes/ui-bus-liveness/report.md`: the exclusion let a Lua `BusRouting = UI` route eleven engine `IsBeingPlayed` readers onto physical playback), `SoundContainer.Pos` was bound by value in `bb9207773` (a reference could name shared state on one peer and the local copy on another) — **that is a mod-compatibility REGRESSION, not an accepted contract**: the user's binding correction of 2026-09-08 (`reviews/claude-review-2026-09-08/MOD_COMPATIBILITY_STEERING_PROMPT.md`) requires `sound.Pos.X = value` and retained aliases (`local p = sound.Pos; p.X = value`) to keep their established behaviour while determinism is preserved; lane `lanes/pos-alias-repair` (worktree `D:/Projects/pos-alias-repair`, branch `stage2/pos-alias-repair` from `4ec709e83`) owns the engine repair and the unchanged Lua fixtures run on the pre-change reference (`grouped-build-bd9da823`, Source31), the broken build and the repaired build, and lane `lanes/mod-compat-audit` audits every other mod-facing change of this generation, selftest `ui_bus_in_simulation_is_logical`. Source32 evidence (`chain-source32-1.json`): nine engine selftests `selftests-source32` pass; `sound-volume-authority-source32-match-1` passes every check including `authority_values_follow_team_owner` (identical readings on both peers, 0.25 under host control, 0.75 from the transfer at 240, 0.25 from 420, shared trace 600/600); `sound-volume-authority-source32-fault-1` (`CC_FAULT_INJECT=local_audibility`) diverges 72 ticks as required; `sound-query-wounds-source32-1`, `sound-query-rng_particles-source32-1` and `sound-query-ui_bus-source32-1` match 600/600. Mac Source32 (`e4cc9501`, built from the identical export with zero mismatches, amended runner): native suite pass with 0 FAIL, authority match with identical controlling-player readings, sound-query `wounds`, `rng_particles` and `ui_bus` match 600/600 (`audit-20260907-evidence/chain-source32.json`), and the Mac `ui_bus` negative on Source28 diverges 475 ticks from tick 31 exactly like Windows Source31 (`lanes/mac-native-triage/report.md` Follow-up 2, which also scoped the runner allowlist to the provoking selftests with a gameplay-shaped control kept red). The rest of the Windows family on `65730f25` (`chain-source32-remaining.json`): native suite `native-source32-1` 501 PASS / 0 FAIL, two-peer heal `reviews/recovery-2026-09-06/expanded-mod-gates/20260908_081424_heal_e84c3d2f` 23/23 checks with one resync and 600 ticks, prediction invariance `20260908_081448_invariance_feac445d` (reference, clean, Lua-fault) PASS, restoration `restore-source32-1/20260908_081512_114cd88b` 13/13 (memory/file/launch at 50/60/100/400 with the late global), and the ordinary-load transaction gate `full-runtime-transaction-valid-32` complete with `continuation_checks_passed`; its continued-state raw comparison (`ordinary-load_valid_full_100/continuation-raw-classification.json`, `classify_continuation_raw.py` on the `continued.state` dumps) now has 8 differences, all `RENDER_SCRATCH` MOSRotating temp-bitmap hashes, so the two `HDFirearm::m_FireSound` positions that stayed OPEN on Source22 are closed by the logical-audio build with direct evidence (no mask was widened; the classifier still treats any playback-gated field as OPEN). The UI-bus fixture (`mac-peer-20260907/ui_bus_liveness.lua`, mode `ui_bus` of `run_sound_query_gate.py`, lane `reviews/claude-review-2026-09-08/lanes/ui-bus-fixture`) is a demonstrated negative on Source31 (`sound-query-ui_bus-source31-negative-1`: 475 changed ticks from tick 31, the `ui_audible` arm differs on 192/192 samples with host 0.25 / client 0.75 while the identical SFX control agrees on 192/192); its `ui_playing`/`ui_held` arm never crossed a liveness edge because the sample (132 ticks) outlives the 90-tick replay interval, so that arm is untested rather than passing. The same lane repaired `run_sound_query_gate.py`, which had not compiled since 2026-09-07 23:23 (a literal newline in the console-dedup edit; pre-edit copy retained beside the report). Observation for the gates: the fixture's per-object Lua accumulator differed on every sample while the `lua_state` tick hash never changed, so per-object script state reaches the tick trace only through native effects. Lane `lanes/lua-hash-coverage/report.md` established why: the `lua_state` subsystem hashes only the master VM's mt19937 state (`LuaMan.cpp` `HashAllLuaStatesIntoSimChecksum`, one feed site), no globals, no `_ScriptedObjects` per-instance tables, no worker states, and the master RNG never draws in these runs, so `lua_state` took exactly one value per run across all 26 retained two-peer traces; the `cccp-ctl.cpp` description "All Lua states' RNG + key tables" is stale. Per-object Lua divergence is therefore caught only by the full five-VM graph comparisons of the checkpoint, heal, restoration and transaction gates, not by live tick hashes; a per-object tick hash is not a safe extension because AI-hook fields on `self` are per-peer by design, so the fixtures keep driving native particles from every reading. The stale description is a follow-up for the next source family, not a Source32 change. **Mac:** Source28 sound-query `wounds` and `rng_particles` match 600/600 (`audit-20260907-evidence/sound-query-*-source28*`); the Mac native suite's `pass:false` for Source23–28 is explained in `lanes/mac-native-triage/report.md`: Source23/24 were the real arm64 SIGSEGV (fixed by Source25), Source25 the codec metamethod error (fixed by Source26), Source26–28 exit 0 with 501 PASS and zero FAIL and fail only because the runner gates on three deliberate negative-control `ERROR:` lines that Windows prints identically; the runner is being amended (allowlist of those exact lines, backward.hpp crash signature, non-PASS selftest lines) with Source24/25 kept red as controls. **Checklist reconciliation:** `reviews/claude-review-2026-09-08/lanes/b1-inventory/report.md` walks every §B-1 sub-item against the retained evidence (11 DONE, 6 PARTIAL, 8 OPEN, 2 BLOCKED in part; no `[x]` mark needs downgrading): 2d's audio caveat is discharged by the Source32 classification, 2c's member counts are stale in §B-1 (later §B checkpoints record 877–893/0), item 3a is NOT covered by the running matrix because the invariance driver runs at tick 143 while 3a needs 300, so lane `lanes/invariance-300` ran a tick-300 copy of the driver on the frozen build: `lanes/invariance-300/evidence/20260908_083844_invariance_6afc05cf` (and a repeat `20260908_083942_invariance_020da7ad`) pass 8/8 cases with the Lua-fault control firing on all eight, `source_unchanged` true, executable equal to the pin; the caveat is that `pickup_fire.ccreplay` has no player action after tick 161, so at 300 only the already-armed arm is exercised (a recording with actions after 300 is a fixture gap, tracked under item 3b's battery lane), three of 5a's five sub-gates (16-case interp matrix, GNS-disabled build, rollback fuzz) and a 200 ms fake-lag duel have no matrix job, 5b's co-op/PvPvE/replay legs and every item in 6–8 sit outside the matrix, and its proposed lane order after the matrix is L1 triage, L2 tick-300 invariance, L3 semantic gameplay battery, L4 breadth debt, L6 cross-platform (Mac Source32 results are now mirrored at `mac-peer-20260907/results-source32/`), then L7 H4 (`lanes/h4-plan-pins` is pinning the §4/§5/§7 values), L8 session UX, L9 discovery, L10 feel last. **Lanes on the frozen build (2026-09-08 ~10:30 UTC):** the retained semantic gameplay battery is green on Source32, 10/10 lanes with the six semantic detail lines character-identical to the 2026-09-06 baseline `gameplay_20260906_071715_45788` and the last 2026-09-06 red (`fire_reload` `aimspeed` subnormal on the AI-driven dummy) now byte-identical between peers with no tolerance widened (`reviews/claude-review-2026-09-08/lanes/semantic-battery/report.md`, runs under `runs/`); that lane ran only through a copy of `run_interp_e2e.ps1` because the stock harness kills every `Cortex Command` process and launches from the repo directory. Startup from the repo directory crashes on Source32 (`FATAL: unhandled exception 0xE06D7363`, exit `0xC0000005`, any mode): the stack symbolized from `AbortCode.txt` against the PDB is `std::filesystem::filesystem_error` thrown inside `System::PathExistsCaseSensitive` (`System.cpp:153`) from `System::Initialize` (`System.cpp:105`), whose recursive walk of the whole working directory (`s_CaseSensitive` defaults to true) hit `p4b-interp-validation/reviews/recovery-2026-09-06/`, a stray 2026-09-07 gib-target lane output written relative to the repo cwd with 175,949 files and long paths; it is an environment regression, not a Source23–32 change (that tree appeared 2026-09-06 22:04, after the last green repo-cwd battery run), every retained gate uses private runtimes, and the tree was moved to `reviews/recovery-2026-09-06/_in-repo-stray-20260907/`, after which a headless `-scenario SimBaseline` launched with the repo as cwd boots and passes on the frozen executable (`lanes/semantic-battery/evidence/repo-cwd-boot-after-move/`, before/after pair retained); a defensive follow-up for the next source family is to iterate that walk with `error_code` and `skip_permission_denied` so a long-path or unreadable entry cannot abort startup. **H4 Phase A, slice A1** (`reviews/claude-review-2026-09-08/lanes/h4-a1-protocol/report.md`, isolated worktree `D:\Projects\h4-phase-a`, branch `stage2/h4-phase-a` from `bb9207773`, eight commits `7f76216e2`…`026a40940`): the §4 admission messages ride the session wire under the unchanged header version (P6) with pinned sizes and canonical-byte selftests, proofs bind to the §5 canonical transcript (HMAC-SHA-256 known answers, cross-domain refusals, fail-closed provider/RNG faults, a secret-canary scan of the engine's own console output), the txId cache replays a duplicate transaction's terminal result (P1/P17), every failed reclaim releases on one uniform injected-clock schedule (P11/P12/P16/P23), and the unbounded `PeerState` growth found by the pinning lane is closed at eight half-open connections (P14); all nine socket-free selftests pass on the GNS build `cf4c6f68` and the GNS-disabled build compiles and fails closed; the txId stays out of the signed transcript (the plan's §5 schema governs; the challenge store binds it), `SessionEnded = 16`, the ticket-file canary allowlist, the fencing logic and the two-peer §9a gates are A2/A3 work, and `-net-discovery-selftest` (UDP broadcast socket) plus every socket gate must run on the approved path after the matrix. **Slice A2** (`lanes/h4-a2-session/report.md`, eleven commits `35db8c80f`…`73e16e1f5` on the same branch): the admission transaction runs as a session state machine (`NetReconnectSession`) with P2's provisional seat, reclaim/challenge/proof through the A1 auth core, the leave protocol and `SessionEnded = 16`, single-active-incarnation fencing (the old transport's delayed packets are counted and dropped, its later timeout returns `Fenced`), the durable one-record ticket store with atomic replace, 24 h bound and the file-artifact canary allowlist, the drop-time ownership ledger with per-mode restoration and `NetGameReseat` as a system-authored lockstep command (lockstep codec 11 → 12, canonical header re-pinned, host-sender gate in `MovableMan`), and lobby admission isolation; all ten socket-free selftests pass on the GNS build `8a611c5a` and on the GNS-disabled build, and the SimBaseline hash is byte-identical to A1's, so the codec bump leaves the sim untouched. The A2 tip (`73e16e1f5`) also builds on the Mac from the exact export (`combined-source-h4a2`, binary `5d431fbb`, zero source mismatches) and passes the same ten socket-free selftests there (`audit-20260907-evidence/selftests-h4a2-result.json`, runner `run_mac_selftests.py`). **Slice A3** (`lanes/h4-a3-live/report.md`, seven commits `b8b558c33`…`0fe0b92f6`, tip built as `c527eb2f`): the admission plane runs on the live match service with `Ready` held until `JoinCommitted` and the reclaim ladder budget asserted and measured at 200 ms; the production drop-time census is a sim-thread-only `MovableMan` walk handed to the host and the reseat is enqueued through `ScenarioRunner::EnqueueLocalGameCommand`; §11 is a clock-injected UI state machine wired into the menu (auto-retry, crash/relaunch offer of the stored ticket, roster indication); §10's old-wire probe has both branches built and gated; a seam the live wiring exposed (a retransmitted `Reclaim` was rate-limited by P16 after the reclaim had committed) is closed from P1's replay rule with no pin changed; all ten socket-free selftests pass on both configurations and the SimBaseline hash stays byte-identical to A1/A2. Eight two-peer §9a gate drivers wait under `lanes/h4-a3-live/gates/` (`run_all.py`) for the approved path, and §11 still needs a headed check. The A3 tip also builds on the Mac from the exact export (`combined-source-h4a3`, binary `fc7905ed`, zero mismatches) and passes the same ten socket-free selftests there (`selftests-h4a3-result.json`). Lead decisions on A3's judgement calls: a denied reclaim falling back to one fresh join stays; a retransmitted `Reclaim` is dropped rather than re-challenged (no enumeration oracle); and the new rule that a ticketless mid-match joiner is refused (`-net-no-reconnect-admission` restores the old handshake for bisects only) must be confirmed by the two-peer gates, including that the lobby rejoin lanes of the matrix relaunch a peer with its own `Userdata` ticket rather than a fresh runtime. Next for H4: merge `stage2/h4-phase-a` (26 commits) into the main line after the matrix, run the eight gate drivers on the approved path, and the headed §11 review on the private desktop. **Breadth debt on the frozen build** (`reviews/claude-review-2026-09-08/lanes/breadth-debt/report.md`): the 16-case interp matrix is 15/16 with every sim-gated case identical host/client over 600/900 ticks and both positive controls firing; the one failure, `mismatch`, is a stale assertion in `run_interp_e2e.ps1` (it expects the rejection reason in the host's `setup_error`, which since `b0531ada7`/`d227a1a85` only happens under `-net-match-e2e-join-rejection` because the host deliberately keeps listening; the same case passes with that flag) and is a harness fix for after the matrix, not an engine defect. The fake-lag duel passes at 200 ms and at the 100 ms control. **RED:** the rollback fidelity fuzz on `buy_900.ccreplay` (`-rollback-fidelity-fuzz 1:17:30`) passes captures 107–305 and fails deterministically at capture 338 with `[audio-checkpoint] voice 40 has no registered owner 9293` thrown from `AudioMan::LoadCheckpoint` (`AudioMan.cpp` ~1442) out of `ActivityMan::OnCommit` (`ActivityMan.cpp` ~1088); the pickup fuzz passes 10/10; a 25-run bisect over fourteen retained executables shows every build since Source2 (2026-09-07 14:31) fails that probe and the last passing binary is `ef82285a` (09:47), so the cause is in the checkpoint-generation work now committed as `bb728e006`, not in the Source23–32 audio work (the message took its audio shape at Source19), and it is specific to the in-memory rollback restore (save/load, heal and the transaction gate are green on the same executable); lane `lanes/rollback-audio-owner` (isolated worktree `D:\Projects\rollback-audio-fix`, branch `stage2/rollback-audio-owner` from `bb9207773`) found and fixed it: identity 9293 is a `Flesh Giblet Impact` SoundContainer created from Lua (`GibletSounds.lua`) that had lost its last Lua reference and was swept by the LuaJIT collector inside the probe window, so the capture named an owner no restore could produce (the throw is in `MovableMan::ReinstateWorld` putting the tick-368 originals back, not in the restore to 338); three commits (`52a5561af` settle the Lua heap to a fixed point in `ActivityMan::CaptureRuntimeGlobals` via `LuaMan::CollectGarbageForCheckpoint`, `bf888e981` release the `_ScriptGraph` stash when a set-aside world is reinstated, `b972ac36b` clamp a captured voice position inside its sample) make the buy fuzz 17/17 and the pickup fuzz 10/10 byte-identical, probes 272/305/338/371 pass, the native suite passes 502/0 with the new `checkpoint_settles_script_owned_sound` regression (fails without the fix, strict owner refusal intact), and restoration is 13/13 on the fixed binary `f397f8ec`; the heal, invariance and transaction gates must re-run on the integrated build because the capture path changed, and the Mac has not built this branch yet. **Matrix triage:** the first `FAILED_OR_INVALID` job of `full-matrix-source32-1`, `failure_refusals`, is a harness defect, not an engine one (`reviews/claude-review-2026-09-08/lanes/matrix-refusals-triage/report.md`, verdict (b) with high confidence): in refusals mode the child compares every refused-load continuation against the default `observe` reference with `compare_graphs` projecting `shared=False`, so real-clock anchors (`Timer.m_StartRealTime` in 3,478 of 5,371,300 raw fields, `clock.real_time`, `clock.sim_accumulator`) differ between two OS processes while VMs 0/2/3 are byte-identical and every VM1/VM4 difference is that one scalar; zero sim fields differ and the valid/staged siblings pass because their references come from the same process pair. Closure is a driver correction in `full_collected_matrix.py` (mask real-clock anchors for cross-process refusal comparisons or pass the proper `--reference-operation`), to land before the next matrix run; the running matrix's own copy must not be edited. The same lane's follow-up classified `failure_staged-ordinary` as an engine defect with an environmental trigger (ten launches died at startup in `ContentFile::GetAsBitmap`, `ContentFile.cpp` ~416, dereferencing a null `IMG_Load` result; the lead then pinned the trigger to sprite paths under the matrix's deep output root exceeding Windows MAX_PATH, the same cause as the eight multiplayer reds; Source20 never ran that job) and `failure_unknown-property` as a contract decision the check pre-judged (the restore reader is built with `SetThrowOnError(true)`, so an unknown INI property refuses the whole load while `CONTRACTS.md` expects compatibility acceptance); the lead's decision is that an unknown property name inside a well-formed object is tolerated with one warning and structural errors keep refusing. Both fixes are the polish lane's follow-up (`lanes/polish-33/followup-1.md`). `archive_loads` is harness/seed wiring (very high confidence): the matrix seeds `test_game_loads.py` with the `failure_seed` archive, whose `Save.ini` line 348 copies the `UserScenes.rte/Checkpoint Global` global-script preset that only `run_audit.py` materialises, so all 16 cases fail at the staging load with `Couldn't find the preset`; the engine behaves correctly and the driver must create the same fixture module (a `tools/test_game_loads.py` change for after the matrix). Three later reds (`lobby_4_rejoin_200`, `lobby_4_drop_200`, `mp_d3_deliver_cargo`) turned out to be the MAX_PATH class described below. **Source33 polish** (`lanes/polish-33/report.md`, branch `stage2/polish-33`, four commits `174726fe9`…`f1b9019af`): the `[audibility]` prints sit behind `CC_TRACE_AUDIBILITY` and the miss set clears with the table; `System::PathExistsCaseSensitive` walks with `error_code` and `skip_permission_denied` so an over-long or unreadable entry loses only its own subtree (five `[path-case-selftest]` cases with a >300-character chain and a deny-DACL directory; the frozen Source32 executable still dies on that shape while the polish build boots), the `lua_state` description in `cccp-ctl.cpp` says what it hashes, `MovableObject.cpp` has its header first again; the stock `run_interp_e2e.ps1` no longer kills foreign engine processes, launches from private runtimes and asserts the current mismatch contract (client `Failed` with the reason, host `service.error` set and still listening), with the `mismatch` case re-run green on the frozen executable. The polish follow-up adds `c463895d8` (`ContentFile::GetAsBitmap` and `LoadAndReleaseBitmap` abort with the file name and the SDL_image error instead of dereferencing a null `IMG_Load` result; five `[content-file-selftest]` cases, and a same-input control build without the guard still access-violates at `ContentFile.cpp:417`) and `fe23532b0` (an unknown property name in a checkpoint reader is skipped with one warning naming object, property, file and line through the new `Reader::ReportUnknownProperty`, while every other reader and every structural error keep throwing; four `[reader-selftest]` cases prove parsing continues past the skip, and a control build without the change refuses the same input), so `stage2/polish-33` carries six commits. **Full collected matrix, first run (`full-matrix-source32-1`, executable `65730f25`, 104 jobs, finished 2026-09-08 ~12:40 UTC, status `COLLECTED_REQUIRES_GATE_REVIEW`):** 91 jobs returned 0 and sit at `NEEDS_REVIEW` (the driver's rule that a return code is not a fidelity verdict), 12 are `FAILED_OR_INVALID` and `peer_roundtrip` is blocked on one of them. The twelve: `failure_refusals` (harness real-clock comparison, triaged), `failure_staged-ordinary` (startup null-deref in `ContentFile::GetAsBitmap` when an image path under the matrix's deep output root exceeded Windows MAX_PATH; engine null-check in the polish follow-up, matrix re-run from a short root), `failure_unknown-property` (contract decision, fix in the polish follow-up), `archive_loads` (harness seed wiring, patch in preparation), six multiplayer jobs (`mp_d3_deliver_cargo`, `mp_d3_stall_repeat`, `mp_fakelag100_auto`, `mp_perturb_control`, `mp_mismatch_control`, `mp_snapshot_p5`) whose e2e setup reported `unreadable module file` for `Data/Browncoats.rte/.../RefineryAuthorizationConsolePieSliceActions.lua` because job names long enough push that junctioned path past 260 characters (263–265 versus 249 for the passing siblings; the matrix output root's depth, not the engine), and the two 4-peer 200 ms lobby lanes `lobby_4_rejoin_200` and `lobby_4_drop_200`, which are a real lockstep problem (`lanes/matrix-lobby4-triage/report.md`): the replacement peer stalls on `MissingFrameTimeout` at tick 145 and never recovers while the 3-peer/200 ms and 4-peer/100 ms lanes run 180 ticks with zero stalls, and a confirmed secondary defect is that peer-drop adjudication is host-only and slow (35–43 s) while a client's grace is a 20,000-poll budget rather than wall time (`ScenarioRunner.cpp` ~1073–1080), so healthy survivors give up before the host tells them a peer left; lane `lanes/lobby-grace` (worktree `D:/Projects/lobby-grace`, branch `stage2/lobby-grace` from `cf9dcda67`) confirmed and fixed the grace defect in three commits (`ea68bf1e5` one monotonic wall-clock grace for both lockstep drivers instead of a poll counter that a retained host log shows recovering a stall after 39,264 ms under a 20,000 ms grace; `f8b42790a` per-remote lockstep counters, the relay send result no longer discarded, a lockstep report from menu-driven matches; `7768341eb` the relay host calls a wedged or unreachable remote gone after half the grace with guards for the 2-peer host, its own stalled pipeline and never dropping every peer at once, and a refused forward is held and retried) and narrowed the replacement's permanent stall to the discarded relay send at `NetLockstep.cpp` ~1899; its 4-peer proof runs on the next approved build with the instrumented lobby driver copy under that lane. The MAX_PATH jobs were re-run alone from the short root `D:/mx/s32r1` on the same executable, and every later matrix runs from a short root, and the ten required gates keep their REQUIRED/REVIEW/EXTERNAL statuses until the Source33 matrix. **Source32 reds re-run alone from the short root `D:/mx/s32r1` (same executable):** `failure_staged-ordinary`, `mp_d3_deliver_cargo`, `mp_d3_stall_repeat`, `mp_fakelag100_auto`, `mp_perturb_control` and `mp_mismatch_control` return 0, confirming the MAX_PATH cause; `lobby_4_rejoin_200` and `lobby_4_drop_200` stay red on their `stayer2`/`replacement` peers (the lockstep problem above) and `mp_snapshot_p5` stays red on `snapshots_sim_identical`, which the same triage showed is a harness projection gap with zero differing simulation fields: all 41 differences are per-peer FMOD listener positions, listener-derived attenuation/occlusion values, device playback cursors and two Allegro palette scratch values, while the committed `audibility` table, every voice identity, the Scene, the Activity and all five VM graphs are byte-identical; `snapshot_runtime.SCHEMAS` lacks `AudioRuntime2`, so the whole AudioMan payload is compared as an opaque blob, and adding that schema with six named listener-derived masks and a `FramePalette1` mask closes it (patch being prepared, applied before the final matrix). **Integration (2026-09-08 ~13:20 UTC):** the harness patches landed as `a6aa570fb` (tools only: cross-process real-clock masking for refusal comparisons, the seed's global-script fixture for archive loads), then `stage2/rollback-audio-owner`, `stage2/polish-33` and `stage2/h4-phase-a` merged with `--no-ff` as `9d11400db`, `d472db7ae` and `1b7a741a0` (HEAD, 239 ahead and unpushed, clean auto-merges); **Source33** (Windows `0e0c708d`, `grouped-build-0e0c708d`, export `combined-source-33`, `chain-source33-1.json`): all eleven engine selftests pass on the approved path, including `net-discovery` and the two reconnect suites; the authority match and fault-injection gates, and the `wounds`, `rng_particles` and `ui_bus` sound-query gates match 600/600 as on Source32; the remaining family on `0e0c708d` (`chain-source33-remaining.json`) is green: native suite 516 PASS / 0 FAIL (`native-source33-1`), two-peer heal `20260908_1213*_heal_*` under `expanded-mod-gates`, prediction invariance, restoration `restore-source33-1` 13/13, the ordinary-load transaction gate `full-runtime-transaction-valid-33` complete with continuation checks and every raw difference classified render scratch (8), and the rollback fuzzes `fuzz-buy-source33-1` 17/17 and `fuzz-pickup-source33-1` 10/10 byte-identical (the buy fuzz was red on every build since Source2); the gameplay breadth and H4 gate lane (`lanes/source33-lanes`) is running on it. The Mac build of Source33 stopped at `ContentFile.cpp` (`'cout' is not a member of 'std'`: the new image self-test streams through `std::cout` and MSVC reached the header transitively), so `cf9dcda67` names `<iostream>` directly and becomes **Source34** (export `combined-source-34`; the Mac chain and selftests run on it first, the Windows Source34 build follows once the Source33 lanes finish, and the full matrix runs on Source34 from `D:/mx/s34`; the Source33 matrix was stopped after its first jobs so the final family is one unchanged build). On Source33 the eight H4 two-peer gate drivers all returned 0 on the approved path (`D:/mx/s33lanes/h4/run_all_summary.json`: gns_provider_smoke, old_wire_fixture, clean_leave, reclaim_socket, crash_relaunch_provisional, fencing_two_transports, rejoin_after_resync, peers_3_4_regression). **Source33 gameplay breadth and H4 gates** (`lanes/source33-lanes/report.md`, executable `0e0c708d`): the semantic battery is 10/10 with all six semantic lines identical to Source32, the interp matrix 16/16 including `mismatch`, the fake-lag duel passes at 100 and 200 ms (0 resyncs, auto delays 8 and 14), tick-300 invariance 8/8 with the control firing, and `-net-discovery-selftest` passes on the approved path. The eight H4 §9a gate drivers ran for the first time: four driver defects were fixed in lane copies (reports nested under `service`, the leave flag given to the host, uncalibrated mid-match sleeps, a `strict_compare` tuple), after which `gns_provider_smoke`, `old_wire_fixture` and `crash_relaunch_provisional` pass and four findings sit behind the five reds: (i) a 1v1 match ends when its only remote peer drops (`NetLockstepCoordinator::ApplyPeerLeave` "Nobody left to play with"), so 2-peer reconnect cannot work as built; (ii) the §7 leave exchange runs from `Destroy()` after `LeaveMatch()` has torn the link down, so no `LeaveAck` ever arrives and the ticket is never cleared; (iii) a second incarnation presenting the same ticket is refused instead of fencing the first; (iv) RED: the 3-peer drop/reclaim resync fails 4 runs in 5 on the host with `unresolved native references for owner 1048920` → `could not apply MovableMan checkpoint` → `resync failed`, after which the returner crashes on a null Scene in `CameraMan::Update` (survivors byte-identical throughout). Lead decisions: a dropped seat holding a valid ticket is held open for the reclaim window even as the last remote peer (a clean leave with nobody left still ends the match), the leave exchange runs before teardown, the second incarnation must fence the first, and a failed resync returns to the menu instead of running the loop on a null Scene — lane `lanes/h4-a4-semantics` (worktree `D:/Projects/h4-phase-a`); the resync apply failure was root-caused and fixed by lane `lanes/resync-refs` (worktree `D:/Projects/resync-refs`, branch `stage2/resync-native-refs`, two commits `a7557eb49`, `b5a07fb9d`): `LuaStateWrapper::ReleaseScriptOwnedObjects` detaches every Lua-owned MovableObject on restore while `SerializeScriptGraph` carries only what its roots reach (`_ScriptedObjects` excluded from the globals walk, the Lua registry not walked), and `MovableMan::SaveCheckpoint` wrote a borrowed-reference row for every registered object, so a restore demanded back an object the checkpoint never carried; the host fails on its own snapshot in its own process, which allowed a single-process reproduction through the resync's own save → stage → restart path with a fixture holding script-owned objects in seven heap locations (123 file-mode round trips on the two duel recordings pass, so it is not a general fault). The fix writes a borrowed row only for an object that borrows something and refuses a capture that cannot rebuild a script-owned object, naming it, with both sides sharing one `VisitScriptOwnedObjects` walk; the resolver is untouched. **Slice A4** landed those decisions as four commits `e8671293f`…`9b25121af` on `stage2/h4-phase-a` (`lanes/h4-a4-semantics/report.md`): a `NetLockstepSeatState` interface the round consults before adjudicating a lost transport holds a dropped ticket holder's seat for P2's 20,000 ms so a 1v1 does not end under a returning player while an announced leave still ends it at once (the admission plane is now ticked mid-match), `LeaveMatch` runs the §7 exchange on the service worker before the link goes down (a negative control reproduces the old `client_leave_acks: 0` shape), a returning ticket holder on a full session is admitted on a bounded provisional id and handed the seat's own id at commit (the earlier `SessionFull` refusal, not the fencing logic, was why the second incarnation never joined), and a failed `RestartActivity` returns to the menu through the §11 state machine with `CameraMan::Update` guarded against a null Scene (a control build without the guard still access-violates); eleven selftests pass on both configurations and the SimBaseline hash and all 600 tick hashes are unchanged. The eight gate drivers re-run on the next approved build. The snapshot projection patch landed as the tools-only commit after `cf9dcda67` (AudioRuntime2 schema, six listener-derived masks, palette scratch; 21 retained archives decode exactly, both cross-peer pairs now match, six same-length negative controls still fail). **Numbering:** the tools-only snapshot commit `4ec709e83` sits on the Source34 engine tip, so the Windows family built after it is exported as **Source35** (engine bytes identical to Source34; the Mac's Source34 results stand for the engine and the Mac chain re-runs as 35 for consistency); the lobby-grace, H4-A4 and resync-refs lanes will produce the next engine family. **Source35** (Windows `6b5ae9a2`, `grouped-build-6b5ae9a2`, export `combined-source-35`): all eleven selftests, the authority match and fault gates and the three sound-query gates pass (`chain-source35-1.json`); the remaining family on `6b5ae9a2` is green (`chain-source35-remaining.json`: native 516 PASS / 0 FAIL, two-peer heal, prediction invariance, restoration 13/13, the ordinary-load transaction gate with every raw difference render scratch (7), rollback fuzzes 17/17 and 10/10); the Mac builds it from the exact export and passes the native suite, authority, the three sound-query gates and all ten socket-free selftests (`mac-peer-20260907/results-source35/`); the full matrix from `D:/mx/s35` and the breadth lane repeat (`lanes/source33-lanes` follow-up) are running on it. **Mod-compatibility audit (binding correction of 2026-09-08):** `reviews/claude-review-2026-09-08/lanes/mod-compat-audit/report.md` measured, with one unchanged Lua fixture on Source22 (`4244e87e`), Source31 (`bd9da823`) and Source35 (`6b5ae9a2`), that the logical-audio local cohort introduced at Source23 (`f5f0c15ed`: copy-on-write `LocalControls`, cohort index 1, `VoiceMatchesContext`, `SoundSet::m_SimulationSelections[1]`) DISCARDS sound operations made from `UpdateAI`/`ThreadedUpdateAI` instead of deferring them and changes single-player behaviour: R1 `IsBeingPlayed`/`Stop`/`FadeOut`/overlap modes are partitioned away from the AI hook (Browncoats' boss voice arbitration overlaps and cannot stop), R2 every writable property written from an AI hook is discarded, R3 `Pos` by value, R4 `SoundSet` selection partitioned, R5 `WhilePieMenuOpen` moved to that cohort, R6 a freshly added actor skips its first AI pass; the shared-scope control arm is byte-identical on all three builds. This violates the fork's agreed AI-write contract (§B-1 item 1b: AI-hook writes defer to the committed tick via `DeferredEquip`/`NetGameAIEquip`, single-player semantics untouched). Lead decisions: one simulation cohort for queries; AI-hook sound mutations defer to the committed tick through a new owner-issued lockstep command with an immediate read-back overlay inside the AI pass; `GetAudibleVolume` in AI hooks keeps the controlling peer's local reading; `WhilePieMenuOpen` returns to Presentation; script initialization moves to a deterministic pre-pass so no AI pass is skipped; `Pos` is a live alias again with AI-scope alias writes reconciled into deferred writes; logical playback, the committed audibility authority and UI-bus-logical stay. Lane `lanes/sound-ai-deferral` (worktree `D:/Projects/pos-alias-repair`, branch `stage2/pos-alias-repair`) landed the repair as six commits `81bab781f`…`2796086a0` (`lanes/sound-ai-deferral/report.md`): one simulation cohort and one `SoundSet` selection, an AI hook reads what the simulation played, and its `Play`/`Stop`/`Restart`/`FadeOut`/property/`SelectNextSounds`/position calls read back inside the pass and land at the committed tick (immediately in single player, as a new owner-issued `NetGameSoundOp` lockstep command under lockstep, drained where the equip calls drain; lockstep codec 12 → 13 with the canonical pin moved), the pie menu's sounds run in presentation again, object scripts initialize in a deterministic pre-pass so no AI pass is skipped, and `SoundContainer.Pos` is a live alias again with AI-scope alias writes folded into the deferred writes; a `CheckpointVersion` reader that still only recognised `SoundContainer2` while the writer emitted `SoundContainer3` was found and fixed on the way. On the repaired build `5a39f651` the unchanged 26-case fixture fails 0 cases (Source22 reference 0, Source31 11, Source35 15), the native suite is 518 PASS / 0 FAIL, restoration 13/13, both fuzzes byte-identical, ten socket-free selftests pass and the SimBaseline is byte-identical to Source35; the two-peer mode for `run_sound_query_gate.py` is prepared for the approved path. The independent reviewer (`lanes/compat-review/report.md`) accepted the repair with named follow-ups after reproducing the matrix (25 of 26 detail strings character-identical to Source22, the 26th the `UniqueID` counter) and adding 22 cases that match the reference: F1 a position alias handed out in an AI hook and written from a shared hook is delayed a drain and discarded if the AI never touches the container again (measured against Source22), F2 `AudioMan:StopAll()`, the structural `SoundSet` mutators plus `SetTopLevelSoundSet`, and a shared-captured alias written from an AI hook still write shared state directly, F3 the deferred-op tick/ordinal is not checkpoint state, F4 the pending overlay is written outside its mutex, F5 the `SoundContainer2` → `SoundContainer3` reader has no executable control; the family is not called green on compatibility until the deferral lane's follow-up closes all five (`lanes/sound-ai-deferral/followup-1.md`) and the reviewer re-checks. The earlier `lanes/pos-alias-repair` lane was stopped and its fixture folded in. **Source35 matrix red:** `continuation_normal` and `continuation_late` fail deterministically in `file_50` and `launch_50` (`FATAL: unhandled exception 0xC0000005` after the staged restart; three isolated re-runs under `D:/mx/cnr` identical; Source32 passed this job) while `memory_50`, every later capture, the late-global restoration suite and both fuzzes pass on the same executable — lane `lanes/restore-50-fix` (worktree `D:/Projects/restore-50-fix`, branch `stage2/restore-50-fix`, commit `99693a337`) symbolized it to `ActivityMan::CaptureRuntimeGlobals` walking a freed `m_KnownObjects` entry (identity 1049152, a gibbed Battle Rifle): `MovableMan::ConstructionRegistryScope` copies the identity registry on entry and swaps the copy back on exit, `LoadGameToRestart` holds it across the whole archive read, and an object the LuaJIT collector swept inside that window was erased from the live map but resurrected from the copy (the settle only shifted the sweep; captures 49–50 at window 30 hit it, `memory_50` never restarts); `UnregisterObject` now drops the object from every held registry copy with the same pointer-equality test, no refusal or mask changed; the identical `WorldSetAside`/`ReinstateWorld` reinstate is written up as an untested sibling. **Mac Source34** (`19ec5d6d`, zero source mismatches, `mac-peer-20260907/results-source34/`): the native suite, the authority match, the three sound-query gates and all ten socket-free selftests including the two reconnect suites pass. **Source35 matrix stopped at 23/104** (21 green, the two continuation reds now fixed by `99693a337`) so the family could move on. **Source36 (integration):** lane `lanes/integration-36` composed the lobby-grace × A4 conflicts in `NetLockstep.*`/`NetLockstepSelfTest.cpp` (adjudication decides whether the round keeps waiting, the seat hold decides whether it may end because nobody is coming back; a new `TestCoordinatorAdjudicatedPeerKeepsItsSeat` pins it, 66 selftest runs green across both configurations, SimBaseline unchanged); the lead composed the one `LuaMan.cpp` selftest conflict of the restore-50 branch (both new checks kept) and merged the sound AI deferral on top; `stage2/integration-36` (`1281f8186`) merged into the main line as `680315941` (HEAD, 264 ahead and unpushed). **Source36** (Windows `5c12e69d`, `grouped-build-5c12e69d`, export `combined-source-36`, `chain-source36-1.json`): all eleven selftests pass on the approved path, the authority match and fault gates and the `wounds`/`rng_particles`/`ui_bus` sound-query gates match 600/600; the remaining family on `5c12e69d` is green (`chain-source36-remaining.json`: native 520 PASS / 0 FAIL, two-peer heal, prediction invariance, restoration 13/13, the transaction gate with every raw difference render scratch (9), fuzzes 17/17 and 10/10); the Mac builds it from the exact export (`f3cb71c4`, zero mismatches) and passes the native suite, authority, the three sound-query gates and all ten socket-free selftests (`mac-peer-20260907/results-source36/`); the breadth lane (`lanes/source33-lanes` Source36 follow-up: gameplay gates, the eight H4 gates with A4, the instrumented 4-peer lobby lanes, the `ai_defer`/`per_machine` two-peer sound modes now in the stock runner, the 26-case deferral fixture on the approved executable), the compatibility reviewer and the full matrix from `D:/mx/s36` are running on it. **Source36 breadth** (`lanes/source33-lanes/report.md` `## Source36`, executable `5c12e69d`): the semantic battery is 10/10 with no semantic line moved by the sound deferral, the interp matrix 16/16, the fake-lag duel passes at 100 and 200 ms with the same auto delays and 0 resyncs under the shorter wall-clock grace, tick-300 invariance 8/8, discovery passes, the 26-case deferral fixture is 26/26 on the integrated executable and the two-peer `ai_defer` mode matches 600/600; `peers_3_4_regression` is 5/5 (the resync apply failure is gone, finding (iv) closed) and seven H4 checks flipped green with A4's decisions visibly live. Three new reds: the H4 resync now fails at the snapshot SAVE because the host's activity is already `Over` at tick 346 when the returner reconnects (the round survives under the held seat, the activity does not — lead decision: a held seat's player stays counted by the activity until the window expires; lane `lanes/h4-a4-semantics` follow-up, slice A5); every lobby-lifecycle replacement lane, including the 3-peer/200 ms and 4-peer/100 ms controls that passed on Source35, now fails at the lobby rejoin after a leave (`assert_substate expected=Landing actual=Lobby`, session `Failed (members=2)`) — a regression from A4 or the merged grace changes, same lane, with the decision that lobby-phase leave/rejoin keep Source35 behaviour and the H4 rules apply to mid-match joins only; and the `per_machine` positive control of the sound gate did not fire (queued for the deferral lane). **Slice A5** (`lanes/h4-a4-semantics/report.md` `## Slice A5`, four commits `d3671eb9a`…`9ed4c5e04`): the end condition was the duel fixture's own Lua win check (`anyTeamOut and #aliveTeams <= 1`, reached because the departed peer's team had no brain) — a scripted game-over now waits while `NetLockstepCoordinator::IsHoldingSeatForReclaim()` is true (every remote gone and one seat inside its window, so no peer can disagree), with engine and player teardowns unaffected; the lobby regression was the H4 admission plane gating lobby joins (`client_state: Denied` because `FindFreeNeverHeldSeat` never freed a departed member's seat), so a lobby seat goes back to the pool on a leave or drop while mid-match semantics stay; the survivors' `actual=Lobby` was §11 retrying a lobby that was never a match, so recovery applies to matches only; and the instrumented lobby driver landed in `tools/` byte-identical to lobby-grace's verified copy. The lobby lanes and H4 gates re-run on Source37. **Source36 matrix red:** `continuation_normal` fails every in-memory capture with the new refusal `the restored Lua state cannot be carried: a script-owned AHuman (Checkpoint Human) that no script graph root reaches` while file/launch pass; Source35 carried that object through the in-memory restore byte-identically, so the reachability walk of `b5a07fb9d` was narrower than what the capture carries: the resync lane's follow-up (`9fe34c053`) measured 65 uncarried script-owned objects on that fixture, none the live registration for its identity (a set-aside world leaves the originals' script-owned trees shadowed in the Lua heap while the restored copies take their identities), and the check now skips anything that is not the live registration — the same predicate the release path and `MovableMan::SaveCheckpoint`'s reference map use — with a new `checkpoint_ignores_shadowed_script_owner` control (native 518/0, restoration 13/13, fuzzes green, all seven `memory_*` captures of `continuation_normal` green on its build). The deferral lane's follow-up closed the reviewer's F1–F5 in six commits `1fe7b5198`…`506609d72` (tip `4eba5abe`: an alias write lands by the scope of the write and no pending reconcile is dropped, `AudioMan:StopAll` and the structural `SoundSet` calls defer, the deferred-op tick/ordinal ride the `AudioRuntime3` archive with a mid-tick restore control, the overlay is written under its queue lock with a two-state ordering selftest, and a retained `SoundContainer2` artifact loads; two intermediate commits do not compile on their own and are recorded as such). The reviewer's re-check (`lanes/compat-review/report.md` `## Follow-up 1`, runs under `D:/mx/cr2` on the tip `4eba5abe`) closes F1 (both cases character-identical to Source22, Source31/35 still red), F3 (the mid-tick control and a scenario fuzz that genuinely drives `NetGameSoundOp`, 10/10), F4 and F5 (the retained `SoundContainer2` record verified byte-identical), and two of the three F2 items; the residual is an AI pass that touches a container only through a position alias, which never attributes the container and so settles as a shared write — closed by the deferral lane's follow-up 2 (`039269c58`, `lanes/sound-ai-deferral/report.md` `## Follow-up 2`): the shared hooks' alias writes now land at one fixed point before the first AI pass (`AudioMan::SettleSharedSoundWrites` from `MovableMan::UpdateActorsAI`) so the drain window holds only AI passes and reconciles unconditionally, with a native control whose negative-control build fails exactly that check and two new unchanged-Lua cases (`ai_alias_only`, `ai_alias_only_retained`) matching Source22 on the tip `cfcca2d9` (28/28, the reviewer's 22 extra and 8 alias cases identical, native 523/0, restoration 13/13, fuzzes green); the reviewer's final re-check (`lanes/compat-review/report.md` `## Follow-up 2`, runs under `D:\mx\cr3`, tip rebuild `8a0cab0f`) calls the mod-compatibility gate of this family GREEN: fifty cases across four unchanged-Lua fixtures and four executables character-identical to Source22 with no exception (the `spawn_child uid` exception does not exist when both arms run the same fixture, so the ladder is 28/28), the two broken builds red on exactly the detecting cases, the native control and its negative confirmed, ten selftests, native 523/0, restoration 13/13, three fuzzes and SimBaseline unchanged; the reviewer's own F6 is discharged by the same settle point (the baseline refresh before every AI window). Notes carried, none a defect: a shared-scope script dispatched from inside an AI hook (`MovableMan:SendGlobalMessage` from `UpdateAI`) runs in the AI window and has its alias write deferred, which is the contract's intent and belongs on the two-peer gate as an arm; in single-player only, a `PositionAlias` applied at the first drain of a tick can be re-deferred once at the second drain (one redundant op, value-neutral, impossible under lockstep where the first drain enqueues); `NoteAIActor` now takes the container's recursive mutex on every AI-scope read and the settle walks every container Lua ever took a position from, both unmeasured (performance belongs to the feel work). The harness gained the two-peer alias-only arm the deferral lane asked for: `run_sound_query_gate.py` mode `ai_defer_alias` (port +60, same private sample) with `ai_defer_gate.lua` holding a position in one AI pass and writing `Y` then `X` through it in later passes that call nothing else, the shared `Update` reading `Pos.Y` into a particle on both peers; expected `match` on Source38, and it would diverge on Source37, which still settles that write on the owner. That commit makes Source38 the final family. Mac Source37 `c12fc401` is green on the chain (native pass, authority match, wounds/rng_particles/ui_bus 0 changed ticks) and ten selftests. The staged Source38 tree is `de87ff198` on `stage2/integration-36` (main Source37 `802938a1a` plus `039269c58`), exported as `combined-source-i38` and building on the Mac (`chain_mac_i38.zsh`, selftests follow); its Windows family is built only after the Source37 breadth lane releases the approved executable. Lanes now running: `compat-review` follow-up 2 (final verdict on the residual closure), `source33-lanes` follow-up 3 (Source37 breadth), `h4-b1-substitution` (H4 Phase B slice B1 in `D:\Projects\h4-phase-b`, branch `stage2/h4-phase-b`, base `de87ff198`: applicants with the P15 bounds, the substitution transaction through the txId cache with the `Substitution` proof domain, cleanup rules, returner-versus-reassignment CAS, ledgered ownership for a committed substitute, a host moderation API on `NetMatchService`, in-process selftests, two-process gate drivers for the lead), and `world-setaside-audit` (`D:\Projects\registry-audit`, branch `stage2/registry-audit`, base `de87ff198`: whether `WorldSetAside`'s held `knownObjects`/script/sound records can go stale the way the construction registry copy did, with a selftest either way). The Mac built the staged Source38 tree as `30e06f16` with the ten selftests 10/10. The final family runs through `contract-audit/run_source38_family.py` (log `family-source38.log`, record `family-source38.json`): preconditions (the Source37 breadth lane finished, main clean, no engine process), merge `stage2/integration-36` into main with the tree asserted equal to `de87ff198`, `chain_build_gate.py --label source38-1 --source 38` with the eleven selftests, authority match and fault, wounds, rng_particles, ui_bus, ai_defer, the new ai_defer_alias and per_machine (the last recorded, not required), `run_remaining_gates.py --source 38`, the Mac chain plus selftests launched on `Erol-Mac`, the breadth lane follow-up 4 (`lanes/source33-lanes/followup-4.md`, awaited, nothing else running beside it), then the full matrix from `D:/mx/s38`; it is started by the lead once the Source37 breadth lane and the Source37 matrix have released the approved executable. Source37 breadth (`lanes/source33-lanes/report.md` `## Source37`, runs `D:\mx\s37lanes`): gates 1-4 and 6 green and identical to Source36; H4 4 PASS / 4 FAIL with `peers_3_4_regression` 5/5 - A5 removed the `resync snapshot save failed` blocker and the resync now completes (save, read, relaunch), but the relaunched activity then ends in state Over with zero ticks (`activity ended in state Over`, hosts exit 1) in `reclaim_socket`, `rejoin_after_resync` and `clean_leave`'s ambiguous arm, `fencing_two_transports` still fails on `GNS peer was not found` at tick 589 with no fencing, and the clean-leave arm is still unacked (`clean_leave_acked 0`); lobby: the 3-peer/200 ms control is 3/3 (the Source36 regression is closed), `lobby_4_rejoin_200` now runs a match and its counters are on record (host `relay_send_failures` 16-27 with `SendMessageToConnection failed with EResult 25`, 30-50 resends, every peer 176-177 of 180 frames, stalls 6 s against the control's 0.3 s; zero refusals at three peers), while `lobby_4_drop_200` (`connected:4 -> TIMEOUT`) and the 4-peer/100 ms control (`remoteready -> TIMEOUT`) never start a match; compatibility ladder zero differences against Source22 across four fixtures including the UniqueIDs; `ai_defer` match; the `per_machine` control cannot fire as designed because the shared-scope `GetAudibleVolume` reading is the committed authority value (host and client histograms identical value for value across 2404 samples, the private gains never enter it), which is the settled policy, not a defect - the control is re-armed with `CC_FAULT_INJECT=local_audibility` (the chain's `query` specs now carry `:ENV=V` switches). Both residues go to engineering lanes on the Source38 tree: H4 slice A6 (resume of `h4-a4-semantics`, worktree `D:\Projects\h4-phase-a` fast-forwarded to `de87ff198`) for the post-relaunch game-over, the fencing and the clean-leave ack, and a lobby-grace follow-up (worktree `D:\Projects\lobby-grace`, same base) for the EResult 25 relay limit at four peers and the 4-peer lobby start; their commits form the next family after Source38. The Source37 matrix was stopped after 15 of 104 jobs once the breadth lane finished (results retained under `D:\mx\s37`, every finished job NEEDS_REVIEW with return code 0, superseded by the Source38 matrix); the Source38 family driver started at 17:56 UTC. **Source38 is built and pinned: `grouped-build-f29f69bc` (exe `f29f69bc…`) from main HEAD `32312c71e` (the merge of the staged tree, 280 ahead of origin, unpushed).** Fast gates on it (`chain-source38-1.json`): eleven selftests, authority match and fault, wounds, rng_particles, ui_bus, ai_defer all green; the new `ai_defer_alias` arm failed its first run on a fixture error of the lead's (luabind has no equality across the two userdata wrappers, `object ~= self` raised `No such operator defined`; the handler now compares `UniqueID`s) and passed on the second run against the same build (`sound-query-ai_defer_alias-source38-2`: 600/600, 0 changed ticks, no errors; the host-owned actor 1048615 and the client-owned actor 1048634 each hold the position in an AI pass, write Y 77.5 at step 131, X 235.5 at step 146 and Y 67.5 through the handler dispatched by `SendGlobalMessage`, and the shared `Update` on both peers reads the identical sequence at the same steps); `per_machine` with `CC_FAULT_INJECT=local_audibility` diverges (141 changed ticks), so the fixture family is proven sensitive. The driver continued with `--from remaining`: the Mac chain plus selftests were launched (`chain_mac_38.zsh`), the remaining family is running, then the breadth lane follow-up 4 and the final matrix from `D:/mx/s38`. The remaining family on Source38 (`chain-source38-remaining.json`, 18:02-18:08 UTC) is green: native 526 PASS / 0 FAIL, heal (resync) and invariance with `source_unchanged`, restoration `pass: true`, the transaction gate complete with continuation checks passed and 8 RENDER_SCRATCH differences in the raw classification (9 on Source37), fuzzes 17/17 and 10/10 byte-identical. The breadth lane follow-up 4 is running on it; the matrix follows. **Source37** (integration tip `ded4b2ecf` merged into the main line): Source36 plus the shadowed-owner refusal correction, the deferral reviewer follow-ups and H4 slice A5; the Source36 matrix was stopped at 19/104 for it. Source37 is Windows `5ee60605` (`grouped-build-5ee60605`, export `combined-source-37`, main tip `802938a1a`, 279 ahead and unpushed): all eleven selftests, the authority match and fault gates, the `wounds`/`rng_particles`/`ui_bus` and the new `ai_defer` two-peer sound gates pass (`chain-source37-1.json`), and the remaining family is green (`chain-source37-remaining.json`: native 524 PASS / 0 FAIL, heal, invariance, restoration 13/13, transaction with 9 render-scratch differences, fuzzes 17/17 and 10/10); the breadth lane (Source37 follow-up with the stock instrumented lobby driver), the Mac chain and the full matrix from `D:/mx/s37` are running on it, and the deferral lane's follow-up 2 (the last F2 residual) will make Source38 the final compatibility family. **Next engine family (Source36) staging:** worktree `D:/Projects/integration-36` (branch `stage2/integration-36` from `4ec709e83`) carries `stage2/lobby-grace` merged and `stage2/h4-phase-a` (A4) mid-merge with content conflicts in `NetLockstep.cpp/.h` and `NetLockstepSelfTest.cpp` (the grace/relay repairs and the seat-hold interface touch the same adjudication path); lane `lanes/integration-36` composes them, merges `stage2/resync-native-refs`, builds both configurations and runs the selftests; `stage2/pos-alias-repair` (sound AI deferral and the `Pos` alias) and `stage2/restore-50-fix` merge on top when their lanes land, then the Source36 chain, the local family, the breadth and H4 gates, the 4-peer lobby proof with the instrumented driver, the Mac chain, the compat reviewer, the headed §11 check in the morning and the full matrix from `D:/mx/s36`. **Integration plan:** `reviews/claude-review-2026-09-08/INTEGRATION_33_RUNBOOK.md` holds the ordered steps for Source33 (re-run the Source32 reds alone, apply the harness patches, merge the three lane branches, build and pin, run the local family plus the tick-300 invariance, the H4 two-peer gates, the Mac chain, the headed §11 review on the private desktop, then the full matrix with nothing building beside it). **Open:** Source32 Windows build and the full local set (nine selftests, native suite, authority match and fault, sound-query x2, UI-bus fixture, heal, restoration, invariance), Mac Source32 (native under the amended runner, authority, sound-query), re-run of the continuation classification for the two `HDFirearm::m_FireSound` positions on a logical-audio build, per-concern local commits, then the frozen family, fresh seed/control set and the full collected matrix (`full_collected_matrix.py`, 104 jobs) on one unchanged build; normal MP/H4, session UX, discovery and the 100–200 ms feel remain after that.

**2026-09-08 resume checkpoint (~07:00 UTC): HEAD 62c53748c, 189 ahead and unpushed; the paused tree was verified byte-for-byte against the capsule before any edit.** `62c53748c` canonicalizes LuaJIT stitch frames at coroutine capture (`LuaThreadCodec.cpp`): the retained VM4 `job.base` 7 vs 10 was one process yielding from a compiled trace (three stitch slots) while the other yielded from the interpreter, a per-process JIT decision, not simulation state. On Source22 (`4244e87e`, `contract-audit/grouped-build-4244e87e`) `contract-audit/full-runtime-transaction-valid-22` passes: both independent ordinary loads through 521, all five Lua VMs match, trace and dump identical (`CONTINUATION_AND_REFUSAL_CHECKS_PASS`). The 11 raw native differences are classified per record in `.../ordinary-load_valid_full_100/continuation-raw-classification.json` (`classify_continuation_raw.py`): 9 shared MOSRotating temp-bitmap pixel hashes (render scratch cleared before every draw) and 2 `HDFirearm::m_FireSound` positions written under physical `IsBeingPlayed`, which stay OPEN until re-run on a logical-audio build. **Logical audio is integrated (Source23 `68b50ab1`, `grouped-build-68b50ab1`, export `combined-source-23`):** the saved WIP was merged onto the Source21 AudioMan diagnostics (NOSOUND guard and continuation observer retained) and finished as a per-voice anchor model: simulation liveness is a pure function of simulation time (no query mutation, no time scale or bus pitch), local-AI writes land on copy-on-write control copies, UI-bus sounds stay physical, scopes wrap MovableMan stages, script hooks (`UpdateAI`/`ThreadedUpdateAI` local), activity and tick, finished voices retire at `UpdateSim`, and a failed device falls back to FMOD NOSOUND so sample metadata exists on every peer. Native suite `contract-audit/logical-audio-native-1` 352/352 including 21 `[audio-logical-selftest]` checks. The retained Mac gameplay negatives are positive on both platforms with the identical fixture: Windows `contract-audit/sound-query-wounds-source23-2` and `sound-query-rng_particles-source23-2` match 600/600 ticks (`run_sound_query_gate.py`), paired negative controls on the Source22 executable `sound-query-wounds-source22-negative-1` (291 differing ticks from 310) and `sound-query-rng_particles-source22-negative-1` (306 from 295, the Source19 count); Mac Source23 `24925728` in `mac-peer-20260907/results-source23` matches 600/600 for both (effects 4/4 and 1566/1566). Restoration suite `contract-audit/restore-logical-audio-1` passes 13/13 (memory/file/launch at 50/60/100/400 with the late global) and prediction invariance `reviews/recovery-2026-09-06/expanded-mod-gates/20260908_062824_invariance_2e1d360c` passes 8/8 with Lua-fault detection, both on `68b50ab1`. **RED:** (1) the two-peer heal gate fails deterministically with `ProtocolError:lockstep frame sender mismatch` at the tick after the resync relaunch on the pause-state Source21 `e28bce73`, Source22 and Source24 alike (`expanded-mod-gates/20260908_0629*_heal_*` and `20260908_0634*_heal_*`); it predates this session's changes; Source25 adds the offending sender/frame to the failure text so the race can be pinned. (2) The Mac native suite crashed (SIGSEGV, `results-source23/native-graph-source2{3,4}-stdout.log`) resuming the raw stitch-form restore, exposed by the new control; Source25 collapses stitch frames on restore too (`_ScriptGraph.canonicalThread`, refuses a malformed continuation) so the runtime never rebuilds one; Mac re-run pending. (3) `GetAudibleVolume` authority transport is designed, not built: observations sampled at the input boundary ride the sender's delayed frame (lockstep codec v11), every peer commits the identical table, queries take the controlling peer's reading, host for activity/unowned, else the latest committed reading, else 0 before any observation lands. (4) Fresh seed/control family and the full collected matrix on one unchanged build, normal MP/H4 and 100–200 ms feel remain open. Source24 `687d3340` (`grouped-build-687d3340`) equals Source23 plus a selftest progress echo and an unsigned-negation fix; the invariance run and the heal bisect ran on `68b50ab1`/`687d3340` while the tree already carried those two edits.

**2026-09-08 paused checkpoint: HEAD 393fe1e5c, 188 ahead and unpushed. The user requested a resumable stop to conserve tokens. Source21 Windows e28bce73 builds and passes the native checkpoint suite. The orphan-search buffer lifetime fix removes the measured ordinary-load crash: two independent loads complete521 ticks with matching shared traces and marker731 in all five VMs. The full continuation gate remains RED: VMs0-3 are byte-identical, but VM4 differs at a retained job.base value (7 versus10); raw native differences still need classification. The bounded run reuses the Source20 seed and is not final-matrix sign-off. The audio continuation helper and verified NOSOUND guard build, but dedicated runtime and mute-bypass integration remain pending. Incomplete logical sound lifecycle work is preserved outside the active tree as eight byte-verified files with a tested reapplication patch and detailed handoff. Actual-volume authority remains the controlling player, host for activity/unowned objects; transport and callback integration are unfinished. Mac Source20 and prior scoped passes remain in MAC_RESUME.md; sound-query gameplay negatives remain red. Read reviews/recovery-2026-09-07/contract-audit/PAUSED_CHECKPOINT_20260908.md for exact source/binary/backup paths, all lane handoffs and resume order. The full collected matrix, native/VM contracts, normal MP/H4 and100-200ms feel remain open. All three subagents are stopped; no upstream PR or push has occurred.**


**2026-09-07 / 19:27 UTC grouped implementation — not a sign-off:** HEAD remains
`6d6115717`, 154 ahead and unpushed. `contract-audit/native-validation-groups-2`
passes the native codec suite, including 12 malformed-graph controls and SG1/SG2,
on combined executable `149576a467e1cb191644bfd0ef27e25791eae75f98307da941aaeb31abff70c3`.
`contract-audit/native-group-development-1` has a valid 1,523-getter reference;
memory/file losses fall from 151 to 27, all copied wound targets or actor pie menus.
Source and inputs stayed unchanged. Registry transition, per-class snapshot configuration,
nested identity and strict all-VM prevalidation changes are in the working tree.
The preceding `native-validation-groups-1` failure is retained: emission targets were
written as new preset definitions; the corrected build writes their preset references.
The remaining wound/pie-menu and activity-owned reference changes are compiling in
`build-native-references-group-1.log`; no result is claimed for them. Full activity/UI and
manager state, atomic load transactions, ordered pending cohorts, engine RNG, full peer
restoration/shared-state comparison and the complete MP/H4/latency roadmap remain open.
These are development checks while implementing the five collected repair groups;
the complete matrix still must run together on one unchanged final build.


**2026-09-07 / 18:43 UTC grouped-repair baseline: local diagnostic commit 6d6115717 preserves the independent state/alias observer and native/activity/reference fixtures. HEAD is 154 ahead and unpushed. Collection ran on unchanged identified builds without production repairs: the typed native reference passes 1,523 values across 24 classes, while memory/file each lose 151; ordinary activity/UI loading loses 17/28 values and a fresh-process load loses 19/28 including manager options. Eight saveable reference restores pass; ten other valid native reference types refuse capture. Pending queues, RNG, UID/lifetime, destructive failed loads and strict graph parsing are documented in contract-audit/CONTRACTS.md. Grouped ownership/state/transaction/reference repairs now begin, followed by the entire matrix on one unchanged build. Audit executable 65c51ea6 and symbols are retained. The diagnostic commit was split from the tested combined source, not independently built. Native/VM breadth, normal MP/H4, cross-platform gates and the full roadmap remain open.**

**2026-09-07 / 16:58 UTC systematic contract audit — implementation frozen:**
The user has required a comprehensive investigation and failure collection before further
grouped fixes. The frozen combined source is recorded in
`reviews/recovery-2026-09-07/contract-audit/manifest.json`, with source, binary and symbols
preserved; executable `ef82285add68712534c6eb3165a010863e25752ed4f2901738df261c4ff03127`,
HEAD `d227a1a85`, 153 ahead and unpushed. Test-only instrumentation must identify a separate
audit build. Read `contract-audit/CONTRACTS.md` for the complete transition/state matrix,
findings and evidence rules. Do not resume a single-defect repair/fixture expansion loop.

The broad Windows run `stage2_p4/recovery_runs/20260907_165020_6fd61b52` finishes 19/20;
raw cross-peer snapshot comparison remains red. P5 now captures at the completed tick.
`contract-audit/roundtrip-existing-snapshots` passes both full peer archives through the
ordinary load entry in fresh processes, without post-load harness repairs. The frozen build
includes repairs already in progress before the audit: empty Reader fields, pending native
link resolution and saved controller fields. These changes remain uncommitted and do not
prove preservation of fields absent from both saves.

`contract-audit/late-restoration/20260907_165252_508619af` has a valid uninterrupted
reference: 7/10 cases pass, with memory/file/ordinary capture-60 failures. The memory API
refuses valid pending objects, while file restore changes their membership/timing. No repair
has been attempted during this audit. Source inspection also finds engine RNG absent from
file capture, broader probe-only repairs, unequal world snapshot/set-aside/event cohorts,
and destructive restart before late graph restoration failure. These need direct runtime
oracles, not merely archive or hash comparisons. Native/API inventory counts are discovery
scope only; they are not proof of complete state or mod coverage.

**Next:** complete production-entry-point state observations, failure-stage controls and
the native/ownership inventory; collect findings before grouped implementation and run the
entire declared matrix on one unchanged build. Full restoration, graph/world integration,
normal MP/H4, fresh cross-platform verification and the full roadmap remain open.

**2026-09-07 / 16:36 UTC completion and rejected-join checkpoint:**
`5c023f565` carries the final applied simulation tick in Complete and drains the client's
pending final tick before stopping. `net-completion-before-1` fails the new D=0/3 control
("completion discarded an unapplied final simulation tick"); `net-completion-after-1`
passes on build `build-completion-fix-1.log`, executable `4fc2b2dca6fd...`. The rematch
harness then exposed a test-dispatch ordering problem, fixed in the same commit.
`stage2_p4/recovery_runs/20260907_161655_81b619e5` passes fresh rematch, D=3 stall and
brain-spawn lanes on `659a1334fe22...` (`build-completion-rematch-1.log`).

`b0531ada7` retains the exact join rejection in the host's live lobby and grows the error
label to fit wrapped text; the lobby remains available for a compatible replacement.
`d227a1a85` adds the menu regression and an explicit early-exit rejection harness flag.
`lobby-rejection-after-3` passes all process/console/desktop checks and 180 strict matching
host/replacement ticks on build `build-join-rejection-layout-2.log`, executable
`8525e61871075b292a45b726d8498cf48de115439b947df0cf8dd8e98f55f13e`.
The host and rejected-client screenshots are retained and readable. `-after-1` passed
semantics but revealed clipped host text and a timestamped screenshot-path assertion error;
`-after-2` passes with the visual fix; `-after-3` additionally avoids bitmap rebuilds on
unchanged menu dimensions. No source change occurred during each gate. These focused commits
were split from the tested combined source, not independently built.

`stage2_p4/recovery_runs/20260907_162446_43dff8f2` passes the mismatch control (the host
records rejection in Starting rather than timing out) on `5473ac0628bb...`.
`reviews/recovery-2026-09-06/expanded-mod-gates/20260907_162526_heal_bdceb430` passes
real two-peer global/orbit recovery at D=3 through 600 with no console errors on that build.
The preceding D=0 global recovery also passes (`20260907_160911_heal_021523eb`).

The earlier broad `20260907_150624_dcee8bc6` is still recorded as 16/20, never relabeled.
Snapshot investigation found all 63 logical SG3 differences below actor `AI` fields in the
master VM; the other four VMs match byte for byte. Native differences are private limb-path
real-clock anchors, derived AI impulse caches, sound selection and the muzzle-flash frame
chosen by the existing visual RNG. PNG layers and Activity simulation fields match. These
are measured classifications, not a completed comparison/restoration gate. The P5 test
currently captures midway through tick 300; move it to the completed tick before re-gating.
Do not force local AI to run identically or discard its state from full checkpoints.

**Next:** complete the shared simulation and full peer-restoration snapshot gate, then late
failed-load rollback, remaining native contracts and graph/world integration commits, normal
MP/H4 and the full roadmap. HEAD `d227a1a85`, 153 ahead, unpushed. Unqualified build/test
paths are under `reviews/recovery-2026-09-07/`.

**2026-09-07 / 16:03 UTC orbit and recovery checkpoint:** The completed combined build
`build-sound-ready-1.log`, executable `ecf08c428c0200dccacb8e4193362ff57a957ea99d1a0e195437a74bd81c8f1f`,
passes `restore-sound-ready-1/20260907_155709_dc3df1c1`: all 13 cases through 521,
with memory/file/ordinary loads at 50/60/100/400, the sustained mod fixture, four Lua
states, unpublished paths and an enabled global that creates a craft at 60, receives its
orbit callback, then deactivates at 70. The capture-60 disabled controller and quarantine
queue now round-trip. Old Lua-owned identities are detached before the incoming world.

Under `reviews/recovery-2026-09-06/expanded-mod-gates/`, `20260907_155709_heal_06aa568e`
passes real two-peer global recovery through 600, with no console errors and the orbit event
on both peers. `20260907_155749_invariance_7b8fc091` passes original pickup/fire 8/8;
`20260907_155943_invariance_2a2b72d9` passes separate global 8/8. Both detect the Lua-only
fault and report complete/source unchanged. `native-graph-sound-ready-2` passes the native
suite, including SG1/SG2 and 44 portable RNG continuations. `global-events-sound-ready-1`
and `global-events-late-sound-ready-1` pass pause/orbit/end and memory/file controls.

`3033e94c1` fixes the global started guard; `c53ff6dde` adds a nonallocating userdata walk;
`5ad5702f5` reapplies saved attachment offsets; `e80859d18` restores the disabled flag;
`6909e3f61` waits for queued sample decoding at the loading boundary (984 pending samples,
1186 ms in the reference, all decoding remains async); `e466070c3` keeps inputs flowing until
the host completes its tick before saving for desync/rejoin; `52047631e` adds orbit/global
regressions. Split from tested combined source, not separately built. The Lua-owned detach,
join-quarantine and broader graph/world integration remain uncommitted.

The sound negative is `restore-tick-boundary-1/20260907_155325_4540f24f`: uninterrupted
playback tried to configure `Base.rte/Sounds/Craft/JetLoop.flac` in FMOD open state LOADING.
The preceding `43a73db1...` build passes `net-boundary-after-1` (D=0/3 desync/rejoin) and
`20260907_155253_heal_1687a3a4`. Native attempt `native-graph-sound-ready-1` used an invalid
flag and was stopped through its recorded owned PID; it is not coverage. The corrected -2 is.

**Still open:** the broader 16/20 result above, match completion, readable host rejection,
cross-peer snapshot equality, late failed-load rollback, remaining native contracts, H4 and
the full roadmap. HEAD `52047631e`, 150 ahead, unpushed. Unqualified build/test paths
are under `reviews/recovery-2026-09-07/`. Binary/PDB are retained under `sound-ready-ecf08c42/`.

**2026-09-07 / 15:48 UTC continuation — RED, changes not committed:** The broader
`stage2_p4/recovery_runs/20260907_150624_dcee8bc6` finished 16/20. `d3_stall` and
`brain_spawn` share the same premature Complete failure at tick 601; their common gameplay
traces match. The mismatch client reports the exact config rejection, while the host times
out without recording that reason. `snapshot_p5` differs in both Scene and LuaStateGraph.

Build `build-owned-detach-2.log`, executable
`ca5c1719fe8e12e16ee5464b1ac607b947aada139721aeed0a18abb5c1036991`, discovers Lua-owned
native trees directly in each VM, detaches their IDs before scene restoration, and reserves
incoming IDs before temporary reader allocations. Live global healing no longer reports
any duplicate IDs. File/ordinary capture-50 graphs and the later muzzle-flash positions
now match. `restore-owned-detach-2/20260907_153211_ec63f75c` remains RED: capture 60
misses the controller-disabled flag, and later cases report FMOD sound-not-ready errors.
All their tick traces and final native dumps match. Original prediction invariance
`20260907_153211_invariance_6cfd186f` passes 8/8 and detects the Lua-only fault.

The retained archive in `20260907_153850_heal_43fa1c0a/fresh/e2e/resync_heal/host/`
proves the live orbit failure: the save at tick 61 contains dropship 1049088 at y=-1003.2222,
ToDelete=1, after travel and before its orbit callback. Resync was saving a partially updated
tick. The client receives identical archive bytes and inherits the missed event. The heal
wrapper now checks console errors and retains transferred archives.

In progress: defer desync/rejoin stops until the host completes the simulation tick, preserve
the actor quick-disable flag and join-quarantine queue, and identify the exact unloaded
sound. `build-resync-boundary-1.log` failed on a missing diagnostic include; the corrected
`build-resync-boundary-2.log` is running. No new result is claimed for that source. Added
loopback cases cover continuing inputs until the host's boundary at D=0/3; they have not run
yet. HEAD remains `1249a9c38`, 143 ahead, unpushed. The completion race, snapshot equality,
late failed-load rollback, broader native contracts and full roadmap remain open.
Unqualified restoration/build paths are under `reviews/recovery-2026-09-07/`; expanded
healing/invariance paths are under `reviews/recovery-2026-09-06/expanded-mod-gates/`.

**2026-09-07 / 15:16 UTC expanded orbit control — RED:** The working tree fixes the
inverted `GlobalScript::HandleCraftEnteringOrbit` started guard. Valid negative
`global-events-before-3` shows a never-started script receives an event, while a running one
misses it. `global-events-after-1` and `global-events-late-after-1` pass native pause/orbit/end
and memory/file callback continuation. Build `build-global-event-fix-1.log`, executable
`d42d300f08960258a6be9375eefc571575ea5b1cb20a3dbd11476ecb2dec2824`. These changes are
not committed yet.

The enabled global fixture now spawns a real dropship at update 60 and requires exactly one
orbit callback before deactivation at 70. `restore-global-orbit-after-1/20260907_150544_a1a47e37`
has a valid reference and passing memory cases, but all six file/ordinary cases fail full-state
comparison despite matching tick hashes. At 50 the four threaded Lua graphs contain extra
callback records after restoration; at 100/400 muzzle-flash UID 1049151 changes ParentOffset
from (16,1) to the firearm's MuzzleOffset (7,9) in HDFirearm::Create. The latter remains wrong
in the final dump. `restore-global-orbit-at60-before-1/20260907_151215_0e5583ed` stops at an
invalid reference because FMOD reports two sound/DSP-not-ready errors; do not count it as a
capture-60 negative.

Under `reviews/recovery-2026-09-06/expanded-mod-gates/`, `20260907_150544_heal_f861b1a6`
passes the existing hash lane but FAILS global semantics: neither peer sees the orbit callback,
and the client's console contains duplicate native-ID errors. The wrapper needs to check console
errors as well as stdout. Both `20260907_150607_invariance_c1ffe62b` (original pickup/fire) and
`20260907_150607_invariance_6b49ebc2` (global isolation) pass 8/8 and Lua-only fault detection.
Native `native-graph-global-event-1` passes the full suite. Broader run
`stage2_p4/recovery_runs/20260907_150624_dcee8bc6` passes its first 11 gameplay lanes but
reproduces d3_stall: host completes cleanly; client rejects `Complete:e2e complete` while asking
for tick 601 with `running_ticks=599`. No crash. `d3_stall_repeat` passes. Additional lanes are
still running; this is not a full suite pass. Unqualified paths are under
`reviews/recovery-2026-09-07/`. Current working source is preserved by the restoration suite.

Next: repair the measured restoration failures and completion race; keep global/orbit and full
console checks enforced. Late failed-load rollback, remaining native contracts, graph integration
commits and the full multiplayer roadmap stay open. HEAD is still `1249a9c38`, 143 unpushed.

**2026-09-07 / 14:54 UTC activity-continuation checkpoint:** `1249a9c38` skips activity
script reload/startup while resuming a full SG3 checkpoint with saved global instances; native
scene/player setup still runs before Lua continuation restoration. Legacy saved-field startup
semantics remain in place. Staged save parsing no longer executes activity files in the live VM.
`activity-startup-before-1` is the retained valid negative: SimBaseline repeated startup and
collided with restored actor IDs. Both `activity-startup-after-1` (SimBaseline) and
`activity-save-startup-after-1` (P4AlphaDuel) now pass, including an immediate second save/load.

**Built and run:** `build-activity-startup-1.log`, executable
`cbb35bb64b573ea2dcdeb0017fe22300a91731872ecb03999e3a78dc57130239`, retained with PDB at
`activity-startup-cbb35bb6/`. `restore-global-startup-after-1/20260907_145212_029f8bd7`
passes 10/10 through 521 (memory/file/ordinary loads at 50/100/400, four Lua states, pending
paths and enabled global). Under `reviews/recovery-2026-09-06/expanded-mod-gates/`,
`20260907_145212_heal_06df3709` passes live global healing through 600,
`20260907_145212_invariance_8324b0e5` passes original 8/8 pickup/fire invariance, and
`20260907_145310_invariance_3902c7da` passes separate 8/8 global isolation; both invariance
suites detect the Lua-only fault. All four suites are complete with unchanged source.
The commit was split from the tested combined source, not separately built.
Unqualified paths are under `reviews/recovery-2026-09-07/`.

Additional controls on the preceding `2e0d53de...` build: `save-global-final-1` and
`save-menu-global-final-1` pass 5/5 each; `load-global-final-1` passes all 16 staged load
controls. These are archive/staging coverage, not a late graph-failure rollback test.
Next: global craft-orbit and callback breadth, late failed-load rollback and remaining native
contracts, then normal MP lifecycle and the full roadmap. Graph integration is uncommitted.

**2026-09-07 / 14:45 UTC global-script checkpoint:** `a65409b56` reapplies a saved arm's
hand offset after AHuman construction (which moved an empty hand to its holster). `ab049bcee`
retains native global-script instances in saves and binds fresh native instances before their
Lua fields are restored, without re-running their startup. `6f88661cf` adds an enabled-global
fixture, and `0c3d75d7a` checks an immediate second save of a cached activity closure. The graph
integration now captures activity callbacks even when removed from the Lua table, global-script
activation/start/late-update flags, and native global references by their activity-owned index.
**That graph integration is still uncommitted.** The focused commits were split from the tested
combined source and were not separately built.

**Built and run:** `reviews/recovery-2026-09-07/build-arm-offset-1.log`, Final x64/GNS,
executable `2e0d53de7b12f3f43e2ff7b6688a6fb9985ba5e2db944174c6433635d41f41e9`;
retained executable/PDB in `global-callbacks-2e0d53de/`. `restore-global-after-2/20260907_143350_f0912b99`
passes all 10 cases: uninterrupted + memory/file/ordinary loads at 50/100/400 through 521,
with the sustained mod fixture, four Lua states, an enabled global script and unpublished paths.
The global starts once, spawns one actor, then deactivates at 70; the two captures straddle its
native activation change. `save-callback-arm-final-1` passes immediate repeated save/load;
`native-graph-arm-final-1` passes the complete native suite including SG1/SG2 controls.
Under `reviews/recovery-2026-09-06/expanded-mod-gates/`, `20260907_143854_heal_954e4ce5` passes
live healing through 600 with five initial actors and exactly one global startup on each peer.
`20260907_143854_invariance_ebc75387` passes the original pickup/fire timing and 8/8 isolation;
`20260907_143854_invariance_1c4905aa` separately passes 8/8 isolation with the additional global
actor. Both detect the deliberate Lua-only fault; all suites report complete and unchanged source.
Unqualified paths above are under `reviews/recovery-2026-09-07/`.

**Retained failures:** `save-callback-repeat-before-1` lost the second activity callback.
`restore-global-before-1` had an incorrectly indented fixture module and did not exercise a global
script; it is not global coverage. The corrected `restore-global-before-2/20260907_142155_e8c77ff2`
had a passing uninterrupted run, a memory funds divergence at 51 and duplicated global startup on
file/ordinary loads. `restore-global-after-1/20260907_142904_060464e9` fixed global state but exposed
the arm's `hoff` reset in the full dump (tick hashes matched). At 14:33, the global healing runner's
old four-actor limit rejected a valid five-actor world; its explicit initial count and startup-count
checks are now fixture-aware. Adding that actor also changed the fixed pickup timing; the original
pickup/fire gate remains required on its unchanged starting world, and the global run separately
tests isolation. Neither earlier failure was silently accepted.

**Next:** global callback breadth (including craft-orbit dispatch), activity-start continuation,
late load-failure rollback and remaining native contracts; commit the graph integration by concern,
then continue normal multiplayer lifecycle and the full roadmap. No pushes.

**2026-09-07 / 14:14 UTC binding-error checkpoint:** `3bf3c0fd5` corrects Windows
exception unwinding for LuaJIT errors crossing the C API. `/EHsc` assumed those calls could
not throw, leaving a stale destructor state and double-freeing a C++ error string. The engine
and luabind now use `/EHs` (MSBuild and MSVC Meson settings). The valid control
`binding-errors-before-1` crashed; `binding-errors-after-2` passes 401 invalid method, free-function,
constructor/property and coroutine calls on each of four actors, followed by GC and valid calls.

**Built and run:** `reviews/recovery-2026-09-07/build-binding-unwind-1.log`, Final x64/GNS,
executable `b5b61819beff65343e409a4f18346337b45cbdc8a52507ef4cdabcf165e3ff9c`;
retained executable/PDB in `binding-errors-b5b61819/`. All 7 restoration cases pass through 521
at `restore-mod-binding-unwind-1/20260907_140704_602f68ac` (captures 50/400, memory/file/ordinary
load, unpublished path results). Under `reviews/recovery-2026-09-06/expanded-mod-gates/`,
`20260907_140704_heal_5f223a27` passes live healing through 600 and
`20260907_140704_invariance_90f6878a` passes 8/8 with Lua-only fault detection. All 12 fresh
multiplayer gameplay/replay lanes pass under `stage2_p4/recovery_runs/20260907_140704_5129a53c`.
Restore/heal/invariance suites report complete and unchanged source. The commit was split from
this tested combined source, not independently built; MSVC Meson and other platforms are not
newly verified. Unqualified paths above are under `reviews/recovery-2026-09-07/`.

**Next:** reproduce immediate repeated-save and activity/global callback continuation; then late
restore-failure rollback and remaining native contracts, normal MP lifecycle and the full roadmap.
The Lua graph/world/queue integration remains uncommitted. No broad completion claim; no pushes.

**2026-09-07 continuation / 13:59 UTC checkpoint:** The user requested completion of the
whole roadmap, continued local commits and live resume maintenance. Original source/binary/pointers
are preserved at `reviews/recovery-2026-09-07/starting-state/`. `ea3bea15e` fixes pending-object
ownership during purge, including objects spawned by Destroy callbacks and recursive purges.
`b65732e6b` runs activity/object OnSave callbacks before scene cloning, bitmap copying and state
serialization. Image copying still runs in parallel with serialization.

**Built and run:** `recovery-2026-09-07/build-purge-fix-1.log`, Final x64/GNS, executable
`e0e5dc238a7b53e1e53098544d8db99d42de74a22009b0c5a4a9132d7b8ed987`.
The source/binary checkpoint is `recovery-2026-09-07/save-callbacks-e0e5dc23/`.
On that combined source: `save-callback-after-4`, `purge-after-1` and `purge-after-reentry-1`
pass without script errors; `restore-mod-save-callbacks-1/20260907_135439_d267c3e5` passes
7/7 memory/file/ordinary-load cases at 50 and 400 through tick 521, with unpublished path results.
`recovery-2026-09-06/expanded-mod-gates/20260907_135440_heal_ee214b1c` passes real two-peer
healing through 600; `20260907_135439_invariance_3896667c` passes all 8 cases and catches the
Lua-only fault. All three suites report complete and unchanged source. `save-io-callback-1`
and `save-menu-callback-1` pass 5/5 each. The split commits were not independently built.
All paths above are under `reviews/`; unlabeled new paths are under `recovery-2026-09-07/`.

**Retained failures and next action:** The original `save-callback-before-1` failed path
resolution before exercising callbacks. Correct module resolution produced a valid ordering
negative (`save-callback-before-4`: images/scene/activity lost). The first correct P4AlphaDuel
control (`save-callback-after-3`) then exposed queued actors surviving the old purge and
colliding with restored IDs. `purge-before-2` cleanly reproduced pending/Destroy-spawn leaks.
A fixture that cast an owned clone before AddActor hit the binding's ownership check; reporting
that Lua error double-freed a C++ string during Windows unwinding. The dump, executable and PDB
are retained at `purge-before-debug-1/` (binary `ed0f903a5d2507888db454ef1df1b2030394aa3deffd82f33fcb2895d2082997`).
The corrected purge fixture adds its owned clone directly; the binding-error crash is repaired
by the 14:14 UTC checkpoint above. Next are repeated save/load activity and global callback closures,
late restore-failure rollback, remaining native/VM contracts and normal multiplayer lifecycle.
Native graph/world/queue integration remains uncommitted; no broad mod or MP completion claim.
All pushes remain gated on explicit user authorization. **Superseded 2026-09-09: routine pushes of verified development checkpoints to origin after major fix groups are authorised (no PRs, no rewrite, no attribution); see the WORKFLOW CHANGE note at the top of section B.**

| | |
|---|---|
| **Active branch** | `stage2/p4b-interp-lockstep` @ `1249a9c38` (recovery in progress 2026-09-07) |
| **Worktree** | `D:\Projects\p4b-interp-validation` — DIRTY: unfinished Lua graph/native-object checkpoint work; preserve and finish it |
| **Push state** | **143 commits ahead of origin, NOT pushed** (origin `1ab7fc2e1`, tip `1249a9c38`). Local per-concern commits remain the cadence; pushing requires the user's explicit OK. |
| **Next action** | Repair the extended-orbit restoration failures and stall completion race, then late restore-failure rollback and remaining native contracts. Commit the graph integration by concern, then continue §B-1 through the whole roadmap. Flawless normal MP precedes the 100–200 ms experience/performance work. |
| **Current evidence** | The expanded 15:16 UTC orbit fixture is RED on `d42d300f…`; details above. The 14:54 UTC checkpoint is the preceding exact-build evidence (`cbb35bb6…`): activity startup/repeated-save controls, 10/10 global/mod restores, live global-script healing, original pickup/fire invariance and separate global isolation pass. Earlier full gameplay, catalog/load and menu rows retain their separate build provenance. Broader native contracts and normal MP sign-off remain open; graph integration is uncommitted. |
| **Preserved handoff** | `reviews/recovery-2026-09-06/starting-state-212834Z/manifest.json`: HEAD, binary, tracked patch, all 19 changed/untracked source files, original report/harness and planning pointers. Recovery evidence gets unique run directories; prior failures are retained. |
| **Gates at `6097451c8`** (`chain11` RUNNING on the rebuilt tip: text-save 14, script-state 9, boundary, gameplay, trust, fuzz, probe 300, in-memory sweep) | **On the working states that became these four commits — `chain9` on the start-frame + alarm build (binary 07:12):** boundary **7/7** (`boundary_gates_20260906_071410_31736`; the `ai_writes` peer compare is green again with the alarm point classified per-machine, `c4ffe43a8`), gameplay **17/17** (`gameplay_20260906_071715_45788`), text-save 12/12 (`text_save_20260906_072200_46252`), trust 22/22 (`test_trust_20260906_072324_45776`); script-state 8/9 — the heal e2e itself 4/4 (the start-frame fix `900877aba` ended the `MissingFrameTimeout` deadlock) but the healed actors' `Update` counters froze at the save-time value 59: the runtime-added script was not in the file (`script_state_20260906_071338_5384`, root cause + fix `45afb0d1f` under 3d). **`chain10` on the script-set build (binary 07:26): text-save 14/14 incl. the two toggle legs** (`text_save_20260906_072642_41192`: `scr=1afdf9d3…` at 150 with the second script disabled, `…f415` at 250 re-enabled, both restored byte-identically) · **script-state 9/9** (`script_state_20260906_072823_23652`: the healed round's tick 400 reads `1/397/397/0` on both peers — Create once, Update carried straight through the heal, the spawn-time value kept). |
| **Gates at `5af465336`** (binary 06:55, `chain8` complete) | text-save 12/12 (`text_save_20260906_065057_49976`) · trust 22/22 (`test_trust_20260906_065803_45944`) · fuzz 17/17 buy + 10/10 pickup · probe 300 PASS · in-memory sweep **16/16** at every 50th tick 50-850 (`probe_sweep_20260906_065057`) · three reds, all closed by the commits above: script-state heal deadlock (`MissingFrameTimeout` at tick 62 — the healed round's coordinator still expected frame 1 while the verbatim restore kept the sim count), gameplay 15/17 (the checker could not parse the new `reloading=0/0` dump field — `check_fixture.py` fixed), boundary `ai_writes` (the new `atmr=`/`lastalarm=` dump fields exposed the per-machine alarm point, host `0,0` vs client `(1120,719)` at tick 411). **text-save fidelity (item 3d) on the working tree that became these nine commits (binary 06:44): the file-mode probe is byte-identical (hash + full dump, Lua state included) at buy 50/100/150/250 and pickup 50/100/200/300/400/500** (`filesweep_20260906_064345`; buy ≥300 is refused by `SaveCurrentGame` because the duel is over at 281 — a fixture limit, the in-memory sweep covers those ticks). The nine commits were split by concern from one working tree and were NOT built individually; the tip was. |
| **Gates at `ceffd131f`** (binary 04:27; runs since it) | rollback fuzz 17/17 buy (`fuzz_buy_radius_20260906_043002`) + 10/10 pickup (`fuzz_pickup_radius_20260906_043002`) · **the single probe 583:30 that failed on every earlier tip now PASSES** (`probe583_trk/run4.json`, byte-identical hash + full dump; root cause and fix under item 2 below) · script-state battery 9/9 all three legs (`script_state_20260906_043124_49696`, item 2d closed) · `chain7`: gameplay battery **17/17** (10 e2e + 7 semantic, `gameplay_20260906_043255_46848`) · trust battery **22/22** (`test_trust_20260906_043738_14660`) · boundary gates / probe 300 / a 16-window probe sweep at every 50th tick of the buy recording RUNNING — the row below is the last complete chain. |
| **Gates at `dcf44eaa1`** (`chain6`, binary 03:52) | gameplay battery 10/10 e2e + 5/5 semantic incl. `pie_reload` at the SP timing and `ai_orders` with replicated pops (`gameplay_20260906_035333_47260`) · script-state in-memory leg GREEN (`script_state_20260906_035245_32308`) · trust 22/22 · boundary 7/7 · fuzz 17/17 + 10/10 · probe 300:30 PASS — the one red: a fresh single probe at window 583 of the buy recording (`FIDELITY FAIL: dump divergence at 584`, brain FG leg + foot) while the 17-window fuzz covering 583 passed; bisected to be independent of the chain6 commits and of the user-data files, then root-caused 04:20 (item 2, fixed `187ce5230`). |
| **Gates at `d3b6b5360`** (binary 03:20; every dir below ran on it — `chain5`) | trust battery 22/22 (`test_trust_20260906_033120_*`; predicted pickup 144 / shot 161 exact) · boundary gates 7/7 (`boundary_gates_20260906_032742_48964`: 9 selftests incl. codec v10, legacy replays 10/10, D=0 tick-600 `88c5ecc5…` unchanged, pickup_fire D=3 18/18, seat switch 4/4, AI writes 6/6, aim speed 4/4) · gameplay battery 10/10 e2e green incl. deliver/buy/pause/ai_orders at D=3 (`gameplay_20260906_032053_46452`; the two semantic misses were the checker's timing and the single-dummy control, both re-cut — next run pending) · rollback fuzz 17/17 buy (`fuzz_buy_v7_20260906_032053`) + 10/10 pickup (`fuzz_pickup_v7_20260906_032053`) · probe 300:30 PASS (`probe300_aiorder_20260906_032053`) · state inventory 852 / 0 unclassified · the same chain was green on `0436edb59` (`chain3`) and `e66bd25d1` (`chain4`) · matrix / GNS-disabled / fake-lag NOT re-run on this tip (item 5a) |

**06:47 UTC pending-path checkpoint:** A valid uninterrupted control passed while memory, file
and ordinary Load Game each lost unpublished path results at tick 62
(`restore-mod-path-publication-before/20260907_063804_7fbdd2f9`). The working integration now
captures pending request scenes and parameters, restores callback IDs and closures, and resubmits
the requests asynchronously after graph restoration. Worker completions enter a private buffer;
only main-thread publication changes the captured state. Original and preview worlds retain
separate queues. The publication barrier does not block workers; actual worker/scene teardown
coverage is still open. All 7 pending/future-request cases and the delayed-publication live heal
and invariance suites pass at the paths above. Normal-delivery 7/7 and default healing/invariance
also passed before the future-request fixture extension (`restore-mod-path-pending-default-1/20260907_064153_51a5c5f8`,
`expanded-mod-gates/20260907_064153_heal_3bb4bf93`, `20260907_064153_invariance_1bdc62d3`).
`745393914` commits the fixture and runner only; native graph/queue code remains uncommitted.
These split commits were not independently built. All evidence paths are under
`reviews/recovery-2026-09-06/`; normal MP lifecycle and the rest of the roadmap remain open.

**07:09 UTC queued-worker lifetime checkpoint:** The paused-pool control recorded
`queued=0 completed=1`; a separate control destroyed a pathfinder while its real request remained
queued (`path-queue-before-1`, `path-lifetime-before-1`). `f9162248c` counts work from submission
through callback completion, releases the count on failed submission/unwinding, and waits before
clearing pathfinder or scene storage. Worker solves remain parallel. The same tests now pass,
including destruction waiting for the queued job; pending/future restoration, live healing and
prediction invariance pass on the exact working build above. The commit was split from that
combined source, not independently built. The native test helper remains in the uncommitted graph
integration. Fresh D=0, D=3 prediction and buy lanes pass under
`stage2_p4/recovery_runs/20260907_070723_6e6f57b0`; **D=3 delivery crashes the host near tick 379**.
A repeat at `20260907_071247_64cfc711` fails at the same point. The first-fault full dump at
`reviews/recovery-2026-09-06/delivery-crash-debug-3/fresh/e2e/d3_deliver_cargo/host/crash.dmp`
shows `lua_rawgeti -> luabind::detail::unref -> object_rep::garbage_collector`: a borrowed Actor's
dependency reference holds a released coroutine state. Its current GC state is valid. The exact
binary/PDB are preserved at `delivery-crash-bbf5ac1d`. This was RED before the correction below.
Normal MP lifecycle and the rest of the roadmap remain open.

**07:46 UTC coroutine lifetime checkpoint:** `642a5c31c` anchors luabind's strong and weak
registry references to the VM's main state. The native regression first failed on the old binding;
it now proves that the coroutine is collected while its values remain usable. Delivery and the
other 11 gameplay lanes, all 7 restores, live healing and 8/8 invariance with Lua fault detection
pass on the exact build above. The code was split from that working integration, not independently
built. Rematch and two fresh stall checks pass at `stage2_p4/recovery_runs/20260907_074501_164af117`; the intermittent stall failure is not yet considered root-caused.
Native graph/queue integration, remaining native contracts and normal MP lifecycle remain open.

**08:10 UTC menu/shutdown checkpoint:** `861ec16de` preserves scripted menu results through
Update and finishes the intro before normal transitions run. Quit then reproduced the FMOD crash
on all six normal attempts. The opt-in full dump at `menu-quit-native-dump-1/crash.dmp`
shows the main thread inside `FMOD::System::release` and a loader worker accessing a cleared
internal parent pointer; `getUserDataInternal+0x16af` is merely the nearest export, not evidence
of a SoundContainer userdata fault. Binary/PDB/DLL are retained at `menu-crash-56df357d`.
`76ac4ebfd` explicitly releases cached sounds before their audio system, then clears the cache.
`build-audio-release-1.log` and `menu-audio-release-after-1` pass all 9 cases; the checked-in
`tools/test_menu_lifecycle.py` passes 4/4 at `menu-lifecycle-regression-1`, including Quit and
credits. Binary `913baa5b46a8f74969578210dd4243398a17e7a3c80cd888aeb02e25f895f2b4`.
The focused commits were split from verified combined source, not independently built. Normal
match/scenario/replay paths still bypassed full teardown; removing that bypass and verifying
actual cleanup is now in progress (`build-full-shutdown-1.log`). Native graph/queue integration
and broader contracts remain uncommitted/open. Evidence paths in this paragraph are under
`reviews/recovery-2026-09-06/`; normal MP sign-off and the full roadmap remain open.

**08:24 UTC full-cleanup checkpoint:** `3a3816104` adds opt-in native crash dumps. The
working tree now removes full-teardown bypasses from match, replay and scenario paths. Initial
full cleanup passed D=0 and delivery (`stage2_p4/recovery_runs/20260907_081347_cb958d56`) but
crashed after both memory probes. `restore-memory-shutdown-dump-1/memory/crash.dmp` proves
` s_rbProbeWorld` was destroyed at process exit after Allegro; the exact binary/PDB are in
`cleanup-crash-87e3511d`. Clearing retained previews and probe snapshots before their dependencies
fixes both cases. `build-full-shutdown-2.log` / `restore-mod-full-shutdown-2/20260907_082003_b69df035`
pass all 7 restores through 521 and full exit. Binary `5994727b1aefe392eae45af5cf39751fbb82efafe676325e48367270f04275f9`.
`menu-network-start-2` passes a real three-player GUI launch and all 180 trace ticks; the preceding
two-player run also matches all 180 ticks (`menu-network-start-1/trace-recheck-180.json`, the
initial checker incorrectly requested 181). This proves handler-driven transitions, not physical
mouse/keyboard input. A final guard for quitting inside a probe has changed since that build and
still needs rebuilding. Full cleanup remains uncommitted. Screenshot success/error reporting
fails all 3 controls (`screenshots-before-1`); leaving a lobby still drops the host to Landing
(`menu-client-leave-before-1`). Both are now reproduced for repair. Other evidence paths above
are under `reviews/recovery-2026-09-06/`. Normal MP sign-off, native contracts and the roadmap remain open.

**08:34 UTC cleanup/PNG checkpoint:** `0eac8c8f6` restores full match/scenario/replay
shutdown and clears retained previews/probe snapshots before their dependencies. `d46ff4a1e`
queues owned screenshot copies on the background pool and correctly interprets SDL3 save results.
`build-screenshot-save-2.log` / `screenshots-after-1` pass 4/4 including success and failure followed
by immediate exit. `restore-mod-cleanup-final-1/20260907_083025_f232a5ba` passes 7/7 through 521;
`stage2_p4/recovery_runs/20260907_082958_afd05411` passes D=0 and D=3 with their playbacks.
`scenario-cleanup-final-1` exits cleanly; `probe-incomplete-cleanup-final-2` reports incomplete
phase 1 and exits 1 as expected. The preceding `probe-incomplete-cleanup-final-1` used an invalid
short option, so it is ordinary playback only. Binary
`1737036c47df78c39ad06f790d1dc7aa54a438aff2ac65effac50d344265532f`.
These commits were split from the verified combined integration, not independently built.
Other initialized diagnostic exits, world/preview PNG conversion and host lobby survival are next.
Native graph/queue integration and the full roadmap remain open. Other evidence paths above are
under `reviews/recovery-2026-09-06/`.

**09:11 UTC lobby checkpoint:** `427ee74fb` keeps the host listening after individual
connections leave or are refused; replacements pass after a clean leave, malformed join and
handshake timeout. `build-lobby-session-1.log`, `lobby-session-after-1`,
`lobby-admission-session-after-1` and `menu-session-start-1` pass, including 180 ticks on three peers.
The uncommitted lobby integration forwards session traffic while polling once, refreshes membership
and names, binds messages to their connection, broadcasts presence (lobby protocol v3), and restarts
partial state transfer for replacements. Its first native/GUI runs exposed valid short lobby Ready
packets rejected as session ShortHeader errors. Recognizing valid other-phase packets independently
of that initial error fixes the regression. `build-lobby-phase-routing-2.log` and
`lobby-membership-native-3` pass; `menu-client-leave-after-2` and `lobby-rejoin-hang-2p-1` pass.
`lobby-rejoin-3p-1` launches and host/stayer traces agree through 180, but the replacement times out
without its trace. `lobby-rejoin-hang-3p-1` is capturing a reproduction. D=3 also passes at
`stage2_p4/recovery_runs/20260907_091002_26d1947e`. Current binary
`0cb2c0d30c332354c07418bfe0ce7057e07282017ca767d9c2a47596d5e18f7c`.
The native suite also preserves all 245777 state-transfer bytes through replacement and isolates
unbound/forged readiness. Lobby integration and native graph remain uncommitted. Full roadmap and
normal MP sign-off remain open. Other evidence paths above are under `reviews/recovery-2026-09-06/`.

**09:26 UTC replacement/cap checkpoint:** `1d6059c5d` commits the lobby integration and
native checks; `7154f8b27` adds the owned-process GUI lifecycle fixture. The second three-player
reproduction (`lobby-rejoin-hang-3p-2/replacement/hang.dmp`, exact binary/PDB in its `symbols/`)
shows RunMenuLoop, service Completed, sim count 181 and 180 saved hash records. A peer Complete
arrived during the unnecessary extra tick. `5a6048cf3` stops at the completed trace count and
makes partial capped runs write a failed report and exit 1. The current build and gates above pass.
The short-trace control collected 182 of 360 requested ticks, explicitly failed, and matched the
host's 180-tick prefix. This is an expected failure control, not full-trace proof. The delayed run
uses `-net-fake-lag 50`; observed RTT remains in its logs. These commits were split from the tested
combined integration, not independently built. Other earlier intermittent e2e cap failures have
not been separately root-caused by this menu-specific correction. World/preview PNG conversion,
remaining diagnostic cleanup, native graph contracts and the whole roadmap remain open. Evidence
paths in this paragraph are under `reviews/recovery-2026-09-06/`.

**09:43 UTC PNG/diagnostic checkpoint:** The new native bitmap fixture in the working tree
crashes the old saver on its first cropped 8-bit bitmap (`bitmap-saves-before-1`, build
`build-bitmap-save-control-2.log`; build 1 failed compilation on a const BITMAP signature).
`build-bitmap-save-fix-1.log` / `bitmap-saves-after-1` fix that crash, row pitch, actual color-to-index
conversion, owned buffer cleanup and save/error results. All six source RGB pixel comparisons and
the complete 2496x1200 world image now match; the indexed/preview format checks remain RED because
SDL_image's miniz writer expands indexed input to RGBA. Binary
`223e1df1af0b65ad35382bfdac35c2356edcfe035edbfe84eec7ed8a644e59ee`.
The default palette has identical RGB at indices 28/30. SaveCurrentGame uses the same writer, so its
index identity must be verified and preserved; a native/full-save control is next. The vendored STB
reader DOES preserve indexed PNGs (IMG_stb.c); changing image-loading backends is not needed.
`graph-full-cleanup-1` passes every native graph test and normal teardown on the preceding control
build, binary `434532f60a39b82f3b0737a7eff4a89445b5cc3f8c8faee2d69f44058330663c`.
Those diagnostic-exit changes are uncommitted. Menu-script failures and invalid CLI arguments still
bypass cleanup, and replay verification still returns success if report writing fails. Close and
verify those cases too. Source is currently idle with no games/builds running. Native graph/queue
integration, normal MP sign-off and the full roadmap remain open. Evidence paths above are under
`reviews/recovery-2026-09-06/`.

**10:00 UTC indexed-save checkpoint:** `6fd15b1d8` skips placement cleanup for restored
terrain; `3d67ec781` preserves PNG palette indices with a single libpng compression pass and fixes
cropped/conversion/output handling. `material-save-before-1` loaded index 30 as 28; after the PNG
fix only cavity 1 still became 0 (`material-save-after-1`). Removing snapshot placement cleanup
makes `material-save-after-2` pass every RGB/index check and all 256 values through a real save/load.
Build `build-terrain-restore-1.log`, binary `342a23bf37ebe276b58c8a5b8b11f07aa55e7d6446d66043c8eb706996ab4313`.
Live healing also passes at `expanded-mod-gates/20260907_095349_heal_d6694afb`.
The first restoration control failed before starting because lobby v3 rejected older recordings
(`restore-mod-indexed-png-1/20260907_095349_b9ef2dcd`). `e47da4114` upgrades the unchanged recorded
v2 MatchConfig on read; live decoding still rejects v2. `build-replay-lobby-compat-1.log`,
`replay-lobby-native-1`, `restore-mod-indexed-png-2/20260907_095625_82d9f842` (7/7 through 521), and
`expanded-mod-gates/20260907_095625_invariance_3d228230` (8/8 plus Lua fault detection) pass on
binary `7df9ee9002523704df4894dd79770d23d4938f6cacfa7a9b01caef05eba9f4f0`.
These focused commits were split from the verified integration, not independently built. Save I/O
still needs actual completion/error results and atomic publication; initialized diagnostic failure
exits are also open. Native graph/queue integration, normal MP sign-off and the whole roadmap
remain open. Evidence paths in this paragraph are under `reviews/recovery-2026-09-06/`.


**10:35 UTC save-completion checkpoint:** `d7d0f1360` reports the actual background save result,
checks every archive entry and bitmap, closes the complete archive before atomic publication, and
keeps the previous save after failure. The real save-menu handler waits for completion and reports
failure truthfully. `game-save-transaction-3` passes five I/O cases (success, overwrite, missing
folder, locked existing save and directory destination); `game-save-menu-1` passes the same five
through the actual menu handler and pending/completed controls. `7fe417453` runs normal cleanup
after initialized CLI/menu/replay/session diagnostic exits and rejects failed report writes.
`diagnostic-cleanup-after-1` passes 18 cases; `diagnostic-session-cleanup-2` passes two more.
The 18 diagnostics and five GUI cases used `build-save-menu-diagnostic-2.log`, binary
`68da47dc46b6d807d907605141efbb02c363bf7ccebd88e6b2fa186cda80df63`; the five I/O cases and
`material-save-transaction-3` used binary `21d2dbd80fc2cd64d704255918e525c3ab5783eb8daa9b827755ef7e9a54555d`.
Final-build gates and exact binary are in the current evidence row. Negative controls are retained
at `diagnostic-cleanup-before-1`. The first session test needed the explicit private-Userdata opt-in;
its refusal was correct and the fixture was corrected. Split commits were not independently built.
Corrupt/truncated load handling, remaining Lua/native graph contracts and normal MP lifecycle are
next; the full roadmap remains open. Evidence is under `reviews/recovery-2026-09-06/` unless shown
under `stage2_p4/`.


**10:56 UTC load-validation checkpoint:** `54d7a6947` checks archive reads, lengths and CRCs,
validates required images and terrain dimensions, and parses into temporary state. Scoped image
replacement restores the previous cache after failure. Recoverable Reader errors and headless
warnings avoid an abort or undismissable dialog. The negative control `game-load-before-1` had a
valid success leg, a malformed-PNG crash, accepted bad CRC/missing material, and a missing-manifest
timeout. `build-load-transaction-2.log` / `game-load-after-1` pass all 16 controls, including older
RGBA images, parse errors after image installation, preservation of a staged save and its successful
restart. The first build failed on hidden Create overloads; the corrected call uses Serializable.
All seven restores, live two-peer healing and full prediction invariance pass on the unchanged build
in the current evidence row. The focused commit was split from the tested integration, not built
independently. Save-menu Index.ini parsing still uses unchecked reads; OnSave/terrain-copy ordering
and later native graph restore failures remain under audit. Normal MP and the full roadmap remain
open. Evidence paths are under `reviews/recovery-2026-09-06/`.

**11:05 UTC save-menu checkpoint:** `7e733269f` shares complete archive entry reads between
the full loader and the parallel save list. `save-catalog-before-1` had a valid control, malformed
index abort, accepted bad CRC and a directory listed as a save. `build-save-catalog-1.log` passes;
`save-catalog-after-1` passes 8/8, `game-load-catalog-1` passes 16/16 and `material-save-load-1`
passes cropped/world/preview pixels and all 256 saved material indices on binary
`35b3759e1add4010d7f5bcdf9e1b375f6a85a7c2c0b5672f57daeffd27a0b3ca`. Unreadable metadata has a
clear row label while Load remains available to validate the full save. Directories are excluded.
The focused commit was split from the tested integration, not independently built. These are real
menu-handler/list controls, not physical input tests. Save callback timing, later native graph
restore failures, normal multiplayer and the full roadmap remain open. The native callback control
is being added on top of this build. Evidence paths are under `reviews/recovery-2026-09-06/`.

## Audit corrections at the 2026-09-06 handoff

The user requested continuation from the prior work. No separate author handoff is needed: the source,
binary, reports, and reproduction files are retained. Preserve mod compatibility, Controller-sync,
async off-wire AI, fixed simulation timestep, and the full goal. Do not freeze scripts or discard
closures/shared references/coroutines to make restoration pass.

- [x] **Trace/report trust (`4b5b1933d`):** strict comparison validates every hash, sequence and exact
  range; prefix comparison is explicit. Normal ticks require all core fields; reduced pause records
  carry an engine-emitted pause flag. Nine regression tests include missing/null/invalid hashes,
  missing/extra/repeated/reordered ticks and phase errors. Fresh D=0/buy/pause/brain-spawn peers and
  playbacks pass. The audit report now labels counter-only prediction invariance inconclusive and
  decodes images before embedding; its link check validates all 485 rendered images. The original
  report is preserved. Full prediction invariance still needs the actual selftest.
- [x] **Process isolation (`c5bed35b5`):** `tools/run_sim_test.py` launches the approved executable
  with private Userdata, Mods, saves, screenshots, logs, abort files and TEMP/TMP, muted settings,
  an unswitched desktop and an owned job. Native and peer runs retain executable hashes and
  observed input-desktop names. Old PowerShell launchers still require migration; use the new
  runner for unattended tests. No global process kills or firewall changes.
- [x] **Client command replay (`afa0e5628`):** the v5 checksummed envelope preserves each command's
  sender without changing live wire authentication. The previously divergent client brain spawn now
  matches all 600 ticks in playback; buy (900), pause (900) and D=0 (600) also match. The match selftest
  covers mixed senders, empty frames, checksum tampering, invalid sender IDs/counts and v4 semantics.
  A retained v4 baseline still matches 600/600; old formats remain readable, with their original
  semantics (client sender identity already lost in a pre-v5 recording cannot be recovered).
- [ ] **Lua restoration and failed-save cleanup:** the initial native-method/controller refusal and
  snapshot crash are repaired. The new sustained mod fixture passes memory and file restores at
  50/150/300/400, including the full continuation through tick 521. Baseline-library mutations and
  Lua-owned actors outside the resident world now pass the expanded checks below. Broader object
  lifetime/global-environment contracts, full live heal and prediction invariance remain open.
- [ ] **Stall and menu lifecycle:** preserve and reproduce the intermittent tick-cap failure; close
  the FMOD quit crash, automation update-result loss, host failure when its client leaves, and
  detached/inverted PNG-save handling. An empty PNG remains missing evidence, never a valid image.
- [ ] **Re-gate current source:** full invariance, semantic gameplay/restore fixtures, two-peer and
  N-peer lifecycle, live player input and presentation, then breadth/cross-platform and §B-1 remainder.

Independent review: all 1,748 published file hashes checked match, 63 battlefield capture pairs are
present, and 38 retained trace comparisons show only the deliberate perturbation and reported client
replay divergence. This preserves the useful results without making the checker itself trustworthy.
The audit report's local HTML rendering was blocked by the in-app browser policy; static references,
image decoding and representative screenshots were checked, browser layout/interactions were not.

**01:37 UTC actor-order checkpoint:** `4ffea6085` preserves squad mode, ordered waypoint
positions and object IDs, current movement path and carried gold. The fixture first failed every
restore; `build-actor-orders-1.log` / `actor-orders-selftest-1` and
`restore-mod-actor-orders-1/20260907_012245_bb262357` then pass all 5 reference/memory/file cases
at 50/300 through tick 521. `expanded-mod-gates/20260907_012424_heal_5e7a301f` passes real
two-peer healing. `native-api-inventory-5` records 100 exercised writable properties out of 381;
the inventory remains OPEN. The commit was split from verified combined source, not independently
built in a clean checkout. A new future-spawn fixture next exposed delayed divergence at tick 403
after both 30-tick memory probes had passed. Allocation-cursor restoration fixes that continuation,
but now consistently exposes a retained-script-cache mismatch inside the rerun: a file loaded after
capture remains cached after its initialization globals are rewound. Callback/cache graph capture
is being built; the new fixture and allocator changes are uncommitted and not yet green. Evidence:
`restore-mod-future-spawn-baseline/20260907_012735_46adc382` and
`restore-mod-allocation-cursors-1/20260907_013422_2beb714d`. All paths in this paragraph are under
`reviews/recovery-2026-09-06/`. Normal MP lifecycle and the later roadmap remain open.

**01:55 UTC continuation checkpoint:** `df4bf3b4f` extends the fixture with scripted spawns at
counts 61/311/401, a VM-dependent position/state stamp, and replaced global Create/Update functions
whose existing native callbacks must keep their original closures. The working integration now
restores the script-state allocation cursor and UID counter for memory, reinstate and file paths;
captures cached functions plus ordered per-object callbacks; reapplies saved VM affinity to nested
objects during adoption; and releases the temporary callback root after capture and restore.
The callback inventory excludes original presets and unplaced save-template objects with pending
persisted IDs. Each correction followed a measured failure. A temporary-reference check first failed
both restore modes while the short probes still reported PASS; the suite correctly rejected them
because scripts stopped continuing. `build-script-callback-lifetime-1.log` and
`script-callback-lifetime-selftest-1` pass; all **5/5** cases at
`restore-mod-callback-lifetime-1/20260907_015032_f7b0610f` pass exact full traces, final native dumps
and Lua graphs through 521 ticks. `expanded-mod-gates/20260907_015228_invariance_a1d8ed12` passes
reference, 8/8 clean cases and Lua-only fault detection; `expanded-mod-gates/20260907_015228_heal_74d6244b`
passes real two-peer healing through tick 600. Binary SHA-256:
`5cce36d24c54286bbc06682457dc8aca5f9d1fd0f826ef94c3a5d0ec73a1ca37`. Source stayed unchanged
across these suites. The native graph/allocator integration remains **UNCOMMITTED**; the full
contract remains OPEN. Next: native construction/renamed objects, borrowed aliases, remaining
classes, compatibility reads, then the normal MP lifecycle defects and the rest of the roadmap.
The user's 01:50 UTC status request was answered: still milestones 2–3; planning estimate remains
several focused days for normal MP, weeks for the whole roadmap, explicitly low confidence.
All evidence paths in this paragraph are under `reviews/recovery-2026-09-06/`.

**02:26 UTC checkpoint:** `2d2dce1ab` preserves the source preset across cosmetic renames,
live names/descriptions and exact group membership; group names use a stable output order.
The working graph integration also handles bare Lua constructors and restores older per-object
`ScriptState` fields. Six native legacy checks pass; retained `rbprobe.ccsave` fields (archive
SHA-256 `ec3b003fea956b3d56b7bd442eea445967d022bef9a02538696c0f719229a0ba`) pass inside the
sustained fixture. This tests extracted fields and native field installation, not a full historical
save boot. Build/selftest, 5/5 restore cases, real two-peer healing and 8/8 invariance are green at
the evidence paths in the current row. Construction failures, empty-value encoding and group-order
mismatches remain retained. The commit was split from verified combined source, not independently
built. Next: ordinary Load Game currently bypasses the graph restore; then remaining native/API
coverage and MP lifecycle defects. The native state inventory currently reports 877 members and
one unclassified reader cursor (`ACraft.m_ReadExitIncomingCursor`), to reconcile. No full-goal sign-off.

**02:52 UTC checkpoint:** Ordinary Load Game now shares the staged snapshot restore and reports startup failures (`54683c126`); the new launch probe mode (`b22e7e1bc`) proves both capture points match uninterrupted play through 521. All 7 cases and live healing passed on the unchanged combined source cited above. The commits are local and were not individually built. Native geometry is the active extension: `restore-mod-geometry-before/20260907_024249_ba742db4` has a valid reference and all three restore modes failing on Area/Box capture. Owned boxes, borrowed aliases, scene-owned area state and area lifetime are being implemented; the build is in progress. The inventory now reports 877 members / 0 unclassified after resetting and classifying the reader-only exit cursor; this is inventory coverage, not full runtime proof. Full native contracts and normal MP lifecycle remain open.

**03:05 UTC checkpoint:** `361e86117` gives areas proper owned-box cleanup and deep assignment, with Lua box accessors retaining their owner. `d26613dc6` preserves stored signed dimensions, encodes area names in full saves and adds ordered scene-area capture/swap support. The first geometry build exposed negative widths becoming positive through Box assignment; raw member copying fixed the measured next-tick failure. The working graph bridge handles owned Area/Box values, references into areas, const box references and a hidden native owner. Scene areas also move with the set-aside world and are included in prediction canonical-state checks. All seven restore cases, live healing and full invariance pass at the current evidence paths. `1335c4d03` resets the craft reader cursor; the inventory is 877 / 0 unclassified. Fixture commits `56f7a5d23`, `68e4c934f`, `083e5d99a` preserve legacy fields, construction/identity and geometry coverage separately. All commits were split from verified combined source, not independently built. The large native graph integration remains uncommitted; remaining native references/classes, VM random-generator state and async/global callback contracts are still open. Normal MP lifecycle and the later roadmap are not signed off.

**03:25 UTC checkpoint:** `439fdcc8d` adds portable RNG state, seed and draw-count restoration. A new native test first failed (`vm-rng-selftest-before`); the working SG3 graph bridge now restores each VM's generator, and all current gates above pass. `rng-portability-1` compiles the actual RNG class and checkpoint methods with Windows MSVC and Linux g++; both write identical 44-checkpoint files (SHA-256 `592c8e5bf86c7bcf0aeaf7b9f5a1b9e041ba62ac000783cbe521783f086a038b`), read each other's files, match 10,000 following outputs per checkpoint, and reject invalid inputs without mutation. The component test is committed with the RNG foundation; the game integration stays uncommitted. This is component interoperability, not the full cross-platform gameplay gate. SG1/SG2 remain accepted; explicit positive legacy-format and invalid-RNG checks are being added to the native selftest. Remaining native references/classes and async/global callbacks, then normal MP lifecycle and the rest of the roadmap, remain open. The user's 03:12 UTC status request was answered: milestones 2–3, several focused days for normal MP and weeks for the full roadmap, with low confidence.

**04:08 UTC checkpoint:** `5df407ec1` preserves complete limb-path state; `bd080f517` retains limb and vector owners; `6b768d86d` adds borrowed-vector, hidden-owner and cyclic-reference fixtures. Measured failures exposed missing path configuration, physical timer anchors entering the canonical hash, and duplicate wrappers when child references preceded their owner. The working graph now handles these cases. `build-native-owner-order-1.log` / `native-owner-order-selftest-1` pass, including positive SG1/SG2 compatibility and invalid-RNG rejection. `restore-mod-native-owner-order-1/20260907_040043_42d5ec53` passes all seven memory/file/ordinary-launch cases through 521; `expanded-mod-gates/20260907_040339_invariance_46ccb4c6` passes 8/8 clean cases and Lua fault detection; `20260907_040339_heal_dec834ab` passes live two-peer healing through 600. Binary `c8edd7115bb0a49e4c0eb6f2a82f32ec75c99b56581aabe9bcae2ae02ec57db7`. Source stayed unchanged across these gates. Commits were split from verified combined source, not independently built. Inventory: 877 members / 0 unclassified. Native graph integration remains uncommitted; remaining native types, iterators and async/global callbacks, then normal MP lifecycle and the rest of the roadmap, remain open. Evidence paths are under `reviews/recovery-2026-09-06/`.

**04:23 UTC checkpoint:** Nested SoundSet copies now own their complete tree; saves preserve the current sound selection without the redundant outer cycle property resetting it. Sound-set accessors and native iterator results retain their owners. Owned/borrowed/nested sound sets, hidden owners and cyclic aliases pass `build-iterator-owner-1.log`, `iterator-owner-selftest-1` and all 7 cases at `restore-mod-iterator-owner-1/20260907_041703_2031f9c9`. Live two-peer healing passes `expanded-mod-gates/20260907_041938_heal_132a61c8`; full invariance passes 8/8 plus Lua fault detection at `20260907_041938_invariance_8ed642ae`. Binary `80fe9bf7819de24eb302c0f6247bb1aa0e75372db31082e8411f05bce54d84e8`. Source stayed unchanged across the suites. The native selftest also checks nested-copy independence, assignment from an owned child and reset. Inventory now includes SoundSet: 881 / 0 unclassified. The focused commits were split from the verified combined source, not independently built. Graph integration remains uncommitted; retained native iterators themselves, other native types and async/global callbacks remain open before normal MP lifecycle and the rest of the roadmap. Evidence paths are under `reviews/recovery-2026-09-06/`.

**04:45 UTC checkpoint:** Native iterators now preserve their owner and cursor, while owned query ranges carry their remaining results and release their C++ container. Borrowed iteration stays native. Captured loops resume at counts 61/311/401; owned waypoint results survive clearing the source before capture. Empty, exhausted and unstarted ranges are included. `build-native-iterator-2.log` / `native-iterator-selftest-2` pass, including iterator alias/continuation checks. `restore-mod-iterator-bounds-1/20260907_043947_5dd25d28` passes 7/7 through 521; `expanded-mod-gates/20260907_044129_heal_7cd9001e` passes live two-peer healing through 600; `20260907_044129_invariance_21476e9a` passes 8/8 and Lua fault detection. Binary `4e0c7ea1740dbaa2cac6019d1c4ce4f4f7204f24138a8036d2df9a49b1b339af`. Source stayed unchanged across the suites. The API and fixture were committed separately from the uncommitted J-node graph integration and were not independently built. The preceding valid-reference/all-restores-fail control is retained at `restore-mod-native-iterator-before/20260907_042624_8a569694`. Remaining native types, iterator breadth and async/global callbacks, then normal MP lifecycle and the rest of the roadmap, remain open. All evidence is under `reviews/recovery-2026-09-06/`. The user's 04:11 UTC status request was answered: still milestones 2–3; normal MP is not signed off; everything stays local and unpushed.

**05:05 UTC checkpoint:** Gib snapshots preserve all raw configuration fields and replace inherited lists exactly (`e477c1a06`); borrowed gibs and offset aliases are covered by fixture `a654e01e6`. Nullable particle references pass a native check, and save no longer clones the particle merely to write its preset name. The valid-reference/all-restores-fail control is retained at `restore-mod-gib-before-2/20260907_044944_8d631aed`. All 7 restore cases, live two-peer healing and 8/8 invariance with Lua fault detection pass on the unchanged combined source cited above. The split commits were not independently built. Inventory now includes Gib: 892 / 0 unclassified. Custom runtime particle targets and their alias semantics are the next measured extension; native graph integration, remaining native/iterator/callback contracts and normal MP lifecycle remain open. Evidence paths are under `reviews/recovery-2026-09-06/`.

**05:50 UTC checkpoint:** Custom runtime and bare gib particle targets, aliases and later gib spawning pass all 7 restores, live healing and 8/8 invariance with Lua fault detection. The Windows target now rebuilds luabind; the missing-constructor check passes on the linked library. Inventory 893 / 0. Graph integration, remaining native/iterator/async contracts and normal MP lifecycle remain open. Commits were split from verified combined source, not independently built. Runtime reference foundation `9077f3610`, build dependency `4a131d909`, constructor guard `668ab01d0`, fixture `14ab625d2`. Source stayed unchanged across the gates above. The user's 05:45 UTC status request was answered: still milestones 2–3, substantial work left, local and unpushed. All evidence is under `reviews/recovery-2026-09-06/`.

**06:04 UTC checkpoint:** Alarm values and borrowed positions, module references and iterators, and completed asynchronous path results now pass native checks, all 7 restores through 521, live two-peer healing and 8/8 invariance with Lua-only fault detection. Pending callbacks and remaining native/iterator contracts are next; normal MP lifecycle and the full roadmap remain open. Graph integration remains uncommitted; split fixture commits were not independently built. Fixtures `3d6f7235b` and `6b35602b8`; the valid-reference/refused-restore controls are retained at `restore-mod-alarm-module-before/20260907_055011_7917f98f` and `restore-mod-path-request-before/20260907_055713_ea379818`. Source stayed unchanged across the current gates. All evidence is under `reviews/recovery-2026-09-06/`.

**06:31 UTC checkpoint:** Queued path callbacks now retain their results, IDs, arrival order and world ownership in the working graph integration. Both normal delivery and delivery held through tick 59 pass all 7 memory/file/Load Game cases; live healing and 8/8 invariance with Lua fault detection pass. The new purge regression passes after cleanup was moved to scene/activity transitions. Pending worker results and remaining native/iterator contracts are still open, followed by normal MP lifecycle and the full roadmap. Reproducible delivery control and fixture checks are committed as `eb7753e5e`; the native graph and queue integration remain uncommitted. The valid-reference/failing-restore control is retained at `restore-mod-path-pending-before/20260907_060715_4be948ee`; `path-purge-selftest-before` records the additional compatibility failure. Source stayed unchanged across the current gates; split commits were not independently built. All evidence is under `reviews/recovery-2026-09-06/`.

## §B-1 THE COMPLETION CHECKLIST (assignment 2026-09-06 00:40: finish multiplayer and ALL remaining planned work)

The user's order: finish the whole live roadmap, not a milestone. Flawless normal MP first, then the
prediction/performance work for 100-200 ms ping. Nothing below is ticked by reasoning — every item is
BUILT and RUN and names its evidence dir. `[ ]` open · `[~]` in progress · `[x]` done · `[!]` blocked on
the user or another machine (the dependency is recorded, independent work continues). Rules stay: local
commits only, no push/PR/messages, no AI attribution, no disabled gameplay, no weakened tests, no
tolerance increase without a measured cause, Causeless's architecture and mod compatibility intact.

**Recovery checkpoint 2026-09-06 23:07 UTC:** `96a717ffe` adds the isolated restoration suite and a
sustained gameplay mod fixture. On `build-script-registration-swap.log`, all **9 cases pass**:
uninterrupted pickup/fire playback plus memory and file captures at 50/150/300/400, each replaying
30 ticks and continuing to tick 521. Every run validates the exact full trace, the tick-520 actor
dump, and closures/shared tables/Vector/Timer/SoundContainer aliases, ordinary/wrapped coroutines,
and gmatch continuation. Evidence: `reviews/recovery-2026-09-06/restore-mod-registration-swap/20260906_230226_36864b5b`.
Source files (including untracked codec files), patch, binary hashes and private launch records are
retained; source was unchanged across the suite. The native graph selftest also passes on the preceding
stitch build. The main trace now records each forward tick once; rerun windows have their separate
probe comparison, and success continues to the requested cap. Script update and pending lists move
with set-aside worlds, so originals cannot leak into the rerun. A LuaJIT stitch restores through its
interpreter continuation without carrying machine code; JIT remains enabled. Full Lua graph comparison
is being added to prediction invariance. Baseline-table/global overrides and Lua-owned nonresident
objects still need coverage and repair before the Lua contract is complete. Raw cross-peer snapshot
equality is still unclosed (including off-wire AI and render/cache state); its checker preserves all
repeated properties. Earlier false positives/refusals remain retained, including the INVALID
`restore-native-identity/memory` PASS that preceded reinstate-error propagation.

**23:28 UTC extension:** `build-lua-engine-table-state-8.log` and
`script-engine-table-state-selftest-7` pass the expanded graph selftest, including library-table
additions/removals, replaced functions and globals, protected metatables and table keys whose
`__tostring` must never execute during capture. SG2 stores changes relative to the pristine engine
baseline and retains SG1 decoding. Cached built-ins and stable original function references keep the
codec working when mods replace library functions. `restore-mod-engine-table-v2/20260906_232641_194fdfd8`
passes all 5 cases (reference plus memory/file 50/300 through tick 521) with the extended gameplay
fixture holding mutable state inside `math`. Full prediction invariance on the preceding graph build
passes 8/8 depth/repeat cases plus the full 221-tick reference comparison; an injected Lua-only mutation
correctly fails despite matching sim hashes (`full-lua-invariance-1`). The remaining Lua-owned
nonresident-object contract and newest-build live/invariance gates are still open. All recovery paths
in this paragraph are under `reviews/recovery-2026-09-06/`.

**23:51 UTC extension:** `build-lua-owned-objects-4.log` and `script-owned-object-selftest-4`
pass the expanded native graph check, with exact reserialization (851,437 bytes). The new fixture
first exposed both memory and file failures for an actor created and held by Lua outside the world;
ordinary sim hashes still matched. Owned MO nodes now carry full native state and IDs; copies are
created before references resolve, and a set-aside world retains its original owned objects.
Restoration preserves the saved script VM, controller state and attachment poses; discarding preset
parts no longer introduces a collision-ignore link or invokes attach/detach callbacks during restore.
All **5 cases pass** at `restore-mod-owned-v4/20260906_234905_f51a4932`: reference and memory/file
50/300, exact traces and actor dumps through 521 ticks, with semantic checks for the owned actor's
fields, controller, IDs and references. Source was unchanged across the suite. Live healing and full
prediction invariance on this build are next; broader Lua lifetime/environment coverage remains open.
Paths are under `reviews/recovery-2026-09-06/`; preceding failures are retained.

**00:03 UTC checkpoint (2026-09-07 UTC; Sept 6 local):** `5c199fe91` records the LuaJIT
continuation codec; `853ba5fb2` records the sound-set replacement fix. These concerns were verified
on the combined working source, not separate clean checkouts. `build-coroutine-bounds.log`,
`script-coroutine-bounds-selftest` and `restore-mod-coroutine-bounds/20260906_235954_50da7d85`
pass, including malformed-stack refusal and memory/file 300 through 521 ticks. Coroutine capture
keeps the GC list stable during traversal; restore checks allocation and frame bounds.
On the preceding owned-object build, `expanded-mod-gates/20260906_235407_invariance_8713cc78`
passes all 3 runs (reference, 8/8 invariance and Lua-only mutation detected despite identical sim
hashes). `expanded-mod-gates/20260906_235407_heal_44133054` passes real two-peer desync healing
with the sustained mod: both actors on both peers show `1/397/397/0` at tick 400, and both peers
finish the 600-tick healed match. The remaining Lua graph/native integration is uncommitted.
Next coverage checks mutable native configuration and global-environment/lifetime edge cases;
normal MP menu/stall/lifecycle and the whole later roadmap remain open. All evidence paths in this
paragraph are under `reviews/recovery-2026-09-06/`.

**00:20 UTC checkpoint:** `437ef8d46` reads the sound pause/fade flags that its writer already
emitted; `342bcac44` adds live configuration writers for MovableObject, MOSprite and MOSRotating;
`2dbb93f7d` records the expanded owned-actor fixture and native API inventory tool. Final/GNS build
`build-native-configuration-2.log` passes. `restore-mod-native-configuration-3/20260907_001344_b39f6289`
passes **5/5**: uninterrupted reference and memory/file captures at 50/300 through tick 521, with
34 native properties, full traces, final actor dumps and Lua semantics. The native graph selftest
passes on the preceding configuration build. These commits were split from verified combined working
source; separate clean intermediate states were not verified. The earlier failure was an omitted gib
limit; gravity was already serialized. File testing then exposed unreadable sound flags, now fixed.
The user's concern about reactive bug fixing is accepted: native coverage is now explicitly audited.
`native-api-inventory-2/inventory.json` indexes **381 distinct writable properties** (244 in the MO
hierarchy), with **34 exercised by this named fixture**, plus 909 methods requiring mutation/alias
review (281 in the MO hierarchy). These counts describe review coverage, not bugs or proof of general
correctness. Duplicate identical bindings are counted once. Writer-name matches are search leads only;
copy-constructor coverage does not establish file coverage. Finish the class/ownership audit, remaining
Lua environment/lifetime contracts, then rerun live heal/invariance and close lifecycle failures.
All evidence paths here are under `reviews/recovery-2026-09-06/`; source and binary provenance are retained.

**00:50 UTC checkpoint:** `695efeb66` fixes doubled nested SoundSet object boundaries;
`6aee14b33` preserves Actor/AHuman configuration; `bc87ac4f8` preserves attachment and firearm
configuration. The expanded fixture now exercises **94 distinct writable properties** on Lua-owned
AHumans and HDFirearms (`native-api-inventory-4`, still an OPEN review inventory).
The equipment fixture exposed a cross-VM ordering defect: creating owned objects while restoring
later VMs added script globals to earlier VMs after their graphs had been restored. The working
integration now prepares all owned objects, then all script roots, before hydrating any VM graph.
`build-graph-phase-ordering-1.log` and `graph-phase-ordering-selftest-1` pass, including exact native
selftest reserialization of 907,160 bytes. `restore-mod-phase-ordering-1/20260907_004632_b2199858`
passes **5/5** (reference, memory/file 50/300 through 521 ticks), with full Lua graphs, every sim tick,
final actor dumps and script continuation. `expanded-mod-gates/20260907_004756_invariance_fab85219`
passes reference, 8/8 clean prediction cases and detection of a Lua-only mutation despite matching
sim hashes. `expanded-mod-gates/20260907_004756_heal_aece9e6d` passes real two-peer healing:
both peers finish 600 ticks with script counters preserved. All suites used unchanged source and
retained the binary hash `4fae03b7524184eadc143f6f75cdcf028ccfe79afc4794ea7d38834ca7b11cdd`.
Commits were split from the verified combined working source, not independently verified clean
intermediate checkouts. Lua graph integration remains uncommitted. Next: global environment,
object lifetime/alias and native API coverage, then normal MP lifecycle defects and the remaining
roadmap. All evidence paths in this paragraph are under `reviews/recovery-2026-09-06/`.

**01:13 UTC checkpoint:** the global-environment contract now carries non-string keys, removal of
keys added after capture, the `_G` binding and the global table's metatable. The new native selftest
first failed four semantic checks while the old reserialization test still passed; all pass on
`build-global-environment-1.log` / `global-environment-selftest-1`. `9700d8500` adds sustained global
aliases to the mod fixture; `restore-mod-global-environment-1/20260907_005606_79fb6cef` passes 5/5.
`7df22e2d3` adds a scripted weapon's world → Lua → world → Lua ownership transitions and retained
fixture error logs. Its valid baseline and memory/file 50/300 continuations all pass through 521
ticks at `restore-mod-device-lifetime-3/20260907_010731_e04f9b42`. Earlier fixture setup failures are
retained: generic userdata equality is unsupported; RemoveItem returns MovableObject, so its owner
is re-added through AddMO. The fixture uses Old Stock Battle Rifle, whose scripts do not randomly
replace the object being followed. `expanded-mod-gates/20260907_011058_invariance_937fa662` passes
reference, 8/8 clean prediction cases and detection of the injected Lua-only mutation;
`expanded-mod-gates/20260907_011058_heal_d83ed808` passes real two-peer healing with this expanded
fixture. Binary SHA-256: `5d67f9371cfd5ec5f61c5e36906244361f7d34855eee39b89ec61fe10623c429`.
The suites retained unchanged source. Full Lua/native integration remains uncommitted and the
contract remains OPEN. Next native review targets remaining actor state/AI orders, nonresident
constructors, borrowed native aliases and the other MO classes. The user's 01:10 UTC milestone
status was explicit: still closing 2–3, normal multiplayer lifecycle remains open, followed by
latency/feel and the later roadmap; planning estimates are several focused days for normal MP and
weeks for the whole roadmap, with low confidence. All evidence paths here are under
`reviews/recovery-2026-09-06/`.

### 1. Controller boundary + wire/replay compatibility (the review's open findings — first)
- [x] 1a **DONE 2026-09-06 01:11** (`de5e642e3`): ControllerFrame v6 (84 bytes: one-shot intents + the
      owner's device class and digital aim speed), replay header v3 and controller log v2 name their
      frame version, v5 recordings play back through the absolute-state apply, old peers are rejected
      at the hello / lockstep start. Evidence: `legacy_replays_20260906_011034_43244` 13/13 — the three
      pre-change recordings (`e2e_gate_deep`, `rb_replay_20260905`, `rb_replay_buy`) replay with every
      per-tick subsystem hash AND per-tick total identical to their saved host traces (tick-600 total
      `feec6eff…`, buy tick-900 `ca7470e2…`); a v3 recording round-trips (`c1_fx_pickup_fire_20260906_011157_41036`).
      Lesson: a trace's `final_total_hash` is sampled after the cap drain and depends on stop timing —
      pin the last recorded tick's total, not the final.
- [x] 1b **DONE 2026-09-06 01:50** (`ai_writes_20260906_014751_42844`: 6 scripted writes, none applied
      early on the owner, each landed at tick+3 on both peers, dumps identical, direct_writes=0;
      boundary battery `boundary_gates_20260906_014550_9716` 7/7 after the counter threshold fix):
      the owner's AI direct writes land at the committed tick on EVERY peer. Aim/facing writes are captured as one-shot intents (flags 0x4/0x8) and undone on
      the owner until t+D; the AI's equip calls (the whole Equip*/Unequip* family, threaded and
      serial passes) queue a `DeferredEquip` record that under lockstep becomes an owner-issued
      `NetGameAIEquip` game command (lockstep codec v9) executed by the same AHuman method on every
      peer at the committed tick — no resolver duplication, SP semantics untouched; a residual direct
      equipment write is a counted `[controller-boundary] VIOLATION` (`direct_writes` in the report,
      harness check `controller_boundary`). Test mechanisms: `-ai-write-script` (writes inside the
      owner's AI pass), `-net-replay-dump from:to` (the recorded wire), sim dump `aim/flip/fg/bg`.
      Measured on the way: an AI's equip call within D ticks of its own unequip sees the stale hand
      (lockstep's inherent D-tick observation lag), and a moving actor's walk-facing logic re-flips a
      scripted facing within the tick, so the flip's evidence is the recorded intent.
- [x] 1c **DONE 2026-09-06 01:20** (`7f66c1d4b`): `Controller` carries a seat (mode + player this machine
      samples from) beside the wire-owned sim mode; sampling, the AI pass and the frame snapshot follow
      the seat; every registered actor of a lockstep match takes its sim mode from the committed frames
      (`IsWireOwned`), the `OnControllerInputModeChange` script hook fires at the wire apply on every
      peer; HUD, recoil shake, mouse-aim lock, pie-menu mouse writes, NEXT/PREV and craft seating read the
      seat. Evidence: `seat_switch_20260906_011950_8560` — the pre-fix recording shows 31 mode flips
      (period 4, negative control fails as required); the fresh D=3 run hands off once at tick 35 on
      both peers, host==client 600 ticks, replay identical (`test_seat_switch.ps1`, `check_seat_switch.py`).
      D=0 reference unchanged `3fa6c8f7…` (`c2_d0_ref_20260906_012038_48496`); legacy replays 13/13.
- [x] 1d **DONE 2026-09-06 01:49** (`aim_speed_20260906_014821_5176`: host at `-digital-aim-speed 0:2.5`
      vs 1.0, keyboard aim on the brain, both runs host==client 600 ticks, aim delta ratio 2.500):
      per-machine control-scheme facts (digital aim speed, device class) reach the sim only through
      the wire (`Controller::ApplyWireScheme`; sampling keeps this machine's scheme).
      Found by inspection on the way: `AHuman/ACrab::Update` read `g_UInputMan.GetControlScheme(player)`
      (a per-machine setting) inside the aim integration — a desync class for peers with different
      DigitalAimSpeed or device settings that the identical-settings e2e runs could never show.
- [x] 1e **DONE 2026-09-06 02:20** (`52034ba1f`): the preview clone collides with MOs again (the
      `SetHitsMOsRecursive(false)` is gone; MO hits resolve through the overlay's shadows). Evidence:
      trust battery 22/22 `test_trust_20260906_021328_49612` — the predicted pickup+fire is EXACT
      (`-lpinv-expect` pickup 144 / shot 161, no slack) at depths 1/3/8/18 × repeats 1/3 with the clone
      colliding; boundary gates `boundary_gates_20260906_021534_45692` + `seat_switch_20260906_022250_9744`.
- [x] 1f **DONE 2026-09-06 02:20** (`723ae949c`): an actor's travel and update draw from a per-actor
      per-tick RNG stream (`DeterministicMORNGScope` in `TravelStage`/`UpdateStage` for actors, the
      pattern the see-rays and script hooks already used), so a preview of the actor draws exactly
      what its canonical tick will — the reach ray included. **Sim change, measured:** D=0 host==client
      moves `3fa6c8f7…` → `a4e28adf…` (tick-600 total `88c5ecc5…`, `rng_d0_ref_20260906_015330_46648`),
      the legacy recordings' playbacks leave their saved traces at tick 39 in `sim_rng/actors` (the
      actors' first draw) and are re-pinned to the current sim's totals in `test_legacy_replays.ps1`
      (the v5 byte-identity under the old sim stays on record, 01:10). Evidence on the final binary:
      legacy replays 13/13, D=0 `88c5ecc5…` re-pinned, fuzz 17/17 + 10/10 (`fuzz_buy_v4_20260906_021838`,
      `fuzz_pickup_v4_20260906_021849`).
- [x] 1g **DONE 2026-09-06 02:45** (`0436edb59`): **found by the first gameplay battery
      (`gameplay_20260906_022324_49140`): the delivery, buy-order and synced-pause modes DESYNCED at
      D=3** (cargo at tick 55, buy at 573, pause at 621 — 3 ticks after the resume). All three had only
      ever been verified at D=0 (`run_interp_matrix.ps1` never passes `-InputDelay`). Root cause,
      measured with a both-peer dump (`diag_deliver_d3_dump_20260906_0300`): the delivered dropship
      enters the world at tick 53, its first committed frame lands at 56, and for 53-55 the owner's
      controller kept its own fresh AI sample (`ctrl=0x40` MOVE_DOWN at 54 on the host, `0x0` on the
      client until 57). The same gap follows a pause resume and an ownership change, and the first
      D ticks of a match. **Fix:** an actor no frame was committed for this tick runs on neutral input
      on every peer (`NeutralizeUnframedLockstepActors`, `Controller::ApplyWireNeutral`); D=0 is
      untouched (every resident has a frame). Evidence on the fixed binary: deliver 200 ticks, buy
      900, pause 900 sim-gated identical at D=3 (`neutral_{deliver,buy,pause}_d3_20260906_0237`), then
      the whole gameplay battery's eight e2e cases green (`gameplay_20260906_023853_46852`; the four
      semantic checks there fail only on a per-machine pie-menu write at the seat switch, see 3b).
### 2. Restore / identity / state inventory (S3 remainder → G5)
- Found by the trust battery on the re-recorded fixtures 2026-09-06 02:20: its corrupt-recording case
  flips three bytes at the file's midpoint; on the new record layout that landed inside a frame's
  numeric fields and the recording verified `ok` and played `completed` — a replay format with no
  record integrity cannot tell a bit flip from input. **Fixed:** replay header v4 checksums every record
  (FNV-1a over the encoded packet, verified by the reader for v≥4 files; v1-v3 files still read).
- Fixtures re-recorded 2026-09-06 02:12 on the current sim (`refresh_fixtures.ps1`, replay header v4):
  `fixtures/buy_900.ccreplay` (host==client, last tick total `8246bbc4…`) and `fixtures/pickup_fire.ccreplay`
  (`00bfe32a…`; pickup 144, shot 161); each carries a `.provenance.txt`. The battery's invariance
  cases moved to tick 250 because on this sim the duel's team-1 brain is gibbed at tick 281 and a
  preview needs a running activity (the probe stays at 300).
- **Trust battery 22/22 on the final build 2026-09-06 02:20** (`test_trust_20260906_021328_49612`):
  the corrupt recording now fails verify and playback by checksum, the invariance control passes at
  tick 250, the predicted pickup+fire is exact (pickup 144, shot 161, depths 1/3/8/18 × 1/3, no slack),
  the resumed run matches its reference 221 ticks, the full-to-end playback completes with its marker.
- [x] 2a **DONE 2026-09-06 02:20** (`93b5a7624`): `MovableMan::SetAsideWorld/ReinstateWorld` move the live residents
      (and queued adds, alarms, rosters, quarantine) out untouched for the probe's re-run and put them
      back at the verdict; `RestoreWorld` no longer purges a set-aside world; the activity's rollback
      slots resolve by unique id (`RollbackState` carries UIDs, not pointers — the old pointers dangled
      after a purge and only survived because playback seats no human). Evidence: probe 300:30 PASS
      (`probe300_setaside_20260906_020700`, `rbprobe_result=pass`, full dump + terrain identical, the
      originals reinstated), fuzz 17/17 + 10/10 on the final binary (same mechanism per window).
- [x] 2b **DONE 2026-09-06 02:20** (`93b5a7624`): `LuaStateWrapper::DescribeScriptObjectIdentity` (the `_ScriptedObjects[uid]` table
      address) + `MovableMan::DescribeLuaIdentity`; the probe fails when any object registered at the
      capture holds a different Lua object after the reinstatement. Evidence: the same probe/fuzz runs
      (identity checked at every verdict). Replay header v4 (`3747f3fea`) checksums every record; the
      state inventory + check are `be133b5de`.
- [x] **Faithful-clone hole found 2026-09-06 04:20 by the single 583:30 probe on `buy_900.ccreplay`
      (it failed on every tip back to `d3b6b5360`, while the 17-window fuzz covering 583 passed — a
      data-dependent miss, not a regression) and fixed in `187ce5230`:** a faithful clone's own
      construction re-runs `HandlePotentialRadiusAffectingAttachable` for every attachable it re-adds,
      perturbing `m_RadiusAffectingAttachable` / `m_FarthestAttachableDistanceAndRadius` (the brain's BG
      leg landed at 29.25 instead of the live 24.1 and became the radius attachable); the carriers
      (`m_Faithful…UID`, `m_FaithfulFarthest…`) exist and `ResolveFaithfulLinks` lays them back on the
      live clone, but the RESTORE clone derived its carriers from the CAPTURE clone's perturbed live
      fields (the capture clone is never resolved). The restored brain then took the parent's
      refresh-all-joint-positions branch on a different tick than the original (pass 1 refreshed the
      legs' `m_JointPos` before their ankle update at 584, pass 2 after it), which the ankle
      interpolation turned into a 2-ULP leg position. **Fix:** `MOSRotating::Create` takes the
      reference's PENDING carriers when it is itself an unresolved clone. **Measured, not reasoned:**
      the tracked-uid tracer (`CC_TRACK_UID=1048577,1048585 CC_TERRAIN_EVENTS=583:585`) with a new
      `radA` row per radius check (`cd8edaf7f`) showed the two passes' branch sequences side by side
      (`stage2_p4/probe583_trk/run3.json.rbprobe.terrainevents.txt` before, `run4` after: identical).
      Evidence: probe 583:30 PASS, fuzz 17/17 + 10/10 on the fixed binary (dirs in the §B table).
      The other carriers (`m_pMOToNotHit`, item-in-reach, move target, waypoints, supported device,
      incoming MO) are pointers the clone constructor never populates, so the "live pointer else
      carrier" pattern stays right for them; the attachable ORDER already used the pending-first pattern.
- [x] 2c **Native clone inventory checkpoint, 2026-09-06 02:05 — this does not establish file or Lua API restoration coverage**: `tools/check_state_inventory.py` derives which members of the 32
      snapshot classes `Create(reference)` copies (795) and fails on any member that is neither copied
      nor classified in `Source/System/StateInventory.csv` (57 rows: reconstructed 9, transient 9,
      render 18, identity 4, link 8, outside 9; 852 members, 0 unclassified). It exposed one real
      hole, fixed: a faithful clone of an actor's pie menu dropped the slices other sources added and
      lost its hovered/activated slice pointers (`PieMenu::Create` keeps them under `IsFaithfulClone`,
      `MatchingPieSlice`; the sim dump now carries the pie menu's interaction state).
- [~] 2d **FULL CONTRACT REOPENED: Lua graph/native restoration remains incomplete. The following 2026-09-06 04:35 result is a historical fixture checkpoint only** (`script_state_20260906_043124_49696`, tip `ceffd131f`, 9/9: reference
      scripts ran · in-memory probe 300:30 PASS with scripts running · resume == reference at 520,
      counters included · pass 1 == pass 2 · heal resync e2e 4/4 · heal semantics on BOTH peers at tick
      400 of the healed round = `1/399/399/59` (fresh Create, restarted Update, persisted value == the
      new count, the save-format value 59 found by Create) · the peers' healed lines identical).
      Restore-and-resume with a mod-style script counting Create/Update/Destroy + persistent vars;
      heal-path semantics (Create re-runs, save-format persistence) asserted and documented.
      **Re-pinned 2026-09-06 07:30 (`script_state_20260906_072823_23652`, tip `6097451c8`): the heal
      is a verbatim restore now (3d), so the healed round's tick 400 reads `1/397/397/0` on both
      peers — Create once, Update carried straight through the heal, no spawn-time re-roll; the
      test asserts exactly that (`update` within 60 of the tick, `carried == 0`).**
      **In-memory leg DONE (`f8f752507` + `dcf44eaa1`, `test_script_state.ps1`, evidence
      `script_state_20260906_035245_32308`).** Heal leg: Lua `print` never reaches stdout (the
      in-game console only), so the oracle is both peers' sim dumps of the healed round (the resync
      zeroes the sim count, so a tick-400 dump belongs to the healed round alone) with a fourth
      script field, `testCarried` = the value `Create` found in the save-format map; asserts fresh
      Create, restarted Update, carried value ≥ 1, peers identical. Runs after `chain6`. Re-read of
      the probe's text-save mode: it does NOT stack (4 actors per pass — the "8" were the two
      passes' lines of one dump window); its "restore mismatch" is the script fields (Lua state is
      not in the save, by design) and its tick-209 divergence is item 3d's four holes — so that mode
      becomes 3d's oracle once run without the script.
      **Read 2026-09-06 02:50 (the gap this item must close):** the in-memory probe freezes ALL Lua
      (`LuaMan::SetScriptsFrozen(true)` from the capture to the verdict), so pass 1 on the originals
      and pass 2 on the clones both run scriptless — the "full dump identical" verdict covers native
      state only (the probe is honestly labeled "native state, scripts frozen" in §B-0), and after the
      probe the originals have MISSED the window's `Update` calls (their scripts are 30 ticks behind a
      no-probe run). The preview freezes scripts the same way (`LocalPrediction.cpp`), so a scripted
      weapon is not previewed. Plan: `-test-script <lua>` attaches a counting script (Create/Update/
      Destroy counters on `self`, a global per-state tally, a `SetNumberValue` persistent var) to every
      actor; the sim dump + the probe oracle print them; then (a) the file-mode probe (text-save heal
      path) asserts Create re-runs / Update restarts / persisted values survive, and (b) the in-memory
      probe stops freezing pass 1, captures each registered object's Lua fields at T (deep copy of the
      `_ScriptedObjects[uid]` instance table: values, nested tables, entity refs by uid), gives the
      clones their own `self` for pass 2 and discards it at the verdict, so the counters equal the
      reference after the resume. The same machinery lets the preview run scripts on the clone (4b).
      **Built 2026-09-06 04:00 (first run `script_state_20260906_034151_6308`, three holes found by
      it):** (1) `System::PathExistsCaseSensitive` only knows the game's working tree, so an absolute
      script path outside it loads nothing (LoadScript -2, silently) — the test copies the fixture
      under `Userdata/` and `AddActor` now reports a failed attach; (2) held devices and other nested
      MOs script too — the capture/stash/discard now walk every Lua state's registered objects, not
      the top-level residents, and the stash covers every live original (a re-run object that
      initializes lazily overwrites the slot otherwise); (3) **found on the way:** the uid registry
      (`m_KnownObjects`) lost every pre-probe original after a reinstatement — the clones overwrote
      their entries and the purge erased them — so `FindObjectByUniqueID` (commands, Lua) would miss
      them after any probe; the set-aside now keeps and restores the registry. Also: the text save
      refuses a finished activity (the duel is over at 281), so the file-mode heal check probes at 200.
      Second run (`script_state_20260906_034838_41684`): with the nested capture and the registry
      restore the in-memory probe PASSES with scripts running (identity intact, pass 1 == pass 2,
      resume == reference); the script itself still did not attach: `LoadScript` wants the resolved
      module path (`GetFullModulePath`, as every caller passes) — fixed. **Third run
      (`script_state_20260906_035245_32308`, tip `dcf44eaa1`): the in-memory leg is GREEN with the
      counting script attached — every actor `Create` once / `Update` per tick / persisted value ==
      the count, probe 300:30 PASS with scripts running, pass 1 == pass 2 (counters included), the
      resumed run's tick-520 dump identical to a no-probe reference's, counters included.** The
      probe's abandoned text-save mode is NOT a heal oracle: `LoadGameToRestart` in that mode adds
      the reloaded scene on top of the residents (8 actors at tick 229: the originals still counting,
      the copies with no script state) — the heal-path assertion moves onto the live P5 resync e2e
      (`-ResyncTest` + the counting script on both peers), next. **`ceffd131f` (04:31):** the dump's
      fourth script field (`script=create/update/persisted/carried`, `testCarried` = the value `Create`
      found in the save-format map) — the heal leg's oracle; the three-leg battery is running on it.
### 3. Gameplay fixtures + minimizer (S4 → G4, G6)
- [x] 3a **DONE 2026-09-09**: tick-300 invariance is a matrix job (`invariance_pickup_300`, `invariance_global_300`, added by the harness lane) and the `lanes/invariance-300` driver; green on Source38 and Source39. Original text: Invariance selftest at tick 300, full depths × repeats, Lua identity included, on the final build.
- [~] 3b Fixtures through `-input-script` with semantic assertions, host==client, replay identical:
      control switch · AI re-equip · aim · reload · pickup/drop · jetpack · gib/death · door · terrain ·
      cargo (craft passengers) · buy/deliver · mod scripts.
      **Battery `test_gameplay_fixtures.ps1` + `check_fixture.py`** (D=3, each case its own e2e dir):
      fire+reload (semi-auto Battle Rifle: one round per press, 2300 ms reload), weapon switch (fg
      device id at the landing tick), jetpack (emitter on for the 31 landed hold ticks, fuel down,
      ≥10 px rise), terrain fire (aim down, terrain hash carve after the landed shot), pie-menu reload
      (`pie_reload.txt`: Enabling at the landing, Enabled 4 ticks later, "Pick Up" hovered from the
      landed L_UP, reload at the release's landing), plus the harness modes death/brain-kill, cargo
      deliver, buy order, synced pause. **Timing convention, measured (pickup 140→144, shot 157→161,
      switch 100→104):** a script line at tick T is sampled after T's update, rides the frame for T+1
      and lands at T+1+D; the dump (written after the tick's update) shows it from that tick. The
      checker identifies the seated actor by its mode transition, so every window starts at 27.
      Run 1 (`gameplay_20260906_022324_49140`, pre-fix binary): 8/8 e2e except deliver/buy/pause
      DESYNC → item 1g. Run 2 (`gameplay_20260906_023853_46852`, `0436edb59`): 8/8 e2e green; the four
      dump checks failed on `pie=2:…` vs `3:…` at ticks 32-34 — `GameActivity::SwitchToActor/Next/Prev`
      ran the pie menu's selection wobble (`DoDisableAnimation`, sim state) on the seat-side switch,
      host only, three ticks before the wire handoff. **Fixed (`db4131587`):** the wobble runs in
      `Actor::OnControllerInputModeChanged`, i.e. at the handoff on every peer (SP unchanged: the hook
      fires immediately there). **Found on the way (`8e19155ac`):** every pie-menu interaction timer
      (enable/disable 50 ms, hover 100/500/2000 ms, sub-menu open delay) ran on REAL time — a slice
      could activate on different sim ticks on two peers whose lockstep waits differ; all moved to
      sim time (SP feel identical at 1x). **And (`e66bd25d1`):** the menu's UI sounds played on every
      peer for every actor; now only the seat that drives the menu hears them. Run 3 = `chain4`
      (pending). **Still seat-side (audit, open):** `GameActivity` closes the pie menu on
      `FullInventory` / the unit-select circle and strips the BuyMenu slice when `!m_BuyMenuEnabled`
      (`RemovePieSlicesByType`) for its own controlled actor only — sim-state writes on one peer;
      redundant with the wire-driven close in the common path (the pie button release lands on both
      peers the same tick), divergent when the slice is picked with the primary button while the menu
      stays held. Fixture + fix next (the close belongs in the actor's own pie-command handling).
      **Run 3 (`gameplay_20260906_025548_46860`, `e66bd25d1`): 9/9 e2e green, fire_reload / jetpack /
      terrain_fire semantic PASS; weapon_switch's check expected a 2-device cycle (the dummy carries
      three — checker fixed, re-check PASS on the same dumps); `pie_reload` exposed a REAL defect: the
      pie-menu interaction replicated perfectly (Enabling at 153 = T+D, Enabled 157, "Pick Up" hovered
      163-194 on both peers) but the activated command did NOTHING on either peer.** Cause:
      `GameActivity::Update` (seat-side, before the wire apply) is the only consumer of
      `PieMenu::GetPieCommand()` — it called `HandlePieCommand` for its own controlled actor, whose
      controller-state effects (WEAPON_RELOAD/DROP/PICKUP/NEXT/PREV) the tick's wire apply then
      overwrote, and whose direct writes (`m_AIMode`, `ClearAIWaypoints`) would have landed on the seat's
      peer only. **Fix:** `Actor::HandlePendingPieCommand` runs in `MovableMan::UpdateStage` (after the
      wire apply, before the update, in the preview too) on every peer; the activity keeps only the UI
      reactions. Same T→T+1 timing as before (activation in the tick's pie update, command at the
      next update). **Still seat-side, next:** the AI orders the view modes confirm — GoTo waypoints
      (`AddAIMOWaypoint/AddAISceneWaypoint`), unit-select squads (`SetAIMode(SENTRY)` on the commander,
      `ClearAIWaypoints/SetAIMode(SQUAD)/AddAIMOWaypoint` on each unit in the circle), the FormSquad
      disband loop — are direct sim writes on the seat's peer; `Actor::RequestAIMode` already routes the
      AI's own mode changes through `NetGameSetActorAIMode`, the orders get a `NetGameAIOrder`.
      **Done (`d3b6b5360`, lockstep codec v10; v8/v9 recordings still decode):** `NetGameAIOrder`
      {SceneWaypoint, MOWaypoint, ClearWaypoints, FormSquad, DisbandSquad} issued by the seat's view
      modes under lockstep (direct in SP), executed on every peer through `Actor::FormSquad /
      DisbandSquad / HasSquad` (the loops moved out of the activity); the harness mode
      `-AIOrderCommandHost` (`-net-match-e2e-ai-order-command`: go-to at 50, follow at 200, squad at
      400, disband at 600) + the battery case `ai_orders` asserting `aimode/wp` on both peers at T+D
      (the dump now prints `team=` and `wp=`; the per-machine `paths=` hash is excluded from the peer
      compare — the pathfinder is off-wire by design). **Pie-reload run 4 (`10ead3ddf`,
      `gameplay_20260906_031158_30596`):** the reload STILL did not start: with a mouse scheme the
      controller re-samples PRIMARY/SECONDARY and their press/release edges from the mouse buttons
      (`UpdatePlayerAnalogInput`), which the input script did not answer, so RELEASE_SECONDARY never
      rode the wire — `UInputMan::GetMouseButtonState` now reads a scripted player's FIRE / PIEMENU
      elements as its left / right button (`b6d0a8b69`, a test-mechanism gap, not a gameplay one).
      **Run 5 (`chain5`, `d3b6b5360`, `gameplay_20260906_032053_46452`): 10/10 e2e green; `pie_reload`
      now runs end to end on both peers — menu Enabling at 153 (T+D), Enabled 157, "Pick Up" hovered
      from 163, RELEASE_SECONDARY lands 194 (the slice activates: `pie=1:8:Pick Up:Pick Up:Pick Up`),
      WEAPON_RELOAD set at 195, magazine out and reloading at 196; checker pinned to that (PASS on the
      run's dumps). `ai_orders` issued nothing: the duel has ONE team-0 dummy (the other Green Dummy is
      team 1) and the control wanted two — control + checker re-cut for one unit (go-to, follow the
      brain, squad, disband), re-run pending.** **Then measured why the reload landed a tick late
      (release lands 194, WEAPON_RELOAD set 195, reload 196): `AHuman::PreControllerUpdate` consumes
      the reload / weapon-change / drop / pickup states, and that pass runs BEFORE the wire apply while
      my hook ran after it — the state survived only into the next tick's pre-controller pass.
      `7990d9c46`: `MovableMan::PreControllerStage` (canonical loop + preview) handles the pie command
      right before `PreControllerUpdate`, the tick after the activation, exactly the SP timing.
      `c235579e7`: the AI-order control re-cut for one dummy.** **Run 6 (`gameplay_20260906_033302_31416`):
      `pie_reload` PASS at the SP timing (reload at 195); `ai_orders` e2e green and the orders land at
      53 on both peers, but at 56 the host's dummy shows `wp=0` while the client keeps `wp=1`: the
      owner's AI (`HumanBehaviors.lua` → `Actor::UpdateMovePath`) POPS the waypoint it loads into its
      path — an owner-only write to a replicated queue (the same class as the AI's aim/equip writes).
      Fix in progress: the owner's AI keeps a per-machine lead cursor over the queue and sends the pop
      as `NetGameAIOrder::PopWaypoint` (carrying the point, so a stale pop cannot eat a fresh order);
      every peer pops at the committed tick; SP unchanged. `m_WaypointCursor` classified `outside` in
      the inventory. Landed as `aa1827e80` (build 03:52); **run 7 `gameplay_20260906_033922_22468`:
      `ai_orders` PASS — both peers' dumps identical over 27..640 with the pops replicated; go-to at
      53, follow at 203, squad at 403, disband at 603.** **Follow-up
      fixtures:** the UI path of an order
      (pie → AI-modes sub-menu → Go To → waypoint by cursor), the FullInventory / unit-select seat-side
      pie closes, an ACrab and an ACraft under the same script.
- [x] 3c **DONE 2026-09-06 03:45** — `minimize_fixture.ps1` (ddmin over the input script's lines
      against any check command) proven on the game: `check_fired.ps1` (a D=3 two-peer run of the
      candidate; interesting = some team-0 actor spent a round by tick 170) took the six-line
      `pickup_fire.txt` to the single line `157 163 FIRE` in 3 check runs
      (`minimize_20260906_033402_12896/minimal.txt`, each candidate's e2e dir under `minimize_runs/`).
      The seed, the input stream and the first differing tick of a desync are already saved by the
      harness (`simgated_compare.txt`, `provenance.json`, `match.ccreplay` per run).
- [~] 3d **FULL FILE CONTRACT REOPENED: mutable native configuration and Lua restoration remain under audit. The following is a historical fixture checkpoint from 2026-09-06 07:35 (commits `4bdf19c89..6097451c8`, thirteen per-concern commits; battery
      `test_text_save.ps1` = the file-mode probe at buy 50/100/150/200/250, pickup 50/100/200/300/400/500
      and the two toggle legs, **14/14 FIDELITY PASS byte-identical** on `text_save_20260906_072642_41192`;
      the heal e2e on the verbatim restore 9/9 on `script_state_20260906_072823_23652`; `chain11` re-runs
      everything on the rebuilt tip).** The first pass at tick 200 (`probe_file200_b8_20260906_054816`)
      was the start; the sweep across ticks and the second recording then found and closed, in order:
      the file's resident ORDER (the MOID pass and the update walk follow the list; a restoring absorb
      no longer uid-sorts), the actor's last-second position, the item-in-reach and move-target links
      (an unsaved reach target made the pre-controller pass draw one extra sim-RNG number), the
      dropped rifle's atom group carrying a subgroup twice (subgroups now re-key along the whole
      ancestor chain and the group rebuilds exactly from the saved atoms, materials included), a
      wound's damage multiplier, a smoke particle's runtime-scaled air threshold (the raw air values
      ride the save), **and the Lua state**: `OldStockReplacer.lua` re-rolled its 3.5 % weapon swap in
      `Create` on every reload and `MuzzleSmoke.lua` lost its `require`d module — so the save now
      carries each scripted object's fields (`ScriptState`, serialized from the probe's own capture:
      Vector/Timer by value when Lua owns them, entities by UniqueID, shared tables by id, modules
      and global classes by name, metatables by global name) and a reloaded object restores them in
      place of `Create` (no `OnAttach` either); found on the way: a luabind 0.7.1 property cannot
      return `int64_t` (the `Timer.StartSimTimeMS` property always crashed if read from Lua) — the
      tick counts are bound as numbers. **Heal semantics changed accordingly: the resync relaunch is
      now a verbatim restore at the saved sim time (was: spawn-normalized at tick 0), scripts resume
      instead of re-running Create — `test_script_state.ps1`'s heal leg asserts the OLD semantics and
      is expected to need its expectations re-pinned by `chain8`'s measurement.** Measured first (`probe_file200_pre_20260906_044523`):
      the "four holes" were the visible tip — from the first tick after a reload the arms' hand
      offsets, the legs' ankle offsets, the sprite frames, the pie-menu state, the held devices'
      poses and much more differed, and the hash caught up at 209. Closed by measurement, one
      probe run per batch, restore-dump diff → tracer → fix: (1) the reader's attaches normalized
      what the file said (rot inherit, pose re-derivation, timer resets, reload/activation
      clears) — `Attachable::AttachKeepsLiveState()` = faithful clone OR `g_MovableMan.IsRestoringSnapshot()`
      gates every SetParent normalization (Attachable/HDFirearm/Turret); (2) the scene clone at
      `SceneMan::LoadScene` was a spawn clone that drops every faithful-only member — a restoring
      load places FAITHFUL clones (`Scene::Create` under `FaithfulCloneScope` when restoring); (3)
      the add path's `CorrectAttachableAndWoundPositionsAndRotations` re-derived poses — skipped for
      restoring adds, the joint position / prev offsets / mounted angle saved instead; (4)
      `LoadGameToRestart` is now THE verbatim snapshot restore: the read runs under the restoring
      flag, the restart rewinds the sim clock to the saved `SimUpdateCount/SimTimeTicks` (new save
      fields) before placing anything so every timer anchor lands, and the flag covers the whole
      restart (the probe's own RestoreScope is gone) — **this also makes the P5 resync heal a
      verbatim restore at the saved tick (was: spawn-normalized at tick 0) — heal e2e re-run
      pending**; (5) hardcoded-attachable hooks (`m_HardcodedAttachableUniqueIDsAndSetters/Removers`)
      were keyed by the attach-time UniqueID and a reloaded attachable adopts its saved one later →
      the head's removal at the tick-281 gib never nulled `m_pHead` → crash in `AHuman::Draw` on the
      MOID worker (found with a new stack scan in the crash handler: `AbortCode.txt` now lists the
      exe-relative return addresses; symbolized with dumpbin) → `RekeyHardcodedAttachable` on
      adoption; (6) the root atom group's atom ORDER (the live attach history) — the reload
      attached in declaration order and collision walks the list → `AtomGroup::SetAtomOffsets`
      reorders to the saved subgroup sequence; (7) the wound's `DamageMultiplier` (1.1 live, preset
      1.0) was never saved → health drifted by 0.004/tick from a wound's first emission. **Saved
      now (writer + reader, direct or via persisted stashes/anchors where the placement clone or a
      post-read `Create()` would clobber them):** HDFirearm reload flags + timer limit + fire frames ·
      ACraft hatch state/timers, flipped/crash/network-delivery timers, current exit, exits'
      incoming MOs · ACDropShip lateral control · MO ignore link (by a UID recorded at set time, so
      an attachable or a dead shooter is named safely) + ignore timer, prev velocity, wound-damage
      flags · AHuman offhand-reload, prone/arms states, arm climbing, aiming, stride, prone/stride/
      throw timers, crouch · Actor movement state, six timers, recent movement, alarm pos, view
      point (stash: `Actor::Create` re-derives it), gold picked, prev health, sharp-aim speed, pie
      interaction state (`PieMenu::PackInteractionState`, slices by index, sub-menu nested) · Arm hand
      offset/pos/prev, targets queue, delay timer, reached flag, supported device link · Leg ankle
      offset, target, extension · MOSprite frame, prev angvel, anim timer/direction · MOSRotating
      radius cache + radius attachable + attachable order + deep hardness + set damage multiplier ·
      Attachable joint pos, prev offsets, mounted angle, damage count · AEmitter was-emitting, avg
      impulses · PEmitter (particle emitters had NO live state saved at all) · AEJetpack thrust bonus
      · MOSParticle time rest. **The dump grew to cover it** (`atoms=` root atom hash, `nothit=`,
      `pvel=`, `wdmg=`, `mstate=`, `atmr=`, `hstate=`, `htmr=`, `crouch=`, `fire=`, `reloadms=`,
      `hatchms=`, `lateral=`, `ctmr=`, `exit=`, `pem=`, `trest=`, `bonus=`, attachable `dmg=`/`em=`)
      — so every probe, in-memory included, now checks it. **Closed after the first sweep:** the
      file-mode sweep across both recordings (the radius-carrier clone-of-a-clone hole of item 2 was
      the last one) and the heal e2e on the verbatim restore: the healed round's lockstep now starts
      at the restored tick + 1 (the host announces it through the lobby start, `900877aba`; a
      rematch goes back to frame 1 with its zeroed count) — before that the coordinator waited for
      frame 1 forever (`MissingFrameTimeout`). **Found by the heal leg
      (`script_state_20260906_071338_5384`, every healed actor frozen at `1/59/59/0`) and fixed
      (`45afb0d1f`):** the scene saver writes each object as a copy of its preset, so a script added
      at runtime (`AddScript` — the test script, Base.rte's constructor collect) or switched off
      (`DisableScript`, used by vanilla scripts on themselves) came back as the preset had it; the
      save now lists the extra scripts (`ScriptPath`) and the disabled ones (`DisabledScriptPath`)
      and the reader loads them in load order; the dump prints each scripted object's script set as
      `scr=` (paths + enabled flags, `6097451c8`); the toggle legs (fixtures/toggle.lua adds
      tally.lua at Create, disables it at 120 updates, re-enables at 240; probes 150 and 250)
      hold it. **Per-machine by design, classified 2026-09-06 07:10 (`c4ffe43a8`):** the actor's
      alarm point (`m_AlarmTimer`/`m_LastAlarmPos`/`m_PointingTarget`) — the owner's AI sets it
      (`SetAlarmPoint` on hearing an alarm event), only the AI, the HUD exclamation and the alarm
      sound read it, the sim never does — so the dump prints it as `alarm=` and the three peer
      compares (`check_fixture.py`, `check_ai_writes.py`, the heal leg) skip it like `paths=`.
      **Still open under this item:** ADoor's door state machine and ACrab stride/aiming (a door
      / crab fixture — 3b's follow-ups carry them); Lua globals and closures stay outside the save
      by design (the single-player save contract: `OnSave`/`SetNumberValue`); a finished activity
      refuses the text save (fixture limit; the in-memory sweep covers those ticks).
      The 4 save-fidelity holes (HDFirearm mid-reload, ACraft hatch/exit/lateral, projectile
      shooter-ignore, AHuman offhand reload) closed with the banked fixture selftest.
- [ ] 3e `HostCpuRemoteHuman` actor-switch ownership · the controller-ownership state machine
      (crouch/prone under disabled, quarantine-release vs UI-disable) — a product call is recorded if a
      design fork appears.
### 4. Presentation + performance (S5 → G7)
- [x] 4a **Acceptance numbers, pinned 2026-09-06 04:05 before any 4c run** (measured at fake RTTs of
      100 and 200 ms via `-FakeLagMs`, two render rates, both peers, every number from the e2e
      report / the harness, not from impressions):
      · **sim pace** `pace.wall_tps ≥ 59.0` over the whole match (the harness already asserts ≥ 55;
        the headed runs assert 59) and `pace.sim_ms_per_tick ≤ 8` with prediction on;
      · **input delay** auto-picked per sender: D = 4 at 100 ms, D = 7 at 200 ms (D ≤ ceil(RTT/2 /
        16.67 ms) + 1), reported in the lobby (7b) and in `lockstep.input_delay`;
      · **input-to-photon for the local actor**: the preview shows the local input's effect on the
        first render after the tick it was sampled — ≤ 1 sim tick + 1 frame (≤ 34 ms at 60 Hz,
        ≤ 24 ms at 144 Hz) regardless of D; measured by the `-input-script` fixture's landed tick vs
        the first frame whose render substitution carries it (the preview counters `previews /
        actor_ticks / taken` prove the preview ran on that tick);
      · **prediction cost** `local_prediction.ms_total / previews ≤ 2 ms` per tick at D ≤ 7;
      · **corrections**: `violations = 0` always; a correction of the local actor's pose > 4 px happens
        only on a remote-caused event (hit, collision) — counted from the dumps (preview pose vs
        landed pose), ≤ 1 per 10 s of the duel fixtures;
      · **firing / audio response**: the shot's muzzle effect and sound play once, at the preview
        tick, ≤ 34 ms after the press at 60 Hz (needs 4b's preview-event ledger; until it lands the
        audio lags D ticks and the number is reported as a miss, not hidden);
      · **frame-time percentiles**: p99 draw ≤ 1.5× the local single-player baseline at the same
        scene, no frame > 50 ms during the match;
      · **whole cost** at 200 ms RTT, 2 peers: ≤ 15% CPU over the D=0 match on the same machine.
      A miss is reported as open with the measured value; nothing here is relaxed to pass.
- [ ] 4b Local prediction polish: preview-event ledger (a sound/effect plays once, at the preview),
      optimistic projectiles, the Activity UI (pie menu, cursor) following the preview.
- [ ] 4c Headed two-window runs at measured ~100 and ~200 ms RTT, ≥2 render rates: screenshots/video,
      input-to-photon, firing/audio response, correction magnitude/frequency, sim pace, frame-time
      percentiles vs the local baseline, whole cost.
- [ ] 4d Sim-speed program measured; exact full-world corrections (P8 increment 6) if the clone cost
      allows, else the measured verdict and the chosen technique recorded.
### 5. Breadth (G8) + verification debt
- [x] 5a **DONE per family since Source32 (2026-09-08/09)**: the breadth lane runs the 16-case interp matrix, the GNS-disabled build, eleven selftests, both rollback fuzzes and the 100/200 ms fake-lag duel on every pinned family (latest Source39). Original text: On the final tip: 16-case matrix (rebuilt GNS) · GNS-disabled build · 9 selftests · rollback
      fuzz · 200 ms fake-lag e2e.
- [ ] 5b 3/4-peer packs, co-op, PvPvE, leave/drop/rejoin/resync/replay/rematch/pause lifecycle.
- [~] 5c macOS leg **DONE per family** by the lead over `ssh Erol-Mac` since Source32 (exact export, native suite, authority, sound-query gates, selftests; latest Source39 `2bd9bd2e`). WSL2 leg NOT run. Original text: WSL2 leg re-synced and run (this PC, sequential). [!] macOS leg: the user relays.
### 6. H4 authenticated reconnect + host moderation (`STAGE2_H4_RECONNECT_PLAN.md`, design settled)
- [x] 6a **DONE 2026-09-08**: pins P1-P32 in `STAGE2_H4_RECONNECT_PLAN.md`'s table, lead confirmations at ~10:00 and ~20:20 UTC. Original text: Pin the §4/§5/§7 protocol values in the plan doc first.
- [~] 6b Phase A **implemented and merged through slice A7 (2026-09-09)**; of the eight §9a gates four are green and four wait on the A6 mutex fix and the B1 snapshot fix (lanes running); the headed §11 review needs the user present. Original text: Phase A: transaction handshake (NewJoin/TicketOffer/StoredAck/Committed; Reclaim/Challenge/
      Proof; txId cache), single-active-incarnation fencing, the leave protocol, a durable per-process
      ticket store, the drop-time ownership ledger + system-authored reseat, NetLobbySession admission
      isolation, §9a gates, the §10 old-wire probe, §11 reconnect UX (auto-retry, crash/relaunch prompt,
      roster indication).
- [~] 6c Phase B: the substitution transaction (slice B1) is **implemented, reviewed and merged**; its four two-process gates run on the next family (two crash today on the resync snapshot save, lane running); the moderation UI (slice B2) and the rest of the §9b gates are open. Original text: Phase B: substitution transaction + moderation UI + §9b gates.
### 7. Lobby / session UX + robustness (`RECOMMENDED.md`, `NEXTENHANCEMENTS.md` waves A-B)
- [ ] 7a Lobby + in-match chat (the relay host rebroadcasts).
- [ ] 7b "Resyncing…" overlay · in-match event toasts (heal/left/rejoined/pause) · LAN list selection
      kept · server browser v2 (columns + ping + refresh + double-click) · co-op resume seating by slot ·
      join screen Back stops the browser · the auto-picked input delay shown in the lobby.
- [ ] 7c Mod handling: mismatch resolution UX on both peers (which modules differ, what to do).
- [ ] 7d Compressed state transfer · beacon payload rebuilt only on change · delta-encoded frames ·
      `GetPrimaryLocalAddress` re-resolved per lobby entry.
- [ ] 7e Post-resync rounds replayable · periodic background snapshots · desync telemetry bundle ·
      replay round-trip selftest · crash-rejoin prompt.
- [ ] 7f Replay UX: in-game replay browser with pause/step, share/export; post-match report.
### 8. Discovery / internet play + the rest of the roadmap (`NEXTENHANCEMENTS.md` waves B-D)
- [ ] 8a Internet play beyond direct IP: the session-directory client seam built; [!] the hosted
      directory service and a NAT rendezvous need the user's hosting decision.
- [ ] 8b Dedicated headless host (`-net-dedicated`) · host migration · spectator slots · peer cap >4
      (gated on wire slimming + an uplink budget) · cheat-surface hardening · platform icons.
- [ ] 8c Lobby v2: activity/scene picker, team builder, loadout pre-pick, ready-check, kick, password.
- [ ] 8d Adaptive input delay mid-match · audio smoothing under correction · bounded rollback per the
      measured gate; [!] rich presence / async tournaments need external services.

## §B-0 CORRECTIVE MILESTONE — trustworthy sim / restore / prediction boundary (ordered 2026-09-05 late)

Source: `D:\Projects\reviews\2026-09-05-rollback-verification-audit.md` (evidence cutoff ~21:27). The
user's steering: fix the CLASSES of problems, keep the useful work, no engine rewrite, no reduced
goal, Causeless's architecture and mod compatibility intact, guarantees A-D verified SEPARATELY:
**A forward determinism · B restore fidelity · C prediction isolation · D player experience.**

### Findings checked against the tree at `45a185779` (2026-09-05 ~22:00)

1. **Replay gate accepts a truncation as success — CONFIRMED, OPEN.** `Main.cpp` classifies the
   playback error by the substring `replay ended`; the reader's truncation text is `replay ended
   without its end marker (truncated)`, so it passes. Root cause of the truncated e2e recordings
   found too: the recorder closes only in `SetLockstepCoordinator(nullptr)`, which the e2e
   `Complete("e2e complete")` path never reaches before quitting → no end marker on every e2e
   recording, including the baseline's.
2. **The native probe is not a scripted-resume proof — CONFIRMED.** `RestoreWorld` purges the
   originals (`PurgeAllMOs` → `DestroyScriptState` → `_ScriptedObjects[uid] = nil`) and the clones
   re-run `Create` once scripts unfreeze; the probe freezes Lua through both passes. Scope, stated
   honestly: the in-memory `RestoreWorld` is TEST TOOLING — the shipped design previews on clones
   and never restores the canonical world; the production restore is the text-save heal/rejoin
   (`LoadGameToRestart`), whose script semantics are a save/load (Create re-runs; script state
   persists only through the save format). Both get labeled and tested as exactly that.
3. **The preview can mutate the canonical graph — CONFIRMED.** `AHuman::Update` pickup calls
   `g_MovableMan.RemoveMO(m_pItemInReach)` and re-parents the real weapon; `ResolveFaithfulLinks`
   resolves `m_pItemInReach`/waypoints through the live manager; `DiscardAddedSince` cannot put a
   removed canonical object back. Reachable manager mutations from entity updates (grep count):
   MovableMan AddParticle 14 · AddMO 10 · AddActor 10 · RegisterAlarmEvent 6 · RemoveMO 2 ·
   RemoveActor 1 · AddItem 1 · PurgeAllMOs 2; SceneMan terrain writers (`TryPenetrate`,
   `RemoveOrphans`, `DislodgePixel`, unseen reveal/restore); Activity `SwitchToActor` 2,
   `ReportDeath` 1, `ChangeTeamFunds` 1, `HandleCraftEnteringOrbit` 2; Audio/PostProcess effects.
4. **The preview reproduces the tick by hand — CONFIRMED.** Canonical order per MO: Travel stage
   = `ApplyForces → PreTravel → Travel → PostTravel → NewFrame`; then `PreControllerUpdate`,
   controllers, scripts, `Update → UpdateScripts → ApplyImpulses`, `PostUpdate`. The preview
   skipped `ApplyForces` (gravity, accumulated forces) and `ApplyImpulses`.
5. **The promised per-class exhaustive copy test was not delivered — CONFIRMED; the "no hole can
   appear later" claim is withdrawn.** What exists: a source audit, the faithful blocks, the
   full-dump oracle and 74/74 fuzz probes — a bounded guarantee, to be superseded by the state
   inventory + gates below.
6. **Fixed before the audit closed and superseded by later runs:** the preview zeroed the frame
   accumulator (`RewindSimTo`) → `RestoreSimTickAfterPreview`; D=3 and fake-lag e2e runs then
   passed with previews proven executing (`stage2_p4\lp_delay3_trace2`: 2303/2259 previews).
7. **Run hygiene — CONFIRMED:** output dirs were reused (first failures survive only in the
   transcript); test names are not measurements (`lp_lag200` = 200 ms one-way fake lag =
   ~402 ms measured RTT).

### Implementation sequence (each step lands as commits and closes its own gate; nothing is
### marked done by reasoning)

- **S1 — repair false success.** Replay playback outcome as an enum (`Completed | TickCap |
  Truncated | Corrupt | SimFailure`) set where the condition is detected, never by substring;
  outcome + frames consumed + last tick + end-marker in the trace JSON; exit code non-zero for
  Truncated/Corrupt/SimFailure; the recorder closed at EVERY match end; `-net-replay-verify`
  integrity scan (all records decode, end marker present, tick range). Harness v2: unique run
  dirs, `result.json` with provenance (exe SHA-256 + mtime, git HEAD + dirty file list + diff
  hash, build config, module manifest hash, seed, delay/lag/prediction settings), one named
  check per property with its own status, required checks that were skipped = FAIL, the
  prediction-executed check for prediction runs, D=0 labeled regression-only. Fault tests
  (G1): truncated and corrupt recordings, a deliberately wrong expectation, an injected omitted
  faithful field, an injected canonical mutation during a preview — each must FAIL naming the
  property. Fault injection lives only in test paths behind `CC_FAULT_INJECT`.
- **S2 — red regression cases** for finding 3 (pickup in preview), 4 (gravity in preview), 2
  (Create re-run after the probe's restore).
- **S3 — refactor the boundaries.** (a) Shared tick stages in `MovableMan` used by the canonical
  loops AND the preview (canonical output unchanged: baseline + replay). (b) A speculative
  execution contract: one `SimSpeculation` scope that owns the undo ledger (terrain layers,
  sim clock, RNG, UID counter, activity scalars, add/alarm queues, audio/effect suppression) and
  a canonical read-only guard — manager mutators that would touch a canonical object while
  speculative refuse it, count it, and assert under test. (c) The probe's restore keeps the
  originals alive (set-aside) and reinstates them after the verdict: no purge, no Create
  re-run, Lua identity intact; the probe is labeled "native state, scripts frozen". (d) The
  state inventory (`Source/System/StateInventory.csv`): every member of the snapshot classes
  classified captured / preserved / reconstructed / outside-the-boundary(+reason), with
  `tools/check_state_inventory.py` failing on unclassified members (run in the gate scripts).
- **S4 — permanent tests.** Restore-and-resume (probe reinstatement + heal path) with a
  mod-style script that counts Create/Update/Destroy and keeps persistent vars; a
  preview-discard invariance selftest (depths 1/3/8/14 × repeats 1/3 at a canonical tick: full
  dump + timers + queues + RNG + UID + Lua binding table unchanged, then K resumed ticks equal
  a no-preview reference); structural fixtures through an input harness at the `UInputMan`
  boundary (`-input-script`): fire/reload/switch/pickup/drop/jetpack/gib/death/door/terrain/
  cargo/buy/deliver with semantic assertions; a failure minimizer (seed + stream + first diff
  saved, action-sequence bisection into a small regression case).
- **S5 — presentation + performance.** Headed two-window runs driven through the input harness
  at MEASURED ~100 and ~200 ms RTT and at ≥2 render rates: screenshots/video, input-to-photon,
  firing/audio response, correction magnitude/frequency, sim pace, frame-time percentiles vs a
  local baseline, whole-cost (capture + speculation + restore + presentation) — with the
  acceptance numbers pinned in this file BEFORE the runs. Then the 16-case matrix, 3/4-peer,
  lifecycle (pause/leave/rematch/reconnect/resync), and the remote legs (prompts prepared,
  coverage reported as what actually ran).

### Acceptance gates (evidence dir recorded next to each when it runs)

| Gate | Passes when |
|---|---|
| G1 test trust — **CLOSED 2026-09-06** (`test_trust_20260906_002119_39068` 22/22) | truncated / corrupt recording, wrong expectation, omitted field, injected canonical mutation, unreachable invariance tick, unreachable probe, zero/malformed spec, unwritable trace, skipped required check, stale binary, prediction-off under -RequirePrediction, the preview theft → each FAILS naming its property; the clean invariance control and a full-to-end `completed` playback PASS; the pickup+fire positive case and its resume-vs-reference compare PASS; provenance is captured after the build and covers the exe, committed/staged/unstaged/untracked source, the scripts and fixtures, and the module manifest |
| G2 shared stages — **CLOSED 2026-09-06** | the canonical loops and the preview call the same `TravelStage / UpdateStage / PostUpdateStage / ApplyLockstepFrameToActor`; on those stages the D=0 e2e stayed `3292e7fc…` (`test_trust_20260905_231733_42212/e2e_skipped_check`) and the 900-tick buy replay stayed identical (`rb_replay_buy_v2_20260905_232051_42212`); SimBaseline 60 ticks identical to the pre-change build. (The D=0 hash later moved to `3fa6c8f7…` with the intent change below — a separate, explained change.) |
| G3 isolation (re-pinned 2026-09-05 22:30, user + follow-up review) — **CLOSED 2026-09-06** (`test_trust_20260906_002119_39068` cases 15–17 + `fx_pickup_fire_tip_20260906_002045_18648`; details in Progress) | **the speculative pickup SUCCEEDS**: the preview actor equips the intended weapon and performs a dependent action (fires it) inside the preview; discarding the preview leaves the canonical weapon, actor, membership, ownership, timers, queues, RNG and script registrations unchanged; the recorded input then applied normally causes exactly one canonical pickup, no lost/duplicated object, no repeated effect, and the run matches its no-preview reference; the injected illegal canonical mutation still fails; the fixture exercises a real draw between render substitution and its undo. A refusal counter or a suppressed pickup cannot close this gate (the canonical-write guard stays as a tripwire only). The tick-141 host/client desync of `fx_pickup_20260905_222157_34948` is the permanent regression this gate protects. |
| G4 invariance | invariance selftest green for every depth × repeat, Lua bindings included, resume equals the no-preview reference |
| G5 resume | probe reinstatement: script counters + persistent vars equal the reference; heal path semantics asserted and documented (Create re-runs, save-format persistence) |
| G6 fixtures | every listed transition green with its semantic assertion; minimizer produces a small repro from an injected failure |
| G7 presentation | headed evidence inspected; measured RTT and the pinned numbers met or the miss reported as open |
| G8 breadth | matrix 16/16, 3/4-peer, lifecycle; remote legs reported by what ran |

### Progress (evidence dirs under `D:\Projects\stage2_p4\`)

- **S1 first pass 2026-09-05 22:12** (the follow-up review then reopened G1; closed 2026-09-06 below) — recorder close · replay outcomes
  + `-net-replay-verify` · invariance test + fault injection; harness v2 + fault
  battery are scripts in `stage2_p4\`, not in the repo (history re-split 2026-09-05 23:00, see below):
  - Playback classifies its end by an enum set where detected (`completed | tick_cap | truncated |
    corrupt | sim_failure`), exits 2/3/4 on the failures, records outcome/frames/last tick/end
    marker in the trace; `-max-ticks` bounds a playback as `tick_cap`. `-net-replay-verify` scans
    a file (decodes, gaps, end marker). **All three older recordings were truncated** (the
    recorder only closed when the coordinator was cleared, which the e2e completion path never
    did) — fixed; fresh recordings verify `ok` (`s1_20260905_215529\verify_fresh.txt`).
  - Harness v2 (`run_interp_e2e.ps1`): unique run dir per run, `provenance.json` + `result.json`
    (exe SHA-256/mtime/size, git HEAD + dirty list + diff hash, build config, settings, one named
    check per property, `provenance_stable` re-checked at the end), no substring PASS,
    `prediction_executed` from the report's real preview counters (D=0 → `n/a`,
    `-RequirePrediction` forces it), replay = integrity scan + structured outcome + compared-tick
    coverage. `-ExactOutDir` keeps a caller-chosen dir.
  - Preview invariance test `-local-prediction-invariance T:depths:repeats` + `-local-prediction-depth`
    (playback reads ahead and previews the recorded human): at tick 300 of the buy recording,
    depths 1/3/8/14 × repeats 1/3 leave the full dump + clock/accumulator + RNG state/draws + UID
    counter + queues + activity scalars + terrain layers + Lua registrations **byte-identical (8/8)**,
    and the resumed run matches the no-preview reference for 900 ticks
    (`s1_20260905_215529\lpinv3.txt`, `ref.json`). The one hole it found: the RNG draw counter was
    not restored (now is).
  - **G1 fault battery `test_test_trust.ps1`: 9/9** (`test_trust_20260905_221219_29548\result.json`):
    truncated + corrupt recordings fail verify AND playback by name; one altered hash fails the
    compare; `CC_FAULT_INJECT=snapshot_skew` fails the probe (restore mismatch);
    `preview_mutate_canonical` fails the invariance test; the clean control passes; a
    prediction-off run under `-RequirePrediction` fails the harness.
- **S2 DONE (red case reproduced live) 2026-09-05 22:22:** the `UInputMan`-boundary input script
  (`-input-script`, `0fe9f69e7`) drove the host's dummy through switch → drop → walk → pickup in a
  2-peer e2e; the host's preview clone picked the dropped rifle up for real and the peers desynced
  at tick 141 with the host's items hash gone (`fx_pickup_20260905_222157_34948\simgated_compare.txt`)
  — finding 3 live. Also found on the way: the render substitute aborted `Actor::DrawHUD`'s roster
  lookup (fixed by giving the substitute the roster and activity slots for the draw, `daa72418a`).
- **Follow-up review 2026-09-05 22:26 (`reviews\2026-09-05-corrective-progress-followup.md`) —
  all accepted, open until closed:** (1) G3 re-pinned above: the refusal guard (`3300c5d7b`) makes
  the fixture pass host==client but only by suppressing the pickup — NOT the acceptance condition;
  the preview must take the item against ISOLATED speculative state (a shadow copy) and fire it.
  (2) G1 is not closed: `s_lpInvarianceFailures`'s -1 sentinel and a probe scheduled beyond the
  tick cap are never checked at exit; malformed/zero repeats, missing results and skipped required
  properties need failing regressions; harness provenance is captured BEFORE an optional build,
  misses staged/untracked source (the new InputScript files were untracked when measured) and the
  scripts/fixtures/module manifest; a full-to-end `completed` playback must stay a permanent
  positive case; `DescribeScriptBindings` is registration + init flags, not Lua value identity.
  (3) The unpushed split is broken: `42b35eb80:Source/Main.cpp` has replay statements inside the
  discovery-selftest `if` and a top-level block after `WinMain` (zero-context hunks landed at stale
  offsets); rebuild the local history into coherent, individually BUILT states before any sharing.

- **History repaired 2026-09-05 23:00:** the six unpushed commits were rebuilt from `45a185779` as
  coherent states (`973aca342` recorder close → `e374e332d` replay outcomes/verify/lookahead →
  `1cf8f08ae` invariance test + faults → `6419db956` input script → `9f6ecc02d` render identity →
  `76b5348f2` refusal guard) with exact intermediate contents (the reversed tree equalled the base
  byte-for-byte, the new tip equalled the old tip); every cumulative state was BUILT and BOOTED
  (`resplit_verify_20260905_2300\`: 6/6 — SimBaseline 60 ticks identical at every state, replay
  verify + bounded playback from state 2, the input script from state 4). Backup of the old history:
  `backup/pre-resplit-223220`.
- **G1 CLOSED 2026-09-06** (`d59326efc` engine · harness v3 + battery v3 in `stage2_p4\`):
  a requested invariance tick or fidelity probe that never executed now FAILS the run by name in
  playback, scenario and e2e modes (`lpinv_result=not_run`, `rbprobe_result=incomplete`), and the
  invariance failure code propagates out of every mode; zero/malformed depth-repeat specs and a
  bad preview depth are rejected up front; the harness captures provenance AFTER the build
  (`built_by_this_run`, build start vs exe link time) or, under `-SkipBuild`, fails
  `binary_matches_source` when any build input is newer than the exe; provenance covers committed
  + staged + unstaged + untracked source and hashes the harness scripts and fixtures; the module
  manifest and deterministic-config hashes must agree across peers (`content_identity_agreement`);
  a per-mode required-check manifest fails a flow that skipped a property
  (`required_checks_present`); test-only faults `-FaultSkipCheck` / `-FaultNewerSource` prove it.
  Battery `test_test_trust.ps1`: **22/22** (`{battery}`), including the permanent positive
  full-to-end `completed` playback (902 frames, end marker) on the re-recorded
  `fixtures\buy_900.ccreplay` (`{buy_v2}`; the old `rb_replay_buy` recording was itself truncated).
- **G2 CLOSED 2026-09-06** (`d5d6fb39e`): the per-object stages of a tick are single functions
  shared by the world update and the preview (the preview had skipped ApplyForces/ApplyImpulses and
  ran NewFrame and the wire frame before the travel). D=0 e2e `3292e7fc…` unchanged
  (`{d0_stages}`), buy replay 900 ticks identical, SimBaseline identical.
- **G3 CLOSED 2026-09-06 — the ownership boundary is an isolated execution context, not a guard**
  (`{c_overlay}` overlay · `{c_gate}` gate · `{c_intents}` intents):
  - `MovableMan` speculation: while a preview runs, every lookup (`GetMOFromID`,
    `FindObjectByUniqueID`, `ValidMO/IsActor/IsDevice/IsParticle`) resolves a resident to a faithful
    **shadow** created on first use; `Remove*` on a shadow takes it out of the overlay's world and
    hands it to the caller; spawns go to the marked queues; rosters and sort flags are fenced; the
    activity's control/brain/team-active/evacuation state rides `RollbackState` (the two early
    returns are gone); faithful links resolve inside the clone's own tree first; the clone adopts
    its original's MOIDs so its rays ignore the original; the sim checksum drops speculative
    feeds. **Violations** (removing/purging a resident, queueing a wire command from speculation)
    are counted, logged, asserted in debug builds — the fixture requires 0.
  - Live: `{fx_v2}` — D=3, the pickup+fire fixture through the input script: host==client 600
    ticks, host previews took 1 resident, violations 0, recording intact, playback identical.
  - Playback gate (battery case 15, `-lpinv-expect "equip=Battle Rifle@143~2,fire@161"` at tick
    142, depths 1/3/8/19 × repeats 1/3): depth 1 takes nothing yet, depths 3/8 take exactly one
    resident and hold it, depth 19 also fires it (rounds 15→14, tracer + casing spawned), every case
    draws a real frame through the substitution, and every case leaves the world byte-identical
    (dump + clocks + RNG + UID counter + queues + activity slots/flags + rosters + terrain + Lua
    registrations + render-hidden set + registry) — **8/8**. Case 16: `CC_FAULT_INJECT=
    preview_take_canonical` (the theft the overlay prevents, injected after the preview) FAILS the
    gate. Case 17: the run after those previews matches the no-preview playback for 221 ticks.
  - The tick-141 regression stays: `fx_pickup_20260905_222157_34948` (the refusal-guard era's
    `fx_pickup_20260905_222547_24592` passed only by suppressing the pickup).
- **Found and fixed on the way (all measured, see the traces in the run dirs):**
  - **The wire re-applied absolute actor state every tick** (aim angle, facing, view point, hand
    positions, equipped items, sampled at t, applied at t+D on every peer). Measured on the
    fixture's recording: after the canonical pickup at 143 the held rifle flipped between the old
    and the new one every 4 ticks (D+1) for good; keyboard aim integrates once per D+1 ticks; the
    hand offsets were re-derived from absolute positions every tick even at D=0. Now only what the
    owner's AI pass changed rides as **one-shot intents** (frame flags 0x4/0x8/0x10); the
    controller-log replay keeps its absolute boundary. **D=0 e2e reference moves `3292e7fc…` →
    `3fa6c8f7…`** (`{d0_intents}`; first divergence tick 113 `particles`, attributed by
    experiment: re-adding only the per-tick absolute hand-position apply brings the D=0 hash back
    to exactly `3292e7fc…`, `d0_handpos_experiment_20260906_002406_4516`, reverted afterwards). D=3 fixture host==client `{fx_v2}`.
  - Playback's viewer seat cannot follow the recorded human's actor switches (that input is
    presentation-level and not recorded): the preview previewed the brain. Fixture playbacks now
    run with the fixture's `-input-script` (the harness passes the host's to the replay).
  - A picked-up firearm's fire timer restarts at the pickup (250 ms for the Battle Rifle); the
    fixture fires 14 ticks after the pickup lands. The shot leaves the tick after activation.
  - The invariance hook sits inside the checksum's tick window; speculative carves fed
    `carve_math` — suppressed for all speculation (the live preview runs outside the window).
- **Found, NOT fixed (each needs its own gate in S4):**
  - After NEXT switches control from the brain to the dummy, the brain's controller keeps taking
    player 0's inputs (its input mode flips player/AI with period 4; both peers agree, the recorded
    frames show it moving with the dummy). Gameplay defect in the P4 duel / control switch.
  - The owner applies its AI's direct writes (aim, facing, equips) at tick t; remotes at t+D — a
    D-tick divergence window that no fixture exercises yet (the P4 AI never re-equips).
  - The preview's reach ray is a random cast, so its pickup can lag the canonical one by a tick
    or two (the gate's `~2` slack); MO-vs-MO collisions of the clone are still off (HitsMOs).
- **Next:** S3 remainder (probe restore via set-aside originals + Lua identity, state inventory +
  check), then S4 fixtures (control switch, AI re-equip, aim, reload, jetpack, gib/death, doors,
  cargo, buy/deliver, mod scripts) + minimizer, then S5 headed/perf/breadth.

**Order statements elsewhere are superseded by this section** (P8 §header, H4 §header, and the
older "Canonical order after H4" below all describe the July order).

## ⚠️ FIRST TASK BEFORE ANY NEW CODE — re-baseline after the GNS rebuild

The GNS prefix was deleted and rebuilt on 2026-09-05 (§J). Same version (v1.6.0), but a **freshly
compiled** one. The exe was rebuilt against it and **9/9 selftests pass**, but the full matrix and
the canonical baseline hash have **not** been re-run since.

**Baseline re-verified 2026-09-05 late on the rebuilt GNS:** 2-peer 600-tick e2e host==client
`3292e7fc…` byte-identical, replay test PASS, 9/9 selftests (`stage2_p4\e2e_gate_deep\`). **Still
outstanding: the full 16-case matrix** (`run_interp_matrix.ps1`). If a matrix case fails that the
baseline passes → suspect the GNS rebuild before your code; report it, do not debug it as a
regression.

Also stale and untouched since 2026-07-10: the **WSL2** tree (`/home/erol/xarch-combat-p4`) and the
**macOS** leg. Both need a re-sync before their next run (§D).

## After the optimistic driver: H4 Phase A step 3

Per **`D:\Projects\STAGE2_H4_RECONNECT_PLAN.md`**. **Obey the plan's pin-first rule:** pin and
record ALL remaining §4/§5/§7 protocol values (challenge lifetime, provisional-seat expiry, txId
cache-retention window, resource caps, retry cadence, confirmed-session-end definition) in the plan
doc BEFORE writing transaction/lifecycle code. The H4 design is **SETTLED** — build it, do not
re-design it.

### H4 = authenticated reconnect + host moderation. Progress so far:

- **Step 1 — crypto foundation** (`6082a17f8` provider · `c24da93aa` seat credentials + epoch ·
  `c4c185a64` selftest): `NetAuthCrypto` (OpenSSL `RAND_bytes` + HMAC-SHA-256 via `EVP_Q_mac` on
  GNS builds; fail-closed default otherwise; test override only via `SetNetAuthCryptoForTest`;
  `NetAuthConstantTimeEquals`). `NetSeatAuthRegistry` owned by `NetMatchService` — 16B off-sim
  epoch armed in WorkerMain's host path, cleared ONLY at true session end (`Destroy` /
  `ReportRuntimeError`) so it survives resync/rejoin/rematch; 32B per-seat per-holder-generation
  credentials, constant-time verify, revocation never rewinds generations. 8th selftest
  `-net-auth-selftest` (RFC 4231 KATs 1-4/6/7, RNG sanity, failure-injection, registry semantics).
- **Step 2 — §0 admission isolation, BOTH planes** (`5cecc5f88` · `5964292e4` · `1ab7fc2e1`):
  a new `NetTransportEventType::LocalTransportFault` distinguishes our own receive pump breaking
  (genuinely fatal) from per-connection `ConnectionFailed`/`TransportError` — the split is by
  TYPE, never string-matched. Coordinator `HandleEvent`: `LocalTransportFault` → Fail; a
  per-connection fault → non-fatal on the relay host, still fatal on a client's lone link; an
  undecodable `PacketReceived` → Fail only from a bound/trusted source, else counted and dropped
  (`m_Stats.ignoredAdmissionFaults`). `NetSession::ProcessEvent` mirrors this and closes the
  pre-match lobby DoS. 9th selftest `-net-admission-selftest` proves it adversarially.
- **Known follow-up (plan §0):** `NetLobbySession` (the rematch/resync lobby round) still fails on
  unbound joiner faults. Identical fix applies; deferred to §4 where it can be gated.

### Binding execution guidance for H4 (user-relayed 2026-07-10)

1. Do NOT reopen the C+/Phase-A/Phase-B architecture; do NOT write the three later plan docs yet.
2. Preserve §0's fatal-vs-nonfatal split — the local transport's OWN receive failure stays fatal;
   never blanket-ignore `ConnectionFailed`/`TransportError`.
3. Crypto may land first, but NO new live admission traffic until admission isolation is
   adversarially green in BOTH planes (`NetLockstep` AND `NetSession`). — **done as of step 2.**
4. The exact drop-time ownership LEDGER is the Phase-A target; the reseat is system-authored,
   synchronized, applied on every peer after coordinator handoff/snapshot load and BEFORE resumed
   tick 1. Do NOT take the default-reseat fallback without STOPPING and reporting why the ledger
   is infeasible.

### Canonical order (superseded twice; current = §B-0)

2026-09-05 morning: close tick-313 → save-holes 1-4 → fidelity fixture → H4 Phase A step 3 → H4
Phase B → increment 6 → SimChecksum re-pin dead last. **2026-09-05 late: §B-0 S1-S5 first** (test
trust → boundary regressions → boundary refactor → permanent tests → presentation/perf/breadth),
then H4 Phase A step 3, then the §G roadmap.

### ✅ 2026-09-05 late — the in-memory snapshot exists and the 300:30 probe PASSES

The old text-save probe path was the wrong mechanism (~300 ms restore, hand-listed fields). Built
`MovableMan::CaptureWorld/RestoreWorld` — faithful `Clone()` of every resident MO under a
`FaithfulCloneScope` (keeps UniqueID, copies live sim state that spawn-clones reset, skips registry
pollution), plus Activity funds/deaths/state and pending deliveries, Lua frozen inside the window
(`LuaMan::SetScriptsFrozen`). A strict audit found **173 members no `Create(ref)` copied**; the
faithful blocks close them. Structural holes found by running: `Attachable::SetParent` reset every
attachable timer + fired `OnAttach` on clone (guarded), subgroup atom offsets re-derived on
re-attach (stashed + adopted), attachable/wound mass and inventory mass double-counted (stash
preference fixed), `UnregisterObject` erased by UID (now pointer-checked). Probe `300:30` → **PASS**.
Fuzzer `-rollback-fidelity-fuzz seed:count:K` → 29/29 PASS on the 2-peer recording after the last
structural fix (`Attachable::SetParent` re-derived every child's pose from a not-yet-positioned
parent — guarded). Committed per concern: `3e0edf6b5` faithful clone mode · `4747e82b4` script
freeze · `c84f90cd2` activity scalars + deliveries · `e286afc12` CaptureWorld/RestoreWorld ·
`e97fc9e38` in-memory probe + fuzzer.

### ✅ 2026-09-05 later — the hash gate was too weak; the full-dump gate closed the rest

A richer recording (buy + deliver, 900 ticks, `stage2_p4\rb_replay_buy\`) failed 1/17 at capture
809 — and the failing probe could not be reproduced standalone. Cause: **the 30-tick SimChecksum
gate had been passing restores that were not identical** (a gun's activation + reload flags, arm and
leg poses, attachable order, a rifle's stale subgroup atoms). The probe now compares the **full
per-MO dump** — captured vs restored at T, and every window tick pass1 vs pass2 — and fails on any
byte (`4ca82cd3a`). That exposed and closed, by measurement (dump diff → tracer → backtrace):

- `HDFirearm::SetParent` deactivates + clears reload state on every attach; `Turret::SetParent`
  deactivates mounted devices; `Attachable::SetParent` took team + flip from a parent not yet copied
  → all skipped for faithful clones (`4f9f3eea2`).
- attachable order: hardcoded attachables re-attach in declaration order (magazine before flash
  where the live gun had the reverse) → live order recorded, sorted back in `ResolveFaithfulLinks`
  (`611dcaa32`).
- `m_RadiusAffectingAttachable` never carried → the parent's refresh of every attachable's position
  fired only live → a leg's ankle lerp read a stale joint pos after restore → UID link (`611dcaa32`).
- atom groups: the owner-only copy + re-add could not reproduce a rifle that kept its magazine
  subgroup from an earlier root (30 atoms live vs 19 restored) → faithful clones copy the whole
  group verbatim, attachables skip the root re-add, the offset stash is retired (`b3b209aa2`).
- **engine bug:** `AtomGroup::Clear()` never initialised `m_StoredOwnerMass` → every new wound read
  stack garbage (memory-dependent values in the dump; sim-neutral only because MOI==0 forces the
  recompute) → initialised (`f9e3df2b5`). Upstream-worthy; **HELD** with the rest of the feed.
- the muzzle flash frame rides `g_RenderRNG` — legitimately non-sim, excluded from the dump.

**Result: 6 fuzz runs, 74/74 probes byte-identical on hash AND full dump** — buy recording
17/17 (30-tick, seed 1), 16/16 (30, seed 7), 7/7 (60, seed 12); 2-peer recording 16/16 (30),
7/7 (60), 11/11 (45). Baseline `3292e7fc…` unchanged, replay PASS, 9/9 selftests. The snapshot is
the foundation the optimistic driver builds on.

### ✅ 2026-09-05 late — local prediction shipped (`874b782a5` · `bc3c2465d` · `45a185779`)

**The measurement first** (`bc3c2465d`, `[rbprobe] capture_ms/restore_ms`, `CC_CAPTURE_PROFILE`):
an in-memory world capture costs **~4.7 ms and a restore ~4.8 ms** on the buy recording — the
terrain layers are cheap (0.2-1 ms), the cost is the **AHuman clones at 0.5-1 ms each**
(attachable trees, atom groups, sounds, scripts), particles are negligible. With the canonical tick
at ~5.1 ms, a full-world optimistic branch (capture + D re-sims + restore per drawn frame) fits only
at D=1 on this box and cannot hide a 200 ms delay (D≈12-14) — that needs a ~5-10× cheaper actor
clone or re-sim (the sim-speed program). **So the felt latency is removed where it is felt: on
the local actor.**

**`LocalPrediction` (`Source/Managers/LocalPrediction.cpp`):** before each drawn frame, every
locally controlled human actor is faithfully cloned (unregistered), the clone is run through the
local `ControllerFrame`s already queued for the next D ticks (`PeekLockstepLocalControllerFrames`
— the input-delay pipeline the coordinator holds), and the clone is drawn in the actor's slot
(`SwapActorForRender`), so the frame shows the newest input at zero added latency. The canonical
sim is untouched by construction: the clone hits no MOs (`SetToHitMOs(false)` down the tree), its
spawns and alarm events are dropped (`MarkAddQueues`/`DiscardAddedSince`), terrain layers, activity
scalars, `g_SimRNG`, the sim clock (`AdvanceSimTickForPreview`/`RestoreSimTickAfterPreview` — the
frame accumulator is left alone; zeroing it was the first bug) and the UID counter are put back,
Lua stays frozen, sounds and post effects are suppressed while it runs, and the seeing pass is
waited on first. The camera follows the preview. Previews are reused while the sim tick has not
advanced. `LocalPrediction`/`LocalPredictionMaxTicks` settings (on/20), `-net-local-prediction
on|off` for gates, `CC_LOCALPRED_TRACE=1` phase trace, `[localpred]` counters on the match report.

**Gates (all BUILT and RUN):** D=0 baseline `3292e7fc…` unchanged · D=3 e2e sim-gated PASS with
596 previews at **1.3 ms avg** (3 actor-ticks each) · 200 ms fake lag (auto delay: host 1, client
14) sim-gated PASS, client previews **1.2 ms avg at depth 14** · 9/9 selftests. `run_interp_e2e.ps1`
gained `-ExtraArgs` for such one-off knobs.

**Known cosmetic limits (next polish, in order):** sounds/muzzle effects are suppressed in the
preview so they still land D ticks late (a ledger dedup — preview plays, canonical skips the
matching event — is the fix); optimistic projectiles are dropped (the shot appears at the
canonical tick); the Activity-drawn UI (pie menu, cursor) sits at the canonical actor. Exact
full-world corrections at D≤2 remain the follow-on once actor clones get cheaper.

### tick-313 — CLOSED (history; the in-memory snapshot replaced the text-save path)

The rollback fidelity gate: snapshot @300 → run 30 ticks → restore → re-run 30 → per-tick hashes
must be byte-identical. The text-save restore **failed at tick 313**: brain actor only, `rot_angvel` only,
`-0.00567003479` vs `-0.00567000918` (~75 ULPs), appears in the Update phase (phC→phD). The full
`CC_SIM_DUMP` at 311 and 312 is byte-identical between passes ⇒ **the differing state is
un-serialized AND un-dumped.** This is NOT sim nondeterminism — it is a save-format completeness
hole. It moved 301→313 when iteration 2 closed the tick-301 residuals (shrinking tail).

Leads, in test order: **(a) un-serialized Actor/AHuman enums** — `m_MovementState`,
`m_ProneState`, `m_ArmsState`, `m_ArmClimbing`; **the upright spring's target is
`m_RotAngleTargets[m_MovementState]`** — directly feeds rotation, matches the symptom exactly.
(b) recoil trio `m_Recoiled`/`m_RecoilForce`/`m_RecoilOffset` — `Clear()`ed, never saved. (c)
wound-emitter torque arms. (d) `Matrix` trig-cache internals (angle prints identical; cached
sin/cos pair isn't dumped). Evidence: `stage2_p4\p8_4_rng16..18` (rng18/probe2 = the identical
311/312 dumps). Probe exits 1 with "passed=no" — grep `[rbprobe]`.

**The previous agent parked this as "accepted known FAIL@313". The user rejected that on
2026-09-05: no compromises means it gets closed, not accepted.** Closed the same day by replacing
the mechanism (above): the 300:30 probe and every fuzz window now pass on the full-dump gate. The
text-save path (rejoin / desync-heal state transfer) still has its own 4 known holes (§G deferred).

---

# §C. THE RULES

## Causeless's rules (upstream lead — architecture + conduct)

1. **Controller-sync MP, NOT deterministic-AI.** The AI runs per-machine (off-wire, async
   pathfinding allowed); ONLY each actor's Controller crosses the wire. Never determinize the AI
   cross-machine. Discrete player actions cross as owner-issued, tick-stamped game commands; the
   command's RESULT is on-wire and must be bit-identical.
2. **FPU + libm standardization, never fixed-point.** The deterministic trig/exp polys in
   `RTETools.h` + `/fp:precise` + `-ffp-contract=off` are the cross-platform math foundation.
3. **Small, focused, per-concern PRs/commits** — one sentence of purpose each; <~300 line target
   (up to ~600 for unavoidable engine changes); stacked series over mega-bundles. A PR doing
   "small fix + nearby refactor + unrelated cleanup" is three PRs.
4. **Comment style — terse, one line, almost never two.** Plain block labels
   (`// Run seeing rays for all actors`), non-obvious WHY only, TODOs name the constraint, **NO
   fix-narration**, NO milestone/ticket tags in code, `///` Doxygen on header decls. When fixing a
   root cause, DELETE the stale workaround comment rather than leaving it to rot. If Causeless
   wouldn't write it, delete it — over-commenting is a credibility tax on the whole fork.
5. **Never send a broken PR.** Verification = BUILT and BOOTED/RUN, never reasoned. Every
   cumulative state a one-at-a-time merge creates must be built + run before submission. Upstream
   CI/review is NOT our backstop. If you catch yourself writing "covered by logic" / "should be
   fine" / "X implies Y" in place of a result — stop and run it.

## The user's rules (Erol / Madreag)

- **Commit as you go; push at every verified checkpoint, several times a day (2026-09-10; supersedes the 2026-09-09 pushing paragraph and every older 'commit/push only when asked' or 'pushing is gated' clause in this document).** Focused per-concern commits on the worker branch as soon as a piece is built and run; verified checkpoints on the milestone branch are pushed to origin promptly, not every few days, without a per-push OK. Before a push verify the exact source against the pinned manifest, record the push here, state remaining failures; a checkpoint push is not milestone completion; no upstream PRs. Cleanup: authorised 2026-09-10 on the condition that every item is reviewed first and nothing of value is lost (see AGENTS.md).

- **No AI attribution anywhere** — commits, PRs, branches, doc footers, Discord drafts. Author of
  record is the user, always. No `Co-Authored-By`, no "Generated with" footers, no AI signatures.
  *(Reaffirmed emphatically 2026-09-05.)*
- **No upstream actions without express per-action permission**: no upstream PR open/update/close,
  no upstream comments. Pushing to our own fork `origin` is NOT gated any more (rule above); the
  bots on the fork may run.
- **"No compromises"** — see §A.
- **Verify before stating; measure, don't infer.** Never claim green without a run. Partial
  coverage is a checkpoint, never "achieved". Run verification yourself — never ask the user to run
  what you can run.
- **Momentum + decisiveness.** Do the work and report it done; don't over-checkpoint. Present
  options in prose. Honest pushback welcome.
- **Do big writing tasks yourself** — don't delegate plan docs / handoffs to subagents.
- **Don't use subagents/workflows unless asked.**
- **No competing runs**: while WSL2 builds/tests on this PC, stay analysis-only on Windows. The
  macOS Mac Mini never contends.
- **Doc rule**: when a milestone lands or the plan changes, update the pointer surfaces IN THE SAME
  SESSION: `CLAUDE.md` + `AGENTS.md` mission block, this file's §B, `PR_ROADMAP.html`, the active
  phase plan's header. **Stale pointers cost a full re-investigation (proven twice).**
- **Scope**: only make changes directly requested or clearly necessary. But when fixing something,
  proactively fix all related references (docs, configs, scripts) in the same pass.

## MP-sim rules (learned the hard way; all BINDING)

- **Player bindings are PER-PEER** (`PlayerActive`, `GetPlayerBrain`, `m_Team[player]`,
  `IsHumanTeam` via local players, `ScreenOfPlayer`) — **NEVER in sim decisions.** Use team-level
  MovableMan queries; under lockstep `Activity::IsHumanTeam` resolves from the synced match config
  (`ScenarioRunner::IsLockstepHumanTeam`).
- **The AI may only drive the Controller.** Any direct sim mutation from AI Lua
  (`Owner:ReloadFirearms()` was the caught case, 15 sites) forks the sims — signal controller states
  instead (`Controller.WEAPON_RELOAD`, the `BODY_JUMPSTART` precedent). Equip-family AI mutations
  self-heal because the P1B frame re-applies equipment each tick; anything NOT carried by the frame
  does not.
- **An actor leaving the controller wire (death) gets its controller disabled the same tick**; an
  actor JOINING mid-tick is quarantined off its per-machine controller until the next controller
  update hands it to the wire (`MovableMan::AddActor` quarantine, released in `UpdateControllers`).
- **A synced command must NEVER silently no-op on one peer** — log it loudly (console + stdout).
- **MP activity logic uses sim time** (`IsPastSimMS`), never wall clock. The delivery queue, win
  windows, and the pause countdown are all sim-tick/sim-time driven.
- **MO UniqueIDs are pinned at every deterministic launch** (`PinUniqueIDCounter(1<<20)` in
  `ApplyDeterministicConfig`) so UID-addressed commands hold across rematch rounds. This shifts all
  trace hashes vs pre-pin archives; suites record+replay in-run.
- **Per-machine Settings that steer sim decisions must be pinned or identity-gated.** Pinned:
  timestep, `AutomaticGoldDeposit`, crab bombs (enabled + threshold). Identity-gated (reject at
  join): enabled global scripts, lockstep codec version, nls, module manifest, dt bits, AI update
  interval, pathfinder node size, MOID count, settling/subtraction.
- **Unhashed sim state can diverge silently under a green gate for hundreds of ticks** (jet fuel,
  weapon rounds, Status). When a divergence resists analysis, **DUMP MORE STATE** (`CC_SIM_DUMP`) —
  every bug in the big hunt fell in one dump-run each once the right field was visible.
- **No new code assumes 2 peers** — iterate the remote-peer set, never a singular remote.
- **e2e session seeds are FIXED** — per-case traces are comparable across runs; bisect-by-stash
  works.

## Standing invariants

Only fix what makes an **ON-WIRE** subsystem deterministic — the AI/controller is OFF-WIRE and
per-machine. **Do NOT re-apply the reverted off-wire fixes**: SpatialPartitionGrid MO-query sort,
controller release-timer sim-time, alarm-event sort, PathFinder node sort, Lua AI-aim `^`. (Record:
`NIGHT_LOG.md`, `PRs\DETERMINISM_CS_KEEPSET_PACKAGE.md`.) Every sim-mutating path needs the
two-window hash gate (host == client, controller excluded); **headless green alone is NOT
"playable"** — that was the pre-recovery P4A overclaim.

## Architecturally REJECTED — do not bring forward, do not re-litigate

These were tried on `modernization-effort` and are deliberately not part of the foundation:

- **M3 Q40.24 fixed-point math.** Causeless: "really no benefit" given Lua-float cross-platform
  determinism. FPU + libm standardization is the chosen path (ADR-019).
- **Pathfinder serial-epilogue (M4A)** — moving `Scene::UpdatePathFinding` after
  `MovableMan::Update`. Required by deterministic-AI; redundant under Controller-sync.
- **Race A architectural fix** (consumption-side pathfinder serialization). Same reason.
- **The `detmath` musl vendor / FP-env as baseline / scoped-libm.** Superseded — detmath was inert
  for the measured paths; the lean deterministic primitives are the fix. **A deterministic POLY,
  never a heavy vendor.**
- **M5.5 mega-bundling** — the same code comes forward, split per concern.

---

# §D. HOW TO BUILD, RUN, AND VERIFY

## Windows (primary)

```powershell
$env:GNS_ROOT     = "D:\Projects\stage2_p2\gns_spike\install-win-vcpkg-release"
$env:GNS_DEP_ROOT = "D:\Projects\stage2_p2\gns_spike\build-win-vcpkg-release\vcpkg_installed\x64-windows"
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"   # LITERAL path
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
& $msbuild "D:\Projects\p4b-interp-validation\RTEA.sln" /t:RTEA /p:Configuration="Final" /p:Platform=x64 /m /nologo /v:minimal
```

*(These env vars are also set permanently in three places — Windows user env, `~/.bashrc`, and
`~/.claude/settings.json` — per the machine's CLAUDE.md rule. Claude Code shells are non-login and
read ONLY `settings.json`.)*

- Configs: `Debug Full` | `Debug Minimal` | `Debug Release` | `Final`. **There is NO plain
  "Release" config.** `/t:RTEA` incremental, `/t:RTEA:Rebuild` clean; a `:Build` target does NOT
  exist. Allegro is a separate vcxproj that `/t:RTEA` does NOT rebuild.
- Kill prior instances:
  `Get-Process | Where-Object { $_.ProcessName -like "Cortex Command*" } | Stop-Process -Force`
- The exe is GUI-subsystem: **PIPE output** (`2>&1 | Out-Host`). A `>` redirect **DETACHES** the
  process — empty exit code, no output, looks like a crash but isn't.
- MSB4011 duplicate-import and luabind C4297 warnings are pre-existing noise.

## ⚠️ If the GNS prefix is missing — rebuild it

`GNS_ROOT` / `GNS_DEP_ROOT` point into `D:\Projects\stage2_p2\gns_spike\`, which is **build output,
not source, and is NOT in git.** It was accidentally deleted 2026-09-05 and rebuilt. GNS links
**statically** (`GameNetworkingSockets_s.lib`), so an already-built exe keeps working without it —
only recompilation breaks.

```powershell
# 1. vcpkg — must be a FULL clone; --depth 1 breaks the manifest baseline lookup
git clone https://github.com/microsoft/vcpkg.git D:\Projects\gns_rebuild\vcpkg
D:\Projects\gns_rebuild\vcpkg\bootstrap-vcpkg.bat -disableMetrics

# 2. GNS pinned to v1.6.0 — the version P2D was built and 3-platform verified against
git clone https://github.com/ValveSoftware/GameNetworkingSockets.git D:\Projects\gns_rebuild\GameNetworkingSockets
git -C D:\Projects\gns_rebuild\GameNetworkingSockets checkout v1.6.0

# 3. Build + install into the ORIGINAL paths so every doc reference stays valid
cmake -S D:\Projects\gns_rebuild\GameNetworkingSockets `
      -B D:\Projects\stage2_p2\gns_spike\build-win-vcpkg-release `
      -DCMAKE_TOOLCHAIN_FILE=D:\Projects\gns_rebuild\vcpkg\scripts\buildsystems\vcpkg.cmake `
      -DVCPKG_TARGET_TRIPLET=x64-windows -DCMAKE_BUILD_TYPE=Release `
      -DBUILD_SHARED_LIB=OFF -DBUILD_STATIC_LIB=ON -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF `
      -DCMAKE_INSTALL_PREFIX=D:\Projects\stage2_p2\gns_spike\install-win-vcpkg-release
cmake --build   D:\Projects\stage2_p2\gns_spike\build-win-vcpkg-release --config Release --parallel
cmake --install D:\Projects\stage2_p2\gns_spike\build-win-vcpkg-release --config Release
```

What the solution consumes: `$(GNS_ROOT)\include\GameNetworkingSockets`, `$(GNS_ROOT)\lib`
(→ `GameNetworkingSockets_s.lib`), `$(GNS_DEP_ROOT)\include`, `$(GNS_DEP_ROOT)\lib`
(→ `abseil_dll.lib`, `libprotobuf.lib`, `libcrypto.lib`). All guarded by `CCCP_WITH_GNS`; without
the env vars the build is GNS-free and still compiles (that's a required gate flavour). GNS runtime
DLLs are staged beside the exe (gitignored) — bare launches throw DLL dialogs without them.

**Lesson: treat any path referenced by an env var as a dependency, not as scratch.**

## The verification contract

**Build:** MSBuild `RTEA.sln`, config `Final` x64, with the two GNS env vars.

**Gates:** `D:\Projects\stage2_p4\run_p4a_menu_service_e2e.ps1 -SkipBuild -Port <fresh>` with the
case switches `-FundsCommandHost / -SpawnCommandHost / -DeliverCommandHost / -ScuttleCommandHost /
-BrainKillCommandHost / -StallTest / -MismatchSmoke / -PerturbHost / -RematchTest /
-InventoryCommandHost / -BuyCommandHost / -BrainSpawnTest / -PauseTest / -FullLoopTest`
(baseline = no switch; buy/pause/full-loop auto-bump to 900 ticks) + `run_runtime_desync_test.ps1`
(clean + perturb) + the 9 selftests + a 30 s SP boot (ALIVE at ~1490 MB).

**The 9 selftests** (from the active worktree's exe — all verified passing 2026-09-05):
`-controller-frame-selftest -net-protocol-selftest -net-identity-selftest -net-session-selftest
-net-lockstep-selftest -net-match-selftest -net-discovery-selftest -net-auth-selftest
-net-admission-selftest`

**Mismatch and perturb are POSITIVE controls** — mismatch must reject with the rich reason on BOTH
peers; perturb must be CAUGHT. **Control-plane-only changes:** build + affected cases + selftests.
**Sim-mutating changes:** the FULL matrix. **Canonical 2-peer PvP baseline hash: `3292e7fc…`** —
byte-identical is the bar. Other canon: 3-peer `273549b3…`, 4-peer `5a9f0007…`, co-op `b0f3ff99…`.

**Harnesses:** `run_interp_matrix.ps1` + `run_interp_e2e.ps1` + `run_interp_desync.ps1` (interp
worktree; they invoke MSBuild themselves, `-SkipBuild` reuses) · `run_p4c_3peer_e2e.ps1`
(`-Peers 3|4`, `-LeaveTest`, `-DropTest`, `-RematchTest`, `-Mode pvp|coop-pve|pvpve`) ·
`run_p4c_menu_3p.ps1` (GUI-driven 3-peer proof) · `loop_desync_probe.ps1` (repeat pairs until
Desync + auto terrain dump).

## WSL2 (Linux leg — same PC, CONTENDS with Windows builds)

The WSL tree at `/home/erol/xarch-combat-p4` is **NOT a git clone** (its `.git` file points at
Windows worktree metadata, unreadable from WSL). Sync by archive:

```powershell
git -C D:\Projects\xarch-combat archive --format=tar -o "D:\Projects\stage2_p4\p4a_tip.tar" HEAD
wsl -d Ubuntu -- bash -lc "cd /home/erol/xarch-combat-p4 && tar -xf /mnt/d/Projects/stage2_p4/p4a_tip.tar"
wsl -d Ubuntu -- bash -lc "cd /home/erol/xarch-combat-p4 && meson compile -C build-p4-gns"
wsl -d Ubuntu -- bash -lc "/mnt/d/Projects/stage2_p4/run_wsl_p4a_leg.sh"
```

Legs: `run_wsl_p4a_leg.sh` · `run_wsl_p4c_leg.sh` (8 selftests) · `run_wsl_p5_leg.sh` (9
selftests) — all in `D:\Projects\stage2_p4\`. The GNS prefix (`/home/erol/gns-install-p4`) and
builddir `build-p4-gns` already exist from P2/P3. Artifacts land in `stage2_p4/wsl_*_leg/`.
**Stale since 2026-07-10 — re-sync before trusting.**

## macOS (relayed by the user to the Mac Mini agent — arm64, never contends)

Runbook: `D:\Projects\stage2_p4\MACOS_P4A_PROMPT.md` — self-contained; relay the file's content as
a single message, nothing else needed. Later legs: `MACOS_P8_PROMPT.md`, `MACOS_P8B_PROMPT.md`.
Its Part 1 doubles as the **meson-reconciliation proof** (a clean GCC-13 configure proves #283's
clang-gated `<execution>` macros). **Stale since 2026-07-10.**

## Forensic tooling (permanent — this is how every desync was actually found)

- **`CC_SIM_DUMP=<from>:<to>`** — per-tick, per-MO exact-bit dump beside the `-out` trace
  (`<trace>.simdump.txt`): pos / vel / angvel / rot (hexfloat), controller state bits / mode /
  disabled, Actor status, jetpack fuel + emitting, equipped firearm rounds + reloading, per-tick
  activity line. Diff host-vs-client with `Compare-Object` per tick. **This localized every desync
  in one run each.** `ActivityState`: NotStarted=0, Starting=1, Editing=2, PreGame=3, **Running=4**,
  HasError=5, **Over=6**.
- **`CC_TERRAIN_EVENTS=<from>:<to>`** — in-memory, pacing-neutral event trace flushed beside `-out`
  on a Desync stop (`.desync.terrainevents.txt`): terrain dislodges / bakes / stain-reads (with the
  acting MO's uid), particle spawns (pos/vel bits + spawner), limb pushes, MO-hit points, received
  impulses. **`CC_TRACK_UID=<uid>[,...]`** adds per-tick bit-exact rows per tracked MO (pos/vel,
  rot/angvel/rest/osc, flags/prev) + an MXCSR/x87 row; in-tick phase stamps (`phA` pre-travel →
  `phD` post-update) bisect which phase forks first. Events-only arming preserves the pacing forks
  that file/hash arming suppresses. Analyzers: `compare_evt_run.py`, `compare_trk.py`. Battery:
  `run_interp_evt_battery.ps1`.
- **`CC_TERRAIN_DUMP=<tick>`** — material + FG colour layers as raw `.bin` (8-byte w/h header then
  w×h bytes per layer), also on a Desync stop.
- `-controller-debug-dump <path>` + `-controller-debug-ticks <ranges>` — per-phase controller
  snapshots around the P1B apply.
- `-menu-script <file>` — GUI automation (**rebuilt menus only**, not in-game GUIs);
  assert/activate/settext/timeout, failures fatal.
- `-net-replay-out` / `-net-replay` — `.ccreplay` record and offline playback.
- `-net-fake-lag` — synthetic latency (per-PROCESS, affects both send and recv legs).

---

## The rollback fidelity probe (the deep gate — run it after ANY change to Create(ref)/SetParent)

```bash
cd D:/Projects/p4b-interp-validation && export GNS_ROOT=D:/Projects/stage2_p2/gns_spike/install-win-vcpkg-release
# one probe: snapshot at T, run K ticks, restore, re-run K; PASS needs identical hashes AND identical full dumps
./"Cortex Command.exe" -net-replay D:/Projects/stage2_p4/rb_replay_buy/match.ccreplay -tick-hashes -max-ticks 900 \
    -rollback-fidelity-probe 300:30 -out D:/Projects/stage2_p4/rb_replay_buy/rb300.json
# fuzz: seed:count:window, capture ticks drawn from the seed. -max-ticks MUST be <= the recording length
./"Cortex Command.exe" -net-replay <rec> -tick-hashes -max-ticks 900 -rollback-fidelity-fuzz 1:17:30 -out <out.json>
```

Recordings: `stage2_p4\rb_replay_20260905\match.ccreplay` (2-peer, **600** ticks) and
`stage2_p4\rb_replay_buy\match.ccreplay` (buy + deliver, **900** ticks). Outputs beside `-out`:
`.rb_captured[_T]` / `.rb_restored[_T]` (dump at T before/after the restore — a
`RESTORE MISMATCH` line means the clone itself is unfaithful), `.rb_deep_<T>_pass1/2` (the first
window tick whose dumps differ). Diff them field-by-field with
`python D:/Projects/stage2_p4/rb_diffsim.py <T>` (run inside the recording's dir; reads `rb<T>.json.*`).
Localisation recipe that found every hole: dump diff → `CC_TRACK_UID=<uid> CC_TERRAIN_EVENTS=a:b`
tracer rows (`ankl`/`ank2` for legs, `limb`/`aimp`/`ghit`/`spwn`) → if a value changes with no sim
writer in sight, a temporary `CaptureStackBackTrace`+`SymFromAddr` print in the setter (the PDB is
beside the exe). The muzzle flash `frame=` is printed as `-` on purpose (render RNG).

# §E. ARCHITECTURE — how this multiplayer works

- **Deterministic lockstep, Controller-sync** (ADR-020/024). Stage 1 made the replicated SIM
  bit-identical across Win / Linux / arm64 *given identical Controllers* (branch `determinism-cs`).
  The **keep-set**: portable mt19937-state hash, explicit RNG-distribution mapping, Allegro integer
  `fixmul`/`fixdiv`, dt config-lock, deterministic trig/exp polys **at the primitive level**,
  eval-order fixes, Lua-state seed fix, PieMenu listener ordering, per-MO RNG, sim/render RNG split,
  Path E. The AI/controller is **OFF-WIRE and advisory** — excluded from every MP gate.
- **The wire (P1B/P3):** per-tick `ControllerFrame`s per actor (controls + pose / aim / facing /
  equipped UIDs / hand positions) exchanged via the `NetLockstep` coordinator over
  GameNetworkingSockets (P2D; default builds are GNS-free behind `CCCP_WITH_GNS`). **Ownership:**
  `TeamOwner` policy — each peer produces frames for its team's actors; remote actors get injected
  wire frames in `MovableMan::UpdateControllers` (which runs INSIDE `MovableMan::Update`, after
  `PreControllerUpdate`). The coordinator collects from a **SET** of remote peers (per-peer maps,
  advance only when all remotes are in, peerId-ordered merge) with a **host-star relay** — the
  2-peer path is a 1-element set and stays byte-identical.
- **Input delay is per-SENDER** (P8-1; lockstep codec v8, config v2). Auto-pick is
  `D_i = ceil(RTT(i,host)/tick) + 1` — **own round trip only**; receive legs settle as a run-behind
  offset, so any D is stall-free in steady state. The 33.3 ms divisor deliberately errs toward
  LOWER own-latency — **do not "fix" it** to the true 16.7 ms tick; that would double every
  player's felt delay for no smoothness gain. First D frames free-run empty.
- **Discrete actions = `NetGameCommand`** (`Source/Network/NetGameCommand.h`): SetTeamFunds,
  SpawnActor (with `aiMode`, codec v4), DeliverCargo (direct spawn OR `queuedPurchase` — the real
  buy order with cost, through `GameActivity::QueuePurchaseDelivery`, the sim core shared with SP),
  ScuttleCraft, InventoryOp (5 ops), PauseMatch, SwitchControl (co-op handoff, codec v6). Commands
  ride the lockstep frame, are sender-stamped by the coordinator, apply on both peers at the same
  frame after a **stable sort by sender** (`ApplyLockstepGameCommands`), and are authority-gated
  per team (`ResolveTeamCommandAuthority` — only the peer controlling a team may issue its economy
  commands; identical resolution on both peers).
- **The synced pause** is a null-tick freeze: `TimerMan::SetSimTimeFrozen` advances the update COUNT
  but not sim TIME; the main loop skips the sim body (`if (!lockstepPausedTick)`) but still
  exchanges empty frames + applies commands (so unpause and disconnect handling flow); resume = a
  shared 180-null-tick countdown. All tick-synced; no wall clock.
- **Safety nets:** runtime desync detection (sim-gated hash every 30 ticks exchanged on the wire →
  `Desync` stop on BOTH peers) · the join gate (`NetIdentity`) hashes build/version/codec versions,
  dt bits, nls, module manifest, sim-steering settings, enabled global scripts → mismatches reject
  at join with a **RICH reason surfaced on BOTH peers** · 20 s missing-frame grace with a "Waiting
  for the other player" overlay drawn from the blocked wait loop · leave/rematch lifecycle over the
  live session.
- **Recovery stack:** desync → **HEAL** (host snapshots the diverged match, streams it through the
  lobby round via `NetLobbyStateChunk` codec v2, every peer reloads the identical file via the
  deferred `ActivityMan::LoadGameToRestart`) · mid-match **REJOIN** (coordinator hands session
  traffic to a sink, the service pumps the dormant session, host `RequestResync`s, codec v7) ·
  **REPLAY** (`.ccreplay`, playback proven sim-identical to the recorded match) · **LAN discovery**
  (host beacon + join-screen browser).
- **Topology limit:** star relay through the host. A host drop ends the match. Host migration is a
  known open item (§G).

---

# §F. WHAT HAS BEEN DONE (the project history)

## Stage 0 — `modernization-effort` (legacy, frozen)

The original working branch, cut from `upstream/development` @ `41e6b7010`. Source-of-truth for
code selectively brought forward. Its rejected experiments are listed in §C. **It is still the
fork's GitHub default branch** — a leftover worth knowing about, since the (now disabled) nightly
CI built from it.

## Stage 1 — cross-platform deterministic sim — **DONE**

Outcome branch **`determinism-cs`**. The replicated SIM is **bit-identical across x86 Windows, x86
Linux, and arm64 macOS** for all 600 ticks of SimBaseline, at nls 1/2/4/8.

How it was actually achieved (the lessons matter more than the list):

- **The lean fixes are what work — a deterministic POLY, never a heavy vendor.** The `detmath`
  vendor ended up inert; the keep-set plus a deterministic transcendental poly was the fix.
- **The trig poly is routed at the PRIMITIVE level** (`DeterministicSinCos` in `RTETools.h`;
  Allegro's `_AL_SINCOS` for sprite rotation; `Vector::GetRadRotatedCopy` for physics) — per-site
  routing was mislocalized twice. The poly equals x86 libm (a no-op there; only the arm64/Apple
  outlier converges) but differs from glibc by 1 ULP at ~1.3% of angles, so combat values shift on
  all platforms to the poly consensus. That's fine — cross-platform identity is the goal.
- **Unsequenced eval-order of RNG-consuming calls is a determinism bug class.** C++ leaves function
  argument evaluation unsequenced; MSVC/GCC evaluate right-to-left, Apple clang left-to-right. So
  `Vector(-RandomNum(x), -RandomNum(y))` consumes the two `g_SimRNG` draws in opposite order
  cross-compiler. This — not any FP divergence — was the SimBaseline `particles@29` closer. **Fix:
  sequence draws into named locals.** The hunt chased physics → trig → threads first: a lesson in
  measure-don't-infer.
- **A per-machine `Settings.ini` timestep is poison** — pin one canonical `dt`.
- **No off-wire carve-outs at the foundation level.** "This part is off-wire so it can diverge" is
  a compromise; a divergence anywhere can cascade. The bar was the full trace bit-identical.
- **Pin residuals by MEASUREMENT, not inspection** — instrument a cross-platform dump, build on each
  machine, byte-diff, localize, fix, re-verify, repeat.

*(Note the tension with §C's "only fix on-wire subsystems": Stage 1 pursued full-trace identity as
the foundation; Stage 2's Phase-2 correction then reverted the purely off-wire AI/controller fixes
once Controller-sync made them unnecessary. Both are correct in their own scope.)*

## Stage 2 — the multiplayer build

| Phase | What | Outcome |
|---|---|---|
| **P1 / P1B** | ControllerFrame replay + synctest (v5, cross-nls flag) | DONE, 3-platform. macOS 88/88 replay legs green |
| **P2 A-D** | Protocol/loopback → identity manifest → loopback session → **GNS transport** | DONE, 3-platform @ `3a06da8` |
| **P3** | Headless playable Controller-sync lockstep | FULL GREEN 3-platform @ `1d5a56c5`. 900-tick ActorStress combat ownership proof, no non-controller mismatch across 901 samples |
| **P4A** | Player-facing Multiplayer UI + localhost/LAN alpha | **THREE-PLATFORM GREEN** @ `1aebbaec9`. Menu/lobby/host/join, rematch, leave, inventory ops, buy orders, generic win condition, synced pause. Remaining: user click-through → M7 sign-off |
| **P4B** | Render interpolation + nonzero input delay + wire measurement | DONE. Matrix 16/16 |
| **P4C** | **N-peer 2→4 + modes** (PvP, co-op PvE, PvPvE) | DONE @ `7d629ffe5`. 3- and 4-peer packs all-pairwise sim-identical; graceful leave, hard-drop adjudication, N-peer lobby UI, co-op control handoffs |
| **P5** | Snapshot / desync-HEAL / REJOIN / REPLAY + measurements | DONE |
| **P6** | LAN discovery beacon + join browser; internet = direct IP | DONE |
| **P7** | Auto input delay from measured ping + `-net-fake-lag` | DONE (superseded by P8) |
| **P8** | LAN-feel: per-sender delay ✅ · honest pace measurement ✅ · true 60 tps ✅ · **P8-4 local-margin rollback (fidelity chase)** · P8-5 the 10-player track | P8-4 in progress |
| **H4** | **Authenticated reconnect + host moderation** | **ACTIVE** — Phase A steps 1-2 done |

### Engine bugs this work flushed out (all fixed — they show the failure classes)

1. `IsHumanTeam` was a per-peer sim mutation → resolved from synced config.
2. Join-tick controller quarantine missing → actors acted on stale per-machine input.
3. AHuman disabled-controller gates missing (plus the crab/turret sibling).
4. Native-AI `ReloadFirearms()` was an off-wire sim mutation across 15 Lua sites.
5. **Async `UpdateDrawMOIDs` raced render-frame MO draws** through MOSRotating's shared static
   scratch bitmaps → per-peer MO-hit layer → marginal contacts flipped and cascaded.
6. **MicroPather infinite loop** — a node cost saturating to FLT_MAX made `OpenQueue::Push`'s
   sorted insert walk past the sentinel and spin forever, wedging a path task at teardown. Was
   behind BOTH the brainspawn and pause timeouts.
7. **`RocketAI.lua` owner-side `self:GibThis()`** — hidden everywhere by a `Vel.Largest > 3`
   short-circuit until a Mac rocket descended at 2.29 and exposed it.
8. **Resync/rejoin DOUBLE-SPAWNED every actor** — sim-consistent, so **every hash gate was blind**;
   caught only by an actor census. `RestartActivity` force-reset the staged activity, so Lua's
   `StartActivity(startNewGame)` always got `true`.
9. A leaver's stale socket killed the survivors (Linux timing exposed it).
10. `Activity::Create` dropped `SceneName` — silently ran every scenario on the wrong scene. Found
    by real CI.

### Review rounds applied (three, all gated green)

The P8-4 seed hunt closed the save/restore fidelity class (un-serialized `DeepCheck`, atom-group
offsets, moment of inertia, sharp-aim state, debris pixels). Then an independent review (16/16
findings) and a **round-2** review (8 per-concern commits) caught that round 1's F1 was
**incomplete** and F2 was a live **regression** — plus enemy-craft scuttle by UID, co-op overspend
to negative funds, unbounded allocation from a runaway peer, a stale-snapshot resync, and crash-record
races. Reports: `stage2_p4\REVIEW_P8_4_SEEDHUNT_REPORT.md`, `REVIEW_ROUND2_REPORT.md`,
`D:\Projects\reviews\`.

### Key measured facts (P8-2 / P8-3 — these overturned earlier estimates)

- **Re-sim = 5.1 ms/tick ≈ 196 tps.** The old "16.8 ms / 60 tps" was an MSPSU-clamp + render
  amortization artifact — **3.3× off**. → **Small local rollback is viable today.**
- Live D=0 tick = 12.4 ms, of which 7.3 ms (59%) is the synchronous frame wait; ~0 at D≥1, and real
  matches always have D≥1 via per-sender auto-pick.
- **Matches already run a true 60.0 tps.** The historical "52.2" and "~33 tps" were teardown-drain
  artifacts inside a presence-gated pace window. Real fix shipped anyway: `c_RealToSimCap`
  1dt → 0.25 s (the old cap could never catch up after an over-budget frame — chronic bleed on
  60 Hz displays). A standing `pace.wall_tps ≥ 55` assert is now in the harness.
- Wire cost ≈ 184 B/tick/peer; slimming deferred as premature.
- Snapshot: 225 ms / 1.31 MB, sim-pure, byte-identical across peers.

---

# §G. WHAT'S NEXT

## Immediate (2026-09-10; the dated NEXT ACTIONS entry at the top of §B is the live order)

1. Commit and back up the positive20 group (`takeover-fixes`), triage the 13 Source41 breadth
   non-pass cases, land the comparator repair, close the `AI_StuckForTime` residual.
2. Windows candidate build + Source42 family on `takeover-fixes`; merge into main; push the checkpoint.
3. The long-red H4 reconnect gates and B1 substitution (normal MP + reconnect UX come first).
4. **Lobby + in-match chat, host moderation polish, UX polish** (`RECOMMENDED.md`), replay UX.
5. **Local prediction polish**: preview-event ledger dedup for sounds/effects, optimistic
   projectiles, Activity UI following the preview; then a human playtest at `-net-fake-lag 200`.
6. **Exact full-world corrections (P8 increment 6 proper)** once an AHuman clone costs well under
   0.2 ms (`STAGE2_P8_PLAN.md`); SimChecksum three-platform re-pin **dead last**.

## Deferred with fix direction (from the review rounds)

4 save-fidelity holes (reload / hatch / shooter-ignore / offhand — need the banked fixture
selftest) · transport-provenance binding (hot-path) · SimChecksum extension (forces a canonical
re-pin) · replay truncation footer · the controller-ownership state machine (crouch/prone under
disabled, quarantine-release vs UI-disable, disabled-reads-as-stateless — a **product call**) ·
`HostCpuRemoteHuman` latent actor-switch ownership bug (fix BEFORE exposing co-op widely).

## Near-term polish (full detail: `RECOMMENDED.md`)

A "Resyncing…" overlay during the 2-4 s heal (reuse `DrawLockstepStallOverlay`) · in-match event
toasts for desync-heal / player-left / rejoined / pause · keep LAN list selection across refreshes ·
server browser v2 with a ping column · co-op resume seating by human slot index · show the
auto-picked input delay in the lobby · compress the state transfer (~5× on the Save.ini) · rebuild
the LAN beacon payload only on change · delta-encode ControllerFrames (184 B → likely <60 B).

## The road to "best in genre" (full detail: `NEXTENHANCEMENTS.md`)

- **Infrastructure:** master server / session directory · NAT traversal (GNS already supports
  ICE/STUN) · **dedicated headless server** (`-net-dedicated` — a host whose human never seats;
  the ownership model already supports it) · **host migration** (all building blocks exist; the
  hard seam is transport re-listen) · server-authoritative variant for public play.
- **Matchmaking & identity:** profiles, quickmatch, parties, region/ping filters.
- **Lobby & social — where "best in genre" is mostly won:** **lobby + in-match chat** (highest
  social value, trivial to build) · activity/scene picker with previews · team builder UI ·
  loadout pre-pick, ready-check, host kick, password · **spectator slots** · Discord rich presence.
- **Feel (the measured path):** sim-speed program (profile the replay MSPSU breakdown, attack Lua
  AI scheduling / path solves / particles / MOID draws; target re-sim ≥5× realtime) · continuous
  adaptive input delay · **render-side local prediction** (cosmetic-only, removes most *felt*
  latency — do this before touching the sim) · **bounded rollback** (capped at D frames; the
  deterministic foundation, frame-addressed inputs, and snapshot serialization are all proven) ·
  audio smoothing under correction (plan it *with* rollback, not after).
- **Robustness:** periodic background snapshots (async, 225 ms — instant heals, fresher rejoin
  baselines, replay seek keyframes) · desync telemetry bundle · replay round-trip selftest.

## Priority call — what actually matters most after H4

*(Assessment 2026-09-05. The netcode is the hard part and it is done; the gap is everything around
it. Treat this as a recommendation, not a binding decision — the user has not ruled on it.)*

**The engine is good and nobody can find it or talk in it.** After H4 closes, the highest
value-per-effort items are not more netcode:

1. **Lobby + in-match chat.** Highest social value on the entire list and one of the smallest jobs
   (a new lobby-codec message + a scrollback box; the relay host rebroadcasts). No multiplayer game
   survives without it.
2. **Master server / session directory.** Today players type an IP. A tiny HTTPS
   register/heartbeat/list service turns that into a browsable list — the difference between "my
   friend and I can play" and "people can play." The client seam already exists (the LAN browser
   becomes one tab of two).
3. **NAT traversal.** Direct IP needs a port-forward today. GNS already supports ICE/STUN, and the
   transport is isolated behind `INetTransport` — config + a rendezvous, not engine surgery.

Only then the feel endgame (render-side prediction → bounded rollback), which is **measured as
viable today** — P8-2 put re-sim at 196 tps, 3.3× better than the estimate that had previously
ruled rollback out. Do render-side local prediction FIRST: it is cosmetic-only, cheap, and removes
most of the *felt* latency without touching the sim.

## Upstream — **HELD** (user decision 2026-09-05)

**NO further PRs to CCCP until #283 merges.** Do not open, re-cut, or propose upstream PRs. This is
a standing instruction, not a pending task.

Context: `upstream/development` has had **zero commits since 2026-05-28** — and that commit was the
merge of our own PR #280. #283 is **APPROVED + MERGEABLE** and has sat unmerged since June (Causeless
commented "Nice one!" on 2026-08-07 and did not merge). Their backlog runs 20+ months, including
one of Causeless's own PRs open since Jan 2024.

| PR | What | State |
|---|---|---|
| #279, #280 | LuaJIT → WohlSoft fork; Windows source build | **MERGED** |
| #282 | Vendored nlohmann/json | Closed by design — rides PR 9, its first consumer |
| **#283** | Cross-platform stability + FP contract | **OPEN, APPROVED, unmerged.** Nothing left for us to do |
| 5, 6, 7, 9, 10 | LuaJIT STRID · Lua math/os determinism · checksum+RNG split · cccp-ctl harness · CI gates | Built + VERIFIED, **held** |
| 8 | Threaded-sim determinism contract | Held; also needs one human vanilla-AI playtest |
| 11 | Vanilla object-key determinism | Built + verified (`a6118dcad`); ships after the foundation |

Per **ADR-008**: fork-first; upstreaming is opportunistic, **never a gate**. The MP work does not
depend on any of it — `stage2/p4b-interp-lockstep` already contains `determinism-cs`.

---

# §H. QUIRKS & GOTCHAS (hard-won — read before touching anything)

## Build & run
- GUI-subsystem exe: **pipe, never redirect.**
- No plain "Release" config. `/t:RTEA` does not rebuild allegro.
- vswhere: use the LITERAL `C:\Program Files (x86)\...` path.
- WSL2 tree ≠ git clone; sync via `git archive` tar. WSL2 builds **contend** with Windows builds.
- SDL3 renamed keycodes: `SDLK_p` → `SDLK_P` (a rename-shim macro fires on the old name).
- MSVC vs GCC include graphs differ — GCC needs explicit includes MSVC gets transitively. Expect
  this class on every "first Linux build in a while".
- Meson CI upstream fails on a pre-existing `GUIRect` issue — not ours.

## Engine / determinism
- **Every capped e2e run reports `Over` after its own teardown** — `winner_team` is the
  discriminator (-1 = no winner/draw; the brain-kill case asserts 0).
- e2e arms fire on `GetSimUpdateCount()`; the match starts at simcount ~2, so trace tick ≈ sim
  count. Commands enqueued pre-match drain at frame 1.
- Delivery queue pops on **sim time**; `DeliveryDelay` 4500 ms ≈ 270 ticks; Dropship MK1 ×1.3 →
  buy arrival ≈ order + 351 ticks. Buy / pause / full-loop cases need 900-tick caps.
- `Timer`/`TimerMan` sim family freezes under the synced pause; the wall-clock family does not.
- **Selftests run engine paths BEFORE manager construction** — never touch manager singletons
  unconditionally in code reachable from selftests (caused a 0xC0000005 with zero output).
- Wire structs use positional aggregate init in tests/arms — **append new fields AFTER existing
  ones.** `NetGameCommandTeam` requires every payload variant to have `.team`.
- Clones don't reliably carry module identity — serialize preset triples from the PRESET pointers
  (`GetClassName()/GetPresetName()/GetModuleName()`), not from clones.
- Per-MO RNG scopes (`DeterministicMORNGScope`) don't touch `g_SimRNG` — a divergence with a clean
  `sim_rng` can still be RNG-adjacent.
- `Players::NoPlayer = -1`; stock display code indexes player arrays **UNGUARDED**.
- ESC in a lockstep match = the LEAVE flow. There is **no in-match pause menu** — **P** is the
  synced pause toggle.
- An older build hard-aborts reading a newer full save (base `ReadProperty` errors on unknown keys).
  MP saves are build-locked by the identity gate anyway; for SP this is an open policy question.
- All `.ini` the game writes now use shortest-round-trip float notation (may emit `1e-07`-style) —
  the engine parses it fine; flag for external mod tooling if serialization goes upstream.

## Interp / render-cadence determinism (check when touching render code)
- **Anything a render-frame path WRITES that sim code later READS is a desync channel.** The five
  caught instances: settle draws (lerped pose), trail bakes (alpha-sized count), `Exit::m_Clear`
  (DrawHUD wrote it, `SuckInMOs` read it — and DrawHUD only runs on the peer VIEWING that HUD), the
  MOID spatial grid (`RegisterDrawing`'s non-layer branch), and Lua AI `GibThis()` (owner-only).
  **Audit pattern:** grep new render-path code for writes to members/managers, and sim code for
  `GetSimUpdateProportion` / `GetRenderPos` / `GetRenderRotMatrix`.
- **NO `UInputMan.Update()` inside sim ticks** — the render frame pumps; sim-rate key edges read via
  `KeyPressedSim` / `ElementPressedSim` (accumulated across frames, cleared once per sim tick by
  `UInputMan::EndSimUpdate()`). Plain `KeyPressed` in sim code double-fires or drops presses
  depending on frame cadence.
- The render section of RunGameLoop must stay wrapped: `t_simRNGOverride = &g_RenderRNG` AND
  `g_SceneMan.SetRenderDrawContext(true)` before `UInputMan.Update/RenderUpdate/Draw`, restored
  after `UploadFrame`.
- **E2E choreography arms are sim-version-sensitive** — the brainkill drop/scuttle constants are
  tuned against exact physics. **Winner-asserts failing with hash-clean traces = retune the arm,
  not a desync.**
- **Dumps at tick T are END-of-tick**: a tick-T fork with a clean T-1 dump means the divergent input
  acted WITHIN tick T (or between dumps — the render frames).
- The localization playbook that worked, in order: sim-gated compare (subsystem + tick) →
  `CC_SIM_DUMP` boundary diff → raw terrain-layer pixel diff → enriched dump fields →
  `[gib-cause]` tags.

## Harness / verification
- The ps1's `$failures` list throws before the trace compare; mismatch / rematch / perturb cases
  exit through their OWN pass paths with distinct success strings — a wrapper grepping only
  `PASS (liveness` reads them as "UNCLEAR".
- **PowerShell switch params CANNOT be passed as strings from an array** — `"-FundsCommandHost"`
  binds positionally to `$OutDir`. Use explicit literal invocations.
- `Compare-Object` on dump lines: an MO whose FIELDS differ appears on **BOTH** sides of the diff —
  don't misread as presence/absence.
- Old archived P1B `.ccflog` replays are invalid post-UID-pin; suites record+replay in-run.
- **Sim-CONSISTENT bugs need state-census gates, not just divergence gates** — the resync
  double-spawn was invisible to every hash gate because both peers did it identically. The actor
  census is now a permanent gate. **Census asserts must BOUND state, not pin outcomes** (the alpha
  dummies genuinely skirmish, so outcomes legitimately vary run-to-run; they never vary cross-peer).
- The interp build renders every frame → two-instance e2e paces near vsync; long cases can outlive
  harness waits. Fix harness pacing before trusting wall-clock-bound verdicts.

---

# §I. PATHS, CONVENTIONS, PEOPLE

## Docs

| Purpose | Path |
|---|---|
| **This resume doc** | `D:\Projects\RESUME.md` |
| Project instructions (identical twins) | `D:\Projects\CLAUDE.md` **==** `D:\Projects\AGENTS.md` |
| **Archived originals** (HANDOFF, HANDOFF2, 15 plans, patches, 4.5k reports) | `D:\Projects\_archive\docs_archive_20260905\` |
| **H4 plan — THE ACTIVE BUILD** | `D:\Projects\STAGE2_H4_RECONNECT_PLAN.md` |
| P8 plan (endgame, paused behind H4) | `D:\Projects\STAGE2_P8_PLAN.md` |
| Near-term polish backlog | `D:\Projects\RECOMMENDED.md` |
| Long-range feature backlog | `D:\Projects\NEXTENHANCEMENTS.md` |
| MP strategy P0-P7 | `D:\Projects\MULTIPLAYER_BEST_PLAN.md` |
| Upstream PR feed (HELD) | `D:\Projects\PRs\PR_ROADMAP.html`, `PRs\PR283_HANDOFF.md` |
| Determinism keep/drop manifest | `D:\Projects\PRs\DETERMINISM_CS_KEEPSET_PACKAGE.md` |
| Stage-1 forensic record | `D:\Projects\xarch_diff\cs\NIGHT_LOG.md` |
| Review reports | `stage2_p4\REVIEW_P8_4_SEEDHUNT_REPORT.md`, `REVIEW_ROUND2_REPORT.md`, `D:\Projects\reviews\` |
| **Testing audit 2026-09-06** (evidence report: what every test does, fresh runs, battlefield captures, findings) | `D:\Projects\reviews\testing-audit-2026-09-06\index.html` (+ `fresh\notes.md`, `launcher\firewall-analysis.md`, `captures\`, `historical\`, `menus\`, `inventory\`) |
| ADRs (through ADR-024) | `D:\Projects\cccp\modernization-docs\decisions.html` |
| Public planning wiki (LIVE) | https://madreag.github.io/cortex-modern/ |

## Worktrees (4 — reconciled 2026-09-10 after the cleanup)

| Path | Branch | Role |
|---|---|---|
| `D:\Projects\p4b-interp-validation` | `stage2/p4b-interp-lockstep` @ `c8f8188ae0` | **The approved tree / milestone branch (main)**; the only executable the two-process harness launches |
| `D:\Projects\takeover-fixes` | `stage2/takeover-next` @ `60cb698146` + uncommitted positive20 group | **ACTIVE worker tree** (recovery/codec18/B2 group; see §B 2026-09-10 entries) |
| `D:\Projects\xarch-combat` | `stage2/p4a-multiplayer-ui-alpha` | P4A / M7 click-through reference |
| `D:\Projects\cccp` | `modernization-effort` | Legacy ref + **the shared object store** |

Removed 2026-09-10 (branches kept, see `reviews/takeover-20260909/cleanup-20260910/REPORT.md`): `audio-flake`, `h4-a6-fix`, `h4-b1-fix`, `h4-phase-a`, `lobby-pacing`, `setaside-residuals`, `transaction-controls`, `h4-fixes-review`, `lobby-pacing-review`, `round-start-review`, `setaside-review`, `h4-a7-review`, `round-start`, `h4-b2`. `testing-audit-engine` was removed in the 2026-09-09 cleanup.

### GitHub is the safety net — how to get anything back

**Remote:** `git@github.com:Madreag/cortex-modern.git` (`origin`). The 2026-09-05 cleanup pushed
the then-existing branch tips. **Current recovery work includes 135 local commits and unfinished
source that are not pushed.** On 2026-09-07, `ls-remote` confirms the active remote branch is still
`1ab7fc2e1`. Preserve the local work and recovery snapshots. Upstream remote is
`cortex-command-community/Cortex-Command-Community-Project`.

- **Deleting a worktree cannot lose commits** — all worktrees share one object store at
  `D:\Projects\cccp\.git`, and every branch also lives on GitHub.
- **To restore any branch as a working folder:**
  `git -C D:\Projects\cccp worktree add D:\Projects\<dir> <branch>`
  (add `-b <new>` to branch from it). If the ref is missing locally:
  `git -C D:\Projects\cccp fetch origin <branch>`.
- **Before deleting ANY folder, check three things** (2026-09-05 lessons, both learned the hard way):
  1. `git status` — uncommitted tracked changes? Save `git diff HEAD` as a patch first.
  2. Is any **env var or build setting** pointing into it? `GNS_ROOT` pointed into a folder deleted
     as "scratch" — it was the GameNetworkingSockets link dependency.
  3. Are non-obvious files mixed in? `stage2_p4` looked like pure test dumps but held 106 reports.
- **Removed 2026-09-05, fully recoverable:** the `determinism-revamp` worktree. It was parked on the
  throwaway measurement branch `xref/keepset-dtlock-terrainprobe` (@ `411c05c09`, pushed), and its
  14 uncommitted files were self-labelled `TEMP cross-arch draw-index probe (never ships)`.
- **Patches from every deleted folder** are in
  `D:\Projects\_archive\docs_archive_20260905\uncommitted_patches\`.

## Branch naming

`flagship/<milestone>` · `feature/<name>` · `experiment/<name>` (throwaway) · `fix/<name>` ·
`pr/<name>` (**cut from `upstream/development`**, not the local stack) · `stage/<name>`
(integration) · `xref/<name>` (cross-platform measurement, throwaway).

**Worktree pattern** for any substantial work — isolated, parallel-workable, individually
deletable. Keep experimental branches local until you actively want CI/bots/others involved.

## Agent topology

- **Windows (this PC)** — drives edits and builds. Primary.
- **WSL2 (same PC)** — the cross-OS Linux leg. **Contends with Windows builds**; stay
  analysis-only while it runs.
- **macOS Mac Mini (arm64)** — the cross-arch reference. Never contends. The user relays prompts
  and reports; comparison artifacts ride `xref/*` branches.

## GitHub / repo state

Public fork, **staying a fork** (decision 2026-09-05 — keeps the PR path one click away, reads as
building-on rather than competing; the only real cost is search invisibility, which doesn't matter
here; detaching is a support ticket available any time). Issues **enabled**, description and 6
topics set. The inherited nightly release workflow is **disabled** (it built all 3 platforms fine;
only its "Publish Release" step 403'd on a fork). Default branch is still `modernization-effort`.

## People

- **Causeless** — upstream lead. Candid, high bar, fast merges when clean (historically). His three
  stated directions are §C's rules 1-3.
- **HeliumAnt** — reviewed and approved #283; adding native macOS-ARM CI upstream.
- **getcetc** (= ooaaaoaoao) — FoW co-author; the only upstream contributor active since July.
- The user relays everything to/from Discord and the Mac agent. **Claude has no direct outside
  access.**

---

# §J. CHANGELOG — 2026-09-05 (the cleanup session)

After an ~8-week gap, session state was reconstructed from the docs plus git — **no prior Claude
transcript survived** (the handoff discipline is what made recovery possible; it took ~15 minutes).

1. **Backed up everything.** 47 branches pushed to origin — 44 existed **only on this disk**,
   including the whole `stage2/p2*` transport/session/identity phase, `verify/dev-283-pr5`, and the
   21 H4/review commits. **Zero local-only work remains.**
2. **Stopped the CI failure emails.** The inherited `nightly.yaml` failed daily — all three platform
   builds SUCCEED; only "Publish Release" 403'd because the default `GITHUB_TOKEN` lacks
   `contents: write` on a fork. Workflow disabled. *(The fork therefore has a working nightly
   Windows + Linux + macOS pipeline, currently switched off — re-enabling it needs only a
   `permissions:` block.)*
3. **Repo settings:** Issues **enabled** (forks default to off), description set, 6 topics added.
   Decision: **stay a fork, keep the name.**
4. **Disk 128 GB → 23.6 GB** (~104 GB freed). Removed 35 dead worktrees, `stage2_p2/gns_spike`,
   `old_achrive`, 566 `stage2_p4` artifact dirs. All reports and `.md` files preserved. Caught two
   near-misses: `old_achrive` held a second fow checkout with **uncommitted** changes (saved as a
   patch), and `stage2_p4` held 106 macOS/review reports mixed into the artifact dirs.
5. **Docs collapsed.** 17 handoff/plan files + 4,581 reports + uncommitted-work patches archived;
   this file written; `CLAUDE.md` trimmed 46.6 KB → ~29 KB with two stale pointers fixed;
   `AGENTS.md` created as its twin.
6. **Broke and rebuilt the GNS prefix.** `gns_spike` was deleted as "scratch build output" without
   checking that `GNS_ROOT`/`GNS_DEP_ROOT` pointed into it. The built exe was unaffected (GNS is
   statically linked — all 9 selftests still passed), but recompilation was blocked until GNS
   **v1.6.0** was rebuilt into the original paths. Then **verified properly**: engine recompiled
   clean (`MSBUILD_EXIT=0`, including `GnsTransport.cpp` and `NetAuthCrypto.cpp`) and **9/9
   selftests pass on the fresh exe.** Env vars persisted in all three required locations.
   **→ The matrix + baseline re-verify is still outstanding. See §B.**

7. **Root cleaned: 253 items -> 23.** 17 stale directories + 212 loose files MOVED (not deleted)
   into `D:\Projects\_archive\`. The doc archive moved under it too. Other projects (`keystone`,
   `finance`, `game-research`, `tempest`, `_corefall-xpoll-research`) untouched. All 18 critical
   paths re-verified afterward + 3 selftests re-run: green.
8. **`determinism-revamp` worktree removed** (2.5 GB) — Stage 1 is done and it sat on a throwaway
   `xref/*` probe branch. Worktrees: 4 -> 3.

9. **`gns_rebuild` deleted (7.4 GB)** — it was the *factory* (vcpkg buildtrees/downloads/packages)
   that produced the GNS libs, not the libs themselves. **Proven unneeded before deleting:** the
   folder was moved aside, the engine was CLEAN-rebuilt (`/t:RTEA:Rebuild`, `MSBUILD_EXIT=0`) and
   all 9 selftests re-run PASS with it absent. The installed prefix in `stage2_p2\gns_spike\`
   stands alone. If GNS ever needs rebuilding, §D re-clones from GitHub.

**Final disk: 128 GB -> 22 GB. Project root: 253 items -> 22. Worktrees: 39 -> 3.**

10. **Rollback fidelity closed (late session).** Built the in-memory faithful snapshot, then found
    the 30-tick hash gate itself was too weak and replaced it with the full per-MO dump gate, which
    exposed six more holes (all listed in §B) including one genuine engine bug (`AtomGroup`
    uninitialised `m_StoredOwnerMass`). 74/74 fuzz probes byte-identical on both recordings;
    baseline + replay + 9/9 selftests green. 11 per-concern commits on `stage2/p4b-interp-lockstep`
    (`3e0edf6b5..f9e3df2b5`), **unpushed — pushing needs the user's OK.**
11. **Verification debt carried:** the 16-case matrix and the GNS-disabled build have not been
    re-run since the GNS rebuild; the baseline e2e + replay have.
12. **Local prediction shipped (late session, 3 more commits, 14 unpushed):** measured the
    in-memory snapshot cost first (actor clones dominate, full-world rollback cannot hide 200 ms
    on this box), then built the local-actor preview through the input-delay pipeline; gated
    sim-transparent at D=3 and 200 ms fake lag, ~1.3 ms per frame. See §B.

## Decisions SETTLED by the user — do not re-propose

- **NO upstream PRs to CCCP until #283 merges.** Standing instruction, not a pending task.
- **NO branch pruning. Keep all 189 remote branches.** Branches cost nothing, and since the
  worktree deletion GitHub is the ONLY copy of many of them — pruning is now strictly riskier than
  keeping.

**Next action is §G item 1: H4 Phase A step 3.** Then lobby/chat/moderation/UX, then the
prediction polish.

# §K. PRESERVED FROM THE FORMER CLAUDE.md MISSION BLOCK (2026-09-09 consolidation)

**`D:\Projects\RESUME.md` is now THE single live resume doc** — it replaced the 450 KB sprawl
(`HANDOFF.md` + `HANDOFF2.md` + 15 phase plans, all archived verbatim at
`D:\Projects\_archive\docs_archive_20260905\`). Start every session there; it carries the current state,
the binding rules, build/verify commands, architecture, roadmap, quirks, and the path index.

**State (reconciled 2026-09-07):** branch `stage2/p4b-interp-lockstep` @ `393fe1e5c`,
worktree `D:\Projects\p4b-interp-validation`, **188 commits UNPUSHED** and **unfinished
Lua graph/native-object checkpoint work** in the working tree.
The original source, binary and pointers are preserved under
`reviews/recovery-2026-09-06/starting-state-212834Z/`. Local per-concern commits remain authorized;
pushing is gated on the user's explicit OK.

**16:58 UTC contract audit: implementation is frozen at combined executable ef82285add68, HEAD d227a1a85, 153 local commits ahead and unpushed. The user requires a systematic investigation and complete failure collection before grouped fixes. The fresh broad Windows suite is 19/20, with raw cross-peer snapshot comparison still failing; both direct full archive round-trips pass on this frozen build. The independent late-update fixture has a valid reference and three capture-60 failures across memory/file/ordinary loading, with seven other cases passing. Probe-specific repairs, engine RNG omission, inconsistent queue/event cohorts, late-load transaction rollback and native coverage are under audit. See reviews/recovery-2026-09-07/contract-audit/CONTRACTS.md and its frozen source/binary manifest. No milestone or full-roadmap completion is claimed.**

**16:36 UTC completion/join checkpoint: 5c023f565 drains the final applied tick before Complete, with retained failing and passing D=0/3 controls; fresh rematch, stall and brain-spawn runs pass. b0531ada7 keeps rejected-join reasons visible while the host accepts replacements; d227a1a85 adds menu regressions. Both menus are visually checked and the replacement completes 180 matching ticks on executable 8525e6187107. Live global recovery at D=3 also passes on the preceding 5473ac06 build. HEAD d227a1a85, 153 ahead, unpushed. Cross-peer full snapshots contain measured local AI, visual RNG and real-clock differences; shared simulation comparison and faithful cross-peer restoration still need a complete gate. Late failed-load rollback, graph/world integration, native breadth and the full MP roadmap remain open. Split commits were not independently built; see RESUME for exact evidence.**

**16:03 UTC orbit/recovery checkpoint: all 13 memory/file/ordinary restore cases at 50/60/100/400 pass through 521, with pending paths, four Lua states and a real global-script orbit event. Live global healing, original 8/8 prediction invariance, separate global 8/8 invariance, native graph controls and normal/late global dispatch all pass on executable ecf08c428c02. Local commits preserve the started guard, userdata walk, attachment offset, controller disable flag, sound-loading completion and recovery at a completed host tick. HEAD 52047631e, 150 ahead, unpushed. Native graph/world integration and the new join queue remain uncommitted. The broader suite remains 16/20: completion, mismatch reporting and cross-peer snapshot equality are next, then late load-failure rollback and the full MP roadmap. See RESUME for exact evidence; split commits were not independently built.**

**15:16 UTC extended-orbit checkpoint: the running/never-started global callback guard is fixed in the working tree and native pause/orbit/end controls pass for normal and late global scripts, including memory/file continuation. A real future craft exposes new failures: file/ordinary restores have stale Lua callback records at capture 50 and a muzzle-flash parent offset reset at 100/400; live healing logs duplicate native IDs and loses the orbit callback. Both invariance suites pass, but this extension is RED. The broader multiplayer run also reproduced the stall completion race (client stops at tick 601 after host Complete with only 599 counted running ticks); a repeat passed. No new build/commit after d42d300f; remaining broad lanes are running. HEAD remains 1249a9c38, 143 ahead, unpushed. See RESUME for retained evidence.**

**14:54 UTC activity-continuation checkpoint: 1249a9c38 resumes full activity checkpoints without repeating startup. SimBaseline and P4 immediate repeat-save controls, all 10 global/mod restores, live two-peer healing, original pickup/fire invariance and separate global-script isolation pass on the combined build. Now 143 ahead, unpushed. Global craft-orbit dispatch, late failed-load rollback, remaining native contracts and the full MP roadmap remain open; graph integration is still uncommitted.**

**14:45 UTC global-script checkpoint: a65409b56 preserves arm offsets through actor construction; ab049bcee carries native global-script instances in saves; 6f88661cf and 0c3d75d7a add global and repeated-save regressions. The combined build passes 10/10 restores, live two-peer healing with one global-script startup, the original 8/8 pickup/fire invariance and a separate 8/8 global-script isolation test, both with Lua-only fault detection. Now 142 ahead, unpushed. Cached activity/global graph integration, remaining callbacks, failed-load rollback and the full MP roadmap remain open.**

**14:14 UTC binding-error checkpoint: 3bf3c0fd5 uses correct Windows exception unwinding for LuaJIT errors across C API calls. The native regression handles 401 invalid calls on each of four actors, including a coroutine and GC. The exact combined build passes 7/7 restores, real two-peer healing, 8/8 prediction invariance with Lua-only fault detection and all 12 gameplay/replay lanes. Now 138 ahead, unpushed. Repeated activity/global callbacks, late restore-failure rollback, native graph integration and the full MP roadmap remain open.**

**13:59 UTC save-callback checkpoint: ea3bea15e fixes pending-object cleanup, including Destroy-created objects and recursive purges; b65732e6b runs save callbacks before scene, image and activity capture. The exact combined build passes the callback/purge controls, 7/7 sustained restores, live two-peer healing, 8/8 prediction invariance with Lua-only fault detection and 10 save/menu I/O controls. Now 137 ahead, unpushed. A newly reproduced Lua binding-error unwind crash is next; repeated activity/global callbacks, broader restoration, native graph integration and the full MP roadmap remain open.**

**11:05 UTC save-menu checkpoint: 7e733269f shares validated archive reads with the parallel save list and keeps damaged metadata from aborting the menu. All 8 catalog controls, 16 load controls and full material/image checks pass on the combined build. Now 135 ahead, unpushed. Save callbacks and remaining Lua/native restore contracts are next; native graph integration and the full roadmap remain open.**

**08:10 UTC:** `861ec16de` fixes menu result/intro transitions; `76ac4ebfd` releases cached sounds before FMOD teardown. Six repeatable Quit crashes became 9/9 passing menu/shutdown cases, plus 4/4 checked-in regressions. Rematch and two fresh stall runs pass. Full match/replay cleanup is now being re-enabled and tested. Native graph integration, broader contracts and normal MP sign-off remain open. See RESUME for exact evidence.

**01:37 UTC:** Actor orders and carried gold now pass memory/file 50/300 through 521 ticks and real two-peer healing (`4ffea6085`); the fixture covers 100 mutable properties. A future-spawn extension exposed VM allocation and script-cache state outside the old probe window. Those corrections are in progress; full Lua/native restoration remains open.

**01:55 UTC:** Future spawns, cached/native callbacks and temporary-capture lifetime now pass 5/5 restoration cases, real two-peer healing and 8/8 prediction invariance, including detection of a Lua-only fault (`df4bf3b4f` fixture; binary `5cce36d24c54…`). Native graph/allocator integration remains uncommitted. Remaining native construction/alias/class coverage and normal MP lifecycle are still open.

**02:26 UTC:** Renamed/bare objects and legacy saved fields pass native checks, 5/5 restores, live healing and 8/8 invariance (`2d2dce1ab` identity; binary `7ee9267af55a…`). Graph integration remains uncommitted. Next: repair ordinary Load Game, which bypasses graph restoration; remaining native coverage and MP lifecycle stay open.

**02:52 UTC:** Ordinary Load Game now uses snapshot restoration (`54683c126`), with a launch regression mode (`b22e7e1bc`). All 7 restore cases through 521 and live two-peer healing through 600 passed on the combined build; individual split commits were not separately built. The new native Area/Box fixture has a valid reference and three refused restores; geometry state, borrowed aliases and lifetime support are now building. Full native contracts and MP lifecycle remain open.

**03:05 UTC:** Native area/box ownership and geometry pass all 7 restores, live two-peer healing and 8/8 invariance with Lua-only fault detection (binary `ce6e08607947…`). Area lifetime and checkpoint support are committed as `361e86117` / `d26613dc6`; fixtures through `083e5d99a`. Inventory 877 / 0 unclassified (`1335c4d03`). Native graph/world integration stays uncommitted; remaining native references, VM RNG, async/global callbacks and normal MP lifecycle remain open.

**03:25 UTC:** VM random-generator restoration now passes 44 sequence checks, 7/7 game restores, live healing and 8/8 invariance with Lua fault detection. `439fdcc8d` adds portable RNG checkpoints; Windows MSVC and Linux libstdc++ exchange all 44 checkpoints with identical bytes and 10,000 matching continuation outputs each. SG3 graph integration remains uncommitted. Remaining native references and async/global callbacks, then normal MP lifecycle, are still open.

**07:09 UTC:** `f9162248c` tracks real queued work through callback completion and keeps pathfinder/scene storage alive until it finishes. Both negative controls are retained; queue/lifetime tests, 7/7 pending/future restores, live healing and 8/8 invariance with Lua fault detection now pass. Native graph/queue integration remains uncommitted. Native/iterator breadth and normal MP lifecycle remain open. See RESUME for exact evidence and the additional running gameplay lanes.

**07:32 UTC correction:** The fresh D=0, D=3 prediction and buy lanes pass, but D=3 delivery repeatedly crashes near tick 379. The first-fault dump identifies an Actor dependency reference retaining a released Lua coroutine state during GC. Repair and re-gate this before resuming native breadth; gameplay remains RED. RESUME records the exact runs, dump and preserved binary/PDB.

**07:46 UTC:** `642a5c31c` fixes that coroutine lifetime failure. The native negative/positive control, delivery plus 11 gameplay lanes, all 7 restores, live healing and 8/8 invariance with Lua fault detection pass. There are now 119 local commits ahead of origin. Native graph/queue integration and tests remain uncommitted; rematch and two fresh stall checks subsequently passed (08:10 checkpoint). Remaining native contracts and normal MP lifecycle stay open. RESUME records exact evidence.

**06:04 UTC:** Alarm values and borrowed positions, module references and iterators, and completed asynchronous path results now pass native checks, all 7 restores through 521, live two-peer healing and 8/8 invariance with Lua-only fault detection. Pending callbacks and remaining native/iterator contracts are next; normal MP lifecycle and the full roadmap remain open. Graph integration remains uncommitted; split fixture commits were not independently built. See RESUME current evidence.

**05:50 UTC:** Custom runtime and bare gib particle targets, aliases and later gib spawning pass all 7 restores, live healing and 8/8 invariance with Lua fault detection. The Windows target now rebuilds luabind; the missing-constructor check passes on the linked library. Inventory 893 / 0. Graph integration, remaining native/iterator/async contracts and normal MP lifecycle remain open. Commits were split from verified combined source, not independently built. See RESUME current evidence.

**05:05 UTC:** Complete gib configuration and borrowed offset aliases pass all 7 restores, live healing and 8/8 invariance with Lua fault detection (`gib-state-2`). Native nullable-particle checks pass; inventory is 892 / 0. Commits `e477c1a06` / `a654e01e6` were split from verified combined source, not independently built. Custom runtime particle targets, native/iterator/callback breadth and normal MP lifecycle remain open; graph integration stays uncommitted.

**NEXT:** `RESUME.md` §B audit corrections, then finish §B-1 and the whole roadmap. The completed
audit `reviews/testing-audit-2026-09-06/index.html` and independent review
`reviews/audit-report-review-2026-09-06/REVIEW.md` establish useful passing matches AND current
failures. Recovery has fixed client-command replay identity, strict trace validation, failed-save
world cleanup and the first real desync-heal path. Fresh runs use private writable runtimes and
retained executable hashes. Expanded restoration checks now pass closures/coroutines, library mutations
and 94 mutable native properties on Lua-owned nonresident actors/firearms through 521 ticks (RESUME §B-1, 00:50 UTC). All-VM preparation fixes the equipment fixture's restore-order failure; live healing and 8/8 prediction invariance pass this combined build. The native API inventory is open: 381 writable properties, 94 exercised by the named fixture. Still open:
remaining Lua/native contracts, cross-peer snapshot fidelity, stalls and menu/quit defects. **Earlier green rows belong to earlier states.** Preserve mods,
closures, shared references, coroutine continuations, Controller-sync and the fixed timestep.
The 01:13 UTC checkpoint also passes global-environment and weapon ownership-transition fixtures,
live healing and full prediction invariance; broader native contracts and normal MP lifecycle remain open.
All unattended tests must keep game windows, focus, sound and firewall prompts off the user's desktop;
use owned processes, private writable runtimes and the already approved executable path.

**04:08 UTC:** Limb paths, retained vectors, hidden owners and cyclic aliases now pass all 7 restore cases, live healing and 8/8 invariance with Lua fault detection (`native-owner-order-1`). Local commits `5df407ec1`, `bd080f517`, `6b768d86d` preserve the state, ownership and fixture. The combined source was built and run; individual split commits were not. SG1/SG2 positive tests pass. Native graph integration and remaining native/iterator/callback contracts are open, followed by normal MP lifecycle and the full roadmap.

**04:23 UTC:** Sound-set copies, selection state, nested references and hidden owners pass all 7 restores, live healing and 8/8 invariance with Lua fault detection (`iterator-owner-1`). The native iterator binding now retains the owner of returned objects. Inventory 881 / 0 unclassified. Retaining the iterator itself, remaining native types and async/global callbacks are still open, followed by normal MP lifecycle and the full roadmap. Combined source was built and run; split commits were not independently built.

**04:45 UTC:** Retained native iterators now pass all 7 restores, live healing and 8/8 invariance with Lua fault detection (`native-iterator-2` / `iterator-bounds-1`). Coverage includes paused loops, aliases, empty/exhausted/unstarted cursors and owned query results after the source is cleared. Iterator API and fixture commits are local; graph integration remains uncommitted. Native type/iterator/callback breadth and normal MP lifecycle remain open. The combined source was built and run; split commits were not independently built.

**Upstream is DORMANT** (re-assessed 2026-09-05): `upstream/development` has had zero commits since
2026-05-28 — our own PR #280's merge. #283 is APPROVED + MERGEABLE and unmerged since June; their
backlog runs 20+ months. Per ADR-008 upstreaming is opportunistic, **never a gate** — the MP work
does not depend on any of it merging.

**2026-09-05 cleanup:** 47 branches pushed (44 were disk-only); nightly-CI failure emails stopped;
Issues enabled + description/topics set on the fork (decision: stay a fork, keep the name); disk
128 GB → 22 GB, worktrees 39 → 3, project root 253 items → 22; GNS prefix accidentally deleted then rebuilt (v1.6.0) —
**recovery procedure is in RESUME.md §2; treat any env-var-referenced path as a dependency.**

**Supporting docs:** strategy `D:\Projects\MULTIPLAYER_BEST_PLAN.md` · active build `D:\Projects\STAGE2_H4_RECONNECT_PLAN.md` · endgame `D:\Projects\STAGE2_P8_PLAN.md` · backlogs `RECOMMENDED.md` + `NEXTENHANCEMENTS.md` · upstream feed `D:\Projects\PRs\PR_ROADMAP.html`. **End goal (binding):** perfect-feel multiplayer at 100-200ms ping (no lag/warping, clean prediction); flawless normal MP + UX (join/leave/rematch/reconnect) comes strictly first; measurements pick the endgame technique, not whether we pursue it.
- **Project history:** the full phase-by-phase record (Stage-1 determinism → P1-P8 → H4) lives in `D:\Projects\RESUME.md` §3-§4; the verbatim originals are in `D:\Projects\_archive\docs_archive_20260905\`.
- **Standing invariants:** only fix what makes an ON-WIRE subsystem deterministic — the AI/controller is OFF-WIRE, per-machine (Causeless #1); do NOT re-apply the reverted off-wire fixes (SpatialPartitionGrid MO-query sort, controller release-timer sim-time, alarm/PathFinder sorts, Lua AI-aim `^` — see `NIGHT_LOG.md` + `PRs\DETERMINISM_CS_KEEPSET_PACKAGE.md`). Every sim-mutating path needs the two-window hash gate (host==client, controller excluded) — headless green alone is NOT "playable" (the pre-recovery P4A overclaim). MP activity logic uses sim time (`IsPastSimMS`), never wall clock. Verification = BUILT and RUN, never reasoned. Partial coverage is a checkpoint, never "achieved".
- **Upstream PR feed — HELD (user decision 2026-09-05):** NO further PRs go to CCCP until #283 merges. #283 is APPROVED + MERGEABLE and has sat unmerged since June; `upstream/development` has had zero commits since 2026-05-28. PRs 5/6/7/9/10 are built and verified but are NOT to be opened, re-cut, or proposed; PR 8 additionally needs a human vanilla-AI playtest. #279/#280 merged; #282 closed by design (nlohmann rides PR 9). Per ADR-008 upstreaming is opportunistic and never a gate — the MP work does not depend on any of it. **Do not re-propose the feed.**
- **Doc rule (2026-07-01):** when a milestone lands or the feed state changes, update the pointer surfaces in the SAME session: this block, the `HANDOFF.md` top pointer, `PR_ROADMAP.html`, and the active phase plan's header. Stale pointers cost a full re-investigation (proven 2026-07-01).
- **Provenance:** all work before 2026-07-01 was authored by the previous coding agent. Verify its claims (line refs, config names, "verified" labels) against the current tree as you touch each area; correct errors in passing.
- **Topology (unchanged):** Windows drives edits/builds; WSL2 (same PC — contends, stay analysis-only while it runs) + macOS Mac Mini (arm64, no contention) build/verify in parallel; the user relays prompts/reports; test branches ride `origin` so agents pull.
