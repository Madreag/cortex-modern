# Accepted-source immutable inventory

Binary `ef5fbbb7` passes seven of nine network rows and the complete
world-join suite. The two collections in [inventory](inventory.json) use
the same executable: native lockstep/world-join, then the other eight
network rows. Fixed catalogue: 179/239 GREEN, all 108 saved schedules
executed. Random schedules: 89/200 GREEN, all 200 executed. No timeout.

The accepted-source slow-receipt case and seed-109 nested-question double
return stay GREEN. Every remaining RED and named engine failure is kept
in ten complete headed [receipts](receipts.json) and the
[catalogue groups](fixed-failure-groups.txt), single-box, proof=false.

Remaining groups include connected history, split/image return, consumer
deadlines, legacy cursor phase, and relay phase setup. The original
seed-1020 return is still 8110 ms against 8082 ms. Three random groups
still expose 51 ms waits against 50 ms. No property or bound changed.
B3 and #45/#46 follow those groups; the final report stays untouched.
