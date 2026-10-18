# Healthy return observation and a fresh fault

FLAGGED fixture departure: the original repeated-slowdown row injects a
new 92-percent machine slowdown 200 ms after a successful return, but
the synchronous image helper unconditionally demanded no hold for its
entire next second. On `ef5fbbb7`, return 629 and human input 638 succeed;
the seat then volunteers hold 662 for its measured 55.2 ticks/s. The
helper wrongly rejected that intended second hold.

On `35e9738e`, the helper checks for a rehold on every healthy observation
step and returns to the outer fault/recovery loop when new own-machine
trouble starts. This row observes 165 ms after its handoff, then passes
the full second fault and return. It still preserves every survivor's
displayed prefix, history equality, empty held input and 50 ms bound.
Rows without new trouble keep the complete 1000 ms observation.

Both repeated-slowdown rows, the original 102-percent four-seat return,
250 ms three-seat image return and all five native direct split/image
controls pass. The different-donor row remains RED at frame 1694; the
host-autosave return remains RED at 10586 ms against 10000 ms. Neither
was exempted. No engine, SP or bound change.

Seventeen complete headed [receipts](receipts.json), single-box,
proof=false, include the original RED and five unmatched session-plane
selections (harness errors); those five controls were then correctly run
under native lockstep and passed. Wider groups, B3 and #45/#46 remain
open; final report untouched.
