The jitter, reorder and duplicate arms require native packet-effect evidence. The existing GNS configuration setters do not
publish affected-packet counts; setting a knob cannot satisfy this pin. Until the engine emits these measurements the pin fails.

The tools consume a native stdout line `[net-fake-link] <JSON>` within its owning `net-lockstep start round` context. Its object
names `round` and `peer`, the configured `jitter_ms`, `reorder_percent`, `dup_percent`, and measured positive `jitter_packets`,
`reordered_packets`, `duplicated_packets` for each requested effect. No count is inferred from command arguments or RTT.
Every requested effect on every affected peer and round must have a positive count. The counters must measure packet effects,
not merely successful configuration calls.

The same arm requires the existing native `[net-match] delay change peer=... frame=... delay=... revision=...` records,
bound to the round on both peers. Their live effective-frame decisions must agree exactly. Timing retains every existing bar.
