# Lua determinism — what changed for mods

The engine now fences the Lua environment so script behaviour reproduces exactly
across runs (the foundation for deterministic multiplayer). Three things changed.
None of them alters a function signature, and a mod that does not depend on
hash-bucket iteration order needs no migration.

## `pairs()` iteration is reproducible across runs

`pairs(t)` is the stock LuaJIT builtin — unchanged. What changed is the VM: LuaJIT
is now built with `LUAJIT_SECURITY_STRID=0`, so string keys get allocation-order
ids instead of randomised ones. A string-keyed table therefore iterates in the
same order on every run of a given build, where before the order shifted from one
process to the next.

- A mod that already treated `pairs()` order as unspecified (the correct
  assumption) needs no change.
- Numeric-keyed iteration was already reproducible and is unaffected.
- Keys that are tables / functions / userdata iterate in a pointer-derived order
  that is **not** reproducible. If your mod iterates an object-keyed table in a way
  that affects gameplay, key it by a stable id (e.g. an entity's `UniqueID`).

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
