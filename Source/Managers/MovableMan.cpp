#include "MovableMan.h"
#include <chrono>
#include <map>

#include "SimChecksum.h"
#include "PrimitiveMan.h"
#include "PostProcessMan.h"
#include "PerformanceMan.h"
#include "PresetMan.h"
#include "ConsoleMan.h"
#include "AEmitter.h"
#include "AEJetpack.h"
#include "AHuman.h"
#include "Arm.h"
#include "Leg.h"
#include "ACraft.h"
#include "MOPixel.h"
#include "HeldDevice.h"
#include "HDFirearm.h"
#include "SLTerrain.h"
#include "Controller.h"
#include "AtomGroup.h"
#include "Actor.h"
#include "ADoor.h"
#include "Atom.h"
#include "Scene.h"
#include "FrameMan.h"
#include "GameActivity.h"
#include "SceneMan.h"
#include "SettingsMan.h"
#include "ControllerFrame.h"
#include "ScenarioRunner.h"
#include "LuaMan.h"
#include "ThreadMan.h"

#include <bit>

#include "nlohmann/json.hpp"
#include "tracy/Tracy.hpp"

#include <cmath>
#include <cstdint>
#include <execution>
#include <fstream>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

using namespace RTE;

namespace {
	using json = nlohmann::json;

	json MovableObjectDebugJson(const MovableObject* object) {
		json j;
		if (!object) {
			return j;
		}
		j["uid"] = static_cast<int64_t>(object->GetUniqueID());
		j["moid"] = static_cast<int>(object->GetID());
		j["root_moid"] = static_cast<int>(object->GetRootID());
		j["class"] = object->GetClassName();
		j["preset"] = object->GetPresetName();
		j["module"] = object->GetModuleName();
		j["team"] = object->GetTeam();
		j["pos"] = {object->GetPos().m_X, object->GetPos().m_Y};
		j["vel"] = {object->GetVel().m_X, object->GetVel().m_Y};
		j["rot"] = object->GetRotAngle();
		j["ang_vel"] = object->GetAngularVel();
		j["h_flipped"] = object->IsHFlipped();
		j["mass"] = object->GetMass();
		j["age_ms"] = object->GetAge();
		j["lifetime_ms"] = object->GetLifetime();
		j["to_delete"] = object->IsSetToDelete();
		j["to_settle"] = object->ToSettle();
		return j;
	}

	json ControllerFrameDebugJson(const ControllerFrame& frame) {
		json j;
		j["actor_uid"] = frame.actorUniqueID;
		j["state_mask"] = frame.stateMask;
		j["analog_move"] = {frame.analogMoveX, frame.analogMoveY};
		j["analog_aim"] = {frame.analogAimX, frame.analogAimY};
		j["analog_cursor"] = {frame.analogCursorX, frame.analogCursorY};
		j["mouse_delta"] = {frame.mouseDeltaX, frame.mouseDeltaY};
		j["input_mode"] = frame.inputMode;
		j["player_raw"] = frame.playerRaw;
		j["flags"] = frame.flags;
		j["h_flipped"] = frame.IsActorHFlipped();
		j["aim_angle"] = frame.aimAngle;
		j["view_point"] = {frame.viewPointX, frame.viewPointY};
		j["equipped_fg_uid"] = frame.equippedFGUniqueID;
		j["equipped_bg_uid"] = frame.equippedBGUniqueID;
		j["fg_hand_pos"] = {frame.fgHandPosX, frame.fgHandPosY};
		j["bg_hand_pos"] = {frame.bgHandPosX, frame.bgHandPosY};
		return j;
	}

	json ActorDebugJson(const Actor* actor) {
		json j = MovableObjectDebugJson(actor);
		if (!actor) {
			return j;
		}
		j["health"] = actor->GetHealth();
		j["prev_health"] = actor->GetPrevHealth();
		j["max_health"] = actor->GetMaxHealth();
		j["status"] = actor->GetStatus();
		j["dead"] = actor->IsDead();
		j["ai_mode"] = actor->GetAIMode();
		j["movement_state"] = actor->GetMovementState();
		j["wound_count"] = actor->GetWoundCount(true, true, true);
		j["gib_wound_limit"] = actor->GetGibWoundLimit(true, true, true);
		j["inventory_size"] = actor->GetInventorySize();
		j["inventory_mass"] = actor->GetInventoryMass();
		j["gold"] = actor->GetGoldCarried();
		j["aim_angle"] = actor->GetAimAngle(false);
		j["view_point"] = {actor->GetViewPoint().m_X, actor->GetViewPoint().m_Y};

		json inventory = json::array();
		if (const std::deque<MovableObject*>* items = actor->GetInventory()) {
			for (const MovableObject* item: *items) {
				inventory.push_back(MovableObjectDebugJson(item));
			}
		}
		j["inventory"] = std::move(inventory);

		if (const AHuman* human = dynamic_cast<const AHuman*>(actor)) {
			j["equipped_fg"] = MovableObjectDebugJson(human->GetEquippedItem());
			j["equipped_bg"] = MovableObjectDebugJson(human->GetEquippedBGItem());
			if (const Arm* fgArm = human->GetFGArm()) {
				j["fg_hand_pos"] = {fgArm->GetHandPos().m_X, fgArm->GetHandPos().m_Y};
			}
			if (const Arm* bgArm = human->GetBGArm()) {
				j["bg_hand_pos"] = {bgArm->GetHandPos().m_X, bgArm->GetHandPos().m_Y};
			}
			j["human_upper_state"] = human->GetUpperBodyState();
			j["human_prone_state"] = human->GetProneState();
		}
		return j;
	}

	void DumpControllerDebugSnapshot(const std::string& phase,
	                                 uint64_t tick,
	                                 const std::deque<Actor*>& actors,
	                                 const std::vector<ControllerFrame>* frames = nullptr,
	                                 const std::string* error = nullptr,
	                                 const std::deque<MovableObject*>* particles = nullptr) {
		if (!ScenarioRunner::ShouldControllerDebugDumpTick(tick)) {
			return;
		}

		json root;
		root["tick"] = tick;
		root["phase"] = phase;
		root["actor_count"] = actors.size();
		root["actors"] = json::array();
		for (const Actor* actor: actors) {
			root["actors"].push_back(ActorDebugJson(actor));
		}
		if (particles) {
			root["particle_count"] = particles->size();
			root["particles"] = json::array();
			for (const MovableObject* particle: *particles) {
				root["particles"].push_back(MovableObjectDebugJson(particle));
			}
		}
		if (frames) {
			root["frame_count"] = frames->size();
			root["frames"] = json::array();
			for (const ControllerFrame& frame: *frames) {
				root["frames"].push_back(ControllerFrameDebugJson(frame));
			}
		}
		if (error) {
			root["error"] = *error;
		}

		const std::string& path = ScenarioRunner::GetControllerDebugDumpPath();
		static std::unordered_set<std::string> truncatedPaths;
		const bool firstWrite = truncatedPaths.insert(path).second;
		std::ofstream out(path, firstWrite ? std::ios::trunc : std::ios::app);
		if (out.is_open()) {
			out << root.dump() << '\n';
		}
	}
}

AlarmEvent::AlarmEvent(const Vector& pos, int team, float range) :
	m_ScenePos(pos),
	m_Team((Activity::Teams)team),
	m_Range(range * g_FrameMan.GetPlayerScreenWidth() * 0.51F) {}

const std::string MovableMan::c_ClassName = "MovableMan";

// Comparison functor for sorting movable objects by their X position using STL's sort
struct MOXPosComparison {
	bool operator()(MovableObject* pRhs, MovableObject* pLhs) { return pRhs->GetPos().m_X < pLhs->GetPos().m_X; }
};

// Sorts MO containers by unique ID for a stable per-frame iteration order across same-seed runs.
struct MOUniqueIDLess {
	bool operator()(const MovableObject* a, const MovableObject* b) const noexcept {
		return a->GetUniqueID() < b->GetUniqueID();
	}
};

// A Lua state's registered-MO set copied into canonical unique-ID order (the set itself is unordered).
static std::vector<MovableObject*> SortedRegisteredMOs(const LuaStateWrapper& state) {
	const auto& registered = state.GetRegisteredMOs();
	std::vector<MovableObject*> sorted(registered.begin(), registered.end());
	std::sort(sorted.begin(), sorted.end(), MOUniqueIDLess());
	return sorted;
}

static std::vector<ControllerFrame> SnapshotControllerFrames(const std::deque<Actor*>& actors) {
	std::vector<ControllerFrame> frames;
	frames.reserve(actors.size());
	for (Actor* actor: actors) {
		frames.push_back(ControllerFrameCodec::Snapshot(static_cast<int64_t>(actor->GetUniqueID()), *actor->GetController(), actor));
	}
	return frames;
}

static bool ApplyControllerFramesToActors(const std::deque<Actor*>& actors, const std::vector<ControllerFrame>& frames, std::string& error) {
	if (actors.size() != frames.size()) {
		error = "controller frame count mismatch: actors=" + std::to_string(actors.size()) + " frames=" + std::to_string(frames.size());
		return false;
	}

	for (size_t i = 0; i < actors.size(); ++i) {
		Actor* actor = actors[i];
		const int64_t actorID = static_cast<int64_t>(actor->GetUniqueID());
		const ControllerFrame& frame = frames[i];
		if (actorID != frame.actorUniqueID) {
			error = "controller frame actor mismatch at index " + std::to_string(i) + ": actor=" + std::to_string(actorID) + " frame=" + std::to_string(frame.actorUniqueID);
			return false;
		}

		std::string applyError;
		if (!ControllerFrameCodec::ApplyActorState(frame, *actor, &applyError)) {
			error = "controller frame actor-state apply failed for actor " + std::to_string(actorID) + ": " + applyError;
			return false;
		}
		if (!ControllerFrameCodec::Apply(frame, *actor->GetController(), &applyError)) {
			error = "controller frame apply failed for actor " + std::to_string(actorID) + ": " + applyError;
			return false;
		}
	}
	return true;
}

static bool IsLockstepLocalActor(const Actor* actor) {
	return ScenarioRunner::IsLockstepLocalActor(static_cast<int64_t>(actor->GetUniqueID()), actor->GetTeam(), !actor->IsPlayerControlled());
}

static std::vector<ControllerFrame> SnapshotLockstepControllerFrames(const std::deque<Actor*>& actors, bool localOwned) {
	std::vector<ControllerFrame> frames;
	frames.reserve(actors.size());
	for (Actor* actor: actors) {
		const int64_t actorID = static_cast<int64_t>(actor->GetUniqueID());
		if (IsLockstepLocalActor(actor) == localOwned) {
			frames.push_back(ControllerFrameCodec::Snapshot(actorID, *actor->GetController(), actor));
		}
	}
	return frames;
}

static bool ApplyControllerFramesToLockstepActors(const std::deque<Actor*>& actors, const std::vector<ControllerFrame>& frames, bool localOwned, std::string& error) {
	std::map<int64_t, Actor*> actorsByID;
	for (Actor* actor: actors) {
		const int64_t actorID = static_cast<int64_t>(actor->GetUniqueID());
		if (IsLockstepLocalActor(actor) == localOwned) {
			actorsByID[actorID] = actor;
		}
	}

	for (const ControllerFrame& frame: frames) {
		const auto actorIt = actorsByID.find(frame.actorUniqueID);
		if (actorIt == actorsByID.end()) {
			// The actor can die deterministically on both peers while its frame is in flight; skip it.
			// A real roster divergence is caught by the periodic sim-hash exchange, not here.
			continue;
		}

		if (!MovableMan::ApplyLockstepFrameToActor(*actorIt->second, frame, static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()), &error)) {
			return false;
		}
	}
	return true;
}

bool MovableMan::ApplyLockstepFrameToActor(Actor& actor, const ControllerFrame& frame, uint64_t simTick, std::string* error) {
	std::string applyError;
	if (!ControllerFrameCodec::ApplyActorState(frame, actor, &applyError)) {
		if (error) {
			*error = "lockstep actor-state apply failed for actor " + std::to_string(frame.actorUniqueID) + ": " + applyError;
		}
		return false;
	}
	if (!ControllerFrameCodec::Apply(frame, *actor.GetController(), &applyError)) {
		if (error) {
			*error = "lockstep controller apply failed for actor " + std::to_string(frame.actorUniqueID) + ": " + applyError;
		}
		return false;
	}
	actor.GetController()->SetWireApplyTick(static_cast<int64_t>(simTick));
	return true;
}

static void ApplyLockstepGameCommands(const NetLockstepReadyFrame& readyFrame) {
	if (readyFrame.localCommands.empty() && readyFrame.remoteCommands.empty()) {
		return;
	}
	Activity* activity = g_ActivityMan.GetActivity();
	if (!activity) {
		return;
	}
	// Merge local + remote and sort by sender so both peers apply the identical, identically-ordered set.
	std::vector<NetGameCommand> commands;
	commands.reserve(readyFrame.localCommands.size() + readyFrame.remoteCommands.size());
	commands.insert(commands.end(), readyFrame.localCommands.begin(), readyFrame.localCommands.end());
	commands.insert(commands.end(), readyFrame.remoteCommands.begin(), readyFrame.remoteCommands.end());
	std::stable_sort(commands.begin(), commands.end(), [](const NetGameCommand& lhs, const NetGameCommand& rhs) {
		return lhs.senderPeerId < rhs.senderPeerId;
	});
	for (const NetGameCommand& command: commands) {
		// Only a peer that controls a team may issue economy commands for it — ANY of a shared
		// co-op team's human peers counts; every peer resolves this identically.
		const int32_t commandTeam = NetGameCommandTeam(command.payload);
		if (!ScenarioRunner::IsLockstepTeamCommandSender(commandTeam, command.senderPeerId)) {
			g_ConsoleMan.PrintString("ERROR: Rejected a " + std::string(NetGameCommandTypeName(NetGameCommandTypeOf(command.payload))) + " command from a peer that does not control team " + std::to_string(commandTeam));
			continue;
		}
		if (const NetGameSetTeamFunds* funds = std::get_if<NetGameSetTeamFunds>(&command.payload)) {
			if (funds->team < Activity::TeamOne || funds->team >= Activity::MaxTeamCount) {
				continue;
			}
			activity->SetTeamFunds(static_cast<float>(funds->funds), funds->team);
		} else if (const NetGameSpawnActor* spawn = std::get_if<NetGameSpawnActor>(&command.payload)) {
			// Reject an out-of-range team or a non-finite position before applying.
			if (spawn->team < Activity::TeamOne || spawn->team >= Activity::MaxTeamCount || !std::isfinite(spawn->posX) || !std::isfinite(spawn->posY)) {
				continue;
			}
			if (const Entity* preset = g_PresetMan.GetEntityPreset(spawn->className, spawn->preset, spawn->module)) {
				Entity* clone = preset->Clone();
				if (Actor* actor = dynamic_cast<Actor*>(clone)) {
					actor->SetTeam(spawn->team);
					actor->SetPos(Vector(spawn->posX, spawn->posY));
					if (spawn->aiMode >= 0 && spawn->aiMode < Actor::AIMODE_COUNT) {
						actor->SetAIMode(static_cast<Actor::AIMode>(spawn->aiMode));
					}
					g_MovableMan.AddActor(actor);
				} else {
					delete clone;
				}
			} else {
				g_ConsoleMan.PrintString("ERROR: Deploy rejected - unknown preset \"" + spawn->preset + "\"");
			}
		} else if (const NetGameDeliverCargo* delivery = std::get_if<NetGameDeliverCargo>(&command.payload)) {
			// Reject an out-of-range team or a non-finite spawn before building the craft.
			if (delivery->team < Activity::TeamOne || delivery->team >= Activity::MaxTeamCount || !std::isfinite(delivery->posX) || !std::isfinite(delivery->posY)) {
				continue;
			}
			GameActivity* gameActivity = dynamic_cast<GameActivity*>(activity);
			if (delivery->queuedPurchase && (!gameActivity || !std::isfinite(delivery->cost) || delivery->cost < 0.0F || !std::isfinite(delivery->waypointX) || !std::isfinite(delivery->waypointY))) {
				g_ConsoleMan.PrintString("ERROR: Buy order rejected - bad order fields");
				std::cout << "[net-match] buy order rejected: bad order fields" << std::endl;
				continue;
			}
			const Entity* craftPreset = g_PresetMan.GetEntityPreset(delivery->craftClassName, delivery->craftPreset, delivery->craftModule);
			if (!craftPreset) {
				g_ConsoleMan.PrintString("ERROR: Delivery rejected - unknown craft preset \"" + delivery->craftPreset + "\"");
				continue;
			}
			Entity* craftClone = craftPreset->Clone();
			ACraft* craft = dynamic_cast<ACraft*>(craftClone);
			if (!craft) {
				delete craftClone;
				continue;
			}
			if (delivery->queuedPurchase) {
				// A committed buy order rides the single-player purchase core, so both peers queue the
				// identical arrival and deduct the identical cost at the same synced frame.
				std::list<const SceneObject*> purchases;
				for (const NetGameCargoItem& item: delivery->cargo) {
					const SceneObject* purchase = dynamic_cast<const SceneObject*>(g_PresetMan.GetEntityPreset(item.className, item.preset, item.module));
					if (!purchase) {
						g_ConsoleMan.PrintString("NETWORK: buy order item skipped - unknown preset \"" + item.preset + "\"");
						std::cout << "[net-match] buy order item skipped: unknown preset " << item.preset << std::endl;
						continue;
					}
					purchases.push_back(purchase);
				}
				GameActivity::PurchaseOrder order;
				order.purchases = std::move(purchases);
				order.team = delivery->team;
				order.passengerAIMode = delivery->passengerAIMode;
				order.waypoint = Vector(delivery->waypointX, delivery->waypointY);
				if (delivery->targetUID != 0) {
					order.pTargetMO = dynamic_cast<Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(delivery->targetUID)));
					if (!order.pTargetMO) {
						g_ConsoleMan.PrintString("NETWORK: buy order target not found: UID " + std::to_string(delivery->targetUID));
						std::cout << "[net-match] buy order target not found: UID " << delivery->targetUID << std::endl;
					}
				}
				order.totalCost = delivery->cost;
				// The ordering player index only means something on the peer that issued the order; display-only.
				order.orderedByPlayer = command.senderPeerId == ScenarioRunner::GetLockstepLocalPeerId() ? delivery->orderedByPlayer : Players::NoPlayer;
				order.aiReturnCraft = delivery->returnCraft;
				Vector landingZone(delivery->posX, delivery->posY);
				g_SceneMan.ForceBounds(landingZone);
				order.landingZone = landingZone;
				order.multiOrderYOffset = delivery->multiOrderYOffset;
				craft->SetNetworkDelivery(true);
				const float fundsBefore = activity->GetTeamFunds(delivery->team);
				// Co-op teammates each vet cost against their own view of the funds at issue time; two
				// same-frame orders can both pass a stale check. The apply frame sees identical funds on
				// every peer, so reject here deterministically rather than let the team go negative.
				if (delivery->cost > fundsBefore) {
					delete craft;
					g_ConsoleMan.PrintString("NETWORK: buy order rejected - insufficient team funds");
					std::cout << "[net-match] buy order rejected: team " << delivery->team << " cost " << delivery->cost << " > funds " << fundsBefore << std::endl;
					continue;
				}
				if (!gameActivity->QueuePurchaseDelivery(craft, order)) {
					delete craft;
					g_ConsoleMan.PrintString("NETWORK: buy order did not queue: team " + std::to_string(delivery->team));
					std::cout << "[net-match] buy order did not queue: team " << delivery->team << std::endl;
					continue;
				}
				std::cout << "[net-match] buy order queued: team " << delivery->team << " cost " << delivery->cost << " funds " << fundsBefore << " -> " << activity->GetTeamFunds(delivery->team) << " items " << delivery->cargo.size() << std::endl;
			} else {
				// Load the manifest in order so both peers clone the same presets and assign matching unique ids.
				for (const NetGameCargoItem& item : delivery->cargo) {
					const Entity* itemPreset = g_PresetMan.GetEntityPreset(item.className, item.preset, item.module);
					if (!itemPreset) {
						g_ConsoleMan.PrintString("ERROR: Delivery cargo skipped - unknown preset \"" + item.preset + "\"");
						continue;
					}
					Entity* itemClone = itemPreset->Clone();
					if (MovableObject* cargo = dynamic_cast<MovableObject*>(itemClone)) {
						craft->AddInventoryItem(cargo);
					} else {
						delete itemClone;
					}
				}
				craft->SetTeam(delivery->team);
				craft->SetPos(Vector(delivery->posX, delivery->posY));
				craft->SetControllerMode(Controller::CIM_AI);
				craft->SetAIMode(Actor::AIMODE_DELIVER);
				craft->SetNetworkDelivery(true);
				g_MovableMan.AddActor(craft);
			}
		} else if (const NetGameScuttleCraft* scuttle = std::get_if<NetGameScuttleCraft>(&command.payload)) {
			// Set the scuttle AI mode on every peer so the gib (which runs in both peers' ungated physics) matches.
			if (MovableObject* mo = g_MovableMan.FindObjectByUniqueID(static_cast<long int>(scuttle->actorUID))) {
				// The authority gate checks the CLAIMED team; the craft must really be on it, or a peer
				// could scuttle an enemy craft by UID.
				if (ACraft* craft = dynamic_cast<ACraft*>(mo); craft && craft->GetTeam() == scuttle->team) {
					craft->SetAIMode(Actor::AIMODE_SCUTTLE);
				} else if (craft) {
					g_ConsoleMan.PrintString("ERROR: Rejected a scuttle command for a craft off its claimed team");
				}
			} else {
				// A synced command that silently no-ops on one peer is a desync in the making; say so.
				g_ConsoleMan.PrintString("NETWORK: scuttle command target not found: UID " + std::to_string(scuttle->actorUID));
				std::cout << "[net-match] scuttle command target not found: UID " << scuttle->actorUID << std::endl;
			}
		} else if (const NetGameSetActorAIMode* setMode = std::get_if<NetGameSetActorAIMode>(&command.payload)) {
			Actor* actor = dynamic_cast<Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(setMode->actorUID)));
			if (actor && actor->GetTeam() == setMode->team && setMode->aiMode < Actor::AIMODE_COUNT) {
				actor->SetAIMode(static_cast<Actor::AIMode>(setMode->aiMode));
			} else {
				g_ConsoleMan.PrintString("NETWORK: AI mode command target not found: UID " + std::to_string(setMode->actorUID));
				std::cout << "[net-match] AI mode command target not found: UID " << setMode->actorUID << std::endl;
			}
		} else if (const NetGameSwitchControl* switchControl = std::get_if<NetGameSwitchControl>(&command.payload)) {
			// A peer may only take control for itself; the team gate above already vetted membership.
			if (switchControl->newOwnerPeerId != command.senderPeerId) {
				g_ConsoleMan.PrintString("ERROR: Rejected a SwitchControl command claiming another peer");
				continue;
			}
			// The actor may legally be gone by apply time; the override applies either way so every
			// peer's map stays identical, but a live actor must really be on the claimed team.
			const Actor* actor = dynamic_cast<const Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(switchControl->actorUID)));
			if (actor && actor->GetTeam() != switchControl->team) {
				g_ConsoleMan.PrintString("ERROR: Rejected a SwitchControl command for an actor off its claimed team");
				continue;
			}
			ScenarioRunner::SetLockstepControlOverride(switchControl->actorUID, switchControl->newOwnerPeerId);
			std::cout << "[net-match] control of actor " << switchControl->actorUID << " -> peer " << static_cast<int>(switchControl->newOwnerPeerId) << std::endl;
		} else if (const NetGameInventoryOp* inventoryOp = std::get_if<NetGameInventoryOp>(&command.payload)) {
			Actor* actor = dynamic_cast<Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(inventoryOp->actorUID)));
			AHuman* human = dynamic_cast<AHuman*>(actor);
			bool applied = false;
			// The authority gate checks the CLAIMED team; the actor must really be on it.
			if (actor && actor->GetTeam() == inventoryOp->team) {
				switch (inventoryOp->op) {
					case NetGameInventoryOp::SwapHands:
						applied = human && human->SwapEquippedHeldDevices();
						break;
					case NetGameInventoryOp::SwapEquipped:
						applied = human && human->SwapEquippedItemAndInventoryItem(inventoryOp->a, inventoryOp->b);
						break;
					case NetGameInventoryOp::Reorder:
						if (inventoryOp->a >= 0 && inventoryOp->a < actor->GetInventorySize()) {
							if (inventoryOp->b >= actor->GetInventorySize()) {
								actor->AddInventoryItem(actor->RemoveInventoryItemAtIndex(inventoryOp->a));
								applied = true;
							} else {
								applied = actor->SwapInventoryItemsByIndex(inventoryOp->a, inventoryOp->b);
							}
						}
						break;
					case NetGameInventoryOp::Reload:
						applied = human && human->ReloadEquippedOrInventoryFirearm(inventoryOp->a, inventoryOp->b);
						break;
					case NetGameInventoryOp::Drop: {
						const Vector dropDirection(inventoryOp->dirX, inventoryOp->dirY);
						applied = actor->DropHeldOrInventoryItem(inventoryOp->a, inventoryOp->b, inventoryOp->hasDropDirection ? &dropDirection : nullptr);
						break;
					}
					default:
						break;
				}
			}
			if (!applied) {
				g_ConsoleMan.PrintString("NETWORK: inventory command did not apply: op " + std::to_string(inventoryOp->op) + " UID " + std::to_string(inventoryOp->actorUID));
				std::cout << "[net-match] inventory command did not apply: op " << static_cast<int>(inventoryOp->op) << " UID " << inventoryOp->actorUID << std::endl;
			}
		} else if (const NetGamePauseMatch* pauseMatch = std::get_if<NetGamePauseMatch>(&command.payload)) {
			ScenarioRunner::ApplyLockstepPauseCommand(pauseMatch->pause);
		}
	}
}

static bool CanonicalizeControllerFramesThroughWire(std::vector<ControllerFrame>& frames, std::string& error) {
	for (ControllerFrame& frame: frames) {
		const std::vector<uint8_t> encoded = ControllerFrameCodec::Encode(frame);
		ControllerFrame decoded;
		std::string decodeError;
		if (!ControllerFrameCodec::Decode(encoded.data(), encoded.size(), decoded, &decodeError)) {
			error = "controller frame canonicalization failed for actor " + std::to_string(frame.actorUniqueID) + ": " + decodeError;
			return false;
		}
		frame = decoded;
	}
	return true;
}

// Routes any sim-RNG draws made during a draw to g_RenderRNG, so the draw-rate-dependent
// number of draw passes can't drift the deterministic g_SimRNG stream.
struct ScopedRenderRNG {
	RandomGenerator* m_Prev;
	ScopedRenderRNG() :
	    m_Prev(t_simRNGOverride) { t_simRNGOverride = &g_RenderRNG; }
	~ScopedRenderRNG() { t_simRNGOverride = m_Prev; }
	ScopedRenderRNG(const ScopedRenderRNG&) = delete;
	ScopedRenderRNG& operator=(const ScopedRenderRNG&) = delete;
};

bool MovableMan::RunLockstepPausedTick() {
	const uint64_t simTick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
	std::string error;
	if (!ScenarioRunner::QueueLockstepLocalControllerFrames(simTick, {}, &error)) {
		ScenarioRunner::SetControllerReplayError("tick " + std::to_string(simTick) + " paused queue: " + error);
		return false;
	}
	NetLockstepReadyFrame readyFrame;
	if (!ScenarioRunner::WaitForLockstepControllerFrame(simTick, readyFrame, &error)) {
		ScenarioRunner::SetControllerReplayError("tick " + std::to_string(simTick) + " paused wait: " + error);
		return false;
	}
	// Only the game commands apply on a paused tick; the sim itself holds still.
	ApplyLockstepGameCommands(readyFrame);
	return true;
}

// Snapshot forensics: one line per attachable and wound, recursively, so limb-level state is diffable.
static void DumpAttachableTree(uint64_t tick, const MOSRotating* parent, std::ostream& out) {
	auto dumpNode = [&](const char* kind, const Attachable* node) {
		const HDFirearm* parentFirearm = dynamic_cast<const HDFirearm*>(parent);
		const AEmitter* parentEmitter = dynamic_cast<const AEmitter*>(parent);
		const bool isFlash = (parentFirearm && parentFirearm->GetFlash() == node) || (parentEmitter && parentEmitter->GetFlash() == node);
		out << tick << " " << kind << " uid=" << node->GetUniqueID() << " " << node->GetPresetName()
		    << std::defaultfloat << " par=" << parent->GetUniqueID() << " moid=" << node->GetID() << "/" << node->GetRootID() << " frame=";
		if (isFlash) {
			out << "-";
		} else {
			out << node->GetFrame();
		}
		out
		    << std::hexfloat << " pos=" << node->GetPos().m_X << "," << node->GetPos().m_Y
		    << " vel=" << node->GetVel().m_X << "," << node->GetVel().m_Y
		    << " angvel=" << node->GetAngularVel() << " rot=" << node->GetRotAngle()
		    << " mass=" << node->GetMass() << " awm=" << node->GetAttachableAndWoundMassForSave()
		    << " jp=" << node->GetJointPos().m_X << "," << node->GetJointPos().m_Y
		    << " po=" << node->GetParentOffset().m_X << "," << node->GetParentOffset().m_Y
		    << " jo=" << node->GetJointOffset().m_X << "," << node->GetJointOffset().m_Y
		    << std::defaultfloat << " hf=" << (node->IsHFlipped() ? 1 : 0) << std::hexfloat;
		if (const AtomGroup* group = const_cast<Attachable*>(node)->GetAtomGroup()) {
			out << " moi=" << group->GetStoredMomentOfInertia() << "/" << group->GetStoredOwnerMass() << std::defaultfloat << " atoms=" << group->GetAtomCount() << std::hexfloat;
		}
		if (const Leg* leg = dynamic_cast<const Leg*>(node)) {
			out << " ankle=" << leg->GetAnkleOffset().m_X << "," << leg->GetAnkleOffset().m_Y << " tgt=" << leg->GetTargetPosition().m_X << "," << leg->GetTargetPosition().m_Y;
		}
		if (const Arm* arm = dynamic_cast<const Arm*>(node)) {
			out << " hand=" << arm->GetHandPos().m_X << "," << arm->GetHandPos().m_Y << " hoff=" << arm->GetHandCurrentOffset().m_X << "," << arm->GetHandCurrentOffset().m_Y
			    << std::defaultfloat << " htgts=" << arm->GetNumberOfHandTargets() << " reached=" << (arm->GetHandHasReachedCurrentTarget() ? 1 : 0) << std::hexfloat;
		}
		if (const HeldDevice* device = dynamic_cast<const HeldDevice*>(node)) {
			out << std::defaultfloat << " act=" << (device->IsActivated() ? 1 : 0) << std::hexfloat;
		}
		out << std::defaultfloat << "\n" << std::hexfloat;
		DumpAttachableTree(tick, node, out);
	};
	for (const Attachable* attachable: parent->GetAttachables()) {
		dumpNode("att", attachable);
	}
	for (const AEmitter* wound: parent->GetWoundList()) {
		dumpNode("wnd", wound);
	}
}

void MovableMan::DumpSimState(uint64_t tick, std::ostream& out) const {
	auto dumpMO = [&](const char* kind, MovableObject* mo) {
		out << tick << " " << kind << " uid=" << mo->GetUniqueID() << " " << mo->GetPresetName()
		    << " pos=" << std::hexfloat << mo->GetPos().m_X << "," << mo->GetPos().m_Y
		    << " prev=" << mo->GetPrevPos().m_X << "," << mo->GetPrevPos().m_Y
		    << " vel=" << mo->GetVel().m_X << "," << mo->GetVel().m_Y
		    << " angvel=" << mo->GetAngularVel()
		    << std::defaultfloat << " moid=" << mo->GetID() << "/" << mo->GetRootID() << std::hexfloat
		    << " mass=" << mo->GetMass();
		{
			auto fnv = [](uint64_t h, uint32_t v) { return (h ^ v) * 1099511628211ULL; };
			uint64_t forceHash = 1469598103934665603ULL;
			for (const auto& [force, offset]: mo->GetForces()) {
				forceHash = fnv(fnv(fnv(fnv(forceHash, std::bit_cast<uint32_t>(force.m_X)), std::bit_cast<uint32_t>(force.m_Y)), std::bit_cast<uint32_t>(offset.m_X)), std::bit_cast<uint32_t>(offset.m_Y));
			}
			uint64_t impulseHash = 1469598103934665603ULL;
			for (const auto& [impulse, offset]: mo->GetImpulses()) {
				impulseHash = fnv(fnv(fnv(fnv(impulseHash, std::bit_cast<uint32_t>(impulse.m_X)), std::bit_cast<uint32_t>(impulse.m_Y)), std::bit_cast<uint32_t>(offset.m_X)), std::bit_cast<uint32_t>(offset.m_Y));
			}
			out << std::defaultfloat << " frc=" << mo->GetForces().size() << ":" << std::hex << forceHash << " imps=" << std::dec << mo->GetImpulses().size() << ":" << std::hex << impulseHash << std::dec << std::hexfloat;
			if (const MOSRotating* rotating = dynamic_cast<const MOSRotating*>(mo)) {
				if (const AtomGroup* group = const_cast<MOSRotating*>(rotating)->GetAtomGroup()) {
					out << " moi=" << group->GetStoredMomentOfInertia() << "/" << group->GetStoredOwnerMass();
				}
			}
		}
		out
		    << " rest=" << mo->GetRestTimerElapsedSimMS() << std::defaultfloat
		    << " osc=" << mo->GetVelOscillations() << " settle=" << mo->ToSettle() << std::hexfloat;
		if (const MOSprite* sprite = dynamic_cast<const MOSprite*>(mo)) {
			out << " rot=" << sprite->GetRotAngle();
		}
		if (Actor* actor = dynamic_cast<Actor*>(mo)) {
			Controller* controller = actor->GetController();
			uint64_t states = 0;
			for (int s = 0; s < ControlState::CONTROLSTATECOUNT && s < 64; ++s) {
				if (controller->IsState(static_cast<ControlState>(s))) {
					states |= 1ULL << s;
				}
			}
			out << std::defaultfloat << " awm=" << std::hexfloat << actor->GetAttachableAndWoundMassForSave() << " inv=" << actor->GetInventoryMass() << " gold=" << actor->GetGoldCarried() << " base=" << actor->MovableObject::GetMass() << std::defaultfloat << " ninv=" << actor->GetInventorySize() << " ctrl=0x" << std::hex << states << std::dec << " mode=" << static_cast<int>(controller->GetInputMode()) << " dis=" << controller->IsDisabled() << " status=" << static_cast<int>(actor->GetStatus()) << " aimode=" << static_cast<int>(actor->GetAIMode()) << " health=" << std::hexfloat << actor->GetHealth() << std::defaultfloat;
			if (const ACraft* craft = dynamic_cast<const ACraft*>(mo)) {
				out << " hatch=" << static_cast<int>(craft->GetHatchState()) << " deathms=" << craft->GetDeathTimerElapsedSimMS();
			}
			if (const MOSRotating* rotating = dynamic_cast<const MOSRotating*>(mo)) {
				out << " imp=" << std::hexfloat << rotating->GetTravelImpulse().GetMagnitude() << std::defaultfloat << " wounds=" << rotating->GetWoundCount();
			}
			if (const AHuman* human = dynamic_cast<const AHuman*>(mo)) {
				if (const AEJetpack* jetpack = human->GetJetpack()) {
					out << " jet=" << std::hexfloat << jetpack->GetJetTimeLeft() << std::defaultfloat << " emit=" << jetpack->IsEmitting();
				}
				if (const HDFirearm* gun = dynamic_cast<const HDFirearm*>(const_cast<AHuman*>(human)->GetEquippedItem())) {
					out << " gun=" << gun->GetPresetName() << " rounds=" << gun->GetRoundInMagCount() << " reloading=" << gun->IsReloading();
					out << " gate[" << gun->DescribeFireGate() << "]";
				}
				out << " limbs=" << human->GetLimbGroupPositions();
				// Snapshot forensics: the walk paths, the foot groups and the identity the MO-hit layer sees.
				auto fnv = [](uint64_t h, uint32_t v) { return (h ^ v) * 1099511628211ULL; };
				uint64_t pathHash = 1469598103934665603ULL;
				for (const std::string& state: human->GetLimbPathStates()) {
					for (unsigned char c: state) {
						pathHash = fnv(pathHash, c);
					}
					pathHash = fnv(pathHash, 0x7Cu);
				}
				uint64_t feetHash = 1469598103934665603ULL;
				for (const AtomGroup* group: {human->GetFGFootGroup(), human->GetBGFootGroup()}) {
					if (!group) {
						feetHash = fnv(feetHash, 0xFFFFFFFFu);
						continue;
					}
					for (const Atom* atom: group->GetAtomList()) {
						feetHash = fnv(feetHash, std::bit_cast<uint32_t>(atom->GetOffset().m_X));
						feetHash = fnv(feetHash, std::bit_cast<uint32_t>(atom->GetOffset().m_Y));
					}
					feetHash = fnv(feetHash, std::bit_cast<uint32_t>(group->GetRawLimbPos().m_X));
					feetHash = fnv(feetHash, std::bit_cast<uint32_t>(group->GetRawLimbPos().m_Y));
					feetHash = fnv(feetHash, std::bit_cast<uint32_t>(group->GetStoredMomentOfInertia()));
					feetHash = fnv(feetHash, std::bit_cast<uint32_t>(group->GetStoredOwnerMass()));
					for (const MOID ignored: group->GetIgnoreMOIDs()) {
						feetHash = fnv(feetHash, static_cast<uint32_t>(ignored));
					}
					feetHash = fnv(feetHash, static_cast<uint32_t>(group->GetAtomCount()));
				}
				out << " paths=" << std::hex << pathHash << " feet=" << feetHash << std::dec;
			}
		}
		out << std::defaultfloat << "\n";
		if (const MOSRotating* tree = dynamic_cast<const MOSRotating*>(mo)) {
			DumpAttachableTree(tick, tree, out);
		}
	};
	if (const Activity* activity = g_ActivityMan.GetActivity()) {
		out << tick << " activity state=" << static_cast<int>(activity->GetActivityState());
		for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
			out << " t" << team << "=" << (GetFirstBrainActor(team) ? 1 : 0) << "/" << m_ActorRoster[team].size();
		}
		out << "\n";
	}
	for (Actor* actor: m_Actors) {
		dumpMO("actor", actor);
	}
	for (MovableObject* item: m_Items) {
		dumpMO("item", item);
	}
	for (MovableObject* particle: m_Particles) {
		dumpMO("particle", particle);
	}
	out.flush();
}

int64_t MovableMan::GetFirstCraftUniqueID(int team) const {
	for (Actor* actor: m_Actors) {
		if (actor->GetTeam() == team && dynamic_cast<const ACraft*>(actor)) {
			return static_cast<int64_t>(actor->GetUniqueID());
		}
	}
	return 0;
}

int64_t MovableMan::GetFirstUnloadingCraftUniqueID(int team) const {
	for (Actor* actor: m_Actors) {
		if (actor->GetTeam() != team) {
			continue;
		}
		if (const ACraft* craft = dynamic_cast<const ACraft*>(actor)) {
			if (craft->GetHatchState() == ACraft::OPENING || craft->GetHatchState() == ACraft::OPEN) {
				return static_cast<int64_t>(actor->GetUniqueID());
			}
		}
	}
	return 0;
}

MovableMan::MovableMan() {
	Clear();
}

MovableMan::~MovableMan() {
	Destroy();
}

void MovableMan::Clear() {
	m_Actors.clear();
	m_ContiguousActorIDs.clear();
	m_Items.clear();
	m_Particles.clear();
	m_AddedActors.clear();
	m_AddedItems.clear();
	m_AddedParticles.clear();
	m_ValidActors.clear();
	m_ValidItems.clear();
	m_ValidParticles.clear();
	m_ActorRoster[Activity::TeamOne].clear();
	m_ActorRoster[Activity::TeamTwo].clear();
	m_ActorRoster[Activity::TeamThree].clear();
	m_ActorRoster[Activity::TeamFour].clear();
	m_SortTeamRoster[Activity::TeamOne] = false;
	m_SortTeamRoster[Activity::TeamTwo] = false;
	m_SortTeamRoster[Activity::TeamThree] = false;
	m_SortTeamRoster[Activity::TeamFour] = false;
	m_AddedAlarmEvents.clear();
	m_AlarmEvents.clear();
	m_MOIDIndex.clear();
	m_SplashRatio = 0.75;
	m_MaxDroppedItems = 100;
	m_SettlingEnabled = true;
	m_MOSubtractionEnabled = true;
	// HitWhatMOID / HitWhatTerrMaterial compare against this each tick; it's otherwise only incremented.
	m_SimUpdateFrameNumber = 0;
}

int MovableMan::Initialize() {
	// TODO: Increase this number, or maybe only for certain classes?
	Entity::ClassInfo::FillAllPools();

	return 0;
}

int MovableMan::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return Serializable::ReadProperty(propName, reader));

	MatchProperty("AddEffect", { g_PresetMan.GetEntityPreset(reader); });
	MatchProperty("AddAmmo", { g_PresetMan.GetEntityPreset(reader); });
	MatchProperty("AddDevice", { g_PresetMan.GetEntityPreset(reader); });
	MatchProperty("AddActor", { g_PresetMan.GetEntityPreset(reader); });
	MatchProperty("SplashRatio", { reader >> m_SplashRatio; });

	EndPropertyList;
}

int MovableMan::Save(Writer& writer) const {
	Serializable::Save(writer);

	writer << m_Actors.size();
	for (std::deque<Actor*>::const_iterator itr = m_Actors.begin(); itr != m_Actors.end(); ++itr)
		writer << **itr;

	writer << m_Particles.size();
	for (std::deque<MovableObject*>::const_iterator itr2 = m_Particles.begin(); itr2 != m_Particles.end(); ++itr2)
		writer << **itr2;

	return 0;
}

void MovableMan::Destroy() {
	for (std::deque<Actor*>::iterator it1 = m_Actors.begin(); it1 != m_Actors.end(); ++it1)
		delete (*it1);
	for (std::deque<MovableObject*>::iterator it2 = m_Items.begin(); it2 != m_Items.end(); ++it2)
		delete (*it2);
	for (std::deque<MovableObject*>::iterator it3 = m_Particles.begin(); it3 != m_Particles.end(); ++it3)
		delete (*it3);
	for (std::vector<AlarmEvent*>::iterator it4 = m_AlarmEvents.begin(); it4 != m_AlarmEvents.end(); ++it4)
		delete (*it4);
	for (std::vector<AlarmEvent*>::iterator it5 = m_AddedAlarmEvents.begin(); it5 != m_AddedAlarmEvents.end(); ++it5)
		delete (*it5);

	Clear();
}

MovableObject* MovableMan::GetMOFromID(MOID whichID) {
	if (whichID != g_NoMOID && whichID != 0 && whichID < m_MOIDIndex.size()) {
		// This is really, really awful
		// But, Lua scripts can take ownership of an MO which exists in this list
		// And then the MO can be deallocated by Lua GC
		// Meaning that this ptr points to stale memory.
		// Due to our pooled memory system, we can avoid a crash by just... checking that the MO isn't NoMOID
		// But this is still atrociously awful and terrible and this can point to a newly-allocated object that just so happens to be allocated the same place.
		// Anyways, until we can fix the god-awful abomination that is this game's memory ownership semantics, we're stuck with this
		// Which is also technically undefined behaviour
		MovableObject* candidate = m_MOIDIndex[whichID];
		if (candidate->GetID() != whichID) {
			return nullptr;
		}

		return candidate;
	}
	return nullptr;
}

MOID MovableMan::GetMOIDPixel(int pixelX, int pixelY, const std::vector<int>& moidList) {
	// Note - We loop through the MOs in reverse to make sure that the topmost (last drawn) MO that overlaps the specified coordinates is the one returned.
	for (auto itr = moidList.rbegin(), itrEnd = moidList.rend(); itr < itrEnd; ++itr) {
		MOID moid = *itr;
		const MovableObject* mo = GetMOFromID(moid);

		// Commented... see MovableMan::GetMOFromID
		// RTEAssert(mo, "Null MO found in MOID list!");
		if (mo == nullptr) {
			continue;
		}

		if (mo->GetScale() == 0.0f) {
			return g_NoMOID;
		} else if (mo->HitTestAtPixel(pixelX, pixelY)) {
			return moid;
		}
	}

	return g_NoMOID;
}

void MovableMan::RegisterObject(MovableObject* mo) {
	if (!mo) {
		return;
	}

	std::lock_guard<std::mutex> guard(m_ObjectRegisteredMutex);
	m_KnownObjects[mo->GetUniqueID()] = mo;
}

void MovableMan::UnregisterObject(MovableObject* mo) {
	if (!mo) {
		return;
	}

	std::lock_guard<std::mutex> guard(m_ObjectRegisteredMutex);
	// Only drop the entry this object owns; an off-world snapshot clone shares its UniqueID with the live resident.
	auto entry = m_KnownObjects.find(mo->GetUniqueID());
	if (entry != m_KnownObjects.end() && entry->second == mo) {
		m_KnownObjects.erase(entry);
	}
}

const std::vector<MovableObject*>* MovableMan::GetMOsInBox(const Box& box, int ignoreTeam, bool getsHitByMOsOnly) const {
	std::vector<MovableObject*>* vectorForLua = new std::vector<MovableObject*>();
	*vectorForLua = std::move(g_SceneMan.GetMOIDGrid().GetMOsInBox(box, ignoreTeam, getsHitByMOsOnly));
	return vectorForLua;
}

const std::vector<MovableObject*>* MovableMan::GetMOsInRadius(const Vector& centre, float radius, int ignoreTeam, bool getsHitByMOsOnly) const {
	std::vector<MovableObject*>* vectorForLua = new std::vector<MovableObject*>();
	*vectorForLua = std::move(g_SceneMan.GetMOIDGrid().GetMOsInRadius(centre, radius, ignoreTeam, getsHitByMOsOnly));
	return vectorForLua;
}

const std::vector<MovableObject*>* MovableMan::GetMOsAtPosition(int pixelX, int pixelY, int ignoreTeam, bool getsHitByMOsOnly) const {
	std::vector<MovableObject*>* vectorForLua = new std::vector<MovableObject*>();
	*vectorForLua = std::move(g_SceneMan.GetMOIDGrid().GetMOsAtPosition(pixelX, pixelY, ignoreTeam, getsHitByMOsOnly));
	return vectorForLua;
}

void MovableMan::WorldSnapshot::Clear() {
	for (Actor* actor: actors) {
		delete actor;
	}
	for (MovableObject* item: items) {
		delete item;
	}
	for (MovableObject* particle: particles) {
		delete particle;
	}
	actors.clear();
	items.clear();
	particles.clear();
	joinQuarantine.clear();
	uniqueIDCounter = 0;
}

bool MovableMan::CaptureWorld(WorldSnapshot& out) const {
	out.Clear();
	if (!m_AddedActors.empty() || !m_AddedItems.empty() || !m_AddedParticles.empty()) {
		return false;
	}
	const long counter = MovableObject::GetUniqueIDCounter();
	{
		MovableObject::FaithfulCloneScope scope(false);
		out.actors.reserve(m_Actors.size());
		out.items.reserve(m_Items.size());
		out.particles.reserve(m_Particles.size());
		std::map<std::string, std::pair<int, double>> profile;
		const auto timed = [&profile](const MovableObject* mo, auto&& fn) {
			const auto start = std::chrono::steady_clock::now();
			fn();
			auto& slot = profile[mo->GetClassName() + " " + mo->GetPresetName()];
			++slot.first;
			slot.second += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		};
		for (const Actor* actor: m_Actors) {
			timed(actor, [&] { out.actors.push_back(dynamic_cast<Actor*>(actor->Clone())); });
		}
		for (const MovableObject* item: m_Items) {
			timed(item, [&] { out.items.push_back(dynamic_cast<MovableObject*>(item->Clone())); });
		}
		for (const MovableObject* particle: m_Particles) {
			timed(particle, [&] { out.particles.push_back(dynamic_cast<MovableObject*>(particle->Clone())); });
		}
		if (std::getenv("CC_CAPTURE_PROFILE")) {
			std::vector<std::pair<std::string, std::pair<int, double>>> rows(profile.begin(), profile.end());
			std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
			for (size_t i = 0; i < rows.size() && i < 12; ++i) {
				std::cout << "[capture-profile] " << rows[i].first << " n=" << rows[i].second.first << " ms=" << rows[i].second.second << std::endl;
			}
		}
	}
	out.joinQuarantine = m_LockstepJoinQuarantine;
	// Faithful clones keep their identity, but anything the clone chain drew from the counter is undone.
	MovableObject::PinUniqueIDCounter(counter);
	out.uniqueIDCounter = counter;
	return true;
}

bool MovableMan::RestoreWorld(const WorldSnapshot& in) {
	CompleteQueuedMOIDDrawings();
	for (Actor* actor: m_AddedActors) {
		actor->DestroyScriptState();
		delete actor;
	}
	for (MovableObject* item: m_AddedItems) {
		item->DestroyScriptState();
		delete item;
	}
	for (MovableObject* particle: m_AddedParticles) {
		particle->DestroyScriptState();
		delete particle;
	}
	m_AddedActors.clear();
	m_AddedItems.clear();
	m_AddedParticles.clear();
	PurgeAllMOs();
	{
		MovableObject::FaithfulCloneScope scope(true);
		for (const Actor* actor: in.actors) {
			Actor* live = dynamic_cast<Actor*>(actor->Clone());
			live->AdoptPersistedUniqueID();
			m_Actors.push_back(live);
			m_ValidActors.insert(live);
			AddActorToTeamRoster(live);
		}
		for (const MovableObject* item: in.items) {
			MovableObject* live = dynamic_cast<MovableObject*>(item->Clone());
			live->AdoptPersistedUniqueID();
			m_Items.push_back(live);
			m_ValidItems.insert(live);
		}
		for (const MovableObject* particle: in.particles) {
			MovableObject* live = dynamic_cast<MovableObject*>(particle->Clone());
			live->AdoptPersistedUniqueID();
			m_Particles.push_back(live);
			m_ValidParticles.insert(live);
		}
	}
	MovableObject::PinUniqueIDCounter(in.uniqueIDCounter);
	m_LockstepJoinQuarantine = in.joinQuarantine;
	for (Actor* actor: m_Actors) {
		actor->ResolveFaithfulLinks();
	}
	for (MovableObject* item: m_Items) {
		item->ResolveFaithfulLinks();
	}
	for (MovableObject* particle: m_Particles) {
		particle->ResolveFaithfulLinks();
	}
	return true;
}

MovableMan::AddQueueMark MovableMan::MarkAddQueues() {
	std::scoped_lock lock(m_AddedActorsMutex, m_AddedItemsMutex, m_AddedParticlesMutex, m_AddedAlarmEventsMutex);
	return {m_AddedActors.size(), m_AddedItems.size(), m_AddedParticles.size(), m_AddedAlarmEvents.size()};
}

void MovableMan::DiscardAddedSince(const AddQueueMark& mark) {
	std::scoped_lock lock(m_AddedActorsMutex, m_AddedItemsMutex, m_AddedParticlesMutex, m_AddedAlarmEventsMutex);
	while (m_AddedActors.size() > mark.actors) {
		Actor* actor = m_AddedActors.back();
		m_AddedActors.pop_back();
		if (actor->GetTeam() >= 0) {
			RemoveActorFromTeamRoster(actor);
		}
		m_ValidActors.erase(actor);
		actor->DestroyScriptState();
		delete actor;
	}
	while (m_AddedItems.size() > mark.items) {
		MovableObject* item = m_AddedItems.back();
		m_AddedItems.pop_back();
		m_ValidItems.erase(item);
		item->DestroyScriptState();
		delete item;
	}
	while (m_AddedParticles.size() > mark.particles) {
		MovableObject* particle = m_AddedParticles.back();
		m_AddedParticles.pop_back();
		m_ValidParticles.erase(particle);
		particle->DestroyScriptState();
		delete particle;
	}
	while (m_AddedAlarmEvents.size() > mark.alarms) {
		delete m_AddedAlarmEvents.back();
		m_AddedAlarmEvents.pop_back();
	}
}

bool MovableMan::SwapActorForRender(Actor* original, Actor* substitute) {
	const auto found = std::find(m_Actors.begin(), m_Actors.end(), original);
	if (found == m_Actors.end()) {
		return false;
	}
	*found = substitute;
	// The HUD walks the team roster by identity; the substitute takes that slot too.
	for (std::list<Actor*>& roster: m_ActorRoster) {
		const auto slot = std::find(roster.begin(), roster.end(), original);
		if (slot != roster.end()) {
			*slot = substitute;
		}
	}
	return true;
}

void MovableMan::WaitForActorsSeeTask() {
	m_ActorsSeeFuture.wait();
}

std::string MovableMan::DescribeScriptBindings() const {
	std::string out;
	const auto describe = [&out](const std::string& name, const LuaStateWrapper& state) {
		out += name;
		for (const MovableObject* mo: SortedRegisteredMOs(state)) {
			out += " " + std::to_string(mo->GetUniqueID()) + (mo->ObjectScriptsInitialized() ? "+" : "-");
		}
		out += "\n";
	};
	describe("master", g_LuaMan.GetMasterScriptState());
	int index = 0;
	for (const LuaStateWrapper& state: g_LuaMan.GetThreadedScriptStates()) {
		describe("thread" + std::to_string(index++), state);
	}
	return out;
}

bool MovableMan::RefuseSpeculativeRemoval(const MovableObject* mo, const char* kind) {
	if (!m_Speculative || !ValidMO(mo)) {
		return false;
	}
	++m_SpeculativeRefusals;
	if (m_SpeculativeRefusals <= 3) {
		std::cout << "[speculation] refused removal of canonical " << kind << " uid=" << mo->GetUniqueID() << " " << mo->GetPresetName() << " during a preview" << std::endl;
	}
	return true;
}

void MovableMan::PurgeAllMOs() {
	if (m_Speculative) {
		++m_SpeculativeRefusals;
		std::cout << "[speculation] refused PurgeAllMOs during a preview" << std::endl;
		return;
	}
	for (std::deque<Actor*>::iterator itr = m_Actors.begin(); itr != m_Actors.end(); ++itr) {
		(*itr)->DestroyScriptState();
	}
	for (std::deque<MovableObject*>::iterator itr = m_Items.begin(); itr != m_Items.end(); ++itr) {
		(*itr)->DestroyScriptState();
	}
	for (std::deque<MovableObject*>::iterator itr = m_Particles.begin(); itr != m_Particles.end(); ++itr) {
		(*itr)->DestroyScriptState();
	}

	for (std::deque<Actor*>::iterator itr = m_Actors.begin(); itr != m_Actors.end(); ++itr) {
		delete (*itr);
	}
	for (std::deque<MovableObject*>::iterator itr = m_Items.begin(); itr != m_Items.end(); ++itr) {
		delete (*itr);
	}
	for (std::deque<MovableObject*>::iterator itr = m_Particles.begin(); itr != m_Particles.end(); ++itr) {
		delete (*itr);
	}

	m_Actors.clear();
	m_Items.clear();
	m_Particles.clear();
	m_AddedActors.clear();
	m_AddedItems.clear();
	m_AddedParticles.clear();
	m_ValidActors.clear();
	m_ValidItems.clear();
	m_ValidParticles.clear();
	m_ActorRoster[Activity::TeamOne].clear();
	m_ActorRoster[Activity::TeamTwo].clear();
	m_ActorRoster[Activity::TeamThree].clear();
	m_ActorRoster[Activity::TeamFour].clear();
	m_SortTeamRoster[Activity::TeamOne] = false;
	m_SortTeamRoster[Activity::TeamTwo] = false;
	m_SortTeamRoster[Activity::TeamThree] = false;
	m_SortTeamRoster[Activity::TeamFour] = false;
	m_AddedAlarmEvents.clear();
	m_AlarmEvents.clear();
	m_LockstepJoinQuarantine.clear();
	m_MOIDIndex.clear();
	// We want to keep known objects around, 'cause these can exist even when not in the simulation (they're here from creation till deletion, regardless of whether they are in sim)
	// m_KnownObjects.clear();
}

Actor* MovableMan::GetNextActorInGroup(std::string group, Actor* pAfterThis) {
	if (group.empty())
		return 0;

	// Begin at the beginning
	std::deque<Actor*>::const_iterator aIt = m_Actors.begin();

	// Search for the actor to start search from, if specified
	if (pAfterThis) {
		// Make the iterator point to the specified starting point actor
		for (; aIt != m_Actors.end() && !((*aIt)->IsInGroup(group) && *aIt == pAfterThis); ++aIt)
			;

		// If we couldn't find the one to search for,
		// then just start at the beginning again and get the first actor at the next step
		if (aIt == m_Actors.end())
			aIt = m_Actors.begin();
		// Go one more step so we're not pointing at the one we're not supposed to get
		else
			++aIt;
	}

	// Now search for the first actor of the team from the search point (beginning or otherwise)
	for (; aIt != m_Actors.end() && !(*aIt)->IsInGroup(group); ++aIt)
		;

	// If nothing found between a specified actor and the end,
	// then restart and see if there's anything between beginning and that specified actor
	if (pAfterThis && aIt == m_Actors.end()) {
		for (aIt = m_Actors.begin(); aIt != m_Actors.end() && !(*aIt)->IsInGroup(group); ++aIt)
			;

		// Still nothing?? Should at least get the specified actor and return it! - EDIT No becuase it just may not be there!
		//        RTEAssert(aIt != m_Actors.end(), "Search for something after specified actor, and didn't even find the specified actor!?");
	}

	// Still nothing, so return nothing
	if (aIt == m_Actors.end())
		return 0;

	if ((*aIt)->IsInGroup(group))
		return *aIt;

	return 0;
}

Actor* MovableMan::GetPrevActorInGroup(std::string group, Actor* pBeforeThis) {
	if (group.empty())
		return 0;

	// Begin at the reverse beginning
	std::deque<Actor*>::reverse_iterator aIt = m_Actors.rbegin();

	// Search for the actor to start search from, if specified
	if (pBeforeThis) {
		// Make the iterator point to the specified starting point actor
		for (; aIt != m_Actors.rend() && !((*aIt)->IsInGroup(group) && *aIt == pBeforeThis); ++aIt)
			;

		// If we couldn't find the one to search for,
		// then just start at the beginning again and get the first actor at the next step
		if (aIt == m_Actors.rend())
			aIt = m_Actors.rbegin();
		// Go one more step so we're not pointing at the one we're not supposed to get
		else
			++aIt;
	}

	// Now search for the first actor of the team from the search point (beginning or otherwise)
	for (; aIt != m_Actors.rend() && !(*aIt)->IsInGroup(group); ++aIt)
		;

	// If nothing found between a specified actor and the end,
	// then restart and see if there's anything between beginning and that specified actor
	if (pBeforeThis && aIt == m_Actors.rend()) {
		for (aIt = m_Actors.rbegin(); aIt != m_Actors.rend() && !(*aIt)->IsInGroup(group); ++aIt)
			;

		// Still nothing?? Should at least get the specified actor and return it! - EDIT No becuase it just may not be there!
		//        RTEAssert(aIt != m_Actors.rend(), "Search for something after specified actor, and didn't even find the specified actor!?");
	}

	// Still nothing, so return nothing
	if (aIt == m_Actors.rend())
		return 0;

	if ((*aIt)->IsInGroup(group))
		return *aIt;

	return 0;
}

Actor* MovableMan::GetNextTeamActor(int team, Actor* pAfterThis) {
	if (team < Activity::TeamOne || team >= Activity::MaxTeamCount || m_ActorRoster[team].empty())
		return 0;
	/*
	    // Begin at the beginning
	    std::deque<Actor *>::const_iterator aIt = m_Actors.begin();

	    // Search for the actor to start search from, if specified
	    if (pAfterThis)
	    {
	        // Make the iterator point to the specified starting point actor
	        for (; aIt != m_Actors.end() && !((*aIt)->GetTeam() == team && *aIt == pAfterThis); ++aIt)
	            ;

	        // If we couldn't find the one to search for,
	        // then just start at the beginning again and get the first actor at the next step
	        if (aIt == m_Actors.end())
	            aIt = m_Actors.begin();
	        // Go one more step so we're not pointing at the one we're not supposed to get
	        else
	            ++aIt;
	    }

	    // Now search for the first actor of the team from the search point (beginning or otherwise)
	    for (; aIt != m_Actors.end() && (*aIt)->GetTeam() != team; ++aIt)
	        ;

	    // If nothing found between a specified actor and the end,
	    // then restart and see if there's anything between beginning and that specified actor
	    if (pAfterThis && aIt == m_Actors.end())
	    {
	        for (aIt = m_Actors.begin(); aIt != m_Actors.end() && (*aIt)->GetTeam() != team; ++aIt)
	            ;

	        // Still nothing?? Should at least get the specified actor and return it! - EDIT No becuase it just may not be there!
	//        RTEAssert(aIt != m_Actors.end(), "Search for something after specified actor, and didn't even find the specified actor!?");
	    }

	    // Still nothing, so return nothing
	    if (aIt == m_Actors.end())
	        return 0;

	    if ((*aIt)->GetTeam() == team)
	        return *aIt;

	    return 0;
	*/
	// First sort the roster
	m_ActorRoster[team].sort(MOXPosComparison());

	// Begin at the beginning
	std::list<Actor*>::const_iterator aIt = m_ActorRoster[team].begin();

	// Search for the actor to start search from, if specified
	if (pAfterThis) {
		// Make the iterator point to the specified starting point actor
		for (; aIt != m_ActorRoster[team].end() && *aIt != pAfterThis; ++aIt)
			;

		// If we couldn't find the one to search for, then just return the first one
		if (aIt == m_ActorRoster[team].end())
			aIt = m_ActorRoster[team].begin();
		// Go one more step so we're not pointing at the one we're not supposed to get
		else {
			++aIt;
			// If that was the last one, then return the first in the list
			if (aIt == m_ActorRoster[team].end())
				aIt = m_ActorRoster[team].begin();
		}
	}

	RTEAssert((*aIt)->GetTeam() == team, "Actor of wrong team found in the wrong roster!");
	return *aIt;
}

Actor* MovableMan::GetPrevTeamActor(int team, Actor* pBeforeThis) {
	if (team < Activity::TeamOne || team >= Activity::MaxTeamCount || m_Actors.empty() || m_ActorRoster[team].empty())
		return 0;
	/* Obsolete, now uses team rosters which are sorted
	    // Begin at the reverse beginning
	    std::deque<Actor *>::const_reverse_iterator aIt = m_Actors.rbegin();

	    // Search for the actor to start search from, if specified
	    if (pBeforeThis)
	    {
	        // Make the iterator point to the specified starting point actor
	        for (; aIt != m_Actors.rend() && !((*aIt)->GetTeam() == team && *aIt == pBeforeThis); ++aIt)
	            ;

	        // If we couldn't find the one to search for,
	        // then just start at the beginning again and get the first actor at the next step
	        if (aIt == m_Actors.rend())
	            aIt = m_Actors.rbegin();
	        // Go one more step so we're not pointing at the one we're not supposed to get
	        else
	            ++aIt;
	    }

	    // Now search for the first actor of the team from the search point (beginning or otherwise)
	    for (; aIt != m_Actors.rend() && (*aIt)->GetTeam() != team; ++aIt)
	        ;

	    // If nothing found between a specified actor and the end,
	    // then restart and see if there's anything between beginning and that specified actor
	    if (pBeforeThis && aIt == m_Actors.rend())
	    {
	        for (aIt = m_Actors.rbegin(); aIt != m_Actors.rend() && (*aIt)->GetTeam() != team; ++aIt)
	            ;

	        // Still nothing?? Should at least get the specified actor and return it! - EDIT No becuase it just may not be there!
	//        RTEAssert(aIt != m_Actors.rend(), "Search for something after specified actor, and didn't even find the specified actor!?");
	    }

	    // Still nothing, so return nothing
	    if (aIt == m_Actors.rend())
	        return 0;

	    if ((*aIt)->GetTeam() == team)
	        return *aIt;

	    return 0;
	*/
	// First sort the roster
	m_ActorRoster[team].sort(MOXPosComparison());

	// Begin at the reverse beginning of roster
	std::list<Actor*>::reverse_iterator aIt = m_ActorRoster[team].rbegin();

	// Search for the actor to start search from, if specified
	if (pBeforeThis) {
		// Make the iterator point to the specified starting point actor
		for (; aIt != m_ActorRoster[team].rend() && *aIt != pBeforeThis; ++aIt)
			;

		// If we couldn't find the one to search for, then just return the one at the end
		if (aIt == m_ActorRoster[team].rend())
			aIt = m_ActorRoster[team].rbegin();
		// Go one more step so we're not pointing at the one we're not supposed to get
		else {
			++aIt;
			// If that was the first one, then return the last in the list
			if (aIt == m_ActorRoster[team].rend())
				aIt = m_ActorRoster[team].rbegin();
		}
	}

	RTEAssert((*aIt)->GetTeam() == team, "Actor of wrong team found in the wrong roster!");
	return *aIt;
}

Actor* MovableMan::GetClosestTeamActor(int team, int player, const Vector& scenePoint, int maxRadius, Vector& getDistance, bool onlyPlayerControllableActors, const Actor* excludeThis) {
	if (team < Activity::NoTeam || team >= Activity::MaxTeamCount || m_Actors.empty() || m_ActorRoster[team].empty())
		return 0;

	Activity* pActivity = g_ActivityMan.GetActivity();

	float sqrShortestDistance = static_cast<float>(maxRadius * maxRadius);
	Actor* pClosestActor = 0;

	// If we're looking for a noteam actor, then go through the entire actor list instead
	if (team == Activity::NoTeam) {
		for (std::deque<Actor*>::iterator aIt = m_Actors.begin(); aIt != m_Actors.end(); ++aIt) {
			if ((*aIt) == excludeThis || (*aIt)->GetTeam() != Activity::NoTeam || (onlyPlayerControllableActors && !(*aIt)->IsPlayerControllable())) {
				continue;
			}

			// Check if even within search radius
			float sqrDistance = g_SceneMan.ShortestDistance((*aIt)->GetPos(), scenePoint, g_SceneMan.SceneWrapsX() || g_SceneMan.SceneWrapsY()).GetSqrMagnitude();
			if (sqrDistance < sqrShortestDistance) {
				sqrShortestDistance = sqrDistance;
				pClosestActor = *aIt;
			}
		}
	}
	// A specific team, so use the rosters instead
	else {
		for (std::list<Actor*>::iterator aIt = m_ActorRoster[team].begin(); aIt != m_ActorRoster[team].end(); ++aIt) {
			if ((*aIt) == excludeThis || (onlyPlayerControllableActors && !(*aIt)->IsPlayerControllable()) || (player != NoPlayer && ((*aIt)->GetController()->IsPlayerControlled(player) || (pActivity && pActivity->IsOtherPlayerBrain(*aIt, player))))) {
				continue;
			}

			Vector distanceVec = g_SceneMan.ShortestDistance((*aIt)->GetPos(), scenePoint, g_SceneMan.SceneWrapsX() || g_SceneMan.SceneWrapsY());

			// Check if even within search radius
			float sqrDistance = distanceVec.GetSqrMagnitude();
			if (sqrDistance < sqrShortestDistance) {
				sqrShortestDistance = sqrDistance;
				pClosestActor = *aIt;
				getDistance.SetXY(distanceVec.GetX(), distanceVec.GetY());
			}
		}
	}

	return pClosestActor;
}

Actor* MovableMan::GetClosestEnemyActor(int team, const Vector& scenePoint, int maxRadius, Vector& getDistance) {
	if (team < Activity::NoTeam || team >= Activity::MaxTeamCount || m_Actors.empty())
		return 0;

	Activity* pActivity = g_ActivityMan.GetActivity();

	float sqrShortestDistance = static_cast<float>(maxRadius * maxRadius);
	Actor* pClosestActor = 0;

	for (std::deque<Actor*>::iterator aIt = m_Actors.begin(); aIt != m_Actors.end(); ++aIt) {
		if ((*aIt)->GetTeam() == team)
			continue;

		Vector distanceVec = g_SceneMan.ShortestDistance((*aIt)->GetPos(), scenePoint, g_SceneMan.SceneWrapsX() || g_SceneMan.SceneWrapsY());

		// Check if even within search radius
		float sqrDistance = distanceVec.GetSqrMagnitude();
		if (sqrDistance < sqrShortestDistance) {
			sqrShortestDistance = sqrDistance;
			pClosestActor = *aIt;
			getDistance.SetXY(distanceVec.GetX(), distanceVec.GetY());
		}
	}

	return pClosestActor;
}

Actor* MovableMan::GetClosestActor(const Vector& scenePoint, int maxRadius, Vector& getDistance, const Actor* pExcludeThis) {
	if (m_Actors.empty())
		return 0;

	Activity* pActivity = g_ActivityMan.GetActivity();

	float sqrShortestDistance = static_cast<float>(maxRadius * maxRadius);
	Actor* pClosestActor = 0;

	for (std::deque<Actor*>::iterator aIt = m_Actors.begin(); aIt != m_Actors.end(); ++aIt) {
		if ((*aIt) == pExcludeThis)
			continue;

		Vector distanceVec = g_SceneMan.ShortestDistance((*aIt)->GetPos(), scenePoint, g_SceneMan.SceneWrapsX() || g_SceneMan.SceneWrapsY());

		// Check if even within search radius
		float sqrDistance = distanceVec.GetSqrMagnitude();
		if (sqrDistance < sqrShortestDistance) {
			sqrShortestDistance = sqrDistance;
			pClosestActor = *aIt;
			getDistance.SetXY(distanceVec.GetX(), distanceVec.GetY());
		}
	}

	return pClosestActor;
}

Actor* MovableMan::GetClosestBrainActor(int team, const Vector& scenePoint) const {
	if (team < Activity::TeamOne || team >= Activity::MaxTeamCount || m_ActorRoster[team].empty())
		return 0;

	float sqrShortestDistance = std::numeric_limits<float>::infinity();
	sqrShortestDistance *= sqrShortestDistance;

	Actor* pClosestBrain = 0;

	for (std::list<Actor*>::const_iterator aIt = m_ActorRoster[team].begin(); aIt != m_ActorRoster[team].end(); ++aIt) {
		if (!(*aIt)->HasObjectInGroup("Brains"))
			continue;

		// Check if closer than best so far
		float sqrDistance = g_SceneMan.ShortestDistance((*aIt)->GetPos(), scenePoint, g_SceneMan.SceneWrapsX() || g_SceneMan.SceneWrapsY()).GetSqrMagnitude();
		if (sqrDistance < sqrShortestDistance) {
			sqrShortestDistance = sqrDistance;
			pClosestBrain = *aIt;
		}
	}

	return pClosestBrain;
}

Actor* MovableMan::GetClosestOtherBrainActor(int notOfTeam, const Vector& scenePoint) const {
	if (notOfTeam < Activity::TeamOne || notOfTeam >= Activity::MaxTeamCount || m_Actors.empty())
		return 0;

	float sqrShortestDistance = std::numeric_limits<float>::infinity();
	sqrShortestDistance *= sqrShortestDistance;

	Actor* pClosestBrain = 0;
	Actor* pContenderBrain = 0;

	for (int t = Activity::TeamOne; t < g_ActivityMan.GetActivity()->GetTeamCount(); ++t) {
		if (t != notOfTeam) {
			pContenderBrain = GetClosestBrainActor(t, scenePoint);
			float sqrDistance = (pContenderBrain->GetPos() - scenePoint).GetSqrMagnitude();
			if (sqrDistance < sqrShortestDistance) {
				sqrShortestDistance = sqrDistance;
				pClosestBrain = pContenderBrain;
			}
		}
	}
	return pClosestBrain;
}

Actor* MovableMan::GetUnassignedBrain(int team) const {
	if (/*m_Actors.empty() || */ m_ActorRoster[team].empty())
		return 0;

	for (std::list<Actor*>::const_iterator aIt = m_ActorRoster[team].begin(); aIt != m_ActorRoster[team].end(); ++aIt) {
		if ((*aIt)->HasObjectInGroup("Brains") && !g_ActivityMan.GetActivity()->IsAssignedBrain(*aIt))
			return *aIt;
	}

	// Also need to look through all the actors added this frame, one might be a brain.
	int actorTeam = Activity::NoTeam;
	for (std::deque<Actor*>::const_iterator aaIt = m_AddedActors.begin(); aaIt != m_AddedActors.end(); ++aaIt) {
		int actorTeam = (*aaIt)->GetTeam();
		// Accept no-team brains too - ACTUALLY, DON'T
		if ((actorTeam == team /* || actorTeam == Activity::NoTeam*/) && (*aaIt)->HasObjectInGroup("Brains") && !g_ActivityMan.GetActivity()->IsAssignedBrain(*aaIt))
			return *aaIt;
	}

	return 0;
}

bool MovableMan::AddMO(MovableObject* movableObjectToAdd) {
	if (!movableObjectToAdd) {
		return false;
	}

	if (Actor* actorToAdd = dynamic_cast<Actor*>(movableObjectToAdd)) {
		AddActor(actorToAdd);
		return true;
	} else if (HeldDevice* heldDeviceToAdd = dynamic_cast<HeldDevice*>(movableObjectToAdd)) {
		AddItem(heldDeviceToAdd);
		return true;
	}
	AddParticle(movableObjectToAdd);

	return true;
}

void MovableMan::ReapplyPersistedControllerModes() {
	for (Actor* actor: m_Actors) {
		actor->ApplyPersistedControllerMode();
	}
}

void MovableMan::AddActor(Actor* actorToAdd) {
	if (actorToAdd && g_ActivityMan.GetActivity()) {
		actorToAdd->SetAsAddedToMovableMan();
		actorToAdd->CorrectAttachableAndWoundPositionsAndRotations();

		if (m_RestoringSnapshot) {
			// A snapshot resident enters exactly as captured.
			actorToAdd->AdoptPersistedUniqueID();
		} else {
			// A normal add spawn-normalizes; drop pending snapshot stashes so later saves read live state.
			actorToAdd->DiscardPersistedSnapshotState();
			if (actorToAdd->IsTooFast()) {
				actorToAdd->SetToDelete(true);
			} else {
				if (!dynamic_cast<ADoor*>(actorToAdd)) {
					actorToAdd->MoveOutOfTerrain(g_MaterialGrass);
				}
				if (actorToAdd->IsStatus(Actor::INACTIVE)) {
					actorToAdd->SetStatus(Actor::STABLE);
				}
				actorToAdd->NotResting();
				actorToAdd->NewFrame();
				actorToAdd->SetAge(0);
			}
		}

		{
			std::lock_guard<std::mutex> lock(m_AddedActorsMutex);
			m_AddedActors.push_back(actorToAdd);
			m_ValidActors.insert(actorToAdd);

			// A joiner's per-machine controller must not drive sim effects on its join tick; the
			// wire takes over from the next tick's controller update.
			if (!m_RestoringSnapshot && ScenarioRunner::IsLockstepControllerSyncActive()) {
				actorToAdd->GetController()->SetDisabled(true);
				m_LockstepJoinQuarantine.emplace_back(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()), actorToAdd->GetUniqueID());
			}

			// This will call SetTeam and subsequently force the team as active.
			AddActorToTeamRoster(actorToAdd);
		}
	}
}

void MovableMan::AddItem(HeldDevice* itemToAdd) {
	if (itemToAdd && g_ActivityMan.GetActivity()) {
		g_ActivityMan.GetActivity()->ForceSetTeamAsActive(itemToAdd->GetTeam());

		itemToAdd->SetAsAddedToMovableMan();
		itemToAdd->CorrectAttachableAndWoundPositionsAndRotations();

		if (m_RestoringSnapshot) {
			itemToAdd->AdoptPersistedUniqueID();
		} else {
			itemToAdd->DiscardPersistedSnapshotState();
			if (itemToAdd->IsTooFast()) {
				itemToAdd->SetToDelete(true);
			} else {
				if (!itemToAdd->IsSetToDelete()) {
					itemToAdd->MoveOutOfTerrain(g_MaterialGrass);
				}
				itemToAdd->NotResting();
				itemToAdd->NewFrame();
				itemToAdd->SetAge(0);
			}
		}

		std::lock_guard<std::mutex> lock(m_AddedItemsMutex);
		m_AddedItems.push_back(itemToAdd);
		m_ValidItems.insert(itemToAdd);
	}
}

void MovableMan::AddParticle(MovableObject* particleToAdd) {
	if (particleToAdd && g_ActivityMan.GetActivity()) {
		SceneMan::TraceTerrainEvent("spwn", std::bit_cast<int32_t>(particleToAdd->GetPos().m_X), std::bit_cast<int32_t>(particleToAdd->GetPos().m_Y), static_cast<int>(particleToAdd->GetUniqueID()), static_cast<int>(SceneMan::GetTerrainEventContext()), std::bit_cast<int32_t>(particleToAdd->GetVel().m_X));
		g_ActivityMan.GetActivity()->ForceSetTeamAsActive(particleToAdd->GetTeam());

		particleToAdd->SetAsAddedToMovableMan();
		if (MOSRotating* particleToAddAsMOSRotating = dynamic_cast<MOSRotating*>(particleToAdd)) {
			particleToAddAsMOSRotating->CorrectAttachableAndWoundPositionsAndRotations();
		}

		if (m_RestoringSnapshot) {
			particleToAdd->AdoptPersistedUniqueID();
		} else {
			particleToAdd->DiscardPersistedSnapshotState();
			if (particleToAdd->IsTooFast()) {
				particleToAdd->SetToDelete(true);
			} else {
				// TODO consider moving particles out of grass. It's old code that was removed because it's slow to do this for every particle.
				particleToAdd->NotResting();
				particleToAdd->NewFrame();
				particleToAdd->SetAge(0);
			}
		}
		if (particleToAdd->IsDevice()) {
			std::lock_guard<std::mutex> lock(m_AddedItemsMutex);
			m_AddedItems.push_back(particleToAdd);
			m_ValidItems.insert(particleToAdd);
		} else {
			std::lock_guard<std::mutex> lock(m_AddedParticlesMutex);
			m_AddedParticles.push_back(particleToAdd);
			m_ValidParticles.insert(particleToAdd);
		}
	}
}

Actor* MovableMan::RemoveActor(MovableObject* pActorToRem) {
	Actor* removed = nullptr;

	if (pActorToRem && RefuseSpeculativeRemoval(pActorToRem, "actor")) {
		return nullptr;
	}
	if (pActorToRem) {
		for (std::deque<Actor*>::iterator itr = m_Actors.begin(); itr != m_Actors.end(); ++itr) {
			if (*itr == pActorToRem) {
				std::lock_guard<std::mutex> lock(m_ActorsMutex);
				removed = *itr;
				m_ValidActors.erase(*itr);
				m_Actors.erase(itr);
				break;
			}
		}
		// Try the newly added actors if we couldn't find it in the regular deque
		if (!removed) {
			for (std::deque<Actor*>::iterator itr = m_AddedActors.begin(); itr != m_AddedActors.end(); ++itr) {
				if (*itr == pActorToRem) {
					std::lock_guard<std::mutex> lock(m_AddedActorsMutex);
					removed = *itr;
					m_ValidActors.erase(*itr);
					m_AddedActors.erase(itr);
					break;
				}
			}
		}
		RemoveActorFromTeamRoster(dynamic_cast<Actor*>(pActorToRem));
		pActorToRem->SetAsAddedToMovableMan(false);
	}
	return removed;
}

MovableObject* MovableMan::RemoveItem(MovableObject* pItemToRem) {
	MovableObject* removed = nullptr;

	if (pItemToRem && RefuseSpeculativeRemoval(pItemToRem, "item")) {
		return nullptr;
	}
	if (pItemToRem) {
		for (std::deque<MovableObject*>::iterator itr = m_Items.begin(); itr != m_Items.end(); ++itr) {
			if (*itr == pItemToRem) {
				std::lock_guard<std::mutex> lock(m_ItemsMutex);
				removed = *itr;
				m_ValidItems.erase(*itr);
				m_Items.erase(itr);
				break;
			}
		}
		// Try the newly added items if we couldn't find it in the regular deque
		if (!removed) {
			for (std::deque<MovableObject*>::iterator itr = m_AddedItems.begin(); itr != m_AddedItems.end(); ++itr) {
				if (*itr == pItemToRem) {
					std::lock_guard<std::mutex> lock(m_AddedItemsMutex);
					removed = *itr;
					m_ValidItems.erase(*itr);
					m_AddedItems.erase(itr);
					break;
				}
			}
		}
		pItemToRem->SetAsAddedToMovableMan(false);
	}
	return removed;
}

MovableObject* MovableMan::RemoveParticle(MovableObject* pMOToRem) {
	MovableObject* removed = nullptr;

	if (pMOToRem && RefuseSpeculativeRemoval(pMOToRem, "particle")) {
		return nullptr;
	}
	if (pMOToRem) {
		for (std::deque<MovableObject*>::iterator itr = m_Particles.begin(); itr != m_Particles.end(); ++itr) {
			if (*itr == pMOToRem) {
				std::lock_guard<std::mutex> lock(m_ParticlesMutex);
				removed = *itr;
				m_ValidParticles.erase(*itr);
				m_Particles.erase(itr);
				break;
			}
		}
		// Try the newly added particles if we couldn't find it in the regular deque
		if (!removed) {
			for (std::deque<MovableObject*>::iterator itr = m_AddedParticles.begin(); itr != m_AddedParticles.end(); ++itr) {
				if (*itr == pMOToRem) {
					std::lock_guard<std::mutex> lock(m_AddedParticlesMutex);
					removed = *itr;
					m_ValidParticles.erase(*itr);
					m_AddedParticles.erase(itr);
					break;
				}
			}
		}
		pMOToRem->SetAsAddedToMovableMan(false);
	}
	return removed;
}

void MovableMan::AddActorToTeamRoster(Actor* pActorToAdd) {
	if (!pActorToAdd) {
		return;
	}

	// Add to the team roster and then sort it too
	int team = pActorToAdd->GetTeam();
	// Also re-set the TEam so that the Team Icons get set up properly
	pActorToAdd->SetTeam(team);
	// Only add to a roster if it's on a team AND is controllable (eg doors are not)
	if (team >= Activity::TeamOne && team < Activity::MaxTeamCount && pActorToAdd->IsControllable()) {
		std::lock_guard<std::mutex> lock(m_ActorRosterMutex);
		m_ActorRoster[pActorToAdd->GetTeam()].push_back(pActorToAdd);
		m_ActorRoster[pActorToAdd->GetTeam()].sort(MOXPosComparison());
	}
}

void MovableMan::RemoveActorFromTeamRoster(Actor* pActorToRem) {
	if (!pActorToRem) {
		return;
	}

	int team = pActorToRem->GetTeam();

	// Remove from roster as well
	if (team >= Activity::TeamOne && team < Activity::MaxTeamCount) {
		std::lock_guard<std::mutex> lock(m_ActorRosterMutex);
		m_ActorRoster[team].remove(pActorToRem);
	}
}

void MovableMan::ChangeActorTeam(Actor* pActor, int team) {
	if (!pActor) {
		return;
	}

	if (pActor->IsPlayerControlled()) {
		g_ActivityMan.GetActivity()->LoseControlOfActor(pActor->GetController()->GetPlayer());
	}

	RemoveActorFromTeamRoster(pActor);
	pActor->SetTeam(team);
	AddActorToTeamRoster(pActor);

	// Because doors affect the team-based pathfinders, we need to tell them there's been a change.
	// This is hackily done by erasing the door material, updating the pathfinders, then redrawing it and updating them again so they properly account for the door's new team.
	if (ADoor* actorAsADoor = dynamic_cast<ADoor*>(pActor); actorAsADoor && actorAsADoor->GetDoorMaterialDrawn()) {
		actorAsADoor->TempEraseOrRedrawDoorMaterial(true);
		g_SceneMan.GetTerrain()->AddUpdatedMaterialArea(actorAsADoor->GetBoundingBox());
		g_SceneMan.GetScene()->UpdatePathFinding();
		actorAsADoor->TempEraseOrRedrawDoorMaterial(false);
		g_SceneMan.GetTerrain()->AddUpdatedMaterialArea(actorAsADoor->GetBoundingBox());
		g_SceneMan.GetScene()->UpdatePathFinding();
	}
}

bool MovableMan::ValidateMOIDs() {
#ifdef DEBUG_BUILD
	for (const MovableObject* mo: m_MOIDIndex) {
		RTEAssert(mo, "Null MO found!");
	}
#endif
	return true;
}

bool MovableMan::ValidMO(const MovableObject* pMOToCheck) {
	bool exists = m_ValidActors.find(pMOToCheck) != m_ValidActors.end() ||
	              m_ValidItems.find(pMOToCheck) != m_ValidItems.end() ||
	              m_ValidParticles.find(pMOToCheck) != m_ValidParticles.end();

	return pMOToCheck && exists;
}

bool MovableMan::IsActor(const MovableObject* pMOToCheck) {
	return pMOToCheck && m_ValidActors.find(pMOToCheck) != m_ValidActors.end();
}

bool MovableMan::IsDevice(const MovableObject* pMOToCheck) {
	return pMOToCheck && m_ValidItems.find(pMOToCheck) != m_ValidItems.end();
}

bool MovableMan::IsParticle(const MovableObject* pMOToCheck) {
	return pMOToCheck && m_ValidParticles.find(pMOToCheck) != m_ValidParticles.end();
}

bool MovableMan::IsOfActor(MOID checkMOID) {
	if (checkMOID == g_NoMOID)
		return false;

	bool found = false;
	MovableObject* pMO = GetMOFromID(checkMOID);

	if (pMO) {
		MOID rootMOID = pMO->GetRootID();
		if (checkMOID != g_NoMOID) {
			for (std::deque<Actor*>::iterator itr = m_Actors.begin(); !found && itr != m_Actors.end(); ++itr) {
				if ((*itr)->GetID() == checkMOID || (*itr)->GetID() == rootMOID) {
					found = true;
					break;
				}
			}
			// Check actors just added this frame
			if (!found) {
				for (std::deque<Actor*>::iterator itr = m_AddedActors.begin(); !found && itr != m_AddedActors.end(); ++itr) {
					if ((*itr)->GetID() == checkMOID || (*itr)->GetID() == rootMOID) {
						found = true;
						break;
					}
				}
			}
		}
	}
	return found;
}

int MovableMan::GetContiguousActorID(const Actor* actor) const {
	auto itr = m_ContiguousActorIDs.find(actor);
	if (itr == m_ContiguousActorIDs.end()) {
		return -1;
	}

	return itr->second;
}

MOID MovableMan::GetRootMOID(MOID checkMOID) {
	MovableObject* pMO = GetMOFromID(checkMOID);
	if (pMO)
		return pMO->GetRootID();

	return g_NoMOID;
}

bool MovableMan::RemoveMO(MovableObject* pMOToRem) {
	if (pMOToRem) {
		if (RemoveActor(pMOToRem))
			return true;
		if (RemoveItem(pMOToRem))
			return true;
		if (RemoveParticle(pMOToRem))
			return true;
	}

	return false;
}

int MovableMan::KillAllTeamActors(int teamToKill) const {
	int killCount = 0;

	for (std::deque<Actor*> actorList: {m_Actors, m_AddedActors}) {
		for (Actor* actor: actorList) {
			if (actor->GetTeam() == teamToKill) {
				const AHuman* actorAsHuman = dynamic_cast<AHuman*>(actor);
				if (actorAsHuman && actorAsHuman->GetHead()) {
					actorAsHuman->GetHead()->GibThis();
				} else {
					actor->GibThis();
				}
				killCount++;
			}
		}
	}

	return killCount;
}

int MovableMan::KillAllEnemyActors(int teamNotToKill) const {
	static const bool s_gibLogArmed = std::getenv("CC_SIM_DUMP") != nullptr;
	if (s_gibLogArmed) {
		std::cout << "[gib-cause] killall sparing team " << teamNotToKill << " at tick " << g_TimerMan.GetSimUpdateCount() << std::endl;
	}
	int killCount = 0;

	for (std::deque<Actor*> actorList: {m_Actors, m_AddedActors}) {
		for (Actor* actor: actorList) {
			if (actor->GetTeam() != teamNotToKill) {
				const AHuman* actorAsHuman = dynamic_cast<AHuman*>(actor);
				if (actorAsHuman && actorAsHuman->GetHead()) {
					actorAsHuman->GetHead()->GibThis();
				} else {
					actor->GibThis();
				}
				killCount++;
			}
		}
	}

	return killCount;
}

int MovableMan::GetAllActors(bool transferOwnership, std::list<SceneObject*>& actorList, int onlyTeam, bool noBrains) {
	int addedCount = 0;

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
		Actor* actor = *aIt;
		// Only grab ones of a specific team; delete all others
		if ((onlyTeam == Activity::NoTeam || actor->GetTeam() == onlyTeam) && (!noBrains || !actor->HasObjectInGroup("Brains"))) {
			actorList.push_back(actor);
			addedCount++;
		} else if (transferOwnership) {
			delete actor;
		}
	}

	if (transferOwnership) {
		// Clear the internal Actor lists; we transferred the ownership of them
		m_Actors.clear();
		m_AddedActors.clear();
		m_ValidActors.clear();

		// Also clear the actor rosters
		for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
			m_ActorRoster[team].clear();
		}
	}

	return addedCount;
}

int MovableMan::GetAllItems(bool transferOwnership, std::list<SceneObject*>& itemList) {
	int addedCount = 0;

	// Add all regular Items
	for (std::deque<MovableObject*>::iterator iIt = m_Items.begin(); iIt != m_Items.end(); ++iIt) {
		itemList.push_back((*iIt));
		addedCount++;
	}

	// Add all Items added this frame
	for (std::deque<MovableObject*>::iterator iIt = m_AddedItems.begin(); iIt != m_AddedItems.end(); ++iIt) {
		itemList.push_back((*iIt));
		addedCount++;
	}

	if (transferOwnership) {
		// Clear the internal Item list; we transferred the ownership of them
		m_Items.clear();
		m_AddedItems.clear();
		m_ValidItems.clear();
	}

	return addedCount;
}

int MovableMan::GetAllParticles(bool transferOwnership, std::list<SceneObject*>& particleList) {
	int addedCount = 0;

	// Add all regular particles
	for (std::deque<MovableObject*>::iterator iIt = m_Particles.begin(); iIt != m_Particles.end(); ++iIt) {
		particleList.push_back((*iIt));
		addedCount++;
	}

	// Add all particles added this frame
	for (std::deque<MovableObject*>::iterator iIt = m_AddedParticles.begin(); iIt != m_AddedParticles.end(); ++iIt) {
		particleList.push_back((*iIt));
		addedCount++;
	}

	if (transferOwnership) {
		// Clear the internal Particle list; we transferred the ownership of them
		m_Particles.clear();
		m_AddedParticles.clear();
		m_ValidParticles.clear();
	}

	return addedCount;
}

int MovableMan::GetTeamMOIDCount(int team) const {
	if (team > Activity::NoTeam && team < Activity::MaxTeamCount)
		return m_TeamMOIDCount[team];
	else
		return 0;
}

void MovableMan::OpenAllDoors(bool open, int team) const {
	for (std::deque<Actor*> actorDeque: {m_Actors, m_AddedActors}) {
		for (Actor* actor: actorDeque) {
			if (ADoor* actorAsADoor = dynamic_cast<ADoor*>(actor); actorAsADoor && actorAsADoor->GetTeam() == team) {
				if (actorAsADoor->GetDoorState() != (open ? ADoor::DoorState::OPEN : ADoor::DoorState::CLOSED)) {
					actorAsADoor->Update();
					actorAsADoor->SetClosedByDefault(!open);
				}
				actorAsADoor->ResetSensorTimer();
				if (open) {
					actorAsADoor->OpenDoor();
				} else {
					actorAsADoor->CloseDoor();
				}
			}
		}
	}
}

// TODO: Completely tear out and delete this.
// It shouldn't belong to MovableMan, instead it probably ought to be on the pathfinder. On that note, pathfinders shouldn't be part of the scene!
// AIMan? PathingMan? Something like that. Ideally, we completely tear out this hack, and allow for doors in a completely different way.
void MovableMan::OverrideMaterialDoors(bool eraseDoorMaterial, int team) const {
	for (std::deque<Actor*> actorDeque: {m_Actors, m_AddedActors}) {
		for (Actor* actor: actorDeque) {
			if (ADoor* actorAsDoor = dynamic_cast<ADoor*>(actor); actorAsDoor && (team == Activity::NoTeam || actorAsDoor->GetTeam() == team)) {
				actorAsDoor->TempEraseOrRedrawDoorMaterial(eraseDoorMaterial);
			}
		}
	}
}

void MovableMan::RegisterAlarmEvent(const AlarmEvent& newEvent) {
	std::lock_guard<std::mutex> lock(m_AddedAlarmEventsMutex);
	m_AddedAlarmEvents.push_back(new AlarmEvent(newEvent));
}

void callLuaFunctionOnMORecursive(MovableObject* mo, const std::string& functionName, const std::vector<const Entity*>& functionEntityArguments, const std::vector<std::string_view>& functionLiteralArguments, const std::vector<LuabindObjectWrapper*>& functionObjectArguments) {
	if (MOSRotating* mosr = dynamic_cast<MOSRotating*>(mo)) {
		for (auto attachablrItr = mosr->GetAttachableList().begin(); attachablrItr != mosr->GetAttachableList().end();) {
			Attachable* attachable = *attachablrItr;
			++attachablrItr;

			attachable->RunScriptedFunctionInAppropriateScripts(functionName, false, false, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
			callLuaFunctionOnMORecursive(attachable, functionName, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
		}

		for (auto woundItr = mosr->GetWoundList().begin(); woundItr != mosr->GetWoundList().end();) {
			AEmitter* wound = *woundItr;
			++woundItr;

			wound->RunScriptedFunctionInAppropriateScripts(functionName, false, false, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
			callLuaFunctionOnMORecursive(wound, functionName, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
		}
	}

	mo->RunScriptedFunctionInAppropriateScripts(functionName, false, false, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
};

void MovableMan::RunLuaFunctionOnAllMOs(const std::string& functionName, bool includeAdded, const std::vector<const Entity*>& functionEntityArguments, const std::vector<std::string_view>& functionLiteralArguments, const std::vector<LuabindObjectWrapper*>& functionObjectArguments) {
	if (includeAdded) {
		for (Actor* actor: m_AddedActors) {
			callLuaFunctionOnMORecursive(actor, functionName, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
		}

		for (MovableObject* item: m_AddedItems) {
			callLuaFunctionOnMORecursive(item, functionName, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
		}

		for (MovableObject* particle: m_AddedParticles) {
			callLuaFunctionOnMORecursive(particle, functionName, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
		}
	}

	for (Actor* actor: m_Actors) {
		callLuaFunctionOnMORecursive(actor, functionName, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
	}

	for (MovableObject* item: m_Items) {
		callLuaFunctionOnMORecursive(item, functionName, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
	}

	for (MovableObject* particle: m_Particles) {
		callLuaFunctionOnMORecursive(particle, functionName, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
	}
}

void reloadLuaFunctionsOnMORecursive(MovableObject* mo) {
	if (MOSRotating* mosr = dynamic_cast<MOSRotating*>(mo)) {
		for (auto attachablrItr = mosr->GetAttachableList().begin(); attachablrItr != mosr->GetAttachableList().end();) {
			Attachable* attachable = *attachablrItr;
			++attachablrItr;

			attachable->ReloadScripts();
			reloadLuaFunctionsOnMORecursive(attachable);
		}

		for (auto woundItr = mosr->GetWoundList().begin(); woundItr != mosr->GetWoundList().end();) {
			AEmitter* wound = *woundItr;
			++woundItr;

			wound->ReloadScripts();
			reloadLuaFunctionsOnMORecursive(wound);
		}
	}

	mo->ReloadScripts();
};

void MovableMan::ReloadLuaScripts() {
	for (Actor* actor: m_AddedActors) {
		reloadLuaFunctionsOnMORecursive(actor);
	}

	for (MovableObject* item: m_AddedItems) {
		reloadLuaFunctionsOnMORecursive(item);
	}

	for (MovableObject* particle: m_AddedParticles) {
		reloadLuaFunctionsOnMORecursive(particle);
	}

	for (Actor* actor: m_Actors) {
		reloadLuaFunctionsOnMORecursive(actor);
	}

	for (MovableObject* item: m_Items) {
		reloadLuaFunctionsOnMORecursive(item);
	}

	for (MovableObject* particle: m_Particles) {
		reloadLuaFunctionsOnMORecursive(particle);
	}
}

// Phase-stamped pose/vel rows for CC_TRACK_UID MOs, to bisect which update phase forks first.
static void TraceTrackedPhase(const char* tag) {
	for (long uid: SceneMan::GetTrackedUIDs()) {
		const MovableObject* mo = g_MovableMan.FindObjectByUniqueID(uid);
		if (mo) {
			SceneMan::TraceTerrainEvent(tag, std::bit_cast<int32_t>(mo->GetPos().m_X), std::bit_cast<int32_t>(mo->GetPos().m_Y), std::bit_cast<int32_t>(mo->GetVel().m_X), std::bit_cast<int32_t>(mo->GetVel().m_Y), static_cast<int>(uid));
			if (const MOSprite* sprite = dynamic_cast<const MOSprite*>(mo)) {
				char rotTag[6];
				std::snprintf(rotTag, sizeof(rotTag), "%sr", tag);
				SceneMan::TraceTerrainEvent(rotTag, std::bit_cast<int32_t>(sprite->GetRotAngle()), std::bit_cast<int32_t>(sprite->GetAngularVel()), 0, 0, static_cast<int>(uid));
			}
			if (const MOSRotating* mosr = dynamic_cast<const MOSRotating*>(mo)) {
				if (const AtomGroup* group = const_cast<MOSRotating*>(mosr)->GetAtomGroup()) {
					uint64_t offsetHash = 1469598103934665603ULL;
					for (const Atom* atom: group->GetAtomList()) {
						offsetHash = (offsetHash ^ static_cast<uint32_t>(std::bit_cast<int32_t>(atom->GetOffset().m_X))) * 1099511628211ULL;
						offsetHash = (offsetHash ^ static_cast<uint32_t>(std::bit_cast<int32_t>(atom->GetOffset().m_Y))) * 1099511628211ULL;
					}
					char groupTag[6];
					std::snprintf(groupTag, sizeof(groupTag), "%sg", tag);
					SceneMan::TraceTerrainEvent(groupTag, static_cast<int32_t>(offsetHash & 0xFFFFFFFFu), static_cast<int32_t>(offsetHash >> 32), group->GetAtomCount(), 0, static_cast<int>(uid));
				}
			}
		}
	}
}

void MovableMan::AbsorbAddedMOs() {
	// Sort the added queues before the drain so the transfer order is stable across runs.
	std::sort(m_AddedActors.begin(), m_AddedActors.end(), MOUniqueIDLess());
	std::sort(m_AddedItems.begin(), m_AddedItems.end(), MOUniqueIDLess());
	std::sort(m_AddedParticles.begin(), m_AddedParticles.end(), MOUniqueIDLess());

	for (Actor* addedActor: m_AddedActors) {
		// Delete instead if it's marked for it
		if (!addedActor->IsSetToDelete()) {
			m_Actors.push_back(addedActor);
		} else {
			if (addedActor->GetTeam() >= 0) {
				RemoveActorFromTeamRoster(addedActor);
			}
			addedActor->DestroyScriptState();
			delete addedActor;
			m_ValidActors.erase(addedActor);
		}
	}
	m_AddedActors.clear();

	for (MovableObject* addedItem: m_AddedItems) {
		if (!addedItem->IsSetToDelete()) {
			m_Items.push_back(addedItem);
		} else {
			addedItem->DestroyScriptState();
			delete addedItem;
			m_ValidItems.erase(addedItem);
		}
	}
	m_AddedItems.clear();

	for (MovableObject* addedParticle: m_AddedParticles) {
		if (!addedParticle->IsSetToDelete()) {
			m_Particles.push_back(addedParticle);
		} else {
			addedParticle->DestroyScriptState();
			delete addedParticle;
			m_ValidParticles.erase(addedParticle);
		}
	}
	m_AddedParticles.clear();
}

void MovableMan::ClearLockstepJoinQuarantine() {
	m_LockstepJoinQuarantine.clear();
}

void MovableMan::Update() {
	ZoneScoped;

	// Don't update if paused
	if (g_ActivityMan.GetActivity() && g_ActivityMan.ActivityPaused()) {
		return;
	}

	m_SimUpdateFrameNumber++;

	// If this is the first sim update since a drawn one, then clear the post effects
	if (g_TimerMan.SimUpdatesSinceDrawn() == 0) {
		g_PostProcessMan.ClearScenePostEffects();
	}

	// Reset the draw HUD roster line settings
	for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
		m_SortTeamRoster[team] = false;
	}

	// Move all last frame's alarm events into the proper buffer, and clear out the new one to fill up with this frame's
	for (AlarmEvent* alarmEvent: m_AlarmEvents) {
		delete alarmEvent;
	}
	m_AlarmEvents.clear();
	for (std::vector<AlarmEvent*>::iterator aeItr = m_AddedAlarmEvents.begin(); aeItr != m_AddedAlarmEvents.end(); ++aeItr) {
		m_AlarmEvents.push_back(*aeItr);
	}
	m_AddedAlarmEvents.clear();

	// Lock iteration order before the sim passes so same-seed runs visit MOs identically.
	std::sort(m_Actors.begin(), m_Actors.end(), MOUniqueIDLess());
	std::sort(m_Items.begin(), m_Items.end(), MOUniqueIDLess());
	std::sort(m_Particles.begin(), m_Particles.end(), MOUniqueIDLess());

	// A stale travel context would mislabel this tick's pre-travel trace rows.
	SceneMan::SetTerrainEventContext(0);

	TraceTrackedPhase("phA");

	// Travel MOs
	Travel();

	SceneMan::SetTerrainEventContext(0);

	TraceTrackedPhase("phB");

	// If our debug settings switch is forcing all pathing requests to immediately complete, make sure they're done here
	if (g_SettingsMan.GetForceImmediatePathingRequestCompletion() && g_SceneMan.GetScene()) {
		g_SceneMan.GetScene()->BlockUntilAllPathingRequestsComplete();
	}

	// Finish our Seeing rays from last frame
	m_ActorsSeeFuture.wait();

	// Prior to controller/AI update, execute lua callbacks
	g_LuaMan.ExecuteLuaScriptCallbacks();

	// Updates everything needed prior to AI/user input being processed
	// Fugly hack to keep backwards compat with scripts that rely on weird frame-delay-ordering behaviours
	// TODO, cleanup the pre-controller update and post-controller updates to have some consistent logic of what goes where
	PreControllerUpdate();

	// Updates AI/user input
	UpdateControllers();
	if (ScenarioRunner::HasControllerReplayError()) {
		return;
	}

	TraceTrackedPhase("phC");

	// Will use some common iterators
	std::deque<Actor*>::iterator aIt;
	std::deque<Actor*>::iterator amidIt;
	std::deque<MovableObject*>::iterator iIt;
	std::deque<MovableObject*>::iterator imidIt;
	std::deque<MovableObject*>::iterator parIt;
	std::deque<MovableObject*>::iterator midIt;

	// Update all multithreaded scripts for all objects
	g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ScriptsUpdate);
	{
		ZoneScopedN("Multithreaded Scripts Update");

		const std::string threadedUpdate = "ThreadedUpdate"; // avoid string reconstruction

		g_LuaMan.SetThreadLuaStateOverride(&g_LuaMan.GetMasterScriptState());
		for (MovableObject* mo: SortedRegisteredMOs(g_LuaMan.GetMasterScriptState())) {
			if (ValidMO(mo->GetRootParent())) {
				mo->RunScriptedFunctionInAppropriateScripts(threadedUpdate, false, false, {}, {}, {});
			}
		}
		g_LuaMan.SetThreadLuaStateOverride(nullptr);

		LuaStatesArray& luaStates = g_LuaMan.GetThreadedScriptStates();
		// One Lua state per task; the assert below requires it regardless of pool size.
		g_ThreadMan.GetPriorityThreadPool().parallelize_loop(luaStates.size(),
		                                                     [&](int start, int end) {
			                                                     RTEAssert(start + 1 == end, "Threaded script state being updated across multiple threads!");
			                                                     LuaStateWrapper& luaState = luaStates[start];
			                                                     g_LuaMan.SetThreadLuaStateOverride(&luaState);

			                                                     for (MovableObject* mo: SortedRegisteredMOs(luaState)) {
				                                                     if (ValidMO(mo->GetRootParent())) {
					                                                     mo->RunScriptedFunctionInAppropriateScripts(threadedUpdate, false, false, {}, {}, {});
				                                                     }
			                                                     }

			                                                     g_LuaMan.SetThreadLuaStateOverride(nullptr);
		                                                     },
		                                                     luaStates.size())
		    .wait();
	}

	{
		ZoneScopedN("Multithreaded Scripts SyncedUpdate");

		// The serial, MOID-ordered channel for script-driven shared-state mutation;
		// scripts opt in via RequestSyncedUpdate. See Data/Modding/threaded-determinism.md.
		const std::string syncedUpdate = "SyncedUpdate"; // avoid string reconstruction

		g_LuaMan.SetThreadLuaStateOverride(&g_LuaMan.GetMasterScriptState());
		for (MovableObject* mo: SortedRegisteredMOs(g_LuaMan.GetMasterScriptState())) {
			if (ValidMO(mo->GetRootParent())) {
				mo->RunScriptedFunctionInAppropriateScripts(syncedUpdate, false, false, {}, {}, {});
			}
		}
		g_LuaMan.SetThreadLuaStateOverride(nullptr);

		for (LuaStateWrapper& luaState: g_LuaMan.GetThreadedScriptStates()) {
			g_LuaMan.SetThreadLuaStateOverride(&luaState);

			for (MovableObject* mo: SortedRegisteredMOs(luaState)) {
				if (mo->HasRequestedSyncedUpdate()) {
					mo->RunScriptedFunctionInAppropriateScripts(syncedUpdate, false, false, {}, {}, {});
					mo->ResetRequestedSyncedUpdateFlag();
				}
			}

			g_LuaMan.SetThreadLuaStateOverride(nullptr);
		}
	}
	g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ScriptsUpdate);

	{
		{
			ZoneScopedN("Actors Update");

			g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ActorsUpdate);
			for (Actor* actor: m_Actors) {
				UpdateStage(actor);
			}
			g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ActorsUpdate);
		}

		{
			ZoneScopedN("Items Update");

			int count = 0;
			int itemLimit = m_Items.size() - m_MaxDroppedItems;
			for (iIt = m_Items.begin(); iIt != m_Items.end(); ++iIt, ++count) {
				UpdateStage(*iIt);
				if (count <= itemLimit) {
					(*iIt)->SetToSettle(true);
				}
			}
		}

		{
			ZoneScopedN("Particles Update");

			g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ParticlesUpdate);
			for (MovableObject* particle: m_Particles) {
				UpdateStage(particle);
				particle->RestDetection();
				// Copy particles that are at rest to the terrain and mark them for deletion.
				if (particle->IsAtRest()) {
					particle->SetToSettle(true);
				}
			}
			g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ParticlesUpdate);
		}

		{
			ZoneScopedN("Post Update");

			for (Actor* actor: m_Actors) {
				PostUpdateStage(actor);
			}

			for (MovableObject* item: m_Items) {
				PostUpdateStage(item);
			}

			for (MovableObject* particle: m_Particles) {
				PostUpdateStage(particle);
			}
		}

		TraceTrackedPhase("phD");
	} // namespace RTE

	//////////////////////////////////////////////////////////////////////
	// TRANSFER ALL MOs ADDED THIS FRAME
	// All Actors, Items, and Particles added this frame now are officially added

	{
		ZoneScopedN("MO Transfer and Deletion");

		AbsorbAddedMOs();

		////////////////////////////////////////////////////////////////////////////
		// Copy (Settle) Pass

		{
			// DEATH //////////////////////////////////////////////////////////
			// Transfer dead actors from Actor list to particle list
			aIt = partition(m_Actors.begin(), m_Actors.end(), std::not_fn(std::mem_fn(&Actor::IsDead)));
			amidIt = aIt;

			// Move dead Actor to particles list
			if (amidIt != m_Actors.end() /* && m_Actors.size() > 1*/) {
				while (aIt != m_Actors.end()) {
					// Report the death of the actor to the game
					g_ActivityMan.GetActivity()->ReportDeath((*aIt)->GetTeam());

					// The corpse leaves the controller wire here, so its per-machine controller must
					// stop driving sim effects like the jetpack.
					if (ScenarioRunner::IsLockstepControllerSyncActive()) {
						(*aIt)->GetController()->SetDisabled(true);
					}

					// Add to the particles list
					m_Particles.push_back(*aIt);
					m_ValidParticles.insert(*aIt);
					// Remove from the team roster

					if ((*aIt)->GetTeam() >= 0) {
						// m_ActorRoster[(*aIt)->GetTeam()].remove(*aIt);
						RemoveActorFromTeamRoster(*aIt);
					}

					m_ValidActors.erase(*aIt);
					aIt++;
				}
				// Try to set the existing iterator to a safer value, erase can crash in debug mode otherwise?
				aIt = m_Actors.begin();
				m_Actors.erase(amidIt, m_Actors.end());
			}

			// ITEM SETTLE //////////////////////////////////////////////////////////
			// Transfer excess items to particle list - use stable partition, item orde is important
			iIt = stable_partition(m_Items.begin(), m_Items.end(), std::not_fn(std::mem_fn(&MovableObject::ToSettle)));
			imidIt = iIt;

			// Move force-settled items to particles list
			if (imidIt != m_Items.end() /* && m_Items.size() > 1*/) {
				while (iIt != m_Items.end()) {
					(*iIt)->SetToSettle(false);
					// Disable TDExplosive's immunity to settling
					if ((*iIt)->GetRestThreshold() < 0) {
						(*iIt)->SetRestThreshold(500);
					}
					m_Particles.push_back(*iIt);
					m_ValidItems.erase(*iIt);
					iIt++;
				}
				m_Items.erase(imidIt, m_Items.end());
			}

			// DELETE //////////////////////////////////////////////////////////
			// Only delete after all travels & updates are done
			// Actors
			aIt = partition(m_Actors.begin(), m_Actors.end(), std::not_fn(std::mem_fn(&MovableObject::ToDelete)));
			amidIt = aIt;

			while (aIt != m_Actors.end()) {
				// Set brain to 0 to avoid crashes due to brain deletion
				Activity* pActivity = g_ActivityMan.GetActivity();
				if (pActivity) {
					if (pActivity->IsAssignedBrain(*aIt))
						pActivity->SetPlayerBrain(0, pActivity->IsBrainOfWhichPlayer(*aIt));

					pActivity->ReportDeath((*aIt)->GetTeam());
				}

				// Remove from team rosters
				if ((*aIt)->GetTeam() >= Activity::TeamOne && (*aIt)->GetTeam() < Activity::MaxTeamCount)
					// m_ActorRoster[(*aIt)->GetTeam()].remove(*aIt);
					RemoveActorFromTeamRoster(*aIt);

				// Delete
				(*aIt)->DestroyScriptState();
				delete (*aIt);
				m_ValidActors.erase(*aIt);
				aIt++;
			}
			// Try to set the existing iterator to a safer value, erase can crash in debug mode otherwise?
			aIt = m_Actors.begin();
			m_Actors.erase(amidIt, m_Actors.end());

			// Items
			iIt = stable_partition(m_Items.begin(), m_Items.end(), std::not_fn(std::mem_fn(&MovableObject::ToDelete)));
			imidIt = iIt;

			while (iIt != m_Items.end()) {
				(*iIt)->DestroyScriptState();
				delete (*iIt);
				m_ValidItems.erase(*iIt);
				iIt++;
			}
			m_Items.erase(imidIt, m_Items.end());

			// Particles
			parIt = partition(m_Particles.begin(), m_Particles.end(), std::not_fn(std::mem_fn(&MovableObject::ToDelete)));
			midIt = parIt;

			while (parIt != m_Particles.end()) {
				(*parIt)->DestroyScriptState();
				delete (*parIt);
				m_ValidParticles.erase(*parIt);
				parIt++;
			}
			m_Particles.erase(midIt, m_Particles.end());
		}

		// SETTLE PARTICLES //////////////////////////////////////////////////
		// Only settle after all updates and deletions are done
		if (m_SettlingEnabled) {
			parIt = partition(m_Particles.begin(), m_Particles.end(), std::not_fn(std::mem_fn(&MovableObject::ToSettle)));
			midIt = parIt;

			while (parIt != m_Particles.end()) {
				Vector parPos((*parIt)->GetPos());
				Material const* terrMat = g_SceneMan.GetMaterialFromID(g_SceneMan.GetTerrain()->GetMaterialPixel(parPos.GetFloorIntX(), parPos.GetFloorIntY()));
				int piling = (*parIt)->GetMaterial()->GetPiling();
				if (piling > 0) {
					for (int s = 0; s < piling && (terrMat->GetIndex() == (*parIt)->GetMaterial()->GetIndex() || terrMat->GetIndex() == (*parIt)->GetMaterial()->GetSettleMaterial()); ++s) {
						if ((piling - s) % 2 == 0) {
							parPos.m_Y -= 1.0F;
						} else {
							parPos.m_X += (RandomNum() >= 0.5F ? 1.0F : -1.0F);
						}
						terrMat = g_SceneMan.GetMaterialFromID(g_SceneMan.GetTerrain()->GetMaterialPixel(parPos.GetFloorIntX(), parPos.GetFloorIntY()));
					}
					(*parIt)->SetPos(parPos.GetFloored());
				}
				if ((*parIt)->GetDrawPriority() >= terrMat->GetPriority()) {
					(*parIt)->DrawToTerrain(g_SceneMan.GetTerrain());
				}
				(*parIt)->DestroyScriptState();
				delete (*parIt);
				m_ValidParticles.erase(*parIt);
				parIt++;
			}
			m_Particles.erase(midIt, m_Particles.end());
		}
	}

	// Feed each actor's stable end-of-tick state into the `actors` checksum subsystem.
	// Fields go in individually with fixed-width types so the byte stream is cross-OS-stable.
	if (g_SimChecksum.IsActive()) {
		DumpControllerDebugSnapshot("end_tick_before_checksum", static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()), m_Actors, nullptr, nullptr, &m_Particles);

		for (Actor* a: m_Actors) {
			const int64_t uniqueID = static_cast<int64_t>(a->GetUniqueID());
			g_SimChecksum.Update("actors", &uniqueID, sizeof(uniqueID));
			const float posX = a->GetPos().m_X;
			g_SimChecksum.Update("actors", &posX, sizeof(posX));
			const float posY = a->GetPos().m_Y;
			g_SimChecksum.Update("actors", &posY, sizeof(posY));
			const float velX = a->GetVel().m_X;
			g_SimChecksum.Update("actors", &velX, sizeof(velX));
			const float velY = a->GetVel().m_Y;
			g_SimChecksum.Update("actors", &velY, sizeof(velY));
			const float health = a->GetHealth();
			g_SimChecksum.Update("actors", &health, sizeof(health));
			// Rotational state is on-wire but absent from the linear actors fingerprint — angle and angular velocity split into separate subsystems so a divergence localizes to the update vs the integration.
			const float actorRotAngle = a->GetRotAngle();
			g_SimChecksum.Update("rot_angle", &actorRotAngle, sizeof(actorRotAngle));
			const float actorAngVel = a->GetAngularVel();
			g_SimChecksum.Update("rot_angvel", &actorAngVel, sizeof(actorAngVel));
		}

		// Controller input state per actor — catches control drift the actors fingerprint misses.
		for (Actor* a: m_Actors) {
			const Controller* controller = a->GetController();
			const int64_t controllerID = static_cast<int64_t>(a->GetUniqueID());
			g_SimChecksum.Update("controller", &controllerID, sizeof(controllerID));
			for (int state = 0; state < ControlState::CONTROLSTATECOUNT; ++state) {
				const uint8_t pressed = controller->IsState(static_cast<ControlState>(state)) ? 1 : 0;
				g_SimChecksum.Update("controller", &pressed, sizeof(pressed));
			}
			const Vector move = controller->GetAnalogMove();
			const Vector aim = controller->GetAnalogAim();
			const Vector cursor = controller->GetAnalogCursor();
			const float analog[6] = {move.m_X, move.m_Y, aim.m_X, aim.m_Y, cursor.m_X, cursor.m_Y};
			g_SimChecksum.Update("controller", analog, sizeof(analog));
			const int32_t inputMode = static_cast<int32_t>(controller->GetInputMode());
			g_SimChecksum.Update("controller", &inputMode, sizeof(inputMode));
			const int32_t aiMode = static_cast<int32_t>(a->GetAIMode());
			g_SimChecksum.Update("controller", &aiMode, sizeof(aiMode));
		}

		// Compact per-particle fingerprint — uniqueID + pos + vel.
		for (MovableObject* p: m_Particles) {
			const int64_t particleID = static_cast<int64_t>(p->GetUniqueID());
			g_SimChecksum.Update("particles", &particleID, sizeof(particleID));
			const float ppX = p->GetPos().m_X;
			g_SimChecksum.Update("particles", &ppX, sizeof(ppX));
			const float ppY = p->GetPos().m_Y;
			g_SimChecksum.Update("particles", &ppY, sizeof(ppY));
			const float pvX = p->GetVel().m_X;
			g_SimChecksum.Update("particles", &pvX, sizeof(pvX));
			const float pvY = p->GetVel().m_Y;
			g_SimChecksum.Update("particles", &pvY, sizeof(pvY));
			const float partAngVel = p->GetAngularVel();
			g_SimChecksum.Update("rot_angvel", &partAngVel, sizeof(partAngVel));
		}

		// Same fingerprint for free items — a dropped device's state was only visible as a count before.
		for (MovableObject* i: m_Items) {
			const int64_t itemID = static_cast<int64_t>(i->GetUniqueID());
			g_SimChecksum.Update("items", &itemID, sizeof(itemID));
			const float ipX = i->GetPos().m_X;
			g_SimChecksum.Update("items", &ipX, sizeof(ipX));
			const float ipY = i->GetPos().m_Y;
			g_SimChecksum.Update("items", &ipY, sizeof(ipY));
			const float ivX = i->GetVel().m_X;
			g_SimChecksum.Update("items", &ivX, sizeof(ivX));
			const float ivY = i->GetVel().m_Y;
			g_SimChecksum.Update("items", &ivY, sizeof(ivY));
			const float itemAngVel = i->GetAngularVel();
			g_SimChecksum.Update("rot_angvel", &itemAngVel, sizeof(itemAngVel));
		}

		// Lightweight population metadata — catches spawn/delete count drift.
		const int32_t actorCount = static_cast<int32_t>(m_Actors.size());
		g_SimChecksum.Update("scene", &actorCount, sizeof(actorCount));
		const int32_t itemCount = static_cast<int32_t>(m_Items.size());
		g_SimChecksum.Update("scene", &itemCount, sizeof(itemCount));
		const int32_t particleCount = static_cast<int32_t>(m_Particles.size());
		g_SimChecksum.Update("scene", &particleCount, sizeof(particleCount));
		for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
			const int32_t rosterSize = static_cast<int32_t>(m_ActorRoster[team].size());
			g_SimChecksum.Update("scene", &rosterSize, sizeof(rosterSize));
		}
		if (const Activity* activity = g_ActivityMan.GetActivity()) {
			for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
				const float teamFunds = activity->GetTeamFunds(team);
				g_SimChecksum.Update("funds", &teamFunds, sizeof(teamFunds));
			}
		}

		// Snapshot the sim + Lua RNG states here — before the see-ray and MOID-draw futures launch
		// and start mutating g_SimRNG on the thread pool — so the snapshot can't be raced.
		const std::string rngState = g_SimRNG.SerializeStateForHashing();
		g_SimChecksum.Update("sim_rng", rngState.data(), rngState.size());
		g_LuaMan.HashAllLuaStatesIntoSimChecksum();
	}

	// Freeze the material terrain for the threaded vision pass so carves can't race the see-ray reads.
	if (SLTerrain* terrain = g_SceneMan.GetTerrain()) {
		terrain->UpdateMaterialCopy();
	}

	// Run seeing rays for all actors
	m_ActorsSeeFuture = g_ThreadMan.GetPriorityThreadPool().parallelize_loop(m_Actors.size(),
	                                                                         [&](int start, int end) {
		                                                                         ZoneScopedN("Actors See");
		                                                                         for (int i = start; i < end; ++i) {
			                                                                         m_Actors[i]->CastSeeRays();
		                                                                         }
	                                                                         });

	// GC kicked off from Main after LateUpdateGlobalScripts, when no more Lua runs on main this tick.

	// The MOID draw task is kicked off from Main after LateUpdateGlobalScripts, once no more
	// main-thread sim code can mutate MO state this tick; an earlier kick let the async draw
	// read mid-mutation state, making the layer a per-peer wall-clock snapshot.

	// Sort team rosters if necessary
	for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
		if (m_SortTeamRoster[Activity::TeamOne]) {
			m_ActorRoster[team].sort(MOXPosComparison());
		}
	}
}

void MovableMan::Travel() {
	ZoneScoped;

	// Travel Actors
	{
		ZoneScopedN("Actors Travel");

		g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ActorsTravel);
		for (Actor* actor: m_Actors) {
			TravelStage(actor);
		}
		g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ActorsTravel);
	}

	// Travel items
	{
		ZoneScopedN("Items Travel");

		for (MovableObject* item: m_Items) {
			TravelStage(item);
		}
	}

	// Travel particles
	{
		ZoneScopedN("Particles Travel");

		g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ParticlesTravel);
		for (MovableObject* particle: m_Particles) {
			TravelStage(particle);
		}
		g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ParticlesTravel);
	}
}

void MovableMan::TravelStage(MovableObject* mo) {
	if (!mo->IsUpdated()) {
		mo->ApplyForces();
		mo->PreTravel();
		mo->Travel();
		mo->PostTravel();
	}
	mo->NewFrame();
}

void MovableMan::UpdateStage(MovableObject* mo) {
	mo->Update();

	g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ScriptsUpdate);
	mo->UpdateScripts();
	g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ScriptsUpdate);

	mo->ApplyImpulses();
}

void MovableMan::PostUpdateStage(MovableObject* mo) {
	mo->PostUpdate();
}

void MovableMan::UpdateControllers() {
	ZoneScoped;

	// Re-sort: ExecuteLuaScriptCallbacks may have added or removed actors since the frame-start sort.
	std::sort(m_Actors.begin(), m_Actors.end(), MOUniqueIDLess());
	std::sort(m_Items.begin(), m_Items.end(), MOUniqueIDLess());
	std::sort(m_Particles.begin(), m_Particles.end(), MOUniqueIDLess());

	// Rebuild the contiguous actor-ID map here, in sync, so the AI-update gate reads it stably;
	// the old async rebuild in UpdateDrawMOIDs raced this tick's actor edits.
	m_ContiguousActorIDs.clear();
	int actorID = 0;
	for (Actor* actor: m_Actors) {
		m_ContiguousActorIDs[actor] = actorID++;
	}

	const uint64_t simTick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
	DumpControllerDebugSnapshot("controller_pre", simTick, m_Actors);

	auto applyReplayFrame = [&](const char* phase) -> bool {
		std::vector<ControllerFrame> frames;
		std::string error;
		if (!ScenarioRunner::GetReplayControllerFrames(simTick, frames, &error)) {
			DumpControllerDebugSnapshot(std::string(phase) + "_missing_frame", simTick, m_Actors, nullptr, &error);
			ScenarioRunner::SetControllerReplayError(std::string("tick ") + std::to_string(simTick) + " " + phase + ": " + error);
			return false;
		}
		DumpControllerDebugSnapshot(std::string(phase) + "_pre_apply", simTick, m_Actors, &frames);
		if (!ApplyControllerFramesToActors(m_Actors, frames, error)) {
			DumpControllerDebugSnapshot(std::string(phase) + "_apply_error", simTick, m_Actors, &frames, &error);
			ScenarioRunner::SetControllerReplayError(std::string("tick ") + std::to_string(simTick) + " " + phase + ": " + error);
			return false;
		}
		DumpControllerDebugSnapshot(std::string(phase) + "_post_apply", simTick, m_Actors, &frames);
		return true;
	};

	if (ScenarioRunner::IsControllerReplayStrict()) {
		applyReplayFrame("strict replay");
		return;
	}

	const bool lockstepActive = ScenarioRunner::IsLockstepControllerSyncActive();
	// A stopped coordinator still owns the sim: surface its stop reason so the match-level
	// handling (resync, clean end, error) runs — never silently degrade to per-machine control.
	if (!lockstepActive && ScenarioRunner::HasLockstepCoordinator()) {
		const std::string reason = ScenarioRunner::GetLockstepStopReason();
		ScenarioRunner::SetControllerReplayError(std::string("tick ") + std::to_string(simTick) + " lockstep stopped: " + (reason.empty() ? "coordinator not running" : reason));
		return;
	}
	auto isLocalControllerActor = [&](const Actor* actor) {
		return !lockstepActive || IsLockstepLocalActor(actor);
	};
	// Release last tick's quarantined joiners here, where the controller wire takes over;
	// a same-tick joiner stays held through its join tick and a corpse stays disabled.
	if (lockstepActive && !m_LockstepJoinQuarantine.empty()) {
		std::vector<long int> released;
		{
			std::lock_guard<std::mutex> lock(m_AddedActorsMutex);
			std::vector<std::pair<uint64_t, long int>> stillHeld;
			for (const auto& entry: m_LockstepJoinQuarantine) {
				if (entry.first < simTick) {
					released.push_back(entry.second);
				} else {
					stillHeld.push_back(entry);
				}
			}
			m_LockstepJoinQuarantine.swap(stillHeld);
		}
		for (long int uid: released) {
			Actor* joined = dynamic_cast<Actor*>(FindObjectByUniqueID(uid));
			if (joined && IsActor(joined)) {
				joined->GetController()->SetDisabled(false);
			}
		}
	}
	if (lockstepActive && ScenarioRunner::IsControllerLogReplaying()) {
		ScenarioRunner::SetControllerReplayError("lockstep controller sync cannot be combined with controller log replay.");
		return;
	}

	g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ActorsAI);
	{
		for (Actor* actor: m_Actors) {
			if (isLocalControllerActor(actor)) {
				actor->GetController()->Update();
			}
		}

		g_LuaMan.SetThreadLuaStateOverride(&g_LuaMan.GetMasterScriptState());
		for (Actor* actor: m_Actors) {
			if (isLocalControllerActor(actor) && actor->GetLuaState() == &g_LuaMan.GetMasterScriptState() && actor->GetController()->ShouldUpdateAIThisFrame()) {
				// Mark the running AI actor so its Equip* mutators defer the mutation to the post-pass drain.
				g_CurrentAIActor = actor;
				actor->RunScriptedFunctionInAppropriateScripts("ThreadedUpdateAI", false, true, {}, {}, {});
				g_CurrentAIActor = nullptr;
			}
		}
		g_LuaMan.SetThreadLuaStateOverride(nullptr);

		LuaStatesArray& luaStates = g_LuaMan.GetThreadedScriptStates();
		g_ThreadMan.GetPriorityThreadPool().parallelize_loop(luaStates.size(),
		                                                     [&](int start, int end) {
			                                                     RTEAssert(start + 1 == end, "Threaded script state being updated across multiple threads!");
			                                                     LuaStateWrapper& luaState = luaStates[start];
			                                                     g_LuaMan.SetThreadLuaStateOverride(&luaState);
			                                                     for (Actor* actor: m_Actors) {
				                                                     if (isLocalControllerActor(actor) && actor->GetLuaState() == &luaState && actor->GetController()->ShouldUpdateAIThisFrame()) {
					                                                     g_CurrentAIActor = actor;
					                                                     actor->RunScriptedFunctionInAppropriateScripts("ThreadedUpdateAI", false, true, {}, {}, {});
					                                                     g_CurrentAIActor = nullptr;
				                                                     }
			                                                     }
			                                                     g_LuaMan.SetThreadLuaStateOverride(nullptr);
		                                                     },
		                                                     luaStates.size())
		    .wait();

		// Drain the equip mutations AHuman::Equip* queued under parallel AI, in MOID order.
		for (Actor* actor: m_Actors) {
			if (isLocalControllerActor(actor)) {
				if (AHuman* asHuman = dynamic_cast<AHuman*>(actor)) {
					asHuman->DrainPendingDeferredMutations();
				}
			}
		}

		for (Actor* actor: m_Actors) {
			if (isLocalControllerActor(actor) && actor->GetController()->ShouldUpdateAIThisFrame()) {
				actor->RunScriptedFunctionInAppropriateScripts("UpdateAI", false, true, {}, {}, {});
			}
		}
	}
	g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ActorsAI);

	if (lockstepActive) {
		std::string error;
		// Sample this tick's local AI decisions and schedule them to APPLY inputDelayFrames ticks from
		// now (QueueLocalInput stamps targetFrame = simTick + D). Actors are then driven this tick by
		// the frame COMMITTED for simTick — sampled D ticks ago — so local and remote apply in phase.
		// At D=0 the committed local frame is this tick's snapshot, so behavior is unchanged.
		std::vector<ControllerFrame> localFrames = SnapshotLockstepControllerFrames(m_Actors, true);
		DumpControllerDebugSnapshot("lockstep_local_pre_canonicalize", simTick, m_Actors, &localFrames);
		if (!CanonicalizeControllerFramesThroughWire(localFrames, error)) {
			DumpControllerDebugSnapshot("lockstep_local_canonicalize_error", simTick, m_Actors, &localFrames, &error);
			ScenarioRunner::SetControllerReplayError(std::string("tick ") + std::to_string(simTick) + " lockstep canonicalize: " + error);
			return;
		}
		if (!ScenarioRunner::QueueLockstepLocalControllerFrames(simTick, localFrames, &error)) {
			DumpControllerDebugSnapshot("lockstep_local_queue_error", simTick, m_Actors, &localFrames, &error);
			ScenarioRunner::SetControllerReplayError(std::string("tick ") + std::to_string(simTick) + " lockstep queue: " + error);
			return;
		}

		NetLockstepReadyFrame readyFrame;
		if (!ScenarioRunner::WaitForLockstepControllerFrame(simTick, readyFrame, &error)) {
			DumpControllerDebugSnapshot("lockstep_remote_wait_error", simTick, m_Actors, nullptr, &error);
			ScenarioRunner::SetControllerReplayError(std::string("tick ") + std::to_string(simTick) + " lockstep wait: " + error);
			return;
		}
		if (!ApplyControllerFramesToLockstepActors(m_Actors, readyFrame.localFrames, true, error)) {
			DumpControllerDebugSnapshot("lockstep_local_apply_error", simTick, m_Actors, &readyFrame.localFrames, &error);
			ScenarioRunner::SetControllerReplayError(std::string("tick ") + std::to_string(simTick) + " lockstep local apply: " + error);
			return;
		}
		if (!ApplyControllerFramesToLockstepActors(m_Actors, readyFrame.remoteFrames, false, error)) {
			DumpControllerDebugSnapshot("lockstep_remote_apply_error", simTick, m_Actors, &readyFrame.remoteFrames, &error);
			ScenarioRunner::SetControllerReplayError(std::string("tick ") + std::to_string(simTick) + " lockstep remote apply: " + error);
			return;
		}
		// A leaver's actors dropped off the wire: their control handoffs revert to the policy owner
		// (a surviving teammate's AI picks them up), and actors with no surviving owner stand down —
		// on every survivor at the same tick.
		ScenarioRunner::PurgeLockstepControlOverridesForGonePeers(readyFrame.frame);
		for (Actor* actor: m_Actors) {
			if (ScenarioRunner::IsLockstepActorOwnerGone(static_cast<int64_t>(actor->GetUniqueID()), actor->GetTeam(), !actor->IsPlayerControlled(), readyFrame.frame)) {
				actor->GetController()->SetDisabled(true);
			}
		}
		DumpControllerDebugSnapshot("lockstep_post_apply", simTick, m_Actors, &readyFrame.remoteFrames);
		ApplyLockstepGameCommands(readyFrame);

		if (ScenarioRunner::IsControllerLogRecording()) {
			std::vector<ControllerFrame> frames = SnapshotControllerFrames(m_Actors);
			ScenarioRunner::RecordControllerFrames(simTick, std::move(frames));
		}
		return;
	}

	if (ScenarioRunner::IsControllerLogReplaying()) {
		if (!applyReplayFrame("replay")) {
			return;
		}
	}

	if (ScenarioRunner::IsControllerLogRecording()) {
		std::vector<ControllerFrame> frames = SnapshotControllerFrames(m_Actors);
		DumpControllerDebugSnapshot("record_pre_canonicalize", simTick, m_Actors, &frames);
		if (ScenarioRunner::ShouldCanonicalizeControllerLog()) {
			std::string error;
			if (!CanonicalizeControllerFramesThroughWire(frames, error)) {
				DumpControllerDebugSnapshot("record_canonicalize_error", simTick, m_Actors, &frames, &error);
				ScenarioRunner::SetControllerReplayError(std::string("tick ") + std::to_string(simTick) + " record canonicalize: " + error);
				return;
			}
			if (!ApplyControllerFramesToActors(m_Actors, frames, error)) {
				DumpControllerDebugSnapshot("record_canonicalize_apply_error", simTick, m_Actors, &frames, &error);
				ScenarioRunner::SetControllerReplayError(std::string("tick ") + std::to_string(simTick) + " record canonicalize: " + error);
				return;
			}
		}
		DumpControllerDebugSnapshot("record_post_canonicalize", simTick, m_Actors, &frames);
		ScenarioRunner::RecordControllerFrames(simTick, std::move(frames));
	}
}

void MovableMan::PreControllerUpdate() {
	ZoneScoped;

	g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ActorsUpdate);
	for (Actor* actor: m_Actors) {
		actor->PreControllerUpdate();
	}
	g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ActorsUpdate);

	for (MovableObject* item: m_Items) {
		item->PreControllerUpdate();
	}

	g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ParticlesUpdate);
	for (MovableObject* particle: m_Particles) {
		particle->PreControllerUpdate();
	}
	g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ParticlesUpdate);
}

void MovableMan::DrawMatter(BITMAP* pTargetBitmap, Vector& targetPos) {
	ScopedRenderRNG renderRNG;
	// Draw objects to accumulation bitmap
	for (std::deque<Actor*>::iterator aIt = --m_Actors.end(); aIt != --m_Actors.begin(); --aIt)
		(*aIt)->Draw(pTargetBitmap, targetPos, g_DrawMaterial);

	for (std::deque<MovableObject*>::iterator parIt = --m_Particles.end(); parIt != --m_Particles.begin(); --parIt)
		(*parIt)->Draw(pTargetBitmap, targetPos, g_DrawMaterial);
}

void MovableMan::VerifyMOIDIndex() {
	int count = 0;
	for (std::vector<MovableObject*>::iterator aIt = m_MOIDIndex.begin(); aIt != m_MOIDIndex.end(); ++aIt) {
		if (*aIt) {
			RTEAssert((*aIt)->GetID() == g_NoMOID || (*aIt)->GetID() == count, "MOIDIndex broken!");
			RTEAssert((*aIt)->GetRootID() == g_NoMOID || ((*aIt)->GetRootID() >= 0 && (*aIt)->GetRootID() < g_MovableMan.GetMOIDCount()), "MOIDIndex broken!");
		}
		count++;
		if (count == g_NoMOID)
			count++;
	}

	for (std::deque<MovableObject*>::iterator itr = m_Items.begin(); itr != m_Items.end(); ++itr) {
		RTEAssert((*itr)->GetID() == g_NoMOID || (*itr)->GetID() < GetMOIDCount(), "MOIDIndex broken!");
		RTEAssert((*itr)->GetRootID() == g_NoMOID || ((*itr)->GetRootID() >= 0 && (*itr)->GetRootID() < g_MovableMan.GetMOIDCount()), "MOIDIndex broken!");
	}
	// Try the items just added this frame
	for (std::deque<MovableObject*>::iterator itr = m_AddedItems.begin(); itr != m_AddedItems.end(); ++itr) {
		RTEAssert((*itr)->GetID() == g_NoMOID || (*itr)->GetID() < GetMOIDCount(), "MOIDIndex broken!");
		RTEAssert((*itr)->GetRootID() == g_NoMOID || ((*itr)->GetRootID() >= 0 && (*itr)->GetRootID() < g_MovableMan.GetMOIDCount()), "MOIDIndex broken!");
	}
}

void MovableMan::UpdateDrawMOIDs() {
	ScopedRenderRNG renderRNG;
	ZoneScoped;

	///////////////////////////////////////////////////
	// Clear the MOID layer before starting to delete stuff which may be in the MOIDIndex
	g_SceneMan.ClearAllMOIDDrawings();

	// Clear the index each frame and do it over because MO's get added and deleted between each frame.
	m_MOIDIndex.clear();

	// Add a null and start counter at 1 because MOID == 0 means no MO.
	// - Update: This isnt' true anymore, but still keep 0 free just to be safe
	m_MOIDIndex.push_back(0);

	MOID currentMOID = 1;

	for (Actor* actor: m_Actors) {
		if (!actor->IsSetToDelete()) {
			actor->UpdateMOID(m_MOIDIndex);
			actor->Draw(nullptr, Vector(), g_DrawMOID, true);
			currentMOID = m_MOIDIndex.size();
		} else {
			actor->SetAsNoID();
		}
	}

	for (MovableObject* item: m_Items) {
		if (!item->IsSetToDelete()) {
			item->UpdateMOID(m_MOIDIndex);
			item->Draw(nullptr, Vector(), g_DrawMOID, true);
			currentMOID = m_MOIDIndex.size();
		} else {
			item->SetAsNoID();
		}
	}

	for (MovableObject* particle: m_Particles) {
		if (!particle->IsSetToDelete()) {
			particle->UpdateMOID(m_MOIDIndex);
			particle->Draw(nullptr, Vector(), g_DrawMOID, true);
			currentMOID = m_MOIDIndex.size();
		} else {
			particle->SetAsNoID();
		}
	}

	// COUNT MOID USAGE PER TEAM  //////////////////////////////////////////////////
	for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; team++) {
		m_TeamMOIDCount[team] = 0;
	}

	for (auto itr = m_MOIDIndex.begin(); itr != m_MOIDIndex.end(); ++itr) {
		if (*itr) {
			int team = (*itr)->GetTeam();
			if (team > Activity::NoTeam && team < Activity::MaxTeamCount) {
				m_TeamMOIDCount[team]++;
			}
		}
	}
}

void MovableMan::StartMOIDDrawTask() {
	m_DrawMOIDsTask = g_ThreadMan.GetPriorityThreadPool().submit([this]() {
		UpdateDrawMOIDs();
	});
}

void MovableMan::CompleteQueuedMOIDDrawings() {
	if (m_DrawMOIDsTask.valid()) {
		m_DrawMOIDsTask.wait();
	}
}

void MovableMan::Draw(BITMAP* pTargetBitmap, const Vector& targetPos) {
	ScopedRenderRNG renderRNG;
	ZoneScoped;

	// Draw objects to accumulation bitmap, in reverse order so actors appear on top.

	{
		ZoneScopedN("Particles Draw");

		for (std::deque<MovableObject*>::iterator parIt = m_Particles.begin(); parIt != m_Particles.end(); ++parIt) {
			(*parIt)->Draw(pTargetBitmap, targetPos);
		}
	}

	{
		ZoneScopedN("Items Draw");

		for (std::deque<MovableObject*>::reverse_iterator itmIt = m_Items.rbegin(); itmIt != m_Items.rend(); ++itmIt) {
			(*itmIt)->Draw(pTargetBitmap, targetPos);
		}
	}

	{
		ZoneScopedN("Actors Draw");

		for (std::deque<Actor*>::reverse_iterator aIt = m_Actors.rbegin(); aIt != m_Actors.rend(); ++aIt) {
			(*aIt)->Draw(pTargetBitmap, targetPos);
		}
	}
}

void MovableMan::DrawHUD(BITMAP* pTargetBitmap, const Vector& targetPos, int which, bool playerControlled) {
	ScopedRenderRNG renderRNG;
	ZoneScoped;

	// Draw HUD elements
	for (std::deque<MovableObject*>::reverse_iterator itmIt = m_Items.rbegin(); itmIt != m_Items.rend(); ++itmIt)
		(*itmIt)->DrawHUD(pTargetBitmap, targetPos, which);

	for (std::deque<Actor*>::reverse_iterator aIt = m_Actors.rbegin(); aIt != m_Actors.rend(); ++aIt)
		(*aIt)->DrawHUD(pTargetBitmap, targetPos, which);
}
