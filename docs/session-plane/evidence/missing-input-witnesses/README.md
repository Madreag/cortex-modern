# Missing inputs and incompatible witnesses

The original loss schedules 80 and 106 blocked for 51 ms. Each missing
input was already accepted elsewhere: the proposed local survivor group
included a witness that had receipted that input. The conflict guard
correctly rejected that witness's contradictory hold, but the group was
never narrowed to a compatible continuation.

The receipt group now excludes other witnesses known to have accepted a
locally absent, required input. The absent owner is excluded by the hold
itself. Supplied accepted sources retain their existing receipt rules;
input authority, the conflict guard and the actual deadline are unchanged.

The six-arm native detector covers every local seat in direct/directory
play. Refined detector `d6919591` is RED; `cc67d093` is GREEN, including
the contradictory-witness rejection, no early hold, exact 50 ms local
hold, own human input, empty held inputs and preceding-prefix checks.
The initial detector's receipt-mask assertion was refined to distinguish
the absent owner from its other witnesses; both RED receipts are kept.

All 13 focused native controls and complete native lockstep/world-join
2/2 pass. The 102-percent four-seat return, 250 ms three-seat image,
steady 250 ms/120 s, accepted-source slow-receipt and original seed 197
controls remain GREEN.

Original 80 installs its hold at 6755 ms; original 106 at 4451 ms. Both
now remain RED at their first different committed histories, frames 385
and 239 respectively. Their lossy links are not classified as physical
cuts by the current fixture oracle. That contract/return investigation
remains open: no history property changed here. The different-donor row
also retains its unchanged frame-1694 RED; no wider score is claimed.

Twenty-seven complete headed [receipts](receipts.json) retain the original
traces, both detector REDS and final checks, single-box, proof=false. No
bound, quorum, wire, fixture property or SP path changed. Wider groups,
B3 and #45/#46 remain open; final report untouched.
