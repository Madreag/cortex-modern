# Concurrent holds at one frame

The original simultaneous 70-percent joiner schedule waits 51 ms at frame
358. Seats 2 and 3 both volunteer that cutoff, but the decision table's
single `{frame, hold}` key retains only the first owner's promise. Once that
hold certifies, it also prevents the survivor from installing the other one.

The native detector is RED on `a6381d32` and GREEN on `d2a698a8` after hold
identity includes its named-seat set. Independent same-frame holds compose;
each retains its actual owner and survivor signatures, both AI handoffs land
at the named frame, and the preceding history stays immutable. Input fences,
conflict resolution, pending decisions and retained tail proofs use that same
identity. Return/delay/administration domains and wire encoding stay unchanged.

The complete original simultaneous-slowdown schedule is GREEN on `d2a698a8`.
Four native deadline/receipt/authority/consent controls and all five direct
split/image controls pass. Complete native lockstep/world-join suites pass
2/2. Existing native inspections follow the new internal keys; no assertion,
bound or history/return property changes.

The host-autosave 51 ms wait and seed-109 pending-decision 51 ms wait remain
RED, with their exact original reasons retained. They are separate roots,
not complete-row successes. No wider catalogue GREEN is claimed here.

Twenty-one complete headed official-runner receipts are indexed in
[receipts.json](receipts.json), single-box, proof=false. Wider engine rules,
B3, #45/#46 and the final gate remain open. No single-player change; final
report untouched.
