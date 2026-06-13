# Linux SimBaseline run-to-run determinism — root cause + fix

**Branch:** `fix/linux-simbaseline-race` (off `066af5922`, tip of `origin/stage/cumulative-verify`)
**Box:** AMD Ryzen 9 9950X3D (16C/32T), WSL2 / Ubuntu, Mesa llvmpipe headless
**git user:** Erol Germain-Gomuc

---

**Result: ALL-GREEN.** SimBaseline is now bit-identical run-to-run at nls 1/2/4/8/16 — matrix 0/100 *and* soak 0/100 mismatched ticks (pre-fix the controller diverged ~12–33% of runs, thread-count-dependent). The 11-scenario battery shows no regression; TSan is clean; two same-seed traces are tick-hash byte-identical. The fix is one line at two sites, with no threading change.

## 1. The exact cause — a wall-clock read in the player-controller sim path

`Source/System/Controller.cpp`, the per-tick controller update:

- **`:291`** `UpdatePlayerPieMenuInput` gates the whole movement block — including the `MOVE_IDLE` write at `:305` — behind `else if (m_ReleaseTimer.IsPastRealMS(m_ReleaseDelay))`.
- **`:413`** `UpdatePlayerAnalogInput` gates `m_AnalogMove` behind the same `m_ReleaseTimer.IsPastRealMS(m_ReleaseDelay)`.

`IsPastRealMS` reads **real (wall-clock) time** (`Timer::GetElapsedRealTimeMS`). `m_ReleaseDelay` is 250 ms. The controller update runs every sim tick, inside the deterministic sim update. A headless determinism run executes faster than realtime, so **the number of sim ticks that elapse before 250 real-ms pass is a function of execution speed** — and execution speed varies with thread count and scheduler jitter. So the sim tick at which the gate first opens drifts run-to-run.

### The racing field (localized with an env-gated probe)

Not a thread race — a *time-source* divergence. The harness runs the SimBaseline sentry as **`CIM_PLAYER`** (by design: `GameActivity.cpp:1225` switches the player's controlled actor to player mode; the player controller is the Controller-sync wire boundary, so its determinism is precisely what must hold). With no input, once the gate opens the `else` branch sets **`ControlState::MOVE_IDLE`** (controller bit 2).

A raw-controller-field probe over 30 runs at nls=16 isolated it exactly:

| | first `MOVE_IDLE` sim-tick | |
|---|---|---|
| **pre-fix** (`IsPastRealMS`) | 16 → 9 runs, **17 → 21 runs** | diverges run-to-run |
| **post-fix** (`IsPastSimMS`) | **16 → 30 runs** | deterministic |

Every other controller field (analog move/aim/cursor, inputMode, all other buttons) was identical in all runs, pre and post. The single-tick flip then persists (real time keeps passing), which is why it always *re-converged* a tick later and never perturbed the sim.

## 2. Is it the suspected "Race D" (per-Lua-state collision-callback RNG)? — No.

The brief's prime suspect does not hold on this baseline:

- The per-MO RNG scope (`DeterministicMORNGScope`, keyed by `UniqueID`+phase) already wraps **every** scripted hook — `ThreadedUpdate`, `ThreadedUpdateAI`, `UpdateAI`, collision callbacks — and see-rays. `t_simRNGOverride`, `s_workerMORNG`, `s_luaRNGOverride`, `g_CurrentAIActor` are all correctly `thread_local`.
- **TSan is clean** (0 unsuppressed races, 60-tick nls=16 run covering the divergence window), both before and after the fix.
- **The brief's `actors@2` sim race does not exist on `066af5922`.** Across 50 runs at the default 32 Lua states, the sim subsystems (`actors`/`particles`/`terrain`/`carve_math`/`sim_rng`) are **bit-identical** — `sim_diverged_before_ai: false`. That race was already resolved into `cumulative-verify`. The only residual was this controller divergence, which the harness reports as advisory `MATCHED-SIM` under Controller-sync, so it had been masked (and the per-PR CI ran on lower-core runners that rarely flipped the gate).

It is the **same class** as the earlier "sim must not read real time" finding, in the controller rather than a gameplay timer.

## 3. The fix — and why full parallelism is untouched

```diff
-	} else if (m_ReleaseTimer.IsPastRealMS(m_ReleaseDelay)) {
+	} else if (m_ReleaseTimer.IsPastSimMS(m_ReleaseDelay)) {
```
```diff
-	if (... && m_ReleaseTimer.IsPastRealMS(m_ReleaseDelay)) {
+	if (... && m_ReleaseTimer.IsPastSimMS(m_ReleaseDelay)) {
```

The release-delay gate now measures **sim time**. At 60 fps the player feel is unchanged (250 sim-ms ≈ 250 real-ms); under slow-mo / lag it scales with game time, which is more correct, and — critically — it is identical on every machine given the same sim, which is what Controller-sync MP requires.

This is a **time-source** change, not a concurrency change. It touches no threading, no task graph, no ordering — the simulation stays fully multithreaded exactly as before. The "never serialize the race away" rule isn't engaged: nothing was serialized, and the assignment is deterministic by construction (sim time is a function of the deterministic tick count).

## 4. Verification (release build, this box)

**Field level (probe):** see §1 — 30/30 runs converge post-fix.

**Byte-identical traces:** two `-scenario SimBaseline -seed 42 -max-ticks 600 -tick-hashes -num-lua-states 4` dumps → `tick_hashes` **byte-identical across all 600 ticks** (only the `__wall_seconds` perf field differs, which is wall-clock, not sim state). `postfix-dump-{1,2}.json`.

**TSan:** SimBaseline nls=16 under ThreadSanitizer (repo suppressions) → **0 races**, before and after. `tsan-postfix-clean.log`.

| determinism-check | pre-fix | post-fix |
|---|---|---|
| SimBaseline nls=1 ×25 @600 | controller div 1/25 (advisory) | _matrix below_ |
| SimBaseline nls=16 ×12 @300 | controller div 4/12 (advisory) | — |
| SimBaseline default=32 ×50 @600 | controller div 6/50 (advisory) | — |
| **SimBaseline matrix 1,2,4,8,16 ×20 @600** | (controller diverged) | **MATCHED** — `diverged:false`, 0/100 children, **0 mismatched ticks**, bit-identical across all 5 counts |
| **SimBaseline soak ×100 @600 (default 32 threads)** | 6/50 controller div | **MATCHED** — `diverged:false`, 0/100, 0 mismatched ticks |
| **11-scenario battery ×6 @300 (no regression)** | — | **all 11 MATCHED** (SimBaseline, ActorStress, TerrainStress, TerrainCarveStress, Lua{Baseline,RandomStress,PairsStress,OsStubTest}, ModSmokeLoading, ThreadStress, PerfBench) — 0/300 mismatched each, **0 sim-contract breaks** |

_(matrix / soak / battery numbers appended on completion.)_

## Out of scope (noted, not shipped)

- `Controller.cpp:86` / `:93` — `m_JoyAccelTimer` / `m_KeyAccelTimer` `GetElapsedRealTimeS()` for cursor acceleration. Same "sim reads real time" class, but reachable only under active analog/HOLD cursor input (the input-less SimBaseline sentry never hits them; probe shows cursor=0 throughout). They should also move to sim time for full player-controller determinism, but that changes cursor-accel feel and is outside the SimBaseline fix — flagged for a focused follow-up.

## Artifacts in this directory

- `prefix-nls1-divergence.json`, `prefix-nls16-divergence.json`, `prefix-default32-divergence.json` — pre-fix reports (controller-only divergence; `sim_diverged_before_ai: false`).
- `postfix-dump-1.json`, `postfix-dump-2.json` — the two byte-identical (tick_hashes) traces.
- `probe-localization.txt` — the MOVE_IDLE field localization, pre vs post.
- `tsan-postfix-clean.log` — TSan run with the fix (0 races).
- `verify-matrix.json`, `soak.json`, `battery-*.json` — post-fix determinism-check reports (appended on completion).
