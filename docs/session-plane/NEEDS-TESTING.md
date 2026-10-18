# NEEDS TESTING

These multi-machine runs belong to the lead. The current lane measurements use
the assigned Mac only, engine source `3e35b69c437a6f34d7673c820c04db38f4ccc5c3`,
including `origin/cm-dev` `d1cbc8e09a` and fixture `c05a4ea84`. The executable
SHA-256 is `9cd877ab2482d62db341c646b45ded0e4a6b2d589b8ef5272426bc0bccda44a1`.
The [Mac gate index](evidence/mac-gate/README.md) records the complete local
detectors and their failures. A local native pair has topology single-box and
proof=false for a real relay game. Earlier Linux receipts remain historical.
Each peer must build the same source and the private patched native dependency.
The lead supplies the box facts, assigns free boxes and records their build receipts.
Every run below needs a quiet interval without another engine or build on its peers.
Linux and Mac players have visible windows; a Windows host uses its assigned player
session and the lead's approved headed/headless policy. No box is assigned here.

WHAT IT PROVES: a real relay host's own hold and return, the drawn own-hold sentence,
trigger records, input delay and equal per-tick histories; peers: Windows/MSVC host
and Mac/GCC joiner, 960x540, quiet. Exact command:
`python3 tools/e2e_peers.py --scenario fight15-field-held-seat --repo <REPO> --out <FRESH_OUT> --peer-boxes host=<WINDOWS_HOST_BOX>,joiner=<MAC_JOINER_BOX> --boxes-file <BOX_FACTS> --engine-env CC_TEST_RETURN_INPUT_DETAILS=1`
The scripted stop is at simulation tick 6000 for 2000 milliseconds. It is not a
stop six seconds after launch. Retain the `h15-own-hold-line` screen evidence and
the complete hold/proposal/certificate/return records from both peers. The required
sentence is “The AI is playing for you until you are back”. Record the actual RTT,
receipt p95 and delay throughout; a measured 30 ms link must remain at six ticks or
fewer, with no survivor stalls during the host's slow-machine holds.

WHAT IT PROVES: three-player relay fight, automatic holds/returns and equal histories
through the complete fight; peers: Windows/MSVC host, Mac/GCC joiner1 and Linux/GCC
joiner2, 960x540, quiet. Exact command:
`python3 tools/e2e_peers.py --scenario fight15-field-observer-b --repo <REPO> --out <FRESH_OUT> --peer-boxes host=<WINDOWS_HOST_BOX>,joiner1=<MAC_JOINER_BOX>,joiner2=<LINUX_JOINER_BOX> --boxes-file <BOX_FACTS> --engine-env CC_TEST_RETURN_INPUT_DETAILS=1`

WHAT IT PROVES: four-player relay fight, every held seat's independent return and
equal histories; peers: Windows/MSVC host, Mac/GCC joiner1, Linux/GCC joiner2 and a
second Windows/MSVC joiner3, 960x540, quiet. Exact command:
`python3 tools/e2e_peers.py --scenario fight15-field-observer-a --repo <REPO> --out <FRESH_OUT> --peer-boxes host=<WINDOWS_HOST_BOX>,joiner1=<MAC_JOINER_BOX>,joiner2=<LINUX_JOINER_BOX>,joiner3=<WINDOWS_JOINER_BOX> --boxes-file <BOX_FACTS> --engine-env CC_TEST_RETURN_INPUT_DETAILS=1`

These three commands passed the runner's local scene validation without launching
an engine. They do not add actual 200 ms network latency, repeated host stops,
simultaneous returners or a private-image restart. The legacy scenes
`mp-host-stall-3p-200`, `mp-host-restall`, `mp-two-returners-3p` and
`mp-host-stall-image-rejoin` are refused by the current public-directory runner.
The lead/harness lane needs committed public relay variants for those cases before
they can be named as executable spread tests. A path-RTT measurement override does
not create network latency. Preserve the existing bounds and failure oracles.

Field A still needs the two exact-round files
`match-6004495977551122484-r2423981022284705150.ccreplay` and their startup/checkpoint
images, or both peers' object and controller application traces for ticks
57174–57177 (UID, parent UID, position/rotation, actual producer and applied input).
The supplied component hashes cannot recover those fields. Commit `7ad1e02ef`
closes a demonstrated controller-disable divergence: learning a future return
used to change the held host's controller state at the current frame. The actual
actor/controller detector is RED on `7b589c18e` and GREEN on the tip; the native
host-stall reproduction has 1,200 equal hashes and identical handoff object dumps.
The field's exact attachable UID remains unknown. See
[the controller evidence](evidence/held-host-controller/README.md).

Native diagnostics need a real TURN run long enough to observe allocation and
permission refreshes, followed by a transport timeout/close with both ends' packet
timestamps, route, native refresh result and close reason. The local native test
uses DIRECT and cannot prove TURN behavior. Build the pinned native dependency in
a fresh private checkout with `external/patches/build_gns_turn.py`; its build record
must contain `transport_diagnostics_version=1`. The patches are not idempotent.
The complete Mac/GCC engine build against the supplied v6 prefix is now measured.
Its native archive and supplied build record were verified; archive SHA-256 is
`4f358a5d55bd28948b13a9e67f2ddb9f1d2494f53f26a3624f9ccb6716cbe4f3`,
with lifetime version 5 and transport diagnostics version 1. Windows/MSVC remains
NEEDS TESTING; a successful Mac build does not prove that platform or real TURN.

The signed directory capacity row is now GREEN locally in `ea64acb81`, retaining
all decisions and the original 2 MiB record/entry bound. The new authenticated
history storage is not deployed; rollout and rollback need matching directory
code and state. The current Mac suite's isolated service checks pass 144/144, and
the unchanged separate signed-capacity fixture passes freshly too;
the live public service was not touched. See
[the storage proof](evidence/directory-history/README.md).

The old pending-directory wait-floor objection is withdrawn by the current
section 11 ruling. Survivors must install their local hold within 50 ms and continue;
the directory arbitrates a split asynchronously. Historical wait-floor receipts
are not a current contract blocker. Current bound, history and automatic-return
failures are individually retained in the [complete Mac catalogue](evidence/mac-gate/catalogue.json)
and [all 200 random schedules](evidence/mac-gate/random.json).

One direct-play contract issue remains: a client observes the same transport
history through its bound when the host loop is fully hung and when the live host
is cut off. The former must hold/migrate; the latter must stop and image-return to
the host side. The [939 equal wire observations](evidence/authority-local-20261017/direct-fault-observations.json)
identify the missing independent liveness distinction. No timeout or property is
waived, and this does not excuse the other engine failures.

The current [matched B3 measurements](evidence/mac-gate/matched-result.json) use
24 single-player controls, 12 per actor team, frozen before six network pairs.
SP/30 ms/200 ms medians are 28.624/28.395/28.075 ms. The measured SP bands are
28.479–30.573 ms for team 0 and 26.168–28.812 ms for team 1. Eight of twelve network
aggregates and 46/60 action comparisons remain outside; all twelve strict B3 rows
are RED. All 32 response edges per steady observation are retained. The
[stage decomposition](evidence/mac-gate/matched-stages.json) localizes a contribution
before the first draw: median press-to-first-draw is 8.53/8.67 ms in SP, 9.92/12.85 ms
at 30 ms and 9.73/8.85 ms at 200 ms (host/client). Draw work adds about 0.35 ms; the
first-to-response draw interval remains about 16.67 ms. The sample phase and movement
response differences still need a causal resolution; larger SP sampling alone
has not made B3 GREEN. Measurement ends at completed UploadFrame, not scanout.

All twelve steady native observations pass B1 delay caps (peaks 4/15 ticks), B2 full
preview depth and B4 live text, with equal 1200-tick paired histories. The
[cap-one live resize](evidence/mac-gate/live-resize.json) applies the same four
owner/frame/delay changes on both peers and finishes with depth 15/effective cap 15.
Its 1400/372 ms maximum waits and six/three missing responses remain RED. The
[own-hang control](evidence/mac-gate/own-hold.json) has 101 active sentence frames,
zero sentence violations, 1200 equal hashes and 44 ms maximum survivor wait. Its
[complete object dumps](evidence/mac-gate/own-handoff.json) agree byte for byte at
710–790. Its held host has 63 generic depth-zero frames and six missing responses;
no whole-control or B3 success is claimed. Independent non-substitute cap 1/cap 20
preview controls each pass 8/8, including successful-depth retention without an actor.

The two headless-policy rows now use CCCP_HEADLESS=1 by default under the explicit
brief authorization. The remaining suite runs visibly through the official runner.
Current counts, exact failures and actual launch modes are in the
[suite receipt](evidence/mac-gate/suite.json) and [launch receipts](evidence/mac-gate/launches.json).
Multi-machine relay play, physical display response, the exact field-A object and
real TURN behavior remain NEEDS TESTING; no off-box run or service rollout is claimed.
