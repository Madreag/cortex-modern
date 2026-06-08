# Thread-count determinism — what the gates check, and why controller flicker is advisory

`determinism-thread-matrix-{linux,windows}` runs `ThreadStress` at Lua-state counts
1/2/4/8/16 and diffs the per-tick traces ACROSS counts. The sweep jobs
(`determinism-linux` / `determinism-windows`) diff repeat runs at one count. Both
classify divergence by subsystem: divergence that is controller-ONLY for the whole run —
the flicker re-converges and no sim subsystem ever differs — reports `MATCHED-SIM`
(advisory, `ai_only_divergence` in the report); any sim-subsystem divergence fails,
whatever diverged first. The matrix is the advisory exception: across counts the AI
inputs legitimately differ, the downstream sim follows, and DIVERGED is expected there.

## The contract

The blocking property is the sim given the same Controllers: same seed, bit-identical
sim subsystems (actors/particles/terrain/carve_math/scene/sim_rng/lua_state), every
scenario, run-to-run and across state counts. Controller state is the AI's output —
under the Controller-sync MP architecture it arrives as an input over the wire, so its
bit-stability across local replays is not part of the contract.

## The AI-controller flicker

Combat-stress scenarios (`ThreadStress`, `TerrainCarveStress`) show a content-anchored
controller flicker: at a fixed tick per content (591 / 315 on the current scenarios) the
`controller` hash diverges for 1-2 ticks in some runs and re-converges; no sim subsystem
follows. Onset and frequency vary with machine, state count and content — it reproduces
run-to-run at fixed counts and across counts. The residual is a cross-actor read in the
parallel AI whose timing decides a decision boundary; per-MO RNG is not the cause
(seeded by uniqueID+tick+phase, thread-invariant).

Forcing the AI bit-stable — freezing a pre-AI snapshot, or serializing the AI phase —
would trade parallelism for a property Controller-sync does not need. The flicker stays
visible: the matrix and TSan jobs run full-length, the report flags it, and the read is
tracked for a root-cause fix.

Under ThreadSanitizer the high-count flakiness resolves to LuaJIT-runtime internals
(JIT mcode allocation + the allocator's mmap-hint global, rooted in `lj_*`) fired by concurrent
Lua states — phantom noise TSan can't track through JIT frames, suppressed via `race:^lj_` (see
`tsan-known-races.md`). Address-level, benign for values, not a game-logic race.
