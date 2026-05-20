# Source/Network — determinism scaffold + the M1 determinism check

This directory holds the M0 / M1 determinism observability infrastructure plus
the future home of the MP M6+ networking stack. As of M1 (Block F) it contains:

- **`SimChecksum.{h,cpp}`** — per-tick BLAKE3 state hasher with per-subsystem
  breakdown. The total-tick hash is `BLAKE3(concat over sorted subsystems of
  (name || subsystem_hash))`. Subsystems wired: `tick`, `terrain`, `decisions`,
  `actors`, `sim_rng`, `particles`, `scene` (M0 + M1), `lua_state` (M2). Not yet
  wired: `carve_math` (M3), `controller` (M7+).
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
   - `terrain` diverging means terrain pixels diverged. At M1 this hash
     is a placeholder (just the sim tick number) — divergence here would
     mean `tick` schema is broken too. M2 wires the real terrain hash.

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
| `terrain`    | M0       | Sim frame number (placeholder; real carve hash at M2)    | `Main.cpp` end-of-tick block |
| `decisions`  | M0       | Drained `AIDecisionChannel::Event` records               | `MovableMan::Update` drain   |
| `actors`     | M1 Block F | Per-actor {uniqueID, pos, vel, health, AIMode}, in MOID order | `MovableMan::Update`     |
| `particles`  | M1 Block F follow-up | Per-particle {uniqueID, pos, vel}, in MOID order      | `MovableMan::Update`         |
| `scene`      | M1 Block F follow-up | Tick metadata: actor/item/particle counts + per-team roster size | `MovableMan::Update` |
| `sim_rng`    | M1 Block F | Full `g_SimRNG` mt19937 internal state, end-of-tick (snapshot inside `MovableMan::Update` before async futures launch) | `MovableMan::Update` |
| `lua_state`  | M2       | Per-Lua-state RNG, master then threaded states in order  | `MovableMan::Update`         |
| `carve_math` | (M3)     | Deterministic terrain carve / penetrate / dislodge math  | —                            |
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
