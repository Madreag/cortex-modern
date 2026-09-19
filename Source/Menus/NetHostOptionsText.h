#pragma once

#include "NetMatchConfig.h"
#include "NetLobbySnapshot.h"
#include "NetMatchService.h"

#include <string>

namespace RTE {

	inline const char* NetHostOptionsApplyText(NetMatchServiceState state) {
		return state == NetMatchServiceState::Completed ? "Options staged for the next match." : "Apply republishes this lobby.";
	}

	inline std::string NetHostSeatRemovalRefusal(NetMatchServiceState state, bool published, uint8_t peerId, const std::string& verb) {
		if (state == NetMatchServiceState::Completed) return verb + ": match completed; return to the lobby first.";
		if (!published) return verb + ": no moderation row for peer " + std::to_string(peerId) + " (" + NetMatchService::StateName(state) + ").";
		return {};
	}

	inline bool NetHostRepairEnabled(const NetMatchService& service) {
		return service.IsHost() && service.CanResyncMatch();
	}

	inline std::string NetHostRepairHint(const NetMatchService& service, bool armed = false, const std::string& refusal = {}) {
		if (!refusal.empty()) return refusal;
		bool inFlight = false;
		uint64_t bytes = 0, elapsedMs = 0;
		service.GetResyncStatus(&inFlight, &bytes, &elapsedMs);
		if (inFlight) {
			return "Repairing: " + std::string(bytes > 0 ? "transfer" : "snapshot") + " " +
			       std::to_string(bytes) + " B " + std::to_string(elapsedMs / 1000) + "s";
		}
		if (armed) return "Every peer pauses and reloads the host's snapshot - press again";
		if (bytes > 0 || elapsedMs > 0) return "Repaired: " + std::to_string(bytes) + " B " + std::to_string(elapsedMs / 1000) + "s";
		if (!service.IsHost()) return "Repair is the host's call";
		return service.CanResyncMatch() ? "Every peer reloads the host's snapshot" : "Repair needs a live match session";
	}

	inline bool NetHostRepairPress(NetMatchService& service, bool& armed, std::string& refusal) {
		refusal.clear();
		if (!NetHostRepairEnabled(service)) {
			armed = false;
			refusal = NetHostRepairHint(service);
			return false;
		}
		if (!armed) {
			armed = true;
			return true;
		}
		armed = false;
		std::string error;
		if (!service.ResyncMatch(&error)) {
			refusal = "Repair refused - " + error;
			return false;
		}
		return true;
	}

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
		line("Frame redundancy: " + std::to_string(config.frameRedundancyTicks) + " ticks");
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
