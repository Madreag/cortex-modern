# Threaded-sim determinism — what changed for mods

Cortex Command runs parts of the simulation across worker threads — actor AI,
the `ThreadedUpdate` script hook, and see-ray vision casting. This work makes
that threaded work reproducible: on one machine the simulation produces the same
result regardless of how many threads (CPU cores) it runs on. That same-machine
reproducibility is what replay and the determinism checks rely on, and it is the
sim-side foundation the multiplayer work builds on (which keeps clients in sync
by exchanging Controller input, not by syncing AI). None of this changes a script
API or an INI property. One modding habit matters, and it is described below.

## `SyncedUpdate` is the channel for changing shared simulation state

A `MovableObject` script can define two per-frame hooks:

- **`ThreadedUpdate`** runs on a worker thread, in parallel with other objects'
  `ThreadedUpdate`. Use it for work that only reads the world and writes the
  script's own object — the common case: timers, state machines, self
  animation, reading nearby objects.
- **`SyncedUpdate`** runs in a serial, ordered pass. Use it for anything that
  changes *shared* simulation state — spawning objects (`MovableMan:AddMO` and
  friends), modifying other objects, writing global tables.

A script asks for a `SyncedUpdate` by calling `self:RequestSyncedUpdate()`; the
hook then runs that frame. This was always the designed boundary — this work
makes it the one to rely on for determinism.

**Rule of thumb: if your script changes state that is not its own object, do it
in `SyncedUpdate`, not `ThreadedUpdate`.** A `ThreadedUpdate` that mutates shared
state was always a latent data race — two objects' `ThreadedUpdate`s run at the
same time on different cores. This does not newly break such a mod; it surfaces a
bug that was already there. Vanilla `Base.rte` already follows this — objects
that spawn from a threaded hook defer the spawn to `SyncedUpdate`.

The built-in `Equip*` deferral covers an AI equipping its own actor. A threaded
script that mutates a *different* actor must use `SyncedUpdate` — concurrent
`Equip*` calls against the same third actor from two threads are not synchronized.

## `math.random` in a threaded hook is per-object

Inside `ThreadedUpdate`, `ThreadedUpdateAI`, `UpdateAI`, `Update` and the other
per-object script hooks, `math.random` (and `RangeRand` / `SelectRand` /
`PosRand` / `NormalRand`) now draws from a generator keyed to the object and the
sim tick. The same object on the same tick draws the same sequence regardless of
thread count or which Lua state it was assigned to. Scripts need no change — the
signatures and ranges are identical; only the determinism guarantee is new.

## Nothing else changes

No script function changed signature; no INI property changed. A mod that does
not mutate shared state from `ThreadedUpdate` needs no migration at all.
