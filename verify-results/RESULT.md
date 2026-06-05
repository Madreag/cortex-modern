# CUMULATIVE macOS CONFIRM — 6/5/2026

**Branch:** `stage/cumulative-verify` @ `0bf49039cdb8711875d58c1625adb1eb7ce6b6ff` (`0bf49039c`)
**Host:** Mac Mini M4 Pro · macOS 15.7.3 (Darwin 24.6.0) · Apple Silicon arm64
**Toolchain:** Apple clang 17.0.0 (clang-1700.6.3.2) · meson 1.11.1 · ninja 1.13.2
**Build config:** `meson setup -Dbuildtype=release -Db_lto=false` · `debug_type=minimal` (the project's "debug-minimal"; effective: `-O3`, `-DRELEASE_BUILD`, `debug=false`) — identical to the r5/r6/r7 GREEN runs.

---

## Headline (template)

```
CUMULATIVE macOS CONFIRM — 6/5/2026
Branch: stage/cumulative-verify @ 0bf49039c | apple clang 17.0.0 | meson debug-minimal arm64 (buildtype=release, b_lto=false)
Build: PASS   BUILD PATCHES APPLIED (verbatim diffs in macos-build-patches.diff): [Main.cpp __APPLE__ headless guard] [meson.build libc++ auto_ptr/binders opt-in macros]
                 NOT NEEDED this round: luabind source edit (handled via the libc++ opt-in macros), cpp_std override (c++20 OK), GUIRect (built clean)
STRID=0 reached lj_str.c? Y | det_pairs refs 0? Y | blake3 in SimChecksum 0? Y
Positive control (perturb): DIVERGED (good) — first tick 49, subsystem sim_rng, 251/300 ticks mismatched
Single-core sweep runs=10:  11/11 MATCHED   [no divergences]
Thread matrix 1/2/4/8 identical?  M4:Y  M3Terr:Y  M1Actor:Y  M1Terr:Y  M2Pairs:Y  M2Lua:Y
Audio-ON soak (M4 r30 t900 th8): MATCHED
MPerfBench: MATCHED (did NOT trigger the known MOID-rebuild/draw race this run)
VERDICT: GREEN — repackaged stack determinism-equivalent to r7 on macOS arm64
```

---

## 1. Build

**Result: PASS.** 545/545 targets compiled and linked clean (`builddir/CortexCommand: Mach-O 64-bit executable arm64`, 17.36 MB). No errors, no warnings of note.

### Build patches applied (the deliverable for PR 4 — verbatim diffs in `macos-build-patches.diff`)

The branch's `Source/Main.cpp` and `meson.build` are byte-identical to r7 (i.e. *un*patched), so the same macOS bring-up patches from r5/r6/r7 were re-applied to the working tree (NOT committed to the branch):

1. **`Source/Main.cpp` — `__APPLE__` headless guard.** `#ifdef _WIN32 … #else setenv(SDL_VIDEODRIVER,offscreen)` gains an `#elif defined(__APPLE__)` arm that sets `CCCP_HEADLESS=1` instead. macOS Cocoa needs a real NSOpenGL context; SDL3's offscreen driver loads no GL on Darwin (gladLoadGL leaves `glReadBuffer` NULL) and the engine aborts in `WindowMan::Initialize`. CCCP_HEADLESS creates a hidden **real-GL** window and **keeps FMOD/audio active** (audio-off was the round-5 false-green — this path is audio-ON).
2. **`meson.build` — libc++ `auto_ptr`/binders opt-in macros** (darwin branch, `add_global_arguments`). Apple Clang 17 / libc++ 200100 removed `std::auto_ptr` / `unary_function` / binders; vendored luabind 0.7.1 + boost 1.75 reference them. Re-enabled via the `_LIBCPP_ENABLE_CXX17_REMOVED_*` / `_LIBCPP_ENABLE_CXX20_REMOVED_*` macros (the Windows analogue is `_HAS_AUTO_PTR_ETC=1`). This is the same fix as "luabind auto_ptr → unique_ptr", done at the libc++ level so **no luabind source edit is required**.

**Not needed this round** (listed in the brief as possible patches): a luabind source edit (subsumed by macro #2), a `cpp_std` override to `gnu++20`/libstdc++ (`-std=c++20` built clean — the `std::execution` concern did not surface), and a `GUIRect` meson workaround (built clean).

### FP-determinism flags (compile_commands.json, `Source/Main.cpp`) — 8/8 present
```
-ffp-contract=off  -fno-fast-math  -fno-finite-math-only  -fno-associative-math
-fno-reciprocal-math  -fno-unsafe-math-optimizations  -frounding-math  -fsignaling-nans
```
These live in `extra_args` unconditionally (clang takes the `get_argument_syntax()=='gcc'` path), so the r5 in-tree fix ("keep FP flags in release builds") is intact on this branch — they persist even at `buildtype=release`.

---

## 2. Determinism-code sanity (pre-build, on the checked-out branch)

| Check | Command | Expect | Got |
|---|---|---|---|
| LuaJIT STRID=0 | `grep -c LUAJIT_SECURITY_STRID=0 external/sources/LuaJIT-2.1/src/meson.build` | 1 | **1** ✅ |
| no global `det_pairs` | `grep -c det_pairs Source/Managers/LuaMan.cpp` (and whole `Source/`) | 0 | **0** ✅ (0 across all of `Source/`) |
| no blake3 in checksum | `grep -c blake3 Source/Network/SimChecksum.cpp` | 0 | **0** ✅ |
| **STRID=0 reached the compile** | `lj_str.c` entry in `compile_commands.json` | contains `LUAJIT_SECURITY_STRID=0` | **Y** ✅ |

Note: 4 stale `blake3` references remain in **comments/help-text only** (`AI/MetricsCollector.h` ×2, `CI/DeterminismCheck.{h,cpp}`) — the orchestrator help string still says "BLAKE3 hash traces". The actual per-tick hash is the built-in FNV/splitmix in `SimChecksum.cpp` (0 blake3 refs there). No functional blake3 remains.

---

## 3. Verification — audio ON throughout, `--seed 42`

Drove the game binary's `-determinism-check` mode directly (`exit 0 = MATCHED`, `1 = DIVERGED`). Every child run goes headless via `-tick-hashes` → `CCCP_HEADLESS=1` (hidden real-GL window, **FMOD active** — `LogConsole.txt` shows live `SoundContainer` property updates every tick; no `SDL_VIDEODRIVER=offscreen`, no audio-off anywhere in the logs).

### 3.0 Positive control — `--scenario M1Baseline --determinism-selftest-perturb --runs 10 --ticks 300`
**DIVERGED (exit 1) — GOOD.** The hash + orchestrator detect the injected perturbation.
- `first_divergence_tick`: **49**, in subsystem **`sim_rng`** (earliest); propagates outward: terrain @52, actors/particles @54, controller @55, scene/carve_math @56.
- `total_mismatched_ticks`: 251 / 300.
- A broken/constant hash would have MATCHED here — it did not.

### 3.1 Single-core sweep — all 11 scenarios, `--runs 10 --ticks 300` → **11/11 MATCHED**

| Scenario | exit | result | diverged | total_runs | compared_ticks | tick-count uniform |
|---|---:|---|---|---:|---:|---|
| M1Baseline | 0 | ✅ MATCHED | false | 10 | 300 | yes (all 300) |
| M1ActorStress | 0 | ✅ MATCHED | false | 10 | 300 | yes |
| M1TerrainStress | 0 | ✅ MATCHED | false | 10 | 300 | yes |
| **M2LuaBaseline** ★ | 0 | ✅ MATCHED | false | 10 | 300 | yes |
| M2LuaRandomStress | 0 | ✅ MATCHED | false | 10 | 300 | yes |
| **M2PairsStress** ★ | 0 | ✅ MATCHED | false | 10 | 300 | yes |
| M2OsStubTest | 0 | ✅ MATCHED | false | 10 | 300 | yes |
| M2ModSmokeLoading | 0 | ✅ MATCHED | false | 10 | 300 | yes |
| M3TerrainStress | 0 | ✅ MATCHED | false | 10 | 300 | yes |
| M4ThreadStress | 0 | ✅ MATCHED | false | 10 | 300 | yes |
| MPerfBench | 0 | ✅ MATCHED | false | 10 | 300 | yes |

★ **M2LuaBaseline + M2PairsStress are the stock-`pairs()` + `STRID=0` canaries** — both MATCHED, confirming string-key iteration order is deterministic without the old global `det_pairs`. No `length_mismatch` on any scenario (no early-exiting run masquerading as a match).

### 3.2 Thread matrix — multi-core == single-core, `--threads 1,2,4,8 --runs 10 --ticks 300` (40 runs/scenario) → **6/6 identical**

| Scenario | exit | result | thread_counts | total_runs (spread) | any run diverged? |
|---|---:|---|---|---|---|
| M4ThreadStress | 0 | ✅ MATCHED | [1,2,4,8] | 40 (10/10/10/10) | no |
| M3TerrainStress | 0 | ✅ MATCHED | [1,2,4,8] | 40 | no |
| M1ActorStress | 0 | ✅ MATCHED | [1,2,4,8] | 40 | no |
| M1TerrainStress | 0 | ✅ MATCHED | [1,2,4,8] | 40 | no |
| M2PairsStress | 0 | ✅ MATCHED | [1,2,4,8] | 40 | no |
| M2LuaBaseline | 0 | ✅ MATCHED | [1,2,4,8] | 40 | no |

The matrix diffs **across** Lua-state counts: all 40 runs (1, 2, 4, 8 threads) produce the bit-identical per-tick hash trace → the parallel threaded-sim path is deterministic and equals the single-core path.

**Thread matrix identical?  M4:Y  M3Terr:Y  M1Actor:Y  M1Terr:Y  M2Pairs:Y  M2Lua:Y**

### 3.3 Audio-ON heavy soak — `M4ThreadStress --threads 8 --runs 30 --ticks 900` → **MATCHED (exit 0)**
- 30 runs, all at 8 Lua-states, `per_run_tick_count` = `[900 × 30]` (uniform, no early exit), `diverged=false` on every run.
- FMOD active throughout (no audio-off). This is the configuration that exposed the round-5 audio-RNG-leak false-green; it is GREEN here.

### 3.4 MPerfBench
**MATCHED** — did **not** trigger the known pre-existing async MOID-rebuild/draw race (SIGSEGV) on this run. No crash, abort, or segfault appears in any battery log. (Had it crashed rather than diverged, it would be noted as the separate known bug, not a determinism failure.)

---

## 4. Tally
- **18 MATCHED** orchestrator runs (11 single-core + 6 thread-matrix + 1 soak) + **1 DIVERGED** (positive control, expected) = 19 total invocations, every one with the expected outcome.
- 0 crashes, 0 early-exits/length-mismatches, 0 audio-off leaks.

## VERDICT

**GREEN — the repackaged cumulative stack (`stage/cumulative-verify` @ `0bf49039c`) is determinism-equivalent to r7 on macOS arm64.** Built-in FNV/splitmix hash, `LUAJIT_SECURITY_STRID=0` reaching `lj_str.c`, stock `pairs()` (no `det_pairs`), the render/audio/effect RNG-leak fixes, per-MO collision RNG, terrain snapshot, and per-activity Lua reseed all reproduce the r5/r6/r7 result: bit-identical intra-mac determinism across the 11-scenario battery, the 1/2/4/8 thread matrix, and the audio-ON 8-thread/900-tick soak, with the positive control correctly diverging.

## 5. Honest gaps
- **Two macOS-only build patches still required** (Main.cpp `__APPLE__` guard, meson.build libc++ macros), applied to the working tree only and captured in `macos-build-patches.diff` for folding into **PR 4**. Both are CI/link blockers, distinct from determinism. A macOS meson CI agent fails at compile/link without them.
- **Build is `buildtype=release` + `debug_type=minimal`** (matching the proven r5/r6/r7 GREEN config). The `debug_type=minimal` preprocessor effect (`-DMIN_DEBUG_BUILD -DDEBUGMODE`) is gated behind `debug=true` in `meson.build`, so at `buildtype=release` it does not apply and `-DRELEASE_BUILD` is defined instead — a pre-existing meson quirk, identical to r5, not introduced here.
- **arm64 vs x64 hash equality is out of scope** (Apple libm vs glibc/MSVCRT transcendentals differ bit-for-bit). The bar here is intra-mac determinism — same machine, same args → same hash — which is GREEN.
- **No upstream changes to `stage/cumulative-verify`.** Results pushed only to `verify/cumulative-macos`.
