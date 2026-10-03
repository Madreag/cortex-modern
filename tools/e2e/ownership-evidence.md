Returning-player and accepted-applicant checks require native ownership evidence; `service=Running` alone does not prove control.

The reducer consumes the returned process's `events.jsonl` stream. A committed `ownership_reclaim` row names `round`, `peer`,
`incarnation`, `stable_seat`, `actor`, `owner_peer`, the newly accepted `ticket_incarnation`, `seat_incarnation`, `activation_tick`,
`tick` and `committed=true`. The owner must be this peer and the accepted ticket incarnation must equal the committed seat
incarnation. Tick is the actual activation frame. The writer must derive these fields from the committed seat/actor authority
and accepted ticket, never from a requested seat or a scripted expectation.

The same process must then emit the existing `recovery/first_controllable_input` receipt for that actor, round and seat incarnation.
The receipt must show a controllable actor, no hold/catch-up, a matching wire apply tick and a queue-confirmed fresh controller
produced at/after activation. Every ownership receipt needs its own matching fresh input. Malformed or missing evidence fails.

Engine work remains: the plain e2e return/application path does not currently emit the complete ownership receipt or arm the
cross recovery writer. These are required native instrumentation changes before the engineer's return and moderation runs can
pass. The tools arm the record stream, enforce the contract and retain the evidence; they do not synthesize ownership.
