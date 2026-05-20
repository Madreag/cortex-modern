# MP M2 — Lua determinism baseline (Block A landing state)

State of Lua-environment determinism **at the moment Block A (the `lua_state`
SimChecksum subsystem) lands**, before Blocks B–F. The starting-point reading the
later blocks are measured against.

## How CC's Lua RNG actually works

The M2 plan was drafted assuming `math.random` reaches LuaJIT's native per-state
`PRNGState`. It does not. `LuaStateWrapper::Initialize()` (`LuaMan.cpp`) overrides
`math.random` in every Lua state with a Lua shim that routes to `LuaMan:SelectRand`
/ `PosRand` → a per-`LuaStateWrapper` C++ `RandomGenerator` (`m_RandomGenerator`, an
`std::mt19937`). LuaJIT's native `lib_math.c` PRNG is dead code in CC.

So the `lua_state` subsystem hashes `m_RandomGenerator` for each state (via
`RandomGenerator::SerializeStateForHashing()`, the same M1 Block F mechanism used
for `sim_rng`) — **no LuaJIT-internals coupling, no `lj_obj.h` include, no LuaJIT
source patch.** Blocks A and B work entirely against `m_RandomGenerator`.

## What `lua_state` covers

Hashed in `MovableMan::Update`, co-located with the `sim_rng` snapshot (before the
see-ray / MOID futures launch). Order: master state, then threaded states by index
(`m_ScriptStates` is a fixed vector, never reordered — index is a stable state id).

## Baseline state (M2LuaBaseline, 600 ticks)

`m_RandomGenerator` is seeded once per state in `LuaStateWrapper::Initialize()` from
a `g_SimRNG` pull. On one machine + build that pull is reproducible, so for
main-thread-only Lua the `lua_state` trace is **already deterministic across
same-machine runs at Block A** — `M2LuaBaseline` is expected to MATCH.

That is the correct, honest baseline: M2 is not closing a within-machine
single-build divergence the way the M1 C++ blocks did. The non-determinism
`lua_state` exists to catch is:

| Source | Closed / handled by |
|---|---|
| Per-state RNG not reseeded per activity → drift across activity transitions | Block B |
| Lua RNG not coupled to the activity/sim seed (`-seed` doesn't reach it) | Block B |
| `pairs()` hash-bucket iteration order (cross-build, non-primitive keys) | Block C |
| `os.time`/`os.clock` wall-clock reads in sim Lua | Block D |
| Threaded-state RNG drift when AI Lua runs under the M1 `ThreadedUpdate` race | MP M4 |

The last row is why M2's MATCH scenarios run their Lua in the Activity script
(main thread): a scenario that runs actor AI Lua inherits the residual M1 race and
will diverge at ~tick 10–20 in some runs until M4. That is inherited, not an M2
regression — see `M2_PLAN.md` §2.1.

## How to run locally

### Windows
```pwsh
& ".\Cortex Command.exe" -determinism-check `
    --scenario M2LuaBaseline --ticks 600 --seed 42 --runs 10 --output lua-report.json
```

### Linux
```bash
./CortexCommand -determinism-check \
    --scenario M2LuaBaseline --ticks 600 --seed 42 --runs 10 --output lua-report.json
```

The per-tick trace carries the `lua_state` subsystem hash alongside `sim_rng`,
`actors`, etc. `--keep-runs` retains the per-run JSONs for inspection.

## References

- `D:\Projects\M2_PLAN.md` — the M2 plan (Block A §4)
- `Source\Network\SimChecksum.{h,cpp}` — the per-subsystem hasher
- `Source\Managers\LuaMan.cpp` — `HashAllLuaStatesIntoSimChecksum`, `math.random` override
- `Source\System\RTETools.h` — `RandomGenerator::SerializeStateForHashing`
- `Data\Tests.rte\Activities\M2LuaBaseline.lua` — the baseline scenario
- `.github/workflows/determinism.yml` — the non-blocking CI step
