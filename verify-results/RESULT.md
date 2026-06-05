CUMULATIVE LINUX CONFIRM — 2026-06-05
Branch: stage/cumulative-verify @ 0bf49039c | g++ 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1) | meson debug-minimal (-Dbuildtype=debug -Ddebug_type=minimal -Db_lto=false)
Build: PASS   local patches applied (report verbatim for PR-folding): [1 — Tracy debug-build assert fix, see local-patches/tracy-debug-assert.patch]
STRID=0 reached lj_str.c? Y | det_pairs refs 0? Y | blake3 in SimChecksum 0? Y
Positive control (perturb): DIVERGED @ tick 49 = good
Single-core sweep runs=10:  10/11 MATCHED   [MPerfBench did not MATCH: it CRASHED (SIGSEGV), not a divergence — see MPerfBench line below]
Thread matrix 1/2/4/8 identical?  M4:Y M3Terr:Y M1Actor:Y M1Terr:Y M2Pairs:Y M2Lua:Y
Audio-ON soak (M4 r30 t900 th8): MATCHED
MPerfBench: crashed = known pre-existing MOID race (SIGSEGV in worker-thread RTTI/dynamic_cast path; not a divergence; r7-unchanged code; not introduced by the repackaging)
VERDICT: GREEN — repackaged stack determinism-equivalent to r7 on Linux

================================================================================
DETAIL
================================================================================

WHAT THIS RUN CONFIRMS
  The per-concern repackaging (PR3 nlohmann vendor, PR4 cross-platform stability,
  PR5 LuaJIT STRID=0, PR6 Lua determinism stock pairs()+math.random+os stubs,
  PR7 M0/M1 + SimChecksum built-in hash + RNG split + reseed, PR8 Path E threaded-sim,
  PR9 harness/scenarios) merged into stage/cumulative-verify preserves, on Linux,
  exactly the determinism that was cleared on the r6/r7 integration branch:
  multi-core == single-core (incl. the stock-pairs()+STRID=0 string-key canaries),
  built-in checksum hash stable run-to-run and divergence-sensitive.

PRE-RUN GATES (never green-by-argument)
  - Pull: git checkout -B stage/cumulative-verify origin/stage/cumulative-verify (origin = local fork /mnt/d/Projects/cccp)
  - HEAD: 0bf49039c "Fix determinism-check length-divergence and scenario grading; comment cleanup" (15 commits over tag cum-base-3-7)
  - Sanity greps: STRID=0 in external/sources/LuaJIT-2.1/src/meson.build = 1 ; det_pairs in LuaMan.cpp = 0 ; blake3 in SimChecksum.cpp = 0
  - STRID placement note: in the repackaged tree the flag lives in lj_defines (src/meson.build:47-48),
    which flows into the library() c_args in the top-level meson.build:25 that compiles ljcore_sources
    (incl. lj_str.c). This is cleaner than r7's direct-on-library() append; same effect.
  - Build: FRESH build dir builddir-cum (no stale-object risk). ninja [875/875] Linking target CortexCommand, exit 0.
  - STRID-reached-compile proof: compile_commands.json entry for lj_str.c carries BOTH
    -DLUAJIT_SECURITY_STRID=0 AND -DLUAJIT_SECURITY_PRNG=0 (71 LuaJIT TUs total). Fresh dir => actually compiled this build.

LOCAL PATCH (repackaging gap to fold into a PR)
  The Tracy debug-build assert fix was NOT present on stage/cumulative-verify and is required for a
  clean debug-minimal run on Linux (the worker-thread broadcast asserts activeTime>=0; system_clock
  can yield ts < m_epoch at startup, tripping the assert). Re-applied from the r6/r7 local patch.
  Patch is byte-for-byte in local-patches/tracy-debug-assert.patch. Suggested home: PR4 (cross-platform stability).

VERIFY BAR (audio ON throughout — FMOD active on WSLg PulseServer; device NoSound, leak surface exercised)
  Harness: builddir-cum/CortexCommand -determinism-check, isolated per-scenario cwds, seed 42.

  Positive control:  M1Baseline --determinism-selftest-perturb --runs 3  -> DIVERGED @ tick 49 (rc=1).  Built-in hash IS sensitive (not a constant false-green).

  Single-core sweep (--threads 1, --ticks 300, --runs 10), 11 scenarios:
    M1Baseline           MATCHED
    M1ActorStress        MATCHED
    M1TerrainStress      MATCHED
    M2LuaBaseline        MATCHED   (STRID canary)
    M2LuaRandomStress    MATCHED
    M2PairsStress        MATCHED   (STRID canary)
    M2OsStubTest         MATCHED
    M2ModSmokeLoading    MATCHED
    M3TerrainStress      MATCHED
    M4ThreadStress       MATCHED
    MPerfBench           CRASHED (SIGSEGV, rc=2) — known pre-existing MOID race, NOT a divergence (see below)
    => 10/11 MATCHED, 0 DIVERGED, 1 known crash.

  Thread matrix (--threads 1,2,4,8, --ticks 300, --runs 10 => 40 child runs each, all diffed vs child 0):
    M4ThreadStress       MATCHED (40 runs across 1,2,4,8)
    M3TerrainStress      MATCHED (40 runs across 1,2,4,8)
    M1ActorStress        MATCHED (40 runs across 1,2,4,8)
    M1TerrainStress      MATCHED (40 runs across 1,2,4,8)
    M2PairsStress        MATCHED (40 runs across 1,2,4,8)   (STRID canary — stock pairs()+STRID=0 holds multi-core)
    M2LuaBaseline        MATCHED (40 runs across 1,2,4,8)   (STRID canary)
    => all 6 identical across 1/2/4/8. multi-core == single-core.

  Audio-ON heavy soak:  M4ThreadStress --threads 8 --runs 30 --ticks 900  -> MATCHED (900 ticks across 30 runs).

KNOWN, NOT A DETERMINISM FAILURE — MPerfBench crash
  MPerfBench SIGSEGV'd on run 2/10 (single-core). Signature: a non-main worker thread crashing in
  "vtable for __cxxabiv1::__enum_type_info" with "Invalid permissions for mapped object" — i.e. an
  RTTI/dynamic_cast read on a dangling/torn MovableObject*. This is the worker-thread side of the
  pre-existing async MOID-rebuild/draw data race that was TSan-named during the r6 run
  (UpdateDrawMOIDs/RegMOID writer vs Draw/AI-MO reader on the same MO). It is:
    - a CRASH (rc=2), NOT a determinism divergence (which is rc=1);
    - in code untouched by the repackaging (and unchanged since r7);
    - timing-dependent (it MATCHED 10/10 in the r7 sweep, crashed here — same Heisenbug, different draw).
  Per the run brief, noted as the known separate bug and run continued. It does not affect the determinism verdict.
  (MPerfBench is a perf brawl scenario, not a determinism scenario.)

VERDICT
  GREEN — the repackaged per-concern stack is determinism-equivalent to r7 on Linux:
  stock pairs() + LUAJIT_SECURITY_STRID=0 holds string-key order multi-core == single-core (both canaries),
  the built-in FNV/splitmix SimChecksum hash is stable run-to-run and divergence-sensitive,
  audio-on soak is clean. No divergences in any determinism scenario at any thread count.
  Only caveat: the pre-existing, separately-tracked async-MOID worker-thread crash still fires under
  MPerfBench load — unrelated to determinism and not introduced by the repackaging.
