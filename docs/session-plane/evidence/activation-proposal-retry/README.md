# A private activation can retry its proposal

FLAGGED fixture departure: the image helper treated the first refused reclaim
as fatal and stopped progress reports after its first activation announcement.
The service retains a private bootstrap, continues reports and retries a refused
proposal. The helper now does so, marking the activation proposed when scheduling
succeeds. The engine's future-frame guard, certificate electorate, complete
return, no-rehold, saved-history and survivor-bound assertions are unchanged.

The retained `1b5c07e6` inventory rejects the original 250 ms return at activation
757 with proposer next frame 444, before the unchanged 240-frame guard.
On `f8267eb4` it passes that stage and completes the live handshake, then remains
RED because the returning seat is held again. This is not a complete row fix.
The 102-percent four-seat, healed tie, silent voter and closing-reannouncement
controls pass, as do complete native lockstep and world-join suites, 2/2.
The 200 ms returns still fail their earlier catch-up admission gate.

An initial candidate `408393a3` also imposed the service's near-head notice
window. That exposed the synchronous helper's fixed-donor limitation when a
later fault interrupts the donor, regressing two controls. That restriction
is removed, and its exact rejected patch and complete results are retained.
The helper continues to let the coordinator accept or refuse each proposal;
it does not reproduce the service's separate notice-window scheduling policy.

Fourteen complete headed official-runner receipts are in the [index](receipts.json),
single-box, proof=false. Wider catalogue/schedules, B3, #45, #46 and the final
gate remain open. No engine or single-player change; final report untouched.
