# MP M2 — Lua determinism state

State of Lua-environment determinism for the MP M2 `lua_state` SimChecksum
subsystem and the Block B–D Lua guarantees, with the post-implementation
determinism-check results.

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

## Verified state (determinism-check, 3 runs, 2026-05-19)

The four no-actor M2 scenarios were run through the determinism-check. Every one
DIVERGED on the total-tick hash — but the cause is inherited from M1, not M2:

- **`sim_rng` is the earliest-diverging subsystem** (tick 1–49). It carries M1's
  residual sim-thread race. M1's own `M1Baseline` diverges identically — `sim_rng`
  at tick 1 — in *both* the M1 build and this M2 build, so the divergence is a
  pre-existing M1-harness property, not an M2 regression.
- **`lua_state` (M2's subsystem) is the LAST subsystem to diverge** (~tick 307–325),
  downstream of the M1 race — not an independent M2 fault.
- **The `decisions` subsystem did NOT diverge** for `M2LuaRandomStress` /
  `M2PairsStress` / `M2OsStubTest`. Those scenarios fold each tick's `math.random`
  sequence / `pairs()` order / `os.*` values into a decision event; that subsystem
  matching across runs is the positive signal that M2's Lua-language guarantees
  hold. Only the surrounding M1 sim race keeps `total` diverging.

So the M2 CI scenarios stay **informational** alongside M1's until MP M4 closes the
sim-thread race. The non-determinism each M2 block addresses:

| Source | Addressed by |
|---|---|
| Per-state RNG not reseeded per activity → drift across activity transitions | Block B |
| Lua RNG not coupled to the activity/sim seed | Block B |
| `pairs()` hash-bucket iteration order (cross-build, non-primitive keys) | Block C |
| `os.time` / `os.clock` wall-clock reads in sim Lua | Block D |
| Threaded-state RNG drift under the M1 `ThreadedUpdate` race | MP M4 |

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
