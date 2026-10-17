# NEEDS TESTING

These runs belong to the lead. This lane has used only the assigned Linux box.
The source under test is `51b578e56`; the Linux executable SHA-256 is
`b41bdef11752db24df328be81a5b30506c8fe2c4e393d83f74ede2e263521c8a`.
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
The supplied one-way component hashes cannot recover those fields. The branch's
33 sampled frames establish equality through 57175 and an attachables-only first
difference at 57176; both reported host seat 1 under AI at 57175. Neither the
synthetic certified-hold detector nor those AI labels identify the field object or
prove why the third hold differs from the two earlier clean holds.

Native diagnostics need a real TURN run long enough to observe allocation and
permission refreshes, followed by a transport timeout/close with both ends' packet
timestamps, route, native refresh result and close reason. The local native test
uses DIRECT and cannot prove TURN behavior. Build the pinned native dependency in
a fresh private checkout with `external/patches/build_gns_turn.py`; its build record
must contain `transport_diagnostics_version=1`. The patches are not idempotent.
Windows/MSVC and Mac/GCC builds of that dependency remain unmeasured in this lane.

The signed directory capacity row is still RED at 13129 decisions / 2097084 bytes
with HTTP 503. Server persistence/deployment belongs to the lead's server lane.
The direct-host partition row remains reachable and RED: its symmetric partition
holds the joiner while the documented no-referee host half continues. The lead must
resolve that row's expected survivor against the existing host-half rule; this lane
does not waive either check. The default suite's headless-only policy rows remain
measured under the owner's mandatory visible Linux policy.
