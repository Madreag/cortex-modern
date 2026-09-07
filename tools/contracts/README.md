# Direct restoration contract audit

`-contract-audit` observes the production operation, without repairing its result.
The generated native observer records values and float bits, object/alias identities,
world queues, registry discrepancies, clocks, RNG, terrain and full Lua graphs.
Opaque fields remain explicit gaps. Lua fixtures add independent getters and alias checks.

Run from a built Windows checkout; `run_sim_test` creates owned processes on a hidden
desktop with private writable runtimes and disabled game audio:

```powershell
python tools/contracts/run_audit.py --replay D:/Projects/stage2_p4/fixtures/pickup_fire.ccreplay --out D:/Projects/reviews/native-contract-run --operations observe save stage memory file hold preview --script tools/contracts/mod_native_contracts.lua
```

Use `mod_activity_contracts.lua` for activity and UI values. Use
`mod_reference_contracts.lua --variant-env CC_CONTRACT_REFERENCE --variants ...`
for independent retained-reference controls. See `AUDIT.md` for cases and evidence.
`load:NAME --snapshots PATH` exercises an existing archive through staging/restart.
`CC_CONTRACT_FRESH_DEFAULTS=1` leaves the activity fixture unmutated before loading a
previously saved fixture, detecting manager values accidentally inherited from the old process.

Every run retains source/input hashes, the executable hash, complete output, independent
state differences and losslessly compressed observations. `complete` means the operations
executed cleanly on unchanged inputs; it does **not** mean faithful restoration. Getter
mismatches, graph refusal, native fields and identities must all be assessed. Raw difference
counts include legitimate candidate objects and clock context and are not bug counts.

The instrumentation was built and run with the combined source at audit executable
`65c51ea60dfa281d4c4449bd4b0b41b339ba5334b8d2659793c30a383926b8ef`.
Its separate commit is a decomposition of that tested working state, not an independently
built configuration or a restoration sign-off. The retained declaration generator and full
discovery inventories are in `D:/Projects/reviews/recovery-2026-09-07/contract-audit`.
