# B3 re-measured with 48 single-player controls

Driver: `tools/feel/matched_session_delay.py --single-player-repeats 24 --network-repeats 3` on build 8c903518 (the
source of 858a265c4), one Linux box, headed, every case quiet (no other engine), software input sample through the
completed UploadFrame, no physical scanout. 48 single-player controls first (24 per actor team, alternating), then three
pairs at each of 30 ms and 200 ms. The envelope is the controls' own p50 minimum and maximum per team and per action; no
tolerance is added (`frozen-sp-envelope.json`, `result.json`, `plan.json`).

| | single player | 30 ms pairs | 200 ms pairs |
|---|---|---|---|
| median of p50 (ms) | 29.36 | 31.50 | 29.73 |

Frozen bands: team 0 29.282-32.722 ms, team 1 26.497-29.445 ms; frames 2-2 for both teams.

- Aggregates inside the band: 9 of 12. Outside: the 30 ms pair 1 host (team 0) 32.785 ms, its client (team 1) 32.082 ms,
  and the 30 ms pair 2 client (team 1) 33.473 ms. Every 200 ms peer is inside.
- Presented frames: 2 in all 12 (inside the frame band).
- Per action: 12 of 60 p50s inside their single-player action bands.
- B1 (delay at most 6 ticks at 30 ms, 16 at 200 ms; measured 4 and 15), B2 (the preview covers the whole delay), B4 (the
  overlay's delay line live) and the duplicate-frame check: 12 of 12.

The own-input sample point was not moved: the sample is taken inside the producing pass of the simulation tick, which also
runs the local AI and owns the input it produces; moving it out of that pass changes what the pass produces, a determinism
risk this lane did not take. B3 stays RED on the strict per-action and aggregate check, with these numbers.
