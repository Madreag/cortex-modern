#pragma once

#include "AEmitter.h"
#include "Actor.h"
#include "ACraft.h"
#include "Attachable.h"
#include "BunkerAssembly.h"
#include "Scene.h"
#include "GameActivity.h"
#include "CheckpointFrozenContainers.h"

#include <unordered_set>

namespace RTE {

	// Follow owning edges only. Preset flags describe how an entity was read, and
	// do not say whether PresetMan owns it (children and renamed runtime objects
	// can carry either value). Shared preset targets are reached from their own roots.
	inline void CollectOwnedMovableObjects(const Entity* entity, std::unordered_set<const Entity*>& visited, std::unordered_set<const MovableObject*>& objects) {
		if (!entity || !visited.insert(static_cast<const Entity*>(CheckpointNativeStorage::Original(entity))).second) return;
		entity = CheckpointNativeStorage::Source(entity);
		const auto visit = [&visited, &objects](const Entity* child) { CollectOwnedMovableObjects(child, visited, objects); };
		if (const auto* mo = dynamic_cast<const MovableObject*>(entity)) objects.insert(static_cast<const MovableObject*>(CheckpointNativeStorage::Original(mo)));
		if (const auto* rotating = dynamic_cast<const MOSRotating*>(entity)) {
			CheckpointForEachValue(rotating->GetAttachables(), visit);
			CheckpointForEachValue(rotating->GetWoundList(), visit);
		}
		if (const auto* part = dynamic_cast<const Attachable*>(entity)) part->VisitOwnedWoundTemplates(visit);
		if (const auto* actor = dynamic_cast<const Actor*>(entity)) {
			CheckpointForEachValue(*actor->GetInventory(), visit);
		}
		if (const auto* craft = dynamic_cast<const ACraft*>(entity)) {
			CheckpointForEachValue(craft->GetCollectedInventory(), visit);
		}
		if (const auto* scene = dynamic_cast<const Scene*>(entity)) {
			for (int set = 0; set < Scene::PLACEDSETSCOUNT; ++set) {
				CheckpointForEachValue(*scene->GetPlacedObjects(set), visit);
			}
			for (int player = 0; player < Players::MaxPlayerCount; ++player) visit(scene->GetResidentBrain(player));
		}
		if (const auto* assembly = dynamic_cast<const BunkerAssembly*>(entity)) {
			CheckpointForEachValue(*assembly->GetPlacedObjects(), visit);
		}
		if (const auto* activity = dynamic_cast<const GameActivity*>(entity)) activity->VisitCheckpointOwnedObjects(visit);
	}
}
