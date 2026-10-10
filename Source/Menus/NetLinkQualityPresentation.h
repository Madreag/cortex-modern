#pragma once

#include "NetLinkQuality.h"

#include <cstdint>
#include <string>

namespace RTE::NetLinkQualityPresentation {
	inline constexpr const char* c_ToggleTitle = "Connection indicator";
	struct Words {
		const char* state;
		const char* hint;
		uint8_t red, green, blue;
	};

	inline Words Describe(NetLinkQuality::State state) {
		switch (state) {
			case NetLinkQuality::State::Good: return {"Good", "Presses arrive on time", 105, 210, 120};
			case NetLinkQuality::State::Marginal: return {"Unsteady", "Presses still arrive", 245, 185, 70};
			case NetLinkQuality::State::Substituting: return {"Inputs affected", "Presses may not register now", 245, 100, 100};
			case NetLinkQuality::State::Lost: return {"Reconnecting", "The AI plays your units", 175, 180, 190};
		}
		return {"Reconnecting", "The AI plays your units", 175, 180, 190};
	}

	inline std::string SeatText(const NetLinkQuality& quality) {
		return (quality.state == NetLinkQuality::State::Lost ? "--" : std::to_string(quality.rttMs)) + std::string(" ms / ") + Describe(quality.state).state;
	}

	inline std::string HudText(const NetLinkQuality& quality) {
		const std::string headline = quality.state == NetLinkQuality::State::Lost ? "reconnecting" :
		    quality.state == NetLinkQuality::State::Good ? std::to_string(quality.rttMs) + " ms" : SeatText(quality);
		return headline + '\n' + Describe(quality.state).hint;
	}

	inline const char* ToggleHint(bool enabled) {
		return enabled ? "Shows how your presses arrive in a match." : "Hidden on your HUD; Seats still shows connections.";
	}
}
