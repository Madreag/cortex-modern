**2026-09-10 22:07 UTC: recovery handoff after the 02:47 UTC usage-limit stop.** Positive20 passes the full Mac A7 group (five arms) plus the native/no-GNS gates; the working tree also holds about 650 lines of unbuilt journal/observer instrumentation (handoff-20260910/post-positive20.patch). Source41 Windows breadth finished 68/81 with 13 unreviewed non-pass cases. Main c8f8188ae0 and exe bb3cf264 unchanged, unpushed. See RESUME.md §B and reviews/takeover-20260909/handoff-20260910/README.md.

**2026-09-10 02:34 UTC: Mac positive19 network/runtime groups pass; strict native gate remains red for craft-fixture duplicate-ID errors.** Positive18 strict co-op recovery passes with 12,002 matching resumed frames and original selection preserved. Source41 breadth remains active on unchanged main/binary. See RESUME.md §B and reviews/takeover-20260909/b2-mac-verification.md for pins, retained controls and open acceptance work.

**2026-09-10 02:18 UTC: grouped complete-input recovery and native restoration review.** Windows Source41 breadth is active on unchanged main/binary. The comparator correction is independently checked; original red evidence and the pending peer roundtrip are retained. Mac positive14/15 pass 10 CLI tests plus controller-frame and retain three native graph failures. The grouped recovery/native fixes in takeover-fixes are not built together or accepted. See RESUME.md §B, b2-mac-verification.md and reviews/takeover-20260909/resync-fixture-verified-0215 for exact evidence; full MP remains open.

**2026-09-10 01:54 UTC: Source41 continuation collected 44 additional jobs; mp_snapshot_p5 remains recorded red because the comparator handles only Activity1 and selects the brain instead of the controlled actor, and peer_roundtrip remains unrun/blocked. A focused isolated comparator repair passes 61 detecting tests and the retained pair; root review is pending. The continuation stopped before breadth; a fresh breadth-only guarded wrapper is being prepared. Main c8f8188ae0 and approved bb3cf264 remain unchanged/unpushed. Isolated Mac positive12 fe2c0bc84cff builds and passes nine of ten network CLI tests plus controller-frame; lockstep crashes on a fixture manager used before construction, and native graph fails on an invalid const-Actor Lua fixture call. Corrections are in progress, not verified. Review also found resync loses accepted controller/sound inputs and remote backlog across repeated heals; bounded complete-input transport, exact-target retention and detecting tests are being implemented together in takeover-fixes. No integration, new verified checkpoint, cleanup or full MP acceptance.**

**2026-09-10 01:25 UTC:** Resumed after user-confirmed reboot. Source41 continuation v2 is collecting the 46 unfinished cases, then 81 breadth cases, on unchanged main/binary. Positive10 slow-save and silent-eight recovery pass; strict co-op selection remains red. Grouped codec18, ownership/command continuity and local UI/alias restoration are unfinished and unbuilt in takeover-fixes. Read RESUME.md §B and the new postreboot A7/matrix reviews for scope, retained failures and exact paths. No integration or milestone sign-off.

# STAGE 2 — H4: AUTHENTICATED RECONNECT + HOST MODERATION (the plan)

**2026-09-10 01:02 UTC - PAUSED FOR USER REBOOT.** User reported persistent desktop sluggishness and is rebooting. All owned Windows family/matrix/breadth wrappers and their verified descendants were stopped; no Mac engine remains running, and all subagents are idle. Source41 main c8f8188ae0 stays unchanged and unpushed. Matrix D:/mx/s41 has 60 NEEDS_REVIEW, 45 NOT_RUN, and the interrupted lobby_3_rejoin_100 still marked RUNNING in its original untouched matrix.json; it must be rerun in a fresh output, not counted as failed or passed. Breadth has not started. Pause/process measurements and all 47 modified/untracked non-vendor source files from takeover-fixes HEAD e48e33f5e1 are byte-verified under reviews/takeover-20260909/pc-sluggishness-20260910; wip-source-manifest.json records hashes. Mac positive10 60570fa497cf passed ten CLI checks; strict co-op now proves wrong local selection (1048615 expected, 1048577 observed). Slow-save attempt3 reached the journal's 16 MiB harness cap and is NOT a gameplay verdict. The repaired a7_support.py raises only journals to 64 MiB with bounded reads, passes 30 controls, and is NOT deployed to Mac yet. Root began codec18 PlayerBindings observation support and a snapshot-construction command guard; this is INCOMPLETE, UNBUILT WIP, with no source export after positive10. The new codec calls ReadVar but the existing helper is ReadVarOrFail: repair before building. Binding production/apply, peer-local restore after full checkpoint commit, canonical ownership envelope, pending reseats/future commands, and relevant regression gates remain to implement. Detailed restart order is pc-sluggishness-20260910/REBOOT_RESUME.md. No merge, commit, push, cleanup or full MP acceptance is claimed. Keep Windows tests paused until the user's PC is responsive.


**2026-09-10 00:43 UTC - co-op ledger and UI completion pass; player rebind remains RED.** Main/Source41 remain unchanged; Windows matrix has 31 completed jobs awaiting review, one running and 74 not run, with breadth queued. The fixed 26-job review found no new engine failure but explicit async/nullable/native coverage gaps (reviews/takeover-20260909/source41-matrix-review/review.md). Isolated Mac positive9 `147969bb776e` passes ten CLI selftests, the old co-op UID oracle (12,002 matching resumed frames), and actual UI inputs with both peers completing simulation tick 602. Co-op still steals the host brain during snapshot startup and copies host-local player/camera/input state onto the client; the exact paths are in wip-resync-ownership-review.md. Slow-save returner survives 7,335 ms and reloads, but the stock duel then ends. Silent8 with the unarmed test fixture exposes missing writable save-module initialization under -module. Positive10 `60570fa497cf` builds and passes ten CLI selftests: it initializes saved-game storage for selected modules, gives lobby session decisions the actual shared clock, and adds local selected-unit observations. Its stricter co-op control is running; player-rebind fixes are not implemented. Driver guards pass 25/25. All 348 prior-run files are hash-verified in a7-verified-mac-group-0044. No main merge or push; full MP/latency acceptance remains open.**


> 2026-09-10 00:15 UTC: positive7 on Mac passes the corrected real P21 leave control and ten CLI tests. Additional A7 runs expose waiting-client timeout during slow save and co-op ledger/hand-back loss; silent8 expiry/reclaim reaches an already-ended duel and save refuses Over. Applied-tick reporting/oracle and host-save keepalives are locally implemented but unbuilt. Full details and limits are in RESUME.md. Source41 Windows remains unchanged and active; no milestone acceptance or push.

> 2026-09-09 23:53 UTC: real Mac host/client SDL tests pass 30 steps per peer after fixing label height and roster font depth; UI evidence is preserved and verified. The isolated journal build passes ten CLI selftests and A7 stagger_seat_survives. The retained leave control is running; remaining socket/GUI/Windows/normal-MP acceptance is open. Main and Source41 Windows validation remain unchanged. See RESUME.md and b2-mac-verification.md for scope and pins.

> 2026-09-09 23:16 UTC: the broken B2 composition is reproduced and retained. Its repairs build on Mac with and without GNS and pass eleven selftests plus the native checkpoint suite in both configurations. They remain uncommitted in takeover-fixes; live GUI and socket acceptance are pending. Source41 Windows matrix and queued breadth remain isolated and active. See RESUME.md and reviews/takeover-20260909/b2-mac-verification.md. No main merge, push or full milestone claim.

> 2026-09-09 22:05 UTC: see RESUME.md for the current Source41 checkpoint. Mac attempt 3 passes all 5+10 gates with retained evidence validated. Windows matrix remains active; the repaired 81-case breadth stage follows it. B2 repairs in takeover-fixes remain incomplete and unbuilt; no full MP milestone or push is claimed.

> Current checkpoint, 2026-09-09 21:33 UTC: Source41 is integrated locally at c8f8188ae0. The full unchanged-source Windows/Mac verification family is running; no new milestone acceptance or push. The six-branch preview and five takeover fixes, retained controls, harness repairs and open coverage are recorded in D:/Projects/reviews/takeover-20260909/source41-integration-review.md. RESUME.md section B is the live status. Round-start, B2 moderation, additional A7 fault arms, normal multiplayer and 100-200 ms feel remain open. Earlier dated paragraphs below are historical.

> 2026-09-08 ~18:10 UTC fourth checkpoint: HEAD 32312c71e, 280 ahead and unpushed. Source38 (Windows f29f69bc) is the final family: Source37 plus the last sound-deferral residual (an AI pass writing through a position it kept defers like every other AI write); the independent reviewer calls the mod-compatibility gate green (fifty unchanged-Lua cases on four executables identical to Source22). On Source38 the eleven selftests, authority match and fault, wounds, rng_particles, ui_bus, ai_defer, the new alias-only two-peer arm and the fault-armed per_machine control are green; the remaining family, the Mac chain, the breadth lane and the final matrix are running. Engineering lanes on the Source38 tree: H4 slice A6 (the resync relaunch ends the activity at once, the fence never engages, the clean leave is unacked), lobby-grace (EResult 25 relay refusals at four peers, the 4-peer lobby start), H4 Phase B slice B1 (substitution), the world set-aside registry audit. Open: the headed section 11 review on the interactive desktop, then the rest of the roadmap. Details: D:\Projects\RESUME.md second checkpoint paragraph.
> 2026-09-08 ~10:35 UTC third checkpoint: HEAD 802938a1a, 279 ahead and unpushed. Source37 (Windows 5ee60605) carries the night's integrated families: the lockstep grace and relay repairs, H4 Phase A slices A1–A5 (admission transaction, fencing, ticket store, ledger and reseat, live admission, reconnect UX, held seats, lobby seat return), the script-owned object checkpoint repairs, the construction-scope registry repair, and the mod-compatibility correction (sound AI-hook operations defer to the committed tick instead of being discarded, SoundContainer.Pos is a live alias again; an independent reviewer closed all but one residual). On Source37 all eleven selftests, the authority, sound-query and ai_defer gates, the native suite, heal, invariance, restoration, the transaction gate and both rollback fuzzes are green on Windows; the Mac builds and passes each family from the exact export. Open: the last F2 residual (Source38), the breadth and H4 gate re-run, the 4-peer 200 ms lobby lanes, the final matrix on the final family, the headed §11 review on the interactive desktop, then the rest of the roadmap. Details: D:\Projects\RESUME.md second checkpoint paragraph.

> 2026-09-08 ~09:35 UTC second checkpoint: HEAD bb9207773, 200 ahead and unpushed; the Source32 tree is committed as eleven per-concern local commits (bb728e006 … bb9207773), only the two rebuilt vendor .lib binaries stay uncommitted. Stitch frames are canonical at capture (62c53748c) and collapsed on restore (Source25); the two-peer heal race is fixed with lockstep rounds, Start retransmission and pre-start buffering (Source26, heal gate 20260908_065147_heal_7d27f107 PASS); logical audio is integrated and the retained Mac gameplay negatives are positive on both platforms; the actual-volume authority transport rides lockstep frames (codec v11) and, after the AI/script-initialization ordering fix (Source31), both peers read identical controlling-player values with a passing fault-injection negative; Source32 (65730f25 Windows, e4cc9501 Mac: all-peer observations, UI-bus sounds logical in simulation, SoundContainer.Pos by value) passes the whole local family on both platforms: nine selftests, native suite, authority match and fault-injection negative, sound-query wounds/rng/ui_bus, heal, invariance, restoration, and the ordinary-load transaction gate with every raw difference classified render scratch; the full collected matrix (104 jobs) is executing on that unchanged build. The Mac native suite reds for Source26–28 were a runner false-red on deliberate negative-control lines (runner amended, Source28 green, Source24/25 kept red). Open: Source32 gates on both platforms, per-concern local commits, the frozen family and the full collected matrix, then normal MP/H4, session UX, discovery and the 100–200 ms feel. Details: D:\Projects\RESUME.md "2026-09-08 second checkpoint". Delegation: one Fable lead, all workers claude-opus-5 at max.

> 2026-09-08 paused checkpoint: HEAD 393fe1e5c, 188 ahead and unpushed. The user requested a resumable stop to conserve tokens. Source21 Windows e28bce73 builds and passes the native checkpoint suite. The orphan-search buffer lifetime fix removes the measured ordinary-load crash: two independent loads complete521 ticks with matching shared traces and marker731 in all five VMs. The full continuation gate remains RED: VMs0-3 are byte-identical, but VM4 differs at a retained job.base value (7 versus10); raw native differences still need classification. The bounded run reuses the Source20 seed and is not final-matrix sign-off. The audio continuation helper and verified NOSOUND guard build, but dedicated runtime and mute-bypass integration remain pending. Incomplete logical sound lifecycle work is preserved outside the active tree as eight byte-verified files with a tested reapplication patch and detailed handoff. Actual-volume authority remains the controlling player, host for activity/unowned objects; transport and callback integration are unfinished. Mac Source20 and prior scoped passes remain in MAC_RESUME.md; sound-query gameplay negatives remain red. Read reviews/recovery-2026-09-07/contract-audit/PAUSED_CHECKPOINT_20260908.md for exact source/binary/backup paths, all lane handoffs and resume order. The full collected matrix, native/VM contracts, normal MP/H4 and100-200ms feel remain open. All three subagents are stopped; no upstream PR or push has occurred.

> 16:58 UTC contract audit: implementation is frozen at combined executable ef82285add68, HEAD d227a1a85, 153 local commits ahead and unpushed. The user requires a systematic investigation and complete failure collection before grouped fixes. The fresh broad Windows suite is 19/20, with raw cross-peer snapshot comparison still failing; both direct full archive round-trips pass on this frozen build. The independent late-update fixture has a valid reference and three capture-60 failures across memory/file/ordinary loading, with seven other cases passing. Probe-specific repairs, engine RNG omission, inconsistent queue/event cohorts, late-load transaction rollback and native coverage are under audit. See reviews/recovery-2026-09-07/contract-audit/CONTRACTS.md and its frozen source/binary manifest. No milestone or full-roadmap completion is claimed.

> 16:36 UTC completion/join checkpoint: 5c023f565 drains the final applied tick before Complete, with retained failing and passing D=0/3 controls; fresh rematch, stall and brain-spawn runs pass. b0531ada7 keeps rejected-join reasons visible while the host accepts replacements; d227a1a85 adds menu regressions. Both menus are visually checked and the replacement completes 180 matching ticks on executable 8525e6187107. Live global recovery at D=3 also passes on the preceding 5473ac06 build. HEAD d227a1a85, 153 ahead, unpushed. Cross-peer full snapshots contain measured local AI, visual RNG and real-clock differences; shared simulation comparison and faithful cross-peer restoration still need a complete gate. Late failed-load rollback, graph/world integration, native breadth and the full MP roadmap remain open. Split commits were not independently built; see RESUME for exact evidence.

> 16:03 UTC orbit/recovery checkpoint: all 13 memory/file/ordinary restore cases at 50/60/100/400 pass through 521, with pending paths, four Lua states and a real global-script orbit event. Live global healing, original 8/8 prediction invariance, separate global 8/8 invariance, native graph controls and normal/late global dispatch all pass on executable ecf08c428c02. Local commits preserve the started guard, userdata walk, attachment offset, controller disable flag, sound-loading completion and recovery at a completed host tick. HEAD 52047631e, 150 ahead, unpushed. Native graph/world integration and the new join queue remain uncommitted. The broader suite remains 16/20: completion, mismatch reporting and cross-peer snapshot equality are next, then late load-failure rollback and the full MP roadmap. See RESUME for exact evidence; split commits were not independently built.

> 15:16 UTC extended-orbit checkpoint: the running/never-started global callback guard is fixed in the working tree and native pause/orbit/end controls pass for normal and late global scripts, including memory/file continuation. A real future craft exposes new failures: file/ordinary restores have stale Lua callback records at capture 50 and a muzzle-flash parent offset reset at 100/400; live healing logs duplicate native IDs and loses the orbit callback. Both invariance suites pass, but this extension is RED. The broader multiplayer run also reproduced the stall completion race (client stops at tick 601 after host Complete with only 599 counted running ticks); a repeat passed. No new build/commit after d42d300f; remaining broad lanes are running. HEAD remains 1249a9c38, 143 ahead, unpushed. See RESUME for retained evidence.

> 14:54 UTC activity-continuation checkpoint: 1249a9c38 resumes full activity checkpoints without repeating startup. SimBaseline and P4 immediate repeat-save controls, all 10 global/mod restores, live two-peer healing, original pickup/fire invariance and separate global-script isolation pass on the combined build. Now 143 ahead, unpushed. Global craft-orbit dispatch, late failed-load rollback, remaining native contracts and the full MP roadmap remain open; graph integration is still uncommitted.

> 14:45 UTC global-script checkpoint: a65409b56 preserves arm offsets through actor construction; ab049bcee carries native global-script instances in saves; 6f88661cf and 0c3d75d7a add global and repeated-save regressions. The combined build passes 10/10 restores, live two-peer healing with one global-script startup, the original 8/8 pickup/fire invariance and a separate 8/8 global-script isolation test, both with Lua-only fault detection. Now 142 ahead, unpushed. Cached activity/global graph integration, remaining callbacks, failed-load rollback and the full MP roadmap remain open.

> 14:14 UTC binding-error checkpoint: 3bf3c0fd5 uses correct Windows exception unwinding for LuaJIT errors across C API calls. The native regression handles 401 invalid calls on each of four actors, including a coroutine and GC. The exact combined build passes 7/7 restores, real two-peer healing, 8/8 prediction invariance with Lua-only fault detection and all 12 gameplay/replay lanes. Now 138 ahead, unpushed. Repeated activity/global callbacks, late restore-failure rollback, native graph integration and the full MP roadmap remain open.

> 13:59 UTC save-callback checkpoint: ea3bea15e fixes pending-object cleanup, including Destroy-created objects and recursive purges; b65732e6b runs save callbacks before scene, image and activity capture. The exact combined build passes the callback/purge controls, 7/7 sustained restores, live two-peer healing, 8/8 prediction invariance with Lua-only fault detection and 10 save/menu I/O controls. Now 137 ahead, unpushed. A newly reproduced Lua binding-error unwind crash is next; repeated activity/global callbacks, broader restoration, native graph integration and the full MP roadmap remain open.

> 11:05 UTC save-menu checkpoint: 7e733269f shares validated archive reads with the parallel save list and keeps damaged metadata from aborting the menu. All 8 catalog controls, 16 load controls and full material/image checks pass on the combined build. Now 135 ahead, unpushed. Save callbacks and remaining Lua/native restore contracts are next; native graph integration and the full roadmap remain open.

> **08:10 UTC:** `861ec16de` fixes menu result/intro transitions; `76ac4ebfd` releases cached sounds before FMOD teardown. Six repeatable Quit crashes became 9/9 passing menu/shutdown cases, plus 4/4 checked-in regressions. Rematch and two fresh stall runs pass. Full match/replay cleanup is now being re-enabled and tested. Native graph integration, broader contracts and normal MP sign-off remain open. See RESUME for exact evidence.

> **Current execution order (2026-09-06): RESUME.md §B audit corrections, then §B-1.**
> Branch `stage2/p4b-interp-lockstep` at `d227a1a85`, 153 local commits ahead of origin, with unfinished
> Lua graph/coroutine work. Replay and failed-save defects are repaired. Expanded memory/file
> restoration checks pass for closures, coroutines, library mutations and Lua-owned nonresident actors;
> 94 mutable native properties pass the latest 5/5 restore suite after fixing all-VM preparation order.
> Live healing and 8/8 invariance pass this build; broader Lua/native contracts remain open (RESUME 00:50 UTC).
> Global-environment and weapon ownership-transition fixtures, live healing and invariance also pass
> the 01:13 UTC checkpoint; full native coverage and normal multiplayer lifecycle remain open.
> Actor orders/gold now pass 5/5 and live healing (01:37 UTC); future spawns exposed VM allocator
> and script-cache omissions. Those fixes are in progress; full restoration remains open.
> The 01:55 UTC future-spawn/callback fixture passes 5/5, live healing and 8/8 invariance with
> Lua-only fault detection. Native graph integration remains uncommitted; broader contracts stay open.
> The 02:26 UTC renamed/bare/legacy-field fixture passes 5/5, live healing and 8/8 invariance.
> Ordinary Load Game bypasses graph restoration and is next; native integration remains uncommitted.
> The 02:52 UTC loader fix and launch fixture are committed locally: 7/7 restore cases and live healing pass the combined build.
> The new Area/Box fixture exposes unsupported geometry; implementation is building, with full native/MP contracts open.
> The 03:05 UTC geometry checkpoint passes 7/7 restores, live healing and 8/8 invariance with Lua fault detection.
> Area lifetime/checkpoint support and fixtures are committed locally; native graph integration and full MP contracts remain open.
> The 03:25 UTC VM RNG checkpoint passes 44 sequences, Windows/Linux exchange, 7/7 restores, live healing and 8/8 invariance.
> RNG foundation `439fdcc8d` is committed; graph integration and broader native/MP contracts remain open.
> 04:08 UTC: full limb state, borrowed vectors and reference cycles pass 7/7 restores, live healing and 8/8 invariance.
> Current combined-source build: `gib-state-2`; gib configuration and borrowed offsets pass all 7 restores, live healing and 8/8 invariance.
> 07:09 UTC: `f9162248c` closes the measured queued-worker lifetime failure. Native queue/lifetime checks, 7/7 pending/future restores, live healing and 8/8 invariance with Lua fault detection pass. Native graph/queue integration is uncommitted; native/iterator breadth and normal MP lifecycle remain open. See RESUME current evidence.
> 07:32 UTC correction: D=0, D=3 prediction and buy pass, but D=3 delivery repeatedly crashes near tick 379. The first-fault dump identifies an Actor dependency reference retaining a released coroutine state during Lua GC. Gameplay is RED; repair and re-gate it before resuming native breadth. Exact evidence is in RESUME.
> 07:46 UTC: `642a5c31c` fixes the coroutine lifetime failure. Native controls, delivery plus 11 gameplay lanes, all 7 restores, live healing and 8/8 invariance with Lua fault detection pass. Native graph/queue integration remains uncommitted; rematch and two fresh stall checks subsequently passed (08:10 checkpoint). Remaining native contracts and normal MP lifecycle stay open. See RESUME for exact evidence.
> 06:04 UTC: Alarm values and borrowed positions, module references and iterators, and completed asynchronous path results now pass native checks, all 7 restores through 521, live two-peer healing and 8/8 invariance with Lua-only fault detection. Pending callbacks and remaining native/iterator contracts are next; normal MP lifecycle and the full roadmap remain open. Graph integration remains uncommitted; split fixture commits were not independently built. See RESUME current evidence.
> 05:50 UTC: Custom runtime and bare gib particle targets, aliases and later gib spawning pass all 7 restores, live healing and 8/8 invariance with Lua fault detection. The Windows target now rebuilds luabind; the missing-constructor check passes on the linked library. Inventory 893 / 0. Graph integration, remaining native/iterator/async contracts and normal MP lifecycle remain open. Commits were split from verified combined source, not independently built. See RESUME current evidence.
> 05:05 UTC: inventory 892 / 0; custom runtime gib targets, native/iterator/callback breadth and normal MP contracts remain open.
> 04:45 UTC: inventory 881 / 0; native type/iterator/callback breadth and normal MP contracts remain open.
> The July order below is historical; this plan's reconnect DESIGN still binds under §B-1 item 6.

> Worktree `D:\Projects\p4b-interp-validation` · **LOCAL-ONLY, push prohibited** until the user's
> explicit go. Historical design-review base **`aa30a2282`**:
> Converged across review rounds; the plan-review must-fixes (admission isolation, transaction
> semantics, leave protocol, restoration ownership, crypto byte schema) are folded in, AND a
> **final read-only verification PASSED 2026-07-10** (brief
> `stage2_p4\REVIEW_H4_VERIFICATION_PROMPT.md`) — all 8 checkpoints confirmed against the code, its
> refinements folded too (§0 both-plane admission isolation, §5 stateless synthetic challenges +
> txId-cache window + canary allowlist, §8 reseat via `SetLockstepControlOverride`). **The design
> is SETTLED — do NOT re-design/re-verify; BUILD it.** **Phase A STARTED 2026-07-10.** Implement +
> gate the two phases SEPARATELY; the design is whole so Phase A leaves no throwaway state.
>
> **Execution guidance (user-relayed 2026-07-10, BINDING):** crypto may land first, but NO new
> live admission traffic is enabled until §0 isolation is implemented and adversarially green in
> BOTH planes (`NetLockstep` AND `NetSession`); preserve the fatal-vs-nonfatal split (the local
> transport's own receive failure stays fatal — never blanket-ignore
> `ConnectionFailed`/`TransportError`); the §8 LEDGER is the Phase-A target (fallback only after
> STOPPING and reporting infeasibility); before the §4/§7 transaction/lifecycle build, pin +
> record ALL remaining timeout/cap/cache-retention/retry/confirmed-session-end values in this doc
> (the pin-first rule); do not write the three later plan docs yet; report T=300 honestly as the
> accepted known FAIL@313; pushing remains a separate explicit user authorization.
>
> **Binding-order note:** NOT a detour. Per HANDOFF2 §0, *normal* MP — "getting into and out of
> games, leaving, rematching, rejoining/reconnecting" — must be flawless BEFORE the advanced
> netcode. Order: **H4 Phase A → Phase B → fidelity fixture → save-holes 1-4 → close T=300
> tick-313 → increment 6 → SimChecksum re-pin DEAD LAST.**

## Pinned values (2026-09-08)

> Lead confirmation (2026-09-08 ~10:00 UTC): the eight LEAD-CONFIRM pins are confirmed as pinned — P2 provisional-seat expiry 20 000 ms, P6 H4 messages as new types under the unchanged `NetProtocol::c_Version = 1` (feeds the §10 probe, not a §10 decision), P11 challenge lifetime 10 000 ms, P14 eight concurrent unauthenticated connections (load-bearing; the unbounded `PeerState` growth in `NetSession::ProcessEvent` reported by the pinning lane is a named Phase A fix with its own selftest), P16 one attempt per connection per second with a scheduled +1 000 ms denial release, P17 txId cache 60 000 ms window with 64 entries as the floor, P23 a stale-epoch `Reclaim` takes the uniform-denial path with the same scheduled delay, P25 one ticket record with a 24 h outer bound. Pin derivations and code anchors: `reviews/claude-review-2026-09-08/lanes/h4-plan-pins/report.md`.
> Lead confirmation (2026-09-08 ~20:20 UTC): the four Phase-B LEAD-CONFIRM pins are confirmed as pinned - P27 a host-drawn 128-bit CSPRNG substitution txId whose cache key binds the stable seat and holder generation but not the identity block (possession already proves receipt of the offer; the ack is bound to the approved connection), P30 the superseded credential retained verify-only for P2 in a slot only `VerifyRetiredProof` reads, a verifying proof refused with `SeatReassigned = 17` on P16's uniform release schedule and a non-verifying one taking the ordinary uniform denial (the precise wording is gated on possession of the 256-bit credential, so no enumeration oracle opens), P31 an `Applicant` carrying the identity block, display name and stable seat with no epoch (nothing is granted or proven by it, and P15's bounds plus the uniform denial cap its abuse), P32 an ack only for a seat reassignable right now with every other seat taking the identical uniform denial (the vacancy is already public through the section 11 roster indication; the ack carries no credential and no generation). P28 and P29 were pinned by derivation and stand. Derivations and code anchors: `reviews/claude-review-2026-09-08/lanes/h4-b1-substitution/report.md`.

The §4/§5/§7 protocol values the plan left as ranges, placeholders or open questions, pinned here
per the 2026-07-10 pin-first rule (RESUME §B-1 item 6a). Design is unchanged — only the open
numbers and rules are fixed. Line references are against HEAD `bb9207773`; every cited file was
verified byte-identical to it in `D:\Projects\p4b-interp-validation\`. The §0/§8 references
inside the design sections below are historical, against the design-review base `aa30a2282`,
and were NOT rewritten.
Time base: one sim tick = 16.667 ms (`c_DefaultDeltaTimeS = 0.0166666F`, `System/Constants.h:32`).
Peer cap: 2-4 humans (`NetMatchConfigUtil::c_MinPeerCount`/`c_MaxPeerCount`, `Network/NetMatchConfig.h:54-55`).
Latency target: 100-200 ms RTT. **LEAD-CONFIRM** marks a judgment call for the lead to confirm.

| # | Value | Pin | Derivation |
|---|---|---|---|
| P1 | txId width + owner + cache key (§4) | 16 B, CSPRNG-drawn by the transaction's INITIATOR (client for NewJoin/Reclaim/Applicant/LeaveRequest, host for a Phase-B approval). Cache key = txId; a hit replays the cached terminal result ONLY when the re-presented message type, stable seat, holder generation and identity hashes are byte-identical, else it is a fresh (denied) transaction. | Matches the epoch width already fixed in §3/§5 (`NetSeatAuth.h:9`); `NetAuthCrypto::RandomBytes` (`NetAuthCrypto.h:19`) already supplies it. Client-drawn txIds are attacker-chosen, so the re-presentation check is what makes "an ACK loss must never turn a success into a failure" safe. |
| P2 | Provisional-seat expiry = resume window (§4) | **20 000 ms** | Must outlive one TicketOffer -> durable persist -> StoredAck round trip plus a disk flush and one full P3 retry ladder, and expire well inside the host's patience. Equals the in-match `missingFrameGraceMs = 20000` (`NetMatchRunner.h:39`) - the same "this peer is genuinely gone" horizon; 4x the 5 000 ms session timeout (`NetMatchService.cpp:1236`) and far below the 600 000 ms menu-lobby wait (`NetMatchService.cpp:47`). **LEAD-CONFIRM** |
| P3 | Admission retransmit interval + attempts (§4) | **250 ms, 8 attempts (2 000 ms budget)** per handshake step, then fail closed | 250 ms is the codebase's existing retransmit cadence on both the lockstep start path (`c_StartRetransmitMs`, `NetLockstep.cpp:19`) and the lobby (`resendIntervalMs`, `NetLobbySession.h:34`), and it exceeds one 200 ms RTT so a retransmit never races an in-flight reply at the top of the target band. 2 000 ms sits inside the 5 000 ms session heartbeat timeout (`NetMatchService.cpp:1236`), so the handshake gives up before - not because of - the transport. All admission traffic rides `ControlReliable` (`NetTransport.h:14`), so these cover a peer restart/loss, not ordinary packet loss. |
| P4 | Peer id on a committed reclaim (§4) | The stable seat's recorded `NetMatchPlayerSlot::peerId` from the live `NetMatchConfig` (`NetMatchConfig.h:24-31`), asserted unoccupied by the P20 fence. `NetSession::AllocatePeerId` is never called on this path. | Ownership resolution keys on the lockstep peerId (`NetLockstepCoordinator::ResolveActorOwner`, `NetLockstep.cpp:1651`), as do `peerInputDelayFrames` and `remoteTransportPeerIds` (`NetLockstep.h:158,165`); a different peerId would silently re-point every actor's owner. `AllocatePeerId` returns the first FREE candidate (`NetSession.cpp:599-608`), which after a drop is the wrong answer whenever another seat freed earlier. |
| P5 | Identity re-validation set + ordering (§4) | The four hashes the session already gates on - `deterministicConfigHash`, `moduleManifestHash`, `sessionRulesHash`, `sessionIdentityHash` (`NetProtocol.h:80-83`) - plus `controllerFrameVersion`, `controllerFrameEncodedSize`, `gameVersion`, `buildId`. Validated FIRST, before the seat lookup and before any challenge is issued; a mismatch is denied with its specific existing `NetRejectReason` (`NetProtocol.h:26-42`). | Reuses the existing mismatch path (`NetSession.cpp:695-731`) with no new comparison surface. Validating before the seat lookup keeps the specific rejection free of any seat-existence signal, so §5's no-enumeration rule is untouched while §11 still gets a clear error. The hello carries hashes only, not manifests, so the message stays small (P18). |
| P6 | H4 wire version bytes (§4/§5) | H4 messages are added as NEW `NetMessageType` values under an UNCHANGED `NetProtocol::c_Version = 1` header (`NetProtocol.h:209`); each H4 payload carries its own `h4Version` u16 LE = 1 as its first field. | `NetProtocol::Decode` rejects a header-version mismatch at offset 4 before reading any payload (`NetProtocol.cpp:620-622`) and `Encode` always writes `c_Version` (`NetProtocol.cpp:576`), so a header bump makes the host's own `JoinRejected` undecodable by the old client - the exact failure §10 warns about. Holding the header at 1 leaves an old client failing at `UnknownMessageType` (`NetProtocol.h:52`) with the envelope intact and the host's rejection still decodable. Evidence FOR the §10 probe, not a §10 decision. **LEAD-CONFIRM** |
| P7 | Transcript domain/message-type tag (§5) | **16 B zero-padded ASCII**, not length-prefixed, one distinct value per proof type: `CCCP.H4.RECLAIM\0` (reclaim), `CCCP.H4.SUBST\0\0\0` (Phase-B substitution), `CCCP.H4.TICKET\0\0` (durable-record MAC). | Fixed-width 16 B matches the epoch/nonce widths already pinned in §5 and needs no length prefix under the "all length-prefixed / fixed-width" rule. Distinct tags give cross-protocol separation: a reclaim proof can never be replayed as a substitution proof. The `CC` prefix matches the existing magic family ("CCL3"/"CCN2"/"CCL4", `NetLockstep.h:214`, `NetProtocol.h:208`, `NetLobbyProtocol.h:145`). |
| P8 | Transcript "protocol version (u16 LE)" (§5) | `NetProtocol::c_Version` (currently **1**, `NetProtocol.h:209`) - NOT the lockstep codec version. | The transcript binds the proof to the ADMISSION wire that carries it, and the admission handshake decodes through `NetProtocol` (`NetSession::ProcessPacket`, `NetSession.cpp:321`). Binding `NetLockstepCodec::c_Version` (11 and rising with sim features, `NetLockstep.h:215`) would invalidate every outstanding ticket on an unrelated sim-wire bump - exactly the spurious invalidation the one-credential-per-holder-generation rule exists to avoid. |
| P9 | Stable seat id (§5/§3) | **u16 LE**, value = the seat's 0-based slot index in `NetMatchConfig::players`; NOT `NetMatchPlayerSlot::peerId`. | §3 requires a stable id, never a transient peer id, and `peerId` IS reassigned by `NetSession::AllocatePeerId` (`NetSession.cpp:599-608`) whenever a slot frees. The slot index is stable for the hosted session and bounded by `NetMatchConfigUtil::c_MaxPlayers = 5` (`NetMatchConfig.h:57`). `NetSeatAuthRegistry` already keys on `uint16_t seat` (`NetSeatAuth.h:32`), so no type change. |
| P10 | Holder generation (§5/§3) | **u32 LE**, first issue = 1, monotonic, never rewound; 0 reserved for "no holder". | Records the shipped behaviour: `IssueCredential` pre-increments from a zero-initialised `lastGeneration` (`NetSeatAuth.cpp:33-37`), `MatchesActiveCredential` rejects generation 0 (`NetSeatAuth.cpp:43-45`), and `RevokeSeat` clears `active` without touching `lastGeneration` (`NetSeatAuth.cpp:61-67`). |
| P11 | Challenge lifetime (§5) | **10 000 ms** from issue; consumed atomically on the first proof attempt. | Must cover one Challenge -> Proof round trip at the top of the band (200 ms) plus the client HMAC and a full P3 retry ladder (2 000 ms), with headroom for a stalled frame. 2x the 5 000 ms session timeout (`NetMatchService.cpp:1236`), so a link healthy enough to remain in the session is never rejected for challenge expiry, while an abandoned slot frees inside one session-timeout period. Ordering is binding: P11 < P2 < P17. **LEAD-CONFIRM** |
| P12 | Outstanding STATEFUL challenges (§5) | **2 per connection, 8 global** | A legitimate reclaimer needs one live challenge; 2 allows one in-flight retry overlap. 8 = 2 x `NetMatchConfigUtil::c_MaxPeerCount` (`NetMatchConfig.h:55`), so all four seats can reclaim simultaneously with a retry each. Unknown-seat SYNTHETIC challenges are stateless by §5 and draw no slot, so this cap is unreachable by unknown-seat spam (the verification finding this cap exists to honour). |
| P13 | Provisional seats (§5) | **4 concurrent**, at most **1 per stable seat** | A provisional seat is a seat, and the config admits at most 4 human peers (`NetMatchConfig.h:55`); `AllocatePeerId` already refuses beyond `maxPeers` (`NetSession.cpp:599-608`) and `RejectPeer(SessionFull)` fires at `NetSession.cpp:381`. One per stable seat because a second provisional claim on one seat IS the two-simultaneous-holders case §6 fences. |
| P14 | Unauthenticated connections (§5) | **8 concurrent** half-open (pre-validated-`ClientHello`) transport peers, oldest disconnected first; a connection with no decodable `ClientHello` within **5 000 ms** is dropped. | Closes a live hole: `ProcessEvent` pushes a `PeerState` for every `PeerConnected` with NO cap (`NetSession.cpp:263-273`), and the `SessionFull` check only fires later when a `ClientHello` is handled (`NetSession.cpp:381`), so `m_Peers` grows without bound today. 8 = 2x the 4-peer cap, leaving every legitimate joiner a slot while one reconnects. The 5 000 ms deadline is the existing session `timeoutMs` (`NetMatchService.cpp:1236`) that `NetSession::ExpireSilentHandshakes` applies to `Handshake` peers (`NetSession.cpp:755-768`, called from `TickAdmissionPlane:171` and `CheckTimeouts:731`, on every evaluation including a resumed one, so a resumption never extends P14) - the new thing here is the CAP, not the timeout. **LEAD-CONFIRM** |
| P15 | Pending applicants, Phase B (§4/§5) | **4 global, 2 per stable seat, 1 per connection**; records expire at P2 (20 000 ms). | §9b gates "two pending applicants for one seat", so 2/seat is the plan's own floor; 4 global = `c_MaxPeerCount` (`NetMatchConfig.h:55`); 1/connection stops one socket filling the queue. Expiry reuses P2 so the lifecycle has one lifetime constant, not two. |
| P16 | Uniform rate-limited failure (§5) | **1 reclaim/applicant attempt per connection per 1 000 ms**, 4 per hosted session per 1 000 ms; every denial (unknown seat, wrong epoch, bad proof, expired challenge) is RELEASED at issueTime + **1 000 ms**, scheduled - never a blocking sleep on the host thread; at most 1 pending denial per connection. | The fixed release time is what makes known-seat and synthetic-challenge flows indistinguishable by timing, which is the whole point of the synthetic challenge. 1 000 ms is a cadence this codebase already treats as slow-background (`peerStateIntervalMs`, `NetLobbySession.h:35`; `c_BeaconIntervalMs`, `NetLanDiscovery.h:29`). At 1/s across the 8 connections of P14 an attacker gets <=8 guesses/s against a 256-bit credential (`NetSeatCredential`, `NetSeatAuth.h:10`) - brute force was never the threat; enumeration was, and the fixed delay removes the oracle. **LEAD-CONFIRM** |
| P17 | txId terminal-result cache (§5) | **64 entries per hosted session**, retention **60 000 ms**, eviction = expire-by-age first, then oldest-insertion-first when full. | Retention must exceed both windows it is pinned against: 60 000 ms = 3x P2 and 6x P11, so an entry outlives every transaction that could still be legitimately retransmitted, which makes the plan's "benign spurious denial after eviction" provably intentional rather than accidental. 64 = 16x the 4-peer cap (`NetMatchConfig.h:55`), covering each seat's join + reclaim + leave + substitution across a session's rematches; at ~64 B/entry that is ~4 KiB, negligible against the 64 KiB control payload cap (`NetProtocol.h:211`). **LEAD-CONFIRM** |
| P18 | H4 admission message size (§5) | **<= 1 024 B payload** (<= 1 048 B on the wire with the 24 B header); the host rejects a larger H4 message before parsing it. | The largest H4 message is `Reclaim`, which re-validates identity (P5): 4 x `NetHash32` = 128 B (`NetProtocol.h:11,80-83`) + `displayName` <= 64 B (`NetProtocol.h:212`) + `gameVersion`/`buildId` <= 128 B each (`c_MaxShortTextBytes`, `NetProtocol.h:213`) + 54 B of txId/epoch/seat/generation/nonce ~= 502 B worst case. 1 024 B doubles that for the Phase-B fields while staying 64x below `c_MaxControlPayloadBytes` (`NetProtocol.h:211`), so an unauthenticated joiner cannot make the host allocate 64 KiB per admission packet, and 512x below the 512 KiB GNS reliable-message ceiling (`steamnetworkingtypes.h:846`), so nothing fragments. |
| P19 | Secret-canary allowlist (§5) | Exactly **one** entry: the resolved ticket-store path (default `<Userdata>/reconnect.ticket`, `System::GetUserdataDirectory`, `System.h:59` / `System.cpp:57`), as an exact resolved path - not a glob, not a directory. The scan FAILS if that path is absent from the artifact set while a ticket was issued. | The plan requires the carve-out to keep its teeth. Making it one exact path plus a required-presence assertion turns the exclusion into a positive check, so "naively excluded" cannot silently disarm the gate. |
| P20 | Incarnation number (§6, used by §4/§7 fencing) | **u32 LE per (stable seat, holder generation)**, 1 at the seat's first commit, +1 on every successful proof that binds a new transport; the seat stores only the CURRENT value. Any packet or timeout attributed to a lower incarnation is discarded. Saturates at `UINT32_MAX` and refuses further reclaims on that generation (fail closed). | The fence needs a total order neither connection can both claim; a host-incremented per-seat counter is the only value both peers agree on before the superseded transport is torn down. `SenderOwnsTransport` already discriminates by transport id (`NetLockstep.cpp:1978-1987`), so re-pointing the seat's `m_RemoteTransports` entry IS the lockstep-plane fence; the incarnation number is what makes the superseded transport's later `PeerDisconnected` identifiable as stale instead of a seat loss. u32 shares the generation width; 2^32 reconnects per generation is unreachable. |
| P21 | LeaveAck retry + ambiguity rule (§7) | Client sends `LeaveRequest` reliably, retries per P3 (250 ms x 8); with no `LeaveAck` inside 2 000 ms it KEEPS the ticket and closes the link (an unacknowledged leave is an ambiguous loss). Host revokes on `LeaveRequest` and caches the `LeaveAck` under the txId (P1/P17), so a retransmitted request replays the ack. | §7 binds "only an acknowledged LeaveAck clears the client ticket" and "never clear on an ambiguous network loss"; the retry ladder reuses P3 so there is one constant. `NetSeatAuthRegistry::RevokeSeat` is idempotent (`NetSeatAuth.cpp:61-67`), so a replayed request is safe. |
| P22 | Confirmed hosted-session end (§7) | EITHER (a) a `NetDisconnect` carrying the new `NetRejectReason::SessionEnded = 16` (`NetProtocol.h:26-42`), sent by the host from the same place the registry is cleared (`NetMatchService.cpp:356` `Destroy`, `:391` `ReportRuntimeError`), OR (b) a clean `LeaveAck` (P21). NOTHING else - not a transport `PeerDisconnected`, not a session timeout, not a match `Complete`. | The host's `m_SeatAuth.EndSession()` is the exact point after which no credential can verify (`NetSeatAuth.cpp:18-22`), so the client's record is provably dead only there. A rematch deliberately keeps epoch and tickets alive (`NetSeatAuth.h:24`), which is why `Complete` must not qualify. `NetDisconnect` already exists with a `uint16_t disconnectReason` (`NetProtocol.h:162-163`) and is already sent on the disconnect path (`NetSession.cpp:514`), so this needs a new reason value, not a new message. |
| P23 | Stale-epoch handling (§7) | A `Reclaim` whose 16 B epoch is not the armed epoch takes the SAME synthetic-challenge + uniform rate-limited denial path as an unknown seat (P16), so the two cannot be told apart by timing. Any OTHER message tagged with a non-current epoch is dropped and counted, never answered ("ignore stale epochs"). Comparison is exact 16 B, constant time (`NetAuthConstantTimeEquals`, `NetAuthCrypto.h:33`). | Resolves the seam between §5's one-denial-path rule and §7's "ignore stale epochs": the ignore rule is about a previous session's in-flight traffic, the uniform denial about an admission attempt. Splitting them by message class honours both without a second denial branch to time. **LEAD-CONFIRM** |
| P24 | Rematch-lobby seat protection (§7) | Lasts the WHOLE hosted session - until `RevokeSeat` (clean leave / substitution) or `EndSession`. **No timer.** | `NetSeatAuthRegistry` entries persist until revoked or the session ends (`NetSeatAuth.cpp:18-22,61-67`) and the epoch explicitly survives resync/rejoin/rematch (`NetSeatAuth.h:24`). A shorter protection timer would open a window in which a claimed seat silently becomes ticketless-joinable - precisely the H4 threat - so "no timer" is both the safe pin and the one needing no new state. |
| P25 | Client ticket record (§7/§11) | Contents: `recordVersion` u16, epoch 16 B, stableSeat u16, holderGeneration u32, credential 32 B, hostSessionId u64, host address <= 128 B (`c_MaxShortTextBytes`), `issuedAtUnixMs` u64, matchConfigHash 32 B (~250 B). Store holds exactly **ONE** record - a TicketOffer for a different session atomically replaces it. Path: injectable, default `<Userdata>/reconnect.ticket` (`System.cpp:57`). Deleted ONLY on LeaveAck, on P22, or at age > **24 h**. No time-based rotation: the credential rotates exactly when the holder generation increments (`IssueCredential`, `NetSeatAuth.cpp:24-40`). | §7 forbids clearing on ambiguous loss and permits deletion only at a confirmed session end, so the age bound exists purely to stop an abandoned record being offered forever at startup (§11's stale-ticket error). 24 h outlasts any plausible single session and keeps a next-day launch from prompting about a dead match. The record is worthless once the host's epoch is cleared, so the bound is UX hygiene, not a security control. One record matches §11's singular "the recovery record". **LEAD-CONFIRM** |
| P26 | Drop-time ownership ledger (§8, used by §7 lifecycle) | One entry per stable seat holding the `m_PersistedUniqueID` list captured at the drop frame (`Entities/MovableObject.cpp:402,474`), capped at **512 actor UIDs per seat**, replaced wholesale on each new drop of that seat, retained for the whole hosted session and cleared on `RevokeSeat`/`EndSession` (same lifetime as P24). | Retention must at least match the ticket that can redeem it, so it follows the registry lifetime rather than inventing a second clock. 512 UIDs is 4 KiB at 8 B each - far above any plausible per-seat actor count in a 2-4 peer skirmish, and a bound against a degenerate or hostile scene. It MUST be a separate structure: `s_LockstepControlOverrides` is cleared at coordinator handoff (`System/ScenarioRunner.cpp:578`) and purged for gone peers (`:664-674`), which is §8's finding, still true in the current tree. |
| P27 | Substitution txId owner + cache key (§4/§9b, Phase B) | 16 B, CSPRNG-drawn by the **HOST** at approval (P1's "host for a Phase-B approval"). Cache key = txId; a hit replays the cached terminal result when the re-presented message type, stable seat and holder generation match. Unlike a reclaim's key it does NOT bind the identity block. | P1's identity binding exists because a client-drawn txId is attacker-chosen. A host-drawn 128-bit CSPRNG id is not: holding it already proves the offer was received, so the identity adds no reachable check. The key still binds seat and generation, so a cached result can never be replayed onto another seat, and the ack is additionally bound to the approved connection. **LEAD-CONFIRM** |
| P28 | Seat generation - the substitution CAS value (§9b) | **u32 LE per stable seat**, 1 at the seat table's creation, +1 on every event that changes who may hold the seat: a commit (`BindIncarnation`), a mid-match drop, a clean-leave close, a lobby release, and the hosted session's end. An approval records the value; the commit requires it unchanged. Saturates at `UINT32_MAX` (no wrap). | §9b's "seat-generation CAS" needs a value that moves on exactly the events that invalidate an approval, and on nothing else - a counter over "who may hold this seat" is that. It is deliberately NOT the holder generation (which a substitution itself moves) and NOT the incarnation (which is per holder generation, P20). |
| P29 | Substitution approval lifetime + ladder (§4/§9b) | The offer retransmits at **P3** (250 ms x 8) and the approval expires at **P2** (20 000 ms), after which the record is invalidated and removed and the substitute is told. The credential is DRAWN at approval and INSTALLED only at the commit. | Reuses the two constants the first-join transaction already uses rather than inventing a third clock, so the whole admission plane has one retransmit cadence and one provisional horizon. Drawing without installing is what makes "the first COMMIT wins" true rather than "the first approval wins". |
| P30 | Superseded-credential retention (§5/§9b) | The credential a committed substitution supersedes is retained **verify-only** for **P2 (20 000 ms)**, in a separate registry slot that only `VerifyRetiredProof` reads. A `Reclaim` naming that generation gets a real (stateful) challenge; a proof that VERIFIES is refused with the new `NetRejectReason::SeatReassigned = 17`, released on P16's uniform +1 000 ms schedule; a proof that does not verify takes the ordinary uniform denial. | §9b requires the loser of the race to get a precise rejection, and §5 forbids an enumeration oracle. Gating the precise wording on possession of the 256-bit credential satisfies both: nobody who lacks it can tell the seat apart from one that never existed, and the wording rides the unchanged release schedule. The retained credential can never claim - its one reader's only outcome is a refusal. P2 is used because that is already how long the plane holds a dropped seat's other state. **LEAD-CONFIRM** |
| P31 | Applicant message contents (§4, Phase B) | An `Applicant` carries the P5 identity block, a display name and the stable seat, and **no epoch**. Every other H4 message keeps its epoch. | A brand-new player has never been issued an epoch, and a ticketless original's may be stale, so requiring one would make the message unsendable by exactly the people it exists for. Nothing is granted or proven by an Applicant, so there is no cross-session replay to bind against; P23's "ignore stale epochs" still governs every message that does carry one. **LEAD-CONFIRM** |
| P32 | What an applicant may learn (§5/§9b) | An `Applicant` for a seat that is reassignable RIGHT NOW (a live match, a real human seat, whose holder has dropped or cleanly left) is acknowledged; every other seat - live, CPU, the host's own, out of range, or a lobby seat - takes the identical uniform rate-limited denial. A held seat and an abandoned one both accept applicants. | Applying is a public act on a public server, and the fact a seat is vacant is already published to the session by §11's persistent roster indication. What stays secret is unchanged: an ack carries no credential and no holder generation, and says nothing about whether the seat's ticket holder can still return - which the returner-versus-substitute race requires it not to. **LEAD-CONFIRM** |
| P33 | Phase-B wire additions (§4/§10) | Four new `NetMessageType` values under the unchanged `NetProtocol::c_Version = 1` (P6): `Applicant = 20`, `ApplicantAck = 21`, `SubstitutionOffer = 22`, `SubstitutionAck = 23`, each carrying `h4Version = 1`. Pinned payload sizes: ApplicantAck 24 B, SubstitutionOffer 116 B, SubstitutionAck 76 B; worst-case Applicant 478 B, still under the P18 1 024 B cap, which the worst-case Reclaim (498 B) continues to set. Plus `NetRejectReason::SeatReassigned = 17`. | Follows P6 exactly: new types under an unchanged envelope, so an old client still fails at `UnknownMessageType` with the host's rejection decodable. The sizes are asserted by `-net-protocol-selftest` with canonical bytes for `SubstitutionAck`. |

**What the 5 000 ms session `timeoutMs` means, as of `f14d9a5d67` (no pin value changes).** P2, P3, P11
and P14 all anchor to it, so its meaning is pinned here: the session timeout is a silence budget
measured only across intervals in which the session was actually evaluated. `NetSession::CheckTimeouts`
runs from `NetSession::Tick` alone - never from `TickAdmissionPlane` or `InjectEvent`, which is the
pair a live round drives - so a round hands the session no evaluation and no receive for its whole
length. An interval longer than the budget between two evaluations is therefore read as a resumption
and restarts every seated peer's window instead of evicting it, ONCE per silence: a peer that still
says nothing through the next full budget is evicted however slowly the caller evaluates from there,
and any packet from it earns it a fresh restart. **P14 is exempt and unchanged**: a handshake expires
on the connection's own age (`connectedAtMs`), which no resumption moves, and
`ExpireSilentHandshakes` runs on the resumption evaluation like any other.

**The resumption floor's precondition, as of `0a5d6878f9` (no pin value changes).** "Once per silence"
means a peer whose window has been restarted and has not spoken since is judged normally at the next
evaluation. So **two unwatched phases with nothing decodable between them evict**: the second
transition restarts nothing and the accumulated silence is measured in full. A normal round between
them clears the mark - any decodable session message from that peer does, at
`NetSession::ProcessPacket` - and the second transition is granted its restart. Production satisfies
this many times over: `NetLobbySession::Tick` calls `session->Tick` every 5 ms and
`heartbeatIntervalMs` is 50, so a round of a few hundred milliseconds is enough, and a real match
phase hands the session **zero** decodable messages, which is the premise the whole rule rests on.
`timeout_resumptions` is the number that shows it - a transition that restarts nothing does not
increment it - and more than one per round transition on a live host is the signature of a caller
ticking the session more slowly than the budget.

**The seat's two bounds, and which should own the other (recorded for the next slice; NOT implemented
here).** Once `stage2/h4-b1-resync-fix` merges, one seat is bounded by two independent constants: the
plane's `c_ProvisionalExpiryMs` = **P2, 20 000 ms** and the round's `c_ReclaimHoldFrames` = **1200
frames** (`NetLockstep.h:467` on that branch), which `RefreshLeftSeatHolds` conjoins - so the round's
hold is `min(plane, frames)` and the frame bound is a deterministic ceiling the plane may only
shorten. At the pinned 30 Hz timestep 1200 frames is 40 s against P2's 20 000 ms, so the plane binds
first and the ceiling is never reached in a real round. **The residual is that neither constant is
derived from the other**: a change to P2, to `c_ReclaimHoldFrames` or to the timestep could put the
ceiling under the plane, and a headless replay that advances frames faster than wall clock already
can. The frame clock should own the seat hold - it is the only one every peer shares, and the plane
already learns the lockstep frame (`NetMatchService.cpp` sets it on the session every pump) - so
expressing P2 in frames derived from the timestep would make the seat and the round expire together
and remove the coupling. **No pin value changes for this**; it is the next slice's item, and until it
is done a change to either constant needs the other checked against it.

## 0. Problem, threat model, and the ADMISSION-ISOLATION requirement

A dropped player's seat (team/units/funds) sits empty; admission is automatic, identity-gated
only by build/config/mods, with NO moderation — so any compatible connection can claim a
dropped seat. Closes **H4 as defined** (opportunistic direct-IP takeover). NOT in scope: a
malicious host, active MITM (GNS encrypts), or **host-process loss/rejoin (= host migration).**

**⛔ MUST-FIX (found by plan review): admission traffic can currently kill a running match.**
`NetLockstepCoordinator::HandleEvent` (`NetLockstep.cpp:1579`) fails the coordinator on a
`ConnectionFailed`/`TransportError` from ANY transport, and a `PacketReceived` that isn't
recognized session/lobby traffic falls to `Fail(ProtocolError)`. So an unauthenticated joiner's
malformed/oversized/stale packet or dropped connection can STOP the live match. **The design
REQUIRES an isolated admission plane:**
- An UNBOUND transport (one not yet the active incarnation of a committed seat) can never feed
  the lockstep sim, and its malformed packets / codec failures / disconnects / transport errors
  can never stop or mutate the running match.
- Only after authentication + commit may a transport become the seat's active incarnation and
  reach lockstep.
- **Achievability note (verification finding — the harder-than-it-looks part):** malformed
  `PacketReceived` DOES carry a usable transport peerId, so bound/unbound discrimination works
  there (`SenderOwnsTransport`, `NetLockstep.cpp:1673`). BUT `ConnectionFailed`/`TransportError`
  are emitted with `c_InvalidNetPeerId` (`GnsTransport.cpp:341,391,420`), so they are NOT
  peer-attributable inside `HandleEvent` — and today they unconditionally `Fail` the coordinator
  (`NetLockstep.cpp:1634`). Real seat losses instead arrive as peer-attributed `PeerDisconnected`
  (already handled safely). So the isolation needs work OUTSIDE the peerId check: reclassify
  `ConnectionFailed`/`TransportError` as NON-fatal to a running match (a joiner's connection
  dropping is not a lockstep failure), handling only the one genuinely-fatal sub-case — the local
  transport's own `ReceiveMessages` failing (`GnsTransport.cpp:341`) — as a real error. Consider a
  separate admission transport as the clean structural alternative.
- **BOTH planes must be hardened (verification finding):** the admission plane is the mid-match
  `NetSession` pumped via `PumpSessionEvents`→`InjectEvent` (`NetMatchService.cpp:488`), and that
  session ALSO `SetFailed`s unconditionally on `ConnectionFailed`/`TransportError`
  (`NetSession.cpp:299`). Routing unbound errors to the admission plane without hardening it just
  moves the kill (a reconnect-plane DoS). Harden the lockstep coordinator AND the admission
  session. Latent today (those events don't reach the session yet), so it WILL be missed if not
  called out here.
- Gate (9a): live-match attacks — malformed, oversized, random, and old-version packets from an
  unbound transport, plus its abrupt disconnect/connection-failure through BOTH planes — and
  PROVE existing players continue with unchanged sim hashes.
- **Scope note (Step 2 review, 2026-07-10):** the two LIVE-match planes are hardened (the
  `NetLockstep` coordinator + the mid-match `NetSession`). A THIRD surface — `NetLobbySession`,
  used only for the rematch/resync lobby ROUND *between* matches (never during a running match) —
  still `Fail`s on any `ConnectionFailed`/`TransportError`/`PeerDisconnected`, so an unbound
  joiner faulting inside a rematch/resync lobby window can abort that round. It carries the same
  `m_Config.host` + `m_RemoteTransports` context the coordinator uses, so the identical fix
  applies: host → a per-connection fault, and a `PeerDisconnected` whose transport id is NOT a
  committed remote, are non-fatal; a committed-remote disconnect + `LocalTransportFault` stay
  fatal; a client's lone link stays fatal. DEFERRED to land WITH §4 (when mid-session admission
  becomes a first-class, exercised path) so it is gated by a lobby-plane admission test rather
  than expanded into Step 2 against the named-planes scope. Exposure today is narrow (only the
  brief rematch/resync lobby window on a public-IP host; the pre-existing lobby already aborts on
  any peer disconnect). `NetLobbySession` already treats the new `LocalTransportFault` as fatal
  (correct; no regression).

## 1. Chosen model + phase split (+ history)

Automatic authenticated reclaim; host as a moderator who can override (not a per-reconnect
bouncer). A claimant WITHOUT a valid ticket cannot prove they are the original player, so "host
approves a ticketless original" is cryptographically identical to "substitute a new player" —
both are explicit seat reassignment. Therefore:
- **Phase A = secure automatic reclaim ONLY.** Valid ticket+proof → back in; missing/wrong →
  denied. No host override, no applicants.
- **Phase B = explicit substitution / moderation.**

*History:* a first cut bound reclaim to the client's random per-process nonce; it was implemented
then REVERTED because `-RejoinTest` launches a fresh process (new nonce) → strict nonce-binding
denies a relaunched player its seat. C+ (host-issued, match-scoped, per-generation credential)
supersedes it.

## 2. Completion criteria (corrected — the old §2/§8 contradiction is resolved by split gates §9)

- **After Phase A → "H4 authentication / automatic-reclaim security CLOSED; reconnect UX
  functional"** (the §9a gates pass, including the real same-process + crash/relaunch UX,
  headed-tested). Phase A does NOT claim "flawless reconnect."
- **After Phase B → "moderation / substitution FUNCTIONAL"** (the §9b gates pass).
- **"Flawless reconnect" / product sign-off comes LATER** — only after the fidelity fixture, the
  four save-holes, and T=300:30 are all green, because the reconnect path loads a snapshot that
  still has those latent fidelity holes + the tick-313 divergence. This keeps H4 first without a
  false completion claim.

## 3. Core identifiers + state ownership

- **Auth-session epoch:** separate, off-sim CSPRNG. Random per newly-hosted session; STABLE
  across resync/rejoin/rematch; cleared on true session end. **DISTINCT from any identifier in
  the deterministic-config / sim-identity hashes** (never randomize `c_UiSessionId` or anything
  feeding `deterministicConfigHash`/`sessionIdentityHash`).
- **Stable seat id + holder generation.** Reclaim addresses a seat by its stable id, never a
  transient peer id. "One credential, no rotation" = one credential per HOLDER GENERATION,
  reusable across that holder's reconnects; substitution INCREMENTS the generation and issues a
  new credential, which invalidates the prior generation's credential + challenges.
- **Seat-ticket registry lives in the long-lived match service** (`NetMatchService` / a
  dedicated seat registry it owns), NOT `PeerState`, NOT a `NetSession` instance or a match
  round. Survives resync/rejoin/rematch; disappears on true session end.

## 4. Handshake + reclaim — TRANSACTION semantics (not just "idempotent")

The current client sends `Ready` immediately after `JoinAccepted`, so the handshake is expanded.
Every message carries a **transaction id** (P1: 16 B, CSPRNG, drawn by the initiator); terminal
results are cached and REPLAYED on a duplicate (an ACK loss must never turn a success into a
failure). **Build/config/module identity is re-validated on NewJoin, Reclaim, AND Applicant.**
Reclaims never use ordinary `AllocatePeerId`.

- **First join (lobby):** `NewJoin(+identity) → provisional seat (provisional expiry 20 000 ms, P2) →
  TicketOffer(txId) → client durably persists → TicketStoredAck(txId) → JoinCommitted/
  JoinAccepted → Ready`. Persistence-failure BLOCKS joining (fail closed). **Failure windows to
  handle + gate:** TicketOffer persisted but `TicketStoredAck` LOST with the connection → the
  client owns a ticket for an UNCOMMITTED provisional seat → on reconnect it RESUMEs that
  transaction (host recognizes the txId + provisional seat) or the provisional seat EXPIRES and
  the client's orphan ticket is ignored (stale). **Provisional-expiry timeout = resume window =
  20 000 ms (P2); each handshake step retransmits at 250 ms up to 8 times (P3).**
- **Reclaim (live match):** `Reclaim(epoch, stableSeat, holderGeneration, +identity) →
  Challenge → Proof → atomic claim → JoinAccepted(txId)`. A valid proof retransmitted because
  `JoinAccepted` was lost gets the CACHED terminal success (the challenge was consumed on first
  proof — a duplicate must NOT read as an auth failure). Authenticate BEFORE peer-id allocation;
  a committed reclaim reuses the seat's own recorded peer id and never calls `AllocatePeerId`
  (P4), and identity is re-validated ahead of the seat lookup (P5).
- **Applicant (Phase B):** `Applicant(+identity) → PendingReassignment` (bounded — 4 global /
  2 per seat / 1 per connection, P15; no peer id / team / snapshot / authority until an approved
  commit).

## 5. Cryptographic transcript + resource contract (pinned, not descriptive)

- Proof = **HMAC-SHA-256(credential, transcript)**. **Transcript byte schema (fixed):** a
  domain/message-type tag (16 B zero-padded ASCII, P7) ‖ protocol version (u16 LE =
  `NetProtocol::c_Version`, P8) ‖ auth epoch (16 B) ‖ stable seat (u16 LE, the slot index, P9)
  ‖ holder generation (u32 LE, from 1, P10) ‖ server challenge (32 B) ‖ fresh client nonce (16 B), all
  length-prefixed / fixed-width, little-endian, canonical order. Credential = 32 B; challenge =
  32 B; nonce = 16 B; tag = 32 B.
- **Challenges:** server-issued, **connection-bound**, expiring (**10 000 ms**, P11), **consumed
  atomically on the first proof attempt**, compared at **exact length in constant time**.
- **No-enumeration (decided):** an unknown seat is **issued a SYNTHETIC challenge** and then a
  uniform rate-limited failure (1 attempt per connection per 1 000 ms; every denial released at a
  fixed issue+1 000 ms, P16) — identical flow/timing to a known-seat bad-proof — so unknown vs
  known is not distinguishable. (Immediately rejecting an unknown seat while challenging a known
  one is itself an oracle; the synthetic challenge removes it.) **Synthetic challenges MUST be
  STATELESS — they do not draw from the bounded outstanding-challenge pool** (an unknown seat has
  no stored secret, so nothing need be retained); otherwise unknown-seat spam refills the shared
  cap and starves legitimate reclaimers' challenge slots (verification finding).
- **Resource caps:** bounded outstanding (stateful) challenges (**2 per connection / 8 global**,
  P12), bounded provisional seats (**4, one per stable seat**, P13), bounded unauthenticated
  connections (**8 concurrent, dropped after 5 000 ms without a valid ClientHello**, P14);
  pending-applicant spam bound (**4 / 2 / 1**, P15). **The txId terminal-result cache is bounded
  with a retention window** pinned against the challenge- and provisional-expiry timeouts
  (**64 entries, 60 000 ms = 6× P11 = 3× P2, expire-by-age then oldest-first**, P17); every H4
  admission message is capped at **1 024 B** of payload and refused before parsing above it (P18).
  A duplicate arriving after eviction is a benign spurious-denial for reclaim and is CAS-guarded
  for substitution (the window above is what makes that intentional, not accidental).
- **Provider:** OpenSSL (`RAND_bytes`/HMAC), GNS-enabled only (`RTEA.vcxproj:745` /
  `meson.build:288-292`), behind an injectable interface; default GNS-disabled build compiles +
  **fails closed** (no issuance → no auto-reclaim). The **deterministic test provider is
  test-only and NOT selectable via any runtime/network input.** No `std::random_device`,
  home-grown HMAC, or non-crypto hash fallback.
- **Secret hygiene + gate:** never DELIBERATELY serialize the credential/challenge/proof into
  logs, reports, saves, replays, or crash artifacts; add a **secret-canary scan** over all
  generated artifacts as a gate. **The scan carves out the ONE sanctioned durable ticket store**
  (the user-private recovery record, §4/§11) via an explicit allowlist (**exactly one resolved
  path, whose presence is itself asserted whenever a ticket was issued**, P19) — otherwise the gate
  either false-positives on the ticket file or, if naively excluded, loses its teeth (verification
  finding). Honest caveat: no *deliberate* emission is guaranteed; a full process-memory dump is
  NOT claimed impossible.
- **Failure injection (gate):** RNG failure, HMAC failure, provider-unavailable — all fail
  closed.

## 6. Single-active-incarnation (decided)

Two connections may hold the same valid ticket. **Policy: a newly PROVEN connection REPLACES the
old transport for the seat.** Incarnations are numbered per (stable seat, holder generation),
u32 from 1, incremented at each proven rebind, saturating fail-closed (P20). The superseded
transport is **fenced** — its delayed/in-flight packets are ignored for the seat, and its later
timeout MUST NOT evict the seat (attributed to the dead incarnation). Gate the
two-simultaneous-valid-holders case.

## 7. Lifecycle + the LEAVE protocol

- **Initial LOBBY:** ticketless clients may fill NEVER-HELD seats. **Rematch LOBBY:** previously
  claimed seats remain PROTECTED for the whole hosted session, with no timer (only their
  holder-generation credential reclaims; P24). **Live match:** ticketless joins are DENIED
  (Phase A) / become Applicants (Phase B).
- **Clean leave IS a protocol, not a flag:** `LeaveRequest → host closes the seat + revokes the
  holder generation + its challenges → LeaveAck`. Only an acknowledged LeaveAck clears the
  client ticket; the request retransmits at 250 ms up to 8 times and, unacknowledged after
  2 000 ms, is an ambiguous loss that KEEPS the ticket (P21). Gate: after a clean leave the OLD
  ticket cannot reclaim.
- **Never clear the client ticket on an ambiguous network loss** (that's when it's needed). The
  store holds exactly ONE record at the injectable path, deleted only on LeaveAck, on a confirmed
  session end, or past 24 h; the credential rotates only with the holder generation (P25).
- **Confirmed hosted-session end** is the authoritative event that permits deleting the recovery
  record. **Defined (P22): EITHER a `NetDisconnect` carrying the new `SessionEnded` reason, which
  the host sends from the same place it clears the seat registry, OR a clean LeaveAck — and
  nothing else.** A transport disconnect, a session timeout and a match `Complete` explicitly do
  NOT qualify, because a rematch keeps the epoch and tickets alive.
- **Ignore stale epochs** — a non-current epoch on any message other than a `Reclaim` is dropped
  and counted, never answered; a `Reclaim` carrying one takes the same synthetic-challenge +
  uniform rate-limited denial path as an unknown seat, so the two are indistinguishable (P23).
  **Completed → rematch:** epoch + tickets carry (session persists); a return during the lobby
  phase (between matches) skips snapshot resync (no live match to sync).
- **Host-process loss/rejoin OUT OF SCOPE** (host migration).

## 8. Restoration ownership (CORRECTED — the named seam cannot do it)

**Plan-review finding:** control overrides are CLEARED at coordinator handoff
(`ScenarioRunner.cpp:445`, `s_LockstepControlOverrides.clear()`), and the `NetGameSwitchControl`
command's self-only guard (`MovableMan.cpp:466`) only lets a sender take control FOR ITSELF — so
"restore control via a peer-issued NetGameSwitchControl" is NOT implementable, and no persistent
pre-drop ownership ledger exists. **BUT the underlying mechanism IS available for a system-authored
reseat:** `ScenarioRunner::SetLockstepControlOverride(uid, peerId)` (`ScenarioRunner.cpp:497`) is a
plain setter NOT gated by that self-only check (the check lives only in the command handler), read
by `IsLockstepLocalActor` (`:486`), and UID keys survive snapshot reload via `m_PersistedUniqueID`
(`MovableObject.cpp:316`) — so a reseat onto ANY peer, applied at a synced tick, is deterministic.
**Also corrected:** current **PvPvE gives each human their OWN team plus a CPU team**
(`NetMatchService.cpp:731`, CPU team `:740`) — it is NOT a shared human team, so the earlier
"co-op restoration" text was wrong.

The plan chooses (no-compromise target): **a deterministic drop-time ownership LEDGER** — at the
tick a seat drops, record which actors that seat controlled (deterministically, off-sim/on the
authoritative registry; a SEPARATE structure since the override map is purged for gone peers,
`ScenarioRunner.cpp:501`; retained for the whole hosted session, capped at 512 actor UIDs per
seat, cleared with the seat's registry entry — P26). On an authenticated return, before resumed
tick 1, apply a
**system-authored restoration transition** via `SetLockstepControlOverride` at a synced tick (not
a peer-issued SwitchControl) that reseats the returner/substitute onto the ledgered actors,
census-clean (no duplicated actors, no stale authority, no funds change, no abandoned commands).
Per mode:
- **PvP / PvPvE (each human owns a distinct team):** the returner regains control of their
  team's SURVIVING actors; dead actors are not resurrected — they get what survives, or stand
  down / lose per the win condition.
- **Co-op (shared human team):** units that reverted to a teammate hand control BACK per the
  ledger; dead actors stay dead; reactivate from stand-down.
- **Substitute (Phase B):** receives the ledgered ownership from resumed tick 1.
**The ledger IS the Phase-A target (binding, 2026-07-10).** The reseat is system-authored,
synchronized, and applied on every peer after coordinator handoff/snapshot load and BEFORE
resumed tick 1. The **defined deterministic default-reseat policy** (e.g., reseat onto the
team's brain/first-surviving actor by a fixed rule) exists only as a reported fallback: it may
NOT be taken without stopping and reporting to the user why the ledger is infeasible. Do not
promise "exact control" without one of them built.

## 9. Gate batteries (split)

### 9a. Phase-A / H4 gates (H4 CLOSED when these pass)
- **Admission isolation:** unbound-transport malformed/oversized/random/old-version packets +
  abrupt disconnect → existing players continue, sim hashes UNCHANGED.
- Valid ticket+proof → exact-seat reclaim; missing/wrong ticket → SYNTHETIC-challenge +
  uniform rate-limited denial (no enumeration oracle).
- Replayed / parallel / expired / cross-context (wrong epoch/seat/generation) challenge → denied.
- Duplicate valid proof (JoinAccepted lost) → cached terminal SUCCESS, not a failure.
- Provisional-seat failure windows: TicketOffer-persisted-but-Ack-lost → resume-or-expire; a
  disconnect before commit leaves no committed seat; persistence-failure BLOCKS the join.
- Single-active-incarnation: new proven connection replaces old; old transport's delayed packets
  + later timeout fenced.
- Clean-leave protocol: LeaveAck clears the ticket + closes the seat; old ticket can't reclaim.
- Ambiguous network loss does NOT clear the ticket.
- Restoration exact per mode (ledger or defined default), census clean, funds/authority unchanged.
- Rejoin after a resync round and across a rematch (ticket carries; lobby-phase return skips
  resync).
- Crypto: HMAC-SHA-256 known-answer tests + a real-provider GNS smoke (not only the deterministic
  provider) + RNG/HMAC/provider-failure injection (fail closed) + secret-canary scan clean.
- Default GNS-DISABLED build compiles + fails closed; GNS-ENABLED build green.
- Old-wire fixture: explicit rejection if a stable negotiation envelope exists, else documented
  best-effort disconnect (see §10).
- 2/3/4-player regression; **sim baseline `3292e7fc…` byte-identical** (auth is off-sim).

### 9b. Phase-B feature gates (FEATURE complete when these pass, plus 9a)
- Substitution wire transaction: `approval → provisional ticket → durable ack → seat-generation
  CAS → replayable commit result`. Gate initial-disconnect-before-ack, ack-lost,
  commit-result-lost, delayed-duplicate.
- Cleanup: returner-wins / host-cancel / substitute-disappears-before-ack each invalidate +
  remove the provisional record.
- Valid-returner-vs-simultaneous-host-reassignment (both orders): first COMMIT wins; loser gets a
  precise rejection.
- Two pending applicants for one seat; pending-applicant spam bound.
- Never "release seat then let a substitute join"; host action atomically targets one applicant +
  one seat.
- Substitute inert (no peer id/team/snapshot/authority) until the commit.
- Moderation UI: host sees disconnected seats + pending applicants → wait / substitute / cancel.

## 10. Wire / codec + old-wire

The new handshake/reclaim/substitution messages bump the relevant codec version + update the
selftests' canonical-byte assertions. **Whether an old client gets an EXPLICIT version-mismatch
rejection is an implementation-discovery gate:** verify if a stable version-independent
negotiation envelope exists (a magic+version prefix that always decodes) — if yes, explicit
rejection; if not, honestly weaken to a documented best-effort disconnect (a naïve bump currently
fails to decode before a clean rejection can be sent). Record the outcome; do NOT call every wire
decision unambiguous until that probe is done.

## 11. Reconnect UX (Phase A not done until this lands + is headed-tested)

- Same-process loss → automatic retry with visible status + cancel / manual-retry controls.
- Crash/relaunch → startup detects the recovery record (**injectable per-process store path** —
  the multiprocess e2e shares Userdata) and offers rejoin. Clear missing/corrupt/stale-ticket
  errors.
- Ticket persistence is DURABLE: flush + atomic replace + sync metadata where applicable;
  preserve the previous valid record if the new write fails; user-private permissions.
- **Persistent roster indication** for the remaining players ("Player X — disconnected /
  reconnecting"), not a transient toast.

## 12. Execution evidence

Worktree `D:\Projects\p4b-interp-validation`, clean base `aa30a2282`, **push prohibited**;
evidence under `D:\Projects\stage2_p4\` (own subfolder per gate run). Exact-tip expectations:
**Windows GNS-DISABLED build compiles + fails closed**, **Windows GNS-ENABLED build green**, WSL
green, macOS green (user-relayed; the P8B runbook predates this work → needs a fresh exact-SHA
runbook). Hash discipline: the **sim hash `3292e7fc…` stays byte-identical** (auth is off-sim);
the **protocol / session-identity hashes WILL move with the codec bump** — gate the sim hash
unchanged and re-pin the identity/canonical-byte expectations deliberately.

## 13. Execution record

- **Step 1 (crypto provider + off-sim epoch + seat credentials) LANDED 2026-07-10** — commits
  `6082a17f8` (`NetAuthCrypto`: OpenSSL `RAND_bytes` + HMAC-SHA-256 via `EVP_Q_mac` on GNS
  builds; fail-closed default otherwise; test override only via `SetNetAuthCryptoForTest`;
  constant-time compare) · `c24da93aa` (`NetSeatAuthRegistry` owned by `NetMatchService`: 16B
  epoch armed on the WorkerMain host path, cleared only at `Destroy`/`ReportRuntimeError` so it
  survives resync/rejoin/rematch; 32B per-seat per-holder-generation credentials; revocation
  never rewinds generations) · `c4c185a64` (8th selftest `-net-auth-selftest`: RFC 4231 KATs
  1-4/6/7, RNG sanity, failure injection fail-closed, registry semantics, both build flavors;
  `run_wsl_p5_leg.sh` runs 8). Gates run: GNS-enabled build + 8/8 selftests + baseline
  `3292e7fc…` byte-identical with the epoch armed on the host (`stage2_p4\h4_step1_baseline`);
  GNS-DISABLED build compiles + its selftest proves fail-closed. No wire messages yet = no new
  live admission traffic (guidance order holds).
- **Step 2 (§0 admission isolation, BOTH planes) LANDED 2026-07-10** — commits `5cecc5f88`
  (new `NetTransportEventType::LocalTransportFault`: `GnsTransport`'s receive-pump failure emits
  it instead of `TransportError`, so the genuinely-fatal case is distinguished by TYPE, not by a
  matched string) · `5964292e4` (the isolation: coordinator `HandleEvent` fails on
  `LocalTransportFault`, treats per-connection `ConnectionFailed`/`TransportError` as non-fatal on
  the relay host — every production host relays, the same discriminator `SenderOwnsTransport` uses
  — still fatal on a client's lone link, and fails on an undecodable `PacketReceived` only from a
  bound/trusted source, counting+dropping unbound garbage via `ignoredAdmissionFaults`;
  `NetSession::ProcessEvent` makes the host role non-fatal on per-connection faults
  (`unboundConnectionFaults`, which also closes the pre-match lobby DoS), keeps the client's
  lone-link fast-fail, and fails on `LocalTransportFault`; `NetLobbySession` handles the new enum)
  · `1ab7fc2e1` (the 9th selftest `-net-admission-selftest`: a pass-through transport injects
  unbound garbage/oversized/truncated packets + connection faults into a REAL running 2-peer match,
  asserting the commit stream stays byte-identical and every fault is counted, with positive
  controls that a bound peer's garbage AND a local fault still Fail, and that the session plane
  survives unbound faults while a client's lone-link fault + a local fault still fail). Gates at tip
  `1ab7fc2e1`: GNS-enabled build + 9/9 selftests + baseline `3292e7fc…` byte-identical + perturb
  (real desync caught @50) + mismatch (rich reject both peers) + GNS-DISABLED build (admission +
  auth selftests green on it). Satisfies §0 / gate 9a's admission-isolation line and is the safe
  floor for the handshake. Still NO new live admission traffic on the wire.
  - **Post-commit review (2026-07-10) closed the §9a "2/3/4-player regression" gate** that the
    first pass had missed (only 2-peer was run): 3-peer sim-gated identical all peers
    `273549b3…`, 4-peer `5a9f0007…`, and a 3-peer hard-DROP adjudication (survivors identical
    `a4c07f6a…`) — the last confirms the untouched `PeerDisconnected` relay-adjudication path is
    undisturbed by the `ConnectionFailed`/`TransportError` reclassification. The review also
    surfaced the `NetLobbySession` third-plane follow-up recorded in §0 (deferred to §4).

## 14. Sequencing

Phase A (logic → UX, headed) → Phase B (substitution + moderation UI) → **manager-booted
fidelity fixture** → **save-holes 1-4** → **close T=300 tick-313 (repeated 300:30 byte-identical)
→ cross-platform fidelity checkpoint** → increment 6 (fast in-memory restore → D=1 → D=2 driver)
→ exact-tip cross-platform rollback gates → **SimChecksum extension + three-platform re-pin DEAD
LAST**. The fidelity fixture, increment 6, and the re-pin each get their own plan doc
(`STAGE2_FIDELITY_FIXTURE_PLAN.md`, `STAGE2_P8_4_INCREMENT6_PLAN.md`,
`STAGE2_SIMCHECKSUM_REPIN_PLAN.md`) before that work begins.
