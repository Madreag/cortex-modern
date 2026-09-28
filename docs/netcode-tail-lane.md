# The committed tail on its own lane

A held seat that returns in place replays the round's committed frames from the state it kept, then takes its seat back at an
agreed activation frame. This page covers two things: how the host sends those frames (the tail), and how it picks the
activation frame. Code: `NetWorldJoinHost::NextTailDatagram`, `AcknowledgeTailDatagrams` and `NoteCatchUpProgress` in
`Source/Network/NetWorldJoin.cpp`; `SendWorldJoinTailTo`, `StepWorldJoinCatchUpClient` and `ApplyWorldJoinReport` in
`Source/Network/NetMatchService.cpp`.

## The lane

`NetTransportLane::BulkUnreliable` (`NetTransport.h`, value 3) is unreliable and unordered. GNS sends it with
`k_nSteamNetworkingSend_Unreliable`, which queues the message behind the connection's send rate. The input lane instead
uses `UnreliableNoDelay`, which drops a message that cannot leave at once. A lost tail datagram delays only the frames it
carries; nothing waits behind it.

- Host to returner: the tail datagrams.
- Returner to host: the returner's periodic progress report. Each report carries the whole replay state (applied through,
  ticks replayed, elapsed time), so the newest one wins and a lost one costs nothing.
- Everything else stays on the ordered control lane: the first catch-up report, the activation, the handover and the
  round-ended records.

On the host, the lobby accepts a state chunk off the ordered lane only when it is a catch-up report
(`c_NetWorldReportTransferId` with kind `c_NetWorldReportCatchUp`). On a joiner, it accepts one only when it is a tail
chunk (`c_NetWorldTailTransferId`).

## The datagram

One `NetLobbyStateChunk` with `transferId = c_NetWorldTailTransferId`, `chunkIndex 0` of `chunkCount 1`. Its bytes are:

| bytes | field |
|---|---|
| 8 | the round id, little-endian (`c_NetWorldTailRoundBytes`): a chunk of another round is dropped |
| repeated | `u32` frame length, little-endian, then one whole committed frame, as the lockstep encodes it |

The host packs up to `c_NetWorldTailDatagramFrames` = 4 consecutive frames. It stops adding frames before the payload
passes `c_NetWorldTailDatagramBytes` = 1000 bytes, but a datagram always carries at least one frame.

A frame larger than `c_NetWorldTailDatagramFrameLimit` goes on the ordered chunk path instead
(`NextTailChunk`, the lobby's reliable lane, one piece per pump). The limit is `NetLobbyProtocol::c_MaxStateChunkBytes`
(48 KiB) minus the round and the length prefix. While that stream is pending, the datagrams wait behind it.

The returner keeps a frame only when it is past what it has applied and it does not already hold that frame. Repeats and
late copies are counted and dropped (`kept=`/`repeated=` on its progress line).

## Sending and resending

Each pump sends up to 32 datagrams per returner. Resends come first, then new frames:

- **Repeat:** every datagram goes a second time `c_NetWorldTailRepeatMs` = 20 ms after its first send.
- **Resend:** after that, a datagram the returner has not yet passed goes again once
  `1.5 x RTT + 40 ms` has passed since its last send. The resend applies only to the lowest `c_NetWorldTailResendDepth` = 2
  datagrams in flight: only those hold the replay up, and the ones above them most likely arrived already.
  - RTT is the link's own round trip to the returner as its transport measures it (`NoteTailLinkRtt`, from
    `GetPeerPingMs`). A report passes a datagram only once the replay has applied it, which a returner far behind does long
    after it arrived, so the report's timing is used only while the transport has no reading: then RTT is the shortest time
    from a datagram's first send to the progress report that passes it.
  - Until an RTT is known, the resend interval is 1000 ms.
- **Acknowledgement:** a progress report pops every in-flight datagram whose last frame is at or below the report's applied
  frame.
- **Limits:** at most `c_NetWorldTailInFlightLimit` = 8192 datagrams are in flight, so a replay far behind does not hold new
  frames back. Each pump looks only at the lowest two and at the newest not yet sent twice. A datagram whose frames the
  host's record no longer keeps is dropped, and the returner's reports say what it still lacks.
- **Handover to the round:** frames from the activation frame on reach the returner through the round itself, never through
  the tail.

The host logs the tail's traffic every 2 s:

```
[net-world] tail peer= new= repeat= resend= bytes= refused= in_flight= delivered= acknowledged= rtt_ms= link_rtt_ms= resend_ms=
```

## Activation

The host measures the returner against the round in frames, never on a wall clock: a loaded machine slows the round too.

- Every `c_NetWorldClosingWindowFrames` = 20 horizon frames, the host computes the closing rate:
  `(applied gained - horizon gained) / horizon gained`.
- `behind` = the round's horizon minus the returner's applied frame.
- The lead is `c_NetWorldActivationLeadFrames` = 60 frames.

The host checks these gates in order. The first one that applies decides:

1. **`link-fit`:** the link does not fit the round's delay, so the host waits. It prints
   `activation waits peer=N: its link does not fit the round's delay (replay ratio R)` once.
2. **`announced`:** an activation is already announced.
3. **`no-progress`:** the applied frame did not move for `c_NetWorldNoProgressWindows` = 3 windows (60 horizon frames). The
   returner stays held; the host prints `activation waits peer=N: its replay made no progress for three windows (behind B
   frames, closing rate C)` once.
4. **`closing-losing`:** behind > 15 frames (a quarter of the lead) and the closing rate is below -0.1. The returner stays
   held; the host prints `activation waits peer=N: its replay falls behind the round (behind B frames, closing rate C)` once.
5. **`replay-slow`:** more than a lead behind, the returner's measured replay rate (ticks replayed x tick length / work time) is
   below 1.0: it cannot play the round at its rate, so every frame it is required for after its return would reach the survivors
   late. At the head of its tail the tail's arrival paces it and this gate does not apply. The returner stays held;
   the host prints `activation waits peer=N: its replay runs slower than the round (...)` once.
6. **`outside-lead`:** behind more than the lead, unless the returner is at the round's pace (measured closing rate
   <= 0.1) and at most two leads (120 frames) behind.
7. **Activated.** The activation frame is the larger of horizon + 60, the round's sent-input frame + 1, and the returner's
   own last input + 1. Then, when behind > 15:
   - Rate unmeasured: wait (`closing-unmeasured`).
   - Rate > 0.1: the activation also covers the frames the measured rate needs to reach the horizon, plus the lead:
     `horizon + ceil(behind / rate) + 60`.
   - Otherwise, at the round's pace, the returner is activated with its trail: `activationTrailFrames = behind`. The gate is
     `activated-trailing`. The reclaim's neutral window widens by that trail
     (`NetLockstepCoordinator::SchedulePeerReclaim(..., trailFrames)`: `neutralThroughFrame += trailFrames`), so the host
     does not hold the returner again while it reaches the frame.

The host logs each decision:

```
[net-world] catch-up gate peer= gate= applied= acknowledged= horizon= work_ticks= closing_measured= closing_rate= replay_ratio= replay_ready=
[net-world] activation announced peer= at= applied= horizon= replay_ticks= replay_ms=
```

The returner logs `held client catch-up applied= ... datagrams= kept= repeated= buffered=` every 60 applied frames.
