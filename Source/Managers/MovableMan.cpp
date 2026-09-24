#include "CaptureSentinel.h"
#include "CheckpointArchive.h"
#include "CheckpointImage.h"
#include "Constants.h"
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
#include "NetActorOwnership.h"
#include "NetLockstep.h"
#include "NetWorldJoin.h"
#include "NetGameCommand.h"
#include "AIWriteScript.h"
#include "LuaMan.h"
#include "ThreadMan.h"
#include "PreviewEventLedger.h"
#include "RTETools.h"
#include "TimerMan.h"

#include <bit>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include "nlohmann/json.hpp"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <execution>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <sstream>
#include <tuple>
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
		// The live controller is what the checksum's controller subsystem reads; without it a dump cannot say
		// which field two peers disagree on.
		const Controller& controller = *const_cast<Actor*>(actor)->GetController();
		uint64_t controlStates = 0;
		for (int state = 0; state < ControlState::CONTROLSTATECOUNT; ++state)
			if (controller.IsState(static_cast<ControlState>(state))) controlStates |= uint64_t{1} << state;
		j["ctrl_states"] = controlStates;
		j["ctrl_move"] = {controller.GetAnalogMove().m_X, controller.GetAnalogMove().m_Y};
		j["ctrl_aim"] = {controller.GetAnalogAim().m_X, controller.GetAnalogAim().m_Y};
		j["ctrl_cursor"] = {controller.GetAnalogCursor().m_X, controller.GetAnalogCursor().m_Y};
		j["ctrl_input_mode"] = static_cast<int>(controller.GetInputMode());
		j["ctrl_player"] = controller.GetPlayer();
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
	m_Range(range * c_DefaultResX * 0.51F) {}

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

struct ThreadedSyncedUpdateSelfTestContext {
	std::vector<long> order;
	// Armed by the deleted-object row; the first SyncedUpdate of the pass retires another state's object.
	std::function<void()> retire;
	// Armed for the ordered runs; the first SyncedUpdate of the pass spawns one object into the world.
	std::function<void()> spawn;
	// The object every SyncedUpdate writes into: the contract's "modifying other objects" channel.
	MovableObject* witness = nullptr;
	std::vector<long> spawnedIDs;
	// The contract's global-table channel: one write per object, landing in as many per-state tables
	// as the peer runs states. The count of tables is what F2 is about, so it is counted, never hashed.
	long globalWrites = 0;
	std::set<const void*> globalWriteStates;
	// Whether the engine still knew the object a script asked about, at the moment that script ran.
	int aliveHere = -1;
};

ThreadedSyncedUpdateSelfTestContext* s_ThreadedSyncedUpdateSelfTestContext = nullptr;

int AppendThreadedSyncedUpdateSelfTestOrder(lua_State* state) {
	if (!s_ThreadedSyncedUpdateSelfTestContext) return 0;
	s_ThreadedSyncedUpdateSelfTestContext->order.push_back(static_cast<long>(luaL_checknumber(state, 1)));
	return 0;
}

int ReadThreadedSyncedUpdateSelfTestOrderLength(lua_State* state) {
	lua_pushinteger(state, s_ThreadedSyncedUpdateSelfTestContext ? static_cast<lua_Integer>(s_ThreadedSyncedUpdateSelfTestContext->order.size()) : 0);
	return 1;
}

int RunThreadedSyncedUpdateSelfTestRetire(lua_State* state) {
	if (s_ThreadedSyncedUpdateSelfTestContext && s_ThreadedSyncedUpdateSelfTestContext->retire) {
		std::exchange(s_ThreadedSyncedUpdateSelfTestContext->retire, nullptr)();
	}
	return 0;
}

// The contract's "modifying other objects": every SyncedUpdate folds its own identity into one other
// object's field, so the value carries the order the pass ran in and not merely its membership.
int RunThreadedSyncedUpdateSelfTestWitness(lua_State* state) {
	if (!s_ThreadedSyncedUpdateSelfTestContext || !s_ThreadedSyncedUpdateSelfTestContext->witness) return 0;
	MovableObject* witness = s_ThreadedSyncedUpdateSelfTestContext->witness;
	const auto previous = static_cast<uint32_t>(witness->GetNumberValue("threaded_synced_witness"));
	const auto uniqueID = static_cast<uint32_t>(static_cast<long>(luaL_checknumber(state, 1)));
	witness->SetNumberValue("threaded_synced_witness", static_cast<double>((previous ^ uniqueID) * 16777619u));
	// The caller has just written its own state's global table; the value it reports is that table's
	// running count, so a positive one says the write happened and the state says which table took it.
	if (luaL_checknumber(state, 2) > 0) {
		++s_ThreadedSyncedUpdateSelfTestContext->globalWrites;
		s_ThreadedSyncedUpdateSelfTestContext->globalWriteStates.insert(g_LuaMan.GetThreadLuaStateOverride());
	}
	return 0;
}

// The contract's "spawning objects": the first SyncedUpdate of an armed pass puts one object into the
// world. Its unique ID is part of what the row hashes, so a spawn at a different point in the order shows.
int RunThreadedSyncedUpdateSelfTestSpawn(lua_State* state) {
	if (s_ThreadedSyncedUpdateSelfTestContext && s_ThreadedSyncedUpdateSelfTestContext->spawn) {
		std::exchange(s_ThreadedSyncedUpdateSelfTestContext->spawn, nullptr)();
	}
	return 0;
}

// Deletes the object running this script through the engine's own script-facing deletion, from inside
// that object's hook - what a mod's DeleteEntity(self) reaches.
int RunThreadedSyncedUpdateSelfTestDeleteSelf(lua_State* state) {
	const long uniqueID = static_cast<long>(luaL_checknumber(state, 1));
	if (MovableObject* object = g_MovableMan.FindObjectByUniqueID(uniqueID)) {
		DeleteEntityFromScript(object);
	}
	return 0;
}

// Frees the object running this script the way engine code does - a plain delete, not the script-facing
// one - so the row can prove a hook loop survives a deletion that never waited for it.
int RunThreadedSyncedUpdateSelfTestEngineDeleteSelf(lua_State* state) {
	const long uniqueID = static_cast<long>(luaL_checknumber(state, 1));
	delete g_MovableMan.FindObjectByUniqueID(uniqueID);
	return 0;
}

// Answers whether the engine still knows the object running this script: a hook that runs on a
// destroyed object reads freed memory, which is what the self-delete row is about.
int ReadThreadedSyncedUpdateSelfTestAlive(lua_State* state) {
	if (s_ThreadedSyncedUpdateSelfTestContext) {
		const long uniqueID = static_cast<long>(luaL_checknumber(state, 1));
		s_ThreadedSyncedUpdateSelfTestContext->aliveHere = g_MovableMan.FindObjectByUniqueID(uniqueID) ? 1 : 0;
	}
	return 0;
}

void InstallThreadedSyncedUpdateSelfTestCallbacks(LuaStateWrapper& state) {
	std::lock_guard<std::recursive_mutex> lock(state.GetMutex());
	lua_State* luaState = state.GetLuaState();
	lua_pushcfunction(luaState, AppendThreadedSyncedUpdateSelfTestOrder);
	lua_setglobal(luaState, "_ThreadedSyncedUpdateAppend");
	lua_pushcfunction(luaState, ReadThreadedSyncedUpdateSelfTestOrderLength);
	lua_setglobal(luaState, "_ThreadedSyncedUpdateLength");
	lua_pushcfunction(luaState, RunThreadedSyncedUpdateSelfTestRetire);
	lua_setglobal(luaState, "_ThreadedSyncedUpdateRetire");
	lua_pushcfunction(luaState, RunThreadedSyncedUpdateSelfTestWitness);
	lua_setglobal(luaState, "_ThreadedSyncedUpdateWitness");
	lua_pushcfunction(luaState, RunThreadedSyncedUpdateSelfTestSpawn);
	lua_setglobal(luaState, "_ThreadedSyncedUpdateSpawn");
	lua_pushcfunction(luaState, ReadThreadedSyncedUpdateSelfTestAlive);
	lua_setglobal(luaState, "_ThreadedSyncedUpdateAliveHere");
	lua_pushcfunction(luaState, RunThreadedSyncedUpdateSelfTestDeleteSelf);
	lua_setglobal(luaState, "_ThreadedSyncedUpdateDeleteSelf");
	lua_pushcfunction(luaState, RunThreadedSyncedUpdateSelfTestEngineDeleteSelf);
	lua_setglobal(luaState, "_ThreadedSyncedUpdateEngineDeleteSelf");
}

// A registered MO beside the identity the synced pass orders on, read once when the pass snapshots the
// state. A script earlier in the same pass can delete an object still held in the snapshot, so nothing
// after the snapshot may read the object to order it.
struct SyncedUpdateEntry {
	MovableObject* object = nullptr;
	long uniqueID = 0;
	MOID moid = g_NoMOID;
	long registrationSerial = 0;
};

// The snapshot order: unique ID, then MOID for the pair that shares one (a faithful clone copies the
// source's ID), then the place each took in the registration order, which settles the pair the MOID
// index never saw. All three come from the sim, so none depends on how many Lua states the peer runs;
// the state-vector position the heap would otherwise fall back on does.
static bool SyncedUpdateEntryEarlier(const SyncedUpdateEntry& lhs, const SyncedUpdateEntry& rhs) {
	if (lhs.uniqueID != rhs.uniqueID) return lhs.uniqueID < rhs.uniqueID;
	if (lhs.moid != rhs.moid) return lhs.moid < rhs.moid;
	return lhs.registrationSerial < rhs.registrationSerial;
}

// A Lua state's registered-MO set copied into canonical order with each object's identity read once.
static std::vector<SyncedUpdateEntry> SnapshotRegisteredMOs(const LuaStateWrapper& state) {
	const auto& registered = state.GetRegisteredMOs();
	std::vector<SyncedUpdateEntry> snapshot;
	snapshot.reserve(registered.size());
	for (MovableObject* mo: registered) {
		snapshot.push_back({mo, mo->GetUniqueID(), mo->GetID(), mo->GetScriptRegistrationSerial()});
	}
	std::sort(snapshot.begin(), snapshot.end(), SyncedUpdateEntryEarlier);
	return snapshot;
}

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
		const int player = activity->PlayerOfScreen(0);
		Actor* controlled = activity->GetLocallyControlledActor(player);
		const int screen = activity->ScreenOfPlayer(player);
		json view = {{"round_id", round}, {"frame", frame}, {"peer_id", ScenarioRunner::GetLockstepLocalPeerId()},
			{"player_index", player}, {"input_player", activity->LocalInputOfPlayer(player)},
			{"player_controller_input", activity->GetPlayerController(player) ? activity->GetPlayerController(player)->GetInputPlayer() : Players::NoPlayer},
			{"player_active", activity->PlayerActive(player)}, {"player_human", activity->IsLocalHumanSeat(player)},
			{"team", player >= 0 ? activity->GetTeamOfPlayer(player) : Activity::NoTeam}, {"screen", screen},
			{"controlled_uid", uid(controlled)}, {"brain_uid", uid(activity->GetPlayerBrain(player))},
			{"view_state", static_cast<int>(player >= 0 ? activity->GetViewState(player) : Activity::Observe)}, {"camera_target", nullptr},
			{"seat_mode", nullptr}, {"seat_player", nullptr}, {"controller_input", nullptr}, {"seat_facts", json::array()}};
		if (uid(controlled) != 0) {
			view["seat_mode"] = static_cast<int>(controlled->GetController()->GetSeatMode());
			view["seat_player"] = controlled->GetController()->GetSeatPlayer();
			view["controller_input"] = controlled->GetController()->GetInputPlayer();
		}
		for (int seat = Players::PlayerOne; seat < Players::MaxPlayerCount; ++seat) {
			view["seat_facts"].push_back({{"player", seat}, {"active", activity->IsSeatActive(seat)},
				{"human", activity->IsHumanSeat(seat)}, {"team", activity->GetTeamOfPlayer(seat)},
				{"brain_uid", uid(activity->GetPlayerBrain(seat))}, {"input", activity->LocalInputOfPlayer(seat)}, {"screen", activity->ScreenOfPlayer(seat)}});
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
		const uint8_t owner = ScenarioRunner::GetLockstepActorOwner(actorID, actor->GetTeam(), !actor->IsPlayerControlled());
		if (ScenarioRunner::IsLockstepSeatReclaimGap(owner, static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()))) continue;
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
		// Every peer applies this committed frame, so the seat it names is where the shared control binding comes from.
		if (Activity* activity = g_ActivityMan.GetActivity()) {
			activity->NoteLockstepControlBinding(frame.actorUniqueID, actorIt->second->GetController()->GetPlayer());
		}
		applied.insert(frame.actorUniqueID);
	}
	return true;
}

// An actor no frame was committed for this tick (its first D ticks in the world, the ticks after a
// pause or an ownership change) runs on neutral input on every peer, not on its owner's fresh sample.
static void NeutralizeUnframedLockstepActors(const std::deque<Actor*>& actors, const std::unordered_set<int64_t>& applied, bool canonicalStartup = false) {
	for (Actor* actor: actors) {
		if (applied.find(static_cast<int64_t>(actor->GetUniqueID())) == applied.end()) {
			actor->GetController()->ApplyWireNeutral();
			if (canonicalStartup) {
				actor->GetController()->ApplyWireMode(Controller::CIM_NETWORK, Players::NoPlayer);
				actor->GetController()->ApplyWireEnabled();
			}
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

// A buy order rejected at apply never commits, so the seat that issued it goes back to its committed funds.
static void ClearRejectedPurchaseView(Activity& activity, const NetGameDeliverCargo& delivery, uint8_t senderPeerId) {
	if (!delivery.queuedPurchase) {
		return;
	}
	const int player = senderPeerId == ScenarioRunner::GetLockstepLocalPeerId() ? delivery.orderedByPlayer : Players::NoPlayer;
	activity.ClearPreviewedPurchase(player, delivery.team, delivery.cost);
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
		if (std::holds_alternative<NetGameSeatHold>(command.payload) || std::holds_alternative<NetGameInputDelay>(command.payload) || std::holds_alternative<NetGameSeatReclaim>(command.payload)) continue;
		if (const auto* bindings = std::get_if<NetGamePlayerBindings>(&command.payload)) {
			ScenarioRunner::ObserveLockstepPlayerBindings(command.senderPeerId, readyFrame.frame, *bindings);
			continue;
		}
		if (!ScenarioRunner::ConsumeLockstepGameCommand(command)) continue;
		if (const auto* checkpoint = std::get_if<NetGameCheckpoint>(&command.payload)) {
			// The schedule is the host's; any peer may report its own writer.
			if (checkpoint->kind == NetGameCheckpoint::Capture && command.senderPeerId != ScenarioRunner::GetLockstepHostPeerId()) {
				g_ConsoleMan.PrintString("ERROR: Rejected a checkpoint schedule from a peer that is not the host");
				continue;
			}
			ScenarioRunner::NoteAppliedCheckpoint(command.senderPeerId, *checkpoint);
			continue;
		}
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
		} else if (std::holds_alternative<NetGameWorldTransition>(command.payload)) {
			// A world's membership, spawns and bindings are the host's alone; a seatless dedicated host
			// owns no team, so the team gate cannot vet this one either.
			if (command.senderPeerId != ScenarioRunner::GetLockstepHostPeerId()) {
				g_ConsoleMan.PrintString("ERROR: Rejected a WorldTransition command from a peer that is not the host");
				continue;
			}
		} else if (const NetGameAIOrder* order = std::get_if<NetGameAIOrder>(&command.payload)) {
			if (!ScenarioRunner::IsLockstepAIOrderAuthorized(command.senderPeerId, *order)) {
				const Actor* target = dynamic_cast<const Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(order->actorUID)));
				const bool cpu = !target || !target->IsPlayerControlled();
				const uint8_t owner = ScenarioRunner::GetLockstepActorOwner(order->actorUID, order->team, cpu);
				const uint8_t authority = ScenarioRunner::ResolveTeamCommandAuthority(order->team);
				const std::string line = "ERROR: Rejected a AIOrder command from a peer that does not control team " + std::to_string(commandTeam) + " (sender=" + std::to_string(static_cast<int>(command.senderPeerId)) + " owner=" + std::to_string(static_cast<int>(owner)) + " authority=" + std::to_string(static_cast<int>(authority)) + ")";
				g_ConsoleMan.PrintString(line);
				std::cout << line << std::endl;
				continue;
			}
		} else if (const NetGameAIScriptMessage* message = std::get_if<NetGameAIScriptMessage>(&command.payload)) {
			// An AI pass's message and gib are authorized by their writer, like the AI orders they sit beside.
			if (!ScenarioRunner::IsLockstepAIWriteAuthorized(command.senderPeerId, message->team, message->writerUID, message->writerUID)) {
				g_ConsoleMan.PrintString("ERROR: Rejected an AIScriptMessage command from a peer that does not drive actor " + std::to_string(message->writerUID));
				continue;
			}
		} else if (const NetGameAIGib* gibCommand = std::get_if<NetGameAIGib>(&command.payload)) {
			if (!ScenarioRunner::IsLockstepAIWriteAuthorized(command.senderPeerId, gibCommand->team, gibCommand->writerUID, gibCommand->writerUID)) {
				g_ConsoleMan.PrintString("ERROR: Rejected an AIGib command from a peer that does not drive actor " + std::to_string(gibCommand->writerUID));
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
				ClearRejectedPurchaseView(*activity, *delivery, command.senderPeerId);
				continue;
			}
			GameActivity* gameActivity = dynamic_cast<GameActivity*>(activity);
			if (delivery->queuedPurchase && (!gameActivity || !std::isfinite(delivery->cost) || delivery->cost < 0.0F || !std::isfinite(delivery->waypointX) || !std::isfinite(delivery->waypointY))) {
				ClearRejectedPurchaseView(*activity, *delivery, command.senderPeerId);
				g_ConsoleMan.PrintString("ERROR: Buy order rejected - bad order fields");
				std::cout << "[net-match] buy order rejected: bad order fields" << std::endl;
				continue;
			}
			const Entity* craftPreset = g_PresetMan.GetEntityPreset(delivery->craftClassName, delivery->craftPreset, delivery->craftModule);
			if (!craftPreset) {
				ClearRejectedPurchaseView(*activity, *delivery, command.senderPeerId);
				g_ConsoleMan.PrintString("ERROR: Delivery rejected - unknown craft preset \"" + delivery->craftPreset + "\"");
				continue;
			}
			Entity* craftClone = craftPreset->Clone();
			ACraft* craft = dynamic_cast<ACraft*>(craftClone);
			if (!craft) {
				delete craftClone;
				ClearRejectedPurchaseView(*activity, *delivery, command.senderPeerId);
				continue;
			}
			if (delivery->queuedPurchase) {
				MovableMan::ApplyQueuedPurchaseDelivery(*gameActivity, *delivery, command.senderPeerId, craft);
			} else {
				// Load the manifest in order so both peers clone the same presets and assign matching unique ids.
				int loaded = 0;
				for (const NetGameCargoItem& item : delivery->cargo) {
					const Entity* itemPreset = g_PresetMan.GetEntityPreset(item.className, item.preset, item.module);
					if (!itemPreset) {
						g_ConsoleMan.PrintString("ERROR: Delivery cargo skipped - unknown preset \"" + item.preset + "\"");
						continue;
					}
					Entity* itemClone = itemPreset->Clone();
					if (MovableObject* cargo = dynamic_cast<MovableObject*>(itemClone)) {
						craft->AddInventoryItem(cargo);
						++loaded;
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
				// One line per applied delivery on every peer, so the peers' logs compare directly.
				std::cout << "[net-match] deliver command host applied tick=" << readyFrame.frame << " team=" << delivery->team << " order=" << command.sequence << " items=" << loaded << " peer=" << static_cast<int>(command.senderPeerId) << std::endl;
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
					case NetGameAIOrder::SetMOMoveTarget:
						actor->SetMOMoveTarget(order->targetUID ? g_MovableMan.FindObjectByUniqueID(static_cast<long int>(order->targetUID)) : nullptr);
						break;
					case NetGameAIOrder::SetAlarmPoint:
						actor->AlarmPoint(Vector(order->x, order->y));
						break;
					default:
						break;
				}
			} else {
				g_ConsoleMan.PrintString("NETWORK: AI order target not found: UID " + std::to_string(order->actorUID));
				std::cout << "[net-match] AI order target not found: UID " << order->actorUID << std::endl;
			}
		} else if (const NetGameSwitchControl* switchControl = std::get_if<NetGameSwitchControl>(&command.payload)) {
			// The actor may legally be gone by apply time; the override applies either way so every
			// peer's map stays identical, but a live actor must really be on the claimed team.
			const Actor* actor = dynamic_cast<const Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(switchControl->actorUID)));
			if (actor && actor->GetTeam() != switchControl->team) {
				g_ConsoleMan.PrintString("ERROR: Rejected a SwitchControl command for an actor off its claimed team");
				continue;
			}
			if (!MovableMan::ApplyLockstepControlClaim(switchControl->actorUID, command.senderPeerId, switchControl->newOwnerPeerId, readyFrame.frame)) {
				continue;
			}
			std::cout << "[net-match] control of actor " << switchControl->actorUID << " -> peer " << static_cast<int>(switchControl->newOwnerPeerId) << std::endl;
		} else if (const NetGameReseat* reseat = std::get_if<NetGameReseat>(&command.payload)) {
			if (auto* game = dynamic_cast<GameActivity*>(activity)) game->ApplyNetworkSeatAI(reseat->newOwnerPeerId, false, readyFrame.frame);
			// Like SwitchControl, the override lands even for an actor that is already gone so every
			// peer's map stays identical; a live actor that left the team is not reseated.
			int reseated = 0;
			int liveReseated = 0;
			for (const int64_t actorUID: reseat->actorUIDs) {
				const Actor* actor = dynamic_cast<const Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(actorUID)));
				if (actor && actor->GetTeam() != reseat->team) {
					continue;
				}
				ScenarioRunner::ReclaimLockstepActor(actorUID, reseat->newOwnerPeerId);
				++reseated;
				if (actor) ++liveReseated;
			}
			std::cout << "[net-match] reseat: team " << reseat->team << " -> peer " << static_cast<int>(reseat->newOwnerPeerId) << " actors " << reseated << "/" << reseat->actorUIDs.size() << std::endl;
			std::cout << "[net-match] seat-reclaimed peer=" << static_cast<int>(reseat->newOwnerPeerId) << " frame=" << readyFrame.frame << " live_actors=" << liveReseated << std::endl;
		} else if (const NetGameWorldTransition* transition = std::get_if<NetGameWorldTransition>(&command.payload)) {
			if (transition->team < Activity::Teams::TeamOne || transition->team >= Activity::Teams::MaxTeamCount ||
			    !std::isfinite(transition->posX) || !std::isfinite(transition->posY)) {
				continue;
			}
			std::string transitionError;
			if (!ScenarioRunner::AcceptWorldTransition(*transition, &transitionError)) {
				g_ConsoleMan.PrintString("ERROR: Rejected a stale WorldTransition: " + transitionError);
				continue;
			}
			if (transition->kind == NetGameWorldTransition::Release) {
				// The seat's player left, so its characters are nobody's again and the next Activate
				// may seat the brain it left behind instead of cloning a second resident.
				ScenarioRunner::ReleaseLockstepControlOverridesOf(transition->peerId);
			}
			Actor* seated = nullptr;
			if (transition->kind == NetGameWorldTransition::Activate && transition->player >= 0 && transition->player < Players::MaxPlayerCount &&
			    std::find(readyFrame.reclaimedPeerIds.begin(), readyFrame.reclaimedPeerIds.end(), transition->peerId) != readyFrame.reclaimedPeerIds.end()) {
				Actor* brain = activity->GetPlayerBrain(transition->player);
				if (g_MovableMan.IsActor(brain) && brain->GetTeam() == transition->team &&
				    ScenarioRunner::GetLockstepControlOverrideOwner(brain->GetUniqueID()) == transition->peerId) seated = brain;
			}
			if (transition->kind == NetGameWorldTransition::Activate) {
				// The candidates come from lockstep state alone - the committed roster's order and the
				// synced handoff map - so every peer picks the same actor, and never one a member holds.
				std::vector<NetWorldBrainCandidate> brains;
				if (transition->team >= Activity::Teams::TeamOne && transition->team < Activity::Teams::MaxTeamCount) {
					for (const Actor* candidate: *g_MovableMan.GetTeamRoster(transition->team)) {
						if (candidate == nullptr || !candidate->HasObjectInGroup("Brains")) {
							continue;
						}
						const int64_t uid = static_cast<int64_t>(candidate->GetUniqueID());
						brains.push_back(NetWorldBrainCandidate{uid, candidate->GetTeam(), ScenarioRunner::GetLockstepControlOverrideOwner(uid)});
					}
				}
				if (const int64_t chosen = ChooseWorldActivateBrain(brains, *transition); !seated && chosen != 0) {
					seated = dynamic_cast<Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(chosen)));
				}
			}
			if (!seated && !transition->className.empty()) {
				if (const Entity* preset = g_PresetMan.GetEntityPreset(transition->className, transition->preset, transition->module)) {
					Entity* clone = preset->Clone();
					if (Actor* actor = dynamic_cast<Actor*>(clone)) {
						actor->SetTeam(transition->team);
						actor->SetPos(Vector(transition->posX, transition->posY));
						if (transition->aiMode >= 0 && transition->aiMode < Actor::AIMODE_COUNT) {
							actor->SetAIMode(static_cast<Actor::AIMode>(transition->aiMode));
						}
						// Every peer clones at the same committed tick in this order, so the resident's
						// unique id is the same number on all of them and the binding below can name it.
						g_MovableMan.AddActor(actor);
						seated = actor;
					} else {
						delete clone;
					}
				} else {
					g_ConsoleMan.PrintString("ERROR: World transition rejected - unknown preset \"" + transition->preset + "\"");
					continue;
				}
			}
			if (WorldTransitionSeatsMember(*transition) && transition->peerId != 0 && seated) {
				ScenarioRunner::SetLockstepControlOverride(static_cast<int64_t>(seated->GetUniqueID()), transition->peerId);
			}
			if (WorldTransitionBindsBrain(*transition, seated != nullptr)) {
				activity->SetPlayerBrain(seated, transition->player);
			}
			std::cout << "[net-match] world transition: kind " << static_cast<int>(transition->kind)
			          << " peer " << static_cast<int>(transition->peerId) << " team " << transition->team
			          << " revision " << transition->membershipRevision
			          << " actor " << (seated ? static_cast<int64_t>(seated->GetUniqueID()) : 0) << std::endl;
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
			ScenarioRunner::ApplyLockstepPauseCommand(pauseMatch->pause, command.senderPeerId);
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
		} else if (const NetGameAIScriptMessage* scriptMessage = std::get_if<NetGameAIScriptMessage>(&command.payload)) {
			// The AI's message runs here on every peer, so a receiver script that gates a sim write on it
			// decides alike everywhere; only the peer driving the writing actor may issue it.
			const Actor* writer = dynamic_cast<const Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(scriptMessage->writerUID)));
			if (!writer) {
				g_ConsoleMan.PrintString("NETWORK: AI message command writer not found: UID " + std::to_string(scriptMessage->writerUID));
				std::cout << "[net-match] AI message command writer not found: UID " << scriptMessage->writerUID << std::endl;
				continue;
			}
			if (writer->GetTeam() != scriptMessage->team || !ScenarioRunner::IsLockstepActorOwner(scriptMessage->writerUID, writer->GetTeam(), !writer->IsPlayerControlled(), command.senderPeerId)) {
				g_ConsoleMan.PrintString("ERROR: Rejected an AI message command from a peer that does not drive actor " + std::to_string(scriptMessage->writerUID));
				continue;
			}
			MovableObject* receiver = g_MovableMan.FindObjectByUniqueID(static_cast<long int>(scriptMessage->objectUID));
			if (!receiver) {
				g_ConsoleMan.PrintString("NETWORK: AI message command target not found: UID " + std::to_string(scriptMessage->objectUID));
				std::cout << "[net-match] AI message command target not found: UID " << scriptMessage->objectUID << std::endl;
				continue;
			}
			receiver->DeliverSyncedScriptMessage(scriptMessage->context, scriptMessage->number, scriptMessage->contextUID, scriptMessage->message, scriptMessage->text);
		} else if (const NetGameAIGib* gib = std::get_if<NetGameAIGib>(&command.payload)) {
			// The AI's gib runs here on every peer, at one tick; only the peer driving the writing actor may issue it.
			const Actor* writer = dynamic_cast<const Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(gib->writerUID)));
			if (!writer) {
				g_ConsoleMan.PrintString("NETWORK: AI gib command writer not found: UID " + std::to_string(gib->writerUID));
				std::cout << "[net-match] AI gib command writer not found: UID " << gib->writerUID << std::endl;
				continue;
			}
			if (writer->GetTeam() != gib->team || !ScenarioRunner::IsLockstepActorOwner(gib->writerUID, writer->GetTeam(), !writer->IsPlayerControlled(), command.senderPeerId)) {
				g_ConsoleMan.PrintString("ERROR: Rejected an AI gib command from a peer that does not drive actor " + std::to_string(gib->writerUID));
				continue;
			}
			MOSRotating* gibbed = dynamic_cast<MOSRotating*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(gib->objectUID)));
			if (!gibbed) {
				g_ConsoleMan.PrintString("NETWORK: AI gib command target not found: UID " + std::to_string(gib->objectUID));
				std::cout << "[net-match] AI gib command target not found: UID " << gib->objectUID << std::endl;
				continue;
			}
			MovableObject* ignored = gib->ignoreUID ? g_MovableMan.FindObjectByUniqueID(static_cast<long int>(gib->ignoreUID)) : nullptr;
			gibbed->GibThis(Vector(gib->impulseX, gib->impulseY), ignored);
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
		} else if (const NetGamePlaceBrain* placeBrain = std::get_if<NetGamePlaceBrain>(&command.payload)) {
			// A seat's committed brain placement in the synchronized setup editor.
			if (GameActivity* gameActivity = dynamic_cast<GameActivity*>(activity)) {
				gameActivity->ApplyNetBrainPlacement(*placeBrain, command.senderPeerId);
			}
		}
	}
	MovableMan::ReconcileLockstepControlBindings();
}

// With no coordinator the pending commands have no ready frame to ride in on, so a scenario run
// drains them itself and applies the identical set through the same path the match uses.
static void ApplyOfflineGameCommands(uint64_t simTick) {
	if (ScenarioRunner::HasLockstepCoordinator()) {
		return;
	}
	std::vector<NetGameCommand> commands = ScenarioRunner::DrainLocalGameCommands();
	if (commands.empty()) {
		return;
	}
	NetLockstepReadyFrame readyFrame;
	readyFrame.frame = simTick;
	readyFrame.localCommands = std::move(commands);
	ApplyLockstepGameCommands(readyFrame);
}

namespace {
	std::map<int64_t, std::pair<uint64_t, uint8_t>> s_LockstepFrameClaims; //!< Actor -> the frame it was claimed on and by whom.
}

bool MovableMan::ApplyLockstepControlClaim(int64_t actorUniqueID, uint8_t senderPeerId, uint8_t newOwnerPeerId, uint64_t frame) {
	const Actor* actor = dynamic_cast<const Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(actorUniqueID)));
	const int team = actor ? actor->GetTeam() : 0;
	const bool cpuControlled = actor ? !actor->IsPlayerControlled() : true;
	const uint8_t currentOwner = ScenarioRunner::GetLockstepActorOwner(actorUniqueID, team, cpuControlled);
	if (newOwnerPeerId != senderPeerId) {
		// The release form: an owner hands its actor back to the owner the world seeded for it.
		if (senderPeerId != currentOwner || newOwnerPeerId != NetActorOwnership::GetSeededOwner(actorUniqueID)) {
			// A selftest process has no console; the claim rules still have to run there.
			if (ConsoleMan::IsConstructed()) {
				g_ConsoleMan.PrintString("ERROR: Rejected a SwitchControl command claiming another peer");
			}
			return false;
		}
	} else if (const auto claimed = s_LockstepFrameClaims.find(actorUniqueID); claimed != s_LockstepFrameClaims.end() && claimed->second.first == frame && claimed->second.second < senderPeerId) {
		if (ConsoleMan::IsConstructed()) {
			g_ConsoleMan.PrintString("NETWORK: Rejected a SwitchControl claim on an actor a lower peer already claimed this frame");
		}
		std::cout << "[net-match] rejected a claim on actor " << actorUniqueID << " already claimed this frame by peer " << static_cast<int>(claimed->second.second) << std::endl;
		return false;
	} else {
		s_LockstepFrameClaims[actorUniqueID] = {frame, senderPeerId};
	}
	ScenarioRunner::SetLockstepControlOverride(actorUniqueID, newOwnerPeerId);
	if (Actor* live = dynamic_cast<Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(actorUniqueID)))) {
		ApplyLockstepControlHandoffToActor(*live, newOwnerPeerId == senderPeerId);
	}
	return true;
}

// Production and the sim-facing mode move on the same committed tick: the frames the old owner still
// has in flight are dropped by the ownership gate, so nothing puts the old mode back in between.
void MovableMan::ApplyLockstepControlHandoffToActor(Actor& actor, bool seated) {
	Controller& controller = *actor.GetController();
	const Controller::InputMode handedMode = seated ? Controller::CIM_PLAYER : Controller::CIM_AI;
	const Controller::InputMode previousMode = controller.GetInputMode();
	const int previousPlayer = controller.GetPlayer();
	if (!seated) controller.ResetLocalInputState();
	if (previousMode == handedMode) {
		return;
	}
	controller.ApplyWireMode(handedMode, controller.GetPlayerRaw());
	actor.OnControllerInputModeChanged(previousMode, previousPlayer);
}

// Frames a synced pause committed: the sim does not advance on them, so they are not frames the match played.
static uint64_t s_LockstepPausedFrames = 0;

uint64_t RTE::LockstepPlayedFrame() {
	const uint64_t applied = ScenarioRunner::GetLockstepAppliedFrame();
	return applied > s_LockstepPausedFrames ? applied - s_LockstepPausedFrames : 0;
}

void RTE::ResetLockstepPausedFrames() {
	s_LockstepPausedFrames = 0;
}

uint64_t RTE::GetLockstepPausedFrames() { return s_LockstepPausedFrames; }
void RTE::RestoreLockstepPausedFrames(uint64_t frames) { s_LockstepPausedFrames = frames; }

void RTE::ApplyLockstepSeatReclaims(const NetLockstepReadyFrame& ready, const std::deque<Actor*>& actors) {
	for (uint8_t peer: ready.reclaimedPeerIds) {
		size_t reclaimed = 0;
		for (Actor* actor: actors) {
			const int64_t uid = static_cast<int64_t>(actor->GetUniqueID());
			if (ScenarioRunner::GetLockstepReclaimSeat(uid, actor->GetTeam(), !actor->IsPlayerControlled(), ready.frame) != peer) continue;
			ScenarioRunner::ReclaimLockstepActor(uid, peer);
			actor->GetController()->ResetLocalInputState(actor->GetController()->GetInputMode());
			actor->TouchCheckpoint(); ++reclaimed;
		}
		if (auto* activity = dynamic_cast<GameActivity*>(g_ActivityMan.GetActivity())) activity->ApplyNetworkSeatAI(peer, false, ready.frame);
		if (peer == ScenarioRunner::GetLockstepLocalPeerId()) ScenarioRunner::NoteLocalSeatReclaimed();
		std::cout << "[net-match] seat-reclaimed peer=" << static_cast<int>(peer) << " frame=" << ready.frame << " live_actors=" << reclaimed << std::endl;
	}
}

void RTE::ApplyLockstepLeaveHandoffs(const NetLockstepReadyFrame& readyFrame, const std::deque<Actor*>& actors, bool paused) {
	// A round that restarts its frame numbering restarts the count with it.
	if (readyFrame.frame <= ScenarioRunner::GetLockstepAppliedFrame()) {
		s_LockstepPausedFrames = 0;
	}
	if (paused) {
		++s_LockstepPausedFrames;
	}
	ScenarioRunner::SetLockstepAppliedFrame(readyFrame.frame);
	ScenarioRunner::PurgeLockstepControlOverridesForGonePeers(readyFrame.frame);
	for (uint8_t peer: readyFrame.aiHeldPeerIds) ScenarioRunner::ApplyLockstepSeatAI(peer, readyFrame.frame);
	for (Actor* actor: actors) {
		const int64_t uid = static_cast<int64_t>(actor->GetUniqueID());
		const uint8_t claimant = readyFrame.aiHeldPeerIds.empty() ? ScenarioRunner::GetLockstepDropTimeActorOwner(uid, actor->GetTeam(), !actor->IsPlayerControlled())
		    : ScenarioRunner::GetLockstepHeldSeat(uid, actor->GetTeam(), !actor->IsPlayerControlled(), readyFrame.frame);
		const bool aiTakeover = std::find(readyFrame.aiHeldPeerIds.begin(), readyFrame.aiHeldPeerIds.end(), claimant) != readyFrame.aiHeldPeerIds.end();
		const bool playerControlled = actor->IsPlayerControlled();
		const bool disabled = actor->GetController()->IsQuickDisabled();
		if (aiTakeover || (playerControlled && std::find(readyFrame.departedPeerIds.begin(), readyFrame.departedPeerIds.end(), claimant) != readyFrame.departedPeerIds.end())) {
			if (aiTakeover) {
				ScenarioRunner::HandLockstepActorToAI(uid, claimant);
				if (!playerControlled) actor->GetController()->ResetLocalInputState(actor->GetController()->GetInputMode());
				actor->TouchCheckpoint();
			}
			if (playerControlled) {
				MovableMan::ApplyLockstepControlHandoffToActor(*actor, false);
				if (aiTakeover && disabled) actor->GetController()->SetDisabled(true);
			}
			ScenarioRunner::NoteE2eOwnerTransfer(uid);
		}
		if (ScenarioRunner::TakeExpiredDroppedClaim(uid, readyFrame.frame)) {
			const uint8_t seeded = NetActorOwnership::GetSeededOwner(uid);
			if (seeded != 0 && ScenarioRunner::GetLockstepActorOwner(uid, actor->GetTeam(), true) == seeded) {
				MovableMan::ApplyLockstepControlHandoffToActor(*actor, false);
				ScenarioRunner::NoteE2eOwnerTransfer(uid);
				std::cout << "[net-match] claim of actor " << uid << " returned to peer " << static_cast<int>(seeded)
				          << " after seat " << static_cast<int>(claimant) << " expired" << std::endl;
				continue;
			}
		}
		if (ScenarioRunner::IsLockstepActorOwnerGone(uid, actor->GetTeam(), !actor->IsPlayerControlled(), readyFrame.frame)) {
			actor->GetController()->SetDisabled(true);
		}
	}
}

std::vector<long int> MovableMan::BeginLockstepProducingPass(const std::deque<Actor*>& actors, const std::function<bool(const Actor*)>& isLocal) {
	std::vector<long int> producing;
	producing.reserve(actors.size());
	for (Actor* actor: actors) {
		if (isLocal(actor)) {
			producing.push_back(static_cast<long int>(actor->GetUniqueID()));
			actor->GetController()->BeginLocalProduction();
		}
	}
	return producing;
}

void MovableMan::EndLockstepProducingPass(const std::vector<long int>& producing) {
	for (long int actorID: producing) {
		Actor* actor = dynamic_cast<Actor*>(g_MovableMan.FindObjectByUniqueID(actorID));
		if (actor && g_MovableMan.IsActor(actor)) {
			actor->GetController()->EndLocalProduction();
		}
	}
}

void MovableMan::ReconcileLockstepControlBindings() {
	Activity* activity = g_ActivityMan.GetActivity();
	if (!activity || !ScenarioRunner::IsLockstepControllerSyncActive()) {
		return;
	}
	const uint8_t localPeerId = ScenarioRunner::GetLockstepLocalPeerId();
	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		// Only a seat this machine presents holds a binding to release; the shared one belongs to its owner.
		const Actor* controlled = activity->GetLocallyControlledActor(player);
		if (!controlled || !g_MovableMan.IsActor(const_cast<Actor*>(controlled))) {
			continue;
		}
		const int64_t uid = static_cast<int64_t>(controlled->GetUniqueID());
		if (ScenarioRunner::GetLockstepActorOwner(uid, controlled->GetTeam(), !controlled->IsPlayerControlled()) != localPeerId) {
			activity->ReleaseLockstepControlOfActor(player);
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

namespace {
	// The end-of-tick state every peer has to agree on. A held tick runs no simulation but still applies
	// commands and runs the activity, so the same census goes in there too - a divergence inside a hold
	// would otherwise sit under a hash made of terrain alone.
	//
	// The added deques go in beside the live ones: the checkpoint archive writes both (Scene writes
	// GetAllParticles, which concatenates them), so an object only one peer holds has to move the hash.
	// Every attachable in the tree, depth first in attachment order: its identity and where the sim put it.
	void FeedAttachableTransforms(const MOSRotating* parent) {
		for (const Attachable* attachable: parent->GetAttachableList()) {
			const int64_t uniqueID = static_cast<int64_t>(attachable->GetUniqueID());
			const float transform[3] = {attachable->GetPos().m_X, attachable->GetPos().m_Y, attachable->GetRotAngle()};
			g_SimChecksum.Update("attachables", &uniqueID, sizeof(uniqueID));
			g_SimChecksum.Update("attachables", transform, sizeof(transform));
			FeedAttachableTransforms(attachable);
		}
	}

	void FeedSimChecksum(const std::deque<Actor*>& actors, const std::deque<Actor*>& addedActors,
	                     const std::deque<MovableObject*>& items, const std::deque<MovableObject*>& addedItems,
	                     const std::deque<MovableObject*>& particles, const std::deque<MovableObject*>& addedParticles,
	                     const std::list<Actor*>* rosters) {

		auto eachActor = [&](auto&& body) {
			for (Actor* a: actors) body(a);
			for (Actor* a: addedActors) body(a);
		};
		auto eachParticle = [&](auto&& body) {
			for (MovableObject* p: particles) body(p);
			for (MovableObject* p: addedParticles) body(p);
		};
		auto eachItem = [&](auto&& body) {
			for (MovableObject* i: items) body(i);
			for (MovableObject* i: addedItems) body(i);
		};

		eachActor([](Actor* a) {
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
			// The actor's sim-time timers and its limbs' transforms; the alarm timer is the owner's AI perception, so it stays out.
			const double timers[5] = {a->GetLastSecondTimerElapsedSimMS(), a->GetStableRecoverTimerElapsedSimMS(), a->GetHeartBeatTimerElapsedSimMS(),
			                          a->GetNewControlTimerElapsedSimMS(), a->GetDeathTimerElapsedSimMS()};
			g_SimChecksum.Update("actor_timers", &uniqueID, sizeof(uniqueID));
			g_SimChecksum.Update("actor_timers", timers, sizeof(timers));
			FeedAttachableTransforms(a);
		});

		// Controller input state per actor — catches control drift the actors fingerprint misses.
		eachActor([](Actor* a) {
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
			// The seat's input mode and player are routing, not input: the seat that owns an actor is CIM_PLAYER
			// with its player number on its OWN machine and CIM_NETWORK/NoPlayer on every other one, so they are
			// per-peer by construction and belong beside the applied input, never inside it.
			const int32_t inputMode = static_cast<int32_t>(controller->GetInputMode());
			g_SimChecksum.Update("controller_route", &controllerID, sizeof(controllerID));
			g_SimChecksum.Update("controller_route", &inputMode, sizeof(inputMode));
			const int32_t controllerPlayer = controller->GetPlayer();
			g_SimChecksum.Update("controller_route", &controllerPlayer, sizeof(controllerPlayer));
			const int32_t aiMode = static_cast<int32_t>(a->GetAIMode());
			g_SimChecksum.Update("controller", &aiMode, sizeof(aiMode));
		});

		// Compact per-particle fingerprint — uniqueID + pos + vel.
		eachParticle([](MovableObject* p) {
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
		});

		// Same fingerprint for free items — a dropped device's state was only visible as a count before.
		eachItem([](MovableObject* i) {
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
		});

		// Lightweight population metadata — catches spawn/delete count drift. The counts are of the
		// whole census, added deques included, for the same reason the bodies above are.
		const int32_t actorCount = static_cast<int32_t>(actors.size() + addedActors.size());
		g_SimChecksum.Update("scene", &actorCount, sizeof(actorCount));
		const int32_t itemCount = static_cast<int32_t>(items.size() + addedItems.size());
		g_SimChecksum.Update("scene", &itemCount, sizeof(itemCount));
		const int32_t particleCount = static_cast<int32_t>(particles.size() + addedParticles.size());
		g_SimChecksum.Update("scene", &particleCount, sizeof(particleCount));
		for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
			const int32_t rosterSize = static_cast<int32_t>(rosters[team].size());
			g_SimChecksum.Update("scene", &rosterSize, sizeof(rosterSize));
		}
		if (const Activity* activity = g_ActivityMan.GetActivity()) {
			for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
				const float teamFunds = activity->GetTeamFunds(team);
				g_SimChecksum.Update("funds", &teamFunds, sizeof(teamFunds));
			}
		}

	}

	// Snapshot the sim + Lua RNG states here — before the see-ray and MOID-draw futures launch
	// and start mutating g_SimRNG on the thread pool — so the snapshot can't be raced. This is why
	// the randomness feed stays inside Update while the object census moved to the tick's end.
	void FeedSimChecksumRandomness() {
		const std::string rngState = g_SimRNG.SerializeStateForHashing();
		g_SimChecksum.Update("sim_rng", rngState.data(), rngState.size());
		g_LuaMan.HashAllLuaStatesIntoSimChecksum();
	}
} // namespace

bool MovableMan::RunLockstepPausedTick() {
	const uint64_t simTick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
	std::string error;
	NetLockstepReadyFrame readyFrame;
	if (ScenarioRunner::WorldCatchUpActive()) {
		if (!ScenarioRunner::TakeWorldCatchUpReadyFrame(simTick, readyFrame, &error)) {
			ScenarioRunner::SetControllerReplayError("tick " + std::to_string(simTick) + " paused catch-up: " + error);
			return false;
		}
	} else {
		if (!ScenarioRunner::QueueLockstepLocalControllerFrames(simTick, {}, &error)) {
			ScenarioRunner::SetControllerReplayError("tick " + std::to_string(simTick) + " paused queue: " + error);
			return false;
		}
		if (!ScenarioRunner::WaitForLockstepControllerFrame(simTick, readyFrame, &error)) {
			ScenarioRunner::SetControllerReplayError("tick " + std::to_string(simTick) + " paused wait: " + error);
			return false;
		}
	}
	ApplyLockstepSeatReclaims(readyFrame, m_Actors);
	ApplyLockstepLeaveHandoffs(readyFrame, m_Actors, true);
	// Only the game commands apply on a paused tick; the sim itself holds still.
	g_AudioMan.CommitSoundObservations(readyFrame.frame, readyFrame.localObservations, readyFrame.remoteObservations);
	CommitValueObservations(readyFrame.frame, readyFrame.localValueObservations, readyFrame.remoteValueObservations);
	ApplyLockstepGameCommands(readyFrame);
	// A held tick hashes what a simulated one does: the activity, its funds and every Lua state still run
	// while the world waits, so a divergence inside a setup or pause hold is caught by the same exchange.
	// The object census rides the tick's end with every other tick's, so only the randomness goes in here.
	if (g_SimChecksum.IsActive()) {
		FeedSimChecksumRandomness();
	}
	return true;
}

void MovableMan::FeedTickEndChecksum() {
	if (!g_SimChecksum.IsActive()) {
		return;
	}
	// The census is taken where the checkpoint archive is written, so the hash and the archive
	// describe ONE instant: a held tick never drains its added deques, and a running tick can add
	// to them after AbsorbAddedMOs in the same update.
	std::scoped_lock lock(m_AddedActorsMutex, m_AddedItemsMutex, m_AddedParticlesMutex);
	FeedSimChecksum(m_Actors, m_AddedActors, m_Items, m_AddedItems, m_Particles, m_AddedParticles, m_ActorRoster);
	// Keep the census the hash covered, so a capture can be held to describing the same objects.
	m_LastChecksumCensus.clear();
	m_LastChecksumCensus.reserve(m_Actors.size() + m_AddedActors.size() + m_Items.size() +
	                             m_AddedItems.size() + m_Particles.size() + m_AddedParticles.size());
	for (const auto* deque: {&m_Actors, &m_AddedActors}) {
		for (const Actor* a: *deque) m_LastChecksumCensus.push_back(a->GetUniqueID());
	}
	for (const auto* deque: {&m_Items, &m_AddedItems, &m_Particles, &m_AddedParticles}) {
		for (const MovableObject* mo: *deque) m_LastChecksumCensus.push_back(mo->GetUniqueID());
	}
	m_LastChecksumCensusTick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
}

uint8_t MovableMan::ValueObservationAuthority(uint64_t objectUID) const {
	const uint8_t host = ScenarioRunner::GetLockstepHostPeerId();
	if (objectUID == 0) {
		return host;
	}
	const MovableObject* object = const_cast<MovableMan*>(this)->FindObjectByUniqueID(static_cast<long>(objectUID));
	const Actor* actor = object ? dynamic_cast<const Actor*>(object->GetRootParent()) : nullptr;
	if (!actor) {
		return host;
	}
	return ScenarioRunner::GetLockstepActorOwner(static_cast<int64_t>(actor->GetUniqueID()), actor->GetTeam(), !actor->IsPlayerControlled());
}

void MovableMan::CommitValueObservations(uint64_t frame, const std::vector<NetValueObservation>& local, const std::vector<NetValueObservation>& remote) {
	(void)frame;
	std::vector<const NetValueObservation*> observations;
	observations.reserve(local.size() + remote.size());
	for (const NetValueObservation& observation: local) {
		observations.push_back(&observation);
	}
	for (const NetValueObservation& observation: remote) {
		observations.push_back(&observation);
	}
	std::stable_sort(observations.begin(), observations.end(), [](const NetValueObservation* a, const NetValueObservation* b) {
		return std::tie(a->senderPeerId, a->objectUID, a->ordinal) < std::tie(b->senderPeerId, b->objectUID, b->ordinal);
	});
	const bool lockstep = ScenarioRunner::IsLockstepControllerSyncActive();
	for (const NetValueObservation* observation: observations) {
		if (lockstep && observation->senderPeerId != ValueObservationAuthority(observation->objectUID)) {
			++m_ValueObservationsRejected;
			continue;
		}
		MovableObject* object = FindObjectByUniqueID(static_cast<long>(observation->objectUID));
		if (!object) {
			continue;
		}
		MovableObject::PendingValueOp op;
		op.objectUID = observation->objectUID;
		op.map = static_cast<MovableObject::ValueMapKind>(observation->mapKind);
		op.op = static_cast<MovableObject::ValueMapOp>(observation->op);
		op.key = observation->key;
		op.number = observation->numberValue;
		op.text = observation->stringValue;
		op.ordinal = observation->ordinal;
		op.tick = observation->tick;
		object->ApplySharedValueOp(op);
		object->DropMatchingValueOverlay(op);
	}
}

void MovableMan::CommitOfflineValueWrites() {
	for (const MovableObject::PendingValueOp& op: MovableObject::SamplePendingValueOps()) {
		MovableObject* object = FindObjectByUniqueID(static_cast<long>(op.objectUID));
		if (!object) {
			continue;
		}
		object->ApplySharedValueOp(op);
		object->DropMatchingValueOverlay(op);
	}
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
	// The queued MOID draw renumbers m_MOID on the pool while this reads it; join it so one dump holds one tick's numbering.
	g_MovableMan.CompleteQueuedMOIDDrawings();
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
			if (const ADoor* door = dynamic_cast<const ADoor*>(mo)) {
				out << " door=" << static_cast<int>(door->GetDoorState());
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
	DropAllPreviewGhosts();
	m_PreviewGhostPeak = 0;
	m_LastPreviewSwap = PreviewSwap();
	m_RenderHidden.clear();
	m_RenderSubstitutes.clear();
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
	m_ValueObservationsRejected = 0;
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
		if (!candidate || candidate->GetID() != whichID) {
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
	++m_KnownObjectsVersion;
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
		++m_KnownObjectsVersion;
	}
}

// Only a destruction may take an object out of a held copy. Unregistering also happens to live objects:
// a restore detaches every Lua-owned tree, and those have to come back with the world that named them.
void MovableMan::ForgetDestroyedObject(MovableObject* mo) {
	if (m_LinkRoot == mo) {
		m_LinkRoot = nullptr;
	}
	{
		std::lock_guard<std::mutex> guard(m_ObjectRegisteredMutex);
		// By address, not by key: the object may have taken a new identity since the copy was made.
		for (auto* held: m_HeldRegistries) {
			std::erase_if(*held, [mo](const auto& entry) { return entry.second == mo; });
		}
	}
	g_LuaMan.ForgetDestroyedRegisteredMO(mo);
}

MovableObject* MovableMan::ViewIfSpeculating(MovableObject* found) const {
	if (!found || !m_Speculation.active) {
		return found;
	}
	return const_cast<MovableMan*>(this)->SpeculativeView(found);
}

const std::vector<MovableObject*>* MovableMan::GetMOsInBox(const Box& box, int ignoreTeam, bool getsHitByMOsOnly) const {
	std::vector<MovableObject*>* vectorForLua = new std::vector<MovableObject*>();
	*vectorForLua = std::move(g_SceneMan.GetMOIDGrid().GetMOsInBox(box, ignoreTeam, getsHitByMOsOnly));
	if (m_Speculation.active) {
		for (MovableObject*& mo: *vectorForLua) {
			mo = ViewIfSpeculating(mo);
		}
	}
	return vectorForLua;
}

const std::vector<MovableObject*>* MovableMan::GetMOsInRadius(const Vector& centre, float radius, int ignoreTeam, bool getsHitByMOsOnly) const {
	std::vector<MovableObject*>* vectorForLua = new std::vector<MovableObject*>();
	*vectorForLua = std::move(g_SceneMan.GetMOIDGrid().GetMOsInRadius(centre, radius, ignoreTeam, getsHitByMOsOnly));
	if (m_Speculation.active) {
		for (MovableObject*& mo: *vectorForLua) {
			mo = ViewIfSpeculating(mo);
		}
	}
	return vectorForLua;
}

const std::vector<MovableObject*>* MovableMan::GetMOsAtPosition(int pixelX, int pixelY, int ignoreTeam, bool getsHitByMOsOnly) const {
	std::vector<MovableObject*>* vectorForLua = new std::vector<MovableObject*>();
	*vectorForLua = std::move(g_SceneMan.GetMOIDGrid().GetMOsAtPosition(pixelX, pixelY, ignoreTeam, getsHitByMOsOnly));
	if (m_Speculation.active) {
		for (MovableObject*& mo: *vectorForLua) {
			mo = ViewIfSpeculating(mo);
		}
	}
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
		~CaptureAllocationState() { g_SimRNG = sim; g_RenderRNG = render; MovableObject::PinUniqueIDCounter(uid); }
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
	ArmLuaCheckpointBarrier();
	struct GraphWalk {
		GraphWalk() { CheckpointGraphIndex::Get().BeginWalk(); }
		~GraphWalk() { CheckpointGraphIndex::Get().EndWalk(); }
	};
	bool graphsCaptured = false;
	{
		GraphWalk walk;
		graphsCaptured = CaptureScriptGraphs(out.luaGraphs, luaProblems);
	}
	if (!graphsCaptured) {
		for (const std::string& problem: luaProblems) {
			std::cout << "[scriptgraph] capture refused: " << problem << std::endl;
		}
		out.luaGraphs.clear();
		return false;
	}
	CheckpointCow::Get().RememberLua(out.luaGraphs, LuaCheckpointWriteGeneration());
	g_LuaMan.ArmCheckpointWriteTrap();
	const long counter = MovableObject::GetUniqueIDCounter();
	{
		MovableObject::FaithfulCloneScope scope(false);
		if (const Activity* activity = g_ActivityMan.GetActivity()) out.activity.reset(static_cast<Activity*>(activity->Clone()));
		if (const Activity* activity = g_ActivityMan.GetCheckpointStartActivity()) out.startActivity.reset(static_cast<Activity*>(activity->Clone()));
		out.actors.reserve(m_Actors.size());
		out.items.reserve(m_Items.size());
		out.particles.reserve(m_Particles.size());
		const bool profileCapture = std::getenv("CC_CAPTURE_PROFILE") != nullptr;
		std::map<std::string, std::pair<int, double>> profile;
		const auto timed = [&profile, profileCapture](const MovableObject* mo, auto&& fn) {
			if (!profileCapture) { fn(); return; }
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
		if (profileCapture) {
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
	} catch (const std::exception& error) {
		out.Clear();
		std::cout << "[snapshot] capture refused: " << error.what() << std::endl;
		return false;
	}
}

void MovableMan::WorldSnapshotRing::Reset(uint16_t windowTicks) {
	m_Entries.clear();
	m_Cache = {};
	m_Capacity = windowTicks == 0 ? 0 : static_cast<size_t>(windowTicks) + 1;
	m_LastCaptureUs = 0;
}

bool MovableMan::WorldSnapshotRing::CaptureCommitted(uint64_t tick) {
	if (m_Capacity == 0 || (!m_Entries.empty() &&
	    (m_Entries.back().tick == UINT64_MAX || tick != m_Entries.back().tick + 1)) ||
	    static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) != tick) return false;
	auto snapshot = std::make_unique<WorldSnapshot>();
	const auto start = std::chrono::steady_clock::now();
	m_Cache.Begin();
	bool captured = false;
	{
		CheckpointWriter::CacheScope cache(&m_Cache);
		captured = g_MovableMan.CaptureWorld(*snapshot);
	}
	m_Cache.RetireUnused();
	const bool stored = captured && StoreCommitted(tick, std::move(snapshot));
	m_LastCaptureUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
	return stored;
}

bool MovableMan::WorldSnapshotRing::StoreCommitted(uint64_t tick, std::unique_ptr<WorldSnapshot> snapshot) {
	if (!snapshot || m_Capacity == 0 || (!m_Entries.empty() &&
	    (m_Entries.back().tick == UINT64_MAX || tick != m_Entries.back().tick + 1))) return false;
	m_Entries.push_back({tick, std::move(snapshot)});
	while (m_Entries.size() > m_Capacity) m_Entries.pop_front();
	return true;
}

const MovableMan::WorldSnapshot* MovableMan::WorldSnapshotRing::Find(uint64_t tick) const {
	const auto found = std::lower_bound(m_Entries.begin(), m_Entries.end(), tick,
	                                  [](const Entry& entry, uint64_t value) { return entry.tick < value; });
	return found != m_Entries.end() && found->tick == tick ? found->snapshot.get() : nullptr;
}

void MovableMan::WorldSnapshotRing::DiscardAfter(uint64_t tick) {
	while (!m_Entries.empty() && m_Entries.back().tick > tick) m_Entries.pop_back();
}

bool MovableMan::RestoreWorldCandidate(const WorldSnapshot& in, const std::vector<std::string>& luaGraphs) {
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
	if (!RestoreScriptGraphs(luaGraphs, &luaError)) {
		std::cout << "[scriptgraph] restore failed: " << luaError << std::endl;
		return false;
	}
	MovableObject::PinUniqueIDCounter(in.uniqueIDCounter);
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
	if (Activity* activity = g_ActivityMan.GetActivity()) {
		if (!activity->ResolveCheckpointReferences()) return false;
		activity->ClearAllPresentationViews();
		activity->FillPresentationFromPreview(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()), static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()));
	}
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
		++m_KnownObjectsVersion;
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
		++m_KnownObjectsVersion;
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
		g_LuaMan.GetStateByIndex(static_cast<int>(index)).CallScriptGraph("releaseObjects");
	}
	MovableObject::PinUniqueIDCounter(in.uniqueIDCounter);
	if (in.terrain.width) restored = in.terrain.Restore() && restored;
	restored = g_MusicMan.RestoreCheckpointOwners(in.musicOwners) && restored;
	g_AudioMan.RestoreCheckpointSoundRegistry(std::move(in.soundRegistrations));
	restored = g_FrameMan.LoadCheckpoint(in.frameState) && restored;
	return g_ActivityMan.RestoreRuntimeGlobals(in.runtimeGlobals) && restored;
}

bool MovableMan::CaptureScriptGraphs(std::vector<CheckpointText>& graphs, std::vector<std::string>& problems, bool* fromAnImage, const std::function<void()>& whileWaiting) const {
	AudioMan::CheckpointRegistryScope captureSounds;
	LuaCheckpointBarrierPause barrierPause;
	// A world capture opens the lookups at its fence; a capture of the graphs alone opens its own.
	std::optional<LuaScriptGraphNativeCaptureScope> nativeCapture;
	if (!LuaScriptGraphNativeCaptureScope::Current()) nativeCapture.emplace();
	struct PathCapture {
		PathCapture() { g_LuaMan.BeginPathCallbackCapture(); }
		~PathCapture() { g_LuaMan.EndPathCallbackCapture(); }
	} pathCapture;
	auto& states = g_LuaMan.GetThreadedScriptStates();
	const auto captureAll = [&](bool frozen, std::vector<std::string>& into) {
		graphs.clear();
		const auto capture = [&](LuaStateWrapper& state) {
			CheckpointText text;
			const bool complete = state.CaptureScriptGraph(text, into, frozen);
			graphs.push_back(std::move(text));
			return complete;
		};
		// The live walk collects every state's refusals; a frozen capture stops at the first state it cannot freeze.
		bool complete = capture(g_LuaMan.GetMasterScriptState());
		for (LuaStateWrapper& state: states) {
			if (frozen && !complete) break;
			complete = capture(state) && complete;
		}
		return complete;
	};
	// Every state off a frozen image or none: the live walk's index and chunk caches stay coherent
	// only while a walk covers every state, so one state that cannot freeze sends the whole capture that way.
	bool frozen = g_LuaMan.GetMasterScriptState().FrozenCaptureAvailable();
	for (const LuaStateWrapper& state: states) frozen = frozen && state.FrozenCaptureAvailable();
	const auto started = std::chrono::steady_clock::now();
	const auto elapsed = [&] { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count(); };
	if (frozen) {
		FrozenCaptureStats stats;
		std::vector<std::string> frozenProblems;
		// The page copies land off this thread; a state waits for its own at its gate before it runs again.
		const bool complete = LuaStateWrapper::CaptureFrozenScriptGraphs(graphs, frozenProblems, stats, whileWaiting);
		std::cout << "[script-graph-capture] path=frozen states=" << stats.states << " us=" << elapsed() << " native_us=" << stats.nativeUs
		          << " freeze_us=" << stats.heapUs << " copy_us=" << stats.copyUs << " pages=" << stats.pages << " bytes=" << stats.bytes
		          << " userdata=" << stats.userdata << " cached=" << stats.cached << " iterators=" << stats.iterators << " owned=" << stats.owned
		          << " callbacks_us=" << stats.callbacksUs << " roots_us=" << stats.rootsUs << " enum_us=" << stats.enumUs << " world_us=" << stats.worldUs << " answer_us=" << stats.answerUs
		          << " receivers_us=" << stats.receiversUs << " activity_us=" << stats.activityUs << " async_us=" << stats.asyncUs << " cache_us=" << stats.cacheUs << " objects_us=" << stats.objectsUs << " scripts=" << stats.cachedScripts
		          << " prev_faults=" << stats.faults << " prev_fault_us=" << stats.faultUs
		          << " copy_mapped=" << stats.copyMapped << " copy_idle=" << stats.copyIdle << std::endl;
		if (complete) {
			if (fromAnImage) *fromAnImage = true;
			return true;
		}
		for (const std::string& problem: frozenProblems) std::cout << "[frozen-graph] fallback: " << problem << std::endl;
	}
	CheckpointGraphIndex::Get().BeginWalk();
	const bool complete = captureAll(false, problems);
	CheckpointGraphIndex::Get().EndWalk();
	std::cout << "[script-graph-capture] path=live states=" << 1 + states.size() << " us=" << elapsed() << std::endl;
	return complete;
}

bool MovableMan::SerializeScriptGraphs(std::vector<std::string>& graphs, std::vector<std::string>& problems) const {
	AudioMan::CheckpointRegistryScope captureSounds;
	struct PathCapture {
		PathCapture() { g_LuaMan.BeginPathCallbackCapture(); }
		~PathCapture() { g_LuaMan.EndPathCallbackCapture(); }
	} pathCapture;
	// One walk over every state: a state's walk that ended alone would clear the others' dirty roots.
	struct IndexWalk {
		IndexWalk() { CheckpointGraphIndex::Get().BeginWalk(); }
		~IndexWalk() { CheckpointGraphIndex::Get().EndWalk(); }
	} indexWalk;
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

MovableMan::KnownObjectsScope::KnownObjectsScope() {
	MovableMan& manager = g_MovableMan;
	{
		std::lock_guard<std::mutex> guard(manager.m_ObjectRegisteredMutex);
		m_Version = manager.m_KnownObjectsVersion.load();
	}
	m_Previous = manager.m_KnownObjectsScope.exchange(this);
}

void MovableMan::KnownObjectsScope::Copy() const {
	std::call_once(m_Copied, [this] {
		CaptureSentinel::NoteCreation("known-objects index", this);
		MovableMan& manager = g_MovableMan;
		{
			std::lock_guard<std::mutex> guard(manager.m_ObjectRegisteredMutex);
			m_ByIdentity.reserve(manager.m_KnownObjects.size());
			for (const auto& [uid, object]: manager.m_KnownObjects) m_ByIdentity.push_back(object);
		}
		m_ByAddress.assign(m_ByIdentity.begin(), m_ByIdentity.end());
		std::sort(m_ByAddress.begin(), m_ByAddress.end());
	});
}

MovableMan::KnownObjectsScope::~KnownObjectsScope() {
	g_MovableMan.m_KnownObjectsScope.store(m_Previous);
}

std::string MovableMan::KnownObjectsScopeMissedChange() {
	std::list<SceneObject*> actors;
	GetAllActors(false, actors);
	auto* live = actors.empty() ? nullptr : dynamic_cast<MovableObject*>(actors.front());
	if (!live) return "no live actor";
	std::string missed;
	const auto agrees = [this, live](const char* name, std::string& into) {
		bool scanned = false;
		{
			std::lock_guard<std::mutex> guard(m_ObjectRegisteredMutex);
			for (const auto& [uid, object]: m_KnownObjects) scanned = scanned || object == live;
		}
		if (IsKnownObject(live) != scanned && into.empty()) into = name;
	};
	KnownObjectsScope scope;
	agrees("none", missed);
	UnregisterObject(live);
	agrees("unregister", missed);
	RegisterObject(live);
	agrees("register", missed);
	return missed;
}

std::vector<MovableObject*> MovableMan::SnapshotKnownObjects() {
	if (const KnownObjectsScope* scope = m_KnownObjectsScope.load(std::memory_order_acquire); scope && scope->m_Version == m_KnownObjectsVersion.load(std::memory_order_acquire)) {
		scope->Copy();
		return scope->m_ByIdentity;
	}
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
	AudioMan::RestorePlayPhaseScope playPhase("RestoreScriptGraphs");
	if (!ValidateScriptGraphs(graphs, error)) return false;
	std::vector<std::string> errors;
	if (!reuseHeld) g_LuaMan.ResetPathCallbacks();
	struct AllocationState {
		long uidCounter = MovableObject::GetUniqueIDCounter();
		~AllocationState() {
			MovableObject::PinUniqueIDCounter(uidCounter);
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
		state.CallScriptGraph("clearPrepared");
		if (reuseHeld) {
			state.CallScriptGraph("releaseObjects");
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
	if (const KnownObjectsScope* scope = m_KnownObjectsScope.load(std::memory_order_acquire); scope && scope->m_Version == m_KnownObjectsVersion.load(std::memory_order_acquire)) {
		scope->Copy();
		return std::binary_search(scope->m_ByAddress.begin(), scope->m_ByAddress.end(), object);
	}
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
		m_ContiguousActorIDs.erase(actor);
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

void MovableMan::RecordSpeculativeSpawnMeta(MovableObject* mo) {
	if (!m_Speculation.active || !mo) {
		return;
	}
	Speculation::Spawn meta;
	meta.object = mo;
	meta.emitterUID = SoundSimulationScope::CurrentKey().objectUID;
	meta.tick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
	m_Speculation.spawnMeta[mo] = meta;
}

void MovableMan::DestroySpeculativeSpawn(MovableObject* mo) {
	if (!mo) {
		return;
	}
	if (Actor* actor = dynamic_cast<Actor*>(mo)) {
		if (actor->GetTeam() >= 0) {
			RemoveActorFromTeamRoster(actor);
		}
		m_ValidActors.erase(actor);
	}
	m_ValidItems.erase(mo);
	m_ValidParticles.erase(mo);
	mo->DestroyScriptState();
	delete mo;
}

void MovableMan::HarvestSpeculativeSpawns() {
	if (!m_Speculation.active) {
		return;
	}
	std::scoped_lock lock(m_AddedActorsMutex, m_AddedItemsMutex, m_AddedParticlesMutex);
	const auto take = [this](auto& queue, size_t mark) {
		for (size_t i = mark; i < queue.size(); ++i) {
			MovableObject* mo = queue[i];
			Speculation::Spawn spawn;
			spawn.object = mo;
			if (const auto found = m_Speculation.spawnMeta.find(mo); found != m_Speculation.spawnMeta.end()) {
				spawn.emitterUID = found->second.emitterUID;
				spawn.tick = found->second.tick;
				m_Speculation.spawnMeta.erase(found);
			} else {
				spawn.emitterUID = SoundSimulationScope::CurrentKey().objectUID;
				spawn.tick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
			}
			m_Speculation.spawns.push_back(spawn);
		}
		queue.resize(mark);
	};
	take(m_AddedActors, m_Speculation.mark.actors);
	take(m_AddedItems, m_Speculation.mark.items);
	take(m_AddedParticles, m_Speculation.mark.particles);
}

void MovableMan::TravelSpeculativeSpawns() {
	if (!m_Speculation.active) {
		return;
	}
	for (Speculation::Spawn& spawn: m_Speculation.spawns) {
		if (!spawn.object || spawn.object->IsSetToDelete()) {
			continue;
		}
		TravelStage(spawn.object, dynamic_cast<Actor*>(spawn.object) != nullptr);
	}
	for (Speculation::Spawn& spawn: m_Speculation.spawns) {
		if (!spawn.object || spawn.object->IsSetToDelete()) {
			continue;
		}
		UpdateStage(spawn.object, dynamic_cast<Actor*>(spawn.object) != nullptr);
	}
}

size_t MovableMan::GetSpeculativeSpawnCount() const {
	return m_Speculation.spawns.size();
}

std::string MovableMan::DescribeSpeculativeSpawns() const {
	std::string out;
	for (const Speculation::Spawn& spawn: m_Speculation.spawns) {
		if (!spawn.object) {
			continue;
		}
		out += (out.empty() ? "" : ",") + spawn.object->GetPresetName();
	}
	return out;
}

static bool SameGhostKey(const PreviewEventLedger::Key& a, const PreviewEventLedger::Key& b) {
	return a.kind == b.kind && a.emitterUID == b.emitterUID && a.presetHash == b.presetHash && a.tick == b.tick && a.seq == b.seq;
}

bool MovableMan::InstallPreviewGhost(MovableObject* mo, const PreviewEventLedger::Key& key, uint64_t poseTick) {
	if (!mo) {
		return false;
	}
	// NextKey numbers every emission inside its tick, so one live ghost per key: a second would never be reposed or dropped.
	for (const PreviewGhost& ghost: m_PreviewGhosts) {
		if (SameGhostKey(ghost.key, key)) {
			return false;
		}
	}
	mo->SetAsAddedToMovableMan(false);
	mo->DestroyScriptState();
	m_ValidActors.erase(mo);
	m_ValidItems.erase(mo);
	m_ValidParticles.erase(mo);
	if (Actor* actor = dynamic_cast<Actor*>(mo); actor && actor->GetTeam() >= 0) {
		RemoveActorFromTeamRoster(actor);
	}
	UnregisterObject(mo);
	mo->SetAsNoID();
	PreviewGhost ghost;
	ghost.object = mo;
	ghost.key = key;
	ghost.poseTick = poseTick;
	m_PreviewGhosts.push_back(std::move(ghost));
	if (m_PreviewGhosts.size() > m_PreviewGhostPeak) {
		m_PreviewGhostPeak = m_PreviewGhosts.size();
	}
	return true;
}

void MovableMan::ReposePreviewGhost(const PreviewEventLedger::Key& key, const MovableObject& spawn, uint64_t poseTick) {
	for (PreviewGhost& ghost: m_PreviewGhosts) {
		if (ghost.object && SameGhostKey(ghost.key, key)) {
			ghost.object->SetPos(spawn.GetPos());
			ghost.object->SetVel(spawn.GetVel());
			ghost.poseTick = poseTick;
			// A re-pose after the adoption moves the pose the adoptee is travelling to, so its tag follows.
			if (MovableObject* adoptee = const_cast<MovableObject*>(ghost.adoptee.get())) {
				adoptee->HoldForPreviewAdoption(key, poseTick);
			}
			return;
		}
	}
}

void MovableMan::AdoptPreviewGhost(const PreviewEventLedger::Key& key, MovableObject* adoptee, uint64_t committedTick) {
	for (size_t index = 0; index < m_PreviewGhosts.size(); ++index) {
		PreviewGhost& ghost = m_PreviewGhosts[index];
		if (!SameGhostKey(ghost.key, key)) {
			continue;
		}
		// Nothing to lead with: the ghost is at or behind the spawn, so the pixel changes hands this tick.
		if (!adoptee || !ghost.object || ghost.poseTick <= committedTick) {
			DropPreviewGhost(key);
			return;
		}
		ghost.adopted = true;
		ghost.adoptionTick = committedTick;
		ghost.adoptee = adoptee;
		adoptee->HoldForPreviewAdoption(key, ghost.poseTick);
		return;
	}
}

void MovableMan::ReleaseAdoptionHold(PreviewGhost& ghost) {
	MovableObject* adoptee = const_cast<MovableObject*>(ghost.adoptee.get());
	if (!adoptee) {
		return;
	}
	adoptee->ReleasePreviewAdoptionHold();
	ghost.adoptee = nullptr;
}

void MovableMan::ResolvePreviewAdoptions(uint64_t committedTick) {
	for (size_t index = 0; index < m_PreviewGhosts.size();) {
		PreviewGhost& ghost = m_PreviewGhosts[index];
		const MovableObject* adoptee = ghost.adoptee.get();
		if (!ghost.adopted || (adoptee && committedTick < ghost.poseTick)) {
			++index;
			continue;
		}
		if (adoptee && ghost.object) {
			m_LastPreviewSwap.tick = committedTick;
			m_LastPreviewSwap.adoptionTick = ghost.adoptionTick;
			m_LastPreviewSwap.leadTicks = ghost.poseTick - ghost.adoptionTick;
			m_LastPreviewSwap.poseDelta = g_SceneMan.ShortestDistance(ghost.object->GetPos(), adoptee->GetPos(), true).GetMagnitude();
			m_LastPreviewSwap.adopteeUID = adoptee->GetUniqueID();
			++m_LastPreviewSwap.count;
			FrameMan::FeelPreviewSwap(ghost.adoptionTick, committedTick, m_LastPreviewSwap.leadTicks, m_LastPreviewSwap.poseDelta);
		}
		ReleaseAdoptionHold(ghost);
		delete ghost.object;
		m_PreviewGhosts.erase(m_PreviewGhosts.begin() + index);
	}
}

void MovableMan::DropPreviewGhost(const PreviewEventLedger::Key& key) {
	for (auto ghost = m_PreviewGhosts.begin(); ghost != m_PreviewGhosts.end(); ++ghost) {
		if (SameGhostKey(ghost->key, key)) {
			ReleaseAdoptionHold(*ghost);
			delete ghost->object;
			m_PreviewGhosts.erase(ghost);
			return;
		}
	}
}

void MovableMan::DropAllPreviewGhosts() {
	for (PreviewGhost& ghost: m_PreviewGhosts) {
		ReleaseAdoptionHold(ghost);
		delete ghost.object;
	}
	m_PreviewGhosts.clear();
}

std::vector<MovableMan::PreviewGhostState> MovableMan::GetPreviewGhostStates() const {
	std::vector<PreviewGhostState> out;
	out.reserve(m_PreviewGhosts.size());
	for (const PreviewGhost& ghost: m_PreviewGhosts) {
		if (!ghost.object) {
			continue;
		}
		PreviewGhostState state{ghost.key, ghost.object->GetPos(), ghost.object->GetVel(), ghost.object->GetGlobalAccScalar(), ghost.object->GetAirResistance(), ghost.object->GetAirThreshold()};
		state.poseTick = ghost.poseTick;
		state.adoptionTick = ghost.adoptionTick;
		state.adopted = ghost.adopted;
		if (const MovableObject* adoptee = ghost.adoptee.get()) {
			state.adopteeHeld = adoptee->IsHeldForPreviewAdoption();
			state.adopteeUID = adoptee->GetUniqueID();
			state.adopteePos = adoptee->GetPos();
			state.adopteeVel = adoptee->GetVel();
			state.adopteeGlobalAccScalar = adoptee->GetGlobalAccScalar();
			state.adopteeAirResistance = adoptee->GetAirResistance();
			state.adopteeAirThreshold = adoptee->GetAirThreshold();
		}
		out.push_back(state);
	}
	return out;
}

bool MovableMan::ApplyQueuedPurchaseDelivery(GameActivity& activity, const NetGameDeliverCargo& delivery, uint8_t senderPeerId, ACraft* craft) {
	std::list<const SceneObject*> purchases;
	for (const NetGameCargoItem& item: delivery.cargo) {
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
	order.team = delivery.team;
	order.passengerAIMode = delivery.passengerAIMode;
	order.waypoint = Vector(delivery.waypointX, delivery.waypointY);
	if (delivery.targetUID != 0) {
		order.pTargetMO = dynamic_cast<Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(delivery.targetUID)));
		if (!order.pTargetMO) {
			g_ConsoleMan.PrintString("NETWORK: buy order target not found: UID " + std::to_string(delivery.targetUID));
			std::cout << "[net-match] buy order target not found: UID " << delivery.targetUID << std::endl;
		}
	}
	order.totalCost = delivery.cost;
	order.orderedByPlayer = senderPeerId == ScenarioRunner::GetLockstepLocalPeerId() ? delivery.orderedByPlayer : Players::NoPlayer;
	order.aiReturnCraft = delivery.returnCraft;
	Vector landingZone(delivery.posX, delivery.posY);
	g_SceneMan.ForceBounds(landingZone);
	order.landingZone = landingZone;
	order.multiOrderYOffset = delivery.multiOrderYOffset;
	craft->SetNetworkDelivery(true);
	const float fundsBefore = activity.GetTeamFunds(delivery.team);
	if (delivery.cost > fundsBefore) {
		delete craft;
		activity.ClearPreviewedPurchase(order.orderedByPlayer, order.team, order.totalCost);
		g_ConsoleMan.PrintString("NETWORK: buy order rejected - insufficient team funds");
		std::cout << "[net-match] buy order rejected: team " << delivery.team << " cost " << delivery.cost << " > funds " << fundsBefore << std::endl;
		return false;
	}
	if (!activity.QueuePurchaseDelivery(craft, order)) {
		delete craft;
		activity.ClearPreviewedPurchase(order.orderedByPlayer, order.team, order.totalCost);
		g_ConsoleMan.PrintString("NETWORK: buy order did not queue: team " + std::to_string(delivery.team));
		std::cout << "[net-match] buy order did not queue: team " << delivery.team << std::endl;
		return false;
	}
	std::cout << "[net-match] buy order queued: team " << delivery.team << " cost " << delivery.cost << " funds " << fundsBefore << " -> " << activity.GetTeamFunds(delivery.team) << " items " << delivery.cargo.size() << std::endl;
	return true;
}

bool MovableMan::PreviewGhostsAreUnregistered() const {
	for (const PreviewGhost& ghost: m_PreviewGhosts) {
		const MovableObject* mo = ghost.object;
		if (!mo) {
			continue;
		}
		if (mo->GetID() != g_NoMOID || ValidMO(mo)) {
			return false;
		}
	}
	return true;
}

static bool IsNamedSpeculativeSpawn(const MovableObject* mo) {
	return mo && mo->GetPresetName() != "None" && !mo->GetPresetName().empty();
}

static void NoteProjectileEvent(const PreviewEventLedger::Key& key, bool predicted) {
	for (const PreviewEventLedger::EventStart& start: PreviewEventLedger::GetEventStarts()) {
		if (start.kind == key.kind && start.emitterUID == key.emitterUID && start.eventTick == key.tick && start.seq == key.seq && start.predicted == predicted) {
			return;
		}
	}
	PreviewEventLedger::NoteEventStart(PreviewEventLedger::CommittedTick(), key, predicted);
}

void MovableMan::TakePreviewSpawn(MovableObject* particle) {
	if (!IsNamedSpeculativeSpawn(particle)) {
		return;
	}
	const uint64_t emitter = SoundSimulationScope::CurrentKey().objectUID;
	if (PreviewEventLedger::IsArmed() || !PreviewEventLedger::IsPreviewedEmitter(emitter)) {
		return;
	}
	const uint64_t tick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
	const uint64_t presetHash = Hash(particle->GetPresetName() + "@" + std::to_string(particle->GetModuleID()));
	const PreviewEventLedger::Key key = PreviewEventLedger::NextKey(PreviewEventLedger::Projectile, emitter, 0, presetHash, tick);
	std::vector<int> voices;
	if (PreviewEventLedger::Consume(key, voices)) {
		// The ghost already shows where this spawn is going, so it keeps the pixel until the spawn gets there.
		AdoptPreviewGhost(key, particle, tick);
		NoteProjectileEvent(key, false);
	}
}

void MovableMan::DisposeSpeculativeSpawns() {
	// Still on the preview's advanced clock: this is the tick every spawn was travelled to.
	const uint64_t horizonTick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
	for (Speculation::Spawn& spawn: m_Speculation.spawns) {
		MovableObject* mo = spawn.object;
		if (!mo) {
			continue;
		}
		if (mo->IsSetToDelete() || !IsNamedSpeculativeSpawn(mo) || !PreviewEventLedger::IsPreviewedEmitter(spawn.emitterUID)) {
			DestroySpeculativeSpawn(mo);
			continue;
		}
		const uint64_t presetHash = Hash(mo->GetPresetName() + "@" + std::to_string(mo->GetModuleID()));
		const PreviewEventLedger::Key key = PreviewEventLedger::NextKey(PreviewEventLedger::Projectile, spawn.emitterUID, 0, presetHash, spawn.tick);
		if (PreviewEventLedger::AlreadyPlayed(key)) {
			// A later preview re-runs the same shot: its spawn carries the ghost to the new horizon.
			ReposePreviewGhost(key, *mo, horizonTick);
			DestroySpeculativeSpawn(mo);
			continue;
		}
		PreviewEventLedger::Insert(key, {});
		NoteProjectileEvent(key, true);
		if (!InstallPreviewGhost(mo, key, horizonTick)) {
			// The key's ghost is still live from an earlier preview, so this spawn carries it to the new horizon.
			ReposePreviewGhost(key, *mo, horizonTick);
			DestroySpeculativeSpawn(mo);
		}
	}
	m_Speculation.spawns.clear();
	m_Speculation.spawnMeta.clear();
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
	for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
		m_ActorRoster[team] = m_Speculation.rosters[team];
		m_SortTeamRoster[team] = m_Speculation.sortRoster[team];
		m_Speculation.rosters[team].clear();
	}
	DisposeSpeculativeSpawns();
	DiscardAddedSince(m_Speculation.mark);
	for (auto& [resident, shadow]: m_Speculation.shadows) {
		if (shadow.inWorld) {
			delete shadow.object;
		}
	}
	if (takenResidents) {
		*takenResidents = m_Speculation.taken;
	}
	m_Speculation.shadows.clear();
	m_Speculation.residents.clear();
	m_Speculation.taken.clear();
}

MovableObject* MovableMan::OverlaySurvivorOf(MovableObject* mo, const std::unordered_set<const MovableObject*>& retiring) const {
	if (!mo || !m_Speculation.active) {
		return mo;
	}
	const auto resident = m_Speculation.residents.find(mo);
	if (resident != m_Speculation.residents.end()) {
		// A taken shadow lives on with its taker unless the taker retires too.
		const Speculation::Shadow& shadow = m_Speculation.shadows.at(resident->second);
		if (shadow.inWorld || retiring.count(mo) > 0) {
			return resident->second;
		}
		return mo;
	}
	if (retiring.count(mo) == 0) {
		return mo;
	}
	// mo is read only once found in a retiring shadow's tree, which is alive until EndSpeculation.
	for (const auto& [shadowRoot, shadowResident]: m_Speculation.residents) {
		if (retiring.count(shadowRoot) == 0) {
			continue;
		}
		std::unordered_set<const Entity*> visited;
		std::unordered_set<const MovableObject*> parts;
		CollectOwnedMovableObjects(shadowRoot, visited, parts);
		if (parts.count(mo) > 0) {
			return shadowResident->FindPartByUniqueID(mo->GetUniqueID());
		}
	}
	return nullptr;
}

std::unordered_set<const MovableObject*> MovableMan::RetiringOverlayObjects() {
	std::unordered_set<const MovableObject*> retiring;
	// The previews, the render substitutes and the residents outlive the overlay, so no walk enters them.
	std::unordered_set<const Entity*> visited;
	std::unordered_set<const MovableObject*> surviving;
	for (const MovableObject* preview: LuaMan::PreviewRoots()) {
		CollectOwnedMovableObjects(preview, visited, surviving);
	}
	for (const MovableObject* substitute: m_RenderSubstitutes) {
		CollectOwnedMovableObjects(substitute, visited, surviving);
	}
	for (const auto& [resident, shadow]: m_Speculation.shadows) {
		visited.insert(resident);
	}
	const auto retire = [&visited, &retiring](const MovableObject* root) {
		if (root) {
			CollectOwnedMovableObjects(root, visited, retiring);
		}
	};
	std::scoped_lock lock(m_AddedActorsMutex, m_AddedItemsMutex, m_AddedParticlesMutex);
	for (const Speculation::Spawn& spawn: m_Speculation.spawns) {
		retire(spawn.object);
	}
	for (size_t i = m_Speculation.mark.actors; i < m_AddedActors.size(); ++i) {
		retire(m_AddedActors[i]);
	}
	for (size_t i = m_Speculation.mark.items; i < m_AddedItems.size(); ++i) {
		retire(m_AddedItems[i]);
	}
	for (size_t i = m_Speculation.mark.particles; i < m_AddedParticles.size(); ++i) {
		retire(m_AddedParticles[i]);
	}
	for (const auto& [resident, shadow]: m_Speculation.shadows) {
		if (shadow.inWorld) {
			retire(shadow.object);
		}
	}
	// A spawnMeta key that left the add queues may already be deleted, so it is listed but never read.
	for (const auto& entry: m_Speculation.spawnMeta) {
		if (entry.first && surviving.count(entry.first) == 0 && m_Speculation.shadows.count(entry.first) == 0) {
			retiring.insert(entry.first);
		}
	}
	return retiring;
}

int MovableMan::ResidentKind(const MovableObject* mo) const {
	if (!mo) {
		return 0;
	}
	// Whatever entered the add queues during the speculation is speculative itself, not a resident.
	if (m_Speculation.active) {
		for (const Speculation::Spawn& spawn: m_Speculation.spawns) {
			if (spawn.object == mo) {
				return 0;
			}
		}
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

MovableMan::ControllerBoundaryBaseline MovableMan::CaptureControllerBoundary(Actor* actor) {
	const AHuman* human = dynamic_cast<const AHuman*>(actor);
	const ACraft* craft = dynamic_cast<const ACraft*>(actor);
	return {actor, actor->GetAimAngle(false), actor->IsHFlipped(),
	        human && human->GetEquippedItem() ? static_cast<int64_t>(human->GetEquippedItem()->GetUniqueID()) : 0,
	        human && human->GetEquippedBGItem() ? static_cast<int64_t>(human->GetEquippedBGItem()->GetUniqueID()) : 0,
	        craft ? craft->GetHatchState() : 0u,
	        craft ? craft->GetHatchTimerStartTicks() : 0};
}

void MovableMan::RestoreControllerBoundary(const ControllerBoundaryBaseline& before, long long simTick) {
	Actor* actor = before.actor;
	if (const float aim = actor->GetAimAngle(false); aim != before.aim) {
		actor->MarkOffWireAim(simTick, aim);
		actor->RestoreAimAngle(before.aim);
		++m_ControllerBoundaryStats.aimIntents;
	}
	if (const bool flipped = actor->IsHFlipped(); flipped != before.flipped) {
		actor->MarkOffWireFlip(simTick, flipped);
		actor->SetHFlipped(before.flipped);
		++m_ControllerBoundaryStats.flipIntents;
	}
	if (ACraft* craft = dynamic_cast<ACraft*>(actor)) {
		if (const unsigned int hatch = craft->GetHatchState(); hatch != before.hatch) {
			craft->MarkOffWireHatch(simTick, hatch == ACraft::OPENING || hatch == ACraft::OPEN);
			craft->RestoreHatch(before.hatch, before.hatchTimerStart);
		}
	}
	if (AHuman* human = dynamic_cast<AHuman*>(actor)) {
		const int64_t fg = human->GetEquippedItem() ? static_cast<int64_t>(human->GetEquippedItem()->GetUniqueID()) : 0;
		const int64_t bg = human->GetEquippedBGItem() ? static_cast<int64_t>(human->GetEquippedBGItem()->GetUniqueID()) : 0;
		if (fg != before.fg || bg != before.bg) {
			ReportControllerBoundaryViolation("the equipment", actor);
		}
	}
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

void MovableMan::NoteLocalAIPassScriptMessage() {
	// Threaded AI states deliver local messages concurrently.
	m_ControllerBoundaryStats.localScriptMessages.fetch_add(1, std::memory_order_relaxed);
}

bool MovableMan::IsHiddenFromRender(const MovableObject* mo) const {
	// Either a preview draws in its place, or the ghost it adopted still shows the pose it is travelling to.
	return mo->IsHeldForPreviewAdoption() || (!m_RenderHidden.empty() && m_RenderHidden.count(mo) > 0);
}

void MovableMan::HideForRender(const MovableObject* mo, bool hidden) {
	if (hidden) {
		m_RenderHidden.insert(mo);
	} else {
		m_RenderHidden.erase(mo);
	}
}

void MovableMan::AddRenderSubstitute(const MovableObject* mo) {
	if (mo) {
		m_RenderSubstitutes.insert(mo);
	}
}

void MovableMan::ClearRenderSubstitutes() {
	m_RenderSubstitutes.clear();
}

bool MovableMan::IsRenderSubstitute(const MovableObject* mo) const {
	return mo && m_RenderSubstitutes.count(mo) > 0;
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
	m_ContiguousActorIDs.clear();
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
	NetActorOwnership::ClearSeededOwners();
	s_LockstepFrameClaims.clear();
	m_MOIDIndex.clear();
	// We want to keep known objects around, 'cause these can exist even when not in the simulation (they're here from creation till deletion, regardless of whether they are in sim)
	// m_KnownObjects.clear();
}

Actor* MovableMan::GetNextActorInGroup(std::string group, Actor* pAfterThis) {
	if (LuaMan::IsRunningPreviewHook()) {
		ReportSpeculationViolation("GetNextActorInGroup", pAfterThis);
	}
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
	if (LuaMan::IsRunningPreviewHook()) {
		ReportSpeculationViolation("GetPrevActorInGroup", pBeforeThis);
	}
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
	if (LuaMan::IsRunningPreviewHook()) {
		ReportSpeculationViolation("GetNextTeamActor", pAfterThis);
	}
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
	if (LuaMan::IsRunningPreviewHook()) {
		ReportSpeculationViolation("GetPrevTeamActor", pBeforeThis);
	}
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

	return static_cast<Actor*>(ViewIfSpeculating(pClosestActor));
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

	return static_cast<Actor*>(ViewIfSpeculating(pClosestActor));
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

	return static_cast<Actor*>(ViewIfSpeculating(pClosestActor));
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

	return static_cast<Actor*>(ViewIfSpeculating(pClosestBrain));
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
	return static_cast<Actor*>(ViewIfSpeculating(pClosestBrain));
}

bool MovableMan::IsPlayerBrain(const Actor* actor) const {
	return actor && m_PlayerBrainIDs.contains(actor->GetUniqueID());
}

bool MovableMan::HasPlayerBrainOfTeam(int team) {
	for (long uid: m_PlayerBrainIDs) {
		const Actor* actor = dynamic_cast<const Actor*>(FindObjectByUniqueID(uid));
		if (actor && actor->GetTeam() == team) {
			return true;
		}
	}
	return false;
}

void MovableMan::NotePlayerBrain(long uniqueID, bool isBrain) {
	if (uniqueID <= 0) {
		return;
	}
	if (isBrain) {
		m_PlayerBrainIDs.insert(uniqueID);
	} else {
		m_PlayerBrainIDs.erase(uniqueID);
	}
}

Actor* MovableMan::GetUnassignedBrainByID(int team) const {
	if (team < Activity::TeamOne || team >= Activity::MaxTeamCount) return nullptr;
	Actor* brain = nullptr;
	const auto consider = [&](Actor* candidate) {
		if (candidate->GetTeam() == team && !candidate->IsDead() && candidate->HasObjectInGroup("Brains") &&
			!g_ActivityMan.GetActivity()->IsAssignedBrain(candidate) && (!brain || candidate->GetUniqueID() < brain->GetUniqueID())) brain = candidate;
	};
	for (Actor* actor: m_ActorRoster[team]) consider(actor);
	for (Actor* actor: m_AddedActors) consider(actor);
	return static_cast<Actor*>(ViewIfSpeculating(brain));
}

Actor* MovableMan::GetUnassignedBrain(int team) const {
	if (/*m_Actors.empty() || */ m_ActorRoster[team].empty())
		return 0;

	for (std::list<Actor*>::const_iterator aIt = m_ActorRoster[team].begin(); aIt != m_ActorRoster[team].end(); ++aIt) {
		if ((*aIt)->HasObjectInGroup("Brains") && !g_ActivityMan.GetActivity()->IsAssignedBrain(*aIt))
			return static_cast<Actor*>(ViewIfSpeculating(*aIt));
	}

	// Also need to look through all the actors added this frame, one might be a brain.
	int actorTeam = Activity::NoTeam;
	for (std::deque<Actor*>::const_iterator aaIt = m_AddedActors.begin(); aaIt != m_AddedActors.end(); ++aaIt) {
		int actorTeam = (*aaIt)->GetTeam();
		// Accept no-team brains too - ACTUALLY, DON'T
		if ((actorTeam == team /* || actorTeam == Activity::NoTeam*/) && (*aaIt)->HasObjectInGroup("Brains") && !g_ActivityMan.GetActivity()->IsAssignedBrain(*aaIt))
			return static_cast<Actor*>(ViewIfSpeculating(*aaIt));
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
			RecordSpeculativeSpawnMeta(actorToAdd);
			m_AddedActors.push_back(actorToAdd);
			m_ValidActors.insert(actorToAdd);

			// A joiner's per-machine controller must not drive sim effects on its join tick; the
			// wire takes over from the next tick's controller update.
			if (!m_RestoringSnapshot && ScenarioRunner::IsLockstepControllerSyncActive()) {
				actorToAdd->GetController()->SetDisabled(true);
				// A preview's spawn is discarded with the preview, so only the canonical add holds a seat in the quarantine.
				if (!m_Speculation.active) m_LockstepJoinQuarantine.emplace_back(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()), actorToAdd->GetUniqueID());
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
		RecordSpeculativeSpawnMeta(itemToAdd);
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
		RecordSpeculativeSpawnMeta(particleToAdd);
		if (!m_Speculation.active && !m_RestoringSnapshot) {
			TakePreviewSpawn(particleToAdd);
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
				m_ContiguousActorIDs.erase(*itr);
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
					m_ContiguousActorIDs.erase(*itr);
					m_AddedActors.erase(itr);
					break;
				}
			}
		}
		RemoveActorFromTeamRoster(dynamic_cast<Actor*>(pActorToRem));
		pActorToRem->SetAsAddedToMovableMan(false);
		if (Actor* actor = dynamic_cast<Actor*>(pActorToRem)) {
			actor->GetController()->DropLocalProduction();
		}
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
	if (!m_RenderSubstitutes.empty() && m_RenderSubstitutes.count(pMOToCheck) > 0) {
		return true;
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
	if (!m_RenderSubstitutes.empty() && m_RenderSubstitutes.count(pMOToCheck) > 0) {
		return true;
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

void MovableMan::RebuildContiguousActorIDs() {
	m_ContiguousActorIDs.clear();
	int actorID = 0;
	for (const Actor* actor: m_Actors) {
		m_ContiguousActorIDs[actor] = actorID++;
	}
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
		m_ContiguousActorIDs.clear();

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

bool MovableMan::TeamHasDoorMaterialInBox(int team, const Box& box) const {
	const float sceneWidth = static_cast<float>(g_SceneMan.GetSceneWidth());
	const float sceneHeight = static_cast<float>(g_SceneMan.GetSceneHeight());
	std::array<Vector, 9> shifts{Vector()};
	int shiftCount = 1;
	const bool wrapsX = g_SceneMan.SceneWrapsX() && sceneWidth > 0.0F;
	const bool wrapsY = g_SceneMan.SceneWrapsY() && sceneHeight > 0.0F;
	if (wrapsX) {
		shifts[shiftCount++] = Vector(sceneWidth, 0.0F);
		shifts[shiftCount++] = Vector(-sceneWidth, 0.0F);
	}
	if (wrapsY) {
		shifts[shiftCount++] = Vector(0.0F, sceneHeight);
		shifts[shiftCount++] = Vector(0.0F, -sceneHeight);
	}
	if (wrapsX && wrapsY) {
		// A corner door meets the box only after both seams are crossed.
		shifts[shiftCount++] = Vector(sceneWidth, sceneHeight);
		shifts[shiftCount++] = Vector(sceneWidth, -sceneHeight);
		shifts[shiftCount++] = Vector(-sceneWidth, sceneHeight);
		shifts[shiftCount++] = Vector(-sceneWidth, -sceneHeight);
	}
	for (const std::deque<Actor*>* actorDeque: {&m_Actors, &m_AddedActors}) {
		for (const Actor* actor: *actorDeque) {
			const ADoor* actorAsDoor = dynamic_cast<const ADoor*>(actor);
			// An override only moves pixels for a door whose material is currently drawn.
			if (!actorAsDoor || !actorAsDoor->GetDoorMaterialDrawn() || !actorAsDoor->GetDoor()) {
				continue;
			}
			if (team != Activity::NoTeam && actorAsDoor->GetTeam() != team) {
				continue;
			}
			const Box doorBox = actorAsDoor->GetDoor()->GetBoundingBox();
			for (int shift = 0; shift < shiftCount; ++shift) {
				if (doorBox.IntersectsBox(Box(box.GetCorner() + shifts[shift], box.GetWidth(), box.GetHeight()))) {
					return true;
				}
			}
		}
	}
	return false;
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
			m_ContiguousActorIDs.erase(addedActor);
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

static void ForgetActivitySlots(MovableObject* object) {
	Activity* activity = g_ActivityMan.GetActivity();
	if (!activity) return;
	if (const Actor* actor = dynamic_cast<Actor*>(object)) activity->ForgetDestroyedActor(actor);
}

void MovableMan::RunThreadedSyncedUpdatePass(bool globalMoidOrder) {
	const std::string syncedUpdate = "SyncedUpdate"; // avoid string reconstruction
	m_SyncedPassSkippedDeadEntries = 0;
	// DeleteEntity frees an object where the script stands (LuaAdapters.cpp), so a SyncedUpdate can
	// destroy an object this pass read before it started - its own included. The live registration set
	// answers for the whole pass: a destructor unregisters, and a new registration waits in the pending
	// set until the next LuaMan::Update, so nothing joins the set in between. The removal count tells
	// the ordinary tick, where nothing died, from the one that has to look each object up.
	const uint64_t unregistrationsAtSnapshot = LuaStateWrapper::RegisteredMOUnregistrationCount();
	const auto stillRegistered = [unregistrationsAtSnapshot](LuaStateWrapper& state, MovableObject* mo) {
		return LuaStateWrapper::RegisteredMOUnregistrationCount() == unregistrationsAtSnapshot || state.IsRegisteredMO(mo);
	};
	if (!globalMoidOrder) {
		for (LuaStateWrapper& luaState: g_LuaMan.GetThreadedScriptStates()) {
			g_LuaMan.SetThreadLuaStateOverride(&luaState);
			for (MovableObject* mo: SortedRegisteredMOs(luaState)) {
				if (!stillRegistered(luaState, mo)) {
					++m_SyncedPassSkippedDeadEntries;
					continue;
				}
				// The request flag alone, as the threaded loop has always had it: the master pass owns
				// the ValidMO rule, and a threaded object that asked for a SyncedUpdate still gets one.
				if (mo->HasRequestedSyncedUpdate()) {
					mo->RunScriptedFunctionInAppropriateScripts(syncedUpdate, false, false, {}, {}, {});
					if (!stillRegistered(luaState, mo)) {
						++m_SyncedPassSkippedDeadEntries;
						continue;
					}
					mo->ResetRequestedSyncedUpdateFlag();
				}
			}
		}
		g_LuaMan.SetThreadLuaStateOverride(nullptr);
		return;
	}

	struct Cursor {
		LuaStateWrapper* state = nullptr;
		std::vector<SyncedUpdateEntry> objects;
		size_t next = 0;
	};
	struct Pending {
		SyncedUpdateEntry entry;
		size_t cursor = 0;
	};
	// Orders on the identity stored at snapshot time, never on the object: an earlier script in this
	// same pass may have deleted it.
	const auto earlier = [](const Pending& lhs, const Pending& rhs) {
		return SyncedUpdateEntryEarlier(rhs.entry, lhs.entry);
	};

	std::vector<Cursor> cursors;
	cursors.reserve(g_LuaMan.GetThreadedScriptStates().size());
	std::priority_queue<Pending, std::vector<Pending>, decltype(earlier)> pending(earlier);
	for (LuaStateWrapper& luaState: g_LuaMan.GetThreadedScriptStates()) {
		Cursor& cursor = cursors.emplace_back();
		cursor.state = &luaState;
		cursor.objects = SnapshotRegisteredMOs(luaState);
		if (!cursor.objects.empty()) pending.push({cursor.objects.front(), cursors.size() - 1});
	}

	LuaStateWrapper* currentState = nullptr;
	while (!pending.empty()) {
		const Pending next = pending.top();
		pending.pop();
		Cursor& cursor = cursors[next.cursor];
		if (currentState != cursor.state) {
			g_LuaMan.SetThreadLuaStateOverride(cursor.state);
			currentState = cursor.state;
		}
		MovableObject* mo = next.entry.object;
		// Ordered on the identity read at snapshot time and reached only while the object is still
		// registered: an earlier script in this same pass may have freed it, on any state.
		if (!stillRegistered(*cursor.state, mo)) {
			++m_SyncedPassSkippedDeadEntries;
		} else if (mo->HasRequestedSyncedUpdate()) {
			mo->RunScriptedFunctionInAppropriateScripts(syncedUpdate, false, false, {}, {}, {});
			if (stillRegistered(*cursor.state, mo)) {
				mo->ResetRequestedSyncedUpdateFlag();
			} else {
				++m_SyncedPassSkippedDeadEntries;
			}
		}
		++cursor.next;
		if (cursor.next < cursor.objects.size()) pending.push({cursor.objects[cursor.next], next.cursor});
	}
	g_LuaMan.SetThreadLuaStateOverride(nullptr);
}

bool MovableMan::RunLuaStateAssignmentSelfTest() {
	constexpr int c_ObjectCount = 256;
	// What a machine spends before a match: a single-player scene's objects taking their script states.
	constexpr int c_PreMatchObjects = 7;
	// And what it spends DURING it: one object of its own between two of the match's. A cursor shifted
	// before the match only rotates the states; a cursor shifted inside it regroups the objects, which
	// is the difference a per-state global carries into the world.
	constexpr int c_InterleavedAt = 100;
	constexpr std::string_view c_Fixture = "Tests.rte/Activities/ThreadedSyncedOrderSelfTest.lua";
	const std::string fixturePath = g_PresetMan.GetFullModulePath(std::string(c_Fixture));
	LuaStatesArray& states = g_LuaMan.GetThreadedScriptStates();
	if (states.empty()) {
		std::cout << "[script-graph-selftest] FAIL lua_state_assignment_follows_the_unique_id no threaded Lua states" << std::endl;
		return false;
	}
	if (GetMOIDCount() != 0 || !m_ValidActors.empty() || !m_ValidItems.empty() || !m_ValidParticles.empty()) {
		std::cout << "[script-graph-selftest] FAIL lua_state_assignment_follows_the_unique_id live movable objects prevent the temporary state-set test" << std::endl;
		return false;
	}

	const long savedCounter = MovableObject::GetUniqueIDCounter();
	LuaStatesArray savedStates;
	savedStates.swap(states);
	std::array<std::string, 2> sharedCounterHashes{};
	std::array<std::vector<int>, 2> assignments{};
	std::array<long, 2> firstObjectID{};
	bool byTheUniqueID = true;
	bool ready = true;

	// Every object's own field after the pass: the per-state global its state holds, which is the channel
	// Data/Modding/threaded-determinism.md blesses and the one a shared assignment decides the value of.
	const auto hashSharedCounters = [](const std::vector<std::unique_ptr<MOPixel>>& objects) {
		uint64_t hash = 0xcbf29ce484222325ULL;
		const auto mix = [&hash](uint64_t value) {
			for (size_t byte = 0; byte < sizeof(value); ++byte) {
				hash ^= static_cast<uint8_t>(value >> (byte * 8));
				hash *= 0x100000001b3ULL;
			}
		};
		for (const auto& object: objects) {
			mix(static_cast<uint64_t>(object->GetUniqueID()));
			mix(static_cast<uint64_t>(object->GetNumberValue("threaded_synced_shared_counter")));
		}
		std::ostringstream text;
		text << std::hex << std::setw(16) << std::setfill('0') << hash;
		return text.str();
	};

	for (size_t arm = 0; arm < 2 && ready; ++arm) {
		LuaStatesArray replacement(static_cast<size_t>(c_LuaStateCount));
		states.swap(replacement);
		for (LuaStateWrapper& state: states) {
			state.Initialize();
			InstallThreadedSyncedUpdateSelfTestCallbacks(state);
		}
		ThreadedSyncedUpdateSelfTestContext context;
		s_ThreadedSyncedUpdateSelfTestContext = &context;

		// One object of this machine's own: made, scripted and dropped, leaving only what it spent.
		const auto spendOneAssignment = [this, &fixturePath, &ready](long idBase) {
			MovableObject::PinUniqueIDCounter(idBase);
			auto object = std::make_unique<MOPixel>();
			if (object->Create() < 0 || object->LoadScript(fixturePath, true) < 0) {
				ready = false;
				return;
			}
			object->DestroyScriptState();
			object->Destroy();
		};

		// The second arm played first: its objects took states before the match's own objects did.
		if (arm == 1) {
			for (int index = 0; index < c_PreMatchObjects && ready; ++index) {
				spendOneAssignment(savedCounter + c_ObjectCount + 4096 + index);
			}
		}

		// The match's objects, with the same unique IDs in both arms and no forced placement: what state
		// each one lands on is the assignment's answer and nothing else.
		std::vector<std::unique_ptr<MOPixel>> objects;
		objects.reserve(c_ObjectCount);
		MovableObject::PinUniqueIDCounter(savedCounter);
		for (int index = 0; index < c_ObjectCount && ready; ++index) {
			auto object = std::make_unique<MOPixel>();
			if (object->Create() < 0) {
				ready = false;
				break;
			}
			m_ValidParticles.insert(object.get());
			m_AddedParticles.push_back(object.get());
			objects.push_back(std::move(object));
			MOPixel* fixtureObject = objects.back().get();
			const int loadStatus = fixtureObject->LoadScript(fixturePath, true);
			const int adoptStatus = loadStatus < 0 ? -1 : fixtureObject->AdoptScriptObject();
			if (loadStatus < 0 || adoptStatus < 0) {
				std::cout << "[script-graph-selftest] lua_state_assignment_fixture_refused index=" << index
				          << " load=" << loadStatus << " adopt=" << adoptStatus << std::endl;
				ready = false;
			}
			if (arm == 1 && index == c_InterleavedAt) {
				const long next = MovableObject::GetUniqueIDCounter();
				spendOneAssignment(savedCounter + c_ObjectCount + 8192);
				MovableObject::PinUniqueIDCounter(next);
			}
		}
		for (LuaStateWrapper& state: states) state.Update();

		if (ready && objects.size() == c_ObjectCount) {
			assignments[arm].reserve(objects.size());
			for (const auto& object: objects) {
				const int stateIndex = g_LuaMan.GetStateIndex(object->GetLuaState());
				assignments[arm].push_back(stateIndex);
				// The save index is one-based over the threaded states, so the ID's index is one less.
				byTheUniqueID = byTheUniqueID && stateIndex - 1 == static_cast<int>(g_LuaMan.ScriptStateIndexForObject(object->GetUniqueID()));
			}
			firstObjectID[arm] = objects.front()->GetUniqueID();
			context.order.clear();
			for (const auto& object: objects) object->RequestSyncedUpdate();
			RunThreadedSyncedUpdatePass(true);
			sharedCounterHashes[arm] = hashSharedCounters(objects);
		} else {
			ready = false;
		}

		s_ThreadedSyncedUpdateSelfTestContext = nullptr;
		for (const auto& object: objects) {
			m_ValidParticles.erase(object.get());
			std::erase(m_AddedParticles, object.get());
			object->DestroyScriptState();
			object->Destroy();
		}
		states.swap(replacement);
	}

	states.swap(savedStates);
	MovableObject::PinUniqueIDCounter(savedCounter);
	const bool sameAssignment = ready && !assignments[0].empty() && assignments[0] == assignments[1];
	const bool sameSharedCounters = ready && !sharedCounterHashes[0].empty() && sharedCounterHashes[0] == sharedCounterHashes[1];
	const bool passed = sameAssignment && sameSharedCounters && byTheUniqueID && firstObjectID[0] == firstObjectID[1];
	size_t firstDifference = assignments[0].size();
	for (size_t index = 0; index < assignments[0].size() && index < assignments[1].size(); ++index) {
		if (assignments[0][index] != assignments[1][index]) {
			firstDifference = index;
			break;
		}
	}
	std::cout << "[script-graph-selftest] " << (passed ? "PASS" : "FAIL")
	          << " lua_state_assignment_follows_the_unique_id states=" << c_LuaStateCount
	          << " pre_match_objects=0," << c_PreMatchObjects << " objects=" << c_ObjectCount
	          << " shared_counters=" << sharedCounterHashes[0] << "," << sharedCounterHashes[1]
	          << " same_assignment=" << (sameAssignment ? 1 : 0) << " by_unique_id=" << (byTheUniqueID ? 1 : 0)
	          << " first_uid=" << firstObjectID[0] << "," << firstObjectID[1]
	          << " first_state=" << (assignments[0].empty() ? -1 : assignments[0].front()) << "," << (assignments[1].empty() ? -1 : assignments[1].front())
	          << (sameAssignment ? "" : " first_difference_at=" + std::to_string(firstDifference))
	          << (passed ? "" : " (a machine that ran objects before the match put the same objects on other states)") << std::endl;
	return passed;
}

bool MovableMan::RunLuaStateRestoreBoundarySelfTest() {
	constexpr std::string_view c_Fixture = "Tests.rte/Activities/ThreadedSyncedOrderSelfTest.lua";
	const std::string fixturePath = g_PresetMan.GetFullModulePath(std::string(c_Fixture));
	LuaStatesArray& states = g_LuaMan.GetThreadedScriptStates();
	if (states.empty()) {
		std::cout << "[script-graph-selftest] FAIL restore_keeps_the_object_with_its_saved_script_graph no threaded Lua states" << std::endl;
		return false;
	}
	if (GetMOIDCount() != 0 || !m_ValidActors.empty() || !m_ValidItems.empty() || !m_ValidParticles.empty()) {
		std::cout << "[script-graph-selftest] FAIL restore_keeps_the_object_with_its_saved_script_graph live movable objects prevent the temporary state-set test" << std::endl;
		return false;
	}
	const long savedCounter = MovableObject::GetUniqueIDCounter();
	LuaStatesArray savedStates;
	savedStates.swap(states);
	LuaStatesArray replacement(static_cast<size_t>(c_LuaStateCount));
	states.swap(replacement);
	for (LuaStateWrapper& state: states) state.Initialize();

	int serialSpawnState = -1;
	int serialSpawnOwnState = -1;
	int parallelSpawnState = -1;
	int parallelSpawnSpawnerState = -1;
	uint64_t spawnerPlacements = 0;
	int restoredState = -1;
	int savedIndex = -1;
	int idIndex = -1;
	double restoredField = -1.0;
	bool ready = true;

	{
		// A spawn from a SERIAL pass - the channel the contract blesses - takes its OWN state, though the
		// pass is running inside another one. A spawn from a parallel per-state task cannot: every other
		// state belongs to another thread for the length of that task, so it takes its spawner's.
		const size_t spawnerIndex = 3;
		MovableObject::PinUniqueIDCounter(savedCounter + 64);
		auto serialObject = std::make_unique<MOPixel>();
		g_LuaMan.SetThreadLuaStateOverride(&states[spawnerIndex]);
		ready = serialObject->Create() >= 0 && serialObject->LoadScript(fixturePath, true) == 0;
		g_LuaMan.SetThreadLuaStateOverride(nullptr);
		if (ready) {
			serialSpawnState = g_LuaMan.GetStateIndex(serialObject->GetLuaState());
			serialSpawnOwnState = static_cast<int>(g_LuaMan.ScriptStateIndexForObject(serialObject->GetUniqueID())) + 1;
		}
		serialObject->DestroyScriptState();
		serialObject->Destroy();

		const uint64_t spawnerPlacementsBefore = LuaMan::ScriptStatesTakenFromASpawner();
		auto parallelObject = std::make_unique<MOPixel>();
		g_LuaMan.SetThreadLuaStateOverride(&states[spawnerIndex], true);
		ready = ready && parallelObject->Create() >= 0 && parallelObject->LoadScript(fixturePath, true) == 0;
		g_LuaMan.SetThreadLuaStateOverride(nullptr);
		if (ready) {
			parallelSpawnState = g_LuaMan.GetStateIndex(parallelObject->GetLuaState());
			parallelSpawnSpawnerState = static_cast<int>(spawnerIndex) + 1;
			spawnerPlacements = LuaMan::ScriptStatesTakenFromASpawner() - spawnerPlacementsBefore;
		}
		parallelObject->DestroyScriptState();
		parallelObject->Destroy();
	}

	{
		// The restore boundary. The image says which state held this object and its own _ScriptedObjects
		// table, and the graph is restored into that state by index: the object has to land there too, or
		// the mod's fields are orphaned and the peer groups its per-state globals differently from the
		// live ones. The image here names a state the object's ID does not, which is what an object
		// spawned inside a hook looks like in any save.
		MovableObject::PinUniqueIDCounter(savedCounter + 128);
		auto restored = std::make_unique<MOPixel>();
		if (restored->Create() < 0 || restored->LoadScript(fixturePath, true) != 0) {
			ready = false;
		} else {
			const long uniqueID = restored->GetUniqueID();
			idIndex = static_cast<int>(g_LuaMan.ScriptStateIndexForObject(uniqueID)) + 1;
			savedIndex = idIndex == static_cast<int>(states.size()) ? 1 : idIndex + 1;
			// What RestoreScriptGraphs puts back into the state the image named, keyed on the object.
			g_LuaMan.GetStateByIndex(savedIndex).RunScriptString(
			    "_ScriptedObjects = _ScriptedObjects or {}; _ScriptedObjects[\"" + std::to_string(uniqueID) + "\"] = { charge = 7 }");
			restored->StageRestoredIdentity(uniqueID, savedIndex);
			restored->AdoptPersistedUniqueID();
			restoredState = g_LuaMan.GetStateIndex(restored->GetLuaState());
			if (LuaStateWrapper* state = restored->GetLuaState()) {
				state->RunScriptString("_RestoreBoundaryCharge = _ScriptedObjects and _ScriptedObjects[\"" + std::to_string(uniqueID) + "\"] and _ScriptedObjects[\"" + std::to_string(uniqueID) + "\"].charge or -1");
				std::lock_guard<std::recursive_mutex> lock(state->GetMutex());
				lua_State* luaState = state->GetLuaState();
				lua_getglobal(luaState, "_RestoreBoundaryCharge");
				restoredField = lua_tonumber(luaState, -1);
				lua_pop(luaState, 1);
				state->RunScriptString("_RestoreBoundaryCharge = nil");
			}
		}
		restored->DestroyScriptState();
		restored->Destroy();
	}

	states.swap(replacement);
	states.swap(savedStates);
	MovableObject::PinUniqueIDCounter(savedCounter);

	const bool serialGreen = ready && serialSpawnState > 0 && serialSpawnState == serialSpawnOwnState;
	const bool parallelKnown = ready && parallelSpawnState > 0 && parallelSpawnState == parallelSpawnSpawnerState && spawnerPlacements == 1;
	const bool restoreGreen = ready && restoredState > 0 && restoredState == savedIndex && restoredField == 7.0;
	const bool passed = serialGreen && parallelKnown && restoreGreen;
	std::cout << "[script-graph-selftest] " << (passed ? "PASS" : "FAIL")
	          << " restore_keeps_the_object_with_its_saved_script_graph states=" << c_LuaStateCount
	          << " serial_spawn_state=" << serialSpawnState << " serial_spawn_own_state=" << serialSpawnOwnState
	          << " parallel_spawn_state=" << parallelSpawnState << " parallel_spawner_state=" << parallelSpawnSpawnerState
	          << " spawner_placements=" << spawnerPlacements
	          << " saved_state=" << savedIndex << " id_state=" << idIndex << " landed=" << restoredState
	          << " field=" << restoredField
	          << (serialGreen ? "" : " (a spawn inside a serial pass took its spawner's state)")
	          << (restoreGreen ? "" : " (the restore moved the object away from the state its saved script graph is in)")
	          << std::endl;
	return passed;
}

bool MovableMan::RunLuaStateIdentitySelfTest() {
	constexpr std::string_view c_Fixture = "Tests.rte/Activities/ThreadedSyncedOrderSelfTest.lua";
	const std::string fixturePath = g_PresetMan.GetFullModulePath(std::string(c_Fixture));
	LuaStatesArray& states = g_LuaMan.GetThreadedScriptStates();
	if (states.empty()) {
		std::cout << "[script-graph-selftest] FAIL create_keeps_a_live_scripted_object_running no threaded Lua states" << std::endl;
		return false;
	}
	if (GetMOIDCount() != 0 || !m_ValidActors.empty() || !m_ValidItems.empty() || !m_ValidParticles.empty()) {
		std::cout << "[script-graph-selftest] FAIL create_keeps_a_live_scripted_object_running live movable objects prevent the temporary state-set test" << std::endl;
		return false;
	}
	const long savedCounter = MovableObject::GetUniqueIDCounter();
	LuaStatesArray savedStates;
	savedStates.swap(states);
	LuaStatesArray replacement(static_cast<size_t>(c_LuaStateCount));
	states.swap(replacement);
	for (LuaStateWrapper& state: states) {
		state.Initialize();
		InstallThreadedSyncedUpdateSelfTestCallbacks(state);
	}
	ThreadedSyncedUpdateSelfTestContext context;
	s_ThreadedSyncedUpdateSelfTestContext = &context;

	const auto add = [this, &fixturePath](long counterBefore) {
		MovableObject::PinUniqueIDCounter(counterBefore);
		auto object = std::make_unique<MOPixel>();
		if (object->Create() < 0) {
			return std::unique_ptr<MOPixel>();
		}
		m_ValidParticles.insert(object.get());
		m_AddedParticles.push_back(object.get());
		if (object->LoadScript(fixturePath, true) != 0 || object->AdoptScriptObject() < 0) {
			return std::unique_ptr<MOPixel>();
		}
		return object;
	};
	const auto drop = [this](std::unique_ptr<MOPixel>& object) {
		if (!object) {
			return;
		}
		m_ValidParticles.erase(object.get());
		std::erase(m_AddedParticles, object.get());
		object->DestroyScriptState();
		object.reset();
	};
	const auto promote = [&states] {
		for (LuaStateWrapper& state: states) state.Update();
	};
	const auto ran = [&context](long uniqueID) {
		return static_cast<int>(std::count(context.order.begin(), context.order.end(), uniqueID));
	};
	// A field of the mod's own table, written and read where a script would see it.
	const auto writeCharge = [](LuaStateWrapper* state, long uniqueID, int value) {
		if (state) state->RunScriptString("_ScriptedObjects[\"" + std::to_string(uniqueID) + "\"].charge = " + std::to_string(value));
	};
	const auto readCharge = [](LuaStateWrapper* state, long uniqueID) {
		double value = -1.0;
		if (!state) return value;
		const std::string key = "_ScriptedObjects[\"" + std::to_string(uniqueID) + "\"]";
		state->RunScriptString("_IdentityRowCharge = " + key + " and " + key + ".charge or -1");
		std::lock_guard<std::recursive_mutex> lock(state->GetMutex());
		lua_State* luaState = state->GetLuaState();
		lua_getglobal(luaState, "_IdentityRowCharge");
		value = lua_tonumber(luaState, -1);
		lua_pop(luaState, 1);
		state->RunScriptString("_IdentityRowCharge = nil");
		return value;
	};

	bool ready = true;

	// A Create on a live scripted object draws it a new identity. Its hooks find self under that
	// identity, so the script object has to come along or the object silently stops running.
	long createBefore = 0;
	long createAfter = 0;
	int createRanBefore = 0;
	int createRanAfter = 0;
	int createRanUnderTheOldID = 0;
	double createCharge = -1.0;
	{
		auto object = add(savedCounter + 256);
		ready = object != nullptr;
		if (ready) {
			promote();
			createBefore = object->GetUniqueID();
			writeCharge(object->GetLuaState(), createBefore, 7);
			context.order.clear();
			object->RequestSyncedUpdate();
			RunThreadedSyncedUpdatePass(true);
			createRanBefore = ran(createBefore);
			ready = object->Create() >= 0;
			createAfter = object->GetUniqueID();
			promote();
			context.order.clear();
			object->RequestSyncedUpdate();
			RunThreadedSyncedUpdatePass(true);
			createRanAfter = ran(createAfter);
			createRanUnderTheOldID = ran(createBefore);
			createCharge = readCharge(object->GetLuaState(), createAfter);
		}
		drop(object);
	}

	// A restore adopts the identity its image carries. A live object already holding it drew that ID
	// from the counter, so the holder is the one that moves - and two live objects never share an ID.
	long holderBefore = 0;
	long holderAfter = 0;
	long adoptedID = 0;
	int liveHoldersOfTheAdoptedID = 0;
	int adoptedIDNamesTheRestored = 0;
	double holderCharge = -1.0;
	{
		auto holder = add(savedCounter + 512);
		auto restored = add(savedCounter + 600);
		ready = ready && holder != nullptr && restored != nullptr;
		if (ready) {
			promote();
			holderBefore = holder->GetUniqueID();
			writeCharge(holder->GetLuaState(), holderBefore, 9);
			restored->StageRestoredIdentity(holderBefore, -1);
			restored->AdoptPersistedUniqueID();
			adoptedID = restored->GetUniqueID();
			holderAfter = holder->GetUniqueID();
			liveHoldersOfTheAdoptedID = 1 + (holderAfter == adoptedID ? 1 : 0);
			adoptedIDNamesTheRestored = FindObjectByUniqueID(adoptedID) == restored.get() ? 1 : 0;
			holderCharge = readCharge(holder->GetLuaState(), holderAfter);
		}
		drop(restored);
		drop(holder);
	}

	// Two live registered objects can still share one unique ID and no MOID - a private copy takes the
	// identity it shadows and registers with nobody - so the walk's key has to tell them apart itself.
	long tieSharedID = 0;
	long tieSerials[2] = {0, 0};
	int tieKeyIsTotal = 0;
	int tieEarlierRegistrationIsFirst = 0;
	{
		// One state holds both: the assignment is the unique ID modulo the count, so IDs a count apart
		// land together.
		const long firstID = savedCounter + 1024;
		auto first = add(firstID - 1);
		auto twin = add(firstID + c_LuaStateCount - 1);
		ready = ready && first != nullptr && twin != nullptr;
		if (ready) {
			promote();
			tieSharedID = first->GetUniqueID();
			{
				// A private copy is not in the canonical map, so the staged one leaves it first.
				MovableObject::FaithfulCloneScope faithful(false);
				UnregisterObject(twin.get());
				twin->StageRestoredIdentity(tieSharedID, -1);
				twin->AdoptPersistedUniqueID();
			}
			tieSerials[0] = first->GetScriptRegistrationSerial();
			tieSerials[1] = twin->GetScriptRegistrationSerial();
			const SyncedUpdateEntry earlierEntry{first.get(), tieSharedID, first->GetID(), tieSerials[0]};
			const SyncedUpdateEntry laterEntry{twin.get(), twin->GetUniqueID(), twin->GetID(), tieSerials[1]};
			tieKeyIsTotal = SyncedUpdateEntryEarlier(earlierEntry, laterEntry) && !SyncedUpdateEntryEarlier(laterEntry, earlierEntry) ? 1 : 0;
			LuaStateWrapper* sharedState = first->GetLuaState();
			if (sharedState == twin->GetLuaState()) {
				const std::vector<SyncedUpdateEntry> snapshot = SnapshotRegisteredMOs(*sharedState);
				for (const SyncedUpdateEntry& entry: snapshot) {
					if (entry.object == first.get() || entry.object == twin.get()) {
						tieEarlierRegistrationIsFirst = entry.object == first.get() ? 1 : 0;
						break;
					}
				}
			}
		}
		drop(twin);
		drop(first);
	}

	// ENGINE 335: the pass reads the request flag through the entry's pointer. A script in the same
	// pass frees another object synchronously (LuaAdapters.cpp DeleteEntity), so the entry a later pop
	// reaches - and the entry an earlier pop already read - must both be safe.
	long laterVictimID = 0;
	long earlierVictimID = 0;
	int laterVictimSkipped = 0;
	int earlierVictimRan = 0;
	int passesCompleted = 0;
	int poisonOverAFreedEntry[2] = {0, 0};
	{
		auto runner = add(savedCounter + 2048);
		auto laterVictim = add(savedCounter + 2176);
		ready = ready && runner != nullptr && laterVictim != nullptr;
		if (ready) {
			promote();
			laterVictimID = laterVictim->GetUniqueID();
			// The runner's ID is the lower one, so the merge reaches the victim after the script freed it.
			void* poison = nullptr;
			void* victimAddress = laterVictim.get();
			context.order.clear();
			runner->RequestSyncedUpdate();
			context.retire = [this, &laterVictim, &poison] {
				m_ValidParticles.erase(laterVictim.get());
				std::erase(m_AddedParticles, laterVictim.get());
				laterVictim.reset();
				// A block of the same size over the freed one, in a value no flag survives, so a stale
				// read of the request flag faults instead of looking alive.
				poison = ::operator new(sizeof(MOPixel));
				std::memset(poison, 0xDD, sizeof(MOPixel));
			};
			RunThreadedSyncedUpdatePass(true);
			context.retire = nullptr;
			laterVictimSkipped = static_cast<int>(GetSyncedPassSkippedDeadEntries());
			poisonOverAFreedEntry[0] = poison == victimAddress ? 1 : 0;
			++passesCompleted;
			if (poison) ::operator delete(poison);

			// And the other way: the entry the merge popped and read before a later script freed it.
			auto earlierVictim = add(savedCounter + 1920);
			ready = earlierVictim != nullptr;
			if (ready) {
				promote();
				earlierVictimID = earlierVictim->GetUniqueID();
				poison = nullptr;
				victimAddress = earlierVictim.get();
				context.order.clear();
				runner->RequestSyncedUpdate();
				context.retire = [this, &earlierVictim, &poison] {
					m_ValidParticles.erase(earlierVictim.get());
					std::erase(m_AddedParticles, earlierVictim.get());
					earlierVictim.reset();
					poison = ::operator new(sizeof(MOPixel));
					std::memset(poison, 0xDD, sizeof(MOPixel));
				};
				RunThreadedSyncedUpdatePass(true);
				context.retire = nullptr;
				earlierVictimRan = ran(earlierVictimID);
				poisonOverAFreedEntry[1] = poison == victimAddress ? 1 : 0;
				++passesCompleted;
				if (poison) ::operator delete(poison);
			}
			drop(earlierVictim);
		}
		drop(laterVictim);
		drop(runner);
	}

	// An image that carries two scripted copies of ONE identity - what a private overlay restores -
	// read on two peers whose residents came up in opposite order. The pair stays live on one identity
	// by design, and the walk orders it on the registration serial, which is a per-process fact: the
	// row records both arms' answers, so the cross-peer half of that is visible in its own numbers.
	long tieRestoreIDs[2][2] = {{0, 0}, {0, 0}};
	long tieRestoreSerials[2][2] = {{0, 0}, {0, 0}};
	int tieRestoreFirstIsEarlier[2] = {-1, -1};
	int tieRestoreDuplicate[2] = {1, 1};
	// What the image recorded for the pair: the writer's own registration order.
	const long imageSerials[2] = {savedCounter + 7000, savedCounter + 7001};
	const auto addFromImage = [this, &fixturePath](long counterBefore, long serial) {
		MovableObject::PinUniqueIDCounter(counterBefore);
		auto object = std::make_unique<MOPixel>();
		if (object->Create() < 0) {
			return std::unique_ptr<MOPixel>();
		}
		m_ValidParticles.insert(object.get());
		m_AddedParticles.push_back(object.get());
		object->StageRestoredScriptRegistration(serial);
		if (object->LoadScript(fixturePath, true) != 0 || object->AdoptScriptObject() < 0) {
			return std::unique_ptr<MOPixel>();
		}
		return object;
	};
	for (size_t arm = 0; arm < 2 && ready; ++arm) {
		const long firstOwnID = savedCounter + 3072;
		const long secondOwnID = firstOwnID + c_LuaStateCount;
		const long imageID = savedCounter + 3008;
		// The only difference between the arms is which resident registered first.
		std::unique_ptr<MOPixel> first;
		std::unique_ptr<MOPixel> second;
		if (arm == 0) {
			first = addFromImage(firstOwnID - 1, imageSerials[0]);
			second = addFromImage(secondOwnID - 1, imageSerials[1]);
		} else {
			second = addFromImage(secondOwnID - 1, imageSerials[1]);
			first = addFromImage(firstOwnID - 1, imageSerials[0]);
		}
		ready = first != nullptr && second != nullptr;
		if (ready) {
			promote();
			tieRestoreSerials[arm][0] = first->GetScriptRegistrationSerial();
			tieRestoreSerials[arm][1] = second->GetScriptRegistrationSerial();
			// The image is read in its own order on every peer, from the same pinned counter.
			MovableObject::PinUniqueIDCounter(savedCounter + 3500);
			{
				// Both are private copies of one identity: neither is in the canonical map.
				MovableObject::FaithfulCloneScope faithful(false);
				UnregisterObject(first.get());
				UnregisterObject(second.get());
				first->StageRestoredIdentity(imageID, -1);
				first->AdoptPersistedUniqueID();
				second->StageRestoredIdentity(imageID, -1);
				second->AdoptPersistedUniqueID();
			}
			tieRestoreIDs[arm][0] = first->GetUniqueID();
			tieRestoreIDs[arm][1] = second->GetUniqueID();
			tieRestoreDuplicate[arm] = tieRestoreIDs[arm][0] == tieRestoreIDs[arm][1] ? 1 : 0;
			const SyncedUpdateEntry firstEntry{first.get(), first->GetUniqueID(), first->GetID(), tieRestoreSerials[arm][0]};
			const SyncedUpdateEntry secondEntry{second.get(), second->GetUniqueID(), second->GetID(), tieRestoreSerials[arm][1]};
			tieRestoreFirstIsEarlier[arm] = SyncedUpdateEntryEarlier(firstEntry, secondEntry) ? 1 : 0;
		}
		drop(second);
		drop(first);
	}

	// A mod's hook deletes its own object through DeleteEntity, which frees where the script stands
	// (LuaAdapters.cpp). The loop still holds that object's script list and runs its next script, so
	// the object has to outlive the loop and go the moment the hook returns.
	long selfDeleteID = 0;
	int selfDeleteRan = 0;
	int selfDeleteSecondScriptRan = 0;
	int selfDeleteAliveInTheLoop = -1;
	int selfDeleteStillRegistered = 1;
	int selfDeleteStillKnown = 1;
	{
		const std::string deletePath = g_PresetMan.GetFullModulePath("Tests.rte/Activities/SelfDeleteHookSelfTest.lua");
		const std::string witnessPath = g_PresetMan.GetFullModulePath("Tests.rte/Activities/SelfDeleteHookWitnessSelfTest.lua");
		MovableObject::PinUniqueIDCounter(savedCounter + 2560);
		auto object = std::make_unique<MOPixel>();
		bool armed = object->Create() >= 0;
		if (armed) {
			m_ValidParticles.insert(object.get());
			m_AddedParticles.push_back(object.get());
			armed = object->LoadScript(deletePath, true) == 0 && object->LoadScript(witnessPath, true) == 0 &&
			        object->AdoptScriptObject() >= 0;
		}
		if (armed) {
			promote();
			selfDeleteID = object->GetUniqueID();
			LuaStateWrapper* state = object->GetLuaState();
			// The script owns the object from here: DeleteEntity is what frees it.
			MovableObject* raw = object.release();
			context.order.clear();
			context.aliveHere = -1;
			raw->RequestSyncedUpdate();
			RunThreadedSyncedUpdatePass(true);
			selfDeleteRan = ran(selfDeleteID);
			selfDeleteSecondScriptRan = ran(-selfDeleteID);
			selfDeleteAliveInTheLoop = context.aliveHere;
			// Both answers read the address only, never the object.
			selfDeleteStillRegistered = state && state->IsRegisteredMO(raw) ? 1 : 0;
			selfDeleteStillKnown = FindObjectByUniqueID(selfDeleteID) ? 1 : 0;
			m_ValidParticles.erase(raw);
			std::erase(m_AddedParticles, raw);
			if (selfDeleteStillKnown) delete raw;
		} else {
			ready = false;
			drop(object);
		}
	}

	// The serial has to survive the image the way the unique-ID counter does: a restored object keeps
	// the writer's place in the order, and this machine's registrations continue above it.
	long roundTripSerials[2] = {0, 0};
	long roundTripNextSerial = 0;
	int roundTripBlobCarriesSerial = 0;
	int roundTripImageApplied = 0;
	int roundTripScriptsReady = 0;
	{
		const long counterBeforeRoundTrip = MovableObject::GetScriptRegistrationSerialCounter();
		auto source = add(savedCounter + 3968);
		ready = ready && source != nullptr;
		std::string image;
		if (ready) {
			promote();
			roundTripSerials[0] = source->GetScriptRegistrationSerial();
			image = source->SaveRuntimeImage();
			roundTripBlobCarriesSerial = image.find(std::to_string(roundTripSerials[0])) != std::string::npos ? 1 : 0;
		}
		drop(source);
		if (ready) {
			// A machine that has drawn nothing of its own reads the image.
			// A machine that has drawn nothing of its own takes what the image recorded, in the order a
			// restore has it: the serial is in place before the scripts initialize.
			MovableObject::PinScriptRegistrationSerial(0);
			MovableObject::PinUniqueIDCounter(savedCounter + 4032);
			auto restored = std::make_unique<MOPixel>();
			ready = restored->Create() >= 0;
			if (ready) {
				m_ValidParticles.insert(restored.get());
				m_AddedParticles.push_back(restored.get());
				restored->StageRestoredScriptRegistration(roundTripSerials[0]);
				roundTripImageApplied = 1;
				roundTripScriptsReady = restored->LoadScript(fixturePath, true) == 0 && restored->AdoptScriptObject() >= 0 ? 1 : 0;
				ready = roundTripScriptsReady == 1;
			}
			if (ready) {
				promote();
				roundTripSerials[1] = restored->GetScriptRegistrationSerial();
				auto next = add(savedCounter + 4096);
				if (next) roundTripNextSerial = next->GetScriptRegistrationSerial();
				drop(next);
			}
			drop(restored);
			MovableObject::PinScriptRegistrationSerial(counterBeforeRoundTrip);
		}
	}

	// The same hook, freed by an engine path that never waited for it: the loop must end there and
	// nothing may read the object afterwards - the scope included.
	long engineDeleteID = 0;
	int engineDeleteRan = 0;
	int engineDeleteSecondScriptRan = 0;
	int engineDeleteStillKnown = 1;
	uint64_t engineDeleteLoopsEnded = 0;
	{
		const std::string deletePath = g_PresetMan.GetFullModulePath("Tests.rte/Activities/EngineDeleteHookSelfTest.lua");
		const std::string witnessPath = g_PresetMan.GetFullModulePath("Tests.rte/Activities/SelfDeleteHookWitnessSelfTest.lua");
		MovableObject::PinUniqueIDCounter(savedCounter + 2816);
		auto object = std::make_unique<MOPixel>();
		bool armed = object->Create() >= 0;
		if (armed) {
			m_ValidParticles.insert(object.get());
			m_AddedParticles.push_back(object.get());
			armed = object->LoadScript(deletePath, true) == 0 && object->LoadScript(witnessPath, true) == 0 &&
			        object->AdoptScriptObject() >= 0;
		}
		if (armed) {
			promote();
			engineDeleteID = object->GetUniqueID();
			const uint64_t loopsEndedBefore = MovableObject::HookLoopsEndedOnADestroyedObject();
			MovableObject* raw = object.release();
			context.order.clear();
			context.aliveHere = -1;
			raw->RequestSyncedUpdate();
			RunThreadedSyncedUpdatePass(true);
			engineDeleteRan = ran(engineDeleteID);
			engineDeleteSecondScriptRan = ran(-engineDeleteID);
			engineDeleteStillKnown = FindObjectByUniqueID(engineDeleteID) ? 1 : 0;
			engineDeleteLoopsEnded = MovableObject::HookLoopsEndedOnADestroyedObject() - loopsEndedBefore;
			m_ValidParticles.erase(raw);
			std::erase(m_AddedParticles, raw);
			if (engineDeleteStillKnown) delete raw;
		} else {
			ready = false;
			drop(object);
		}
	}

	s_ThreadedSyncedUpdateSelfTestContext = nullptr;
	states.swap(replacement);
	states.swap(savedStates);
	MovableObject::PinUniqueIDCounter(savedCounter);

	const bool createGreen = ready && createBefore > 0 && createAfter > 0 && createAfter != createBefore &&
	                         createRanBefore == 1 && createRanAfter == 1 && createRanUnderTheOldID == 0 && createCharge == 7.0;
	const bool adoptGreen = ready && adoptedID == holderBefore && holderAfter != adoptedID && liveHoldersOfTheAdoptedID == 1 &&
	                        adoptedIDNamesTheRestored == 1 && holderCharge == 9.0;
	const bool tieGreen = ready && tieSharedID > 0 && tieSerials[0] > 0 && tieSerials[1] > tieSerials[0] &&
	                      tieKeyIsTotal == 1 && tieEarlierRegistrationIsFirst == 1;
	const bool freedGreen = ready && passesCompleted == 2 && laterVictimSkipped == 1 && earlierVictimRan == 0;
	const bool selfDeleteGreen = ready && selfDeleteID > 0 && selfDeleteRan == 1 && selfDeleteSecondScriptRan == 1 &&
	                             selfDeleteAliveInTheLoop == 1 && selfDeleteStillRegistered == 0 && selfDeleteStillKnown == 0;
	// The image's serials travel with it, so both peers hold the same pair and walk it the same way.
	const bool tieRestoreGreen = ready && tieRestoreDuplicate[0] == 1 && tieRestoreDuplicate[1] == 1 &&
	                             tieRestoreIDs[0][0] == tieRestoreIDs[1][0] && tieRestoreIDs[0][1] == tieRestoreIDs[1][1] &&
	                             tieRestoreSerials[0][0] == tieRestoreSerials[1][0] && tieRestoreSerials[0][1] == tieRestoreSerials[1][1] &&
	                             tieRestoreFirstIsEarlier[0] == tieRestoreFirstIsEarlier[1] && tieRestoreFirstIsEarlier[0] >= 0;
	const bool roundTripGreen = ready && roundTripSerials[0] > 0 && roundTripBlobCarriesSerial == 1 &&
	                            roundTripSerials[1] == roundTripSerials[0] && roundTripNextSerial > roundTripSerials[0];
	const bool engineDeleteGreen = ready && engineDeleteID > 0 && engineDeleteRan == 1 && engineDeleteSecondScriptRan == 0 &&
	                               engineDeleteStillKnown == 0 && engineDeleteLoopsEnded == 1;

	std::cout << "[script-graph-selftest] " << (createGreen ? "PASS" : "FAIL")
	          << " create_keeps_a_live_scripted_object_running states=" << c_LuaStateCount
	          << " id_before=" << createBefore << " id_after=" << createAfter
	          << " ran_before=" << createRanBefore << " ran_after=" << createRanAfter
	          << " ran_under_the_old_id=" << createRanUnderTheOldID << " charge=" << createCharge
	          << (createGreen ? "" : " (Create drew a new identity and the object's scripts stopped running)") << std::endl;
	std::cout << "[script-graph-selftest] " << (adoptGreen ? "PASS" : "FAIL")
	          << " restore_gives_an_adopted_unique_id_to_one_object_only states=" << c_LuaStateCount
	          << " adopted=" << adoptedID << " holder_before=" << holderBefore << " holder_after=" << holderAfter
	          << " live_holders=" << liveHoldersOfTheAdoptedID << " adopted_id_names_the_restored=" << adoptedIDNamesTheRestored
	          << " holder_charge=" << holderCharge
	          << (adoptGreen ? "" : " (the restore left two live objects sharing one unique ID)") << std::endl;
	std::cout << "[script-graph-selftest] " << (tieGreen ? "PASS" : "FAIL")
	          << " synced_pass_orders_a_shared_identity_by_registration states=" << c_LuaStateCount
	          << " shared_id=" << tieSharedID << " serials=" << tieSerials[0] << "," << tieSerials[1]
	          << " key_is_total=" << tieKeyIsTotal << " earlier_registration_first=" << tieEarlierRegistrationIsFirst
	          << (tieGreen ? "" : " (two objects sharing a unique ID and a MOID are ordered by the set, not by the key)") << std::endl;
	std::cout << "[script-graph-selftest] " << (freedGreen ? "PASS" : "FAIL")
	          << " synced_pass_survives_a_script_freeing_either_side states=" << c_LuaStateCount
	          << " passes=" << passesCompleted << " later_victim=" << laterVictimID << " later_skipped=" << laterVictimSkipped
	          << " earlier_victim=" << earlierVictimID << " earlier_ran=" << earlierVictimRan
	          << " poison_over_the_freed_block=" << poisonOverAFreedEntry[0] << "," << poisonOverAFreedEntry[1]
	          << (freedGreen ? "" : " (the pass did not finish after a script freed an entry it holds)") << std::endl;
	std::cout << "[script-graph-selftest] " << (selfDeleteGreen ? "PASS" : "FAIL")
	          << " hook_loop_survives_a_script_deleting_its_own_object states=" << c_LuaStateCount
	          << " uid=" << selfDeleteID << " deleting_script_ran=" << selfDeleteRan
	          << " second_script_ran=" << selfDeleteSecondScriptRan << " alive_in_the_loop=" << selfDeleteAliveInTheLoop
	          << " still_registered_after=" << selfDeleteStillRegistered << " still_known_after=" << selfDeleteStillKnown
	          << (selfDeleteGreen ? "" : " (the loop ran a script of an object the delete had already destroyed)") << std::endl;
	std::cout << "[script-graph-selftest] " << (tieRestoreGreen ? "PASS" : "FAIL")
	          << " a_restored_shadow_pair_walks_in_registration_order states=" << c_LuaStateCount
	          << " ids_first_registered=" << tieRestoreIDs[0][0] << "," << tieRestoreIDs[0][1]
	          << " ids_second_registered=" << tieRestoreIDs[1][0] << "," << tieRestoreIDs[1][1]
	          << " serials=" << tieRestoreSerials[0][0] << "," << tieRestoreSerials[0][1] << ";"
	          << tieRestoreSerials[1][0] << "," << tieRestoreSerials[1][1]
	          << " pair_on_one_identity=" << tieRestoreDuplicate[0] << "," << tieRestoreDuplicate[1]
	          << " first_walks_earlier=" << tieRestoreFirstIsEarlier[0] << "," << tieRestoreFirstIsEarlier[1]
	          << " peers_agree=" << (tieRestoreFirstIsEarlier[0] == tieRestoreFirstIsEarlier[1] ? 1 : 0)
	          << (tieRestoreGreen ? "" : " (a shadowed identity's pair did not walk in the peer's own registration order)") << std::endl;
	std::cout << "[script-graph-selftest] " << (roundTripGreen ? "PASS" : "FAIL")
	          << " a_registration_serial_survives_the_image states=" << c_LuaStateCount
	          << " saved=" << roundTripSerials[0] << " restored=" << roundTripSerials[1]
	          << " image_carries_it=" << roundTripBlobCarriesSerial << " image_applied=" << roundTripImageApplied
	          << " scripts_ready=" << roundTripScriptsReady << " next_after_the_restore=" << roundTripNextSerial
	          << (roundTripGreen ? "" : " (the image lost the registration serial, so a restored peer drew its own)") << std::endl;
	std::cout << "[script-graph-selftest] " << (engineDeleteGreen ? "PASS" : "FAIL")
	          << " hook_loop_survives_an_engine_delete_of_its_object states=" << c_LuaStateCount
	          << " uid=" << engineDeleteID << " deleting_script_ran=" << engineDeleteRan
	          << " second_script_ran=" << engineDeleteSecondScriptRan << " still_known_after=" << engineDeleteStillKnown
	          << " loops_ended_on_a_destroyed_object=" << engineDeleteLoopsEnded
	          << (engineDeleteGreen ? "" : " (the loop ran on after an engine path freed its object, and its scope wrote through it)") << std::endl;

	return createGreen && adoptGreen && tieGreen && freedGreen && selfDeleteGreen && tieRestoreGreen && roundTripGreen && engineDeleteGreen;
}

bool MovableMan::RunThreadedSyncedUpdateOrderSelfTest() {
	constexpr int c_ObjectCount = 1024;
	constexpr int c_MeasureRounds = 32;
	constexpr std::string_view c_Fixture = "Tests.rte/Activities/ThreadedSyncedOrderSelfTest.lua";
	const std::string fixturePath = g_PresetMan.GetFullModulePath(std::string(c_Fixture));
	LuaStatesArray& states = g_LuaMan.GetThreadedScriptStates();
	if (states.empty()) {
		std::cout << "[script-graph-selftest] FAIL threaded_synced_update_global_moid_order no threaded Lua states" << std::endl;
		return false;
	}
	if (GetMOIDCount() != 0 || !m_ValidActors.empty() || !m_ValidItems.empty() || !m_ValidParticles.empty()) {
		std::cout << "[script-graph-selftest] FAIL threaded_synced_update_global_moid_order live movable objects prevent the temporary state-set test" << std::endl;
		return false;
	}

	const long savedCounter = MovableObject::GetUniqueIDCounter();
	LuaStatesArray savedStates;
	savedStates.swap(states);
	std::array<std::string, 2> perStateHashes;
	std::array<std::string, 2> globalHashes;
	long long perStateUs = 0;
	long long globalUs = 0;
	std::array<std::string, 2> duplicateHashes;
	std::array<std::string, 2> freedAcrossStatesHashes{};
	std::array<std::string, 2> freedAcrossStatesExpected{};
	std::array<long, 2> freedAcrossStatesSkipped{};
	std::array<long, 2> freedAcrossStatesRan{};
	std::array<long, 2> freedAcrossStatesPoisonHit{};
	std::array<long, 2> freedAcrossStatesVictimID{};
	bool freedAcrossStatesVictimRan = false;
	bool freedAcrossStatesReady = true;
	std::array<std::array<long, 2>, 2> duplicatePositions{};
	std::array<std::array<long, 2>, 2> duplicateIDs{};
	std::array<std::array<long, 2>, 2> duplicateMOIDs{};
	std::array<long, 2> duplicateOrderLength{};
	long retiredExpectedAt = 0;
	long retiredActualAt = 0;
	size_t retiredExpectedLength = 0;
	size_t retiredActualLength = 0;
	std::string retiredExpectedHash;
	std::string retiredActualHash;
	size_t retiredFirstDivergence = 0;
	bool unlistedRootRan = false;
	std::array<long, 2> globalWriteTotals{};
	std::array<long, 2> globalWriteStatesWritten{};
	std::array<long, 2> spawnedInPass{};
	long long loadPerStateUs = 0;
	long long loadGlobalUs = 0;
	long long loadGlobalAfterADeletionUs = 0;
	int loadObjectsRegistered = 0;
	bool passed = true;

	const auto hashOrder = [](const std::vector<long>& order) {
		uint64_t hash = 0xcbf29ce484222325ULL;
		for (long uniqueID: order) {
			const auto value = static_cast<uint64_t>(uniqueID);
			for (size_t byte = 0; byte < sizeof(value); ++byte) {
				hash ^= static_cast<uint8_t>(value >> (byte * 8));
				hash *= 0x100000001b3ULL;
			}
		}
		std::ostringstream text;
		text << std::hex << std::setw(16) << std::setfill('0') << hash;
		return text.str();
	};

	const auto hashFixture = [](const ThreadedSyncedUpdateSelfTestContext& context, const std::vector<std::unique_ptr<MOPixel>>& objects) {
		uint64_t hash = 0xcbf29ce484222325ULL;
		const auto mix = [&hash](uint64_t value) {
			for (size_t byte = 0; byte < sizeof(value); ++byte) {
				hash ^= static_cast<uint8_t>(value >> (byte * 8));
				hash *= 0x100000001b3ULL;
			}
		};
		for (long uniqueID: context.order) mix(static_cast<uint64_t>(uniqueID));
		for (const auto& object: objects) {
			mix(static_cast<uint64_t>(object->GetUniqueID()));
			mix(static_cast<uint64_t>(object->GetNumberValue("threaded_synced_order_length")));
		}
		// The two permitted writes whose value carries the order: what the pass wrote into another
		// object, and what it spawned. The third, a global table, is per state and is counted instead.
		if (context.witness) mix(static_cast<uint64_t>(context.witness->GetNumberValue("threaded_synced_witness")));
		for (long uniqueID: context.spawnedIDs) mix(static_cast<uint64_t>(uniqueID));
		std::ostringstream text;
		text << std::hex << std::setw(16) << std::setfill('0') << hash;
		return text.str();
	};

	// The count is a build constant, so the two arms differ in PLACEMENT instead: the second puts every
	// object c_PlacementShift states along, which is where a round-robin peer with a different pre-match
	// history would have put it. The global walk orders by unique ID, so both arms must run identically.
	constexpr size_t c_PlacementShift = 7;
	for (size_t countIndex = 0; countIndex < 2; ++countIndex) {
		const size_t shift = countIndex == 0 ? 0 : c_PlacementShift;
		LuaStatesArray replacement(static_cast<size_t>(c_LuaStateCount));
		states.swap(replacement);
		for (LuaStateWrapper& state: states) {
			state.Initialize();
			InstallThreadedSyncedUpdateSelfTestCallbacks(state);
		}

		ThreadedSyncedUpdateSelfTestContext context;
		std::vector<std::unique_ptr<MOPixel>> objects;
		std::vector<std::unique_ptr<MOPixel>> twins;
		objects.reserve(c_ObjectCount);
		MovableObject::PinUniqueIDCounter(savedCounter);
		s_ThreadedSyncedUpdateSelfTestContext = &context;
		bool fixtureReady = true;
		for (int index = 0; index < c_ObjectCount; ++index) {
			auto object = std::make_unique<MOPixel>();
			if (object->Create() < 0) {
				fixtureReady = false;
				break;
			}
			m_ValidParticles.insert(object.get());
			m_AddedParticles.push_back(object.get());
			objects.push_back(std::move(object));
			MOPixel* fixtureObject = objects.back().get();
			fixtureObject->MoveScriptsToState(states[(static_cast<size_t>(index) + shift) % states.size()]);
			const int loadStatus = fixtureObject->LoadScript(fixturePath, true);
			const int adoptStatus = loadStatus < 0 ? -1 : fixtureObject->AdoptScriptObject();
			if (loadStatus < 0 || adoptStatus < 0) {
				std::cout << "[script-graph-selftest] threaded_synced_update_fixture_refused index=" << index << " load=" << loadStatus << " adopt=" << adoptStatus << std::endl;
				fixtureReady = false;
				break;
			}
		}
		for (LuaStateWrapper& state: states) state.Update();

		// The object every SyncedUpdate writes into, and the objects the pass spawns. Neither is
		// registered with a state, so they are written and made, never run.
		std::vector<std::unique_ptr<MOPixel>> spawned;
		std::unique_ptr<MOPixel> witness;
		{
			MovableObject::PinUniqueIDCounter(savedCounter + c_ObjectCount + 64);
			auto created = std::make_unique<MOPixel>();
			if (created->Create() >= 0) {
				witness = std::move(created);
				context.witness = witness.get();
			}
			MovableObject::PinUniqueIDCounter(savedCounter + c_ObjectCount);
		}

		const auto run = [&](bool globalOrder, bool armWrites = false) {
			context.order.clear();
			const long counterBeforeRun = MovableObject::GetUniqueIDCounter();
			if (armWrites) {
				// Above every other fixture object, so a spawn never takes an identity the rows expect.
				MovableObject::PinUniqueIDCounter(savedCounter + c_ObjectCount + 128);
				if (context.witness) context.witness->SetNumberValue("threaded_synced_witness", 0);
				context.spawnedIDs.clear();
				context.globalWrites = 0;
				context.globalWriteStates.clear();
				context.spawn = [this, &spawned, &context] {
					auto object = std::make_unique<MOPixel>();
					if (object->Create() < 0) return;
					m_ValidParticles.insert(object.get());
					m_AddedParticles.push_back(object.get());
					context.spawnedIDs.push_back(object->GetUniqueID());
					spawned.push_back(std::move(object));
				};
			}
			for (const auto& object: objects) object->RequestSyncedUpdate();
			RunThreadedSyncedUpdatePass(globalOrder);
			if (armWrites) {
				context.spawn = nullptr;
				MovableObject::PinUniqueIDCounter(counterBeforeRun);
			}
			return hashFixture(context, objects);
		};

		// What the per-state globals hold after a pass: the total has to be one write per object, and
		// how it is spread over the states is the count's business, never the world's.
		if (fixtureReady && objects.size() == c_ObjectCount) {
			perStateHashes[countIndex] = run(false, true);
			globalHashes[countIndex] = run(true, true);
			globalWriteTotals[countIndex] = context.globalWrites;
			globalWriteStatesWritten[countIndex] = static_cast<long>(context.globalWriteStates.size());
			spawnedInPass[countIndex] = static_cast<long>(context.spawnedIDs.size());

			// Two live objects can share a unique ID: a faithful clone copies the source's, and a
			// restored object adopts a persisted one. A pair of those, on two states, must run in the
			// same order however many states there are. Their MOIDs come from the sim's own index.
			const long sharedUniqueID = savedCounter + c_ObjectCount + 1;
			std::vector<MovableObject*> moidIndex;
			for (size_t twin = 0; twin < 2 && fixtureReady; ++twin) {
				auto object = std::make_unique<MOPixel>();
				MovableObject::PinUniqueIDCounter(sharedUniqueID - 1);
				if (object->Create() < 0) {
					fixtureReady = false;
					break;
				}
				m_ValidParticles.insert(object.get());
				m_AddedParticles.push_back(object.get());
				twins.push_back(std::move(object));
				MOPixel* twinObject = twins.back().get();
				twinObject->UpdateMOID(moidIndex);
				twinObject->MoveScriptsToState(states[(twin * 3 + 1 + shift) % states.size()]);
				const int loadStatus = twinObject->LoadScript(fixturePath, true);
				if (loadStatus < 0 || twinObject->AdoptScriptObject() < 0) fixtureReady = false;
			}
			MovableObject::PinUniqueIDCounter(savedCounter + c_ObjectCount);
			for (LuaStateWrapper& state: states) state.Update();
			context.order.clear();
			for (const auto& object: objects) object->RequestSyncedUpdate();
			for (const auto& twin: twins) twin->RequestSyncedUpdate();
			RunThreadedSyncedUpdatePass(true);
			duplicatePositions[countIndex] = {static_cast<long>(twins[0]->GetNumberValue("threaded_synced_order_length")),
			                                  static_cast<long>(twins[1]->GetNumberValue("threaded_synced_order_length"))};
			duplicateIDs[countIndex] = {twins[0]->GetUniqueID(), twins[1]->GetUniqueID()};
			duplicateMOIDs[countIndex] = {static_cast<long>(twins[0]->GetID()), static_cast<long>(twins[1]->GetID())};
			duplicateHashes[countIndex] = hashOrder({duplicatePositions[countIndex][0], duplicatePositions[countIndex][1]});
			duplicateOrderLength[countIndex] = static_cast<long>(context.order.size());

			// The Harvester shape: the first object of the pass frees another state's object, which the
			// snapshot already holds and the merge reaches later. DeleteEntity frees where the script
			// stands (LuaAdapters.cpp), so the pass must never touch that entry again.
			{
				void* victimPoison = nullptr;
				const void* victimAddress = nullptr;
				long victimID = 0;
				MovableObject::PinUniqueIDCounter(savedCounter + c_ObjectCount + 8);
				auto victim = std::make_unique<MOPixel>();
				if (victim->Create() < 0) {
					freedAcrossStatesReady = false;
				} else {
					m_ValidParticles.insert(victim.get());
					m_AddedParticles.push_back(victim.get());
					MOPixel* victimObject = victim.get();
					// One state on from the first object of the walk, at either placement, and the highest
					// unique ID there is, so the walk reaches it last.
					victimObject->MoveScriptsToState(states[(1 + shift) % states.size()]);
					const int loadStatus = victimObject->LoadScript(fixturePath, true);
					if (loadStatus < 0 || victimObject->AdoptScriptObject() < 0) freedAcrossStatesReady = false;
					victimAddress = victimObject;
					victimID = victimObject->GetUniqueID();
					for (LuaStateWrapper& state: states) state.Update();
					std::vector<long> expectedOrder;
					expectedOrder.reserve(objects.size() + twins.size());
					for (const auto& object: objects) expectedOrder.push_back(object->GetUniqueID());
					for (const auto& twin: twins) expectedOrder.push_back(twin->GetUniqueID());
					std::sort(expectedOrder.begin(), expectedOrder.end());
					context.order.clear();
					for (const auto& object: objects) object->RequestSyncedUpdate();
					for (const auto& twin: twins) twin->RequestSyncedUpdate();
					victimObject->RequestSyncedUpdate();
					context.retire = [this, &victim, &victimPoison] {
						m_ValidParticles.erase(victim.get());
						std::erase(m_AddedParticles, victim.get());
						victim.reset();
						// A block of the same size over the freed one, filled with a value no vtable
						// pointer or flag survives, so a stale read faults instead of looking alive.
						victimPoison = ::operator new(sizeof(MOPixel));
						std::memset(victimPoison, 0xDD, sizeof(MOPixel));
					};
					RunThreadedSyncedUpdatePass(true);
					context.retire = nullptr;
					freedAcrossStatesSkipped[countIndex] = static_cast<long>(GetSyncedPassSkippedDeadEntries());
					freedAcrossStatesRan[countIndex] = static_cast<long>(context.order.size());
					freedAcrossStatesHashes[countIndex] = hashOrder(context.order);
					freedAcrossStatesExpected[countIndex] = hashOrder(expectedOrder);
					freedAcrossStatesVictimID[countIndex] = victimID;
					freedAcrossStatesPoisonHit[countIndex] = victimPoison == victimAddress ? 1 : 0;
					freedAcrossStatesVictimRan = freedAcrossStatesVictimRan || std::find(context.order.begin(), context.order.end(), victimID) != context.order.end();
				}
				if (victimPoison) ::operator delete(victimPoison);
				if (victim) {
					m_ValidParticles.erase(victim.get());
					std::erase(m_AddedParticles, victim.get());
					victim->DestroyScriptState();
				}
				MovableObject::PinUniqueIDCounter(savedCounter + c_ObjectCount);
			}

			if (shift == c_PlacementShift) {
				run(false);
				run(true);
				std::vector<long long> perStateSamples;
				std::vector<long long> globalSamples;
				perStateSamples.reserve(c_MeasureRounds);
				globalSamples.reserve(c_MeasureRounds);
				for (int round = 0; round < c_MeasureRounds; ++round) {
					const auto perStateStarted = std::chrono::steady_clock::now();
					run(false);
					perStateSamples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - perStateStarted).count());
					const auto globalStarted = std::chrono::steady_clock::now();
					run(true);
					globalSamples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - globalStarted).count());
				}
				std::sort(perStateSamples.begin(), perStateSamples.end());
				std::sort(globalSamples.begin(), globalSamples.end());
				perStateUs = perStateSamples[perStateSamples.size() / 2];
				globalUs = globalSamples[globalSamples.size() / 2];

				// The same walk at the size of a battle. What the global order added is per registered
				// MO - one snapshot entry, one heap push and one pop each - so the accepted cost scales
				// with the count and the budget with it. These carry no scripts, which is what most of a
				// battle's registered objects are to this pass: a request flag read and nothing more.
				constexpr int c_LoadObjectCount = 5000;
				std::vector<std::unique_ptr<MOPixel>> loadObjects;
				loadObjects.reserve(c_LoadObjectCount);
				MovableObject::PinUniqueIDCounter(savedCounter + c_ObjectCount + 1024);
				for (int index = 0; index < c_LoadObjectCount; ++index) {
					auto object = std::make_unique<MOPixel>();
					if (object->Create() < 0) break;
					states[(static_cast<size_t>(index) + shift) % states.size()].RegisterMO(object.get());
					loadObjects.push_back(std::move(object));
				}
				MovableObject::PinUniqueIDCounter(savedCounter + c_ObjectCount);
				loadObjectsRegistered = static_cast<int>(loadObjects.size());
				for (LuaStateWrapper& state: states) state.Update();
				{
					std::vector<long long> loadPerStateSamples;
					std::vector<long long> loadGlobalSamples;
					std::vector<long long> loadAfterDeletionSamples;
					const auto measure = [this](bool globalOrder) {
						const auto started = std::chrono::steady_clock::now();
						RunThreadedSyncedUpdatePass(globalOrder);
						return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
					};
					measure(false);
					measure(true);
					for (int round = 0; round < c_MeasureRounds; ++round) {
						loadPerStateSamples.push_back(measure(false));
						loadGlobalSamples.push_back(measure(true));
					}
					// And again with the liveness lookup engaged: one unregistration means the pass can no
					// longer take a pointer on trust, so every entry is looked up in its state's set.
					if (!loadObjects.empty()) {
						states[(loadObjects.size() - 1 + shift) % states.size()].UnregisterMO(loadObjects.back().get());
					}
					for (int round = 0; round < c_MeasureRounds; ++round) {
						loadAfterDeletionSamples.push_back(measure(true));
					}
					std::sort(loadPerStateSamples.begin(), loadPerStateSamples.end());
					std::sort(loadGlobalSamples.begin(), loadGlobalSamples.end());
					std::sort(loadAfterDeletionSamples.begin(), loadAfterDeletionSamples.end());
					loadPerStateUs = loadPerStateSamples[loadPerStateSamples.size() / 2];
					loadGlobalUs = loadGlobalSamples[loadGlobalSamples.size() / 2];
					loadGlobalAfterADeletionUs = loadAfterDeletionSamples[loadAfterDeletionSamples.size() / 2];
				}
				for (size_t index = 0; index < loadObjects.size(); ++index) {
					states[(index + shift) % states.size()].UnregisterMO(loadObjects[index].get());
				}
				loadObjects.clear();

				// The first object of the pass deletes another state's later object, which the pass has
				// already snapshotted. The merge must order that entry by the identity it read at
				// snapshot time; reading the object again picks up whatever took its place.
				constexpr size_t c_RetiredIndex = 500;
				MOPixel* retired = objects[c_RetiredIndex].get();
				std::vector<long> expectedOrder;
				expectedOrder.reserve(objects.size() + twins.size() - 1);
				for (const auto& object: objects) {
					if (object.get() != retired) expectedOrder.push_back(object->GetUniqueID());
				}
				for (const auto& twin: twins) expectedOrder.push_back(twin->GetUniqueID());
				std::sort(expectedOrder.begin(), expectedOrder.end());
				context.order.clear();
				for (const auto& object: objects) object->RequestSyncedUpdate();
				for (const auto& twin: twins) twin->RequestSyncedUpdate();
				context.retire = [this, retired, shift, &states] {
					// What the engine leaves behind when a mid-pass deletion takes an object, short of
					// freeing the fixture's memory: off the valid sets, out of its Lua state, and its
					// slot handed to a new object, which is what a re-used allocation reads back as.
					m_ValidParticles.erase(retired);
					std::erase(m_AddedParticles, retired);
					states[(c_RetiredIndex + shift) % states.size()].UnregisterMO(retired);
					retired->Create();
					retired->ResetRequestedSyncedUpdateFlag();
				};
				RunThreadedSyncedUpdatePass(true);
				context.retire = nullptr;
				retiredExpectedHash = hashOrder(expectedOrder);
				retiredActualHash = hashOrder(context.order);
				retiredExpectedLength = expectedOrder.size();
				retiredActualLength = context.order.size();
				const auto divergence = std::mismatch(expectedOrder.begin(), expectedOrder.end(), context.order.begin(), context.order.end());
				retiredFirstDivergence = static_cast<size_t>(divergence.first - expectedOrder.begin());
				retiredExpectedAt = retiredFirstDivergence < expectedOrder.size() ? expectedOrder[retiredFirstDivergence] : 0;
				retiredActualAt = retiredFirstDivergence < context.order.size() ? context.order[retiredFirstDivergence] : 0;

				// The threaded pass ran on the request flag alone before the global walk, so a
				// registered object whose root is outside the valid sets kept getting its SyncedUpdate.
				constexpr size_t c_UnlistedRootIndex = 7;
				MOPixel* unlisted = objects[c_UnlistedRootIndex].get();
				m_ValidParticles.erase(unlisted);
				context.order.clear();
				for (const auto& object: objects) object->RequestSyncedUpdate();
				RunThreadedSyncedUpdatePass(true);
				unlistedRootRan = std::find(context.order.begin(), context.order.end(), unlisted->GetUniqueID()) != context.order.end();
				m_ValidParticles.insert(unlisted);
			}
		} else {
			passed = false;
			std::cout << "[script-graph-selftest] FAIL threaded_synced_update_global_moid_order fixture_objects=" << objects.size() << " expected=" << c_ObjectCount << std::endl;
		}

		s_ThreadedSyncedUpdateSelfTestContext = nullptr;
		context.witness = nullptr;
		witness.reset();
		for (auto* group: {&objects, &twins, &spawned}) {
			for (const auto& object: *group) {
				m_ValidParticles.erase(object.get());
				std::erase(m_AddedParticles, object.get());
				object->DestroyScriptState();
				object->Destroy();
			}
		}
		states.swap(replacement);
	}

	states.swap(savedStates);
	MovableObject::PinUniqueIDCounter(savedCounter);
	const bool perStateRed = perStateHashes[0] != perStateHashes[1];
	const bool globalGreen = globalHashes[0] == globalHashes[1];
	const double deltaPercent = perStateUs == 0 ? 0.0 : (100.0 * static_cast<double>(globalUs - perStateUs) / static_cast<double>(perStateUs));
	// The accepted cost of the global walk over the per-state walk, as time rather than as a share of
	// whatever the per-state walk happened to take: 0.15 ms of a 16.7 ms tick, at 1,024 registered MOs
	// across 32 states, median of 32 interleaved passes.
	constexpr long long c_AddedBudgetUs = 150;
	const long long addedUs = globalUs - perStateUs;
	const bool timingGreen = perStateUs > 0 && addedUs <= c_AddedBudgetUs;
	const bool retiredGreen = !retiredExpectedHash.empty() && retiredExpectedHash == retiredActualHash;
	const bool duplicateGreen = !duplicateHashes[0].empty() && duplicateHashes[0] == duplicateHashes[1];
	const bool freedAcrossStatesGreen = freedAcrossStatesReady && !freedAcrossStatesHashes[0].empty() &&
	                                    freedAcrossStatesHashes[0] == freedAcrossStatesExpected[0] &&
	                                    freedAcrossStatesHashes[1] == freedAcrossStatesExpected[1] &&
	                                    freedAcrossStatesHashes[0] == freedAcrossStatesHashes[1] &&
	                                    freedAcrossStatesSkipped[0] == 1 && freedAcrossStatesSkipped[1] == 1 &&
	                                    !freedAcrossStatesVictimRan;
	// One global write per object and one spawn per armed pass, at either placement, and the writes land
	// in every state the build runs. Which OBJECTS share a table is what the assignment decides, and that
	// is the row the assignment fixture owns.
	const bool permittedWritesGreen = globalWriteTotals[0] == c_ObjectCount && globalWriteTotals[1] == c_ObjectCount &&
	                                  globalWriteStatesWritten[0] == c_LuaStateCount && globalWriteStatesWritten[1] == c_LuaStateCount &&
	                                  spawnedInPass[0] == 1 && spawnedInPass[1] == 1;
	// 150 us was accepted at 1,024 registered MOs; the walk's added cost is per MO, so the budget is too.
	constexpr long long c_LoadBudgetUs = c_AddedBudgetUs * 5000 / c_ObjectCount;
	const long long loadAddedUs = loadGlobalUs - loadPerStateUs;
	const long long loadAddedWithLookupUs = loadGlobalAfterADeletionUs - loadPerStateUs;
	const bool loadTimingGreen = loadObjectsRegistered == 5000 && loadPerStateUs > 0 &&
	                             loadAddedUs <= c_LoadBudgetUs && loadAddedWithLookupUs <= c_LoadBudgetUs;
	passed = passed && perStateRed && globalGreen && timingGreen && retiredGreen && duplicateGreen && unlistedRootRan && freedAcrossStatesGreen && permittedWritesGreen && loadTimingGreen;
	std::cout << "[script-graph-selftest] " << (perStateRed ? "PASS" : "FAIL")
	          << " threaded_synced_update_per_state_order_red states=" << c_LuaStateCount << " placement=0," << c_PlacementShift
	          << " hash_placed=" << perStateHashes[0] << " hash_shifted=" << perStateHashes[1]
	          << (perStateRed ? "" : " (the per-state control ran the same order at both placements, so it detects nothing)") << std::endl;
	std::cout << "[script-graph-selftest] " << (globalGreen ? "PASS" : "FAIL")
	          << " threaded_synced_update_global_moid_order states=" << c_LuaStateCount << " placement=0," << c_PlacementShift
	          << " hash_placed=" << globalHashes[0] << " hash_shifted=" << globalHashes[1] << std::endl;
	std::cout << "[script-graph-selftest] " << (duplicateGreen ? "PASS" : "FAIL")
	          << " threaded_synced_update_duplicate_unique_id_order states=" << c_LuaStateCount << " placement=0," << c_PlacementShift
	          << " hash_placed=" << duplicateHashes[0] << " hash_shifted=" << duplicateHashes[1]
	          << " pos_placed=" << duplicatePositions[0][0] << "," << duplicatePositions[0][1]
	          << " pos_shifted=" << duplicatePositions[1][0] << "," << duplicatePositions[1][1]
	          << " ids_placed=" << duplicateIDs[0][0] << "," << duplicateIDs[0][1]
	          << " ids_shifted=" << duplicateIDs[1][0] << "," << duplicateIDs[1][1]
	          << " moids_placed=" << duplicateMOIDs[0][0] << "," << duplicateMOIDs[0][1]
	          << " moids_shifted=" << duplicateMOIDs[1][0] << "," << duplicateMOIDs[1][1]
	          << " ran_placed=" << duplicateOrderLength[0] << " ran_shifted=" << duplicateOrderLength[1]
	          << (duplicateGreen ? "" : " (the duplicate pair's order follows where the objects were placed)") << std::endl;
	std::cout << "[script-graph-selftest] " << (unlistedRootRan ? "PASS" : "FAIL")
	          << " threaded_synced_update_unlisted_root_still_runs states=" << c_LuaStateCount
	          << (unlistedRootRan ? "" : " (a registered object whose root is outside the valid sets lost its SyncedUpdate)") << std::endl;
	std::cout << "[script-graph-selftest] " << (retiredGreen ? "PASS" : "FAIL")
	          << " threaded_synced_update_deleted_object_not_reread states=" << c_LuaStateCount << " expected_order=" << retiredExpectedHash
	          << " pass_order=" << retiredActualHash << " expected_length=" << retiredExpectedLength
	          << " pass_length=" << retiredActualLength << " first_divergence=" << retiredFirstDivergence
	          << " expected_at=" << retiredExpectedAt << " pass_at=" << retiredActualAt << std::endl;
	std::cout << "[script-graph-selftest] " << (freedAcrossStatesGreen ? "PASS" : "FAIL")
	          << " threaded_synced_update_freed_across_states_is_skipped states=" << c_LuaStateCount
	          << " placement=0," << c_PlacementShift << " order_placed=" << freedAcrossStatesHashes[0]
	          << " order_shifted=" << freedAcrossStatesHashes[1] << " expected_placed=" << freedAcrossStatesExpected[0]
	          << " expected_shifted=" << freedAcrossStatesExpected[1] << " skipped_placed=" << freedAcrossStatesSkipped[0]
	          << " skipped_shifted=" << freedAcrossStatesSkipped[1] << " ran_placed=" << freedAcrossStatesRan[0]
	          << " ran_shifted=" << freedAcrossStatesRan[1] << " victim_placed=" << freedAcrossStatesVictimID[0]
	          << " victim_shifted=" << freedAcrossStatesVictimID[1] << " victim_ran=" << (freedAcrossStatesVictimRan ? 1 : 0)
	          << " poison_over_the_freed_block=" << freedAcrossStatesPoisonHit[0] << "," << freedAcrossStatesPoisonHit[1]
	          << (freedAcrossStatesGreen ? "" : " (the pass reached an object a script in the same pass had freed)") << std::endl;
	std::cout << "[script-graph-selftest] " << (timingGreen ? "PASS" : "FAIL")
	          << " threaded_synced_update_pass_timing registered=" << c_ObjectCount << " states=" << c_LuaStateCount << " before_us=" << perStateUs
	          << " after_us=" << globalUs << " added_us=" << addedUs << " budget_us=" << c_AddedBudgetUs
	          << " delta_pct=" << std::fixed << std::setprecision(2) << deltaPercent << std::endl;
	std::cout << "[script-graph-selftest] " << (loadTimingGreen ? "PASS" : "FAIL")
	          << " threaded_synced_update_pass_timing_under_load registered=" << loadObjectsRegistered
	          << " states=" << c_LuaStateCount << " before_us=" << loadPerStateUs << " after_us=" << loadGlobalUs
	          << " added_us=" << loadAddedUs << " after_a_deletion_us=" << loadGlobalAfterADeletionUs
	          << " added_with_lookup_us=" << loadAddedWithLookupUs << " budget_us=" << c_LoadBudgetUs
	          << (loadTimingGreen ? "" : " (the walk costs more per registered MO than 1,024 said it would)") << std::endl;
	std::cout << "[script-graph-selftest] " << (permittedWritesGreen ? "PASS" : "FAIL")
	          << " threaded_synced_update_permitted_writes states=" << c_LuaStateCount << " placement=0," << c_PlacementShift
	          << " global_writes=" << globalWriteTotals[0]
	          << "," << globalWriteTotals[1] << " expected=" << c_ObjectCount
	          << " states_written=" << globalWriteStatesWritten[0] << "," << globalWriteStatesWritten[1]
	          << " spawned=" << spawnedInPass[0] << "," << spawnedInPass[1]
	          << (permittedWritesGreen ? "" : " (a spawn, an other-object write or a global write did not happen once per object)") << std::endl;
	return passed;
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
			                                                     g_LuaMan.SetThreadLuaStateOverride(&luaState, true);

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

		// The serial, global-MOID channel for script-driven shared-state mutation;
		// scripts opt in via RequestSyncedUpdate. See Data/Modding/threaded-determinism.md.
		g_LuaMan.SetThreadLuaStateOverride(&g_LuaMan.GetMasterScriptState());
		for (MovableObject* mo: SortedRegisteredMOs(g_LuaMan.GetMasterScriptState())) {
			if (ValidMO(mo->GetRootParent())) {
				mo->RunScriptedFunctionInAppropriateScripts("SyncedUpdate", false, false, {}, {}, {});
			}
		}
		g_LuaMan.SetThreadLuaStateOverride(nullptr);
		RunThreadedSyncedUpdatePass(true);
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
					m_ContiguousActorIDs.erase(*aIt);
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
					pActivity->ForgetDestroyedActor(*aIt);
					pActivity->ReportDeath((*aIt)->GetTeam());
				}

				// Remove from team rosters
				if ((*aIt)->GetTeam() >= Activity::TeamOne && (*aIt)->GetTeam() < Activity::MaxTeamCount)
					// m_ActorRoster[(*aIt)->GetTeam()].remove(*aIt);
					RemoveActorFromTeamRoster(*aIt);

				// Delete
				m_ContiguousActorIDs.erase(*aIt);
				(*aIt)->GetController()->DropLocalProduction();
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
				ForgetActivitySlots(*iIt);
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
				ForgetActivitySlots(*parIt);
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
				ForgetActivitySlots(*parIt);
				(*parIt)->DestroyScriptState();
				delete (*parIt);
				m_ValidParticles.erase(*parIt);
				parIt++;
			}
			m_Particles.erase(midIt, m_Particles.end());
		}
	}

	// This tick's travel is done, so an adopted spawn that has reached its ghost's pose takes the frame back.
	ResolvePreviewAdoptions(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()));

	if (g_ActivityMan.LockstepRelaunchInProgress()) {
		if (Activity* activity = g_ActivityMan.GetActivity()) activity->RebindNonOwnedActorSlots();
		g_ActivityMan.EndLockstepRelaunch();
	}

	// The RNG snapshot has to be taken here, before the see-ray and MOID-draw futures launch below;
	// the object census is taken at the tick's end instead, where the checkpoint archive is written.
	if (g_SimChecksum.IsActive()) {
		DumpControllerDebugSnapshot("end_tick_before_checksum", static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()), m_Actors, nullptr, nullptr, &m_Particles);
		FeedSimChecksumRandomness();
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
	actor->GetController()->ExpireSyncedOrderDisable(static_cast<int64_t>(g_TimerMan.GetSimUpdateCount()));
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
	RebuildContiguousActorIDs();

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
	if (ScenarioRunner::LockstepStopHoldsControllers()) {
		const std::string reason = ScenarioRunner::GetLockstepStopReason();
		ScenarioRunner::SetControllerReplayError(std::string("tick ") + std::to_string(simTick) + " lockstep stopped: " + (reason.empty() ? "coordinator not running" : reason));
		return;
	}
	auto isLocalControllerActor = [&](const Actor* actor) {
		return !ScenarioRunner::WorldCatchUpActive() && (!lockstepActive || (!ScenarioRunner::IsHostMigrationCatchUp() && IsLockstepLocalActor(actor)));
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
	// Record every registered actor's owner once per team it plays for, here, where the wire takes
	// over: the policy would otherwise re-derive it from the control mode a switch is about to change.
	// A live claim is not the owner to return to, so the entry takes the policy's peer, not the claimant.
	if (lockstepActive) {
		for (const Actor* actor: m_Actors) {
			const int64_t uid = static_cast<int64_t>(actor->GetUniqueID());
			// The ownership query normalizes a negative team to 0; the seed records the team it was resolved at.
			const uint8_t team = actor->GetTeam() < 0 ? uint8_t{0} : static_cast<uint8_t>(actor->GetTeam());
			if (!NetActorOwnership::HasSeededOwnerForTeam(uid, team)) {
				NetActorOwnership::SeedOwner(uid, ScenarioRunner::GetLockstepPolicyActorOwner(uid, actor->GetTeam(), !actor->IsPlayerControlled()), team);
			}
		}
	}
	if (lockstepActive && ScenarioRunner::IsControllerLogReplaying()) {
		ScenarioRunner::SetControllerReplayError("lockstep controller sync cannot be combined with controller log replay.");
		return;
	}

	// The pass that samples this machine's seats and runs its AI owns the input it produces; the sim keeps
	// the frame the wire committed for this tick until the frames are snapshotted.
	std::vector<long int> producingActors;
	if (lockstepActive) {
		producingActors = BeginLockstepProducingPass(m_Actors, isLocalControllerActor);
	}

	g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ActorsAI);
	{
		for (Actor* actor: m_Actors) {
			if (isLocalControllerActor(actor)) {
				actor->GetController()->Update();
			}
		}

		// Under lockstep the AI pass may not change the canonical actor: its aim, facing and hatch writes
		// are taken as one-shot intents and undone here, its equip calls become commands, and every peer
		// (this one included) applies them at the committed tick.
		std::vector<ControllerBoundaryBaseline> directBefore;
		if (lockstepActive) {
			for (Actor* actor: m_Actors) {
				if (isLocalControllerActor(actor)) {
					directBefore.push_back(CaptureControllerBoundary(actor));
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
		auto drainDeferredWaypoints = [&]() {
			// The waypoint calls the AI queued, in MOID order: the owner sends them so every peer's
			// queue takes the same writes at the same tick. A non-owner's queued calls are dropped.
			for (Actor* actor: m_Actors) {
				if (isLocalControllerActor(actor)) {
					actor->SendDeferredWaypoints();
				} else {
					actor->TakePendingDeferredWaypoints();
				}
			}
		};
		auto drainDeferredAIModes = [&]() {
			// The AI modes the AI pass wrote, in MOID order: the owner sends them as synced requests so
			// every peer takes the mode at the same tick. A non-owner's queued writes are dropped.
			for (Actor* actor: m_Actors) {
				if (isLocalControllerActor(actor)) {
					actor->SendDeferredAIModes();
				} else {
					actor->TakePendingDeferredAIModes();
				}
			}
		};
		auto drainDeferredScriptMessages = [&]() {
			// The messages the AI pass sent, in MOID order: the owner sends them so every peer's receiver
			// script hears them at the same tick. A non-owner's queued messages are dropped.
			for (Actor* actor: m_Actors) {
				if (isLocalControllerActor(actor)) {
					actor->SendDeferredScriptMessages();
				} else {
					actor->TakePendingDeferredScriptMessages();
				}
			}
		};
		auto drainDeferredGibs = [&]() {
			// The gibs the AI pass asked for, in MOID order: the owner sends them so every peer gibs at the
			// same tick. A non-owner's queued gibs are dropped.
			for (Actor* actor: m_Actors) {
				if (isLocalControllerActor(actor)) {
					actor->SendDeferredGibs();
				} else {
					actor->TakePendingDeferredGibs();
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
			                                                     g_LuaMan.SetThreadLuaStateOverride(&luaState, true);
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
		drainDeferredWaypoints();
		drainDeferredAIModes();
		drainDeferredScriptMessages();
		drainDeferredGibs();
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
		drainDeferredWaypoints();
		drainDeferredAIModes();
		drainDeferredScriptMessages();
		drainDeferredGibs();
		drainDeferredSoundOps();
		// A fixture's scripted writes come last, so they are the pass's final word on the actor.
		if (AIWriteScript::IsActive()) {
			AIWriteScript::RunTick(simTick, m_Actors, isLocalControllerActor);
			drainDeferredEquips();
			drainDeferredWaypoints();
			drainDeferredAIModes();
			drainDeferredScriptMessages();
			drainDeferredGibs();
			drainDeferredSoundOps();
		}

		for (const ControllerBoundaryBaseline& before: directBefore) {
			RestoreControllerBoundary(before, static_cast<long long>(simTick));
		}
	}
	g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ActorsAI);

	if (ScenarioRunner::OfflineCommandsDriveTick()) {
		CommitOfflineValueWrites();
		ApplyOfflineGameCommands(simTick);
	}

	if (ScenarioRunner::WorldCatchUpActive()) {
		std::string error;
		NetLockstepReadyFrame readyFrame;
		if (!ScenarioRunner::TakeWorldCatchUpReadyFrame(simTick, readyFrame, &error)) {
			if (!error.empty()) ScenarioRunner::SetControllerReplayError("private replay tick " + std::to_string(simTick) + ": " + error);
			return;
		}
		std::unordered_set<int64_t> applied;
		ApplyLockstepSeatReclaims(readyFrame, m_Actors);
		if (!ApplyControllerFramesToLockstepActors(m_Actors, readyFrame.localFrames, true, applied, error) ||
		    !ApplyControllerFramesToLockstepActors(m_Actors, readyFrame.remoteFrames, false, applied, error)) {
			ScenarioRunner::SetControllerReplayError(std::string("tick ") + std::to_string(simTick) + " world catch-up apply: " + error);
			return;
		}
		NeutralizeUnframedLockstepActors(m_Actors, applied);
		ApplyLockstepLeaveHandoffs(readyFrame, m_Actors, false);
		g_AudioMan.CommitSoundObservations(readyFrame.frame, readyFrame.localObservations, readyFrame.remoteObservations);
		CommitValueObservations(readyFrame.frame, readyFrame.localValueObservations, readyFrame.remoteValueObservations);
		ApplyLockstepGameCommands(readyFrame);
		ReconcileLockstepControlBindings();
		return;
	}

	if (lockstepActive) {
		std::string error;
		// Sample this tick's local AI decisions and schedule them to APPLY inputDelayFrames ticks from
		// now (QueueLocalInput stamps targetFrame = simTick + D). Actors are then driven this tick by
		// the frame COMMITTED for simTick — sampled D ticks ago — so local and remote apply in phase.
		// At D=0 the committed local frame is this tick's snapshot, so behavior is unchanged.
		std::vector<ControllerFrame> localFrames = SnapshotLockstepControllerFrames(m_Actors, true);
		EndLockstepProducingPass(producingActors);
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
		ApplyLockstepSeatReclaims(readyFrame, m_Actors);
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
		const bool canonicalStartup = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) < ScenarioRunner::GetLockstepEffectiveStartFrame();
		NeutralizeUnframedLockstepActors(m_Actors, applied, canonicalStartup);
		// The round's opening actors join after this apply; before the first frame they take the same route on every peer.
		if (canonicalStartup) NeutralizeUnframedLockstepActors(m_AddedActors, applied, true);
		ApplyLockstepLeaveHandoffs(readyFrame, m_Actors, false);
		DumpControllerDebugSnapshot("lockstep_post_apply", simTick, m_Actors, &readyFrame.remoteFrames);
		g_AudioMan.CommitSoundObservations(readyFrame.frame, readyFrame.localObservations, readyFrame.remoteObservations);
		CommitValueObservations(readyFrame.frame, readyFrame.localValueObservations, readyFrame.remoteValueObservations);
		ApplyLockstepGameCommands(readyFrame);
		// A leave purge moves owners without a command, so the bindings settle here every tick.
		ReconcileLockstepControlBindings();

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
			if (!IsHiddenFromRender(*parIt)) {
				(*parIt)->Draw(pTargetBitmap, targetPos);
			}
		}
		for (const PreviewGhost& ghost: m_PreviewGhosts) {
			if (ghost.object) {
				ghost.object->Draw(pTargetBitmap, targetPos);
			}
		}
	}

	{
		ZoneScopedN("Items Draw");

		for (std::deque<MovableObject*>::reverse_iterator itmIt = m_Items.rbegin(); itmIt != m_Items.rend(); ++itmIt) {
			if (!IsHiddenFromRender(*itmIt)) {
				(*itmIt)->Draw(pTargetBitmap, targetPos);
			}
		}
	}

	{
		ZoneScopedN("Actors Draw");

		for (std::deque<Actor*>::reverse_iterator aIt = m_Actors.rbegin(); aIt != m_Actors.rend(); ++aIt) {
			if (!IsHiddenFromRender(*aIt)) {
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
		if (!IsHiddenFromRender(*itmIt)) {
			(*itmIt)->DrawHUD(pTargetBitmap, targetPos, which);
		}
	}

	for (std::deque<Actor*>::reverse_iterator aIt = m_Actors.rbegin(); aIt != m_Actors.rend(); ++aIt) {
		if (!IsHiddenFromRender(*aIt)) {
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
	// Only the shared world this checkpoint writes comes back under its own identities, so only its
	// objects may be named. A peer's own setup editor holds objects nothing shared owns, and a row for
	// one of those can only refuse the archive on the peer that never had it.
	std::unordered_set<const Entity*> visited;
	std::unordered_set<const MovableObject*> carried;
	const auto collect = [&visited, &carried](const auto& roots) {
		for (const Entity* root: roots) CollectOwnedMovableObjects(root, visited, carried);
	};
	collect(m_Actors); collect(m_Items); collect(m_Particles);
	collect(m_AddedActors); collect(m_AddedItems); collect(m_AddedParticles);
	CollectOwnedMovableObjects(g_SceneMan.GetScene(), visited, carried);
	g_LuaMan.VisitScriptHeldMovableObjects([&visited, &carried](MovableObject* object) {
		CollectOwnedMovableObjects(object, visited, carried);
	});
	const auto shared = [&visited, &carried](const Activity* activity) {
		if (const auto* game = dynamic_cast<const GameActivity*>(activity)) {
			game->VisitCheckpointSharedObjects([&visited, &carried](const Entity* child) { CollectOwnedMovableObjects(child, visited, carried); });
		}
	};
	shared(g_ActivityMan.GetActivity());
	shared(g_ActivityMan.GetCheckpointStartActivity());
	std::map<long, std::vector<bool>> perPeer;
	for (const auto& [identity, object]: m_KnownObjects) {
		if (!carried.contains(object)) continue;
		std::vector<long> links = object->GetCheckpointBorrowedReferences();
		if (std::none_of(links.begin(), links.end(), [](long target) { return target != 0; })) continue;
		references.emplace(identity, std::move(links));
		perPeer.emplace(identity, object->GetCheckpointPerPeerReferences());
	}
	// Written as the map is, with the links only this machine holds (an actor's loaded move target) and a row that holds
	// nothing else marked as its own, and so the count.
	writer.PerPeer(references.size());
	for (const auto& [identity, links]: references) {
		const std::vector<bool>& own = perPeer.at(identity);
		const auto ownLink = [&own](size_t index) { return index < own.size() && own[index]; };
		bool shared = false;
		for (size_t index = 0; index < links.size(); ++index) shared = shared || (links[index] != 0 && !ownLink(index));
		if (!shared) writer.BeginPerPeer();
		writer(identity, links.size());
		for (size_t index = 0; index < links.size(); ++index) {
			if (shared && ownLink(index)) writer.PerPeer(links[index]); else writer(links[index]);
		}
		if (!shared) writer.EndPerPeer();
	}
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
				// Every refusal names itself: which owner, and whether it is the owner, the arity or a target that is missing.
				if (!object) throw std::runtime_error("unresolved native references for owner " + std::to_string(identity) + ": the owner is not in the restored world");
				if (!object->RebindCheckpointBorrowedReferences(links, true)) {
					std::string reason = object->GetClassName() + " " + object->GetPresetName() + " carries " + std::to_string(object->GetCheckpointBorrowedReferences().size()) + " references, the checkpoint names " + std::to_string(links.size());
					for (long target: links) if (target && !FindObjectByUniqueID(target)) reason += "; target " + std::to_string(target) + " is not in the restored world";
					throw std::runtime_error("unresolved native references for owner " + std::to_string(identity) + ": " + reason);
				}
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
		std::map<long, std::pair<int, int>> actorOwners; // Actor -> its seeded owner and the team that owner was seeded at.
		std::array<int, Activity::MaxTeamCount> teamMOIDCount{};
		std::array<std::set<long>, 3> validObjects;
		std::set<long> playerBrains; // The actors human players depend on as their brains.
		// A WorldStructure1 payload predates the owner map, a WorldStructure2 payload the brain record.
		template <class Archive> void Fields(Archive& archive, int version = 3) {
			archive(cohorts);
			// A roster is re-sorted when a seat's own interface asks for the next actor, so its order is this machine's own.
			archive.PerPeer(rosters, sortRoster);
			archive(alarms, quarantine, moidIndex, contiguousActorIDs);
			if (version >= 2) archive(actorOwners);
			archive(teamMOIDCount, validObjects);
			if (version >= 3) archive(playerBrains);
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
	// A slot can outlive the object drawn into it (a script that took the object may have freed it), so only a
	// registered object names its slot; a freed one is never read.
	std::vector<const MovableObject*> registered;
	const std::vector<const MovableObject*>* known = &registered;
	if (const KnownObjectsScope* scope = m_KnownObjectsScope.load(std::memory_order_acquire); scope && scope->m_Version == m_KnownObjectsVersion.load(std::memory_order_acquire)) {
		scope->Copy();
		known = &scope->m_ByAddress;
	} else {
		std::lock_guard<std::mutex> guard(m_ObjectRegisteredMutex);
		registered.reserve(m_KnownObjects.size());
		for (const auto& [uid, object]: m_KnownObjects) registered.push_back(object);
		std::sort(registered.begin(), registered.end());
	}
	state.moidIndex.reserve(m_MOIDIndex.size());
	for (const MovableObject* object: m_MOIDIndex) state.moidIndex.push_back(object && std::binary_search(known->begin(), known->end(), object) ? object->GetUniqueID() : 0);
	for (int team = 0; team < Activity::MaxTeamCount; ++team) {
		identities(m_ActorRoster[team], state.rosters[team]); state.sortRoster[team] = m_SortTeamRoster[team];
		state.teamMOIDCount[team] = m_TeamMOIDCount[team];
	}
	// Derived from the live actors, not from the index's keys: a key is only as alive as the actor it points at.
	for (const Actor* actor: m_Actors) {
		if (auto entry = m_ContiguousActorIDs.find(actor); entry != m_ContiguousActorIDs.end()) state.contiguousActorIDs.emplace(actor->GetUniqueID(), entry->second);
	}
	// Only a live actor's owner travels: a seeded owner for a removed actor would fail the load's live-actor check.
	const auto saveOwner = [&state](const Actor* actor) {
		const int64_t uid = static_cast<int64_t>(actor->GetUniqueID());
		if (const uint8_t owner = NetActorOwnership::GetSeededOwner(uid); owner != 0) {
			state.actorOwners.emplace(static_cast<long>(uid), std::pair{static_cast<int>(owner), static_cast<int>(NetActorOwnership::GetSeededOwnerTeam(uid))});
		}
	};
	for (const Actor* actor: m_Actors) saveOwner(actor);
	for (const Actor* actor: m_AddedActors) saveOwner(actor);
	// Same rule for the brain record: only a live actor's entry travels.
	const auto saveBrain = [this, &state](const Actor* actor) {
		if (m_PlayerBrainIDs.contains(actor->GetUniqueID())) state.playerBrains.insert(actor->GetUniqueID());
	};
	for (const Actor* actor: m_Actors) saveBrain(actor);
	for (const Actor* actor: m_AddedActors) saveBrain(actor);
	for (const AlarmEvent* event: m_AlarmEvents) state.alarms[0].emplace_back(event->m_ScenePos, std::pair{static_cast<int>(event->m_Team), event->m_Range});
	for (const AlarmEvent* event: m_AddedAlarmEvents) state.alarms[1].emplace_back(event->m_ScenePos, std::pair{static_cast<int>(event->m_Team), event->m_Range});
	state.quarantine = m_LockstepJoinQuarantine;
	CheckpointWriter writer("WorldStructure3"); state.Fields(writer); return writer.Text();
}

bool MovableMan::LoadWorldStructure(std::string_view text, bool validateOnly) {
	try {
		WorldStructure state;
		// A game saved before the owner map or the brain record keeps loading; the missing fields stay empty.
		const int version = text.starts_with("15 WorldStructure1 ") ? 1 : (text.starts_with("15 WorldStructure2 ") ? 2 : 3);
		const char* tag = version == 1 ? "WorldStructure1" : (version == 2 ? "WorldStructure2" : "WorldStructure3");
		CheckpointReader reader(text, tag); state.Fields(reader, version); reader.Finish();
		std::set<long> incoming;
		for (const auto& cohort: state.cohorts) for (long uid: cohort) {
			if (uid <= 0 || !incoming.insert(uid).second) throw std::runtime_error("invalid or duplicate world member");
		}
		for (int kind = 0; kind < 3; ++kind) {
			std::set<long> allowed(state.cohorts[kind].begin(), state.cohorts[kind].end());
			allowed.insert(state.cohorts[kind + 3].begin(), state.cohorts[kind + 3].end());
			for (long uid: state.validObjects[kind]) if (!allowed.contains(uid)) throw std::runtime_error("invalid world validity member");
		}
		{
			std::set<long> actors(state.cohorts[0].begin(), state.cohorts[0].end());
			actors.insert(state.cohorts[3].begin(), state.cohorts[3].end());
			for (const auto& entry: state.contiguousActorIDs) {
				if (entry.first <= 0 || !actors.contains(entry.first)) throw std::runtime_error("invalid contiguous actor index member " + std::to_string(entry.first));
			}
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
		std::map<int64_t, NetSeededActorOwner> owners;
		for (const auto& [uid, entry]: state.actorOwners) {
			const auto& [owner, team] = entry;
			if (!actor(uid) || owner < 0 || owner > 255) throw std::runtime_error("owner entry names no live actor");
			if (team < 0 || team > 255) throw std::runtime_error("owner entry names no team");
			owners.emplace(static_cast<int64_t>(uid), NetSeededActorOwner{static_cast<uint8_t>(owner), static_cast<uint8_t>(team)});
		}
		for (long uid: state.playerBrains) {
			if (!actor(uid)) throw std::runtime_error("player brain entry names no live actor");
		}
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
		// The record decides which brains the players depend on, not what this peer's seats found at start.
		m_PlayerBrainIDs.swap(state.playerBrains);
		// A payload from before the record carries none, so re-seed it the way the assignment would have.
		if (version < 3) {
			if (Activity* activity = g_ActivityMan.GetActivity()) {
				activity->RecordSeatedPlayerBrains();
			}
		}
		NetActorOwnership::RestoreSeededOwners(std::move(owners));
		// A restored tick can be reached again after a resync or a rematch; claims made past it must not decide a later tie.
		s_LockstepFrameClaims.clear();
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

bool MovableMan::RunLegacyBrainRecordSelfTest(const Actor* seatBrain) {
	if (!seatBrain) {
		return false;
	}
	const std::string current = SaveWorldStructure();
	WorldStructure live;
	try {
		CheckpointReader reader(current, "WorldStructure3"); live.Fields(reader); reader.Finish();
	} catch (const std::exception&) {
		return false;
	}
	bool reseeded = true;
	for (int version = 1; version <= 2; ++version) {
		CheckpointWriter writer(version == 1 ? "WorldStructure1" : "WorldStructure2"); live.Fields(writer, version);
		reseeded = reseeded && LoadWorldStructure(writer.Text()) && IsPlayerBrain(seatBrain);
	}
	return LoadWorldStructure(current) && reseeded;
}

bool MovableMan::RunContiguousActorIndexSelfTest(Actor* craft) {
	if (!craft) return false;
	if (m_Actors.empty()) {
		delete craft;
		return false;
	}
	AddActor(craft);
	AbsorbAddedMOs();
	RebuildContiguousActorIDs();
	const long craftUID = craft->GetUniqueID();
	const bool indexed = std::find(m_Actors.begin(), m_Actors.end(), craft) != m_Actors.end() && GetContiguousActorID(craft) >= 0;

	const bool cleared = RemoveActor(craft) == craft && GetContiguousActorID(craft) < 0;

	const std::string text = SaveWorldStructure();
	const bool tagged = text.starts_with("15 WorldStructure3 ");
	bool archived = false;
	bool roundTripped = false;
	try {
		WorldStructure parsed;
		CheckpointReader reader(text, "WorldStructure3"); parsed.Fields(reader); reader.Finish();
		const std::set<long> live(parsed.cohorts[0].begin(), parsed.cohorts[0].end());
		archived = parsed.contiguousActorIDs.size() == m_ContiguousActorIDs.size() && !parsed.contiguousActorIDs.contains(craftUID);
		for (const auto& entry: parsed.contiguousActorIDs) archived = archived && live.contains(entry.first);
		CheckpointWriter rewriter("WorldStructure3"); parsed.Fields(rewriter);
		roundTripped = rewriter.Text() == text;
	} catch (const std::exception&) {
	}

	// The brain record travels with the world: note a live actor, save, drop it, and read it back from the record.
	Actor* const recordActor = m_Actors.empty() ? nullptr : m_Actors.front();
	const long recordUID = recordActor ? recordActor->GetUniqueID() : 0;
	const bool recordWasBrain = recordActor && IsPlayerBrain(recordActor);
	bool brainArchived = false;
	bool brainRestored = false;
	if (recordActor) {
		NotePlayerBrain(recordUID, true);
		const std::string withBrain = SaveWorldStructure();
		try {
			WorldStructure parsed;
			CheckpointReader reader(withBrain, "WorldStructure3"); parsed.Fields(reader); reader.Finish();
			brainArchived = parsed.playerBrains.contains(recordUID) && parsed.playerBrains.size() == m_PlayerBrainIDs.size();
		} catch (const std::exception&) {
		}
		NotePlayerBrain(recordUID, false);
		brainRestored = !IsPlayerBrain(recordActor) && LoadWorldStructure(withBrain) && IsPlayerBrain(recordActor);
		NotePlayerBrain(recordUID, recordWasBrain);
	}
	// One human seat and one AI seat: only the human seat's brain is recorded; the legacy load and the
	// last-ditch placement have to reach the record too.
	bool brainLegacyReseeded = false;
	bool brainLastDitch = false;
	const bool brainSeats = g_ActivityMan.GetActivity() && recordActor && m_Actors.size() > 1 &&
	                        g_ActivityMan.GetActivity()->RunPlayerBrainRecordSelfTest(recordActor, m_Actors[1], &brainLegacyReseeded, &brainLastDitch);
	const bool sharedSeats = Activity::RunSharedSeatSelfTest();

	// The shape the crashed resync archives carried: an index entry for an actor the world does not have.
	WorldStructure clean;
	for (long uid = 1001; uid <= 1005; ++uid) clean.cohorts[0].push_back(uid);
	for (int id = 0; id < 5; ++id) clean.contiguousActorIDs.emplace(1001 + id, id);
	WorldStructure orphaned = clean;
	orphaned.contiguousActorIDs.emplace(0, 5);
	WorldStructure stale = clean;
	stale.contiguousActorIDs.emplace(4242, 5);
	CheckpointWriter cleanWriter("WorldStructure3"); clean.Fields(cleanWriter);
	CheckpointWriter orphanedWriter("WorldStructure3"); orphaned.Fields(orphanedWriter);
	CheckpointWriter staleWriter("WorldStructure3"); stale.Fields(staleWriter);
	const bool accepted = LoadWorldStructure(text, true) && LoadWorldStructure(cleanWriter.Text(), true);
	const bool refused = !LoadWorldStructure(orphanedWriter.Text(), true) && !LoadWorldStructure(staleWriter.Text(), true);

	// A game saved before the record carried the owner map: its payload has no owners field at all.
	const auto writeLegacyFields = [](const WorldStructure& state, CheckpointWriter& writer) {
		writer(state.cohorts, state.rosters, state.sortRoster, state.alarms, state.quarantine, state.moidIndex, state.contiguousActorIDs, state.teamMOIDCount, state.validObjects);
	};
	CheckpointWriter legacyWriter("WorldStructure1"); writeLegacyFields(clean, legacyWriter);
	CheckpointWriter legacyTrailingWriter("WorldStructure1"); writeLegacyFields(clean, legacyTrailingWriter); legacyTrailingWriter.Value(7);
	CheckpointWriter trailingWriter("WorldStructure3"); clean.Fields(trailingWriter); trailingWriter.Value(7);
	CheckpointWriter unknownWriter("WorldStructure4"); clean.Fields(unknownWriter);
	// The layout the owner map first shipped as: owners under the old tag, which no reader can tell apart.
	WorldStructure owned = clean;
	owned.actorOwners.emplace(1001, std::pair{2, 1});
	CheckpointWriter intermediateWriter("WorldStructure1"); owned.Fields(intermediateWriter, 2);
	// The same mistake for the brain record: the new field carried under the tag before it.
	WorldStructure brained = clean;
	brained.playerBrains.insert(1001);
	CheckpointWriter intermediateBrainWriter("WorldStructure2"); brained.Fields(intermediateBrainWriter, 3);
	// A game saved before the brain record: owners, no brains.
	CheckpointWriter legacyOwnersWriter("WorldStructure2"); owned.Fields(legacyOwnersWriter, 2);
	const bool legacyAccepted = LoadWorldStructure(legacyWriter.Text(), true) && LoadWorldStructure(legacyOwnersWriter.Text(), true);
	const bool legacyRefused = !LoadWorldStructure(legacyTrailingWriter.Text(), true) && !LoadWorldStructure(trailingWriter.Text(), true) &&
	                           !LoadWorldStructure(unknownWriter.Text(), true) && !LoadWorldStructure(intermediateWriter.Text(), true) &&
	                           !LoadWorldStructure(intermediateBrainWriter.Text(), true);

	auto plantAdded = [this](Actor* actor, int id) {
		if (!actor) {
			return false;
		}
		AddActor(actor);
		m_ContiguousActorIDs[actor] = id;
		return true;
	};

	Actor* addedRemove = craft ? dynamic_cast<Actor*>(craft->Clone()) : nullptr;
	const bool plantedRemove = plantAdded(addedRemove, 9001);
	const bool removedAdded = plantedRemove && RemoveActor(addedRemove) == addedRemove;
	const bool addedRemoveCleared = removedAdded && GetContiguousActorID(addedRemove) < 0;
	if (addedRemove && (removedAdded || !plantedRemove)) {
		delete addedRemove;
		addedRemove = nullptr;
	}

	Actor* absorbDelete = craft ? dynamic_cast<Actor*>(craft->Clone()) : nullptr;
	const Actor* absorbKey = absorbDelete;
	const bool plantedAbsorb = plantAdded(absorbDelete, 9002);
	if (plantedAbsorb) {
		absorbDelete->SetToDelete(true);
		AbsorbAddedMOs();
		absorbDelete = nullptr;
	}
	Actor* absorbRecycled = craft ? dynamic_cast<Actor*>(craft->Clone()) : nullptr;
	const bool absorbDeleteCleared = plantedAbsorb && m_ContiguousActorIDs.find(absorbKey) == m_ContiguousActorIDs.end() &&
	                                 absorbRecycled && (absorbRecycled != absorbKey || GetContiguousActorID(absorbRecycled) < 0);
	if (absorbRecycled) {
		delete absorbRecycled;
	}

	Actor* discardAdded = craft ? dynamic_cast<Actor*>(craft->Clone()) : nullptr;
	const Actor* discardKey = discardAdded;
	const AddQueueMark discardMark = MarkAddQueues();
	const bool plantedDiscard = plantAdded(discardAdded, 9003);
	if (plantedDiscard) {
		DiscardAddedSince(discardMark);
		discardAdded = nullptr;
	}
	Actor* discardRecycled = craft ? dynamic_cast<Actor*>(craft->Clone()) : nullptr;
	const bool discardAddedCleared = plantedDiscard && m_ContiguousActorIDs.find(discardKey) == m_ContiguousActorIDs.end() &&
	                                 discardRecycled && (discardRecycled != discardKey || GetContiguousActorID(discardRecycled) < 0);
	if (discardRecycled) {
		delete discardRecycled;
	}

	AddActor(craft);
	const bool passed = indexed && cleared && archived && roundTripped && tagged && accepted && refused && legacyAccepted && legacyRefused &&
	                    brainArchived && brainRestored && brainSeats && sharedSeats && brainLegacyReseeded && brainLastDitch &&
	                    addedRemoveCleared && absorbDeleteCleared && discardAddedCleared;
	std::cout << "[contiguous-index-selftest] " << (passed ? "PASS" : "FAIL") << " indexed=" << indexed << " cleared=" << cleared
	          << " archived=" << archived << " round_trip=" << roundTripped << " tagged=" << tagged
	          << " accepted=" << accepted << " refused=" << refused
	          << " legacy=" << legacyAccepted << " legacy_refused=" << legacyRefused
	          << " brain_archived=" << brainArchived << " brain_restored=" << brainRestored << " brain_seats=" << brainSeats << " shared_seats=" << sharedSeats
	          << " brain_legacy_reseeded=" << brainLegacyReseeded << " brain_lastditch=" << brainLastDitch
	          << " added_remove=" << addedRemoveCleared << " absorb_delete=" << absorbDeleteCleared
	          << " discard_added=" << discardAddedCleared << std::endl;
	return passed;
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
	m_Counter(MovableObject::GetUniqueIDCounter()) {
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
		++g_MovableMan.m_KnownObjectsVersion;
	}
	g_MovableMan.LoadWorldStructure(m_Structure);
	MovableObject::PinUniqueIDCounter(m_Counter);
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
		g_LuaMan.GetStateByIndex(static_cast<int>(index)).CallScriptGraph("releaseObjects");
	}
	in.primitiveQueues.reset();
	in.musicOwners.reset();
	in.luaGraphs.clear();
	in.held = false;
	m_WorldSetAside = nullptr;
}

bool MovableMan::RestoreWorld(const WorldSnapshot& in) {
	std::string error;
	std::vector<std::string> luaGraphs;
	luaGraphs.reserve(in.luaGraphs.size());
	try {
		for (const CheckpointText& graph: in.luaGraphs) luaGraphs.push_back(graph.Text());
	} catch (const std::exception& exception) {
		std::cout << "[scriptgraph] restore graph refused: " << exception.what() << std::endl;
		return false;
	}
	if (!LoadWorldStructure(in.structure, true) || !ValidateScriptGraphs(luaGraphs, &error)) {
		std::cout << "[scriptgraph] restore refused before replacement: " << error << std::endl;
		return false;
	}
	// A caller running a speculative world already owns the originals and its rollback.
	if (m_WorldSetAside) return RestoreWorldCandidate(in, luaGraphs);
	const std::string globals = g_ActivityMan.CaptureRuntimeGlobals();
	WorldSetAside original;
	if (!SetAsideWorld(original)) return false;
	bool restored = false;
	try { restored = RestoreWorldCandidate(in, luaGraphs); }
	catch (const std::exception& exception) { std::cout << "[scriptgraph] candidate failed: " << exception.what() << std::endl; }
	if (restored) {
		DiscardWorld(original);
	} else {
		if (!ReinstateWorld(original)) std::cout << "[scriptgraph] original world could not be reinstated" << std::endl;
		g_ActivityMan.RestoreRuntimeGlobals(globals);
	}
	return restored;
}
