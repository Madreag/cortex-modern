# Private-start immutable inventory

Binary `2d023ac4` passes seven of nine network rows and the complete world-join suite. The [inventory](inventory.json) combines eight network rows with the same-binary native lockstep/world-join collection. Ten complete headed [receipts](receipts.json), single-box/proof=false; no timeout.

Fixed catalogue: 183/240 GREEN, all 108 saved schedules executed. Four new GREEN and zero new RED versus `2f576164`: the direct host/joiner silence, healed tie/host change, split during another return, and host slowdown with a third held seat. Random schedules: 93/200 GREEN, all 200 executed. The [comparison](comparison.json) and complete logs retain the measured results.

Every fixed failure is retained in the [groups](fixed-failure-groups.txt). Remaining engine groups include incompatible continuation histories, private replay/handshake completion, automatic return and healthy-observation reholds. The legacy Pause policy still explicitly disables bounded waits, leaving its 51 ms contract failure open. Seed 1020 still returns at 8110 ms against 8082 ms. Relay pacing remains 33 ms pre-fault lead and 4079 ms return. No bound, history or return assertion is waived.

The next investigated failure is the fixture's frame-708 author attribution: an excluded held reader consumes another group's certified tail but is recorded as its author. A change requires matching authenticated commit evidence; no mask-only or hash-only provenance is accepted. #45/#46, B3 last and the final gate remain open. Final report untouched.
