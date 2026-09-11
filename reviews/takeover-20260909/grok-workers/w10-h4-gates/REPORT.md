# W10: causal trace of four long-red H4 gates and the match-start AIOrder

Read-only on `D:\mx\s41b3` and every repository. Wrote only under this folder.

Pin: Source41 breadth v3, tree `c8f8188ae0` (exe `bb3cf264…`), jobs `j33`/`j34`/`j36`/`j37`. Commands that built the quotes: `python extract_evidence.py` → `job_listing.txt`; `python copy_quotes.py` → `quotes\`; `python extract_fields.py` → `report_fields.txt`; `python search_resume_findings.py` → `doc_findings.txt`; `python show_c8f_findings.py` → `c8f8188ae0_findings.txt`. `git -C D:\Projects\p4b-interp-validation rev-parse HEAD` now prints `9751a90e29`; finding A/B sites below are `git show c8f8188ae0:Source/Network/…` (same text as the live files at those lines).

---

## Findings A and B at `c8f8188ae0`

RESUME.md:95 (2026-09-09 ~02:20 UTC): `A (NetMatchService.cpp:724 PumpSessionEvents adds 15 ms per call and is called twice a tick, halving the 20 s seat hold; use monotonic elapsed time) and B (NetSession.cpp:165 TickAdmissionPlane never expires silent connections, so eight of them block reconnects)`.

**A is not present at the cited site.** `c8f8188ae0:NetMatchService.cpp:724` is `events.swap(m_PendingSessionEvents);`. `AdmissionNowMs` (`:610-611`) is `m_AdmissionClock.NowMs(SteadyNowMs())`. `NetAdmissionClock::NowMs` (`NetReconnectSession.h:53`) is elapsed steady time. The pump is still invoked twice in a stall (`Main.cpp:2459` once per tick, plus `ScenarioRunner.cpp:1152-1156` every 15 ms of wall time inside `WaitForLockstepControllerFrame`); that 15 is pump *pacing*, not a clock step. Self-test comment `NetReconnectSessionSelfTest.cpp:4256-4259` names the old defect. A would have shortened the P2 hold on `clean_leave` ambiguous / `reclaim_socket` / `rejoin_after_resync`. These Source41 runs recovered the stall in 4447–4450 ms and completed the reclaim, so A is not the residue that failed them.

**B is not present as stated.** `TickAdmissionPlane` is at `:166` (RESUME said `:165`). At `c8f8188ae0` it calls `ExpireSilentHandshakes()` (`NetSession.cpp:172`). That function (`:762-774`) expires only `Handshake` peers older than `timeoutMs`. Mid-match `Tick()`/`CheckTimeouts` still does not run from the pump (`PumpSessionEvents` calls `TickAdmissionPlane` only, `:750`). B would block a returner if eight silent hellos filled `c_MaxUnauthenticatedPeers`. These runs' returners committed (`client_commits=1`), so B did not fire here. It is not why fencing's host report reads `None`/`0`.

---

## 1. `clean_leave` (`D:\mx\s41b3\j33\clean_leave_20260910_021812`)

### 1.1 Failing checks

`quotes\j33\clean_leave_20260910_021812\verdict.json:40-42,64-66`:

```
"name": "clean_old_ticket_refused", "status": "fail", "detail": "used_stored_ticket=True commits=1"
"name": "hosts_survived", "status": "fail", "detail": "exits=[0, 0, 1] fatal=False"
```

Driver (`clean_leave.py:199-203,281-290`): refuse means `client_used_stored_ticket is not True` **or** `client_commits==0`. `hosts_survived` requires every record whose key endswith `"host"` to exit 0. Insertion order: `clean.host=0`, `clean.stale_host=0`, `ambiguous.host=1` → `[0,0,1]`. The exit-1 peer is **ambiguous_host**, not a clean-arm host.

Passing on this run: `clean_leave_acked 1`, `clean_leave_cleared_ticket`, `host_closed_the_seat 1`, `ambiguous_kept_ticket`, `ambiguous_reclaim_worked used_stored_ticket=True commits=1`.

### 1.2 Intended behaviour

`STAGE2_H4_RECONNECT_PLAN.md:382-386`:

> Clean leave IS a protocol, not a flag: `LeaveRequest → host closes the seat + revokes the holder generation + its challenges → LeaveAck`. Only an acknowledged LeaveAck clears the client ticket; … Gate: after a clean leave the OLD ticket cannot reclaim.

`STAGE2_H4_RECONNECT_PLAN.md:388-394` (P22): a transport disconnect and a match `Complete` do **not** clear the recovery record; rematch keeps epoch and tickets.

Driver docstring (`clean_leave.py:3-8`): clean arm must LeaveAck, close the seat, delete the client ticket; a later process with the pre-leave copy must not reclaim. Ambiguous terminate must keep the ticket and reclaim.

### 1.3 Event sequence

Clean arm (`clean_client\stdout.log:6-7`, `clean_host\stdout.log:7`, `clean_host\runtime\LogConsole.txt:9-11`):

```
[net-match] leave: quitting to menu at tick 300
[net-reconnect] leave: Left (ticket cleared)
[net-match] Client left the match at frame 301 (Match left)
NETWORK: Match complete: Victory!
```

Stale arm is a **new** host on `PORT_CLEAN+1=46471` with the copied ticket (`clean_leave.py:161-182`). `clean_stale\stdout.log:4-7` joined and wrote a report; `clean_stale_report` `client_used_stored_ticket=True`, `client_commits=1`, `client_reject_reason=HostNotAccepting` (`report_fields.txt:55-57`). `clean_host2` `admission.reclaims_accepted=0`, `new_joins=1`, `incarnations_bound=1`, seat 1 `committed=True` (`report_fields.txt:76-92`).

Ambiguous arm (`ambiguous_host\stdout.log:7-16`, `LogConsole.txt:8-17`):

```
ERROR: Rejected a AIOrder command from a peer that does not control team 1
[net-match] waiting on peer frames (tick 249, Client)
[net-match] Client left the match at frame 249 (connection lost)
[net-match] peer stall recovered after 4447ms (tick 249)
[net-match] rejoin: Client reconnected - resyncing the match
[snapbench] save … saved=1
[net-match] resync: match relaunched from the snapshot
SYSTEM: Activity "P4 Alpha Duel" was successfully started
SYSTEM: Activity "P4 Alpha Duel" was ended
```

`ambiguous_host_report`: `exit_code=1`, `runtime_error=activity ended in state Over`, `running_ticks=0`, `resyncs=1`, `service.state=Failed`, `runner` absent, `reconnect.seats=[]`, `host_seats_dropped=1` (`report_fields.txt:1-30`). `ambiguous_client2` same Over after snapshot launch (`stdout.log:4-8`, `LogConsole.txt:9-11`).

### 1.4 Engine path for the observed outcomes

**`hosts_survived` third exit = 1.** After a successful relaunch `Main.cpp:2114-2117` zeros `s_netMatchServiceE2ERunningTicks`. `Main.cpp:2935-2938`: `activityState == Over && runningTicks < 100` sets `s_netMatchServiceE2EError = "activity ended in state Over"` and exit 1. `ReportRuntimeError` (`NetMatchService.cpp:490-518`) moves `m_Session`/`m_Runner` out, `EndAdmissionSession` (`:377-387`) clears `m_SeatStatuses`, so `BuildReportJson` (`:928-930`) omits `runner` and writes `seats=[]`.

The Over itself: `P4AlphaDuel.lua:116-134` calls `ActivityMan:EndActivity()` when a team that had a brain no longer has one. `ActivityMan.cpp:1011-1017` *defers* that end while `IsLockstepHoldingSeatForReclaim()` (`LogConsole.txt:10` `Holding the match open`). After the returner is in, the hold is gone; the first post-resync `UpdateActivity` completes the end. `winner_team=0`, `actors=2` / `actors_peak=4`.

**`clean_old_ticket_refused used_stored_ticket=True commits=1`.** `NetReconnectClient::BeginAdmission` (`NetReconnectSession.cpp:1340-1347`): if the file loads and `record.hostAddress == m_HostAddress`, set `m_UsedStoredTicket=true` and `BeginReclaim`. `SetHostContext` uses `request.address` (`NetMatchService.cpp:1203`) = `"127.0.0.1"` with no port (`Main.cpp:3630`). The copy therefore looks like this new host's ticket. Reclaim is refused (`reclaims_accepted=0`, last reason `HostNotAccepting`). `AbsorbRejection` (`:1358-1373`) keeps `m_UsedStoredTicket` and `BeginNewJoin`. Host2 accepts a **new join** (`new_joins=1`). The driver check cannot see that distinction.

### 1.5 Mechanisms

1. **Sticky `UsedStoredTicket` after fallback new-join** (`NetReconnectSession.cpp:1342-1373`). Must execute: load ticket → address match → `BeginReclaim` → deny → `AbsorbRejection` → `BeginNewJoin` → commit. Confirms: host2 `reclaims_accepted=0` / `new_joins=1` and stale `client_reject_reason=HostNotAccepting` with `commits=1`.
2. **Stale control is a different hosted session.** `clean_leave.py:161-168` starts `clean_host2` on port 46471. P22/P21 revoke applies to the *first* session (seat 1 `closed=True` on `clean_host`). Confirms: first host `seats_closed_by_leave=1`; second host seat 1 is a fresh committed join.
3. **Ambiguous host exit 1 is post-resync Over, not a crash.** `Main.cpp:2116-2117` then `:2935-2938`. Confirms: `saved=1`, relaunch line, then `running_ticks=0` / `activity ended in state Over`. Finding A did not cut the 4447 ms hold short of P2.

---

## 2. `reclaim_socket` (`D:\mx\s41b3\j34\reclaim_socket_20260910_021916`)

### 2.1 Failing checks

`quotes\j34\reclaim_socket_20260910_021916\verdict.json:16-18,48-50`:

```
"name": "host_exit_zero", "status": "fail", "detail": "1"
"name": "host_saw_seat_drop_and_return", "status": "fail", "detail": "0 committed seats"
```

`host_reseat_issued` **pass** (`verdict.json:72-74`): `host_reseats_without_survivors=1`. Returner reclaim/commit pass. `no_authority_error` only looks for `Rejected a Reseat`, not AIOrder.

### 2.2 Intended behaviour

Plan §4/P4 + §6/P20 + §8 ledger: a mid-match drop holds the stable seat; the returner proves and keeps the same lockstep peer id; the host reseats from the drop-time ledger; the match continues after resync. Driver (`reclaim_socket.py:1-19`): host exit 0; `reconnect.seats` has a committed, not-dropped seat; reseat line or equivalent counters.

RESUME.md:107 lead decision: a dropped seat with a valid ticket is held open for the reclaim window even as the last remote peer.

### 2.3 Event sequence

`host\stdout.log:7-16` / `LogConsole.txt:8-17` (same shape as ambiguous_host; leave at **tick/frame 250**, stall 4448 ms):

```
ERROR: Rejected a AIOrder command from a peer that does not control team 1
… Client left the match at frame 250 (connection lost)
… peer stall recovered after 4448ms (tick 250)
… rejoin: Client reconnected - resyncing the match
… saved=1 … resync: match relaunched from the snapshot
SYSTEM: Activity "P4 Alpha Duel" was successfully started
SYSTEM: Activity "P4 Alpha Duel" was ended
```

`host_report`: `exit_code=1`, `runtime_error=activity ended in state Over`, `running_ticks=0`, `resyncs=1`, `runner` absent, `seats=[]`, `host_seats_dropped=1` (`report_fields.txt:125-154`). Returner: snapshot launch then Over (`returner\LogConsole.txt:9-11`).

### 2.4 Engine path

`host_exit_zero` is the same `Main.cpp:2935-2938` path as §1.4 after `Main.cpp:2114-2117` reset. `host_saw_seat_drop_and_return` reads `host_report.reconnect.seats` for `committed and not dropped` (`reclaim_socket.py:158-166`). After `ReportRuntimeError` → `EndAdmissionSession` (`NetMatchService.cpp:386` `m_SeatStatuses.clear()`), `BuildReportJson:888-898` emits `seats=[]`. The drop *did* happen: `host_seats_dropped=1` is copied from `m_ReconnectHost` stats before the seat list (`:878-880`).

### 2.5 Mechanisms

1. **Post-resync P4 Alpha Duel Over in the first 100 reset ticks** (`P4AlphaDuel.lua:128-134` + `ActivityMan.cpp:1011-1017` hold during the drop + `Main.cpp:2116-2117,2935-2938`). Confirms: `saved=1`, then `running_ticks=0` / exit 1 / `Holding the match open` only on the *first* activity.
2. **Failed-report seat list is empty by construction** (`NetMatchService.cpp:386,490-518,888-898,928-930`). Confirms: `seats=[]` while `host_seats_dropped=1` and the returner `client_commits=1`.
3. Finding A unused here (4448 ms hold). Finding B unused (`census_refusals=0`, returner committed).

---

## 3. `fencing_two_transports` (`D:\mx\s41b3\j36\fencing_two_transports_20260910_022107`)

### 3.1 Failing checks

`quotes\j36\fencing_two_transports_20260910_022107\verdict.json:24-26,32-34,40-42,56-58`:

```
"name": "host_bound_second_incarnation", "status": "fail", "detail": "None"
"name": "host_fenced_the_old_transport", "status": "fail", "detail": "fenced_disconnects+fenced_packets=0"
"name": "seat_not_dropped_by_stale_timeout", "status": "fail", "detail": "[]"
"name": "host_survived", "status": "fail", "detail": "1"
```

`second_client_committed` and `first_client_superseded` pass. No AIOrder line in this job.

### 3.2 Intended behaviour

`STAGE2_H4_RECONNECT_PLAN.md:368-374`:

> Two connections may hold the same valid ticket. Policy: a newly PROVEN connection REPLACES the old transport for the seat. … The superseded transport is **fenced** — its delayed/in-flight packets are ignored for the seat, and its later timeout MUST NOT evict the seat.

P20 (`STAGE2_H4_RECONNECT_PLAN.md:149`): incarnation +1 on each proven rebind; a lower incarnation's `PeerDisconnected` is stale, not a seat loss. Driver (`fencing_two_transports.py:7-10,146-157`): `incarnations_bound>=2`, `fenced_disconnects+fenced_packets>=1`, a committed not-dropped seat, host exit 0. Caveat in the driver (`:21-23`): GNS timeout may delay the fenced disconnect.

### 3.3 Event sequence

Host `stdout.log:7-9` / `LogConsole.txt:8-13`:

```
[net-match] rejoin: Client reconnected - resyncing the match
[net-match] resync failed: resync snapshot save failed
SYSTEM: Activity "P4 Alpha Duel" was successfully started
SYSTEM: Activity "P4 Alpha Duel" was ended
NETWORK: Resyncing from the host (1): ResyncRequested:player rejoined
ERROR: Cannot save when there's no game running, or the game is finished!
NETWORK: Resync failed: resync snapshot save failed
```

No `Holding the match open`, no `Client left`, no AIOrder. Host report: `running_ticks=563`, `resyncs=1`, `runtime_error=resync failed: resync snapshot save failed`, `runner` absent, `seats=[]`, `host_seats_dropped=0` (`report_fields.txt:156-185`).

Client1 `stdout.log:6` / `LogConsole.txt:9-10`: `tick 566 lockstep wait: PeerDisconnected:Host disconnected: seat reclaimed by a newer connection`. Client2 `stdout.log:4`: `setup failed: SendMessageToConnection failed with EResult 3` … `closed by remote host, reason code 1000. (transport stopped)`. Client2 still `client_used_stored_ticket=True`, `client_commits=1` (`report_fields.txt:210-211`).

### 3.4 Engine path

**Why the activity is Over before the save.** `P4AlphaDuel.lua:128-134` ends the match when a brain that existed is gone. First client is still connected, so `IsLockstepHoldingSeatForReclaim()` is false and `EndActivity` is **not** deferred (`ActivityMan.cpp:1011-1017`). E2E allows Over after 100 running ticks (`Main.cpp:2933-2946`); host reached `running_ticks=563`. Then client2's commit makes a Ready session peer that lockstep does not yet use → `PumpSessionEvents:772-776` `RequestResync("player rejoined")`. Host `ResyncMatch` (`NetMatchService.cpp:197-199`) calls `SaveCurrentGame`. `ActivityMan.cpp:140-142`: if `activity->GetActivityState()==Over`, print `Cannot save when there's no game running, or the game is finished!` and return false. `Main.cpp:2120-2127` `ReportRuntimeError("resync failed: resync snapshot save failed")`, exit 1. Transport stop is reason 1000 → client2 EResult 3.

**Why `host_bound_second_incarnation` is `None` and fence count is 0.** The second incarnation *did* bind: `NetSession.cpp:583-588` `Disconnect(…, "seat reclaimed by a newer connection")` is exactly client1's stop; `BindIncarnation` (`NetReconnectSession.cpp:556-587`) increments `incarnationsBound` and records a fence. Those counters live on `m_ReconnectHost` / `m_Session`. `ReportRuntimeError` destroys the session before `BuildReportJson` (`:928` requires `m_Runner && m_Session && m_Coordinator`). Driver reads `runner.session.admission.incarnations_bound` and `session.stats.fenced_*` (`fencing_two_transports.py:127-158`) → missing → `None` / `0`. `fenced_disconnects` increments on the *later* `PeerDisconnected` of the old transport (`NetSession.cpp:336-338`, `NetReconnectSession.cpp:1043-1051`). Host already Failed before that timeout was counted into a report.

**Residue `activity ended in state Over` / cannot-save.** Not a timer and not the 900/1200 tick cap (`running_ticks=563`, `frames_planned=1200`). It is `P4AlphaDuel`'s brain-death win (`lua:116-134`) completing because no seat was held. Rematch on j37 shows the same activity can `Victory!` and continue only when `-net-match-e2e-rematch` rides Over (`Main.cpp:2874-2878`). Fencing has no rematch flag.

### 3.5 Mechanisms

1. **Resync save of an already-Over P4 Alpha Duel** (`P4AlphaDuel.lua:134` → `ActivityMan.cpp:140-142` → `NetMatchService.cpp:198-199` → `Main.cpp:2120-2127`). Confirms: console order is Activity ended **then** Resyncing **then** Cannot save; `running_ticks=563>=100`.
2. **Failed teardown strips the fencing counters the gate reads** (`NetMatchService.cpp:490-518,928-930`). Confirms: client1 superseded text + client2 `commits=1`, host `admission` absent.
3. Finding B is not why client2 missed the seat (it committed). Finding A is irrelevant (no drop hold; `host_seats_dropped=0`).

---

## 4. `rejoin_after_resync` (`D:\mx\s41b3\j37\rejoin_after_resync_20260910_022132`)

### 4.1 Failing checks

`quotes\j37\rejoin_after_resync_20260910_022132\verdict.json:32-34`:

```
"name": "resync_match_continued", "status": "fail", "detail": "1"
```

That check is `host.exit_code==0` (`rejoin_after_resync.py:154-158`). Rematch checks pass (`rematch_completed 0`, seat 1 committed, ticket survived).

### 4.2 Intended behaviour

Plan §7: a return during a live match resyncs from the host snapshot; the seat stays the holder's; the match keeps running. Rematch keeps epoch and tickets (P24, no timer). Driver (`rejoin_after_resync.py:5-8,17-19`): returner uses the stored ticket; host log has `resyncing the match`; host exits 0 after the tick cap.

### 4.3 Event sequence

`resync_host\stdout.log:7-16` / `LogConsole.txt:8-17`: same as reclaim, leave at **tick/frame 248**, stall 4450 ms, `saved=1`, relaunch, then Activity ended. Report: `exit_code=1`, `activity ended in state Over`, `running_ticks=0`, `resyncs=1`, `seats=[]`, `host_seats_dropped=1` (`report_fields.txt:218-247`). Returner: snapshot then Over (`resync_client2\LogConsole.txt:9-11`).

Rematch arm (`rematch_host\stdout.log:7-9`): `rematch: match 1 over (Victory!), returning to lobby` then `round 2 launching`; both rematch peers exit 0; `running_ticks=901`.

### 4.4 Engine path

Identical to §2.4: `Main.cpp:2116-2117` reset, first post-resync Over, `:2935-2938` exit 1. Rematch uses a different ride (`IsFirstE2ERematchReady` / `:2874-2930`) that *expects* Over and relaunches from lobby; that is why rematch is green and resync is not.

### 4.5 Mechanisms

1. **Same post-resync Over-in-first-100-ticks as reclaim/ambiguous** (`P4AlphaDuel.lua:128-134`, `Main.cpp:2935-2938`). Confirms: `resyncs=1`, `saved=1`, `running_ticks=0`, rematch on the same binary exits 0 after a deliberate Over.
2. **E2E treats Over after a snapshot restart as a broken setup**, not as a finished duel. Confirms: rematch comment at `Main.cpp:2933-2934` vs the `<100` fail; rematch resets ticks *and* continues (`:2928-2930`).
3. Findings A/B unused (4450 ms hold, returner committed, rematch seats intact).

---

## 5. Match-start `AIOrder` rejection

### 5.1 Facts

Printed only on the **host** console at first activity start, `LogConsole.txt:9`, in `clean_leave` ambiguous, `reclaim_socket`, and `rejoin_after_resync`. Absent from fencing, clean-arm hosts, rematch, and every surviving client console (first clients were terminated before `LogConsole.txt` was written: no file under `ambiguous_client1` / `j34\client1` / `resync_client1`). Present on Source40 at the same path:line (TRIAGE.md).

Apply site `MovableMan.cpp:415-417`:

```
} else if (!ScenarioRunner::IsLockstepTeamCommandSender(commandTeam, command.senderPeerId)) {
    g_ConsoleMan.PrintString("ERROR: Rejected a " + … NetGameCommandTypeName(…) … " command from a peer that does not control team " + …);
```

`IsLockstepTeamCommandSender` (`ScenarioRunner.cpp:697-701`) is `NetActorOwnership::IsTeamCommandAuthority(matchConfig, team, senderPeerId)`: a human slot on that team with `peerId==sender` (`NetActorOwnership.cpp:59-72`). E2E sets `ownershipPolicy = TeamOwner` (`Main.cpp:3634`). Lobby lines on every host: `peer1=Host(team0,…) | peer2=Client(team1,…)`. Team 1's lawful sender is peer 2; the host is peer 1 / team 0.

`setup_surface` in every report is `fixed-alpha-duel` (`Main.cpp:3479`). Gate argv is `-net-match-service-e2e` + `-net-match-e2e-resync` (ambiguous/reclaim/rejoin/fencing), **not** `-net-match-e2e-ai-order-command`. That selftest (`Main.cpp:2307-2329`, `ScenarioRunner.h:52`) therefore did not run. Headless has no pie-menu `IssueAIOrder` (`GameActivity.cpp:1412,1486,1684`).

The remaining production enqueue that can fire at match start is `Actor.cpp:1477-1478`: if lockstep-local and a waypoint is loaded, `EnqueueLocalGameCommand(NetGameCommand{0, NetGameAIOrder{… PopWaypoint, team=m_Team …}})`. `QueueLocalInput` restamps `senderPeerId` to `localPeerId` (`NetLockstep.cpp:2004-2006`). `P4AlphaDuel:StartActivity` (`P4AlphaDuel.lua:70-86`) `SwitchToActor`s every active human, including team 1, on **both** peers.

### 5.2 Mechanisms (two)

1. **Host-authored PopWaypoint for a team-1 unit.** `IsLockstepLocalActor` / `TeamOwner` falls back to `hostPeerId` when `FindHumanPeerForTeam` still sees `peerId==0` (`NetActorOwnership.cpp:28-31,59-72`). Host then queues `AIOrder` with team 1; restamp makes `senderPeerId=1`. Apply requires peer 2. Must execute: first `UpdateMovePath` on host while the team-1 slot is still unowned → `Actor.cpp:1478` → `MovableMan.cpp:415`. Confirms: reject is host-only, team **1**, immediately after `Activity … started`, before any leave.
2. **Client-authored order applied before the team-1 human is in `matchConfig.players`.** Client is the TeamOwner once peer 2 is listed; if the first committed frame carries the order while `teamHasHuman` is still false, only `hostPeerId` is accepted (`NetActorOwnership.cpp:72`). Confirms the same host print (both peers apply the same command set). Weaker: lobby snapshot already names `peer2=Client(team1)` before the first tick, and fencing (same e2e start, no drop) has no line, so the race is not every launch.

Sender: not the `-net-match-e2e-ai-order-command` fixture. The command is a `NetGameAIOrder` for team 1 that entered the first applied lockstep frame from `Actor::UpdateMovePath` (or, less likely, `GameActivity::IssueAIOrder`) on the peer that then believed it owned that actor. The reject means that frame's `senderPeerId` was not the team-1 human.

---

## Evidence index

| File | Produced by |
|---|---|
| `quotes\j33\…` `j34` `j36` `j37` | `copy_quotes.py` from `job_listing.txt` |
| `report_fields.txt` | `extract_fields.py` |
| `doc_findings.txt` | `search_resume_findings.py` |
| `c8f8188ae0_findings.txt` | `git show c8f8188ae0:Source/Network/NetMatchService.cpp` and `NetSession.cpp` |
| Drivers | `D:\Projects\reviews\claude-review-2026-09-08\lanes\source33-lanes\driver\h4gates\{clean_leave,reclaim_socket,fencing_two_transports,rejoin_after_resync,common}.py` |
