# Lua determinism — what changed for mods

The engine now fences the Lua environment so script behaviour reproduces exactly
across runs (the foundation for deterministic multiplayer). Three things changed.
None of them alters a function signature, and a mod that does not depend on
hash-bucket iteration order needs no migration.

## `pairs()` iterates in sorted order

`pairs(t)` now visits keys in a defined order: numeric keys ascending, then string
keys lexicographically. Previously the order was LuaJIT's internal hash-bucket
order, which the Lua manual has always documented as undefined.

- If your mod already treated `pairs()` order as unspecified (the correct
  assumption), nothing changes.
- If you relied on a particular hash order, you now get sorted order instead.
- `pairs()` snapshots the table's keys when called, so keys inserted *during*
  iteration are not visited. Removing keys mid-iteration is still fine.
- Keys that are tables / functions / userdata have an unspecified order relative
  to each other — sort by string or numeric keys if you need a guarantee.

### `pairs_unordered` — the opt-out

The original hash-order builtin is still available as `pairs_unordered`. It is
faster on large tables (it keeps LuaJIT's JIT-compiled fast path), but its order
is **not deterministic across machines**. Only use it for iteration that does not
affect gameplay state — never in code that runs inside the simulation.

## `math.random` is reproducible per activity

Each script state's `math.random` generator is reseeded at the start of every
activity. The same activity started from the same engine seed produces the same
random sequence every time. `math.random()`'s signatures are unchanged.

`math.randomseed` still works. If your mod calls it, it takes over that state's
RNG — that is allowed, but it then owns the reproducibility of its own sequence.

## `os.time` / `os.clock` return sim time

Both now return seconds measured in simulation ticks, not the wall clock, so
sim-context scripts cannot pull in real-world time. If your mod used `os.time()`
for a real timestamp (e.g. a save-file label), read it outside the simulation or
use a different source. `os.date` is unchanged.
