#pragma once

#include "NetMatchConfig.h"
#include "NetLobbySnapshot.h"
#include "NetMatchService.h"
#include "SettingsMan.h"

#include <string>

namespace RTE {

	inline const char* NetHostNatTraversalState(bool enabled) {
		return enabled ? "Automatic" : "Off (LAN or port-forwarded only)";
	}

	inline std::string NetHostNatModeText(const SettingsMan& settings) {
		if (!settings.GetNetworkIceEnableSetting()) return "Port forwarding required";
		if (settings.GetNetworkHostRelayMode() != SettingsMan::NetworkHostRelayMode::Off) return "NAT: STUN + relay";
		return settings.GetNetworkStunServersSetting().empty() ? "Port forwarding required" : "NAT: STUN";
	}

	inline std::string NetHostRelayHint(const SettingsMan& settings) {
		if (!settings.GetNetworkIceEnableSetting()) return "NAT traversal is Off, so this match offers no relay. Enable Automatic above to offer one.";
		switch (settings.GetNetworkHostRelayMode()) {
			case SettingsMan::NetworkHostRelayMode::Off: return "Off: direct connections have the lowest latency. Some routers need port forwarding.";
			case SettingsMan::NetworkHostRelayMode::Fixed: return "Offer this private relay when direct fails; it adds the relay's round trip.\nAddress: host:port or comma-separated TURN URLs. Enter a login, never a signing secret.\nEach player can choose Direct only or their own relay in Settings > Network > Connection.";
			default: return "The directory supplies a short-lived relay login; direct first has the lowest latency.\nRelay adds its round trip. A directory without relay credentials leaves direct only.\nUDP TURN only in this build; TCP/TLS and live credential renewal are unavailable.";
		}
	}

	inline std::string NetHostNatTraversalHint(const SettingsMan& settings, bool setup, bool readOnly, const std::string& route) {
		std::string text = "Players behind home routers connect directly. Off means they need your port forwarded.\n";
		if (!settings.GetNetworkIceEnableSetting()) {
			text += "Off: use LAN or forward the host's UDP port.";
		} else if (settings.GetNetworkStunServersSetting().empty()) {
			text += "STUN list empty: direct candidates are LAN-only. Edit Settings > Network > Connection.";
		} else {
			text += "STUN finds direct routes. The Relay row offers a fallback for stricter routers.";
		}
		text += "\n";
		if (readOnly) return text + "This is your saved preference; only the host sets up this session.";
		if (settings.GetNetworkIceEnable() != settings.GetNetworkIceEnableSetting() || settings.GetNetworkStunServers() != settings.GetNetworkStunServersSetting()) {
			return text + "Command-line ICE/STUN overrides apply to this run; this row saves your preference.";
		}
		if (!setup) {
			if (route == "ip") return text + "Current session uses direct IP: forward the host's UDP port or use LAN.";
			if (route == "ice") return text + "This session is using this preference. End it to change the setting.";
			if (!settings.GetNetworkIceEnableSetting()) return text + "NAT traversal is Off for this session. End it to change the setting.";
		}
		return text + (settings.GetSessionDirectoryUrl().empty()
		                   ? "Internet NAT traversal needs a session directory URL in Network settings."
		                   : !setup ? "No ICE listener is active yet; the lobby is still setting up."
		                            : "Applied when you create the lobby; direct address joins use the host's UDP port.");
	}

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
		if (!service.RequestHostRepair(&error)) {
			refusal = error.empty() ? "Repair refused" : "Repair refused - " + error;
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
