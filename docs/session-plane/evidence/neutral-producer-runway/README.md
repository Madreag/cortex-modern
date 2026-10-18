# Neutral replay aligns the returning input producer

Measured replay capacity now caps the closing projection: a burst of delivered
tail records cannot imply more computation than the reader supplied. A reader
with sustained spare work can spend a bounded neutral interval aligning its
producer before its first human target. The existing 240-frame future guard,
link-derived permanent delay, survivor bound and authority quorums remain.

The 105-percent 200/250 ms detectors are RED on `a3282ee9`, GREEN on
`b011c3ab`; the over-cap negative stays GREEN. The burst-projection detector is
RED on `4b4a4f64`, GREEN on `b011c3ab`. Original four-seat 200/250 ms returns,
105-percent 200 ms and 102-percent four-seat controls pass. Their complete
history, no-rehold, return and 50 ms bound checks remain enabled.

FLAGGED native-fixture correction: five first-input negatives supplied 4x CPU
work while requiring refusal for lack of alignment capacity. Their complete
suite RED on `b011c3ab` is retained. They now supply 1x work and still require
refusal at the same cursor/link values; explicit 4x positive arms retain the
old work samples and require a sufficient neutral interval. The complete
native lockstep and world-join suites pass on `da7fdccb`.

Complete inventory: 7/9 network rows plus world-join GREEN, 172/237 fixed
(all 108 saved schedules), 73/200 random (all 200 executed), no harness timeout.
Every RED remains in the complete logs and portable inventory. The different-
donor round-rate row remains RED: with the strict absolute 18-second rate
transition, the prior engine `62905c90` never announces activation; `da7fdccb`
reaches human frame 1694 with residual producer lag and forks. The timer and
all assertions are unchanged. Admission must handle loss of spare capacity
before alignment finishes; that engine rule remains open. Relay setup still
measures 33 ms against its 60 ms lever; legacy bounds, other history and
automatic-return groups also remain open, followed by B3, #45 and #46.

Twenty-four complete headed official-runner receipts are in the
[index](receipts.json), single-box, proof=false. No single-player change.
The complete gate is not met; the final report remains untouched.
