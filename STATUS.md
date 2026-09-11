# STATUS — live board (the answer to "where do we stand / what are you working on")

Rules: updated on every status request and at every checkpoint; percentages are the lead's judgement per §B-1 item of `RESUME.md`,
grounded in the evidence named there; Δ is against the previous status request; "Working on" names the exact task and the worker.
Never reformat; append to the history. Every task, worker or Mac agent must move a number; one that lands without moving a number is reported here as a NEGATIVE delta (wasted time) with the reason, and the lead re-evaluates before spawning the next.

Last update: 2026-09-11 06:44 UTC · Overall **57%** (W21 landed: five H4/B1 secondary defects with detecting tests)

| # | Item (RESUME §B-1) | % | Δ | Working on NOW | Not started / next |
|---|---|---|---|---|---|
| 1 | Controller boundary + wire/replay compat | 93 | +3 | Mac leg DONE (W15): clean clone of `0e2aa3812f` builds a binary byte-identical to positive20 (`238066fa01d9`), 13/13 selftests PASS. MSVC build loop of the group still running (W12) | Windows family Source42 after the fix group merges |
| 2 | Restore / identity / state inventory | 80 | 0 | Both bugs root-caused and designed (`FIX_GROUP_1_DESIGN.md` C, D). Audio owner registry: implementation starting now (W19, `stage2/audio-owner-registry`). AI-written value maps: design D final, implementation waits for a free lane (Mac preferred) | Merge with item 6 group → Source42 |
| 3 | Gameplay fixtures + minimizer + matrix | 85 | 0 | Nothing running | P5 / peer_roundtrip reruns fold into Source42 |
| 4 | Presentation + performance (measured 100–200 ms feel) | 15 | 0 | Nothing running | After the fix group is green |
| 5 | Breadth + verification debt | 88 | +3 | Nothing running; Source41 CLOSED (independent review accepted all five oracle changes) | Source42 |
| 6 | H4 reconnect + host moderation | 60 | +5 | Five secondary defects LANDED on stage2/h4-secondary 0520bbc4f7 (W21; detecting tests red->green; 10 network selftests + controller-frame PASS; pushed). Pause-the-match implementing (W18) | Merge into takeover-msvc after W12 links; Source42 |
| 7 | Lobby / session UX + robustness | 20 | 0 | Nothing running | After the fix group |
| 8 | Discovery / internet play + roadmap | 10 | 0 | Nothing running | After the fix group |

Since the previous ask (05:37 UTC): Source41 closed (#5); item 6 root cause found (hostile duel kills the dropped brain during the hold)
and your decision recorded (pause the match); fix-group design written for items 2 and 6; two implementation workers running for #6;
Mac made a worker host via the Cursor CLI (launcher works; the long-prompt stall is being isolated); stale pollers cleaned up.

## History (one line per status request)

- 2026-09-11 05:25 UTC · overall 55 · 1:90 2:80 3:85 4:15 5:85 6:55 7:20 8:10 · first baseline
- 2026-09-11 05:37 UTC · overall 55 · unchanged · working on 1, 2, 5, 6
- 2026-09-11 05:45 UTC · overall 55 · unchanged · board created
- 2026-09-11 06:20 UTC · overall 55 · 1:90 2:80 3:85 4:15 5:88 6:55 7:20 8:10 · working on 1, 2, 6 (5 closed)
- 2026-09-11 06:32 UTC · overall 56 · 1:93 · W15 landed: Mac clean-clone build byte-identical to positive20, 13/13 selftests
- 2026-09-11 06:44 UTC · overall 57 · 6:60 · W21 landed: five H4/B1 secondary defects with detecting tests
