# Causal opposing-half hold

FLAGGED fixture departure: an exact input-loss split can make the opposing
continuation hold a survivor, just as a total cut does. The cause is limited
to the proven impaired hop and its scheduled fault interval, and to a hold
span containing the first differing display. An unrelated seat, link, old
fault, earlier-ended hold or later hold cannot inherit it. All original
recovery deadlines and no-rehold checks remain.

The original detector is the retained seed-80 RED in
[causal-input-history](../causal-input-history/README.md). On `63619402`,
the expanded causal/authority controls pass 2/2 and seed 80 advances to the
unchanged deadline RED: seat 2 returns at 10661 ms against 9842 ms, an
819 ms overrun. Both private image returns occur. The synchronous image
helper is the next investigation; no deadline is increased.

Seed 106 retains its same-group history RED. Its trace shows host frame 242
sent at 4218 ms, first reaching peer 2 at 4401 ms and forwarded to peer 3
at 4501 ms, after peer 3's 4490 ms local hold. No new pacing cause is
claimed from this trace.

Four complete headed Mac [receipts](receipts.json), single-box, proof=false.
No engine, bound, quorum, delay, wire-format or SP change. This is not a
wider inventory or the final gate.
