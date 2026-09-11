# W17: local-AI copy-on-write + audibility observation transport

Read-only. Source `D:\Projects\p4b-interp-validation\Source` at `9751a90e292eb300013c1d6052f3df7bbe228869`. No builds, no launches. Writes only under this folder.

Commands:

```
git -C D:\Projects\p4b-interp-validation rev-parse HEAD
git -C D:\Projects\p4b-interp-validation log -1 --format="%H %s" 9751a90e29
git -C D:\Projects\p4b-interp-validation log -1 --format="%H %s%n%b" 0821d77d3
git -C D:\Projects\p4b-interp-validation log -1 --format="%H %s%n%b" 90498a1b2
```

Outputs: this file. HEAD printed `9751a90e292eb300013c1d6052f3df7bbe228869 Compare controlled actors across every activity in snapshot diffs`. Quotes below are from that tree.

W9 (`w9-ai-stuckfortime\REPORT.md` §§2–3): `SetNumberValue` / `GetNumberValue` hit the one shared `m_NumberValueMap` (`MovableObject.cpp:1465-1466`); local AI is scoped only for sound. This report documents that sound template. No value-map fix is designed here.

`0821d77d32` subject: `Commit each peer's actual audibility with its delayed frame`. Body:

> The settled policy is that a shared query takes the controlling player's reading, the host supplies activity and unowned readings, and local presentation keeps its own value.

`90498a1b21` subject: `Keep local sound scopes out of the shared key sequence`.

RESUME.md §C (`# §C. THE RULES`, line 1728) restates Controller-sync and on-wire rules; it does not reprint the GetAudibleVolume sentence. That sentence lives in the `0821d77d3` commit body and in the AGENTS.md binding operating rules. The implementation is `SoundContainer.cpp:357-363` + `AudioMan.cpp:1919-2004`.

---

## 1. Scope machinery

### Domain enum and thread-local stack

`Source\System\SoundSimulation.h:6-44`:

```
enum class SoundExecutionDomain : uint8_t { Presentation, SharedSimulation, LocalSimulation };
...
class SoundSimulationScope {
	SoundSimulationScope(uint64_t objectUID, uint64_t phase, SoundExecutionDomain domain = SoundExecutionDomain::SharedSimulation, uint64_t occurrence = 0);
	~SoundSimulationScope();
	static SoundExecutionDomain Domain();
	static bool IsSimulation() { return Domain() != SoundExecutionDomain::Presentation; }
	static SoundExecutionKey CurrentKey();
	static SoundExecutionKey NextQueryKey();
	static SoundExecutionKey NextPlayKey();
```

`SoundExecutionKey` (`SoundSimulation.h:11-18`) is `{domain, objectUID, tick, phase, occurrence, ordinal}`.

Enter: constructor pushes `s_Current`. Exit: destructor restores `m_Previous`.

`AudioMan.cpp:37`: `thread_local SoundSimulationScope* SoundSimulationScope::s_Current = nullptr;`

`AudioMan.cpp:45-64` (constructor + dtor):

```
SoundSimulationScope::SoundSimulationScope(...) : m_Previous(s_Current) {
	// A nested native callback inside local AI remains local unless it explicitly
	// enters presentation. Its shared cohort must not inherit local AI decisions.
	if (m_Previous && m_Previous->m_Key.domain == SoundExecutionDomain::LocalSimulation && domain == SoundExecutionDomain::SharedSimulation) domain = SoundExecutionDomain::LocalSimulation;
	// Only shared children number their parent's shared sequence: a per-machine AI scope must not shift the keys every peer derives.
	if (m_Previous && !occurrence && domain == SoundExecutionDomain::SharedSimulation && m_Previous->m_Key.domain == domain) occurrence = Mix(m_Previous->m_Seed ^ ++m_Previous->m_ChildOrdinal);
	else if (m_Previous && !occurrence && domain == SoundExecutionDomain::LocalSimulation) occurrence = Mix(m_Previous->m_Seed ^ ~(++m_Previous->m_LocalChildOrdinal));
	m_Key = {domain, objectUID, static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()), phase, occurrence, 0};
	...
	s_Current = this;
}
SoundSimulationScope::~SoundSimulationScope() { s_Current = m_Previous; }
```

`AudioMan.cpp:65`: `Domain()` returns `s_Current ? s_Current->m_Key.domain : Presentation`. Natives query this static. There is no Lua binding of the scope class; Lua-called C++ (`Play`, `SetVolume`, `GetAudibleVolume`, `SoundSet::*`) calls `Domain()` / `Deferring()`.

The local-child sequence is the `90498a1b2` fix. Selftest `local_scopes_keep_shared_keys` (`AudioMan.cpp:2214-2231`).

### How a script hook becomes LocalSimulation

`MovableObject.cpp:1137-1141`:

```
// The AI passes are per-machine: their sound calls are decisions this machine makes, deferred
// to the committed tick. The pie menu is local UI and plays on this machine only.
const bool localAI = functionName == "UpdateAI" || functionName == "ThreadedUpdateAI";
const bool presentation = functionName == "WhilePieMenuOpen";
SoundSimulationScope soundScope(m_UniqueID, Hash(functionName), presentation ? SoundExecutionDomain::Presentation : (localAI ? SoundExecutionDomain::LocalSimulation : SoundExecutionDomain::SharedSimulation));
```

Any other script function (`Update`, collision, …) is `SharedSimulation`.

### AI pass stages and who enters them

`MovableMan.cpp:4038-4039`: `isLocalControllerActor` = `!lockstepActive || IsLockstepLocalActor(actor)`.

`MovableMan.cpp:270-271` forwards to `ScenarioRunner::IsLockstepLocalActor(uid, team, !IsPlayerControlled())`.

`ScenarioRunner.cpp:619-632`: replay owns nobody; no coordinator → everyone local; else `s_LockstepControlOverrides` else `coordinator->IsLocalActor`.

`Controller.cpp:313-325` `ShouldUpdateAIThisFrame`: false if `m_Disabled` or seat is not `CIM_AI`; else throttle by contiguous actor ID vs `GetAIUpdateInterval()`.

AI pass (`MovableMan.cpp:4069-4217`), in order:

1. Local actors: `Controller::Update` (`4071-4075`).
2. Script init on every actor (`4158-4163`).
3. `g_AudioMan.SettleSharedSoundWrites()` (`4164-4166`) — shared-hook alias writes land before any AI hook.
4. Master-state `ThreadedUpdateAI` with `g_CurrentAIActor = actor` (`4167-4173`).
5. Thread-pool `ThreadedUpdateAI` per Lua state, same `g_CurrentAIActor` (`4177-4193`).
6. `drainDeferredEquips` + `drainDeferredSoundOps` (`4195-4196`).
7. Serial `UpdateAI` (`4198-4207`); under lockstep `g_CurrentAIActor` is set so those writes defer too.
8. Drain again (`4208-4211`).
9. Optional `AIWriteScript::RunTick` + drain (`4213-4217`).
10. Aim/flip revert (`4219-4238`).

`g_CurrentAIActor`: `Controller.h:84` `extern thread_local Actor* g_CurrentAIActor;` / `Controller.cpp:16`. Sound ops attribute themselves from it (`SoundContainer.cpp:610-617`, `:624-633`). Equip mutators also key off it (`AHuman.cpp` sites).

### Shared stages also open scopes

Default domain is `SharedSimulation`:

| Site | Phase |
|---|---|
| `MovableMan.cpp:3951` TravelStage | `Hash("Travel")` |
| `:3964` PreControllerStage | `Hash("PreController")` |
| `:3972` UpdateStage | `Hash("Update")` |
| `:3984` PostUpdateStage | `Hash("PostUpdate")` |
| `:384` / `:4133` deferred sound apply | `Hash("DeferredSoundOp")` + explicit `SharedSimulation` + `NextDeferredSoundOpOrdinal()` |
| `ActivityMan.cpp:1033`, `:1042` | activity sound phase |
| `Main.cpp:2468` | tick-level `simulationSounds(0, soundPhase)` |

### How natives know they are in local AI

`SoundContainer.h:505`: `static bool Deferring() { return SoundSimulationScope::Domain() == SoundExecutionDomain::LocalSimulation; }`

Writers (`SetVolume`, `Play`, `Stop`, `SoundSet::AddSound`, …) call `Deferring()`. Readers of controls use `Control()` (`SoundContainer.h:519-535`), which also checks `Deferring()`. `GetAudibleVolume` (`SoundContainer.cpp:357-363`) branches on `Domain() == SharedSimulation` vs local/presentation.

---

## 2. Copy-on-write for sounds

Not a cloned `SoundContainer`. A per-pass overlay (`PendingControls`) plus a queued `PendingOp` list. Comment at `SoundContainer.h:51-53`:

```
/// One sound call an AI hook made. The decision is the owning machine's, so the call reads back
/// inside that pass and lands on shared state at the committed tick, exactly like the equip
/// calls the AI queues.
```

`SoundContainer.h:460-462`:

```
/// The AI pass's private view of the controls it has written this pass. It exists only between
/// the write and the drain -- long enough for the hook to read back what it just set -- and is
/// never archived: the values reach shared state through the deferred operations instead.
```

### Write path

`DeferProperty` (`SoundContainer.cpp:587-607`): if `!Deferring()` return false (caller mutates shared). Else lock, `NoteAIActor`, overlay via `ApplyPendingControl`, queue `SetProperty`.

`Control()` (`SoundContainer.h:519-535`):

- not deferring → return shared member
- deferring read → `NoteAIActor`; return overlay if that field’s bit is set, else shared
- deferring write-ref → copy shared into overlay on first touch, set the bit, return overlay

So `sound.Pos.X = value` / `local p = sound.Pos; p.X = value` stay live aliases (`GetScriptPosition` `SoundContainer.cpp:488-510`): AI holds `m_Pending.m_Pos`; shared holds `m_Pos` and `m_SharedAliasHeld`. An AI write through a shared-handed alias is pulled back by `AdoptSharedAliasWrite` (`:513-521`).

Play/Stop/Restart/FadeOut/SelectSounds/structure (`PendingOp::Op` `SoundContainer.h:54-68`) queue instead of mutating. `IsBeingPlayed` (`SoundContainer.cpp:442-448`) answers from `m_PendingPlays` / `m_PendingStopped` while deferring.

`SoundSet.cpp:198-200` `DeferringOwner()`: LocalSimulation → `m_OwnerContainer`. Structural calls (`AddSound` `:202-217`, `SetSoundSelectionCycleMode` `:230-237`, `SelectNextSounds` `:411-420`) queue on the owner. Cycle-mode read-back: `SoundSet.h:149` returns `m_PendingCycleMode` only in LocalSimulation.

### Drain / discard / promote

`NotePending` → `AudioMan::NotePendingSoundOps` (`AudioMan.cpp:1151-1155`) records the container by checkpoint identity.

`TakePendingSoundOps` (`SoundContainer.cpp:720-751`): adopt alias writes, sort by `(actorUID, sequence)`, swap out `m_PendingOps`, clear overlay except a held Pos alias, `ClearPendingCycleMode`. Overlay discarded here; ops are the promotion record.

`MovableMan.cpp:4127-4156` `drainDeferredSoundOps`:

- no lockstep: apply now in a SharedSimulation `DeferredSoundOp` scope
- lockstep: pack `NetGameSoundOp` (`actorUID`, `team`, `soundIdentity`, op/property/player/value/x/y/path/payload) and `EnqueueLocalGameCommand`

Apply on every peer: `ApplyDeferredSoundOp` (`MovableMan.cpp:366-386`) + `ApplyPendingSoundOp` (`SoundContainer.cpp:754-784`). Authority gate at `:689-691`: actor team must match and `IsLockstepActorOwner(..., command.senderPeerId)`.

Promotion is the committed-tick command, not an in-place overlay merge.

### Snapshot and hash of copies

`SoundContainer::SaveCheckpoint` (`SoundContainer.cpp:907-912`) archives shared controls + `m_LogicalPlayback` only. No `m_Pending`, no `m_PendingOps`.

`LoadCheckpoint` SoundContainer2 (`:934-935`): old second cohort + `LocalSoundControls` are read into `discardedCohort` / `discardedControls` and dropped. Comment `:934-935`: “only the shared cohort was ever simulation state.”

`AudioMan::SaveCheckpoint` (`:1433-1478`) does not archive pending-op containers. It does archive `m_CommittedAudibility` and the deferred-op ordinal (`:1438`, `:1443-1445`, `AudioRuntime::Save` `:1398`).

Contract-audit visitor `ContractAudit.h:1434-1456` `Visit(SoundContainer)` lists shared control fields only. No `m_Pending`, no `m_LogicalPlayback`, no `m_PendingOps`.

---

## 3. The observation transport

Deferred `NetGameSoundOp` is the write. `NetSoundObservation` is a per-peer **mixer reading** of an already-shared playback, committed with the delayed lockstep **frame** (same packet as controller frames + game commands; not a separate stream).

### Record

`NetLockstep.h:90-99`:

```
/// One peer's actual audibility of a shared simulation sound, sampled at its input boundary and
/// committed with the sender's delayed frame so every peer reads the identical value.
struct NetSoundObservation {
	uint8_t senderPeerId;
	uint64_t objectUID, tick, phase, occurrence, ordinal;
	float value;
};
```

Key without value: `NetSoundObservationKey` (`:106-111`) = those five uint64s. Playback identity is `LogicalSoundPlayback.identity` / `GetSharedPlaybackIdentity()` (`SoundContainer.h:28`). `lastObjectUID` (`LogicalSound.h:113`) is “the object whose phase last played this container; its controller answers for the sound.” Set at play (`AudioMan.cpp:562-563`): `playback.lastObjectUID = playKey.objectUID`.

### Who produces it

`AudioMan::SampleSoundObservations` (`:1928-1954`):

- no-op unless lockstep active
- `VisitSharedSimulationSounds` — live **shared-domain** logical voices only (`:1214-1223`)
- sort by checkpoint identity
- skip if `!identity.ordinal`
- comment `:1938`: “Every peer reports every live shared sound, so the reading of whoever controls it next is already committed when control moves.”
- value = `GetLocalSoundAudibility` (`:1226-1236`): first FMOD `getAudibility` of a voice whose stored domain is `SharedSimulation` (AI-only voices excluded)
- skip if `m_LastSentAudibility[key] == value` (delta)
- `senderPeerId = localPeer`

`GetLocalSoundAudibility` is this machine’s mixer, not the committed table.

`ScenarioRunner.cpp:867-870`:

```
const bool queued = s_LockstepCoordinator->QueueLocalInput(tick, frames, DrainLocalGameCommands(), error, g_AudioMan.SampleSoundObservations());
g_AudioMan.ForgetSentAudibility(s_LockstepCoordinator->TakeDroppedObservations());
```

Call site: after the AI pass, `MovableMan.cpp:4255` (and paused tick `:726`).

### Queue and wire

`QueueLocalInput` (`NetLockstep.cpp:1971-2068`):

- `targetFrame = producedFrame + m_Config.inputDelayFrames` (`:1978`)
- prepend `m_PendingObservations` that this sample did not refresh (`:2008-2021`)
- stamp every observation `senderPeerId = localPeerId`
- cap `c_MaxObservationsPerPacket` (4096); leftover → `m_PendingObservations`
- encode; if the byte budget (`c_MaxObservationBytesPerPacket` = 24 KiB, `NetLockstep.h:356`) encodes fewer, remainder prepended to pending (`:2037-2042`)
- pending above `c_MaxCarriedObservations` → `m_DroppedObservations` (handed back so they are sampled again) (`:2045-2054`, `AudioMan.cpp:1957-1958`)
- store `m_LocalObservations[targetFrame]`

Packet type: `NetLockstepFrame` (`NetLockstep.h:171+`) — `senderPeerId`, `targetFrame`, `frames` (controllers), `commands` (`NetGameCommand`, including `NetGameSoundOp`), `observations`, `roundId`. Lane: `m_Config.frameLane` (default `ControlReliable`). Codec version 16; observations since v11; slots v14; binding-sequence v15 (`NetLockstep.h:340-352`).

Encode `AppendObservations` (`NetLockstep.cpp:368-437`): varint `BindingCount`, u16 count, then per observation: slot varint (low bit = full key), optional mask + changed key fields, u32 LE float bits. Dictionary only advanced for observations that fit.

Decode `ReadObservations` (`:679-801`): binding-count gap → `ObservationBindingGap`; unknown slot → `UnboundObservationSlot`; `senderPeerId` taken from the authenticated frame sender (`:789-790`). Other-round blocks `discard` without touching the live table (`:675-678`).

Relay: `NetLockstep.cpp:2835-2860` — host picks the sender table from the **transport** peer. Unbound slot / binding gap on a real peer: drop the frame, `++m_Stats.unresolvedObservationPackets`, first time print `[lockstep] dropped a frame over its sound observations:`.

### Settle frame

Ready-frame assembly (`NetLockstep.cpp:3326-3335`) copies `m_LocalObservations` / `m_RemoteObservations` for `nextFrame` into `NetLockstepReadyFrame` (`NetLockstep.h:250-258`).

`MovableMan.cpp:4261-4291` (same tick’s committed frame, which was produced `inputDelayFrames` ago; D=0 → this tick’s sample):

1. apply local then remote controller frames
2. gone-owner disable
3. **`g_AudioMan.CommitSoundObservations(readyFrame.frame, readyFrame.localObservations, readyFrame.remoteObservations)`** (`:4290`)
4. `ApplyLockstepGameCommands` (sound ops apply after observations settle)

Paused tick: commit then commands only (`:735-737`).

`CommitSoundObservations` (`AudioMan.cpp:1961-1980`): concatenate local+remote, **stable-sort by `senderPeerId`**, write `m_CommittedAudibility[key][sender] = {frame, value}`. Every 600 frames prune readings whose `frame + 36000 <= now` (`:1910-1911`, `:1975-1980`) — “ten minutes”, same frame on every peer.

### Owner local read vs shared settled value

`SoundContainer::GetAudibleVolume` (`:357-363`):

```
// A shared query reads the committed observation set so every peer sees one value; local AI and presentation keep this machine's.
if (SoundSimulationScope::Domain() == SharedSimulation && lockstep && !FaultInjected("local_audibility"))
	return g_AudioMan.GetCommittedAudibility(*this);
return g_AudioMan.GetSoundContainerAudibleVolume(this);
```

`GetSoundContainerAudibleVolume` (`AudioMan.cpp:667-689`): first FMOD `getAudibility` whose `VoiceMatchesContext` passes. Shared simulation sees only shared-domain voices (`:1146-1148`); local AI / presentation see every voice on the container.

`GetCommittedAudibility` (`:1983-2004`): lookup by shared playback identity; no row → 0; else **authority peer’s** reading; if that peer has not sent yet, latest `frame` among committed peers (`:1998-2004`).

So the controlling peer’s **live** FMOD can differ from the **settled** table until its delayed frame commits. Shared scripts on that same peer then read the table, not live FMOD.

### Checkpoint / restore / `unresolved_observation_packets`

Committed table: `CommittedAudibilityRecord` / `AudioRuntime3` (`AudioMan.cpp:1356-1398`, save `:1443-1445`, load `:1620-1622`). `m_LastSentAudibility.clear()` on load (`:1622`) so the next sample is not suppressed as a duplicate.

Coordinator queues (`m_LocalObservations`, `m_RemoteObservations`, `m_PendingObservations`, slot tables) are **not** in `AudioMan::SaveCheckpoint`. They live for the current lockstep round (`Reset` on round start, `NetLockstep.cpp:1766-1772`). Overflow pending is “checkpointed” only as carry onto the next **frame**, not into the game snapshot.

`unresolvedObservationPackets` (`NetLockstep.h:324`): “Frames dropped because an observation named a slot this peer never got.” Increment `:2860`. Stats JSON `:2468` `"unresolved_observation_packets"`.

Replay: `NetMatchReplay.cpp:69-115` encodes observations on the same record; sender ids as a trailer. Playback: `QueueReplayFrame` (`NetLockstep.cpp:1921-1944`) fills `m_RemoteObservations`. Recorder: `ScenarioRunner.cpp:1176-1179`.

### Selftests

**AudioMan `RunLogicalPlaybackSelfTest`** (`AudioMan.cpp:2014-2457`):

| Name | Lines | Pins |
|---|---|---|
| `ai_reads_the_simulation_cohort` | 2119-2125 | AI sees shared liveness; overlay read-back; Stop defers |
| `ai_writes_wait_for_the_drain` | 2132-2134 | shared members unchanged until drain |
| `ai_writes_land_at_the_drain` | 2137-2141 | apply pending → shared volume/pos |
| `deferred_call_survives_the_wire` | 2146-2171 | `PendingOp` → `NetGameSoundOp` encode/decode |
| `shared_inside_local_stays_local` | 2224-2226 | nested Shared requested stays Local |
| `local_scopes_keep_shared_keys` | 2214-2231 | local child does not bump shared occurrence (`90498a1b2`) |
| `committed_latest_reading` | 2243-2254 | later sender/frame wins when authority row missing |
| `committed_table_checkpoint` | 2255-2258 | Save/Load `m_CommittedAudibility` |
| `committed_table_prunes_stale` | 2259-2260 | prune at frame 36600 |
| `deferred_ordinal_survives_a_restore` | 2264-2297 | `NextDeferredSoundOpOrdinal` in AudioRuntime3 |
| `drained_order_is_deterministic` | 2302-2335 | two actors, either thread order → same drain |
| `legacy_sound_container_record_loads` | 2341-2385 | SoundContainer2 drops `LocalSoundControls1` |
| `ai_alias_only_write_defers` | 2388-2423 | retained Pos alias writes only at drain |
| `shared_alias_only_write_settles` | 2424-2435 | shared alias lands immediately |
| `dropped_reading_is_sampled_again` | 2438-2452 | `ForgetSentAudibility` |

**NetLockstepSelfTest.cpp** (observation codec / relay / holes):

| Print / assert | Approx. lines |
|---|---|
| `observation_slot_codec` | 154-535 |
| `four_peer_observation_relay` | 3165-3317 |
| `observation_overflow_carry` | 3348-3452 |
| `unresolvedObservationPackets != 0` on a hole; `== 0` on admission / stale-round | 3511, 3594-3614, 3675-3738 |
| `a resync's in-flight frames were counted as holes` | 3807-3808 |
| `observation_faults_told_apart` | 3613-3614 |

---

## 4. Authority

### Observation read authority (audibility)

`AudioMan::AudibilityAuthority` (`:1919-1926`):

```
uint8_t AudioMan::AudibilityAuthority(uint64_t objectUID) {
	const uint8_t host = ScenarioRunner::GetLockstepHostPeerId();
	if (objectUID == 0) return host;
	const MovableObject* object = g_MovableMan.FindObjectByUniqueID(static_cast<long>(objectUID));
	const Actor* actor = object ? dynamic_cast<const Actor*>(object->GetRootParent()) : nullptr;
	if (!actor) return host;
	return ScenarioRunner::GetLockstepActorOwner(actor UID, team, !IsPlayerControlled());
}
```

`GetCommittedAudibility` (`:1997`) calls this with `lastObjectUID`, not the observation’s `objectUID` field (those two are the play-key object and are usually the same).

Matches `0821d77d3`: shared query = controlling player’s reading; activity-wide (`objectUID == 0`) and unowned / no actor root = host. Local AI / presentation skip this function.

### Who is the controlling peer

`GetLockstepActorOwner` (`ScenarioRunner.cpp:646-654`): override map else `ResolveActorOwner`.

`NetActorOwnership.cpp:20-43` `ResolveOwnerPeer`:

- `UniqueIdModPeerCount`: `1 + (absId % peerCount)`
- `TeamOwner`: first human slot on that team, else host (`:27-31`)
- `HostCpuRemoteHuman`: human on team if not CPU-controlled, else host

`IsLockstepTeamCommandSender` (`ScenarioRunner.cpp:697-701`): `team < 0` → true; else `NetActorOwnership::IsTeamCommandAuthority` (`NetActorOwnership.cpp:59-72`): any human on that team, or host if the team has no human.

Team `< 0` NOTEAM: W9 mapped lockstep local-actor via `NetLockstep.cpp:2294` (team `< 0` → team 0). Audibility authority uses the **actor’s real team** in `GetLockstepActorOwner` / `FindHumanPeerForTeam`. Unowned / no-root still host via `AudibilityAuthority`.

`IsLockstepLocalActor` (who **runs** AI / who **issues** sound commands) uses the same owner resolution. Sound-command apply (`MovableMan.cpp:689`) requires `IsLockstepActorOwner(..., senderPeerId)`.

### Reseat / substitution

`NetGameSwitchControl` (`MovableMan.cpp:593-607`): sender may only claim itself; live actor must be on claimed team; `SetLockstepControlOverride(actorUID, newOwnerPeerId)`.

`NetGameReseat` (`:408-414`, `:608-620`): host-only (team gate skipped); override per listed UID if missing or still on `reseat->team`.

`GetLockstepActorOwner` then returns the override (`ScenarioRunner.cpp:650-652`). `PurgeLockstepControlOverridesForGonePeers` (`:684-694`) drops overrides whose peer is gone at the applied frame; policy owner resumes.

On reseat, `AudibilityAuthority` flips to the new owner. Sample comment (`AudioMan.cpp:1938`) + fallback (`:1998-2004`): every peer already sent readings, so the new controller’s row is usually present; otherwise the latest committed reading stands until that peer’s next sample.

`lastObjectUID` does not change on reseat; only owner lookup does.

---

## 5. The value maps today

Three maps on `MovableObject` (`MovableObject.h:1367-1369`):

- `m_StringValueMap` — `unordered_map<string,string>`
- `m_NumberValueMap` — `unordered_map<string,double>`
- `m_ObjectValueMap` — `unordered_map<string,Entity*>`

No `SoundExecutionDomain` check. No overlay. No observation.

### Native API (`MovableObject.cpp:1421-1494`, decls `MovableObject.h:952-1023`)

| API | Missing key / remove |
|---|---|
| `GetStringValue` / `GetEncodedStringValue` | `""` / decode of missing → `""` |
| `GetNumberValue` | `0.0` |
| `GetObjectValue` | `nullptr` |
| `SetStringValue` | assign |
| `SetEncodedStringValue` | store `base64_encode(value, true)` in the **string** map |
| `SetNumberValue` | assign (`:1465-1466`) |
| `SetObjectValue` | assign pointer |
| `RemoveStringValue` / `RemoveNumberValue` / `RemoveObjectValue` | `erase` |
| `StringValueExists` / `NumberValueExists` / `ObjectValueExists` | `find != end` |
| `GetStringValueMap` / `GetNumberValueMap` | const ref; **no** `GetObjectValueMap` |

INI reader `ReadCustomValueProperty` (`:676-694`): `NumberValue` / `StringValue` only. `ObjectValue` is an error.

`Create(ref)` copies all three (`:397-399`). `Clear` clears all three (`:194-196`).

### Lua (`LuaBindingsEntities.cpp:986-999`)

Bound 1:1: Get/Set/Remove/Exists for String, EncodedString, Number, Object. `GetNumberValueMap` / `GetStringValueMap` are **not** bound.

### Checkpoint

Cereal runtime `SaveMovableObjectRuntime` (`:705`) / load (`:731`): `m_StringValueMap`, `m_NumberValueMap`. **`m_ObjectValueMap` is not archived.**

INI snapshot `Scene.cpp:1590-1602`: every string and number as `AddCustomValue` / `StringValue` or `NumberValue`. No object entries.

`MovableObject::Save` (`:864-872`): same Number/String `AddCustomValue` objects. No ObjectValue.

### Tick hash vs dump visitor

`g_SimChecksum` subsystems (`SimChecksum.h:22-23`): `tick`, `terrain`, `carve_math`, `actors`, `items`, `particles`, `rot_angle`, `rot_angvel`, `scene`, `funds`, `sim_rng`, `lua_state`, `controller`. Actor feed (`MovableMan.cpp:3783-3823`) is uid/pos/vel/health/controller bits — **not the maps**. `lua_state` (`LuaMan.cpp:6487-6494`) is master Lua RNG only. `SimGatedHash` (`SimChecksum.cpp:132-147`) drops only `controller`. **Value maps are not in SimGatedHash.**

Contract-audit / raw-state visitor **does** include them (`ContractAudit.h:1202-1204`):

```
Field(path + ".MovableObject.m_StringValueMap", object.m_StringValueMap);
Field(path + ".MovableObject.m_NumberValueMap", object.m_NumberValueMap);
Field(path + ".MovableObject.m_ObjectValueMap", object.m_ObjectValueMap);
```

`Value` for maps (`:118-124`) sorts keys and emits `[key:value;...]`. Object pointers become `uid:` / `ref:` (`:104-109`).

Simdump `nv=` (`MovableMan.cpp:899-903`) prints sorted `GetNumberValueMap()` under test scripts. That is how W9 saw host-only `AI_StuckForTime`.

---

## 6. Mod-compat surface (value maps as they behave today)

Observable today, from the APIs above. No observation/CoW, so MP timing of a **local-AI write** is “this peer only, immediately.”

1. **Immediate read-after-write in the same script.** `Set*` then `Get*` / `*Exists` in the same native call stack sees the new map entry. Same as overlay read-back for sounds, except the write is already on the shared object.
2. **Cross-script visibility in the same tick.** Maps are members of the MO. Any later hook on that MO (`Update`, another script, C++, Automover `Controller.lua`) sees the write. W9: `SharedBehaviors.lua:547` in `ThreadedUpdateAI` is visible to `Controller.lua:1135-1148` in `Update` if that reader runs after on a peer that did the write.
3. **Cross-object visibility.** Any Lua/C++ holding the MO pointer sees the same maps. No copy, no scope filter.
4. **Missing-key defaults.** Number `0`, string `""`, object `nullptr`; `*Exists` is the presence test (0 is a stored value).
5. **Remove.** `erase`. After remove: default + Exists false. Re-Set recreates. W9: `RemoveNumberValue("AI_StuckForTime")` at GoToWpt entry/exit (`SharedBehaviors.lua:435`, `:1033`).
6. **Overwrite.** Last `Set*` for a key wins. No version / no per-peer layer.
7. **INI / save persistence (string + number).** `AddCustomValue` round-trips; cereal runtime blob includes both maps; Scene snapshot writes both. Survive `SaveCurrentGame` / ordinary load (W9 §3).
8. **Object values do not persist.** Pointers only; omitted from cereal and INI. After restore the map is empty unless something Set them again. `Create(ref)` copies the pointer.
9. **Encoded strings** share `m_StringValueMap`; `GetStringValue` returns the stored (possibly base64) bytes; `GetEncodedStringValue` decodes.
10. **Clone.** `Create(const MovableObject&)` copies all three maps onto the new object.
11. **Dump / audit.** Number map appears in `nv=` and ContractAudit; string and object maps in ContractAudit only.
12. **No MP delay on the write itself.** Unlike sound CoW, a local-AI `SetNumberValue` is not deferred to `simTick+D` and is not wrapped as a command. The only MP timing today is “which peer ran the AI.”
13. **Hash gate does not see the maps.** Two peers can diverge in `m_NumberValueMap` with matching `SimGatedHash` (W9 heal jobs). ContractAudit / `CC_SIM_DUMP` sees it.

---

## Template summary

Pieces the sound solution has, in order, with file:line. Instantiate the same list for value maps; this report does not choose that design.

1. **Scope detection** — `SoundExecutionDomain` + thread-local `SoundSimulationScope` (`SoundSimulation.h:9-44`; ctor `AudioMan.cpp:45-64`; `Domain()` `:65`). Entered for `UpdateAI` / `ThreadedUpdateAI` (`MovableObject.cpp:1139-1141`). Actor attribution `g_CurrentAIActor` (`MovableMan.cpp:4170-4205`; `SoundContainer.cpp:610-617`). Natives: `Deferring()` (`SoundContainer.h:505`). Nested shared-inside-local stays local (`AudioMan.cpp:48`). Local children use a separate occurrence sequence (`:50-51`, commit `90498a1b2`).
2. **Copy-on-write store** — `PendingControls` + dirty bits + `Control()` (`SoundContainer.h:460-535`). Writes queue `PendingOp` (`h:54-101`; `cpp:587-607`, `:624-635`). Reads in LocalSimulation see the overlay. Drain sorts by `(actorUID, sequence)` (`cpp:720-751`). Not archived (`h:460-462`; `SaveCheckpoint` `cpp:907-912` omits it; SoundContainer2 discards `LocalSoundControls` `cpp:934-939`).
3. **Observation record** — `NetSoundObservation` `{senderPeerId, objectUID, tick, phase, occurrence, ordinal, value}` (`NetLockstep.h:92-99`). Key = playback identity (`LogicalSound.h:102-108`; `SoundContainer.h:28`). `lastObjectUID` for authority (`LogicalSound.h:113`; set `AudioMan.cpp:563`).
4. **Transport** — sampled at the input boundary (`AudioMan.cpp:1928-1954`; `ScenarioRunner.cpp:867-868`). Rides the **lockstep frame** with controllers and `NetGameCommand`s (`NetLockstep.cpp:1999-2062`; encode `:372-437`; decode `:679-801`). Not a separate stream. Slot dictionary per sender per round (`NetLockstep.h:123-168`). Overflow pending on the next frame (`:2008-2042`). `NetGameSoundOp` is the **write** path on the same frame (`MovableMan.cpp:4141-4153`, `:366-386`), distinct from the observation.
5. **Settle frame** — `targetFrame = producedFrame + inputDelayFrames` (`NetLockstep.cpp:1978`). Commit when that frame is ready, after controller apply, before game-command apply (`MovableMan.cpp:4290-4291`). `CommitSoundObservations` stable-sorts by sender (`AudioMan.cpp:1961-1973`). Shared `GetAudibleVolume` reads the table (`SoundContainer.cpp:357-361`; `GetCommittedAudibility` `AudioMan.cpp:1983-2004`). Local AI / presentation keep `GetSoundContainerAudibleVolume` (`:667-689`).
6. **Checkpoint of pending / committed observations** — committed table + deferred-op ordinal in `AudioRuntime3` (`AudioMan.cpp:1356-1398`, `:1443-1445`, `:1620-1622`). `m_LastSentAudibility` cleared on load. Coordinator `m_PendingObservations` / in-flight maps / slot tables are round-memory only (`NetLockstep.cpp:1766-1772`, `:621` in the header). Replay records observations (`NetMatchReplay.cpp:69-115`; `ScenarioRunner.cpp:1176-1179`). `unresolved_observation_packets` counts frames dropped for slot/binding holes (`NetLockstep.h:324`; `NetLockstep.cpp:2847-2860`, stats `:2468`).
7. **Authority rule** — `AudibilityAuthority` (`AudioMan.cpp:1919-1926`): UID 0 or no actor root → host; else `GetLockstepActorOwner` (`ScenarioRunner.cpp:646-654`) = override or `NetActorOwnership::ResolveOwnerPeer` (`NetActorOwnership.cpp:20-43`, TeamOwner `:27-31`). Between reseat and the new controller’s first row, latest committed reading (`AudioMan.cpp:1998-2004`). Reseat/SwitchControl set overrides (`MovableMan.cpp:593-620`). Team command gate `IsLockstepTeamCommandSender` (`ScenarioRunner.cpp:697-701`) is for `NetGameCommand`s, not for picking the audibility row.
8. **Selftests** — AudioMan logical/AI/committed/alias tests (`AudioMan.cpp:2119-2452`, names in §3). NetLockstep `observation_slot_codec`, `four_peer_observation_relay`, `observation_overflow_carry`, `observation_faults_told_apart`, resync-hole assertion (`NetLockstepSelfTest.cpp:535`, `:3317`, `:3452`, `:3613`, `:3807-3808`).
