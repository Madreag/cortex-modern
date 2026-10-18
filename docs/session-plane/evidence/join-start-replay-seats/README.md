# Replayed seat state during a live handshake

Seed 2810 begins returner 3's live handshake at replay frame 413 for activation 466. The ruling continuation subsequently holds peer 2 at frame 433. The private replay reaches 465, but the live join still waits for peer 2's Start: the production handshake pump imported only frame prefixes, while seat-state adoption was behind `IsRunning`. The fixture had the same ordering. Complete original trace on `71f48dea` retains the missing-Start failure.

The ten-role production-pump detector is RED on `acbc5dd1` and GREEN on `0ab747001e75d1a82a33c22a97f0179b56a0a2627c6ad4f27577a797a37b524b`. The pump now imports replayed seat transitions before evaluating its handshake. Peer-frame seat adoption uses the existing same-session/round/config/local-identity/playback eligibility, and clips its cursor to the completed replay and the frame before activation. No extra lifecycle state is added.

The detector preserves incomplete, future, wrong-session, wrong-round, wrong-config, wrong-identity, non-playback and unavailable-source negatives. An active seat still needs a genuine Start; importing a hold fabricates none. Replay completion still gates live production, and the live simulation cursor never advances during adoption. Five related native controls, all five direct split/image controls and complete native lockstep/world-join 2/2 pass.

FLAGGED fixture departure: its two replay-progress calls now import the same completed seat state as the production handshake pump. The final handoff import stays in place. No assertion, deadline, input property, quorum, delay, wire format or single-player path changes.

The original 2810 handshake now completes; the schedule reaches a later RED return deadline, seat 2 at 18951 ms against 11069 ms. Consecutive Internet splits and the earlier private-catch-up schedule stay GREEN. Lossy-voter and stalled-lane return reholds and seed 69's held-host return remain RED. No wider inventory score is claimed. `receipts.json` retains 21 complete headed launches and both streams, single-box/proof=false. Remaining catalogue/schedules, #45/#46, B3 last and final gate remain open; final report untouched.
