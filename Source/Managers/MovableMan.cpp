#include "CheckpointArchive.h"
#include "OwnedMovableObjects.h"
#include "MovableMan.h"
#include "NetA7Journal.h"
#include "PrimitiveMan.h"
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
#include "ACDropShip.h"
#include "PEmitter.h"
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
#include "ActivityMan.h"
#include "CameraMan.h"
#include "SceneMan.h"
#include "AudioMan.h"
#include "SoundSimulation.h"
#include "SettingsMan.h"
#include "ControllerFrame.h"
#include "PieMenu.h"
#include "ScenarioRunner.h"
#include "AIWriteScript.h"
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
static std::vector<MovableObject*> SortedRegisteredMOs(const LuaStateWrapper& state, bool includePending = false) {
	const auto& registered = state.GetRegisteredMOs();
	std::vector<MovableObject*> sorted(registered.begin(), registered.end());
	if (includePending) {
		for (MovableObject* mo: state.GetPendingRegisteredMOs()) {
			if (registered.find(mo) == registered.end()) {
				sorted.push_back(mo);
			}
		}
	}
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

void MovableMan::RecordA7UnitOwnership(uint64_t round, uint64_t frame) const {
	if (!NetA7Journal::Enabled()) return;
	if (Activity* activity = g_ActivityMan.GetActivity()) {
		const auto uid = [](const Actor* actor) { return actor && g_MovableMan.IsActor(actor) ? static_cast<int64_t>(actor->GetUniqueID()) : int64_t{0}; };
		Actor* controlled = activity->GetControlledActor(Players::PlayerOne);
		const int screen = activity->ScreenOfPlayer(Players::PlayerOne);
		json view = {{"round_id", round}, {"frame", frame}, {"peer_id", ScenarioRunner::GetLockstepLocalPeerId()},
			{"player_active", activity->PlayerActive(Players::PlayerOne)}, {"player_human", activity->PlayerHuman(Players::PlayerOne)},
			{"team", activity->GetTeamOfPlayer(Players::PlayerOne)}, {"screen", screen},
			{"controlled_uid", uid(controlled)}, {"brain_uid", uid(activity->GetPlayerBrain(Players::PlayerOne))},
			{"view_state", static_cast<int>(activity->GetViewState())}, {"camera_target", nullptr},
			{"seat_mode", nullptr}, {"seat_player", nullptr}};
		if (uid(controlled) != 0) {
			view["seat_mode"] = static_cast<int>(controlled->GetController()->GetSeatMode());
			view["seat_player"] = controlled->GetController()->GetSeatPlayer();
		}
		if (screen >= 0) {
			const Vector target = g_CameraMan.GetScrollTarget(screen);
			view["camera_target"] = {target.m_X, target.m_Y};
		}
		NetA7Journal::Emit("local_control", std::move(view));
	}
	for (Actor* actor: m_Actors) {
		if (!actor) continue;
		const int64_t uid = static_cast<int64_t>(actor->GetUniqueID());
		const bool alive = actor->GetHealth() > 0 && actor->GetStatus() != Actor::DYING && !actor->IsDead() && !actor->IsSetToDelete();
		NetA7Journal::Emit("unit_owner", {{"round_id", round}, {"frame", frame}, {"uid", uid}, {"team", actor->GetTeam()},
			{"owner_peer_id", ScenarioRunner::GetLockstepActorOwner(uid, actor->GetTeam(), !actor->IsPlayerControlled())},
			{"player_controlled", actor->IsPlayerControlled()}, {"status", actor->GetStatus()}, {"alive", alive},
			{"controller_mode", static_cast<int>(actor->GetController()->GetInputMode())}, {"controller_player", actor->GetController()->GetPlayerRaw()},
			{"controller_disabled", actor->GetController()->IsDisabled()}});
	}
}

std::vector<MovableMan::LockstepActorOwner> MovableMan::BuildLockstepOwnershipCensus() const {
	// Only the settled list: an actor still in m_AddedActors has not been agreed on by every peer yet,
	// and a ledger entry naming one would reseat something a returning peer never held.
	std::vector<LockstepActorOwner> census;
	census.reserve(m_Actors.size());
	for (const Actor* actor: m_Actors) {
		if (!actor) {
			continue;
		}
		const int64_t actorID = static_cast<int64_t>(actor->GetUniqueID());
		census.push_back({actorID, actor->GetTeam(), ScenarioRunner::GetLockstepDropTimeActorOwner(actorID, actor->GetTeam(), !actor->IsPlayerControlled())});
	}
	return census;
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

static bool ApplyControllerFramesToLockstepActors(const std::deque<Actor*>& actors, const std::vector<ControllerFrame>& frames, bool localOwned, std::unordered_set<int64_t>& applied, std::string& error) {
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
		applied.insert(frame.actorUniqueID);
	}
	return true;
}

// An actor no frame was committed for this tick (its first D ticks in the world, the ticks after a
// pause or an ownership change) runs on neutral input on every peer, not on its owner's fresh sample.
static void NeutralizeUnframedLockstepActors(const std::deque<Actor*>& actors, const std::unordered_set<int64_t>& applied) {
	for (Actor* actor: actors) {
		if (applied.find(static_cast<int64_t>(actor->GetUniqueID())) == applied.end()) {
			actor->GetController()->ApplyWireNeutral();
		}
	}
}

bool MovableMan::ApplyLockstepFrameToActor(Actor& actor, const ControllerFrame& frame, uint64_t simTick, std::string* error) {
	std::string applyError;
	// A legacy recording applied the actor state absolutely every tick; it keeps that semantics.
	const bool applied = frame.IsLegacy() ? ControllerFrameCodec::ApplyActorState(frame, actor, &applyError) : ControllerFrameCodec::ApplyActorStateIntents(frame, actor, &applyError);
	if (!applied) {
		if (error) {
			*error = "lockstep actor-state apply failed for actor " + std::to_string(frame.actorUniqueID) + ": " + applyError;
		}
		return false;
	}
	Controller& controller = *actor.GetController();
	const Controller::InputMode previousMode = controller.GetInputMode();
	const int previousPlayer = controller.GetPlayer();
	if (!ControllerFrameCodec::Apply(frame, controller, &applyError)) {
		if (error) {
			*error = "lockstep controller apply failed for actor " + std::to_string(frame.actorUniqueID) + ": " + applyError;
		}
		return false;
	}
	controller.SetWireApplyTick(static_cast<int64_t>(simTick));
	if (controller.GetInputMode() != previousMode || controller.GetPlayer() != previousPlayer) {
		actor.OnControllerInputModeChanged(previousMode, previousPlayer);
	}
	return true;
}

// The wire form and the queued form of a sound call name the same operations.
static_assert(static_cast<uint8_t>(SoundContainer::PendingOp::OpCount) == NetGameSoundOp::OpCount);
static_assert(static_cast<uint8_t>(SoundContainer::PendingOp::PropertyCount) == NetGameSoundOp::c_PropertyCount);

// Runs one AI sound call for real, in a shared scope every peer derives the same key from.
static void ApplyDeferredSoundOp(const NetGameSoundOp& command) {
	SoundContainer* container = g_AudioMan.FindSimulationSoundContainer(command.soundIdentity);
	if (!container) {
		g_ConsoleMan.PrintString("NETWORK: sound command names no live sound: " + std::to_string(command.soundIdentity));
		return;
	}
	SoundContainer::PendingOp op;
	op.op = static_cast<SoundContainer::PendingOp::Op>(command.op);
	op.property = static_cast<SoundContainer::PendingOp::Property>(command.property);
	op.actorUID = command.actorUID;
	op.team = command.team;
	op.player = command.player;
	op.value = command.value;
	op.x = command.x;
	op.y = command.y;
	op.soundSetPath = command.soundSetPath;
	op.payload = command.payload;
	SoundSimulationScope sounds(static_cast<uint64_t>(command.actorUID), Hash("DeferredSoundOp"), SoundExecutionDomain::SharedSimulation, g_AudioMan.NextDeferredSoundOpOrdinal());
	container->ApplyPendingSoundOp(op);
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
		if (const auto* bindings = std::get_if<NetGamePlayerBindings>(&command.payload)) {
			ScenarioRunner::ObserveLockstepPlayerBindings(command.senderPeerId, readyFrame.frame, *bindings);
			continue;
		}
		if (!ScenarioRunner::ConsumeLockstepGameCommand(command)) continue;
		// Only a peer that controls a team may issue economy commands for it — ANY of a shared
		// co-op team's human peers counts; every peer resolves this identically.
		const int32_t commandTeam = NetGameCommandTeam(command.payload);
		if (std::holds_alternative<NetGameReseat>(command.payload)) {
			// A reseat is system-authored: the host hands a returning holder back its own team, which
			// the host itself need not control, so the team gate cannot vet it.
			if (command.senderPeerId != ScenarioRunner::GetLockstepHostPeerId()) {
				g_ConsoleMan.PrintString("ERROR: Rejected a Reseat command from a peer that is not the host");
				continue;
			}
		} else if (!ScenarioRunner::IsLockstepTeamCommandSender(commandTeam, command.senderPeerId)) {
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
		} else if (const NetGameAIOrder* order = std::get_if<NetGameAIOrder>(&command.payload)) {
			Actor* actor = dynamic_cast<Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(order->actorUID)));
			if (actor && actor->GetTeam() == order->team && std::isfinite(order->x) && std::isfinite(order->y)) {
				switch (order->op) {
					case NetGameAIOrder::SceneWaypoint:
						actor->AddAISceneWaypoint(Vector(order->x, order->y));
						break;
					case NetGameAIOrder::MOWaypoint:
						if (const MovableObject* target = g_MovableMan.FindObjectByUniqueID(static_cast<long int>(order->targetUID))) {
							actor->AddAIMOWaypoint(target);
						}
						break;
					case NetGameAIOrder::ClearWaypoints:
						actor->ClearAIWaypoints();
						break;
					case NetGameAIOrder::FormSquad:
						actor->FormSquad(Vector(order->x, order->y));
						break;
					case NetGameAIOrder::DisbandSquad:
						actor->DisbandSquad();
						break;
					case NetGameAIOrder::PopWaypoint:
						actor->PopFrontWaypoint(Vector(order->x, order->y));
						break;
					default:
						break;
				}
			} else {
				g_ConsoleMan.PrintString("NETWORK: AI order target not found: UID " + std::to_string(order->actorUID));
				std::cout << "[net-match] AI order target not found: UID " << order->actorUID << std::endl;
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
		} else if (const NetGameReseat* reseat = std::get_if<NetGameReseat>(&command.payload)) {
			// Like SwitchControl, the override lands even for an actor that is already gone so every
			// peer's map stays identical; a live actor that left the team is not reseated.
			int reseated = 0;
			for (const int64_t actorUID: reseat->actorUIDs) {
				const Actor* actor = dynamic_cast<const Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(actorUID)));
				if (actor && actor->GetTeam() != reseat->team) {
					continue;
				}
				ScenarioRunner::SetLockstepControlOverride(actorUID, reseat->newOwnerPeerId);
				++reseated;
			}
			std::cout << "[net-match] reseat: team " << reseat->team << " -> peer " << static_cast<int>(reseat->newOwnerPeerId) << " actors " << reseated << "/" << reseat->actorUIDs.size() << std::endl;
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
		} else if (const NetGameAIEquip* equip = std::get_if<NetGameAIEquip>(&command.payload)) {
			// The AI's equip call runs here on every peer; only the peer driving the actor may issue it.
			AHuman* human = dynamic_cast<AHuman*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(equip->actorUID)));
			if (!human) {
				g_ConsoleMan.PrintString("NETWORK: AI equip command target not found: UID " + std::to_string(equip->actorUID));
				std::cout << "[net-match] AI equip command target not found: UID " << equip->actorUID << std::endl;
				continue;
			}
			if (human->GetTeam() != equip->team || !ScenarioRunner::IsLockstepActorOwner(equip->actorUID, human->GetTeam(), !human->IsPlayerControlled(), command.senderPeerId)) {
				g_ConsoleMan.PrintString("ERROR: Rejected an AI equip command from a peer that does not drive actor " + std::to_string(equip->actorUID));
				continue;
			}
			AHuman::DeferredEquip deferred;
			deferred.op = static_cast<AHuman::DeferredEquip::Op>(equip->op);
			deferred.depositToFront = equip->depositToFront;
			deferred.group = equip->group;
			deferred.excludeGroup = equip->excludeGroup;
			deferred.moduleName = equip->moduleName;
			deferred.presetName = equip->presetName;
			human->ExecuteDeferredEquip(deferred);
		} else if (const NetGameSoundOp* sound = std::get_if<NetGameSoundOp>(&command.payload)) {
			// The AI's sound call runs here on every peer; only the peer driving the actor may issue it.
			Actor* actor = dynamic_cast<Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(sound->actorUID)));
			if (!actor) {
				g_ConsoleMan.PrintString("NETWORK: sound command target not found: UID " + std::to_string(sound->actorUID));
				continue;
			}
			if (actor->GetTeam() != sound->team || !ScenarioRunner::IsLockstepActorOwner(sound->actorUID, actor->GetTeam(), !actor->IsPlayerControlled(), command.senderPeerId)) {
				g_ConsoleMan.PrintString("ERROR: Rejected a sound command from a peer that does not drive actor " + std::to_string(sound->actorUID));
				continue;
			}
			ApplyDeferredSoundOp(*sound);
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
	g_AudioMan.CommitSoundObservations(readyFrame.frame, readyFrame.localObservations, readyFrame.remoteObservations);
	ApplyLockstepGameCommands(readyFrame);
	return true;
}

// The object's script set as one hash: each loaded path and whether it is enabled, in load order.
static uint64_t ScriptSetHash(const MovableObject& mo) {
	uint64_t hash = 1469598103934665603ULL;
	for (const std::string& scriptPath: mo.GetAllLoadedScripts()) {
		for (unsigned char c: scriptPath) {
			hash = (hash ^ c) * 1099511628211ULL;
		}
		hash = (hash ^ (mo.ScriptEnabled(scriptPath) ? 0x7Cu : 0x7Du)) * 1099511628211ULL;
	}
	return hash;
}

// Snapshot forensics: one line per attachable and wound, recursively, so limb-level state is diffable.
static void DumpAttachableTree(uint64_t tick, const MOSRotating* parent, std::ostream& out) {
	auto dumpNode = [&](const char* kind, const Attachable* node) {
		const HDFirearm* parentFirearm = dynamic_cast<const HDFirearm*>(parent);
		const AEmitter* parentEmitter = dynamic_cast<const AEmitter*>(parent);
		const bool isFlash = (parentFirearm && parentFirearm->GetFlash() == node) || (parentEmitter && parentEmitter->GetFlash() == node);
		out << tick << " " << kind << " uid=" << node->GetUniqueID() << " " << node->GetPresetName()
		    << std::defaultfloat << " par=" << parent->GetUniqueID() << " moid=" << node->GetID() << "/" << node->GetRootID();
		if (node->HasAnyScripts()) {
			out << " scr=" << std::hex << ScriptSetHash(*node) << std::dec;
		}
		out << " frame=";
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
		out << " dmg=" << node->GetDamageCount();
		if (const AEmitter* emitter = dynamic_cast<const AEmitter*>(node)) {
			out << std::defaultfloat << " em=" << emitter->IsEmitting() << "/" << emitter->GetEmitCount() << "/" << emitter->IsSetToBurst() << "/" << emitter->WasEmitting() << std::hexfloat << "/" << emitter->GetThrottle() << "/" << emitter->GetBurstTimerElapsedSimMS() << "/" << emitter->GetLastEmitTimerElapsedSimMS();
			for (double accumulator: emitter->GetEmissionAccumulators()) {
				out << "/" << accumulator;
			}
			for (const auto& [startElapsed, stopElapsed]: emitter->GetEmissionTimerElapsed()) {
				out << "/" << startElapsed << ":" << stopElapsed;
			}
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
		    << std::defaultfloat << " moid=" << mo->GetID() << "/" << mo->GetRootID();
		if (mo->HasAnyScripts()) {
			out << " scr=" << std::hex << ScriptSetHash(*mo) << std::dec;
		}
		out << std::hexfloat << " mass=" << mo->GetMass();
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
					// The root group's atoms in list order: offsets and subgroup ids drive collision and are not visible elsewhere.
					uint64_t atomHash = 1469598103934665603ULL;
					int atomIndex = 0;
					for (const Atom* atom: group->GetAtomList()) {
						atomHash = fnv(fnv(fnv(atomHash, std::bit_cast<uint32_t>(atom->GetOffset().m_X)), std::bit_cast<uint32_t>(atom->GetOffset().m_Y)), static_cast<uint32_t>(atom->GetSubID()));
						if (SceneMan::IsTrackedUID(mo->GetUniqueID())) {
							SceneMan::TraceTerrainEvent("atom", std::bit_cast<int32_t>(atom->GetOffset().m_X), std::bit_cast<int32_t>(atom->GetOffset().m_Y), static_cast<int32_t>(atom->GetSubID()), atomIndex, static_cast<int>(mo->GetUniqueID()));
						}
						++atomIndex;
					}
					out << std::defaultfloat << " atoms=" << group->GetAtomCount() << ":" << std::hex << atomHash << std::dec << std::hexfloat;
				}
			}
		}
		out
		    << " rest=" << mo->GetRestTimerElapsedSimMS() << std::defaultfloat
		    << " osc=" << mo->GetVelOscillations() << " settle=" << mo->ToSettle() << std::hexfloat
		    << " pvel=" << mo->GetPrevVel().m_X << "," << mo->GetPrevVel().m_Y << std::defaultfloat << " wdmg=" << mo->GetApplyWoundDamageOnCollision() << mo->GetApplyWoundBurstDamageOnCollision() << std::hexfloat
		    << " air=" << mo->GetAirResistance() << "/" << mo->GetAirThreshold() << "/" << mo->GetGlobalAccScalar();
		if (const PEmitter* emitter = dynamic_cast<const PEmitter*>(mo)) {
			out << std::defaultfloat << " pem=" << emitter->IsEmitting() << "/" << emitter->GetEmitCount() << "/" << emitter->IsSetToBurst() << "/" << emitter->WasEmitting() << std::hexfloat << "/" << emitter->GetThrottle() << "/" << emitter->GetBurstTimerElapsedSimMS() << "/" << emitter->GetLastEmitTimerElapsedSimMS();
			for (double accumulator: emitter->GetEmissionAccumulators()) {
				out << "/" << accumulator;
			}
			for (const auto& [startElapsed, stopElapsed]: emitter->GetEmissionTimerElapsed()) {
				out << "/" << startElapsed << ":" << stopElapsed;
			}
		}
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
			out << std::defaultfloat << " awm=" << std::hexfloat << actor->GetAttachableAndWoundMassForSave() << " inv=" << actor->GetInventoryMass() << " gold=" << actor->GetGoldCarried() << " base=" << actor->MovableObject::GetMass() << std::defaultfloat << " ninv=" << actor->GetInventorySize() << " ctrl=0x" << std::hex << states << std::dec << " mode=" << static_cast<int>(controller->GetInputMode()) << " dis=" << controller->IsDisabled() << " status=" << static_cast<int>(actor->GetStatus()) << " team=" << actor->GetTeam() << " aimode=" << static_cast<int>(actor->GetAIMode()) << " wp=" << actor->GetWaypointsSize() << " health=" << std::hexfloat << actor->GetHealth() << " aim=" << actor->GetAimAngle(false) << std::defaultfloat << " flip=" << actor->IsHFlipped();
			if (const PieMenu* pieMenu = actor->GetPieMenu()) {
				out << " pie=" << pieMenu->DescribeInteractionState();
			}
			out << " inv=[";
			for (const MovableObject* inventoryItem: *actor->GetInventory()) {
				out << inventoryItem->GetUniqueID() << ":" << inventoryItem->GetPresetName() << ";";
			}
			out << "]";
			out << " mstate=" << static_cast<int>(actor->GetMovementState()) << " goldpicked=" << actor->GetGoldPicked() << std::hexfloat
			    << " atmr=" << actor->GetLastSecondTimerElapsedSimMS() << "/" << actor->GetStableRecoverTimerElapsedSimMS() << "/" << actor->GetHeartBeatTimerElapsedSimMS() << "/" << actor->GetNewControlTimerElapsedSimMS() << "/" << actor->GetDeathTimerElapsedSimMS()
			    << " recent=" << actor->GetRecentMovement().m_X << "," << actor->GetRecentMovement().m_Y << " view=" << actor->GetViewPointRaw().m_X << "," << actor->GetViewPointRaw().m_Y
			    << " prevhealth=" << actor->GetPrevHealth();
			// The alarm point is the owner's AI perception, per machine like the walk paths; peer compares skip it.
			out << " alarm=" << actor->GetAlarmTimerElapsedSimMS() << "@" << actor->GetLastAlarmPosRaw().m_X << "," << actor->GetLastAlarmPosRaw().m_Y << std::defaultfloat;
			if (!ScenarioRunner::GetArgs().testScript.empty()) {
				LuaStateWrapper* state = actor->GetLuaState();
				const long create = state ? static_cast<long>(state->GetScriptObjectNumberField(actor->GetUniqueID(), "testCreate", -1.0)) : -1;
				const long update = state ? static_cast<long>(state->GetScriptObjectNumberField(actor->GetUniqueID(), "testUpdate", -1.0)) : -1;
				const long carried = state ? static_cast<long>(state->GetScriptObjectNumberField(actor->GetUniqueID(), "testCarried", -1.0)) : -1;
				out << " script=" << create << "/" << update << "/" << static_cast<long>(actor->GetNumberValue("TestUpdates")) << "/" << carried;
				std::vector<std::pair<std::string, double>> numberValues(actor->GetNumberValueMap().begin(), actor->GetNumberValueMap().end());
				std::sort(numberValues.begin(), numberValues.end());
				out << " nv=[" << std::hexfloat;
				for (const auto& [name, value]: numberValues) {
					out << name << ":" << value << ";";
				}
				out << std::defaultfloat << "]";
			}
			if (const ACraft* craft = dynamic_cast<const ACraft*>(mo)) {
				out << " hatch=" << static_cast<int>(craft->GetHatchState()) << " deathms=" << craft->GetDeathTimerElapsedSimMS() << std::hexfloat << " hatchms=" << craft->GetHatchTimerElapsedSimMS() << " exitms=" << craft->GetExitTimerElapsedSimMS();
				if (const ACDropShip* dropShip = dynamic_cast<const ACDropShip*>(craft)) {
					out << " lateral=" << dropShip->GetLateralControl();
				}
				out << " ctmr=" << craft->GetFlippedTimerElapsedSimMS() << "/" << craft->GetCrashTimerElapsedSimMS() << "/" << craft->GetNetworkDeliveryTimerElapsedSimMS() << std::defaultfloat << " netdel=" << craft->IsNetworkDelivery() << " exit=" << craft->GetCurrentExitIndex() << "/" << craft->GetExitLinePhase();
				for (long uid: craft->GetExitIncomingMOUniqueIDs()) {
					out << "/" << uid;
				}
			}
			if (const MovableObject* moToNotHit = mo->GetWhichMOToNotHit(); moToNotHit && const_cast<MovableMan*>(this)->FindObjectByUniqueID(mo->GetMOToNotHitUID()) == moToNotHit) {
				out << std::defaultfloat << " nothit=" << mo->GetMOToNotHitUID() << std::hexfloat << ":" << mo->GetMOIgnoreTimerElapsedSimMS() << "/" << mo->GetMOIgnoreTimerLimitMS() << std::defaultfloat;
			}
			if (const MOSRotating* rotating = dynamic_cast<const MOSRotating*>(mo)) {
				out << " imp=" << std::hexfloat << rotating->GetTravelImpulse().GetMagnitude() << std::defaultfloat << " wounds=" << rotating->GetWoundCount();
			}
			if (const AHuman* human = dynamic_cast<const AHuman*>(mo)) {
				if (const AEJetpack* jetpack = human->GetJetpack()) {
					out << " jet=" << std::hexfloat << jetpack->GetJetTimeLeft() << " bonus=" << jetpack->GetJetThrustBonusMultiplier() << std::defaultfloat << " emit=" << jetpack->IsEmitting();
				}
				out << " hstate=" << static_cast<int>(human->GetProneState()) << "/" << static_cast<int>(human->GetUpperBodyState()) << "/" << human->IsArmClimbing(0) << human->IsArmClimbing(1) << "/" << human->IsAiming() << "/" << human->StrideFrame() << human->GetStrideStart()
				    << std::hexfloat << " htmr=" << human->GetProneTimerElapsedSimMS() << "/" << human->GetStrideTimerElapsedSimMS() << "/" << human->GetThrowTimerElapsedSimMS() << " crouch=" << human->GetCrouchAmount() << "/" << human->GetCrouchAmountOverride() << std::defaultfloat;
				const HeldDevice* fgItem = const_cast<AHuman*>(human)->GetEquippedItem();
				const HeldDevice* bgItem = const_cast<AHuman*>(human)->GetEquippedBGItem();
				out << " fg=" << (fgItem ? fgItem->GetUniqueID() : 0) << " bg=" << (bgItem ? bgItem->GetUniqueID() : 0);
				out << " offhandwait=" << (human->IsWaitingToReloadOffhand() ? 1 : 0);
				if (const HDFirearm* gun = dynamic_cast<const HDFirearm*>(fgItem)) {
					out << " gun=" << gun->GetPresetName() << " rounds=" << gun->GetRoundInMagCount() << " reloading=" << gun->IsReloading() << "/" << gun->DoneReloading() << std::hexfloat << " reloadms=" << gun->GetReloadTimerElapsedSimMS() << "/" << gun->GetReloadTimerLimitMS() << std::defaultfloat << " fire=" << gun->FiredFrame() << gun->FiredLastFrame();
					out << " gate[" << gun->DescribeFireGate() << "]";
				}
				out << " limbs=" << human->GetLimbGroupPositions();
				// Snapshot forensics: the walk paths, the foot groups and the identity the MO-hit layer sees.
				auto fnv = [](uint64_t h, uint32_t v) { return (h ^ v) * 1099511628211ULL; };
				uint64_t pathHash = 1469598103934665603ULL;
				for (const std::string& state: human->GetLimbPathStates(true)) {
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
	m_Speculation = Speculation();
	m_RenderHidden.clear();
	m_LinkRoot = nullptr;
	m_WorldSetAside = nullptr;
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

MovableObject* MovableMan::LookupMOID(MOID whichID) const {
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

MovableObject* MovableMan::GetMOFromID(MOID whichID) {
	MovableObject* found = LookupMOID(whichID);
	if (found && m_Speculation.active) {
		return SpeculativeView(found);
	}
	return found;
}

MOID MovableMan::GetMOIDPixel(int pixelX, int pixelY, const std::vector<int>& moidList) {
	// Note - We loop through the MOs in reverse to make sure that the topmost (last drawn) MO that overlaps the specified coordinates is the one returned.
	for (auto itr = moidList.rbegin(), itrEnd = moidList.rend(); itr < itrEnd; ++itr) {
		MOID moid = *itr;
		const MovableObject* mo = LookupMOID(moid);

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

// Only a destruction may take an object out of a held copy. Unregistering also happens to live objects:
// a restore detaches every Lua-owned tree, and those have to come back with the world that named them.
void MovableMan::ForgetDestroyedObject(MovableObject* mo) {
	{
		std::lock_guard<std::mutex> guard(m_ObjectRegisteredMutex);
		// By address, not by key: the object may have taken a new identity since the copy was made.
		for (auto* held: m_HeldRegistries) {
			std::erase_if(*held, [mo](const auto& entry) { return entry.second == mo; });
		}
	}
	g_LuaMan.ForgetDestroyedRegisteredMO(mo);
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
	activity.reset();
	startActivity.reset();
	sceneRuntime.clear();
	terrain = {};
	runtimeGlobals.clear();
	frameState.clear();
	sceneAreas.areas.clear();
	sceneAreas.navigableAreas.clear();
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
	for (Actor* actor: addedActors) delete actor;
	for (MovableObject* item: addedItems) delete item;
	for (MovableObject* particle: addedParticles) delete particle;
	addedActors.clear(); addedItems.clear(); addedParticles.clear();
	structure.clear();
	joinQuarantine.clear();
	luaGraphs.clear();
	uniqueIDCounter = 0;
}

bool MovableMan::CaptureWorld(WorldSnapshot& out) {
	CompleteQueuedMOIDDrawings();
	WaitForActorsSeeTask();
	out.Clear();
	try {
	struct CaptureAllocationState {
		AudioMan::CheckpointRegistryScope sounds;
		RandomGenerator sim = g_SimRNG, render = g_RenderRNG;
		long uid = MovableObject::GetUniqueIDCounter();
		int cursor = g_LuaMan.GetScriptStateCursor();
		~CaptureAllocationState() { g_SimRNG = sim; g_RenderRNG = render; MovableObject::PinUniqueIDCounter(uid); g_LuaMan.SetScriptStateCursor(cursor); }
	} allocationState;
	out.runtimeGlobals = g_ActivityMan.CaptureRuntimeGlobals();
	out.frameState = g_FrameMan.SaveCheckpoint();
	if (!out.terrain.Capture()) return false;
	out.structure = SaveWorldStructure();
	if (const Scene* scene = g_SceneMan.GetScene()) {
		scene->CaptureAreas(out.sceneAreas);
		out.sceneRuntime = scene->SaveRuntimeCheckpoint();
	}
	std::vector<std::string> luaProblems;
	if (!SerializeScriptGraphs(out.luaGraphs, luaProblems)) {
		for (const std::string& problem: luaProblems) {
			std::cout << "[scriptgraph] capture refused: " << problem << std::endl;
		}
		out.luaGraphs.clear();
		return false;
	}
	const long counter = MovableObject::GetUniqueIDCounter();
	{
		MovableObject::FaithfulCloneScope scope(false);
		if (const Activity* activity = g_ActivityMan.GetActivity()) out.activity.reset(static_cast<Activity*>(activity->Clone()));
		if (const Activity* activity = g_ActivityMan.GetCheckpointStartActivity()) out.startActivity.reset(static_cast<Activity*>(activity->Clone()));
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
		for (const Actor* actor: m_AddedActors) out.addedActors.push_back(dynamic_cast<Actor*>(actor->Clone()));
		for (const MovableObject* item: m_AddedItems) out.addedItems.push_back(dynamic_cast<MovableObject*>(item->Clone()));
		for (const MovableObject* particle: m_AddedParticles) out.addedParticles.push_back(dynamic_cast<MovableObject*>(particle->Clone()));
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
	out.luaStateCursor = allocationState.cursor;
	return true;
	} catch (const std::exception& error) {
		out.Clear();
		std::cout << "[snapshot] capture refused: " << error.what() << std::endl;
		return false;
	}
}

bool MovableMan::RestoreWorldCandidate(const WorldSnapshot& in) {
	if (!LoadWorldStructure(in.structure, true) || !in.terrain.CanRestore() || !g_ActivityMan.RestoreRuntimeGlobals(in.runtimeGlobals, true)) return false;
	struct RestoreFlag {
		bool previous = g_MovableMan.IsRestoringSnapshot();
		RestoreFlag() { g_MovableMan.SetRestoringSnapshot(true); }
		~RestoreFlag() { g_MovableMan.SetRestoringSnapshot(previous); }
	} restoreFlag;
	if (!g_ActivityMan.PrepareCheckpointMaterials(in.runtimeGlobals)) return false;
	if (!in.terrain.Restore()) return false;

	CompleteQueuedMOIDDrawings();
	if (!m_Actors.empty() || !m_Items.empty() || !m_Particles.empty() || !m_AddedActors.empty() || !m_AddedItems.empty() || !m_AddedParticles.empty()) {
		PurgeAllMOs();
	}
	if (Scene* scene = g_SceneMan.GetScene()) {
		scene->RestoreAreas(in.sceneAreas);
		if (!in.sceneRuntime.empty() && !scene->LoadRuntimeCheckpoint(in.sceneRuntime)) return false;
	}
	{
		MovableObject::FaithfulCloneScope scope(true);
		std::unique_ptr<Activity> candidate(in.activity ? static_cast<Activity*>(in.activity->Clone()) : nullptr);
		g_ActivityMan.SwapCheckpointActivity(candidate);
		std::unique_ptr<Activity> startCandidate(in.startActivity ? static_cast<Activity*>(in.startActivity->Clone()) : nullptr);
		g_ActivityMan.SwapCheckpointStartActivity(startCandidate);
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
		for (const Actor* actor: in.addedActors) {
			Actor* live = dynamic_cast<Actor*>(actor->Clone());
			live->AdoptPersistedUniqueID(); m_AddedActors.push_back(live); m_ValidActors.insert(live);
		}
		for (const MovableObject* item: in.addedItems) {
			MovableObject* live = dynamic_cast<MovableObject*>(item->Clone());
			live->AdoptPersistedUniqueID(); m_AddedItems.push_back(live); m_ValidItems.insert(live);
		}
		for (const MovableObject* particle: in.addedParticles) {
			MovableObject* live = dynamic_cast<MovableObject*>(particle->Clone());
			live->AdoptPersistedUniqueID(); m_AddedParticles.push_back(live); m_ValidParticles.insert(live);
		}
	}
	MovableObject::PinUniqueIDCounter(in.uniqueIDCounter);
	m_LockstepJoinQuarantine = in.joinQuarantine;
	if (!LoadWorldStructure(in.structure)) return false;
	if (Activity* activity = g_ActivityMan.GetActivity(); activity && !activity->PrepareCheckpointUI()) return false;
	if (!g_ActivityMan.PrepareCheckpointPrimitives(in.runtimeGlobals)) return false;
	std::string luaError;
	if (!RestoreScriptGraphs(in.luaGraphs, &luaError)) {
		std::cout << "[scriptgraph] restore failed: " << luaError << std::endl;
		return false;
	}
	MovableObject::PinUniqueIDCounter(in.uniqueIDCounter);
	g_LuaMan.SetScriptStateCursor(in.luaStateCursor);
	for (Actor* actor: m_Actors) {
		actor->ResolveFaithfulLinks();
	}
	for (MovableObject* item: m_Items) {
		item->ResolveFaithfulLinks();
	}
	for (MovableObject* particle: m_Particles) {
		particle->ResolveFaithfulLinks();
	}
	for (Actor* actor: m_AddedActors) actor->ResolveFaithfulLinks();
	for (MovableObject* item: m_AddedItems) item->ResolveFaithfulLinks();
	for (MovableObject* particle: m_AddedParticles) particle->ResolveFaithfulLinks();
	if (Activity* activity = g_ActivityMan.GetActivity(); activity && !activity->ResolveCheckpointReferences()) return false;
	if (!g_PrimitiveMan.ResolveCheckpointReferences()) return false;
	ResolvePendingSnapshotLinks();
	RedrawRestoredMOIDs();
	return g_FrameMan.LoadCheckpoint(in.frameState) && g_ActivityMan.RestoreRuntimeGlobals(in.runtimeGlobals);
}

bool MovableMan::SetAsideWorld(WorldSetAside& out, bool holdActivity) {
	if (out.held || m_WorldSetAside) {
		return false;
	}
	CompleteQueuedMOIDDrawings();
	WaitForActorsSeeTask();
	{
	AudioMan::CheckpointRegistryScope captureSounds;
	// The settle comes first, as it does in CaptureWorld: a graph captured before it names the objects it sweeps.
	out.runtimeGlobals = g_ActivityMan.CaptureRuntimeGlobals();
	out.soundRegistrations = g_AudioMan.CaptureCheckpointSoundRegistry();
	std::vector<std::string> luaProblems;
	m_ScriptGraphFailure.clear();
	if (!SerializeScriptGraphs(out.luaGraphs, luaProblems)) {
		for (const std::string& problem: luaProblems) {
			m_ScriptGraphFailure += (m_ScriptGraphFailure.empty() ? "" : "; ") + problem;
		}
		std::cout << "[scriptgraph] set-aside capture refused: " << m_ScriptGraphFailure << std::endl;
		out.luaGraphs.clear();
		return false;
	}
	out.frameState = g_FrameMan.SaveCheckpoint();
	if (holdActivity && !out.terrain.Capture()) return false;
	if (holdActivity && g_SceneMan.GetScene()) out.sceneRuntime = g_SceneMan.GetScene()->SaveRuntimeCheckpoint();
	}
	out.primitiveQueues = std::make_shared<PrimitiveQueuesSetAside>();
	g_PrimitiveMan.SetAsideQueues(*out.primitiveQueues);
	out.musicOwners = g_MusicMan.TakeCheckpointOwners();
	out.uniqueIDCounter = MovableObject::GetUniqueIDCounter();
	g_LuaMan.SwapPathCallbacks(out.pathCallbacks);
	if (Scene* scene = g_SceneMan.GetScene()) {
		scene->SwapAreas(out.sceneAreas);
	}
	out.luaStateCursor = g_LuaMan.GetScriptStateCursor();
	out.scriptRegistrations.clear();
	// A throw before the world is held must not leave a pointer into this frame published.
	struct Publication {
		MovableMan& man;
		WorldSetAside& world;
		~Publication() { if (!world.held) man.ForgetHeldWorld(world); }
	} publication{*this, out};
	const auto stashState = [this, &out](LuaStateWrapper& state) {
		state.RunScriptString("_ScriptGraph.stashObjects()");
		for (const MovableObject* mo: SortedRegisteredMOs(state, true)) {
			if (mo->ObjectScriptsInitialized()) {
				state.StashScriptObject(mo->GetUniqueID());
				out.scriptObjects.emplace_back(&state, mo->GetUniqueID());
			}
		}
		auto& lists = out.scriptRegistrations.emplace_back();
		state.SwapAndHoldRegisteredMOs(lists.first, lists.second);
	};
	stashState(g_LuaMan.GetMasterScriptState());
	for (LuaStateWrapper& state: g_LuaMan.GetThreadedScriptStates()) {
		stashState(state);
	}
	std::scoped_lock lock(m_AddedActorsMutex, m_AddedItemsMutex, m_AddedParticlesMutex, m_AddedAlarmEventsMutex);
	out.actors.swap(m_Actors);
	out.items.swap(m_Items);
	out.particles.swap(m_Particles);
	out.addedActors.swap(m_AddedActors);
	out.addedItems.swap(m_AddedItems);
	out.addedParticles.swap(m_AddedParticles);
	out.alarmEvents.swap(m_AlarmEvents);
	out.addedAlarmEvents.swap(m_AddedAlarmEvents);
	for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
		out.rosters[team].swap(m_ActorRoster[team]);
		out.sortRoster[team] = m_SortTeamRoster[team];
		m_SortTeamRoster[team] = false;
	}
	out.joinQuarantine.swap(m_LockstepJoinQuarantine);
	out.pendingLinks.swap(m_PendingLinkResolves);
	{
		std::unordered_set<const Entity*> visited;
		std::unordered_set<const MovableObject*> heldObjects;
		const auto collect = [&visited, &heldObjects](const auto& roots) {
			for (const Entity* root: roots) CollectOwnedMovableObjects(root, visited, heldObjects);
		};
		collect(out.actors); collect(out.items); collect(out.particles);
		collect(out.addedActors); collect(out.addedItems); collect(out.addedParticles);
		// File restores hold Scene/Activity in an outer transaction. Their owning
		// trees still leave this registry even when that outer hold has not run yet.
		CollectOwnedMovableObjects(g_SceneMan.GetScene(), visited, heldObjects);
		CollectOwnedMovableObjects(g_ActivityMan.GetActivity(), visited, heldObjects);
		CollectOwnedMovableObjects(g_ActivityMan.GetCheckpointStartActivity(), visited, heldObjects);
		std::lock_guard<std::mutex> guard(m_ObjectRegisteredMutex);
		out.knownObjects = m_KnownObjects;
		// The copy waits for the whole hold, so a destruction in that window has to reach it too.
		m_HeldRegistries.push_back(&out.knownObjects);
		for (auto entry = m_KnownObjects.begin(); entry != m_KnownObjects.end();) {
			if (heldObjects.contains(entry->second)) entry = m_KnownObjects.erase(entry);
			else ++entry;
		}
	}
	out.validActors.swap(m_ValidActors);
	out.validItems.swap(m_ValidItems);
	out.validParticles.swap(m_ValidParticles);
	out.moidIndex.swap(m_MOIDIndex);
	out.contiguousActorIDs.swap(m_ContiguousActorIDs);
	for (int team = 0; team < Activity::MaxTeamCount; ++team) out.teamMOIDCount[team] = m_TeamMOIDCount[team];
	g_SceneMan.SwapMOIDGrid(out.moidGrid);
	if (holdActivity) {
		g_ActivityMan.SwapCheckpointActivity(out.activity);
		g_ActivityMan.SwapCheckpointStartActivity(out.startActivity);
		if (Scene* scene = g_SceneMan.GetScene()) {
			out.sceneOwners = std::make_unique<Scene::RuntimeOwners>();
			scene->SwapRuntimeOwners(*out.sceneOwners);
		}
	}
	out.held = true;
	m_WorldSetAside = &out;
	return true;
}

void MovableMan::ForgetHeldWorld(WorldSetAside& in) {
	{
		std::lock_guard<std::mutex> guard(m_ObjectRegisteredMutex);
		std::erase(m_HeldRegistries, &in.knownObjects);
	}
	for (size_t index = 0; index < in.scriptRegistrations.size(); ++index) {
		auto& lists = in.scriptRegistrations[index];
		g_LuaMan.GetStateByIndex(static_cast<int>(index)).ForgetHeldRegisteredMOs(lists.first, lists.second);
	}
}

bool MovableMan::ReinstateWorld(WorldSetAside& in) {
	if (!in.held) {
		return false;
	}
	// Everything that can be judged before the world moves is judged here, so a refusal costs nothing.
	if ((in.terrain.width && !in.terrain.CanRestore()) || !g_ActivityMan.RestoreRuntimeGlobals(in.runtimeGlobals, true)) {
		std::cout << "[scriptgraph] reinstate refused before the world moved" << std::endl;
		return false;
	}
	CompleteQueuedMOIDDrawings();
	WaitForActorsSeeTask();
	// discardState and PurgeAllMOs run below, so the record stays published until it is put back.
	struct Withdraw {
		MovableMan& man;
		WorldSetAside& world;
		bool done = false;
		void Now() { if (!done) { man.ForgetHeldWorld(world); done = true; } }
		~Withdraw() { Now(); }
	} withdraw{*this, in};
	// The re-run never happened: every scripted object of its world (nested ones included) drops its script object without Destroy, and the originals' slots come back.
	const auto isOriginal = [&in](const MovableObject* mo) {
		const auto known = in.knownObjects.find(mo->GetUniqueID());
		return known != in.knownObjects.end() && known->second == mo;
	};
	const auto discardState = [&isOriginal](LuaStateWrapper& state) {
		for (MovableObject* mo: SortedRegisteredMOs(state, true)) {
			if (isOriginal(mo)) {
				continue;
			}
			if (mo->ObjectScriptsInitialized()) {
				state.RunScriptString("_ScriptedObjects[\"" + std::to_string(mo->GetUniqueID()) + "\"] = nil;");
			}
			mo->DiscardScriptState();
		}
	};
	discardState(g_LuaMan.GetMasterScriptState());
	for (LuaStateWrapper& state: g_LuaMan.GetThreadedScriptStates()) {
		discardState(state);
	}
	PurgeAllMOs();
	std::unique_ptr<Activity> rejectedActivity;
	std::unique_ptr<Activity> rejectedStart;
	std::unique_ptr<Scene::RuntimeOwners> rejectedSceneOwners;
	if (in.activity) {
		g_ActivityMan.SwapCheckpointActivity(rejectedActivity);
		g_ActivityMan.SwapCheckpointActivity(in.activity);
		g_ActivityMan.SwapCheckpointStartActivity(rejectedStart);
		g_ActivityMan.SwapCheckpointStartActivity(in.startActivity);
	}
	if (in.sceneOwners && g_SceneMan.GetScene()) {
		g_SceneMan.GetScene()->SwapRuntimeOwners(*in.sceneOwners);
		rejectedSceneOwners = std::move(in.sceneOwners);
	}
	withdraw.Now();
	{
		std::lock_guard<std::mutex> guard(m_ObjectRegisteredMutex);
		m_KnownObjects = std::move(in.knownObjects);
	}
	for (size_t index = 0; index < in.scriptRegistrations.size(); ++index) {
		auto& lists = in.scriptRegistrations[index];
		g_LuaMan.GetStateByIndex(static_cast<int>(index)).SwapRegisteredMOs(lists.first, lists.second);
	}
	in.scriptRegistrations.clear();
	for (const auto& [state, uid]: in.scriptObjects) {
		state->UnstashScriptObject(uid);
	}
	in.scriptObjects.clear();
	std::scoped_lock lock(m_AddedActorsMutex, m_AddedItemsMutex, m_AddedParticlesMutex, m_AddedAlarmEventsMutex);
	m_Actors.swap(in.actors);
	m_Items.swap(in.items);
	m_Particles.swap(in.particles);
	m_AddedActors.swap(in.addedActors);
	m_AddedItems.swap(in.addedItems);
	m_AddedParticles.swap(in.addedParticles);
	m_AlarmEvents.swap(in.alarmEvents);
	m_AddedAlarmEvents.swap(in.addedAlarmEvents);
	for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
		m_ActorRoster[team].swap(in.rosters[team]);
		m_SortTeamRoster[team] = in.sortRoster[team];
	}
	m_LockstepJoinQuarantine.swap(in.joinQuarantine);
	m_PendingLinkResolves.swap(in.pendingLinks);
	in.pendingLinks.clear();
	for (Actor* actor: m_Actors) {
		m_ValidActors.insert(actor);
	}
	for (MovableObject* item: m_Items) {
		m_ValidItems.insert(item);
	}
	for (MovableObject* particle: m_Particles) {
		m_ValidParticles.insert(particle);
	}
	for (Actor* actor: m_AddedActors) {
		m_ValidActors.insert(actor);
	}
	for (MovableObject* item: m_AddedItems) {
		m_ValidItems.insert(item);
	}
	for (MovableObject* particle: m_AddedParticles) {
		m_ValidParticles.insert(particle);
	}
	m_ValidActors.swap(in.validActors);
	m_ValidItems.swap(in.validItems);
	m_ValidParticles.swap(in.validParticles);
	m_MOIDIndex.swap(in.moidIndex);
	m_ContiguousActorIDs.swap(in.contiguousActorIDs);
	for (int team = 0; team < Activity::MaxTeamCount; ++team) m_TeamMOIDCount[team] = in.teamMOIDCount[team];
	g_SceneMan.SwapMOIDGrid(in.moidGrid);
	in.held = false;
	m_WorldSetAside = nullptr;
	// Past this point the candidate world is gone, so a failure is reported rather than returned:
	// every remaining step still runs, or the originals come back only half restored.
	bool restored = g_ActivityMan.PrepareCheckpointMaterials(in.runtimeGlobals);
	if (Scene* scene = g_SceneMan.GetScene()) {
		scene->SwapAreas(in.sceneAreas);
		in.sceneAreas.areas.clear();
		if (!in.sceneRuntime.empty()) restored = scene->LoadRuntimeCheckpoint(in.sceneRuntime, false, false) && restored;
	}
	// The originals' Lua state as it was when they stepped aside, entity references pointing at them again.
	g_LuaMan.SwapPathCallbacks(in.pathCallbacks);
	in.pathCallbacks.reset();
	if (in.primitiveQueues) g_PrimitiveMan.ReinstateQueues(*in.primitiveQueues);
	auto rejectedPrimitives = std::move(in.primitiveQueues);
	std::string luaError;
	if (!RestoreScriptGraphs(in.luaGraphs, &luaError, true)) {
		std::cout << "[scriptgraph] reinstate failed: " << luaError << std::endl;
		restored = false;
	}
	// The stash only has to outlive the candidate. Held past that it keeps unreachable script
	// objects alive into the next capture, which then names sound owners no restore can produce.
	for (size_t index = 0; index < in.luaGraphs.size(); ++index) {
		g_LuaMan.GetStateByIndex(static_cast<int>(index)).RunScriptString("_ScriptGraph.releaseObjects()");
	}
	MovableObject::PinUniqueIDCounter(in.uniqueIDCounter);
	g_LuaMan.SetScriptStateCursor(in.luaStateCursor);
	if (in.terrain.width) restored = in.terrain.Restore() && restored;
	restored = g_MusicMan.RestoreCheckpointOwners(in.musicOwners) && restored;
	g_AudioMan.RestoreCheckpointSoundRegistry(std::move(in.soundRegistrations));
	restored = g_FrameMan.LoadCheckpoint(in.frameState) && restored;
	return g_ActivityMan.RestoreRuntimeGlobals(in.runtimeGlobals) && restored;
}

bool MovableMan::SerializeScriptGraphs(std::vector<std::string>& graphs, std::vector<std::string>& problems) const {
	AudioMan::CheckpointRegistryScope captureSounds;
	struct PathCapture {
		PathCapture() { g_LuaMan.BeginPathCallbackCapture(); }
		~PathCapture() { g_LuaMan.EndPathCallbackCapture(); }
	} pathCapture;
	graphs.clear();
	bool complete = true;
	graphs.emplace_back();
	complete = g_LuaMan.GetMasterScriptState().SerializeScriptGraph(graphs.back(), problems) && complete;
	for (LuaStateWrapper& state: g_LuaMan.GetThreadedScriptStates()) {
		graphs.emplace_back();
		complete = state.SerializeScriptGraph(graphs.back(), problems) && complete;
	}
	return complete;
}

std::vector<MovableObject*> MovableMan::SnapshotKnownObjects() {
	std::lock_guard<std::mutex> guard(m_ObjectRegisteredMutex);
	std::vector<MovableObject*> objects;
	objects.reserve(m_KnownObjects.size());
	for (const auto& [uid, object]: m_KnownObjects) {
		objects.push_back(object);
	}
	return objects;
}

bool MovableMan::ValidateScriptGraphs(const std::vector<std::string>& graphs, std::string* error) {
	std::vector<std::string> errors;
	if (graphs.size() > g_LuaMan.GetThreadedScriptStates().size() + 1) {
		errors.emplace_back("the snapshot contains more script states than this runtime");
	} else {
		for (size_t index = 0; index < graphs.size(); ++index) {
			if (!graphs[index].empty()) g_LuaMan.GetStateByIndex(static_cast<int>(index)).ValidateScriptGraph(graphs[index], errors);
		}
	}
	if (!errors.empty()) {
		if (error) {
			error->clear();
			for (const auto& message: errors) *error += (error->empty() ? "" : "; ") + message;
		}
		return false;
	}
	return true;
}

bool MovableMan::RestoreScriptGraphs(const std::vector<std::string>& graphs, std::string* error, bool reuseHeld) {
	if (!ValidateScriptGraphs(graphs, error)) return false;
	std::vector<std::string> errors;
	if (!reuseHeld) g_LuaMan.ResetPathCallbacks();
	struct AllocationState {
		long uidCounter = MovableObject::GetUniqueIDCounter();
		int luaStateCursor = g_LuaMan.GetScriptStateCursor();
		~AllocationState() {
			MovableObject::PinUniqueIDCounter(uidCounter);
			g_LuaMan.SetScriptStateCursor(luaStateCursor);
		}
	} allocationState;
	// A receiving peer may never have captured its VM. Detach every old Lua-owned
	// tree across all states before any replacement can adopt a saved native ID.
	if (!reuseHeld) {
		for (size_t index = 0; index < graphs.size(); ++index) {
			if (!graphs[index].empty()) g_LuaMan.GetStateByIndex(static_cast<int>(index)).ReleaseScriptOwnedObjects();
		}
	}
	for (size_t index = 0; index < graphs.size(); ++index) {
		if (!graphs[index].empty()) {
			g_LuaMan.GetStateByIndex(static_cast<int>(index)).PrepareScriptGraph(&graphs[index], errors, reuseHeld);
		}
	}
	if (errors.empty()) {
		for (size_t index = 0; index < graphs.size(); ++index) {
			if (!graphs[index].empty()) {
				g_LuaMan.GetStateByIndex(static_cast<int>(index)).PrepareScriptGraph(nullptr, errors);
			}
		}
	}
	const bool prepared = errors.empty();
	for (size_t index = 0; index < graphs.size(); ++index) {
		if (graphs[index].empty()) {
			continue;
		}
		LuaStateWrapper& state = g_LuaMan.GetStateByIndex(static_cast<int>(index));
		if (prepared) {
			state.RestoreScriptGraph(graphs[index], errors, reuseHeld);
		}
		state.RunScriptString("_ScriptGraph.clearPrepared()");
		if (reuseHeld) {
			state.RunScriptString("_ScriptGraph.releaseObjects()");
		}
	}
	if (!errors.empty()) {
		std::string joined;
		for (const std::string& message: errors) {
			joined += (joined.empty() ? "" : "; ") + message;
		}
		if (error) {
			*error = joined;
		}
		return false;
	}
	if (!reuseHeld) g_LuaMan.ResumePathCallbacks();
	return true;
}

bool MovableMan::IsKnownObject(const MovableObject* object) {
	std::lock_guard<std::mutex> guard(m_ObjectRegisteredMutex);
	for (const auto& [uid, known]: m_KnownObjects) {
		if (known == object) {
			return true;
		}
	}
	return false;
}

std::string MovableMan::DescribeLuaIdentity() const {
	std::string out;
	const auto describe = [&out](const std::string& name, LuaStateWrapper& state) {
		out += name;
		for (const MovableObject* mo: SortedRegisteredMOs(state)) {
			out += " " + std::to_string(mo->GetUniqueID()) + "@" + state.DescribeScriptObjectIdentity(mo->GetUniqueID());
		}
		out += "\n";
	};
	describe("master", g_LuaMan.GetMasterScriptState());
	int index = 0;
	for (LuaStateWrapper& state: g_LuaMan.GetThreadedScriptStates()) {
		describe("thread" + std::to_string(index++), state);
	}
	return out;
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

std::string MovableMan::DescribeAddedSince(const AddQueueMark& mark) const {
	std::string out;
	const auto append = [&out](const MovableObject* mo) { out += (out.empty() ? "" : ",") + mo->GetPresetName(); };
	for (size_t i = mark.actors; i < m_AddedActors.size(); ++i) {
		append(m_AddedActors[i]);
	}
	for (size_t i = mark.items; i < m_AddedItems.size(); ++i) {
		append(m_AddedItems[i]);
	}
	for (size_t i = mark.particles; i < m_AddedParticles.size(); ++i) {
		append(m_AddedParticles[i]);
	}
	return out;
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

void MovableMan::BeginSpeculation() {
	RTEAssert(!m_Speculation.active, "Speculation began while another speculation was active.");
	m_Speculation.active = true;
	g_SimChecksum.SetSuppressed(true);
	m_Speculation.mark = MarkAddQueues();
	for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
		m_Speculation.rosters[team] = m_ActorRoster[team];
		m_Speculation.sortRoster[team] = m_SortTeamRoster[team];
	}
}

void MovableMan::EndSpeculation(std::vector<MovableObject*>* takenResidents) {
	if (!m_Speculation.active) {
		return;
	}
	m_Speculation.active = false;
	g_SimChecksum.SetSuppressed(false);
	m_LinkRoot = nullptr;
	DiscardAddedSince(m_Speculation.mark);
	for (auto& [resident, shadow]: m_Speculation.shadows) {
		if (shadow.inWorld) {
			delete shadow.object;
		}
	}
	for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
		m_ActorRoster[team] = m_Speculation.rosters[team];
		m_SortTeamRoster[team] = m_Speculation.sortRoster[team];
		m_Speculation.rosters[team].clear();
	}
	if (takenResidents) {
		*takenResidents = m_Speculation.taken;
	}
	m_Speculation.shadows.clear();
	m_Speculation.residents.clear();
	m_Speculation.taken.clear();
}

int MovableMan::ResidentKind(const MovableObject* mo) const {
	if (!mo) {
		return 0;
	}
	// Whatever entered the add queues during the speculation is speculative itself, not a resident.
	if (m_Speculation.active) {
		for (size_t i = m_Speculation.mark.actors; i < m_AddedActors.size(); ++i) {
			if (m_AddedActors[i] == mo) {
				return 0;
			}
		}
		for (size_t i = m_Speculation.mark.items; i < m_AddedItems.size(); ++i) {
			if (m_AddedItems[i] == mo) {
				return 0;
			}
		}
		for (size_t i = m_Speculation.mark.particles; i < m_AddedParticles.size(); ++i) {
			if (m_AddedParticles[i] == mo) {
				return 0;
			}
		}
	}
	if (m_ValidActors.count(mo) > 0) {
		return 1;
	}
	if (m_ValidItems.count(mo) > 0) {
		return 2;
	}
	if (m_ValidParticles.count(mo) > 0) {
		return 3;
	}
	return 0;
}

MovableObject* MovableMan::ShadowOf(MovableObject* resident) {
	if (const auto existing = m_Speculation.shadows.find(resident); existing != m_Speculation.shadows.end()) {
		return existing->second.object;
	}
	const int kind = ResidentKind(resident);
	MovableObject* shadow = nullptr;
	{
		MovableObject::FaithfulCloneScope scope(false);
		const long counter = MovableObject::GetUniqueIDCounter();
		shadow = dynamic_cast<MovableObject*>(resident->Clone());
		MovableObject::PinUniqueIDCounter(counter);
	}
	if (!shadow) {
		ReportSpeculationViolation("shadowing", resident);
		return nullptr;
	}
	static const bool traceShadows = std::getenv("CC_LOCALPRED_TRACE") != nullptr;
	if (traceShadows) {
		std::cout << "[speculation] shadow of " << resident->GetPresetName() << " uid=" << resident->GetUniqueID() << " kind=" << kind << std::endl;
	}
	Speculation::Shadow& entry = m_Speculation.shadows[resident];
	entry.object = shadow;
	entry.kind = kind;
	entry.inWorld = true;
	m_Speculation.residents[shadow] = resident;
	++m_SpeculationStats.shadows;
	MovableObject* previousRoot = m_LinkRoot;
	m_LinkRoot = shadow;
	shadow->ResolveFaithfulLinks();
	m_LinkRoot = previousRoot;
	return shadow;
}

MovableObject* MovableMan::SpeculativeView(MovableObject* found) {
	MovableObject* root = found->GetRootParent();
	if (!IsResident(root)) {
		return found;
	}
	MovableObject* shadowRoot = ShadowOf(root);
	if (!shadowRoot || root == found) {
		return shadowRoot;
	}
	if (MovableObject* part = shadowRoot->FindPartByUniqueID(found->GetUniqueID())) {
		return part;
	}
	return shadowRoot;
}

MovableObject* MovableMan::TakeShadow(MovableObject* mo, int kind) {
	const auto resident = m_Speculation.residents.find(mo);
	if (resident == m_Speculation.residents.end()) {
		return nullptr;
	}
	Speculation::Shadow& shadow = m_Speculation.shadows.at(resident->second);
	if (!shadow.inWorld || shadow.kind != kind) {
		return nullptr;
	}
	shadow.inWorld = false;
	m_Speculation.taken.push_back(resident->second);
	++m_SpeculationStats.taken;
	mo->SetAsAddedToMovableMan(false);
	return mo;
}

void MovableMan::ReportSpeculationViolation(const char* what, const MovableObject* mo) {
	++m_SpeculationStats.violations;
	const std::string subject = mo ? mo->GetPresetName() + " uid=" + std::to_string(mo->GetUniqueID()) : std::string("the world");
	if (m_SpeculationStats.violations <= 3) {
		std::cout << "[speculation] VIOLATION: " << what << " " << subject << " from speculative execution" << std::endl;
	}
#ifdef DEBUG_BUILD
	RTEAssert(false, "Speculative execution wrote to the world: " + std::string(what) + " " + subject);
#endif
}

void MovableMan::ReportControllerBoundaryViolation(const char* what, const Actor* actor) {
	++m_ControllerBoundaryStats.directWrites;
	const std::string subject = actor ? actor->GetPresetName() + " uid=" + std::to_string(actor->GetUniqueID()) : std::string("an actor");
	if (m_ControllerBoundaryStats.directWrites <= 3) {
		std::cout << "[controller-boundary] VIOLATION: the AI pass wrote " << what << " of " << subject << " directly" << std::endl;
	}
#ifdef DEBUG_BUILD
	RTEAssert(false, "The AI pass wrote to the canonical actor outside the controller boundary: " + std::string(what) + " " + subject);
#endif
}

void MovableMan::HideForRender(const MovableObject* mo, bool hidden) {
	if (hidden) {
		m_RenderHidden.insert(mo);
	} else {
		m_RenderHidden.erase(mo);
	}
}

std::string MovableMan::DescribeTeamRosters() const {
	std::string out;
	for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
		out += "team" + std::to_string(team) + (m_SortTeamRoster[team] ? " sort" : "") + ":";
		for (const Actor* actor: m_ActorRoster[team]) {
			out += " " + std::to_string(actor->GetUniqueID());
		}
		out += "\n";
	}
	return out;
}

bool MovableMan::RunPurgeSelfTest() {
	if (m_Actors.empty()) return false;
	auto report = std::make_unique<MOPixel>();
	if (report->Create() < 0) return false;
	const auto* actorPreset = g_PresetMan.GetEntityPreset("AHuman", "Green Dummy", "Base.rte");
	const auto* itemPreset = g_PresetMan.GetEntityPreset("HDFirearm", "Old Stock Battle Rifle", "Base.rte");
	if (!actorPreset || !itemPreset) return false;
	auto actor = std::unique_ptr<Actor>(dynamic_cast<Actor*>(actorPreset->Clone()));
	auto item = std::unique_ptr<HeldDevice>(dynamic_cast<HeldDevice*>(itemPreset->Clone()));
	auto particle = std::make_unique<MOPixel>();
	if (!actor || !item || particle->Create() < 0) return false;
	MovableObject* objects[] = {m_Actors.front(), actor.get(), item.get(), particle.get()};
	std::vector<long> ids;
	for (size_t i = 0; i < std::size(objects); ++i) {
		MovableObject* object = objects[i];
		object->SetNumberValue("purge_report", report->GetUniqueID());
		object->SetNumberValue("purge_kind", i);
		if (object->LoadScript(g_PresetMan.GetFullModulePath("UserScenes.rte/mod_purge.lua")) < 0 ||
		    object->RunScriptedFunctionInAppropriateScripts("OnSave") < 0) return false;
		ids.push_back(object->GetUniqueID());
	}
	AddActor(actor.release());
	AddItem(item.release());
	AddParticle(particle.release());
	PurgeAllMOs();
	const bool callbacks = report->GetNumberValue("purge_callbacks") == std::size(objects);
	bool removed = true, spawned = true;
	for (size_t i = 0; i < ids.size(); ++i) {
		removed &= FindObjectByUniqueID(ids[i]) == nullptr;
		const long spawnedUID = static_cast<long>(report->GetNumberValue("purge_spawn_" + std::to_string(i)));
		spawned &= spawnedUID > 0 && FindObjectByUniqueID(spawnedUID) == nullptr;
	}
	const bool empty = m_Actors.empty() && m_Items.empty() && m_Particles.empty() &&
	                   m_AddedActors.empty() && m_AddedItems.empty() && m_AddedParticles.empty();
	const bool retained = FindObjectByUniqueID(report->GetUniqueID()) == report.get();
	const bool passed = callbacks && removed && spawned && empty && retained;
	std::cout << "[purge-selftest] " << (passed ? "PASS" : "FAIL") << " callbacks=" << callbacks << " removed=" << removed
	          << " spawned=" << spawned << " empty=" << empty << " retained=" << retained << std::endl;
	return passed;
}

void MovableMan::PurgeAllMOs() {
	if (m_Speculation.active) {
		ReportSpeculationViolation("purging", nullptr);
		return;
	}
	if (m_PurgingAllMOs) return;
	m_PurgingAllMOs = true;
	struct FinishPurge {
		bool& active;
		~FinishPurge() { active = false; }
	} finishPurge{m_PurgingAllMOs};

	// Keep other objects alive while Destroy callbacks run. A callback can transfer
	// ownership or add objects, so iterate stable identities and drain new roots too.
	std::unordered_set<long> visited;
	for (;;) {
		std::vector<long> callbacks;
		const auto collect = [&visited, &callbacks](const auto& objects) {
			for (const MovableObject* object: objects) {
				if (visited.insert(object->GetUniqueID()).second) callbacks.push_back(object->GetUniqueID());
			}
		};
		collect(m_Actors);
		collect(m_Items);
		collect(m_Particles);
		collect(m_AddedActors);
		collect(m_AddedItems);
		collect(m_AddedParticles);
		if (callbacks.empty()) break;
		for (long uid: callbacks) {
			if (MovableObject* object = FindObjectByUniqueID(uid); object && ValidMO(object)) object->DestroyScriptState();
		}
	}
	const auto deleteObjects = [](auto& objects) {
		while (!objects.empty()) {
			auto* object = objects.front();
			objects.pop_front();
			delete object;
		}
	};
	deleteObjects(m_Actors);
	deleteObjects(m_Items);
	deleteObjects(m_Particles);
	deleteObjects(m_AddedActors);
	deleteObjects(m_AddedItems);
	deleteObjects(m_AddedParticles);
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
	for (AlarmEvent* event: m_AddedAlarmEvents) delete event;
	for (AlarmEvent* event: m_AlarmEvents) delete event;
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
	for (Actor* actor: m_AddedActors) {
		actor->ApplyPersistedControllerMode();
	}
}

void MovableMan::AddActor(Actor* actorToAdd) {
	if (actorToAdd && g_ActivityMan.GetActivity()) {
		if (!ScenarioRunner::GetArgs().testScript.empty()) {
			if (const int status = actorToAdd->LoadScript(g_PresetMan.GetFullModulePath(ScenarioRunner::GetArgs().testScript), true); status < 0 && status != -3) {
				std::cout << "[test-script] ERROR: could not attach " << ScenarioRunner::GetArgs().testScript << " to " << actorToAdd->GetPresetName() << " (" << status << ")" << std::endl;
			}
		}
		actorToAdd->SetAsAddedToMovableMan();
		if (!m_RestoringSnapshot) {
			actorToAdd->CorrectAttachableAndWoundPositionsAndRotations();
		}

		if (m_RestoringSnapshot) {
			// A snapshot resident enters exactly as captured.
			actorToAdd->AdoptPersistedUniqueID();
			m_PendingLinkResolves.push_back(actorToAdd);
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
		if (!m_RestoringSnapshot) {
			itemToAdd->CorrectAttachableAndWoundPositionsAndRotations();
		}

		if (m_RestoringSnapshot) {
			itemToAdd->AdoptPersistedUniqueID();
			m_PendingLinkResolves.push_back(itemToAdd);
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
		if (MOSRotating* particleToAddAsMOSRotating = dynamic_cast<MOSRotating*>(particleToAdd); particleToAddAsMOSRotating && !m_RestoringSnapshot) {
			particleToAddAsMOSRotating->CorrectAttachableAndWoundPositionsAndRotations();
		}

		if (m_RestoringSnapshot) {
			particleToAdd->AdoptPersistedUniqueID();
			m_PendingLinkResolves.push_back(particleToAdd);
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

	if (pActorToRem && m_Speculation.active) {
		if (MovableObject* shadow = TakeShadow(pActorToRem, 1)) {
			return dynamic_cast<Actor*>(shadow);
		}
		if (ResidentKind(pActorToRem) == 1) {
			ReportSpeculationViolation("removing", pActorToRem);
			return nullptr;
		}
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

	if (pItemToRem && m_Speculation.active) {
		if (MovableObject* shadow = TakeShadow(pItemToRem, 2)) {
			return shadow;
		}
		if (ResidentKind(pItemToRem) == 2) {
			ReportSpeculationViolation("removing", pItemToRem);
			return nullptr;
		}
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

	if (pMOToRem && m_Speculation.active) {
		if (MovableObject* shadow = TakeShadow(pMOToRem, 3)) {
			return shadow;
		}
		if (ResidentKind(pMOToRem) == 3) {
			ReportSpeculationViolation("removing", pMOToRem);
			return nullptr;
		}
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

	if (pActor->GetController()->IsSeatedByPlayer()) {
		g_ActivityMan.GetActivity()->LoseControlOfActor(pActor->GetController()->GetSeatPlayer());
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

bool MovableMan::ValidMO(const MovableObject* pMOToCheck) const {
	if (!pMOToCheck) {
		return false;
	}
	if (m_Speculation.active) {
		if (const auto shadow = m_Speculation.residents.find(pMOToCheck); shadow != m_Speculation.residents.end()) {
			return m_Speculation.shadows.at(shadow->second).inWorld;
		}
		if (m_Speculation.shadows.count(pMOToCheck) > 0) {
			return false;
		}
	}
	return m_ValidActors.find(pMOToCheck) != m_ValidActors.end() ||
	       m_ValidItems.find(pMOToCheck) != m_ValidItems.end() ||
	       m_ValidParticles.find(pMOToCheck) != m_ValidParticles.end();
}

bool MovableMan::IsActor(const MovableObject* pMOToCheck) {
	if (!pMOToCheck) {
		return false;
	}
	if (m_Speculation.active) {
		if (const auto shadow = m_Speculation.residents.find(pMOToCheck); shadow != m_Speculation.residents.end()) {
			const Speculation::Shadow& entry = m_Speculation.shadows.at(shadow->second);
			return entry.inWorld && entry.kind == 1;
		}
		if (m_Speculation.shadows.count(pMOToCheck) > 0) {
			return false;
		}
	}
	return m_ValidActors.find(pMOToCheck) != m_ValidActors.end();
}

bool MovableMan::IsDevice(const MovableObject* pMOToCheck) {
	if (!pMOToCheck) {
		return false;
	}
	if (m_Speculation.active) {
		if (const auto shadow = m_Speculation.residents.find(pMOToCheck); shadow != m_Speculation.residents.end()) {
			const Speculation::Shadow& entry = m_Speculation.shadows.at(shadow->second);
			return entry.inWorld && entry.kind == 2;
		}
		if (m_Speculation.shadows.count(pMOToCheck) > 0) {
			return false;
		}
	}
	return m_ValidItems.find(pMOToCheck) != m_ValidItems.end();
}

bool MovableMan::IsParticle(const MovableObject* pMOToCheck) {
	if (!pMOToCheck) {
		return false;
	}
	if (m_Speculation.active) {
		if (const auto shadow = m_Speculation.residents.find(pMOToCheck); shadow != m_Speculation.residents.end()) {
			const Speculation::Shadow& entry = m_Speculation.shadows.at(shadow->second);
			return entry.inWorld && entry.kind == 3;
		}
		if (m_Speculation.shadows.count(pMOToCheck) > 0) {
			return false;
		}
	}
	return m_ValidParticles.find(pMOToCheck) != m_ValidParticles.end();
}

MovableObject* MovableMan::FindObjectByUniqueID(long int id) {
	if (m_LinkRoot) {
		if (MovableObject* part = m_LinkRoot->FindPartByUniqueID(id)) {
			return part;
		}
	}
	const auto known = m_KnownObjects.find(id);
	MovableObject* found = known == m_KnownObjects.end() ? nullptr : known->second;
	if (found && m_Speculation.active) {
		return SpeculativeView(found);
	}
	return found;
}

bool MovableMan::IsOfActor(MOID checkMOID) {
	if (checkMOID == g_NoMOID)
		return false;

	bool found = false;
	MovableObject* pMO = LookupMOID(checkMOID);

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
	MovableObject* pMO = LookupMOID(checkMOID);
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
	// Sort the added queues before the drain so the transfer order is stable across runs; a restored world arrives in its saved live order.
	if (m_PendingLinkResolves.empty()) {
		std::sort(m_AddedActors.begin(), m_AddedActors.end(), MOUniqueIDLess());
		std::sort(m_AddedItems.begin(), m_AddedItems.end(), MOUniqueIDLess());
		std::sort(m_AddedParticles.begin(), m_AddedParticles.end(), MOUniqueIDLess());
	}

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

	ResolvePendingSnapshotLinks();
	m_PendingLinkResolves.clear();
}

void MovableMan::ResolvePendingSnapshotLinks() {
	// Keep the pending cohort until absorption so the saved resident order is retained.
	for (MovableObject* mo: m_PendingLinkResolves) {
		if (ValidMO(mo)) {
			mo->ResolveFaithfulLinks();
		}
	}
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
				UpdateStage(actor, true);
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
			TravelStage(actor, true);
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

// An actor's travel and update draw from a per-actor per-tick stream, so a preview of the actor draws
// exactly what its canonical tick will.
void MovableMan::TravelStage(MovableObject* mo, bool actor) {
	DeterministicMORNGScope rng(mo->GetUniqueID(), Hash("ActorTravel"), actor);
	static const uint64_t soundPhase = Hash("Travel");
	SoundSimulationScope sounds(mo->GetUniqueID(), soundPhase);
	if (!mo->IsUpdated()) {
		mo->ApplyForces();
		mo->PreTravel();
		mo->Travel();
		mo->PostTravel();
	}
	mo->NewFrame();
}

// The pie command lands before the pre-controller pass that consumes its controller states, as the activity used to do.
void MovableMan::PreControllerStage(Actor* actor) {
	static const uint64_t soundPhase = Hash("PreController");
	SoundSimulationScope sounds(actor->GetUniqueID(), soundPhase);
	actor->HandlePendingPieCommand();
	actor->PreControllerUpdate();
}

void MovableMan::UpdateStage(MovableObject* mo, bool actor) {
	DeterministicMORNGScope rng(mo->GetUniqueID(), Hash("ActorUpdate"), actor);
	static const uint64_t soundPhase = Hash("Update");
	SoundSimulationScope sounds(mo->GetUniqueID(), soundPhase);
	mo->Update();

	g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ScriptsUpdate);
	mo->UpdateScripts();
	g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ScriptsUpdate);

	mo->ApplyImpulses();
}

void MovableMan::PostUpdateStage(MovableObject* mo) {
	static const uint64_t soundPhase = Hash("PostUpdate");
	SoundSimulationScope sounds(mo->GetUniqueID(), soundPhase);
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

		// Under lockstep the AI pass may not change the canonical actor: its aim and facing writes are
		// taken as one-shot intents and undone here, its equip calls become commands, and every peer
		// (this one included) applies them at the committed tick.
		struct DirectState {
			Actor* actor;
			float aim;
			bool flipped;
			int64_t fg;
			int64_t bg;
		};
		std::vector<DirectState> directBefore;
		if (lockstepActive) {
			for (Actor* actor: m_Actors) {
				if (isLocalControllerActor(actor)) {
					const AHuman* human = dynamic_cast<const AHuman*>(actor);
					directBefore.push_back({actor, actor->GetAimAngle(false), actor->IsHFlipped(),
					                        human && human->GetEquippedItem() ? static_cast<int64_t>(human->GetEquippedItem()->GetUniqueID()) : 0,
					                        human && human->GetEquippedBGItem() ? static_cast<int64_t>(human->GetEquippedBGItem()->GetUniqueID()) : 0});
				}
			}
		}
		auto drainDeferredEquips = [&]() {
			// The equip calls the AI queued, in MOID order: performed now, or sent as commands under lockstep.
			for (Actor* actor: m_Actors) {
				if (!isLocalControllerActor(actor)) {
					continue;
				}
				AHuman* human = dynamic_cast<AHuman*>(actor);
				if (!human) {
					continue;
				}
				for (const AHuman::DeferredEquip& equip: human->TakePendingDeferredEquips()) {
					if (!lockstepActive) {
						human->ExecuteDeferredEquip(equip);
						continue;
					}
					NetGameAIEquip command;
					command.actorUID = static_cast<int64_t>(human->GetUniqueID());
					command.team = human->GetTeam();
					command.op = static_cast<uint8_t>(equip.op);
					command.depositToFront = equip.depositToFront;
					command.group = equip.group;
					command.excludeGroup = equip.excludeGroup;
					command.moduleName = equip.moduleName;
					command.presetName = equip.presetName;
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{ScenarioRunner::GetLockstepLocalPeerId(), std::move(command)});
					++m_ControllerBoundaryStats.equipCommands;
				}
			}
		};
		auto drainDeferredSoundOps = [&]() {
			// The sound calls the AI queued, in checkpoint-identity order: performed now, or sent as
			// commands under lockstep. A container the AI only read hands out nothing.
			for (SoundContainer* container: g_AudioMan.TakePendingSoundOpContainers()) {
				for (const SoundContainer::PendingOp& op: container->TakePendingSoundOps()) {
					if (!lockstepActive) {
						SoundSimulationScope sounds(static_cast<uint64_t>(op.actorUID), Hash("DeferredSoundOp"), SoundExecutionDomain::SharedSimulation, g_AudioMan.NextDeferredSoundOpOrdinal());
						container->ApplyPendingSoundOp(op);
						continue;
					}
					if (!op.actorUID) {
						g_ConsoleMan.PrintString("ERROR: Dropped a deferred sound call that no AI actor owns");
						continue;
					}
					NetGameSoundOp command;
					command.actorUID = op.actorUID;
					command.team = op.team;
					command.soundIdentity = container->GetCheckpointIdentity();
					command.op = static_cast<uint8_t>(op.op);
					command.property = static_cast<uint8_t>(op.property);
					command.player = op.player;
					command.value = op.value;
					command.x = op.x;
					command.y = op.y;
					command.soundSetPath = op.soundSetPath;
					command.payload = op.payload;
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{ScenarioRunner::GetLockstepLocalPeerId(), std::move(command)});
					++m_ControllerBoundaryStats.soundCommands;
				}
			}
		};
		// Scripts initialize here, at one deterministic point on the sim thread on every peer, so a
		// freshly added actor keeps its first AI pass and every peer numbers its sound scopes alike.
		g_LuaMan.SetThreadLuaStateOverride(&g_LuaMan.GetMasterScriptState());
		for (Actor* actor: m_Actors) {
			actor->InitializeObjectScriptsIfNeeded();
		}
		// The shared hooks' sound writes land here, after the last shared script of the tick and before
		// the first AI one, so everything the drain finds afterwards was written by an AI pass.
		g_AudioMan.SettleSharedSoundWrites();
		for (Actor* actor: m_Actors) {
			if (isLocalControllerActor(actor) && actor->ObjectScriptsInitialized() && actor->GetLuaState() == &g_LuaMan.GetMasterScriptState() && actor->GetController()->ShouldUpdateAIThisFrame()) {
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
				                                                     if (isLocalControllerActor(actor) && actor->ObjectScriptsInitialized() && actor->GetLuaState() == &luaState && actor->GetController()->ShouldUpdateAIThisFrame()) {
					                                                     g_CurrentAIActor = actor;
					                                                     actor->RunScriptedFunctionInAppropriateScripts("ThreadedUpdateAI", false, true, {}, {}, {});
					                                                     g_CurrentAIActor = nullptr;
				                                                     }
			                                                     }
			                                                     g_LuaMan.SetThreadLuaStateOverride(nullptr);
		                                                     },
		                                                     luaStates.size())
		    .wait();

		drainDeferredEquips();
		drainDeferredSoundOps();

		// The serial UpdateAI pass mutates directly outside lockstep; under it the calls defer like the threaded ones.
		for (Actor* actor: m_Actors) {
			if (isLocalControllerActor(actor) && actor->ObjectScriptsInitialized() && actor->GetController()->ShouldUpdateAIThisFrame()) {
				if (lockstepActive) {
					g_CurrentAIActor = actor;
				}
				actor->RunScriptedFunctionInAppropriateScripts("UpdateAI", false, true, {}, {}, {});
				g_CurrentAIActor = nullptr;
			}
		}
		if (lockstepActive) {
			drainDeferredEquips();
		}
		drainDeferredSoundOps();
		// A fixture's scripted writes come last, so they are the pass's final word on the actor.
		if (AIWriteScript::IsActive()) {
			AIWriteScript::RunTick(simTick, m_Actors, isLocalControllerActor);
			drainDeferredEquips();
			drainDeferredSoundOps();
		}

		for (const DirectState& before: directBefore) {
			Actor* actor = before.actor;
			if (const float aim = actor->GetAimAngle(false); aim != before.aim) {
				actor->MarkOffWireAim(static_cast<long long>(simTick), aim);
				actor->SetAimAngle(before.aim);
				++m_ControllerBoundaryStats.aimIntents;
			}
			if (const bool flipped = actor->IsHFlipped(); flipped != before.flipped) {
				actor->MarkOffWireFlip(static_cast<long long>(simTick), flipped);
				actor->SetHFlipped(before.flipped);
				++m_ControllerBoundaryStats.flipIntents;
			}
			if (AHuman* human = dynamic_cast<AHuman*>(actor)) {
				const int64_t fg = human->GetEquippedItem() ? static_cast<int64_t>(human->GetEquippedItem()->GetUniqueID()) : 0;
				const int64_t bg = human->GetEquippedBGItem() ? static_cast<int64_t>(human->GetEquippedBGItem()->GetUniqueID()) : 0;
				if (fg != before.fg || bg != before.bg) {
					ReportControllerBoundaryViolation("the equipment", actor);
				}
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
		std::unordered_set<int64_t> applied;
		if (!ApplyControllerFramesToLockstepActors(m_Actors, readyFrame.localFrames, true, applied, error)) {
			DumpControllerDebugSnapshot("lockstep_local_apply_error", simTick, m_Actors, &readyFrame.localFrames, &error);
			ScenarioRunner::SetControllerReplayError(std::string("tick ") + std::to_string(simTick) + " lockstep local apply: " + error);
			return;
		}
		if (!ApplyControllerFramesToLockstepActors(m_Actors, readyFrame.remoteFrames, false, applied, error)) {
			DumpControllerDebugSnapshot("lockstep_remote_apply_error", simTick, m_Actors, &readyFrame.remoteFrames, &error);
			ScenarioRunner::SetControllerReplayError(std::string("tick ") + std::to_string(simTick) + " lockstep remote apply: " + error);
			return;
		}
		NeutralizeUnframedLockstepActors(m_Actors, applied);
		// A leaver's actors dropped off the wire: their control handoffs revert to the policy owner
		// (a surviving teammate's AI picks them up), and actors with no surviving owner stand down —
		// on every survivor at the same tick.
		ScenarioRunner::SetLockstepAppliedFrame(readyFrame.frame);
		ScenarioRunner::PurgeLockstepControlOverridesForGonePeers(readyFrame.frame);
		for (Actor* actor: m_Actors) {
			if (ScenarioRunner::IsLockstepActorOwnerGone(static_cast<int64_t>(actor->GetUniqueID()), actor->GetTeam(), !actor->IsPlayerControlled(), readyFrame.frame)) {
				actor->GetController()->SetDisabled(true);
			}
		}
		DumpControllerDebugSnapshot("lockstep_post_apply", simTick, m_Actors, &readyFrame.remoteFrames);
		g_AudioMan.CommitSoundObservations(readyFrame.frame, readyFrame.localObservations, readyFrame.remoteObservations);
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
		PreControllerStage(actor);
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
			if (m_RenderHidden.empty() || m_RenderHidden.count(*parIt) == 0) {
				(*parIt)->Draw(pTargetBitmap, targetPos);
			}
		}
	}

	{
		ZoneScopedN("Items Draw");

		for (std::deque<MovableObject*>::reverse_iterator itmIt = m_Items.rbegin(); itmIt != m_Items.rend(); ++itmIt) {
			if (m_RenderHidden.empty() || m_RenderHidden.count(*itmIt) == 0) {
				(*itmIt)->Draw(pTargetBitmap, targetPos);
			}
		}
	}

	{
		ZoneScopedN("Actors Draw");

		for (std::deque<Actor*>::reverse_iterator aIt = m_Actors.rbegin(); aIt != m_Actors.rend(); ++aIt) {
			if (m_RenderHidden.empty() || m_RenderHidden.count(*aIt) == 0) {
				(*aIt)->Draw(pTargetBitmap, targetPos);
			}
		}
	}
}

void MovableMan::DrawHUD(BITMAP* pTargetBitmap, const Vector& targetPos, int which, bool playerControlled) {
	ScopedRenderRNG renderRNG;
	ZoneScoped;

	// Draw HUD elements
	for (std::deque<MovableObject*>::reverse_iterator itmIt = m_Items.rbegin(); itmIt != m_Items.rend(); ++itmIt) {
		if (m_RenderHidden.empty() || m_RenderHidden.count(*itmIt) == 0) {
			(*itmIt)->DrawHUD(pTargetBitmap, targetPos, which);
		}
	}

	for (std::deque<Actor*>::reverse_iterator aIt = m_Actors.rbegin(); aIt != m_Actors.rend(); ++aIt) {
		if (m_RenderHidden.empty() || m_RenderHidden.count(*aIt) == 0) {
			(*aIt)->DrawHUD(pTargetBitmap, targetPos, which);
		}
	}
}

std::string MovableMan::SaveCheckpoint() const {
	CheckpointWriter writer("MovableMan2");
	VisitCheckpoint(writer, *this);
	std::map<long, std::vector<long>> references;
	// A row exists to rebind borrowed pointers, so an object that borrows nothing needs none.
	// Writing one anyway makes the restore demand back an owner the checkpoint never carried.
	for (const auto& [identity, object]: m_KnownObjects) {
		std::vector<long> links = object->GetCheckpointBorrowedReferences();
		if (std::none_of(links.begin(), links.end(), [](long target) { return target != 0; })) continue;
		references.emplace(identity, std::move(links));
	}
	writer(references);
	return writer.Text();
}

bool MovableMan::LoadCheckpoint(std::string_view text, bool validateOnly) {
	try {
		const bool legacy = text.starts_with("11 MovableMan1 ");
		CheckpointReader reader(text, legacy ? "MovableMan1" : "MovableMan2", validateOnly);
		std::map<long, std::vector<long>> references;
		if (!legacy) reader.OnCommit([this, &references] {
			// The graph has registered every candidate before RuntimeGlobals commits.
			// Validate the whole alias table before changing any manager or native field.
			for (const auto& [identity, links]: references) {
				auto* object = FindObjectByUniqueID(identity);
				if (!object || !object->RebindCheckpointBorrowedReferences(links, true)) throw std::runtime_error("unresolved native references for owner " + std::to_string(identity));
			}
		});
		VisitCheckpoint(reader, *this);
		if (!legacy) {
			reader.Value(references);
			for (const auto& [identity, links]: references) {
				if (identity <= 0 || links.empty() || std::any_of(links.begin(), links.end(), [](long target) { return target < 0; })) return false;
			}
			reader.OnCommit([this, &references] {
				for (const auto& [identity, links]: references) {
					if (!FindObjectByUniqueID(identity)->RebindCheckpointBorrowedReferences(links)) throw std::runtime_error("could not rebind native references for owner " + std::to_string(identity));
				}
			});
		}
		reader.Finish();
		return true;
	} catch (const std::exception& error) {
		std::cout << "[native-references] " << error.what() << std::endl;
		return false;
	}
}


namespace {
	struct WorldStructure {
		std::array<std::vector<long>, 6> cohorts;
		std::array<std::vector<long>, Activity::MaxTeamCount> rosters;
		std::array<bool, Activity::MaxTeamCount> sortRoster{};
		std::array<std::vector<std::pair<Vector, std::pair<int, float>>>, 2> alarms;
		std::vector<std::pair<uint64_t, long>> quarantine;
		std::vector<long> moidIndex;
		std::map<long, int> contiguousActorIDs;
		std::array<int, Activity::MaxTeamCount> teamMOIDCount{};
		std::array<std::set<long>, 3> validObjects;
		template <class Archive> void Fields(Archive& archive) {
			archive(cohorts, rosters, sortRoster, alarms, quarantine, moidIndex, contiguousActorIDs, teamMOIDCount, validObjects);
		}
	};
}

std::string MovableMan::SaveWorldStructure() const {
	WorldStructure state;
	const auto identities = [](const auto& source, auto& target) {
		for (const auto* object: source) target.insert(target.end(), object ? object->GetUniqueID() : 0);
	};
	identities(m_Actors, state.cohorts[0]); identities(m_Items, state.cohorts[1]); identities(m_Particles, state.cohorts[2]);
	identities(m_AddedActors, state.cohorts[3]); identities(m_AddedItems, state.cohorts[4]); identities(m_AddedParticles, state.cohorts[5]);
	identities(m_ValidActors, state.validObjects[0]); identities(m_ValidItems, state.validObjects[1]); identities(m_ValidParticles, state.validObjects[2]);
	identities(m_MOIDIndex, state.moidIndex);
	for (int team = 0; team < Activity::MaxTeamCount; ++team) {
		identities(m_ActorRoster[team], state.rosters[team]); state.sortRoster[team] = m_SortTeamRoster[team];
		state.teamMOIDCount[team] = m_TeamMOIDCount[team];
	}
	for (const auto& [actor, id]: m_ContiguousActorIDs) state.contiguousActorIDs.emplace(actor->GetUniqueID(), id);
	for (const AlarmEvent* event: m_AlarmEvents) state.alarms[0].emplace_back(event->m_ScenePos, std::pair{static_cast<int>(event->m_Team), event->m_Range});
	for (const AlarmEvent* event: m_AddedAlarmEvents) state.alarms[1].emplace_back(event->m_ScenePos, std::pair{static_cast<int>(event->m_Team), event->m_Range});
	state.quarantine = m_LockstepJoinQuarantine;
	CheckpointWriter writer("WorldStructure1"); state.Fields(writer); return writer.Text();
}

bool MovableMan::LoadWorldStructure(std::string_view text, bool validateOnly) {
	try {
		WorldStructure state;
		CheckpointReader reader(text, "WorldStructure1"); state.Fields(reader); reader.Finish();
		std::set<long> incoming;
		for (const auto& cohort: state.cohorts) for (long uid: cohort) {
			if (uid <= 0 || !incoming.insert(uid).second) throw std::runtime_error("invalid or duplicate world member");
		}
		for (int kind = 0; kind < 3; ++kind) {
			std::set<long> allowed(state.cohorts[kind].begin(), state.cohorts[kind].end());
			allowed.insert(state.cohorts[kind + 3].begin(), state.cohorts[kind + 3].end());
			for (long uid: state.validObjects[kind]) if (!allowed.contains(uid)) throw std::runtime_error("invalid world validity member");
		}
		if (validateOnly) return true;
		const auto resolve = [this](long uid) {
			MovableObject* object = uid ? FindObjectByUniqueID(uid) : nullptr;
			if (uid && !object) throw std::runtime_error("missing world reference " + std::to_string(uid));
			return object;
		};
		const auto actor = [&resolve](long uid) {
			Actor* value = dynamic_cast<Actor*>(resolve(uid));
			if (uid && !value) throw std::runtime_error("world actor reference has another type");
			return value;
		};
		std::set<long> present;
		const auto collect = [&present](const auto& objects) { for (const auto* object: objects) present.insert(object->GetUniqueID()); };
		collect(m_Actors); collect(m_Items); collect(m_Particles); collect(m_AddedActors); collect(m_AddedItems); collect(m_AddedParticles);
		if (present != incoming) throw std::runtime_error("loaded world membership differs from checkpoint");
		std::array<std::deque<MovableObject*>, 6> cohorts;
		for (int kind = 0; kind < 6; ++kind) for (long uid: state.cohorts[kind]) cohorts[kind].push_back(kind % 3 == 0 ? actor(uid) : resolve(uid));
		std::array<std::list<Actor*>, Activity::MaxTeamCount> rosters;
		for (int team = 0; team < Activity::MaxTeamCount; ++team) for (long uid: state.rosters[team]) rosters[team].push_back(actor(uid));
		std::vector<MovableObject*> index;
		for (long uid: state.moidIndex) index.push_back(resolve(uid));
		std::unordered_map<const Actor*, int> contiguous;
		for (const auto& [uid, id]: state.contiguousActorIDs) contiguous.emplace(actor(uid), id);
		// Allocate the incoming events before changing any live membership.
		std::array<std::vector<std::unique_ptr<AlarmEvent>>, 2> events;
		for (int group = 0; group < 2; ++group) for (const auto& [position, detail]: state.alarms[group]) {
			auto event = std::make_unique<AlarmEvent>(); event->m_ScenePos = position;
			event->m_Team = static_cast<Activity::Teams>(detail.first); event->m_Range = detail.second;
			events[group].push_back(std::move(event));
		}
		m_Actors.clear(); m_AddedActors.clear();
		for (MovableObject* object: cohorts[0]) m_Actors.push_back(static_cast<Actor*>(object));
		for (MovableObject* object: cohorts[3]) m_AddedActors.push_back(static_cast<Actor*>(object));
		m_Items.swap(cohorts[1]); m_Particles.swap(cohorts[2]); m_AddedItems.swap(cohorts[4]); m_AddedParticles.swap(cohorts[5]);
		m_ValidActors.clear(); m_ValidItems.clear(); m_ValidParticles.clear();
		for (long uid: state.validObjects[0]) m_ValidActors.insert(actor(uid));
		for (long uid: state.validObjects[1]) m_ValidItems.insert(resolve(uid));
		for (long uid: state.validObjects[2]) m_ValidParticles.insert(resolve(uid));
		for (int team = 0; team < Activity::MaxTeamCount; ++team) {
			m_ActorRoster[team].swap(rosters[team]); m_SortTeamRoster[team] = state.sortRoster[team]; m_TeamMOIDCount[team] = state.teamMOIDCount[team];
		}
		m_MOIDIndex.swap(index); m_ContiguousActorIDs.swap(contiguous); m_LockstepJoinQuarantine.swap(state.quarantine);
		const auto replaceEvents = [](auto& live, auto& saved) {
			while (live.size() > saved.size()) { delete live.back(); live.pop_back(); }
			for (size_t i = 0; i < saved.size(); ++i) {
				if (i < live.size()) *live[i] = *saved[i]; else live.push_back(saved[i].release());
			}
		};
		replaceEvents(m_AlarmEvents, events[0]); replaceEvents(m_AddedAlarmEvents, events[1]);
		return true;
	} catch (const std::exception& error) {
		std::cout << "[world-structure] rejected: " << error.what() << std::endl;
		return false;
	}
}

void MovableMan::RedrawRestoredMOIDs() {
	ScopedRenderRNG renderRNG;
	g_SceneMan.ClearAllMOIDDrawings();
	const auto draw = [](const auto& objects) {
		for (const auto* object: objects) if (!object->IsSetToDelete()) object->Draw(nullptr, Vector(), g_DrawMOID, true);
	};
	draw(m_Actors); draw(m_Items); draw(m_Particles);
}

MovableMan::ConstructionRegistryScope::ConstructionRegistryScope() :
	m_OriginalSounds(g_AudioMan.CaptureCheckpointSoundRegistry()), m_SoundCursor(g_AudioMan.GetCheckpointSoundContainerCursor()),
	m_Counter(MovableObject::GetUniqueIDCounter()), m_Cursor(g_LuaMan.GetScriptStateCursor()) {
	g_MovableMan.CompleteQueuedMOIDDrawings();
	g_MovableMan.WaitForActorsSeeTask();
	{
		std::lock_guard<std::mutex> guard(g_MovableMan.m_ObjectRegisteredMutex);
		m_Original = g_MovableMan.m_KnownObjects;
		g_MovableMan.m_HeldRegistries.push_back(&m_Original);
	}
	const auto mark = g_MovableMan.MarkAddQueues();
	m_QueueSizes = {mark.actors, mark.items, mark.particles, mark.alarms};
	m_Structure = g_MovableMan.SaveWorldStructure();
}

MovableMan::ConstructionRegistryScope::~ConstructionRegistryScope() {
	g_MovableMan.DiscardAddedSince({m_QueueSizes[0], m_QueueSizes[1], m_QueueSizes[2], m_QueueSizes[3]});
	{
		std::lock_guard<std::mutex> guard(g_MovableMan.m_ObjectRegisteredMutex);
		std::erase(g_MovableMan.m_HeldRegistries, &m_Original);
		g_MovableMan.m_KnownObjects.swap(m_Original);
	}
	g_MovableMan.LoadWorldStructure(m_Structure);
	MovableObject::PinUniqueIDCounter(m_Counter);
	g_LuaMan.SetScriptStateCursor(m_Cursor);
	g_AudioMan.RestoreCheckpointSoundRegistry(std::move(m_OriginalSounds));
	g_AudioMan.SetCheckpointSoundContainerCursor(m_SoundCursor);
}

CheckpointSoundRegistry MovableMan::ConstructionRegistryScope::GetStagedSoundRegistrations() const {
	return g_AudioMan.AddedCheckpointSoundRegistrations(m_OriginalSounds);
}

MovableMan::WorldSetAside::~WorldSetAside() {
	// A refused reinstate keeps its world held so the caller can retry or discard it; one that does neither gets it discarded here.
	if (held && MovableMan::IsConstructed() && g_MovableMan.m_WorldSetAside == this) {
		g_MovableMan.DiscardWorld(*this);
	}
}

void MovableMan::DiscardWorld(WorldSetAside& in) {
	if (!in.held) return;
	CompleteQueuedMOIDDrawings();
	WaitForActorsSeeTask();
	ForgetHeldWorld(in);
	for (const auto& [uid, object]: in.knownObjects) {
		// Preset-owned trees remain registered while the runtime is held aside.
		// Their script objects and cached callbacks belong to both worlds.
		if (object && FindObjectByUniqueID(uid) != object) object->DiscardScriptState();
	}
	for (const auto& [state, uid]: in.scriptObjects) state->DiscardStashedScriptObject(uid);
	in.scriptObjects.clear();
	const auto retire = [](auto& objects) { for (auto* object: objects) delete object; objects.clear(); };
	retire(in.actors); retire(in.items); retire(in.particles);
	retire(in.addedActors); retire(in.addedItems); retire(in.addedParticles);
	retire(in.alarmEvents); retire(in.addedAlarmEvents);
	in.pathCallbacks.reset();
	in.activity.reset();
	in.startActivity.reset();
	in.sceneOwners.reset();
	in.pendingLinks.clear();
	in.knownObjects.clear();
	in.scriptRegistrations.clear();
	in.validActors.clear(); in.validItems.clear(); in.validParticles.clear();
	in.moidIndex.clear(); in.contiguousActorIDs.clear(); in.joinQuarantine.clear();
	for (auto& roster: in.rosters) roster.clear();
	in.sceneAreas.areas.clear(); in.sceneAreas.navigableAreas.clear();
	for (size_t index = 0; index < in.luaGraphs.size(); ++index) {
		g_LuaMan.GetStateByIndex(static_cast<int>(index)).RunScriptString("_ScriptGraph.releaseObjects()");
	}
	in.primitiveQueues.reset();
	in.musicOwners.reset();
	in.luaGraphs.clear();
	in.held = false;
	m_WorldSetAside = nullptr;
}

bool MovableMan::RestoreWorld(const WorldSnapshot& in) {
	std::string error;
	if (!LoadWorldStructure(in.structure, true) || !ValidateScriptGraphs(in.luaGraphs, &error)) {
		std::cout << "[scriptgraph] restore refused before replacement: " << error << std::endl;
		return false;
	}
	// A caller running a speculative world already owns the originals and its rollback.
	if (m_WorldSetAside) return RestoreWorldCandidate(in);
	const std::string globals = g_ActivityMan.CaptureRuntimeGlobals();
	WorldSetAside original;
	if (!SetAsideWorld(original)) return false;
	bool restored = false;
	try { restored = RestoreWorldCandidate(in); }
	catch (const std::exception& exception) { std::cout << "[scriptgraph] candidate failed: " << exception.what() << std::endl; }
	if (restored) {
		DiscardWorld(original);
	} else {
		if (!ReinstateWorld(original)) std::cout << "[scriptgraph] original world could not be reinstated" << std::endl;
		g_ActivityMan.RestoreRuntimeGlobals(globals);
	}
	return restored;
}
