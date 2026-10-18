# The save writer reads predicted voices as its boundary froze them

Found by the #46 return driver (`tools/test_fixed_delay_return.py`), Linux, headed, single-box, proof=false.

A sound container frozen for a save filtered its playing channels through `AudioMan::IsPredictedVoice`, which read the
live voice map from the checkpoint writer thread while the game changed it. The host crashed taking a returning seat's
private base in 2 of 2 Fixed 6 returns (`segfault-counts.txt`: fx46-base-d6, fx46-base-d6-b; `crash-stack.txt`).
Under a save's read scope the lookup now walks the voices of the boundary view, as AudioMan's own frozen capture does
(34480b85d). After it, 0 host crashes in the next returns (fx46-audio-d6, fx46-auto-d1-b, fx46-auto-d1-c alone; two more
side by side).
