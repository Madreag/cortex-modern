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

`SyncedUpdate` runs the objects in one global order by unique ID across every Lua
state, so there is no per-state grouping for a script to sequence its shared writes on.

### Global tables are per Lua state, and which objects share one is fixed

Every Lua state has its own globals, so a global table written in `SyncedUpdate` is
shared by the objects that sit on the same state — not by every object in the world.
The number of states is the same on every machine (32, a build constant — it is no
longer a setting or a command-line option), and which state an object sits on is the
same on every machine too, so two players in the same match group the same objects in
the same table, whatever either machine did before the match started.

The rule an object's state follows, in full:

- normally, and for anything spawned from `SyncedUpdate`, it is the object's `UniqueID`
  modulo the number of states;
- an object spawned from `ThreadedUpdate` takes the *spawner's* state instead — that hook
  runs in parallel, one thread per state, and a new object cannot be handed a state another
  thread is using. It is the same answer on every peer, but it is not the object's own; one
  more reason to spawn from `SyncedUpdate`;
- a preset with `ForceIntoMasterLuaState = 1` (the Base.rte automovers) is always on the
  master state;
- an object restored from a save or received in a join goes back to the state the image
  recorded for it, which is where its own saved fields are — the same state the peers that
  stayed in the match have it on.

What a mod author needs to know: a global written in `SyncedUpdate` is visible to the
objects whose unique IDs land on the writer's state, identically on every peer. If your
script means "all of my objects", key a table by `UniqueID` and write it from every
object, or keep the shared value on an object (an activity or a chosen owner) rather
than in a global. Nothing here changes a script API, and a mod that only reads and
writes its own object is unaffected.

Running the game with `EnableLuaDebugging` puts every script on the single master state
(no threaded states). That is a single-player debugging mode: hosting or joining a
network match is refused while it is on, because the master state runs `SyncedUpdate`
on a different schedule from the threaded ones.

Memory: 32 states cost about 172 MB more than four on the same machine (about 6.2 MB
per state), and startup is flat across that range — the priority thread pool is already
as wide as the machine's cores, so the extra states queue as tasks rather than threads.

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
thread count or which Lua state it was assigned to. The collision callbacks
`OnCollideWithMO` / `OnCollideWithTerrain` draw from the same per-object stream.
Scripts need no change — the signatures and ranges are identical; only the
determinism guarantee is new.

## Nothing else changes

No script function changed signature; no INI property changed. A mod that does
not mutate shared state from `ThreadedUpdate` needs no migration at all.
