# The history oracle keeps a proven split after its reader returns (FLAGGED fixture correction)

`CompareHistories` (Source/Network/NetSessionPlaneSelfTest.cpp) accepts two different displayed histories only where a split
between their authors is proven (a cut, an exact lost input or an exact impaired receipt). Seed 2208: seat 2's links lose 90 %
and the round splits between seat 2 and the host; seat 3, catching up privately, reads the host's half, so its frames are
authored by the host and the pair (2, 3) is accepted from the hold on. Once seat 3 returns into the host's half it authors
its own frames, and the oracle looked for a new proof between seats 2 and 3 at frame 727 (`histories differ at displayed
frame 727 ... authors=2/3`, `runs.txt`). Histories that diverged under a proven split cannot meet again unless an image
return replaces one of them.

Correction: once a pair's divergence is accepted, its later frames stay separated until the pair shows equal histories again
or an image return replaces one side (which already restarts that seat's comparisons). The authors' own seats must still be
in their frames' members. Named row `history_fixture_keeps_a_proven_split_after_its_reader_returns` with three negatives (no
proven split; two seats of one half differing; a frame whose members omit its author). The existing row
`history_fixture_requires_causal_input_wire_evidence` caught a first version that did not end the carry at equal histories;
both pass after.
