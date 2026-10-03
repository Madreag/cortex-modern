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

The optional preparation is exactly one `silence` on a non-host seat and one
`moderation-ban` by the host targeting a different non-host seat, both before the
kill. No other combined faults are accepted. The silence uses the native
`outage` lever, preserving its schedule id. The engineer must add the scripted
Ban lever to the native cross schedule; it must call the real participant-removal
path and emit the receipts below. Queueing a request does not prove it applied.

The host emits `scheduled_hold` with the silence id, numeric peer, `cause=silent`,
`ai_in_control=true`, committed tick, measured `silence_ms` and `bound_ms`, with
`silence_ms > bound_ms > 0`. The target's own applied `fault` receipt has that id,
`action=outage`, `send_recv_armed=true`, and the requested budget reached.
The host emits `moderation_action` with the Ban id, `action=Ban`, `applied=true`,
target instance and numeric `target_seat`, identity_sha256, budget tick and the
first removal tick. The target emits `moderation_terminal` with the same id and
identity, `result=ParticipantBanned`, `terminal=true`, and `last_live_tick` one
before the removal tick. Its owning runner must finish. Every receipt carries
instance, execution, incarnation, session, match, history branch, source round,
native round and tick. The pre-kill snapshot retains the silent held seat and
the exact banned identity. Only these exact seat/round/tick receipts classify
their hold or ban as SCHEDULED; every other unscheduled hold still fails.

Ban disconnects its target in the production removal path. With this native
proof, HL4 compares all four histories through the target's last live tick,
then the remaining three through the host's observed termination, then the two
survivors through the endpoint. Without a proved Ban, the original four/three
history rule remains. The two-survivor interval retains Q1's two-witness floor;
ordinary cross-history declarations still require three. Full state is compared
in each interval. Survivor feel, pace, measured workload,
memory, quiet collection, four named boxes, source/build identity and record
integrity remain required. No scheduled hold is excused from this arm's zero
unscheduled-hold gate without separate native causal evidence.

`host_loss`, `hl4_passed` and `passed` report the HL4 arm. `v1_passed` continues to
report the ordinary full-workload match/soak predicate: HL4 is an additional
acceptance row and cannot replace the four normal hosted matches. A killed host
is never credited with unmeasured ticks or a normal completion.
