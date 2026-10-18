# Private replay reports its supplied machine capacity

FLAGGED fixture departure: private replay tick credits enforce the named or
scheduled machine rate, but reports previously charged a fixed 1 ms per tick.
That claimed 16.67 times round capacity for a 102-percent machine. Busy work now
accumulates the simulated cost of each replayed tick. The private loop also
honors the named transition to round rate and clears credit while stopped.

The capacity-ceiling detector is RED on `cb3cf4ce`: reported `16.666667`,
supplied `1.020000`. It remains enabled and passes on `f200008c`; the original
102-percent four-seat return passes with measured ratio about 1.02, and the
healed-tie and silent-voter controls remain GREEN. The 250 ms row reports
`1.04993` and still fails no-rehold; the 200 ms row still fails its earlier
admission gate. Neither is claimed fixed by this measurement correction.

Six complete headed official-runner receipts are in the [index](receipts.json),
single-box, proof=false. No engine or single-player change. All original
history, bound, admission, complete-return and no-rehold checks remain.
Wider catalogue/schedules, B3, #45, #46 and the final gate are open.
