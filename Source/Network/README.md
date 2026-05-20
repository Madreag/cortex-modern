# Source/Network — determinism scaffold + the M1 determinism check

This directory holds the M0 / M1 determinism observability infrastructure plus
the future home of the MP M6+ networking stack. As of M1 (Block F) it contains:

- **`SimChecksum.{h,cpp}`** — per-tick BLAKE3 state hasher with per-subsystem
  breakdown. The total-tick hash is `BLAKE3(concat over sorted subsystems of
  (name || subsystem_hash))`. Subsystems wired: `tick`, `terrain`, `decisions`,
  `actors`, `sim_rng`, `particles`, `scene` (M0 + M1), `lua_state` (M2),
  `carve_math` (M3). Not yet wired: `controller` (M7+).
- **`NetworkSimulator.{h,cpp}`** — latency / loss / jitter knobs plumbed
  through SettingsMan. No-op at M0; MP M1 plugs the real transport on top.
- **`ReplayLog.{h,cpp}`** — per-tick replay log (scenario, seed, timeline,
  player controller bytes). Frame markers populated at M0; controller bytes
  populated at MP M1 onward.

## Running the determinism check locally

The check tool runs as a pre-init mode of the main game binary, so it doesn't
engage the engine bootstrap (no allegro / SDL / module load / GL context). On
both platforms the same flags work:

| Flag                  | Meaning                                                                 | Default       |
|-----------------------|-------------------------------------------------------------------------|---------------|
| `--scenario <name>`   | Activity preset suffix (e.g. `M1Baseline`). Required.                   | —             |
| `--ticks <N>`         | Per-run sim-tick cap.                                                   | 600           |
| `--seed <S>`          | Deterministic seed used for every run.                                  | 42            |
| `--runs <R>`          | Number of independent runs to compare. Must be ≥ 2.                     | 10            |
| `--output <path>`     | JSON divergence report path.                                            | `determinism-report.json` |
| `--game-bin <path>`   | Override the binary to spawn. Default: argv[0].                         | argv[0]       |
| `--keep-runs`         | Keep the per-run JSONs under `<tmp>/cccp-determinism-<ts>/` for inspection. | off       |

Both single-dash (`-scenario`) and double-dash (`--scenario`) forms are
accepted for parity with the rest of the engine's CLI.

### Windows (PowerShell)

```pwsh
& ".\Cortex Command.exe" -determinism-check `
    --scenario M1Baseline `
    --ticks 600 `
    --seed 42 `
    --runs 10 `
    --output determinism-report.json
```

### Linux

```bash
./CortexCommand -determinism-check \
    --scenario M1Baseline \
    --ticks 600 \
    --seed 42 \
    --runs 10 \
    --output determinism-report.json
```

### Output

```text
[determinism-check] binary  : .../Cortex Command.exe
[determinism-check] scenario: M1Baseline
[determinism-check] ticks   : 600
[determinism-check] seed    : 42
[determinism-check] runs    : 10
[determinism-check] tmp dir : .../cccp-determinism-<timestamp>
[determinism-check] run 1/10: "..." -scenario M1Baseline -seed 42 -max-ticks 600 -tick-hashes -out "..."
[determinism-check] run 2/10: ...
...
[determinism-check] wrote report: determinism-report.json
[determinism-check] RESULT: MATCHED (600 ticks across 10 runs)
```

If runs diverge, RESULT becomes `DIVERGED` and the report lists:

```json
{
  "scenario": "M1Baseline",
  "seed": 42,
  "ticks_requested": 600,
  "runs": 10,
  "compared_ticks": 600,
  "diverged": true,
  "total_mismatched_ticks": <M>,
  "first_divergence_tick": <T>,
  "per_subsystem_first_divergence": {
    "decisions": <T_dec>,
    "actors":    <T_act>,
    "sim_rng":   <T_rng>,
    ...
  },
  "per_run_tick_count": [600, 600, 600, ...]
}
```

## Debugging a determinism failure

1. **Look at `per_subsystem_first_divergence`** — the subsystem with the
   earliest tick is the proximate cause. The conventions:
   - `tick` diverging means the hash schema itself broke — the trace
     mechanism is the culprit, not the sim. Investigate `SimChecksum` /
     `MetricsCollector::RecordTickHash` plumbing first.
   - `sim_rng` diverging means the sim consumed a different number of RNG
     draws across runs. Usual cause: a code path crossed sim/render RNG
     wires after Block B (look for `RandomNum` calls in newly-added cosmetic
     code that should use `g_RenderRNG`).
   - `actors` diverging means actor state diverged. Usually downstream of
     `sim_rng` (RNG-driven AI behavior) or `decisions`.
   - `decisions` diverging means the AI emitted different events. Usually
     downstream of `sim_rng` or iteration-order drift (Block C).
   - `terrain` diverging means terrain pixels diverged. M3 wired this to the
     real material + foreground-colour bitmaps; divergence is normally
     downstream of `sim_rng` (raced carve inputs), not the carve math.
   - `carve_math` diverging means a penetration decision differed. The math
     is fixed-point — bit-identical given identical inputs — so this is
     downstream of `sim_rng`; check `carve_math`'s tick is not earlier.

2. **Re-run with `--keep-runs`** to retain the per-run JSON traces, then
   inspect the first diverging tick across runs:

   ```bash
   jq -r '.runs[0].tick_hashes[<T>]' /tmp/cccp-determinism-<ts>/run_0.json
   jq -r '.runs[0].tick_hashes[<T>]' /tmp/cccp-determinism-<ts>/run_1.json
   # spot the per-subsystem hex that disagrees.
   ```

3. **Bisect**: cut `--ticks` in half until the divergence disappears.
   That pins the failure to a 1-tick window. Then look at what the sim
   was doing at that tick — usually a Lua AI hook, an actor death/spawn,
   or a particle settling.

4. **Bisect on history**: if a previously-clean PR introduced divergence,
   `git bisect` against `./Cortex\ Command.exe -determinism-check --scenario
   M1Baseline --ticks 600 --seed 42 --runs 5 --output /tmp/probe.json` will
   surface the offending commit. The blocking CI gate (Block F) should
   prevent this, but the local probe is useful for in-flight branches.

## Subsystem schema reference

| Subsystem    | Wired at | What's fed in                                            | Source                       |
|--------------|----------|----------------------------------------------------------|------------------------------|
| `tick`       | M0       | Sim frame number (uint64)                                | `SimChecksum::BeginTick`     |
| `terrain`    | M0 / M3  | Material + FG-colour bitmaps, end-of-tick (M3; tick-number placeholder before) | `SceneMan::FeedTerrainToSimChecksum` |
| `decisions`  | M0       | Drained `AIDecisionChannel::Event` records               | `MovableMan::Update` drain   |
| `actors`     | M1 Block F | Per-actor {uniqueID, pos, vel, health, AIMode}, in MOID order | `MovableMan::Update`     |
| `particles`  | M1 Block F follow-up | Per-particle {uniqueID, pos, vel}, in MOID order      | `MovableMan::Update`         |
| `scene`      | M1 Block F follow-up | Tick metadata: actor/item/particle counts + per-team roster size | `MovableMan::Update` |
| `sim_rng`    | M1 Block F | Full `g_SimRNG` mt19937 internal state, end-of-tick (snapshot inside `MovableMan::Update` before async futures launch) | `MovableMan::Update` |
| `lua_state`  | M2       | Per-Lua-state RNG, master then threaded states in order  | `MovableMan::Update`         |
| `carve_math` | M3       | Per penetration decision: pos, material, result, fixed-point impulse / velocity / retardation | `SceneMan` penetration path |
| `controller` | (M7+)    | Per-player controller state                              | —                            |

## Lua determinism contract

M2 fences the Lua language environment so a Lua call inside a sim tick
reproduces across runs. What is guaranteed, and what is not:

- **`math.random`** — every Lua state's RNG is reseeded from `g_SimRNG` at
  every activity start, so the same activity replays the same sequence.
  `math.randomseed` still works; a mod that calls it takes over its own RNG
  state (the `lua_state` subsystem surfaces any drift this causes).
- **`pairs()`** — iterates a sorted snapshot of the table's keys: numeric
  keys ascending, then string keys lexicographically. The snapshot is taken
  when `pairs()` is called — keys added mid-iteration are not visited, and a
  key removed mid-iteration is still visited (its value reads as nil).
  Non-primitive keys (table / function / userdata) compare equal, so their
  relative order is unspecified. `pairs_unordered` is the original
  hash-order builtin, kept for mods with hot pairs loops that want it back.
- **`os.time` / `os.clock`** — return sim-tick seconds, not the wall clock.
- **`next`** — *not* patched. `for k, v in next, t do` still iterates in
  hash order. Use `pairs()` for ordered iteration.

A `lua_state` subsystem divergence means a Lua state consumed a different
number of RNG draws across runs. Usual causes: a mod calling
`math.randomseed`, or actor AI Lua hitting the threaded-sim race (closes at
MP M4 — see below). The modder-facing summary lives in
`Data/Modding/lua-determinism.md`.

## Fixed-point determinism (M3)

M3 converts the terrain-destruction math to integer fixed-point, so it is
bit-identical across compilers and OSes by construction — no `/fp` flags, no
libm. `Source/System/FixedPoint.h` is the Q40.24 library: `Fixed` (int64, 24
fractional bits), `FixedWide` (128-bit, for squared magnitudes / dot products
kept wide), `FixedVector`, and integer `Sqrt` / `Sin` / `Cos` / `Atan2`.
`FixedPointTests` — a standalone binary, built by
`Source/System/FixedPointTests.vcxproj` or the Meson `FixedPointTests` target —
unit-tests the library and prints a self-check hash that CI compares
Windows↔Linux.

**Converted — the terrain-destruction path.** `SceneMan::WillPenetrate` /
`TryPenetrate` / `DislodgePixel`, the `DislodgePixelCircle/Ring/Box/Line`
geometry helpers, and `SLTerrain::EraseSilhouette`'s sizing math. Float storage
is untouched — `Vector`, `Material`, the INI format and the Lua API are
unchanged; the conversion happens inside the functions, float↔Fixed at the
boundary. A mod that carved terrain before M3 carves the same terrain after,
only now identically on every machine.

**Converted — the atom collision response (Block D).** `Atom::MOHitResponse` /
`Atom::TerrHitResponse` / `Atom::Travel`, and `AtomGroup::GetMomentOfInertia` /
`Travel` / `ResolveMOSIntersection` / `PushTravel` compute their restitution,
friction, impulse, moment-of-inertia and segment-trajectory math in fixed-point.
The libm transcendentals (`GetMagnitude` / `Normalize` / `SetMagnitude` /
`RadRotate` / `CapMagnitude`) become `FixedPoint` integer routines. The
Bresenham stepping stays integer; the `HitData` struct stays float (it crosses
the Lua `OnBounce` / `OnSink` / `OnMOHit` callback boundary) and is read/written
at the fixed-point boundary, which is a defined deterministic conversion.

**Debugging a `carve_math` divergence.** The penetration math is fixed-point —
bit-identical given identical inputs — so a `carve_math` divergence is a
divergent *input*, not divergent math. If `carve_math`'s first-divergence tick
is earlier than `sim_rng`'s, a stray float slipped onto the converted path
(grep the penetration functions for `float` / `std::sqrt`). If it is not, the
divergence is inherited from the upstream `sim_rng` thread race — MP M4's to
close. The modder-facing summary is in `Data/Modding/fixed-point-physics.md`.

## CI

`.github/workflows/determinism.yml` runs the check on every PR to
`modernization-effort` and the MP milestone branches. The divergence report
is uploaded as an artifact (`determinism-<scenario>-<os>`).

All scenarios are **informational** (`continue-on-error`). The per-tick total
hash combines every subsystem, and M1's `sim_rng` / `actors` / `particles` /
`scene` subsystems still carry the residual sim-thread race (Lua-called C++
helpers transitively touching `g_SimRNG` from worker threads during
`ThreadedUpdate` / `SyncedUpdate`). No scenario reaches a whole-tick MATCH yet
— M1's own `M1Baseline` diverges `sim_rng` at tick 1 in both the M1 and M2
builds. **MP M4 closes that race**; the gate flips to blocking then.

M2's Lua determinism is verified in the meantime by the `decisions` subsystem:
`M2LuaRandomStress` / `M2PairsStress` / `M2OsStubTest` fold each tick's
`math.random` sequence, `pairs()` order, and `os.*` values into a decision
event, and that subsystem does not diverge across runs — the Lua-language
guarantees hold even while the inherited sim race keeps `total` diverging.
