# Round 18 — arm64 macOS aim-chain dump (xref/arm64-aimchain-r18)

**One-line:** C++ controller-loop dump (env-gated `CCCP_AIMCHAIN_DUMP=1`, ticks 78–82) of the
divergent analog-aim value; the intended Lua decomposition is dead on this arm64 build (int64
luabind getters segfault the AI worker threads), so the value is captured in C++ where the tick +
UniqueID are read directly. 93 of 121 actors carry an active (non-zero) analog aim at t80.

## Files
- `aimchain-instrumentation.patch` — the C++ instrumentation (`Source/Managers/MovableMan.cpp`).
- `aimchain_arm64.csv` — the dump (no header, same raw-float-bits format as r15 ctrlfield).
- This note.

## CSV columns (no header row, matches r15 format)
```
tick, uniqueID, bits(AnalogAim.X), bits(AnalogAim.Y), bits(GetAimAngle(false)), bits(GetAimAngle(true))
```
- Each float is the raw IEEE-754 bit pattern as a decimal `uint32` (`memcpy` float->uint32), exactly
  like the r15 dump — round-trips the float bit-for-bit and is directly string-diffable across legs.
- `uniqueID` = `Actor::GetUniqueID()` read in C++ (no luabind). Align Linux/arm64 by `(tick, uniqueID)`.
- `AnalogAim` = `Controller::GetAnalogAim()` (the divergent field). `GetAimAngle(false)` is raw
  `m_AimAngle`; `GetAimAngle(true)` is facing-adjusted.

## Run
```
CCCP_AIMCHAIN_DUMP=1 ./builddir/CortexCommand -scenario PerfBench -seed 42 -max-ticks 85 \
    -tick-hashes -num-lua-states 4 -out pb_aimchain.json
```
- 606 rows: 124@t78, 122@t79, 121@t80, 120@t81, 119@t82.
- Determinism preserved: controller hash at t80 is still `990695b27362…` with the dump enabled
  (the dump is read-only + env-gated).

## File-output method
C++ `static std::ofstream("aimchain_arm64.csv", trunc)` in the single-threaded SimChecksum
controller loop (`MovableMan::Update`), flushed per row. **Not** Lua io.open — see below.

## Why the Lua-side decomposition was dropped (the briefing's planned approach)
Measured on this build (Apple clang / arm64 / luabind):
1. `TimerMan:GetSimUpdateCount()` is **not bound to Lua** — no Lua tick accessor exists.
2. **Any int64-returning luabind getter segfaults the AI worker threads.** Backtrace:
   `value_converter<cpp_to_lua>::apply<long long>` → `EXC_BAD_ACCESS @ 0x58`, under
   `thread_pool::worker` running `UpdateControllers`. This kills `Timer().StartSimTimeMS`, and by
   type signature also `Owner.UniqueID` (`long`) and `Owner.Age` (`unsigned long`). The lead's
   "UniqueID + Age are safe" holds on x86 Win/Linux but **not** on this arm64 luabind build.
3. The briefing's target line `HumanBehaviors.lua:1708` (ShootArea/FullAuto) is **never reached**
   by t85 — the live combat aim is set by other behaviors (ShootTarget `:1184`, etc.).

Since `lua_state` is byte-identical at the divergence and the controller hash splits 3-way, the
divergent value is a C++ value, fully captured by this dump with zero luabind/threading exposure.

## Active analog-aim actor IDs at t80 (candidates for the 3-way split; lead diffs vs Linux)
93 actors (of 121) at t80 have non-zero AnalogAim. Cross-platform divergence cannot be determined
from arm64 alone — the full set is in the CSV. IDs:

```
16989 17017 17031 17073 17087 17101 17143 17269 17297 17311 17339 17353 17367 17395 17423 17451
17479 17493 17507 17549 17591 17605 17619 17647 17675 17689 17717 17731 17745 17773 17787 17801
17815 17843 17857 17871 17885 17913 17941 17955 17969 17983 17997 18011 18025 18039 18053 18081
18095 18109 18123 18137 18165 18179 18207 18221 18235 18249 18305 18319 18333 18347 18417 18431
18445 18473 18501 18515 18529 18543 18557 18585 18613 18627 18641 18711 18725 18753 18795 18809
18823 18837 18851 18865 18879 18907 18921 18963 18977 18991 19005 19033 19061
```
(t79 active set is essentially the same; both ticks are in the CSV.)
