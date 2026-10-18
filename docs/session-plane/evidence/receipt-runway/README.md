# Receipt runway and explicit repairs

Complete gate pending. These are headed Mac native detectors through the official
runner, topology single-box, proof=false. The [receipt index](receipts.json)
retains complete portable compressed stdout/stderr and original byte hashes.

The active-pacing negative on `603b6664` grants extra simulation ticks before a
healthy 250 ms receipt trip can fit. The correction preserves each playing
reader's outstanding receipt runway before spending the existing extra budget;
ordinary simulation still runs at its own pace. Private replay and an ordered
delay decrease keep their separate bounded budgets. The unchanged 120-second
`steady_rtt_250ms_120s` is GREEN on `684aa365`, `93049411`, `1277f1d2` and
`4dcfde54`, without changing its wait, history or no-substitution assertions.

The explicit-tail negative on `5c3a60f4` reproduces a donor timer vetoing an
authenticated missing-record request. The correction honors that request within
the existing work/record/byte caps. Its native positive and negative arms prove
one bounded repair, no unsolicited repeats and no already-applied repeats.

The independent-input negative on `6a6ba82d` reproduces requests and small exact
repairs depending entirely on an ordered lane stalled by unrelated packet loss.
Both keep reliable delivery and also use the independent input lane. Existing
measured request spacing remains. The buffered-input negative on `145b7ece`
then proves that repeating an already queued input omits its peer receipt. The
duplicate branch now republishes its cumulative receipt without advancing
acceptance, duplicating input or changing prepared/displayed history.

All five focused pacing/repair/decrease selections are GREEN on `4dcfde54`.
The three decreases remain exactly two ticks, with zero holds and waits
**22 / 0 / 0 ms**. The full nine network rows are **6/9 GREEN**: input acceptance,
released claims, release paths, succession, admission and rejoin matrix pass.
The fixed catalogue is **101/233 GREEN** with all 108 saved schedules executed.
Random schedules are **46/200 GREEN**, with all 200 executed. Every RED remains
in the full logs; no history predicate or bound is weakened.

The three RED suite rows are lockstep, session-plane and hold-schedules. Native
lockstep retains six named failures: obsolete unsigned-invitation construction,
two-two/isolated-host/late-voter direct expectations, unplayed held-incarnation
human input at 362, and placement-return history at 164. Catalogue/schedule
groups retain split-history, decision-frame, automatic-return, pacing and input
cleanup engine failures. In particular seed 2325 still disagrees at frame 370;
its original late-return failure and every subsequent diagnostic remain here.
This is not a complete pacing or session-plane gate claim.

One mistyped steady-case selection is a failed invocation without a property
verdict; its corrected, exactly named selection is GREEN. No complete-gate
report has been replaced.
