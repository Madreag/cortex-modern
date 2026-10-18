# Physical transport RTT in the jitter fixture

FLAGGED fixture departure: injected physical packet jitter previously left
`GetPeerPingMs` constant at the bare link RTT, while claiming a measurement.
The fixture now measures the physical outbound/ACK trip, excluding application
delay, replay/report work, ordered-lane backlog and ambiguous retransmissions.
Reverse ACK jitter uses an independent stream; existing packet/fault draws and
application deliveries are unchanged. RTT samples become visible only after
their trip completes, independently of application polling.

The smoothing follows the linked prefix's pinned GNS revision
`2cb93a06350bb065db53abdb0d87cf297e0bfd34`:
[PingTracker implementation](https://github.com/ValveSoftware/GameNetworkingSockets/blob/2cb93a06350bb065db53abdb0d87cf297e0bfd34/src/steamnetworkingsockets/steamnetworkingsockets_stats.cpp#L74)
(100 ms buckets, the two lowest of three recent samples).

The independent oracle is RED on `77346f98` (constant 100 ms) and GREEN on
`bd62c242` (100–143 ms). Both retain exactly 400 deliveries and the identical
delivery-time digest `14686769955866270130`, with zero application polls.
The unchanged seed-85 no-fault jitter schedule is RED at frame 12 on the
former and GREEN through its complete 30 seconds on the latter.

The complete immutable `bd62c242` inventory has seven of nine network rows
GREEN, plus native world-join GREEN. The complete native lockstep row passes;
the catalogue and schedules remain RED: 174/238 fixed cases, all 108 saved
schedules executed, and 76/200 random schedules GREEN with every seed run.
No harness timeout, history predicate, bound, packet schedule or engine
delay rule changes. The remaining failure groups are retained in full.

Fourteen complete headed official-runner receipts are indexed in
[receipts.json](receipts.json), single-box, proof=false. Wider engine rules,
B3, #45/#46 and final gate remain open; no single-player change and final
report untouched.
