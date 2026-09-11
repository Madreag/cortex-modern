# W9: `AI_StuckForTime` shared-state residual

Read-only. Jobs found under `D:\mx\s41r2\` (not under `D:\mx\s41`). Engine tree `D:\Projects\p4b-interp-validation` unread for writes. Commands and extract files are listed with each claim.

Commands:

```
python D:\Projects\reviews\takeover-20260909\grok-workers\w9-ai-stuckfortime\walk_jobs.py
python D:\Projects\reviews\takeover-20260909\grok-workers\w9-ai-stuckfortime\extract_dummy_rows.py
```

Outputs: `job_roots.json`, `inventory.json`, `extracts.json`, `dummy_rows.json`.

`D:\mx\s41` exists and has no `heal_global_d0` / `heal_global_d3` directories. Both jobs are `D:\mx\s41r2\heal_global_d0` and `D:\mx\s41r2\heal_global_d3`. Walk skipped `Data` / `external` / `modules` and reparse points. 36 `*.json`/`*.log`/`*.txt` files under 20 MB hashed in `inventory.json`. `.ccsave` files were not read.

---

## 1. Evidence

### Review classification

`D:\Projects\reviews\takeover-20260909\source41-matrix-review\continuation-20260910\review.md:3`:

> 42 of the 44 new completed jobs pass their explicitly bounded review scope. `heal_global_d0` and `heal_global_d3` retain an open shared-state residual.

`review.md:50-52`:

> In each global-heal case, only the host's UID 1048653 has `NumberValues.AI_StuckForTime`. Removing precisely that text from the host row makes it equal to the client row; all other rows are already byte-identical. This classification does **not** waive the difference.
>
> `Data/Base.rte/AI/SharedBehaviors.lua:547` writes the field. `Data/Base.rte/Scenes/Objects/Bunkers/BunkerSystems/Automovers/Controller/Controller.lua:1135–1148` reads its existence/value and changes movement, including a random deviation. It is a script-visible NumberValue with an unchanged stock consumer. No evidence justifies a generic local-AI exclusion. Both jobs remain `OPEN_SHARED_STATE_RESIDUAL` pending a field-specific causal investigation and detecting control.

`raw-state-findings.md:9-10` (same folder):

| Job | Host-only value | Object at tick 400 |
|---|---|---|
| `heal_global_d0` | `AI_StuckForTime:0x1.d4bb333333333p+11` | UID 1048653 Green Dummy; particle list, status 4, health `-0x1.8a3d700000000p+1` |
| `heal_global_d3` | `AI_StuckForTime:0x1.963bd70a3d70ap+12` | UID 1048653 Green Dummy; actor list, status 0, health 100 |

The earlier `source41-matrix-review\review.md` does not name these jobs (they were not in that 26-job slice).

Comparator: `verify_oracles.py:246-254` sets `OPEN_SHARED_STATE_RESIDUAL` when the only raw-dump difference is host `AI_StuckForTime` and asserts `only_host_AI_StuckForTime`. Full rows: `oracle-review.json` `heal_global_d0.raw_dump.different_lines[0]` (file line 21148) and `heal_global_d3` (file line 21481).

### Comparator output (path:line)

Extract: `dummy_rows.json` from the four simdumps. `CC_SIM_DUMP=400:400` (`inspection.json` host/client env). One dump tick. No pre-heal dump.

**`heal_global_d0`** — `D:\mx\s41r2\heal_global_d0\fresh\e2e\resync_heal\<peer>\trace.json.simdump.txt:55`

- Object: UniqueID `1048653`, preset `Green Dummy`, class `AHuman` (particle list at dump; attachables/wounds still parented).
- `team=-1`, `mode=2` (`CIM_AI`), `aimode=4` (`AIMODE_BRAINHUNT`), `status=4` (DEAD), `dis=1`, `health=-0x1.8a3d700000000p+1` (~-3.08), `script=1/399/399/0`.
- Host `nv=`: `AI_StuckForTime:0x1.d4bb333333333p+11` (3749.85 ms); `TestCheckpointState`; `TestUpdates:0x1.8f00000000000p+8` (399); `global_script_spawn:1`.
- Client `nv=`: same three keys, no `AI_StuckForTime`.
- Removing the host `AI_StuckForTime:...;` text makes the rows byte-identical (`oracle-review.json` `only_host_AI_StuckForTime: true`).
- Hashes (`raw-state-findings.md:16-17`, re-checked `inventory.json`): host `ad52563332f6216f3a0df4ead383f3cf1888d7c5aebd0573b6830284a892f5ee` 43067 B; client `76b5814a4550fe5dd676be5fd4f55acd7dabc8d3bf01e65a507faf348639c462` 43029 B (38-byte gap = the missing key/value).

**`heal_global_d3`** — `...\heal_global_d3\...\trace.json.simdump.txt:72`

- Same UID/preset/class/team/mode/aimode. Actor list, `status=0`, `dis=0`, `health=0x1.9000000000000p+6` (100), `script=1/399/399/0`.
- Host `AI_StuckForTime:0x1.963bd70a3d70ap+12` (6499.74 ms). Client lacks that key. Other three NumberValues match.
- Hashes: host `d987ae13b1bc34086b9ce587312b25c357dec81f6e9a0f8694ff9a0ca1ce197f` 85828 B; client `b1a1f9aaff5ccee8c008d3a97015ede07f4cdea117e05b065f3ba1ec9ceab33f` 85790 B.

### When the difference exists

Heal is at tick 60, not at tick 400.

- `result.json` (both jobs): `"the host perturbs its sim at 50; runtime desync detection must fire and both peers must heal from the host's snapshot"`.
- `Main.cpp:2260`: perturb at `simTick == 50`. `Main.cpp:2248`: desync sample every 30 ticks → next sample is tick 60.
- Host console `heal_global_d0\...\host\trace.json.console.txt:13`: `NETWORK: Resyncing from the host (1): Desync:sim state diverged at tick 60 (Client)`.
- Host console `heal_global_d3\...\host\trace.json.console.txt:13`: same tick-60 desync.
- Client d0 console `:14`: `tick 62 lockstep stopped: Desync:sim state diverged at tick 60 (Client)`.
- Both peers reload (`stdout.log` `[net-match] resync: desync detected, reloading from the host snapshot`; `NetMatchService.cpp:188-190`).
- Dummy spawned once: console `[global-callback] start=1 actor=1048653` (`mod_global_callbacks.lua:6-15`). After reload, `start=` is not printed again (`verify_oracles.py:240` requires exactly one `start=`).

Pinned `dt = 0.0166666` s (`Constants.h:32`; both `stdout.log` `[scenario] pinned dt`). 1000 ms = 60 ticks. Host `AI_StuckForTime` at tick 400:

- d3 6499.74 ms / 16.6666 ms = **390.00 ticks** → `StuckTimer` started at tick **10**.
- d0 3749.85 ms / 16.6666 ms = **225.00 ticks** → last write at tick **235**. Host `stdout.log:15` `killall sparing team 0 at tick 226` (NOTEAM dummy is not team 0).

At snapshot tick 60, elapsed from tick 10 is 50 × 16.6666 ms = **833 ms**, below `StuckTimer:SetSimTimeLimitMS(1000)` (`SharedBehaviors.lua:416-417`). Line 547 has not run yet. The compared dump is **only after the heal**. The host-only key is a **post-heal write** (first eligible write ~tick 70). It does not grow as a host-vs-client value pair: the client key is absent, not stale. Shared keys written from `Update` (`TestUpdates`, `TestCheckpointState`, `global_script_spawn`) match on both peers.

Pickup controls (`raw-state-findings.md:21`): exact raw equality (no global-script dummy).

---

## 2. Writers and readers of `AI_StuckForTime`

Grep of `Data\` and `Source\`: Lua hits only in `SharedBehaviors.lua` and Automovers `Controller.lua`. Zero C++ hits (`HumanBehaviors`, `NativeHumanAI`, `AHuman`, `ACrab`, etc.).

### Writers / removers — `Data\Base.rte\AI\SharedBehaviors.lua` function `GoToWpt`

| Site | Action | Trigger |
|---|---|---|
| `:435` | `Owner:RemoveNumberValue("AI_StuckForTime")` | coroutine entry |
| `:523` | `RemoveNumberValue` | `AverageVel` above 2.5 after the stuck limit had been passed |
| `:547` | `Owner:SetNumberValue("AI_StuckForTime", StuckTimer.ElapsedSimTimeMS)` | `StuckTimer:IsPastSimTimeLimit()` (1000 ms, reset only while moving) |
| `:1033` | `RemoveNumberValue` | coroutine exit |

`:547` is inside the `GoToWpt` `while true` loop, after `coroutine.yield` resumes. It is **not** in a shared `Update` hook.

Call chain (per-machine local AI):

1. `MovableMan.cpp:4168-4171` / `:4184-4186` / `:4200-4204`: `ThreadedUpdateAI` then `UpdateAI` only if `isLocalControllerActor(actor)` and `ShouldUpdateAIThisFrame()`.
2. `HumanAI.lua:6-8`: `ThreadedUpdateAI(self)` → `self.AI:Update(self)`.
3. `NativeHumanAI.lua:479-480`: `coroutine.resume(self.GoToBehavior, self, Owner, false)`.
4. `NativeHumanAI.lua:746-747`: `CreateGoToBehavior` → `coroutine.create(SharedBehaviors.GoToWpt)`.
5. This dummy: `AHuman.cpp:112-113` sets `AIMODE_NONE` → `AIMODE_BRAINHUNT`; `NativeHumanAI.lua:221-222` / `:718-725` → `SharedBehaviors.BrainSearch`; `SharedBehaviors.lua:152` / `:222` → `CreateGoToBehavior`. Fixture `mod_global_callbacks.lua:8-9` pins the actor (`PinStrength = 10000`), so velocity stays ~0 and the stuck timer is not reset.

Same writer on crabs: `NativeCrabAI.lua:423-424` (`CreateGoToBehavior` → `SharedBehaviors.GoToWpt`), also under `ThreadedUpdateAI` (`CrabAI.lua`).

`ShouldUpdateAIThisFrame` (`Controller.cpp:313-325`): false when `m_Disabled` or seat mode is not `CIM_AI`. Dead d0 dump has `dis=1`; further writes stop after death (d0 value frozen at 225 ticks).

### Reader — Automovers controller (shared sim `Update`)

`Controller.lua:170` `function Update(self)` (not `UpdateAI`). `:1135-1148`: if `NumberValueExists("AI_StuckForTime")`, reads it, and either zeroes analog move or `RadRotate(RangeRand(-0.15,0.15))`. Grasslands scene files have no Automover objects (grep of `Data\Base.rte\Scenes\*Grassland*`). The reader is stock and live if an Automover is present; this Grasslands pair does not show it executing.

### Who runs this dummy's AI

`MovableMan.cpp:4038-4039`: `isLocalControllerActor` = `!lockstepActive || IsLockstepLocalActor(actor)`.

`NetLockstep.cpp:2294` / `:2306`: `actorTeam < 0` is mapped to team `0`.

`NetActorOwnership.cpp:27-31` (`team-owner`): human peer for that team, else host. Match config (`oracle-review.json` `identical_full_match_config`): host peer 1 team 0, client peer 2 team 1. Team `-1` → team 0 → **host only**.

Client never enters `ThreadedUpdateAI` / `UpdateAI` for UID 1048653. Client never hits `:435` / `:523` / `:547` / `:1033`.

### Shared vs local NumberValues — there is no NumberValue CoW

Grep of `copy-on-write`, `LocalAI`, `AIScope`, `ScopedAI`, `NumberValue` ownership in `MovableObject.*`, `LuaMan.*`, `MovableMan.*`: no NumberValue overlay.

The only local-vs-shared rule at the script hook is **sound**:

`MovableObject.cpp:1137-1141`:

```
// The AI passes are per-machine: their sound calls are decisions this machine makes, deferred
// to the committed tick. The pie menu is local UI and plays on this machine only.
const bool localAI = functionName == "UpdateAI" || functionName == "ThreadedUpdateAI";
const bool presentation = functionName == "WhilePieMenuOpen";
SoundSimulationScope soundScope(..., presentation ? Presentation : (localAI ? LocalSimulation : SharedSimulation));
```

`SetNumberValue` / `GetNumberValue` / `RemoveNumberValue` (`MovableObject.cpp:1465-1490`) read and write the single `m_NumberValueMap`. Lua binds those functions directly (`LuaBindingsEntities.cpp:988-998`). No `SoundExecutionDomain` check. No second map.

Sim hash includes the map: `ContractAudit.h:1203` `Field(...m_NumberValueMap, object.m_NumberValueMap)`.

Rule that actually decides shared vs local: **NumberValues are always shared.** Local AI is scoped only for sound (and aim/flip revert + deferred equip at `MovableMan.cpp:4077-4237`). Aim/flip written by AI are undone; NumberValue writes are not.

---

## 3. Heal path for NumberValues

`NetMatchService.cpp:188-190`: host `SaveCurrentGame` of the live match; **both** peers reload that file.

`ActivityMan.cpp:129` `SaveCurrentGame` → `:211` `Writer::SnapshotScope` → scene objects with full snapshot data.

Capture (no key filter):

- `Scene.cpp:1413-1414`: `SaveSnapshotConfiguration(writer)`.
- `MovableObject.cpp:749-750` / `:705`: `SaveMovableObjectRuntime` cereal-archives the **entire** `m_NumberValueMap`.
- `Scene.cpp:1597-1602`: INI `AddCustomValue` / `NumberValue` for every map entry.
- `ActivityMan.cpp:236-246`: Lua graphs (`SerializeScriptGraphs`) so `GoToWpt` upvalues (`StuckTimer`) ride the save.

Restore:

- `MovableObject.cpp:512-514`: snapshot property stores the runtime blob (validated).
- `MovableObject.cpp:454-457` `AdoptPersistedUniqueID`: `LoadMovableObjectRuntime` (`:731`) replaces `m_NumberValueMap` with the archived map.
- Same map on host and client after reload. No branch that drops host-only or `AI_*` keys. No local-AI CoW copy that could be present on the host snapshot and absent on the client load.

Host console after save: `Game saved to "p5resync_*"` then activity reset and scene reload (`trace.json.console.txt:14-18`). Dummy UID 1048653 is kept. Shared `Update` NumberValues match after continuation, so restore of `m_NumberValueMap` is not generally broken.

---

## 4. Candidate mechanisms (no fix)

### M1 — Host-only local AI writes the shared map after the tick-60 snapshot (leading)

Lines that must run: `MovableMan.cpp:4168-4171` (host, `isLocalControllerActor`) → `HumanAI.lua:6-8` → `NativeHumanAI.lua:479-480` → `SharedBehaviors.lua:547`. Snapshot at tick 60 via `ActivityMan.cpp:211` + `MovableObject.cpp:705`. Client AI for this UID does not run (`NetLockstep.cpp:2294` + `NetActorOwnership.cpp:27-31`).

Observation that **confirms**: d3 host value 6499.74 ms = 390 × 16.6666 ms from tick 10 to 400; at tick 60 elapsed is 833 ms < 1000 ms, so `:547` has not run at capture; client dump lacks the key; shared `Update` keys match; pickup jobs (no this dummy) are equal.

### M2 — Snapshot/restore drops `AI_StuckForTime` on the client only

Lines: `LoadMovableObjectRuntime` `:731` would have to skip one key, or INI vs cereal would have to disagree per peer.

Observation that **rules it out**: `TestUpdates`, `TestCheckpointState`, and `global_script_spawn` on the same object are byte-identical. There is no `AI_` filter in `SaveMovableObjectRuntime` / `Scene.cpp:1597-1602`. A key-specific client-only drop is not in the code that ran.

### M3 — Client `GoToWpt` `:435` clears a restored key

Lines: `SharedBehaviors.lua:435` inside `ThreadedUpdateAI`.

Observation that **rules it out**: client is not the lockstep owner of team `-1`. `ThreadedUpdateAI` is not called. If both peers ran `GoToWpt` after restore, both would either clear and rewrite or keep matching values; the dump is presence-only on the host.

---

## Detecting control

Existing red jobs: matrix `heal_global_d0` and `heal_global_d3`, check `post_heal_raw_dump_identical` (`verify_oracles.py:246`). They stay the production detectors.

Existing equal control: `heal_pickup_d0` / `heal_pickup_d3` (no `mod_global_callbacks.lua` dummy).

A control that is red now and green after, without a mask:

- Same two-peer heal, `CC_SIM_DUMP=61:61` and `400:400` (or one dump immediately after `resync: match relaunched`).
- Assert UID 1048653 `GetNumberValueMap()` is **equal** on host and client at the first post-heal dump **and** at tick 400.
- Today: tick 61 both lack `AI_StuckForTime` (M1); tick 400 host-only. After a fix that stops local AI from mutating the shared map (or makes that write identical on every peer), both dumps match.
- Optional native lockstep selftest: NOTEAM pinned `AHuman`, host-only `ThreadedUpdateAI` that `SetNumberValue`, save/load checkpoint, N ticks; assert maps equal after load and after N ticks. `ActivityMan.cpp:552` / `:571` already assert a NumberValue after save/load, but that value is written from shared `Update`, not from `ThreadedUpdateAI`.

No mask, no exclusion, no serialization of local AI. The hashed/snapshotted NumberValue map must not diverge because only one peer ran `GoToWpt`.
