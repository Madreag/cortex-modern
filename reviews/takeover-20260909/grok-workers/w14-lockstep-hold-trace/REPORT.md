# W14: lockstep hold-pause hook map

Read-only trace of `D:\Projects\takeover-build` at `be0a2672a90c0d191fea1bacf0636146af629a4f` (`stage2/takeover-msvc`). Commands: `rg` / `Read` on `Source\` and the named gate drivers. No builds, no launches.

Today a dropped seat **does not pause lockstep**. After `ApplyPeerLeave`, that peer stops being required and survivors keep committing frames. The "hold" is a **scripted-outcome / ownership** hold (1200 sim frames) plus a **separate admission wall-clock** (20 000 ms). The user's 2026-09-11 decision (no committed frames while a seat is held) is not implemented.

---

## 1. Peer drop detection and continuation

### Prints

`[net-match] waiting on peer frames (tick N, …)` and `peer stall recovered after N ms` are **not** in `NetLockstep*.cpp` / `NetMatchRunner.cpp` / `NetMatchService.cpp` / `Main.cpp`. They are in the sim wait:

```1581:1591:D:\Projects\takeover-build\Source\System\ScenarioRunner.cpp
			const uint32_t stallMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count());
			if (stallMs >= nextOverlayMs) {
				const std::string missing = s_LockstepCoordinator->DescribeMissingPeers();
				if (!stalled) {
					stalled = true;
					std::cout << "[net-match] waiting on peer frames (tick " << tick << (missing.empty() ? "" : ", " + missing) << ")" << std::endl;
				}
				if (s_LockstepStallOverlayEnabled) {
					DrawLockstepStallOverlay(stallMs, timeoutMs, missing);
				}
				nextOverlayMs = stallMs + 200;
```

Recovery print at `ScenarioRunner.cpp:1547-1549`. Overlay first fires at **1500 ms** (`nextOverlayMs = 1500` at `:1529`).

Leave print:

```4190:4195:D:\Projects\takeover-build\Source\Network\NetLockstep.cpp
		std::cout << "[net-match] " << DescribePeer(peerId) << " left the match at frame " << firstFrameWithout << " (" << message << ")" << std::endl;
		NetLockstepStop notice;
		notice.senderPeerId = peerId;
		notice.reason = announced ? NetLockstepStopReason::PeerLeft : NetLockstepStopReason::PeerDropped;
		notice.frame = firstFrameWithout;
		notice.message = message;
```

Messages used: `"connection lost"` (`:3698`), `"no frames for " + budget + "ms"` (`:4284`), `"unreachable: …"` (`:4299`). Clean leave uses the caller's string (`Leave("bye")` / `"Match left"`).

### Which timer decides the peer is gone

Three independent drop clocks, then one missing-frame fail:

| Clock | Where | Bound | What it does |
|---|---|---|---|
| Host silence | `AdjudicateSilentPeers` `NetLockstep.cpp:4238-4285` | `PeerSilenceLeaveMs()` = `timeoutMs / 2` (`NetLockstep.h:619`) | Relay host, **≥2 remotes**, required peer silent → `ApplyPeerLeave(..., false, true)` |
| Transport disconnect | `HandleEvent` `PeerDisconnected` `:3668-3709` | Immediate | Relay host + mapped peer + `Running` + `nextFrame > 0` → `ApplyPeerLeave(..., "connection lost", false)` |
| Relay unreachable | `FlushRelayBacklog` / `DropUnreachablePeers` `:3628-4299` | silence / `CongestionHoldLeaveMs()` = `timeoutMs * 3/4` | `ApplyPeerLeave(..., "unreachable: …", false, true)` |
| Missing-frame fail | `AdvanceReadyFrames` `:4400-4402` | `timeoutMs` | `Fail(MissingFrameTimeout)` — ends the **round**, not one seat |
| Sim wait give-up | `WaitForLockstepControllerFrame` `:1522-1598` | `timeoutMs + 50` | `"timed out waiting for lockstep frame N"` |

Production lockstep `timeoutMs` is `NetMatchRunnerConfig::missingFrameGraceMs` default **20000** (`NetMatchRunner.h:39`, assigned `NetMatchRunner.cpp:328`). `NetMatchService` does not override it (`:1113-1134`). Session heartbeat `timeoutMs` is a **different** clock (5000 ms, `NetMatchService.cpp:1394`).

`AdjudicateSilentPeers` **does not run** on a 1-remote (1v1) host (`m_RemotePeerIds.size() < 2`, `:4239`). That 1v1 drop is `PeerDisconnected` or `MissingFrameTimeout`. Comment at `:4237-4238`.

`Tick` order (`:3023-3049`): `RefreshLeftSeatHolds` → poll → backlog → `DropUnreachablePeers` → `AdjudicateSilentPeers` → `AdvanceReadyFrames` → `EndRoundIfNobodyIsComingBack`.

### Missing seat's inputs after the leave

**No synthetic / empty frames are injected for the dropped seat.**

`IsRemoteRequiredForFrame` (`:3273-3279`):

```3273:3279:D:\Projects\takeover-build\Source\Network\NetLockstep.cpp
	bool NetLockstepCoordinator::IsRemoteRequiredForFrame(uint8_t peerId, uint64_t frame) const {
		if (frame < EffectiveStartOf(peerId)) {
			return false;
		}
		const auto leaveIt = m_PeerLeaveFrames.find(peerId);
		return leaveIt == m_PeerLeaveFrames.end() || frame < leaveIt->second;
	}
```

From `firstFrameWithout` onward the peer is not required. `AdvanceReadyFrames` (`:4303-4377`) commits as soon as local + every **still-required** remote are in. `ApplyPeerLeave` ends with `AdvanceReadyFrames(nowMs)` (`:4219`), so the next frames commit immediately.

Survivors' apply set has no controller frames from the leaver. `NeutralizeUnframedLockstepActors` (`MovableMan.cpp:365-370`) calls `ApplyWireNeutral()` on those actors. Then (`MovableMan.cpp:4321-4329`) `PurgeLockstepControlOverridesForGonePeers` and `IsLockstepActorOwnerGone` may `SetDisabled(true)`.

`IsPeerGoneAtFrame` (`:3223-3228`): `frame >= leaveFrame`.

### How the coordinator keeps producing

- `IsRunning()` = `m_State == Running` (`NetLockstep.h:520`).
- `HasCommittedAFrame()` = `m_Stats.framesAccepted > 0` (`:563`).
- After a drop with a survivor, or a 1v1 drop with the seat still held, `m_State` stays `Running` (`ApplyPeerLeave` `:4213-4218` only `Stopped` when every remote has left **and** (`announced` **or** `!AnyLeftSeatHeld()`)).
- Commit path: `Tick` / `HandleFrame` / `ApplyPeerLeave` → `AdvanceReadyFrames` → `m_ReadyFrames` + `++framesAccepted` + `++nextFrame` → `PopReadyFrame` (`:3148-3154`) → `WaitForLockstepControllerFrame` (`:1545-1570`).

`HasCommittedAFrame` is **not** a resume switch. It only widens `IsLockstepHoldingSeatForReclaim` on a resync relaunch before the first commit (`ScenarioRunner.cpp:872-873`).

---

## 2. The seat hold

Two clocks. They are not the same object.

### Coordinator (sim frames)

```550:557:D:\Projects\takeover-build\Source\Network\NetLockstep.h
		// P2's 20 000 ms reclaim window as a count of frames at the pinned timestep (c_DefaultDeltaTimeS
		// = 0.0166666 s, so 20 000 / 16.6666 = 1200). A frame, never a clock: every peer must reach the
		// same answer at the same tick, and only the tick is shared.
		static constexpr uint64_t c_ReclaimHoldFrames = 1200;
		bool IsSeatHeldForReclaimAtFrame(uint64_t frame) const;
```

```4148:4158:D:\Projects\takeover-build\Source\Network\NetLockstep.cpp
	bool NetLockstepCoordinator::IsSeatHeldForReclaimAtFrame(uint64_t frame) const {
		if (m_State != NetLockstepState::Running) {
			return false;
		}
		for (uint8_t peerId: m_DroppedSeats) {
			const auto left = m_PeerLeaveFrames.find(peerId);
			if (left != m_PeerLeaveFrames.end() && frame < left->second + c_ReclaimHoldFrames) {
				return true;
			}
		}
		return false;
	}
```

Only **`m_DroppedSeats`** (unannounced leaves). Announced leave never enters this set (`ApplyPeerLeave` `:4186-4188`).

`RefreshLeftSeatHolds` (`:4137-4145`): `nextFrame < leave + 1200` **and** `SeatStateOf(...).heldForReclaim` (host admission). Clients' `QuerySeatState` returns default (`heldForReclaim=false`, `NetMatchService.cpp:1315-1317`). Derived hold for the activity gate is the **frame deadline on the leave notice**, not the admission plane (`NetLockstepSelfTest.cpp:2471-2474`).

`IsHoldingSeatForReclaim` (`:4165-4168`): `Running` and **every** remote has left and `AnyLeftSeatHeld()`. Used for **ownership** (host plays units), not for `EndActivity`.

### Admission (wall-clock)

```45:53:D:\Projects\takeover-build\Source\Network\NetReconnectSession.h
	class NetAdmissionClock {
		void Start(uint64_t steadyNowMs);
		uint64_t NowMs(uint64_t steadyNowMs) const { return m_Started && steadyNowMs > m_OriginMs ? steadyNowMs - m_OriginMs : 0; }
```

`c_ProvisionalExpiryMs = 20000` (`NetReconnectSession.h:244`). Service: `m_AdmissionClock.Start(SteadyNowMs())` (`NetMatchService.cpp:1139`), `AdmissionNowMs()` `:660-661`.

Hold start (`NotifyDisconnect`, live match, `:1210-1225`): `dropped=true`, `droppedAtMs=m_NowMs`, `holdExpired=false`, `RecordDrop` at `m_LockstepFrame` (pumped as `coordinator->GetStats().nextFrame`, `NetMatchService.cpp:843`).

Hold end:

| Event | Where | Effect |
|---|---|---|
| Reclaim accepted | `BindIncarnation` `:618-633` | `dropped=false`, `holdExpired=false`; `IssueReseat` `:552` |
| Substitution committed | `:1137-1167` | `committed=true`, `closed=false`, `BindIncarnation`, `IssueReseat` |
| Wall expiry | `Tick` `:1326-1330` | `nowMs - droppedAtMs > 20000` → `holdExpired=true` |
| Frame expiry | `IsSeatHeldForReclaimAtFrame` | `frame >= leave + 1200` |
| Clean leave | `HandleLeaveRequest` `:577-588` | `closed=true`, `dropped=false`, ledger cleared; **no hold** |

`IsSeatHeldForReclaim` (admission) `:1335-1338`: `committed && !closed && !holdExpired`. After wall expiry the seat stays reclaimable; the **round** is no longer kept alive (`:1324-1325`). Ticket store age is **24 h** (`NetReconnectTicketStore.h:43`), not 20 s. Challenge lifetime 10 s (`NetReconnectAdmission.h:59`). UX resume window = 20 s (`NetReconnectUx.h:41`).

### Sim gates

```865:873:D:\Projects\takeover-build\Source\System\ScenarioRunner.cpp
	bool ScenarioRunner::IsLockstepHoldingSeatForReclaim() {
		return s_LockstepCoordinator->IsSeatHeldForReclaimAtFrame(s_LockstepAppliedFrame) ||
		       (s_LockstepCoordinator->IsRunning() && !s_LockstepCoordinator->HasCommittedAFrame());
	}
```

`ActivityMan::EndActivity` `:1013-1024`: if `ScriptedEndIsDeferred(scripted, Running, IsLockstepHoldingSeatForReclaim())` (`ActivityMan.h:247-249` = all three true) it prints `"NETWORK: Holding the match open for a player who can still return"` and **returns without `End()`**. Engine teardown is not deferred.

`IsLockstepActorOwnerGone` (`ScenarioRunner.cpp:876-887`) → `IsActorOwnerGone` (`NetLockstep.cpp:3210-3220`): owner gone **and** no surviving human on the team **and** `!IsHoldingSeatForReclaim()`. With a survivor on another team, units **stand down**. With nobody left and the seat held, the **host** plays them (`ResolveActorOwner` `:3186-3191`). Apply site: `MovableMan.cpp:4324-4329`.

---

## 3. Resync on reclaim / substitution

`PumpSessionEvents` (`NetMatchService.cpp:825-890`):

1. `PumpSeatPresence` / host `TickAdmissionPlane(AdmissionNowMs())`.
2. `InjectEvent` for coordinator-sunk transport events.
3. `DriveAutoSubstitution`, `TakePendingReseats` → `EnqueueLocalGameCommand(NetGameReseat)`.
4. If a session-Ready transport is **not** a lockstep remote (`:882-887`):

```882:887:D:\Projects\takeover-build\Source\Network\NetMatchService.cpp
		if (m_IsHost && m_ResyncOnDesync && m_Coordinator && m_Coordinator->IsRunning()) {
			for (const NetSessionPeerInfo& peer: m_Session->GetReadyPeers()) {
				if (!m_Coordinator->UsesTransportPeer(peer.transportPeerId)) {
					std::cout << "[net-match] rejoin: " << ... << " reconnected - resyncing the match" << std::endl;
					m_Coordinator->RequestResync("player rejoined");
```

`RequestResync` (`NetLockstep.cpp:3095-3113`): stop frame is **`m_Stats.nextFrame`** (first uncommitted), or deferred to `FinishSimulationTick`. That `nextFrame` has already walked past the drop because frames kept committing.

`Main.cpp:2182-2224`: on `ResyncRequested` / `Desync` → `ResyncMatch` → wait `ConsumeReadyToLaunch` (60 s) → `StageResyncedMatchLaunch` → `RestartActivity`.

`ResyncMatch` (`NetMatchService.cpp:196-275`):

- Host `SaveCurrentGame` **now** (`:221`), then `CaptureNetResyncState(GetSimUpdateCount(), …)` (`:246`).
- `runner->SetStartFrame(GetSimUpdateCount() + 1)` (`:275`).
- Comment `:198-199`: snapshot is the **live** match at the request, not the drop.

`StageResyncedMatchLaunch` `:356-395`: `LoadGameToRestart`; restore requires `GetSimUpdateCount() == state->savedTick` (`:388`).

`CaptureNetResyncState` (`ScenarioRunner.cpp:1111-1116`): `savedTick` is the argument (current sim count). Restore checks `savedTick + 1 == startFrame` (`:1178-1179`).

**What must hold for snapshot == held frame:** `GetSimUpdateCount()` / `s_LockstepAppliedFrame` / `m_Stats.nextFrame` must still be the drop's `firstFrameWithout` (or the last committed frame before it) when `RequestResync` / `SaveCurrentGame` run. That is true only if **`AdvanceReadyFrames` produces no commits after `ApplyPeerLeave`**. Today it does (`:4219`).

---

## 4. Consumers of "frames keep flowing during a hold"

### Engine

- **`IsRemoteRequiredForFrame` + `AdvanceReadyFrames`**: leave removes the requirement; commits continue. This is the pause site.
- **`WaitForLockstepControllerFrame`**: overlay at 1.5 s; fail at `timeoutMs+50` (**20050 ms** production). A 20 s pause **equals** this budget. Overlay text: `"Waiting for " + who + "... Ns"` / `"The match ends in Ns if they do not return"` (`ScenarioRunner.cpp:150-153`).
- **`AdvanceReadyFrames` `MissingFrameTimeout`**: if local/remote stay queued (`pending`) and wait ≥ `timeoutMs`, the round **Fails**. A hold that stops commits while `QueueLocalInput` still runs **trips this** unless the wait clock is frozen or the timeout is skipped.
- **`AdjudicateSilentPeers`**: after leave the peer is not required (`:4252`), so silence is not re-adjudicated. A pause that **kept** requiring the seat would call them gone at `timeoutMs/2`.
- **Existing match pause** (`ApplyLockstepPauseCommand` `:811-824`, `RunLockstepPausedTick` `MovableMan.cpp:765-780`): freezes **TimerMan** but still **queues and waits a lockstep frame** and applies commands. Opposite of a hold-pause (lockstep stops, wall continues). Wired from `NetGamePauseMatch` / `Main.cpp:2393-2404`.
- **Input delay**: `WaitForLockstepControllerFrame` `:1516-1519` free-runs `tick < effectiveStartFrame` with an empty ready frame. `QueueLockstepLocalControllerFrames` `:961` targets `tick + D`. A stopped commit pipeline fills delay slots and then blocks on `WaitForLockstepControllerFrame`.
- **Fake-lag**: `-net-fake-lag` → `GnsTransport::SetSimulatedLagMs` (`Main.cpp:727-728`). Transport delay only. No hold awareness.
- **Local prediction**: skipped when `IsLockstepPaused()` (`LocalPrediction.cpp:72`). Hold-pause is not that flag today.

### `NetLockstepSelfTest` (oracles that encode "play on")

| Test | File:lines | Old-semantics oracle |
|---|---|---|
| `TestCoordinatorPeerLeave` | `2059-2158` | After clean leave, frames 2–3 commit **without** B; `hostRemotesByFrame[2] == 1` |
| `TestActivityGateAgreesAcrossPeers` | `2487-2623` | Samples `leave`, `leave+600`, `leave+1199`, `leave+1200`, `leave+1800`; deferral iff `frame < deadline` |
| `TestAnnouncedLeaveHoldsNothing` | `3304-3390` | Announced leave → gate false at leave frame |
| `TestCoordinatorHeldSeatWithASurvivor` (B1) | `3398-3513` | Held at `leaveFrame`, not at `leave+1200`; `ScriptedEndIsDeferred` true then false |
| `TestCoordinatorDroppedSeatHold` | `3518-3703` | `"Whatever the host decided, it must be able to keep producing frames on its own"` (`:3593`); `framesAlone == 0` is failure (`:3638-3640`); unheld 1v1 must **not** produce (`:3666-3668`); announced 1v1 ends at once (`:3671-3682`) |
| `TestCoordinatorAdjudicatedPeerKeepsItsSeat` | `3710-3763` | After last remote gone, host must commit **+4** frames (`:3747`) |
| `TestCoordinatorHeldSeatKeepsPlaying` | `3769-3887` | Host takes units; they are **not** stood down while held |
| `TestHeldSeatOwnershipAgreesAcrossPeers` | `3943-4120` | 20 000 ms injected-clock sample of ownership (`:4034`); stayer must **not** commit after host hold (`:4114-4118`) |
| `TestCoordinatorMissingFrameTimeout` | `1779-1813` | `timeoutMs=40` → `MissingFrameTimeout` |
| `TestSessionPumpRunsWhileTheRoundWaits` | `5096-5157` | Wait pumps admission; `timeoutMs=200` |

### Admission selftests (wall-clock hold; round still "play on")

`NetReconnectSessionSelfTest.cpp`: `TestSeatHoldWindow` `:2707-2789` (held for 20 000 ms, not one ms past; clean leave closes immediately); `TestAdmissionClockIsElapsedTime` `:4925-4993` (double-pump / stall / pause must not move P2); `TestComposedSeatHoldMeetsP2` `:5034+`; `TestModerationViewAgesTheDrop` `:3181-3220` (`holdFramesRemaining == 0` on the plane alone).

### H4 gate drivers (`reviews/claude-review-2026-09-08/lanes/source33-lanes/driver/h4gates/`)

| Driver | Tick / time assumptions |
|---|---|
| `common.py:135-144` | `wait_in_match`: `lobby_snapshot: state=Running` within **240 s**, then `settle_s=4` (default) |
| `clean_leave.py` | `TICKS=900`; comment `:124` leave at tick **300** (~6 s); 20 s sleep was "after the match" |
| `reclaim_socket.py` | `TICKS=900`; drop after `wait_in_match`; 1.0 s then relaunch |
| `rejoin_after_resync.py` | `TICKS=900`; same wait shape |
| `fencing_two_transports.py` | `TICKS=1200` (= `c_ReclaimHoldFrames`); `TIMEOUT_S=480` |
| `peers_3_4_regression.py` | `TICKS=600` |
| `crash_relaunch_provisional.py` | `TICKS=900`; arm B `PAST_PROVISIONAL_S=26.0` (past 20 s P2) |
| `old_wire_fixture.py` / `gns_provider_smoke.py` | `TICKS=600` |

These drivers assume the match **keeps Running and consuming ticks** after a drop (900/1200-tick caps, mid-match drop then more ticks). A pause that stops `nextFrame` changes when `-net-match-ticks` fires (`Main.cpp:3072`) and when `wait_in_match` still sees `Running`.

### A7 (`reviews/takeover-20260909/a7-repaired-driver/a7_driver.py`)

```17:27:D:\Projects\reviews\takeover-20260909\a7-repaired-driver\a7_driver.py
ARMS = {
    'stagger_seat_survives': {..., 'frames': 900, ...},
    'silent_socket_bound': {..., 'frames': 12000, ...},
    'leave_ack_dropped': {..., 'frames': 900, ...},
    'slow_resync_save': {..., 'frames': 12000, ...},
    'coop_hand_back': {..., 'frames': 12000, ...},
}
P14_MS, P21_MS, HOLD_MS = 5000, 2000, 20000
```

`compare_progress` (`:276-300`): consecutive applied-frame hashes; `matched_frame_count = len(overlap)`; requires ≥61 consecutive; last applied `+1 == lockstep next_frame`. `leave_ack_dropped` (`:358-360`): `next_frame == leave_begin.frame + 1` and `running_ticks + 1 == frames_accepted`. Reclaim arms launch `-net-match-ticks 12000`. Journal `seq:12002` on host `progress` events is the **event ordinal**, not a frame oracle; write-ups that say "12,002 matched frames" (e.g. `b2-mac-verification.md`) count **overlap length** after resync (example: frames 282–12283). A pause that stops commits during the hold changes pre-resync `frames_accepted` / `running_ticks` and the overlap start.

### B1

`TestCoordinatorHeldSeatWithASurvivor` comment `:3393-3397`: "A scripted outcome the absent player produced must wait for that player, or the activity ends and the resync snapshot has no running game to save. The hold is a frame deadline off the relayed leave notice." That is **EndActivity deferral while sim advances**, not a lockstep pause.

---

## 5. Presentation hooks

| Surface | Path:line | Today |
|---|---|---|
| Console hold | `ActivityMan.cpp:1022` | `"NETWORK: Holding the match open for a player who can still return"` (once per deferred end) |
| Stall overlay | `ScenarioRunner.cpp:137-161` | `"Waiting for <name>... Ns"` + grace countdown; **missing-frame stall**, not hold. Enabled interactive (`Main.cpp:4219`) |
| Stall cout | `ScenarioRunner.cpp:1586` | `waiting on peer frames` |
| F6 panel title | `NetModerationGUI.cpp:122` | `"SEATS  /  The match continues while this panel is open"` |
| F6 seat line | `:144-146` | `"Disconnected  /  away Ns  /  sim hold Ns"` via `HoldSeconds(holdFramesRemaining)` |
| F6 roster strip | `:195-198` | `"Seats  [F6]"` + `statusLine` |
| Moderation describe | `NetReconnectUx.cpp:164-174` | `"dropped Ns ago"` + `"hold Nf (Ns)"` / `"hold over"` |
| Client roster line | `NetSeatPresence::Line` `:331-343` | `"<name>: disconnected - round hold Ns"` (`HoldWallSecondsRemaining`) |
| Reconnect UX | `NetReconnectUx.cpp:148-152` | `" - Disconnected"` / `" - Reconnecting"` on the **dropped client's** UI |
| F6 key | `UInputMan.cpp:1205`, `Main.cpp:1621` | toggles panel |

`holdFramesRemaining` is filled only on the host from **applied frame vs `leave+1200`** (`NetMatchService.cpp:812-815`). Wall remaining is `holdUntilMs` (admission clock). A pause that stops `appliedFrame` freezes **sim hold seconds** at 20; wall line can still count down.

Banner attach points: `DrawLockstepStallOverlay` (already a full-screen wait), `NetModerationGUI::DrawRoster` / `Refresh` detail line, `NetSeatPresence::Line` (client statusLine).

---

## 6. Clean leave (P21 / P22)

**Admission (closes seat immediately):**

- Client `LeaveMatch` (`NetMatchService.cpp:604-629`): if ticket + live link, `LeaveWorkerMain` → `RunCleanLeave` (`:441-476`) → `BeginLeave` → `NetH4LeaveRequest`.
- P21: `c_LeaveAckBudgetMs = 2000` (`NetReconnectSession.h:527-529`). No ack → ticket kept (`NotifyAmbiguousLoss`).
- Host `HandleLeaveRequest` (`:555-588`): live match → `closed=true`, `committed=false`, `dropped=false`, ledger cleared, `LeaveAck`. **No** `droppedAtMs` / hold.
- Then `coordinator->Leave(reason)` (`LeaveWorkerMain` `:482-483`).

**Lockstep (announced):**

- `Leave` (`NetLockstep.cpp:3069-3092`): sends `NetLockstepStop` `PeerLeft`, `frame = lastQueued+1`. Host with >1 remote `Complete`s instead (`:3074-3076`).
- `HandleStop` `:4051-4054`: `ApplyPeerLeave(..., announced = (reason == PeerLeft))`.
- Announced → **not** in `m_DroppedSeats` → `IsSeatHeldForReclaimAtFrame` false → `TestAnnouncedLeaveHoldsNothing`.
- 1v1 announced: `ApplyPeerLeave` `:4213-4217` → `Stopped` immediately even if admission would hold.

**P22:** `EndAdmissionSession` (`NetMatchService.cpp:426-430`) / `EndHostedSession`: hosted-session-end is the only event besides `LeaveAck` that may delete the ticket (`NetReconnectSession.h:561-562`).

Wire: `LeaveRequest=18`, `LeaveAck=19` (`NetProtocol.h:40-41`) vs lockstep `PeerLeft=7` / `PeerDropped=9` (`NetLockstep.h:45-46`). Clean leave can keep closing the seat immediately: admission already does; lockstep already omits `m_DroppedSeats`.

---

## Hook points

Minimal functions where a pause state must be introduced. Current behavior only.

1. **`NetLockstepCoordinator::AdvanceReadyFrames`** `NetLockstep.cpp:4303-4403` — commits `nextFrame` when required remotes are in; after a drop the leaver is not required, so commits continue; `MissingFrameTimeout` at `timeoutMs` if `pending` and wait expires.
2. **`NetLockstepCoordinator::ApplyPeerLeave`** `:4182-4219` — records leave, relays `PeerLeft`/`PeerDropped`, then **`AdvanceReadyFrames`**. Unannounced → `m_DroppedSeats`. Announced + last remote → `Stopped`.
3. **`NetLockstepCoordinator::IsRemoteRequiredForFrame`** `:3273-3279` — `frame >= leave` ⇒ not required. This is why frames flow.
4. **`NetLockstepCoordinator::IsSeatHeldForReclaimAtFrame`** `:4148-4158` — `frame < leave+1200` for `m_DroppedSeats`. If sim stops, this stays true until someone advances `frame` or the predicate moves to wall-clock.
5. **`NetLockstepCoordinator::RefreshLeftSeatHolds` / `EndRoundIfNobodyIsComingBack` / `IsHoldingSeatForReclaim`** `:4137-4177` — 1v1 hold keeps `Running` until admission `heldForReclaim` clears **or** `nextFrame` reaches `leave+1200`.
6. **`ScenarioRunner::WaitForLockstepControllerFrame`** `:1506-1599` — blocks the sim thread; overlay 1.5 s; fail `timeoutMs+50`; pumps admission every 15 ms (`:1537-1542`). A 20 s pause hits the give-up. Stall overlay would fire unless gated.
7. **`NetReconnectHost::Tick` hold expiry** `:1326-1330` — wall-clock `c_ProvisionalExpiryMs`. This is the clock that can keep counting while sim time is stopped (`TestAdmissionClockIsElapsedTime` already requires that).
8. **`NetMatchService::ResyncMatch` + `RequestResync`** `NetMatchService.cpp:196-275`, `:882-887`; `NetLockstep.cpp:3095-3107` — snapshot / stop frame = **current** `GetSimUpdateCount()` / `nextFrame`. Equal to the held frame only if (1) has stopped.
9. **`NetMatchService::PumpSessionEvents` + `QuerySeatState`** `:825-890`, `:1308-1320` — admission + reseat + rejoin detect; host-only `heldForReclaim`.
10. **`ActivityMan::EndActivity`** `:1013-1024` — defers scripted over; does not stop ticks.
11. **`MovableMan` lockstep apply** `:4284-4333` and **`RunLockstepPausedTick`** `:765-780` — every sim tick waits a committed frame; existing P-pause still commits.
12. **Presentation** — `DrawLockstepStallOverlay` `:137-161`; `NetModerationGUI::Refresh` `:122-146`; `NetSeatPresence::Line` `:331-343`; `ActivityMan.cpp:1022`.

Hold-timer split: **admission wall-clock already counts during stalls/pauses**; **coordinator hold is a frame count** and would freeze if commits stop. Expiry that should drop the seat and resume is `holdExpired` (`:1327`) plus `EndRoundIfNobodyIsComingBack` / stand-down when `IsSeatHeldForReclaimAtFrame` becomes false.

---

## Tests / gates whose oracles encode the old semantics

**Must change or grow if frames stop during a hold:**

- `NetLockstepSelfTest`: `TestCoordinatorDroppedSeatHold` (`framesAlone`), `TestCoordinatorAdjudicatedPeerKeepsItsSeat` (+4 frames after last remote), `TestCoordinatorHeldSeatKeepsPlaying`, `TestActivityGateAgreesAcrossPeers` (mid-window / +1800 frame samples), `TestCoordinatorHeldSeatWithASurvivor` (deadline is a **frame**), `TestHeldSeatOwnershipAgreesAcrossPeers` (20 s of `Tick` while `nextFrame` is the sample key), `TestCoordinatorPeerLeave` (clean leave still advances — keep if announced leave stays unpaused).
- `NetReconnectSessionSelfTest`: `TestSeatHoldWindow`, `TestAdmissionClockIsElapsedTime`, `TestComposedSeatHoldMeetsP2` (P2 = 20 s **elapsed**, independent of sim). Frame-hold UI tests that assume `HoldSeconds(1200)==20` (`:3292-3293`).
- H4: `h4gates/clean_leave.py` (tick 300 / 900), `reclaim_socket.py` (900 ticks after mid-match drop), `rejoin_after_resync.py`, `fencing_two_transports.py` (1200 ticks), `crash_relaunch_provisional.py` (26 s past P2), `peers_3_4_regression.py` (600).
- A7: `a7_driver.py` `frames: 900|12000`, `HOLD_MS=20000`, `compare_progress` consecutive-frame overlap, `leave_ack_dropped` `next_frame == leave_begin.frame+1`.

**Can stay if announced leave still closes immediately:** `TestAnnouncedLeaveHoldsNothing`, `HandleLeaveRequest` close-on-leave tests, A7 `leave_ack_dropped` announced path, H4 `clean_leave.py` arm "clean".
