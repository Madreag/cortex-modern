#include "LocalPrediction.h"

#include "Activity.h"
#include "ActivityMan.h"
#include "Actor.h"
#include "AHuman.h"
#include "Attachable.h"
#include "AudioMan.h"
#include "CameraMan.h"
#include "Controller.h"
#include "ControllerFrame.h"
#include "FaultInjection.h"
#include "HDFirearm.h"
#include "LuaMan.h"
#include "MOSRotating.h"
#include "MovableMan.h"
#include "PostProcessMan.h"
#include "RTETools.h"
#include "ScenarioRunner.h"
#include "SceneMan.h"
#include "SettingsMan.h"
#include "TerrainLayerSnapshot.h"
#include "TimerMan.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>

namespace RTE {

	std::vector<LocalPrediction::Preview> LocalPrediction::s_Previews;
	std::vector<MovableObject*> LocalPrediction::s_TakenResidents;
	LocalPrediction::Outcome LocalPrediction::s_LastOutcome;
	bool LocalPrediction::s_Rendering = false;
	int LocalPrediction::s_Override = -1;
	int LocalPrediction::s_DepthOverride = 0;
	long long LocalPrediction::s_PreviewedTick = -1;
	uint64_t LocalPrediction::s_PreviewCount = 0;
	uint64_t LocalPrediction::s_PreviewTicks = 0;
	double LocalPrediction::s_PreviewMs = 0.0;

	// Gives the clone the MOIDs its original holds this frame, so its own rays and hits ignore the original.
	static void AdoptMOIDs(Actor* clone, const Actor* original) {
		const MOID rootMOID = original->GetID();
		if (rootMOID == g_NoMOID || rootMOID <= 0) {
			return;
		}
		std::vector<MovableObject*> scratch(static_cast<size_t>(rootMOID), nullptr);
		clone->UpdateMOID(scratch);
	}

	static bool TraceEnabled() {
		static const bool enabled = std::getenv("CC_LOCALPRED_TRACE") != nullptr;
		return enabled;
	}

	static void Trace(const char* what) {
		if (TraceEnabled()) {
			std::cout << "[localpred] " << what << " tick=" << g_TimerMan.GetSimUpdateCount() << std::endl;
		}
	}

	bool LocalPrediction::IsEnabled() {
		if (s_Override >= 0) {
			return s_Override == 1;
		}
		return g_SettingsMan.LocalPredictionEnabled();
	}

	void LocalPrediction::RunPreview() {
		if (!IsEnabled() || !ScenarioRunner::IsLockstepControllerSyncActive() || ScenarioRunner::IsLockstepPaused() || !g_ActivityMan.ActivityRunning()) {
			if (TraceEnabled()) {
				std::cout << "[localpred] skip: enabled=" << IsEnabled() << " lockstep=" << ScenarioRunner::IsLockstepControllerSyncActive() << " paused=" << ScenarioRunner::IsLockstepPaused() << " running=" << g_ActivityMan.ActivityRunning() << std::endl;
			}
			Clear();
			return;
		}
		// Nothing new arrives between sim ticks, so a frame drawn from the same tick reuses the previews.
		if (!s_Previews.empty() && s_PreviewedTick == g_TimerMan.GetSimUpdateCount()) {
			return;
		}
		Clear();
		const int delay = s_DepthOverride > 0 ? s_DepthOverride : static_cast<int>(ScenarioRunner::GetLockstepLocalInputDelay());
		const int depth = std::min(delay, std::max(0, g_SettingsMan.GetLocalPredictionMaxTicks()));
		if (depth <= 0) {
			return;
		}
		Activity* activity = g_ActivityMan.GetActivity();
		std::vector<Preview> targets;
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
			if (!activity->PlayerActive(player) || !activity->PlayerHuman(player)) {
				continue;
			}
			Actor* actor = activity->GetControlledActor(player);
			if (!actor || !g_MovableMan.ValidMO(actor) || !g_MovableMan.IsActor(actor)) {
				continue;
			}
			// Playback owns nothing on the wire, but the recording carries the human's frames; preview them.
			if (!ScenarioRunner::IsLockstepReplayPlayback() && !ScenarioRunner::IsLockstepLocalActor(static_cast<int64_t>(actor->GetUniqueID()), actor->GetTeam(), !actor->IsPlayerControlled())) {
				continue;
			}
			if (std::any_of(targets.begin(), targets.end(), [actor](const Preview& preview) { return preview.original == actor; })) {
				continue;
			}
			targets.push_back({actor, nullptr, activity->ScreenOfPlayer(player)});
		}
		if (targets.empty()) {
			if (TraceEnabled()) {
				for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
					const Actor* actor = activity->GetControlledActor(player);
					std::cout << "[localpred] no target: player " << player << " active=" << activity->PlayerActive(player) << " human=" << activity->PlayerHuman(player)
					          << " actor=" << (actor ? static_cast<long long>(actor->GetUniqueID()) : 0) << " valid=" << (actor ? g_MovableMan.ValidMO(actor) : false)
					          << " isactor=" << (actor ? g_MovableMan.IsActor(actor) : false)
					          << " local=" << (actor ? ScenarioRunner::IsLockstepLocalActor(static_cast<int64_t>(actor->GetUniqueID()), actor->GetTeam(), !actor->IsPlayerControlled()) : false) << std::endl;
				}
			}
			return;
		}
		Trace("preview start");

		const auto start = std::chrono::steady_clock::now();
		// The seeing pass reads the terrain the clone may carve; let it finish first.
		g_MovableMan.WaitForActorsSeeTask();
		// Fence everything a preview tick can touch; all of it goes back before the canonical sim resumes.
		const long long simCount = g_TimerMan.GetSimUpdateCount();
		const long long simTicks = g_TimerMan.GetSimTimeTicks();
		const std::mt19937 rngState = g_SimRNG.GetEngineState();
		const uint64_t rngDraws = g_SimRNG.GetDrawCount();
		const long uidCounter = MovableObject::GetUniqueIDCounter();
		const uint64_t soundIdentityCursor = g_AudioMan.GetCheckpointSoundContainerCursor();
		Activity::RollbackState activityState;
		activity->CaptureRollbackState(activityState);
		static TerrainLayerSnapshot terrain;
		if (!terrain.Capture()) {
			return;
		}
		Trace("fenced");
		const MovableMan::AddQueueMark mark = g_MovableMan.MarkAddQueues();
		const MovableMan::SpeculationStats statsBefore = g_MovableMan.GetSpeculationStats();
		LuaMan::SetScriptsFrozen(true);
		AudioMan::SetPlaybackSuppressed(true);
		PostProcessMan::SetRegistrationSuppressed(true);
		g_MovableMan.BeginSpeculation();
		{
			MovableObject::FaithfulCloneScope scope(false);
			for (Preview& preview: targets) {
				preview.clone = dynamic_cast<Actor*>(preview.original->Clone());
			}
		}
		MovableObject::PinUniqueIDCounter(uidCounter);
		Trace("cloned");
		for (Preview& preview: targets) {
			// Links into the world resolve to the overlay's shadows; links inside the clone stay inside it.
			g_MovableMan.SetFaithfulLinkRoot(preview.clone);
			preview.clone->ResolveFaithfulLinks();
			g_MovableMan.SetFaithfulLinkRoot(nullptr);
			AdoptMOIDs(preview.clone, preview.original);
		}
		Trace("resolved");

		std::string error;
		std::vector<ControllerFrame> frames;
		for (int step = 1; step <= depth; ++step) {
			g_TimerMan.AdvanceSimTickForPreview();
			const uint64_t tick = static_cast<uint64_t>(simCount) + static_cast<uint64_t>(step);
			frames.clear();
			ScenarioRunner::PeekLockstepLocalControllerFrames(tick, frames);
			for (Preview& preview: targets) {
				Actor* clone = preview.clone;
				// The same stages in the same order as the world update: travel, pre-controller, wire, update, post.
				MovableMan::TravelStage(clone, true);
				Trace("traveled");
				MovableMan::PreControllerStage(clone);
				for (const ControllerFrame& frame: frames) {
					if (frame.actorUniqueID == static_cast<int64_t>(clone->GetUniqueID())) {
						MovableMan::ApplyLockstepFrameToActor(*clone, frame, tick, &error);
						if (TraceEnabled()) {
							std::cout << "[localpred] frame tick=" << tick << " of " << frames.size() << " mask=" << std::hex << frame.stateMask << std::dec << " pickup=" << clone->GetController()->IsState(ControlState::WEAPON_PICKUP)
							          << " fg=" << frame.equippedFGUniqueID << " mode=" << static_cast<int>(frame.inputMode) << " quick_disabled=" << frame.IsQuickDisabled() << std::endl;
						}
					}
				}
				Trace("applied");
				MovableMan::UpdateStage(clone, true);
				Trace("updated");
				if (TraceEnabled()) {
					const HeldDevice* reach = clone->GetItemInReach();
					const HeldDevice* held = dynamic_cast<AHuman*>(clone) ? dynamic_cast<AHuman*>(clone)->GetEquippedItem() : nullptr;
					std::cout << "[localpred] step tick=" << tick << " pickup=" << clone->GetController()->IsState(ControlState::WEAPON_PICKUP) << " disabled=" << clone->GetController()->IsDisabled()
					          << " reach=" << (reach ? reach->GetPresetName() + "#" + std::to_string(reach->GetUniqueID()) + (g_MovableMan.IsDevice(reach) ? "(device)" : "(not a device)") : std::string("-"))
					          << " held=" << (held ? held->GetPresetName() + "#" + std::to_string(held->GetUniqueID()) : std::string("-")) << " status=" << clone->GetStatus();
					if (const HDFirearm* gun = dynamic_cast<const HDFirearm*>(held)) {
						std::cout << " rounds=" << gun->GetRoundInMagCount() << " canfire=" << gun->CanFire() << " activated=" << gun->IsActivated() << " firedonce=" << gun->FiredOnce() << " reloading=" << gun->IsReloading()
						          << " since_fire_ms=" << gun->GetMSSinceLastFire() << " since_act_ms=" << gun->GetMSSinceActivation() << " ms_per_round=" << gun->GetMSPerRound() << " act_delay=" << gun->GetActivationDelay() << " deact_delay=" << gun->GetDeactivationDelay();
					}
					std::cout << std::endl;
				}
				MovableMan::PostUpdateStage(clone);
			}
		}
		Trace("stepped");

		Outcome outcome;
		for (const Preview& preview: targets) {
			if (const AHuman* human = dynamic_cast<const AHuman*>(preview.clone)) {
				if (const HeldDevice* equipped = human->GetEquippedItem()) {
					outcome.equipped = equipped->GetPresetName();
					if (const HDFirearm* firearm = dynamic_cast<const HDFirearm*>(equipped)) {
						outcome.roundsInMag = firearm->GetRoundInMagCount();
						outcome.firedOnce = firearm->FiredOnce();
					}
				}
				break;
			}
		}
		const MovableMan::AddQueueMark after = g_MovableMan.MarkAddQueues();
		outcome.spawned = (after.actors - mark.actors) + (after.items - mark.items) + (after.particles - mark.particles);
		outcome.spawnedNames = g_MovableMan.DescribeAddedSince(mark);
		std::vector<MovableObject*> taken;
		g_MovableMan.EndSpeculation(&taken);
		Trace("discarded");
		const MovableMan::SpeculationStats statsAfter = g_MovableMan.GetSpeculationStats();
		outcome.shadows = statsAfter.shadows - statsBefore.shadows;
		outcome.taken = statsAfter.taken - statsBefore.taken;
		outcome.violations = statsAfter.violations - statsBefore.violations;
		for (const MovableObject* resident: taken) {
			if (const HDFirearm* firearm = dynamic_cast<const HDFirearm*>(resident)) {
				outcome.takenRounds = firearm->GetRoundInMagCount();
			}
		}
		if (FaultInjected("preview_take_canonical")) {
			// Test-only: the theft the overlay exists to prevent, so the gates can be shown to catch it.
			for (MovableObject* resident: taken) {
				std::cout << "[fault] preview_take_canonical: removing " << resident->GetPresetName() << " uid=" << resident->GetUniqueID() << " from the world" << std::endl;
				g_MovableMan.RemoveMO(resident);
			}
		}
		terrain.Restore();
		activity->RestoreRollbackState(activityState);
		g_SimRNG.SetEngineState(rngState);
		g_SimRNG.SetDrawCount(rngDraws);
		g_TimerMan.RestoreSimTickAfterPreview(simCount, simTicks);
		MovableObject::PinUniqueIDCounter(uidCounter);
		// The rounds a preview pops take fresh sound identities with them; the canonical cursor keeps its place.
		g_AudioMan.SetCheckpointSoundContainerCursor(soundIdentityCursor);
		PostProcessMan::SetRegistrationSuppressed(false);
		AudioMan::SetPlaybackSuppressed(false);
		LuaMan::SetScriptsFrozen(false);

		for (const Preview& preview: targets) {
			if (preview.screen >= 0) {
				Vector scrollPos = preview.clone->GetPos();
				g_SceneMan.ForceBounds(scrollPos);
				g_CameraMan.SetScrollTarget(scrollPos, 0.1F, preview.screen);
			}
		}
		Trace("restored");
		s_Previews = std::move(targets);
		s_TakenResidents = std::move(taken);
		s_LastOutcome = outcome;
		s_PreviewedTick = simCount;
		++s_PreviewCount;
		s_PreviewTicks += static_cast<uint64_t>(depth) * s_Previews.size();
		s_PreviewMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	void LocalPrediction::BeginRender() {
		if (s_Rendering || s_Previews.empty()) {
			return;
		}
		Trace("render begin");
		g_MovableMan.WaitForActorsSeeTask();
		g_MovableMan.CompleteQueuedMOIDDrawings();
		Activity* activity = g_ActivityMan.GetActivity();
		for (const Preview& preview: s_Previews) {
			g_MovableMan.SwapActorForRender(preview.original, preview.clone);
			if (activity) {
				activity->SubstituteActorForRender(preview.original, preview.clone);
			}
		}
		// The previews carry their taken items; the residents stay off the frame meanwhile.
		for (const MovableObject* resident: s_TakenResidents) {
			g_MovableMan.HideForRender(resident, true);
		}
		s_Rendering = true;
	}

	void LocalPrediction::EndRender() {
		if (s_Rendering) {
			Activity* activity = g_ActivityMan.GetActivity();
			for (const Preview& preview: s_Previews) {
				g_MovableMan.SwapActorForRender(preview.clone, preview.original);
				if (activity) {
					activity->SubstituteActorForRender(preview.clone, preview.original);
				}
			}
			for (const MovableObject* resident: s_TakenResidents) {
				g_MovableMan.HideForRender(resident, false);
			}
			s_Rendering = false;
		}
	}

	void LocalPrediction::Clear() {
		EndRender();
		for (Preview& preview: s_Previews) {
			delete preview.clone;
		}
		if (!s_Previews.empty()) {
			Trace("clones dropped");
		}
		s_Previews.clear();
		s_TakenResidents.clear();
		s_PreviewedTick = -1;
	}

	uint64_t LocalPrediction::GetShadows() {
		return g_MovableMan.GetSpeculationStats().shadows;
	}

	uint64_t LocalPrediction::GetTaken() {
		return g_MovableMan.GetSpeculationStats().taken;
	}

	uint64_t LocalPrediction::GetViolations() {
		return g_MovableMan.GetSpeculationStats().violations;
	}

	std::string LocalPrediction::DescribeLastOutcome() {
		const Outcome& o = s_LastOutcome;
		return "equipped=" + (o.equipped.empty() ? std::string("-") : o.equipped) + " rounds=" + std::to_string(o.roundsInMag) + " taken_rounds=" + std::to_string(o.takenRounds) + " fired_once=" + std::to_string(o.firedOnce ? 1 : 0) +
		       " spawned=" + std::to_string(o.spawned) + (o.spawnedNames.empty() ? std::string() : " [" + o.spawnedNames + "]") + " taken=" + std::to_string(o.taken) + " shadows=" + std::to_string(o.shadows) + " violations=" + std::to_string(o.violations);
	}

	std::string LocalPrediction::DescribeStats() {
		if (s_PreviewCount == 0) {
			return "";
		}
		const MovableMan::SpeculationStats& stats = g_MovableMan.GetSpeculationStats();
		return "previews=" + std::to_string(s_PreviewCount) + " actor_ticks=" + std::to_string(s_PreviewTicks) + " ms_total=" + std::to_string(s_PreviewMs) + " avg_ms=" + std::to_string(s_PreviewMs / static_cast<double>(s_PreviewCount)) +
		       " shadows=" + std::to_string(stats.shadows) + " taken=" + std::to_string(stats.taken) + " violations=" + std::to_string(stats.violations);
	}
} // namespace RTE
