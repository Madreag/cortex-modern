# M1 Determinism — baseline divergence (Block A landing state)

This doc captures the state of per-tick determinism **at the moment Block A
(CI scaffold) lands**, before any of Blocks B–F have run. It is the "starting
point" reading against which the subsequent blocks are measured.

> Run locally: see [the determinism-check usage](#how-to-run-locally) below.

## Why Block A's test is expected to fail

The plan (D:\Projects\M1_PLAN.md §4 Block A) explicitly states:

> At commit A, the test will **FAIL** (most likely) because current code has
> known non-determinism sources (RNG sharing, iteration order, wall-clock).
> That's fine. The PR description should document the baseline divergence rate
> per subsystem. **Commits B through F drive that rate to zero.**

The CI workflow (`.github/workflows/determinism.yml`) is therefore configured
as **non-blocking** at Block A (`continue-on-error: true`). Block F flips it
to blocking once Blocks B–E have stabilised the system.

## Known non-determinism sources (pre-Block-B)

These are the four classes the M1 blocks remove. Each Block B–F closes one
category. Until they land, the per-tick hash trace from the M1Baseline scenario
will diverge across same-seed runs.

| Class                                | Subsystem(s) most affected      | Closed by |
|--------------------------------------|---------------------------------|-----------|
| Sim/render RNG share one generator   | `decisions`, `actors`, terrain  | Block B   |
| Container iteration is hash-ordered  | `actors`, `decisions`           | Block C   |
| Wall-clock reads in sim paths        | `decisions`                     | Block D   |
| Compiler-driven FP rearrangement     | `terrain`, `actors`             | Block E   |
| All of the above lock in CI gate     | (whole-tick)                    | Block F   |

> The exact divergence rate depends on the host CPU, OS, and scenario length.
> Numbers below are illustrative of what the user should expect to see — run
> the tool locally to get the precise starting point on your machine.

## Expected starting state (M1Baseline, 600 ticks, 10 runs)

Approximate baseline — to be filled in with the actual first-divergence
output from a Block A local run:

```text
[determinism-check] scenario: M1Baseline
[determinism-check] runs    : 10
[determinism-check] RESULT  : DIVERGED
    first_divergence_tick: <T>
    total_mismatched_ticks: <M> / 600
    per-subsystem first divergence:
        decisions: tick <T_dec>
        terrain:   tick <T_terrain>
        tick:      n/a  (sim frame number is sourced from g_TimerMan and is
                         identical across same-seed runs — included to prove the
                         trace mechanism itself isn't the culprit)
```

The exact `tick` subsystem (sim frame number) should match across all runs
already at Block A — that's the sanity check that the hash trace is plumbed
correctly. If `tick` ever diverges, the M1Baseline JSON didn't carry the
expected schema; investigate before chasing real determinism work.

## What "good" looks like (post-Block-F at M1, fully deterministic at M4)

```text
[determinism-check] RESULT: MATCHED (600 ticks across 100 runs)
```

At M1 Block F's landing, the M1Baseline trace is **close** to this — but a
residual thread-race in `MovableMan::Update`'s `ThreadedUpdate` /
`SyncedUpdate` paths (Lua-called C++ helpers transitively touching
`g_SimRNG` from worker threads) keeps the M1Baseline test from MATCHED on
every single run. Empirically the first divergence has been pushed out from
tick 1 (Block A baseline) to tick ~16 in roughly 1 of 5 runs.

The Block F changes that bought that improvement:
  * `actors` + `sim_rng` subsystem feeds (catches the drift directly).
  * Content-based decisions sort + field-by-field hash (drops the
    process-lifetime `sequence` atomic and the intern-order-dependent
    `StringId`s out of the hash bytes).
  * Wait for the previous frame's see-ray future BEFORE Travel() (closes
    the most obvious see-ray ↔ sim-thread race).
  * Snapshot `sim_rng` state inside `MovableMan::Update`, before the next
    frame's see-ray + MOID-draw futures launch (closes the same-tick race).

The remaining race is genuine threading non-determinism in code paths Lua
reaches through. MP M4 (multiplayer.html §7) is the milestone that
restructures the threaded-sim path to deterministic-merge; that's when the
last drops fall out of the trace and the CI gate flips to blocking.

If you're picking up M4: the failing-with-low-frequency tick is the smoking
gun. Capture a `--keep-runs` trace and bisect on the first diverging
subsystem. `sim_rng` diverging while `actors` matches at the same tick
indicates a "consumer that doesn't affect actor state" (e.g.,
cosmetic-classified sim path); `actors` diverging without prior `sim_rng`
divergence indicates a non-RNG state path (probably also threading).

## How to run locally

### Windows

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

### Inspecting the per-run JSONs

Pass `--keep-runs` to retain the per-run JSON files in
`<tmp>/cccp-determinism-<timestamp>/runs/run_<i>.json`. The path is printed at
the end of the run.

Each per-run JSON has a `tick_hashes` array under `runs[0]`, where each entry
is `{tick: N, total: "<hex>", subsystems: {"name": "<hex>", ...}}`. A small
`jq` invocation surfaces the trace:

```bash
jq -r '.runs[0].tick_hashes[] | "\(.tick) \(.total)"' run_0.json
```

## References

- `D:\Projects\M1_PLAN.md` — the M1 plan (Block A §4)
- `Source\CI\DeterminismCheck.{h,cpp}` — the comparator implementation
- `Source\AI\MetricsCollector.{h,cpp}` — `RecordTickHash` + JSON emission
- `Source\System\ScenarioRunner.{h,cpp}` — `-tick-hashes` flag plumbing
- `Source\Main.cpp` — the `-determinism-check` short-circuit + `RecordTickHash` hook
- `Data\Tests.rte\Activities\M1Baseline.lua` — the baseline scenario
- `Data\Tests.rte\Activities.ini` — preset registration
- `.github/workflows/determinism.yml` — the non-blocking CI step
