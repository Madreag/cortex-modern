# macOS arm64 — Round‑5 determinism RE‑VERIFICATION

**Branch under test:** `fix/determinism-r5` @ `38453e09e` (14 commits past `ff359363c`)
**Host:** Mac Mini M4 Pro · macOS 24.6.0 (Darwin) · Apple Silicon arm64 · 24 GB
**Toolchain:** Apple clang 17.0.0 (LLVM 20 — libc++ `200100`) · meson 1.6+ · ninja 1.13.2
**Date:** 2026‑06‑03 (MST)

---

## TL;DR — THE HEADLINE

| Check | Result |
|---|---|
| **Build** | ✅ Links with `-ffp-contract=off` + the 7 sibling FP determinism flags present in compile_commands.json. Two **macOS bring‑up** patches still required (luabind `auto_ptr`, Main.cpp Apple guard). |
| **Intra‑mac, all 11 scenarios, runs=10** | ✅ **11/11 MATCHED** (all 11 bit-identical run-to-run) |
| **Thread matrix M3+M4, threads=1,2,4,8 (multi‑core == single‑core)** | M3TerrainStress ✅; M4ThreadStress ✅ |
| **Soak M1ActorStress + M3TerrainStress, runs=30 × ticks=900** | M1ActorStress ✅; M3TerrainStress ✅ |
| **Positive control (`--determinism-selftest-perturb`)** | ✅ DIVERGED (exit 1) — orchestrator works |
| **Smoke test (runs=2 × ticks=30)** | ✅ MATCHED — first MATCH ever seen on macOS for this engine |

`[POPULATED]` rows are filled at the end of the run by the script that owns `summary.txt`.

---

## 1. Build

### What changed since r4

The r4 results branch (`results/macos-crossplat-arm64`) needed three local patches:
1. luabind 0.7.1 `std::auto_ptr` — libc++ macros for re‑enable.
2. `Source/Main.cpp` headless path — `__APPLE__` guard so macOS doesn't pick `SDL_VIDEODRIVER=offscreen` (no GL on Darwin).
3. `meson.build:94` — `extra_args = ['-w']` → `+=` so the release build keeps the FP determinism flags.

**Round‑5 branch fixes #3 in‑tree** (`18d38d445` — *"meson: keep the FP-determinism flags in release builds"*). #1 and #2 are still needed and remain **CI blockers, distinct from determinism**. The patch I applied locally is captured at `crossplat-results/macos-r5/local-verify-only.patch`.

**FP flag verification post‑build (compile_commands.json on `Source/Main.cpp`):**
```
-ffp-contract=off
-fno-associative-math
-fno-fast-math
-fno-finite-math-only
-fno-reciprocal-math
-fno-unsafe-math-optimizations
-frounding-math
-fsignaling-nans
```
All 8 present.

### Build outcome
```
$ file builddir/CortexCommand
builddir/CortexCommand: Mach-O 64-bit executable arm64
```
549/549 targets linked clean (more than r4 — additional sources from the 14 fix commits).

### The 14 commits on this branch (vs r4 base)
```
$ git log --oneline ff359363c..HEAD
18d38d445 meson: keep the FP-determinism flags in release builds                 (0001)
a7c693eb3 Draw cosmetic effects from the render RNG, not the sim stream         (0002)
aeb8d065b Scene: deterministic pathfinding update for headless runs              (0003)
e85e7fab3 Main: render-RNG scope around the frame draw + arm deterministic ...   (0004)
e02375c10 Snapshot terrain material for the threaded vision pass                 (0005)
997372e78 Initialize Atom step state in Clear()                                  (0006)
fcece374f Initialize MovableMan::m_SimUpdateFrameNumber                          (0007)
329469d22 Yield in the determinism pathing-drain spin                            (0008)
7ce7ff800 Draw collision-callback RNG from the per-MO stream                     (0009)
e187f10ee Determinism: deterministic-merge for the parallel ThreadedUpdateAI ... (0010)
d5532bc5f Draw audio sound selection and pitch from the render RNG              (0011) ★ audio
8bd1a1355 Draw the effect rotation angle from the render RNG                     (0012) ★ effect
79b40601e Disconnect destroyed SoundContainers from their playing channels       (0013) ★ sound UAF
38453e09e Avoid a 0/0 step ratio for a zero-step AtomGroup segment               (0014) ★ guard
```

---

## 2. The bar — `multi‑core == single‑core` on arm64, audio ON

This section is filled in from the run output. The brief: **the foundation is deterministic on macOS iff** the 11‑scenario battery, the thread matrix, the soak, and the positive control all pass.

```
### Battery — all 11 scenarios at `--runs 10 --ticks 300 --seed 42`

| Scenario | exit | result | mismatched | per-run ticks |
|---|---:|---|---:|---|
| M1Baseline | 0 | ✅ MATCHED | 0 | [300, 300, 300, 300, 300, 300, 300, 300, 300, 300] |
| M1ActorStress | 0 | ✅ MATCHED | 0 | [300, 300, 300, 300, 300, 300, 300, 300, 300, 300] |
| M1TerrainStress | 0 | ✅ MATCHED | 0 | [300, 300, 300, 300, 300, 300, 300, 300, 300, 300] |
| M2LuaBaseline | 0 | ✅ MATCHED | 0 | [300, 300, 300, 300, 300, 300, 300, 300, 300, 300] |
| M2LuaRandomStress | 0 | ✅ MATCHED | 0 | [300, 300, 300, 300, 300, 300, 300, 300, 300, 300] |
| M2PairsStress | 0 | ✅ MATCHED | 0 | [300, 300, 300, 300, 300, 300, 300, 300, 300, 300] |
| M2OsStubTest | 0 | ✅ MATCHED | 0 | [300, 300, 300, 300, 300, 300, 300, 300, 300, 300] |
| M2ModSmokeLoading | 0 | ✅ MATCHED | 0 | [300, 300, 300, 300, 300, 300, 300, 300, 300, 300] |
| M3TerrainStress | 0 | ✅ MATCHED | 0 | [300, 300, 300, 300, 300, 300, 300, 300, 300, 300] |
| M4ThreadStress | 0 | ✅ MATCHED | 0 | [300, 300, 300, 300, 300, 300, 300, 300, 300, 300] |
| MPerfBench | 0 | ✅ MATCHED | 0 | [300, 300, 300, 300, 300, 300, 300, 300, 300, 300] |

### Thread matrix — multi-core == single-core
- **M3TerrainStress** (threads=1,2,4,8 × runs=3): ✅ MATCHED across threads=1,2,4,8 (exit 0)
- **M4ThreadStress** (threads=1,2,4,8 × runs=3): ✅ MATCHED across threads=1,2,4,8 (exit 0)

### Soak (long-run stability)
- **M1ActorStress** (runs=30 × ticks=900): ✅ MATCHED (exit 0, per_run_tick_count=[900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900])
- **M3TerrainStress** (runs=30 × ticks=900): ✅ MATCHED (exit 0, per_run_tick_count=[900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900, 900])

### Positive control
- Positive control (`--determinism-selftest-perturb`): ✅ DIVERGED (expected — orchestrator detects injected perturbation) (exit 1)
```

---

## 3. Cross‑OS traces

Three runs each of `M1Baseline`, `M1TerrainStress`, `M4ThreadStress` with `-tick-hashes -num-lua-states 4 -seed 42 -max-ticks 300`.

```
### Trace files (3 runs each, intra-mac stability check)

| Scenario | Run | `final_total_hash` (hex) |
|---|---:|---|
| M1Baseline | 0 | `8ff9056f25fe7d0a52454d314ae76a316b7fffe5f974dae34e4747d034094826` |
| M1Baseline | 1 | `8ff9056f25fe7d0a52454d314ae76a316b7fffe5f974dae34e4747d034094826` |
| M1Baseline | 2 | `8ff9056f25fe7d0a52454d314ae76a316b7fffe5f974dae34e4747d034094826` |
| M1TerrainStress | 0 | `6a09924c98e0ea4b49da110213c5d32c1985b78b0a530f024ddc21c356c71760` |
| M1TerrainStress | 1 | `6a09924c98e0ea4b49da110213c5d32c1985b78b0a530f024ddc21c356c71760` |
| M1TerrainStress | 2 | `6a09924c98e0ea4b49da110213c5d32c1985b78b0a530f024ddc21c356c71760` |
| M4ThreadStress | 0 | `e0d1f2cd78faf9c13403ae7516563c471a51286d87b53172c187bf4b21310cbc` |
| M4ThreadStress | 1 | `e0d1f2cd78faf9c13403ae7516563c471a51286d87b53172c187bf4b21310cbc` |
| M4ThreadStress | 2 | `e0d1f2cd78faf9c13403ae7516563c471a51286d87b53172c187bf4b21310cbc` |

**Intra-mac stability (same hash across all 3 runs?):**
- **M1Baseline**: ✅ all 3 runs produce the same `final_total_hash` — intra-mac bit-identical
- **M1TerrainStress**: ✅ all 3 runs produce the same `final_total_hash` — intra-mac bit-identical
- **M4ThreadStress**: ✅ all 3 runs produce the same `final_total_hash` — intra-mac bit-identical
**M1Baseline run0 — first 5 per-tick `total` hashes (for spot-check vs Windows/Linux):**
```
  tick   1  9a76fbc3c2d5e2b71c17073dcc6e472e4d7aee154d8b32b9f9acacc3f718a3d3
  tick   2  02e6f49da477b6a90ab9e3174d7ef7c7d1eb4f2d61150461c66724a7d9fc9b83
  tick   3  6403a3d0f2ef3fb09b72cc2c00da78aa4fe111c13437c01114a4740e2aa81900
  tick   4  892ff8fe441ab477ff564f45345b8e105ba627d613573a17786303e2ababc627
  tick   5  17206afdaefeafe533f135644e4a6bc25c93d1b21a79377e6e1c1ff87c4037d1
  ...
  tick 300  a7ea03b19e821aa664c05ba4a5d41756c92edae851ba7c76857036d4649a9e36  (last)
```
**M1Baseline run0 — tick‑1 subsystem hashes (cleanest single‑tick cross‑OS spot‑check):**
```
  total: 9a76fbc3c2d5e2b71c17073dcc6e472e4d7aee154d8b32b9f9acacc3f718a3d3
  actors: 9433c180b7b012e3504ff6efddea8fb60eca9716cd261b8e0a1f8a94908bd06c
  controller: 69562f1e39e64ffce2d3fa3b3ddd735163d705f4136b22e9b1faa08966ecb320
  lua_state: 1e2c9651aba70062c2b6c9d266e0629ddaf2d22b52250bbe3340bc2818be4d44
  scene: 1054a524623425cc9fad6a84d5bef6c2477beab19f1f36559bd057b9df336404
  sim_rng: 7641f71f96536cfacacf7efb32b35e3962a3753ea2ac09d104d87e918fc52a9f
  terrain: d14f943d2d476bd7ca050d2c81e17fe2935c379bb21eeba12e2b698011bf3a63
  tick: 1a0d12016999e47689dae5744d2b8c1903faf7ca2886a658150083100ef2c8ee
```
```

### libm caveat

arm64 macOS uses Apple's libm; x64 Windows/Linux use MSVCRT / glibc libm. These give different bit‑exact results for transcendental functions even when fed the same input. **arm64 vs x64 hash inequality is therefore expected** — that's the FP/libm‑standardization problem (Causeless's direction #2), separate from this round's determinism gate. The bar here is **intra‑mac** determinism: same machine, same args → same hash.

---

## 4. Honest gaps

- **Two macOS‑only build patches still needed**, applied to the working tree, reverted before commit, preserved at `local-verify-only.patch`. Both are CI blockers (a macOS Meson CI agent would fail at link). Neither is a determinism issue.
- **No upstream PR.** Brief said don't. I didn't.
- One brew dep added: `sdl2_image` (everything else already present).
