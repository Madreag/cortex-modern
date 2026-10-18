# Return prefix electorate

`ProposePeerReclaim` selected its common boundary from every connected
member, including a concurrently returning seat inside a certified neutral
interval. The certificate correctly required no vote from that seat. Its
stale cursor therefore selected a boundary older than the private image.
The host-slow original used boundary 344 with capture 900; the lossy-voter
original used 344 with capture 420. Both complete traces are retained.

The four-arm native detector is RED on `428a02f3`, GREEN on `2a219176`.
The proposal now uses `PeerDecisionRequiredVoters` at its activation frame
when selecting the common prefix. Neutral members with or without progress
cannot drag it backwards. A member human again at activation still supplies
its common prefix, and missing progress still refuses the proposal.
Membership, authority, signatures, activation and bounds are unchanged.

Final `2a219176` passes all nine focused native/replay/direct controls and
the complete native lockstep/world-join suites 2/2. Five of ten previously
boundary-blocked original schedules are GREEN, including host slowdown,
host autosaves, held-host followed by a joiner split/stall/slowdown. The
other five advance to healthy-observation reholds, a member-Start mismatch
or a 51 ms wait against 50 ms; these remain RED. The lossy-voter original
now uses boundary 499 and reaches its later rehold at 10801 ms.

The added fixture diagnostic only names the received boundary and private
replay range. No fixture assertion, quorum, bound, delay, wire or SP change.
Twenty-six complete headed Mac [receipts](receipts.json), single-box,
proof=false. No wider inventory or final-gate claim; final report untouched.
