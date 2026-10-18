# Certified neutral slots in cumulative input receipts

The concurrent return in the autosave original left peer 3's receipt of
peer 1 stuck at frame 504. Frames 504–517 were certified neutral and had
no human samples. Real samples from 518 onwards accumulated ahead of that
permanent gap. Once peer 2's next autosave required a hold, the stale receipt
removed a healthy sender from acceptance and caused the 51 ms failure.

The six-arm native detector is RED on `6b0e3d51`, GREEN on `2f576164`.
`NotePeerInputAccepted` now walks the contiguous receipt through actual
input or an existing certified neutral slot. Active and retired gaps work;
missing human input before or after a gap still stops progress. An absent
gap or another seat's gap grants no receipt. Out-of-order delivery retains
its ordinary ordering. No timeout, inferred silence or new receipt source.

Final `2f576164` passes all 11 focused receipt, electorate and direct split
controls, plus complete native lockstep/world-join suites 2/2. The original
returned-joiner autosave passes all three autosaves: both survivors resume
frame 594 after 33 ms, without holding each other. The refereed two-two split
with a second drop is also GREEN, as are both basic three/four-seat image
controls. Lossy-voter, silent-voter and stalled-lane image returns retain
their later healthy-observation rehold REDs.

Twenty-three complete headed Mac [receipts](receipts.json), single-box,
proof=false. No fixture predicate, bound, quorum, delay, wire or SP change.
The wider catalogue, #45/#46, B3 and final gate remain open; report untouched.
