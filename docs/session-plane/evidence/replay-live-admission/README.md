# Replay initialization and live admission

The second image return in the consecutive Internet split schedule copied `joinsRunningRound` and an earlier `seatStateThroughFrame` into its private replay. The shared round reset installed the live wait, and only afterward did `StartReplay` select playback. The replay could not produce the seat state it was waiting for. The same ordering also inherited snapshot admission from `resumeFromSnapshot`.

The original is RED on `761a7498`; diagnostic-only `921d6ab5` identifies a Running replay stalled at its first committed frame 781. The eleven-role native detector is RED on `eff68911`: both playback constructors fail on either live gate and their combination, while unflagged playback and the live-reset controls pass. Complete streams are retained.

`ResetRoundState` now selects its mode before restoring state. Only live mode installs the two admission waits. Recorded holds, including the playback machine's own held seat, are restored in that one reset; the later duplicate restoration pass is removed. Live callers retain the existing mode and guards. No frame validation, quorum, bound, wire format, delay or single-player path changes. The fixture edit adds failure diagnostics only.

Final binary `71f48deae0f6bc6cc88c71df38e2e6003912700e25bd3bcb85a5fb336c6c7efe` passes all eleven native roles, five related native controls, all five direct split/image controls, and complete native lockstep/world-join 2/2. The consecutive-split schedule is GREEN end to end; the earlier private-catch-up schedule stays GREEN. Recorded input, authority, hold and return checks remain strict, as do the live-reset waits.

Seed 2810 remains RED waiting for peer 2's live member Start; seed 69 remains RED with its host held beyond the automatic-return deadline. No wider inventory score is claimed. `receipts.json` retains 20 complete headed launches with both streams, single-box/proof=false. Remaining catalogue/schedule groups, #45/#46, B3 last and final gate remain open; final report untouched.
