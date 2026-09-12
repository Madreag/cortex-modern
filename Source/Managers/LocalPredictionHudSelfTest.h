#pragma once

#include "ActivityMan.h"
#include "Actor.h"
#include "AHuman.h"
#include "HDFirearm.h"
#include "InventoryMenuGUI.h"
#include "LocalPrediction.h"
#include "MovableMan.h"
#include "PieMenu.h"
#include "SceneMan.h"
#include "TimerMan.h"

#include <algorithm>
#include <iostream>
#include <string>

namespace RTE {
namespace LocalPredictionHudSelfTest {

	inline long long g_PressTick = 0;
	inline bool g_Checked = false;
	inline bool g_Sampled = false;
	inline int g_AmmoDrawn = -1;
	inline int g_AmmoOutcome = -1;
	inline bool g_IsActor = false;
	inline bool g_IsValidMO = false;
	inline double g_MinTimerElapsed = 0.0;
	inline Vector g_DrawCenter;
	inline Vector g_CloneCpu;
	inline Actor* g_DrawActor = nullptr;
	inline Actor* g_DrawnActor = nullptr;
	inline bool g_PieEnabledBefore = false;
	inline bool g_PieVisibleBefore = false;
	inline bool g_PieEnabledAfter = false;
	inline bool g_PieVisibleAfter = false;
	inline std::string g_DrawEquipped;
	inline std::string g_CloneEquipped;

	inline void SampleBeforeRender() {
		if (g_PressTick <= 0 || g_TimerMan.GetSimUpdateCount() != g_PressTick) {
			return;
		}
		Activity* activity = g_ActivityMan.GetActivity();
		if (!activity) {
			return;
		}
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
			if (Actor* actor = activity->GetControlledActor(player)) {
				if (PieMenu* pie = actor->GetPieMenu()) {
					g_PieEnabledBefore = pie->IsEnabled();
					g_PieVisibleBefore = pie->IsVisible();
				}
				break;
			}
		}
	}

	inline void SampleDuringRender() {
		if (g_PressTick <= 0 || g_Sampled || g_TimerMan.GetSimUpdateCount() != g_PressTick) {
			return;
		}
		if (!LocalPrediction::IsRendering()) {
			return;
		}
		Activity* activity = g_ActivityMan.GetActivity();
		if (!activity) {
			return;
		}
		g_AmmoOutcome = LocalPrediction::GetLastOutcome().roundsInMag;
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
			Actor* drawn = activity->GetControlledActor(player);
			if (!drawn) {
				continue;
			}
			g_DrawnActor = drawn;
			g_IsActor = g_MovableMan.IsActor(drawn);
			g_IsValidMO = g_MovableMan.ValidMO(drawn);
			g_MinTimerElapsed = std::min({drawn->GetLastSecondTimerElapsedSimMS(), drawn->GetStableRecoverTimerElapsedSimMS(), drawn->GetHeartBeatTimerElapsedSimMS(), drawn->GetNewControlTimerElapsedSimMS(), drawn->GetAlarmTimerElapsedSimMS(), drawn->GetDeathTimerElapsedSimMS()});
			g_CloneCpu = drawn->GetRenderCPUPos();
			if (const AHuman* human = dynamic_cast<const AHuman*>(drawn)) {
				if (const HeldDevice* held = human->GetEquippedItem()) {
					g_CloneEquipped = held->GetPresetName();
					if (const HDFirearm* gun = dynamic_cast<const HDFirearm*>(held)) {
						g_AmmoDrawn = gun->GetRoundInMagCount();
					}
				}
			}
			g_Sampled = true;
			break;
		}
	}

	inline void SampleAfterDraw() {
		if (g_PressTick <= 0 || g_TimerMan.GetSimUpdateCount() != g_PressTick) {
			return;
		}
		g_DrawActor = InventoryMenuGUI::GetLastDrawActor();
		g_DrawCenter = InventoryMenuGUI::GetLastDrawCenter();
		g_DrawEquipped = InventoryMenuGUI::GetLastDrawEquippedName();
	}

	inline void SampleAfterRender() {
		if (g_PressTick <= 0 || g_TimerMan.GetSimUpdateCount() != g_PressTick) {
			return;
		}
		Activity* activity = g_ActivityMan.GetActivity();
		if (!activity) {
			return;
		}
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
			if (Actor* actor = activity->GetControlledActor(player)) {
				if (PieMenu* pie = actor->GetPieMenu()) {
					g_PieEnabledAfter = pie->IsEnabled();
					g_PieVisibleAfter = pie->IsVisible();
				}
				break;
			}
		}
	}

	inline bool Check() {
		if (g_PressTick <= 0 || g_Checked) {
			return true;
		}
		g_Checked = true;
		bool passed = true;
		const auto check = [&passed](const char* name, bool ok, const std::string& detail) {
			std::cout << "[preview-hud-selftest] " << (ok ? "PASS " : "FAIL ") << name << ": " << detail << std::endl;
			passed = passed && ok;
		};
		check("the_ammo_readout_follows_the_preview", g_Sampled && g_AmmoDrawn >= 0 && g_AmmoDrawn == g_AmmoOutcome,
		      "drawn rounds " + std::to_string(g_AmmoDrawn) + " outcome rounds " + std::to_string(g_AmmoOutcome) + (g_Sampled ? "" : " (not sampled)"));
		const float carouselDist = g_SceneMan.ShortestDistance(g_DrawCenter, g_CloneCpu, g_SceneMan.SceneWrapsX()).GetMagnitude();
		check("the_carousel_follows_the_preview", g_Sampled && g_DrawActor == g_DrawnActor && carouselDist < 0.5F && g_DrawEquipped == g_CloneEquipped,
		      "draw actor " + std::string(g_DrawActor == g_DrawnActor ? "is" : "is not") + " the preview, center delta " + std::to_string(carouselDist) + ", equipped '" + g_DrawEquipped + "' vs '" + g_CloneEquipped + "'");
		check("the_activity_cursor_survives_a_preview", g_Sampled && g_IsActor && g_IsValidMO,
		      "IsActor=" + std::to_string(g_IsActor ? 1 : 0) + " ValidMO=" + std::to_string(g_IsValidMO ? 1 : 0) + " on the substituted actor");
		check("a_preview_timer_reset_reads_forward", g_Sampled && g_MinTimerElapsed >= 0.0,
		      "min clone timer elapsed " + std::to_string(g_MinTimerElapsed) + " ms");
		check("no_canonical_hud_write", g_PieEnabledBefore == g_PieEnabledAfter && g_PieVisibleBefore == g_PieVisibleAfter,
		      "canonical pie enabled " + std::to_string(g_PieEnabledBefore ? 1 : 0) + "->" + std::to_string(g_PieEnabledAfter ? 1 : 0) + " visible " + std::to_string(g_PieVisibleBefore ? 1 : 0) + "->" + std::to_string(g_PieVisibleAfter ? 1 : 0));
		std::cout << "[preview-hud-selftest] " << (passed ? "PASS" : "FAIL") << " press tick " << g_PressTick << std::endl;
		return passed;
	}

} // namespace LocalPredictionHudSelfTest
} // namespace RTE
