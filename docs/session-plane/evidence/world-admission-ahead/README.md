# A world seat admitted ahead is not held before its admission frame (#45)

Linux (GCC, v6 network library), headed engines through the POSIX runner, topology single-box, proof=false.

Shape: `tools/measure_checkpoint_unscheduled.py --case world-join` - a fresh persistent host with capacity 3, Fixed 0,
autosave 0, 12000 ticks; a bootstrap client plays 600 ticks and retires; the host warms to 1800; a fresh late client
with a new identity and no ticket joins.

- RED, binary f2202df1 (source 731854009): `wj45-base-1-summary.json`. The host holds the admitted seat for silence one
  tick after the activation is agreed (`base-red-lines.txt`: `played=0 ... last_heard_ms=0 cause=disconnect`), which
  removes its transport binding; 585 of the joiner's live starts are then dropped `bound=none` and the joiner stays at
  3270.
- GREEN, binary 727c0cd5 (source 8003e33cd): `wj45-fix-2-summary.json`, `wj45-fix-3-summary.json` pass; `wj45-fix-1`
  completed the join with every join check true and was judged before the checker correction below.
- Native row `world_admission_ahead_owes_no_input` (net-lockstep-seat-succession): RED on 2ee0b09c, GREEN on 727c0cd5
  (`native-row-red-green.txt`).

Checker correction (flagged): `no_capture_refusals` failed on the catching-up joiner's
`[autosave] named tick=2276 not taken: catch_up=true` - a capture named with writers=1 that the joiner is not a writer
of. It now fails on a live peer's refusal (`catch_up=false`) or any `[autosave] failed`. The base run stays RED on
`no_driver_failure` either way.
