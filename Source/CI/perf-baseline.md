# Perf baseline

Local-rig wall-clock for the determinism scenarios, so a future change that silently
serializes or de-threads the sim is caught as a regression. Informational — CI is too
noisy for perf signal, so this is captured on a fixed local rig, not gated.

## Method

Single headless run per scenario at its design length, boot-to-end wall-clock
(`__wall_seconds` in the report JSON):

```
<bin> -scenario <S> -seed 42 -max-ticks <design ticks> -tick-hashes -num-lua-states 4 -out <S>.json
```

The number is boot + sim (the determinism path skips the menu, so boot is the fixed
~constant; the rest is sim). `-num-lua-states 4` pins the threaded-Lua count.

## Baseline (2026-06-11)

Reference machine: x64. Windows = `Final` config; Linux = meson `release` (Ubuntu 24.04,
WSL2 on the same machine). Seed 42, num-lua-states 4, scenarios on Grasslands.

| scenario          | ticks | Windows (Final) | Linux (release) |
|-------------------|-------|-----------------|-----------------|
| SimBaseline       | 600   | 10.0 s          | 10.0 s          |
| TerrainStress     | 900   | 15.9 s          | 16.3 s          |
| LuaRandomStress   | 600   | 10.0 s          | 10.0 s          |
| ThreadStress      | 900   | 15.0 s          | 16.5 s          |
| PerfBench         | 1200  | 22.3 s          | 29.9 s          |

Boot dominates the short runs; for sim-throughput signal compare PerfBench (compute-bound
by design) or subtract a 0-tick boot baseline. A fixed reference point, not a microbenchmark.
