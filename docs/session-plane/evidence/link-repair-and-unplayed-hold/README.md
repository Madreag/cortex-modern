# Link repair and unplayed held inputs

Complete gate pending. All receipts are headed Mac native runs through the
official runner, topology single-box, proof=false. The [index](receipts.json)
retains complete portable compressed stdout/stderr and original byte hashes.

Both native negatives are RED on `b477faca` and GREEN on `06817972`.

Receipt repair had added the gameplay hold budget to its transport clock. On a
22 ms link it therefore waited 72 ms before repairing a missing acknowledgement,
past the link-derived three-tick input queue. Repair now uses the measured RTT
and the link estimator's p95 envelope, with an ordinary-tick minimum. Missing
transport measurements retain the conservative existing fallback. The detector
rejects premature repair and premature repetition, and requires repair at the
measured envelope for both steady and jittered links. No hold bound or input
delay changes. The unchanged saved seed 2325 is now GREEN, including its strict
history and original automatic-return deadline.

A held reader could already have prepared its own human inputs past a hold it
had not displayed. Accepting that certificate left the old ready suffix queued.
The accepted hold now invalidates only that unplayed suffix, including its
prepared prefixes, and resumes certified tail preparation at the hold frame.
The native detector preserves the preceding ready prefix, completed history and
an already granted preceding tick. The unchanged two-player internet main-loop
hitch is GREEN: hold **362**, return **585**, independent keepalives **156**,
no host change, no survivor rewind.

All eight focused native selections pass. The unchanged steady 250 ms run,
three/eighteen-second hung-host controls and all five full direct split/heal
controls also pass. The decreases remain exactly two ticks, zero holds and
**22 / 0 / 0 ms** waits. The broader catalogue, remaining native fixture
disagreements and B3 remain open; the complete-gate report is untouched.
