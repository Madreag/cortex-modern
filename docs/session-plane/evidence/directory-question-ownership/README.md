# Directory question ownership

Nested split questions shared one mutable asynchronous directory slot.
Between exchanges, polling another split discarded the previous reply
and retry/acknowledgment state. Seed 109's host exhausted all 30 requests
without receiving a ruling, so its image return never began.

The production detector is RED on `973f7934`, GREEN on `b20b8587`. Each
split now owns its request, reply, retry and acknowledged deadline state.
One HTTP exchange services eligible questions in rotation; a waiting read
cannot starve a different check-in. A healed reader still retires its
failed check-in without joining a worker. Rematches and newly listed
matches retire the old scope while retaining the per-minute allowance.

FLAGGED fixture departure: the asynchronous fixture follows the same
question ownership. Its old two-question assertion required discarding
an authenticated answer and making a third exchange; it now requires
both separate answers after their own two exchanges, with no inheritance
or early reply. A new row retains an undecided query's clock while a later
check-in completes, then receives two different immutable rulings.

The original seed 109 is GREEN through both image returns. The host uses
three requests and first receives a decision 291 ms after its first
question, compared with 30 requests and no decision before the fix. Eight
directory fixture controls, complete native lockstep/world-join 2/2, all
five direct split/image controls, the native production detector and the
[Python directory service checks](directory-service-tests.log.gz) pass.

Eighteen complete headed [receipts](receipts.json), single-box, proof=false,
retain RED and GREEN. No bound, rate allowance, authentication, quorum,
wire encoding or SP change. The wider catalogue is not re-scored on this
candidate; connected-history, return/phase and other pending-decision
groups remain open, then B3 and #45/#46. Final report untouched.
