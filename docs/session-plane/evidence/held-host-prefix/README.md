# A held host keeps its committed replay boundary

Live peer-framed history records the held host's leave boundary in its exact
applied prefix. Playback omitted that boundary specifically for the host, so
an otherwise correct returning host could not adopt the survivor certificate.
Playback now retains it as it already did for other held seats. No image or
wire encoding changes, no prefix-field removal and no single-player change.

The corrected native detector is RED on `27586413`, GREEN on `b2f9f295`.
It compares exact leave facts, departed seats and complete applied prefixes,
for host and non-host holds, at the hold and following frame. Both original
host-spike rows reproduce the prefix failure on `945e6784`; both are GREEN
on `b2f9f295`, together with the joiner spike pair, healed tie, silent voter
and different-donor return. Complete native lockstep and world-join pass 2/2.

The first detector setup wrongly put a local host command on its remote side,
where the live encoder correctly omitted it. That detector's REDS on
`945e6784` and `5940153a` are retained as setup errors, not causal evidence.
The latter candidate already passes both original spike rows. The command
placement was corrected, the engine change removed, and the valid RED/GREEN
pair above rerun before accepting this fix.

All seventeen complete headed receipts and hashes are in the [index](receipts.json),
official runner, single-box, proof=false. The wider catalogue/schedule gate,
B3, #45 and #46 remain open. REPORT-final.txt is unchanged.
