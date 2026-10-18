# Scheduled image records

FLAGGED fixture departure: every scheduled round retains its committed records. Loss- and slowdown-only schedules can install real holds and need an image just as cut schedules do; their former default discarded those records. The inline schedule path retained them already. Checkpoint commands, deadlines, image verification, history, no-rehold and authority assertions are unchanged.

The two original named detectors fail on executable `2f5761641d9ae2522a8037de745fd76b5dbd253127c5d8fcf260e918d33ba733`: seed 2810 lacks capture 360 at 6602 ms, and seed 69 lacks capture 300 at 6032 ms. On `4914c05b224858eb26c968d22386e9bb53a21ea46d36a2e72436e7e39086c3e9`, both pass capture selection and expose later engine REDS: seed 2810 rejects peer 3's Start at activation 980; seed 69 never completes the held host's return. Neither whole schedule is claimed GREEN.

The existing causal-history, per-half authority, three-seat 250 ms image-return and four-seat 100 ms image-return controls pass 4/4 on the new executable. `receipts.json` retains eight complete headed launches and both output streams, including all later failures. These are single-box detectors with `proof=false`; the complete gate remains open.
