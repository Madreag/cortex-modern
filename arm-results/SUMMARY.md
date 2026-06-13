# macOS arm64 determinism traces — lean (no detmath) vs full (detmath) — 6/13/2026

**Question under test:** on Apple Silicon, is the determinism sim core bit-identical to x86
*without* the vendored-musl `detmath` libm layer (lean), or is `detmath` needed to close an
arm64 libm/FMA gap (full)? This run produces the macOS arm64 traces; the cross-*architecture*
diff (arm64 vs x86) is completed on the x86 side against these artifacts.

## Host / toolchain
- **Host:** Mac Mini M4 Pro · macOS 15.7.3 (Darwin 24.6.0) · Apple Silicon **arm64**
- **Toolchain:** Apple clang **17.0.0** (clang-1700.6.3.2) · meson **1.11.1** · ninja **1.13.2**
- **Build config:** `meson setup builddir --buildtype=release` (b_lto default = off). The FP
  contract is pinned unconditionally for clang (`-ffp-contract=off -fno-fast-math` …), so arm64
  FMA contraction is already disabled on both branches — the one remaining cross-arch variable
  is the libm transcendentals, which is exactly what `detmath` standardizes.
- **Runtime:** runs go headless via the branches' committed `__APPLE__` path
  (`CCCP_HEADLESS=1` → hidden **real-GL** window, FMOD/audio active; SDL offscreen loads no GL on
  Darwin). `GLAD: ERROR 1280 in glGetIntegerv` in the logs is a benign headless-GL warning — every
  run completes with `passed=yes`.
- **No local overlay applied.** The macOS bring-up patches (libc++ `_LIBCPP_ENABLE_CXX*_REMOVED_*`
  macros + the `__APPLE__` headless arm) are **already committed on both test branches**, so
  `git reset --hard origin/<branch>` was a clean, faithful build of origin.

## Build status — both branches PASS
| Branch | HEAD | Commit subject | ninja | Targets |
|---|---|---|---|---|
| `test/arm-detmath-lean` | `9bcbcf926` | Determinism keep-set (no detmath): portable mt19937 hash + RNG mapping + Allegro integer fixmul/fixdiv | **exit 0** | 545/545 |
| `test/arm-detmath-full` | `988e49a4f` | PR 12 (full): libm standardization via vendored musl detmath | **exit 0** | 577/577 |

**The detmath subproject compiles cleanly on arm64/clang** (the flagged risk did *not*
materialize). `libdetmath.a` is built from the musl objects (`musl_sin.c.o`, `musl_cos.c.o`,
`musl_tan.c.o`, `musl_pow.c.o`, `musl_exp.c.o`, `musl_log.c.o`, …) and linked into the binary; the
`include_shim/` covers the `<features.h>`/`<endian.h>` gap on Darwin. Build log: **0 `error:`**,
97 `warning:` (all benign, pre-existing third-party: allegro unused-var/shift-negative, musl
`libm.h` unused-var).

## Step-2 same-arch A/B verdict (each scenario run twice; A must equal B)
Exit codes — **all 0** on both branches (Sim-A, Sim-B, Terr-A, Terr-B):

| Branch | scenario | ticks | exit A/B | A/B verdict (task script) |
|---|---|---|---|---|
| lean | SimBaseline   | 600 | 0 / 0 | **DIVERGE@15** — controller-only jitter |
| lean | TerrainStress | 900 | 0 / 0 | **BIT-IDENTICAL** (900/900) |
| full | SimBaseline   | 600 | 0 / 0 | **DIVERGE@15** — controller-only jitter |
| full | TerrainStress | 900 | 0 / 0 | **DIVERGE@14** — controller-only jitter |

### These A/B "divergences" are a benign, branch-independent harness artifact — NOT a sim race
Every A/B difference is confined to the **`controller`** subsystem, lasts **1–2 ticks**, and
**self-heals**; the deterministic sim core (`sim_rng`, `actors`, `terrain`, `scene`, `carve_math`,
`particles`, `lua_state`, `tick`) is **bit-identical run-to-run on both branches**. A real
nondeterminism would propagate into `sim_rng`/physics (cf. the repo's positive control, which
spreads to 251/300 ticks); this does not.

It is a startup-timing jitter in the controller's first activation, landing at slightly different
ticks across runs (harness/window-event timing), independent of branch:
- **SimBaseline** (single one-time idle→active controller transition), activation tick over 5 runs
  each: **lean = [15,16,16,16,16]**, **full = [15,16,16,16,16]** — i.e. 4/5 land at tick 16, 1/5 at
  tick 15. *(Run A is the tick-15 early outlier in both branches.)*
- **TerrainStress** (controller updates every tick), first controller diff-vs-run-A over 5 runs:
  **full = [—,14,12,13,14]**, each touching only 1–2 ticks; **sim core == run A for all 5 runs.**
  `lean` Terr A/B happened not to catch the jitter in its 2 runs (low-probability early landing).

**Guidance for the cross-arch diff:** exclude the `controller` subsystem from cross-arch
equivalence (or tolerate jitter in roughly the first 16 ticks). Compare the sim-core subsystems /
`total` from tick ≥ ~16. The pushed `*-Sim-A.json` traces carry the tick-15 (minority) controller
activation; the modal mac value is tick 16.

## Does detmath change arm64 results?  (lean-arm64 A vs full-arm64 A, per-subsystem)
- **SimBaseline — IDENTICAL.** lean and full are **byte-identical across all 600 ticks, every
  subsystem.** On arm64, Apple libm ≡ musl detmath for everything SimBaseline exercises ⇒ detmath
  is a **no-op for SimBaseline** here.
- **TerrainStress — DIFFERS.** lean ≠ full, first divergence per subsystem:
  `particles@77`, `carve_math@79`, `actors@82`, then `sim_rng@482`, `scene@482`, `terrain@482`
  (`lua_state`, `tick` unaffected; `controller@14` is the jitter above). Terrain carving exercises
  transcendentals (sin/cos/atan2/pow/…) where Apple libm ≠ musl, and the difference propagates.

## Implication for the headline question *(arm64-side evidence; confirm against x86 traces)*
- **SimBaseline:** detmath changes nothing on arm64 (lean-arm64 ≡ full-arm64), so the scenario's
  hashes do not depend on divergent transcendentals → expected to match x86 **with or without**
  detmath. detmath not needed for Sim-class scenarios.
- **TerrainStress:** detmath *does* change arm64 output. Without it, arm64 uses Apple libm (≠ the
  x86 libm); with it, both arches use the same vendored musl. So **detmath is the likely
  requirement to close the arm64↔x86 gap for transcendental-heavy scenarios (terrain carving).**
  The x86-side diff against these traces is what confirms it.

## Artifacts in this directory
8 traces — A **and** B for each (branch × scenario), so the same-arch A/B verdict is reproducible
from the artifacts alone:
`{lean,full}-mac-Sim-{A,B}.json` (600t), `{lean,full}-mac-Terr-{A,B}.json` (900t).
Each is `runs[0].tick_hashes[]` with per-tick `total` + `subsystems` (the format the
`determinism-cross-os` diff consumes).
