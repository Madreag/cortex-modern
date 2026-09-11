# W16: live SoundContainer owners that can exist at save time without being part of the saved world

Source: `D:\Projects\p4b-interp-validation` @ `9751a90e292eb300013c1d6052f3df7bbe228869` (`git -C D:\Projects\p4b-interp-validation rev-parse HEAD`). Read-only. No build. No launch. Writes only this directory.

Commands: `git -C D:\Projects\p4b-interp-validation rev-parse HEAD`; file reads of `Source/Managers/ActivityMan.cpp`, `AudioMan.cpp`, `MovableMan.cpp`, `LuaMan.cpp`, `Main.cpp`, `NetMatchService.cpp`; `Source/Entities/Scene.cpp`, `MOSRotating.cpp`, `AEmitter.cpp`, `Attachable.cpp`, `SoundContainer.cpp`, `MovableObject.cpp`, `Activity.h`; `Source/System/Entity.cpp`, `Serializable.cpp`, `Serializable.h`, `Writer.h`; `Source/GUI/GUISound.cpp`; `Source/Managers/MusicMan.cpp`; `Data/Base.rte/Effects/Wounds.ini`. Quotes are `path:line` from that tree.

There is no `TransferAttachable` symbol. Detach paths are `MOSRotating::RemoveAttachable` and `Attachable::GibThis` → `RemoveAttachable` → `MovableMan::AddMO`.

---

## 1. `Scene::RetrieveSceneObjects(false)` — lists it pulls and lists it does not

`SaveCurrentGame` borrows after the voice capture:

```219:225:Source/Managers/ActivityMan.cpp
	// Pull all stuff from MovableMan into the Scene for saving, so existing Actors/ADoors are saved, without transferring ownership, so the game can continue.
	// TODO- copying may be faster, and lets us move all this actual writing into async
	struct BorrowedSceneObjects {
		Scene& scene;
		~BorrowedSceneObjects() { scene.ClearPlacedObjectSet(Scene::PlacedObjectSets::PLACEONLOAD, false); }
	} borrowedObjects{*modifiableScene};
	modifiableScene->RetrieveSceneObjects(false);
```

`RetrieveSceneObjects` is only those three GetAll* calls. Nothing else.

```2202:2209:Source/Entities/Scene.cpp
int Scene::RetrieveSceneObjects(bool transferOwnership, int onlyTeam, bool noBrains) {
	int found = 0;

	found += g_MovableMan.GetAllActors(transferOwnership, m_PlacedObjects[PlacedObjectSets::PLACEONLOAD], onlyTeam, noBrains);
	found += g_MovableMan.GetAllItems(transferOwnership, m_PlacedObjects[PlacedObjectSets::PLACEONLOAD]);
	found += g_MovableMan.GetAllParticles(transferOwnership, m_PlacedObjects[PlacedObjectSets::PLACEONLOAD]);

	return found;
}
```

`SaveCurrentGame` uses the defaults (`onlyTeam = -1`, `noBrains = false`). `Activity::NoTeam` is `-1` (`Activity.h:46`), so GetAllActors takes every team and brains.

GetAll* **do** include the pending-add queues (`m_AddedActors` / `m_AddedItems` / `m_AddedParticles`). They do **not** filter `IsSetToDelete`. `transferOwnership` is false, so lists are not cleared.

```3102:3124:Source/Managers/MovableMan.cpp
	// Add all regular Actors
	for (std::deque<Actor*>::iterator aIt = m_Actors.begin(); aIt != m_Actors.end(); ++aIt) {
		Actor* actor = *aIt;
		// Only grab ones of a specific team; delete all others
		if ((onlyTeam == Activity::NoTeam || actor->GetTeam() == onlyTeam) && (!noBrains || !actor->HasObjectInGroup("Brains"))) {
			actorList.push_back(actor);
			addedCount++;
		} else if (transferOwnership) {
			delete actor;
		}
	}

	// Add all Actors added this frame
	for (std::deque<Actor*>::iterator aIt = m_AddedActors.begin(); aIt != m_AddedActors.end(); ++aIt) {
```

Same pattern: `GetAllItems` walks `m_Items` then `m_AddedItems` (`3144:3154`). `GetAllParticles` walks `m_Particles` then `m_AddedParticles` (`3169:3179`).

Not pulled by Retrieve / GetAll*:

| not in GetAll* | where it lives | written anyway? |
|---|---|---|
| attached wounds | parent `m_Wounds` | yes, if the root is placed: `SaveSceneObject` `1584:1587` |
| attached attachables / inventory / hardcoded parts | parent lists | yes, same walk (`1578:1582`, inventory ~`1793`, hardcoded `WriteHardcodedAttachableOrNone`) |
| `m_ValidActors` / `m_ValidItems` / `m_ValidParticles` | validity sets only; `ValidMO` `2916:2930` | not walked |
| pending-delete set | none; flag is `m_ToDelete` on the MO | ToDelete roots still in the deques are pulled |
| `m_KnownObjects` extras | every `Create` registers (`MovableObject.cpp:279`, `310`, `448`) | only if the object is a GetAll* root or nested under one |
| `m_MOIDIndex` / MOID-less | `UpdateDrawMOIDs` `4397:4424` sets ToDelete to `SetAsNoID` | MOID is not a Retrieve filter |
| `m_PendingLinkResolves` | restore absorption helper `3409:3415` | those pointers are already in added/settled deques |
| Lua-only / leaked MOs | `m_KnownObjects` only | no |
| GUI / Music / Activity natives | RuntimeGlobals `1054:1069` | not via Retrieve |

`Scene::Save` then writes PLACEONLOAD (the borrowed live pointers) with `SaveSceneObject`. It skips a placed root whose preset name is empty or `"None"`, except a full-save `MOPixel`:

```1288:1292:Source/Entities/Scene.cpp
			if (placedObject->GetPresetName().empty() || placedObject->GetPresetName() == "None") {
				// Preset-less MOPixels (terrain debris) serialize in full form on full-game saves; anything else preset-less is unplaceable.
				if (!(doFullGameSave && dynamic_cast<const MOPixel*>(placedObject))) {
					continue;
				}
			}
```

`doFullGameSave` is true when the activity is not an `EditorActivity` (`1255`). Nested wounds of a skipped root are not written.

`scene->Clone()` at `ActivityMan.cpp:175` copies the **original scene's** placed sets (new Allocate ids, `Scene.cpp:571:598`). `ClearPlacedObjectSet(PLACEONLOAD, true)` then drops those clones (`178`). Retrieve replaces PLACEONLOAD with **live** MovableMan pointers. The INI Scene block at `251` therefore writes the live objects, not the clone-Allocate SoundContainers.

`MOSRotating::Save` does **not** write wounds (that loop is commented, `505:509`). Wounds are only on the `SaveSceneObject` path (`1584:1587`) and the script-graph MO path (`LuaMan.cpp:3356:3358`).

---

## 2. Wound lifecycle

### Creation and BurstSound play

`AddWound` → `AddWoundExt`. Parent ToDelete refuses the add and does not delete the pointer:

```603:646:Source/Entities/MOSRotating.cpp
void MOSRotating::AddWoundExt(AEmitter* woundToAdd, const Vector& parentOffsetToSet, bool checkGibWoundLimit, bool isEntryWound, bool isExitWound) {
	if (woundToAdd && !m_ToDelete) {
		if (checkGibWoundLimit && m_GibWoundLimit > 0 && m_Wounds.size() + 1 >= m_GibWoundLimit) {
			// ...
			} else {
				GibThis(Vector(-5.0F, 0).RadRotate(woundToAdd->GetEmitAngle()));
				woundToAdd->DestroyScriptState();
				delete woundToAdd;
				return;
			}
		}
		// ...
		woundToAdd->SetParent(this);
		woundToAdd->SetIsWound(true);
		if (woundToAdd->GetBurstSound()) {
			if (isEntryWound) { /* may SetPlayBurstSound(false) */ }
			if (isExitWound) { /* may SetPlayBurstSound(false) */ }
		}
		// ...
		m_Wounds.push_back(woundToAdd);
	}
}
```

`SetParent` does not `TriggerBurst` or `Play` (`Attachable.cpp:632:670`). `AddWound` does not `Play`.

`AEmitter::Create(const&)` clones `m_BurstSound` (`83:85`) and copies `m_BurstTriggered` (`100`). The Leaking Machinery wound preset (Metal Impact Machinery) ships `BurstTriggered = 1` and `EmissionEnabled = 1` (`Data/Base.rte/Effects/Wounds.ini:721-730`).

First play is the first `AEmitter::Update` while emitting and burst-triggered:

```545:572:Source/Entities/AEmitter.cpp
	if (m_EmitEnabled) {
		if (!m_WasEmitting) {
			if (m_EmissionSound) {
				m_EmissionSound->Play(m_Pos);
			}
			// ...
		}
		// ...
		if (m_BurstTriggered && CanTriggerBurst()) {
			if (m_BurstSound && m_PlayBurstSound) {
				m_BurstSound->Play(m_Pos);
			}
			m_BurstTimer.Reset();
		}
```

That Update is reached from the parent's `MOSRotating::Update` (`1752:1755`), which runs only if MovableMan's actor/item/particle Update pass runs (after `UpdateControllers`, `3571:3596`). Penetration `AddWoundExt` is during Travel (`986:1010`), **before** that pass, so a same-tick entry wound Plays later in the same `MovableMan::Update` if that pass is reached.

### How wounds are saved

`SaveSceneObject` (snapshot, `saveFullData`) writes each `m_Wounds` entry:

```1584:1587:Source/Entities/Scene.cpp
			for (const AEmitter* wound: mosRotatingToSave->GetWoundList()) {
				writer.NewProperty("SpecialBehaviour_AddWound");
				SaveSceneObject(writer, wound, true, saveFullData);
			}
```

Load: `SpecialBehaviour_AddWound` constructs a new `AEmitter`, `reader >> wound`, `AddWound` (`MOSRotating.cpp:426:429`).

The wound's `AEmitter::SaveSnapshotConfiguration` writes the live `BurstSound` pointer (`289`). `operator<<(Writer, Serializable*)` calls `SoundContainer::Save` (`Serializable.cpp:51:54`). Under snapshot that Save writes `SpecialBehaviour_SoundCheckpoint` (`SoundContainer.cpp:274`). So a wound that is actually walked **does** carry the live BurstSound identity.

### Remove / heal / gib / detach

`RemoveWounds` pops the wound, then destroys it immediately (not queued, not AddMO):

```680:690:Source/Entities/MOSRotating.cpp
	auto removeFirstWoundEmitter = [this]() {
		if (m_Wounds.empty()) {
			return 0.0F;
		}
		AEmitter* wound = m_Wounds.front();
		float woundDamage = wound->GetBurstDamage();
		m_AttachableAndWoundMass -= wound->GetMass();
		std::iter_swap(m_Wounds.begin(), m_Wounds.end() - 1);
		m_Wounds.pop_back();
		wound->DestroyScriptState();
		delete wound;
		return woundDamage;
	};
```

Lifetime / `IsSetToDelete` on an attached wound, inside parent Update: pop, `delete wound` (no `DestroyScriptState` first):

```1757:1761:Source/Entities/MOSRotating.cpp
		if (wound->IsSetToDelete() || (wound->GetLifetime() > 0 && wound->GetAge() > wound->GetLifetime())) {
			std::iter_swap(woundItr, m_Wounds.end() - 1);
			m_Wounds.pop_back();
			m_AttachableAndWoundMass -= wound->GetMass();
			delete wound;
```

`GibThis` sets `m_ToDelete` and keeps wounds on the object (`1042:1079`). Wounds die when the parent is `delete`d (`MOSRotating::Destroy` `731:733`).

`RemoveAttachable(..., addToMovableMan, addBreakWounds)` unparents, optionally clones break wounds onto parent/child, then if `addToMovableMan || IsSetToDelete()` calls `AddMO` (`1914:1922`). The detached object is then in an added deque (or deleted on Absorb if ToDelete). Wounds already on it travel with it.

`Attachable::GibThis` first `RemoveAttachable(this, true, true)` then `MOSRotating::GibThis` (`477:481`) — AddMO then ToDelete.

At the moment `SaveCurrentGame` can run:

- Wound still in `m_Wounds` of a GetAll* root (or nested attachable/inventory): alive, written with BurstSound checkpoint.
- `RemoveWounds` / lifetime `delete`: BurstSound `~SoundContainer` Disowns before Save unless Save is nested inside that delete (see §5).
- Parent ToDelete, not yet `delete`d: still in a GetAll* deque, written (including wounds).
- `AddWoundExt` refused because parent already ToDelete: wound pointer leaked, not in `m_Wounds`, not in GetAll*, not written.

---

## 3. Deletion timing vs `SaveCurrentGame`

Host `ResyncMatch` calls `SaveCurrentGame` (`NetMatchService.cpp:198`). That is reached from `HandleControllerReplayFailure` (`Main.cpp:2074:2084`) after `g_MovableMan.Update()`:

```2476:2484:Source/Main.cpp
					g_MovableMan.Update();
				}
			}
			if (ScenarioRunner::HasControllerReplayError()) {
				HandleControllerReplayFailure(returnToMenuAfterNetworkEnd);
				g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::SimTotal);
				break;
			}
```

Inside `MovableMan::Update`, a controller-replay error returns **before** actor/item/particle Update, Absorb, and delete:

```3486:3488:Source/Managers/MovableMan.cpp
	UpdateControllers();
	if (ScenarioRunner::HasControllerReplayError()) {
		return;
	}
```

Absorb + delete are later in the same function:

```3628:3631:Source/Managers/MovableMan.cpp
	{
		ZoneScopedN("MO Transfer and Deletion");

		AbsorbAddedMOs();
```

```3692:3716:Source/Managers/MovableMan.cpp
			// DELETE //////////////////////////////////////////////////////////
			// Only delete after all travels & updates are done
			// Actors
			aIt = partition(m_Actors.begin(), m_Actors.end(), std::not_fn(std::mem_fn(&MovableObject::ToDelete)));
			// ...
				(*aIt)->DestroyScriptState();
				delete (*aIt);
				m_ValidActors.erase(*aIt);
```

Same delete for items `3723:3733` and particles `3735:3745`. Settle then `delete`s resting particles `3748:3777`.

Between `SetToDelete` / `GibThis` (`1079`) and that `delete`:

- The MO stays in `m_Actors` / `m_Items` / `m_Particles` (or in an added deque).
- `GetAll*` still returns it. Retrieve would write it, including wounds.
- `UpdateDrawMOIDs` gives it no MOID (`4398:4404`) but Retrieve does not read MOIDs.
- SoundContainers are still registered; voices still name them (`~SoundContainer` has not run).

If the desync is first raised at `SubmitLockstepChecksum` (`Main.cpp:2516`), that is **after** the `2478` check on a tick that already finished Absorb/delete. The next sim iteration's `Update` hits `3488` and returns; `SaveCurrentGame` then sees post-Travel, post-controllers, **unabsorbed** added queues and **not-yet-deleted** ToDelete objects. GetAll* still includes both.

`AudioMan::Update` (`2486`) has not run yet on the Resync path.

---

## 4. Attachables and the add-queue merge

`AddActor` / `AddItem` / `AddParticle` push the added deques and the Valid sets (`2648:2650`, `2691:2693`, `2721:2728`). They no-op if `!GetActivity()` (`2614`, `2666`, `2698`).

Merge point is `AbsorbAddedMOs`, only from `MovableMan::Update` after the Update passes (`3631`):

```3360:3403:Source/Managers/MovableMan.cpp
void MovableMan::AbsorbAddedMOs() {
	// Sort the added queues before the drain so the transfer order is stable across runs; a restored world arrives in its saved live order.
	// ...
	for (Actor* addedActor: m_AddedActors) {
		if (!addedActor->IsSetToDelete()) {
			m_Actors.push_back(addedActor);
		} else {
			// ...
			addedActor->DestroyScriptState();
			delete addedActor;
			m_ValidActors.erase(addedActor);
		}
	}
	m_AddedActors.clear();
	// same for items and particles
```

A detach/gib this tick (`RemoveAttachable` → `AddMO`) sits in `m_Added*` until Absorb. Retrieve **does** see it (`GetAll*` walks added). If it is already ToDelete (gib / `DeleteWhenRemovedFromParent`), Absorb `delete`s it and Disowns; a Save **after** a full Update does not see it; a Save **before** Absorb (Lua mid-tick, or resync after `3488` early return) still sees it in GetAll* and would write it.

There is no extra pending list beyond those three added deques.

---

## 5. Voice ownership on destruction — windows where the voice still names a removed owner

Close of ownership is `~SoundContainer` then `Clear`:

```47:51:Source/Entities/SoundContainer.cpp
SoundContainer::~SoundContainer() {
	m_IsDestroying = true;
	g_AudioMan.DisownSoundContainerPlayingChannels(this);
	Destroy(true);
}
```

```801:807:Source/Managers/AudioMan.cpp
void AudioMan::DisownSoundContainerPlayingChannels(const SoundContainer* soundContainer) {
	if (!soundContainer) return;
	for (auto& [identity, voice]: m_PlayingVoices) {
		if (voice.owner != soundContainer) continue;
		voice.owner = nullptr;
		if (voice.channel) voice.channel->setUserData(nullptr);
	}
}
```

`Clear` also Disowns if registered (`SoundContainer.cpp:68:70`) and Unregisters. `AEmitter::Destroy` `delete`s `m_BurstSound` (`390`).

Windows (owner pointer still valid, object not in the saved world, voice still owned):

1. **Wound popped from `m_Wounds`, not yet `delete`d** — opened by `RemoveWounds` `687:689` or Update `1758:1761`; closed by `delete wound` → `~AEmitter` → `delete m_BurstSound` → Disown. Voice still owned in the gap. Object is not in GetAll* and not in any parent's wound list, so Retrieve would not write it. SaveCurrentGame is not called from these lines unless a script inside `DestroyScriptState` (`RemoveWounds` only) invokes it.

2. **Parent removed from MovableMan lists, not yet `delete`d** — opened by the delete-pass erase after `DestroyScriptState` (`3714:3721` etc.); closed by `delete` → `MOSRotating::Destroy` `731:733` → wound BurstSound Disown. After `erase`, GetAll* no longer returns the parent. Voice still owned during `DestroyScriptState` / until BurstSound dtor.

3. **Absorb of a ToDelete added MO** — opened by Absorb's `delete` path (`3376:3378`, `3387:3389`, `3398:3400`); same close. After `m_Added*.clear` the pointer is gone from GetAll*.

4. **`AddWoundExt` refuse on a ToDelete parent** — opened by `AddWoundExt` `604` returning without taking the pointer; never closed by engine. Wound is live (`RegisterObject` already ran on Clone/Create) and not in `m_Wounds` or GetAll*. BurstSound has not Played unless something else called `Play`.

5. **`RemoveAttachable(..., false)` orphan** — opened at `1867` (`SetParent(nullptr)`) when `addToMovableMan` is false and not ToDelete (`1923:1925` returns the pointer). Closed when the caller inventories it, `AddMO`s it, or `delete`s it. Not in GetAll* in the gap. Inventory/equip call sites usually close this before returning to the sim loop.

6. **Lua-owned SoundContainer / owned MO not in MovableMan** — opened by `Create`/`Clone` + `Play`; closed by GC (`CollectGarbageForCheckpoint` `6432:6451`, which `CaptureRuntimeGlobals` runs at `1057` **before** the voice table is written) or by `~SoundContainer`. After that GC, only still-referenced Lua owners remain. Graph encoding for a non-MO Entity that is `owned || IsOriginalPreset()` is name-only, no checkpoint (`LuaMan.cpp:3300:3308`). An owned MO is tagged `"copy"` with class/preset/uid (`3253:3264`), not `SaveSceneObject`.

7. **Between “removed from world” and Disown, voice still owned.** Disown is the SoundContainer destructor / Clear, not Unregister-from-lists. `UnregisterCheckpointSoundContainer` (`1061:1067`) does not null `voice.owner`.

---

## 6. Restore-side lookup

Apply resolves each archived voice owner with `Find` over the **live** map:

```1519:1521:Source/Managers/AudioMan.cpp
		for (const auto& voice: state.voices) {
			SoundContainer* owner = voice.owner ? FindCheckpointSoundContainer(voice.owner) : nullptr;
			if (voice.owner && !owner) throw std::runtime_error("voice " + std::to_string(voice.identity) + " has no registered owner " + std::to_string(voice.owner));
```

```1069:1071:Source/Managers/AudioMan.cpp
SoundContainer* AudioMan::FindCheckpointSoundContainer(uint64_t identity) const {
	const auto found = m_CheckpointSoundContainers.find(identity);
	return found == m_CheckpointSoundContainers.end() || found->second.empty() ? nullptr : found->second.back();
}
```

`Find` is `back()` of whatever pointers are currently registered for that id. It does not consult `m_PendingCheckpoint.soundRegistrations`.

`LoadGameToRestart` stores Added-during-read registrations, then the construction scope puts the pre-read registry and cursor back:

```717:717:Source/Managers/ActivityMan.cpp
	candidate.soundRegistrations = registryScope.GetStagedSoundRegistrations();
```

```4702:4703:Source/Managers/MovableMan.cpp
CheckpointSoundRegistry MovableMan::ConstructionRegistryScope::GetStagedSoundRegistrations() const {
	return g_AudioMan.AddedCheckpointSoundRegistrations(m_OriginalSounds);
```

```4698:4699:Source/Managers/MovableMan.cpp
	g_AudioMan.RestoreCheckpointSoundRegistry(std::move(m_OriginalSounds));
	g_AudioMan.SetCheckpointSoundContainerCursor(m_SoundCursor);
```

`RestoreCheckpointSoundRegistry` swaps `m_CheckpointSoundContainers`; it does not clear `m_LiveCheckpointSoundContainers` or container members (`1085:1093`). Staged objects keep their persisted `m_CheckpointIdentity`.

`ActivateCheckpointSoundRegistrations` re-Registers only candidates that are still in `m_Live` with the same id:

```1096:1099:Source/Managers/AudioMan.cpp
void AudioMan::ActivateCheckpointSoundRegistrations(const CheckpointSoundRegistry& candidates) {
	for (const auto& [identity, owners]: candidates) for (SoundContainer* owner: owners) {
		const auto live = m_LiveCheckpointSoundContainers.find(owner);
		if (live != m_LiveCheckpointSoundContainers.end() && live->second == identity) RegisterCheckpointSoundContainer(owner, identity);
	}
}
```

`RestartActivity` Activate is `1191`, **after** `SetAsideWorld` and **before** Start / `RestoreRuntimeGlobals` apply (`1202`).

`SetAsideWorld` captures another Audio scope (restore-on-exit) and **swaps the live deques out without deleting**:

```1411:1414:Source/Managers/MovableMan.cpp
	AudioMan::CheckpointRegistryScope captureSounds;
	// The settle comes first, as it does in CaptureWorld: a graph captured before it names the objects it sweeps.
	out.runtimeGlobals = g_ActivityMan.CaptureRuntimeGlobals();
	out.soundRegistrations = g_AudioMan.CaptureCheckpointSoundRegistry();
```

```1461:1466:Source/Managers/MovableMan.cpp
	out.actors.swap(m_Actors);
	out.items.swap(m_Items);
	out.particles.swap(m_Particles);
	out.addedActors.swap(m_AddedActors);
	out.addedItems.swap(m_AddedItems);
	out.addedParticles.swap(m_AddedParticles);
```

Scope dtor restores the pre-set-aside `m_CheckpointSoundContainers` (`AudioMan.cpp:1105:1107`). Held objects stay alive and stay in `m_Live` / the restored map.

### (a) Fresh process at apply (`LoadCheckpoint` `1520`)

Registry contents `Find` can see:

- Boot registrations still in the map after ConstructionRegistryScope restore (GUI/preset containers created at process start).
- `Activate(m_PendingCheckpoint.soundRegistrations)` — staged Added pointers from the INI read, if still in `m_Live` with that id.
- Start / `Scene::Create` under `FaithfulCloneScope(true)` (`Scene.cpp:581:583`) — clones Register the persisted ids (`back()` becomes the clone).
- `RestoreScriptGraphs` (`ActivityMan.cpp:939:946`) — any graph-native create/Register.
- GUI then Music apply **inside** the same `RestoreRuntimeGlobals` commit, **before** AudioMan apply (`1110:1125` → `GUISound.cpp:274:280` inserts GUI members; `MusicMan.cpp:650:653` Activate then `LoadCheckpoint`). Those natives are in the map when `1520` runs.

No prior-match MOs. Set-aside on a returner holds an empty MovableMan, not a live match world.

### (b) Resyncing process at apply

Everything in (a), **plus** leftover pre-restart owners: SetAside keeps the old actors/items/particles/added (`1461:1466`) and the registry restore puts those live pointers back in `m_CheckpointSoundContainers`. `Find(9310)` can return a set-aside container that is not in the new Start world.

### Restricting lookup to the restored world

`m_PendingCheckpoint.soundRegistrations` is exactly `AddedCheckpointSoundRegistrations` from the INI read (staged owners only). `ActivateCheckpointSoundRegistrations` is the existing filter: “register this identity only if this pointer is still live with that id.” Apply does not use that set. A lookup that walked only (staged Activated ∪ Start-clone registrations ∪ script-graph registrations ∪ GUI/Music apply inserts), and not the pre-Activate leftover map, would be a restored-world-only Find. The current `Find` is the whole map, leftovers included.

---

## Windows in which a live owner is not saved

| window | opened by | closed by | is the voice still owned | would the object be written |
|---|---|---|---|---|
| Wound still on a retrieved parent / nested attachable / inventory item | `AddWoundExt` `645`; first Play at `AEmitter::Update` `570:571` | `RemoveWounds`/`Update` `delete`, or parent `Destroy` | yes | **yes** — `SaveSceneObject` `1584:1587` + BurstSound checkpoint `289`/`274` |
| ToDelete root still in settled/added deques | `GibThis` `1079` / `SetToDelete` | Absorb ToDelete `delete` or delete-pass `3714:3745` | yes | **yes** — GetAll* does not skip ToDelete |
| This-tick AddMO, not yet absorbed | `RemoveAttachable` `1918` / `AddParticle` `1155` | `AbsorbAddedMOs` `3360` | yes | **yes** — GetAll* walks `m_Added*` |
| Wound popped, not yet `delete`d | `RemoveWounds` `687:689`; Update `1758:1761` | `delete wound` → BurstSound Disown | **yes** | **no** |
| Parent erased from deques, not yet `delete`d | delete-pass after `DestroyScriptState` `3714:3721` | `delete` → wound BurstSounds Disown | **yes** | **no** |
| Absorb ToDelete added MO | `AbsorbAddedMOs` `3372:3378` | same `delete` | **yes** | **no** (already dropped from added) |
| `AddWoundExt` on ToDelete parent (leak) | `AddWoundExt` `604` returns without taking the pointer | never (or caller `delete`) | only if something `Play`s it | **no** |
| `RemoveAttachable(..., false)` orphan not yet inventoried/deleted | `1854:1925` | inventory / `AddMO` / `delete` | yes if already playing | **no** |
| `Add*` no-op (`!GetActivity()`) | `AddActor` `2614` / `AddItem` `2666` / `AddParticle` `2698` | never (leak) | yes if already playing | **no** |
| Lua-owned `SoundContainer` encoded as `copy`/`preset` | `LuaMan.cpp:3300:3308` | GC at Capture `1057`/`6432` or dtor | yes if still referenced after that GC | **no identity** in the graph |
| Lua-owned MO tagged `"copy"` (not AddMO'd) | `LuaMan.cpp:3253:3264` | AddMO or dtor | yes if `Play` | **no** Scene INI; graph is preset name+uid, not `SaveSceneObject` |
| GetAll* root skipped for empty/`None` preset name (not MOPixel) | `Scene.cpp:1288:1292` | n/a | yes | **no** (nested wounds skipped too) |
| GUI / Music natives | `GUISound`/`MusicMan` Initialize / Play | apply / dtor | yes | **not via Retrieve**; carried in RuntimeGlobals |
| Set-aside leftover (restore only) | `SetAsideWorld` `1461:1466` | `DiscardWorld` / `ReinstateWorld` | n/a at host save | n/a at save; **present in (b) Find** |

Resync host save after a `3488` early-return still has unabsorbed added and undeleted ToDelete; those rows stay “would be written.” After a **full** Update, added is empty and ToDelete roots are already destroyed (voices Disowned).

---

## Facts for the fix

**Save site, line 161 vs 225.**

- `161` (`CaptureRuntimeGlobals` → `AudioMan::SaveCheckpoint` `1469:1473`): after `CollectGarbageForCheckpoint` (`1057`). Each physical voice stores `owner->GetCheckpointIdentity()` or `0`. It does not walk MovableMan, `m_KnownObjects`, or the Scene placed list. It does not know whether that pointer will be borrowed at `225` or skipped at `1288`.
- `225` (`RetrieveSceneObjects(false)`): containment is `m_Actors+m_AddedActors`, `m_Items+m_AddedItems`, `m_Particles+m_AddedParticles`, then `SaveSceneObject` nesting (attachables, wounds, inventory, hardcoded parts) under SnapshotScope (`1379`, `211`). It does not consult the voice table, `m_KnownObjects`, Lua userdata, GUI/Music, or `m_Valid*`. A live BurstSound on a wound that is actually walked is written with `SpecialBehaviour_SoundCheckpoint` (`289`/`274`). A live owner that is only in `m_KnownObjects`, only in Lua as `copy`/`preset`, leaked from `AddWoundExt`, or sitting in a popped-but-not-deleted wound slot is not written.

**Apply site.**

- `FindCheckpointSoundContainer` is the whole `m_CheckpointSoundContainers` map (`1069:1071`), leftovers included after `SetAsideWorld` (`1411:1428`, `1461:1466`).
- `m_PendingCheckpoint.soundRegistrations` + `ActivateCheckpointSoundRegistrations` (`1096:1099`) already name the staged INI owners that are still live with that id. Apply does not restrict `1520` to that set (or to Start-clone / script-graph / GUI-Music inserts). That restriction is the existing hook for a restored-world-only lookup.
