# Source/Network — determinism scaffold + the M1 determinism check

This directory holds the M0 / M1 determinism observability infrastructure plus
the future home of the MP M6+ networking stack. As of M1 (Block F) it contains:

- **`SimChecksum.{h,cpp}`** — per-tick BLAKE3 state hasher with per-subsystem
  breakdown. The total-tick hash is `BLAKE3(concat over sorted subsystems of
  (name || subsystem_hash))`. Subsystems wired through M1: `tick`, `terrain`,
  `decisions`, `actors`, `sim_rng`. M2 adds `carve_math`; M5 grows to whole-
  tick (`particles`, `controller`, `lua_states`, scene metadata).
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
| `sim_rng`    | M1 Block F | Full `g_SimRNG` mt19937 internal state, end-of-tick     | `Main.cpp` end-of-tick block |
| `carve_math` | (M2)     | Deterministic terrain carve / penetrate / dislodge math  | —                            |
| `particles`  | (M5)     | Per-particle stable state                                | —                            |
| `controller` | (M5/M6)  | Per-player controller state                              | —                            |
| `lua_states` | (M5)     | Per-Lua-state math.random RNG state                      | —                            |

## CI

`.github/workflows/determinism.yml` runs the check on every PR to
`modernization-effort` and `flagship/mp-m1-determinism-cleanup`. As of M1
Block F the gate is **informational** — the divergence report is uploaded
as an artifact (`determinism-<scenario>-<os>`) but the job is marked
`continue-on-error: true` so the workflow doesn't block merges. M1's
within-OS divergence has been narrowed from "every tick diverges" at the
start of Block A to "first divergence at tick ~16 in a small fraction of
runs," but the last thread-race (Lua-called C++ helpers transitively
touching `g_SimRNG` from worker threads during `ThreadedUpdate` /
`SyncedUpdate`) remains. **MP M4 closes that race** as part of its
deterministic-merge work; the gate flips to blocking then.
