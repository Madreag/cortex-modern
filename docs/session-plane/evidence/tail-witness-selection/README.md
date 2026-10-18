# Witness selection for a committed tail

The ordinary tail sender included every retained witness for a frame, including authentic witnesses naming another continuation. The strict receiver then rejected the entire record for a mismatching group, digest or prepared prefix. Retention and quorum were correct; selecting the record's certificate at the sender was missing.

Expanded nine-role native detector is RED on `6154dd1a` when a prior authenticated witness names group 6 instead of the record's group 7. It is GREEN on `761a7498ec57e467a01b2e99755565e62010ce72ad35d00329fe63cf36f14240`, including same-group wrong-record and wrong-prepared-prefix witnesses. A neutral member serves the actual committed record through the ordinary sender, and a held reader must still confirm it. The five malformed/forged/duplicate-body negatives retain their strict parser checks and import no partial evidence.

The sender now includes only seated witnesses matching the served record's group, frame, committed digest and prepared prefix. Foreign evidence remains available locally; the receiver, quorum, protocol layout and frame bytes do not change. No fixture or single-player change.

Final binary passes four focused native controls, all five direct split/image controls, three provenance/history/authority controls and complete native lockstep/world-join 2/2. The complete native suite explicitly passes `applied_inputs_preserve_the_agreed_prefix_and_tail_witness`. One focused launch used nonexistent selector `applied-input-prefix`; its complete failure is retained as an invalid verification attempt, not an engine defect or a passing control.

The original private-catch-up schedule remains GREEN. Lossy/silent-voter and stalled-lane reholds, consecutive-split private replay, seed 2810's missing member handshake and seed 69's held host remain RED, with complete failures retained. No wider score is claimed. `receipts.json` retains 23 complete headed launches and both streams, single-box/proof=false. Remaining catalogue/schedule groups, #45/#46, B3 last and final gate are open; final report untouched.
