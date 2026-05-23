# M5 bring-up — Windows x86-64

This is the Windows-x86-64 arm of the M5 bring-up — the first formal capture of
a Windows determinism baseline against the post-fix `exp/determinism-foundation`
tip (commit `c7d38d87a`, which integrates the macOS-arm64 bring-up and the
SeedRNG honor-CLI fix `6537ca214`). The build is green on MSBuild Final x64,
the eleven M-series scenarios run to completion, ten of the eleven are
bit-identical run-to-run, and the M4 thread-count matrix is bit-identical
across 1/2/4/8/16 Lua-state counts. Cross-platform trace diffing is the team's
job — the per-tick traces produced here live under
`Source/CI/windows-x86_64-traces/` for that comparator step.

Host: Intel Core i7-4790K (4C/8T Haswell @ 4.00 GHz), 31.9 GB RAM, Windows 11
Pro 10.0.26100, MSVC `19.44.35227` (Visual Studio Build Tools 2022 17.14, MSBuild
17.14.40), Windows SDK 10.0.26100.0. Target: `x86_64-pc-windows-msvc`.

Branch: `exp/determinism-windows` off `exp/determinism-foundation`
(`c7d38d87a102788f46326aa375cabb7f436b4799`).

## TL;DR

| Surface | Status |
|---|---|
| MSBuild Final x64 | green |
| `FixedPointTests` (Q40.24 self-check) | 42 / 42 — SELF-CHECK: `5ce9c33b84d29932` |
| `M1Baseline` @ `--runs 10` `--ticks 600` | MATCHED |
| `M1TerrainStress` @ `--runs 10` `--ticks 900` | MATCHED |
| `M1ActorStress` @ `--runs 10` `--ticks 900` | MATCHED |
| `M2LuaBaseline` @ `--runs 10` `--ticks 600` | MATCHED |
| `M2LuaRandomStress` @ `--runs 10` `--ticks 600` | MATCHED |
| `M2PairsStress` @ `--runs 10` `--ticks 600` | MATCHED |
| `M2OsStubTest` @ `--runs 10` `--ticks 600` | MATCHED |
| `M2ModSmokeLoading` @ `--runs 10` `--ticks 600` | MATCHED |
| `M3TerrainStress` @ `--runs 10` `--ticks 900` | MATCHED |
| `M4ThreadStress` @ `--runs 10` `--ticks 900` | MATCHED |
| `MPerfBench` @ `--runs 10` `--ticks 1200` | DIVERGED — *by design, not a determinism scenario; see notes* |
| `M4ThreadStress` thread-count matrix `--threads 1,2,4,8,16` `--runs 2` `--ticks 900` | MATCHED across all five counts |

All ten genuine determinism scenarios are bit-identical run-to-run at
`--runs 10 --seed 42`. `MPerfBench` is a wall-clock perf-regression benchmark
that explicitly disclaims determinism in its scenario header (see *Known
issues / follow-ups*); it was not run on the macOS-arm64 or Linux-x86_64
bring-up branches either. `FixedPointTests` reports the same Q40.24
`SELF-CHECK` hash as the macOS-arm64 and Linux-x86_64 bring-ups
(`5ce9c33b84d29932`) — the Q40.24 library is portable as designed.

The `M4ThreadStress` thread-count matrix passing across 1/2/4/8/16 on this
host is **stronger** than the Block A Windows baseline captured in
`Source/CI/baseline_thread_divergence.md` (which had a residual at ~tick 136
post-Block-F). See *M4 thread-count matrix* below for the host-specific
caveat.

## Build environment

Toolchain installed for this bring-up (no Visual Studio was present on the
host):

```pwsh
winget install --id Microsoft.VisualStudio.2022.BuildTools `
    --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --quiet --wait --norestart" `
    --silent --accept-source-agreements --accept-package-agreements
```

That installed VS Build Tools 2022 17.14 with the Desktop C++ workload, MSVC
`14.44.35207` (cl.exe `19.44.35227`), MSBuild 17.14.40, and Windows SDK
10.0.26100.0. No IDE, headless build chain only.

Build command:

```pwsh
$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
    -latest -products * -requires Microsoft.Component.MSBuild `
    -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
& $msbuild .\RTEA.sln /p:Configuration=Final /p:Platform=x64 /m /nologo /v:minimal
```

(The agent prompt's `-latest -requires Microsoft.Component.MSBuild` form
returns blank for a Build Tools install — `-products *` is required because
Build Tools is not a "main" Visual Studio product. The direct path
`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe`
also works.)

Build result on this host: `Cortex Command.exe` (12.1 MB, Final x64, LTCG /
WholeProgramOptimization) produced in the repo root in ~2 min 16 s wall
(`20:11:01 → 20:13:17`). Zero errors. Eighteen warnings, all pre-existing in
the tree:

* 10 × `MSB4011` — `RTEA.common.props` / `Microsoft.Cpp.Default.props`
  imported twice from `RTEA.vcxproj`. The agent prompt notes these as
  harmless.
* 1 × `C4312` — `Source/Renderer/BigTexture.cpp:136` `reinterpret_cast` int →
  void* (renderer-side, pre-existing).
* 2 × `C4297` — vendored `external/sources/luabind-0.7.1` `proxy_function_void_caller`
  destructor noexcept assumption (vendored library, pre-existing).
* 4 × `C4244` — vendored `external/sources/raylib` `rtextures.c:5063` float →
  unsigned char (vendored library, pre-existing).
* 1 × `C4090` — vendored `external/sources/raylib` `rlgl.c:3914` const
  qualifier drop (vendored library, pre-existing).

No new warnings. No source changes needed for the build itself.

### Runtime DLL deployment

`Cortex Command.exe` links statically against most of its dependencies (no
`SDL3.dll`, no `OpenGL32.dll` import) but does import `fmod.dll`. On this
host that one DLL had to be copied next to the binary:

```pwsh
Copy-Item .\external\lib\win\fmod.dll .\fmod.dll
```

Without it, the binary exits immediately with
`STATUS_DLL_NOT_FOUND (0xC0000135)` — the determinism check's first child
spawn produces no output and the orchestrator reports nothing. The repo
`.gitignore` excludes `*.dll`, so `fmod.dll` is intentionally not committed
next to the EXE; a release packager would copy it as part of binary
deployment. Flagged as a small follow-up — see *Known issues*.

The Microsoft Visual C++ Runtime DLLs (`MSVCP140.dll`, `VCRUNTIME140.dll`,
`VCRUNTIME140_1.dll`, `MSVCP140_ATOMIC_WAIT.dll`) and the Universal CRT
`api-ms-win-crt-*.dll` set were already present in `C:\Windows\System32`
under Windows 11 — no separate VCRedist install needed.

## `FixedPointTests` (Q40.24 fixed-point self-check)

Built standalone via the existing `FixedPointTests.vcxproj`:

```pwsh
& $msbuild .\Source\System\FixedPointTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
```

The vcxproj already sets the right toolchain options for cross-OS hash
parity: `<LanguageStandard>stdcpp20`, `<FloatingPointModel>Precise`,
`<EnableEnhancedInstructionSet>StreamingSIMDExtensions2`,
`<ConformanceMode>true`. No source change needed.

Run output:

```
FixedPoint.h tests (MP M3 Block A) — Q40.24 fixed-point
  round-trip max error: 5.913e-08
  Sqrt max error: 5.960e-08
  Sin max error: 1.400e-07   Cos max error: 1.585e-07
  Atan2 max error: 2.091e-07

SELF-CHECK: 5ce9c33b84d29932
42 passed, 0 failed
```

The `SELF-CHECK` hash matches the macOS-arm64 bring-up
(`Source/CI/m5-macos-arm64.md` §`FixedPointTests self-check`) and the
Linux-x86_64 bring-up (`Source/CI/m5-linux-x64.md` §`FixedPointTests`) bit
for bit. The Q40.24 library is pure integer code (`Fixed`, `Sqrt`, `Sin`,
`Cos`, `Atan2`, `FixedVector` — no `libm`), so it is expected to hash
identically on every platform; the CI `fixedpoint-tests-cross-os` job is
the comparator that gates this for the team. All three platforms now
report the same hash — that's exactly the design contract.

The float-error rows above are bookkeeping, not a determinism signal — the
hash is taken over integer state. (The values match the macOS bring-up
numbers because the per-platform float reference is built from the same
constexpr table on every host.)

## Same-platform determinism

Each scenario was run through the determinism harness on this machine:

```pwsh
& ".\Cortex Command.exe" -determinism-check `
    --scenario <S> --ticks <T> --seed 42 --runs 10 `
    --output "Source\CI\windows-x86_64-traces\<S>-determinism.json"
```

`diverged: false` means the per-tick BLAKE3 trace is bit-identical
run-to-run on Windows x86-64. All ten genuine determinism scenarios passed.

### Per-scenario results (`--runs 10`, `--seed 42`)

| Scenario             | Ticks | Result   | Wall (s) | Single-run `final_total_hash` |
|----------------------|------:|----------|---------:|---|
| `M1Baseline`         |   600 | MATCHED  |      194 | `813efb81c88a4a0d1b9fdc5d992ff92c576eae4b2c2a543cac15064dd307e2cc` |
| `M1TerrainStress`    |   900 | MATCHED  |      290 | `ca20707a342cd07ebef927f254e33dc98f7fe1ff8858ed67d7c35230f3121862` |
| `M1ActorStress`      |   900 | MATCHED  |      287 | `7201a6e8dd85dcb6ff857408141faec27df96d0f167547687944c52a4da4064c` |
| `M2LuaBaseline`      |   600 | MATCHED  |      169 | `f59d89ebda75bff3a065e567fd7b606d935133421725b989be86352486a0d0eb` |
| `M2LuaRandomStress`  |   600 | MATCHED  |      170 | `786976916f6c9757f91b6b079ba188e47775fbdaceb82f04650aca9e7fb586c6` |
| `M2PairsStress`      |   600 | MATCHED  |      170 | `71bf55388518e2c4cf6a009cf2a9e09c6a74d0b697b31df166f8ab431609be15` |
| `M2OsStubTest`       |   600 | MATCHED  |      170 | `4083460c210b5a89fe9a3bea9a4ecc8da1036dd249aa12de1caa3db0b921d01b` |
| `M2ModSmokeLoading`  |   600 | MATCHED  |      183 | `19a525e81824bd9c367c2a1b8d737596a8eaa62ac164ed9e9651d41aec951486` |
| `M3TerrainStress`    |   900 | MATCHED  |      289 | `43e5349ed0c36272ccd3939bf6cac905a68fcacfd4906ba81f02bbcf291a5702` |
| `M4ThreadStress`     |   900 | MATCHED  |      301 | `90c75ebf3f644c1f1dd08767daf44b05b38113dab8bb1c6202205a329ccd4b13` |
| `MPerfBench`         |  1200 | DIVERGED |      515 | `917ed7ebf6663536ac0e383a946f2ece60d281b19e4b53676a36ffae923e3759` (single-run only) |

Sweep wall time: 55 min total (`20:20:10 → 21:15:19`) covering Phase 1
(11 × `--runs 10` determinism checks), Phase 2 (M4 thread-count matrix), and
Phase 3 (11 single-run per-tick `-tick-hashes` traces). Per-run cost on this
host averaged ~19 s for 600-tick scenarios and ~29 s for 900-tick scenarios —
~3.1× slower than the macOS-arm64 bring-up's M-series M3 host (10–15 s/run),
consistent with the Haswell-vs-M3 single-core gap. No source changes were
needed for any of the matching scenarios; the foundation tip's existing
Block B/C/D/E/F + macOS-merge fixes already cover everything exercised on
this host.

The single-run `final_total_hash` column is the cross-platform reference
hash for the team's per-tick comparator step (the same hashes are also the
last entry in each scenario's `<scenario>-trace.json` under
`runs[0].final_total_hash`). Note that on Windows x86-64 these are the
**first post-seed-fix** baselines — the macOS-arm64 traces under
`Source/CI/macos-arm64-traces/*.json` and the Linux-x86_64 traces under
`Source/CI/M*-trace.json` were produced before commit `6537ca214` and so
reflect the engine's `constSeed` path regardless of the `-seed 42` they
were invoked with. Until those two platforms refresh their baselines,
cross-OS hash equality at `-seed 42` only holds for the Q40.24
`FixedPointTests` SELF-CHECK above; the per-scenario hashes here are the
authoritative Windows-x86_64 reference for the next refresh cycle.

### `M4ThreadStress` thread-count matrix

```pwsh
& ".\Cortex Command.exe" -determinism-check `
    --scenario M4ThreadStress --ticks 900 --seed 42 `
    --threads 1,2,4,8,16 --runs 2 `
    --output "Source\CI\windows-x86_64-traces\M4ThreadStress-threadmatrix.json"
```

`RESULT: MATCHED (900 ticks across 10 runs at thread counts 1,2,4,8,16)`.

All 10 child processes (5 thread counts × 2 runs) produced the same 900-tick
trace. `diverged: false` on the matrix report.

This is **stronger** than the Block A Windows landing state captured in
`Source/CI/baseline_thread_divergence.md` §`Expected baseline divergence`,
which documented the matrix as `DIVERGED` at `first_divergence_tick: 0`,
and stronger than the `M4 outcome (Block F)` post-block-F state that
documented a residual cross-thread-count divergence around tick 136 (the
collision-callback / per-Lua-state RNG window characterised there as an
M4-follow-up).

Why the residual didn't reproduce here is **host-dependent, not a fix**.
This box has `hardware_concurrency() == 8` (i7-4790K, 4 physical / 8
logical cores), so when the matrix forces `--threads 16` the engine's
`SettingsMan::SetNumberOfLuaStatesOverride(16)` raises `m_ScriptStates`
above the priority pool's thread count; per `MovableMan.cpp` the
`parallelize_loop` num_blocks pin to `luaStates.size()` (commit
`5` in the macOS-arm64 bring-up) collapses the per-state task width, and
the actor-to-state assignment is also serial-style at 16. Pair that with
`thread_count(==8) < 16` and the threaded-pass concurrency is bounded
below the 16-state ceiling on every count. The residual at ~tick 136 in
the Block F report was measured on a wider box where 16 states genuinely
ran on ≥16 threads. The matrix MATCHED on this host should therefore be
read as "this host doesn't exercise the failing path" rather than "the
M4-follow-up residual is closed". The team's cross-platform run on a
≥16-thread Windows host (the `determinism-windows` CI runner, when one is
provisioned) is the authoritative re-test.

The `M4ThreadStress --runs 10` repeat-runs result (above) and the matrix
result here together confirm that **same-thread-count** determinism is
solid on Windows — the within-OS scheduling race M1 documented and Blocks
B–F closed is, as expected, still closed.

## `MPerfBench` — DIVERGED by design, not a determinism scenario

`MPerfBench` is the **only** scenario in the M-series that diverged. It
diverged at `first_divergence_tick: 3` in the `decisions` subsystem; the
per-subsystem first-divergence map:

```text
controller:  7
decisions:   3        ← earliest
sim_rng:    38
scene:      38
carve_math: 39
terrain:    39
actors:     35
particles:  35
```

This is **expected**: `Data/Tests.rte/Activities/MPerfBench.lua` opens
with

```lua
-- MPerfBench: heavy compute-bound performance-measurement scenario.
-- One hundred-fifty BRAINHUNT actors armed with SMGs and frag grenades open
-- the run, with six reinforcement waves dropping in over the 1200 ticks. The
-- goal is that one sim tick's compute consistently exceeds the real-time frame
-- budget so the sim is compute-bound for most of the run -- only then does
-- __wall_seconds reflect the build's actual per-tick sim cost rather than
-- TimerMan's idle-sleep. [...]
--
-- Not a determinism scenario.
```

The scenario is a wall-clock perf-regression benchmark, explicitly carved
out from the determinism set. Its self-check is structural (`ran the full
1200 ticks, all six reinforcement waves dropped, ≥250 actors total
spawned`) and does **not** check per-tick hashing; the single-run trace
self-check `passed=true` on this host (`final_tick=1199`, 280 actors
total spawned, all waves dropped). The 150-actor compute-bound brawl
exercises the collision-callback RNG draws characterised in
`Source/CI/baseline_thread_divergence.md` §`Out of M4 scope — documented
residual`:

> The synchronous Lua collision callbacks `OnCollideWithMO` /
> `OnCollideWithTerrain` (`MovableObject.cpp:766,778`) fire from inside
> serial `Travel()`. They are serial and deterministically ordered, so
> they are not a *threading* race. They do, however, read `math.random`
> from the colliding MO's per-Lua-state RNG, whose state index is
> thread-count-dependent — so a mod that draws RNG inside a collision
> callback has a residual count-dependency.

`MPerfBench`'s grenade detonations, SMG fire and dropped-weapon physics
fire collision callbacks at a rate well above any of the other M-series
scenarios. The `decisions` subsystem leading at tick 3 (AI decision
output) is consistent with the same-tick AI re-evaluations driven off
collision-callback RNG draws — the documented M4-follow-up territory.

The macOS-arm64 (`Source/CI/m5-macos-arm64.md`) and Linux-x86_64
(`Source/CI/m5-linux-x64.md`) bring-ups did **not** run `MPerfBench`
through the determinism harness — both reports cover the nine M1/M2/M3
scenarios plus the M4 matrix, and neither references the perf-bench
scenario. The agent brief for this Windows bring-up included `MPerfBench`
in its eleven-scenario list, so the determinism-check output is captured
here as `MPerfBench-determinism.json` (`diverged: true`) for honest
accounting; the single-run `MPerfBench-trace.json` is committed alongside
as the wall-clock-bench reference trace (the entire point of the
scenario, per its header). No source change is applied — the divergence
is by-design and the in-scenario disclaimer is authoritative.

If the team wants `MPerfBench` to be removed from a future determinism
sweep list, the right place is the harness wrapper / CI job's scenario
enumeration; the scenario itself is doing the right thing.

## Cross-platform notes

### Q40.24 fixed-point — bit-identical across all three platforms

| Platform | Compiler | `FixedPointTests` SELF-CHECK |
|---|---|---|
| Windows x86-64 (this bring-up) | MSVC `19.44.35227` | `5ce9c33b84d29932` |
| Linux x86-64 (`m5-linux-x64.md`) | GCC 13.3 | `5ce9c33b84d29932` |
| macOS arm64 (`m5-macos-arm64.md`) | Apple Clang 17.0.0 | `5ce9c33b84d29932` |

The Q40.24 library is portable as designed. The CI
`fixedpoint-tests-cross-os` job's contract is now satisfied on every
host the team has tested.

### Per-scenario `final_total_hash` — Windows baselines are the first post-fix reference

Per the agent brief, the per-tick BLAKE3 traces committed under
`Source/CI/macos-arm64-traces/*.json` and the Linux baselines committed
flat under `Source/CI/M*-trace.json` were produced **before** commit
`6537ca214` ("Harness: SeedRNG honors CLI -seed"), so they reflect the
engine's `constSeed` initialisation regardless of the `-seed 42` they
were invoked with. The Windows traces in this bring-up are the **first**
post-fix baselines: `cccp-determinism-check --seed 42` actually drives
`g_SimRNG.seed(42)` from the harness path now (and not the prior
constSeed escape hatch), so the per-scenario `final_total_hash` values in
the table above are the new reference any cross-OS comparator should
diff against, once macOS-arm64 and Linux-x86_64 refresh their own traces
on top of `c7d38d87a` (or later).

To make the gap concrete on the canonical baseline:

| `M1Baseline` final hash | Source |
|---|---|
| `813efb81c88a4a0d1b9fdc5d992ff92c576eae4b2c2a543cac15064dd307e2cc` | Windows x86-64 post-fix (this bring-up, `-seed 42`) |
| `c7282c4487df76c6829dedd1140a16cd9ded1894329e1c1d34837cc8db8a7520` | macOS arm64 stale (`macos-arm64-traces`, `constSeed`-driven) |
| `afe157dc46d16bdc36a67a205cdf2ed4ac340abac90b2c58997e851f2243df65` | Linux x86-64 stale (`Source/CI/M1Baseline-trace.json`, `constSeed`-driven) |

The three disagree by construction (different effective seed; macOS
arm64 also differs by FP ABI per `m5-macos-arm64.md` §`Cross-platform
divergence characterisation`). After the macOS-arm64 and Linux-x86_64
bring-ups refresh their traces on top of foundation, the team's
cross-OS comparator step should expect:

* `tick / scene / sim_rng / lua_state` per-subsystem hashes to be
  bit-identical Windows-x86_64 ↔ Linux-x86_64 (integer-only state — any
  difference is a real determinism bug, most likely an LP64 vs LLP64
  integer-width drift).
* The float-touching subsystems (`actors`, `controller`, `particles`,
  `decisions`, `carve_math`, `terrain`) to be bit-identical
  Windows-x86_64 ↔ Linux-x86_64 (same FP ABI baseline: SSE2 IEEE 754
  doubles, MSVC `/fp:precise` and GCC `-fno-fast-math -fno-associative-math`
  both disallow reassociation and contraction). A mismatch there is a
  real bug, not a cross-OS expectation.
* macOS-arm64 may legitimately diverge from x86-64 on the float-touching
  subsystems for the reasons documented in
  `Source/CI/m5-macos-arm64.md` §`Cross-platform divergence
  characterisation` (ARM-default FMA contraction, integer arithmetic on
  `__int128` for `file_clock` etc.). The macOS report's
  `-ffp-contract=off` block on the Meson side is what closes that on
  Apple Clang.

## Known issues / follow-ups

* **`fmod.dll` deployment.** The Final x64 binary imports `fmod.dll`
  (`external/lib/win/fmod.dll`) but the build does not copy it next to
  `Cortex Command.exe`. On a fresh checkout the binary fails to start
  with `STATUS_DLL_NOT_FOUND` until the file is manually copied. The
  `.gitignore` excludes `*.dll`, so the copy can't be committed; a
  release packager / build post-step would handle this. Small follow-up
  for whichever team owns the Windows release pipeline — could be a
  trivial `<PostBuildEvent>` in `RTEA.vcxproj`.
* **Pre-existing build warnings.** Eighteen warnings, all enumerated
  above, all in tree or vendored code. None are determinism-relevant.
  Cleaning the `RTEA.common.props` import-twice (the ten `MSB4011`
  ones) would be the cheapest win — the renderer / luabind / raylib
  ones are vendored.
* **`MPerfBench`.** Diverged at tick 3 by design. The scenario script
  declares "Not a determinism scenario"; the prior platform bring-ups
  excluded it from their determinism tables. The agent brief listed it
  in the eleven-scenario sweep, so the determinism-check output is
  committed here for honest accounting; no fix is applied. If a future
  CI step wants to skip it from the determinism gate, the right surface
  is the harness wrapper, not the scenario.
* **M4 thread-count matrix residual.** The matrix MATCHED on this
  4-physical-core / 8-logical-core host, but the residual at ~tick 136
  documented in `baseline_thread_divergence.md` §`M4 outcome (Block F)`
  was measured on a wider box. The team should re-run the matrix on a
  ≥16-thread Windows host (the future `determinism-windows` CI runner)
  to know whether the residual is genuinely closed by the macOS-merge
  set or just dormant on this host. Linux x86-64
  (`m5-linux-x64.md` §`Notes`) suggested the async-pathfinder fix on
  `exp/determinism-linux` (`679b53c84`) is a plausible partial cause —
  that fix is **not** on `exp/determinism-foundation` yet, so it does
  not influence the Windows result here.
* **VS Build Tools install path quirk.** The agent brief's `vswhere
  -latest -requires Microsoft.Component.MSBuild` form returns blank for
  a Build Tools install (no IDE); the form has to be
  `vswhere -latest -products * -requires Microsoft.Component.MSBuild`
  to find the MSBuild that ships with Build Tools. A two-line update to
  the brief would save the next bring-up agent a debugging round.

## Conclusion

**10 / 10 genuine determinism scenarios MATCHED on Windows x86-64.**
`MPerfBench` diverged by design and is documented above; the M4
thread-count matrix MATCHED across all five counts on this host. The
Q40.24 `FixedPointTests` hash is bit-identical with the macOS-arm64 and
Linux-x86_64 bring-ups. Per-tick traces for all eleven M-series scenarios
plus the M4 thread-count matrix are committed under
`Source/CI/windows-x86_64-traces/` as the post-seed-fix reference for the
team's cross-OS comparator.

**Gate recommendation: enable** — the Windows side of the determinism
foundation is solid against the eleven-scenario brief, with `MPerfBench`
carved out as the by-design exception and the M4-follow-up residual
explicitly flagged for re-test on a wider host. No source fix was needed
for this bring-up.

## Files added / committed

* `Source/CI/windows-x86_64-traces/` — 23 per-scenario JSON artefacts:
  * `<scenario>-determinism.json` × 11 (`--runs 10` divergence reports).
  * `<scenario>-trace.json` × 11 (single-run per-tick BLAKE3 traces).
  * `M4ThreadStress-threadmatrix.json` (`--threads 1,2,4,8,16 --runs 2`).
* `Source/CI/m5-windows-x86_64.md` — this report.
