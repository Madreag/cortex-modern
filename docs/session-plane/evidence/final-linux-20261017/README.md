Final Linux evidence, 2026-10-17

Engine source `99dd25fe7`, including cm-dev `d1cbc8e09a` through merge `667b621c0`.
Executable SHA-256: `6cfe0e58f73e0c03ea5ca7bcabd717eeb9ccaac513449452427e4070e990f5fa`.
All 89 current runner receipts are headed and use this executable. The private GNS
v6 prefix contains both required patches; v5 remains installed.

| Gate | Result | Evidence |
|---|---|---|
| Complete suite |49/55; four network rows plus two mode-incompatible headless-policy probes RED|[suite](suite.json)|
| Fixed session-plane catalogue |104/226; all 108 saved schedules ran|[each row and source line](catalogue.json)|
| Random schedules |200 ran; 141 failed an original property|[suite](suite.json)|
| Four reviewer lists |121 covered: 64 GREEN, 57 RED, 0 missing|[mapping](reviewer-shapes.json)|
| Steady delay / preview / overlay |30 ms maximum 5 ticks; 200 ms maximum 16; full preview depth and native counters; no duplicate or delay-line violations|[native matrix](native-input-delay.json)|
| Own-input response |SP/30/200 ms aggregate medians 31.263/30.811/30.802 ms; 10 of 12 network observations outside SP 30.459–31.602 ms; RED|[all repeats](native-input-delay.json)|
| Live resize, cap 1 |Both peers apply (2,675,14),(1,722,14),(1,870,15); full depth and 1,200 equal full hash rows; transition stalls/censored responses remain RED|[resize](live-resize.json)|
| Own host hold |1,200 equal full hash rows; byte-identical object dumps 710–790; 54 active sentence checks; zero duplicate violations|[hold](own-hold.json), [host capture](host-held-frame.png), [client capture](client-held-frame.png)|
| Slow host |Five schedules have zero completed survivor waits; four seats at 200 ms has 1,856, including 486 before the fault, longest 167 ms|[six schedules](slow-host.json)|
| Hold triggers |547 observed installations, every one carries a cause|[audit](hold-trigger-audit.json)|
| Single player |All 1,200 full hashes equal the pre-merge control; reference, preview invariance and joystick updater GREEN|[reference](single-player-reference.json), [suite](suite.json)|

The current first-decision directory contract cannot guarantee the 50 ms survivor
bound in the supplied outage shapes. The exact 100 ms directory / 2 ms peer shape waits 101 ms;
the 300 ms directory / 30 ms peer shape waits 632 ms. Even an immediate fresh answer cannot fit the
latter within the maximum six-tick 100 ms reserve plus 50 ms. [Final repeats and
causal proof](authority-floor.json). This does not excuse other failures.

Named REDs include `a_kept_hold_waits_for_its_own_directory_without_changing_history`
(103 ms), `four_internet_seats_300ms_jitter_40ms_two_return_frames_seed_131` (71 ms),
and `image_return_three_seats_over_250ms_keeps_equal_histories_and_bound` (150 ms).
The native host-hold generic B2 oracle also remains RED for 33 AI-owned frames at
730–817; it incorrectly requires human preview there and has not been weakened.
The own-hang control records a 42 ms survivor wait at 606 and intentionally missing
host inputs. Those controls are not B3 or zero-stall successes.

The original near-2 MiB directory capacity fixture is GREEN in `ea64acb81`, with
all retained decisions and the limit unchanged. [Storage proof](../directory-history/README.md).
The source hashes were reverified while mapping reviewer row `referee:28`.
The storage format was not deployed; rollback requires matching code and state.

A's actual actor/controller detector is RED on 7b589c18e and GREEN here. Closing
commit `7ad1e02ef` preserves held-host controller state before its future return
record arrives. [Detecting pair](../held-host-controller/README.md). The field's
exact attachable UID remains unknown; spread proof belongs to the lead.

[Source and merge audit](source-and-merge.json), [runner receipts](runner-launches.json),
[merged build](../merged-20261017/compile-receipt.json),
[final build](../merged-20261017/preview-report-compile.json).
All observations preserve their original assertions and timing bounds. Raw local
paths and hashes are in the JSON receipts; no spread or physical scanout claim.
