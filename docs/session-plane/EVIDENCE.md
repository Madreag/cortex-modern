# Session-plane evidence

The records below preserve individual failing and passing observations. A green
coordinator detector does not make its complete match green. The [completed
validation](evidence/kept-b41-20261016/gate.json) of engine source `51b578e56`
is RED: suite 48/54, 3218 failures in 5000 schedules, and 63 GREEN / 58 RED of
121 listed shapes. Every schedule ran, seeds 1 through 5000, on the same frozen
executable. The earlier complete gate under `evidence/20261016` used `ec5d90c835`.
All engine observations in this lane are headed Linux runs through the official
runners. No spread match has run in this lane.

## Field A

[The 33 sampled frames](evidence/20261016/field-A-frames.json) retain the six samples
at the two earlier clean host AI boundaries and every frame 57165–57191. Every
component hash agrees through 57175. Both peers report seat 1 under AI at 57175.
Only `attachables` first differs at 57176; controller, routing, actor, terrain and
RNG hashes still agree. Both report the desync at 57180. The announced return at
57209 is beyond both saved histories. There is no brain loss in this evidence.

At 57176 the host attachables hash is
`c8f730837bd4ea0143d779060d55515e1d8bf5ace4428e2e2b5e4a353a1b9069`;
the joiner's is
`a7c1cf4f2f3237653744d2ef6ce262cb493054587e8af23cf34c6e01f0e5ab47`.
Those one-way hashes do not name an object, its parent or its applied input.
The reported AI state is not a trace of the human/AI producer actually applied.
The same one-frame reporting offset occurs at the earlier clean boundaries.
Why the third hold diverges, and its actual attachable UID and producer, remain
unproved. The saved checkpoint-saver timing is correlation, not that root cause.

`host_hold_ai_authority_is_the_certified_frames` detects a related rule defect:
the certified hold's AI frame must be authoritative before local simulation
staging installs its hold transaction. Commit `524085ec6` makes that read use the
certified peer bridge. The detector is RED on exactly `7b589c18e` plus its function
and hooks, and GREEN after the correction. Its [RED/GREEN records](evidence/20261016/detectors.json)
explicitly do not claim the field object/producer root. The latest A-authority
preflight is [GREEN on the kept executable](evidence/C-final-preflight/A-authority.json).
The exact missing field data and the lead's runnable relay commands are in
[NEEDS TESTING](NEEDS-TESTING.md).

## Link delay and presentation

Commit `8238e31c7` corrects the sizing path through
`NetLobbySession::SampleInputDelays`, `NetInputDelayEstimator::SenderRequiredFrames`,
`NetMatchRunner::Start` and `TickPeerInputDelay`. On the field build, startup
measured 58 ms, then added `max(200 ms, jitter) + ceil(4 * 16.6667 ms) = 267 ms`:
`ceil((58 + 267) / 16.6667) = 20 ticks`. Live receipt p95 67–85 ms plus the same
margin gives 21–22 ticks. The field's settled transport RTT is 29–35 ms.

The kept startup rule is `ceil(RTT p95 / tick) + 1`; the live rule is the maximum
of `ceil(receipt p95 / tick)` and `ceil(transport RTT / tick) + 1`. Thus 30 ms needs
3 ticks, 58 ms needs 5, and the observed 67–85 ms receipts need 5–6. Receipt p95
already includes receipt-pump scheduling and jitter. A returning reader's private
trail belongs in its temporary signed neutral interval, not permanent delay.
The owner-approved design embedded in the supplied rulebook states
`delay = ceil(RTT / tick) + 1 + margin(p95 jitter over ~5 s)`.
There is no separate `docs/ROLLBACK.md` in this tree. The original detecting row
and its RED/GREEN records are included in [detectors.json](evidence/20261016/detectors.json).

Commits `6fd44a821`, `a391ace03` and `51b578e56` keep the overlay, hold notification,
private catch-up headline, documentation and probe on the sentence
“The AI is playing for you until you are back”. Proposal, immediate/deferred
vote, certificate installation, certified tail and accepted held-start-record
paths name their evidence in diagnostics. [The kept C preflight](evidence/C-final-preflight/C-ui.json)
is green; actual drawn spread evidence remains outstanding.

## Other detecting corrections

The first thirteen detector pairs are indexed in
[the earlier detector manifest](evidence/20261016/detectors.json). Newer pairs
have their own compact directories. None of this lane's detecting functions or
timing oracles was relaxed between its RED and GREEN. Fixture-owned source was
left to its lane apart from the explicitly authorized A function/hooks.

Checker changes are explicit: `573f4d3c1` changes the inherited match fallback
expectation for 120 ms from 24 ticks to 9, retaining the unmeasured one-tick case
and the 800 ms/36-tick ceiling. Its timed native fault self-test calls an internal
operation so the mandatory visible run still checks invalid bounds, real native
configuration readback and expiry; the public headless guard remains. The own-hold
visible-text probe now requires the documentation's exact “until you are back”
phrase. Fixture-lane merges separate the fixed catalogue from the random driver
and reuse a fresh same-binary fixed failure index during minimization; the random
schedule oracles still all run. Both catalogue and random failures remain RED.

| Correction | Engine commit(s) | Detecting evidence and retained negative |
| --- | --- | --- |
| Preserve each seat's own hold and ownership history across other returns | `a743b7e5a`, `9d2d66a06` | Earlier manifest: another seat stays held; removed ownership is not revived |
| Retain pending authority through local clock expiry | `273c24f98` | Earlier manifest: elapsed time grants no installation authority |
| Authenticate established member starts and retry the exact roster removal | `d6b8d0d33` | Earlier manifest: forged member/start delay remains refused |
| Commit future departure metadata only at its boundary | `2a7952194` | Earlier manifest: equal prefixes before 366; removal present at 366 |
| Measure sending rate between packet arrivals | `11c6171e5` | Earlier manifest: silence is not production time; 40 tick/s, stale and reversed-clock negatives remain |
| Preserve one return promise in both voting paths | `47aa7cc39` | Earlier manifest: both an earlier and later second activation remain refused |
| Guard private capture's wire/admission contract | `434ccdda7` | [capture-contract](evidence/capture-contract): valid-HMAC version 6 refused; migration 7/admission 51, frame 40/protocol 8 unchanged |
| Exclude only certified neutral seats from decision voters | `1907d03d2` | [neutral-gap-voters](evidence/neutral-gap-voters): seat 1 is required again immediately after 4490 |
| Preserve a newer certified hold when delivering an older return | `f34243824` | [delivered-return-hold](evidence/delivered-return-hold): hold 4460 remains before its AI event 4580 despite delivering return 4400 |
| Pad only owed live input after a return delay rise | `127c326d0` | [remaining-safety](evidence/remaining-safety): owed 534–537 present; pre-admission input 479 still refused |
| Repeat only this receiver's accepted receipt watermark | `a920a0ece` | Same directory: dropped own receipt is repaired; foreign watermark 99 is not adopted |
| Query the assigned referee half after voter normalization | `cfbad37d0` | Same directory: neutral requester queries; real playing requester still checks in |

The capture contract's [164-reference census](evidence/capture-contract/encoder-census-after.json)
names every found encoder/decoder and admission stamp. The named
`lead-tools/find_encoders.py` is absent locally and did not run.

Commit `4c87aa917` retains the existing bounded catch-up budget through the certified
neutral interval and sizes that temporary interval from the measured surplus.
[The complete before/after rows](evidence/return-neutral-runway/outcomes.json) add
two GREEN whole rows: `joiner_catching_up_at_105_percent_returns_over_100ms` and
`three_seats_held_host_catches_up_then_runs_at_round_rate_over_100ms`.
The four-player rows still fail their initial hold bounds. The stricter readiness
trial added no whole-row GREEN and was [reverted with both measurements retained](evidence/return-readiness-trial).

## Held-host capture and transport

The [native host handoff investigation](evidence/held-host-controller/README.md)
records an additional current-binary desync: disabled flags first differ at727,
then the Brain Robot's front arm/rifle at728. The new actual-handoff detector is
RED before its correction. Field logs identify why the third hold differs in
return-record arrival order; they still do not identify the exact field object.

Commits `c9ccc3d76` and `23dd1a907` let the host whose own seat is held request a
private base. Its authenticated request is embedded in a future capture proposal
from the already agreed AI producer; all actual playing voters certify the same
frame. The request changes neither roster authority nor the public capture
cadence. Its dedicated detector is indexed in the earlier manifest and the
[kept preflight is GREEN](evidence/C-final-preflight/held-host-base-capture.json).
The complete held-host image row still fails a 66 ms startup wait; it is RED.
Cold multi-seat image return without retained canonical member-start terms remains
unproved.

Commits `05dd30663`, `3afeb9691` and `937405f17` add native packet send/receive
timestamps including keepalives, route, queue availability, send outcomes and close
reason, plus TURN allocation/refresh/permission request/result timestamps. The
[private dependency patch check](evidence/20261016/native-dependency-apply.json)
and [native direct transport observations](evidence/20261016/native-transport.json)
are local evidence. The native concurrency check sends 20,000 **messages**, not
20,000 played ticks. Actual TURN refreshes, a 5003 drop and the MSVC/Mac builds
still require the lead's runs.

The [signed directory capacity regression](evidence/20261016/directory-capacity.json)
remains RED: HTTP 503 at 13129 decisions / 2097084 bytes. Server persistence and
deployment are outside this engine/client lane. The direct-host partition row
remains reachable and RED; its expected held host conflicts with the existing
no-referee host-half continuation rule. No winner or authority check was changed.
Native B1 live rise / B2 cap control (2026-10-16): [live-rise-and-cap-one.json](evidence/feel-20261016/live-rise-and-cap-one.json) records three completed headed two-engine cases on2c224533. The30→200ms link really changes inside GNS at outgoing600; both observers apply each delay at the same frame679/710/830, with1200 equal hashes. With the private legacy preview cap set to1, both peers still preview their whole4–5/15-tick delays. Transition waits and missing response edges are retained; this is not a B3 or overall zero-survivor-wait pass.


The 2026-10-17 brief explicitly includes the full directory record. The [directory history correction](evidence/directory-history/README.md) makes that original signed capacity fixture GREEN locally, preserving its property and all retained decisions. It supersedes the capacity verdict above; deployment and spread proof remain unperformed.
