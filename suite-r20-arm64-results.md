# Round 20 — arm64 macOS 3-way suite verification (xref/arm64-suite-r20)

**Build:** `experiment/cross-arch-combat @ 9647a0285` (Sort SpatialPartitionGrid MO-query results by MOID).
**Harness:** `-scenario <S> -seed 42 -max-ticks 900 -tick-hashes -num-lua-states 4` per scenario.

## MILESTONE: combat controller convergence (arm64)

| scenario@tick | arm64 controller (first 16) | target |
|---|---|---|
| PerfBench@80 | `06e80c659adaf737` | Windows `06e80c659adaf737` -> **MATCH: YES** |
| ActorStress@246 | `efc21715419b2885` | sorted-order value (lead confirms vs Win/Linux) |
| ThreadStress@82 | `33a2e3fa7a150712` | sorted-order value (lead confirms vs Win/Linux) |

The SpatialPartitionGrid `unordered_set<MOID>` -> sorted `std::set<MOID>` fix makes the AI weapon-pickup
scan platform-independent: arm64 PerfBench@80 controller now equals Windows' `06e80c659adaf737` exactly.

## Full 11-scenario trace fingerprints (arm64) — sha256(all 900 per-tick hashes), first 24 hex

All scenarios ran the full 900 ticks (PerfBench exit=1 is the perf-threshold flag only; trace valid).

| scenario | ticks | total-trace fp | controller-trace fp |
|---|---|---|---|
| SimBaseline | 900 | `f67a49f47ccb0fbbfca709f1` | `86f1d991f0cf64ddbaa5c409` |
| LuaBaseline | 900 | `f2b47d5ce1cc2ca869872dfb` | `e3b0c44298fc1c149afbf4c8` |
| LuaOsStubTest | 900 | `31ed913fe859e9ace292ab6b` | `e3b0c44298fc1c149afbf4c8` |
| LuaPairsStress | 900 | `31ed913fe859e9ace292ab6b` | `e3b0c44298fc1c149afbf4c8` |
| LuaRandomStress | 900 | `153020f21fc3105644445444` | `e3b0c44298fc1c149afbf4c8` |
| ModSmokeLoading | 900 | `9de1d1c5fcf4be2bc5f4011f` | `2f34030b0485e02cf3a2823a` |
| TerrainStress | 900 | `7da8e0a63bf8f1d28364360f` | `97a679541f061fa6d84eec70` |
| TerrainCarveStress | 900 | `0289f224bd4538f6ddde096d` | `3368b6e8960ad5eee55388fc` |
| ActorStress | 900 | `73d2bb34fa23afc4068ced1b` | `836761929e265577d85c6d4e` |
| ThreadStress | 900 | `343b14fbb5092375a5eca2c4` | `e8c68c9d6710ae8e0739eff1` |
| PerfBench | 900 | `c1b630bb4e2a35867bf03d9d` | `10d12273c528cc44bd255702` |

**3-way verdict (arm64 leg):** convergence confirmed at the carrier point (PerfBench@80 == Windows).
Lead diffs these fingerprints vs Win/Linux; if all 11 match 3-way across all 900 ticks, determinism is ACHIEVED.
