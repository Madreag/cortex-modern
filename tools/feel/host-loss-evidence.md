# Four-box host loss

An explicit match schedule entry `action=host-kill`, targeting the declared host,
terminates that process at its measured budget tick and does not restart it.
The native ticket path uses the recorded address (NetMatchService.cpp,
BuildTicketRejoinRequest), so this action has no endpoint-fix prohibition.
Host restart/rejoin remains a different, unserved variant.

The payload retains `terminations.jsonl` only after finish and poll confirm the
owning PID's exit. Its `actual` record is the last drained native progress record;
execution, incarnation, host identity, generation, session, match and round bind
the loss to that history. A pre-termination intent line cannot satisfy HL4.

The engine must emit `moderation_snapshot` observations through its native cross
record stream. The host's `stage=before` snapshot must be at the terminal applied
frame; each survivor's `stage=after` snapshot follows migration. Each contains
`state.held_seats` (seat and named cause), `state.bans` (stable identity digests),
and `state.tickets` (seat, ticket incarnation and identity_sha256, never secrets).
Exercise all three collections. The complete state must carry unchanged; every
survivor agrees on the new host and generation. Existing source does not yet
emit this complete snapshot: engine instrumentation and HL4 are requested.

HL4 compares all four histories through observed termination, then all three
survivors through the configured endpoint, with full subsystem schemas. Full
state is also compared in both intervals. Survivor feel, pace, measured workload,
memory, quiet collection, four named boxes, source/build identity and record
integrity remain required. No scheduled hold is excused from this arm's zero
unscheduled-hold gate without separate native causal evidence.

`host_loss`, `hl4_passed` and `passed` report the HL4 arm. `v1_passed` continues to
report the ordinary full-workload match/soak predicate: HL4 is an additional
acceptance row and cannot replace the four normal hosted matches. A killed host
is never credited with unmeasured ticks or a normal completion.
