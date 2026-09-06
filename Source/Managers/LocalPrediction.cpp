#include "LocalPrediction.h"

#include "Activity.h"
#include "ActivityMan.h"
#include "Actor.h"
#include "Attachable.h"
#include "AudioMan.h"
#include "CameraMan.h"
#include "ControllerFrame.h"
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
	bool LocalPrediction::s_Rendering = false;
	int LocalPrediction::s_Override = -1;
	int LocalPrediction::s_DepthOverride = 0;
	long long LocalPrediction::s_PreviewedTick = -1;
	uint64_t LocalPrediction::s_PreviewCount = 0;
	uint64_t LocalPrediction::s_PreviewTicks = 0;
	double LocalPrediction::s_PreviewMs = 0.0;
	uint64_t LocalPrediction::s_Refusals = 0;

	static void SetHitsMOsRecursive(MovableObject* mo, bool hitsMOs) {
		mo->SetToHitMOs(hitsMOs);
		if (MOSRotating* rotating = dynamic_cast<MOSRotating*>(mo)) {
			for (Attachable* attachable: rotating->GetAttachables()) {
				SetHitsMOsRecursive(attachable, hitsMOs);
			}
		}
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
		Activity::RollbackState activityState;
		activity->CaptureRollbackState(activityState);
		static TerrainLayerSnapshot terrain;
		if (!terrain.Capture()) {
			return;
		}
		Trace("fenced");
		const MovableMan::AddQueueMark mark = g_MovableMan.MarkAddQueues();
		Trace("marked");
		LuaMan::SetScriptsFrozen(true);
		AudioMan::SetPlaybackSuppressed(true);
		PostProcessMan::SetRegistrationSuppressed(true);
		const uint64_t refusalsBefore = g_MovableMan.GetSpeculativeRefusals();
		g_MovableMan.SetSpeculative(true);
		{
			MovableObject::FaithfulCloneScope scope(false);
			for (Preview& preview: targets) {
				preview.clone = dynamic_cast<Actor*>(preview.original->Clone());
			}
		}
		MovableObject::PinUniqueIDCounter(uidCounter);
		Trace("cloned");
		for (Preview& preview: targets) {
			preview.clone->ResolveFaithfulLinks();
			SetHitsMOsRecursive(preview.clone, false);
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
				for (const ControllerFrame& frame: frames) {
					if (frame.actorUniqueID != static_cast<int64_t>(clone->GetUniqueID())) {
						continue;
					}
					ControllerFrameCodec::ApplyActorState(frame, *clone, &error);
					ControllerFrameCodec::Apply(frame, *clone->GetController(), &error);
					clone->GetController()->SetWireApplyTick(static_cast<int64_t>(tick));
				}
				Trace("applied");
				clone->NewFrame();
				clone->PreTravel();
				clone->Travel();
				clone->PostTravel();
				Trace("traveled");
				clone->PreControllerUpdate();
				clone->Update();
				Trace("updated");
				clone->PostUpdate();
			}
		}

		Trace("stepped");
		g_MovableMan.SetSpeculative(false);
		s_Refusals += g_MovableMan.GetSpeculativeRefusals() - refusalsBefore;
		g_MovableMan.DiscardAddedSince(mark);
		Trace("discarded");
		terrain.Restore();
		activity->RestoreRollbackState(activityState);
		g_SimRNG.SetEngineState(rngState);
		g_SimRNG.SetDrawCount(rngDraws);
		g_TimerMan.RestoreSimTickAfterPreview(simCount, simTicks);
		MovableObject::PinUniqueIDCounter(uidCounter);
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
		s_PreviewedTick = -1;
	}

	std::string LocalPrediction::DescribeStats() {
		if (s_PreviewCount == 0) {
			return "";
		}
		return "previews=" + std::to_string(s_PreviewCount) + " actor_ticks=" + std::to_string(s_PreviewTicks) + " ms_total=" + std::to_string(s_PreviewMs) + " avg_ms=" + std::to_string(s_PreviewMs / static_cast<double>(s_PreviewCount)) + " refusals=" + std::to_string(s_Refusals);
	}
} // namespace RTE
