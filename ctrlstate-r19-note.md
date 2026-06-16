# Round 19 — arm64 macOS control-state dump (xref/arm64-ctrlstate-r19)

**Branch note:** the round-19 brief said push to `xref/linux-x86-ctrlstate-r19`, but this data is from
the **arm64 macOS** leg. Pushing arm64 rows onto a `linux-x86` branch would mislabel the platform and
break the 3-way diff, so it is on **`xref/arm64-ctrlstate-r19`** (matching the r15/r18 arm64 convention).
Redirect me if you actually want it elsewhere.

**One-line:** r18 proved AnalogAim is byte-identical 3-way, so controller@80 is a control-STATE /
inputMode flip. This dumps the full 57-bit `ControlState` bitstring + `inputMode` per actor at t78-82,
in the single-threaded SimChecksum controller loop (C++, no luabind). Env-gated `CTRLSTATE_DUMP=1`.

## Files
- `ctrlstate-instrumentation.patch` — the C++ instrumentation (`Source/Managers/MovableMan.cpp`).
- `ctrlstate.csv` — the dump (no header).
- This note.

## CSV format (no header)
```
tick, uniqueID, <57-char 0/1 string of ControlState[0..56]>, inputMode
```
- Bit at position N = `controller->IsState(ControlState(N))`. Align Linux/arm64 by `(tick, uniqueID)`.
- 606 rows: 124@t78, 122@t79, 121@t80, 120@t81, 119@t82.
- Determinism preserved: controller hash at t80 still `990695b27362…` with the dump enabled.

## ControlState bit index → name (0-based, matches the bitstring positions)
```
 0 PRIMARY_ACTION    1 SECONDARY_ACTION  2 MOVE_IDLE        3 MOVE_RIGHT
 4 MOVE_LEFT         5 MOVE_UP           6 MOVE_DOWN        7 MOVE_FAST
 8 MOVE_FAST_TOGGLE  9 BODY_JUMPSTART   10 BODY_JUMP       11 BODY_CROUCH
12 BODY_PRONE       13 AIM_UP          14 AIM_DOWN        15 AIM_SHARP
16 WEAPON_FIRE      17 WEAPON_RELOAD   18 WEAPON_RELOADHELD 19 PIE_MENU_OPENED
20 PIE_MENU_ACTIVE  21 PIE_MENU_ACTIVE_ANALOG 22 PIE_MENU_ACTIVE_DIGITAL 23 WEAPON_CHANGE_NEXT
24 WEAPON_CHANGE_PREV 25 WEAPON_PICKUP 26 WEAPON_DROP     27 WEAPON_PRIMARY_HOTKEYSTART
28 WEAPON_AUXILIARY_HOTKEYSTART 29 ACTOR_PRIMARY_HOTKEYSTART 30 ACTOR_AUXILIARY_HOTKEYSTART 31 WEAPON_PRIMARY_HOTKEY
32 WEAPON_AUXILIARY_HOTKEY 33 ACTOR_PRIMARY_HOTKEY 34 ACTOR_AUXILIARY_HOTKEY 35 ACTOR_NEXT
36 ACTOR_PREV       37 ACTOR_BRAIN     38 ACTOR_NEXT_PREP 39 ACTOR_PREV_PREP
40 HOLD_RIGHT       41 HOLD_LEFT       42 HOLD_UP         43 HOLD_DOWN
44 PRESS_PRIMARY    45 PRESS_SECONDARY 46 PRESS_RIGHT     47 PRESS_LEFT
48 PRESS_UP         49 PRESS_DOWN      50 RELEASE_PRIMARY 51 RELEASE_SECONDARY
52 PRESS_FACEBUTTON 53 RELEASE_FACEBUTTON 54 SCROLL_UP    55 SCROLL_DOWN
56 DEBUG_ONE
```

## What arm64 shows at t80 (lead diffs vs Linux/Windows to find the split)
- `inputMode`: 120 actors = `2`, 1 actor = `1` (uniqueID 16975).
- Active state bits are dominated by **15 AIM_SHARP** and **16 WEAPON_FIRE** (the combat actors),
  with some **10 BODY_JUMP**. Distinct t80 bitstrings (arm64):
  - 68× `AIM_SHARP`
  - 22× `AIM_SHARP + WEAPON_FIRE`
  - 12× `BODY_JUMP + AIM_SHARP`
  - 8× (all zero)
  - 6× `BODY_JUMP + AIM_SHARP + WEAPON_FIRE`
  - 3× `BODY_JUMP`
  - 1× `MOVE_IDLE` (id 16975, inputMode 1)
  - 1× bit 25 `WEAPON_PICKUP`
- The 3-way split is almost certainly one actor's `WEAPON_FIRE` (16) or `AIM_SHARP` (15) toggling a
  tick early/late between platforms (a sim-time gate), or its `inputMode`. Diff `(tick,uniqueID)` to pin it.

## Run
```
CTRLSTATE_DUMP=1 ./builddir/CortexCommand -scenario PerfBench -seed 42 -max-ticks 86 \
    -tick-hashes -num-lua-states 4 -out pb.json
```
