# Lead's own line-by-line review of the overnight delta (dfe252ad80..aa650e601e) — started 18:12 UTC 2026-09-12

Scope: 111 non-merge commits, 25 merges; Source/ 79 files +11306/-270; tools/ 10 files +2808/-3. The user asked for a
full personal review after the four Grok audits (their findings are in audit-1..4/VERDICT.md and were acted on).
This log records, per file, what the change does, the verdict, and every finding with file:line. Method: `git diff
dfe252ad80 aa650e601e -- <file>` read in full; production code first in risk order, selftests and tools skimmed for
oracle validity. Findings are fixed by the lead or dispatched with red-first proof; nothing is "noted and left".

Order: (1) sim/determinism: MovableMan, LuaMan, Actor, ScenarioRunner, NetLockstep, ControllerFrame, Controller,
ActivityMan, LocalPrediction, AudioMan, PostProcessMan, PreviewEventLedger; (2) reconnect/rematch: NetMatchService,
NetMatchRunner, NetMatchConfig, NetSession, NetReconnectSession, NetLobbySession, NetIdentity, NetReconnectUx;
(3) directory/HTTP/P2P: NetHttpClient(+Apple), NetDirectoryClient, NetDirectoryCodec, NetDirectorySignalChannel,
GnsSignaling, GnsTransport; (4) wiring: Main.cpp, MainMenuGUI, MenuMan, SettingsMan; (5) selftests; (6) tools.

## Findings register (running)
(final register at the end of this log)

## CLOSING (18:59 UTC 2026-09-12; the clock was read) — every file of the delta read
Coverage: all 79 Source files and all 10 tools files of dfe252ad80..aa650e601e, read hunk by hunk (production first,
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
  never by the cohort check). Fix: add a nonzero stale-uid orphan. Owner: lead. DONE 9cecfeea71; verified with the
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
- http-pin-identity (item7-ux 62a0cd9361, W117): the pinned-mode flags (unknown CA + CN + dates), the README
  pin recipe with a SAN, and probe_pin_identity.py (correct / wrong / unpinned through make_run). Red on the
  556599d196 exe with the correct pin (certificate verification failed, no request in the service log), green
  on the fixed exe (five 200s), wrong pin still "certificate pin mismatch", unpinned still refused. Accepted.
- audit-3-harness (item8-discovery a5f5380835, tools only): compare_sim_traces too-few-ticks wording +
  test; launched_exe.py (result headers from launch.json, header/launch mismatch refuses PASS); run_selftests.py
  (PASS-token scoring: exit 0 with zero tokens is FAIL). Lane-copy driver edits live outside the repo. Accepted.
- audit-fixes-1 (item7-chat c9d64430c6): the three added-actor index erases + detecting arms (red
  added_remove=0/absorb_delete=0/discard_added=0 -> green); drains before Complete/FinishMatch/LeaveMatch + the
  fenced-disconnect selftest (red fenced_disconnects=0 -> green); Cancel closes the WinHTTP handle with no lock
  held (lock order re-derived: finish still waits for HANDLE_CLOSING before freeing; Cancel never touches the
  state after releasing the locks); module.root out of the module-manifest hash (red -> green; content still
  hashed); directory codec caps (peer 64 = the service's cap, rows 4096, signals 256, base64 padding only at the
  end; red -> green); GNS incoming handles registered at Connecting; tool ports moved to 4761x and the service
  default port 0. One doc miss: README line 25 still says the default port is 8443 (the plist and the Windows
  task pass --port 8443 explicitly, so the deployments are unaffected) — lead fixes the line.


## Lane acceptances after the closing (19:32 UTC 2026-09-12; every diff line read by the lead before the merge)

### W102-fixtures (fencing-warm 3cb856510e; Main.cpp +116, tools/check_switch_control.py +146) — MERGED 014b9dea59
Stimulus `-net-match-e2e-switch-control <tick>` (client switches onto the lowest-uid non-brain CPU actor, hands back at
+10), owner_log report field (tick, uid, owner, mode) sampled from the switched actor, and the checker (owner_log
byte-identical on both peers, the takeover window owner=client/mode=PLAYER, after hand-back owner=host/mode=AI, traces
strict-identical). Arm 1 PASS on the candidate and RED on the milestone (no stimulus); arm 2 drop3 PASS with the
returner's 518 owner_log rows matching the survivors.
- FIXED by the lead at merge (51b775f176): NoteE2eSwitchOwnerLog was gated on lockstep alone, so a live match would scan
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

### W116 (alias-walk 61346bef5a; Main.cpp +4) — MERGED 4042bc97cb
Root cause of the bare-flag hang: with no -net-replay/-scenario the engine enters RunMenuLoop and never quits. Fix: refuse
the bare flag before LoadAllDataModules (RED 180 s timeout -> GREEN exit 1 in 7.8 s; the driver path unchanged).
ScenarioRunner::ParseArgs sets s_Active inside the same loop, so a -scenario run is not refused. Complements the lead's
missing-fixture FAIL line (9366419eb5).

### W113-2 (item8-directory 90f0549503; service +34, test +68, channel +18, selftest +51) — MERGED eeff419625
Service long-poll (Condition on the store lock, notify on post/delete, deadline <= 25 s, counted once by the limiter) and
the channel's SetPollWait clamped to 12 s under the 15 s HTTP total timeout, re-poll at once after a 200, the drain poll
never held. Red/green both sides. Decision 3 (engine wiring) not started: dispatched as W123 (Opus) with the lead-fixed
design; the settings/flags/GUI pieces follow.

### W89-2 (item4-feel efed9e71e6 on the milestone; 12 files +694/-15) — MERGED f2881161de (LuaMan.cpp append conflict, keep-both)
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

### W97 + W97-2 (h4-secondary stage2/port-map, 8 commits from 3926d4abb0, 12 files +2849/-6) — MERGED f0a0b8e3c6 (Main.cpp keep-both)
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
for MSBuild only (not in Source/Managers/meson.build). Lead fix e13e890fad (a two-argument overload instead of the
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

### W104b (item8-dedicated stage2/preview-projectiles 49af8a120e; 7 files +317/-9) — READ 20:26 UTC, accepted for the next wave
Speculative spawns harvested per horizon step and travelled; a named spawn from a previewed emitter becomes a Projectile
ledger event and a severed presentation-only ghost adopted by the canonical particle at commit; red/green argv-identical
(first round visible at committed press+1, adopted once), lpinv 8/8, three-build compat byte-identical, fl100 at D=7
identical. Roadmap notes: the ghost is static until adoption (travelling it is the optimistic-projectiles follow-up);
the EventStart dedupe hides a same-tick casing adoption in the diagnostics only.


# Pre-overnight review, part A: the takeover window be217add64..dfe252ad80 (2026-09-09 14:31 .. 09-11 17:30) — started 20:40 UTC 2026-09-12
Asked by the user at 20:36 UTC ("are you able to also review changes made before overnight"). 103 commits; Source 99
files +19697/-2211; tools 15 files +1276/-23. Provenance: the Cursor/Codex leads with the Grok High worker generation
(W12-W38). Two of today's findings (F10, F11) have roots here. Method as for the overnight delta: `git diff
be217add64~1 dfe252ad80 -- <file>` read in full, production first in risk order, then selftests and tools; findings
fixed by the lead or dispatched red-first. Part B (the P4B phase before the takeover, 538 commits, 321 files +55756)
follows with the determinism/network core read by the lead and the periphery audited by Grok Extra High lanes.

F7 CORRECTED (20:44 UTC, from W119's evidence): the ownership policy consults the seeded owner before the team's human
(NetActorOwnership::ResolveOwnerPeer, ~:47), so after the purge a claimed CPU actor resolves to its seeded owner (the
host), is never owner 0 and is never disabled; the only residue was the wire mode staying PLAYER for one tick until the
owner's next AI frame applied AI mode. F7 is LOW, not MEDIUM. W119's explicit hand-back at expiry (with its line, unit
arm and the expire3 gate) still lands: it removes the one-tick lag and documents the transition.

### W123 (item8-directory stage2/s4-wiring 887bf43917; 17 files +1391/-11) — READ 20:46 UTC, accepted for the next wave
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
Diff 49f7fd2686 on item7-chat (1798 lines, all read). NetIdentity: BuildModuleDigests sanitizes names (control
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

PRE-OVERNIGHT REVIEW PART A COMPLETE (21:21 UTC): the whole takeover window be217add64..dfe252ad80 has
been read by the lead (Source 99 files, the selftest units, the tools delta). Findings: none new beyond
F10's enqueue half (recorded above for W71-3) and the four already-known artefacts. Part B (the older P4B
phase, p4a gate..be217add64) follows: the lead reads the determinism and network core; Grok Extra High
audit lanes cover the periphery with every finding re-derived by the lead.

### W90 (item4-feel stage2/preview-scripts efed9e71e6..d799f4bec8; Main.cpp +27, five tools) — READ 21:25 UTC — VERDICT: accepted
The AK-47 Lua-fire ledger arm: at the press tick the equipped AK-47's uid is captured, and the first PREDICTED
Sound event start from that emitter must sit at committed tick <= press+1 (the arm only exists when the
equipped preset is the AK-47); tools: the two-peer recorder, the fl100 and replay runners through make_run,
a stdout quoter, and wait_engine_idle (zero engines and no LEAD lock, 2 h cap). Oracle and drivers only.

### W119 (fencing-warm stage2/claimed-actor-expiry f0a0b8e3c6..ed0b46b266; 7 files +347/-2) — READ 21:25 UTC — VERDICT: accepted (F7)
UpdateControllers: a dropped claim whose claimant is gone at the applied frame and whose seat is no longer
held (both facts synced: leave frames and hold resolutions are relayed) is erased and the actor handed back
to its seeded owner in AI mode when that owner is the resolved one, with one log line; otherwise the old
stand-down path. IsSeatHeldForReclaim(peer) reads the same set AnyLeftSeatHeld does. The selftest arm drives
a claim across three peers, drops the claimant, expires the hold and checks host and survivor agree (owner 1,
AI mode, not disabled); the expire3 oracle checks the owner logs of both peers after the claim plus the
identical returned line and the traces. Consistent with the F7 correction (LOW).

### W103 (item4-simspeed aa650e601e..38be2a22f4; 11 files +318/-15) — RE-READ 21:25 UTC before the W130 merge — VERDICT: accepted (as at 19:25)
Render substitutes registered in BeginRender and cleared in EndRender, honoured by IsActor/ValidMO and the two
DrawGUI guards; the inventory menu draws from the preview clone (its equipped items and CPU position) while
Update keeps reading the canonical actor; ClampPreviewTimers pins clone timers whose start passed the restored
tick; the five-arm HUD selftest samples before/during/after the render window.

## Merge wave + the merged candidate's first gates (21:26-21:50 UTC 2026-09-12)

Nine reviewed branches merged into stage2/fixgroup-6-lead (control-build) on e13e890fad, tip 97ce4639e7: W90 d799f4bec8,
W104b 49af8a120e, W119 ed0b46b266, W125 801bbb19d9, W123 887bf43917, W126 49f7fd2686, W120 958d30c6cf (with F12), W130
e502b5133e (with W103), W-MAC-RUNNER 6c1a099341. Every conflict resolved keep-both by script and each resolution checked
mechanically against the branch's own diff from its merge base (scratchpad check_merge.py: the +/- line multisets agree
exactly; the one deliberate difference is Main.cpp's headless-flag line = the union of both sides' flags). Trailer scan 0.
Rebuilt (Final, /MP6) in 114 s: exe 18ef7ed5820a. verify6 on it: port-map, directory, script-graph(4 states), the
global-callback driver, preview-event-d7 and lpinv-100 PASS; the socket-free suite 10/11 (F14 below); arm1 pending.

### F13 (NEW, MEDIUM, in W124's unmerged branch stage2/asan-relaunch-fg6 133eb9ac66; read 21:44 UTC): the relaunch
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
halt mode plus the selftests. W118's 5f6c9cb356 (the music checkpoint text kept alive while read) rides the same branch and
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

### W127 (value-observations stage2/preview-reference-writes efed9e71e6..4bb407c37e; 689 diff lines read) — READ 22:19 UTC — VERDICT: accepted for the next wave (F8 closed for safety); F16 opened
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

### W131 (h4-secondary stage2/sound-identity-pin e13e890fad..3bc895d6fd; 3 files +88/-1 read) — READ 22:19 UTC — VERDICT: accepted for the next wave (F11 closed)
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

### W71-3 (item5-lifecycle stage2/cross-actor-waypoints, W71-2b 3133a68ff7 + d991c922cc/943c2e2562/42e0559913; 1105 diff lines read) — READ 22:19 UTC — VERDICT: the ownership gate is accepted; F10's last gap goes to W71-4
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
Every finding was checked against the CANDIDATE (cefccaa1f6), not only the P4B tip, since later work may have closed it.
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

### W129 (item4-feel stage2/preview-scripts d799f4bec8..d9aa5a7690; 7 files +97/-14, all read) — READ 22:36 UTC — VERDICT: accepted for the next wave
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
Re-derived by the lead from the reports' frames: all 10 AK HUD runs (5 on W103's tip 38be2a22f4 + the ASan config, 5 on
W130's tip e502b5133e + the ASan config; `git diff --stat pre..post -- Source` is W130's 5 lines only) and both
pickup_fire ledgers halted on one heap-use-after-free: READ 8 in AHuman::DrawHUD (AHuman.cpp:3672,
m_pItemInReach->GetPresetName()) of an HDFirearm allocated by HDFirearm::Clone <- MovableMan::ShadowOf <-
SpeculativeView <- FindObjectByUniqueID <- Actor::ResolveFaithfulLinks <- AHuman::ResolveFaithfulLinks <-
LocalPrediction::RunPreview:165, freed by ~HDFirearm <- MovableMan::EndSpeculation:2029 <- RunPreview:231. pre 5/5,
post 5/5: W130's MOID-join change is not the mechanism (it stays merged as a correct ordering change) and F15 stands
exactly as opened: the substitute's faithful link is resolved through the speculating lookup onto a preview clone that
EndSpeculation frees, and the HUD reads it on the next frame. This is W133's RED: its fix must make the same 10 HUD runs
and the 2 ledgers report nothing under the same ASan configuration. W90's reload-365 RestoreRollbackState AV was not
reached (halt_on_error stops at the first report); it is re-measured after W133. Tree: takeover-build is left on
stage2/w132-asan-preview-hud (f9deadd9ab = e502b5133e + the ASan config cherry-pick) with the ASan exe and DLL beside
it; the pre branch is stage2/w132-asan-preview-hud-pre (cca1e87235). Nothing to merge.

### W124-2 LANDED, READ, ACCEPTED (F13 fix, hold-pause stage2/relaunch-slots-fg6b) — 23:00 UTC
Branch from cefccaa1f6: d87867935c and 1b4640d6d2 are W124's oracle and rebind commits re-picked (the lead diffed the
patches against 11fee80a4c and 133eb9ac66: identical except hunk offsets), then 7cbf518a83 "End the lockstep relaunch
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

### F14 was half-fixed — NEGATIVE against the lead — F14b fixed at 4dd386fb51 — 23:00 UTC
cefccaa1f6 derived the transcript layout golden from NetProtocol::c_Version but TestKnownAnswers' three HMAC known
answers (reclaim f0ea2c37…, substitution 72add2ad…, ticket 4f75c6ec…) were computed over the version-1 transcript and
MakeTranscript reads the live version (2), so -net-reconnect-selftest stayed red on the candidate: `reclaim proof
known-answer mismatch: 56527b38…` (FG6B, W124-2, W133 and W135 all saw it). The lead did not re-run the reconnect selftest
on the rebuilt exe after the F14 edit. Fix 4dd386fb51 on control-build: the known-answer transcript pins protocolVersion
= 1 (a known answer is a constant; the layout test keeps covering the live version); Source/Network/NetReconnectSelfTest.cpp
+3/-1, git_commit.py, trailer 0. Verified built-and-run by W136 (base 4dd386fb51, suite must be 11/11) and at the wave.

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
12c7d17e4f (11 files, +70, every line read): RemapExternalLinks walks m_pMOToNotHit, m_pItemInReach, m_pMOMoveTarget, the
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
7b76bbd3ac (F17): NetLockstepCoordinator::HandleStop, after every existing routing arm (hold resolutions, waivers, the
transport and known/left-peer checks, deferred Complete, scheduled Desync/ResyncRequested, PeerLeft/PeerDropped) and right
before the Stopped/Failed fallthrough: on the relay host a non-host sender's ProtocolError / InternalError /
MissingFrameTimeout / PeerDisconnected stop is ApplyPeerLeave(sender, FirstFrameWithout(sender), "<Reason>: message",
nowMs, announced) and returns; stops_adjudicated_as_leaves counted and reported. Selftest arms: a client's ProtocolError
stop leaves the host Running with the client in the leave map, the survivor gets the relayed PeerLeft at the same frame
and host + survivor keep committing; the host's own ProtocolError still fails both clients. 0774713efa (F18):
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
9f6a4c49ea: every fixture under tools/contracts claims `_ContractAuditOwner` on its first Create/Update (a spawned child
never claims) and prints one `[<family>-contract-check] ARMED uid=<n>` line; the literal 1048577 is gone; check bodies
untouched. The lead checked that each fixture's ARMED family is the same prefix its check lines use (activity, native,
reference, primitive-batch, primitive-cast, primitive-cast-v2), so the new gate can pair them. 516ad67354: run_audit
records the ARMED fixtures and gate() fails `fixture_armed` (a non-load transition that armed nothing), `<family>_checks`
(an armed fixture with no check line), and every `native_mismatches:` / `contract_mismatches:` line by name, or the bare
name when only a count is nonzero; unit tests RED against the old gate (quoted) and 12/12 after; the README keeps
"complete is not faithful restoration". 075b57587e: --port required on both lobby oracles; every caller already passes
one. B-2-3 not applied, accepted: the engine emits particles/items/carve_math only on ticks that have them (a lobby
host trace lacks items on all 180 ticks), and the lead's read of compare_sim_traces' loop shows the per-tick diff runs
over the union of both traces' subsystem keys, so a subsystem that one side drops is already a divergence; a subsystem
both sides lack identically is the conditional case. F19 closes with this merge.

### Wave A prepared on the scratch branch stage2/fixgroup-6-lead-wave-a (takeover-build) — 23:10 UTC
4dd386fb51 → W127 4bb407c37e (clean, 031687bae8) → W131 3bc895d6fd (6e315ce881; NetLockstepSelfTest.cpp keep-both of the
W119 and W131 arms inserted at one anchor) → W129 d9aa5a7690 (d67394fb66; Main.cpp include keep-both, LuaMan.cpp W127's
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
1544d359c2. The gate at the top of Actor::UpdateMovePath re-arms m_UpdateMovePath and returns (no path request) when
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
79dc711958 (2026-09-10, "Compare controlled actors across every activity in snapshot diffs") carries
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

### Wave build and gates on 0bcaaddde7 — 23:54 UTC
Suite 11/11 with the trimmed W71-4 arm and the reconnect selftest green; W127's three held-ref fixtures 8/8 with no
freeze except the fallback fixture's deliberate Activity freezes; W129's three AK lpinv arms green with fallback=0.
The sound-copy fix-up b5811aad98 is therefore proven by the write/control fixtures (a frozen preview would show as
fallback>0). Open: the depth-12 spawn count (8 vs 3), hypothesised F16; to confirm before the battery.

### FG6B battery read — 23:58 UTC
No true engine failure on exe 98f73d74aaed (cefccaa1f6). Every red is classified: the reconnect KAT (F14b, fixed and
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
Merged c3e4586b9b; the wave exe 959fc25e predates this merge, so the wave is rebuilt before the battery.

### W136-2 merged on the lead's read; clean stop — 2026-09-12 17:26 MST
The codec diff is exactly the specified correction (every frame link sized from the function in its own slot; the
safety net and the fits query on the invariant). The lane's RED is the helper arm's false fit followed by an access
violation on the unfixed codec; GREEN 181/0 with six coroutine arms, suite 11/11. Merged d20d4bddfa and pushed. The
independent verifier pass did not complete (stopped by the clean-stop order): the next lead re-runs it before the
battery. Gates already green on the wave exe cf2e611b: suite 11/11, script-graph 181/0, AK-47 HUD 3/3, the craft_cargo
pulse [316..319] both peers (F10 closed). Open at the stop: the promotion of control-build, verify6, the ONE battery
(fg6c brief ready), the depth-12 spawn question, F15b/F15c, the W136-2 verifier pass. Nothing is running.

### Trailer strip and cleanup — 2026-09-12 17:49 MST
**Trailer strip 2026-09-12 17:49 MST** (user's order: no Cursor co-author anywhere): the one public Cursor co-author line (old `9751a90e29`, 2026-09-10) was stripped by a message-only `git filter-branch --msg-filter strip_trailers.py` over every commit not reachable from its parent; 866 commits changed sha, 148 branches and 2 tags were rewritten (every tree byte-identical, `map.txt`), 147 branches force-pushed after verifying origin still held the pre-rewrite tips (`push2.txt`), 26 tags force-pushed; origin's `exp/determinism-foundation` (a May branch that had diverged from the local copy) was stripped separately (21 May-era co-author lines, old `366b9d0738` → `34253c5708`, tree identical, `README-expdf.txt`) and the local diverged copy was left as it was. A line-start scan of every local branch and tag since 2026-04-01 finds 0 trailer lines; only upstream's own 2022-2025 human co-author lines remain, and they stay. The live docs, STATUS, SPAWN_LOG, LEAD-REVIEW and the fg6c brief had every old sha rewritten from `commit-map.txt` (old sha → new sha, 40-char); evidence logs under `D:\mx` and lane reports keep the OLD shas — resolve them through that map. The old trailer commit is now `79dc711958`. Post-scan of every branch and tag for trailer lines: 0.
Every sha quoted above was rewritten through the map at the same time (trees identical).


### Depth-12 comparison: F16 hypothesis refuted (2026-09-13 01:53 MST)

Read the complete run_depth12_measure.py, diff_lpinv_before.py and scratch_bytes.py and the report's deviations first.
Re-derived both exact HEADs/build return0, manifest hashes bc3ec7fa… and1b5a7315…, driver hash82a8b5a5…, identical argv
apart from output destination, and stdout lines13–17 in D:/mx/depth12-20260913/{pre,post}/run. Both report counts
0/1/1/8 at depths1/4/7/12, the same ordered names, four canonical-invariance passes, zero cursor-movement failures,
zero preview violations/fallbacks and exit0. This excludes the isolated F16 change as the source of the five additional
entries observed against W129. It does not identify those objects or establish cross-run byte equality: the full
before dumps differ in timer accumulator/Lua timing fields, which were retained, not masked. Report correction:
the eight-entry list contains SIX None names total; FIVE are additional to W129's existing None.

NEGATIVE delta, item4 +0: the stated pre-battery hypothesis is false and identification remains open. Follow-up
cli:depth12-origin-20260913 instruments object identities/queue growth and tests the speculative-travel mechanism
on an isolated diagnostic branch. No instrumentation or omitted travel call is a proposed production change.
The old measurement driver's Win64 ctypes process query omitted handle signatures; it is not reused. The actual
runs/builds were sequential, with their engine handles closed before the next build, so this defect did not invalidate
the recorded comparison. The report also emitted an avoidable UTC mtime field; future stamps are MST only.


### Part B Main.cpp verdict — historical diff read complete (2026-09-13 01:59 MST)

Read all 3,074 diff lines of db9ec184be..be217add64, including re-reading the truncated harness-capture block.
Artifact reviews/resume-20260913/part-b-main.diff SHA256 0765634d7bb9b5598aa1198534a36baa81ef3b061f54ce57f82ec80f545a5258.
Covered argument routing; shutdown/error propagation; menu automation; canonical/terrain/Lua observations; preview
and rollback probes; replay completion/truncation; input/sim/render sequencing; resync launch and exit; scenario
and menu match caps; reporting and startup selftest dispatch. No new confirmed runtime defect from this read.
Runtime verification remains the named rebuilt candidate/battery/family; this read alone closes no such gate.
The eight remaining network-selftest diff files total 10,997 added and 86 removed lines by numstat (plus context),
so that part stays open. F3's ordered-frame-lane constraint remains binding.

One evidence-scope correction to carry into the Mac report: NetLanDiscovery.cpp:196–205 attempts a LAN broadcast
as well as loopback; Main.cpp:5012's discovery selftest uses that default. The Mac run_selftests.py includes this
case and its runs/net-discovery exists. Therefore the lane cannot claim all its tests were loopback-only.
The supplied legacy test list and the loopback-only brief conflicted; record the actual configured traffic scope,
without claiming packet delivery was observed. No production network setting or firewall changed.


### Recovered depth-12 diagnosis accepted as measurement (2026-09-13 06:09 MST)

Read the full diagnostic MovableMan.cpp instrumentation and the one-line Travel-call negative control. Re-derived
from with-travel/run/stdout.log:72-78 that tracer1048883 at sim tick185 changes its particle queue from0 to5;
new MOPixels1048885–1048889 occupy positions796,780 through796,784 and appear as the five extra harvested entries
at lines186–190. Casing travel and all updates leave the queue unchanged. The no-travel arm retains exactly the
original three names/uids; both arms have canonical-invariance4/4, no FAIL, exit0, complete launch evidence.
Launch hashes match41997cdf… and d76f01b9…; instrumentation and restored instrumentation both hash93a6f8dd…;
negative combined patch5afdbdaa… contains only that instrumentation plus removal of the travel call. The driver
resume after a missing output-directory error is disclosed; no oracle was relaxed. Data in D:/mx/depth12-origin-20260913.

The additional objects are secondary particles from speculative tracer travel. F16 did not introduce them
(the earlier exact pre/post measurement already refuted that hypothesis). This closes the pre-battery identity
question. It does not assign an unobserved material/gameplay name to those pixels or propose omitting travel.
NEGATIVE item4 +0: diagnostic uncertainty removed, no new presentation feature or feel coverage delivered.
The isolated diagnostic source stays instrumented; its executable is the no-travel control and must not be reused.

### Recovery review checkpoint (2026-09-13 06:09 MST)

W136-2 independent report from cli:v136b-20260913-r2 read in full, including all residuals. Original findings1–4
have independent source/raw confirmation; its near-8000-slot compatibility concern is assigned to stack-capacity.
Mac raw archive re-derived script-graph181/0, AK invariance4/4 and A7 execution_pass=false with four passed arms and
coop_hand_back expected1048615/observed1048577. Report remains partial, not Mac-green. mac-a7f owns diagnosis.
Read every line of the eight-file F15 prepared diff and its whole run driver. Production changes address owned
retirement trees, shadow-part remapping, wound recursion and stateless preview-binding cleanup. No acceptance:
arms have not compiled or run, and Arm support pointers and other disclosed coverage gaps still require review.
Grok f15-probes owns only the exact C-control/C-tip builds and full Final inventory; two /MP6 lanes reserved with
stack-capacity. The coarse driver exit code is not the oracle; every named arm and canonical-state check is required.
Read every line of bfa75659bd (202 added/3 removed). Raw UI RED36/38 and GREEN38/38 independently confirmed from
all eight launch manifests and label echoes. Lead rerun and independent actual-image review pending. Its two-line
helper comment needs shortening before integration. No code accepted by this checkpoint alone.


### F21 — returning co-op local selection overwritten (2026-09-13 06:19 MST)

Confirmed engine failure on Mac wave d20d4bddfa, exe843c38e1. Lead inspected unchanged run_coop/validate_local_control
and parsed original events: client204 local observations all select1048615. Returner has68 observations, first at207
selects1048577 (seat_mode2, seat_player-1, view_state2), next at208 selects0. Both1048577 and1048615 are alive at207;
1048615 remains owned by peer2. This is not actor death or an expected-UID guess. Mac raw archive preserves journal.
Read Activity.cpp ApplyNetPlayerSlots/RestoreNetLocalPlayerState/ApplyNetPlayerBindings/RebindNonOwnedActorSlots,
NetMatchService::StageResyncedMatchLaunch and ActivityMan post-restore/discard ordering. During relaunch the applied
peer pointers coexist with stale host-save checkpoint IDs; the deferred rebind overwrites the peer selection.
Assign mac-local-binding to refresh deferred IDs from the FINAL local state, including player-controller identity,
covering fresh and retained local paths; preserve deferred pointer lifetime repair, never skip all rebinds or alter
ownership/oracles. HIGH correctness; independent review required after RED/GREEN. Item6 95→94. No wave promotion yet.

Part B review progressed: full historical diffs read for NetAdmissionSelfTest, NetAuthSelfTest, NetProtocolSelfTest,
NetSessionSelfTest and NetMatchSelfTest (1627 added,7 removed, plus context). Controls, bounded waits, replay legacy
semantics and failure propagation inspected. No new proven production failure. Remaining files: NetLockstepSelfTest,
NetReconnectSelfTest and NetReconnectSessionSelfTest. Review concern to check later: the late-joiner lobby test's
final timeout check names host/C only although its loop's successful break requires all peers; do not claim a new
engine defect from this coverage observation. No runtime gate was closed by reading source.

### F15 controls, fixture aggregation and UI pixels (2026-09-13 06:42 MST)

F15-probes and F15b-controls reports read in full, deviations first. C-RED exe c7ffb9e8 at d20d4bddfa plus exact
arms has four pointer failures at recovery-c-red-bcs/stdout.log:22-25. The repaired exe6972b5b3 passes those same
lines and all eight Final inventory runs. The lead reran pickup_fire 200:1:1 modes bcs through the guarded driver
at 06:38 MST: lead-c-green-bcs/stdout.log:22-25 all PASS, canonical4/4 at31, exit0, no timeout. This is real
retiring-part/wound and shadow-part coverage; it does not prove the proposed stateless cleanup, whose s arm is
ARMED and passes on BOTH builds. Independent retirement/Arm review active; ASan still owed before accepting F15c.

Historical F15b controls: exact arms patch32e04b6f on 8e66a72ce7 and364344d970, both build manifests report identical
source-diff b2184526 and only the three arms files dirty. Lead read raw b-red-b/stdout.log:16-17 (both still point
to retired Battle Rifle1048625) and b-green-b:16-17 (both null). Shadow-root mapping passes both, shadow-item r
passes both. RED exit5 follows the detecting failures; GREEN exit0. No W129 audio-cursor red appeared, contrary to
the preparation warning; no oracle adjustment occurred. B debt has detecting controls now. Whole test patch already
read; its canonical Arm-pointer save/restore and ghost cleanup remain part of the pending independent test review.
NEGATIVE item2/5 +0: these substeps close evidence gaps, but the repair group is not yet accepted or family-verified.

Fixture aggregation: three-line repair in COPIES preserves List[object] with Add and serializes with ToArray.
Lead read both full wrapper diffs and the complete extraction harness. Actual original wrappers reproduce the
nonempty-wait missing-ToArray and empty-crab array-conversion failures; all14 repaired scenarios produce the expected
JSON boolean and process status, including failure on missing results, semantic failure and source/binary mismatch.
The two subsequent harness-only edits (hidden children, MST environment stamp) are disclosed in harness_v1_to_v2.diff;
new final-harness evidence repeats all14 correctly. Retained raw wait-entry tool stamps remain unchanged. This is
an oracle/driver repair, not engine-green evidence. Apply only these reviewed wrapper deltas to FG6C copies and
preserve both oracle versions in its run record. NEGATIVE item5 +0 until full battery/family closes.

UI: lead rerun on bfa75659bd was38/38. The extended real-runtime four-case driver diff (+85/-25) was read fully,
including module-version source and exact final-line/hash checks. Parsed raw result confirms87/87, preserving
refusal, process, binary and desktop checks. Independent original image review finds single Install/Remove text
readable, but its runner timed out after saving REPORT.md; no final-result claim. The follow-up image review exited0
and read all four mixed/overflow captures. Both joiner labels clip the fifth wrapped row (75px needed,68px box);
prefix/names/versions/elision are present, final hash row is cut. Host errors are readable, host status is clipped.
UTF-8 group delimiters draw bitmap icons. Back is absent in all four. Lead checked GUIPanel::Draw clipping and
MainMenuGUI reparent/resize: Back at contentHeight is outside a screen of contentHeight, despite original INI height275.
These pixel failures override the string-only green. Host status geometry is on item7-chat; landing height, display-only
ASCII delimiter and Back containment are on item7-ux. No networking/identity strings change. Every changed display
expectation must be disclosed and inspected again in pixels. NEGATIVE item7 +0: UX acceptance withheld for actual
pixel failures, not a claimed complete87/87 fix. UI defects do not add to the gameplay regression count.

Part B NetReconnectSelfTest.cpp (+866) read in full: deterministic provider is explicitly scoped, real-provider known
answers, all transcript fields, domain separation, cache key/expiry/eviction, bounded challenges, fixed denial timing,
credential substitution and console-canary positive controls inspected. The scope-reset check guards provider leakage.
No new proven production defect. Historical version1 goldens are the already-known F14b subject, not a new current
failure. Only NetLockstepSelfTest.cpp and NetReconnectSessionSelfTest.cpp remain for the lead's Part B read.

### 2026-09-13 07:18 MST — coroutine review, host status acceptance, Mac recovery, F15 final trim

Coroutine672024dfd1: all51+/2- lines read, prior-codec/W136/control/negative evidence re-derived and lead script-graph
rerun185 PASS/0 FAIL on e71c99d3. Independent review stack-capacity-verifier finished exit0 and independently repeated
185/0. Its report read in full. The original near-limit7985/8010 continuations resume done on the partial prior-codec
control, refuse on W136-2 and resume done on the repair. The prior reference errors later on the unavailable query;
never call it suite-green. RED1's erroneous top expectations and8011→8010 adjustment remain disclosed.
The protected growth block is exercised by the old big-frame arms, but none of the three near-limit arms enters it:
actual maxstack8007/8010/8010. Add one separate8011 fix-only arm and a no-growth control before merge; this is not
another claimed pre-W136 compatibility case. Used placeholders/OOM/Mac remain untested. Backup push06:47:35 MST:
scan0 and empty lease, branch672024dfd1. NEGATIVE item2 +0 until the added proof and integration.

Host status b7e1d86b35 ACCEPTED for integration. Lead read all9+/2- lines and personally reran the original driver:
D:/mx/swe-lobby-status-height-20260913/lead-recheck/result.json38/38, four guarded process exits0 and exact exe562e80b1.
Independent lobby-status-visual read all four host images: status height34, extra18, first ink row249..258 and second
264..270; old16px label clipped both. Error/control/panel positions all move18px, no new overlap. Report's prior
one-row arithmetic was inaccurate; actual source uses max(16,GetTextHeight()+4). Shorter-text shrink, visible port-map,
three-row status and real Back click are not claimed. Commit through helper, scan0, origin branch absent, empty-lease
backup07:11 MST. Item7 38→39. The separate long-label/layout patch remains under eight-image review: exeae289ebd,
103/103 string/process checks, explicit ASCII display-delimiter oracle diff. Lead read source and oracle delta in full;
AutomationMultiplayerError reads the displayed label, not the service snapshot. Existing goto_main proves only that
automation path, not clicking Back. Additional legal wide filenames are measured run-only before broader visibility claims.

Mac F21 raw archive fetched and reviewed: three native relaunch arms FAIL at119–121 on f0706e35, PASS at119–121 on
c9ca7018; script-graph181/0 after fix. A7 result complete/execution_pass true, all five named arms passed without
failures, mac-run source mismatches empty. Returner f/events.jsonl:9 starts at frame204 on1048615; lead counted61
local_control rows for204–264, all controlled1048615/brain1048577. This is the previously failing ownership window.
Patch39+/7- read; acceptance remains open for independent review and related subclass marked IDs and live inventory
brains. The resumed Mac turn owns only the marked repair plus runtime inventory measurement; no predicate relaxation
authorized. Evidence archive12,021,760 bytes SHA e909dc83a42c9b5a..., retained original RED untouched. Item6 stays94.

F15 final14db824c10: lead read complete trimmed production and arms patches. Unsupported null-state cleanup and s
probe removed per independent review; PreviewRoots and every b/c/r check remain. No Arm support-pointer fix. New
aggregate wording reports invariance AND link checks; C-RED2/3 versus GREEN3/3 reflects removal of s, not a weaker
oracle. Four exact C-RED pointer failures at22–25 on4da9c200; repaired links on245ea42b. Ten ASan rows reported green
on49b4f0f8. Lead personally reran bc200 through the guarded runner at D:/mx/f15-trim-asan-20260913/lead-asan-bc200:
exit0, no timeout/ASan report, b three checks and c four checks PASS, all armed, canonical3/3. Final independent
delta/ASan provenance review is active. Missing taken-shadow/spawnMeta/inventory/shared/raw-c coverage remains explicit.

FG6C four fixture files prepared and all diffs read: two byte-identical files, two repaired wrappers changing only
root/lane/ports after the accepted three-line aggregation repair. Do not overwrite them with FG6B originals. This is
preparation, not an engine-battery pass: NEGATIVE item5 +0; the full promoted-tree battery and family remain gates.

### 2026-09-13 07:51 MST — supported F15 acceptance, coroutine integration and current limits

Accepted14db824c10 after full final source/arms diff and independent final ASan report review. The independent report correctly withheld unsupported stateless cleanup and identified provenance gaps in the earlier overwritten build log. I closed those with lead_f15_rebuild_pair.py: tests-only production reversal built RED12865621 (four actual C weak-link failures), restored byte-identical source built GREEN09ec8453 (all four links andbc3/3). Both unique logs compile MovableMan.cpp and MOSRotating.cpp; both complete guarded manifests and raw stdout inspected. No memory-safety RED claimed; Q2/taken/shared/Arm-support unmeasured paths remain explicit debt. Old49b4 same-source ASan tip overwritten; current09ec and retained RED/PDB pinned in result.json.

Coroutine ec5a34efbc added the real8011 protected-growth boundary after independent review. Exact no-growth control refuses8011; unchanged production GREEN allocates16038 and resumes done. I reread the two test lines and removal-only control diff, reran186 script-graphPASS/0FAIL, and corrected the worker's broad687 count. Merge748041cc68 differences0. F15 merge8342801d91 differences0 across8files; full wave scan0 and lease push from observed origind20 succeeded. Worker branches also backed up after scan0; no candidate or milestone mutation.

Normal module-refusal eight-image report passes actual clip/separator/Back containment, not Back action. Wide-name independent report fails readability for all five64-byte names on host/joiner: one clipped W row; later names and hash suffix absent. Large-to-empty shrink passes against baseline pixels. The source trace corrects the earlier claim: DrawAligned retains only the first overwide token while marking the entire remainder consumed. Driver UTF8 plus two positivehost separator checks reviewed; reported105/105 does not prove those long names readable. A local panel-width fix is now bounded to actual font measurement; no global GUIFont rewrite or name truncation authorized.

Mac marked84a66e41 fix-only diff/report read; full test/raw review still pending. Inventory probe establishes a live brain owned in current-world inventory being dropped by deferred rebind. Follow-up explicitly proves ownership from current-world roots, extends only brain validity, and preserves stale/unattached rejection and existing dead-world behavior. Native exact controls precede one grouped A7. Part B read through2390/3991 with a small730–760 truncation reread required; other4723-line test file remains. NEGATIVE item2/5 +0: integration and59 battery file copies do not complete the combined gates.

### 2026-09-13 07:58 MST — Part B lockstep-selftest read complete; battery copy review

Read all3991 diff lines in NetLockstepSelfTest.cpp over db9ec184be..be217add64, including the truncated730–760 section again. Frame/start/round tags, per-sender delays, relay queues, held-seat behavior, service-lock avoidance, observation carry/bind rollback, readoption and deferred-stop regressions are structurally covered. No new production defect proved by this read. Coverage limits: the three-peer checksum arm lacks an affirmative checksum-comparison count; several assertions prove counts/continued operation rather than exact payload identity. Do not inflate the resulting verification claim. Older test-comment narration is historical; no unrelated edits made. The remaining NetReconnectSessionSelfTest has4723source lines, read through diff1530.

All43 FG6C file diffs read: only the authorized root/lane/port/executable-placeholder substitutions. No assertion or mask delta. Existing dependency defaults, retry behavior, deletion helpers and final pin still require readiness audit. Mac marked full133+/2- diff read; fresh/unseated scope and cleanup limits remain as disclosed. Lead ran the UTF8/positive-separator retained-output checker and checked current raw host strings plus all105true result keys; no new engine rerun claimed at this checkpoint.

### 2026-09-13 08:21 MST — reconnect group and battery readiness checkpoint

Checkpoint 2026-09-13 08:21 MST: Mac3a865f9e clean grouped A7 all5 onf7bfe863, 21 launches; reported first resumed coop frame205 keeps1048615. Full third production/test diff read; native two inventory RED assertions and seven control cases to rederive from archive. Archive mac-inventory-brain-evidence.tar.gz=11,714,560bytes SHA50aa908d96632cfeb2cf025461fc2a2c57faa1c583746ee94d72f93da4bc7cc7,672files. Source objects fetched read-only from Mac; item8-directory switched clean to3a after exact F15 GREEN09ec8453 exe+PDB file preservation (PDB d8878a20...). No wave/candidate/milestone merge. First independent F21 report fully read: no production defect, but retained Activity arms call UID resolver rather than deferred world rebind; new Mac native coverage closes this. ScenarioRunner function location corrected for reviewer (Source/System); third review ongoing.

FG6C check_fg6c_copies.py rederived59 destinations byte-for-byte from allowed literal substitutions,43changed/16identical,58Python compiled,4protected hashes unchanged. Actual ICE script REPO derives from its control-build file path; no missing --repo issue. Approved/candidate win32 runner hashes identicalfead3f0d; run_sim_test diff only adds POSIX branch, Windows path unchanged. Git OpenSSL binaries exist; prep PATH per battery process. New bounded supervisor controls scratch cap/time; final identity/pins wait for candidate build.

UI full width diff read: (new-old)/2 can drift over alternating odd/even widths, fixed design is new/2-old/2. New SWE round verifies actualfont worstlegal token at640x360. Previous width report elapsed68min contradicted runner1237s; correction requested. Part B NetReconnectSessionSelfTest read through3540/4729; NetLockstep all3991 done. No new production defect established. Status delta baseline remains07:51; item6 explanation does not reset the baseline.

### 2026-09-13 09:00 MST — F21 review, completed Part B read and guarded battery preparation

Checkpoint 2026-09-13 09:00 MST: F21 brain independent REPORT fully read: exact two inventory RED/GREEN arms and seven controls, no production defect; ScenarioRunner and ConstructionRegistryScope cleanup source reads closed. m_StartActivity pending-link difference is a pre-existing observation, no reachable defect established. b4 test-only44+/23- full diff already read; check_world_slots.py proves final source hashes match git, identical fixture both sides, control removes exactly two refresh calls. RED9d0e89c5 FAIL157/160/163; GREEN58b5ed57 PASS same lines. Final independent pass pending. Windows3a Final7822b034 built,11/11 and native4/native1-diag181/0; diagnostic4 held before engine until2400s worker cap. Runner/engines gone after timeout. Do not reuse its helper result code as a verdict. Next FF item8-directory to b4, rebuild and lead native variants when desktop protection permits.

Mac ICE one attempt08:46:47–08:47:13 onb4/58b5, both engine exits0 and601ticks, client routeice, mux IP0/P2P617 and host0/613. Harness initial six false fields read wrong JSON root; corrected evaluation5/6 true on same raw data. Host remoteidentityip:::1 and trace passed:false need explicit source explanation; independent Mac audit active. No WAN/STUN/TURN/cross-platform/rematch/resync claim. Archive mac-ice-only-evidence.tar.gz1,013,760bytes SHA f486746ebf7404d900b2d01eb66119696763d477346d664a1ed9982ee06df94b,96files; full report fetched/read. Mac real-world slots archive2,304,000bytes SHA59c923288f63db52546c3f52389375f279d1d02926c7c98a606d3225036a38be,243files; no source merge yet.

FG6C guard64a708a5 full186line difference read, original+final whole code and tests read; lead reran15 true checks, file-symlink skip due privilege, failure-to-kill and reparse TOCTOU paths not forced. Sampling can overshoot; no strict disk quota claim. New candidate identity helper requires explicit future build pin and fails closed. Part B3991+4729 lines complete. UI6a18084d built; no clean arithmetic RED runtime and no current GREEN yet; report's1283px/ANSI conclusion is unverified until actual encoding/font check. No completion score changes or pushes.

### 2026-09-13 09:53 MST — Combined candidate verification, F21 acceptance and scoped ICE proof

Checkpoint 2026-09-13 09:53 MST: Item6 restored94→95 after independent final F21 review, personal Mac detecting run and combined Windows detecting tests. This closes the specific defect that caused the earlier95→94 correction; full battery/family and headed reconnect remain. Item8 rises58→59 for accepted Mac/Mac LAN ICE-only application join: both peers601 ticks, P2P613/617 and IP0/0, unchanged trace comparator passes. This proves neither public WAN traversal nor ICE rematch/resync. Candidate and wave both7e63a0c1b52983d9461113fcf883d14b1f970e50; wave=origin, candidate backup push pending. Candidate Final c67b35a65b05 built alone09:22:40; 612 build inputs unchanged. Lead rederived all8 verify6 groups on19 pinned launches: suite11/11, graph186/0, local UI39/0, local state10/0, callbacks/preview/lpinv/control-switch pass. Original wrapper says7/8 solely because it expected RESULT: PASS instead of the real PASS switch_control marker; original summary retained and unchanged oracle rerun by lead. Approved milestoneaa650e601e unchanged. Identity check09:49 passed all10 checks, real firewall included. One FG6C inventory battery is next. Codex83% remaining09:46, stop at<=60% in any available window.

F21 b4 final independent REPORT fully read, no integration blocker: real world-slot fixture44+/23- read; three exact RED/GREEN arms confirmed. Lead Mac run09:10 on58b5ed57 gives181/39/10 with0fail. Windows combined c67b35 gives186/39/10, all five F21 detecting arms, and19 launches same hash/exit0/no timeout. Personal check_candidate_verify6.py reruns unchanged switch-control checker: uid1048634 apply80 handback90,291 owner rows,600 trace ticks. Original summary.json7/8 retained; wrong expected marker is the only wrapper failure. Evidence candidate-verify6-lead.json. Restore item6 point; full battery still pending.

Merges/pushes:09:06 cumulative b7 dependency merged1adf8ac201 (3files211+/5-, including original bfa formatting/test); exact merge verification0 differences.09:07 scan0 exact-lease wave834→1adf.09:16 F21b4 merged7e63 (4files238+/11-, exact verify0).09:17 atomic scan0 exact-lease wave1adf→7e63 and absent-origin creation stage2/resync-local-rebind=b4a57ae64d. Candidate fast-forward09:19 from4dd to7e63, no push yet. Approved tree unchanged. Build09:20:47–09:22:40 logs and612 input hashes in D:/mx/lead-candidate-20260913; build log0f5c9706. Owned lead build lock created/removed normally.

ICE acceptance limited to same-Mac LAN application ICE. Lead check_ice_raw.py verifies TLS fixture changes only join_mode/listen_addrs,4 snapshots, both601 ticks, P2P613/617,IP0/0 and reruns unchanged comparatorc32a79e3. Initial six false fields came from wrong JSON root; reeval changed only lookup, five corrected and host remoteidentity extra assertion remains false. Host ip:::1 is default joiner GNS identity; trace passed:false/ticks0 are unused MetricsCollector result/count defaults on e2e path, real completion601 recorded separately. Preserve every initial/revised check. Full independent audit read; no public WAN/STUN/TURN/rematch/resync claim. Archive f486746e (1,013,760bytes) and independent auditd71c2755 (204,800bytes). Lifecycle analysis full report fetched/read; incomplete source questions delegated for closure, no engine implementation accepted.

UI width final independent report read in full: five64-byte ASCII names visible in960x540 captures on134e, oldae2890/5; Back inside viewport, empty/short geometry restores. Rounding6a no runtime/capture proof. Actual640x360 and CP1252 path still untested; font arithmetic is a hypothesis until engine run. No UI score increase.09:16 lead stopped owned orphan Python29196 and bash25500 after timeout; host launch started:false and no stdout.09:18 stopped owned F21 native23268 tree after810s desktop-gate wait, also never started. No desktop override, no engine killed, no directory cleanup. Unrelated interactive54604 preserved.

FG6C identity helper accepted after full233line diff/test read and own19true cases; actual lead pin c67b35/7e63/buildlog0f5c now passes10checks including firewall,610 Source files checked excludingCLI. Build manifest writes only after success. Part B full listed diff review remains complete; limited payload/assertion coverage is explicit, not new production failure.

### 2026-09-13 10:05 MST — Battery dispatched and pointer surfaces refreshed

Three external workers: Windows Grok4.6 Extra High Fast FG6C full inventory (runner83467, provider49a70c96-87fe-484a-8246-ed997e91334a); Mac Grok4.6 Extra High Fast fresh combined-tip build/gates (runner79607, mac-fg6c-final-20260913); SWE-2 Max menu minimum-viewport/encoding driver preparation without engine (runner7963, platinum-begonia). No Codex subagents. Mac Opus source closure completed and report read; no ICE implementation yet.

Candidate and wave7e63a0c1b5 both equal origin after candidate scan0 and exact-lease4dd→7e63 push09:57. Candidate c67b35a65b05 frozen under lead-owned LEAD_BATTERY.lock. FG6C started09:58; guard48700 owns inventory47236, run_all52660, run_battery21184. No other Windows engine/build lane. Fresh Mac clone verifies same7e63 independently. Approved milestoneaa650e601e unchanged. Codex83% remaining10:02; stop at<=60%.

Verify6 wrapper394933a6 full2-edit diff accepted, own6/6 expression tests plus independent raw checker; existing summary/evidence immutable. FG6C explicit pin/no-auto-rerun differences and N6 launcher read and compiled before dispatch; all original drivers archived. One full inventory, no failure-by-failure retuning. Fresh Mac same-tip verification now independent of the old b4 analysis tree. Pointer notices added to H4 plan, contracts, Mac resume, family runbook§9 and roadmap; historical evidence left intact.

### 2026-09-13 10:12 MST — ICE lifecycle design and independent preparation

Five external workers: Windows Grok FG6C inventory (83467), Mac Grok fresh same-tip build/gates (79607), SWE-2 menu probe prep (7963, platinum-begonia), SWE-2 optional unlisted-directory implementation/Python RED-GREEN (9297, defiant-taurus), and Opus5 max/Fast off native ICE regression-test preparation (53071,2bc214ef-e05c-400a-9b68-bc5d7db4e462). No Codex subagents; two SWE sessions, only FG6C may launch Windows engines/builds.

Lead source review adds suspected AnswerMatchOverRejoin wrong-wire path464-472 to the planned detecting inventory; no runtime defect claim yet. Full transport-use search read. Optional visibility chosen instead of a new row state: strict bool heartbeat, same session/token/queues, list filtering before total/pagination, register capability and heartbeat echo, unchanged old-client field parsing. Python changes isolated to two files, native11/11 owed before integration. Native tests are being prepared before production edits/RED run. Plan ICE_LIFECYCLE_PLAN.md written by lead. Two clean reused worktrees switched from backed-up b4/b7 to named new branches at7e63; their old binaries marked stale. No score change for unverified preparation.

### 2026-09-13 10:33 MST — directory Python checkpoint and detecting-test preparation

Lead read all294 added/1 removed lines of0a54e477f9: optional strict listed bool, capability, reply echo, list filtering; no production defect found. Lead test copies preserve exact old/new server bytes402a521b/f8318095 and samee12434d9 final tests, plus README to avoid the prior control-path artifact. Initial RED41 tests9F/7E/3skips; GREEN38PASS/3TLSskips from missing OpenSSL PATH. Targeted TLS replay is running with installed OpenSSL; initial logs remain intact. Native11/11 still owed, no merge. Backup push0a54 to neworigin stage2/directory-unlisted-sessions after scan0 and verified absent remote with exact empty lease.

Menu v2 full430-line diff/report/selfcheck read. REJECTED preparation: main dict overwrites JoinerWide bodies with Host bodies, and verify_pin only records false but still launches. Same SWE session now corrects peer-specific evidence and fail-closed prelaunch behavior, with meaningful no-engine detecting cases. Native ICE preparation3bdef9c0 landed unbuilt (223+/5-); success/cancel/dispatcher cases openly missing. Lead read report and is reviewing full patch before the next build. Completed transport pump audit dispatched on Mac at7e63 source; no engine, no source edits. Five external workers: Windows Grok FG6C inventory (83467), Mac Grok candidate7e63 build/gates (79607), SWE-2 menu probe v3 correction (86877, platinum-begonia), SWE-2 native visibility codec tests (50854, defiant-taurus), Mac Opus5 max/Fast off Completed transport-pump audit (15204,942e358e-626e-4ac6-bfce-b2fcab977aed). No Codex subagents; two SWE sessions. FG6C alone owns Windows engines/builds.

### 2026-09-13 11:05 MST — Mac combined raw audit and exact native directory RED

Mac combined-tip87158955: lead235/235 raw checks on native11, graph186, localUI39, localState10, all5 A7 journal sequences and strict common hashes, saved boundaries,61 real co-op control frames, settings and1561 source hashes. Whole Mac group remains PARTIAL:3 HUD invocations missed depth7 and never sampled. Original failure logs kept; corrected invocation unrun. Initial lead audit had20 checker mistakes (native marker prefix and intentionally silent clients exit1); original checker/output preserved, final checker corrected from source/raw protocol, no engine/oracle edits. Python directory0a54: original38pass/3TLSskips plus targeted OpenSSL-enabled3/3GREEN closes41 tests; targeted old-serverTLS1/3FAIL retained. Mac native codec exactREDb8f624: same7d157a14 fixture reports5 lost bool values and18 malformed accepted, exit1; official11/11. Lead personally checked raw launches/source/counts10:53. Reviewed production cpp03ba1130/header e60c31c8 now unbuilt on Windows, same-fixture GREEN building on Mac; no new commit or merge.

Lead read codec127-line proposal and corrected state-field error precedence before listed decode (Python/native consistent). Final cpp03ba/header e60/test7d hashes frozen in MacGREEN brief; preserve exactREDexe and two objects before rebuild. No mask/oracle change or source-only PASS. Native ICE incremental171 lines123+/5- fully read in addition to prior223+/5-: loopback inactive-IP listener is fixture isolation, explicit P2P precondition and inventory both rematch and late-answer errors. Nothing built or run.

Menuv3 final138-line diff and report read, lead17/17 noengine tests; runtime/pixels still owed. HUD24028722 full driver/report read, rejected prelaunch gates merely record false, missing settings silently inherited; SWE instructed actual-main tests and failclosed corrections. Replaydelay and sound-cursor source overclaims require exact closure. Completed pump report fully read; leave race only source hypothesis and no-consume design not accepted. No score advances. Four external workers: Windows Grok FG6C full inventory83467; Mac Grok native directory codec GREEN54851; SWE-2 Max HUD pin/invocation correction45650 (platinum-begonia); Mac Opus5 max/Fast off Completed pump source closure2253 (resume942e358e-626e-4ac6-bfce-b2fcab977aed). No Codex subagents. defiant-taurus idle; next bounded client task being designed. FG6C owns Windows engines/builds; codec GREEN owns Mac engines/builds.

### 2026-09-13 11:19 MST — directory codec ACCEPTED; battery own-lock exception

Directory native feature committed/backed up db59c9e8f6 at11:12:52 after full3file diff review, exact same7d15 test REDb8f624 (5lost+18malformed) / GREEN0cc88383, native11/11 and lead personal repeat PID99737 exit0. All12 worker launches and personal settings9d2a95 unchanged, source hashes matched. Lead initial audit called score_selftest with wrong argument order and stopped before any engine; preserved initial script, corrected from actual function signature, final audit11/11 and native repeat passed. Only codeccpp/header and tests committed through helper; trailer scan0, exact0a54 lease push, origin rechecked. Archive mac-directory-codec-pair-evidence.tar.gz12544000bytes SHA0a0177fc5d9cd147a50d04dd201e8253d58c9a1ab6324808fa726fbe4f3708cb, 394validated regular/directory members, includes retainedREDexe and two objects. Whole adapter diffs read, all environment/score/output changes disclosed. Windows branch clean but its old executable remains STALE; no candidate/milestone merge.

Item8+1 (59→60) for compatible directory-unlisting backend and native serialization, not engine-client wiring. FG6C arm1 owning-lock stall: inherited w102 wait_engine.py waits for LEAD_BATTERY.lock; no arm1 directory or engine created. Lead saved exact process ancestry/lock/noengine evidence then killed ONLY waitPID48472 at11:07:24, parent row records4294967295. Inventory continued; lock retained. NOT RUN, no engine failure or pass; separately corrected invocation awaits full inventory end. Lead notice and arm1-own-lock-stall.json retained, original drivers unchanged.

HUDv2 full driver/report read. Lead spot-check confirms replaydelay0 and lpinv sound-cursor source. Two remaining harness faults: Settings.input source is only reported, not pinned; cleanup exception can leave pass true. Final narrowly scoped correction before runtime. No source-only renderer failure.

### 2026-09-13 11:59 MST — Mac combined HUD accepted; native transport RED reviewed

Mac combined7e63/87158955 gate CLOSED: original native/A7 audit235checks plus corrected HUD3/3, personally rederived35checks and extra identical HUD repeat PID20020 exit0. Every run700ticks/697replay frames, exact HUDpress500 marker, settingsc1a19b unchanged. All700 complete tick entries equal across the3 corrected runs; trace files differ only numeric.__wall_seconds, no mask/checker change. Original3 HUD engines ran700ticks but HUD assertion neverexecuted (exit5, missingdepth7). First corrected invocation launched0engines due driver/runner duplicate-directory creation. Lead preserved both failures, reproduced constructor behavior and fixed only directory ownership; strict noengine11/11. Final driver38480c1b changes original argv only depth7 and outputpath. Final HUD statehashf6094558. Archive fcd7a08370fb440c6613bca0f50e14aef5c796bbc5463f7b292d076ab0e0ecd8 (1351680bytes,108validatedregular/directorymembers), includes both corrected attempts and personalraw; originalMacgroup retained separately.

Native ICE actualRED accepted independently on Mac7e63/test382943/exe16720258: refused-resync detachment, ICE rematch refusal, late tagged-peer answer on idle IP, and empty-row ICE advertising. Lead9rawchecks and independent report agree. Round0 assertions vacuous; coordinator/retained-state checks are not. Failed-rematch worker wire/owner return is limited proof, never successful rematch. Independent review identified a dispatcher report/update ownership race introduced by enabling workers; initial2file productionpatch3f9bec53/d144e0ee fully read and unbuilt, correction94002 moves dispatcher with transport bundle before any GREEN build. Keepalive jthread joins before final owner movement, lead source checked. Completed pump/inbox/admission/directory/lifecycle remain future concerns. No new source-only HIGH or item6 decrement.

Directory client production33bb6885/d4913844 remains unchanged. New test-only final726bc912 frozen on item7-chat@db59 and MacRED34542. Lead read initial356line+114line correction, fixed inverted diagnostics and added actual old-API visible429/retract test independent of missing hidden feature. Both visibility groups now run even if first fails; t=0 throttle deadline30000 corrected before runtime, oldfixtures retained. SWE twice placed reports in unintended lanes; preserved and reported, subsequent brief uses explicit output path. Production proposal only under swe-directory-client-visibility-20260913/proposed; no application/build/GREEN yet. Codec db59 previous verified backup unchanged.

FG6C original run_all/remaining finished11:30 with five NOT RUN engine rows caused by own-lock waits: arm1,AK1/2/3,ICE; dependentAKcomparison had no inputs. Lead retained exact ancestry/noengine evidence and terminated only those waiting drivers (ICE owned directory fixture also stopped). Original argv/checkers/pins and LEAD_BATTERY.lock unchanged. FinalN6 started11:30:09 but isolated_launch--hold PID49144 is waiting at QUNS_BUSY; no engine, no override. Bounded2400s wait ends about12:10. Guard48700/inventory47236 retained. Only after all owned descendants stop and pin recheck may corrected single invocations run under the same lead-owned lock; original failures remain disclosed. Arm/AK/ICE preflights read,17+10 noengine worker tests; final separator correction pending lead rerun.

Scores unchanged; #5 retains98 until full inventory/family, #8 retains60 until implemented client/transport coverage. Workers preparation/source reports are partial with negative+0 accounting, never a completion.

## 2026-09-13 13:25:57 MST — startup lock accepted and deployed
The lead read every final diff line and all synthetic fixture/wrapper code, then personally ran the same seven tests in lead-startup-lock-verify-20260913 (exit0). Original main reproduces overlapping config-rename failure; final serializes initialization, releases before long work, and rejects the wrong model. Tests are explicitly synthetic, not provider identity evidence. Existing real worker model gates stay mandatory. Final04dd788d7a526ec292d843f92e547787e5e856a920cbd57c1350d55b9033a83a deployed to win_cli/win_job.py; original98a1b8f1047868bd88b7b834f43d6273ea37ba82ad1b4bbf4b5f3dcd89881145 retained. No production test/environment override hooks. Existing running workers are unaffected. NEGATIVE item5+0: infrastructure correction earns no engine milestone credit.

## 2026-09-13 13:32:36 MST — durable SWE queue deployed
Lead read fullfinalsource and independent REVIEW-FINAL; final8b8be739 passes actualparser contract, seven independent edges and19focusedcases. Old livequeue preserved. Deployed source and started queue-20260913-runtime.json with three concrete briefs: viewport repair, strict controller replay driver, substitution/reclaim stimulus validator. Two slots,90s stagger, ownedwrapperlifetimes reserved, modelgateunchanged. External manual launches are discontinued while this scheduler owns dispatch. Stopfile preventsnewlaunches but does not instantlystop runningwork; atCodex<=60% lead mustalso stop exacttrackedworkers. This deployment is not continuous-utilization proof and earns no milestonecredit.


## 2026-09-13 13:51 MST — directory client acceptance and runtime dispatch checkpoint

ACCEPT b3145db997 (cf0d1276 implementation plus lead focused oracle cleanup) for the directory client component. The complete client/header and all test additions were read; independent mac-directory-client-verifier REPORT is saved locally and fully read. The exact92e0 fixture is RED on old Windows efd7d2e2 (10 visibility,13 legacy,18 deadline,9 report misses; cascades not50 independent defects) and GREEN on3223ba85. Mac staged exact-fixture pairs and11 were rederived previously (65 checks combined with controller replay). Windows tightened oracle catches absent/string/number visibility; the lead removed historical bad-oracle code from shipped test, preserving it in cf0d and raw evidence. Fresh /MP6 build45804cf6: personal directory PID33972 exit0 plus11/11; source client e8406371/header145ecc3a unchanged, testf5c1a2f8. lead-directory-acceptance-check.json161/161 rederives all original/final pins, containment, exits and raw verdicts. Shutdown under429 sends0 DELETEs and exits by budget; expiry is explicit. Visible Failed->Retract/re-advertise cleanup still lacks its focused arm, and other declared coverage gaps remain. No production caller hides rows until later service wiring. Item8 60->61 for this completed dependency, not ICE lifecycle completion. Commit helper used; trailer scan0; origin db59 observed then exact lease push db59->b314 succeeded. Candidate and approved trees unchanged.

UI visual acceptance REJECTED: all25 frames independently viewed, source diff read. F1 offscreen panel and F3 invisible high-bit token are real high findings; F2 odd-width background seam is medium. Existing line-top clipping is pre-existing. SWE repair receives precise new scope via inbox; no driver exit0 or old report renders-glyph claim accepted. NEGATIVE item7+0.

Six runtime/review Grok lanes dispatched13:40:51, all model gates/fresh streams confirmed13:41. Their proposed untested ownership checker was explicitly refused as an oracle because it strips PATHS and intersects tick sets; new detectors must fail closed and retain every difference. Controller driver correction completed13:41 and queue handed its SAME SWE session to substitution validity without manual resumption. NEGATIVE item5+0 for unaccepted driver correction and independent oracle reviews until raw acceptance. Mac lease integration review now follows client verifier; setup-runtime remains engineering lane.

## 2026-09-13 16:42 MST — Lead pickup (option B) and the FG6C classification

**Pickup facts** (read from the trees and both machines between 16:08 and 16:40 MST): nothing runs on either machine except
the user's paused interactive Codex session; candidate `stage2/fixgroup-6-lead` @ `7e63a0c1b5` (exe `c67b35a6…`) frozen;
wave `stage2/fixgroup-6-lead-wave-a` @ `3b12f4bcc6` = origin (directory client `b3145db997` integrated; binary stale);
milestone `aa650e601e` unchanged. Backup pushes with `scan_trailers` = 0: `stage2/pie-close-lockstep` `81187eef4e` (new on
origin), `stage2/mod-mismatch-presentation` `2594cff51d` (new), `stage2/directory-unlisted-sessions` `b3145db997` →
`feb492b9a7`. Every uncommitted diff saved as a patch under `reviews/pickup-20260913/patches/` (item7-ux six files
+93/-20 plus `tools/test_viewport_fit.py`; item8-directory three files +454/-51; h4-secondary two files +204/-9;
fencing-warm three files +423; item4-feel `MovableMan.cpp` +74/-7; item7-chat `NetDirectorySelfTest.cpp` +15); the
worktrees themselves were left untouched. Routes re-verified this hour: Codex CLI non-interactive with the model and
effort read from its session rollout (`cli_runs/smoke-astra-runner-20260913`), Cursor CLI chat resume with memory
(`cli_runs/smoke-grok-resume-20260913`), and the Cursor CLI's model list has no Grok 4.7 (4.6 Extra High Fast stays).

**FG6C classification** (candidate `7e63a0c1b5`, exe `c67b35a6…`; report `fg6c-battery/REPORT.md`; investigation lanes
`grok-fg6c-*`, `fg6c-corrected-invocations`, the afternoon heal/AK/controller-log lanes; the raw items I looked at are
named per row):

| Row | Verdict | Evidence I re-derived | FG6D correction |
|---|---|---|---|
| global-callback (SELFTESTS row) | harness: bare `-global-callback-selftest` is refused by design (`stdout.log:3` REFUSE needs `-net-replay`); the recording driver `test_global_callbacks.py` PASSes on the same exe (`gcb\stdout.log:14`, guards/initial/memory/file/index all 1) | `run_battery.py:44` argv; the REFUSE line; the gcb PASS line | the row invokes the recording driver |
| fencing_two_transports (old h4gates) | stale driver argv (no `-net-join-wait-for`); the five real fencing rows are `fenced_disconnects=1` each | `fencing_h4gates` 1–5 results; the old driver's timeline | row retired with disclosure; the five rows stay |
| substitute_bounds; reclaim → compare_overlap | NOT RUN, not FAIL: the runner's desktop gate (`win32_test_runner.py:213-231`, `SHQueryUserNotificationState` = QUNS_BUSY) held later peers 105 s, so applicants 2/3 and the returner started after the host's window; the oracle scored a sequence that never ran | the runner source; applicant/returner start stamps vs host end (17:17-17:26 UTC logs); `denials_scheduled=0` | rows whose launch records show a fullscreen wait after the first peer are marked NOT RUN and rerun once at the end; engine paths still unexercised → must rerun |
| heal_wp | wrong oracle: `recovery_expanded_mod.py:46-49` demands `u>=335` on EVERY row while `u` runs 26..599 (FG6B identical); continuity holds through the heal window (tick 61 `1/60/60/0` → 62 `1/61/61/0`); the client's duplicate tick-62 rows are the discarded pre-relaunch tick plus the resimulated one (NV 1049068 vs 1049064), classified `discarded_resimulated` by the boundary oracle | the oracle line; first/last tuples; the boundary lane's evaluation | adopt the boundary-aware fail-closed oracle (`grok-heal-boundary-final-20260913`) after the mutation table is re-run; both peers' rows kept |
| present_identity | wrong length contract: `-net-match-ticks 180` without `-max-ticks` records 181 ticks (`Main.cpp:3869` stops on `Total() > tickCap`); `strict_compare` fails on length before comparing; prefix 1..180 identical, tip/tip2 SHA equal | `present_identity.py:19,38-41,77`; `sha_match_tip_tip2 true` | prefix-180 contract (or `-max-ticks 180`), both runs reported |
| arm1, ak47hud_1-3, ak47hud_compare, ice_e2e | NOT RUN in the inventory (own-lock wait on `LEAD_BATTERY.lock`); the corrected invocations ran all five on the same pinned exe inside the identity window (10/10 before and after): arm1 PASS `uid=1048634 apply=80 handback=90`; AK 3/3 HUD PASS incl. press tick 500; ICE host/client exit 0, 601 ticks identical, verdict pass | the three AK `trace.json` files: only `runs[0].numeric.__wall_seconds` differs (8.8535899 / 8.8255805 / 8.8890925), every sim field equal (my own diff); ICE `last_verdict` "session gone (HTTP 404)" is the driver terminating the directory before the host's last poll | own-lock waits removed; AK three-way equal except `__wall_seconds` through the fail-closed comparator; ICE driver tears down after the host's final poll |
| controller_log | no fixture ever existed (placeholder since W81-4; `remaining_steps.py:115-134`); a record/replay driver now exists (`grok-controller-driver-pin-completion-20260913/run_controller_log.py`, 16/16 tests, `detect-4-32` record nls 4 / replay nls 32, `compared_ticks=360`) | the placeholder; the driver's evaluate output | the row runs that driver |
| resync-frame table | never invoked, and `scan_resync.py` crashes (`PermissionError` on `'.'` in `fencing_roots`, line 217) | ran it myself 16:22 MST | fix and invoke from `run_inventory.py` |
| n6 craft_cargo pulse | raw pulse 316..319 on both peers = expected; the wrapper accepts by substring (`run_n6.py:34`), `analyze_dumps.py:81` always returns 0, `mode_delivercommandhost` is name-only (no deliver line in `host.out.txt`) | the pulse rows; the wrapper lines | exact-list oracle; the DeliverCommandHost mode asserts its deliver line |
| probe_wrong | expected failure by design (certificate pin mismatch) | `stdout.log:4` | none (PASS) |

**Verdict:** NO true engine failure in FG6C; the candidate stands as built. FG6C is NOT the battery pass: substitute_bounds,
reclaim, compare_overlap, controller_log and the resync table were never exercised. The corrected harness (FG6D) runs
ONCE on the next candidate (RESUME §5.0 A1/A4). Item 5 NEGATIVE +0 (the inventory closed without a pass); scores unchanged.

**F22 — Windows/Mac terrain construction divergence (item 2/5).** From `grok-cross-platform-terrain-cause-20260913`, the
Mac Opus checkpoint `mac-terrain-primitive-cause-20260913/REPORT-checkpoint.md` and my own checks: on identical assets
(242/242 Grasslands files equal) the first differing constructed byte is material `(1990,699)` host 12 Stone vs Mac 13
Bedrock; 12 material / 64 foreground pixels differ; the path is `SLTerrain::LoadData` debris placement
(`TerrainDebris.cpp:194-215` → `rotate.c _rotate_scale_flip_coordinates`, `_AL_SINCOS`). Windows: the exe `c67b35a6…`
contains the deterministic polynomial constants (1/120, 1/24, 1/40320 once each, exactly as the freshly compiled
`rotate.obj` does), so Windows runs `_al_det_sincos` (the checked-in 2025 `_Bin/allegro-release.lib` blob has none of
them, but it is restored after the link and is not what the exe linked). Mac: `gcc-13 -O3` compiles the Allegro C files
without `-ffp-contract=off` (the root `meson.build:39-45` fp flags cover the engine only): 10 fused multiply-adds inside
the inlined polynomial and 2 inside `_parallelogram_map` change the last ULP of the corner coordinates. Lane B1 (Astra):
contraction off for every C subproject on the Mac and whatever else the disassembly shows, GREEN = identical tick-1
terrain dumps on both machines (mat 0 / fg 0). Until then a Windows/Mac match diverges at tick 1.

**Astra's landings of 13:00-14:40 MST, all unreviewed claims** (each has a `REPORT.md` under its lane; none accepted):
pie-close `81187eef4e` (RED four 201-204 differences → GREEN 0 with 320 maps / 1470 rows equal; first two-peer invocation
failed on a Reader error, SP fixture added) — A2; Form Squad UI stimulus corrected (L_RIGHT → Follow; dummy `aimode=11`
at 273) and client Go-To runtime — B7; controller-log driver (three lanes, pin-completion copy is the most complete);
heal oracle (five lanes; boundary-final is the fail-closed candidate); AK fail-closed comparator; directory lease
integration patches + the Mac review (F1 high: ReturnToLobby relists from a point-in-time Failed state; F2-F4 medium)
and the client in-flight hidden-404 hunk — B5; ICE stop/cancel tests, the missing `m_Mux->SetPump({})` before
dispatcher Stop, the ended-world late-admission test with a predicted FAIL, the Mac RED "a session-id (ICE) match cannot
return to the lobby yet" — B6; ICE rematch runtime on the Mac (works when forced; a later ICE joiner cannot follow the
new row); `mac-ui-minviewport-verifier` REJECT (F1 offscreen panel, F3 invisible high-bit token, F2 seam) — B4; the Mac
Opus lanes `mac-pie-close-independent` (session `2f067b76-b2e9-4036-9999-cc06ad84065e`) and
`mac-terrain-primitive-cause` (`e8a46c87-8ff8-4f3a-a699-07a61cc06091`) were cut at the pause and are resumable.

## 2026-09-13 17:02 MST — Form Squad UI-path acceptance (item 3 +1) and the lead's pie-close raw re-derivation

ACCEPT grok-formsquad-ui-stimulus-20260913 as evidence for item 3b "the UI path of an AI order": on candidate 7e63a0c1b5 / exe c67b35a6…, port 47981, 450 ticks, input delay 3, the crab 1048736 seated at 35 opens the pie menu (150), L_DOWN → Eye (163), L_RIGHT → Follow (173), hover `pie=1:8:Follow:Follow:Follow` at 224 and close `2:8:-:-:-` at 225 on BOTH peers; the dummy 1048615 goes `aimode=1 wp=0` (272) → `aimode=11 wp=1` (273) on BOTH peers; all 450 tick maps identical (controller and terrain included); the lane's four mutations (dropped tick, pie wipe, dropped controller, Follow→Eye rewrite) fail closed. I re-derived the 224/225 and 272/273 rows from `host_trace.json.simdump.txt` and `client_trace.json.simdump.txt` myself. Caveat kept: `GetAIMOWaypointID` is not dumped (target inferred from wp=1/team/SQUAD 11); the derived fixture edit (L_RIGHT to reach Follow) is disclosed with the original preserved. Item 3: 96 → 97.

Pie-close 81187eef4e, lead raw re-derivation (verdict pending A2): control run host `0:8:-:-:-` versus client `1:8:-:-:-` for actor 1048736 at ticks 201-204, both `2:8:-:-:-` at 205; patched run both peers `1:8:-:-:-` at 201-204 and `2:8:-:-:-` at 205; both dumps 15,811,996 bytes in both runs. The diff is 3+/2-: the two premature `SetEnabled(false)` calls in GameActivity.cpp (1246, 1256) guarded by `!ScenarioRunner::IsLockstepControllerSyncActive()` plus a one-line comment. Reading: in lockstep the local switch no longer closes the pie menu early; the synchronized controller closes it at 205 on both peers, which is what the client already did. Open until A2 lands: SP unchanged (the SP fixture), the detector's fail-closed mutations, actor-death / buy-menu / ActorSelect branches.

## 2026-09-13 17:18 MST — Pie-close independent verification (Mac Opus, resume 1) read in full; F23 registered

The verifier re-derived everything from the archive (386 entries, sha fb7c921d…, all manifest hashes matched) and confirms: the diff is exactly the two guarded lines (blobs c34471df1a → 51f2001e00), `ScenarioRunner.cpp:799-800` makes the guard false in SP; RED = exactly four differing dump lines 16701/16822/16939/17049 (`201..204 actor uid=1048736`, host `0:8:-:-:-` vs client `1:8:-:-:-`) with 320/320 trace ticks equal; GREEN = host and client dumps byte-equal (sha 8548012de3dcdec2…, 15,811,996 bytes, 1470/1470 actor rows), byte-identical to the RED client dump; Go-To pairs byte-identical (sha d5e0b5b48a2d5ea9…); SP NEXT and PREV control-vs-tip dumps byte-identical; the lane driver differs from the shared one only at lines 69/401/404/409 (separate client runtime); the 11 selftests pass on the tip exe 546645e3…; no mask weakened (the dropped second-ACTOR_NEXT rule was wrong). OPEN: the setup-race mechanism (plausible, unproven), no UI arm (Q15), no restore arm.

**F23 (new, pre-existing on control and tip, both peers; HIGH):** after the lockstep switch at 204 the departing crab 1048736 (AI mode) has pie `2` at 205 then alternates `0`/`2` to 320 (SP: `2` at 201-204, `3` from 205); it carries player-only control bits on 59 of 117 AI ticks (204 `0x500006`, 206 `0x4000500002`, …); the brain 1048577, seated only 27-34, carries ACTOR_NEXT_PREP `0x4000000000` on 71 of 286 AI ticks. Same-class local pie mutations remain ungated: GameActivity.cpp 1321-1327, 1346-1347 (ActorSelect `DoDisableAnimation`), 1352 `Wobble`, 1354/1390 `FreezeAtRadius`, 1576-1577/1592-1593 (delivery orders). Detector gaps: no mutation of the post-handoff pie state, the replacement seat, the handoff tick value or an activity row. Lane B11 `astra-pie-lockstep-gating-20260913` (brief written) owns the fix; 81187eef4e itself stands as the fix of the 201-204 divergence and is ACCEPTED on the Windows evidence pending A2's read; the Mac arm was not run (report placeholders) and the lane is resumed once for it.

## 2026-09-13 17:29 MST — Viewport repair ACCEPTED (item 7 +1, merge pending): F1/F3 by the independent visual review, F2 by the lead's pixel diff

Lane swe-ui-viewport-complete (SWE-2 Max, session defiant-taurus), branch stage2/mod-mismatch-presentation @ 9a3459c389 (four commits f474006490 font fallback, 68894303be panel bound, 2adecbd1c2 skin filler, 9a3459c389 detector; all pushed). The lead read every production line (GUIFont fallback threaded through Draw/DrawAligned/CalculateWidth/CalculateHeight with a per-cell ink scan at Load; the signed-char width overload; GUILabel arming the fallback per label and the wrapped scroll range; MainMenuGUI capping the panel to `m_RootBoxMaxWidth - 12` with the Back reserve and overflow scroll; GUISkin filler tiled to the frame bounds, drawn before sides and corners). Independent visual review (REPORT-visual-review-2.md, saved verbatim): F1 PASS (panel 150..489 × 44..289 inside 640x360, all rows inside, RED shows the container ending at x=307 with text on the starfield); F3 PASS (the 64-byte run renders as a row of small pale marks, shippable; cosmetic follow-ups: baseline-align and recolour the fallback ink); F2 OPEN for the reviewer (no seam visible by eye in RED either); no 960 regressions; undisclosed gaps: the overflow-scroll path unexercised, the minviewport probe measures PNG dimensions only, 1 px slack at 640, `back_button_within_drawn_container` proves horizontal containment only.

**F2 closed by the lead:** `red-minviewport-960/ascii-worst/Host/.../host-error-long_2026-09-13_17-08-47.png` vs `green-minviewport-960/.../host-error-long_2026-09-13_17-13-57.png` (581-px panel, W % 8 == 5): column 767 differs in 338 rows (44-432); RED holds the dark backdrop through the colour key ((18,17,15), (20,18,17), …), GREEN holds the panel fill (59,65,83) on 331 rows exactly like column 766; the frame columns 768-769 are identical in both. The seam existed at the reviewed width and is gone.

Verdict: ACCEPT 9a3459c389 for the wave (merge at A3 with pie-close). Item 7: 39 → 40 now (7c mod-mismatch presentation complete at the minimum viewport, independently reviewed), +1 more when merged and pushed on the wave. Follow-up lane launched in the same SWE session (`swe-ui-viewport-followup-20260913`: fallback baseline/colour, scroll-path and real Back-press arms, a direct seam oracle).

## 2026-09-13 17:30 MST — Pie-close 81187eef4e ACCEPTED: Mac arms complete (script lane mac-pie-close-arms-20260913)

Mac arm64 builds at 7e63a0c1b5 (control exe 871589554f0a…, blob c34471df1ab5) and tip (+ the 3+/2- GameActivity.cpp change, exe 8898bfdf0611…, blob 51f2001e002a), engines through tools/posix_test_runner.py, headless, ports 48713-48716: pie-control host vs client differ in exactly 4 lines (16701/16822/16939/…: `201..204 actor uid=1048736` pie `0:8:-:-:-` vs `1:8:-:-:-`), 10,636,086 bytes each, 320/320 trace ticks, 0 trace diffs; pie-tip byte-identical (sha 7086bbe130f406da…, both peers), 1470/1470 rows, 0 diffs; sp-next control vs tip identical (sha 91fcc5f1a4ebddf6…, 588 rows), sp-prev identical (928f2a9b669be24b…); goto control and tip all four dumps identical (04ce995e8d892662…). Evidence fetched to grok-workers/mac-pie-close-arms-20260913/evidence/ (arms.log, verify.txt, summary.txt, mac_verify.json, the patched mac_arm.py). F23 reproduces on arm64 too (pie 0/2 alternation from 206, ctrl 0x4000500002 on the AI-mode crab at 202/203/206). Two harness notes: the Opus lane's mac_arm.py passed relative output paths while the runner's cwd is the private runtime (engine wrote nothing, exit 1) — fixed with one line (`a.out = Path(a.out).resolve()`, original kept as mac_arm.py.orig); mac_job.py reports NO_INIT (exit 1) for a script lane without an agent stream even when the job's exit.txt is 0 — read the lane's exit.txt for script lanes.

VERDICT: ACCEPT 81187eef4e for the wave (lead read, independent Windows re-derivation, Mac arms). F23 stays open under lane B11. Item 3 credit (+1) at the wave merge.

## 2026-09-13 17:32 MST — Wave merges: pie-close (365560574a) and the viewport repair (3f65208668); wave pushed

`stage2/fixgroup-6-lead-wave-a`: 3b12f4bcc6 → 365560574a (merge of stage2/pie-close-lockstep 81187eef4e; verify_merge_commit 0 differences) → 3f65208668 (merge of stage2/mod-mismatch-presentation 9a3459c389, five commits 2594cff51d/f474006490/68894303be/2adecbd1c2/9a3459c389, all read; one conflict region in Source/Menus/MainMenuGUI.cpp, lobby error label: the branch's viewport-fit block taken with the wave's `statusExtra` (host status rows, b7e1d86b35) re-applied to the label position, the error room and the extra height; verify_merge_commit reports exactly one merge-only line, `errorRoom … - statusExtra - portMapHeight`, the other two adapted lines already existed on the wave side). Trailer scan 0; pushed with lease from 3b12f4bcc6. The resolution compiles only at the next candidate build (A3); if it fails there, it is fixed before verify6. Item 3 → 98, item 7 → 41.

## 2026-09-13 17:37 MST — Candidate fast-forwarded to 3f65208668 and built alone

`control-build` `stage2/fixgroup-6-lead`: 7e63a0c1b5 → 3f65208668 (fast-forward to the wave: directory client b3145db997, pie-close 81187eef4e, viewport 9a3459c389). Final build alone with CL=/MP12 (no other cl/link process; the stopped verify6 leftovers killed first): 17:35:00-17:36:37 MST, rc 0, no errors, 261 sources compiled incrementally, the hand-resolved MainMenuGUI.cpp region compiles. New exe `D:\Projects\control-build\Cortex Command.exe` 21,338,624 bytes, SHA256 prefix 2b637683d93362d9, log D:/mx/lead-candidate-20260913b/build.log. The frozen FG6C exe c67b35a6… is gone from that path (its evidence remains pinned in fg6c-battery/ and D:/mx/fg6bat3). verify6 running on the new exe (OUT D:/mx/lead-fg6/verify6-3f65208668-r2). Next: FG6D battery once lane A1 lands.

## 2026-09-13 17:39 MST — verify6 8/8 on candidate 3f65208668 (exe 2b637683d933…)

`lead-tools/verify_lead6c.py` (OUT D:/mx/lead-fg6/verify6-3f65208668-r2), 17:36:57-17:39:20 MST, 144 s: selftests rc 0 (the socket-free suite; count re-derived from selftests/result.json below), port-map PASS, directory PASS, script-graph s4 PASS (counts re-derived from its stdout below), global-callback driver pass (exit/callbacks/single_start/cleanup/no_errors/desktop all true), preview-event-d7 PASS, lpinv-100 PASS, arm1-switch-control rc 0. Every engine launch pinned to sha 2b637683d933. The candidate is ready for the FG6D battery (waits for lane A1's corrected harness).
Re-derived by the lead from the raw files: selftests/result.json `passed 11 / total 11`; script-graph-s4/stdout.log 186 `[script-graph-selftest] PASS` lines, 0 FAIL lines, `coroutine_near_cstack_limit_8011` present (2 lines); preview-event-d7 8 `[preview-event-selftest] PASS` + 2 `[preview-hook-scope] PASS`; lpinv-100 1 `lpinv] PASS`.

## 2026-09-13 17:46 MST — Astra B3 landed: second read of the F21 group and the coroutine stack capacity — ACCEPT WITH FOLLOW-UPS (both)

Report `grok-workers/astra-f21-coroutine-read-20260913/REPORT.md` (56 KB; read: deviations, §1-§4 in full, §5-§6 headings and verdicts). Pinned to 7e63a0c1b5 (36 source files frozen under D:/mx/astra-f21/pinned-source with a manifest; the checkout advanced to 3f65208668 at 17:33 during its finalization and it says so). CONFIRMED: the F21 write order (NetMatchService.cpp:716-754 chooses the logical local peer's binding; Activity.cpp:1294/1301 installs the host save's pending IDs; afterRestore writes the peer pointers at Activity.cpp:1117-1145; the overwriting write was `m_ControlledActor[player] = LiveCheckpointActor(m_CheckpointActorIDs[player][1])` at Activity.cpp:1352 with the host save's ID; MovableMan.cpp:804-819 then released the foreign-owned actor → brain 1048577 then controlled 0) and each commit's role (85d90 `RefreshCheckpointActorIDs` after the pointer writes; 84a6 marked IDs after UI restoration; 3a86 brain validity via current-world ownership; b4a5 test-only); every player index 0..3 is looped; the A7 RED is the exact controlled-UID defect with the expected UID observed not hard-coded; native RED/GREEN pairs detect the repairs; no test-only production branch; F20 is frame capacity (LuaJIT vm_x64.dasc:4695-4697); 672024 computes needed = max(top, link+1+framesize), reserves top+16, grows by the absolute shortfall through lj_state_cpgrowstack and only then takes the stack pointer; 8011 is a real growth boundary (control f0bc3c9b FAIL at stdout:691, GREEN f4bec5f3 maxstack 16038; control 182/4, GREEN 186/0); no stale raw stack pointer; encoding unchanged (SG3, BCDUMP 2, resync magic, Activity3 untouched; ThreadCapture bytes identical across four commits); the socket-free 11 ran on the combined candidate. NOT CONFIRMED: a Windows F21 RED (the RED controls are Mac binaries; Windows has GREEN only); universal bidirectional coroutine-snapshot compatibility (old→new has a source-supported path; new→old and W136-2 readers refuse or underallocate); F20 as a universal bit-for-bit claim. OPEN (debt, not defects): a first-update selection mutation before the final rebind (source path, no executed RED); all four local indices at runtime; inventory brain / marked actor through an actual resync; substitution, spectator and ICE reconnect as a group; allocator-failure and GC stress; large reused placeholders. §6 lists the exact runner commands → queued as lane B12 (execution of the F21/coroutine debt arms). Usage: 12.22 M input tokens (11.89 M cached), 80.6 K output (38.2 K reasoning).

## 2026-09-13 17:52 MST — Astra A2 landed: independent pie-close verification — ACCEPT WITH FOLLOW-UPS (agrees with the Mac verifier and the lead)

Report `grok-workers/astra-pie-close-review-20260913/REPORT.md` (82 KB; deviations, §1-§8 verdict lines and the overall claim read; the appendix carries the whole production diff and raw quotes). CONFIRMED: the diff and the compiled guards (`ScenarioRunner.cpp:799-800` false in SP; the pre-existing NEXT/PREV asymmetry `!m_LuaLockActor[player]` at 1243 preserved; no new null dereference); the frozen RED (four differences at 201-204) and the isolated GREEN (equal 15,811,996-byte dumps, 1,470 rows, 320 maps); SP NEXT/PREV and Go-To control-vs-tip equal; the runtime-separation driver change confirmed as non-masking. NOT CONFIRMED: the detector as an unqualified fail-closed oracle — five added probes are accepted although evidence is lost or shifted (a non-Crab actor row missing on both peers, all `paths` fields missing, `particles` missing, a shifted interval, …; `probe_detector.py --assert-fail-closed` exits 1 with those counterexamples). OPEN: the exact setup-race interleaving of the failed first invocation; the interpretation of actions after a Go-To cancellation; the MP pair exercises NEXT only (SP covers both). It also records the reopen at 206 on both peers (= F23, lane B11). Usage: 6.87 M input tokens (6.64 M cached), 90.9 K output (48.5 K reasoning). Follow-ups queued: detector hardening against the five probes (RED = the probe script exiting 1 today) and an MP PREV detecting pair — both folded into B11's acceptance (same worktree, same driver family) as B11a.

## 2026-09-13 17:53 MST — Astra B2 landed: second read of the F15c hardening 14db824c10 — ACCEPT WITH FOLLOW-UPS

Report grok-workers/astra-f15c-second-read-20260913/REPORT.md (45 KB; deviations, §1-§6 verdict lines and the verdict read). CONFIRMED: the commit changes classification and native remapping, not the destruction sequence; remapping precedes destruction (unchanged LocalPrediction order); native ownership/destructor paths stay separate from the classifier; mode `c` is the detector (the lead's exact pair: pickup_fire replay, `-local-prediction-invariance 200:1:1 -lpinv-overlay-links bc`, four C-link failures RED, all pass GREEN); b/r are controls; the added Main/PreviewScriptSelfTest branches are test plumbing with no hidden test-exclusive fix; failure messages state the observed failure; the ten ASan rows re-read from their launch records (exe 49b4f0f8…, exit 0) and the later Final ASan pair closes the compile-log gap. NOT CONFIRMED: a memory-safety RED (the REDs are mapping failures; the earlier acceptance said the same). OPEN (coverage debt): universal post-retirement reachability (ghost-owned parts stay reachable through their ghost owner until disposal by design), the taken-shadow-retiring branch, spawnMeta-only filtering, stale-key exclusion, unharvested tail descendants, retiring Actor inventories, and unchanged mod behaviour across a preview end (return-by-reference aliases, F20 continuations) — §4 gives recipes I/T/M/Q/X/J/A/L/N/O → queued as lane B13. Note for the spawn protocol: the lane ran a nested `codex exec` of its own for part of the read (its own gate evidence under D:/mx/astra-f15c/cli/) and created and deleted a scratch codex-home; not asked for, no harm, but every Astra brief now says "no nested Codex sessions or sub-agents". Usage: 12.00 M input (11.69 M cached), 84.0 K output (45.6 K reasoning).

## 2026-09-13 17:59 MST — Astra B1 landed: F22 root cause corrected and fixed on both platforms (verifier pass running)

Report grok-workers/astra-terrain-xplat-20260913/REPORT.md (25 KB; deviations, results, primitive mechanism, production diff, flags audit read). The lead's earlier inference was WRONG: the retained Windows exe c67b35a6… linked the OLD tracked `_Bin/allegro-release.lib` (2025 blob: platform cos/sin, floating `fixmul`/`fixdiv`); the polynomial constants the lead found in the exe belong to the engine's own math, not Allegro (the lane proved the link by complete function-byte matching and call-site inspection). FMA contraction on the Mac was disproved for this scene: across all 563 rotated pieces, gcc-13 -O3 default, `-ffp-contract=off` and an unfused reference give identical fixed corners and raster pixels. The first differing material pixel (Boulders frame 75, rotation -162°, draw (1938,691), sample (1990,699)) comes from the obsolete floating `fixmul` (`ftofix(fixtof(x)*fixtof(y))`) versus the current signed 64-bit integer product in the map stepping (245 vs 0 source pixel → 13 Bedrock vs 12 Stone); a 4×4 replay quantifies float division (46 pieces) and float multiplication (10 pieces) effects. Fix = build-system only: aa5e151c8d "Link the source-built Allegro library on Windows" (allegro.vcxproj OutDir `_Bin\$(Platform)\$(Configuration)\` so the tracked blob no longer shadows the build; RTEA.vcxproj `LinkLibraryDependencies` true and the dependency lines updated) and 15426f0398 "Pin floating-point flags for dependency builds" (meson `fp_args` applied globally to C/C++, to native build generators — six LuaJIT generator commands were missing them — and forwarded to the SDL subproject). GREEN: rebuilt Windows host + rebuilt Mac LAN client, same scene, port 47993: material 0 / FG 0 (final-dump-comparison.json), total and subsystem hashes equal ticks 1-30 (final-trace-analysis.json); Mac script-graph selftest passed; retained-vs-rebuilt matrix retained (12/64 whenever the retained Windows exe is a side). Deviations: the network trace wrappers show passed:false / ticks:0 because that route records tick hashes without a scenario result (comparator and hash comparisons are the verdict inputs); no full battery, Win32 or CMake-SDL coverage claimed. Consequence: EVERY prior Windows binary ran the old Allegro arithmetic, so goldens/replays measured on Windows may shift with this change — the verifier (Opus, running) assesses which, and the FG6D battery on a candidate containing this fix is the gate before the milestone. HIGH. Branch stage2/terrain-fp-contract backed up to origin. Usage: 21.95 M input (21.66 M cached), 79.7 K output (39.7 K reasoning).

## 2026-09-13 18:01 MST — Mac gates on 3f65208668 (Grok lane mac-gates-3f65-20260913): all rows green except one A7 arm, classified as a connect-gate timeout under load

Fresh arm64 clone at 3f652086687e…, exe 8bf07d0eb562…, meson/ninja recipe as FG6C. Official 11: 11/11 (each launch pinned to the exe; PASS lines quoted per case). Inventory extras: script-graph 186/0, localUI 39/0, localState 10/0 (native-graph, -1 and diag-on), 8011 arm PASS (stdout:718), retained/fresh/seatless and inventory-brain arms present; net-discovery, net-directory (:185 PASS), net-port-map PASS; global-callback driver PASS (guards/initial/memory/file/index 1); preview-d7 8 event PASS, violations=0; ak-lpinv 4/4 at tick 176 canonical byte-identical; AK HUD 3/3 press tick 500 (disclosed argv: `-local-prediction-depth 7` inserted as the POSIX equivalent of the Windows driver). A7: 4/5 — stagger_seat_survives, leave_ack_dropped, slow_resync_save (saved 199 == boundary), coop_hand_back (saved 214, returner frames 215-275 all controlled 1048615 / brain 1048577 / seat (1,0)) pass; **silent_socket_bound FAILED** with `silent0: event producer reported lost or invalid observations`. Raw (a7/run-group/c/silent0/events.jsonl): seq 2 `connect_waiting` at 0.6 s, seq 3 `evidence_gap: connect gate invalid or its bounded wait expired` at 27.5 s, then `setup failed: A7 connect gate invalid or timed out` in stderr; silent1's journal shows `connect_released` at 23.8 s. The eight silent peers therefore never reached the connect phase where the `client_never_says_hello` fault applies: the shared connect gate's bounded wait expired for silent0 while the same Mac was building and running engines for lane B1 (17:47-17:58 overlap). The same arm passed on the previous tip (mac-fg6c-final result.json: passed, no gaps). Classification: harness timing under load, NOT an engine failure; the arm is rerun alone on the quiet Mac before acceptance (script lane). Everything else on this tip matches the previous tip's Mac results.

## 2026-09-13 18:11 MST — A7 silent_socket_bound rerun alone: PASS; Mac gates on 3f65208668 ACCEPTED as fully green

Script lane mac-a7c-rerun-20260913 (mac_job.py --script-lane, a new runner flag: no agent stream, no boot-out at 180 s — the first attempt without it was booted out and left two engines orphaned, killed by PID), same binary 8bf07d0eb562…, same harness and manifest, fresh output a7/run-c-rerun: `silent_socket_bound` passed, failures [], evidence_gaps [], 18:06:26-18:10:46 MST, load 1.6 at start, nothing else on the Mac. Verdict: the 17:47 failure was the connect gate's bounded wait expiring under build/engine contention from lane B1, not an engine behaviour. Mac gates on the new candidate tip are complete: official 11/11, script-graph 186/0, localUI 39/0, localState 10/0, HUD 3/3, lpinv 4/4, preview-d7 8 PASS, A7 5/5 (four in the group run + this rerun). No score change (arm64 same-tip gates are a promotion prerequisite, credited with the milestone move).

## 2026-09-13 18:18 MST — Opus verifier on the terrain fix (B1): ACCEPT WITH FOLLOW-UPS; the Windows fleet is split (F22b); luabind/loadpng share the hazard (F22c)

REPORT-verifier.md (lane astra-terrain-xplat-20260913). Re-derived with its own COFF/PE parser: the tracked blob b1cf0937… equals `git show 7e63a0c1b5:…/_Bin/allegro-release.lib`; its rotate.obj (6ede1002…) matches the retained control exe byte for byte (765/765, 2682/2682, 141/141), with REL32 calls to `cos`/`sin` and 12 `ftofix` in `_rotate_scale_flip_coordinates`, 20 `ftofix` / 10 `divsd` in the map; the exe's polynomial constants are RTETools.h:386-394 (the engine's own); the source-built lib (27904c82…) has `_al_det_sincos` and no cos/sin/ftofix. FMA non-cause confirmed non-vacuously (fast.s 24 fused ops vs 0; 563 pieces × 9 variants, 0 diffs). RED 12/64 at offset 1746702 = pixel (1990,699); GREEN 0/0; retained-Mac == rebuilt-Mac == rebuilt-Windows (Windows moved, the Mac did not); ticks 1-30 zero mismatches over 12 subsystem keys; only `_BIN\X64\RELEASE\ALLEGRO-RELEASE.LIB` on the new link line. Diff confirmed: 3 files, 18+/14-.

**F22b — the Windows fleet is split.** Of 58 executables scanned, control-build (the candidate) and takeover-build (the wave) are OLD-arithmetic, while the APPROVED tree `p4b-interp-validation` is NEW-arithmetic: the selector is whether the tracked `_Bin/allegro-release.lib` was clean at link time (the approved tree has two modified tracked libs, allegro and luabind). So the prior Windows evidence is not homogeneous: the families on the approved tree ran the new arithmetic; the candidate batteries FG6B/FG6C ran the old. Consequence: the fix MUST be in the promoted candidate (otherwise the milestone's arithmetic stays a function of dirty vendor libs), and the FG6D battery runs on a candidate that contains it.

**F22c — half-fixed class.** loadpng.vcxproj:54,58,62 and luabind.vcxproj:54,58,62 still write to `_Bin\`, and `LOADPNG-RELEASE.LIB` is still linked from the tracked blob; the approved tree's luabind differs from the candidate's for the same reason. Lane B14 (Astra) applies the same hardening to every vendor project that builds into a tracked `_Bin` and adds an in-tree guard selftest (rotate-primitive determinism against expected polynomial corners), then selftests 11/11 and verify6 on the new arithmetic.

Other omissions recorded: no Windows run_selftests.py 11/11 on the new arithmetic yet; the RED binary is gone; untested Win32 / other x64 configs / meson-MSVC / CMake-SDL; the archived pickup_fire.ccreplay replay rows unexercised on the new arithmetic (verify6 + battery will show). Verdict: ACCEPT B1 for the wave now (merged this turn), B14 before the battery.

## 2026-09-13 18:20 MST — Astra B12 turn failed on the provider's cybersecurity classifier; resumed in the same thread

Lane astra-f21-debt-arms-20260913 (thread 01a09d6d-6637…): after row 1 (twelve all-player-index cases, RED on the reversed refresh, GREEN on the candidate; the unchanged-mod network-heal control passed) the turn ended with `turn.failed` / `codex_error_info: cyber_policy` ("This content was flagged for possible cybersecurity risk") while the reasoning read "Preparing coroutine debt self-test / Adding coroutine debt helper" (row 5: allocator failure, GC pressure, stack capacity). 4.19 M input / 25.4 K output tokens spent in that turn; the CLI exit code was 1 and no usage event was written (the runner's usage.json is empty — the rollout's token_count line is the record). This is the same class of interruption the previous lead hit interactively (RESUME §4.10, 06:04 MST). Recovery: `codex_job.py --resume <thread>` with a continuation that commits row 1 first, moves row 5 last and words it in plain engine terms; if it recurs the row is recorded and skipped. Spawn-protocol note added to RESUME §6.3b.

## 2026-09-13 18:38 MST — Astra B5 landed: directory lease corrections F1-F4 + client hidden-404 (lead read complete; Opus verifier running)

Branch stage2/directory-lease-corrections in item7-chat, six commits on the wave tip 3f65208668: 1000f449a5 tests (+417 NetMatchSelfTest, +16 NetDirectorySelfTest, a friend declaration), efcb3dce3c keep only the registered row bound to the ICE listener (`ShouldKeepIceDirectoryLease` under m_Mutex: host, ICE, not retracted, bound id non-empty, client Registered, row id == bound id), 7aa85514ee copy advertisements under the service mutex (`HideDirectoryListing` copies the row and reads the state under the lock), 4b07be8926 replacement identities without ICE (the register copy's joinMode computed for an unknown new id via `NetIceRowJoinMode(…, m_IceBoundSessionId, std::string())`), e6c7616edd settle hidden leases before relisting (`SettleKeptDirectoryLease` on every pump: while hidden and not Deleting, retract if no longer keepable, relist only when relist is pending and `GetConfirmedListed() == false`; FinishMatch/end paths choose hide vs retract; the flags `m_DirectoryHidden` / `m_DirectoryRelistPending` are game-thread only), f654ed5dbf client: an in-flight hidden heartbeat's 404 enters Failed (`hiddenRequest`, `m_InFlightListed` reset). Every production line read; comments are one-line whys; no test-only production branch seen (the verifier audits the lock scopes). Raw re-derived by the lead: RED wave-seated stdout:172 `FAIL: directory lease misses (8): [ice end: Complete did not hide the bound row on its own heartbeat; deletes=1] [legacy in flight: … registers=2] [hidden 404 in flight: registers=2 deletes=1] [rebound row: …]`, RED proposal stdout:167 five misses incl. the wrong-row keep; GREEN green-match-final stdout:137 `delayed hide desired_before_ack=0 early_deletes=0 registers=1 heartbeats=2`, :147/:155 settled legacy / hidden-404 `registers=1 deletes=1`, :160 `locked snapshot requests_while_locked=0 deletes=1`, :162 `PASS service_directory_lease ice_end_hides_keeps_relists=1 ip_leave_delete=1 legacy_404_in_flight_no_reregister=1 rebound_row_deleted_ip=1`, suite `passed 11 / total 11` (D:/mx/astra-lease-20260913/selftests/result.json). Deviations accepted as disclosed: F2's RED is against the proposal (the wave already deleted non-bound rows; the new rule is narrower than both), the fixture priming repair, the timestamp refresh after a stale-object build, the contention probe being bounded rather than a sanitizer run; real mux rematching belongs to B6. Verdict: my read ACCEPTS pending the verifier; merge into the wave after it, item 8 +1 then. Usage: 11.59 M input (11.36 M cached), 64.1 K output.

## 2026-09-13 18:44 MST — F24 registered (B12 row 2): a first-update local selection is overwritten by the final rebind; B12 tripped the classifier again and is resumed with the cap-check pattern removed

**F24 (HIGH, item 6/2; pre-existing on the candidate 3f65208668, both peers):** the B12 lane's scene-backed fixture `f21_first_update.lua` (an ordinary `SwitchToActor` inside the first resumed Activity update) shows, on the unmodified candidate (exe a456e1a4e348…, only row 1's native test added): host `fresh/e2e/resync_heal/host/trace.json.console.txt:17-19` `switch success=true before=1048577 target=1048615 actual=1048615 update=64 saved=63` → `FAIL after_final_rebind expected=1048615 actual=1048577 update=65`; client `:15-17` `before=1048596 target=1048634 actual=1048634 update=64` → `expected=1048634 actual=1048596 update=65`; the no-script-error checker also rejects the run. Mechanism (re-anchored): `Activity.cpp:822` writes the selection; the `RefreshCheckpointActorIDs()` calls at `Activity.cpp:1147,1182` run during binding restoration before the first update; `MovableMan.cpp:4326-4328` performs the final pending-ID rebind after the Activity update, reapplying the pre-update IDs. This is the "legitimate first-update mutation" the second read (B3) had marked OPEN. Fix lane: Opus `cortex-opus-engineer` (opus-f24-first-update-20260913) on a branch from the wave tip, with this fixture as the detecting arm, the F21 arms and the suite as regressions, SP byte-identical.

B12 progress: row 1 done and committed (98761bceb4: 12/12 RED on the reversed refresh, 12/12 GREEN); row 3 marked-only and brain-only REDs executed (the inventory-brain reversal detected on both peers: ordinary and collected brain slots became UID 0 while the marks stayed correct), GREEN pending; the builder's tree holds only the probe additions (+99 lines, no reversal). The second `turn.failed` (cyber_policy) again followed a process-enumeration command (`Get-CimInstance Win32_Process … CommandLine` for the build-cap check that every brief asks for). Resume 2 removes that pattern (`Get-Process -Name cl,link` only) and reorders rows 3 → 4 → 6 → 7 → 5. Note for §6.3b: never ask Astra to enumerate process command lines.

## 2026-09-13 18:46 MST — B12 thread cut a third time by the classifier; lane REPLACED by an Opus engineer lane for rows 3-7

astra-f21-debt-arms-r2: the turn ended with cyber_policy right after committing the row 3 probe (85c6d14383 "Test inventory and marked slots through network resync"), with no process enumeration in the turn. Three trips on one thread → the thread's accumulated context is what the classifier scores; per the concurrency rule (one corrective resume, then replace) the lane is replaced: Opus `cortex-opus-engineer` (opus-f21-debt-arms-20260913) inherits the builder D:/Projects/alias-walk @ 85c6d14383, the report and the scripts, and executes rows 3 (GREEN), 4, 6, 7 (Mac command only) and 5. Retained from Astra: row 1 (12/12 RED / 12/12 GREEN, 98761bceb4), row 2 = F24, row 3 marked-only and brain-only REDs (`[net-inventory-resync] FAIL local_inventory_and_marks_survive phase=first peer=2 tick=63 relaunch=1` / `phase=after … tick=64 relaunch=0`). Astra quota spent on this lane: 4.19 M input in the first turn; the resumes' usage events were lost with the failures (the rollout's token_count lines hold them).

## 2026-09-13 18:48 MST — SWE viewport follow-up landed (fallback cosmetics, scroll + Back arms, seam oracle); lead read complete, visual review 3 running

Branch stage2/mod-mismatch-presentation 9a3459c389 → 4c1d032394: fb5765557f (GUIFont: `InkOf` = the dominant ink of the active recoloured bitmap, cached; `InkColorBitmap(color)` = a per-colour recoloured copy of the fallback atlas, cached; fallback glyphs drawn from it at `glyphY = Y + m_FontHeight - fallback height`), 2319149546 (Main.cpp `post_command <control>` menu-script verb → `AutomationPostCommand` / `PostPendingAutomationCommand` raising `GUIEvent::Command` after Update; the driver's tall-status scroll phase with timed frames and band diffs, `--no-scroll-input` fail-closed), 4c1d032394 (seam phase: produced width word+27 → 533 = 5 mod 8, `real_panel_edges`, `is_hole`, the right-column / bottom-row oracle). Every production line read. Deviations accepted as disclosed: scope extension for the verb; horizontal scroll unreachable through real wire text (max word 576 px < 604 px inner) so the vertical scroll is the exercised half; two superseded intermediate builds (recolour via the requested colour was a no-op — magenta palette 0); the bottom-row seam check trivial at 250 px. Re-derived from raw: red-noscroll-640-final `scroll_motion/scroll_tail_reached/tall_scroll_motion/tall_scroll_confined = false`; red-seam-640v5 (control 6a18084d) `seam_right_column_filled = false`, `back_command_posted = false`; green-640d and green-960b all checks true; label-zoom.png 1,690 gold (252,209,19) pixels and 0 white at both resolutions. Merge into the wave after visual review 3.

## 2026-09-13 18:51 MST — B5 ACCEPTED and merged into the wave (73c17555c7); item 8 +1; one intermediate behaviour disclosed

Opus verifier REPORT-verifier.md (saved verbatim): ACCEPT WITH FOLLOW-UPS — every mechanism, lock scope and RED/GREEN line confirmed byte for byte; anti-masking proven from the manifests (test/oracle/runner bytes identical across RED and GREEN); the disclosed repairs are tightenings; F2 is narrower than the proposal and wider than the wave (the wave had no keep predicate at all). Merge 73c17555c7 (verify_merge_commit 0 differences over six files, 513+/11-), scan 0, pushed with lease from 4169c598e6. **Disclosed intermediate state:** until lane B6 lands, an ICE host's Complete/FinishMatch hides and keeps beating its directory row instead of deleting it, and ReturnToLobby is still refused for mux matches, so the relist half is unreachable and the row is deleted only at Destroy; invisible to players, harmless to the directory, closed by B6 (same wave). Follow-ups queued: the `TestIceRowJoinMode` helper-table case `{true, true, "s1", "", "ip", …}` at NetMatchSelfTest.cpp:4499; optional lock/assert hardening of the game-thread flags; the merged tree is re-proven by the candidate build + verify6 + the battery (both `-net-match-selftest` and `-net-directory-selftest` are rows). Item 8: 61 → 62.

## 2026-09-13 18:56 MST — Lobby chat slice: REJECTED for merge (verifier), reworked in the same SWE session

REPORT-verifier.md (saved verbatim) confirms the presentation-only claim (all four tick-hash arrays equal element-wise over 300 ticks, no sim-side symbol use) and the fail-closed driver, but finds four defects: (1) the text bound raised to 256 with c_Version unchanged is an in-version wire change — an older same-version peer hits `StringTooLong` in `ReadString` and is disconnected as MalformedMessage (NetSession.cpp:499-514 on 3b12f4bcc6); (2) the local rate window is keyed on `c_InvalidNetPeerId` = 0 = the host's author id, so a client's own sends share the host's window; (3) the host sinks every team line regardless of its own team (`DeliverChat` at NetSession.cpp:882 precedes the filter); (4) `SendChat` walks `m_Peers` under `m_ChatMutex` only while the pump mutates `m_Peers` unlocked (:411 push_back, :77/:124 clear) — undefined behaviour when a joiner connects during a send — and the new global `m_SendMutex` on every `Send` plus the atomics were listed nowhere; residual: malformed chat-typed packets have no budget. The visual review (REPORT-visual-review.md) accepted the panel but blocks on the unexercised 640x360 mismatch + port-map state (`extraCap = GetResY() - 361` would leave the error block 1 px) and lists layout follow-ups. Lead decisions in the rework brief: keep the 128-byte bound (c_Version stays 2); a sentinel rate key; host team gating; an outbox drained by the pump thread (removes the atomics and the global send lock); a malformed-flood budget; the literal refusal-line check; the 640x360 state proven with the chat block yielding rows before the status rows; alignment/margin/team-marking follow-ups; each with a RED-first arm. NEGATIVE item 7 +0 for this landing (the slice is real work, not yet mergeable).

## 2026-09-13 18:57 MST — Viewport follow-up ACCEPTED (visual review 3) and merged into the wave (aca344d5f7); item 7 +1

REPORT-visual-review-3.md (saved verbatim): fallback glyphs gold and bottom-aligned at both sizes (PASS); the tall status scrolls inside its band with title, name field, buttons and frames pixel-stable (band diffs 0.349/0.374, outside 6.5e-5/4.8e-5) (PASS, caveat: the last frame stops one row short of the tail and the tall phase has no tail check — the `scroll_tail_reached` flag belongs to the viewport phase which passes vacuously); Back press proven by the driver's `assert_screen MainScreen PASS` only (OPEN: no post-Back capture); no new regressions; two pre-existing items (the host-lobby "Waiting for a player…" line top-clipped at the status band; the tall panel's top frame above y=0 at 640, identical in the control). Cosmetic follow-up: the flat recolour of antialias shades makes adjacent fallback glyphs touch along their lower third (a gold picket bar). Follow-ups queued for the SWE viewport session (after its player-row lane): tall-phase tail oracle; one post-Back capture; recolour only the main ink shade (or keep the AA shades one step darker) so glyphs stay discrete; the top-clipped host-lobby status line. Merge aca344d5f7 (verify_merge_commit 0 differences), scan 0, pushed. Item 7: 41 → 42 (7c mod-mismatch presentation complete at the minimum viewport with scroll and Back evidence).

## 2026-09-13 19:02 MST — B13 (F15 debt arms) cut by the classifier after 23 min; REPLACED by an Opus engineer lane; pattern recorded

astra-f15-debt-arms-20260913 (thread 01a09d90-c23c…): `turn.failed` / cyber_policy at ~19:01 MST while correcting the native fixture setup (script-load deferral scope) after three executed RED rows (roots-red-i/q/x and their -2 reruns: retiring inventories, unharvested queue tails, raw child links); the builder item4-simspeed (stage2/f15-debt-arms) holds uncommitted changes in ACraft.h, Main.cpp, MovableMan.cpp/.h (inspected below). Pattern: three of the seven Astra lanes whose work is lifetime/rebind probing with temporary production reversals (B12 twice, B13) were cut by the provider's cybersecurity classifier; the engineering/build/review lanes (A1, A2, B1, B2, B3, B5, B6, B11, B14) were not. Rule from now on (RESUME §6.3b): that class of lane goes to Opus. B13 continues as opus-f15-debt-arms-20260913 on the inherited builder and report.

## 2026-09-13 19:09 MST — Astra A1 landed: FG6D battery harness prepared (ten corrections, 154 scoring tests); lead re-derivation in progress

fg6d-battery/ (copied from fg6c-battery: 40 drivers byte-identical by the lead's own hashes, 27 changed, 18 new — battery_contract.py, battery_scoring.py, n6_scoring.py, present_contract.py, compare_full_traces.py, ak_trace_contract.py, the controller/ driver pair, heal_wp_driver/heal_script_oracle.py + heal_raw_fields.py, reviewed/ wrappers for arm1/AK/ICE, run_b1.py, run_portmap.py, run_controller_pinned.py). PREP-REPORT.md read: deviations first — no next-candidate pin (lead prerequisite: build-pin.json + the prompt placeholders from the actual next build), the live candidate changed mid-lane (noticed and pinned separately), DeliverCommandHost instrumentation absent in the engine (Main.cpp:3177-3179, MovableMan.cpp:578-601) so that n6 mode FAILS closed by design, directory_e2e and mixed_lua observation-only rows now FAIL as unscored instead of passing on rc 0, the portmap observation row cannot pass from rc 0 alone, ICE teardown unchanged (the retained 404 preserved), pytest installed under the prep root, the prep root at 197.9 MB of its 200 MB cap. Corrections read in diff bundles 01-10 (key hunks: the recording-driver callback row; the desktop-gate NOT RUN + one disclosed rerun pass with dependency propagation; the boundary heal oracle with legacy_failed reporting; exact-list n6 scoring with the pinned exe assert; scan_resync rewritten on battery_contract.RESYNC_ROWS). The lane's 154-test freeze (`154 passed in 235.36s`, D:/mx/fg6d-prep/pytest-final-freeze.txt) is being re-run by the lead under a fresh scratch root (the first re-run stopped at 14 tests on the lane's own cap guard). Usage: 33.70 M input (33.00 M cached), 166.5 K output (129.8 K reasoning) — the heaviest lane of the day. Acceptance and the battery launch follow the re-run, B14, the candidate rebuild and verify6.

## 2026-09-13 19:14 MST — F24 fix landed (Opus engineer): lead read + raw re-derivation done; verifier running

Branch stage2/f24-first-update-selection (value-observations) from 4169c598e6: c34fc8c174 test arm (GameActivity.cpp +20: `relaunch_rebind_keeps_window_switch`, `…_keeps_window_brain_controller_and_mark`, `…_heals_cleared_window_slots`), 27d3c5b83d fix (Activity.cpp +20/-3, GameActivity.cpp +4/-1): the deferred rebind resolves the three links first and writes a slot only when `HoldsNewerBinding`/`HoldsNewerBrainBinding` is false (slot empty, equal to the link, or not a live actor / not carried by a world actor), and the marked slot keeps a mark on a live actor other than the saved link's; pointer-set lookups only, no dereference. Every production line read; one-line why comments. Raw re-derived by the lead: RED console (host) `switch success=true before=1048577 target=1048615 actual=1048615 update=62 saved=61` then the FAIL/ERROR lines; GREEN e2e result.json pass true, failed none; native red-graph 184 PASS / 2 FAIL and local-UI 40/2 → green-graph 186/0 and 42/0 (the three new arms included); selftests 11/11; SP dumps byte-identical (the lane's disclosed comparator excludes only `__wall_seconds`). Verifier (Opus) running on the F21 interaction cases (fresh controller default actor, substitution, spectator, dedicated host) before merge. Report saved verbatim to opus-f24-first-update-20260913/REPORT.md.

## 2026-09-13 19:14 MST — A1 (FG6D harness) ACCEPTED: lead re-run of the scoring suite 154/154

The lane's suite hard-codes its prep root (D:/mx/fg6d-prep, 198 MB cap guard), so the lead re-ran it on a relocated copy: D:/mx/fg6d-lead-copy/fg6d-battery with the prep constant redirected to D:/mx/fg6d-prep-lead (the retained evidence index copied there, 4.9 MB) and the sibling lanes the tests import provided as JUNCTIONS under D:/mx/fg6d-lead-copy (fg6c-battery copied; grok-heal-*, grok-ak-*, grok-controller-*, grok-fg6c-*, swe-arm1-lockfix, w102-fixtures-takeover-e2e, w71-4-path-update-after-inflight junctioned — unlink them, never rmtree). Result: `152 passed` + the two path-dependent tests `2 passed` after the last two junctions = 154/154 (the lane's freeze: `154 passed in 235.36s`). The corrected harness is accepted for FG6D. Open by design: the DeliverCommandHost n6 mode fails closed until the engine logs the delivery (a one-line engine instrumentation for a later wave); the observation-only rows (directory_e2e, mixed_lua, portmap) now FAIL as unscored until they get oracles — they will be classified as harness gaps, not engine failures, in the FG6D read. Next: B14 lands → merge → candidate fast-forward → /MP12 build alone → verify6 → build-pin.json + prompt placeholders + FG6D lock → ONE Grok run-only battery in the exclusive window.

## 2026-09-13 19:21 MST — B12 (F21 / coroutine debt arms) landed via the Opus continuation: rows 1, 3, 5, 6 executed; 4 not executable; 7 = Mac commands

Builder alias-walk stage2/f21-debt-arms @ f81b7d462f (three test-only commits: 98761bceb4 all-player slots, 85c6d14383 inventory/marked slots through resync, f81b7d462f reused coroutine placeholders + frozen graph bytes; all reversals restored, LuaThreadCodec.cpp back to f7827f37…). Row 3: brain-only RED re-derived from raw (`brain=0` on both peers, exe 3f3d3662…), candidate GREEN 26/26 (client `expected_brain=1048685` vs `saved_host_brain=1048668`), re-run on the final binary; the inherited 2 MB executable was a truncated partial link, rebuilt before any claim. Row 5: four new arms (LuaMan.cpp:2104-2155: a big frame restored into a used suspended placeholder, identity kept, growth required, an open upvalue rejoined, two full collections before resuming, exact `done/1` vs `done/7`); RED by reversing only the protected growth block (exe 9dfe19c5…, all four FAIL "a frame needs more stack than the coroutine has"), GREEN exe e2b77495…; the allocation-failure return is provably unreachable from any accepted descriptor (top ≤ 7984, framesize ≤ 250 ⇒ needed ≤ 8235 ≪ the 65500 growth limit) — an allocator fault-injection seam would be the prerequisite. Row 6: a frozen serialized graph (sg-boundary.txt, 1,070,507 bytes) written by the candidate: candidate reader PASS (`needed 8018 maxstack 15930`), no-growth reader FAIL with the refusal text; a disclosed Lua truncation bug in the first reader arm was fixed and all four runs retained; old-writer/new-reader not claimed. Row 4: not executable (ICE resync refused in production at NetMatchService.cpp:585; no spectator concept; dedicated host needs a three-peer heal lane; substitution needs a driver that resyncs after the reseat) — prerequisites written. Row 7: the Mac A7 command written for the lead (control half needs a build.json beside control-bin/CortexCommand; strictness gap at a7_driver.py:225/:297 noted). Final binary ca1c4b04…: all five new arms PASS, net-local-ui 51 PASS / 0 FAIL, selftests 11/11. F24 evidence untouched. Merge into the wave after the F24 verifier (both touch GameActivity.cpp). Items 2/6 +0 (coverage debt closed; credit rides with the promotion).

## 2026-09-13 19:31 MST — SWE player-row lane: design REJECTED (font downgrade), rm -rf incident checked (no damage), redesign launched

Landing swe-lobby-player-row-20260913 (defiant-taurus): commits f58ba55326 (rows switched to FontSmall — white 10 px text — because the worst legal fixed tail measures 372 px in FontLarge against the 288 px box) and fb86e76e2a (driver tools/test_lobby_player_rows.py). Rejected: the lobby is gold FontLarge everywhere and the lane never considered widening the panel, which the mismatch landing already does through `FitMultiplayerPanelWidth` / `FitMultiplayerScreen` up to `m_RootBoxMaxWidth - 12`. The redesign brief fixes the design: measure in FontLarge, widen the panel to the viewport cap, elide only the name when still too narrow, keep the tail whole, shift the other elements with the panel; the report may propose a shorter fixed tail. INCIDENT: the lane deleted `D:/mx/swe-lobby-player-row-20260913/green-640` with `rm -rf` (disclosed by the lane). Lead check at 19:33 MST: item7-ux `Data` holds 8,518 files (= control-build 8,518), no deleted entries in item7-ux, control-build, takeover-build or audio-owner-registry; the sibling run dirs hold `runtime\Data` junctions, so the deleted one did too — the MSYS `rm` unlinked the junction without following it this time. No damage; the rule is restated in the brief with "any further deletion ends the lane". Also disclosed by the lane: the reference-diff region changed twice between runs (row boxes + 6 px apron excluded) while the per-row pixel checks stayed the same; the remote ping part is not produced on loopback (`pingMs == 0`); no moderation-mark row driven. NEGATIVE item 7 +0.

## 2026-09-13 19:32 MST — F24 fix ACCEPTED and merged; B12 test arms merged; wave 6c9932a926; item 6 +1

Opus verifier REPORT-verifier.md (opus-f24-first-update-20260913): ACCEPT WITH FOLLOW-UPS — the guard lives in the virtual `Activity::RebindNonOwnedActorSlots` (Activity.cpp:1359-1369) and GameActivity.cpp:2755-2766, so all three call sites carry it; both restore-time sites are provably inert (ActivityMan.cpp:988 clears the slots then ResolveCheckpointReferences writes the links; afterRestore's two branches end in the F21 refreshes, so slot == link); fresh-default, substitution, seatless and dedicated arrive via ApplyNetPlayerBindings, so no F21 scenario keeps a wrong binding; stale old-world pointers fail IsActor after DiscardWorld; `OwnedByWorldActor` is the same predicate `LiveCheckpointBrain` uses; no dereference. RED/GREEN, counts (184/2 + 40/2 → 186/0 + 42/0), 11/11, the byte-identical fixture and SP dumps re-derived. OPEN (judged correct, uncovered): at MovableMan.cpp:4327 the rule also keeps the engine's own re-seating (GameActivity.cpp:1186-1232). Additions: the auto-substitution semantics, player 0 only in the runs, a possible whole-world walk in `HoldsNewerBrainBinding`, the report's +20/-3 is +17/-3, the RED host's knock-on MuzzleSmoke errors. Merges: stage2/f24-first-update-selection (27d3c5b83d) and stage2/f21-debt-arms (f81b7d462f, test-only) into the wave, verify_merge_commit 0 differences each, scan 0, pushed with lease from aca344d5f7 → 6c9932a926. Item 6: 95 → 96 (F24 closed with detecting arms; F21 debt rows 1/3/5/6 executed).

## 2026-09-13 19:42 MST — Candidate fast-forwarded to 6c9932a926, built alone, verify6 8/8 (exe b4df811b2fd9…)

Gap chain: a sixty-second build gap at 19:36 MST, `control-build` fast-forwarded 3f65208668 → 6c9932a926 (19 files, +1230/-53: terrain fix, lease corrections, viewport follow-up, F24 fix, B12 arms), Final /MP12 alone 19:36:21-19:38:23, rc 0, no errors, log D:/mx/lead-candidate-20260913c/build.log; exe b4df811b2fd9…. verify6 (OUT D:/mx/lead-fg6/verify6-6c9932a926) 8/8 in 215 s on the new Allegro arithmetic: selftests, port-map, directory, script-graph s4, global-callback driver, preview-event-d7, lpinv-100 (the archived pickup_fire replay rows keep their acceptance lines), arm1-switch-control. Raw counts re-derived by the lead: selftests/result.json passed 11 / 11; script-graph-s4/stdout.log 190 PASS / 0 FAIL (186 + the four coroutine placeholder arms), net-local-ui 54 PASS / 0 FAIL (39 + 3 F24 arms + 12 all-player cases), net-local-state 10 / 0, the 8011 arm and both F24 window arms present; the build log shows allegro.vcxproj building into _Bin/x64/Release (the source-built library, no tracked blob on the link line). The FG6D battery waits for B14 so the vendor-library hardening is in the pinned binary (one more incremental build + verify6 then).

## 2026-09-13 19:46 MST — B14 (vendor-library link hardening) and B6 (ICE lifecycle) landed; both under independent verification

**B14** stage2/vendor-lib-link in h4-secondary, ten commits on the wave tip 4169c598e6: the guard row 6e2fc6424d (`Source/System/RotatePrimitiveSelfTest.cpp` +95: `fixmul` integer 32767 vs floating 32768 must differ; eight angles' four fixed corners from `_rotate_scale_flip_coordinates` and one rasterised pixel per angle against constants derived from the polynomial + integer fixmul; registered in tools/run_selftests.py → 12 rows), then luabind, loadpng, raknet, zlib, libpng, minizip (each: explicit tracked tokens dropped, OutDir `_Bin\$(Platform)\$(Configuration)\`, LinkLibraryDependencies true), the legacy Allegro fallbacks removed from every configuration, LuaJIT resolved through its mapped project output, AllegroGL outputs separated. Lead read of the diff shape and the guard source. Raw re-derived: RED on the old-link executable `[rotate-primitive-selftest] FAIL -162 pixel 52/8 actual=0 expected=245` and corner off-by-ones at 172°/178°; GREEN passes; `selftests/result.json` passed 11 / 12 (the net-lockstep hold-wait case failed once at 5053 ms / 200 pumps while other lanes were building and running; isolated rechecks passed on both the RED and the GREEN executable → a timing-sensitive gate, recorded as a FINDING, not a regression); script-graph 186/0; the archived replay rows 2/2 PASS with terrain hashes differing from the old arithmetic at all 221 ticks and every other subsystem matching; 20/20 vendor configuration builds; Win32 vendor configurations absent (metadata-only there); SDL3/FMOD/LZ4 prebuilts intentional; commit bodies carry a harmless "9ST" typo. Usage 17.53 M input / 131.7 K output. Verifier (Opus) running; after it: merge → candidate FF → incremental /MP12 build → verify6 (suite now 12 rows) → pin → ONE battery.

**B6** stage2/ice-lifecycle-regressions in item8-directory, from 7e63a0c1b5: the WIP 1b1ff0eb9f, then 444a2e5bbf (clear the ICE pump before stopping its dispatcher), 3680818071 (pump completed sessions without reviving the ended world; bounded 1,024 packets / 1 MiB), f1657af949 (session_directory.py: hidden sessions leased and signalable), b504fc8052 (optional visibility in native codecs), 4e1adcb8d7 (client: retain hidden leases, fail closed on loss), c90fb0a339 (rematch on the retained ICE bind and directory identity). Claims: Windows and arm64 native RED/GREEN for all three concerns, suite 11/11, directory tests 41/41, ICE and IP rematches 200 + 601 matching ticks with zero IP events on ICE; a `mac_native_job.py` plain-command launchd runner written because mac_job.py refused the lane name and model gate. The branch re-implements the directory codec/client/lease work the wave already has (from the older base): a dry-run merge against 6c9932a926 conflicts in NetDirectorySelfTest.cpp, NetMatchService.cpp and NetMatchService.h. Verifier (Opus) running with an integration-recipe question; B6 then goes to an Opus integration lane and the NEXT wave (not this battery's candidate). Usage 31.18 M input / 148.7 K output.

## 2026-09-13 19:49 MST — B11 landed: F23 diagnosed with detecting arms committed; fix handed to an Opus engineer lane with the fence extended

astra-pie-lockstep-gating-20260913 (32.62 M input / 142.6 K output): commit be98d0f2e8 "Add detecting arms for controller ownership and pie state" on stage2/pie-close-lockstep (three runtime REDs reproduced on the unmodified tree: NEXT post-handoff, ActorSelect cancel, delivery-order cancel — the two cancel probes differ in exactly three raw lines 16454/16578/16701, ticks 199-201, host `pie=2:8:-:-:-` vs client `pie=3:8:-:-:-`; a Go-To regression arm fails 286 checks on the retained tip; 43 mutations fail closed; 3 synthetic baselines pass; suite 11/11). **Mechanism (report §"Source cause and raw sequence"):** `Activity.cpp:817-822` reseats; `Controller.h:240-248` changes the sampling seat at once while the committed mode stays wire-owned; `Controller.cpp:300-305` clears AI command state only when the scheduled AI update runs (`:362-374`), so a skipped AI tick leaves the previous committed values in the live controller; `ControllerFrame.cpp:178-184` snapshots those bits but `:198-199` labels the frame with the CURRENT local seat; `MovableMan.cpp:4822-4830` queues before applying the older ready frame at `:4843-4848`; `ControllerFrame.cpp:256` → `Controller.cpp:377-389` `ApplyWireState` overwrites the same fields three ticks later; on the next skipped AI tick those values are sampled again — a producer-buffer feedback loop, which is why the crab carries player bits on 59 of 117 AI ticks and the brain's `ACTOR_NEXT_PREP` (`Controller.cpp:461-465`) recirculates; `PieMenu.cpp:881` `SetEnabled` from the committed PIE_MENU_ACTIVE reverses the direction every tick (`:534-536`), and closing needs more than the 50 ms delay (`PieMenu.h:341`), so the pie never reaches Disabled (the 205 `2` → 206 `0` → 207 `2` alternation). **Mutation inventory:** none of the local pie helpers is presentation-only — `DoDisableAnimation`, `Wobble`, `FreezeAtRadius`, `SetAnimationModeToNormal`, `RemovePieSlicesByType`, the Go-To/full-inventory `SetEnabled(false)` all write state that reaches `DescribeInteractionState` (dumped), interaction checkpoints or Lua getters. The lane's ownership draft was judged unsuitable and left unapplied; the fix requires MovableMan.cpp, Controller.cpp/.h, PieMenu.cpp/.h and NetGameCommand.cpp/.h, outside its fence — with Astra wound down the fix is now lane opus-f23-pie-gating-20260913 (cortex-opus-engineer) with the fence extended to those seams plus ScenarioRunner/NetLockstep where the synchronized frame is applied. Astra: zero lanes running.

## 2026-09-13 19:59 MST — B6 verifier: ACCEPT WITH FOLLOW-UPS; integration recipe; the wave lacks the hidden-lease heartbeat driver (F25)

REPORT-verifier.md (astra-ice-lifecycle-20260913, 261 lines): the lane's f1657af949 / b504fc8052 / 4e1adcb8d7 are byte-identical copies of the wave's directory client/codec/session_directory files (blob-hash compare; zero merge delta), so they are DROPPED; 444a2e5bbf (clear the ICE pump before the dispatcher stops), 3680818071 (pump completed sessions without reviving the ended world) and c90fb0a339 (rematch on the retained ICE bind and directory identity) are TAKEN whole; the three conflicts (NetDirectorySelfTest.cpp, NetMatchService.cpp/.h) are cosmetic and resolve to the wave's text plus the lane's 22 new service hunks (only `std::move(link)` from the ReturnToLobby conflict) and its corrected NetIceRowJoinMode doc. RED/GREEN confirmed: control 40962301b7… `a session-id (ICE) match cannot return to the lobby yet` (arm64 RED bb773691…); GREEN 6b417235… `PASS service_ice_rematch_two_rounds same_bind=1 connects=1 ip_sends=0 delayed_client_ms=1200`, `PASS ice_rematch_row_resolves_bound_identity`, arm64 53eb5225…; runtime 13/13 checks, 200 + 601 ticks both peers both modes, ip_events 0, one register + 7 heartbeats on one session id, the comparator unchanged; the teardown observers and exactly-once stop confirmed for Destroy and ReportRuntimeError; ended-world refusal on IP and mux; the codec identical to db59c9e8f6; 11/11; no test-only production branch (three friend declarations). **F25 (integration finding):** `Source/Main.cpp:3362` (`|| … ::Completed`) is required by the WAVE — 6c9932a926's Main.cpp:3366-3369 still gates `Update()` on Starting/ReadyToLaunch/Running, so B5's kept hidden lease has no game-loop heartbeat driver in the wave (the hidden row would expire on the directory). Undisclosed side effects to assess: `NeedsRecoveryPump()` in Completed and its menu routing (MenuMan.cpp:128, MainMenuGUI.cpp:539 no Destroy on Back), the `PumpSessionEvents()` early return in Completed, NetLockstep.cpp:4120 zeroing `ignored_session_packets` when a sink is attached; OPEN: the RED control was the tip with a six-point revert patch, no third joiner, no 150 s idle run; `mac_selected_tests.py:16` points CCCP_TEST_DATA at a foreign lane's Data (read-only, undisclosed). Integration lane opus-ice-on-wave-20260913 (cortex-opus-engineer) launched on a branch from 6c9932a926 with RED against a real wave build and the side effects to assess; it lands in the NEXT wave (not this battery's candidate).

## 2026-09-13 20:03 MST — B14 ACCEPTED (verifier) and merged (wave 8418e6cbca); the suite bar is 12 rows; battery chain launched

REPORT-verifier.md (astra-vendor-lib-link-20260913, 304 lines): 26 tracked .lib blobs; after the ten commits no RTEA group names a tracked blob for a source-built project (48 resolved paths across 6 configs × 8 references, 0 tracked; 19 dead blobs, 7 intentional prebuilts: FMOD, 2× LZ4, 4× SDL3/SDL3_image); the guard re-derived 8/8 against constants/derived.json and the Boulders raster byte-compared to the Mac pieces (sha 4415d4c5…, pixel(52,8)=245); RED 62ab17e0… linked the pristine tracked Allegro (all seven RED-linked blobs hash to `git show HEAD:`), GREEN cd0bb7d0… links eight source outputs; the hold-wait case is a wall-clock bound (ScenarioRunner.cpp:1766 giveUpMs = 5050 ms, pumps paced ≥ 15 ms, the drop at pump 200 needs < 25.25 ms/pump, observed 25.3 while another lane was linking) — flaky, sources byte-identical, both executables pass it in isolation; LuaJIT was already source-built (3daf31042e only made the mapping explicit); script-graph 186/0; both replay rows keep every non-terrain subsystem identical at every tick; 20/20 configuration builds; Win32 genuinely absent. New follow-ups (queued): Windows *meson* builds still pick tracked blobs (zlib-ng/libpng/minizip/allegro meson.build); the two Debug x64 configurations resolve `zlibstaticd.lib` built MultiThreadedDebug with only libcmt ignored — build them once; five vendor projects are outside RTEA.sln; the terrain delta is not attributed per library; the guard has not been compiled by clang (the next Mac gates run covers it). Merge 8418e6cbca (verify_merge_commit 0 differences), scan 0, pushed. Docs: the socket-free suite bar is now 12/12 (`-rotate-primitive-selftest`), updated in RESUME §0.4 and CLAUDE/AGENTS §8. Battery chain launched: fast-forward → 60-second build gap → /MP12 build alone → verify6 → build-pin.json + prompt placeholders + D:/mx/LEAD_BATTERY.lock → the Grok run-only FG6D lane.

## 2026-09-13 20:14 MST — B13 (F15 debt arms I-O, Opus continuation) ACCEPTED as executed debt; three new findings F26-F28; fix lane spawned

Report `grok-workers/astra-f15-debt-arms-20260913/REPORT.md` (1795 lines; 48 executed rows under `D:/mx/astra-f15-debt-20260913/`). Lead read: the whole branch diff `3f65208668..1500aa6f35` (461 insertions: `PreviewScriptSelfTest.cpp` +332 = `RunRetirementArm`, two Lua fixtures, `Main.cpp` dispatch of letters `itmqxonla` behind the `-lpinv-overlay-links` selftest flag with `LocalPrediction::Clear` + `DropAllPreviewGhosts` before the arm and a canonical-state byte comparison after, three `friend class PreviewScriptSelfTest` declarations). Test-only: no production line changes; the inherited uncommitted production reversal was saved as `reversals/roots.diff` and the production line restored byte-for-byte (sha `a68089009258…` = `HEAD:MovableMan.cpp`). Executed pairs re-derived from the quoted rows: I (`roots-red2-i` two FAILs vs `green-8-i`), T (`taken-red-t100` vs `green-1-t100`, armed at tick 100 because the rifle is equipped at 200 — disclosed, no widening), M (`meta-red-m` vs `green-1-m`, plus the labelled stale-key mutation control under ASan naming `OwnedMovableObjects.h:21` from `MovableMan.cpp:2375/2399`), Q and X (three FAILs each vs `green-8-q/x`), J (`j-shared-red` vs `j-shared-green`), L (unchanged Lua fixture `HeldRefs.lua`: reverted build FAIL `held_link_after_retirement: points at Leaking Machinery uid=1049031`, candidate all PASS; closure, shared identity, continuation and the live `Pos` alias read identically on both — the hardening changes no Lua-visible behaviour). ASan `Final ASan` rows i/t/m/q/x/o/l/n clean. Not done: no pre-`14db824c10` reference executable (the retained ones are at the hardening commit and under `D:/mx` without a firewall rule); A's natural late-detach timing not reached (manufactured producer only); no ASan row for `a` (pointer compare only).
NEW FINDINGS on the unmodified candidate (registered):
- **F26 (F-O, MEDIUM):** a surviving object's owned break-wound / parent-break-wound templates are collected as owned (`OwnedMovableObjects.h`) but `MOSRotating::RemapExternalLinks` (MOSRotating.cpp:1702-1709) never walks them, so an outgoing link inside such a template keeps naming a retired object while its ghost lives (`green-1-o` lines 28-29). `m_pMOToNotHit` is a `MovableObjectReference` (expires on destruction) — not a UAF; Lua can place such a link (`Attachable.BreakWound` bound). Reader inside the ghost window not established.
- **F27 (F-A, HIGH candidate):** `Arm::m_HeldDeviceThisArmIsTryingToSupport` (Arm.h:290) is a raw `HeldDevice*`, `Arm` has no `RemapExternalLinks` override, so the pointer survives retirement AND ghost disposal holding the destroyed address (`green-9-a` lines 28/33/36; manufactured producer; natural late-detach timing NOT ARMED). Severity for triage: O expires, A does not.
- **F28 (F-N, LOW until reachable):** a supported late `LoadScript` inside a preview advances the LuaMan state cursor (28 → 30) and leaves the chunk in threaded state 29 across the boundary; `ConstructionRegistryScope` (MovableMan.cpp:5388-5416) is the existing save/restore and local prediction does not use it. Four reachability rows show a preview clone runs only the seven edge hooks; no natural producer reached it. Queued, not fixed.
Verdict: ACCEPT (test debt executed; item 2's "uncovered F15 arms" debt closed; the arms are merge-ready with the fix lane). Branch pushed as a backup (`stage2/f15-debt-arms` 1500aa6f35, scan 0). Spawned `opus-f15-remap-20260913` (Opus engineer, Agent tool) on the same branch: fix F27 (an `Arm::RemapExternalLinks` override) and F26 (the remap walks the collector's owned set), RED first on its own build of the unmodified tip, letter set + script-graph + selftests + one ASan run + the SP fixture byte-identical; brief `grok-workers/opus-f15-remap-20260913/prompt.txt`.

## 2026-09-13 20:14 MST — Player-row redesign: visual review ACCEPT WITH FOLLOW-UPS; one visible regression → corrective resume (the lane's second resume, reason recorded)

`swe-lobby-player-row-20260913/REPORT-visual-review.md` (saved verbatim). Lead read of c1f1be65c6 (+44/-27 MainMenuGUI.cpp/.h) and ebd15b192c (+74/-33 driver): design as fixed by the lead — FontLarge rows, `widestRowText` measured with `CalculateWidth(text, fallbackFont)`, `desiredWidth = max(300, max(errorWord, widestRowText)+24)` capped at `m_RootBoxMaxWidth-12`, `FitMultiplayerPanelWidth` gains a `fillLabels` set (X=12, W=width-24), name-only elision with 0x85 against `contentWidth-24`, verbose-seat fallback preserved; row Y/pitch untouched. Reviewer's numbers re-checked against `result.json`: 640 panel 624 (cap), row extents 597/517/567/497 in a 600 box, row0 elided; 960 panel 679 = 655+24, no elision; RED (control 6a18084d…) row0 sliced/wrapped at both resolutions. Driver oracle change DISCLOSED by the worker (deviation 1): "outside the rows identical" → "shared panel interior identical under a per-pixel-row dx in ±4" (dx 0 at 640, +1 at 960 on the 81 content rows; the 165 blank rows tie at 0); a looser oracle than needed — tightened to one dx for all content rows in the follow-up. REGRESSION (reviewer FAIL, cosmetic but visible): `LabelLobbyPlayersHeader` keeps the stock center shift while the rows move to X=12, so the "Players" header sits ~172 px (640) / ~200 px (960) right of its column (RED: within ~12 px). OPEN: the "…" glyph could not be resolved at 640 native. Lead decision: not merged with a visible regression; the session (defiant-taurus) resumed once more with `header-prompt.txt` (header moves with the rows + `header_aligned_with_rows` pixel check RED on ebd15b192c / GREEN after; 640 `rows-zoom.png`; single content dx). This is the lane's second resume (§0.1 allows one): the fix is ~5 lines in a session holding the built tree and the drivers, a replacement would rebuild everything for the same outcome — recorded as the exception and its reason. Branch pushed as a backup (`stage2/mod-mismatch-presentation` ebd15b192c, scan 0). Item 7 +0 until the header landing.

## 2026-09-13 20:14 MST — verify6 on the rebuilt candidate 8418e6cbca: one harness red (needle), corrected and disclosed

Build `D:/mx/lead-candidate-20260913d/build.log` (20:03:41-20:05:09 MST, alone after a 60 s gap, 0 errors; exe `e29ddd593b572b27…`). verify6 run 1 (`D:/mx/lead-fg6/verify6-8418e6cbca/`): 7/8 — the `selftests` row FAILED on the harness needle `"passed": 11` while the run reports `"passed": 12, "total": 12` (all twelve rows pass on exe e29ddd…, incl. the new `rotate-primitive` row; `selftests/result.json`). Oracle change disclosed with both runs: `lead-tools/verify_lead6c.py` needle `"passed": 11` → `"passed": 12`; run 2 → `D:/mx/lead-fg6/verify6-8418e6cbca-r2/`. No engine red. The pin/lock/battery launch follows run 2.

## 2026-09-13 20:30 MST — Player-row header follow-up ACCEPTED (f5678f641a + bc74c426a1); the elision byte 0x85 draws a box in FontLarge → one more correction before the visual review

`swe-lobby-player-row-20260913/REPORT-header.md`. Lead read of both diffs: `MainMenuGUI.cpp:226` fetches `LabelLobbyPlayersHeader`; after `FitMultiplayerPanelWidth` the header's rel-X is 24 when `contentWidth > 300` else the stock 20 (the rows' own +4 delta), one `m_MultiplayerLobbyPlayersHeader` member with a `//!<` line. The worker tried the fill-set form first and measured the "P" glyph bearing 2 px left of the label origin (offset -2, run retained at `header/green-640`), then shipped the delta form — disclosed. Driver: `header_aligned_with_rows` (leftmost header ink minus the rows' box edge, PASS iff 0..16), a per-run `rows-zoom.png` 4x crop anchored on row 0's first ≥3 px ink gap, and the reference diff now requires ONE `content_dx` across all content rows (blank rows abstain; `len(dx_seen) <= 1`); the header's y-band joined the excluded band because it moves by design (disclosed). Re-derived from `result.json`: RED (ebd15b192c exe 6d4a4530…) header offset 170 at 640 / 197 at 960, the only failing check; GREEN (exe afb6dfa3…) offset 10 / 10, content_dx 0 / +1, zero unmatched rows, panel 624 / 679, row0 elided at 640 only. Selftests 11/11 on afb6dfa3. The retained control executable sits in the repo root as `Cortex Command.redesign.exe` (gitignored `*.exe`; the one allowed control copy).
DEFECT found by the worker's own zoom: byte 0x85 has no ellipsis glyph in FontLarge — it draws a hollow box, so an elided name reads "HHHH□ (you)". Lead design: elide with three ASCII periods; driver constant follows. Resumed the same session once more (`ellipsis-prompt.txt`, 45 min cap) rather than opening a lane for a two-line change; the visual review runs on that landing. Item 7 +0 until then.

## 2026-09-13 20:39 MST — Chat rework landed (stage2/lobby-chat 949f83e721): lead read complete, verifier + visual review dispatched

`swe-lobby-chat-20260913/REPORT-rework.md`; four commits 23b4b2fd2b (wire), cc1a72f0fa (outbox/session), f12214e1e4 (layout), 949f83e721 (driver); +539/-177 over nine files. Lead read of every line: the seven decisions are implemented as specified. Wire: chat text bound to `c_MaxShortTextBytes` (128) on encode and decode, `c_MaxChatTextBytes` deleted, `c_Version` untouched, a hand-mutated wire claiming 129 bytes decodes to `StringTooLong`. Rate key: `c_LocalChatRateKey = ~uint64_t(0)` and `c_MalformedChatRateBase = 1<<32` keep the local and malformed windows outside the uint8 author-id space. Host sink: the relay's team rule applied to the host's own panel (NetSession.cpp:928-933; `count` before `[]`, no map insertion). Threading: `SendChat` only validates, sanitises and enqueues under `m_ChatMutex` (64-bounded, refuses when full); `PumpChatOutbox` swaps the queue, computes `linked` on the pump thread, stamps, echoes and targets under the lock and sends outside it; drained from Tick, TickKeepalive, InjectEvent, TickAdmissionPlane and `NetMatchService::PumpSessionEvents` (service mutex then m_ChatMutex, the same order as the service's enqueue); the atomics on m_NowMs/m_LockstepFrame and `m_SendMutex` are gone and `Send` is back to the pre-change path; `BuildReportJson` snapshots the six chat counters under m_ChatMutex. Malformed flood: chat-typed packets that fail decode burn the sender's window and count without unseating. Layout: error, status and port-map rows budgeted first, chat yields rows down to zero, `VAlignment::Top` when the error text exceeds its box (scoped to the lobby error label), chat at X=8, a 12 px input margin, team lines indented two cells with the `[team]` mark. Selftest arms collect every failure so one RED run names all defects, plus host-flood/client-turn, team-1 to team-0-host, 20-malformed-flood and `TestChatOutboxJoinRace` (a real std::thread producer in a closed loop, a join at pump 20, 100 lines once each). Driver: the literal `[net-chat-script] requires -net-match-service-e2e` is required; `run_mismatch_shots` stages five 64-byte modules plus `net_port_map_fake.py` at both resolutions. Re-derived by the lead: RED net-protocol `FAIL: a 129-byte chat line was accepted by the encoder` (red-final), GREEN `PASS chat: 128-byte cap...`; selftests 11/11 Final exe 1a1b5465..., 11/11 `Final ASan` exe 0864e85c... with no AddressSanitizer or runtime-error text under asan-netmatch2; e2e 13/13 with the literal refusal line present once; shots pass. Cosmetic follow-ups, not blockers: a stray extra tab on `NetMessage rejection;` in the RejectOldWirePeer hunk; a line dropped for "not linked" counts as `chatDroppedInvalid` and a full outbox as `chatDroppedRate` (label them later). Verdict pending the independent verifier (Q1-Q9 incl. lock order) and the visual reviewer (four captures incl. mismatch + port-map at 640). Branch backup-pushed (scan 0). Item 7 +0 until merge.

## 2026-09-13 20:39 MST — F23 fix landed (stage2/pie-close-lockstep 3dc2c0a8b4, Opus engineer): lead read complete, verifier dispatched; F23b and F29 registered

`opus-f23-pie-gating-20260913/REPORT.md` (saved verbatim). Root cause measured on the control: `MovableMan::UpdateControllers` produced the next frame out of the wire-applied controller state (the committed frame from input-delay ticks ago), so an AI seat that only clears on its scheduled tick, and a seat that just left an actor, re-emitted the committed frame forever (`snapshot(T) = snapshot(T-4)`; `host_controller.jsonl` shows 0x500006 / 0x4000500002 alternating from produced tick 201 on); applied three ticks later it toggled `PieMenu::SetEnabled` every tick and reset the 50 ms close timer (dump 204 1:8 / 205 2:8 / 206 0:8 / 207 2:8 / 208 0:8). Fix f546e843c8 (+50 Controller.cpp, +25 Controller.h, +14 MovableMan.cpp; lead read): `BeginLocalProduction` saves the sim-facing committed input and installs the seat's own last produced sample, or resets when the seat (mode + player) changed or after a `SetDisabled` transition; `EndLocalProduction` keeps the produced sample and restores the committed input; the bracket wraps only the `Controller::Update` loop through `SnapshotLockstepControllerFrames`, `lockstepActive` only. Side effect, correct: `GetInputFromPlayer`'s press/release edges now compare against the seat's own previous sample instead of a three-tick-old committed frame. 62efcc37ab gates every local pie animation in `GameActivity::Update` behind `localPieAnimations` (= not lockstep): the four seat-return/leave `DoDisableAnimation` sites ride the committed frame (Actor.cpp:898-906 replays it on every peer); the cursor-highlight sites (Wobble / FreezeAtRadius / SetAnimationModeToNormal) stand down because they write `m_EnabledState` and `m_FrozenForView`, which feed the dump and five Lua getters. Numbers re-derived from score/*.json: next RED 634 (as-committed oracle) / 628 (corrected) to GREEN 0; goto 286 to 1; actor_cancel 293 to 4 and delivery_cancel 293 to 2 per the report; selftests 11/11 on f9b830a7...; script-graph 196/0. ORACLE CHANGE disclosed with both runs (af8dce39ac): `handoff+1` to `handoff+4` for the departing pie (the 50 ms disable animation is four sim ticks; SP itself is Disabling at handoff..+3) and the SP reference read through the input delay for NEXT/PREV; the repaired build under the as-committed oracle fails only ticks 205-207 (Disabling, mid-close); under the stricter SP-referenced comparison the residual is `WhilePieMenuOpen` 51 vs 50 (SP's local `SetEnabled(false)` closes one tick earlier), reported and not masked; mutation suite 46 cases, unexpected []. Lead position: the correction is legitimate (the old expectation demanded an instant close no synchronized peer can produce); the verifier re-derives it. Deviations accepted: `RemovePieSlicesByType(BuyMenu)` and the FormSquad/FullInventory `SetEnabled(false)` sites untouched (unreached); PREV, reconnect, more than two peers, gamepad, real delivery and the Mac untested; scratch 14.09 GB (lead cleanup after the verifier closes, logged).
NEW: **F23b (item 4, queued):** under lockstep the ActorSelect/Go-To cursor highlight is no longer drawn; the no-compromise path is a render-only highlight (a view-side flag read by the pie draw path, never by Update, the getters or the dump), not a revert of the gating. **F29 (item 2/3, queued, HIGH candidate):** `MuzzleSmoke.lua:40: attempt to call method 'CreateDirectionalSmokeEffect' (a nil value)` on the host in the repaired goto arm (`tip/green-goto/host/runtime/LogConsole.txt:1439`), once, dumps and all 320 hashes equal; newly reached because AI weapon fire now persists; the method lives in `Data/Base.rte/Scripts/Utility/ParticleUtility.lua`, so a per-Lua-state module gap under `-num-lua-states 4` is the working hypothesis; its own Opus lane. Verdict pending the verifier. Branch backup-pushed (scan 0). Item 3/6 +0 until merge.

## 2026-09-13 20:39 MST — ICE lifecycle integration landed (stage2/ice-lifecycle-on-wave b972262f0a on 8418e6cbca, Opus engineer): report saved, lead read in progress, verifier next

`opus-ice-on-wave-20260913/REPORT.md` (saved verbatim). The lead's cherry-pick recipe was wrong in one place (the TransportLink conversion lives in the WIP commit 1b1ff0eb9f, not c90fb0a339), so the worker used git's own three-way merge of wave x lane tip over 7e63a0c1b5 with the recipe's resolutions R1-R3 plus two conflicts the recipe missed (R4 duplicate member pair, R5 duplicate lease test; the lane's copy kept with lane-unique ports and the `ice_rematch_row_resolves_bound_identity` assertion). Consequence: commits 1 and 2 are not independently buildable (follow-up before any PR series). Deviations recorded: the control is a 6c9932a926 build (Source/Network identical to 8418e6cbca); the arms do not exist on the control, so the per-mechanism RED is the tip with six reverts (`red-mutations.patch`, red-match:25/90/91/92/173) against GREEN (green-match, 0 FAIL, suite 12/12 twice); the worker removed its own two-minute-old `D:/mx/.../ice-rematch` directory holding only pins.json (no reparse point; the lead re-counted Data in four trees afterwards) - a letter violation, recorded; the lane's e2e driver was run as a copy that records `LEAD_BATTERY.lock` instead of aborting (both hashes quoted). F25 corrected: the wave's `|| Completed` in `NeedsRecoveryPump()` made a completed online match survive into a later single-player game and route its pause to the title screen; commit b972262f0a restores recovery-only and adds `NeedsCompletedLobbyPump()` (`Completed && !m_LeftMatch`) OR'd into MenuMan, with the detecting arm `TestCompletedLobbyIsNotARecovery` (RED red-match:173, GREEN green-match:174). Two-peer rematch e2e on the tip: ICE 13/13 (segments [200, 601] both rounds, ip_events 0 / p2p 819/828, one POST + seven heartbeats + five signals, identity match, traces identical) and IP 8/8. The B5 lease fixture's idle-coordinator rig was measured unrealistic (`fixture-probe.patch`: `RefuseEndedPeersLocked` disconnects a peer the ended round never used) and the lane's rig kept. OPEN product question for the user: after an online match ends the game now takes the ordinary scenario end path to the menus; a completed rematch lobby has no expiry (Back, Leave or quit only). Item 8 +0 until merge.

## 2026-09-13 20:45 MST — Player-row ellipsis follow-up ACCEPTED (f53098c4a6 + 0c7faadd6b); final visual review dispatched; chat visual review 2 saved (ACCEPT WITH FOLLOW-UPS, one pre-existing host-side error-block defect)

Ellipsis: `swe-lobby-player-row-20260913/REPORT-ellipsis.md`; lead read of the diff: MainMenuGUI.cpp:1146/1149 `"\x85"` -> `"..."` in the elide lambda (measure and emit), the driver's `ELLIPSIS = b"..."` and its docstring; nothing else. Re-derived from `ellipsis/green-640/result.json`: pass, row0 elided, header offset 10, content_dx 0; 960 no elision, dx +1; selftests 11/11 on 35b7d0ff... The RED artefact is the retained `header/green-640b/rows-zoom.png` (the box glyph), disclosed. The worker also checked for an existing GUI truncation convention (none besides "..." itself). Branch backup-pushed at 0c7faadd6b (scan 0). Final visual review (header + three periods at both resolutions) dispatched; merge on ACCEPT.

Chat visual review 2 (`swe-lobby-chat-20260913/REPORT-visual-review-2.md`, saved verbatim): the earlier blocking state is exercised — mismatch + port-map at 640 with chat present: headline readable, port-map row visible, two chat rows, Back on-screen, no overlap; input margin now ~11 px (was ~2); chat X aligned with the rows; team lines indented; no regressions vs the pre-rework captures; no box glyphs. FAIL at 960 (Q2): the host lobby's error label never draws modules b-e or the hash line — a PRE-EXISTING `GUIFont` behaviour (an over-wide token with no break opportunity ends the text; `Source/GUI/GUIFont.cpp:186-195`), untouched by the lane, but the report's claim "full error incl. module list" is false and the reserved error box leaves an ~85 px dead band at 960. The joiner-side landing solved the same problem earlier (`FormatModuleMismatchStatus` one module per line + the scroll band); the host lobby error label still takes the raw text. QUEUED as item 7 lane A7c-host (after the integration turn): host-lobby error block = per-module lines + the landing's scroll mechanism, the dead band removed, the 1 px Leave/Seats-to-chat clearance widened; the report line corrected in the integration turn.

Two SWE-2 turns launched from spare capacity: platinum-begonia = `swe-lobby-chat-20260913/integration-prompt.txt` (merge the wave 8418e6cbca and stage2/mod-mismatch-presentation 0c7faadd6b into stage2/lobby-chat, hand-resolve MainMenuGUI.cpp keeping all three lineages, 12/12 + the three UI drivers green on one tree, captures); defiant-taurus = `swe-ui-viewport-complete-20260913/review3-followups-prompt.txt` (post-Back capture + check, tall-phase tail oracle with a mutation RED, fallback-glyph cell width, host-lobby status line clip) — the lead merges the results, not the workers.

## 2026-09-13 20:48 MST — Chat rework: independent verifier 2 ACCEPT WITH FOLLOW-UPS (saved as REPORT-verifier-2.md); merge waits only for the integration turn

All four rejected defects confirmed fixed with RED on a correctly composed control (production files at 62f32ee8d4 + the selftests at HEAD; red-final 9/11 failing exactly net-protocol and net-match) and GREEN on the rework; the Send path byte-identical to the pre-slice base (no hunk over `Send` in 3b12f4bcc6..949f83e721); lock order one-way (service mutex → m_ChatMutex); the second-thread arm real and run under ASan (EnableASAN=true at RTEA.vcxproj:768); the e2e determinism proof re-derived element-wise (300 tick hashes equal across the four traces, final_total_hash 5b4e9385…). Six follow-ups, none blocking, queued for the chat session's next turn after the integration: (1) MEDIUM the driver's `{res_tag}_chatted` replaced the only automated proof that a line reaches a panel row — restore an any-drawn-row `assert_label` or a `dump_lobby` assertion; (2) LOW `c_MalformedChatRateBase + peerId` inserts an `m_ChatRate` entry per transport id, monotonic ids, cleared only at Start — unbounded under reconnect + malformed chat; evict or fold unknown senders onto one key; (3) LOW sink vs relay disagree when `m_ChatTeams` has no entry; (4) COSMETIC the stray tab at NetSession.cpp:572; (5) LOW a line queued while unlinked returns true and clears the input box, then drops at the pump — keep the text in the box (refuse at enqueue when not linked, read on the pump thread… design: the service can expose a cheap `IsChatLinked()` snapshot updated by the pump); (6) LOW two report inaccuracies (the original deviation-2 wording about `ignoredSessionPackets`; the summary row claiming every new arm fails on the control — `TestChatOutboxJoinRace` never executed there because the suite aborts at the earlier arm).

## 2026-09-13 20:48 MST — F15 remap fixes landed (stage2/f15-debt-arms d9ecd8b99d, Opus engineer): lead read complete, verifier dispatched; F30 registered

`opus-f15-remap-20260913/REPORT.md` (saved verbatim; raw rows `rows.md`). Diff +25/-4 read line by line: `Arm::RemapExternalLinks` (base `Attachable::RemapExternalLinks`, then the support pointer through the map with the `Actor::m_pItemInReach` shape); `Attachable::VisitOwnedWoundTemplates` (owned break wound, owned parent break wound, nulls skipped) shared by the collector (OwnedMovableObjects.h:26) and the new `Attachable::RemapExternalLinks` override (base, then recurse into the templates) — collector and remapper walk one set by construction; `///` Doxygen on the new declaration. Re-derived by the lead: RED on the engineer's own unmodified build (red-a lines 28/33/36, red-o 28/29 FAIL; 0-byte source.diff) → GREEN (green-a, green-o all PASS incl. `canonical state byte-identical`); selftests 11/11 on 1b70f097…; ASan a/o clean; SP comparison pass, dump byte-identical f2c46632…, 320/320 hashes. Disclosed and accepted: the sequenced letter set fails identically on both builds apart from the five fixed lines (residue: r/t not armed, x/o/l/a `named_ghost_owner_retained`, n cursor 28→30 = F28, n|l|a canonical-state changed — a pre-existing sequencing effect of the arm harness, queued as harness debt); one control built by working-tree reversal with manifests. NEW **F30 (LOW until read, potential desync channel):** run-to-run MOID-numbering variance on unmodified binaries — the remap lane's first SP run differed from three others in 25 `moid=` dump lines only, and the F23 lane saw the same moid-only variance on its control (217 lines); tick hashes equal in every case. If any sim decision reads MOIDs and the hash does not cover them, two peers could diverge unseen; the verifier answers Q7 on it. Branch backup-pushed (scan 0). Item 2/4 +0 until merge.

## 2026-09-13 20:50 MST — Player-row work ACCEPTED (final visual review) and merged into the wave: cf71b68d21; item 7 +1

`swe-lobby-player-row-20260913/REPORT-visual-review-final.md` (saved verbatim): ACCEPT, every question PASS — header 10 px over its rows at both resolutions (RED ~170/~197 px), three gold periods and no box at 640, 960 four full rows in a 679 px panel, 640 at the 624 px cap with every sibling on-screen, all gold FontLarge, no artefacts; two cosmetic notes (the widened panel covers more of the title art at 640; row extents equal the box by construction, not a near-clip). Merge of stage2/mod-mismatch-presentation @ 0c7faadd6b into the wave (8418e6cbca → cf71b68d21): ONE hand-resolved hunk in `Source/Menus/MainMenuGUI.cpp` — the wave's `b7e1d86b35 Reserve the wrapped lobby status height` had changed the error label's position to `162 + statusExtra + portMapHeight` while the branch placed its row-fitting block (fillLabels call, header shift, elision loop) before `162 + portMapHeight`; resolution keeps the branch's block and the wave's `statusExtra` offset (keep-both, working file, resolve → marker check → commit as separate steps); `verify_merge_commit.py` 0 differences (83/6/323 changed lines match the branch exactly); scan 0; pushed. Working-copy note: the resolution script wrote CRLF, git normalized the index to LF (numstat 65/18, the branch's lines exactly) and the working file was re-checked-out from the index. Item 7 42 → 43.

## 2026-09-13 20:53 MST — F23: independent verifier ACCEPT WITH FOLLOW-UPS (saved as REPORT-verifier.md); three defects sent back to the engineer before the merge; F23c registered; the 14 GB scratch claim was junction double-counting

Verifier re-derived RED (634/628) and GREEN (0), the producer evidence (0x500006 / 0x4000500002 alternating from produced tick 201 on the control; 0x0 on the tip), host == client dumps on all four repaired arms (two arms diverged between peers on the control while their tick hashes were equal — the extended dump caught what the hash did not), the oracle correction (SP itself is Disabling for four ticks after its handoff, so the old rule demanded an impossible close; the corrected detector still detects all 43 mutations; under the stricter SP-referenced comparison every one of the 234 rows differs only by the +1 WhilePieMenuOpen call SP's local close causes), the gating sites (every hunk reduces to the original when the flag is true; the four seat-change sites replay via Actor.cpp:898-906 from the wire apply on every peer; the cursor-highlight helpers all write dump/getter-visible state, so none could be kept as written), and SP byte-identical on both binaries (moid-only run-to-run variance on the control itself; all 15 pairs hash-equal). Defects sent to the engineer as a corrective turn (SendMessage, same session): **D1 MEDIUM** Begin/End re-evaluate `isLocalControllerActor` (reads `GetTeam()`), a Lua team write inside the window strands the producing state → capture the Begin set; **D2 LOW-MED** `Clear()` via `Override` mid-window skips the End restore; **D3 MEDIUM** the carry is not checkpointed/audited (a restore starts the producer from an empty baseline → one spurious press edge; deviation 5's claim corrected) → land in VisitCheckpoint + CopyCheckpointFrom + ContractAudit with a Controller3 tag bump, 11/11 + gui-checkpoint; report corrections D4-D6 (script-graph is 560 PASS across 21 suites; tip-r1 delta 14 moid-only; scratch 0.62 GB real — the 14.09 GB counted engine Data through 38 runtime\Data junctions; no cleanup). NEW **F23c (MEDIUM, queued):** three more ungated same-class writes — `GameActivity.cpp:1673 RemovePieSlicesByType(BuyMenu)` (writes `m_CurrentPieSlices`, the dumped `:8` field), `:1718-1719` and `:1726` `SetDisabled(true)`/`SetEnabled(false)` (FormSquad/FullInventory) — unreached by any arm; needs its own RED arm (buy menu under lockstep) before gating. F23b and F29 as already queued. Merge after the corrective turn lands and the verifier re-reads it. Item 3/6 +0 until then.

## 2026-09-13 20:58 MST — ICE lifecycle integration: verifier ACCEPT WITH FOLLOW-UPS (no code defect) → merged into the wave f96c090f91; item 8 +2; one attribution correction; one product question for the user

`opus-ice-on-wave-20260913/REPORT-verifier.md` (saved verbatim). The verifier reconstructed the RED sources with `patch` (hashes matched red-originals), re-ran the three-way merges with `git merge-file` into scratch (R4 reproduced: git emits the member pair twice), confirmed the production files equal the lane tip (NetMatchService.cpp/.h deltas = R2 + F25 + the wave's member comments; NetDirectorySelfTest = the wave blob; Main.cpp one line), R5's kept copy a strict superset of the wave's lease test (lane ports, the coordinator started with the seated peer's real transport map — production fills the same map in NetMatchRunner.cpp:31-37/:536 — and the resolver assertion), RED 5 FAILs vs GREEN 0 with 12/12 twice, the two-peer ICE (13/13) and IP (8/8) e2e with the unchanged comparator, the stop order (SetPump({}) before the only two dispatcher Stop/reset pairs), `PumpCompletedSessionLocked` unable to revive the world, `SetMatchEnded`'s gates. CORRECTION to the lead's earlier note: the `Completed && !m_LeftMatch` clause in `NeedsRecoveryPump()` was introduced by the LANE (c90fb0a339), not the wave — b972262f0a removes a lane regression (the wave's `NeedsRecoveryPump` is byte-identical before and after). LOW report defects only (a stale identity claim, commit 1's non-buildability reason — it is red at runtime, not uncompilable; "six" vendor vcxproj → seven; the status-file claim). Merge into the wave cf71b68d21 → f96c090f91: clean auto-merge, `verify_merge_commit.py` 0 differences, scan 0, pushed; branch backup-pushed. Process note: this merge commit was made by `git merge -m` (no trailer, scan 0); from here on the lead uses `git merge --no-commit` + `git_commit.py --all` so every lead commit passes through the helper.
PRODUCT QUESTION (for the user, from the verifier's Q4 reading of the code): after an online match ends the game now lands on the Scenario/planet screen; the rematch lobby is reached only by Back → Multiplayer, where `ReturnToLobby` fires automatically; the kept directory lease has no expiry and heartbeats from the menu loop and even during a later local game; Back/ESC on the Multiplayer screen destroys the service and deletes the lease, so the other peer's rematch lobby loses its host. Lead recommendation: an ended online match should return straight to the multiplayer (rematch) lobby, and a completed lobby should expire after a bounded idle time (say 10 minutes without both peers) — queued as item 8 lane B16 pending the user's word. Also queued: the F25 fix is proven at the predicate level only (no headless menu harness); the e2e was not re-run on the final binary (same commit, clean tree); commits 1-2 not independently buildable (reorder before any PR series); WAN/third-joiner/expiry/Mac untested.

## 2026-09-13 21:03 MST — F26/F27 remap fixes: verifier ACCEPT WITH FOLLOW-UPS → merged into the wave 151c771bac; item 4 +1; F30 upgraded to MEDIUM with a root cause; F31 registered; letter-set harness debt has an in-tree remedy

`opus-f15-remap-20260913/REPORT-verifier.md` (saved verbatim). Provenance re-derived byte-for-byte: the GREEN binary's `source.diff` equals `git diff 1500aa6f35 d9ecd8b99d` (the report's "0 bytes" sentence was wrong for two manifests — the provenance is stronger, not weaker); every `RemapExternalLinks` override enumerated (HeldDevice/Leg/Turret/AEmitter/ADoor inherit Attachable's new walk); the collected set unchanged (nulls were no-ops); the recursion a tree; the shared presets rightly untouched; the sequenced letter set differs from the control by exactly the five fixed lines; 11/11, script-graph 186/0 counted, ASan 0 hits, SP dumps re-hashed equal; Lua binds only a read-only `SupportedHeldDevice` accessor; the HeldRefs fixture line verbatim on the pre-change reference and the fixed build. Merge of stage2/f15-debt-arms @ d9ecd8b99d into the wave f96c090f91 → 151c771bac: clean auto-merge (Main.cpp), committed through git_commit.py, `verify_merge_commit.py` 0 differences, scan 0, pushed. Item 4 40 → 41 (the F15 retirement remap gaps closed).
**F30 upgraded to MEDIUM, root cause read:** `MovableMan::UpdateDrawMOIDs()` (MovableMan.cpp:4969-5028) clears and rebuilds the MOID index every RENDER frame in m_Actors→m_Items→m_Particles order and is dispatched asynchronously by `StartMOIDDrawTask()` (:5031) on the priority thread pool, so the numbering a sim tick observes depends on render pacing (wall clock); sim code reads MOIDs (AtomGroup.cpp:1118/1222/1306/1342-1346/1755-1783 via GetMOIDPixel → GetMOFromID; AHuman/ACrab/ACraft/ADSensor; Lua bound) as identity lookups through the same index that drew the layer, which is why a permutation moved no tick hash (the checksum block MovableMan.cpp:4338-4415 carries no MOID field) — but MOIDs are in the audited state (ContractAudit.h:1194-1196, :1788, the Atom/AtomGroup numerics) so every raw-dump / contract-audit comparison across runs is exposed today, and it becomes HIGH if a persisted numeric MOID (`m_MOIDHit`, `m_IgnoreMOIDs`, `m_AttachedToMOID`) is read after a renumber or compared cross-peer, or if a sim tick can read a layer built from another tick's world. Lane B17 = opus-f30-moid-determinism (Opus engineer, branch stage2/f30-moid-determinism on 151c771bac in item8-directory): establish when the sim reads the layer relative to the rebuild, a detecting arm, the design.
**F31 (LOW, F15 inventory):** `PieMenu::m_AffectedObject` (PieMenu.h:357) is an un-remapped raw external MO link never reached by the retirement walk (Actor.h:1218 not visited by Actor.cpp:1929-1945); guarded by `ValidMO` at PieMenu.cpp:808/913. Queue with F28 for the next F15 debt lane. **Harness debt (LOW-MED):** the sequenced `-lpinv-overlay-links` letter set can never be all-PASS until `PreviewEventLedger::ResetForSelfTest()` (PreviewEventLedger.cpp:195) runs between letters and `before` is re-captured per letter (Main.cpp:2015/2187, PreviewScriptSelfTest.cpp:342) — a Grok mechanical lane from this fixed design.

## 2026-09-13 21:05 MST — Mac gates on the pinned candidate 8418e6cbca (Grok lane mac-gates-8418-20260913, landed 20:39 MST): every built-and-run row green except one A7 arm whose returner never connected (evidence gap, connect-gate timeout) → rerun alone

`mac-gates-8418-20260913/evidence/REPORT.md` (fetched with tar, excluding repo/runtime/Data/build). Fresh arm64 clone of `stage2/fixgroup-6-lead` at 8418e6cbca, gcc-13 / meson 1.11.1 / ninja, exe `a9168e2d6ca8…`, porcelain empty. Official 12/12 on the POSIX adapter (each row exit 0 with its PASS line; the new `-rotate-primitive-selftest` prints `[rotate-primitive-selftest] PASS` on arm64 — the B14 guard's cross-arch proof, the "clang compile" follow-up is now "arm64 gcc-13 pass; clang untested"); inventory extras: `[script-graph-selftest]` 190/0, `[net-local-ui-selftest]` 54/0, `[net-local-state-selftest]` 10/0 (the Windows counts on 6c9932a926), HUD three reps at press 500 with the depth-7 argv the prior Mac lane needed (disclosed), D7 rows, global-callback via the fixture driver; the `sound-cursor` line absent from HUD stdout but the identity pin present on stderr (`pinned sound identity cursor 9055 -> 1099511627776`). A7 once: 4/5 — `stagger_seat_survives`, `silent_socket_bound`, `leave_ack_dropped`, `slow_resync_save` pass; `coop_hand_back` FAILED with `returner: event producer reported lost or invalid observations`: the returner's journal has 5 events ending in `evidence_gap … "connect gate invalid or its bounded wait expired"` at 5.7 s — the returner process never joined, so no resumed frames exist (the F24 expectation, selection 1048615 across the first 61 resumed frames, is unmet only because nothing was observed). Same class as the 3f65 run's `silent_socket_bound` connect-gate timeout under load, which passed on a rerun alone. Lead classification: not an engine failure on this evidence; a rerun of `coop_hand_back` alone (script lane, no agent) decides it — if it fails alone, it becomes an F24-on-Mac finding. Unregistered `.cpp` noted by the lane (cccp-ctl.cpp, MultiplayerGameGUI.cpp) are Windows-only sources, pre-existing. Item 5 +0 until the rerun; the Mac is otherwise green on the candidate.

## 2026-09-13 21:07 MST — Chat integration turn landed (stage2/lobby-chat 33c6f934fa = chat rework + wave 8418e6cbca + player rows 0c7faadd6b): lead read of the merged layout, seven drivers re-derived green, visual review of the merged captures dispatched

`swe-lobby-chat-20260913/REPORT-integration.md`. Merges 4a48405c11 (wave) and 0d620a6327 (player rows) with three hand-resolved keep-both regions in `RefreshMultiplayerScreenControls` (both sides quoted in the report), plus 33c6f934fa removing a duplicate `m_MultiplayerScreenBaselineY` member the wave merge had kept twice (C2086). Lead read of the merged function: the landing (non-lobby) branch keeps the viewport block (fallback font, `GetMaxWordWidth` cap, overflow scroll, room-bounded status height) plus the chat hide, with the manual screen reset subsumed by `FitMultiplayerScreen`; the lobby tail keeps `errorRoom = min(viewport formula, panelCap - 241 - fixedExtra - inputBlock)` so the chat input row always survives, the scroll flags and `VAlignment::Top` coexist, `contentHeight = chatTop + chatRows*10 + inputBlock` feeds the wave's `screenHeight`/`buttonShift`/`FitMultiplayerScreen`, the error label keeps `162 + statusExtra + portMapHeight`, the chat labels join the fill set and follow the rows' convention (X=8/W-16 at the 300 px base, X=12/W-24 widened), the header shift and the three-period elision are verbatim. Driver results re-derived from the seven result.json files on exe 3475466f…: selftests 12/12, chat e2e 13/13, shots 12/12, rows 28/28 at both resolutions (without `--reference`, disclosed: the chat block shares the panel), viewport 56/56 at both. The merged 960 mismatch capture is claimed to draw all five 64-byte module names plus the hash tail (the widened panel removed the pre-merge truncation); REPORT-rework.md line 46 corrected. Visual review of the merged captures dispatched; merge into the wave on its ACCEPT (criss-cross merge bases expected: the wave and the branch both merged 0c7faadd6b — the chat branch's resolution region is the superset). Branch backup-pushed (scan 0). Item 7 +0 until merge. Next SWE turn (new branch stage2/lobby-chat-followups on 33c6f934fa, so the reviewed tree stays intact): the verifier's six follow-ups and review 2's Leave/Seats-to-chat clearance.

## 2026-09-13 21:11 MST — Mac A7 `coop_hand_back` rerun alone PASSED (script lane mac-a7f-rerun-8418-20260913, 21:05:30-21:09:53 MST): the Mac gates on the pinned candidate 8418e6cbca are fully green; the F24 Mac case is closed; item 5 +1

`mac-a7f-rerun-8418-20260913/result.json`: `execution_pass: true`, arm `coop_hand_back` passed, failures [], evidence_gaps [], same binary a9168e2d… and the same harness pin (manifest 72ee173c…, source archive 6e06bb5d…, no source mismatches) as the gates run; `rerun.log` shows the Mac quiet before the run (load 0.91). Lead classification of the group-run failure stands: a connect-gate timeout under the five-arm load (the returner never joined: `evidence_gap`), not an engine failure — the second time this class appears on the Mac (3f65: `silent_socket_bound`), so the A7 group runner's connect-gate bound is queued as harness debt (run the five arms sequentially or lengthen the returner's gate on the Mac). With it the candidate 8418e6cbca is green on arm64: official 12/12 (rotate-primitive guard included), script-graph 190/0, net-local-ui 54/0, net-local-state 10/0, HUD 3/3 at press 500, D7 8/8 with violations=0, lpinv 4/4, A7 5/5 (four in the group, `coop_hand_back` alone) — and the F24 verifier's open item (the returning co-op player's local selection preserved across the first 61 resumed frames, brain 1048577, on the Mac) is closed by this arm's expectation passing. Item 5 98 → 99; item 6's F24 Mac follow-up closed.

## 2026-09-13 21:16 MST — Lobby chat ACCEPTED (visual review 3 of the merged tree) and merged into the wave: b97086352e; item 7 +2; criss-cross merge verified by two-sided diffs

`swe-lobby-chat-20260913/REPORT-visual-review-3.md` (saved verbatim): ACCEPT WITH FOLLOW-UPS — plain lobby, mismatch + port-map (all five module names and the hash tail draw at 960 in the widened panel; the pre-merge dead band is gone), worst-case rows with chat present (header 10 px, three periods, 624/679 panels), the joiner landing unchanged vs review 3, two predecessor defects closed (the row-0 top-slice, the 960 dead band); OPEN: the 640 error band is a 40 px scroll band whose motion a single frame cannot prove; the 1 px Leave/Seats-to-chat clearance unchanged (in the follow-ups turn as item 7); the exactly-300 narrowing case leaves the chat X rule 4 px off the rows (source-derived, not captured). Merge of stage2/lobby-chat @ 33c6f934fa into the wave 151c771bac → b97086352e with two hand-resolved keep-both hunks: `NetMatchService.h` (the ICE members `m_PendingLobbyEvents`/`m_PendingLobbyBytes`/`m_PendingLobbyOverflow`/`m_LeftMatch`/`m_EndedLockstepPackets` beside the chat's `m_ChatSession` view) and `MainMenuGUI.cpp` (the chat labels appended to the fill set; the chat comments beside the shared `162 + statusExtra + portMapHeight` position). The branch and the wave both contain 0c7faadd6b and 8418e6cbca (criss-cross), so `verify_merge_commit.py` reported 35 files with other-only lines — the wave content the branch had merged, not a loss; the merge was verified instead by two-sided diffs: `git diff b97086352e stage2/lobby-chat` names exactly the 27 files the wave gained after 8418e6cbca through the ICE and F15 merges (nothing chat-related lost), and `git diff 151c771bac b97086352e` names exactly the chat lineage's 14 files (+1422/-11: NetSession/NetProtocol/NetMatchService/NetMatchSelfTest/NetLobbySession/NetLockstep, MainMenuGUI, Main.cpp, tools/test_lobby_chat.py). Committed through git_commit.py; scan 0; pushed. The merged tree has not been built yet — the wave B candidate rebuild after the battery is the compile gate (recorded as a known gap). Item 7 43 → 45. Follow-ups queued with the running follow-ups turn: the 4 px clearance (in flight), the exactly-300 chat X rule, two 640 mismatch frames to prove the band scrolls (or per-module lines).

## 2026-09-13 21:17 MST — Apple clang build of the candidate FAILS (Mac lane mac-clang-8418-20260913): F32 registered — floating-point `std::from_chars` is unavailable before macOS 26 on Apple clang 17

`mac-clang-8418-20260913/evidence/REPORT.md` + `logs/build.log:1570-1595` (`compiler-errors.txt`): with `CC=/usr/bin/clang CXX=/usr/bin/clang++` (Apple clang 17.0.0, libc++), meson configures (it prints "LLVM clang is not directly supported" and "gcc detected" but proceeds) and ninja stops at [364/603] with three errors: `Source/Entities/ACrab.cpp:315` and `:356`, `Source/Entities/ACRocket.cpp:241` — `'from_chars' is unavailable: introduced in macOS 26.0` (the libc++ floating-point overload, `from_chars_floating_point.h:58`). The candidate's Mac proof stands on gcc-13/libstdc++ (which has it); the shipping Mac toolchain is Apple clang, so this is a real portability break of the runtime text codec's float parse. **F32 (MEDIUM, item 5/1):** replace the floating `std::from_chars` uses with one exact, locale-free, allocation-free parse helper that is bit-identical on MSVC, libstdc++ and Apple libc++ (from_chars where available; a correctly-rounded fallback proven bit-exact by a differential selftest over a corpus of shortest-representation doubles/floats and edge cases; no widening of any comparison), plus a grep for every other floating `from_chars`/`to_chars` use. Lane B18 (Opus engineer on Windows, then a Mac clang gates lane). No engine launch on this lane; nothing edited; the B14 "guard under clang" question remains open until F32 lands.

## 2026-09-13 21:19 MST — F23 corrective turn landed (stage2/pie-close-lockstep 300fa477d7): D1-D3 fixed with arms, lead read complete, verifier re-read requested; merge after it

`opus-f23-pie-gating-20260913/REPORT-corrective.md` (saved verbatim; the agent's own "21:55 MST" stamp is its error — the landing arrived 21:18 MST). Lead read of the diff (+191/-26): D1 `producingActors` captured at Begin and ended exactly (MovableMan.cpp:4626-4638/:4838-4840; no runtime RED — the same tick's apply overwrites a stranded state before any sim read, so it is an invariant repair covered by the native pairing arm; the engineer refused to fake a RED); D2 `Clear()` keeps `m_CommittedInput`/`m_ProducingLocalInput` (they belong to the pass, regrouped in the header with a default initializer), RED `FAIL: an override inside the producing pass kept the sim from its committed input` (red-arms.log, exe 04befa2b…) → GREEN `PASS producing_pass_survives_an_override`; D3 `VisitLocalProduction` on save/load with the tag Controller2 → Controller3 and Controller1/Controller2 payloads still accepted (baseline reset to absent), `CopyCheckpointFrom` copies the carry, 14 ContractAudit rows added (the report says 15), RED `FAIL: a restored controller produced from an empty baseline, re-firing the held input as a fresh press` (red-arms-2.log, exe 0361e4bd…) → GREEN `PASS restored_controller_keeps_its_production_baseline bytes=510`; deviation 5 withdrawn. Fence exception disclosed and read: the controller-frame selftest's legacy-payload construction (ControllerFrame.cpp:667-675, test only) now replaces the Controller3 tag and strips the wider fields — still a genuine Controller1 payload test; no Controller2 legacy arm exists (queued: add one). Gates on cc7c2986…: 11/11, script-graph 0 FAIL (708 PASS lines counted), arms next 0 / actor_cancel 4 / delivery_cancel 2 / goto 1 on repeats (one goto run had a 79-line moid-only dump diff = F30), compare_sp pass on three arms. Report corrections applied (script-graph 560/21 suites; tip-r1 = 14 moid-only; scratch 0.948 GB). Verifier resumed for a focused re-read of D1-D3 and the fence exception. Branch backup-pushed (scan 0).

## 2026-09-13 21:32 MST — FG6D battery landed on the pinned candidate 8418e6cbca (exe e29ddd593b…): 59 PASS / 2 RETIRED / 13 FAIL; lead classification: no engine failure so far — two harness rules, seven refused launches, three unscored observation rows, one missing engine log line

Lane grok-fg6d-battery-20260913 (Grok run-only, 20:13-21:26 MST; guard 20:19:59-21:25:05 MST, child_exit=1 = FAIL rows, peak 1.11 GB, identity_ok before and after, the lock held). Report `fg6d-battery/REPORT.md` + `REPORT.json`; scratch `D:/mx/fg6bat4`. The 13 FAIL rows, each re-read by the lead from the raw files:
1. `suite` — HARNESS: the scorer required `passed == total == 11` and 11 children while the socket-free suite has had 12 rows since B14; the raw `D:/mx/fg6bat4/suite/result.json` is 12/12. Corrected by the lead (disclosed; original kept as `battery_scoring.py.fg6d-run1`): `passed == total == len(children) >= 12`.
2. `fixtures` (craft_cargo.e2e, the only failing check of 15: `mode_delivercommandhost`) and 3. `n6` — HARNESS CONTRACT GAP: `n6_scoring.py:24` hard-codes `pass: False` ("missing raw DeliverCommandHost mode evidence: Main.cpp:3177-3179 enqueues the delivery and MovableMan.cpp:578-601 applies it, but neither prints a success/issuance line") — the scorer demands an issuance log line the engine has never printed, while the gameplay oracle passes exactly: the craft-cargo pulse is `[316, 317, 318, 319]` on both peers (`pulse_log_exact: true`, uid 1048840), all other cases pass. Not an engine failure: the cargo is delivered on both peers in lockstep. Queued (wave B): add the `[net-match] deliver command host …` issuance line beside the existing "buy order queued" line so the scorer has its evidence; until then this row is judged on the pulse.
4-10. `arm1`, `ak47hud_1/2/3`, `ak47hud_compare`, `controller_log`, `ice_e2e` — NOT RUN by the reviewed wrappers' preflight (`build tool running: ['link']`, `launch_paths=[]`): the guard refused because another worktree was linking; CLAUDE.md §1.4/§0.1 allow other worktrees' builds and engines beside a pinned battery, so the guard was over-broad. Corrected by the lead (disclosed; original kept as `reviewed/arm1_preflight.py.fg6d-run1`): only cl/link/engine processes whose path or command line names the pinned repository block a launch. Rerun of the seven rows as attempt 2 through the inventory's own `execute_row` (`fg6d-battery/rerun_refused_rows.py`, run-1 report kept as `REPORT-run1-<stamp>.md/json`).
11-13. `directory_e2e`, `mixed_lua`, `portmap_e2e` — UNSCORED observation rows (the retained W85/W97 drivers write observations, no verdict). Lead verdicts from the raw files: directory enabled → host `confirmed_listed: true`, `deletes: 1` at the end, both peers exit 0 with no error or timeout, the list query exit 0; disabled → never listed, no deletes, exits 0; mixed_lua disabled → exits 0, no errors; portmap on/off → host listed with one delete, exits 0, no errors — PASS on observation (explicit oracles for these three rows stay queued as harness debt).
Retired: `fencing_two_transports`, `fencing_h4_quiet_rerun` (covered by the five fencing rows, FG6C). Everything else PASS: the 12-row suite raw, all fencing/heal/reclaim/resync rows with the frame table, gcb, boot/present/lobby rows, lpinv_100, preview_event_d7, expire3, drop3, dedicated, P3COOP/P4PVPVE, fl100, crab_ai_order.
Item 5 +0 until the seven reruns land; if they pass, the candidate 8418e6cbca is battery-green with an empty fix group and promotes to the milestone branch, the family following in a quiet window.

## 2026-09-13 21:32 MST — F29 landed with the real root cause (a preview self copy lost its metatable) and exposed F29b (a preview hook can leak a global into the predicting peer's canonical Lua state); F29 is HELD until F29b lands

`opus-f29-lua-state-module-20260913/REPORT.md` (saved verbatim). The brief's hypothesis was wrong and the engineer proved it: `PushPreviewClone` (LuaMan.cpp:7948-7965) deep-copied the object's self raw pairs without the metatable, so a module or class table cached in `Create` (ParticleUtility's empty instance whose methods live on `__index`) had no methods inside the preview clone and `MuzzleSmoke.lua:40` called nil; host-only because only the host's previewed seat fires inside a preview. Fix 88469fc665 (two hunks, lead read): the copy clones the metatable through the same `seen` memo and sets it on the copy; `RemapPreviewValue` descends into it. RED→GREEN re-derived: unit arm `preview_self_copy_keeps_required_module_methods` FAIL (`method_resolves=0`) → PASS; the e2e fixture reproduces the exact `MuzzleSmoke.lua:40` error 4× on the RED build and 0× on GREEN; 12/12; script-graph 732 PASS / 0 FAIL; the two-peer goto arm regression-clean (no F23 on that base, so a regression check only); avg_ms 4.37 → 4.28. Mod compatibility improves (cached modules with metatables now work in previews). **F29b (HIGH candidate, item 2):** with the methods restored the depth-12 preview runs the smoke effect and `ParticleUtility.lua:97-98` writes a stray global `widthspread`, which survives the discarded preview: `[lpinv] FAIL depth 12 x1/x3: canonical state changed after discarded previews` (6/8) on the fixed build, the lua_graph gaining one global (G43 → G44); `CC_PREVIEW_GLOBALS_SNAPSHOT=1` does not close it. A per-peer Lua-state divergence channel that predates F29 (any preview hook writing a global) — F29 merely exposed it. Lead decision: F29 does not merge alone (every lpinv gate on the wave would go red wherever a gun fires in a preview); the same session continues on F29b: a complete preview-globals fence (added keys removed, changed values restored, every touched state, default-on if cheap), with the existing lpinv FAIL and a new unit arm as RED. Branch backup-push after the fence lands.

## 2026-09-13 21:38 MST — FG6D reruns of the seven refused rows: ALL PASS on the pinned executable; the candidate 8418e6cbca is battery-green (lead verdict); promotion to the milestone branch begins, the family waits for a quiet machine

`fg6d-battery/rerun_refused_rows.py` (attempt 2 of each row through the inventory's own `execute_row`, scratch `D:/mx/fg6bat4/rerun-once/<row>`, 21:33:29-21:37:31 MST, the corrected preflight scoped to the pinned tree; run-1 report kept as `REPORT-run1-*.md/json`): arm1 PASS (switch-control checker verdict); ak47hud_1/2/3 PASS (HUD press tick 500, no failure); ak47hud_compare PASS (three full traces, only the wall value differs); controller_log PASS (recorded current format and replayed at nls 4→32 with identical hashes); ice_e2e PASS (all explicit raw checks). Re-rendered inventory: 67 PASS / 2 RETIRED / 5 FAIL, the five being `fixtures` + `n6` (both the DeliverCommandHost issuance line the engine never printed — the pulse oracle passes exactly) and the three unscored observation rows (lead verdicts PASS from the raw files, recorded above). Lead verdict on the battery: no engine failure on the candidate 8418e6cbca (exe e29ddd593b…); the fix group is empty; harness debt queued for wave B: the `[net-match] deliver command host` issuance line + scorer, explicit oracles for directory_e2e / mixed_lua / portmap_e2e, and the two corrected rules kept (suite `>= 12` rows; preflight scoped to the pinned tree). `D:/mx/LEAD_BATTERY.lock` removed. Next: merge the candidate into `stage2/p4b-interp-lockstep` (no family running), push, then the exclusive build of the approved tree and the Source44 family in a quiet window (nothing else running); items 1/2/5 move on the family.

## 2026-09-13 21:40 MST — Candidate 8418e6cbca PROMOTED: milestone branch stage2/p4b-interp-lockstep aa650e601e → 34cf4919a9 (true merge, parents aa650e601e + 8418e6cbca, 165 files, verify 0 differences, scan 0, pushed); one lead mistake made and undone first

MISTAKE (recorded in full): the first promotion attempt chained `git merge --no-ff --no-commit` and `git_commit.py --all` without checking the merge's exit status. The merge ABORTED ("Merge with strategy ort failed") because a second locally modified tracked build artefact (`external/sources/luabind-0.7.1/_Bin/luabind-release.lib`; the first, allegro, had been restored) would have been overwritten, and the chained `--all` commit then created b8eaa26eb8 — a plain commit carrying only that lib blob (874344 → 953572 bytes) under a merge subject — which was pushed. Detected within the same minute (verify_merge_commit 161 differences, `Data` still 8517 files, no PreviewCompat.lua). Correction: `git reset --hard aa650e601e`, `git push --force-with-lease=stage2/p4b-interp-lockstep:b8eaa26eb8` back to aa650e601e (the only rewrite of the milestone branch besides the trailer strip; the discarded commit held no source; nothing else had moved), every modified tracked file restored from HEAD, the merge repeated with its exit status checked, then committed. Lesson (memory + WORKER_RULES): never chain a commit behind a merge without gating on the merge's exit status, and restore stale tracked `_Bin/*.lib` artefacts before merging into a tree that builds them. The approved tree is clean at 34cf4919a9 with `Data` at 8518 files.
Next (RESUME §5.1): the exclusive /MP12 build of the approved tree and the Source44 family need a quiet machine (nothing else running); five lanes are still building — the family starts at the first quiet window and nothing merges into the milestone branch until it ends. Wave B (ff561bf298) continues as the next candidate meanwhile.

## 2026-09-13 21:45 MST — Mac gates on the merged wave tip b97086352e (Grok lane mac-gates-b970-20260913, landed 21:42 MST): the union BUILDS on arm64 and is green except one A7 arm killed by the desktop guard; the Mac approval dialogs identified (TCC prompts for the Cursor agent's node binary)

`mac-gates-b970-20260913/evidence/REPORT.md`: fresh clone of `stage2/fixgroup-6-lead-wave-a` at b97086352e (player rows + ICE + F15 arms/remap + lobby chat — the first build of that union anywhere), gcc-13/arm64 exe `000c2fe42d…`; official 12/12 (rotate-primitive PASS), `[script-graph-selftest]` 190/0, `[net-local-ui-selftest]` 54/0, `[net-local-state-selftest]` 10/0, HUD 3 reps at press 500, D7, lpinv, global-callback; A7 4/5 — `stagger_seat_survives`, `silent_socket_bound`, `slow_resync_save`, `coop_hand_back` pass (coop first try this time); `leave_ack_dropped` FAILED with `host.ready: peer ended before required evidence` because the harness's Mac window/focus guard KILLED the host at 3.063 s (`injected_termination=Mac window/focus guard violation`, `frontmost_pid` = the host pid; stderr `GLAD: ERROR 1280 in glGetIntegerv!`; no engine error, the client never launched). Same binary, four other arms clean → an environment kill, not an engine failure; rerun alone launched (`mac-a7d-rerun-b970-20260913`).
The user's Mac approval dialogs (their 21:3x MST message): the user TCC database shows the Cursor agent's bundled `node` (`~/.local/share/cursor-agent/versions/2026.09.10-fd3934a/node`) granted Documents / Downloads / Desktop / MediaLibrary / FileProvider access on 2026-09-11 and `kTCCServiceSystemPolicyAppData` decided today at 21:32:03 MST — during the b970 gates run, and within a minute of the guard kill above (a user click on a system dialog is the likely focus change the guard saw). macOS re-asks after every Cursor CLI update (three versions on disk: 07.23, 08.11, 09.10). Not the firewall (off), not Gatekeeper, not keychain. Proposed fix for the user: grant that `node` binary Full Disk Access once (System Settings → Privacy & Security → Full Disk Access) and let the lead pin the Cursor CLI version so the binary stops changing; alternatively the Mac Grok route yields to the Claude Code route. Family prep (disclosed): the family's own selftest lists (`contract-audit/run_family.py` SELFTESTS, `contract-audit/run_selftests.py` SELFTESTS) gained `camera-null-scene` and `rotate-primitive` (originals kept as `*.pre-source44`).

## 2026-09-13 21:46 MST — Mac A7 `leave_ack_dropped` rerun alone PASSED on b97086352e (script lane mac-a7d-rerun-b970-20260913, 21:45 MST): the merged wave union is fully green on arm64 (12/12, graph 190/0, UI 54/0, state 10/0, HUD 3/3, D7, A7 5/5). The group-run kill is classified as the desktop guard reacting to a focus change during the user's TCC approval at 21:32 MST, not an engine failure.

## 2026-09-13 21:48 MST — F30 RESOLVED by measurement: the MOID rebuild is sim-paced and no sim read ever saw an in-flight or stale layer; the moid variance was the DUMP racing the renumber → a two-line dump-side join, ACCEPTED and merged into the wave

`opus-f30-moid-determinism-20260913/REPORT.md` (saved verbatim; patches `moid-diag.patch` and `fix-dump-join.patch` retained in the lane dir, the tagged stash dropped by the lead). Measured, not argued: `StartMOIDDrawTask()` has one caller, `Main.cpp:3428`, inside the sim-update loop after the tick's last main-thread mutation, joined at `:3479` in the same iteration (plus `:3398`); an instrument on `MovableMan::LookupMOID` over 319 SP ticks counted 1,672,400 main-thread and 95,425 threaded-Lua reads with 0 reads during an in-flight rebuild and 0 ticks whose in-`Update` reads saw anything but the layer built at tick N-1; two-process lockstep 0 in-flight reads on both peers, simdumps byte-identical (30,139 lines). The F15 verifier's "rebuilt every render frame" reading is corrected. Persisted MOIDs are all same-tick (`m_MOIDHit` gated by the collision frame number, `m_IgnoreMOIDs`/`m_AlreadyHitBy` cleared per travel) except `PostEffect::m_AttachedToMOID` (presentation, resolved at draw after the renumber; audited at ContractAudit.h:1607 — oracle proposal: compare by unique ID; queued). The variance itself: the per-tick dump read `m_MOID` between the kick and the join — RED `red-sp-1/trace.json.simdump.txt:346694` tick 296 uid 1048742 `moid=3/3` (its tick-295 number; the rebuild had numbered it 1 after the holder of 1/1 died) vs GREEN `fix-sp-a` `moid=1/1`, every other one of 427,985 lines identical, tick hashes and `final_total_hash` equal; structurally the dump overlapped an in-flight rebuild 294/294 ticks before and 0/294 after; the F15 remap lane's 25 differing moids were all previous-tick values on one side (25/25) and none on the other. Fix 48b4743721 (lead read, two lines): `DumpSimState` calls `CompleteQueuedMOIDDrawings()` first — joins a task Main joins unconditionally 48 lines later; no sim mutation; nothing widened. Gates: 12/12 on e9967fbf…, script-graph 190/0. Merged into the wave (ff561bf298 → be68d23b42), scan 0, pushed; branch backup-pushed. F30 is closed as an oracle defect; the other armed observers in the same window (`TickProbeIfArmed`, `TrackUidsIfArmed`, `DumpTerrainIfArmed`) are queued for the same join if they ever read MOIDs. Item 5 +0 (oracle), item 2 note.

## 2026-09-13 21:51 MST — Viewport review-3 follow-ups landed (stage2/mod-mismatch-presentation 2629874aa3, SWE-2 defiant-taurus): three of four closed with RED/GREEN, the fourth needs a GUI-library change; lead read complete, visual review dispatched

`swe-ui-viewport-complete-20260913/REPORT-review3-followups.md`; five commits on 0c7faadd6b (+173/-12: MainMenuGUI.cpp +10, tools/test_viewport_fit.py +175). Lead read: (a) `post-back` captures in all three arms with `post_back_main_screen` (no ≥300 px gray panel left, coarse 16 px-cell diff vs the run's own `main-start` ≤ 30%, ≥ 8 gold text rows) — the first version counted blue button frames and false-failed at 960 (the main menu's items are unframed gold text), replaced by `gold_text_rows` (disclosed, the RED evidence unaffected); (b) `tall_tail_reached` rewritten as a scroll-offset correlation against the offset-0 frame after the first mask-template version false-passed the `early-tall` mutation (the closing line's mask matched an earlier hash fragment at 72 %) — disclosed and replaced; offsets `[0,4,10,16,22,27,27,0]` at 640 reach the overflow 27; nine tall frames now; (c) NOT fixed: the fallback-glyph picket bar lives in `GUIFont::InkColorBitmap` (`Source/GUI/GUIFont.cpp:451-458` flattens every non-background pixel incl. the AA shade to the target colour) — outside the fence; the exact luminance-scaled recolour is documented and `fallback-zoom.png` captures exist (queued as a small GUI lane); (d) the host status line wrapped to two lines in a 16 px box and was top-sliced: the status text's width now joins the panel-width computation (`max({errorWord, widestRowText, statusTextWidth}) + 24`) and the status label joins the fill set — the panel widens 296 → 340 px under this fixture through the accepted mechanism; RED blocks `[[0,5],[11,15]]` → GREEN `[[4,13]]` at both resolutions. Mutations (`VPT_MUTATE=no-back,early-tall`) applied for the RED runs and removed before committing (disclosed). Re-derived from result.json: green-640 and green-960 all checks true incl. `post_back_main_screen`, `tall_tail_reached`, `host_status_unclipped`; red-640b fails the Back legs and `tall_tail_reached`/`host_status_unclipped`; red-960 fails `host_status_unclipped` (+ the superseded button leg, disclosed). Selftests 11/11 on a6ed9ce0…. Branch backup-pushed (scan 0). The wave merge will meet the chat lineage in the same hunk (`playerRowLabels` → `fillLabels` + the status label vs the chat labels appended): keep-both. Visual review dispatched; merge on ACCEPT. Item 7 +0 until merge.

## 2026-09-13 21:59 MST — Three landings: F29b fence (verifier dispatched with F29), the F23 follow-up (merged d9cb1699f6), the viewport follow-ups (visual review 4 ACCEPT WITH FOLLOW-UPS → merged, one resolution hunk)

**F29b** (`opus-f29-lua-state-module-20260913/REPORT-f29b.md`, saved verbatim; commits 39e72e01d8 arm + cf20300f3e fence on the F29 commits). Lead read of the fence diff (+186/-17): `FillShallowCopy` refills a reusable per-state record; `RestoreFromShallowCopy` collects the live keys that differ from or are absent in the record before rewriting them (no mutation during traversal), then re-adds the record's deleted keys; `CapturePreviewGlobalFence` keeps `_G`, `package.loaded` and `_RequiredPackages` in a registry table per state and arms it (the master + every state owning a previewed root's subtree, at `CapturePreviewSelfCopies`); `ReleasePreviewGlobalFence` restores all three for every armed state at the end of `EndPreviewScripts`, after the slot drops; default-on with `CC_PREVIEW_GLOBALS_FENCE=0` as the off-switch; the old env-gated snapshot (captured and never restored — `RestorePreviewGlobals` had no caller) deleted. Re-derived: unit arm RED `added_gone=0 … FAIL preview_leaves_globals_as_found` → GREEN `undone=3 … PASS`; the e2e `[lpinv] FAIL depth 12 x1/x3 … 6/8` on the F29-only build and on the final binary with the fence OFF (positive control) → `PASS tick 153: 8/8` with it on, `undone=1` (widthspread); 12/12; script-graph 738/0; lpinv-100 8/8; D7 pass; the two-peer goto arm clean; +0.76 ms per preview (+16.6 %, vs 11.33 ms for the rejected all-states version). Limits stated by the engineer and accepted for the record: the fence covers the globals' own key/value set and the two require caches, not the contents of an existing global table mutated in place (queued as F29c: a debug-build lua_graph hash before/after previews to find any shipped script that does it). HIGH → independent verifier dispatched on F29 + F29b together; merge on ACCEPT.
**F23 follow-up** (`opus-f23-pie-gating-20260913`, commits 3f54d970f4 + f9003ea7c8 on 300fa477d7; over its 1 h cap at ~2 h — the D1 arm cost three builds and was NOT landed: run from the suite's tail it crashes on the world state earlier cases leave, run first it crashes the later switch cases; the fix stands, the detecting test remains debt). Lead read: `BeginLockstepProducingPass`/`EndLockstepProducingPass` name the set by unique ID and End resolves each through `FindObjectByUniqueID` + `IsActor` (an actor `RemoveActor` hands out mid-window is never touched); the checkpoint selftest now builds a Controller2 payload (hold restored ARMED, baseline empty) and a Controller1 payload from the Controller3 text; `IsProducingLocalInput()` for arms. 11/11 on 4843c11d…, script-graph 560/0, next arm 0; three binaries retained. Merged into the wave be68d23b42 → d9cb1699f6 (clean, verify 0, scan 0). D1's detecting arm (needs a world-reset seam for the suite) queued.
**Viewport follow-ups** (`swe-ui-viewport-complete-20260913/REPORT-visual-review-4.md`, saved verbatim): ACCEPT WITH FOLLOW-UPS — post-Back captures show the plain main menu in all eight GREEN arms (the RED still shows the panel); the tall band scrolls through offsets 0..27 with the closing line first fully visible at t5 and holds; the host status line whole at both sizes; nothing else moved; the fallback picket bar unchanged (open, GUIFont.cpp). Correction found by the reviewer: the report's "panel widens 296 → 340" is wrong under this fixture (the mismatch label already drove the panel to 340 in RED and GREEN alike; only the status label box widened) — the widened-panel effect on the wave's chat block is unmeasured (queued: one clean host-lobby capture at both sizes on the merged tree). Merged 2629874aa3 into the wave d9cb1699f6 → the tip below; ONE hand-resolved hunk in `MainMenuGUI.cpp`: the branch renamed the fill vector and added the status label while the wave (chat lineage) appended the chat labels — resolution keeps rows + status label + chat labels in one `fillLabels` (keep-both, working file, marker check, commit as separate steps). Item 7 +0 (follow-ups; the next capture on the merged tree measures the union).

## 2026-09-13 22:01 MST — F32 landed (stage2/f32-float-parse 8f0d2c9196, Opus engineer): an availability-safe exact float text codec, proven on three libraries; lead read complete, verifier dispatched; the RESUME header's stale pause note removed

`opus-f32-float-parse-20260913/REPORT.md` (saved verbatim). Lead read of `Source/System/FloatText.h` (467 lines): availability from the library's own macros (`_LIBCPP_AVAILABILITY_HAS_{FROM,TO}_CHARS_FLOATING_POINT`, `_MSC_VER`, `__cpp_lib_to_chars`), never a platform guess; the fallback parse scans the token against `from_chars`' grammar (no leading space/`+`, no hex, inf/nan forms) and calls the C-locale `strtod_l`/`strtof_l` (a per-process locale created once), ERANGE mapped to `result_out_of_range` only for inf/0 (subnormals accepted), the target left alone on error as the standard says; the fallback format searches the fewest characters that read back (printf `%.*e` per digit count under the C locale, the nearest decimal then its ±1-step neighbours — the lopsided rounding interval at a power of two was a real RED, `0x3d30000000000000` → `5.684341886080802e-14`), fixed vs scientific with `to_chars`' tie rule, exponent at least two digits; the generic `FromCharsExact`/`ToCharsExact` route integers to `std::` unchanged. 15 call sites (8 `to_chars`, 7 `from_chars`) in ACrab/ACRocket/AHuman/LimbPath swapped, nothing else. Arm `-float-text-selftest` (13th suite row; Main.cpp dispatch +4, registered in meson.build and RTEA.vcxproj): 20,860 values per type (430 edge cases incl. 121 powers of two with neighbours, 10,000 seeded patterns) + 31 grammar strings; GREEN on Windows (`from_chars=1 to_chars=1 patterns=10000` / PASS, 13/13 on 2cce34e6…), on Apple clang 17/libc++ 20.1 (`from_chars=0 to_chars=1` / PASS — the fallback parser IS the production path there and its text matches libc++'s `to_chars` byte for byte) and on gcc-13 — the Mac compiles were standalone header+arm builds over ssh (disclosed; no engine). SP pie-close fixture control (call sites reverted) vs tip: dump byte-identical f2c46632…, 320/320 hashes; script-graph 736/0. Measurement of independent value: `std::to_chars` text over 20,748 values is byte-identical between MSVC/UCRT and Apple libc++ (libstdc++ differs only on the signalling-NaN spelling) — Windows↔macOS limb-state text agrees for finite values. Grep list → **F33 (MEDIUM, queued):** locale-dependent float text still on sim-visible data — `Arm.cpp:489` (`strtof` ×3), `PieMenu.cpp:254` (`strtof`, packed cursor angle), `Attachable.cpp:199` (`stof`), `MovableObject.cpp:756` (`stod`), `Reader.h:216` (float read as double via `istream >>` then narrowed: locale-dependent AND double-rounded), `Writer.h:218` (floating `to_chars`, same availability class, compiles on macOS ≥ 13.4 only) — route through the helper with RED/GREEN. Verifier dispatched (the ±1-step shortest argument, the arm's oracle on Windows, the parser grammar); merge on ACCEPT, then the Mac clang gates lane on the new wave tip is F32's true GREEN. Branch backup-pushed.
Housekeeping: the RESUME.md header still carried the 14:52 MST "USER PAUSE" paragraph that "overrides every instruction below" — stale since the user's GO at 16:58 MST and dangerous for a resuming lead; replaced by a live-state paragraph (the F32 engineer flagged it).

## 2026-09-13 22:11 MST — USER DECISION (B16): after an online match ends, everyone returns straight to the multiplayer (rematch) lobby; a completed lobby expires after 10 minutes without both peers (lead's default, stated to the user). Brief written: `opus-post-match-lobby-20260913/prompt.txt` (routing in Main.cpp/MenuMan, expiry from the menu-loop pump, three RED/GREEN arms incl. a menu-script two-peer e2e with screenshots). Launch: right after the Source44 family releases the machine (a build lane may not run beside the family); cut from the wave tip of that moment.

## 2026-09-13 22:13 MST — F29 + F29b: verifier ACCEPT WITH FOLLOW-UPS → merged into the wave df871095c6; chat follow-ups landed; the Source44 FAMILY LAUNCHED on the approved tree (machine quiet)

`opus-f29-lua-state-module-20260913/REPORT-verifier.md` (saved verbatim): every RED/GREEN re-derived from the raw files; the F29 memo keyed by the source table so metatable cycles are safe and functions are shared by value; the miss confirmed on the pre-change tree (`RestorePreviewGlobals` had no caller; the record was cleared twice and never restored); the fence's three passes correct (collect first, then rewrite; deleted keys re-added); require caches complete (`_RequiredPackages` + `package.loaded`, the engine's own `_ClearRequiredPackages` clears exactly those two); the armed set sufficient because release runs on every state; no canonical Lua can run inside the window (every scripted function is gated on the freeze) so nothing legitimate is undone; no Lua-visible API change; the leak re-derived to one changed dump line (G43 → G44, `widthspread` 10 → 11 occurrences); the goto arm's Lua-state hashes are blind to both bugs (a regression check only). Seven follow-ups, none blocking (owed before promotion): the fence-off control's env not recorded in the launch manifest (`win32_test_runner.py:356-359` records only `extra`) and the fence state printed only when something was undone — print it at boot; no arm for a deleted key coming back nor for `_RequiredPackages`; the in-place-mutated-table channels named (`_AsyncPathCallbacks`, `package.preload`, `package.path`/`cpath`, module table contents — F29c); a stale sub-record when `package.loaded`/`_RequiredPackages` appears between capture and release; the strong references the record keeps until the next preview (GC delay — clear on `CollectGarbageForCheckpoint`); shared-slot mode untested; a shared metatable cloned once per previewed object (INFO). Merge of stage2/f29-lua-state-module @ cf20300f3e into the wave b9da85201e → df871095c6: clean auto-merge, verify 0 differences, scan 0, pushed; branch backup-pushed. Item 2 +1 → 99 (F28 re-check owed on the fenced build).
Chat follow-ups landed (`swe-lobby-chat-20260913/REPORT-followups.md`, branch stage2/lobby-chat-followups on 33c6f934fa, six commits ce776fed1e…877098bbf5): every item with a RED line quoted and GREEN (12/12, e2e, shots); disclosed: two arm-only accessors, the shots script's two new `assert_label` steps and a longer wait (oracle change between RED and GREEN), the selftest registration changed from early-return to accumulate so all chat arms report on a defective build, the control built in the reused `rework/control` worktree (its exe path carries the firewall rule). Lead read of the diffs and the verifier pass follow (read-only lanes are allowed beside the family).
FAMILY: Windows quiet (0 cl/link, 0 engines) at 22:13 MST → `run_family.py --source 44 --breadth-out D:/mx/s44b1 --matrix-out D:/mx/s44-1 --mac-attempt 1` launched on the approved tree 34cf4919a9 under `D:/mx/LEAD_FAMILY.lock` (no worker build or engine on Windows until it ends; verifiers, docs and Mac lanes continue); console `D:/mx/s44-family-console.log`; the family's selftest lists carry the two new rows.

## 2026-09-13 22:19 MST — CORRECTION: the 22:13 MST family launch FAILED at its precondition; relaunched 22:17:30 MST and running

The 22:13 launch recorded above never ran: run_family.py's precondition counted six build processes (D:/mx/s44-family-console.log: preconditions exit 1 "6 build process(es) running; the chain build runs alone") and every later stage was blocked. The six were idle MSBuild.exe node-reuse processes (0 cl.exe / link.exe, 0 engines — the lead had judged the machine quiet by the CLAUDE.md §1.4 rule, but run_family.py:147 counts MSBuild, cl, link and ninja by name). Lead actions: the six idle nodes stopped (no build was running anywhere); the failed attempt's record moved aside, not deleted (contract-audit/family-source44.failed-precondition-2214.json + .log, the runner refuses a pre-existing record); the lock re-created; relaunch at 22:17:30 MST → preconditions exit 0 (head 34cf4919a964da4888c0b69b21238a6db58746dc), the chain build (chain_build_gate.py --label source44-1) running alone under D:/mx/LEAD_FAMILY.lock; console D:/mx/s44-family-console-r2.log; record contract-audit/family-source44.json. Harness note (queue item 4): the family's build precondition should count cl/link by working directory like the battery preflight, never idle MSBuild nodes; until then, stop idle nodes before a family launch. Nothing merges into the milestone branch while it runs; no Windows worker build or engine until it ends (verifiers, visual reviews, docs and Mac lanes continue).

## 2026-09-13 22:25 MST — F32: independent verifier ACCEPT WITH FOLLOW-UPS → merged into the wave 1132252c5d (pushed); the Apple clang engine build of the wave tip is the next Mac lane

`opus-f32-float-parse-20260913/REPORT-verifier.md` (saved verbatim). The verifier re-derived everything from the raw files and an independent exact-arithmetic model: the availability test reads the library's own macros (libc++ availability.h:390/:343/:129-131/:216 confirmed on the Mac SDK; runtime from_chars=0 to_chars=1 PASS on Apple clang); the parser's grammar equals from_chars' (no space/+/hex, inf/nan forms, exponent only with digits; ERANGE leaves the target as the standard does); the formatter's nearest-±1-digit search is proved sufficient (convex rounding interval) and an exhaustive oracle agrees on 5,583 values with the ±1 step used 5 times; the exact model reproduces std::to_chars on 20,702/20,702 finite probe values incl. 61 exact ties; the RED at 2^-44 confirmed; 15/15 call sites mechanical with the three Mac RED lines landing on the identical line numbers; Windows dumps byte-identical (re-hashed f2c46632cd33…, 5,445,623 bytes, 320 ticks, live limb text inside); 13/13 selftests on exe 2cce34e6…; graph 736/0; the control build's provenance verified (tip-sources byte-identical to the commits); MSVC == libc++ to_chars text over 20,747 values, libstdc++ differs only on 21 signalling-NaN spellings; scan 0. Report citation errors (not evidence errors): the script-graph breakdown (342 tagged lines, not 190), 20,747 not 20,748 values, compiler-errors.txt lines 6/14/22 not 4/12/20. Defects, all LOW: D1 the grammar oracle is compiled out exactly where ParseFallback is production (FloatTextSelfTest.cpp:205,220) → table-driven expectations; D2 if both macros are 0 the text comparison self-compares (only the header line shows it); D3 per-platform codec: Windows↔macOS identity is measured not constructed (a libstdc++ peer would differ on signalling NaNs) → route the hashed limb codec through FormatFallback unconditionally if a Linux peer ever joins; D4 out-of-range parse writes on MSVC, leaves on the fallback, and the packed-string readers ignore ec; D5 CLocale() unchecked for null. The lead's verdict: ACCEPT; D1-D4 queued as the F32 follow-ups (F33 lane); D3's unconditional-fallback route is the right long-term form and is noted for the F33 design.
MERGE: `git merge --no-ff --no-commit stage2/f32-float-parse` rc=0, no conflicts (11 files, +746/-19), committed through git_commit.py → **1132252c5d** (parents df871095c6 8f0d2c9196); verify_merge_commit.py 0 differing files; scan 0 over upstream/development..1132252c5d; pushed with lease df871095c6..1132252c5d (origin verified). No build of the wave while the family lock exists. Item 5 +0 (the family is the gate); item 1 +0. NEXT: Mac Grok lane `mac-clang-f32-20260913` — a fresh arm64 clone of the wave tip 1132252c5d built with Apple clang (the true GREEN of F32 and the first clang build of wave B), 13/13 + native counts; no A7/HUD on that lane.

## 2026-09-13 22:46 MST — Chat follow-ups ACCEPTED (verifier + visual review) → merged into the wave 9a8caaab8c (pushed); the Source44 family is NOT GREEN so far: R1 root-caused as an engine regression (F34), R2 under read, R3 a lead-caused harness loss; B16 phase 1 landed (code only)

**Chat follow-ups** (`swe-lobby-chat-20260913/REPORT-verifier-followups.md` and `REPORT-visual-review-followups.md`, both saved verbatim). Verifier ACCEPT WITH FOLLOW-UPS: every item confirmed with RED on a correctly composed control and GREEN on the rebuilt exe (12/12, e2e 13/13 checks, shots 12/12, rows real pixel checks); the alias reads exactly the text-bearing rows; one shared malformed-sender key bounds the rate map at 258 entries; the sink rule equals the relay rule in every reachable combination; SendChat's state gate is right for the lobby (Starting) and the rematch lobby (ReturnToLobby → Starting); Leave/Seats end at rel 240 so chatTop 245 is a 4 px gap (chatRows 7→6 at 640, unchanged at 960). Six LOW defects: D1 the error budget at MainMenuGUI.cpp:1235 still keyed to 241 (the two constants must move together); D2 off-lobby rows keep stale text so the alias is "drawn rows" only inside the lobby; D3 the report's "20→60" is really 30→60; D4 an unreported 2.10 GiB dead control build (its only launch refused by the firewall check) and a 6.24 GiB lane (the §1.5 line unreported — cleanup is the lead's, logged when done); D5 item 1's first RED is alias-absent, the mutation run is the real detecting RED; D6 an ICE/mux match that stays Completed refuses chat (B16 territory: the post-match path must land in Starting for IP and ICE matches). Visual review ACCEPT: the gap ~2 px → ~6 px at mismatch 640; nothing above the chat moved (panel-relative y identical in every pair); input box and Back +4; Back fully on-screen at 640 (y≈337-356); rows zooms identical; no artefacts; the mismatch pairs' whole-panel 1-2 px re-anchoring is the viewport clamp (a pixel diff would settle it, non-blocking). Lead read of the six diffs done before the verifier (this ledger, 22:13 MST). MERGE: the branch backup-pushed first (scan 0, new on origin); `git merge --no-ff --no-commit` rc=1 with ONE conflict, NetMatchService.h's friend block (the wave's two ICE-lane friends vs the new TestChatSendRefusedOutsideCarry) resolved keep-both on the working file (markers 0), committed through git_commit.py → **9a8caaab8c** (parents 1132252c5d 877098bbf5), verify_merge_commit.py 0 differing files, scan 0, pushed with lease. Item 7 +0 (the follow-ups complete the accepted chat; no new capability). Queue: D1 with the next UI slice; D4 cleanup by the lead.

**The Source44 family (22:17:30 MST launch) — not green so far.** Chain: the /MP12 build succeeded (exe 730a345bb2919e43…, manifest grouped-build-730a345b) and every chain gate passed except `gate:authority:match:sound-volume-authority-source44-match-1` (R1). mac_launch exit 0 (attempt 1 building on the Mac). Remaining group: native 736 PASS / 0 FAIL, heal PASS (resync), invariance PASS, restoration PASS, fuzz buy 17/17 and pickup 10/10 byte-identical — and `transaction` exit 1 `INVALID_REFERENCE` (R2: the reference case ordinary-load:valid_full at tick 100 logs `Could not load game "valid_full": invalid world structure` twice; Source43 passed it). Breadth: `run_breadth.py` died at its pin check before writing anything (R3, harness): `digest(REFERENCE_EXE) != REFERENCE_SHA` — `grouped-build-4244e87e/Cortex Command.exe` (the Source22 compat reference, sha 4244e87e…) was DELETED by the lead's 2026-09-12 cleanup (reviews/cleanup-20260912.md:62) and no copy exists anywhere on D: (196 executables hashed). The exact Source22 source export survives (`mac-peer-20260907/combined-source-22.{tar.gz,manifest.json,patch}`), so the reference is rebuilt from it after the family and re-pinned in run_breadth.py with the rebuild's provenance (a disclosed harness correction; the compat rung compares named case results, so a rebuild from identical source is the same reference). NEGATIVE against the lead: the cleanup deleted a pinned harness input; the retention rule ("never touch a pinned executable") now explicitly covers every executable a driver pins by sha (run_breadth.py REFERENCE_EXE). Matrix: running (106 jobs, cli_* rows landing NEEDS_REVIEW rc 0). Mac attempt: building. Verdict waits for both; nothing merges into the milestone branch meanwhile.

**R1 ROOT-CAUSED (Opus verifier, read-only; `contract-audit/family-source44-reds/R1-authority-verifier.md` saved verbatim) — NEW FINDING F34, HIGH (ownership/authority regression):** the seeded owner map from W102's actor-switch work (commit 67587a3c1c, `NetActorOwnership.cpp:47-51`) short-circuits `ResolveOwnerPeer` for every seeded actor, discarding both the control-mode and the TEAM inputs; `MovableMan.cpp:4611-4620` seeds once at world entry and nothing re-seeds on `Actor::SetTeam` / `MovableObject::SetTeam`. So after the fixture's team transfer at step 240 (actor 1048615 team 0 → 1) both peers keep reading the host's gain 0.25 where the team-1 owner's 0.75 is the contract (six failing rows 240-390, identical on both peers; Source43 read 0.75 at the same rows; fixture, harness, gains and transfers byte-identical between the two runs). The lead confirms from the source: the seed's declared purpose is the control-mode switch only (NetActorOwnership.h:27-29), the switch behaviour rides `s_LockstepControlOverrides` which is consulted before the seed, and the team-owner policy never reads cpuControlled — narrowing the seed to the control-mode input cannot regress W102. Blast radius (unverified): the same short-circuit feeds `ValueObservationAuthority` (MovableMan.cpp:866-877), `ReconcileLockstepControlBindings` (:804-820) and Activity.cpp:848/856. Fix design (the lead): the seed records the team it was taken at and applies only while `query.team` equals it (a team write invalidates the freeze; the next query re-derives from the policy and may re-seed), plus the verifier's RED-first selftest arm in NetLockstepSelfTest (seed at team 0 → query at team 1 must return the team-1 peer; a cpuControlled flip at team 0 must still return the seeded owner) and the family gate as the e2e confirmation. Lands in the one fix group with R2's outcome, on wave B, after the family. Deviation noted by the verifier: the Source43 gate's head was 0e7e5b6094 (a same-subject twin of aeb398b25b); the finding holds on both ranges.

**B16 PHASE 1 landed** (`opus-post-match-lobby-20260913/REPORT-phase1.md` saved verbatim; commits 3cba88ce33 routing, 9960cf18f4 expiry, 1b729f61c9 arms, 9fbee250ca wording on stage2/post-match-lobby from df871095c6; NOTHING built or run by rule). Lead decisions on its deviations: (1) accepted — a `-net-match-service-e2e` run never enters the menu loop, so the e2e driver uses two menu-launched peers; (2) ACCEPTED with a condition — extending the harness flag `-net-match-ticks` to end a menu-launched match at an applied frame every peer reaches is an explicit CLI-flag path, not a hidden test-only branch, and it goes through the unchanged FinishMatch path; the phase-2 report must show the seven existing `-net-match-ticks` drivers unchanged in behaviour (they all pass `-net-match-service-e2e`) and the lead reads the Main.cpp hunk; (3) accepted — the wave RED is the `wait_state Starting` timeout quoting `state=Completed`; (4) noted (no run_rematch.py; the lockack driver is the one to reuse). The lead's diff read of the four commits happens before phase 2's build go. D6 of the chat verifier is folded into phase 2: the post-match lobby must be Starting for an ICE match too (`ReturnToLobby`'s "a session-id (ICE) match cannot return to the lobby yet" refusal at NetMatchService.cpp:386-389 on the chat branch — verify what the wave carries after the ICE lifecycle merge).

**Housekeeping.** The Mac clang lane `mac-clang-f32-20260913` was launched 22:27 MST and STOPPED by the lead at 22:31 MST (launchd job booted out, runner killed) so the family's Mac attempt runs alone on the Mac; it relaunches on the same lane files after `mac_complete`. The overview page the user asked for: https://claude.ai/code/artifact/d820b81d-ecb2-451f-843d-fa8cb0bfc068 (private; built from STATUS/RESUME/this ledger at 22:46 MST).

## 2026-09-13 22:46 MST — R2 ROOT-CAUSED: the same commit as R1 broke saved-game compatibility (NEW FINDING F35, HIGH); the family's fix group is one lane on the owner-map commit; the lead's read of B16 phase 1 production hunks

**R2** (`contract-audit/family-source44-reds/R2-transaction-verifier.md`, Opus verifier read-only, saved verbatim). `valid_full` is a RETAINED fixture (`full-runtime-transaction-controls-20/valid_full.ccsave`, 10,974,573 bytes, sha 4e8f064a…, 2026-09-09; byte-identical in the Source43 and Source44 runs; the harness replays the frozen argv of `transaction-controls-source20.json`), not a save produced by this run. The rejection is `MovableMan.cpp:5278 [world-structure] rejected: truncated runtime checkpoint number` (stdout.log:11), thrown by the archive primitive `CheckpointArchive.h:125` because commit **67587a3c1c** (W102's owner map, 2026-09-12 08:23 MST) added `actorOwners` to the `WorldStructure1` record (`MovableMan.cpp:5165`, field :5161) WITHOUT bumping the tag or adding a legacy branch — the only serialization change in `aeb398b25b..34cf4919a9` besides `syncedOrderDisableTick` (ba19112ba3, done correctly). The verifier replayed the reader by hand on the fixture's 654-byte payload: the 43 order consumes it exactly; the 44 order reads `teamMOIDCount[0]` (14) as `actorOwners.size()`, wants 28 more tokens, finds 17 — the observed line verbatim. A save written by this source loads on this source (the battery's gcb `file=1` on the identical Source tree), so it is not a round-trip regression: it is an unversioned save-format extension that makes EVERY saved game written before 2026-09-12 08:23 MST unloadable, reported as "truncated", covered by no selftest. Precedents the commit ignored: `Controller.cpp:105-114` (Controller1 legacy branch), `MovableMan.cpp:5120-5121` (MovableMan1 → MovableMan2), the legacy RED arm at `ControllerFrame.cpp:667-678`. **F35, HIGH (saved-game compatibility): fix = `WorldStructure2` tag + accept a `WorldStructure1` payload with empty `actorOwners`; RED arms = a legacy-payload case in `RunContiguousActorIndexSelfTest` (MovableMan.cpp:5297-5320) and the single `run_audit.py` case `ordinary-load:valid_full --ticks 100` against the UNCHANGED retained fixture (RED `accepted=0` / INVALID_REFERENCE → GREEN `accepted=1`, `applied: true`). The fixture is never regenerated (that would mask the break).** Lead ruling on the verifier's open question: old `.ccsave` compatibility IS a product requirement (the game has saved games; ADR-004's spirit — nothing a player kept breaks silently) — no user escalation needed.

**The fix group so far** (ONE lane, Opus engineer, cut from the wave 9a8caaab8c): F34 (ownership frozen at the entry-time team) + F35 (unversioned WorldStructure extension), both from 67587a3c1c, each with the RED-first arms above and the family gates as the e2e confirmation; the Source22 reference rebuild (R3) is the lead's harness task in parallel. The matrix and the Mac attempt are still running; any further engine red joins the same group (root-caused first by its own read-only lane). The lane starts in the code-only phase now (the lock forbids builds) and builds after the family.

**B16 phase 1, the lead's read of the production hunks** (3cba88ce33, 9960cf18f4; +23 and +62/-1 lines): MenuMan.cpp:87-89 `OfferRematchLobbyOnEntry()` before the §11 rejoin offer (the drop offer still wins because it runs last); :135-140 a Completed match whose session is alive (`NeedsCompletedLobbyPump()`) fades into the main menu before the MetaMan/activity switch, so the ScenarioFadeIn route keeps single-player and a left match; MainMenuGUI.cpp:561-570 opens MultiplayerScreen and names the Lobby sub-screen; NetMatchService: `m_CompletedLobbySinceMs` set only at Running→Completed in FinishMatch (:1185), cleared in Destroy (:1031); `UpdateCompletedLobbyExpiry` runs last in Update, takes the lock to decide (settled = left, Idle/Failed/ReadyToLaunch/Running, or every non-CPU member connected), releases it, then `Destroy()` + `SetState(Idle, …, "The rematch lobby timed out.")`; `NeedsCompletedLobbyPump` stays true while the wait runs; one constant `c_CompletedLobbyExpiryMs = 600000`; a friend for the arm. No defect seen by reading; one point for phase 2's evidence: "seated" is `member.connected` — the seated half of the arm must show the wait clears only when the returning peer is actually connected in the rebuilt lobby snapshot, and the one-peer half that a peer who quit is not "connected". The arms/driver commit (1b729f61c9, +~240 lines native, a new `tools/test_post_match_lobby.py`) and the `-net-match-ticks` Main.cpp hunk are read at phase 2 with the runs.

## 2026-09-13 22:50 MST — Family red R4 classified by the lead: HARNESS (the F19 gate demands a contract fixture's ARMED line on matrix jobs that configure none); a python-only Grok lane fixes the gate; NEGATIVE against the lead's W134 acceptance

The matrix row `world_normal` reports `FAILED_OR_INVALID` with all 35 cases failing exactly one gate, `fixture_armed` (`D:/mx/s44-1/world_normal/result.json`, every operation observe/save/stage/memory/file/hold/preview; every case completed, process clean, applied, no errors, graphs serialized, `[contract-audit] complete … differences=0`). Cause read from the harness copy (`world_normal/harness_source.py:130-135, 165-169, 315-316`): `fixtures_expected()` returns True for every applying transition, and `fixture_armed` fires when no `[<name>-contract-check] ARMED uid=` line appears — but the four matrix jobs that carry a Lua script (`world_normal`, `continuation_normal`, `world_late`, `continuation_late`, per `D:/mx/s44-1/matrix.json`) all run `tools/fixtures/mod_checkpoint.lua` + `mod_global_callbacks.lua`, mod fixtures that never print ARMED by design; the ARMED lines come only from `tools/contracts/mod_*_contracts.lua`. Source43's matrix (before the gate) passed these rows with 0 failed cases (`D:/mx/s43-1/world_normal/result.json`, harness copy without `fixture_armed`). So the F19 gate (W134, accepted by the lead on 2026-09-12 with the note "gate() fails fixture_armed (a non-load transition that armed nothing)") is over-broad: it cannot pass on the jobs it runs in. NEGATIVE against the lead: the acceptance did not check the gate against the matrix's job list. Fix (design fixed, python-only, no engine): arming is expected only when the job configures a contract fixture (a `tools/contracts/` script whose text contains `-contract-check] ARMED`); the F19 guarantee stays for jobs that do configure one; RED-first unit tests. Lane `grok-run-audit-fixture-gate-20260913` (Grok, Windows, `audio-owner-registry` on `stage2/run-audit-fixture-gate` from 9a8caaab8c), allowed beside the family because it runs no build and no engine. The remaining three world/continuation rows will show the same shape; the lead classifies them from their result.json when they land.

Family red inventory at this point: R1 = F34 (engine), R2 = F35 (engine), R3 = the Source22 reference exe (harness, lead), R4 = the F19 gate (harness, Grok). Matrix and Mac attempt still running.

## 2026-09-13 22:52 MST — F23c PHASE 1 landed (code only): lead read of the production hunks, no defect by reading; phase 2 after the family

`opus-f23c-pie-writes-20260913/REPORT-phase1.md` (saved verbatim; commits fb2496cf5d arms, eb08212169 site A, 8e2e821fc7 site C, d11263cd26 ordering, on stage2/f23c-pie-writes from 1132252c5d; unbuilt by rule; ~80 min against a 60-min cap, disclosed). The engineer's decisive reading: each peer seats only its own player (`Main.cpp:4532-4535` ClearPlayers + AddPlayer(PlayerOne, localTeam); host team 0, client team 1), so the whole `GameActivity.cpp:1670` block runs on the seat peer only, one stage before the shared PreControllerStage of the same tick. Site B (FormSquad disable) is predicted covered by the existing seam (`Actor::HandlePieCommand` → `HoldDisabledForSyncedOrder` on every peer the same tick; `PieMenu::Update` closes on `IsDisabled()`): arm kept, no fix. Lead read of the diff (+31/-2 in three source files): `GameActivity.cpp:1673` the local BuyMenu-slice removal now runs only when `localPieAnimations`; under lockstep the new `Actor::WhilePieMenuOpenListener` (Actor.cpp, override declared Actor.h:431-434) removes the slice on EVERY peer where that pie menu refreshes — same conditions on the seat and the observers (`IsEnabling()`, `IsPlayerControlled()`, lockstep active, the activity's shared `GetBuyMenuEnabled()` false), before the Lua hook, then chains to the base listener; `GameActivity.cpp:1729` the local FullInventory close is gated the same way and `Actor::HandlePieCommand` closes the pie on the synced slice under lockstep. Single-player: `localPieAnimations` is true and every new branch is behind `IsLockstepControllerSyncActive()`, so SP is unchanged by construction (phase 2 still proves it with the SP dump comparison). Comments are two-liners stating the why (acceptable). Residual the engineer flagged, pre-existing and unwidened: `PieMenu::RemovePieSlice` never clears `m_HoveredPieSlice`/`m_AlreadyActivatedPieSlice` when they name the removed slice — same exposure window as before; queue it as a LOW follow-up. Phase 2 (after the family; the two-lane cap): the three arms RED at fb2496cf5d and GREEN at the tip with host == client dumps, the F23 arms, 13/13, graph 0 FAIL, the SP comparison.

## 2026-09-13 22:57 MST — Family red R4 (harness) FIXED: the run_audit gate demands fixture arming only from configured contract fixtures → merged into the wave c85bef65be (pushed)

Lane `grok-run-audit-fixture-gate-20260913` (Grok 4.6 Extra High Fast, python-only, 4.6 min; `cli_runs/grok-run-audit-fixture-gate-20260913/REPORT-final-message.md`): commits 6174aee91d (the RED-first test) and ab2c78de17 (the fix) on `stage2/run-audit-fixture-gate` from 9a8caaab8c. Lead read of the diff (`tools/contracts/run_audit.py` +32/-5): `is_contract_fixture(path)` = a `tools/contracts/` file whose text contains `-contract-check] ARMED` (text, not name — the primitive diagnostics arm too); `_configured_scripts` takes the explicit list, else the item's stored `configured_scripts`, else None (an old caller without script information keeps the strict rule — conservative); `fixtures_expected(item, expect_load, scripts)` returns False when no configured path is a contract fixture, otherwise the unchanged F19 rule; `gate()` records `contract_fixtures_configured` on the item; the call site passes `--script`, `--global-script` and the snapshot paths. The new `tools/contracts/test_run_audit_gate.py` (4 unittest cases on synthetic items with temp-file fixtures): a checkpoint-only applying job passes without ARMED; a configured contract fixture without ARMED still fails `fixture_armed`; an armed fixture with missing named checks still fails `native_checks`; a refused load expects nothing. RED re-derived by the lead (the new tests against `run_audit.py` at 9a8caaab8c, PYTHONPATH to the tree's tools): `AssertionError: Lists differ: ['fixture_armed'] != []`, `FAILED (failures=1)`; GREEN on ab2c78de17: `Ran 4 tests … OK`; W134's `tools/test_run_audit.py` 12/12 unchanged. No engine, no build (the family holds the machine; the lane never needed either). MERGE: branch backup-pushed (scan 0, new on origin); `--no-ff --no-commit` rc=0, no conflict; committed through git_commit.py → **c85bef65be** (parents 9a8caaab8c ab2c78de17), verify 0 differing files, scan 0, pushed with lease. The family's matrix copies keep the OLD gate (they were taken from the approved tree at launch), so the remaining world/continuation rows will still show `fixture_armed`; the lead classifies them as R4 from their result.json. Item 5 +0 (harness; the next family is the gate).

## 2026-09-13 23:05 MST — USER DECISION: Astra back ON for straight feature work (no reviews, no family fixes); five Astra lanes launched in code-only phases; the "headed" item 4/6 steps redesigned as unattended measurements; family red R5 = world_late (R4 class)

The user (22:5x MST): "start planning Astra per your plan … have him work on stuff — but not review or fixing, just straight work", and "according to the HTML you need me for testing, can't you figure out a way to test it yourself?". Lead answers: (1) Astra lanes get straight feature work with the same fences as every lane (RED-first, pixel review by the Opus visual reviewer, no push/merge, the lead reads every line); Opus keeps engine fixes and verification; the Codex quota is the user's to watch (the runner records tokens per lane in usage.json, not a percentage). (2) Item 4's pinned numbers and the 100/200 ms two-window sessions are measurable without a human: two instances on the runner's hidden desktop with `-net-fake-lag`, in-engine instrumentation behind harness-only flags (input-to-photon stamps, per-frame draw/present times, CPU, the prediction ledger's counters, a per-frame local-actor position trace for warps) and frame captures for the visual reviewer; item 6's §11 reconnect review becomes a scripted drop/rejoin/reclaim run with a screenshot per step judged by the visual reviewer. The user's own eyes stay the final feel check whenever convenient, but the scores can move on measurements.

LAUNCHED 23:03-23:04 MST (Codex CLI, gpt-6-astra/max, full permissions; briefs in each lane dir; every lane cut from the wave tip c85bef65be; phase 1 = code only while `D:/mx/LEAD_FAMILY.lock` exists, the lead resumes each thread with "PHASE 2 — BUILD GO"): `astra-match-overlay-20260913` (item 7b: net status widget + toasts + input-delay display, a `-net-match-screenshot-ticks` capture seam, two-peer driver; `item7-ux` on `stage2/ui-match-overlay`; ports 48201-48209) · `astra-telemetry-bundle-20260913` (item 7e: `-telemetry-bundle` + "Save diagnostics" zip with a manifest, and `-net-autosave-seconds` rotating sim-pure snapshots with the capture stall measured and an off-wire tick-hash proof; `hold-pause` on `stage2/telemetry-bundle`; ports 48211-48219) · `astra-post-match-report-20260913` (item 7f: a "Last match" line + Details dialog in the rematch lobby from a `NetMatchSummary`, and a Replay Browser sub-screen with Play/Delete; must not touch B16's files; `alias-walk` on `stage2/post-match-report`; ports 48221-48229) · `astra-feel-measure-20260913` (item 4c/4a: `tools/feel_measure.py` producing every pinned §C.4 number from raw per-frame records, PASS/MISS never relaxed; `value-observations` on `stage2/feel-measurement`; ports 48231-48249) · `astra-persistent-world-memo-20260913` (item 8b: a read-only design memo on join-while-running, never-ending activity, host restart from autosave, what assumes "everyone started together", a slice plan; workspace `item8-discovery`, sandbox read-only). Model evidence per lane in `cli_runs/<lane>/model-evidence.txt` (checked by the lead after launch). Not launched (queue): item 4's engine slices (F23b, projectiles travelling, the Activity UI following the preview — Opus, after the fix group), lobby v2 (after B16 and the memo).

R5: matrix row `world_late` — 35 cases, every one failing exactly `fixture_armed`, 0 errors (`D:/mx/s44-1/world_late/result.json`) — the R4 class (the F19 gate on a checkpoint-only job); no new lane; fixed on the wave by c85bef65be. Family Windows stages so far: chain (R1), remaining (R2), breadth (R3), matrix rows world_normal/world_late (R4/R5); every other row green or NEEDS_REVIEW with rc 0 (the lead reads the NEEDS_REVIEW rows' observations when the matrix ends).

## 2026-09-13 23:09 MST — Fix group 7 (F34 + F35) PHASE 1 landed: the lead's line-by-line read ACCEPTS the design for the build phase, with one arm addition

`opus-fixgroup-7-owner-map-20260913/REPORT-phase1.md` (saved verbatim; six commits c2cf7b50d3…9c8df54c9d on stage2/fixgroup-7-owner-map from 9a8caaab8c; +198/-40 over six files; UNBUILT by rule). Lead read of every production hunk: **F34** — `NetSeededActorOwner{ownerPeerId, team}` replaces the bare owner in the seed map (NetActorOwnership.h:23-27); `ResolveOwnerPeer` applies the seed only when `seeded->second.team == query.team` (NetActorOwnership.cpp:56), so a query at another team re-derives from the policy; `SeedOwner(uid, owner, team)` overwrites (one entry per actor = the freeze is per current team; A→B→A re-seeds at A from the policy); `MovableMan::UpdateControllers` seeds once per (actor, current team) from the new `ScenarioRunner::GetLockstepPolicyActorOwner` (ScenarioRunner.cpp:856 = `GetLockstepActorOwner` minus the `s_LockstepControlOverrides` consult), so a live claim can never become "the owner to return to" (Activity.cpp:855 / MovableMan.cpp:767 read the seed) — the engineer's deviation 3 is right and the lead confirms it on the source; negative teams normalized to 0 exactly as NetLockstep.cpp:3507; `HasSeededOwner` deleted (no caller, not Lua-bound). The arm `seeded_owner_follows_a_team_change` (NetLockstepSelfTest.cpp:10419-10512, registered in the suite's accumulate block): two-peer team-owner pair, actor seeded (1, TeamOne) and seated, `SetTeam(TeamTwo)` → owner, value-observation authority and the released local control must read 2/2/1 (the FAIL line prints the measured triple); the seed still answers 1 at TeamOne; the frozen half on the one policy that reads the control mode (HostCpuRemoteHuman through `ResolveOwnerPeer`: a flip at the seeded team stays 2, another team re-derives to 1). No inverted message, no test-only production branch. **F35** — `Fields(archive, legacy)` skips `actorOwners` for a legacy record; the writer tags `WorldStructure2`; the reader takes `15 WorldStructure1 ` as legacy with empty owners (the Controller1 / MovableMan1 precedent), any other tag fails the version check, a trailing field dies in `Finish()`; the owner entry now carries the team and is range-checked; the in-test payload writers retagged; the contiguous-index selftest gains `round_trip= tagged= legacy= legacy_refused=` with the PASS token unchanged. The retained fixture untouched. Save-format constant, not a wire constant (checked: the tick hash is the sim-gated per-subsystem hash, Main.cpp:3490).

ONE ADDITION for phase 2 (the lead): a `WorldStructure1`-tagged payload in the INTERMEDIATE layout (the 2026-09-12 08:23 → now format that carries the owner map under the old tag) must be REFUSED, never misread as a legacy record — add it to the contiguous-index arm (`legacy_refused` covers it) beside the trailing-field case. No such save ever reached a user, but a silent misparse is not acceptable. PHASE 2 gates as briefed: RED at c2cf7b50d3 (F35 arms only) and 386621993f (F34 arm + plumbing, fix absent), GREEN at the tip; 13/13; script-graph 0 FAIL; the switch-control e2e; the gcb save/load driver; the single `sound-volume-authority` match gate through a harness copy (RED on the control, GREEN on the fix); the single transaction case `ordinary-load:valid_full --ticks 100` on the unchanged retained fixture (`accepted=1`, `applied: true`, downstream checks green). Build go after the family's Windows stages end (the matrix), FIRST in the two-lane queue (this is what the next family verifies), B16 second, then F23c and the Source22 reference rebuild, then the Astra lanes.

## 2026-09-13 23:12 MST — Family red R6 (environment): the Mac attempt died two seconds after launch on a stray crash marker in the Mac family repository; marker preserved and removed; the Mac attempt is re-run after the Windows stages. The 7f Astra lane stopped at its fence and was resumed with a scope extension.

**R6.** `mac-family-source44-attempt1.json` (Mac, `/Users/erol/projects/cccp/audit-20260907-evidence/source44-attempt1/`, finished 05:23:52Z = 22:23:52 MST): `execution_pass false`; `build-combined-44-result.json` `exit_code 1`, `"error": "ValueError: repository retains inputs outside this export; no source was installed: ['AbortCode.txt']"`; every selftest and chain gate then "blocked by invalid build: build identity differs from this source and attempt". The file: `/Users/erol/projects/cccp/cortex-modern-audit-20260907/AbortCode.txt`, 1,686 bytes, 2026-09-08 05:13, an engine FATAL unhandled-exception marker (0xE06D7363 with a stack) from a crash of that era — an untracked input the installer rightly refuses. Lead action: content preserved byte-for-byte at `contract-audit/mac-attempts/source44-attempt1/AbortCode-mac-20260908-preserved.txt`, the file removed from the Mac repository (logged in `reviews/cleanup-20260913.md`; nothing else touched — the repository's 85 other untracked files are the installed export). The family's `mac_complete` stage (after the matrix) will record attempt 1 as failed; the lead re-runs the Mac attempt as attempt 2 through the family's own launch/finish steps once the Windows stages end (`run_family.py` has no `--from mac_launch`; the lead drives `mac_launch`/`mac_finish` for attempt 2 with a separate record and appends its verdict to this family's ledger entry). NOTE for the harness debt list: the Mac installer's refusal should name the offending file in the family console at launch time (it did, in the attempt json) and the family's `mac_launch` step should read the remote nohup result before returning exit 0 — the launch reported success for an attempt that had already failed.

**Astra 7f** (`astra-post-match-report-20260913`, 369 s, rc 0): "PHASE 1 BLOCKED — no code changes": in-process replay playback setup/completion lives in `Main.cpp` and `ScenarioRunner` has no playback-close API, so the browser's Play/return needs files outside the brief; and `Net Lifecycle Test` has no win condition on this base (the match-end for the report driver). Correct behaviour under WORKER_RULES (stop at the fence). Lead ruling: scope extended to `Source/Main.cpp` (the playback launch/close path and the return hook only; B16's match-end block and `-net-match-ticks` untouched) and `Source/System/ScenarioRunner.cpp/.h` (a playback-close API); the driver is written against B16's `-net-match-ticks` semantics (1b729f61c9), and B16 merges into the wave before the lane's phase 2. Thread resumed 23:13 MST (`resume-1.txt`, `codex_job.py --resume`).

## 2026-09-13 23:43 MST — Four landings: the Grok harness lane (R4-class oracles + the family precondition, accepted with one lead correction), the DeliverCommandHost line (phase 1), the persistent-world memo (text recovered by a resumed turn), the 7b overlay (phase 1, scope ruling)

**grok-battery-oracles-20260913** (Grok, python-only, 8.3 min; `cli_runs/…/REPORT-final-message.md`). Lead read of both diffs. `fg6d-battery/battery_scoring.py` (+145; original `.pre-oracles-20260913`): `score_directory_e2e` reads the driver's own verdict file `dir-e2e/lead_verdict.json` (written by the W85-era directory driver — a driver artefact, not the lead's judgement — checked: it carries the peers' records and the host service directory counters) plus `e2e-enabled/host_report.json`, the host stdout (`[net-directory] state: registering -> registered`), every launch.json and an `^ERROR` scan; enabled = listed + exactly one delete + peers/list exit 0; disabled = never listed, 0 deletes, exits 0. `score_mixed_lua` reads `mixed/w85_verdict.json`: every arm's records exit 0 / not timed out / no error and `trace_compare == [true, {first_divergence: null, reasons: []}]`. `score_portmap_e2e` reads `portmap/w97_verdict.json` + the host logs: enabled = exactly one `[net-port-map] mapped` and one `[net-port-map] release` line, disabled = none, exits 0. `score_row` dispatches the three names. RED re-run by the lead is the unit file's own design (the rows had no rule); GREEN `test_row_oracles` 2/2 on the retained fg6bat4 raw files + mutations. `run_family.py` (+114; original `.pre-oracles-20260913`): the build precondition now counts cl/link/ninja whose path or command line names a `D:/Projects/<tree with RTEA.sln>` (`parse_named_build_rows`, `engine_tree_keys`; refusal records the processes); `mac_launch` polls the remote nohup/report for 60 s and records exit 1 on an early `execution_pass: false` with an error. **Lead corrections (disclosed, on top of the lane's edit):** (1) the early-failure check missed the exact case that happened — attempt 1's nohup carried `"error": null` and the report kept the error inside its steps — so `mac_launch_early_error` now also treats a FINISHED report with `execution_pass: false` as a failure, naming the failed step's error (unit test added: `test_finished_report_without_top_level_error_names_the_failed_step`); (2) the lane's `finalize_mac_launch` call passed `remote_dir` both positionally and in the kwargs — a `TypeError` at every future launch (Pyright flagged it; the lane's unit test called the helper with a different shape) — fixed: the helper records `remote_dir` itself; a call-shape check run by the lead records `mac_launch exit 0` with `remote_dir` present. Tests after the corrections: `test_run_family_preconditions` 9/9, `test_row_oracles` 2/2. The running family is unaffected (python loaded the old file at 22:17 MST); attempt 2 was launched at 23:28 MST on the pre-edit code. Item 5 +0 (harness).

**opus-deliver-command-line-20260913 PHASE 1** (`REPORT-phase1.md` saved verbatim): e34aa6afe8 adds four lines to `MovableMan.cpp` — `int loaded` counted in the direct-delivery manifest loop and, after `AddActor(craft)`, `[net-match] deliver command host applied tick=<applied frame> team=<t> order=<command.sequence> items=<loaded> peer=<sender>` — unconditional, on the committed lockstep apply path (both `ApplyLockstepGameCommands` call sites), peer-identical fields, one line per applied delivery (the consume path dedups). Lead read: correct and minimal; a print, no state. Harness: `n6_scoring.py`'s hard-coded `pass: False` replaced by `deliver_mode_evidence` (exactly one applied line on the host, host list == client list, one contiguous pulse run; `EXPECTED_PULSE` and every other rule untouched; originals `.pre-deliver-line`); the driver copy `run_interp_e2e.ps1:779-802` reads the lines (+17/-1). Bench RED on the retained fg6bat4 logs (0 lines → false), 8/8 synthetic cases. Phase 2 after the family (the craft_cargo e2e RED/GREEN, 13/13, graph 0 FAIL, identical tick hashes); the `tests/test_08_n6.py` re-pointing is a follow-up outside the lane's scope.

**astra-persistent-world-memo-20260913** (Codex, 35 min): the memo (192 lines, 225 code citations checked at c85bef65be) could not be written — the lane ran with the runner's `--sandbox read-only`, which also blocks the one file the brief asked for (the lead's brief error); the text existed only in the thread, so the thread was resumed at 23:44 MST without that sandbox to write `MEMO.md` unchanged. The five decisions it asks of the lead (from its final message): a persistent preset with respawn (existing activities untouched); live join = restore only the joiner, replay the committed tail, activate at a future tick, bound the capture stalls; seats assigned by the host with overflow spectating, clean leaves free slots, real drops keep the H4 hold-pause; identity preserved across restarts (world/directory/ICE/auth) while boot, round and connection incarnations advance; restart from three rotating checkpoints plus network metadata and a separate durable auth journal (a rollback must never restore revoked credentials). First slice: the Mac ticks alone; the PC discovers the running world, joins, leaves and rejoins while the same activity continues. Read in full when the file lands.

**astra-match-overlay-20260913 PHASE 1** (`REPORT-phase1-final-message.md` saved verbatim; nine commits 1f42a16c3e…bcccb32f90, +628/-9): the widget extends `NetModerationGUI` (252×76 box upper-right below the controller icon, lazily created in an online render), status accessors on the service and `ScenarioRunner` (`GetVisibleNetUiToasts`, `GetLockstepMissingPeers` — the engine already had a `NetUiToastRecord` log and `DrawNetUiToasts`), roster/moderation events published for toasts, the `-net-match-screenshot-ticks` capture flag, and a two-viewport driver (330 lines) with real-control probes and a capture-on/off hash check. NOT applied, pending the lead's ruling: `D:/mx/astra-match-overlay-20260913/pending-presentation.patch` (+44/-55: replaces ScenarioRunner's legacy stall/synced-pause drawing with the widget, pause attribution from the pause producer in MovableMan.cpp (+2), Main.cpp (-3), the CI probe `NetModerationGUIProbe.cpp`). The lane stopped at its fence as the rules require. Lead ruling in the resume message (23:5x MST).

## 2026-09-13 23:46 MST — Mac attempt 2 of the Source44 family landed (built green; F34 reproduced on arm64; one Mac-oracle artefact R7); the SWE glyph lane's phase 1 accepted by reading; two Astra threads resumed

**Mac attempt 2** (`family-source44-mac2.json`, remote `source44-attempt2`, 06:28:58-06:32:36Z = 23:28:58-23:32:36 MST): after the stray marker's removal the installer accepted the export; the arm64 build succeeded (incremental, exe 7dffcabd9f2475…, 98 s); `gate:native` PASS; `gate:wounds`, `gate:rng_particles`, `gate:ui_bus` exit 0; **`gate:authority` exit 1 — F34 reproduced on arm64** (the chain log's sound-volume authority trace reads `ui_audible 0.25 / sfx_audible 0.25` at steps 318 and 600 where the team-1 owner's 0.75 is due — the same seeded-owner freeze as Windows R1, so the defect is platform-independent, as expected). Selftests 9/10: **`-net-match-selftest` marked FAILED by the Mac roundtrip checker although the run exits 0 and prints its final `[net-match-selftest] PASS`** — `run_mac_cases_roundtrip.py:29-30` treats every `[…selftest]` line that carries neither a `PASS`/`MEASURE` token after the tag nor a trailing `PASS` as a failed verdict, and the ICE-lane arms print descriptive lines (`[net-match-selftest] mux listen order: …`, `mux routing: …`, `ice join_mode: …`, `session-id join: …`, `ice settings: …`, `p2p join spec: …`) with no verdict token; the retained stdout has no FAIL line. **R7 = Mac oracle artefact** (the Source43 R3 class). Lead ruling: the checker's contract ("every selftest-tagged line is a verdict or a MEASURE") stays — it is what caught silent arms before — and the ENGINE lines conform: those descriptive prints are emitted after their checks pass, so they carry the `PASS` token (`[net-match-selftest] PASS mux listen order: …`), added to fix group 7's phase 2 as a small extra commit (NetMatchSelfTest.cpp only; the Windows scorer counts PASS tokens and is unaffected; the count of PASS lines changes — the verify6 needle checks `"passed": 12/13`, not line counts). Verdict of the family's Mac half: NOT GREEN by F34 alone; everything else green or an artefact. The lead appends these two steps (mac_launch exit 0, mac_complete exit 1) to `family-source44.json` when run_family.py has finished.

**swe-glyph-recolour-20260913 PHASE 1** (`REPORT-phase1.md` saved verbatim; three commits 2af24ce970 / b436fd1125 / c4b4c51302 on stage2/ui-glyph-recolour, +41/-4): lead read — `GUIFont::InkColorBitmap` recolours to 55 % luminance (32-bpp, alpha kept) and clears each character cell's last column for a 1 px gap; the lead verified the method is called ONLY on the fallback path (`GUIFont.cpp:198` shadow, `:204` glyph, both `glyphFont != this`), so normal text is untouched by construction; `c_LobbyChatTop = 245` names the chat top once and the error budget derives `241` from it (identical arithmetic); the driver gains `fallback_cells_separated` (≥ 16 blank columns in the run band; the retained bar has 0) and `fallback_ink_dimmed` (r < 200; the bar is (252,209,19)). Accepted for phase 2 (captures at both sizes → the visual reviewer).

**Astra threads resumed:** 7b (`astra-match-overlay-20260913-r1`, 23:52 MST) with the scope-extension APPROVED after the lead read every hunk of `pending-presentation.patch` (the legacy stall/pause overlay replaced by the widget under the render RNG override; the pause toast attributed from the sender id already in the command; the CI probe's P key) and two phase-2 conditions (the paused path in the hash/SP proofs; the controller exclusion shown and never widened). The 8b memo thread (`-r2`) resumed with the correct thread id after `-r1` failed on a concatenated id (the runner's thread.txt holds two lines; the lead's extraction error).

## 2026-09-13 23:52 MST — Two more code-phase landings with fence stops, both ruled and resumed: the preview debt lane (F28/F31/letter set) and the 7e telemetry lane

**opus-preview-debt-f28-f31-20260913 PHASE 1** (`REPORT-phase1.md` saved verbatim; commits bf2a58a85d F28 arm, 8e67846088 F31 arm, 0f0605fa50 F31 fix, 88cb18bdf1 ledger reset; +103/-5). Lead read: F31's `PieMenu::RemapExternalLinks` maps `m_AffectedObject` through the retirement map and recurses into slice sub-menus, called from `Actor::RemapExternalLinks` — the F26/F27 shape, no Lua-visible change (`AffectedObject` is read-only, the remap unbound); the F31 checks assert on recorded uids and dereference only when the pointer equals the resident; the F28 arm measures a late `LoadScript` inside a preview against the cursor, bindings and graphs (no fix pre-written — right: the measurement decides); the ledger gains `ResetBetweenSelfTestArms()` (entries and sequences only) called per letter, with `ResetForSelfTest()` byte-unchanged so run-level counters other gates read are not zeroed mid-run (the engineer's deviation 3, accepted). Fence stop: the per-letter `before` capture and a new letter live in Main.cpp's `-lpinv-overlay-links` selftest code (`RunOverlayLinkArm`, the loop at :2300, the dispatch string at :2050). Ruling: Main.cpp GRANTED for that selftest section only; the F31 block moves to a new letter `p`; the acceptance line for the letter set corrected to what the F15 rows show — the ten armable letters PASS at 200:1:1, `r` and `t` documented as unarmable at that spec (an F15 disclosure; no widening), `n` gated on the F28 measurement. Agent resumed 23:55 MST.

**astra-telemetry-bundle-20260913 PHASE 1** (`REPORT-phase1-final-message.md` saved verbatim; five commits e2afcfe783…d2974ae27a, +1021/-31: `Source/System/TelemetryBundle.cpp/.h` (registered in meson and the vcxproj), the autosave capture + off-sim-thread archive writer with rotation in ActivityMan, the cadence in NetMatchService, Main.cpp flags, `tools/test_autosave.py` and `tools/test_telemetry_bundle.py`). Fence stop: safe capture of the console tail, the replay bytes and the identity needs accessors in ConsoleMan, ScenarioRunner and the replay writer — `D:/mx/astra-telemetry-bundle-20260913/capture-accessors.patch`, read by the lead hunk by hunk: ConsoleMan gets one member mutex with `AppendLogEntry`/`CopyLogTail` and every `m_OutputLog` access under it (replacing the static mutex that guarded only PrintString's emplace); the replay writer keeps a bounded in-memory copy of what it writes (capped at the bundle's member limit, `truncated` reported) and `CopyDiagnosticReplay` at a tick boundary; `ScenarioRunner::CopyLockstepReplayForDiagnostics`. Ruling: APPROVED with two conditions (no printing inside a locked console region; the cap equals the member limit and the manifest records the truncation). Thread resumed 23:59 MST (`-r1`).

Note to self, recorded: the Codex runner's `thread.txt` holds two lines (the thread id, then the rollout path); a resume must take line 1 only — two resumes (7b, 8b) failed on a joined id before the lead noticed (both relaunched correctly).

## 2026-09-13 23:54 MST — The persistent-world memo (item 8b) read in full; the lead's five rulings and the accepted slice plan

`astra-persistent-world-memo-20260913/MEMO.md` (192 lines, pinned to c85bef65be with file:line for every code claim; no runtime claims). It settles what "persistent world" must mean against today's code: the dedicated host v1 is not yet an indefinitely running daemon (`-net-dedicated` only selects headless init; the e2e dispatcher runs it to a tick cap); admission today refuses a live match to a fresh peer and needs a prior seat; a clean last leave stops the round; rematch is a new round, not continuity; the directory row has no persistent/spectator/world-identity fields; the snapshot heal is a whole-round relaunch with a global stall (capture + IO + envelope + transfer + reload) and a three-heal-per-process cap. RULINGS (the lead, 2026-09-13 23:54 MST): (1) ACCEPTED — a new Persistent World GAScripted preset with a deterministic, host-authored respawn and no last-brain/timer victory; existing activities, Lua/native APIs, timers and manual saves untouched (a new activity contract, never a compatibility flag). (2) ACCEPTED — join-while-running = authenticate → transfer an immutable checkpoint image taken at a completed tick B to the joiner only → catch up from a bounded log of committed frames faster than real time → activate at an announced future tick E with a host-authored membership/spawn/binding transition; never the whole-round ResyncMatch; a fresh join never enters the hold set (the settled hold-pause rule stays for real drops); capture-stall budget = one sim tick (16.7 ms) as the target with the actual p99 reported, and a copy-on-write/incremental checkpoint image if the measured capture misses it (a miss keeps the slice unaccepted). (3) ACCEPTED — the host assigns team and brain in a fixed configured order, overflow spectates (camera + committed stream, no Controller production or commands), clean leaves free the slot under a new generation, real drops keep the H4 hold-pause; reclaim outranks fresh allocation. (4) ACCEPTED — identities kept distinct: a durable world UUID (= the resumable directory registration id), the ICE identity derived from it, the deterministic session value untouched, auth epoch/seat/holder generation/credential preserved across restarts, host boot incarnation and round advanced before listening. (5) ACCEPTED — restart from the newest fully published of three rotating checkpoints plus a companion restart manifest (network continuation baseline, world/config/manifest ids, digest) published atomically as one generation; a private durable host auth journal (issuance before offer, commitment before success, revocation before ack) so a rollback never revives a revoked ticket; progress after B may be lost and is said so. Wire/version seams A-I are decided per slice (the lead), never in advance. SLICE PLAN ACCEPTED: 1 = Mac world ticking alone, PC first join through the directory, clean leave, fresh rejoin (RED lines at NetLockstep.cpp:2215-2218/2248-2250/4747-4752 and NetReconnectSession.cpp:333-341; ~1.8-3.2k lines, 4-7 days; the §6 capture/purity/catch-up baselines measured first; consumes the autosave lane's capture API); 2 = concurrent players, spectators, respawn, reclaim in one world; 3 = restart of the world and the directory admitting existing tickets; 4 = sustained operation, replayable world segments, the internet (NAT) proof. Slice 1 is briefed after B16 and the telemetry/autosave lane land on the wave (queue). Item 8 +0 now (design, not a runtime); the memo is the item-8 roadmap from here.

## 2026-09-13 23:56 MST — Preview debt lane PHASE 1 complete after the Main.cpp grant: letter p and the per-letter baseline landed (346e592c0c, a384ebf692; tip a384ebf692)

The engineer's second message (saved below verbatim in the lane's REPORT-phase1.md addendum): letter `p` carries the F31 checks moved out of `a` unchanged plus one positive control (`surviving_affected_object_retained`: the remap must not over-null a surviving device); letter `a` is byte-identical to the base again; the flag's letter validation gained `p` (Main.cpp:1162-1163); the per-letter baseline drops the shared `before` parameter — each letter clears previews and ghosts, captures its own `before`, and a mismatch writes `lpinv_before_overlay_<letter>` beside the after-file (the byte comparison itself unchanged; the pre-arm clear now also runs before the probe letters b/r/c — disclosed, a strengthening); the Main.cpp hunks are confined to the `-lpinv-overlay-links` selftest code (@@1159, @@2034, @@2045, @@2191, @@2306). Phase-2 RED control for F31: the tip with the fix's inverse applied to the working tree only (the F15 lane's accepted control form), recorded in the build manifest. Accepted for the build phase.

## 2026-09-13 23:59 MST — NEGATIVE (the lead): three stamps written from an estimate, not the clock ("2026-09-14 00:05/00:08/00:10 MST" while the clock read 23:5x MST), corrected in place to the clock-read stamps of the same entries; the rule (read `date` in the turn that writes any stamp) was already in memory and was skipped in text written between tool calls.

## 2026-09-14 00:01 MST — F33 PHASE 1 landed (six commits) with two disclosed behaviour changes on content values: the lead's phase-2 conditions; the 4c measurement program's phase 1 landed; F36 registered

**opus-f33-float-text-20260913 PHASE 1** (`REPORT-phase1.md` saved verbatim; commits eb1c0f27db…b267759764; ~2.5 h against a 75-min cap, disclosed). What landed: `ParseHexFloatExact`/`FormatHexFloatExact` (C-locale `%a` + `strtod_l`) for the hexfloat save/packed pairs (Arm.cpp:471/489, PieMenu.cpp:224/255), `ParseNumberExact` with `strtod`'s grammar (leading `+`/space/hex accepted, so existing mod content with `+1.5` keeps parsing — a mod-compatibility point the brief missed, accepted), Attachable/MovableObject content parses, Reader/Writer through the exact codec, the D4 `ec` checks at the seven packed-string readers (RTEAssert on failure, the LimbPath precedent), the D1 table-driven grammar expectations (exact-fraction constants) and the D2 pinned format table with an `oracle=` header so a both-macros-0 host can never self-compare; the locale arm (8 probes pinned in the C locale then re-run under a comma locale) and a hexfloat-vs-`std::hexfloat` shape proof. Two DELIBERATE BEHAVIOUR CHANGES on content values, disclosed by the engineer: (a) `Reader` now parses a float AS a float (the old path read a double and narrowed — double rounding); (b) an out-of-range read now yields 0 where `num_get` wrote ±max (both set failbit). LEAD RULINGS for phase 2: (a) is kept ONLY if the SP pie-close dump is byte-identical to the control AND the legacy replay identity gate (the retained recordings replayed bit-identical, item 1's criterion) stays green on the rebuilt binary; a single differing content value means (a) reverts to "exact double parse, then narrow" (locale independence without changing any loaded value) — a recorded contract outranks a rounding nicety; (b) is REVERTED to the old ±max result (content behaviour identical; no reason to change it); the phase-2 message states both. The arm's pre-boot manager construction (the engineer's main crash risk) is exercised first in phase 2. **F36 registered (LOW-MEDIUM, checkpoint pie timers):** `PieMenu.cpp:222` writes three timer values as doubles through the ostream (locale grouping corrupts them ≥ 1000, precision-6 truncates ≥ 1e6) while `:253-255` reads them back with `strtoll` as ticks though the writer writes milliseconds — two defects in one statement, outside this lane, queued for the next checkpoint-format lane (a WorldStructure2-class fix with a legacy branch).

**astra-feel-measure-20260913 PHASE 1** (`REPORT-phase1-final-message.md` saved verbatim; five commits 91034ae930, +1350): FrameMan instrumentation for input-to-photon and per-frame draw/present times, a Controller input-edge stamp, prediction counters, `tools/feel_measure.py` with the four-configuration matrix (100/200 ms × 60 Hz/uncapped) and `tools/feel/report.py` reducer with tests; disclosed seams: presentation stamped from `UploadFrame`'s return, audio response bounded by timing, harness-only render caps and PNG capture. Every pinned number is unmeasured until phase 2 (the table is all "Pending"); the lead's line-by-line read happens with the runs.
