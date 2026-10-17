# NEEDS TESTING

These runs belong to the lead. This lane has used only the assigned Linux box.
The engine source under test is `99dd25fe7`, including `origin/cm-dev`
`d1cbc8e09a` through merge `667b621c0`; the Linux executable SHA-256 is
`6cfe0e58f73e0c03ea5ca7bcabd717eeb9ccaac513449452427e4070e990f5fa`.
Each peer must build the same source and the private patched native dependency.
The lead supplies the box facts, assigns free boxes and records their build receipts.
Every run below needs a quiet interval without another engine or build on its peers.
Linux and Mac players have visible windows; a Windows host uses its assigned player
session and the lead's approved headed/headless policy. No box is assigned here.

WHAT IT PROVES: a real relay host's own hold and return, the drawn own-hold sentence,
trigger records, input delay and equal per-tick histories; peers: Windows/MSVC host
and Mac/GCC joiner, 960x540, quiet. Exact command:
`python3 tools/e2e_peers.py --scenario fight15-s10h --repo <REPO> --out <FRESH_OUT> --peer-boxes host=<WINDOWS_HOST_BOX>,joiner=<MAC_JOINER_BOX> --boxes-file <BOX_FACTS> --engine-env CC_TEST_RETURN_INPUT_DETAILS=1`
The scripted stop is at simulation tick 6000 for 2000 milliseconds. It is not a
stop six seconds after launch. Retain the `h15-own-hold-line` screen evidence and
the complete hold/proposal/certificate/return records from both peers. The required
sentence is “The AI is playing for you until you are back”. Record the actual RTT,
receipt p95 and delay throughout; a measured 30 ms link must remain at six ticks or
fewer, with no survivor stalls during the host's slow-machine holds.

WHAT IT PROVES: three-player relay fight, automatic holds/returns and equal histories
through the complete fight; peers: Windows/MSVC host, Mac/GCC joiner1 and Linux/GCC
joiner2, 960x540, quiet. Exact command:
`python3 tools/e2e_peers.py --scenario fight15-s12 --repo <REPO> --out <FRESH_OUT> --peer-boxes host=<WINDOWS_HOST_BOX>,joiner1=<MAC_JOINER_BOX>,joiner2=<LINUX_JOINER_BOX> --boxes-file <BOX_FACTS> --engine-env CC_TEST_RETURN_INPUT_DETAILS=1`

WHAT IT PROVES: four-player relay fight, every held seat's independent return and
equal histories; peers: Windows/MSVC host, Mac/GCC joiner1, Linux/GCC joiner2 and a
second Windows/MSVC joiner3, 960x540, quiet. Exact command:
`python3 tools/e2e_peers.py --scenario fight15-s11 --repo <REPO> --out <FRESH_OUT> --peer-boxes host=<WINDOWS_HOST_BOX>,joiner1=<MAC_JOINER_BOX>,joiner2=<LINUX_JOINER_BOX>,joiner3=<WINDOWS_JOINER_BOX> --boxes-file <BOX_FACTS> --engine-env CC_TEST_RETURN_INPUT_DETAILS=1`

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
Windows/MSVC and Mac/GCC builds of that dependency remain unmeasured in this lane.

The signed directory capacity row is now GREEN locally in `ea64acb81`, retaining
all decisions and the original 2 MiB record/entry bound. The new authenticated
history storage is not deployed; rollout and rollback need matching directory
code and state. See [the storage proof](evidence/directory-history/README.md).

The unchanged pending-directory survivor bound is still RED. A healthy seat waits
101 ms with a 100 ms directory exchange and 632 ms with a 300 ms exchange. Even
an immediate request cannot fit the latter inside the brief's maximum six-tick
30 ms-link reserve plus the 50 ms survivor bound. The lead needs to resolve this
authority/availability conflict; no property has been waived. See
[the authority proof](evidence/authority-wait-floor/README.md) and
[the final-binary repeats](evidence/final-linux-20261017/authority-floor.json).

The final local B3 measurement remains RED against its unchanged repeat-noise
oracle. SP/30 ms/200 ms aggregate medians are 31.263/30.811/30.802 ms, but ten of
twelve network peer observations lie outside the SP range 30.459–31.602 ms.
The [final quiet matrix](evidence/final-linux-20261017/native-input-delay.json)
contains every peer and repeat; all 32 responses per peer are measured. No loaded
arm was substituted in this final matrix. The earlier measurements remain retained.

The final steady-link observations pass the delay caps (maximum 5/16 ticks at
30/200 ms), complete preview depth, native report counters and visible delay line.
The [live resize](evidence/final-linux-20261017/live-resize.json) applies the same
changes on both peers at frames 675/722/870 and covers the full 14/15-tick delay
with configured cap 1. Its stalls and two undrawn client responses remain RED.
The [own-hold control](evidence/final-linux-20261017/own-hold.json) has 1,200 equal
full hash rows, identical handoff object dumps and 54 actively checked held frames
with no sentence or duplicate-line violations. It also records a 42 ms survivor
wait at frame 606, intentional missing host inputs, and 33 RED generic preview
obligations on AI-held frames 730–817. That generic check incorrectly requires a
human preview while the AI owns the seat; the property was not relaxed.

Five of the six original slow-host schedules pass. Four seats at 200 ms still
hold the healthy host at 1,016 ms, before the fault at 6,000 ms. That startup and
return failure is separate from the proven pending-directory floor.

The direct-host partition rows `two_direct_host_3s` and
`two_host_3s_no_directory_configured` remain RED (2,972 ms survivor waits): their symmetric partition
holds the joiner while the documented no-referee host half continues. The lead must
resolve that row's expected survivor against the existing host-half rule; this lane
does not waive either check. The complete final suite is 49/55, with the four network rows and
`headless-assert-continues` / `headless-render-cap` RED. Rulebook A25 requires every
Linux engine to be visible, so this lane did not use `--headless-policy-rows`.
The lead must route those policy probes to an authorized mode or box; neither
headed refusal was counted as a pass. The other 53 rows were run, with 49 passing.
