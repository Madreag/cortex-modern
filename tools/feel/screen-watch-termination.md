# Screen watches at an intentional termination

Every armed watch needs a summary. A scene-killed peer is judged through its
actual recorded termination tick, from `video/injected-drop.json` and the
owning runner's `scenario drop` termination. A requested tick alone is not proof.

The engine must periodically flush each watch and flush again on the kill path.
Each native `[text-watch] summary` retains the existing watch, frames,
active_frames and violations fields and adds `through_tick` and
`flush=periodic` or `flush=kill`. The summary belongs to the most recent arm of
that watch in the owning process log. The review selects the last summary at or
before the termination tick for each arm and reads violation lines through that
tick, including violations after the last periodic summary. An arm without a
summary remains incomplete. The review states `terminated by the scene at tick T`.

Normal exits still require one final summary for each arm. Periodic summaries
do not replace the normal final summary. The current engine's shutdown-only
summary lacks termination coverage; periodic and kill-path flushes are engineer
work, not evidence inferred by the tools.
