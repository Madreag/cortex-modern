# Direct restoration contract audit

`-contract-audit` observes the production operation, without repairing its result.
The generated native observer records values and float bits, object/alias identities,
world queues, registry discrepancies, clocks, RNG, terrain and full Lua graphs.
Opaque fields remain explicit gaps. Lua fixtures add independent getters and alias checks.

Run from a built Windows checkout; `run_sim_test` creates owned processes on a hidden
desktop with private writable runtimes and disabled game audio:

```powershell
python tools/contracts/run_audit.py --replay tools/fixtures/pickup_fire.ccreplay --out <dir> --operations observe save stage memory file hold preview --script tools/contracts/mod_native_contracts.lua
```

Use `mod_activity_contracts.lua` for activity and UI values. Use
`mod_reference_contracts.lua --variant-env CC_CONTRACT_REFERENCE --variants ...`
for independent retained-reference controls.
`load:NAME --snapshots PATH` exercises an existing archive through staging/restart.
`CC_CONTRACT_FRESH_DEFAULTS=1` leaves the activity fixture unmutated before loading a
previously saved fixture, detecting manager values accidentally inherited from the old process.

Every run retains source/input hashes, the executable hash, complete output, independent
state differences and losslessly compressed observations. `complete` means the operations
executed cleanly on unchanged inputs, every ARMED fixture produced a check list, and those
lists reported no mismatches; it does **not** mean faithful restoration. Raw field differences,
native gaps and identities must still be assessed. Raw difference counts include legitimate
candidate objects and clock context and are not bug counts.

The observer itself is `Source/System/ContractAudit.h`: read-only friend access to the classes it records.

## Audit scripts

`--script` stages one of these as the audited activity's script; each reads values through the public Lua API so the
native observer has an independent account to compare with:

- `mod_native_contracts.lua`: every native property of every object class, through its getters.
- `mod_activity_contracts.lua`: the activity's own values.
- `mod_ui_contracts.lua`: the activity-owned UI and its retained native values; `mod_ui_perturb_contracts.lua` adds a
  perturbation hook the `memory-perturb` operation applies, a change the audit must report.
- `mod_primitive_contracts.lua`: drawing calls queued during a tick, read back primitive by primitive.
- `mod_reference_contracts.lua`: retained references to native objects (`--variant-env CC_CONTRACT_REFERENCE`).
- `mod_failure_continuation.lua`: coroutines, iterators and closures that must continue across a checkpoint, and an
  open coroutine cell that must survive a failed transaction.
