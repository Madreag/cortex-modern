#pragma once

#include "NetMatchConfig.h"
#include "NetLobbySnapshot.h"

#include <string>

namespace RTE {

	/// The match's adopted host options as read-only lines - the one panel the lobby's Details, the
	/// pause menu's Match Options and the F6 seats panel's Options view all show mid-match. It reads
	/// the same NetMatchConfig the lobby's editable pages draft from, so every origin reports the
	/// identical round.
	inline std::string NetHostOptionsSummary(const NetMatchConfig& config, const NetLobbySnapshot& snapshot) {
		std::string text;
		auto line = [&text](const std::string& row) { text += (text.empty() ? "" : "\n") + row; };
		line("Activity: " + config.activityPreset + (config.activityModule.empty() ? "" : "  (" + config.activityModule + ")"));
		line("Site: " + config.sceneName + (config.sceneModule.empty() ? "" : "  (" + config.sceneModule + ")"));
		line("Mode: " + std::string(NetMatchConfigUtil::ModeLabel(config.mode)));
		int humans = 0, cpus = 0;
		for (const NetMatchPlayerSlot& slot : config.players) {
			(slot.cpu ? cpus : humans)++;
		}
		line("Seats: " + std::to_string(humans) + " human, " + std::to_string(cpus) + " CPU of " +
		     std::to_string(config.peerCount) + " peers");
		line("Difficulty: " + std::to_string(config.difficulty));
		line("Starting gold: " + (config.startingGold >= NetMatchConfigUtil::c_InfiniteGold
		                              ? std::string("Infinite")
		                              : std::to_string(config.startingGold) + " oz"));
		line(std::string("Fog of war: ") + (config.fogOfWar ? "on" : "off") +
		     "   Clear path to orbit: " + (config.requireClearPathToOrbit ? "on" : "off") +
		     "   Deploy units: " + (config.deployUnits ? "on" : "off"));
		// L33's row, in the same words the Rules page's combo uses.
		line(std::string("When every human brain is lost: ") +
		     (config.brainlessHumansSpectate ? "Keep playing, humans spectate" : "End the match"));
		line(std::string("Input delay: ") +
		     (config.delayPolicy == NetMatchDelayPolicy::Fixed
		          ? "Fixed " + std::to_string(config.inputDelayFrames) + " ticks"
		          : "Automatic (" + snapshot.inputDelayText + ")"));
		line(config.autosaveEnabled
		         ? "Autosaves: every " + std::to_string(config.autosaveIntervalSeconds) + " sim seconds"
		         : "Autosaves: off");
		line(std::string("Idle wait: ") +
		     (config.idleWaitMinutes == 0 ? "never" : std::to_string(config.idleWaitMinutes) + " minutes") +
		     "   Automatic repair: " + (config.automaticRepair ? "on" : "off"));
		if (!snapshot.lobbyPhase.empty()) {
			line("Phase: " + snapshot.lobbyPhase + (snapshot.statusText.empty() ? "" : " - " + snapshot.statusText));
		}
		return text;
	}

} // namespace RTE
