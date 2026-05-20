# MP M3 — terrain determinism baseline (Block B landing state)

The starting-state reading for the M3 terrain-determinism work, captured when
Block B lands — before Blocks C–E convert the penetration and collision math to
fixed-point. Sibling to `baseline_divergence.md` (the M1 equivalent).

## What Block B added

- **`terrain` subsystem — now real.** M0/M1 wired `terrain` as a placeholder
  that hashed the sim-tick number. Block B replaces it with a hash of the actual
  terrain state — the material bitmap and the foreground-colour bitmap — fed at
  end-of-tick from `SceneMan::FeedTerrainToSimChecksum`.
- **`carve_math` subsystem — new.** Every `WillPenetrate` / `TryPenetrate` /
  `DislodgePixel` decision feeds `carve_math`: position, material, the
  penetrate/dislodge result, and the impulse / velocity / retardation. Finer-
  grained than `terrain` — it catches a divergent destruction *decision* the
  tick it happens, before it accumulates into a visible bitmap difference.
- **`M3TerrainStress`** — a destruction-heavy scenario (six brain-hunters, 900
  ticks) giving both subsystems a demanding workload.

Both feeds are gated on `MetricsCollector::IsRecordingTickHashes()` — they run
only inside a determinism trace, never in normal play, so the full-bitmap hash
is not a gameplay cost.

## Empirical baseline — measured 2026-05-19 (Block B, Debug Minimal x64)

`cccp-determinism-check --scenario M3TerrainStress --ticks 300 --runs 3`:

```text
RESULT: DIVERGED
  per-subsystem first divergence: sim_rng 2, actors 10, scene 11,
                                  carve_math 12, decisions 12, lua_state 14, terrain 15
```

`carve_math` and `terrain` are present in the trace from the first tick a
penetration happens — Block B's wiring works. But the scenario **diverges
single-machine**, and the correction below matters for Blocks C–F.

### The plan's premise was wrong — terrain destruction *is* subject to the M1 race

`M3_PLAN.md` §4 Block B reasoned that terrain destruction is race-free because
`WillPenetrate` / `TryPenetrate` run in the serial `MovableMan::Travel` loop.
The functions do — but their **inputs** do not:

- `TryPenetrate` calls `RandomNum()` (→ `g_SimRNG`) for spray ejection, and
  `g_SimRNG` carries the M1 residual thread race.
- The impulses feeding the penetration path come from objects whose physics is
  downstream of the same raced `g_SimRNG`.

So `carve_math` / `terrain` diverge **single-machine** as a *downstream*
consequence of `sim_rng` — exactly like `actors` / `particles` / `scene`. This
was confirmed by running `M2LuaBaseline` (a zero-actor scenario): it too
flaky-diverges `sim_rng`, so the race is not actor-specific — it is the
always-on M1 residual that M1 and M2 both shipped as a known, informational-gate
issue. **MP M4 closes it; M3 is explicitly scoped not to touch threading.**

The race is *flaky*: across 3 runs, two were bit-identical for 80+ ticks and one
diverged near tick 13. Runs that dodge the race are fully deterministic.

## What Blocks C–E actually deliver

Blocks C–E convert the carve / penetrate / collision **math** to integer
fixed-point (`FixedPoint.h`). That makes the math **bit-identical by
construction** — given identical inputs, `TryPenetrate` computes the identical
result on every compiler and OS, no `/fp` flags, no libm. That is real and
complete at the end of Block E. What it does **not** do is remove the upstream
M1 thread race that perturbs the *inputs*.

The fixed-point conversion is therefore verified by:

1. **`FixedPointTests`** — the Block A cross-OS self-check proves the Q40.24
   library is bit-identical Windows↔Linux.
2. **`carve_math` / `terrain` never first-diverge before `sim_rng`** — once the
   math is fixed-point, any carve-path divergence is strictly inherited from the
   `sim_rng` race; the math itself introduces none. Measurable in the
   per-subsystem first-divergence report.
3. **Cross-OS CI** — at Block B, `carve_math` / `terrain` diverge cross-OS from
   tick 1 (the float libm hole, independent of the race). After Blocks C–E that
   independent cross-OS divergence is gone; what remains tracks `sim_rng`.

## CI gating — corrected from the plan

`M3TerrainStress` is in the determinism workflow matrix, **non-blocking**
(`continue-on-error: true`). `M3_PLAN.md` Block F expected to flip `terrain` /
`carve_math` to **blocking**; that is **not done** — they inherit the M1 race
and so stay informational, the same disposition as every M1/M2 subsystem, and
flip to blocking at **MP M4** when the race closes. This mirrors M2's own
plan-correction (`MISSION_LEDGER.md`, "CI gate — informational, corrected from
the plan").

## Expected progression

| Stage   | carve / penetrate math                | `carve_math` / `terrain` divergence |
|---------|----------------------------------------|-------------------------------------|
| Block B | float (`std::sqrt`, libm)              | diverges cross-OS *and* single-machine (race) |
| Block C | `SceneMan` penetration in fixed-point  | math bit-identical; divergence now only inherited from `sim_rng` |
| Block D | atom collision response in fixed-point | as C, plus the collision-response inputs |
| Block E | `SLTerrain` carve + geometry in fixed-point | whole destruction path's math bit-identical |
| Block F | docs + gate stays informational        | flips to blocking at M4 |

## How to run locally

### Windows
```pwsh
& ".\Cortex Command.exe" -determinism-check `
    --scenario M3TerrainStress --ticks 900 --seed 42 --runs 20 `
    --output m3-divergence.json
```

### Linux
```bash
./CortexCommand -determinism-check \
    --scenario M3TerrainStress --ticks 900 --seed 42 --runs 20 \
    --output m3-divergence.json
```

The cross-OS comparison runs in CI (`.github/workflows/determinism.yml`).

## References

- `D:\Projects\M3_PLAN.md` — the M3 plan (Block B §4)
- `D:\Projects\MISSION_LEDGER.md` — the M1 race + the M2 gate-correction precedent
- `Source\Managers\SceneMan.cpp` — `FeedTerrainToSimChecksum`, `FeedCarveMath`, the penetration path
- `Source\Network\SimChecksum.{h,cpp}` — the per-subsystem hasher
- `Source\System\FixedPoint.h` — the Q40.24 library Blocks C–E convert the math onto
- `Data\Tests.rte\Activities\M3TerrainStress.lua` — the scenario
- `Source\CI\baseline_divergence.md` — the M1 equivalent of this doc
