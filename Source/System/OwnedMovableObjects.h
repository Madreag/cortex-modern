#pragma once

#include "AEmitter.h"
#include "Actor.h"
#include "ACraft.h"
#include "Attachable.h"
#include "BunkerAssembly.h"
#include "Scene.h"
#include "GameActivity.h"

#include <unordered_set>

namespace RTE {

	// Follow owning edges only. Preset flags describe how an entity was read, and
	// do not say whether PresetMan owns it (children and renamed runtime objects
	// can carry either value). Shared preset targets are reached from their own roots.
	inline void CollectOwnedMovableObjects(const Entity* entity, std::unordered_set<const Entity*>& visited, std::unordered_set<const MovableObject*>& objects) {
		if (!entity || !visited.insert(entity).second) return;
		const auto visit = [&visited, &objects](const Entity* child) { CollectOwnedMovableObjects(child, visited, objects); };
		if (const auto* mo = dynamic_cast<const MovableObject*>(entity)) objects.insert(mo);
		if (const auto* rotating = dynamic_cast<const MOSRotating*>(entity)) {
			for (const Attachable* part: rotating->GetAttachables()) visit(part);
			for (const AEmitter* wound: rotating->GetWoundList()) visit(wound);
		}
		if (const auto* part = dynamic_cast<const Attachable*>(entity)) {
			visit(part->GetOwnedBreakWound());
			visit(part->GetOwnedParentBreakWound());
		}
		if (const auto* actor = dynamic_cast<const Actor*>(entity)) {
			for (const MovableObject* item: *actor->GetInventory()) visit(item);
		}
		if (const auto* craft = dynamic_cast<const ACraft*>(entity)) {
			for (const MovableObject* item: craft->GetCollectedInventory()) visit(item);
		}
		if (const auto* scene = dynamic_cast<const Scene*>(entity)) {
			for (int set = 0; set < Scene::PLACEDSETSCOUNT; ++set) {
				for (const SceneObject* object: *scene->GetPlacedObjects(set)) visit(object);
			}
			for (int player = 0; player < Players::MaxPlayerCount; ++player) visit(scene->GetResidentBrain(player));
		}
		if (const auto* assembly = dynamic_cast<const BunkerAssembly*>(entity)) {
			for (const SceneObject* object: *assembly->GetPlacedObjects()) visit(object);
		}
		if (const auto* activity = dynamic_cast<const GameActivity*>(entity)) activity->VisitCheckpointOwnedObjects(visit);
	}
}
