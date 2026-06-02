# macOS arm64 cross-platform verification — Stage‑1 determinism stack

**Branch under test:** `pr/cccp-ctl-test-harness-and-scenarios` @ `ff359363c` (PRs #1→#6 cumulatively)
**Host:** Mac Mini M4 Pro · macOS 24.6.0 (Darwin) · Apple Silicon arm64 · 24 GB
**Toolchain:** Apple clang 17.0.0 (LLVM 20 — `_LIBCPP_VERSION 200100`) · meson 1.6+ · ninja 1.13.2
**Date:** 2026‑06‑01 (MST)

---

## TL;DR

| | Result |
|---|---|
| **Build** | ✅ Links after two **pre‑existing, base‑branch** macOS build fixes the PR series doesn't carry. Also surfaces a meson bug that silently drops the PR's own FP determinism flags on release. |
| **Intra‑macOS determinism** | ❌ **All 11 scenarios DIVERGED** at `--runs 5 --ticks 300 --seed 42`. `sim_rng` is consistently among the first subsystems to diverge — often at **tick 2**. |
| **Orchestrator self‑test** | ✅ `--determinism-selftest-perturb` exits 1 (DIVERGED) as expected — the check itself works. |
| **Thread matrix** | ❌ M4ThreadStress with `--threads 1,2,4,8` exits 1 (DIVERGED). Expected, since `--runs ≥ 2` at any single thread count already diverges. |
| **Cross‑OS traces** | ✅ Produced for M1Baseline, M1TerrainStress, M4ThreadStress (3 runs each). All three intra‑mac runs of M1Baseline are **different** — see hashes below. Any single trace is **one sample of a non‑deterministic distribution**, not a reproducible reference. |

**The headline:** the PR series as committed does not produce intra‑macOS determinism on Apple Silicon with Apple clang 17 / libc++ 20. Cross‑OS comparison is therefore not meaningful from a single trace pair — *which mac trace* you compare against the Windows trace is itself non‑deterministic on macOS. The Windows side can still diff the produced JSONs and will see whatever it sees, but the macOS reference itself is moving.

---

## 1. Build

### What I had to do to get a binary

The PR branch ships with three **separate** macOS bring‑up issues that all need to be addressed before the engine even links + boots into the determinism check. Two of them are pre‑existing on `modernization-effort` (base branch) and were fixed in a separate Erol branch (`exp/determinism-macos`) earlier — the PR stack doesn't inherit them. The third is a **bug introduced by this PR series itself** that silently disables the FP determinism flags on release builds.

#### 1.1 luabind 0.7.1 + boost 1.75 — `std::auto_ptr` removed from libc++ 19+

**File/line:** `external/sources/luabind-0.7.1/luabind/detail/policy.hpp:615`, `function.hpp:295/302`, `detail/has_get_pointer.hpp:61`, `scope.hpp:59`, `src/scope.cpp:49,187`, `src/class.cpp:235`
**Root cause:** Vendored luabind uses `std::auto_ptr`, removed from libc++ 19. The existing `external/sources/luabind-0.7.1/meson.build:6` opt‑in `-D_LIBCPP_ENABLE_CXX17_REMOVED_FEATURES` does **not** bring `auto_ptr` back in LLVM 20 — there's a per‑feature macro that has to be set instead.
**Status on this PR branch:** Not addressed. Build fails with 14 errors during `external/sources/luabind-0.7.1/src/*.cpp` compilation.
**Local fix (applied to working tree, not pushed to PR branch):** Same fix as commit `7f2195090` in the prior macOS bring‑up — `add_global_arguments` in `meson.build` darwin branch with `_LIBCPP_ENABLE_CXX17_REMOVED_AUTO_PTR / _UNARY_BINARY_FUNCTION / _BINDERS / _RANDOM_SHUFFLE / _UNEXPECTED_FUNCTIONS / _LIBCPP_ENABLE_CXX20_REMOVED_BINDER_TYPEDEFS / _NEGATORS / _LIBCPP_ENABLE_EXPERIMENTAL / _LIBCPP_PSTL_BACKEND_SERIAL`.

#### 1.2 Main.cpp headless path uses `SDL_VIDEODRIVER=offscreen` on macOS — no GL functions load

**File/line:** `Source/Main.cpp:541` (was `setenv("SDL_VIDEODRIVER", "offscreen", 1);` on non‑Windows).
**Root cause:** macOS Cocoa needs a real NSOpenGL context; SDL3's offscreen video driver loads no GL extensions on Darwin. `gladLoadGL` returns success but `glReadBuffer` (and friends) stay NULL, the engine then aborts in `WindowMan::Initialize()` and the abort screen path itself crashes calling `glReadBuffer` on the NULL pointer (`GLAD: ERROR glReadBuffer is NULL!`).
**Status on this PR branch:** Not addressed. The engine aborts before reaching the scenario loop.
**Local fix (applied to working tree, not pushed to PR branch):** Replace with `setenv("CCCP_HEADLESS", "1", 1)` on Apple builds — `WindowMan` already supports this env var and creates a real, hidden window (real GL context, no display). Same approach as the prior macOS bring‑up.

#### 1.3 ★ NEW BUG INTRODUCED OR EXPOSED BY THIS PR — `meson.build` overwrites FP determinism flags on release

**File/line:** `meson.build:94`
**The bug:**
```meson
extra_args = ['-w']   # ← `=` overwrites, not `+=`
```
in the `else` branch of `if buildtype_debug`. This is the **release** branch. The earlier block (lines 37–46) populated `extra_args` with PR #2's load‑bearing FP determinism flags:
```meson
extra_args += [
  '-ffp-contract=off',          # comment: "required on ARM, harmless on x86"
  '-fno-fast-math', '-fno-finite-math-only',
  '-fno-associative-math', '-fno-reciprocal-math',
  '-fno-unsafe-math-optimizations',
  '-frounding-math', '-fsignaling-nans',
]
```
The `=` at line 94 **silently discards every one of those flags** when building release (the default). Confirmed by inspecting `builddir/compile_commands.json` before the fix:
```
$ grep -oE '"-f[a-z-]+(=[a-z]+)?"' builddir/compile_commands.json
"-fdiagnostics-color=always"
"-fpch-instantiate-templates"
```
No `-ffp-contract=off`. No `-fno-fast-math`. Nothing.

**This is on the PR branch, not pre‑existing.** It's exactly the kind of thing the brief flagged as load‑bearing for cross‑OS bit‑identity — and the brief asked to confirm `‑ffp‑contract=off` was present.

**Local fix (applied to working tree, not pushed to PR branch):** `extra_args = ['-w']` → `extra_args += ['-w']` at `meson.build:94`. After the fix, `grep -oE` shows all 8 flags landing on `Main.cpp` and every other TU.

**Verification:** intra‑mac determinism was tested **with** the FP flags restored. It still diverges (see §2), so the FP overwrite isn't the *only* macOS determinism issue — but it would be a real regression on every platform on its own.

### What was already fine

- All brew deps available on arm64 (only `sdl2_image` had to be `brew install`ed; sdl2, libpng, flac, lz4, minizip, tbb, luajit, meson, ninja, pkg-config were already there).
- No submodules — vendored sources in `external/sources/*`.
- Meson configures and links the subprojects (BLAKE3 1.8.5, nlohmann_json 3.12.0, LuaJIT 2.1, RakNet, SDL3_image 3.2.4, luabind 0.7.1, allegro 4.4.3.1-custom, tracy) cleanly.

### Final build state

```
$ file builddir/CortexCommand
builddir/CortexCommand: Mach-O 64-bit executable arm64
$ ls -lh builddir/CortexCommand
-rwxr-xr-x  17M  ...  builddir/CortexCommand
```
418/418 targets linked. Single binary (`CortexCommand`) — no separate `cccp-ctl` on this branch; the `-determinism-check` orchestrator + `-scenario … -tick-hashes` trace producer are both inside `CortexCommand` (and `Source/CI/DeterminismCheck.cpp` is folded into the main target via `Source/CI/meson.build`).

---

## 2. Intra‑macOS determinism — every scenario diverges

### Method
```
./builddir/CortexCommand -determinism-check \
    --scenario <name> --runs 5 --ticks 300 --seed 42 \
    --output crossplat-results/macos/det/<name>.json
```
…run for each of the 11 scenarios. Plus the positive control (`--determinism-selftest-perturb` on M1Baseline) and the thread matrix (M4ThreadStress with `--threads 1,2,4,8`).

The build had the FP flags restored (§1.3) and the headless path fixed (§1.2) — i.e. the strongest configuration available on this branch.

### Result — all 11 scenarios DIVERGED, control passes

| Scenario | exit | first_div_tick | mismatched/300 | sim_rng | particles | scene | actors | terrain | carve_math | controller |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| M1Baseline | **1** | **2** | 282 | 2 | 19 | 20 | 19 | 23 | 19 | 23 |
| M1ActorStress | **1** | **3** | 297 | 4 | 3 | 3 | 10 | 8 | 8 | 19 |
| M1TerrainStress | **1** | **2** | 281 | 2 | 20 | 21 | 20 | 21 | 21 | 26 |
| M2LuaBaseline | **1** | **2** | 281 | 2 | 20 | 20 | 20 | 20 | 20 | 22 |
| M2LuaRandomStress | **1** | **3** | 297 | 4 | 3 | 3 | 10 | 8 | 8 | 17 |
| M2PairsStress | **1** | **22** | 278 | 22 | 22 | 22 | 22 | 22 | 23 | 30 |
| M2OsStubTest | **1** | **3** | 297 | 4 | 3 | 3 | 10 | 8 | 8 | 17 |
| M2ModSmokeLoading | **1** | **2** | 269 | 2 | 32 | 32 | 33 | 32 | 33 | 52 |
| M3TerrainStress | **1** | **2** | 278 | 2 | 23 | 26 | 23 | 25 | 23 | 33 |
| M4ThreadStress | **1** | **3** | 297 | 3 | 3 | 3 | 10 | 9 | 4 | 15 |
| MPerfBench | **1** | **2** | 298 | 2 | 3 | 3 | 10 | 8 | 8 | 12 |
| **Positive control** (`--determinism-selftest-perturb`) | **1** | — | — | — | — | — | — | — | — | — |
| **Thread matrix** (M4ThreadStress, threads=1,2,4,8) | **1** | — | — | — | — | — | — | — | — | — |

(Cells are the per‑subsystem **first** divergence tick.)

### Reading the table

- **`sim_rng` is at the front of the divergence wave** — in 8/11 scenarios it diverges at tick 2 or 3, before any other tracked subsystem. The single‑threaded RNG **state itself** is non‑deterministic by tick 2. Everything downstream (`particles`, `scene`, `actors`, `terrain`, `controller`, `carve_math`) inherits that and starts diverging a few ticks later.
- The mismatched‑tick count is ≥269/300 in every scenario — once the runs split, they stay split. This is *not* a converging stochastic system; it's amplifying drift.
- The positive control (`--determinism-selftest-perturb`) **does** exit 1, so the orchestrator's compare is healthy. The runtime perturbation it injects at tick 50 produces the same kind of divergence the engine produces unbidden — i.e. the test instrument is fine; the system under test is broken.
- The thread matrix exit 1 is *not* a new finding — it's the same intra‑mac non‑determinism showing up across thread counts. Since `--runs 5 --ticks 300 --seed 42` already DIVERGED at a fixed thread count, comparing across thread counts can only also DIVERGE.

### Where the non‑determinism is coming from (root cause analysis, not a fix)

Per §1.3 the FP flags were restored before this run, so it's not floating‑point contraction. The candidates left, in priority order:

1. **`std::execution::par_unseq` PSTL backend on Apple libc++ 20.** The vendored fix attempts `-D_LIBCPP_PSTL_BACKEND_SERIAL` + `-D_LIBCPP_ENABLE_EXPERIMENTAL`. I confirmed that with `_LIBCPP_PSTL_BACKEND_SERIAL` defined in user code, `__pstl/backend_fwd.h:55-56` selects `__backend_configuration<__serial_backend_tag, __default_backend_tag>`, so the SERIAL backend templates **are** the active configuration despite Apple's pre‑built default of `LIBDISPATCH` (set in `<__config_site>:40`). So PSTL probably isn't the source on this build. But it's worth flagging that the macro double‑define is fragile — if Apple ever reorders the `#if` chain in `backend.h`, LIBDISPATCH could silently win.
2. **`BS::thread_pool::parallelize_loop` in `MovableMan` for actor see‑rays + MOID draws** (`Source/Managers/MovableMan.cpp:1794`, etc.). This is a real thread pool with timing‑dependent task completion order. If any worker calls `RandomNum*()` and `t_simRNGOverride` isn't installed for that worker, it reads/mutates `g_SimRNG` concurrently with other workers and the main thread → race + non‑deterministic mt19937 state. The `t_simRNGOverride` design (`Source/System/RTETools.h:102‑106`) acknowledges this exact race but is opt‑in per call‑site. Easy to miss one. **This is my best guess for the root cause.**
3. The hash inputs themselves are clean — `g_SimRNG.SerializeStateForHashing()` serializes only the mt19937 internal state via `operator<<`, no pointer addresses. So the divergence isn't from `Update`/`HashAllLuaStatesIntoSimChecksum` writing garbage; the underlying RNG state actually differs run‑to‑run.

If the maintainer wants to pin this down on macOS, the smallest reproducer is **M2OsStubTest** (Lua‑only — no actors, no terrain, no FP heavy lifting) which still diverges at tick 3 with `sim_rng` first at tick 4. That tells you the divergence path is in the *common* tick infrastructure, not in any scenario‑specific code.

---

## 3. Cross‑OS traces

### What was produced
```
crossplat-results/macos/
  M1Baseline_run0.json        300 ticks · numLuaStates=4 · seed=42
  M1Baseline_run1.json        (same args)
  M1Baseline_run2.json        (same args)
  M1TerrainStress_run0.json   (same args)
  M1TerrainStress_run1.json
  M1TerrainStress_run2.json
  M4ThreadStress_run0.json    (same args)
  M4ThreadStress_run1.json
  M4ThreadStress_run2.json
```
Each is the full per‑tick BLAKE3 trace from a single `-scenario … -tick-hashes -num-lua-states 4 -seed 42 -max-ticks 300` run. Per the brief's spec — same args every OS, comparison key is `runs[0].tick_hashes[].total` plus the rolled‑up `runs[0].final_total_hash`.

### Honest caveat

**These traces are samples, not references.** Because intra‑mac is non‑deterministic (§2), every run produces a different `final_total_hash`. The 3 runs per scenario are included so the Windows side can see the **intra‑mac spread** — if Windows‑run0 happens to match macOS‑run1 but not macOS‑run0 or macOS‑run2, that's not a cross‑OS match, it's noise. The Windows team should diff *all three* mac traces against the Windows trace and note whether *any* aligned. The likely outcome is "none match" given the size of the per‑tick drift.

### Inline numbers (key data the Windows side can spot‑check without pulling)

**`final_total_hash` per scenario, per run (all 3 runs DIFFER on the same machine, same seed, same args):**

| Scenario | run0 final_total_hash | run1 final_total_hash | run2 final_total_hash |
|---|---|---|---|
| M1Baseline | `db8eccae21e46fbc86cb77ef10401b3958fc8e5d35f0b5fc08ea1447421cdcf0` | `05473ce5d8bf4d0f059e2e348ceb5f528441a6ce8dc17bc31da9c3e60aa44e1a` | `cb566d2b412ee7f67718e3c0dfa3c54e08eaffc408de154173b638c509a004f0` |
| M1TerrainStress | `91863a0e9de399fc3aeb1a9105a3418f86538fed95be51c53eb831d822dcaf3c` | `931466b708c3d8f5fdc132026c407a08e8ce5bfea43306ee7c55609af58e5f2b` | `fce8935101c8d00e530059381ab41e9b68c45ee65be4d51b191f3e4f98603caa` |
| M4ThreadStress | `ec78c0b364875f9a8c1da98b07fe626d9da5b145de98482ce347bde7a231f1b8` | `8706d716d8beed04f79c906ce9d7f8c722fe4ffdae7892b3f7fb41aca232587b` | `6ad70878618401c6b19314ecbf5b99aadcad8848e4171b8df2b74e354bb13413` |

**M1Baseline first 5 per‑tick `total` (note where the 3 runs split):**

| tick | run0 `total` | run1 `total` | run2 `total` |
|---:|---|---|---|
| 1 | `9e94ed8bc9543c89…f122` | `9e94ed8bc9543c89…f122` | `9e94ed8bc9543c89…f122` ← all 3 match |
| 2 | `13a8968b165f7aa8…d447` | `13a8968b165f7aa8…d447` | `13a8968b165f7aa8…d447` ← all 3 match |
| 3 | `3ea4978416e6dcb4…34e8` | `3ea4978416e6dcb4…34e8` | `838642b06006773f…2bf8` ← run2 splits |
| 4 | `a0c4740b326e196b…7fde` | `981b2c19fb3b89a6…2a78` | `981b2c19fb3b89a6…2a78` ← run0 splits, run1=run2 |
| 5 | `8ed9c4b5f0b19223…c8f1` | `2ce41bbb5c2810db…8ec6` | `2ce41bbb5c2810db…8ec6` ← run0 still split |

That tick 3 / tick 4 pattern — *different* runs splitting off at *different* ticks — is the signature of a racy non‑deterministic mutation, not a deterministic FP rounding difference. It rules out "macOS just gives different numbers" and points squarely at threaded RNG state.

**M1Baseline tick‑1 subsystem hashes (identical across all 3 mac runs — Windows side: compare against your tick‑1 subsystem map for the cleanest single‑tick cross‑OS comparison point):**

```
total:      9e94ed8bc9543c8936c0a193c176d4bf0b2335135a5744230e28d4fbabc2f122
  tick:       1a0d12016999e47689dae5744d2b8c1903faf7ca2886a658150083100ef2c8ee
  sim_rng:    2077ef99fce7c38b5d75f7e6ad54f283a6d38dc6ca8c84e71bd6d1ecc0398e3f
  lua_state:  1e2c9651aba70062c2b6c9d266e0629ddaf2d22b52250bbe3340bc2818be4d44
  actors:     9433c180b7b012e3504ff6efddea8fb60eca9716cd261b8e0a1f8a94908bd06c
  scene:      1054a524623425cc9fad6a84d5bef6c2477beab19f1f36559bd057b9df336404
  controller: 69562f1e39e64ffce2d3fa3b3ddd735163d705f4136b22e9b1faa08966ecb320
  terrain:    d14f943d2d476bd7ca050d2c81e17fe2935c379bb21eeba12e2b698011bf3a63
```
(Full per‑tick subsystem maps are inside each `*_run0.json` under `runs[0].tick_hashes[N].subsystems`.)

### Subsystem hash keys present in each trace

`tick · sim_rng · lua_state · actors · particles · controller · scene · terrain` — `carve_math` only shows up under `per_subsystem_first_divergence` in determinism‑check reports; the per‑tick trace already folds it into `terrain`.

---

## 4. The big question: macOS vs x64

I have **only macOS** here. The actual macOS‑vs‑Windows / macOS‑vs‑Linux diff has to happen on the Windows side once these JSONs land. But two things make the cross‑OS comparison harder than the brief assumed:

1. **The Mac side itself isn't bit‑deterministic** (§2). So "does macOS match Windows" is partly a question of *which* macOS trace. The branch contains all three M1Baseline mac traces so the Windows side can quantify this.
2. **The FP determinism flags were silently absent on every prior macOS release build via this PR series** (§1.3). Any previous macOS trace produced by this PR stack — if one exists in CI artifacts — would have been built without `‑ffp‑contract=off` etc., so it's not a comparable reference even on its own. The traces in this branch are produced with the flags restored.

The brief said "A cross‑platform divergence here is a MAJOR finding, not a failure." Same applies to the intra‑mac divergence. It tells us, plainly: **Stage‑1 as currently committed does not deliver Mac↔PC deterministic sessions, and won't until at least the BS::thread_pool / `t_simRNGOverride` audit is done and the `meson.build:94` overwrite is fixed.**

---

## 5. Honest gaps / things I did not do

- **No upstream PR.** Brief said don't. I didn't.
- **No modifications committed to anything but this results directory.** All three local fixes (luabind macros, macOS headless path, FP flag overwrite) were applied to my working tree to get the build to succeed, **then reverted before commit**. They're preserved as a single patch artifact at `crossplat-results/macos/local-verify-only.patch` so the maintainer can `git apply` it on the PR branch to reproduce my exact build. **Do not interpret the patch as a proposed merge** — the FP overwrite fix is a real one‑line bug; the other two are bring‑up adjustments that may belong elsewhere (e.g. as part of a separate macOS‑bring‑up PR).
- **No fix for the intra‑mac non‑determinism.** I identified the most likely root cause (`t_simRNGOverride` not installed in all `BS::thread_pool` paths) but the brief was "test, don't fix." Fixing it is engineering work, not verification.
- **`carve_math` is reported by the determinism‑check JSON only.** The single‑run `-tick-hashes` trace folds it inside `terrain`. So the per‑run trace JSONs don't independently expose `carve_math` deltas; if the Windows side wants to slice macOS `carve_math` vs Windows `carve_math` they'd have to read it out of the `det/*.json` reports (also in this branch).
- **One brew dep added:** `sdl2_image` (the only deps that wasn't already installed locally; everything else was present).
- **Apple clang/libc++ versions vary** between toolchain installs. This run was on libc++ 20.0100 (LLVM 20). On older Apple clangs (LLVM 18 and earlier) the luabind `auto_ptr` build break would not have happened — so the build‑break observation in §1.1 is partly compiler‑version‑specific. The `meson.build:94` overwrite (§1.3) and the macOS headless path (§1.2) are version‑independent.

---

## 6. Repro recipe (for the maintainer)

```bash
git clone --recurse-submodules https://github.com/Madreag/cortex-modern.git
cd cortex-modern
git checkout pr/cccp-ctl-test-harness-and-scenarios   # ff359363c

# Apply the three local fixes (luabind libc++ macros, macOS headless env var,
# meson.build `=` → `+=` FP-flag overwrite). The patch is saved on this branch at
# crossplat-results/macos/local-verify-only.patch.
git apply crossplat-results/macos/local-verify-only.patch

brew install meson ninja pkg-config sdl2 sdl2_image libpng flac lz4 minizip tbb luajit
meson setup builddir --buildtype=release -Db_lto=false
meson compile -C builddir -j 12

# Intra-mac determinism (will exit 1 — DIVERGED):
./builddir/CortexCommand -determinism-check \
    --scenario M1Baseline --runs 5 --ticks 300 --seed 42 \
    --output /tmp/m1baseline.json

# Single-run trace for cross-OS comparison:
./builddir/CortexCommand \
    -scenario M1Baseline -seed 42 -max-ticks 300 \
    -tick-hashes -num-lua-states 4 \
    -out /tmp/m1baseline_trace.json
```
