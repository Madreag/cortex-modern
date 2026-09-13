# Lead's own line-by-line review of the overnight delta (d81478d2ee..f83cc75099) — started 18:12 UTC 2026-09-12

Scope: 111 non-merge commits, 25 merges; Source/ 79 files +11306/-270; tools/ 10 files +2808/-3. The user asked for a
full personal review after the four Grok audits (their findings are in audit-1..4/VERDICT.md and were acted on).
This log records, per file, what the change does, the verdict, and every finding with file:line. Method: `git diff
d81478d2ee f83cc75099 -- <file>` read in full; production code first in risk order, selftests and tools skimmed for
oracle validity. Findings are fixed by the lead or dispatched with red-first proof; nothing is "noted and left".

Order: (1) sim/determinism: MovableMan, LuaMan, Actor, ScenarioRunner, NetLockstep, ControllerFrame, Controller,
ActivityMan, LocalPrediction, AudioMan, PostProcessMan, PreviewEventLedger; (2) reconnect/rematch: NetMatchService,
NetMatchRunner, NetMatchConfig, NetSession, NetReconnectSession, NetLobbySession, NetIdentity, NetReconnectUx;
(3) directory/HTTP/P2P: NetHttpClient(+Apple), NetDirectoryClient, NetDirectoryCodec, NetDirectorySignalChannel,
GnsSignaling, GnsTransport; (4) wiring: Main.cpp, MainMenuGUI, MenuMan, SettingsMan; (5) selftests; (6) tools.

## Findings register (running)
(final register at the end of this log)

## CLOSING (18:59 UTC 2026-09-12; the clock was read) — every file of the delta read
Coverage: all 79 Source files and all 10 tools files of d81478d2ee..f83cc75099, read hunk by hunk (production first,
then every selftest and every tool script), plus the four lane branches merged today (audit-1, audit-3, W117,
W80-3) and W89's partial branch. Verdict: no determinism defect and no mod-compatibility break in the overnight
delta; six findings, all dispositioned (F1/F2 fixed by the lead and committed on stage2/fixgroup-6-lead with the
selftest checks green on the lead build; F3 recorded as a constraint on the frame lane and carried into W90's brief;
F4 fixed in the A7 driver, V-A7-2 re-running; F5 fixed red-first by W117 and merged; F6 latent, on the item-8
roadmap). One oracle note outside the delta (the lead's first 'concurrent engines' reading was wrong; disproved by a
quiet re-test): the replay-driven -global-callback-selftest needs the Checkpoint Global fixture that
tools/test_global_callbacks.py installs; without it RunGlobalCallbacksSelfTest returns false silently. With
the driver it PASSES on the lead candidate (D:/mx/lead-fg6/gcb-lead), which also verifies F1's new stale-uid
case and audit-1's added-actor arms. Lead fix: a FAIL line naming the missing fixture. W116 is root-causing
the bare-flag hang (likely the same shape).
What was NOT done here: the three-build compat proofs and the batteries are the fixgroup-6 candidate's job (the
battery + ONE Source44 family on stage2/fixgroup-6-lead once W97-2, W71-2b, W102-fixtures and W116 land).

## Per-file verdicts

### Source/Managers/MovableMan.cpp/.h (+86/-6) — VERDICT: sound; one LOW test gap
W105 (contiguous index lifetime): clear in PurgeAllMOs and GetAllActors; erase in RemoveActor and both Update deletion
paths; RebuildContiguousActorIDs extracted (same order); SaveWorldStructure derives the archive from live m_Actors;
LoadWorldStructure validates every entry names a cohort member. W69/W70: PreControllerStage expires the synced-order
hold on the sim tick (every peer, every actor, same tick). W71: drainDeferredWaypoints after the threaded pass, the
serial pass and the AIWriteScript: local-controller actors send, others drop their queued calls (correct: only the
owner's AI output is authoritative). DumpSimState prints the door state (diagnostic).
- LOW (test gap): RunContiguousActorIndexSelfTest's orphan case uses uid 0, which the `entry.first <= 0` check refuses
  before the `!actors.contains` branch ever runs; the R1 shape (a nonzero stale uid) is not exercised. Add a nonzero
  orphan case. -> register.
- The three added-actor sites AUDIT-1 named (RemoveActor on m_AddedActors, AbsorbAddedMOs, DiscardAddedSince) are not
  covered here; audit-1-fixes is adding the erases.

### Source/Managers/LuaMan.cpp (+93/-5) — VERDICT: sound
W106/W106-2: ScriptGraphPushEntity tries the recorded type then GetClassName(), returns false when no cast exists and
ScriptGraphOwnerReference then falls back to the plain luabind push with the static type (the pre-W106 behaviour), the
constant flag re-applied; Graph.validate accepts a class-only token when a cast exists or the global is a bound class
rep (_ScriptGraphEntityCast). Selftests: entity_cast_functions_cover_bound_classes walks LIST_OF_LUABOUND_OBJECTS and the
Lua global class reps (the detecting test that was red on six classes); background_owner_reference_restores (typed,
untyped and unknown-class pushes all resolve to the scene's SLBackground); class_only_owner_reference_validates.
No findings. (The oracle group's +698 LuaMan lines live in fixgroup-6, reviewed separately below.)

### Source/Entities/Actor.cpp/.h (+101/-9, +34/-9) — VERDICT: sound pending one check
W70: BeginGoToOrder holds the seat under lockstep; IsAwaitingGoToPoint + the restore re-derivation (superseded by
W70-2's checkpoint in fixgroup-6). W71: DeferWaypointMutation = the running actor's own writes under lockstep only;
AddAISceneWaypoint / AddAIMOWaypoint (no-duplicate-tail rule read off the pending queue) / ClearAIWaypoints defer;
ExecuteDeferredWaypoint nulls g_CurrentAIActor so the re-entrant call does not re-defer; SendDeferredWaypoints sends
NetGameAIOrder ops under lockstep (a target that died before apply is skipped identically on every peer via
FindObjectByUniqueID), executes directly otherwise (unreachable in practice: nothing queues outside lockstep).
Single-player path unchanged (the defer predicate requires lockstep). AUDIT-2 H3-1 (a cross-actor write from A's AI
onto B stays local and A's per-machine AI can differ) is real and is W71-2b's job.
- CHECK: SendDeferredWaypoints enqueues NetGameCommand{0, ...} (sender 0) while SwitchToActor passes the local id;
  verifying the enqueue path stamps the sender (else the team gate drops the op).
  RESOLVED: EnqueueLocalGameCommand gates on the local id when the sender is 0 and the coordinator stamps every
  local command with localPeerId before it rides (NetLockstep.cpp:2751). No finding.

### Source/System/ScenarioRunner.cpp/.h (+188, +41) — VERDICT: sound
Net-UI toasts (presentation-only deque + report log; never hashed/saved), PumpLockstepStallUI (event poll + F6 +
seats-panel update from the stall wait; the wait runs on the main thread — Main.cpp has no sim thread — so the SDL
poll is legal), DescribeLockstepHoldPause (one answer for the overlay and the seats panel), e2e tick knobs
(leave/pause/end-round ticks), controller-log NLS mismatch downgraded to a note (the threaded state count does not
change the sim — consistent with W111), ResolveResyncDropFrame now refuses any count that is not the completed tick
(W62; the rewind path is gone: NetMatchService no longer calls RewindSimTo), WaivePendingPeersWhileWaiting replaces
ApplyPendingRecoveryStopWhileWaiting in the wait loop. No findings.

### Source/Network/NetLockstep.cpp/.h (+87, +22) — VERDICT: sound; one LATENT dependency recorded (F3)
W62 waiver: the relay host (every host: relayToOtherPeers = config.host) waives a fenced incarnation's frames from
the parked tick only while a recovery stop is pending and only for peers whose seat state says fencedTransport; the
waiver rides as a PeerDropped stop with the "fenced:" prefix, accepted on clients only from the round authority;
waivers reset per round (ResetRoundState); IsRemoteRequiredForFrame honours them; ReclaimResyncPending guards the
three last-player-leave end paths so a pending reclaim resync is not thrown away; the hold-resolution resync is now
deferred to the boundary (RequestResync default immediate=false), which is what makes the snapshot boundary rule hold.
- LATENT (F3, no change now): the ready-frame assembly merges every PRESENT remote frame, required or not. A waived
  incarnation's frame that arrives after the waiver but before the parked tick commits is taken on the host and
  relayed; the clients take it too because frames and the waiver notice share ControlReliable (ordered), so the apply
  set stays identical. If the frame lane is ever switched to InputUnreliable (a latency lane could), the waiver must
  also erase the waived peer's stored frames >= F and drop its later arrivals, or the parked tick's apply set can
  differ between the host and a client. Constraint carried into any lane that touches frameLane (W90).

### Source/System/Controller.cpp/.h, Source/Network/ControllerFrame.cpp — VERDICT: sound
ReleaseDelayPassed counts sim time for every seat (a parked tick cannot re-arm; the single-player debounce now
stretches with time scale and pauses — acceptable). Synced-order hold: ApplyWireState keeps m_Disabled while the hold
is armed and the owner's own quick-disable takes the hold back; ExpireSyncedOrderDisable caps the hold at
c_SyncedOrderDisableMaxHoldTicks = 60 = NetLockstepCodec::c_MaxInputDelayFrames (checked equal); Create copies the
field. Selftest covers real-time no-rearm, sim-time rearm, the pre-order frame, the owner's disable and the cap edge
(364 held / 365 expired). No findings.

### Source/Managers/ActivityMan.cpp/.h — VERDICT: sound
SaveCompression (Fast=2 user saves, Small=6 resync snapshots) threaded to the zip entry level; LastSaveMainMs/ZipMs
written on the pool thread and read after WaitForSaveGameTask (the future's get synchronises). The global-callback
selftest now also runs the contiguous-index selftest. No findings.

### W104a group: AudioMan.cpp/.h, SoundContainer.cpp, PostProcessMan.cpp/.h, PreviewEventLedger.cpp/.h,
### LocalPrediction.cpp/.h — VERDICT: sound with one LOW defect (F2)
A previewed emitter's sound plays during the preview on the canonical container (domain Presentation,
predicted=true), excluded from AudioMan::SaveCheckpoint and SoundContainer::SaveCheckpoint; the canonical emission
adopts it (owner/domain/priority/pitch/pan/3D re-applied), extra predictions retired; consecutive previews dedupe
through AlreadyPlayed; the ledger keys on (kind, emitter, stable asset identity, preset hash, tick, seq) with a
one-tick slip tolerance and expiry one tick after; post effects use the same ledger; the sound identity cursor is
restored after the preview. Sim-visible reads are unaffected: logical containers answer IsBeingPlayed from logical
voices (which die with the shadow clone), and the tick hash does not cover physical voices.
- LOW (F2): RetirePredictedVoice lets a mispredicted ONE-SHOT finish but sets predicted=false and owner=nullptr, so
  from then on it is an unowned presentation voice that AudioMan::SaveCheckpoint captures (ownerIdentity 0). A resync
  snapshot or a save taken while it plays carries a voice that exists on the predicting peer only, and a peer
  restoring that snapshot replays it. Fix: keep predicted=true on the finishing one-shot. Lead fixes with a selftest.
- The at-capacity refusal (a predicted voice counts toward c_MaxPlayingSoundsPerContainer on non-logical containers)
  is presentation-only and rare; noted, not registered.

### Small files: ACrab/AHuman (GoTo -> BeginGoToOrder), LuaAdapters (+6 cast functions), MenuMan (§11 recovery
### pump + rejoin offer on entry), NetModerationGUI (hold title from one source; input-delay line), NetLobbyProtocol
### (reserved bit 0 = dedicated; any other bit still refused), NetLobbySnapshot, AIWriteScript (scene-waypoint /
### clear-waypoints), ContractAudit.h + StateInventory.csv (DeferredWaypoint fields; pending queue transient) —
### VERDICT: sound. No findings.

### Source/Network/NetMatchService.cpp/.h (+427, +98) — VERDICT: sound
Dedicated host (seatless peer 1; humans 2..N; CPU team = humanCount), directory row (advertised from lobby through
running, retracted on complete/finish/leave/destroy, seat counts from the admission table while running), rematch
survivors derived on clients from the coordinator's leave frames (the host reads its live session), the session-event
handover drained through the session before every teardown (census only at a tick boundary), lockstep totals
accumulated across resyncs, resync metrics, §9b one-shot substitute application, §11 recovery pump gate, Update()
de-duplicated per millisecond, input-delay text built beside each lobby publish from the exchanged config. The
snapshot is refused unless the sim stands on the completed tick (no rewind path remains). No findings.

### Reconnect/rematch group: NetIdentity (num_lua_states out of the deterministic-config hash; HashSessionIdentity
### exposed), NetLobbySession (transfer timing, diagnostic), NetMatchConfig (DeriveRematchConfig: survivors sorted and
### unique, host required, seats close up onto 1..N, CPU kept, per-peer delays remapped, validated; dedicated
### validation; "dedicated" in the hash only when true), NetMatchRunner (PrepareRematchRoster: host from the live
### session's Ready peers within the roster, client from its own survivor list; unchanged hash short-circuits;
### RenumberReadySeats + admission seat table rebuilt with the OLD stable seats; client adopts its id;
### VerifyRematchProposal compares peerCount/hostPeerId/dedicated/sorted seats), NetReconnectSession (live-match
### refusal carries a "live_match" NetJoinRejected; any-seat application picks the first substitutable seat with room,
### retransmit dedupe by txId, the ack names the seat), NetReconnectUx (NoteDropped ignored while active/cancelled/
### spent; Offer::Missing; NetModerationPanelTitle), NetSession (RenumberReadySeats validates range/uniqueness/
### coverage; AdoptRematchPeerId) — VERDICT: sound. No findings.

## Findings register (running) — entries
- F1 LOW test gap — MovableMan::RunContiguousActorIndexSelfTest orphan case uses uid 0 (refused by the <= 0 check,
  never by the cohort check). Fix: add a nonzero stale-uid orphan. Owner: lead. DONE d6a0e0f123; verified with the
  official driver on the lead build ('rejected: invalid contiguous actor index member 4242', refused=1).
- F2 LOW presentation leak — AudioMan::RetirePredictedVoice clears predicted on a finishing one-shot; it then lands
  in the audio checkpoint as an unowned voice. Fix: keep predicted=true. Owner: lead, with a selftest check.
- F3 LATENT constraint — the W62 waiver's content safety relies on frameLane == ControlReliable (ordered). Any lane
  that changes the frame lane must add erase-on-waive + drop-after-waive. Owner: lead (brief constraint for W90).
- F4 fixture — the A7 coop_hand_back arm runs the default P4 Alpha Duel (armed; team 0 loses by tick 272 at seed
  42), so the reclaim finds the activity Over and the arm fails regardless of the rematch code (V-A7: identical on
  the milestone and on W80-3). Fix applied 18:2x UTC: preset 'Net Lifecycle Test' in a7-repaired-driver and the
  W80-3 harness copy (its slot-2 human gets the Green Dummy; unarmed; never ends). V-A7-2 re-runs the arm.
  CLOSED 19:03 UTC: V-A7-2 PASS on both pinned exes (milestone D:/mx/va7/m3, W80-3 D:/mx/va7/w3): the unit is handed
  back alive to the returning peer on the resumed round and the local_control views match across the reload.
- F5 MEDIUM platform asymmetry (item 8) — pinned-certificate mode differs between the two HTTP clients. Apple
  (NetHttpClientApple.mm didReceiveChallenge): a leaf whose DER SHA-256 equals the pin is accepted outright, whatever
  its chain, name or dates. Windows (NetHttpClient.cpp WorkerMainImpl): pinned mode sets only
  SECURITY_FLAG_IGNORE_UNKNOWN_CA, so WinHTTP still validates the subject name and the dates; the README's own
  self-signed recipe (`-subj "/CN=cortex-directory"`, no SAN, reached by IP) therefore fails on Windows with
  "certificate verification failed" even with the right pin, while the Mac joins. The README also says "Do not pin a
  certificate hash in the engine" and "a user-supplied CA file" — the engine has no CA-file path and the pin setting
  (SessionDirectoryCertSha256) is the only way a self-signed directory can work. Fix: in pinned mode Windows ignores
  unknown CA + CN mismatch + date (the pin IS the identity, as on Apple); README: the pin recipe (sha256 of the DER)
  and a SAN in the openssl line; a detecting test with a CN-mismatched self-signed cert. Owner: lead designs, Grok
  implements red-first (W117).

### Directory/HTTP group: NetDirectoryCodec (fail-closed JSON decode with the service's caps; peer/base64/join-mode/
### state validation; IsJoinable on the five identity fields), NetHttpClient (WinHTTP async; pin checked in
### SENDING_REQUEST before any request byte and again at send-complete; handle closed under one recursive mutex;
### HANDLE_CLOSING awaited before the state is freed, leaked on the documented-impossible miss; 4 MiB body cap;
### https only; Cancel joins), NetDirectoryClient (register/heartbeat/delete/list state machine; 5-60 s backoff; 429
### retry_after; one re-register on a heartbeat 404; unlist-during-register deletes the row it just made; Shutdown
### bounded at 2 s; browse instances never register), NetDirectorySignalChannel (one request in flight; posts before
### polls; cursor advances only past a signal the sink took; 404/403 terminal; queue_full backs posts off; join nonce
### from std::random_device — non-sim entropy), SettingsMan (three keys; install key minted from non-sim entropy on
### first directory use) — VERDICT: sound except F5 above. No other findings.
  F5 evidence check: W74v's pinned proof used a cert made with -subj "/CN=127.0.0.1" served at 127.0.0.1, so the
  name matched and the CN-mismatch shape (the README's CN=cortex-directory by IP) was never exercised on Windows.

### GnsSignaling.cpp/.h (new), GnsTransport.cpp/.h (+287, +38) — VERDICT: sound
GnsDirectorySignaling: two holders (GNS until Release, the dispatcher until Detach), outbox under a mutex, frames
posted by the dispatcher's pump on the game thread; duplicate copies only under a test flag. RecvContext: admission
callback refuses with CloseConnection + null, else adopts a signaling object for the host side; GNS's own rejection
blob is replaced by a refusal frame (it lacks from_identity). Dispatcher: rendezvous frames go to ReceiveP2PSignal
(the joiner with no context, so it accepts no requests), refusal frames disconnect the joiner, polling armed only
during a rendezvous. GnsTransport: StartHostP2P / ConnectP2P (GNS owns the signaling object from the call on and
releases it on failure; every earlier failure path releases it here), ReceiveP2PSignal through an owning context so
callbacks route to the right transport, ResetIdentity refused while any connection or listen socket is live (the
process-wide identity is set once). The IP path is untouched. No findings.

### Source/Main.cpp (+793) — VERDICT: sound
Directory probes (-net-directory-probe: register/heartbeat/list(+joinable)/delete/list; -net-directory-signal-probe:
three joiner posts, host polls with the session token, two echoes, drains, delete, late joiner fails "session
gone"; -net-directory-list: one GET + a 2 s LAN window merged as the join screen renders it), -net-dedicated
(seatless e2e host; NoTeam clears the players but activates every roster team), named e2e spawns, e2e tick knobs
(pause/leave/end-round through the natural EndActivity path), the e2e tick counter moved before the round's stop
can break the loop, the preview-event-ledger selftest (one preview + one frame per tick; the press's sound and glow
must start by press+1 and reach the output once; counters balance), lpinv now also checks the sound-identity cursor
and the ledger arm count, menu-script wait_ms/wait_state <s>/wait_attempts/goto_main/dump_reconnect/assert_console/
assert_landing_empty, the resync wait screen with elapsed seconds, toasts on resync start/finish, the console no
longer forced open on a controller-sync failure (G2), any "-selftest" flag implies headless, the stall event poll
and the UI-probe arming wired at startup. The boolean flags follow the file's existing `++i; continue;` shape.
No findings.

### Source/Menus/MainMenuGUI.cpp/.h (+220, +26) — VERDICT: sound
§11 rejoin offer on entry (scan only while idle; the landing panel opens on an available offer or an active
recovery); Back keeps the service alive while a recovery runs; §9b "Apply to Substitute" reuses the last join
request and rides the same two buttons; the status line clears only its own sentence; the join list merges LAN
and NET rows (the local identity built once at the pinned dt and restored so browsing never changes a
single-player setting; a NET row without a local identity is not joinable; a non-joinable row is refused at
click with its reason); the host's share-address line resolved once per empty room; the announced input
delay on the local roster row. No findings.

### Selftests: NetLockstepSelfTest (+607), NetMatchSelfTest (+794) — VERDICT: valid oracles
Lockstep: AI waypoint adds cross the wire (the AI pass sends one AIOrder per call and both views apply the same
queue), the seatless relay host (empty frames + bindings command every tick; identical committed sets; a leave
keeps the gate right), the fenced peer waived at the parked tick (seat untouched on host and survivor, both waive
at frame 1, the resync fires at the boundary), resync only at the completed tick (both the direct and the
parked-wait arms), the hold-resolution drivers now assert the stop stays pending to the boundary. Match: dedicated
config validation + hash + lobby reserved bit, the healed-round planned end and the tick clock across a relaunch
(host/client/behind peer), rematch roster derivation and a four-peer rematch that closes 1,2,4 onto 1..3, the
dedicated three-peer lobby, banners named once, the pending session event surviving a teardown (drained, not
discarded; the waiver count carried). Each asserts the mechanism the production change names. No findings.

### tools/session_directory/session_directory.py (+921), README, plist — VERDICT: sound for the self-hosted
### scale; one LATENT finding (F6)
Standard-library service: install-key + per-IP rate limits (stricter wins), register/heartbeat/delete with a
constant-time token compare, signal queues capped per session (16 destinations, 1 MiB undrained, 256 per queue,
120 s idle drop, the host queue never dropped), the peer nonce as the joiner's bearer credential (redacted from
logs), TLS per connection with a handshake timeout, one pruner thread, Connection: close per response.
- F6 LATENT (scale): GET /v1/sessions returns every row (up to MAX_ROWS 4096, ~600 B each) while the engine's
  NetDirectoryCodec refuses a body over c_MaxBodyBytes (128 KiB). Past roughly 200 listed sessions the join list
  cannot decode the reply and shows "unreachable". Not reachable at today's scale; the fix is a server-side limit
  or page (`?limit=`, newest joinable first) mirrored by the client. Owner: item-8 roadmap; no lane now.
- LOW (memory): the rate limiter keeps a bucket per install key and per IP forever (emptied lists are kept), so
  a stream of fresh keys grows memory slowly; prune empty buckets in the sweep. Same owner.

### Selftests: GnsP2PSelfTest (+1229), NetDirectorySelfTest (+1478), NetIdentitySelfTest (+63),
### NetReconnectSessionSelfTest (+76), NetResyncRuntimeSelfTest (+23); tools: the six test_*.py scripts — VERDICT:
### valid oracles
GnsP2P: single-process connect over an in-memory signaling stub with a host-candidate route check (not relayed,
not a loopback pipe, the remote address is a candidate the peer sent), refusal (reject / close-on-accept) must end
the joiner's connect within 2 s, gather counts local candidates, drop rules re-send a rendezvous, two-process file
wire, the identity guard (a third/fourth transport refused while a connection is live and the first keeps
exchanging), and the directory-routed dir-host/dir-join/dir-reject/dir-dup with the dispatcher's counters and the
signaling-object tally (created = released = deleted). NetDirectory: codec round trips, every required field,
wrong types, oversize, the compatibility predicate naming the field, the canned sequence, HTTP client reuse /
cancel (silent listener) / 200+200 stress, the client state machine on a scripted transport (interval, 404
re-register once, 429 retry_after, 5/10/20 s backoff), MergeGameLists reasons, the signal channel (ordering,
cursor held by a refusing sink, 404/403 terminal, queue_full post backoff with polls continuing, 429 holds every
request, transport backoff ladder restarting after a success, the 64 KiB cap both ways, posts before polls,
nonce and credential headers, malformed session id/token refused at configure, drain), the lazy install key
(load leaves the file unchanged; first use writes it). NetIdentity: the Lua state count out of the identity
while ai_update_interval and session_rules still move it. NetReconnectSession: Missing offer text, a cancelled
or spent schedule not re-armed, the panel title from the round's hold. NetResyncRuntime: the boundary refusal
reason. Tools: hold-panel probe (the F6 title during and after the hold, frames from the stall pump), lobby
input delay announced on both peers, reconnect menu recovery (attempts climb on the main screen), startup offer
(offer on entry, dismiss clears, no-record text), substitute application (applicant registered and on the panel).
No findings.

### Lane branches accepted after a full read (merged into stage2/fixgroup-6-lead)
- http-pin-identity (item7-ux 865a739b45, W117): the pinned-mode flags (unknown CA + CN + dates), the README
  pin recipe with a SAN, and probe_pin_identity.py (correct / wrong / unpinned through make_run). Red on the
  470b35200e exe with the correct pin (certificate verification failed, no request in the service log), green
  on the fixed exe (five 200s), wrong pin still "certificate pin mismatch", unpinned still refused. Accepted.
- audit-3-harness (item8-discovery 9027d7e874, tools only): compare_sim_traces too-few-ticks wording +
  test; launched_exe.py (result headers from launch.json, header/launch mismatch refuses PASS); run_selftests.py
  (PASS-token scoring: exit 0 with zero tokens is FAIL). Lane-copy driver edits live outside the repo. Accepted.
- audit-fixes-1 (item7-chat 367177f0a0): the three added-actor index erases + detecting arms (red
  added_remove=0/absorb_delete=0/discard_added=0 -> green); drains before Complete/FinishMatch/LeaveMatch + the
  fenced-disconnect selftest (red fenced_disconnects=0 -> green); Cancel closes the WinHTTP handle with no lock
  held (lock order re-derived: finish still waits for HANDLE_CLOSING before freeing; Cancel never touches the
  state after releasing the locks); module.root out of the module-manifest hash (red -> green; content still
  hashed); directory codec caps (peer 64 = the service's cap, rows 4096, signals 256, base64 padding only at the
  end; red -> green); GNS incoming handles registered at Connecting; tool ports moved to 4761x and the service
  default port 0. One doc miss: README line 25 still says the default port is 8443 (the plist and the Windows
  task pass --port 8443 explicitly, so the deployments are unaffected) — lead fixes the line.


## Lane acceptances after the closing (19:32 UTC 2026-09-12; every diff line read by the lead before the merge)

### W102-fixtures (fencing-warm 048899dbe8; Main.cpp +116, tools/check_switch_control.py +146) — MERGED 1c49db6b5e
Stimulus `-net-match-e2e-switch-control <tick>` (client switches onto the lowest-uid non-brain CPU actor, hands back at
+10), owner_log report field (tick, uid, owner, mode) sampled from the switched actor, and the checker (owner_log
byte-identical on both peers, the takeover window owner=client/mode=PLAYER, after hand-back owner=host/mode=AI, traces
strict-identical). Arm 1 PASS on the candidate and RED on the milestone (no stimulus); arm 2 drop3 PASS with the
returner's 518 owner_log rows matching the survivors.
- FIXED by the lead at merge (8c17ee2056): NoteE2eSwitchOwnerLog was gated on lockstep alone, so a live match would scan
  every roster each tick and grow the log without bound after any switch; now gated on the e2e flag too.
- Checked, no finding: the drop-frame row (tick 83 owner=host mode=PLAYER) is the purge of the gone peer's claim
  (PurgeLockstepControlOverridesForGonePeers) with the wire mode untouched; sampling follows the local SEAT mode
  (Controller::Update reads m_SeatMode), so the host's AI drives it during the hold and the returner's reseat restores the
  claim. That reading exposed F7 below.

### F7 (NEW, MEDIUM, latent): a claimed CPU actor is orphaned when its claimant's seat expires
After the purge the borrowed actor keeps wire mode CIM_PLAYER, so the ownership policy classifies it as the leaver's
human unit (cpuControlled = !IsPlayerControlled()). While the seat is held the relay host owns it (H4 §4); when the hold
EXPIRES with no survivor ResolveActorOwner returns 0, IsLockstepActorOwnerGone says gone and MovableMan disables it for
the rest of the round, while its unclaimed CPU teammates play on. Fix design (dispatched as W119, red-first): the dropped
overrides map names exactly the claimed actors of gone peers; at expiry an entry whose seeded owner is alive hands the
actor back in AI mode (the release form of the handoff) instead of standing it down, on every peer at the same frame.

### W116 (alias-walk 6531152809; Main.cpp +4) — MERGED 030e638061
Root cause of the bare-flag hang: with no -net-replay/-scenario the engine enters RunMenuLoop and never quits. Fix: refuse
the bare flag before LoadAllDataModules (RED 180 s timeout -> GREEN exit 1 in 7.8 s; the driver path unchanged).
ScenarioRunner::ParseArgs sets s_Active inside the same loop, so a -scenario run is not refused. Complements the lead's
missing-fixture FAIL line (4154eb8570).

### W113-2 (item8-directory a38e9593a6; service +34, test +68, channel +18, selftest +51) — MERGED 30b6d14cc5
Service long-poll (Condition on the store lock, notify on post/delete, deadline <= 25 s, counted once by the limiter) and
the channel's SetPollWait clamped to 12 s under the 15 s HTTP total timeout, re-poll at once after a 200, the drain poll
never held. Red/green both sides. Decision 3 (engine wiring) not started: dispatched as W123 (Opus) with the lead-fixed
design; the settings/flags/GUI pieces follow.

### W89-2 (item4-feel acef68efe1 on the milestone; 12 files +694/-15) — MERGED 6afcc347ba (LuaMan.cpp append conflict, keep-both)
The W89 crash root-caused (Graph.deserialize = a full-VM restore on the LIVE state, run for every preview; after tens of
restores the VM faulted) and replaced by a table/Vector deep clone into a hold table; the shadow self slot
(`_ScriptedObjects["<uid>#preview"]`), edge hooks only (OnFire/OnStride/OnReload/OnAttach/OnDetach/OnCollide*), clones
registered by pointer (RegisterMO/ForgetDestroyedRegisteredMO are pointer-keyed, so the original is untouched), query
APIs redirected to shadows (ViewIfSpeculating), roster walks from a preview hook reported as violations. lpinv 8/8 at
ticks 100 and 143, event-selftest PASS at D=7, cost 3.70 ms/preview.
- GAP (dispatched as W122, red-first): PushPreviewClone copies non-Vector userdata by reference, so an entity reference a
  script held in self before the preview still names the CANONICAL object; a hook writing through it escapes the shadow
  overlay. The lpinv oracle is the detector; the fix (map MovableObject userdata through the speculative view at copy
  time, or refuse such selves) waits for W122's census of vanilla scripts.
- CHECK (W122): one uid drift (spawn_child 1049513 vs 1049517) on the repaired default path in a -scenario run that arms
  no preview: run-to-run nondeterminism or a flag-dependent allocation; evidence first.

### W97 + W97-2 (h4-secondary stage2/port-map, 8 commits from 8a5ced34cc, 12 files +2849/-6) — MERGED be4ad7f294 (Main.cpp keep-both)
The whole branch was never merged, so the lead read all of it: NAT-PMP -> PCP -> UPnP chain on a worker thread behind
the NetPortMapWan seam, /24 gates on the SSDP location and (W97-2) the control URL, Content-Length-aware HTTP reader
(reads to close under a 256 KiB cap), lease renewal at the half-life, Release deletes the last MAPPED result, a second
worker start refused, the directory row leads with the public endpoint and join_mode "either", the observed-ip wrap and
the report keys only when mapping is on, NetworkPortMapEnable default off with a non-persisted CLI override, loopback
fakes + e2e driver launching through make_run. Selftest PASS (11 arms), match/session/directory selftests PASS.
- LOW (item 8 roadmap): a failed renewal is not retried before the lease lapses (Update sets m_RenewAtMs = UINT64_MAX).
- LOW (item 8 roadmap): the /24 gate refuses a gateway on a larger LAN for UPnP (NAT-PMP/PCP use the default gateway and
  are unaffected); the -net-port-map-igd override is a test seam, not a setting.
- Item 7 roadmap: no GUI toggle for NetworkPortMapEnable yet.


## Findings after the lane acceptances (19:52 UTC 2026-09-12)

### F8 (NEW, HIGH): a preview hook writing through an entity reference held in self reaches the canonical world
Proven by W122 on W89-2's tip: a test script that captures the firearm's parent at Create and writes `heldRef.Vel`
in OnFire moves the CANONICAL arm's vel after discarded previews (lpinv tick 153 depth 12: 6/8; dump token vel of uid
1048624; shadows=0, i.e. the write never entered the overlay); the control fixture without the write is 8/8. Six
vanilla Base.rte scripts store such references in self (Grapple parent/target, RemoteExplosive target, AADrone
SAM_Target, BrainVsBrain CPUBrain) and NativeHumanAI holds self.Target. Under prediction this is a local-only
mutation of the sim, i.e. a desync hazard that came in with W89-2's unfrozen edge hooks (before W89-2 every hook was
frozen). Disposition: W127 implements the lead-fixed design (when the preview clone binds, after BeginSpeculation,
remap every MovableObject userdata reachable from the copied self through MovableMan::ViewIfSpeculating, the original
mapping to its clone; a self holding any other userdata keeps the clone FROZEN, counted; cost bounded at ~1 ms per
preview on pickup_fire D=7; the same replay twice byte-identical). The candidate is not promoted until W127 is green.
The uid drift W89-2 reported is run-to-run and enters with the sound_ai_deferral fixture: the plain ActorStress
scenario at one Lua state is byte-identical across two runs on the candidate (D:/mx/lead-fg6/det-as1-1 vs -2); W128
measured the fixture path with tick hashes: six runs (three at one Lua state, three at four) on the candidate exe are
pairwise byte-identical over 280 ticks with uid 1049494 every time; W122's single outlier was on its own relinked exe
without tick hashes and is not reproduced on the candidate. Closed as not reproducible; W127 reports its own uid lines.

### F9 (NEW, MEDIUM, fixed): the candidate did not build on macOS
W-MAC-FG6's clean clone stopped at clang 17: NetPortMap.h:128 built the Request default argument from the nested
Options struct with default member initializers (MSVC accepts it), and W89-2's PreviewScriptSelfTest.cpp was listed
for MSBuild only (not in Source/Managers/meson.build). Lead fix 229f12826c (a two-argument overload instead of the
default; the meson entry); the Windows binary is byte-identical before and after (sha 84e0285c8a81), so the battery on
the candidate stays valid; the branch re-pushed and the Mac re-run (W-MAC-FG6B) started with the pickup_fire fixtures
shipped to the lane. Rule for every merge from now on: new .cpp files must be in meson.build; no default arguments of
nested-class type in headers.

### F10 (NEW, MEDIUM, from the W71-2b read; 20:19 UTC): AI-generated queue ops are gated by team command authority, not by ownership
MovableMan::ApplyLockstepGameCommands accepts a NetGameAIOrder only from the team's command authority (a human on the
team, else the host), and Actor::UpdateMovePath sends the native PopWaypoint only when the sender is BOTH the actor's
owner and that team's command sender. Under host-cpu-remote-human a CPU actor on a human's team is owned (its AI run)
by the host while the team authority is the human, so the owner's AI outputs for it are rejected at apply and its pop
is never sent: the queue is never consumed (W71-2b's craft_cargo pulse 316-640 on both peers instead of 316-320).
Controller-sync makes the OWNER's AI decisions authoritative, so the authorization for AI-generated queue ops must be
ownership of the written actor (or of the writing actor for W71-2b's cross-actor writes), with team authority kept for
human-issued commands. Disposition: W71-3 (writerUID on NetGameAIOrder under codec v21 with the v20 decode kept for
replays, owner-gated producers, the widened apply gate, red arms in NetLockstepSelfTest, the craft_cargo pulse as the
two-process red). W71-2b stays unmerged until W71-3 lands; both merge in the next wave.

### F11 (NEW, verification gap; 20:21 UTC): the audio checkpoint identity cursor differs between platforms
W-MAC-LPINV reproduced W90's AK-47 lpinv reds on arm64 with the identical +2-per-preview shape, but the cursor's
absolute value at the same replay tick is 9200 on arm64 against 9212 on Windows: the two platforms allocate a
different number of sound identities before the press (a platform- or headless-dependent set of containers at
startup). If the identity a container takes from that cursor is what NetGameSoundOp / the sound observations name
across peers, a Windows host and a Mac client would disagree about which container an op names. No Windows-host /
Mac-client lockstep match exists in the records (every Mac proof is Mac-vs-Mac). Disposition: (1) read where the
cursor is seeded and whether a match start or a world-structure load resets or transfers it (if the snapshot/world
structure carries it and match start seeds it, a cross-platform match agrees by construction); (2) a lead-run
Windows-host / Mac-client lockstep e2e once the engine is quiet (the first cross-platform two-peer proof), with the
sound observations and 600-tick traces compared.

F11 UPGRADED to HIGH (20:24 UTC) after the lead read the seams: ScenarioRunner pins the MO unique-ID counter at every
deterministic match start precisely because uids cross the wire and "each process has created a different number of
MOs by launch time" (ScenarioRunner.cpp ~1848), but the sound checkpoint identity cursor
(AudioMan::AllocateCheckpointSoundContainerID, a process-global monotonic counter) has no such pin, and sound identities
cross the wire in NetGameSoundOp (MovableMan.cpp ~4391, AudioMan.cpp ~2732; the receiver resolves them with
FindSimulationSoundContainer). Two peers whose processes created a different number of containers before the match (a
rematch after a played round, a player who ran a skirmish before joining, or Windows vs arm64 as measured: 9212 vs 9200
at the same tick) hand out different identities for the same match-time container, so sound ops name the wrong
container or none. Every existing two-peer test starts two fresh identical processes, which is why none sees it.
Disposition: W131 (pin the cursor to 1 << 40 beside the uid pin; unit arm; a fixture flag that pre-allocates identities
on the host only makes the two-process red; the resync path keeps restoring the cursor from the snapshot).

### W104b (item8-dedicated stage2/preview-projectiles 3b8035ddc9; 7 files +317/-9) — READ 20:26 UTC, accepted for the next wave
Speculative spawns harvested per horizon step and travelled; a named spawn from a previewed emitter becomes a Projectile
ledger event and a severed presentation-only ghost adopted by the canonical particle at commit; red/green argv-identical
(first round visible at committed press+1, adopted once), lpinv 8/8, three-build compat byte-identical, fl100 at D=7
identical. Roadmap notes: the ghost is static until adoption (travelling it is the optimistic-projectiles follow-up);
the EventStart dedupe hides a same-tick casing adoption in the diagnostics only.


# Pre-overnight review, part A: the takeover window be217add64..d81478d2ee (2026-09-09 14:31 .. 09-11 17:30) — started 20:40 UTC 2026-09-12
Asked by the user at 20:36 UTC ("are you able to also review changes made before overnight"). 103 commits; Source 99
files +19697/-2211; tools 15 files +1276/-23. Provenance: the Cursor/Codex leads with the Grok High worker generation
(W12-W38). Two of today's findings (F10, F11) have roots here. Method as for the overnight delta: `git diff
be217add64~1 d81478d2ee -- <file>` read in full, production first in risk order, then selftests and tools; findings
fixed by the lead or dispatched red-first. Part B (the P4B phase before the takeover, 538 commits, 321 files +55756)
follows with the determinism/network core read by the lead and the periphery audited by Grok Extra High lanes.

F7 CORRECTED (20:44 UTC, from W119's evidence): the ownership policy consults the seeded owner before the team's human
(NetActorOwnership::ResolveOwnerPeer, ~:47), so after the purge a claimed CPU actor resolves to its seeded owner (the
host), is never owner 0 and is never disabled; the only residue was the wire mode staying PLAYER for one tick until the
owner's next AI frame applied AI mode. F7 is LOW, not MEDIUM. W119's explicit hand-back at expiry (with its line, unit
arm and the expire3 gate) still lands: it removes the one-tick lag and documents the transition.

### W123 (item8-directory stage2/s4-wiring 86b4f82d6c; 17 files +1391/-11) — READ 20:46 UTC, accepted for the next wave
NetMuxTransport (IP + ICE halves, high-bit tag, posted tasks + pump in PollEvents), settings/flags with run-only
overrides, session-id join through NetSessionConfig, MergeGameLists accepting ice/either rows, host identity pinned
from the session id before any listen, reports. Six selftest arms + a true behaviour red; loopback e2e green over the
IP half. Roadmap: ICE-only connect proof (Mac), rematch/resync over ICE, the teardown order, the worker-thread
NetDirectoryClient static check, TURN.

### Pre-A: Source/Network/NetLockstep.cpp (+1847 net, 2653 diff lines) — READ 20:50 UTC — VERDICT: sound
Codec additions (value observations as bit patterns so non-finite numbers ride the wire; player bindings; seat
snapshots with a strict advance rule; recovery chunks on the reliable lane assembled with offset/total checks and a
canonical re-encode check on the assembled input; every list capped) all validate before use; a conflicting retry or
out-of-order chunk fails the round with ProtocolError only for a KNOWN bound sender (SenderOwnsTransport now refuses
departed seats' transports). Round readoption rebuilds own and installed inputs through the recovery pipeline;
resends ride it. Congestion: a peer whose sends the transport refuses as congested is HELD (its silence is our queue's
doing: AdjudicateSilentPeers skips it) until CongestionHoldLeaveMs, then evicted; a relay backlog past the skew window
evicts at once (a reliable-lane hole can never be refilled). Hold pause: commits stop while any dropped seat is held,
hold heartbeats keep the link, resolutions (Reclaimed/Substituted/Expired) are accepted only from the round authority;
Expired with nobody refilling ends the round. An evicted seat's transport is closed on the relay host. Test seams
(CC_TEST_LOCKSTEP_HOLD_BEFORE_TARGET/OBSERVE_TARGET) are env-gated statics on the production path: acceptable, noted.
No new finding; F3 (frame lane must stay ordered) is the standing constraint.

### Pre-A: Source/System/ScenarioRunner.cpp (+533, 708 diff lines) — READ 20:50 UTC — VERDICT: sound; F10's enqueue-side origin confirmed
Local command sequences and outbox with authoritative acks (host checksums carry applied sequences), the resync state
capture/restore with strict consistency checks (conflicting inputs, commands or bindings fail the restore rather than
guess), resync priming of the delay window, the hold-pause wait (the give-up clock restarts after a hold), the drop
overrides moved to a dropped map at the purge. EnqueueLocalGameCommand gates every non-Reseat command on
IsLockstepTeamCommandSender at ENQUEUE (the second half of F10: an owner-run AI's order for an actor on a human's team
is dropped before it is sent, so W71-3 must widen this gate too, not only the apply gate; its pulse red will show it).
ResolveResyncDropFrame here still allowed a rewind; the overnight W62 change refuses it (read earlier).

### Pre-A: Source/Managers/AudioMan.cpp (+518, 638 diff lines) — READ 20:50 UTC — VERDICT: sound; F11's origin and today's UAF site confirmed
Restore-play diagnostics (a play during a snapshot restore prints its phase and an 8-frame backtrace; counted by the
selftest), carried-owner containment (a voice whose owner the save does not carry is disowned; manager-owned identities
collected from the GUI sounds and the music checkpoint: the CheckpointReader-over-a-temporary use-after-free fixed today
was introduced here), restored-registry scopes resolving voice owners to the restored copies, six new selftest arms.
The identity cursor is only ever raised or restored from a checkpoint: no pin at match start (F11, W131).

### Pre-A: Source/Network/NetLockstep.h (490 diff lines) — READ 20:57 UTC — VERDICT: sound
The declarations behind the NetLockstep.cpp verdict: stop reasons 9-12 (PeerDropped and the three hold
resolutions), the seat-presence snapshot payload, value observations, recovery chunks, applied-command acks
on checksums, codec version 15 -> 20 with the per-feature version constants, the recovery/resync/hold API.
Nothing declared here is unused or used differently from what the .cpp does.

### Pre-A: Source/Network/NetMatchService.cpp (938 diff lines) — READ 20:57 UTC — VERDICT: sound
Match-over rejoins answered with a SessionEnded disconnect instead of a resync; the host's resync keeps the
session keepalive ticking from a jthread while the snapshot saves (mutex only, no lock-order hazard: the game
thread holds nothing of the service's during the save); the healed round resumes at the first unapplied frame
(the pre-overnight rewind branch is gone since W62); the resync state rides an envelope both host and clients
decode through one path (PrepareReceivedResync: round-tag checks refuse a stale or same-round snapshot); peers
that were in the round restore their captured local player state, a rejoiner applies the newest wire
bindings; the leave exchange loop is bounded by the client's own Leaving deadline
(NetReconnectSession.cpp:1856, c_LeaveAckBudgetMs) with 5 ms sleeps; seat presence pumped once per session
pump, hold resolutions forwarded to the coordinator, moderation view published as authenticated snapshots;
the match-over answer runs after the lock is released (AnswerMatchOverRejoin takes it itself). The
join-wait file trigger and the A7 journal hooks are test instrumentation, off by default.

### Pre-A: Source/Managers/MovableMan.cpp (280 diff lines) — READ 20:57 UTC — VERDICT: sound
Player-binding commands observed, not applied as economy; ConsumeLockstepGameCommand dedupes sequences before
apply; value observations committed on running and paused ticks, rejected unless the sender is the object's
root actor's owner (host for unowned/unknown), applied in (sender, uid, ordinal) order: the ordinal is a
per-object monotonic counter assigned by the SENDER (MovableObject.cpp:1538, m_ValueWriteOrdinal), so the
order within one object is the write order and objects do not interact during the commit. The set-aside
world is now tracked by pointer with a destructor discard, so a refused reinstate can no longer leak a held
world. A7 journal emission is gated off by default.

### Pre-A: Source/Entities/Activity.cpp (276 diff lines) — READ 20:57 UTC — VERDICT: sound
Snapshot-time actor switches play no UI sound (the restore must not consume sound identities); the net
player bindings capture/apply (range-checked before any write, screens reassigned in player order, dead or
unknown actors resolve to null, never substituted); the local player state capture/restore for peers that
were in the round (controllers, timers, per-actor local input, camera) with a self-test covering the
inventory-carried brain, shared authority preserved, wire state preserved; Activity checkpoint v3 carries
the team icons with the legacy v1 prefix still read.

### Pre-A: Source/Activities/GameActivity.cpp (537 diff lines) — READ 20:57 UTC — VERDICT: sound
Network panel open disables the buy/inventory menus and untraps the mouse; the editor's retained owners are
visited as checkpoint-owned; the local game UI state (menus, banners, strategic pie, purchase overrides)
captured as one checkpoint and restored through RestoreNetLocalMenu (existing initialized menus load in
place, missing ones are constructed from the checkpoint, a failed candidate is deleted, the count field is
bounded by the text size); a joiner gets fresh menus through CreateNetLocalUI. The alias self-test is an
oracle for the retained-owner/private-publication path, not engine logic.

### Pre-A: Source/Managers/ActivityMan.cpp (266 diff lines) — READ 21:02 UTC — VERDICT: sound
SaveCurrentGame no longer perturbs the sim's sound identity cursor: the scene clone runs inside a registry
scope whose destructor restores the registry and the cursor, the Lua graphs and the scene serialize into
side blocks, the cursor is reset to the live value before the runtime globals are captured, and the AudioMan
checkpoint keeps only identities the world or the managers carry (a voice nobody carries is disowned, F11's
mechanism area). A restore raises the cursor over every registered identity, so every peer that restores
the same snapshot lands on the same cursor. The before/after restore callbacks (the resync local-state path)
run with the restoring flag set and any exception marks the restore failed.

### Pre-A: Source/Entities/MovableObject.cpp (403 diff lines) — READ 21:02 UTC — VERDICT: sound
Local-AI value writes (number/string maps) go to a per-object overlay plus a pending op list instead of the
shared map; reads in the local-AI domain see the overlay, shared-domain reads see the committed map on every
peer alike; the shared map changes only at commit (the wire observation on every peer, or the offline
commit at the end of UpdateControllers). The overlay entry is dropped only by the op with its ordinal, so an
older commit never hides a newer local write. PublishNetPrivateObjectGraph assigns identities to a private
graph with a CAS on the counter and re-keys the Lua script objects before the counter moves. Self-test
covers read-after-write, uncommitted isolation, offline commit and remove propagation.

### Pre-A: Source/Network/NetReconnectSession.cpp (477 diff lines) — READ 21:02 UTC — VERDICT: sound
Configure resets every admission table when the epoch or session id changes (a rematch keeps them); a seat
that already exists keeps its whole state across a seat-table refresh; reclaim/substitution/expiry queue the
hold resolutions the coordinator applies (Reclaimed/Substituted/Expired); the moderation view carries the
epoch, generations, incarnation and transaction ids so a stale selection is refused (NetModerationAvailability
re-validated against the live view after a Tick inside ApplyModeration); reseat accounting distinguishes a
missing ledger from no survivors. The ack-drop/ack-duplicate/commit-drop/drop_leave_ack faults are gate-only.

### Pre-A: Source/Network/NetResyncState.cpp (new, 309 lines) — READ 21:02 UTC — VERDICT: sound
The resync envelope codec: every count is bounded by the remaining metadata, owners must be strictly
ascending, bindings and applied acks one per peer in peer order, pending inputs LZ4-packed with the raw size
checked against the recovery cap and the decompressed length, pending commands either inline or by reference
into the pending inputs (re-encoded canonically before a reference is trusted), sequences must exceed the
applied ack and frames must not regress with the sequence, reseats sit at savedTick, trailing metadata bytes
refuse, and Decode requires the session id and savedTick+1 == the healed round's start frame.

### Pre-A: Source/Network/NetSession.cpp (234 diff lines) — READ 21:04 UTC — VERDICT: sound
Timeout resumption: when the timeout check itself was silent for longer than the budget (the round owned
the transport for the whole match) the receive windows restart once, and only once per silence (any packet
re-arms the restart), so a resync round's first tick or the leave exchange no longer measures the whole
match and evicts everyone; handshake expiry stays on the connection's own age. The silent-client and
never-says-hello faults and the A7 journal lines are gate instrumentation, off by default.

### Pre-A: Source/Managers/LuaMan.cpp (695 diff lines) — READ 21:04 UTC — VERDICT: sound (oracle and alias machinery)
Script-graph owner references gain a class-only form and the editor's retained owners, actor controllers
and owned movable parts as addressable owners, with the MovableObject vector properties (Pos/Vel/Prev*,
recoil, joint offsets) walked as aliases; HasNativeAliases is a full reachability walk (weak tables and
integer registry slots excluded, upvalues, thread stacks, fenv and metatables included); RekeyScriptObjects
validates every move before touching _ScriptedObjects (used by PublishNetPrivateObjectGraph under the state
locks). The craft-exit, team-icon and set-aside-sweep arms are self-tests.

### Pre-A: Source/Main.cpp (573 diff lines) — READ 21:06 UTC — VERDICT: sound
Headless moderation driver for the e2e (-net-match-e2e-moderate*, through the panel's own action path),
the -net-h4-fault arm, the join-wait trigger, menu-script moderate/assert_control, the resync wait now
pumps SDL and draws the resync screen with the F6 seat panel, a match-over rejoin answer ends the activity
(quit in the e2e, menu otherwise), the e2e tick clock is one object (NetMatchE2ETickClock) so a resync
relaunch and a rematch re-anchor identically, the rollback probe and the contract audit settle the Lua heap
before every capture (with a self-test that a dropped root never survives into a capture), and ShutDown
discards a refused reinstate instead of leaving the world held. A7 journal calls are gated.

### Pre-A: Source/Network/NetLobbySession.cpp (222 diff lines) — READ 21:06 UTC — VERDICT: sound
The lobby state transfer streams chunks from the byte buffer instead of a pre-built chunk list, tracks the
per-peer send of the current chunk, restarts with a strictly larger transfer id when a peer joins late; the
receiver takes chunks only in order, refuses a changed header or a differing duplicate, grows its buffer by
at most the announced total, and a Start that arrives before the transfer completes fails the lobby.
Chunks are accepted only from the host on the reliable lane. The session clock comes from the service's one
clock when provided (the F1.5 divergence fix).

### Pre-A: NetMatchReplay.cpp (227), NetReconnectUx.cpp (235), Controller.cpp (74), GnsTransport.cpp (120), LoopbackTransport.cpp (74), NetGameCommand.h (54), LuaThreadCodec.cpp (47) — READ 21:08 UTC — VERDICT: all sound
Replay records carry value observations and split a tick with several senders' bindings into per-sender
packets that the reader concatenates (packet sizes read from the 16-byte header, bounded by the record).
Moderation UX keeps the chosen applicant per seat by selection identity and only applies an action the
availability check accepts; seat presence applies snapshots in revision order and computes the hold's wall
seconds from the snapshot's own observation time and the local receipt time (no shared clock). Controller
local input state (seat, ignore flags, timers, cursor limits) captured/restored for the resync; the network
panel blocks player input. GNS reports a full queue as congestion (LimitExceeded) distinct from a dead peer,
with bytes-handed-over next to the pending figure. Loopback meters a send buffer with a drain rate for the
congestion gates. PlayerBindings is command 13 with a sender-local sequence on every command. The LuaJIT
thread-stack visitor skips frame and continuation slots and is used only by the alias oracle.

### Pre-A: headers (Controller.h, Activity.h, NetResyncState.h, ScenarioRunner.h, MovableMan.h, MovableObject.h, AudioMan.h, NetReconnectSession.h, NetMatchService.h) — READ 21:08 UTC — VERDICT: sound
Declarations match what the .cpp reads found; nothing declared is unused. NetResyncCodec's caps are derived
from the lockstep codec's (pending inputs bounded by peers x future skew, metadata by auxiliary + references
+ inputs, archive 64 MiB). The e2e tick clock's cap test reads the MATCH frame, not this process's running
ticks (a resync relaunch or a rematch cannot shorten the cap).

### Pre-A: Source/Network/NetA7Journal.cpp (new, 388 lines) — READ 21:08 UTC — VERDICT: sound (gate instrumentation, off by default)
Enabled only when the four CC_A7_* environment values are all present and consistent (run and peer tokens,
an exclusive fresh journal file, the executable's SHA-256 matching the launch identity, the fault set
matching the parsed faults); a lock-free 1024-slot queue drained by one writer thread with every overflow
counted as an evidence gap; Seal refuses (returns false) when any gap or write failure occurred so an A7
run cannot pass with a hole in its evidence. The connect gate waits at most 240 s on a validated JSON file.
Nothing here runs on a default launch.

Pre-A Source list complete: every file in the takeover window's Source delta has been read by the lead
(NetLockstep.cpp/.h, ScenarioRunner.cpp, AudioMan.cpp, NetMatchService.cpp, MovableMan.cpp, Activity.cpp,
GameActivity.cpp, ActivityMan.cpp, MovableObject.cpp, NetReconnectSession.cpp, NetResyncState.cpp,
NetSession.cpp, LuaMan.cpp, Main.cpp, NetLobbySession.cpp, NetMatchReplay.cpp, NetReconnectUx.cpp,
Controller.cpp, GnsTransport.cpp, LoopbackTransport.cpp, NetGameCommand.h, LuaThreadCodec.cpp,
NetA7Journal.cpp, nine headers). Remaining for part A: the selftest units and the tools delta.

### Pre-A: Source/Network/NetResyncSelfTest.cpp (new, 691 lines) — READ 21:10 UTC — VERDICT: sound, a strong oracle
Sixteen arms: binding round trips with bit-exact float checks (signed zero, subnormals, max float), command
capacity (256 + one binding accepted, 257 refused, wire-level count and duplicate-binding refusals at fixed
offsets), sequence edge values with wire varint overflow refusals, binding field limits (non-finite floats,
team, view, UID ranges) both at encode and by wire mutation, checksum and binding applied-command maps with
ordering refusals, literal v8/v17 byte fixtures independent of the encoder, envelope round trips at the
window boundary, refusal atomicity at EVERY truncation length with the decode outputs verified unchanged,
embedded-packet substitutions for future commands and bindings, the complete-input window with a fixture
larger than the old cap (LZ4 must shrink it), raw-input fallback and corrupt-compressed refusals.

### Pre-A: Source/Network/NetResyncRuntimeSelfTest.cpp (new, 1307 lines) — READ 21:12 UTC — VERDICT: sound, a strong oracle
Twenty-eight groups over two loopback coordinators (plus a three-peer star): asymmetric priming in every
delay combination with wrong-count and wrong-sender batches refused without sending, start-mode mismatch
fails the handshake, a lost start retransmits with its mode, a new authority round resends primed frames
once, the scenario lifetime distinguishes rematch (commands dropped) from same-session resync (kept) from a
new session or epoch (everything reset), binding and checksum ACK authority (an unbound transport is not a
sender, a non-host checksum grants nothing, a stale round is counted), snapshot capture with conflicting
targets refused atomically, deferred-stop capture resumes at the first unapplied frame, full inputs with
bit-exact controller/command/observation bytes, the exact refused recovery chunk resumed first, the local
history bound, a second heal before the sender resends, authoritative restore without a wire resend,
metadata and history conflicts refused with state/queues/transport verified unchanged (down to signed zero),
unsent intent never overtaking an accepted input, and repeated delayed restores keeping the watermarks.

### Pre-A: Source/Network/NetMatchSelfTest.cpp (1021 diff lines) — READ 21:14 UTC — VERDICT: sound
New arms: the join-wait trigger (times out naming the path, releases on a late file), a failed report still
carries the admission counters, a rejoin while the activity is Over answers match_over (from the classify
call and from inside the lockstep wait: the pump must NOT destroy the coordinator; the main loop finishes),
the e2e tick clock (executed ticks, survives a resync, resets on a rematch, the cap and the early-over test
read the MATCH frame), lobby state chunk bounds (2 GiB, 43691 chunks; twelve malformed cases refused at
encode and by wire mutation), eleven chunk-stream consistency cases refused with progress unchanged,
restart semantics (ids strictly increase, progress serial monotonic), per-destination backpressure resume,
and the runner's transfer deadline that resets on progress but times out a stall amid keepalives. Note:
TestRunnerStateTransferProgress reads the wall clock (150 ms lobby wait), so it is the one arm in this
unit that a heavily loaded machine could flake; it passed in the suite runs on record.

### Pre-A: Source/Network/NetReconnectSessionSelfTest.cpp (1431 diff lines) — READ 21:14 UTC — VERDICT: sound, a strong oracle
The unacknowledged leave settles exactly at its budget (not before, no retransmit past it, counted once as an
ambiguous loss; an acknowledged leave counts none); admission holds queue Expired/Reclaimed/Substituted;
the moderation view ages the drop on the plane's clock and never invents a frame hold; the seat-presence
line keeps the wall and frame hold clocks apart and refuses stale revisions; the selection model refuses a
pressed action whose seat, generation, incarnation, epoch or application transaction changed under it (a
vanished applicant never silently selects the next one); a failed proof ends the public "reconnecting"
state without granting anything; the five §9b failure windows (disconnect before ack, ack lost then late
ack refused from the cache, commit result lost then replayed identically once, a duplicate past the cache
window dropped silently, a substitute vanishing leaves a clean seat); the round resumption keeps its peers
(R1: the resync round forms after a 20 s match with zero timeouts and exactly one resumption; the control
evicts a quiet peer one budget later; the floor: a caller slower than the budget evicts on the second
evaluation; R2: an announced leave after the match is acknowledged, not adjudicated lost); the reseat
counters distinguish no-ledger, no-survivors and unnamed-survivor with the census asked exactly the
expected number of times; an empty off-tick second drop does not erase a good record; a refused reclaim
reports new_join_after_refusal.

## W126 read (item-7 wire: module digests, chat, protocol v2) — 21:20 UTC — VERDICT: ACCEPTED
Diff b81df66f48 on item7-chat (1798 lines, all read). NetIdentity: BuildModuleDigests sanitizes names (control
characters, the 64-byte cap), sorts and dedupes by file name, and cuts to the entry and byte caps with a
truncated flag; DiffModules is keyed by file name (an extra module no longer shifts every module after it,
proven against the index-paired Compare that still says module_order); DescribeModuleDiff is one joiner-facing
sentence (Install/Remove/Update with both versions) bounded to the diagnostic text cap with a UTF-8-safe cut,
proven to encode into a NetJoinRejected at the 80-module worst case. NetProtocol v2: ModuleDigestRequest,
ModuleDigests (sorted, unique, 256 entries, 24 KiB, refused on the header before parsing) and Chat (128
bytes, UTF-8 validated with overlong/surrogate/range refusals, scopes all/team); CanEncodeAtVersion(1) so a
v1 peer is told why it was refused in its own envelope, while v2-only types cannot be stamped at v1. NetSession:
a module-manifest refusal is decided at once and only PARKED to fetch the digests: the host's parked peer stays
in Handshake so ExpireSilentHandshakes bounds it and a silent peer is refused on the manifest (never a timeout);
the joiner mirrors with its own deadline and ignores everything but a rejection or disconnect while parked;
digests are sent once per connection; the sentence is always joiner-facing (host list = the diff's local side).
Twelve new selftest arms across identity/protocol/session cover round trips, caps, malformed wire, the silent
peer, the joiner-side mirror and the v1-stamped rejection. No sim-visible change (chat never enters a tick).
Note for the roadmap: chat has a wire payload and no UI or routing yet; a connection that has not passed
admission can request the host's module list once (bounded, diagnostic).

### Pre-A: Source/Network/NetLockstepSelfTest.cpp (+7888, 8711 diff lines, all 20 chunks read) — 21:20 UTC — VERDICT: sound, a strong oracle
Recovery wire refusals (23 mutation cases with live input, history, acks, dictionaries and terminal policy
verified unchanged, and a canary frame proving the live slot references survive), relay retry of the exact
refused chunk across 3 and 4 peers, membership (never-member refused, departed-accepted preserved), the
sender-side team-command gate (F10's enqueue half), completion reports, every packet kind from an unbound,
stranger or departed transport refused without refreshing liveness, the wedged/unreachable peer paths that
now hold until Expired and close the evicted connection, the activity gate agreeing across peers through a
full hold window, B2 seat snapshots (canonical bytes, 25 malformed cases, non-host authors and unbound
transports refused, 14 replay regressions, initial completeness, coalescing under refusal and congestion,
a resync seeding a new peer, a departed transport never reviving), hold pause commits nothing, Expired
resumes without the seat, Reclaimed/Substituted resync at the leave frame (2 and 3 peers) and the resync
round commits contiguously from the drop frame, heartbeats keep survivors unadjudicated through the pause,
the lockstep wait survives a hold (real-clock arms of 0.4 s and 10 s) and applies a parked resync, announced
leaves close at once, value observations (bit-exact doubles including NaN/inf/-0, v19 frames still decode,
relay at N+D, replay round trip, per-sender replay bindings, non-owner dropped, overflow carry), the round
start review cases (readopt clears the left peer and the deferred stop, stray-start rate, authority start
does not fail the round, owed frames survive refusals and end with the round), congestion holds every peer,
a dead link loses only its own seat. The runner also chains the two resync suites.
Pre-A selftest units complete. Remaining for part A: the tools delta (1632 lines).

### Pre-A: tools delta (15 files, 1632 diff lines, all read) — 21:21 UTC — VERDICT: sound
compare_snapshots maps the CONTROLLED actor (actor_links[player][1], the earlier [0] was the brain) with the
layout validated and Activity1-3 accepted; the transaction-control tools (manifest annotation that leaves the
cases untouched, refusal classification by the engine's own log stage, iterator-control inspection, a build
manifest pin, running a retained job against another tree, content-determinism of the control archives);
h4_gate_evidence tells an empty drop ledger from dead units with unit tests; the substitution gates gain
five panel-driven fault gates and the stayer's own roster line; snapshot_runtime learns Activity2/3 and the
icon records with cache-owner uniqueness; test_snapshot_inventory_roles refuses wrong owner/item/alias and
malformed layouts; test_lobby_lifecycle --leave-at-frame drops a peer mid-match on an input barrier the host
has acknowledged and requires every survivor's 180 ticks; the moderation panel's controls are asserted in
the skin; win32_test_runner refuses a -net launch whose executable has no inbound firewall allow rule (read
from the registry rule store, no elevation).

PRE-OVERNIGHT REVIEW PART A COMPLETE (21:21 UTC): the whole takeover window be217add64..d81478d2ee has
been read by the lead (Source 99 files, the selftest units, the tools delta). Findings: none new beyond
F10's enqueue half (recorded above for W71-3) and the four already-known artefacts. Part B (the older P4B
phase, p4a gate..be217add64) follows: the lead reads the determinism and network core; Grok Extra High
audit lanes cover the periphery with every finding re-derived by the lead.

### W90 (item4-feel stage2/preview-scripts acef68efe1..56b9e19f64; Main.cpp +27, five tools) — READ 21:25 UTC — VERDICT: accepted
The AK-47 Lua-fire ledger arm: at the press tick the equipped AK-47's uid is captured, and the first PREDICTED
Sound event start from that emitter must sit at committed tick <= press+1 (the arm only exists when the
equipped preset is the AK-47); tools: the two-peer recorder, the fl100 and replay runners through make_run,
a stdout quoter, and wait_engine_idle (zero engines and no LEAD lock, 2 h cap). Oracle and drivers only.

### W119 (fencing-warm stage2/claimed-actor-expiry be4ad7f294..0d5c318367; 7 files +347/-2) — READ 21:25 UTC — VERDICT: accepted (F7)
UpdateControllers: a dropped claim whose claimant is gone at the applied frame and whose seat is no longer
held (both facts synced: leave frames and hold resolutions are relayed) is erased and the actor handed back
to its seeded owner in AI mode when that owner is the resolved one, with one log line; otherwise the old
stand-down path. IsSeatHeldForReclaim(peer) reads the same set AnyLeftSeatHeld does. The selftest arm drives
a claim across three peers, drops the claimant, expires the hold and checks host and survivor agree (owner 1,
AI mode, not disabled); the expire3 oracle checks the owner logs of both peers after the claim plus the
identical returned line and the traces. Consistent with the F7 correction (LOW).

### W103 (item4-simspeed f83cc75099..3f27189784; 11 files +318/-15) — RE-READ 21:25 UTC before the W130 merge — VERDICT: accepted (as at 19:25)
Render substitutes registered in BeginRender and cleared in EndRender, honoured by IsActor/ValidMO and the two
DrawGUI guards; the inventory menu draws from the preview clone (its equipped items and CPU position) while
Update keeps reading the canonical actor; ClampPreviewTimers pins clone timers whose start passed the restored
tick; the five-arm HUD selftest samples before/during/after the render window.

## Merge wave + the merged candidate's first gates (21:26-21:50 UTC 2026-09-12)

Nine reviewed branches merged into stage2/fixgroup-6-lead (control-build) on 229f12826c, tip 5469f15f2a: W90 56b9e19f64,
W104b 3b8035ddc9, W119 0d5c318367, W125 d3fd36c60c, W123 86b4f82d6c, W126 b81df66f48, W120 6062f464bd (with F12), W130
b944ce84dc (with W103), W-MAC-RUNNER 586fcfce02. Every conflict resolved keep-both by script and each resolution checked
mechanically against the branch's own diff from its merge base (scratchpad check_merge.py: the +/- line multisets agree
exactly; the one deliberate difference is Main.cpp's headless-flag line = the union of both sides' flags). Trailer scan 0.
Rebuilt (Final, /MP6) in 114 s: exe 18ef7ed5820a. verify6 on it: port-map, directory, script-graph(4 states), the
global-callback driver, preview-event-d7 and lpinv-100 PASS; the socket-free suite 10/11 (F14 below); arm1 pending.

### F13 (NEW, MEDIUM, in W124's unmerged branch stage2/asan-relaunch-fg6 8efc5140c9; read 21:44 UTC): the relaunch
window never closes
W124's mechanism is right where it removes the UAF: Activity::ForgetDestroyedActor on every MovableMan delete path (actor,
item, particle and the settle walk at MovableMan.cpp:3965-3968, the ASan free site) and a re-resolution of the checkpoint
UIDs (RebindNonOwnedActorSlots through LiveCheckpointActor = FindObjectByUniqueID + ValidMO + IsActor) after
LoadWorldStructure, after DiscardWorld and at the end of MovableMan::Update. But ActivityMan::m_LockstepRelaunchInProgress
is set by NoteLockstepRelaunch (NetMatchService.cpp:580) and cleared only in ActivityMan::Clear(), and
m_HasCheckpointActorIDs / m_HasCheckpointMarkedActorIDs are kept while it is set: after ONE relaunch the end-of-Update
rebind runs for the rest of the process and re-imposes the snapshot's UIDs on m_Brain, m_ControlledActor, the player
controller's actor and m_pLastMarkedActor every tick. A later legitimate slot change on the relaunched peer (a switch of
control, a brain hand-off, a marked actor) is reverted at the end of its tick. W124's 20/20 drop-3 gates could not see it:
its tree has no switch-control stimulus, and W102's drop3 arm gives the stimulus to the peer that is about to drop, not to
the returner. m_LockstepRelaunchChecksLeft / ArmLockstepRelaunchChecks / ConsumeLockstepRelaunchCheck are dead remnants
of the 16-tick oracle window. Also noted, not a finding: the first ActivityMan::Update after the relaunch still sees the
scene-placed placeholder brain (alive until the same tick's MovableMan::Update deletes it); 20/20 trace-identical gates
say it does not diverge the sim, and the atomic form (flush the placeholders inside RestartActivityCandidate) is a larger
change. Disposition: W124-2 (Grok, hold-pause, lead-fixed design): the window ends at the end of the first
MovableMan::Update after the relaunch (EndLockstepRelaunch clears the flag and drops the kept IDs; ForgetDestroyedActor
stays in force always; the dead code goes); RED first with a returner-side switch after the relaunch (a
drop3_switch_returner arm: the stimulus on the returner's relaunch command line at tick 450), GREEN 10+10 arms under ASan
halt mode plus the selftests. W118's 6ecc65766b (the music checkpoint text kept alive while read) rides the same branch and
is sound. W124's branch is NOT merged until W124-2 lands and is read.

### F14 (NEW, LOW, test pin; fixed by the lead 21:50 UTC): the reclaim transcript golden hard-codes protocol version 1
NetReconnectSelfTest.cpp TestTranscriptLayout builds the reclaim transcript with protocolVersion = NetProtocol::c_Version
and compares the bytes against a hex golden whose version field is 0001; W126 bumped c_Version to 2, so on the merged
candidate the selftest fails with "canonical transcript bytes differ: ...5245434c41494d0002..." (verify6 suite 10/11).
Engine behaviour is correct: the reclaim proof domain binds the protocol version, so a v1 ticket cannot reclaim into a v2
session, which is what the v2 bump means. Fix: the golden's version bytes are derived from c_Version (little-endian, right
after the domain tag), so the test keeps checking the layout AND that the current version is bound. NEGATIVE, recorded:
W126's lane ran seven selftests (protocol, identity, session, match, lockstep, +2) and not -net-reconnect-selftest, and the
lead accepted W126 without running the full 11-selftest suite on its tip; from now on a lane that changes a wire or version
constant runs tools/run_selftests.py (11/11) and the lead's read checks that it did.

### W127 (value-observations stage2/preview-reference-writes acef68efe1..f1fb71f401; 689 diff lines read) — READ 22:19 UTC — VERDICT: accepted for the next wave (F8 closed for safety); F16 opened
Mechanism: at BeginPreviewScripts, after the clones are bound, every clone part's copied self (the preview hold
_ScriptFieldsStash["preview:<uid>"]) is walked (tables recursively with a seen set, keys and values) and every MovableObject
userdata is remapped: an original previewed actor to its clone (s_PreviewRootByUID), a live world object (resident, or a
part whose root is a resident) to the overlay's shadow through ViewIfSpeculating, a uid that is a part of the clone to that
part (s_PreviewPartByUID); any other MovableObject, and any non-MovableObject Entity, freezes the clone's hooks
(counted, printed once per uid, the preview script object dropped). Vector userdata pass through because
PushPreviewClone already copies Vectors by value (a fresh Vector(x, y)), so no live alias to an engine field survives
into the hold — the lead checked that path. Evidence re-derived: the write fixture (heldRef.Vel written in OnFire/OnStride)
goes 6/8 -> 8/8 at tick 153 with shadows=2, i.e. the write landed on overlay objects and never on the canonical arm; the
control fixture 8/8; the fallback fixture freezes once per uid; det1/det2 221 ticks identical; fl100 600 ticks
violations=0; the compat fixtures unchanged from W89-2. Verdict: writes through a self-held reference can no longer reach
the canonical world — F8's desync hazard is closed.
F16 (NEW, LOW-MEDIUM fidelity, not a desync): the branch order in RemapPreviewUserdata puts the live-world test before the
part map, and LiveWorldMO is true for an ATTACHED part of the original (its root is a resident), so a self-held reference
to the original's equipped gun or arm maps to a DUPLICATE shadow of the whole original (ShadowOf(root) -> the shadow's
part) instead of to the preview clone's own part; a preview hook's write to it then lands on a dead duplicate and the
stepped clone never sees it (the "shadows=2" of the green run is that duplicate). The part map is only reached for
detached parts (inventory items), which is why the worker saw the HDFirearm freeze first. Fix for the next lane: consult
s_PreviewPartByUID before the live-world branch (a uid that is a clone part is the original's part by construction), and
preferably register each preview clone as its original's overlay shadow (inWorld=false) so every speculating lookup of
the original resolves to the clone. Owner: lead or a W127-2 lane once a build slot is free; not a merge blocker.

### W131 (h4-secondary stage2/sound-identity-pin 229f12826c..f605a8330a; 3 files +88/-1 read) — READ 22:19 UTC — VERDICT: accepted for the next wave (F11 closed)
ScenarioRunner::ApplyDeterministicConfig pins the checkpoint sound identity cursor to 1<<40 beside the MO uid pin (1<<20)
and prints the old and new values; the unit arm builds two histories (17 extra containers before the match on one side,
none on the other) and proves the first match-time identity differs by 17 without the pin and agrees with it, and that
an encoded NetGameSoundOp resolves through FindSimulationSoundContainer on the other history; the
-selftest-preallocate-sound-identities flag gives the two-process form. Relaunch order checked by the lead: on every
match start and on the resync relaunch (NetMatchService.cpp:679, at staging time) the pin runs BEFORE the checkpoint
restore, and the restore's registrations raise the cursor over the restored identities, so no collision with live
containers (the worker's "…889 -> …776" line is the pre-staging cursor being pinned, then restored). Nits, not blockers:
the warning "already at or above the match base (impossible in practice)" fires on every rematch and relaunch (the
comment is wrong; the lead will drop the warning in a fix-up); the two-process miss-line RED was not demonstrated because
the AI-order e2e sends no sound ops (the unit arm and the identity delta are the proof).

### W71-3 (item5-lifecycle stage2/cross-actor-waypoints, W71-2b 4af9ef59e9 + 113ffa0b28/5c8095a78c/435924a207; 1105 diff lines read) — READ 22:19 UTC — VERDICT: the ownership gate is accepted; F10's last gap goes to W71-4
Accepted: NetGameAIOrder.writerUID under codec v21 (encoder appends, decoder reads at >= 21, v20 fixtures re-pinned,
round trip covered); SendDeferredWaypoints sends only when this peer owns the writer and names the writer when the
target differs; UpdateMovePath's pop is sent by the owner alone (the team-sender clause dropped); ApplyLockstepGameCommands
and EnqueueLocalGameCommand authorize an AIOrder by team authority OR sender-owns-target OR writerUID naming a same-team
actor the sender owns (IsLockstepAIOrderAuthorized), every other command keeps the team-sender rule; the three red arms
(host-run CPU actor on a human team pops; an owned writer may write another owner's actor; a stranger may not) plus
W71-2b's read-through and cross-actor arms; AIWriteScript gains scene-waypoint-for <uid|slot=N> (slot order = ascending
uid, deterministic on every peer). The gameplay pack (17 checks), fl100, heal, match, script-graph and pickup_fire are
green on its exe; its arm1 red is the missing switch-control stimulus on its own tree (not a finding).
Not closed: the craft_cargo pulse stays [316..640] on both peers. Root cause from W71-3's own trace, read by the lead:
the owner's AI pass requested the path (UpdateMovePath at tick 315, queue empty) while its waypoint add was still in
flight over the wire (applied at 316); the request was consumed against the empty queue and never re-armed, so the front
waypoint is never loaded and never popped. Single player lands the add and the request in one tick (pop at 318).
Disposition: W71-4 (lead design): under lockstep UpdateMovePath keeps m_UpdateMovePath armed while this actor has
in-flight writes and the physical queue has nothing to load; the apply of the in-flight add then makes the next
PreControllerUpdate load it and the owner sends the pop as today. Only the sending owner ever has in-flight entries, so
non-owners and single player are untouched. RED = the unit arm + the measured pulse; GREEN = the pulse closes near 320.

# Pre-overnight review, part B: the P4B phase db9ec184be..be217add64 (542 commits, 2026-07-02 .. 2026-09-09; 394 files, +65419/-2327) — started 22:23 UTC 2026-09-12
Split: the lead reads Source/Network (13.7k diff lines), the network selftests (11.5k), Source/Managers (17.3k), Source/System (7.9k)
and Main.cpp (3.1k) itself, in risk order; five read-only Grok audit lanes (pb-audit-1..5: Entities; tools; GUI/Menus/build +
Activities/Lua/Data; and two second readers ranking the network and manager/system hunks by risk for the lead) run beside it.
Every audit finding is re-derived by the lead before it counts.

### Part B: Source/Network/NetLockstep.cpp (2607 diff lines, all read) — 22:23 UTC — VERDICT: sound
Codec: canonical LEB128 varints (non-canonical spellings refused, including a trailing zero byte and a 10th byte past bit 63);
the sound-observation slot dictionary (LRU reuse, bindings staged during a block and applied only after the whole block reads,
a binding-count sequence so a receiver that missed a binding fails closed, a restart when the sender starts from zero, other
rounds' blocks read past with no table); every new command decodes with bounds (op ranges, string caps, path length, reseat
count); the frame/checksum/start payloads carry the round id from c_RoundVersion on and decode the older versions. Coordinator:
the receive set is derived from peerCount and the send targets from the transport map (a relay host must reach every remote);
per-peer input delays must cover every peer and agree with the local delay; ResetRoundState is the single place a round's state
is cleared (Start, StartReplay and a followed round all pass through it); ReadoptRound re-sends the local frames under the new
round and carries their readings on the next frame rather than re-binding slots the refused send never showed; the relay host
answers repeated starts rate-limited and never for a peer that already played this round; frames that outrun a peer's start are
held and flushed when it lands; the sender/transport binding rejects a claim from the wrong socket on the relay host only.
Determinism: AdvanceReadyFrames merges every remote's frames, commands and observations in ascending peer order (the one N-peer
ordering point, and it is a std::map); a leave is applied at FirstFrameWithout as computed by the relay host and relayed as a
PeerLeft notice behind the leaver's last frames on the ordered lane, so every survivor drops the requirement at the same frame;
observations that outrun the byte budget are carried to the next frame and the stalest dropped BEFORE the packet on the sending
peer alone, so what every peer commits is exactly what went out. NOTE (not a finding): ResolveActorOwner consults leave
membership without a frame, so WHICH peer runs a left team's AI can flip a few frames apart on different machines; under
Controller-sync that only changes who produces the controller frames, which the wire replicates identically to all peers, so
the sim cannot diverge from it. Stalls, silence adjudication and start retransmits use the steady clock for pacing only.

### Part B: NetReconnectSession.cpp/.h (new, 1642 + 571 lines, all read), NetMatchService.cpp (1053 diff lines), NetLobbySession.cpp (727), NetSession.cpp (694), NetProtocol.cpp (610) — 22:24 UTC — VERDICT: all sound
NetReconnectSession: the H4 admission plane. Every denial is uniform and delayed (an unknown seat and a stale epoch get a
synthetic challenge and the same refusal a bad proof gets, so timing tells nothing); identity is validated before any seat
lookup; results are cached by transaction id + a key that binds seat, generation and identity, so a lost commit replays
instead of failing; retransmitted reclaims are dropped rather than re-charged to the rate limit or re-answered; a superseded
generation is kept retired for the P2 window so only a claimant who proves the old credential is told the seat was
reassigned; substitution is a compare-and-swap on a per-seat generation bumped by every hand change, the credential drawn but
not installed until the ack proves it; fences keep a superseded transport's later timeout from evicting the live holder;
drops record the ledgered ownership only from the sim tick (a census from another thread is refused and counted) and a
committed reclaim earns a system-authored Reseat that the match service enqueues as a lockstep command; a lobby drop
releases the seat, a live-match leave closes it. The client persists the ticket BEFORE acking, answers challenges from the
stored credential, falls back to exactly one fresh join when a reclaim is refused, never on SeatReassigned, and clears the
record only on an acknowledged leave or a confirmed session end. Hold expiry is host wall-clock state; under the P4B code it
reached the sim through IsHoldingSeatForReclaim -> ResolveActorOwner / IsActorOwnerGone (per-machine timing, see the
NetLockstep note above); the overnight W62 hold-pause superseded that path (commits pause while a seat is held, so the
resolution lands at one frame) and was reviewed on 2026-09-12 as F3's context.
NetMatchService: the host snapshots BEFORE the teardown and both sides reload the identical per-process file; a joiner
whose lobby round carried a state launches from it, never fresh; PumpSessionEvents runs on the sim thread inside the tick
under the census scope; the clean leave runs on the worker with the P21 budget, never sleeping the sim thread; the client
nonce is real entropy (transport identity only); the roster is host-authored and per-peer input delays derived from RTT
are part of the hashed match config every client adopts. NetLobbySession: N-peer star with a committed-transport gate per
message, state transfer in ordered chunks ahead of the Start, a re-sent config re-acked by hash, unbound joiners' faults
never touching the round. NOTE (benign): the transfer id derives from size and chunk count only, so two DIFFERENT snapshots
of equal size in one lobby round would collide; each resync opens a new round, so it cannot happen today. NetSession: an
unauthenticated-connection cap, provisional peer ids for pending admissions replaced by the seat's own id at commit, old-wire
rejections stamped at the claimed version only when that version's schema is writable, fenced traffic dropped before
parsing, host-side unbound faults isolated. NetProtocol: the thirteen H4 payloads decode with the H4 version pinned,
generation and incarnation zero refused, booleans 0/1, reserved bytes zero, and an admission payload refused on the header's
length before any parse.

### Part B: NetLockstep.h (534 diff lines), NetMatchReplay.cpp (new, 421), NetMatchRunner.cpp (352), ControllerFrame.cpp (241) — 22:25 UTC — VERDICT: all sound
NetLockstep.h matches the .cpp (codec v15 with v8 minimum; the observation dictionary's contract; c_MaxFutureFrameSkew = 4 x
the max input delay bounds every per-frame map). NetMatchReplay: version-5 records wrap the wire frame with a length, the
per-command and per-observation sender bytes, a payload checksum (v4+) and an end marker (v2+), so truncation, corruption
and tick gaps are told apart; a record's observations must fit one packet or the writer refuses; lengths are capped (1 MiB
config, 16 MiB record); the config block is re-stamped from lobby v2 only when its header says v2 (a documented layout
assumption, not a defect). NetMatchRunner: the lockstep peer set is the session's ready peers sorted by assigned id; the
host's auto input delay is per sender from its RTT with the manual setting as the floor and lands in the hashed config every
client adopts; the round id is fresh entropy xor the steady clock, never zero, host-only; the lobby and the lockstep
handshake feed one clock into the coordinator. ControllerFrame v6: device class and digital aim speed ride the frame
(class range-checked, speed finite and non-negative), a legacy v5 frame decodes only as v5 with its reserved byte zero, aim
and flip intents ride as flags with the actor's off-wire values so a direct AI write reaches every peer through the wire
and the live state stays what the wire last applied.

### Part B: NetReconnectAdmission.cpp (182), NetSeatAuth.cpp (137), NetReconnectTranscript.cpp (115), NetReconnectTicketStore.cpp (267), NetMatchConfig.cpp (127), NetLanDiscovery.cpp (279) — 22:26 UTC — VERDICT: all sound
Admission: one attempt per connection per second plus a global per-interval cap, challenge caps global and per
connection, challenges expire and are consumed on first use, denials are released after a uniform delay and coalesced per
connection (a proved refusal may upgrade a pending uniform one but keeps its release time), a dropped connection loses its
challenges and denials. Seat auth: epoch and credentials from the crypto provider only (fail closed without one), one
generation counter per seat, retired credentials answered only to their prover, constant-time compares everywhere. The
transcript is all fixed width under a domain tag (F14's golden checks exactly this layout); proofs and the ticket record are
HMAC-SHA256 with domain separation. The ticket store writes a temp file, flushes, commits/fsyncs, then renames (never
truncates in place), MACs the body, refuses a bad version, a zero generation, an oversized address or a stale record, and
sets owner-only permissions. NetMatchConfig validates per-peer delays (cover every peer, never below the manual floor, never
above the cap), CPU slots without a peer, and hashes the config as v2 with the delays in it. LAN discovery: a beacon
broadcast on the discovery port at a fixed interval (LAN only; the product's LAN browser), a bounded browser parse, and a
primary-address probe that does a UDP connect to 8.8.8.8:53 — a UDP connect sends no packet, it only picks the outbound
interface. NOTE: a hosting lobby beacons to the LAN broadcast address whenever an e2e hosts; that is LAN-local by design
and has run through every battery, not a finding.

### Part B: NetReconnectUx.cpp, NetLobbyProtocol.cpp, GnsTransport.cpp, NetReconnectLedger.cpp, NetAuthCrypto.cpp, NetReconnectTxCache.cpp, ControllerLog.cpp, NetGameCommand.cpp, NetActorOwnership.cpp, SimChecksum.cpp, LoopbackTransport.cpp (874 diff lines) — 22:26 UTC — VERDICT: all sound
The reconnect UX is a bounded retry ladder (max attempts, the host's resume window) with cancel and manual retry that never
touch the record; lobby state chunks are bounded on every header field before their bytes are read and the v1 config
(old replay headers) decodes as a uniform delay; the GNS transport raises the send buffer and rates for multi-MB state
transfers, shortens the connected timeout to 4 s, exposes a test-only symmetric fake lag, synthesizes the PeerDisconnected
event for a close it made itself (so a rejected or superseded peer is cleaned up the same way as a lost one) and reports a
broken receive pump as a LocalTransportFault distinct from a joiner's fault; the ledger stores sorted unique uids capped per
seat and restores only alive same-team actors; the crypto provider is OpenSSL (RAND_bytes, HMAC-SHA256 via EVP_Q_mac) and
fails closed without GNS; the tx cache is bounded, expiring, and refuses a key mismatch; controller logs carry v1 (legacy
frame) and v2; team command authority = a human on the team, else the host; SimChecksum gains the suppression the
speculation overlay uses; the loopback transport gains refuse-send and silent-disconnect test hooks.

### Part B: Source/Network production code COMPLETE (54 files, 13,685 diff lines, every line read by the lead) — 22:34 UTC
Verdict for the area: sound. Findings from the lead's own read: none in the code as it stands in the candidate; the two
network second-reader findings below were re-derived and one survives (F17).

### Part B audit lanes pb-audit-1..5 (Grok, read-only, ~6-10 min each, 12+10+8+6+7 findings) — re-derived by the lead 22:34 UTC
Every finding was checked against the CANDIDATE (7d3666aa15), not only the P4B tip, since later work may have closed it.
- B-4-1 (HIGH claimed: a departed peer's absent transport mapping let any sender claim its id): TRUE at be217add64,
  CLOSED since — the candidate's SenderOwnsTransport requires a present, matching binding ("departed seats remain known
  logical peers, but their removed transport binding grants no authority") and HandleStop ignores stops from left peers.
- B-4-2 (MEDIUM: a non-host peer's Stop with a non-recovery reason fails the host's round): TRUE in the candidate —
  HandleStop routes Complete (deferred), Desync/ResyncRequested (scheduled recovery), PeerLeft/PeerDropped (a leave) and
  hold/waiver reasons, but a ProtocolError / InternalError / MissingFrameTimeout / PeerDisconnected stop from a CLIENT
  sets the host's coordinator to Failed, which ends everyone's match; a client that merely disconnects is adjudicated as a
  leave and the survivors play on. So one client's local fault (or one packet from a griefer) ends an N-peer match that
  its socket closing would not. F17 (NEW, MEDIUM, robustness): on the relay host a non-host sender's non-recovery stop is
  that peer's leave (ApplyPeerLeave at FirstFrameWithout with the stop's message, relayed as PeerLeft), never the round's
  failure; a client still fails on the host's stop. Owner: W135 (Grok, lead design) with a selftest arm.
- B-4-3 (Reseat from any sender): the apply gate rejects a Reseat whose sender is not the host — closed. B-4-4/5/6: notes.
- B-1-1 / B-5-3 (Lua worker-state index saved and restored, folded by this machine's state count): the fold is consistent
  with the live assignment rule; the design already lets peers differ in state count (W85 took it out of the identity),
  and cross-object Lua globals are per state by the engine's multithreaded-Lua contract, so the fold is not a new hazard.
  NOTE; re-read when LuaMan.cpp is read.
- B-1-2 (PieMenu sub-menu hover-open delay is a per-machine setting on a sim timer): TRUE — PieMenu::Update runs inside
  Actor::Update on every peer for every actor, the hovered slice comes from the wire's analog cursor, GetPieCommand /
  HandlePieCommand execute on every peer from each peer's own menu state, and SubPieMenuHoverOpenDelay is neither pinned
  by ApplyDeterministicConfig nor part of the deterministic-config hash. Two peers with different values open the sub-menu
  on different ticks; a release in that window resolves a different slice on each, and the slice's action (reload, drop,
  an AI mode) runs on each peer from its own resolution. F18 (NEW, MEDIUM, determinism; needs a settings difference):
  pin the delay in ApplyDeterministicConfig like AutomaticGoldDeposit/CrabBombs (save the user's value, set the default
  for the match, hand it back after). Owner: W135.
- B-1-3 (sub-menu close 50/500 ms by IsMouseControlled): CLOSED — Controller::IsMouseControlled reads the WIRE scheme
  (m_WireDeviceClass) whenever a frame applied one, so every peer sees the owner's device.
- B-1-4 / B-5-6 (Controller::RenderUpdate writes analog fields from local input): the render write lands on the
  Activity's player controllers (menu/cursor), not on the actors' controllers the sim reads; the actor controllers are
  driven by the wire apply in UpdateControllers, and PreControllerUpdate (which runs before that apply, the old
  ordering kept for script compatibility) reads the previous tick's applied state on every peer alike. NOTE.
- B-1-5 (duplicate restored uid registered after the error), B-1-6 (scene restore commits owners then can fail), B-5-7
  (RestoreWorldCandidate's early returns after the purge): all on failure paths that abort the restore; LOW, not blockers.
- B-1-7 / B-3-7 (AI orders enqueued with sender 0): EnqueueLocalGameCommand substitutes the local peer id and QueueLocalInput
  restamps every command — closed. B-1-8..12, B-3-2, B-3-4..6, B-3-8..10: notes, no action.
- B-3-1 (primitive drawing now throws for a borrowed Vector in a table where it used to adopt): the old path adopted an
  alias (undefined behaviour, would have freed an engine member), so no working mod relied on it; the friendlier form is
  to COPY a borrowed value instead of throwing — LOW, queued as a compat nicety, not a blocker.
- B-3-3 (a GUI click could raise two Pushed edges from the render-rate and sim-rate sources): needs the UInputMan read;
  carried into the Managers read (UInputMan.cpp is in it).
- B-5-1 (Atom::DrawTrail draws the sim RNG from Draw): the render path runs under the render-RNG override; the settle
  draws on the sim thread consume the sim RNG at the same point on every peer (settling is sim), so the sequence stays
  identical — NOTE (caching the length in the sim update, as the code's own comment says, remains the cleaner form).
- B-5-2 (which voice's audibility an observation samples is unordered_set order): the sampled value is the owner's own
  reading committed over the wire, so every peer applies the same number; unordered only makes the reading arbitrary among
  equal voices — NOTE. B-5-4 (the preview leaves the camera target at the predicted position, so the next observation's
  listener sits a few pixels ahead): by design of the preview; the value still rides the wire — NOTE. B-5-5: tick-boundary
  snapshots hold no ignore MOIDs — NOTE.
- B-2-1 + B-2-2 (contract-audit fixtures gate on the literal uid 1048577 and run_audit's gate never fails on an empty or
  mismatching check list, so a drifted uid makes the audit vacuous): TRUE by text; the recorded contract-audit runs are
  being checked for the check lines. F19 (LOW-MEDIUM, oracle validity): the fixtures gate on the activity's first
  scripted object rather than a literal uid and print a SKIPPED line when they do not fire; run_audit fails the gate on an
  empty check list or any mismatch. B-2-3 (compare_sim_traces' required set omits particles/items/carve_math): add them
  when the engine always emits them. B-2-5 (two lobby oracles share default ports): require --port or give each its own.
  Owner: W134 (Grok, tools only). B-2-4 (no firewall check at the tip): closed since (the runner refuses unruled paths).

### W129 (item4-feel stage2/preview-scripts 56b9e19f64..6ae2d3c6f4; 7 files +97/-14, all read) — READ 22:36 UTC — VERDICT: accepted for the next wave
H1: a preview clone never runs Create (InitializeObjectScripts returns after naming the clone's script slot; the clone's
self is the copied table) and the clones are deleted under FaithfulCloneScope(false), so neither the clone's creation nor
its teardown allocates checkpoint sound identities. H2: a SoundContainer userdata held in a copied self becomes a preview
COPY (cloned under the faithful scope, so it takes no identity) whose m_PreviewOrigin points at the canonical container;
PlaySoundContainer keys the event and owns the voice through PreviewPlaybackOwner()/PlaybackCheckpointIdentity(), so a
preview hook's Play lands on the origin's identity and adoption works; the copies are dropped at capture and at
EndPreviewScripts. H3: the glow oracle is keyed to the actor and the firearm's owned subtree and passes when that subtree
has no post effect (the AK-47 has none; pickup_fire's Battle Rifle output is byte-identical to the crash-green run). Note:
that makes the glow check conditional; the Lua-fire voice, the once-only delivery and the flash-sprite guard remain the
detectors for the AK-47, so it is not a widened oracle for the case it was written for. Evidence re-derived from the
report: the cursor holds at depths 1/4/7 (was +2 per preview), AK lpinv 4/4 at ticks 150/176, pickup lpinv 8/8, script
graph PASS, dual replay 400 ticks identical, fl100 pass. Merge note: PushPreviewClone is also touched by W127 (the remap
after it); expect a keep-both resolution in LuaMan.cpp.

### Part B: Source/Managers/MovableMan.cpp (2,939 diff lines, every line read by the lead) — 22:43 UTC
Verdict: sound; no new finding. Read: the lockstep frame apply/neutralize and the game-command gates; the DumpSimState /
DumpAttachableTree forensics; WorldSnapshot capture and the candidate restore (RestoreWorld validates the structure and the
script graphs first, sets the original aside, tries the candidate, discards the original on success or reinstates it and
the runtime globals on failure; a caller already running a speculative world owns its own rollback); SetAsideWorld /
ForgetHeldWorld / ReinstateWorld / DiscardWorld (preset-owned trees keep their script state, stashed script objects are
released per state); the speculation overlay (Begin/End/ShadowOf/SpeculativeView/TakeShadow; the resident/shadow-aware
ValidMO/IsActor/IsDevice/IsParticle/FindObjectByUniqueID; Remove* hands back the shadow or reports a violation);
PurgeAllMOs (Destroy callbacks drained over stable identities, new roots included, then the deletes; alarm events freed);
the snapshot-resident Add* path (AdoptPersistedUniqueID + a pending link resolve, no spawn normalisation, no join
quarantine) against the normal path (DiscardPersistedSnapshotState first); AbsorbAddedMOs (the added queues are sorted
only when no restore cohort is pending, so a restored world keeps its saved live order) + ResolvePendingSnapshotLinks;
the per-stage RNG and sound scopes (TravelStage/PreControllerStage/UpdateStage/PostUpdateStage; the pie command lands
before PreControllerUpdate as the activity used to do it); UpdateControllers (a stopped coordinator surfaces its stop
reason instead of degrading to per-machine control; object scripts initialise at one point on every peer;
SettleSharedSoundWrites before the first AI script; the AI's deferred equips and sound ops drain to commands under
lockstep and execute directly outside it; its aim/flip writes are reverted to one-shot intents and an equipment change
is a boundary violation; the committed local and remote frames apply with one `applied` set, unframed actors are
neutralised, a gone owner's overrides are purged and its actors stand down on every survivor at the same tick;
CommitSoundObservations then ApplyLockstepGameCommands); the MOID draw and the GC kicked from Main after
LateUpdateGlobalScripts; render hiding; the MovableMan2 checkpoint (borrowed-reference rows only for borrowers, the
whole alias table validated before the commit and rebound at it; MovableMan1 still readable); WorldStructure save/load
(membership equality against the loaded world, alarm events allocated before any live mutation);
ConstructionRegistryScope and the held registries (UnregisterObject erases by ADDRESS in every held copy,
MovableMan.cpp:1369, so a scope's copy never keeps a dead pointer and never loses an object that took a new identity).
Notes, no action: (1) the AI pass's `directBefore` pointers assume no actor leaves m_Actors during the AI scripts;
MovableMan:RemoveActor from an AI script only unlists (the caller owns the pointer), so nothing dangles inside the tick —
the same exposure as upstream's serial UpdateAI. (2) MovableMan::Update no longer clears or draws the MO colour layer;
the new call sites are checked in the Main/FrameMan read (carried).

### W132 LANDED (Grok, takeover-build, ASan measurement only; report w132-asan-preview-hud/REPORT.md with 12 verbatim reports) — 22:43 UTC
Re-derived by the lead from the reports' frames: all 10 AK HUD runs (5 on W103's tip 3f27189784 + the ASan config, 5 on
W130's tip b944ce84dc + the ASan config; `git diff --stat pre..post -- Source` is W130's 5 lines only) and both
pickup_fire ledgers halted on one heap-use-after-free: READ 8 in AHuman::DrawHUD (AHuman.cpp:3672,
m_pItemInReach->GetPresetName()) of an HDFirearm allocated by HDFirearm::Clone <- MovableMan::ShadowOf <-
SpeculativeView <- FindObjectByUniqueID <- Actor::ResolveFaithfulLinks <- AHuman::ResolveFaithfulLinks <-
LocalPrediction::RunPreview:165, freed by ~HDFirearm <- MovableMan::EndSpeculation:2029 <- RunPreview:231. pre 5/5,
post 5/5: W130's MOID-join change is not the mechanism (it stays merged as a correct ordering change) and F15 stands
exactly as opened: the substitute's faithful link is resolved through the speculating lookup onto a preview clone that
EndSpeculation frees, and the HUD reads it on the next frame. This is W133's RED: its fix must make the same 10 HUD runs
and the 2 ledgers report nothing under the same ASan configuration. W90's reload-365 RestoreRollbackState AV was not
reached (halt_on_error stops at the first report); it is re-measured after W133. Tree: takeover-build is left on
stage2/w132-asan-preview-hud (d6c55c20a5 = b944ce84dc + the ASan config cherry-pick) with the ASan exe and DLL beside
it; the pre branch is stage2/w132-asan-preview-hud-pre (0e03aebafe). Nothing to merge.

### W124-2 LANDED, READ, ACCEPTED (F13 fix, hold-pause stage2/relaunch-slots-fg6b) — 23:00 UTC
Branch from 7d3666aa15: 5cf123538c and dbefb6ef1a are W124's oracle and rebind commits re-picked (the lead diffed the
patches against 0546b30faf and 8efc5140c9: identical except hunk offsets), then 49b7249b78 "End the lockstep relaunch
window after the first world update" (7 files, +17/-10, every line read): ActivityMan::EndLockstepRelaunch clears
m_LockstepRelaunchInProgress and calls Activity::ClearCheckpointActorIDs (GameActivity also clears the marked-actor
flag), called by MovableMan::Update right after the end-of-tick RebindNonOwnedActorSlots; the dead
ArmLockstepRelaunchChecks / ConsumeLockstepRelaunchCheck / m_LockstepRelaunchChecksLeft are gone. Checked: every consumer
of the two flags (the rebind early-outs, the checkpoint reference collection at Activity.cpp:1214, the "clear when not
relaunching" sites) is consistent with clearing them at the window's end — and the F13 damage included later checkpoint
saves writing stale persisted ids, which this also ends. NoteLockstepRelaunch is called from NetMatchService.cpp:734 in
the resync staging and RestartActivity follows in the same HandleControllerReplayFailure call (Main.cpp:2670-2680) with
only the resync UI loop between, so no world update can end the window early. RED `FAIL: tick 460: owner=2 after
hand-back, expected host 1` on the untouched picks; GREEN 10/10 switch-returner arms with check_switch_control PASS and
stale_activity_slots 0, plain drop3 9/10 (the tenth wrote the ASan report that opened F20, below), the three make_run
selftests PASS, suite 10/11 (F14b). Merges in the next wave as the branch.

### F14 was half-fixed — NEGATIVE against the lead — F14b fixed at 5c2c65c2ed — 23:00 UTC
7d3666aa15 derived the transcript layout golden from NetProtocol::c_Version but TestKnownAnswers' three HMAC known
answers (reclaim f0ea2c37…, substitution 72add2ad…, ticket 4f75c6ec…) were computed over the version-1 transcript and
MakeTranscript reads the live version (2), so -net-reconnect-selftest stayed red on the candidate: `reclaim proof
known-answer mismatch: 56527b38…` (FG6B, W124-2, W133 and W135 all saw it). The lead did not re-run the reconnect selftest
on the rebuilt exe after the F14 edit. Fix 5c2c65c2ed on control-build: the known-answer transcript pins protocolVersion
= 1 (a known answer is a constant; the layout test keeps covering the live version); Source/Network/NetReconnectSelfTest.cpp
+3/-1, git_commit.py, trailer 0. Verified built-and-run by W136 (base 5c2c65c2ed, suite must be 11/11) and at the wave.

### F20 OPENED (HIGH, memory safety): a restored coroutine's stack is sized by its saved top, not its frames — 23:00 UTC
Evidence: D:\mx\w124b\asan\report.52732 (W124-2 drop3-10, host side at the resync relaunch): heap-buffer-overflow, WRITE
of size 8 at 0 bytes past a 384-byte region, in mmcall (lj_meta.c:127) <- lj_meta_tget <- lj_vmeta_tgetv <-
lj_ff_coroutine_resume <- lua_pcall <- LuaStateWrapper::RunScriptFunctionObject <- RunScriptedFunctionInAppropriateScripts
<- the threaded AI pass (thread T72); the region was allocated by stack_init <- lj_state_new <- lua_newthread <-
coroutine.create <- lua_pcall <- LuaStateWrapper::RestoreScriptGraph (LuaMan.cpp:4057) <- MovableMan::RestoreScriptGraphs
<- ActivityMan::RestartActivityCandidate <- RestartActivity <- HandleControllerReplayFailure. Lead-read cause,
Source/Managers/LuaThreadCodec.cpp ThreadRestore: `lua_checkstack(co, top + 16)` is the only sizing; the VM relies on
base + framesize <= maxstack for every live Lua frame (checked at call time, never on resume); a yielded frame's saved top
is the yield's argument top, so a function with many locals or temporaries is restored into a stack that cannot hold its
frame and the first register write or metamethod call past the allocation corrupts the heap (1 in 10 here because a fresh
thread's slack only fails for large frames). Any script-graph restore that rebuilds a suspended coroutine is exposed
(relaunch, resync heal, rollback probe, the preview codec paths). Owner: W136 (Grok, alias-walk, lead design: size by the
frames' base + framesize from the saved slots, a post-restore safety net, a _ScriptGraphThreadStackFits query, a RED-first
96-local arm in the script-graph selftest, suite 11/11). The drop3 arm is re-measured under ASan on the merged candidate.

### W133 LANDED + READ: correct but INCOMPLETE (F15 has a second head) — 23:00 UTC
8b3c6590ee (11 files, +70, every line read): RemapExternalLinks walks m_pMOToNotHit, m_pItemInReach, m_pMOMoveTarget, the
waypoints, a craft's exit incoming MO, the attachables and the inventory, through a map; ResidentForRetiringShadow maps
an in-world shadow to its resident (keyed right: m_Speculation.residents is shadow -> resident and m_Speculation.shadows
is resident -> info) and leaves a taken shadow alone; RunPreview calls it for every preview clone before EndSpeculation.
What it misses, from the lead's read of EndSpeculation (MovableMan.cpp ~2290): the overlay also deletes the speculative
SPAWNS (DisposeSpeculativeSpawns, except a named projectile that becomes a ghost) and the add-queue tail past the mark
(DiscardAddedSince). A preview clone that drops its gun in the preview (the AK-47 fixture's WEAPON_DROP at tick 60)
makes that gun a spawn, and its m_pItemInReach names it — W132's crash shape on the other timing (W132's item in reach
was the shadow of the canonical dropped gun). And LuaMan::BeginPreviewScripts walks every clone part into s_PreviewClones
while EndPreviewScripts (LocalPrediction.cpp:318, after EndSpeculation at :278) dereferences those pointers
(mo->GetUniqueID(), GetLuaState()) — the dropped gun was a walked part: W133's own ASan runs halt there 5/5 on the
untouched tip and on its fix (report.48048), so DrawHUD was never reached and the shadow remap is unmeasured on the
fixture. Design for W133-2 (same branch): OverlaySurvivorOf(mo, retiring) = resident for an in-world shadow, nullptr for a
member of RetiringOverlayObjects() (spawns, spawnMeta keys, the added queues past the mark, under the add-queue locks),
mo otherwise; EndPreviewScripts iterates uid/state pairs recorded at Begin and never dereferences s_PreviewClones.
GREEN = the HUD replay 5/5 exit 0 with `[preview-hud-selftest] PASS` and no ASan report, the ledger `PASS press tick
153`, lpinv, suite 11/11. W133 merges in the wave as the branch; W133-2 lands on it.

### W135 LANDED + READ + ACCEPTED (F17 + F18, fencing-warm stage2/stop-leave-piemenu-pin) — 23:00 UTC
2b9a8ca1ff (F17): NetLockstepCoordinator::HandleStop, after every existing routing arm (hold resolutions, waivers, the
transport and known/left-peer checks, deferred Complete, scheduled Desync/ResyncRequested, PeerLeft/PeerDropped) and right
before the Stopped/Failed fallthrough: on the relay host a non-host sender's ProtocolError / InternalError /
MissingFrameTimeout / PeerDisconnected stop is ApplyPeerLeave(sender, FirstFrameWithout(sender), "<Reason>: message",
nowMs, announced) and returns; stops_adjudicated_as_leaves counted and reported. Selftest arms: a client's ProtocolError
stop leaves the host Running with the client in the leave map, the survivor gets the relayed PeerLeft at the same frame
and host + survivor keep committing; the host's own ProtocolError still fails both clients. 9a37f3a246 (F18):
s_SavedSubPieMenuHoverOpenDelay saved once with the other pins, SetSubPieMenuHoverOpenDelay(1000) (the SettingsMan::Clear
default, SettingsMan.cpp:28) in ApplyDeterministicConfig, handed back in the `!coordinator && s_SimSettingsPinned` block;
PieMenu::UpdateSliceActivation re-reads the setting into the hover-open timer's limit before IsPastSimTimeLimit, so a
menu constructed before the pin (scene-placed actors clone preset menus at module load) opens on the match value;
RunHoverOpenDelayPinSelfTest (900 -> pinned 1000 -> fresh menu limit 1000 -> restored 900) called from the pie-menu
checkpoint selftest and -net-lockstep-selftest. Every line read. RED lines quoted on exe b13b1a6e9e; GREEN lockstep and
match PASS, gcb pass, suite 10/11 (F14b). Note, no action: the pin arm calls ApplyDeterministicConfig inside two
selftests (it also pins dt, the uid counter and the sound cursor there; both selftests still PASS). Not done by the lane:
a 2-peer stop arm (ApplyPeerLeave with no survivor ends the round exactly as the client's socket closing would).

### Part B: Source/Managers/LuaMan.cpp (5,107 diff lines) and LuaThreadCodec.cpp/.h (596) read in full — 23:00 UTC
LuaMan.cpp: sound. Read: the deterministic math overrides; the per-MO RNG scope; the script-graph codec (the Lua helper:
baseline capture, path naming, the canonical serializer with owned values as nodes, the strict reader/validator, prepare
with held-object reuse, deserialize in dependency order — tables and functions first, native copies, iterators, gib
references, closures with cells and open upvalues joined, coroutines slot for slot, engine-table patches, the VM RNG;
the native descriptors — vector/timer/alarm/path-request/controller/owner-ref/gib/soundset/limb/box/area/preset/copy;
the owner references into the primitive queues, the activity GUIs and banners, the scene backgrounds; the
script-owned-tree walk that a capture and a release both run; adopt-root; the checkpointed path callbacks with a total
order restored per state; the selftest with its negative controls); GetStateByIndex folds by this machine's state count
(B-1-1/B-5-3 stays a NOTE: consistent with the assignment rule, state counts are not part of the identity since W85);
ClearLuaScriptCache now frees its wrappers; CollectGarbageForCheckpoint repeats full collections until nothing frees.
LuaThreadCodec.cpp: F20 (above) in ThreadRestore's sizing; the capture (frame chain, continuation names, the stitch
collapse and remap), the link/cont rebuild with its chain validation, OpenUpvalues (GC threshold raised for the walk),
FindOpenUpvalue (sorted insertion into the thread's open list and the global uv ring, matching lj_func), JoinOpenUpvalue
(with the GC barrier), VisitUserdata (live objects plus the queued finalizers) are otherwise sound.

### W134 LANDED + READ + ACCEPTED (F19 tools; item7-chat stage2/tools-oracle-fixes) — 23:10 UTC
83cdcaa59f: every fixture under tools/contracts claims `_ContractAuditOwner` on its first Create/Update (a spawned child
never claims) and prints one `[<family>-contract-check] ARMED uid=<n>` line; the literal 1048577 is gone; check bodies
untouched. The lead checked that each fixture's ARMED family is the same prefix its check lines use (activity, native,
reference, primitive-batch, primitive-cast, primitive-cast-v2), so the new gate can pair them. f80879bdfd: run_audit
records the ARMED fixtures and gate() fails `fixture_armed` (a non-load transition that armed nothing), `<family>_checks`
(an armed fixture with no check line), and every `native_mismatches:` / `contract_mismatches:` line by name, or the bare
name when only a count is nonzero; unit tests RED against the old gate (quoted) and 12/12 after; the README keeps
"complete is not faithful restoration". 0bbfaedb10: --port required on both lobby oracles; every caller already passes
one. B-2-3 not applied, accepted: the engine emits particles/items/carve_math only on ticks that have them (a lobby
host trace lacks items on all 180 ticks), and the lead's read of compare_sim_traces' loop shows the per-tick diff runs
over the union of both traces' subsystem keys, so a subsystem that one side drops is already a divergence; a subsystem
both sides lack identically is the conditional case. F19 closes with this merge.

### Wave A prepared on the scratch branch stage2/fixgroup-6-lead-wave-a (takeover-build) — 23:10 UTC
5c2c65c2ed → W127 f1fb71f401 (clean, f6d70fb596) → W131 f605a8330a (2f9e7dabd0; NetLockstepSelfTest.cpp keep-both of the
W119 and W131 arms inserted at one anchor) → W129 6ae2d3c6f4 (2a46169dfb; Main.cpp include keep-both, LuaMan.cpp W127's
remap block followed by W129's DropPreviewSoundCopies). Each merge commit verified with verify_merge_commit.py (the
+/- line multiset of merge^1..merge equals merge-base..other per file; 0 differing files). NEGATIVE: the lead's first
W131 resolution rebuilt the file from stage `:2:` and dropped the two hunks git had already auto-merged (the Run-list
entry and an include) — caught by the verifier, fixed, amended. Rule: keep-both edits the conflicted working file.
Semantic clash found while merging (to fix as a lead commit after the merges): W127's RemapPreviewUserdata freezes a
preview whose hold holds any Entity that is not a MovableObject; W129's PushPreviewClone now puts SoundContainer copies
(SetPreviewOrigin) into that hold, so on the merged tree every preview whose scripts hold a sound would freeze and W129's
AK-47 fix would be moot. The copies are the preview's own values (like the Vectors already passed through), so
SoundContainer passes the remap; other non-MovableObject entities keep freezing. Verified on the wave build with W127's
held-ref fixtures and W129's AK lpinv arms.

### Part B: Managers complete; System in progress — 23:10 UTC
Read in full since the last entry: AudioMan.cpp/.h (logical playback, voice identities, the staged checkpoint restore
with rollback, committed audibility, the logical selftest), ActivityMan.cpp/.h (save with callbacks first and a
temp-file zip, staged loads, RestartActivityCandidate with the world set aside, RuntimeGlobals 1-9, the H4 scripted-end
deferral), SceneMan.cpp/.h (terrain event trace, material catalog with retired copies, the MO colour draw moved into
SceneMan::Draw for screen 0, MOID registration suspended for render draws), FrameMan.cpp/.h (queued screen dumps,
palette/blender checkpoint, fonts), UInputMan.cpp/.h (scripted players, sim-rate edge accumulators cleared by
EndSimUpdate and per frame only while paused — B-3-3 closed: the render-rate and sim-rate edges are separate buffers
read by different consumers), MusicMan, PrimitiveMan (queue checkpoint with shared vertices), PostProcessMan (attached
effects read the render pos), LocalPrediction.cpp/.h (the preview fences, clones, steps, restores; F15 aside), the
remaining manager headers and small files; System: ScenarioRunner.cpp (replay record/playback with the rewind buffer
and read-ahead, control overrides and gone-owner purge, IsLockstepHoldingSeatForReclaim's "no frame committed yet"
clause, the wall-clock give-up and the session pump in the frame wait), Controller.cpp (seat player/mode separate from
the wire-applied mode, wire scheme facts, sim-rate edges, RenderUpdate writes only analog values of the seat),
Atom.cpp (faithful clone state, material and link references, trails drawn at render time), CheckpointArchive.h,
PathFinder.cpp (grid checkpoint, request scope counting), Entity.cpp (snapshot identity). All sound; no new finding.

### W71-4 LANDED + READ + ACCEPTED WITH CORRECTIONS (F10 pulse; item5-lifecycle stage2/cross-actor-waypoints) — 23:31 UTC
b0e33ba3c7. The gate at the top of Actor::UpdateMovePath re-arms m_UpdateMovePath and returns (no path request) when
lockstep is on, the actor is idle (empty move path, no valid MO target, m_Waypoints.size() <= the loaded cursor) and a
write for it is pending (its own pending list, or the running AI actor's list targeting it) or in flight
(m_InflightWaypoints, pushed on the TARGET by SendDeferredWaypoints). Actor::Update's `if (m_UpdateMovePath)` re-runs it
each tick until the wire add applies (ConsumeInflightWaypoint erases the entry, the queue grows), then the normal path
loads waypoint[loaded]. Cross-peer: the non-owner never has inflight entries, so its flag clears; its controller comes
from the wire, and the per-machine path state is off-wire (craft_cargo peers matched, fl100 600 ticks clean). Accepted.
Rejected and corrected: the lockstep load W71-4 added inside the `GetScene() == nullptr` branch exists only so the arm
can pop without a scene (production code for a test; a no-scene UpdateMovePath would silently consume a waypoint and
send a PopWaypoint); dropped. The arm keeps the RED/GREEN contract (armed while pending, one SceneWaypoint order sent,
still armed after the applied add, one queued waypoint); the pop assertions that needed the no-scene branch go; four
failure messages that stated the expectation ("stayed armed") instead of the failure are rephrased; SceneMan is built in
Run() with the other managers (NetDirectorySelfTest's pattern) instead of mid-run inside the arm; GetWaypointCursor goes
with its only user. UpdateMovePath becomes public (it is bound to Lua at LuaBindingsEntities.cpp:267; accepted).
Evidence re-derived from D:/mx/w71e (RED L7, GREEN L7, pulse [316..319] both peers).

### Trailer already on origin — 23:31 UTC
9751a90e29 (2026-09-10, "Compare controlled actors across every activity in snapshot diffs") carries
`Co-authored-by: Cursor <cursoragent@cursor.com>` and is an ancestor of origin/stage2/p4b-interp-lockstep and
origin/stage2/fixgroup-6-lead (pushed before tonight), so every branch scanned tonight shows exactly this one hit. A
message-only rewrite would change every descendant sha (the milestone, the candidate, every lane, every sha quoted in
this log, SPAWN_LOG and the reports); not done at wrap-up; the user decides. Tonight's backup push added no new hit.

### Part B: System complete — 23:31 UTC
ContractAudit.h (the audit visitor over every class's fields, PathFinder/UInputMan/FrameMan/PostProcessMan observers,
the pending-checkpoint observer with native identities, the input fixture), the small System files, micropather's
sentinel-by-identity insert (correct: a FLT_MAX cost is not < the FLT_MAX sentinel, so the compare-only loop never
stopped), StateInventory.csv: all sound. Main.cpp and the network selftests remain unread (budget).

### W136 REJECTED; W136-2 spawned (F20) — 23:54 UTC
The Opus verifier's finding, confirmed by the lead on the source: the restore's sizing walk (LuaThreadCodec.cpp
~345-362) handles only links that carry `pcslot`; `ftsz` links are popped and skipped, so the functions of FRAME_CP
(the coroutine's main function), FRAME_VARG, FRAME_CONT and FRAME_PCALL frames never enter `needed`; the fallback
`slots[pcslot]` names the CALLER's function and the extra `base + framesize` term is an accidental over-estimate for
the tested shape. The safety net (~482) gates on `frame_islua` and skips the same frames, so it is not a backstop.
The one arm covers only the shape where the big frame is the yield's immediate caller. Evidence (RED 285/131 then
0xC0000005 on the pre-fix exe; GREEN 177 PASS, suite 11/11) is genuine but proves the narrow shape only. W136-2
(Opus engineer) adds the below-the-yield arm RED-first and sizes every link from its own function slot.

### Wave build and gates on a44e14132f — 23:54 UTC
Suite 11/11 with the trimmed W71-4 arm and the reconnect selftest green; W127's three held-ref fixtures 8/8 with no
freeze except the fallback fixture's deliberate Activity freezes; W129's three AK lpinv arms green with fallback=0.
The sound-copy fix-up c676483586 is therefore proven by the write/control fixtures (a frozen preview would show as
fallback>0). Open: the depth-12 spawn count (8 vs 3), hypothesised F16; to confirm before the battery.

### FG6B battery read — 23:58 UTC
No true engine failure on exe 98f73d74aaed (7d3666aa15). Every red is classified: the reconnect KAT (F14b, fixed and
confirmed on the wave build), the old-driver fencing copy (the current driver 5/5), the heal_wp script_continued
artefact, the stale-exe `binary_matches_source` on the fixture steps with every semantic check green, the crab
driver's PowerShell argument error, the bare global-callback flag, the present_identity artefact, the absent
controller_log fixture. N1-N5 green (AK-47 HUD 3/3 with no dump on this pre-W133 exe; expire3; the loopback
session-id e2e; portmap on/off; drop3_switch, lpinv_100, preview_event_d7). The wave tip needs its own battery.

### W133-2 ACCEPTED WITH FOLLOW-UPS (F15's second head) — 2026-09-12 17:10 MST (stamps from here on are Arizona time)
Verifier's read confirmed by the lead on the source: OverlaySurvivorOf keeps the shadow semantics (in-world shadow →
resident, taken → the object) and adds the retiring branch (nullptr for spawns, spawnMeta keys, added-since-mark);
RetiringOverlayObjects is computed while speculation is still active and nothing adds MOs before it; EndPreviewScripts
walks the recorded (uid, state) bindings, so no clone pointer is read after EndSpeculation. The LuaMan head is proven
red→clean on the raw ASan files; the survivor-link head is correct by reading only (no red was ever produced for it).
Follow-ups recorded as F15b (the two unit arms the verifier specified) and F15c (parts of retiring objects and shadow
parts are not in the retiring set — latent because the weak links self-expire and the raw links store roots; wounds
are not recursed by RemapExternalLinks; a stateless clone part that gains a state during the preview keeps its slot).
Merged 8eaa54d923; the wave exe 959fc25e predates this merge, so the wave is rebuilt before the battery.

### W136-2 merged on the lead's read; clean stop — 2026-09-12 17:26 MST
The codec diff is exactly the specified correction (every frame link sized from the function in its own slot; the
safety net and the fits query on the invariant). The lane's RED is the helper arm's false fit followed by an access
violation on the unfixed codec; GREEN 181/0 with six coroutine arms, suite 11/11. Merged 1fa9b540f4 and pushed. The
independent verifier pass did not complete (stopped by the clean-stop order): the next lead re-runs it before the
battery. Gates already green on the wave exe cf2e611b: suite 11/11, script-graph 181/0, AK-47 HUD 3/3, the craft_cargo
pulse [316..319] both peers (F10 closed). Open at the stop: the promotion of control-build, verify6, the ONE battery
(fg6c brief ready), the depth-12 spawn question, F15b/F15c, the W136-2 verifier pass. Nothing is running.
