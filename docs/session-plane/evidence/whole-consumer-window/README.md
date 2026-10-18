# The complete consumer input window

No-fault seed 187 diverges at frame 55. Its authenticated input was sent at
1,467 ms and physically scheduled for 1,607 ms, but the host installed an AI
hold at 1,606 ms. Its previous displayed frame was at 1,565 ms and its
unchanged deadline at 1,615 ms. A half-tick reservation spent nine
milliseconds of a still-open input window, despite local holds requiring
no remote signature.

The independent whole-window detector is RED on `49b900ab`, GREEN on
`5114ec14` and final `3b36d248`. Local certification now uses the actual
deadline. The detector rejects a premature vote/hold through 49 ms, installs
the absent input's AI hold at exactly 50 ms, accepts an input supplied
inside that window, and preserves the preceding prefix. The bound remains
50 ms; link delay, receipts and future cooperative holds retain their rules.

FLAGGED native-fixture departure: three existing controls observed local
certification at 41 ms. Their intermediate REDS are retained. Observations
now occur at the actual 50 ms deadline, retaining every existing signature,
membership, AI boundary and historical-prefix assertion. The new detector
also adds the missing no-early-hold assertion. Optional exact-frame packet
diagnostics observe the original delivery schedule without changing it.

Final `3b36d248` passes the complete original no-fault jitter schedule,
steady 250 ms/120 s, simultaneous slowdown, four focused native controls,
all five direct split/image controls and complete native lockstep/world-join
2/2. Seed 1020 remains RED: return at 8,110 ms versus 8,082 ms. The original
host-autosave return-deadline RED remains unchanged. No complete catalogue
or random-schedule GREEN is claimed on this candidate.

Twenty-six complete headed official-runner [receipts](receipts.json) retain
all REDS and final controls, single-box, proof=false. Wider engine groups,
B3, #45/#46 and final gate remain open. No single-player change; final
report untouched.
