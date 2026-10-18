# Complete Mac gate observations

FINDINGS. The relay-game gate is unmet. These are local detectors on one headed
Mac through the official runner, topology single-box, proof=false for field play.
The two expressly authorized headless-policy rows use their default headless mode.
No bound, assertion, fixture property or timing envelope was loosened.

Engine source: `3e35b69c437a6f34d7673c820c04db38f4ccc5c3`.
Executable SHA-256: `9cd877ab2482d62db341c646b45ded0e4a6b2d589b8ef5272426bc0bccda44a1`.
Fixture `c05a4ea84` and cm-dev `d1cbc8e09a` are ancestors. Later commits contain
measurement tools, evidence and documentation; the engine source stays fixed.
The v6 native archive SHA-256 is
`4f358a5d55bd28948b13a9e67f2ddb9f1d2494f53f26a3624f9ccb6716cbe4f3`,
with lifetime version 5 and transport diagnostics version 1. GNU 13.4, Meson
1.11.1 and Ninja 1.13.2 built the complete release tree with LTO disabled.
[Source, dependency and complete build receipts](source-and-build.json) also
verify that Main.cpp and LocalPrediction.cpp have identical blobs to the takeover
starting point; their single-player presentation code was not edited.

[suite.json](suite.json) retains every one of the 55 suite verdicts: **50 GREEN,
five RED**. The four network rows below and the known headed-Mac `module-version`
timeout are RED. The latter is findings #33 and reached its unchanged 300-second
timeout; that separate harness was not investigated.
[launches.json](launches.json) retains actual launch modes and executable hashes.
The [42 feel/control launch receipts](feel-launches.json) verify the actual headed
mode and configured preview caps. Exact seed settings and original before/after
hashes are retained; network startup's private preference rewrites are reported.
The [log manifest](retained-logs.json) gives original, portable and compressed
SHA-256 values; original local receipts remain under the owned review root.

| Network suite row | Current result |
| --- | --- |
| net-lockstep | RED: seven exact nested assertions |
| net-session-plane | RED: 86/233 fixed cases GREEN |
| net-session-plane-hold-schedules | RED: 48/200 schedules GREEN |
| net-input-acceptance | GREEN |
| net-lockstep-released-claims | GREEN |
| net-lockstep-release-paths | GREEN |
| net-lockstep-seat-succession | RED: TestAnnouncedLeaveLoneOutcome |
| net-lockstep-seat-admission | GREEN |
| net-rejoin-matrix | GREEN |

All 233 fixed cases, all 108 saved schedules and seeds 1–200 ran. The
[fixed catalogue](catalogue.json) retains every verdict and classifies every RED
as an open engine rule, fixture disagreement or direct-play contract blocker.
The [random catalogue](random.json) assigns every failed seed to its exact first
failure group and retains all 48 successful seeds. Existing minimization limits
do not convert an unminimized failure into success. The
[121-shape mapping](reviewer-shapes.json) is 45 GREEN / 76 RED, refreshed from this
fixed catalogue, the random driver and the separately measured signed-capacity
fixture. The latter passed freshly and now resolves its previously unmatched
mapping name; the one-shape increase is an inventory correction, not an engine
gain. The [capacity log](signed-directory-capacity.txt.gz) and
[144/144 isolated service checks](directory-service-tests.txt.gz) are retained.
This mapping is not a claim to have played those field shapes.

[nested-failures.json](nested-failures.json) records each exact named assertion.
The neutral-seat fixture installs a return at 4420 in its live bridge but leaves
the explicit hold history true from 4300, without a release boundary. Its expected
voting members at frame 5000 contradict that history. The pending-directory
assertion still demands the withdrawn wait; the return-trail and thin-lead rows
ask machine time to inflate permanent delay. These remain RED for the fixture
lane. The decrease row also records a real 102 ms wait and stays an open engine
failure even though its padding expectation is obsolete. The direct two-two split
still lets both halves display new history; placement now recovers, but the later
combat history differs at frame 164. Neither is excused by fixture disagreements.

The current ruling is local hold at the 50 ms bound, then asynchronous split
arbitration. The old directory-latency floor is withdrawn. The separate
[direct-host observations](../authority-local-20261017/direct-fault-observations.json)
contain 939 byte-identical client transport observations through the bound for a
fully hung host and a live host cut. Their required actions differ, so independent
liveness or an architectural ruling remains necessary. This is not an excuse for
the many other current bound, split-history or automatic-return failures.

Two engine root corrections were retained during the takeover:

- The split arbitration map kept a stale check-in after the active request had
  become a query. Updating that request preserves its original electorate and
  hold frame. `split_answer_survives_an_unconfirmed_input` is now GREEN on the
  final executable; [RED/GREEN receipt](../mac-takeover/arbitration-request.json).
- The direct-host loss stop ran inside the placement barrier. It now shares the
  placement guard with hold proposals. Startup and placement recover; the full
  detector remains RED at the later frame 164 divergence. See the
  [placement receipt](../mac-takeover/placement-stage.json).

The producer/replay pacing investigation is retained in
[clock-study.json](../mac-takeover/clock-study.json). Removing ordinary transit and
wall-clock extrapolation removed a demonstrated false steady wait and gained
other rows, but regressed a previously GREEN return. That candidate was rejected
and the original pacing restored. Its 104/233 experimental count is not the
current result. Current 86/233 and the 51 ms steady-link failure remain RED.

The GNU precompiled-header root was also corrected: two headers had byte-order
marks before `#pragma once`, defeating GNU's include identity and causing class
redefinitions in Lua translation units. Removing those bytes passes the minimal
detector and complete build with PCH still enabled. The measurement driver now
uses checked POSIX process discovery and the official runner's actual engine PID
for both local launch routes. Its [harness receipt](../mac-takeover/harness.json)
retains direct/SSH runner checks and twelve passing Python tests.

The suite CLI also retained `--repo .` as a relative path, so its reference input
script was interpreted inside the isolated runtime and could not open. Resolving
the repository once at the CLI boundary corrects every derived repository path.
The new file-open regression is RED before and GREEN after; all thirteen combined
Python tests pass. The corrected native SP reference matches all 320 tick hashes,
the complete portable dump and the compared trace digest. The original complete
49/55 collection remains retained alongside this
[same-oracle RED/GREEN receipt](repo-path-reference.json); the fresh complete
collection in suite.json follows that harness correction on the same executable.

| Independent defect | Fresh detecting evidence |
| --- | --- |
| D1 | `missing_or_zero_rtt_never_uses_machine_receipts` GREEN in all four modes; missing readings retain the link-derived delay, an explicitly measured zero stays distinct from missing, and receipts do not size either |
| D2 | `every_mode_sizes_live_and_return_from_the_link` and `input_delay_follows_the_measured_link` GREEN; steady 30/200 ms yields 3/13 ticks in every mode |
| D3 | `link_rise_has_no_resize_cooldown` and `link_fall_requires_five_continuous_measured_seconds` GREEN |
| D4–D5 | Non-substitute preview controls with configured caps 1 and 20 each pass 8/8, including the absent-actor completed-depth detector; [receipts](preview-caps.json) |
| D6 | Fixture supplies both presentation-watch fields; all twelve driver/oracle/inventory tests pass, and the combined path suite passes thirteen; [stdout](repo-path-green.txt.gz) |
| D7 | No lane machine-path or box references in NOTES, session-plane documents or the new reducer; [whole-tree audit](path-audit.json) enumerates 59 generic/vendor/test-negative or substring candidate files. This is not zero literal pattern matches for the whole tree |

All twelve steady network observations pass B1 link caps, B2 full preview coverage
and native counters, and B4 live delay text and duplicate-line checks. Delay peaks
are 4 ticks at 30 ms and 15 at 200 ms. Every pair has 1200 equal full hash rows.
The [cap-one live resize](live-resize.json) raises real native fake lag at frame
600; both transports report the actual 200 ms RTT. Both peers apply exactly
`(seat,frame,ticks) = (2,681,14), (1,712,14), (2,727,15), (1,746,15)`.
Both final native reports show depth 15 and effective cap 15 with configured cap 1.
The abrupt transition still has maximum waits 1400/372 ms, 86/65 generic
depth-zero frames and six/three missing responses. No whole-control pass is claimed.

The [own-host pause](own-hold.json) retains 1200 equal hashes, 101 actively checked
own-hold sentence frames and zero sentence or duplicate violations. The survivor's
longest wait is 44 ms. Every object-dump byte agrees at ticks 710–790;
[complete dump and comparison](own-handoff.json). Its held host still has 63
generic depth-zero frames and six missing responses. Unchanged engine captures
and their hashes are in [captures.json](captures.json); they were visually checked.

B3 stays RED after the predeclared larger SP measurement. The
[plan](matched-plan.json) takes 12 SP controls per actor team before three native
pairs at each RTT, and [freezes the observed bands](matched-frozen-sp-envelope.json)
before any network case. Same fixture, actor team/model, weapon, script, cap and
resolution; no extra tolerance. All 36 peer observations are complete and quiet,
with all 32 responses measured. SP/30 ms/200 ms medians of run/seat medians are
28.624/28.395/28.075 ms. SP bands are 28.479–30.573 ms for team 0 and
26.168–28.812 ms for team 1. Eight of twelve network aggregates and 46/60 action
comparisons fall outside; all twelve strict B3 rows remain RED. Every median is
two drawn frames. [Every measurement and verdict](matched-result.json),
[every input edge and native counter](matched-cases.json.gz).

The [stage reducer](../../../../tools/feel/session_stages.py) reconstructs every
response exactly from press-to-first-draw, first-to-response draw, response draw
and UploadFrame time; [stage records](matched-stages.json). Median press-to-first
draw is 8.53/8.67 ms in SP, 9.92/12.85 ms at 30 ms and 9.73/8.85 ms at 200 ms
(host/client). The first-to-response draw interval stays about 16.67 ms; match
draw work adds about 0.35 ms and upload stays similar. The 30 ms client's sample
phase varies from 0.88–6.88 ms after the previous presentation, against clustered
SP controls around 6.7 ms. That localizes a measured timing contribution before
the first draw. Movement-action response also differs and remains unresolved;
these observations do not establish a noise-only explanation or a B3 fix.
Measurement is software input sample to completed UploadFrame, not physical scanout.

The [movement marker audit](movement-response-audit.json) preserves a further
measurement limit: 114/576 movement detections accept a velocity change no larger
than the separate sustained-response helper's existing 0.01 floor (60 SP, 26 at
30 ms, 28 at 200 ms). The input bit is drawn, but that tiny change alone does not
establish input causality or a new physical response time. No response predicate,
envelope, recorded time or B3 verdict was changed for this diagnostic.

For reproduction, use a fresh owned output directory and the pinned v6 build:

```text
CC_RUNNER_HEADED=1 CCCP_TEST_BINARY=<REPO>/build-gns/CortexCommand python3 tools/run_selftests.py --repo <REPO> --out <FRESH_SUITE_OUT> --timeout 300
CCCP_TEST_BINARY=<REPO>/build-gns/CortexCommand python3 tools/feel/matched_session_delay.py --out <FRESH_FEEL_OUT> --single-player-repeats 12 --network-repeats 3
python3 tools/feel/session_stages.py <FRESH_FEEL_OUT>
```

Use the runner's normal live desktop policy; never launch the engine directly.
Only one feel pair runs at a time, with no build or other engine overlapping it.
[NEEDS TESTING](../../NEEDS-TESTING.md) retains the multi-machine relay commands,
exact missing field-A traces, Windows/MSVC dependency build and real TURN refresh/
close proof. The local directory service tests do not deploy the public service.
