The relative policy consumes native capacity separately for every peer and
round. Each record contains `round_id`, finite positive `local_capacity_tps`
and `sim_tick_ms`. Every peer's tick length must agree. Missing, duplicate or
conflicting records make only that round incomplete.

The reader visits all retained incarnation reports and every nested native
round record. A process that plays several rounds must retain its earlier
round records in its report; writing only its final coordinator cannot prove
the earlier rounds. The engineer must retain these records before coordinator
replacement. The tools do not infer a capacity from the configured tick rate.

Round timing uses the native final boundary, all exclusive compute partitions
and live clock observations in the required steady interval, and that round's
`steady_missing_frame_stalls`. A round-specific `pace.sim_ms_per_tick` can supply
the native compute measurement when the full timing partition is unavailable;
a whole-process average spanning several rounds cannot replace it. The
existing 59.5 TPS, zero steady missing-frame stalls, below 1 percent waiting,
50 ms maximum wait and 50 ms confirmed-horizon limits remain unchanged. The
whole-heavy relative policy needs all peers' round evidence.
