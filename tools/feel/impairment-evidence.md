The jitter, reorder and duplicate arms require native packet-effect evidence. The existing GNS configuration setters do not
publish affected-packet counts; setting a knob cannot satisfy this pin. Until the engine emits these measurements the pin fails.

The tools consume a native stdout line `[net-fake-link] <JSON>` within its owning `net-lockstep start round` context. Its object
names `round` and `peer`, the configured `jitter_ms`, `reorder_percent`, `dup_percent`, and measured positive `jitter_packets`,
`reordered_packets`, `duplicated_packets` for each requested effect. No count is inferred from command arguments or RTT.
Every requested effect on every affected peer and round must have a positive count. The counters must measure packet effects,
not merely successful configuration calls.

The same arm requires the existing native `[net-match] delay change peer=... frame=... delay=... revision=...` records,
bound to the round on both peers. Their live effective-frame decisions must agree exactly. Timing retains every existing bar.

The four-box L4P arm additionally declares `lag_ms=200`, `loss_percent=5`, and
`jitter_ms=60` on its affected peer. Its same-round native receipt must contain
positive `delayed_packets`, `dropped_packets`, and `jitter_packets`; every box
must retain the identical live delay-change decisions. The owning payload's
observed fault-reset interval must contain the requested 120000 ms duration,
and the native recovery must complete within its unchanged deadline.

The acceptance plan requests 12001 measured ticks for L4P and HL4 so the required
120 s memory warm-up is followed by multiple minute samples. The five ordinary
hosted/rendering match rows remain 1201 ticks. L4P applies item 20's independent
feel bars to every survivor, as V1 item 17 specifies. The affected peer still
owes its completed workload, history, full state, memory and native effect proof.
