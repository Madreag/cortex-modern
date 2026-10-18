# Neutral-receipt immutable inventory

Binary `2f576164` passes seven of nine network rows and the complete
world-join suite. The [inventory](inventory.json) combines eight network
rows with the earlier same-binary native lockstep/world-join collection.
Ten complete headed [receipts](receipts.json), single-box/proof=false.
No row timed out.

Fixed catalogue: 179/240 GREEN, all 108 saved schedules executed, nine
new GREEN rows and no new RED compared with `8aeb250c`. These include the
250 ms/jitter return's former 51 ms overrun, the return under 30 percent
host loss, and the returned-joiner autosaves. Random schedules: 91/200
GREEN, all 200 executed. Their earlier failures remain.

Every remaining fixed failure is retained in the [groups](fixed-failure-groups.txt).
The original seed-1020 return is still 8110 ms against 8082 ms. Pause
explicitly selects an unbounded legacy policy (`UsesBoundedWait` is false);
its bound row remains an open engine contract issue, not a waived check.
Loss and slowdown can also require automatic images: two saved cases still
lack captures because fixture setup predicts them only for selected faults.

Read-only tracing isolates a concurrent Start failure: return 1 at 632 is
omitted by return 2's independently created membership snapshot at 642.
The neutral returner rejects that certificate as foreign, then rejects its
legitimate Start. Capture retention and independent-return membership are
next. #45/#46, B3 and final gate remain open; final report untouched.
