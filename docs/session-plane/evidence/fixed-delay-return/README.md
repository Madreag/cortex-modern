# A returning seat completes its rejoin under a fixed input delay (#46)

Driver: `tools/test_fixed_delay_return.py` - two local engines (host and client), the host's delay policy Fixed at
`--delay` ticks (or the Auto control with `--auto`), the client leaves with its ticket at tick 900 and returns on a new
process with the same identity. Pass = no delay refusal, the private catch-up completes and both peers play 300 more
ticks. Linux (GCC, v6 library), headed engines through the POSIX runner, topology single-box, proof=false.

- cm-dev 786ba8b5e plus the writer voice-read fix (the unpatched host crashes on the base, see `../writer-voice-read`):
  RED at Fixed 1 (`Your connection needs 19 delay frames; the match currently allows 1`) and Fixed 6 (`needs 23 ...
  allows 6`), the published restart counted into the link need; the Auto control passes
  (`fx46-cmdev-audio-*.json`).
- This branch before 2c9e8dcc4 (5e326892a already sized the return from the link alone): no refusal, but no return
  completed under Fixed 1, Fixed 6 or Auto (`fx46-auto-d1-c.json`). The returning seat starts its live round from the
  image's config, which carries no member's start terms; since cc0b1b21b every member answers with its own retained terms
  (the host: frame 1), which the seat read as a straggler start and never ran (`branch-red-host-start-terms.txt`:
  `admission=6528/1 terms=none matches=0`).
- 2c9e8dcc4: a member's authenticated start supplies the terms the image did not carry; terms the image carried still
  bind every start. Native row `an_image_return_takes_its_members_retained_starts`: RED on f24a17c9, GREEN on 09e327e7
  (`native-row-red-green.txt`). Scenes GREEN on 09e327e7: Fixed 1, Fixed 6 (`fx46-terms-d1.json`, `fx46-terms-d6.json`);
  the Auto control GREEN on 4344166d (`fx46-terms-auto-d1.json`).

A link whose measured need exceeds the host's fixed delay is still refused with the same message: the Fixed policy caps
the delay and the return is sized from the link alone, never from restart work.
