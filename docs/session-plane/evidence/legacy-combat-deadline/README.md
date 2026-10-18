# Legacy combat input deadlines

Dedicated and persistent-world production matches use the coordinator input
path rather than ordinary peer-frame groups. Its missing-input adjudication
discarded the missing clock and required fifteen seconds without authenticated
traffic, leaving an established but stalled human seat outside the combat bound.
The existing combat bridge now uses the actual missing-input deadline for these
agreed Substitute matches. An unstarted stream, a host seat, placement and the
older ordinary coordinator contract keep their existing treatment.

The idle authenticated-silence scanner has no owed-input observation. It now
uses the disconnect path directly, preserving the separate fifteen-second rule
instead of interpreting its stale missing clock as a combat deadline.

The deadline detector is RED on `9cfc483f` and its expanded startup negative is
RED on `533e9ec6`. Final `71f4d6fd` passes both deadline/startup arms for both
modes and the unchanged complete native lockstep and world-join suites, 2/2.
Earlier broad candidates and all their native regressions are retained; those
revisions incorrectly conflated the idle scanner or ordinary legacy contract
with an actual bounded combat wait. Candidate source material is retained
byte for byte in the compressed source index.

The original dedicated and persistent-world rows pass the former 51 ms wait
failure, then remain RED because the fixture retained no committed capture at
frame 420. No complete return is claimed. Pause remains an unchanged 51 ms RED;
that older policy's separate contract is not changed here. The fixture capture
and mode-specific image return are next. No bound, history predicate or
single-player path changed.

Twenty complete headed official-runner receipts are in the
[index](receipts.json), single-box, proof=false. Wider catalogue/schedule REDS,
B3, #45/#46 and the final gate remain open; final report untouched.
