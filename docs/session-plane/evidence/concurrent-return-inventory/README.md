# Concurrent-return immutable inventory

Binary `8aeb250c` passes seven of nine network rows and the complete
world-join suite. The [inventory](inventory.json) combines eight network
rows with the earlier same-binary native lockstep/world-join collection.
All ten complete headed [receipts](receipts.json) are single-box,
proof=false. No row timed out.

Fixed catalogue: 170/240 GREEN, all 108 saved schedules executed.
Random schedules: 91/200 GREEN, all 200 executed. Concurrent image jobs
expose return-certificate replay-boundary, healthy-observation rehold and
member-Start failures that the earlier serialized fixture concealed;
the lower fixed score is retained without changing any property.
Original seed 80 is now GREEN; connected-history, return, consumer-bound
and phase failures remain in the [complete groups](fixed-failure-groups.txt).

The original seed-1020 return remains 8110 ms against 8082 ms. The pause
policy still waits 51 ms against 50 ms. B3, #45/#46 and the final gate
remain open; this inventory makes no completion claim.
