# W7: SoundContainer checkpoint-identity registry through the two restore paths

Source tree: `D:\Projects\p4b-interp-validation\Source\` (pin `c8f8188ae0`; AudioMan check strings match W5).
Read-only. No builds. No engine launches. No git writes.
W5 facts: F1 = returner apply `voice 36 has no registered owner 9310` (`AudioMan.cpp:1521`); F2 = returner pre-restore capture `voice 1 has owner 9124 past the sound container cursor 9067` (`AudioMan.cpp:1419`), taken at `ActivityMan.cpp:1178` / `MovableMan.cpp:1413`.

Commands: `rg` / file reads listed at the end. Quotes are `path:line` from that tree.

---

## 1. Every site that sets a SoundContainer checkpoint identity

There is no `SetCheckpointIdentity`. The only writer of `m_CheckpointIdentity` is `ReidentifyCheckpoint`, plus `SwapCheckpoint` (swap) and `Clear` (zero before optional reallocate).

Allocator and register (`AudioMan.cpp`):

```1048:1058:Source/Managers/AudioMan.cpp
uint64_t AudioMan::AllocateCheckpointSoundContainerID() {
	if (m_NextSoundContainerIdentity == std::numeric_limits<uint64_t>::max()) throw std::runtime_error("sound identity space exhausted");
	return ++m_NextSoundContainerIdentity;
}

void AudioMan::RegisterCheckpointSoundContainer(SoundContainer* container, uint64_t identity) {
	if (!identity) return;
	m_LiveCheckpointSoundContainers[container] = identity;
	m_NextSoundContainerIdentity = std::max(m_NextSoundContainerIdentity, identity);
	auto& owners = m_CheckpointSoundContainers[identity];
	if (std::find(owners.begin(), owners.end(), container) == owners.end()) owners.push_back(container);
}
```

`GetCheckpointIdentity` / member (`SoundContainer.h:27`, `SoundContainer.h:579`).

| site | how the identity is obtained | inserts into `m_CheckpointSoundContainers` | bumps cursor (`max` / `++`) | who calls it |
|---|---|---|---|---|
| `SoundContainer::Clear` `SoundContainer.cpp:68-76` | if `m_CheckpointRegistered`: Disown + `Unregister`; then `m_CheckpointIdentity = 0`; if `!m_IsDestroying && (!IsFaithfulClone() \|\| FaithfulCloneRegisters())` then `ReidentifyCheckpoint(AllocateCheckpointSoundContainerID())` | only if that `Reidentify` registers | Allocate does `++`; Register does `max` | ctor `SoundContainer.cpp:33-34`; copy-ctor `37-39` (Clear then Create); `Reset` → `Entity.h:253` `Clear()`; `GUISound::Clear` `GUISound.cpp:17-44` (`m_*.Reset()`); `operator=` `42-44` (`Destroy` then Create) |
| `SoundContainer::ReidentifyCheckpoint` `SoundContainer.cpp:900-904` | argument `identity` stored in `m_CheckpointIdentity` | yes iff `m_CheckpointRegistered = !m_IsDestroying && (!IsFaithfulClone() \|\| FaithfulCloneRegisters())` | only if it calls `Register` (`max`) | `Clear` (new id); `LoadCheckpoint` OnCommit (persisted id) |
| `SoundContainer::LoadCheckpoint` `SoundContainer.cpp:915-927` | archive field `checkpointIdentity`; OnCommit `ReidentifyCheckpoint(checkpointIdentity)` | same as `Reidentify` | same as `Reidentify` | `ReadProperty` `SpecialBehaviour_SoundCheckpoint` `152-154`; `Create(const SoundContainer&)` when `Entity::IsCheckpointClone()` `134-136`; GUI/Music apply; script-graph native load |
| `SoundContainer::Create(const SoundContainer&)` `106-136` | does **not** copy `m_CheckpointIdentity`; Clear already assigned a new id; if `IsCheckpointClone()` then `LoadCheckpoint(reference.SaveCheckpoint())` (persisted id of the reference) | via LoadCheckpoint/`Reidentify` | via those | copy-ctor; `operator=`; `Entity::Clone` `Entity.h:50-64` (`new TYPE()` then `Create(*this)`) |
| `SoundContainer(const SoundContainer&)` `37-39` | `Clear()` then `Create(reference)` | as above | as above | C++ copy; MusicMan/GUI copies; `unique_ptr<SoundContainer>(Clone())` |
| Lua `CreateSoundContainer` `LuaAdapters.cpp:16-22` | `entityPreset->Clone()` → ctor Clear → new Allocate id (unless clone is under Faithful/Checkpoint scope) | yes on normal Clone | yes | Lua scripts; selftests `LuaMan.cpp:4766` |
| Lua `Clone` `LuaAdapters.cpp:90-93` | `thisEntity->Clone()` | same as Entity Clone | same | Lua |
| `SoundContainer::SwapCheckpoint` `953-963` | `swap(m_CheckpointIdentity, other.m_CheckpointIdentity)` | **no** | **no** | `GUISoundCheckpoint::Swap` `GUISound.cpp:243-245` during GUI apply |
| `AudioMan::RegisterCheckpointSoundContainer` `1053-1058` | does not write the member; records the pointer under `identity` | yes (`push_back` if absent) | `max` | `Reidentify` when registered; `ActivateCheckpointSoundRegistrations` `1096-1099`; GUI apply manual insert `274-276` |
| `AudioMan::SetCheckpointSoundContainerCursor` `AudioMan.h:75` | does not set a container identity; assigns `m_NextSoundContainerIdentity` | no | sets cursor absolutely (can lower) | `ConstructionRegistryScope` dtor `MovableMan.cpp:4699`; `CheckpointRegistryScope` dtor `AudioMan.cpp:1107`; GUI/Music apply failure `GUISound.cpp:289`, `MusicMan.cpp:658` |
| `AudioMan::LoadCheckpoint` apply `1613` | writes process cursor from archive `state.nextSoundContainer`; does not write a container member | no | sets cursor to archived value | `RestoreRuntimeGlobals` OnCommit (after F2 capture) |

`IsCheckpointClone()` (`Entity.cpp:14`) is `s_CheckpointCloneDepth != 0 || IsFaithfulClone()`. `Reidentify`/`Clear` gate on **`IsFaithfulClone()` only**, not `IsCheckpointClone()`. `Entity::Create(Reader)` opens `CheckpointCloneScope(reader.IsCheckpoint())` (`Entity.h:233`). `ReadSavedGame` sets `reader.SetCheckpoint(true)` (`ActivityMan.cpp:393-394`). So INI snapshot reads are checkpoint-clones (LoadCheckpoint copies identity) but **still Allocate+Register in Clear** unless a `FaithfulCloneScope` is also open.

`FaithfulCloneScope(false)` (no register, no Allocate in Clear): `GUISound.cpp:210,233`; `AudioMan.cpp:1854,2178`; `MovableMan.cpp:1270,1970`. `FaithfulCloneScope(true)` (register): `Scene.cpp:582-583` when `IsRestoringSnapshot()`; `ActivityMan.cpp` RestartActivity uses `CheckpointCloneScope(true)` at `1189` (not Faithful unless a nested Scene clone opens it).

---

## 2. `RegisterPlayingVoice`, start path, Unregister, `Find`

`RegisterPlayingVoice` (`AudioMan.cpp:1110-1119`) stores `PlayingVoice{channel, owner, path, ...}`. It does **not** read `GetCheckpointIdentity()` and does **not** require `FindCheckpointSoundContainer`. The only production caller is `PlaySoundContainer` after `playSound` (`AudioMan.cpp:573-574`). `SoundContainer::Play` (`400-421`) calls `g_AudioMan.PlaySoundContainer(this, player)` when not deferred.

`PlaySoundContainer` (`490-494`): `physical = !s_PlaybackSuppressed && m_AudioEnabled`. Headless silence (`Main.cpp:332-334`, `AudioMan.h:406`) mutes the master (`AudioMan.cpp:263`); it does not set `s_PlaybackSuppressed`. Physical play still registers a voice.

Unregister / erase from `m_CheckpointSoundContainers`:

```1061:1071:Source/Managers/AudioMan.cpp
void AudioMan::UnregisterCheckpointSoundContainer(SoundContainer* container, uint64_t identity) {
	m_LiveCheckpointSoundContainers.erase(container);
	auto found = m_CheckpointSoundContainers.find(identity);
	if (found == m_CheckpointSoundContainers.end()) return;
	std::erase(found->second, container);
	if (found->second.empty()) m_CheckpointSoundContainers.erase(found);
}

SoundContainer* AudioMan::FindCheckpointSoundContainer(uint64_t identity) const {
	const auto found = m_CheckpointSoundContainers.find(identity);
	return found == m_CheckpointSoundContainers.end() || found->second.empty() ? nullptr : found->second.back();
}
```

Callers of `Unregister`: `Clear` `SoundContainer.cpp:70` (if registered); `Reidentify` `901` (if registered, before the new id); destructor path `~SoundContainer` `47-50` sets `m_IsDestroying`, Disown, `Destroy(true)` → `Clear` (Clear will not Allocate because `m_IsDestroying`). `RestoreCheckpointSoundRegistry` `1085-1093` **swaps** `m_CheckpointSoundContainers` for a filtered copy; it does **not** erase `m_LiveCheckpointSoundContainers` and does **not** change container members.

`Find` when several share one identity: `found->second.back()` = last `push_back` (last successful `Register`). When the last-registered one is destroyed: `Unregister` erases that pointer; if the vector is empty the map entry is removed and `Find` returns null; if others remain, `back()` is the new last.

`DisownSoundContainerPlayingChannels` `801-807` nulls `voice.owner` for that pointer; it does not Unregister.

`RefreshLogicalSound` `1190-1193` inserts into `m_ActiveLogicalSounds` only if `m_CheckpointRegistered && !voices.empty()`. Logical play does not call `RegisterPlayingVoice`. F2's archive voices are physical `m_PlayingVoices` (`SaveCheckpoint` `1469-1473`).

---

## 3. The two restore sequences

Prints: `[net-match] launching from the received snapshot` is `NetMatchService.cpp:335` inside `StageResyncedMatchLaunch` (used by **both** paths). `[net-match] resync: match relaunched from the snapshot` is `Main.cpp:2113` after a **running** process's `RestartActivity` succeeds.

`StageResyncedMatchActivity` is `return g_NetMatchService.StageResyncedMatchLaunch(error)` (`Main.cpp:3486-3488`).

### 3.1 Shared launch helper (`StageResyncedMatchLaunch`)

1. `TakePendingResyncLoad` `NetMatchService.cpp:325`
2. `ActivityMan::LoadGameToRestart(pendingLoad)` `330` / `691-724`
3. `RemoveSavedGame(pendingLoad)` `334` / `686-688` (deletes the `.ccsave`)
4. print `launching from the received snapshot` `335`
5. `GetStartActivity()` + `ClearPlayers`/`AddPlayer` `341-343` (no `Play`; `Activity.cpp:443-490`)
6. `ScenarioRunner::ApplyDeterministicConfig` `345`

`LoadGameToRestart` `691-724`:

1. `ConstructionRegistryScope` ctor `MovableMan.cpp:4673-4686` captures `m_OriginalSounds` + `m_SoundCursor`
2. `SetRestoringSnapshot(true)` `ActivityMan.cpp:705`
3. `ReadSavedGame` `354-520` (`reader.SetCheckpoint(true)` `393`; Scene/Activity `Create(reader)`; `RuntimeGlobals` **validate-only** `431`)
4. `[snapbench] read_ms` `714`
5. `candidate.soundRegistrations = registryScope.GetStagedSoundRegistrations()` `717` = `AddedCheckpointSoundRegistrations` `4702-4703` / `1074-1082`
6. `m_PendingCheckpoint = candidate`; `SetRestartActivity(true)` `718-720`
7. print `SYSTEM: Game "…" staged for restart!` `722`
8. **scope dtor** `4688-4699`: `RestoreCheckpointSoundRegistry(m_OriginalSounds)` + `SetCheckpointSoundContainerCursor(m_SoundCursor)`

After step 8 the staged objects **keep** their `m_CheckpointIdentity` values and remain in `m_LiveCheckpointSoundContainers` if they Registered during the read; `m_CheckpointSoundContainers` and the cursor are the **pre-read** values.

### 3.2 (a) FRESH LAUNCH (returner)

From `RunNetMatchServiceE2E` `Main.cpp:3620`:

1. `NetMatchService::Start` / lobby wait `3643-3678`
2. lobby_snapshot print `3682-3692`
3. `HasPendingResyncLoad()` true → `StageResyncedMatchActivity` `3696-3698`
4. Pending bytes written in `WorkerMain` `NetMatchService.cpp:1009-1019` as `UserSavedGames.rte/p5resync_<pid>_recv.ccsave`
5. Shared helper §3.1 (`read_ms`, `launching … p5resync_*_recv`)
6. `RunGameLoop` `3713` → if `ActivitySetToRestart` then `RestartActivity` `Main.cpp:2185-2188`

`RestartActivity` `1170-1233` (snapshot path; `m_RestartRestoresSnapshot` set true at `713`):

7. `CaptureRuntimeGlobals` `1178` — **F2 archive is this process's live AudioMan after staging, before set-aside**. Internally GC then `g_AudioMan.SaveCheckpoint()` `1054-1069`
8. `SetAsideWorld(originalWorld, false)` `1182` / `1404-1515`: inner `CheckpointRegistryScope` `1411-1428` captures another `runtimeGlobals` at `1413` and `soundRegistrations` at `1414`, then **restores** registry+cursor on scope exit
9. `CheckpointCloneScope(true)` `1189`
10. `PrepareCheckpointMaterials` `1190`
11. `ActivateCheckpointSoundRegistrations(m_PendingCheckpoint.soundRegistrations)` `1191` — re-`Register` staged Added owners (bumps cursor via `max`)
12. `SetSceneToLoad(m_PendingCheckpoint.scene.get(), true, true)` `1193`
13. `m_StartActivity.reset(… activity->Clone())` `1194`
14. `RestartActivityCandidate` `1196` / `889-974`:
    - no `StopAll` because `m_RestartRestoresSnapshot` is still true at `894`
    - `PurgeAllMOs` `895`
    - `restoresSnapshot` captured `903`; flag cleared `904`
    - `SetRestoringSnapshot(true)` `908`
    - `StartActivity` `786-825` → `Activity::Start` `307-368` → `SceneMan::LoadScene` `334-348` clones `m_pSceneToLoad` (`348`). While restoring, `Scene::Create(const Scene&)` opens `FaithfulCloneScope(true)` `581-583`
    - print `SYSTEM: Activity "P4 Alpha Duel" was successfully started` `803`
    - snapshot: skip `GAScripted` `StartActivity` Lua when full script graph `GAScripted.cpp:259-264`
    - `ApplyPendingCheckpoint` / `LoadWorldStructure` / `PrepareCheckpointPrimitives` / `RestoreScriptGraphs` `926-946`
    - `SetRestoringSnapshot(false)` `948`
    - `[snapbench] restart_ms` `971`
15. `RestoreRuntimeGlobals(m_PendingCheckpoint.runtimeGlobals)` `1202` — validate received AudioMan (passed on this job) then OnCommit GUI→Music→AudioMan apply `1123-1126` → **F1 throw** `1521`
16. On failure: `ReinstateWorld` `1226` / `1529-1668` validate-only of set-aside globals `1534` → F2 string; then `RestoreRuntimeGlobals(oldGlobals)` `1230` (same F2 on the `1178` text)

Returner never prints `resync: match relaunched`. `HandleFailedActivityLaunch` `2156-2158` prints `could not launch the activity`.

### 3.3 (b) RESYNC ON A RUNNING PROCESS (host / surviving client)

From the in-loop desync/rejoin handler `Main.cpp:2074-2113`:

1. print `resync: requested, reloading from the host snapshot` `2081`
2. `ResyncMatch` `2084` / `NetMatchService.cpp:187-257`
    - **host only**: `SaveCurrentGame(ResyncSaveName())` `198` (`p5resync_<pid>`), read bytes `207-213`
    - worker `WorkerResyncMain` `260-309`: non-host writes `p5resync_<pid>_recv.ccsave` `273-278`; host `pendingLoad = ResyncSaveName()` `284`
3. wait `ConsumeReadyToLaunch` `2088-2100`
4. `StageResyncedMatchActivity` `2103` — **same helper as (a)** (`read_ms`, `launching …`)
5. `RestartActivity` `2107` — **same function as (a) steps 7-15**
6. on success: print `resync: match relaunched from the snapshot` `2113`

### 3.4 Where the registry and cursor move (both sequences)

| event | `m_CheckpointSoundContainers` | `m_NextSoundContainerIdentity` |
|---|---|---|
| process boot: every `SoundContainer` Clear | insert via Register | `++` per Allocate (returner cursor 9067 in F2 is this pre-read value) |
| `LoadGameToRestart` read | staged objects Register (checkpoint INI) | `max` with persisted ids |
| `ConstructionRegistryScope` dtor | swapped back to pre-read | restored to pre-read |
| `ActivateCheckpointSoundRegistrations` | re-insert Added staged pointers | `max` with those ids |
| `RestartActivityCandidate` Start / scene clone (`FaithfulCloneScope(true)`) | clones Register same persisted ids (`back()` becomes the clone) | `max` |
| `RestoreScriptGraphs` | Lua `ScriptGraphNativeSave`/`Create` may Register new objects | Allocate/`max` |
| GUI/Music/AudioMan apply | GUI swaps+patches `266-280`; Music `WithoutOwners`+`Activate` `650-651`; AudioMan apply sets cursor `1613` | apply writes archived cursor |

### 3.5 Steps in one sequence and not the other (or different order vs apply / first Start `Play`)

Present in **(b)** only, relative to AudioMan apply:

- Prior match already ran; registry already holds live-match owners (ids in the 9xxx range).
- Host `SaveCurrentGame` `ActivityMan.cpp:129-251` under `Writer::SnapshotScope` `211` **and** `AudioMan::CheckpointRegistryScope` `160` **before** `scene->Clone()` `175`.
- `Main.cpp:2081` resync-requested print.
- After successful apply: `Main.cpp:2113` relaunched print.

Present in **(a)** only:

- Cold boot + preset construction (cursor starts as the boot Allocate high-water, 9067 in F2).
- Join-with-state write in `WorkerMain` `1009-1019` (not `WorkerResyncMain`).
- No prior `StartActivity` / no live-match owners.
- Apply failure path: ReinstateWorld / `could not launch` (not reached on client2).

Same function, different **registry contents at apply**:

- `SetAsideWorld` does **not** remove live owners from the registry; its `CheckpointRegistryScope` restores the pre-capture registry (`1411-1428`, `1105-1107`). On **(b)** that is the still-alive match world (objects held aside at `1461-1466`, not deleted). On **(a)** that is boot + whatever staging left in `m_Live`.
- `PurgeAllMOs` `895` walks the current MovableMan lists (emptied by set-aside on (b)), not the held world.
- First `SoundContainer::Play` that can run during activity start is inside `RestartActivityCandidate` **after** `1178`/`1413` and **after** `Activate`, **before** `RestoreRuntimeGlobals` apply: `Activity::Start` → `LoadScene` (no `Play` in `Scene.cpp` / `Scene::LoadData` `639+`); `GAScripted::Start` skips Lua `StartActivity` when restoring a full graph `259-264`. `AEmitter`/`HDFirearm`/`Actor` `Play` sites are Update/fire/death, not Create. So the first start-time `Play` is not in the F2 capture.

`Play` sites in Activities for this activity are GUISound helpers (`GameActivity.cpp:729,1306,…`; `Activity.cpp:571,766,783-789`) — not `Activity::Start`. No `Play(` in `Source/Entities/Scene.cpp`.

---

## 4. F2: identity above cursor, playing, before `RestoreRuntimeGlobals` apply

F2 is `SaveCheckpoint` at `1178`/`1413`: `state.nextSoundContainer = m_NextSoundContainerIdentity` `1437`; each voice `owner->GetCheckpointIdentity()` `1473`; validate `voice.owner > nextSoundContainer` `1419`. So at capture, a live `m_PlayingVoices` entry has owner identity 9124 while the process cursor is 9067.

Proven paths that can leave `GetCheckpointIdentity() > GetCheckpointSoundContainerCursor()`:

1. **`ConstructionRegistryScope` / `CheckpointRegistryScope` restore the cursor without changing object identities** (`MovableMan.cpp:4698-4699`, `AudioMan.cpp:1105-1107`). During `ReadSavedGame`, `LoadCheckpoint` `Reidentify`s persisted ids (9124, 9310, …) and `Register` bumps the cursor; the dtor puts the cursor back to the pre-read value (9067 on a fresh boot). Objects keep the persisted members.
2. **`ReidentifyCheckpoint` with `IsFaithfulClone() && !FaithfulCloneRegisters()`** sets the member and does not `Register` (no `max`) `903-904`. `Clear` under the same gate does not Allocate `74-76`.
3. **`SwapCheckpoint`** swaps members with no register/cursor `963`.
4. **`SetCheckpointSoundContainerCursor`** can lower the cursor under live high identities (`AudioMan.h:75`).

`RegisterPlayingVoice` is only reached from `PlaySoundContainer` `574` (apply-time `playSound` at `1551` builds `candidates` without `RegisterPlayingVoice` and runs **after** F2).

Searched and **not** present: `Play(` in `LoadGameToRestart`, `ReadSavedGame`, `StageResyncedMatchLaunch`, `CaptureRuntimeGlobals`, `Activity::AddPlayer`/`ClearPlayers`, `MusicMan::Initialize` (returns true `45-47`), `MenuMan.cpp`, `Scene.cpp`, Entity `Create` of AEmitter/HDFirearm/Actor/MOSRotating. `LoadCheckpoint` restores `m_PlayingChannels` `927-928` and `RefreshLogicalSound` `948`; it does not call `Play` or `RegisterPlayingVoice`.

INFERENCE: F2's voice 1 therefore requires either (i) a `Play()` reached from a nested `Create(Reader)` / sample path not matched by those greps, or (ii) `Reidentify`/`Swap` of an already-playing owner's identity to 9124 after a boot `Play` (boot GUI identities are assigned in `GUISound::Initialize` `47-120`, which runs **before** `SceneMan::Initialize` `Main.cpp:329-340`, so those ids are low unless later reidentified). The cursor half of F2 (9067 after a read that applied 9xxx ids) is explained by (1) without inference.

Preset path: `Create(const&)` without checkpoint-clone does **not** copy identity (`106-136`). `SpecialBehaviour_SoundCheckpoint` is written only when `writer.IsSnapshot()` `274`. Host save uses `SnapshotScope` `211`. Host **scene** written is `scene->Clone()` `175` **after** `CaptureRuntimeGlobals` `161`, so scene INI identities are the **clone** Allocate ids, not the live-match ids stored in AudioMan voices. Host **Activity** is written as the live pointer `212`.

---

## 5. Which classes persist identity and re-register

Any `SoundContainer::Save` under `Writer::SnapshotScope` writes `SpecialBehaviour_SoundCheckpoint` `274`. Load is `ReadProperty` `152-154` → `LoadCheckpoint` → `Reidentify`. Script-owned Entity save uses the same SnapshotScope (`LuaMan.cpp:3352-3365`).

| class | persisted where | registered on load where | in (a) | in (b) |
|---|---|---|---|---|
| MOSRotating `m_GibSound` | Scene INI `SpecialBehaviour_GibSound` `MOSRotating.cpp:485`; also `GibSound` property | `ReadReflectedPreset` `457-459` / `reader >>` `451-455` → SoundContainer Create+checkpoint | yes, scene staged + Start clone | same phase; **plus leftover live MO still in registry after SetAsideWorld** |
| Actor pain/death/alarm/body/device | `SpecialBehaviour_*Sound` `Actor.cpp:691-695` | same ReadReflectedPreset pattern `624-640` | yes | yes + leftover |
| HDFirearm fire/echo/active/reload/… | `SpecialBehaviour_*Sound` `HDFirearm.cpp:376-383` | `296-324` | yes | yes + leftover |
| AEmitter loop/burst/end | `EmissionSound`/`BurstSound`/`EndSound` `AEmitter.cpp:288-290` (no SpecialBehaviour_ prefix; still nested Save → SoundCheckpoint if snapshot) | `ReadReflectedPreset` `143-154` | yes | yes + leftover |
| ADoor move sounds | `DoorMove*Sound` `ADoor.cpp:201-204` | reader properties `237+` | yes | yes + leftover |
| ACraft hatch/crash | `Hatch*Sound`/`CrashSound` `ACraft.cpp:436-441` | reader `454+` | yes | yes + leftover |
| AHuman/ACrab stride | `SpecialBehaviour_StrideSound` `AHuman.cpp:662` | `623` | yes | yes + leftover |
| Attachable-owned | no own sound members; attachable trees carry MOSRotating/AEmitter sounds | via parent clone/read | yes | yes + leftover |
| Activity-owned | live `Activity` object in Save.ini `212` (not the scene clone); Activity itself plays GUISound, no owned `SoundContainer` members | GUISound path, not MO registry | GUI at apply | GUI at apply + leftover GUI |
| Lua-owned `SoundContainer` userdata | `LuaStateGraph` via `ScriptGraphNativeSave` SnapshotScope `3355-3365`, or as `copy`/`preset` name only `3300-3308` (no identity) | `RestoreScriptGraphs` `ActivityMan.cpp:939-946` **after** Start, **before** apply | yes, that phase | same phase |
| GUI (27 members) | `RuntimeGlobals` `GUISound::SaveCheckpoint` `ActivityMan.cpp:1067` | apply `GUISoundCheckpoint::Apply` `247-283` (Build under `FaithfulCloneScope(false)` `233`, then Swap + **manual** registry insert `274-276`) | apply only | apply only |
| Music | `RuntimeGlobals` `1068` | `MusicMan::LoadCheckpointWithAudio` `634-653` (`WithoutOwners` + `Activate` `650-651`) then AudioMan load | apply only | apply only |

**Phase mark:** MO classes register in (b) from the **pre-resync live world** (still in `m_CheckpointSoundContainers` after SetAsideWorld) **and** from staged/Start clones. In (a) they register only from staged Added + Start clones. That is the class that is present in (b) and missing or different-phase in (a) for a **live** identity such as 9310 if that identity was the host live MO id and the scene INI carried the save-clone id.

---

## 6. Existing detecting coverage

`NetResyncRuntimeSelfTest` is **not in this Source tree** (`Source/Network/` has no such file; `rg NetResyncRuntimeSelfTest` on `Source/` is empty). Later review copies exist under `reviews/takeover-20260909/` but are outside the pin. Nothing in-tree asserts fresh-apply ≡ running-resync-apply.

`[gui-sound-checkpoint-selftest]` `GUISound.cpp:295-333`:

- Plays a GUI owner `307`; saves GUI/music/audio `308-309`.
- Failed apply of a missing sample must not change owners, values, or playback `313`.
- Successful apply must restore native state, owner pointer, and playback `328`.
- Prints `27 native owners, failed-apply identity/playback, nested values and selection continuation PASS` `333`.

Does **not** assert staged-snapshot registration before a start-time `Play`. Does **not** compare fresh vs resync.

`[music-checkpoint-selftest]` `MusicMan.cpp:565-609`:

- Plays three music owners `584-585`; failed apply must not change owners/playback `596`; restore must replace owner pointers and keep playback `599`; hold/reinstate `603-604`.
- Prints `full song/selection, owner rollback, alias and paused playback PASS` `609`.

Does **not** cover snapshot staging vs apply order, or fresh vs resync.

`[audio-logical-selftest]` `AudioMan.cpp:2014+`:

- `registered_while_live` `2060`: `VisitSharedSimulationSounds` sees the logical container (uses `FindCheckpointSoundContainer == container` `1218-1219`), not “registered before first Play”.
- `checkpoint_carries_playback` `2178-2183`: `FaithfulCloneScope(false)` + `LoadCheckpoint` + logical `IsBeingPlayed()` — identity copied **without** Register (`903`).
- Other checks: loops, AI drain, wire encoding of `soundIdentity = GetCheckpointIdentity()` `2152`.

Does **not** assert physical `Find` before `RegisterPlayingVoice`. Does **not** assert fresh ≡ resync.

`[audio-checkpoint-selftest]` (same file, not named in the brief) **does** assert private staging must not change the live registry `1801`, and `ActivateCheckpointSoundRegistrations` makes `Find` return the staged owner `1803`, then live registry is put back `1804`. That is the staging/Activate contract, still not a two-process resync compare.

Related script-graph lines (not the four names): `checkpoint_settles_script_owned_sound` `LuaMan.cpp:4761-4777` (GC before capture so a dropped Lua owner is not demanded); `sound_registry_copy_forgets_destroyed_owner` `4986-5001`.

**Not covered:** registration of staged-snapshot containers before any `Play` during `ReadSavedGame`; identity ≤ cursor after `ConstructionRegistryScope`; `Find(live-owner-id)` after a fresh launch vs after an in-process resync; owner 9310 present in the scene INI vs only in AudioMan.

---

## 7. Retention of `p5resync_*`

Engine write:

- Name: `ResyncSaveName()` = `"p5resync_" + GetProcessID()` `NetMatchService.cpp:39-40`.
- Host save: `SaveCurrentGame(ResyncSaveName())` `198` → `UserSavedGames.rte/p5resync_<pid>.ccsave` `207`, `ActivityMan.cpp:260` (`c_UserScriptedSavesModuleName` = `"UserSavedGames.rte"` `Constants.h:18`).
- Receiver write: `p5resync_<pid>_recv.ccsave` `273-276` (resync worker) and `1013-1016` (fresh join-with-state).

Engine delete:

- `StageResyncedMatchLaunch` **always** `RemoveSavedGame(pendingLoad)` `334` after a successful `LoadGameToRestart`.
- `RemoveSavedGame` `686-688`: `std::filesystem::remove(…/fileName + ".ccsave")`. No flag, no env, no “keep” argument.

`SaveCurrentGame` temp zip is removed on failure (`PendingArchive` dtor `268`).

Harness:

- `peers_3_4_regression.py`: no `p5resync` / keep / save-retain option. Returner argv `91-100` has no keep flag.
- `common.py`: private runtime via `make_run`; no save-retain option.
- `tools/win32_test_runner.py`: no `p5resync` / keep-save / `UserSavedGames` retain option (`rg` empty).

**No keep flag, env var, or driver option exists.** W5 job trees only retain `Index.ini` stubs (99 bytes). The 5,409,186-byte blob is gone after `RemoveSavedGame`.

---

## Candidate mechanisms (≤3; F1 and F2; no fixes)

### C1. Host scene-clone ids in Save.ini vs live ids in AudioMan, plus load-scope cursor rollback

Lines that must run:

- Save: `CaptureRuntimeGlobals` `161` (voices name **live** MO ids, e.g. 9310) **then** `scene->Clone()` `175` (new Allocate ids) **then** `writer Scene` `251` under SnapshotScope `211` (INI `SpecialBehaviour_SoundCheckpoint` carries **clone** ids). Activity is the live object `212`.
- Save scope dtor restores host registry/cursor (`160`, `1105-1107`).
- Fresh load: `ReadSavedGame` LoadCheckpoint of those INI ids; `ConstructionRegistryScope` dtor `4698-4699` restores boot cursor 9067; objects keep 9xxx members.
- Apply `FindCheckpointSoundContainer(9310)` `1520-1521`: fresh registry has clone ids + boot, not live 9310 → F1. Running process still has the live MO in the registry after `SetAsideWorld` `1411-1428` → no F1 line.

Confirm/rule-out: one observation — dump `SpecialBehaviour_SoundCheckpoint` identity for the MO that owns host voice 36 versus `AudioRuntime` voice 36 `owner` in the same `.ccsave`. Equal ⇒ not C1. Unequal and equal to a staged/Start clone id ⇒ C1 for F1. For F2: log `PlaySoundContainer` + `GetCheckpointIdentity()` and `GetCheckpointSoundContainerCursor()` at `1178`; a play with id 9124 after the read and a cursor 9067 confirms the rollback+play pairing.

### C2. Staging Registers then the construction scope unregisters from `m_CheckpointSoundContainers` while leaving identities, `m_Live`, and any voices

Lines: `Register` during read `1053-1058` / `Reidentify` `904`; `GetStagedSoundRegistrations` = Added only `4702-4703`; dtor `RestoreCheckpointSoundRegistry` + cursor restore `4698-4699` (does not touch `m_PlayingVoices` or members). `Activate` `1191` only re-inserts Added pointers that are still in `m_Live` with the same id `1096-1099`. A live owner 9310 that is **not** in Added (never registered during this process's read — e.g. only existed on the host heap) is missing on (a) and still present on (b). A playing owner 9124 with restored cursor 9067 is F2.

Confirm/rule-out: dump `AddedCheckpointSoundRegistrations` vs `m_LiveCheckpointSoundContainers` vs `m_PlayingVoices` at `LoadGameToRestart` return and at `1178`. If 9310 is in live voices' owner ids and absent from Added on the returner, C2 holds for F1. If no `m_PlayingVoices` entry has owner 9124 at `1178`, F2 is not this leftover-voice form.

### C3. `Find` = `back()` plus leftover (b) owners: apply binds a set-aside live container on (b) and throws on (a)

Lines: `Find` `1069-1071`; apply `1520-1521`; (b) set-aside keeps the old objects `1461-1466` and restores the old registry `1428`/`1666` (reinstate path; apply itself uses the restored live registry). (a) has no such objects. Consistent with F2 via the same cursor rollback as C1/C2 (9124 still on a staged or leftover owner at `1178`).

Confirm/rule-out: at apply, log `Find(9310)` pointer and `GetUniqueID()` / address. On client2, if it is a set-aside (pre-restart) MO and not the new Start clone, C3 is confirmed for F1. If both processes' `Find(9310)` are the newly cloned Start object, C3 is ruled out.

---

## Commands run (read-only)

- Read W5 `REPORT.md` §§1,2,5
- `rg` in `D:\Projects\p4b-interp-validation\Source` for identity/register/Play/p5resync/selftest strings
- `rg` in `h4gates/peers_3_4_regression.py`, `common.py`, `tools/win32_test_runner.py` for `p5resync` / keep / `UserSavedGames`
- `rg NetResyncRuntimeSelfTest` in `Source/` (0) and `reviews/takeover-20260909/` (later copies only)
- File reads of `SoundContainer.cpp/.h`, `AudioMan.cpp/.h`, `ActivityMan.cpp`, `MovableMan.cpp`, `NetMatchService.cpp`, `Main.cpp`, `GUISound.cpp`, `MusicMan.cpp`, `LuaMan.cpp`, `LuaAdapters.cpp`, `Scene.cpp`, `SceneMan.cpp`, `GAScripted.cpp`, `Entity.h`, `Writer.h`, entity Save/Read sound properties
