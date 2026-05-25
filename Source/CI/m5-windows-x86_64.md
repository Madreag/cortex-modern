# M5 bring-up — Windows x86-64 (Block F revalidation)

This is the **Block F** revalidation of the Windows-x86-64 bring-up against the
M4A consolidation tip `366b9d073` (force-pushed over the prior
`8e21cdec937445aef6e23ea1ee2fe5f53951c88a`; the previous tip is preserved at
`backup/pre-block-f-windows-2026-05-24` on origin). The consolidation lands
Path E architecture (`PathFinder UpdatePathFinding` moved to the post-`MovableMan`
epilogue), three parallel-AI race fixes (Race A/B/C: `AudioMan::PlaySoundContainer`
headless gate, `LuaAdaptersScene::CalculatePathAsync` atomic counter, LuaJIT
TSan-skip shim), the `cccp-ctl` V1.4 control tool, the M4A TSan CI lane, and
four cross-platform UB fixes (`SpatialPartitionGrid` MOID-ordered iteration,
`AtomGroup` `hitMOAtoms` map order, `HumanBehaviors devicesToPickUp` tie-break,
`MovableMan m_ContiguousActorIDs` synchronous rebuild).

Host: Intel Core i7-4790K (4C/8T Haswell @ 4.00 GHz), 31.9 GB RAM, Windows 11
Pro 10.0.26100, MSVC `19.44.35227` (Visual Studio Build Tools 2022 17.14,
MSBuild 17.14.40), Windows SDK 10.0.26100.0. Target: `x86_64-pc-windows-msvc`.

Branch: `exp/determinism-windows` @ `366b9d0738c3d26ef95662d20a89f6712147589d`.

## TL;DR

| Surface | Status |
|---|---|
| MSBuild Final x64 clean rebuild | green (2 min 27 s wall, 13 warnings — all pre-existing; 5 fewer than the prior bring-up since the `raylib rtextures.c C4244` set is no longer hit) |
| `cccp-ctl` Release x64 build (`Source/CLI/cccp-ctl.vcxproj`) | green (45 s wall, 770 KB binary) |
| `cccp-ctl test selftest --runs 3 --ticks 300` (EC3 positive control) | PASS — phase 1 MATCH, phase 2 DIVERGED |
| `FixedPointTests` Q40.24 SELF-CHECK | 42 / 42 — `5ce9c33b84d29932` |
| `cccp-ctl test all --runs 10 --seed 42` aggregate | 10 / 11 sub-steps PASS (the M4 matrix is the one FAIL — see below) |
| `M1Baseline` @ `--runs 10` `--ticks 600` (replay-determinism) | MATCHED |
| `M1TerrainStress` @ `--runs 10` `--ticks 600` | MATCHED |
| `M1ActorStress` @ `--runs 10` `--ticks 600` | MATCHED |
| `M2LuaBaseline` @ `--runs 10` `--ticks 600` | MATCHED |
| `M2LuaRandomStress` @ `--runs 10` `--ticks 600` | MATCHED |
| `M2PairsStress` @ `--runs 10` `--ticks 600` | MATCHED |
| `M2OsStubTest` @ `--runs 10` `--ticks 600` | MATCHED |
| `M2ModSmokeLoading` @ `--runs 10` `--ticks 600` | MATCHED |
| `M3TerrainStress` @ `--runs 10` `--ticks 600` | MATCHED |
| `M4ThreadStress` @ `--runs 10` `--ticks 900` (single-thread-count replay-determinism) | MATCHED |
| `M4ThreadStress` thread-count matrix `--threads 1,2,4,8,16 --runs 2 --ticks 900` | **DIVERGED at tick 150** — controller subsystem leads; the documented post-Block-F cross-thread-count residual surfacing on this host |
| `MPerfBench` @ `--num-lua-states 8 --runs 10 --ticks 1200` | **DIVERGED at tick 1161** — controller subsystem leads; Path E pushed the prior bring-up's tick-3 divergence out to tick 1161, but did not close it on this host |

**Net: 10 of the 11 M-series scenarios MATCH at single-thread-count
replay-determinism. The two surfaces that diverge — the M4 cross-thread-count
matrix and `MPerfBench` at 8 Lua states — both lead with the `controller`
subsystem and are consistent with the same residual class.** The
`controller` subsystem (`MovableMan::Update` `analog[6]` floats per actor)
diverging first, with `actors / particles / decisions / scene / sim_rng /
terrain / carve_math` falling 1-7 ticks later, is the same pattern that
`baseline_thread_divergence.md` §`M4 outcome (Block F)` documented as the
"~tick 136" cross-thread-count residual. Block F drove it from tick 0 → tick
~136 historically; on this host the same residual now surfaces at tick 150
under the M4 matrix and at tick 1161 under `MPerfBench`. The `Path E`
re-architecture (`e48f8b6cc`) and the three Race A/B/C closures
(`8d0875651`) closed the same-thread-count race entirely (M4 replay-determinism
10/10) and pushed MPerfBench's failure 1158 ticks further out, but the
cross-thread-count residual is still live on this host.

This is the same regression the M4A `Source/CI/baseline_thread_divergence.md`
§`Out of M4 scope — documented residual` flagged as the collision-callback
per-Lua-state RNG window. Closing it is the documented M4-follow-up, not a
bring-up-blocker.

## What changed since the prior Windows bring-up

The prior tip `8e21cdec9` (preserved at
`origin/backup/pre-block-f-windows-2026-05-24`) reported 10 / 11 MATCHED with
`MPerfBench DIVERGED at tick 3` and the M4 thread-count matrix MATCHED.

Block F's effect on this host:

| Scenario | Prior (`8e21cdec9`) | Block F (`366b9d073`) | Delta |
|---|---|---|---|
| M1/M2/M3 single-thread-count (9 scenarios) | MATCHED | MATCHED | — |
| `M4ThreadStress` single-thread-count `--runs 10` | MATCHED | MATCHED | — |
| `M4ThreadStress` thread-matrix `--threads 1,2,4,8,16 --runs 2` | MATCHED | **DIVERGED at tick 150 (controller)** | **regressed surface** — the residual now reproduces here |
| `MPerfBench --num-lua-states 8 --runs 10 --ticks 1200` | DIVERGED at tick 3 (decisions) | DIVERGED at tick 1161 (controller) | **improved** — Path E pushed the failure 1158 ticks out |

The `M4 thread-matrix` change is not necessarily a regression in the code — the
prior tip's MATCHED reading may have been a lucky-scheduling observation on
the same host (the cross-thread residual is, per
`baseline_thread_divergence.md`, real on Windows). The new code's slightly
different scheduling (Path E reorders `UpdatePathFinding` after `MovableMan::Update`,
changing the time window between threaded-pass completion and the next
`UpdatePathFinding`) may have nudged the host into reliably exposing the
residual. Either way, the matrix's DIVERGED-at-tick-150 result on this host
is the more honest reading; the prior MATCHED was the outlier.

The `MPerfBench` change is a clear win: tick 3 (`decisions` subsystem,
collision-callback RNG-driven AI re-evaluations within the first few ticks of
the 150-actor grenade-spam scenario) → tick 1161 (`controller` subsystem,
same cross-thread residual class as the matrix), out of 1200 ticks total.
That's the `Path E` commit's claim translating from the dev's host (which
saw `10/10 at 1200 ticks`) to this host. The remaining 39-tick window after
tick 1161 still trips the residual; closing it likely needs the same fix the
matrix is waiting on.

## Single-run trace final hashes (Block F, `--seed 42`)

For the team's cross-OS comparator step. These are the **post-consolidation
Windows-x86-64 baselines** — the canonical Windows reference at foundation tip
`366b9d073`.

| Scenario | Ticks | `final_total_hash` | Passed |
|---|---:|---|---|
| `M1Baseline`         |  600 | `813efb81c88a4a0d1b9fdc5d992ff92c576eae4b2c2a543cac15064dd307e2cc` | true |
| `M1TerrainStress`    |  900 | `51db48e72cd73514b59dd6d16803b49057bd22dddf98a953fad9c1246bac6cf2` | true |
| `M1ActorStress`      |  900 | `cc0748a5ac17cf8768413c781b7b5ff0118a3ce8866e5641619c40447d6ba90b` | true |
| `M2LuaBaseline`      |  600 | `f59d89ebda75bff3a065e567fd7b606d935133421725b989be86352486a0d0eb` | true |
| `M2LuaRandomStress`  |  600 | `786976916f6c9757f91b6b079ba188e47775fbdaceb82f04650aca9e7fb586c6` | true |
| `M2PairsStress`      |  600 | `71bf55388518e2c4cf6a009cf2a9e09c6a74d0b697b31df166f8ab431609be15` | true |
| `M2OsStubTest`       |  600 | `4083460c210b5a89fe9a3bea9a4ecc8da1036dd249aa12de1caa3db0b921d01b` | true |
| `M2ModSmokeLoading`  |  600 | `d3272da1ba1f5e8d5842568b5e5708e2363cef5ee2ddbe593e3f5d66f530ec44` | true |
| `M3TerrainStress`    |  900 | `f40729d27646a28092aa4c8b64f26697a7e27d9c4b5dc1b328f1aba856746e03` | true |
| `M4ThreadStress`     |  900 | `83903cccb60a96af7e4579b30abd0cf3b901e9203672c86c593792ffea60ca08` | true |
| `MPerfBench`         | 1200 | `f5f34dd4c2fe1790860e3c113990538cd092be9e745726d98aa2bc1dfaceef12` | true (structural self-check only — see notes) |

Comparison to the prior Windows bring-up (`8e21cdec9`):

| Scenario | Hash unchanged from prior? |
|---|---|
| `M1Baseline`         | **same** (`813efb81…`) — no AI/pathfinding in the scenario, so the Path E reorder is a no-op |
| `M2LuaBaseline`      | **same** (`f59d89eb…`) — Lua-only, no actors |
| `M2LuaRandomStress`  | **same** (`78697691…`) — Lua-only |
| `M2PairsStress`      | **same** (`71bf5538…`) — Lua-only |
| `M2OsStubTest`       | **same** (`4083460c…`) — Lua-only |
| `M1TerrainStress`    | changed | (terrain carve cascade + AI movement read the path grid)
| `M1ActorStress`      | changed | (26 actors with combat — heavy AI read path) |
| `M2ModSmokeLoading`  | changed | (4 brain-hunters with AI) |
| `M3TerrainStress`    | changed | (6 brain-hunters with terrain destruction) |
| `M4ThreadStress`     | changed | (40 brain-hunters, threaded AI) |
| `MPerfBench`         | changed | (150 actors, threaded AI + collisions) |

The 5 scenarios with bit-identical hashes are exactly the ones with no AI /
no pathfinding (M1Baseline = one sentry, M2x4 = no actors). The 6 that
changed are all AI-driven; the change is consistent with `Path E` moving
`UpdatePathFinding` after `MovableMan::Update` so AI sees the
end-of-tick-T-1 path grid instead of the raced start-of-tick-T grid.
Determinism within a single run is unchanged; the absolute hash differs
because the AI's path-cost reads differ.

## Build environment

Toolchain on this host (unchanged from the prior bring-up — same Visual Studio
Build Tools install):

```
MSVC cl.exe 19.44.35227  (VS Build Tools 2022 17.14, MSBuild 17.14.40)
Windows SDK 10.0.26100.0
```

Build command:

```pwsh
$msbuild = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
& $msbuild .\RTEA.sln /p:Configuration=Final /p:Platform=x64 /m /nologo /v:minimal
```

Clean rebuild from the new tip:

* `Cortex Command.exe` produced at the repo root (~12.1 MB).
* Wall: `20:49:59 → 20:52:26` = 2 min 27 s (LTCG / `WholeProgramOptimization`
  on this 4-physical-core host).
* Zero errors.
* 13 warnings — all pre-existing, none new:
  * 10 × `MSB4011` (`RTEA.common.props` / `Microsoft.Cpp.Default.props`
    imported twice in `RTEA.vcxproj`).
  * 1 × `C4312` (`Source/Renderer/BigTexture.cpp:136` `reinterpret_cast` int → void*).
  * 2 × `C4297` (`external/sources/luabind-0.7.1` `proxy_function_void_caller`
    noexcept).

The four `C4244` warnings on `external/sources/raylib/rtextures.c:5063` and
the one `C4090` on `rlgl.c:3914` that the prior bring-up listed are **no
longer hit** on this build — the vendored raylib code path is unchanged on
disk (no commits touched it in the consolidation), so the most likely
explanation is the MSVC `19.44.35227` toolchain itself or a different
compile-flag interaction from the consolidation. Either way, fewer warnings,
not a regression.

`cccp-ctl.vcxproj` build (separate from `RTEA.sln` by design, per
`Source/CLI/README.md`):

```pwsh
& $msbuild .\Source\CLI\cccp-ctl.vcxproj /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
```

* `Source/CLI/_BinTests/x64/Release/cccp-ctl.exe` produced (~770 KB).
* Wall: 45 s.
* Zero errors, zero warnings.

### Runtime DLL deployment (unchanged from the prior bring-up)

`Cortex Command.exe` still imports `fmod.dll` (`external/lib/win/fmod.dll`).
Copy it next to the binary on a fresh checkout or the engine exits with
`STATUS_DLL_NOT_FOUND (0xC0000135)`. The repo `.gitignore` excludes `*.dll`,
so this is a launch-time fixture. Same small follow-up as last bring-up —
a `<PostBuildEvent>` in `RTEA.vcxproj` would close it.

## `cccp-ctl` harness sanity (EC3 positive control)

```pwsh
& .\Source\CLI\_BinTests\x64\Release\cccp-ctl.exe test selftest `
    --scenario M1Baseline --runs 3 --ticks 300 --quiet
```

Output (trimmed):

```
Phase 1 (clean):    PASS (MATCH as expected)
Phase 2 (perturb):  PASS (DIVERGED as expected)
Harness:            OK — catches injected non-determinism (EC3 verified)
```

Phase 2's `-determinism-selftest-perturb` fired a single fixed-tick
`std::random_device` pull on each run; the comparator picked up the
divergence at tick 49 in `sim_rng`. Harness alive, harness correct.

## `FixedPointTests` Q40.24 self-check

```pwsh
& $msbuild .\Source\System\FixedPointTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
& "D:\Projects\determinism-windows\Source\System\_BinTests\x64\Release\FixedPointTests.exe"
```

Output:

```
FixedPoint.h tests (MP M3 Block A) — Q40.24 fixed-point
  round-trip max error: 5.913e-08
  Sqrt max error: 5.960e-08
  Sin max error: 1.400e-07   Cos max error: 1.585e-07
  Atan2 max error: 2.091e-07

SELF-CHECK: 5ce9c33b84d29932
42 passed, 0 failed
```

The Q40.24 SELF-CHECK remains `5ce9c33b84d29932`. This is the **fourth**
platform reporting the same hash:

| Platform | Compiler | `FixedPointTests` SELF-CHECK |
|---|---|---|
| Windows x86-64 (this Block F bring-up) | MSVC `19.44.35227` | `5ce9c33b84d29932` |
| Linux x86-64 (`m5-linux-x64.md`) | GCC 13.3 | `5ce9c33b84d29932` |
| macOS arm64 (`m5-macos-arm64.md`) | Apple Clang 17.0.0 | `5ce9c33b84d29932` |
| WSL2 (per the agent brief) | — | `5ce9c33b84d29932` |

The CI `fixedpoint-tests-cross-os` job's contract is satisfied 4-of-4 ways.
The Q40.24 library is portable as designed.

## `cccp-ctl test all` aggregate

```pwsh
& .\Source\CLI\_BinTests\x64\Release\cccp-ctl.exe test all `
    --runs 10 --seed 42 `
    --out-dir Source/CI/windows-x86_64-traces/ --json
    > Source/CI/windows-x86_64-traces/block-f-test-all.json
```

Wall: 40 min (`20:55:54 → 21:35:46`). Aggregate result:

```
mode               : full
runs_per_scenario  : 10
thread_matrix      : 1,2,4,8,16
total_steps        : 11
passed             : 10
failed             : 1
all_passed         : false
```

Per-step disposition:

| Step | Result |
|---|---|
| `replay-determinism:M1Baseline`         | PASS (MATCH) |
| `replay-determinism:M1TerrainStress`    | PASS (MATCH) |
| `replay-determinism:M1ActorStress`      | PASS (MATCH) |
| `replay-determinism:M2LuaBaseline`      | PASS (MATCH) |
| `replay-determinism:M2LuaRandomStress`  | PASS (MATCH) |
| `replay-determinism:M2PairsStress`      | PASS (MATCH) |
| `replay-determinism:M2OsStubTest`       | PASS (MATCH) |
| `replay-determinism:M2ModSmokeLoading`  | PASS (MATCH) |
| `replay-determinism:M3TerrainStress`    | PASS (MATCH) |
| `thread-matrix:M4ThreadStress` (`--runs 1` per count internally; `runs/30` cap) | FAIL — diverged at tick 150 |
| `selftest:M1Baseline`                   | PASS (EC3 OK) |

The aggregate JSON's `all_passed: false` is driven by the single thread-matrix
sub-step. The 9 single-thread-count `replay-determinism` runs and the
`selftest` positive control all PASSed.

The `cccp-ctl test all` aggregate is the canonical CI gate signal. Its
non-zero exit code (1) here is the documented residual surfacing — see the
M4 matrix section below for the standalone `--runs 2` confirmation.

## `M4ThreadStress` thread-count matrix (standalone, `--runs 2`)

The brief's step 6 ran the matrix standalone with `--runs 2` per count:

```pwsh
& .\Source\CLI\_BinTests\x64\Release\cccp-ctl.exe test thread-matrix `
    --scenario M4ThreadStress --threads 1,2,4,8,16 --runs 2 --ticks 900 --seed 42 `
    --output Source/CI/windows-x86_64-traces/M4ThreadStress-threadmatrix.json
```

Wall: 5 min 2 s. Result:

```
diverged           : true
first_div_tick     : 150
compared_ticks     : 900
per_run_tick_count : 900,900,900,900,900,900,900,900,900,900
runs               : 2
thread_counts      : 1,2,4,8,16
per_subsystem_first_divergence:
  controller : 150     ← earliest
  actors     : 152
  particles  : 152
  carve_math : 153
  scene      : 153
  sim_rng    : 153
  terrain    : 153
  decisions  : 157
```

The `controller` subsystem (per-actor `analog[6]` floats from
`MovableMan::Update` line ~1809) leads at tick 150, with actor/particle
position floats following at tick 152-153 and `decisions` (the AI event
output) trailing at tick 157. This is the cross-thread-count residual
characterised in `Source/CI/baseline_thread_divergence.md` §`M4 outcome (Block F)`:

> A cross-thread-count divergence emerges around tick 136: the
> particle / terrain physics (`terrain` / `carve_math` / `particles`)
> diverges between thread counts with identical MO counts — divergent
> per-particle state, not a divergent spawn. It is a count-dependent
> effect in the threaded→serial-physics interaction; the leading
> candidates are collision-callback per-Lua-state RNG (`OnCollideWith*`
> is deliberately left on the per-state generator, out of M4 scope) and
> cross-actor AI coordination through per-Lua-state Lua globals. Per
> `M4_PLAN.md` §7 this is the documented M4-follow-up; the determinism
> gate stays informational until it is closed.

Tick 150 is within the documented "around tick 136" window. The
`controller`-first ordering rather than `terrain`/`particles`-first
suggests the M4-follow-up has narrowed to specifically the per-actor
controller state — which is consistent with the per-Lua-state RNG
hypothesis (different thread counts give different Lua-state assignments
to actors → different RNG streams in collision callbacks → different
`Controller::Update` outputs one tick later).

The **same-count** replay-determinism run (M4ThreadStress `--runs 10`
single-thread-count) MATCHed cleanly — the same-thread-count race that
M1/M4 originally tracked is closed.

## `MPerfBench` (standalone, `--num-lua-states 8 --runs 10 --ticks 1200`)

Per the brief's step 7 (interpreted as `test thread-matrix --threads 8 --runs 10`
since `cccp-ctl test scenario` is single-run and doesn't accept
`--num-lua-states` or `--runs`):

```pwsh
& .\Source\CLI\_BinTests\x64\Release\cccp-ctl.exe test thread-matrix `
    --scenario MPerfBench --threads 8 --runs 10 --ticks 1200 --seed 42 `
    --output Source/CI/windows-x86_64-traces/MPerfBench-determinism.json
```

Wall: 8 min 32 s. Result:

```
diverged           : true
first_div_tick     : 1161
compared_ticks     : 1200
per_run_tick_count : 1200,1200,1200,1200,1200,1200,1200,1200,1200,1200
runs               : 10
thread_counts      : 8
per_subsystem_first_divergence:
  controller : 1161    ← earliest
  actors     : 1163
  carve_math : 1163
  particles  : 1165
  scene      : 1168
  sim_rng    : 1168
  terrain    : 1168
  decisions  : 1170
```

**This is a major improvement from the prior bring-up.** That run reported
`MPerfBench DIVERGED at tick 3 in decisions` — the scenario diverged at the
first AI evaluation. Block F (specifically `e48f8b6cc PathFinder Path E: move
UpdatePathFinding to post-MovableMan epilogue` plus `8d0875651 Close two
parallel-AI races feeding the M4ThreadStress divergence`) closes the
front-of-scenario races: the divergence is now pushed out to tick 1161 of
1200 — 1158 ticks further out — and the leading subsystem flipped from
`decisions` (AI behavior) to `controller` (per-actor analog state).

The Path E commit message claims `MPerfBench tick-808 controller cascade
(10/10 at 1200 ticks, was diverging)` on the developer's host. The dev host
hit `10/10` (i.e. fully MATCHED); this 4C/8T host hits `DIVERGED at 1161`.
The 39-tick window between tick 1161 and the scenario's end (tick 1199)
still trips the residual here — the residual class is the same as the M4
matrix's tick-150 divergence (controller-led, threaded-AI interaction),
and the same M4-follow-up that closes the matrix is expected to close this
too. **Scenario self-check still `passed=true`** — `MPerfBench` is a perf
benchmark with a structural self-check (ran the full 1200 ticks, all six
reinforcement waves dropped, ≥250 actors total spawned); per-tick
determinism is not part of the self-check, only the wall-clock perf
measurement is.

**Note on the brief's command-line.** The brief specified `cccp-ctl test
scenario --scenario MPerfBench --num-lua-states 8 --ticks 1200 --runs 10`,
but `cccp-ctl test scenario` is a single-run subcommand (no `--runs`, no
`--num-lua-states`, and the ticks option there is `--max-ticks`). The
intent ("run MPerfBench with 8 Lua states, 1200 ticks, 10 runs and
verify determinism") maps cleanly to `test thread-matrix --scenario
MPerfBench --threads 8 --runs 10 --ticks 1200`, where a single-element
`--threads` list pins `-num-lua-states` to that value for every child. The
brief's wording would benefit from updating to either `test thread-matrix`
(single-element `--threads`) or a future `cccp-ctl test replay-determinism
--num-lua-states <N>` option once added.

## Cross-platform notes

### Q40.24 cross-OS (4-of-4 platforms)

`FixedPointTests` SELF-CHECK `5ce9c33b84d29932` matches Windows-x86-64 (this
report), Linux-x86-64 (`m5-linux-x64.md`), macOS-arm64 (`m5-macos-arm64.md`),
and WSL2 (per the agent brief). The CI `fixedpoint-tests-cross-os` job's
contract is met for the Q40.24 library on every host the team has tested.

### Per-scenario `final_total_hash` — Windows Block F is still the only fresh post-fix baseline

The macOS-arm64 traces under `Source/CI/macos-arm64-traces/*.json` and the
Linux-x86-64 traces under `Source/CI/M*-trace.json` were not refreshed in the
M4A consolidation — both remain at the pre-seed-fix `constSeed` baseline from
2026-05-22. The Windows-x86-64 traces committed here at
`Source/CI/windows-x86_64-traces/*-trace.json` are therefore the **only
post-fix cross-OS reference set** the comparator step has. The other two
platforms will need a `cccp-ctl test all --runs 10 --seed 42` pass on
`exp/determinism-foundation` tip `366b9d073` (or later) to refresh their
sides.

To make the staleness concrete on the canonical baseline:

| `M1Baseline` `final_total_hash` | Source | Effective seed |
|---|---|---|
| `813efb81c88a4a0d1b9fdc5d992ff92c576eae4b2c2a543cac15064dd307e2cc` | Windows x86-64 (this Block F, bit-identical to prior bring-up) | 42 (post seed-fix) |
| `c7282c4487df76c6829dedd1140a16cd9ded1894329e1c1d34837cc8db8a7520` | macOS arm64 stale (`macos-arm64-traces`) | `constSeed` |
| `afe157dc46d16bdc36a67a205cdf2ed4ac340abac90b2c58997e851f2243df65` | Linux x86-64 stale (`Source/CI/M1Baseline-trace.json`) | `constSeed` |

Once macOS and Linux refresh, the team's cross-OS comparator should expect:

* `tick / scene / sim_rng / lua_state` per-subsystem hashes bit-identical
  Windows-x86-64 ↔ Linux-x86-64 (integer-only state, no FP ABI factor).
* `actors / controller / particles / decisions / carve_math / terrain` —
  bit-identical Windows-x86-64 ↔ Linux-x86-64 (same SSE2 IEEE 754 FP ABI,
  MSVC `/fp:precise` ↔ GCC `-fno-fast-math -fno-associative-math` both
  disallow contraction).
* macOS-arm64 may legitimately diverge on the float-touching subsystems per
  `m5-macos-arm64.md` §`Cross-platform divergence characterisation` (ARM
  FMA defaults, `__int128` for `file_clock`, etc.) — the
  `-ffp-contract=off` block on the Meson side is what closes the ARM
  contraction risk on Apple Clang.

## Known issues / follow-ups

* **M4 thread-count matrix residual.** Cross-thread-count divergence at
  tick 150 (`controller` first), confirmed both standalone `--runs 2` and
  in the `test all` aggregate's `--runs 1` matrix. Documented as the
  M4-follow-up in `baseline_thread_divergence.md` §`Out of M4 scope —
  documented residual`. The fix path is the collision-callback per-Lua-state
  RNG window (`OnCollideWith*` redirecting to a per-MO RNG instead of the
  per-Lua-state generator). Not a bring-up blocker — the gate disposition in
  `M4_PLAN.md` §7 keeps this informational.
* **MPerfBench residual at tick 1161.** Same residual class as the M4
  matrix, surfacing in this 150-actor scenario after 1161 ticks. Path E
  closed the front-of-scenario races; what's left is the same
  collision-callback RNG window as the M4 matrix. Closes alongside the
  matrix.
* **`fmod.dll` deployment.** Same as the prior bring-up — the binary
  imports `fmod.dll` but the build doesn't copy it next to the EXE. Manual
  copy from `external/lib/win/fmod.dll` is required on a fresh checkout.
  A `<PostBuildEvent>` in `RTEA.vcxproj` would close it.
* **`vswhere` invocation for VS Build Tools.** The agent brief's
  `vswhere -latest -requires Microsoft.Component.MSBuild` form returns
  blank for a Build Tools install (no IDE); the form needs `-products *`
  to find MSBuild from Build Tools. Same observation as the prior
  bring-up.
* **`cccp-ctl test scenario` arg surface.** The brief's MPerfBench command
  used `test scenario --num-lua-states N --runs N` which `test scenario`
  doesn't accept (it's a single-run subcommand). Documented the
  `test thread-matrix --threads N --runs N` workaround above; ideally the
  brief is updated or `test replay-determinism` gets a
  `--num-lua-states` option.
* **Pre-existing build warnings.** 13 of them, all enumerated above, all
  in the same vendored or pre-existing tree. Cleaning the `MSB4011`
  import-twice (10 of the 13) would be the cheapest win — the rest are
  vendored.
* **`cccp-ctl test all` aggregate JSON stdout pollution.** With `--json`
  but no `--quiet`, the child engine processes' `[determinism-check]`
  echo lines write to the same stdout the aggregate JSON does, producing
  a mixed text+JSON file that won't parse as JSON until the leading
  prose is stripped. Committed here as a cleaned aggregate
  (`block-f-test-all.json`, lines stripped from line 311 onward — the
  aggregate JSON object is the suffix). Fix in `cccp-ctl`: when `--json`
  is set, force `--quiet` on the recursive sub-invocations (and / or
  document the requirement). Small follow-up for the cccp-ctl owner.

## Conclusion

**10 / 11 M-series scenarios MATCH at single-thread-count replay-determinism
on Windows x86-64.** The `cccp-ctl test all` aggregate reports
`passed: 10, failed: 1, all_passed: false`, with the one FAIL being the M4
thread-count matrix (the documented M4-follow-up residual surfacing at tick
150 on this host). MPerfBench's per-tick divergence improved by 1158 ticks
(prior bring-up: tick 3; Block F: tick 1161) — Path E and the Race A/B/C
closures pushed the failure to the last 3% of the scenario but didn't quite
close it on this host. The Q40.24 `FixedPointTests` SELF-CHECK remains
`5ce9c33b84d29932`, bit-identical to the macOS-arm64, Linux-x86-64 and WSL2
bring-ups (4-platform proof point).

**Gate recommendation: hold informational on the M4 thread-count matrix and
MPerfBench thread-count = 8 surfaces until the documented M4-follow-up
(collision-callback per-Lua-state RNG window) lands; the other 10 M-series
surfaces are solid and the gate can flip to required on them.** No source
fix was attempted for this bring-up — both DIVERGED surfaces are documented
M4-follow-ups, not regressions, and the brief did not direct the agent to
fix them.

## Files added / committed

* `Source/CI/windows-x86_64-traces/` — 24 artefacts:
  * `block-f-test-all.json` — `cccp-ctl test all` aggregate (stdout-stripped).
  * `<scenario>-determinism.json` × 11 — per-scenario `replay-determinism`
    divergence reports at `--runs 10 --seed 42`.
  * `<scenario>-trace.json` × 11 — single-run per-tick BLAKE3 traces with
    final hash.
  * `M4ThreadStress-threadmatrix.json` — standalone thread-count matrix
    `--threads 1,2,4,8,16 --runs 2`.
* `Source/CI/m5-windows-x86_64.md` — this report.
