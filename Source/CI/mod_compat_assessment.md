# Mod-Compatibility Assessment — M0–M4 + Determinism Stack

**Scope:** every change between the pre-M0 baseline and the integrated M0–M4 +
determinism HEAD that is observable to mods (Lua scripts, INI files, runtime
behaviour Lua can perceive).

**Baseline (pre-M0):** `4df75f38bb55eb97af704f4d4a649aa93ee8012b`
(`git merge-base modernization-effort flagship/determinism-foundation`)

**HEAD assessed:** `daeeac2750b32bf719c6c6495acf9f372b3fccfc`
(branch `flagship/determinism-foundation`)

**Change set:** 51 commits, 133 files, +36 424 / −347 (most of that is vendored
BLAKE3 + nlohmann/json and new test scaffolding, not mod-facing code).

---

## Summary verdict

**No ADR-004 break found.** There is **no breaking change** to any mod-facing
Lua API or INI key across M0–M4. Every Lua binding change is additive, every INI
key change is additive, and no bound method changed its signature or was removed
or renamed.

The change set is, for mods, **a determinism overhaul** — the *observable
behaviour* of `pairs()`, `os.time`/`os.clock`, the engine RNG streams, MO
iteration order, and physics math all change, all intentionally, all toward
*more* reproducibility. A mod that was already correct (treated `pairs()` order
as unspecified, did not use `os.time` for sim logic, did not mutate shared state
from `ThreadedUpdate`) needs no migration. A mod that depended on the
*previously-undefined* behaviour will observe different — but now deterministic —
results.

| Class | Count |
|---|---|
| **BREAKING** | **0** |
| **BEHAVIOUR-CHANGING** | **9** |
| **NON-BREAKING** (additive / internal / cosmetic-only) | **13** |

The nine behaviour-changing items are the substance of this report. Three of
them are the high-risk items the brief calls out (`det_pairs`, the `os` stubs,
the deterministic-merge frozen getters); all three are analysed in depth in the
final section. The single most likely to surprise a real mod is the
`os.time`/`os.clock` semantic change (a mod using `os.time()` for a save-file
timestamp now gets a sim-tick number), and it is the one item here a careful
modder should be told about explicitly — but it is still not an API *break*: the
function exists, is callable, and returns a number.

---

## Classification table

| # | Change | Class | File:line (HEAD) | Rationale |
|---|---|---|---|---|
| 1 | `pairs()` global replaced engine-wide by sorted-iteration `det_pairs` | **BEHAVIOUR-CHANGING** | `Source/Managers/LuaMan.cpp:21-110`, installed `:228` | Global builtin `pairs` is swapped for a C function that snapshots+sorts keys. Iteration order changes from LuaJIT hash-bucket order to (type tag, then value) order for *all* Lua in *all* runs. Lua manual always documented `pairs()` order as undefined, so correct mods are unaffected; mods that relied on a specific order observe a different one. Original kept as `pairs_unordered`. In-depth below. |
| 2 | `os.time` / `os.clock` replaced by sim-tick stubs | **BEHAVIOUR-CHANGING** | `Source/Managers/LuaMan.cpp:113-131`, installed `:232` | Both now return `GetSimUpdateCount() / 60.0` (sim-tick seconds) instead of wall-clock. Return *type* also shifts: stock LuaJIT `os.time()` returns an integer epoch; the stub returns a fractional `lua_Number`. A mod using `os.time()` for a real timestamp now gets a sim value. `os.date`, `os.difftime`, `os.getenv` untouched. In-depth below. |
| 3 | Deterministic-merge: `Controller:IsState`, `AHuman.EquippedItem`, `AHuman.FirearmIsReady`, `ACrab.EquippedItem`, `ACrab.FirearmIsReady` return a frozen snapshot for a *foreign* actor during the parallel `ThreadedUpdateAI` pass | **BEHAVIOUR-CHANGING** | `Source/System/Controller.h:179-184`; `Source/Entities/AHuman.h:332-337`; `Source/Entities/AHuman.cpp:1181-1184`; `Source/Entities/ACrab.cpp:569-572,581-584` | During `ThreadedUpdateAI`, if a mod's AI Lua reads *another* actor's `EquippedItem`/`FirearmIsReady`/controller state, it now gets that actor's pre-phase snapshot, not its live (concurrently-mutating) state. Own-actor reads stay live. Closes a real data race; under the old code a foreign read was a race (torn/garbage), so this is a correctness fix, but the *observable value* during that window changes. In-depth below. |
| 4 | `MovableMan.Actors` / `.Items` / `.Particles` iterate in `UniqueID` order | **BEHAVIOUR-CHANGING** | `Source/Managers/MovableMan.cpp:1377-1379` (frame-start sort); bindings `Source/Lua/LuaBindingsManagers.cpp:117-119` | M1 Block C sorts `m_Actors`/`m_Items`/`m_Particles` by `GetUniqueID()` once at the top of `MovableMan::Update()`. These three deques are Lua-iterable via `return_stl_iterator`. A mod doing `for actor in MovableMan.Actors` now sees actors in stable `UniqueID` order instead of insertion/spawn order. No API change; order changes. |
| 5 | `MovableMan.AddedActors` / `.AddedItems` / `.AddedParticles` iterate in `UniqueID` order | **BEHAVIOUR-CHANGING** | `Source/Managers/MovableMan.cpp:1571-1573`; bindings `Source/Lua/LuaBindingsManagers.cpp:120-122` | Same as #4 for the just-added queues, sorted before the MO-transfer drain. Lua-iterable. Order changes; no API change. |
| 6 | `MovableMan.AddedAlarmEvents` (and downstream `AlarmEvents`) iterate in a stable content-key order | **BEHAVIOUR-CHANGING** | `Source/Managers/MovableMan.cpp:1367` (sort by `AlarmEventLess`: team, scenePos.X, scenePos.Y, range); bindings `Source/Lua/LuaBindingsManagers.cpp:123-124` | M4 Block D sorts `m_AddedAlarmEvents` by a content key before the frame-start drain into `m_AlarmEvents`. Both are Lua-iterable. A mod iterating alarm events sees a canonical order instead of worker-thread append order. Order changes; no API change. |
| 7 | Engine RNG split into `g_SimRNG` / `g_RenderRNG`; ~dozens of call sites re-routed | **BEHAVIOUR-CHANGING** | `Source/System/RTETools.cpp:11-47`, `RTETools.h:94-181`; re-routed sites e.g. `MOSParticle.cpp:131`, `TerrainDebris.cpp:116,199,203,216,229`, `SoundSet.cpp:264-273`, `DynamicSong.cpp:151-200`, `SLBackground.cpp:182` | M1 Block B splits the one global RNG into a sim RNG and a render/cosmetic RNG. Cosmetic consumers (particle visual jitter, sound/music selection, debris flip/rotation, background frame) move to `g_RenderRNG`; sim consumers stay on `g_SimRNG`. Lua's `RangeRand`/`PosRand`/`math.random` shims route to the *sim* RNG (via `g_RandomGenerator` alias — `RTETools.cpp:18`). Because the two streams no longer interleave, the *exact value* a Lua RNG call returns at a given moment differs from a pre-M1 build. No Lua signature change. |
| 8 | Per-activity reseed of every Lua state's RNG | **BEHAVIOUR-CHANGING** | `Source/Entities/Activity.cpp:301`, `LuaMan.cpp` `SeedAllLuaRNGs` `:1518-1524` | `Activity::Start()` now reseeds every Lua state's `m_RandomGenerator` (which powers `math.random`/`RangeRand`/`PosRand`/`NormalRand`). Same activity from same engine seed → same Lua random sequence every run. Previously each Lua state was seeded once with a `RandomNum<uint64_t>` pull at state creation and never reseeded. The sequence a mod sees is now reproducible and tied to activity start, not to a one-time boot draw. `math.randomseed` still works and still overrides. |
| 9 | Physics math (atom collision response, terrain penetration/carve) converted from `float` to Q40.24 fixed-point; FP compile flags pinned `/fp:fast`→`/fp:precise` | **BEHAVIOUR-CHANGING** | `Source/System/Atom.cpp:332-440`, `AtomGroup.cpp` (Travel/PushTravel/ResolveMOSIntersection), `SceneMan.cpp:487-660+`, `SLTerrain.cpp:401-404`; flags `meson.build:33-95`, `RTEA.vcxproj` (10 configs) | M3 reworks collision/penetration/carve math in fixed-point; M1 Block E pins FP compilation to Precise + SSE2. Both change the *bit pattern* of physics results. A mod reading post-collision `actor.Vel`, `ResImpulse`, or terrain-carve geometry may see sub-pixel/sub-ULP differences vs a pre-M3 build. `Vector` and `Material` stay float in INI and Lua; no API or INI change. Intentional: makes physics reproducible cross-machine. |
| 10 | 5 new manager classes bound to Lua as globals: `AIDecisionChannel`, `AIDebugOverlay`, `MetricsCollector`, `NetworkSimulator`, `SimChecksum` | **NON-BREAKING** | `Source/Lua/LuaBindingsAI.cpp:56-120`; globals `Source/Managers/LuaMan.cpp:408-412`; registry `LuaBindingRegisterDefinitions.h:19-20,33,35,196-197,210,212` | Purely additive. New global names; no bundled mod or scanned mod uses these identifiers (verified — no hits in `Data/` outside `Tests.rte`). Worst case is a name collision with a mod that already defined a global called e.g. `SimChecksum`; luabind globals are assigned at state init, so a mod would shadow the binding afterward — extremely unlikely and not a regression in any observed mod. |
| 11 | New Lua-bound method `MovableMan:RegisterAlarmEvent` | **NON-BREAKING** | `Source/Lua/LuaBindingsManagers.cpp:125` | The C++ `MovableMan::RegisterAlarmEvent` existed at baseline (`MovableMan.h:431`) but was **not** Lua-bound. The change adds the binding. Additive — gives Lua a new method, removes nothing. |
| 12 | New INI key `IsTestActivity` on `Activity` | **NON-BREAKING** | `Source/Entities/Activity.cpp:151` (ReadProperty), `:254-255` (Save) | New optional bool property; defaults `false` (`Activity.cpp:46`). Activities without it parse exactly as before. Save now also writes it. Additive. |
| 13 | New INI key `ShowTestActivities` on `SettingsMan` | **NON-BREAKING** | `Source/Managers/SettingsMan.cpp:189` (ReadProperty), `:332` (Save) | New optional bool; default `true` in DEBUG builds / `false` in Final (`SettingsMan.cpp:44-48`). Settings.ini without it parses as before. Additive. |
| 14 | New INI keys `SimulatedLatencyMs` / `SimulatedLossPct` / `SimulatedJitterMs` on `SettingsMan` | **NON-BREAKING** | `Source/Managers/SettingsMan.cpp:190-192` (ReadProperty), `:333-335` (Save) | Three new optional int keys routed into `g_NetworkSimulator`. Default 0 (atomic init). Settings.ini without them parses as before. Additive. |
| 15 | New stock-AI global Lua function `AIEmit` + `AI/AIEmit.lua` module | **NON-BREAKING** | `Data/Base.rte/AI/AIEmit.lua:18` | New global function defined in a new Base.rte module. No bundled/scanned mod defines `AIEmit` (verified). Additive — a mod could shadow it harmlessly. |
| 16 | Stock AI Lua scripts gain `AIEmit(...)` calls + `require("AI/AIEmit")` | **NON-BREAKING / BEHAVIOUR-CHANGING (stock AI only)** | `Data/Base.rte/AI/NativeHumanAI.lua:87-105`, `NativeCrabAI.lua:63-81`, `NativeTurretAI.lua:48-64`, `NativeDropShipAI.lua:72-74`, `RocketAI.lua:71-85`, `HumanBehaviors.lua`, `CrabBehaviors.lua`, `SharedBehaviors.lua` (multiple) | These are *content* edits to Base.rte AI scripts, not engine API. `AIEmit` is null-guarded (`AIEmit.lua:19`) so it is inert when `AIDecisionChannel` is absent. Adds new fields to AI tables (`_m0_lastTargetID`, `lastAIMode`) — a mod that subclasses/extends the stock AI tables and happens to use those exact field names could collide, but the names are namespaced (`_m0_`) precisely to avoid this. A mod that *replaces* the stock AI wholesale is unaffected. No engine-API surface. |
| 17 | `pairs_unordered` new global (the opt-out for #1) | **NON-BREAKING** | `Source/Managers/LuaMan.cpp:107-110` | Additive global exposing the original hash-order builtin. New name; nothing removed. |
| 18 | `Scene::Update` pathfinding cadence: `IsPastRealMS(100)` → `IsPastSimMS(100)` | **NON-BREAKING (behaviour nuance)** | `Source/Entities/Scene.cpp:2490` | Partial pathfinding updates are now gated on sim time, not wall-clock. Not directly Lua-observable as an API; a mod cannot read this timer. Effect on AI pathing cadence is subtle and only matters when wall-clock and sim-clock diverge (lag/fast-forward). No API or INI change. Listed for completeness. |
| 19 | `MovableMan::CompleteQueuedMOIDDrawings` renamed to `CompleteDeferredSimTasks` | **NON-BREAKING** | `Source/Managers/MovableMan.h:465`, `MovableMan.cpp:2134`; callers `Main.cpp:501`, `MovableMan.cpp:1336` | Internal C++ method, **not** Lua-bound (verified — no binding in any `LuaBindings*.cpp`). Rename is invisible to mods. |
| 20 | New CLI args `-scenario` / `-out` / `-seed` / `-max-ticks` / `-tick-hashes` / `-determinism-selftest-perturb`; env vars `CCCP_SIM_THREADS`, `-num-lua-states` | **NON-BREAKING** | `Source/Main.cpp:221-228`; `LuaMan.cpp:510-513` | New command-line surface for the determinism harness. Not Lua- or INI-observable. Additive. |
| 21 | `IsTestActivity` activities are hidden from the scenarios menu and hard-auto-exit after a tick budget | **NON-BREAKING (opt-in)** | `Source/Menus/ScenarioGUI.cpp:225-232`; `Source/Main.cpp:368-440` | The menu filter and the auto-exit *only* fire for an activity whose `IsTestActivity` INI property is `true`. The property defaults `false`, so no pre-existing mod activity is affected. A mod would have to explicitly opt in by setting `IsTestActivity = 1`. Additive/opt-in. |
| 22 | `RandomGenerator::SerializeStateForHashing` + `LuaStateWrapper` RNG serialize/seed helpers; `t_simRNGOverride` / `DeterministicMORNGScope` | **NON-BREAKING** | `Source/System/RTETools.h:35-46`; `LuaMan.h:65-70,475-498`; `LuaMan.cpp:226-292` | New C++-only infrastructure for checksumming/redirecting RNG. Not Lua-bound. The *effect* of `DeterministicMORNGScope` on observable RNG values is captured under #7/#8 (per-object RNG in threaded hooks); the class itself is not a mod surface. |

---

## In-depth: the three high-risk items

### A. `det_pairs` replaces global `pairs()` for ALL Lua in ALL runs

**Class: BEHAVIOUR-CHANGING. Not breaking.**

**What changed.** `LuaMan.cpp:107-110` (`RegisterDeterministicPairs`), called
unconditionally from every `LuaStateWrapper::Initialize()` (`LuaMan.cpp:228`),
does:

```
lua_getglobal(L, "pairs");
lua_setglobal(L, "pairs_unordered");   // original saved here
lua_pushcfunction(L, det_pairs);
lua_setglobal(L, "pairs");             // global pairs is now det_pairs
```

This runs for **every** Lua state — master and all threaded states — on **every**
launch, with no setting to disable it. Every mod's `pairs()` is affected.

**The new behaviour** (`det_pairs`, `LuaMan.cpp:64-105`):

1. Snapshots every key of the table into a fresh array (`lua_next` walk).
2. Sorts that array with `det_pairs_compare` (`LuaMan.cpp:25-49`): primary key is
   the Lua type tag (`LUA_TBOOLEAN=1` < `LUA_TNUMBER=3` < `LUA_TSTRING=4`, per
   `LuaJIT-2.1/src/lua.h:76-79`), secondary key is the value (numeric `<`,
   `memcmp` for strings, boolean `<`). Table/function/userdata keys all compare
   equal to each other — their relative order stays unspecified.
3. Returns a closure walking the sorted snapshot, plus `t` and `nil`, so the
   return shape is the full `(iterator, state, control)` triple Lua's `pairs()`
   contract specifies. (The triple was completed in audit commit `2a1e0e0de`;
   the original M2 Block C commit `9bef5a9ed` returned only the closure.)

**Why this is not a break:**

- **The Lua reference manual has always documented `pairs()`/`next()` traversal
  order as unspecified.** Code that relied on a particular order was relying on
  undefined behaviour. `det_pairs` does not violate the contract — it tightens
  an under-specified one.
- **No signature change.** `pairs(t)` still takes a table and is still usable as
  `for k, v in pairs(t)`. `det_pairs` calls `luaL_checktype(L, 1, LUA_TTABLE)`,
  matching stock `pairs` argument checking.
- **`ipairs` is untouched.** Array-style iteration (`1..n`, integer keys, stop at
  first `nil`) is unaffected — and array iteration order never changes anyway.
- **The original is preserved** as `pairs_unordered` for hot loops that want
  LuaJIT's JIT-compiled fast path back.

**Residual risk a modder should know:**

- **Mixed-key tables.** A table with both array part (`[1]..[n]`) and named
  string fields will, under `det_pairs`, always yield the numeric keys first
  (tag 3 < tag 4) in ascending order, then string keys lexicographically. Under
  stock LuaJIT the interleaving was hash-bucket arbitrary. A mod that *happened*
  to get a useful order before will get a different (but now stable) one.
- **Snapshot semantics.** Keys are snapshotted at the `pairs()` call. A key
  **inserted** mid-iteration is **not** visited; a key **removed** mid-iteration
  **is still visited** and its value reads as `nil` (`det_pairs_iter` does a live
  `lua_rawget` against the source table — `LuaMan.cpp:52-62`). Stock LuaJIT
  `next()` says modifying a table during traversal (other than clearing an
  existing field) is undefined, so a correct mod never did this; one that did
  gets a subtly different — but defined — outcome.
- **Performance.** `det_pairs` is O(N log N) per `pairs()` call (snapshot + sort)
  and is not JIT-recordable. A mod with a `pairs()` over a large table in a
  hot path will be slower. `pairs_unordered` is the documented escape hatch
  (`Data/Modding/lua-determinism.md:23-28`).

**Verdict:** the highest-blast-radius change in the set, but correctly classed
**behaviour-changing, not breaking** — it strengthens a contract Lua always left
loose, keeps the signature, and ships an opt-out.

### B. `os.time` / `os.clock` sim-tick stubs

**Class: BEHAVIOUR-CHANGING. Not breaking — but the closest thing in this set to
a surprise for a real mod.**

**What changed.** `LuaMan.cpp:113-131` replaces two fields of the global `os`
table on every Lua state at init (`LuaMan.cpp:232`):

```
os.time  = function returning  g_TimerMan.GetSimUpdateCount() / 60.0
os.clock = function returning  g_TimerMan.GetSimUpdateCount() / 60.0
```

Both ignore all arguments and return sim-tick seconds.

**Two distinct observable changes:**

1. **Value semantics.** Both used to return wall-clock — `os.time()` the real
   Unix epoch, `os.clock()` real CPU seconds since program start. Both now return
   a sim-tick count divided by 60. Inside the simulation this is the point: a sim
   script can no longer pull non-deterministic real-world time. Outside the
   simulation (menus, save-file labelling) this is a real semantic loss.
2. **Return-type / range shift.** Stock LuaJIT `os.time()` returns an **integer**
   epoch (~1.7×10⁹ today) and accepts an optional date-table argument;
   `os.clock()` returns a fractional double. The stub makes **both** return a
   small fractional `lua_Number` starting near 0 at activity start, and `os.time`
   no longer honours its date-table argument. A mod doing
   `os.time{year=...,month=...}` to build a specific timestamp gets a wrong
   answer; a mod treating `os.time()` as a large integer (e.g. modulo for a
   pseudo-seed, or string-formatting an epoch) gets a small number instead.

**Why this is still not an API break:** the functions **exist**, are
**callable**, take the **same call form** for the no-argument case, and return a
**number**. Nothing throws; nothing is `nil`. A script calling `os.time()`
compiles and runs. The contract that loosened ("returns *the wall clock*") was
never a hard signature.

**Mitigating facts:**

- **No bundled Lua and no scanned mod uses `os.time` or `os.clock`** (verified —
  zero hits in `Data/Base.rte/` and across the mod corpus for both names). The
  M2 Block D commit message states the change is "foreclosure only — neither has
  callsites in vanilla Base.rte or the mod corpus."
- **`os.date` is deliberately left intact.** `Base.rte/LuaIntegration/socket/`
  `smtp.lua:143,226` uses `os.date` — the bundled LuaSocket library — and still
  gets real wall-clock dates. `os.difftime` and `os.getenv` are also untouched.

**Verdict:** correctly **behaviour-changing**. It is the one item here worth
calling out to modders in release notes (and `Data/Modding/lua-determinism.md:39`
already does: "If your mod used `os.time()` for a real timestamp ... read it
outside the simulation or use a different source"). Still **not** an ADR-004
break: no removal, no rename, no signature change, no error.

### C. Deterministic-merge frozen getters for the parallel `ThreadedUpdateAI` pass

**Class: BEHAVIOUR-CHANGING. Not breaking — it is a race fix; the value during
one specific window changes.** (Commit `efc82839b`.)

**What changed.** During `MovableMan::UpdateControllers()` the AI of all actors
runs across worker threads. Before each parallel phase, every actor's
cross-actor-read state is snapshotted (`MovableMan.cpp:1975-1977`):

```
for (Actor* actor : m_Actors) { actor->FreezeStateForAIPhase(); }
```

and a `thread_local Actor* g_CurrentAIActor` (`Controller.cpp:13`,
`Controller.h:82`) is set to the actor currently running on this thread
(`MovableMan.cpp:1985-1986,1998-2000`).

Five Lua-bound getters then branch on `g_CurrentAIActor`:

- `Controller::IsState` — `Controller.h:179-184` — Lua-bound `Controller:IsState`
  (`LuaBindingsSystem.cpp:57`).
- `AHuman::GetEquippedItem` — `AHuman.h:332-337` — Lua property
  `AHuman.EquippedItem` (`LuaBindingsEntities.cpp:427`).
- `AHuman::FirearmIsReady` — `AHuman.cpp:1181-1184` — Lua property
  `AHuman.FirearmIsReady` (`LuaBindingsEntities.cpp:430`).
- `ACrab::GetEquippedItem` — `ACrab.cpp:569-572` — Lua property
  `ACrab.EquippedItem` (`LuaBindingsEntities.cpp:63`).
- `ACrab::FirearmIsReady` — `ACrab.cpp:581-584` — Lua property
  `ACrab.FirearmIsReady` (`LuaBindingsEntities.cpp:64`).

Each does, in effect: *if an AI phase is running (`g_CurrentAIActor` non-null)
and the actor being queried is **not** the one this thread is updating, return
the frozen snapshot; otherwise return the live value.*

**The mod-observable consequence.** If a mod's AI Lua (`ThreadedUpdateAI`, or
anything it transitively calls during that phase) reads **another** actor's
`EquippedItem` / `FirearmIsReady` / controller `IsState`, it now receives that
actor's state **as of the start of the AI phase**, not its live state. For the
actor whose AI is running, all reads are still live. For a player-controlled
actor read by an AI, `m_ControlledActor != g_CurrentAIActor` so the controller
read also returns the frozen copy.

**Can a mod observe stale state? Yes — by exactly one AI phase, and only for a
foreign actor.** A mod's AI that, mid-tick, reads an ally's or enemy's
`FirearmIsReady` will see the value from the phase boundary, even if that other
actor's own (concurrently-running) AI has since equipped/holstered something.
The staleness window is bounded by one `ThreadedUpdateAI` pass.

**Why this is a fix, not a regression:**

- Under the **old** code that same foreign read was a **data race**: actor A's
  AI thread read `B->FirearmIsReady()` while actor B's AI thread concurrently
  mutated B's arm/turret state. The result was undefined — a torn `bool`, a
  half-updated pointer, potentially a crash on a dangling `HeldDevice*`. The
  commit message: "let one actor's AI read another actor's AI-phase-mutated
  state ... while that actor's own parallel AI concurrently mutated it."
- The new code replaces *undefined garbage* with a *defined, consistent,
  one-phase-stale snapshot*. A mod relying on the old behaviour was relying on a
  race; it cannot have been depending on a *correct* live cross-actor value,
  because there was no correct live value to depend on.
- The snapshot is taken **after** `Controller::Update()` for all actors
  (`MovableMan.cpp:1971-1977`) — i.e. after that tick's player/AI input is
  applied — so it is a coherent end-of-controller-update view, not arbitrary.

**Residual nuance.** A single-threaded build, or any path where
`ShouldUpdateAIThisFrame()` is false, still hits the live branch
(`g_CurrentAIActor` is null outside the phase). So a mod that reads these getters
**outside** `ThreadedUpdateAI` — from `Update`, `SyncedUpdate`, a UI script, the
console — sees live values exactly as before. The behaviour change is scoped
precisely to foreign reads *inside the parallel AI phase*. `m_FrozenEquippedItem`
holds a raw `MovableObject*`/`HeldDevice*`; it is refreshed every phase
(`FreezeStateForAIPhase`), so it cannot dangle within a tick — but a mod must not
cache it across ticks (the same caveat that already applied to any MO pointer;
the stock AI scripts compare by `UniqueID` for exactly this reason —
`NativeHumanAI.lua:97`).

**Verdict:** correctly **behaviour-changing**. It changes the value of five
Lua-bound getters within one narrow window, but only by replacing a
previously-racy/undefined read with a deterministic stale-by-one-phase snapshot.
No signature change, no removal — **not** an ADR-004 break, and a net correctness
gain for any mod that did cross-actor AI reads.

---

## Cross-checks performed

- **Lua bindings diff** (`LuaBindingsAI.cpp`, `LuaBindings*.cpp`,
  `LuaBindingRegisterDefinitions.h`, `LuabindObjectWrapper.cpp`): every change is
  an *addition* — 5 new manager classes, 1 new `MovableMan` method. No bound
  method removed, renamed, or signature-changed. Confirmed `RegisterAlarmEvent`'s
  C++ method pre-existed at baseline but was unbound, so the binding is new and
  additive.
- **`math.random` Lua shim** (`LuaMan.cpp:257-262`): the
  `SelectRand`/`RangeRand`/`PosRand`/`NormalRand`/`math.random` Lua-side shim
  block is **byte-identical** between baseline and HEAD (the M0–M4 change set
  altered only the C++ RNG those shims route into, not the shim itself).
- **INI ReadProperty/Save diff** across `Source/`: four new keys total
  (`IsTestActivity`, `ShowTestActivities`, `SimulatedLatencyMs/LossPct/JitterMs`).
  No INI key removed, renamed, or repurposed. All have safe defaults; pre-M0 INI
  files parse unchanged.
- **New global names vs mod corpus**: `grep` of `Data/` (excluding `Tests.rte`)
  for `AIDecisionChannel`, `AIDebugOverlay`, `MetricsCollector`, `SimChecksum`,
  `NetworkSimulator`, `AIEmit`, `pairs_unordered` — no collisions in bundled
  content.
- **`os.time`/`os.clock` callers in bundled Lua**: zero. `os.date` (used by
  bundled LuaSocket) is intentionally untouched.
- **Method rename `CompleteQueuedMOIDDrawings`**: not Lua-bound; rename invisible
  to mods.
- The engine's own modding docs (`Data/Modding/lua-determinism.md`,
  `threaded-determinism.md`, `fixed-point-physics.md` — all three new in this
  change set) independently corroborate this assessment: each states no function
  signature and no INI property changed.

---

## Bottom line

The M0–M4 + determinism stack contains **zero breaking changes** to mod-facing
Lua APIs or INI keys — **no ADR-004 break**. It is a behaviour-determinism
overhaul: nine behaviour-changing items, every one of them *tightening* a
previously-loose or previously-racy behaviour toward reproducibility, none of
them altering a signature or removing a symbol. A correct mod (one not depending
on `pairs()` hash order, not using `os.time` for sim logic, not racing shared
state from `ThreadedUpdate`, not depending on RNG-stream interleaving) needs no
migration. The single item worth an explicit line in modder-facing release notes
is the `os.time`/`os.clock` sim-tick semantics (#2) — and even that is a
behaviour change, not an API break.
