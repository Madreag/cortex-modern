# Cross-platform determinism — baseline divergence

What `determinism-cross-os` measures, and the result on the current foundation.

Determinism across hosts is meant to come from the FPU + libm path (see
`libm-standardization.md`), not fixed-point. The compiler FP contract is pinned —
`-ffp-contract=off` on GCC/Clang, `/fp:precise` + `/arch:SSE2` on MSVC. The open
question this captures: with the FP contract pinned but **libm not yet
standardized**, is the same seed bit-identical across same-arch hosts?

## Method

Each host runs the trace producer for a scenario at a fixed config and the per-tick
integer state hashes are diffed:

```
<bin> -scenario <S> -seed 42 -max-ticks 600 -tick-hashes -num-lua-states 4 -out <S>-trace.json
```

`-num-lua-states 4` is pinned so the threaded-Lua state count is identical on every
host. The diff is per-subsystem on `.runs[0].tick_hashes[].subsystems`, not just the
`total`, so the first diverging subsystem fingers the source.

## Result (2026-06-11, x64, design-length traces)

**Same-OS, cross-config (control): bit-identical.** Windows `Final` vs Windows
`Debug Minimal` match for all of SimBaseline / TerrainStress / LuaRandomStress
across the full trace. Determinism is optimization-independent on a host —
so the cross-OS result below is a true OS difference, not a build-config artifact.

**Cross-OS (Linux x64, gcc/meson-release vs Windows x64, MSVC/Final): DIVERGES.**
Every scenario diverges from the first ticks, with a consistent signature across
scenarios (it is in the shared sim/RNG init, not scenario physics). First diverging
tick per subsystem, TerrainStress (900t) shown; SimBaseline / LuaRandomStress /
ThreadStress share the rng/lua/terrain tick-1 onset:

| subsystem  | first diverging tick |
|------------|----------------------|
| sim_rng    | 1 |
| lua_state  | 1 |
| terrain    | 1 |
| actors     | 2 |
| controller | 15 |
| carve_math | 26 |
| scene      | 26 |
| particles  | 27 |

`tick` (the sim-frame counter) matches throughout — the trace mechanism is sound;
the divergence is real state. Scenarios without combat AI never diverge in
`controller` (SimBaseline), and LuaRandomStress diverges only in
`sim_rng`/`lua_state`/`terrain` — the spread tracks what each scenario exercises.

## Interpretation

The FPU contract alone does not buy cross-host bit-identity. `sim_rng` is an
integer generator, so its state only differs cross-OS if the *number/sequence* of
draws differs — i.e. FP-dependent branches earlier in the shared init resolve
differently per host. The remaining cross-host variables, with the FP contract
already pinned, are:

- **libm transcendentals** (`sin`/`cos`/`pow`/…): glibc vs the MSVC/SDL math
  library are not bit-identical, so any FP that feeds a branch or an RNG draw count
  diverges. This is the gap `libm-standardization.md` calls "the last source of
  cross-host drift."
- **cross-OS LuaJIT state ordering** feeding `lua_state`.

Pinning which of these dominates is the libm-standardization track. This
result is the empirical case for it: FPU is necessary but not sufficient.

## Scope

- **In scope, achieved:** intra-host determinism — bit-identical run-to-run and
  optimization-independent on each OS. That is what same-host replay needs
  and it is the blocking gate (`determinism-linux` / `determinism-windows`).
- **In scope, open:** same-arch cross-OS bit-identity (this doc). `determinism-cross-os`
  runs the diff but is advisory until libm is standardized; flip it to blocking then.
- **Out of scope here:** cross-*arch* (arm64 vs x64). Handled by the same
  libm-standardization track plus same-arch matches.

## How to run locally

```bash
# Linux
./builddir/CortexCommand -scenario SimBaseline -seed 42 -max-ticks 600 -tick-hashes -num-lua-states 4 -out lin.json
# Windows
& ".\Cortex Command.exe" -scenario SimBaseline -seed 42 -max-ticks 600 -tick-hashes -num-lua-states 4 -out win.json
# Diff the per-tick totals
diff <(jq -r '.runs[0].tick_hashes[].total' lin.json) <(jq -r '.runs[0].tick_hashes[].total' win.json)
```
