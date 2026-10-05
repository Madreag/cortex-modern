#pragma once

#include "NetMatchConfig.h"
#include "NetLobbySnapshot.h"
#include "NetMatchService.h"
#include "SettingsMan.h"

#include <cmath>
#include <string>

namespace RTE {

	inline const char* NetHostNatTraversalState(bool enabled) {
		return enabled ? "Automatic" : "Off (LAN or port-forwarded only)";
	}

	/// The slow-player policy's words, shared by the Network page's combo and the read-only summary.
	inline const char* NetSlowPlayerPolicyText(NetSlowPlayerPolicy policy) {
		return policy == NetSlowPlayerPolicy::Pause ? "Pause for them (up to 20 s)" : "Give the seat to the AI (host too) until they catch up";
	}

	/// What happens to a player whose input is late, under the policy and bound the host picked.
	inline std::string NetSlowPlayerHint(NetSlowPlayerPolicy policy, uint16_t boundTicks, double tickMs) {
		if (policy == NetSlowPlayerPolicy::Pause) return "Everyone waits for a late player, host included, for up to 20 s.";
		return "A player late past " + std::to_string(boundTicks) + (boundTicks == 1 ? " tick (" : " ticks (") + std::to_string(std::lround(boundTicks * tickMs)) +
		       " ms), host too, is held to the AI while others play on. Other policies return in a later version.";
	}

	/// How the automatic input delay is sized, in the words every delay readout uses.
	inline std::string NetAutoDelayText(uint16_t marginTicks) {
		return "ping plus a " + std::to_string(marginTicks) + "-tick margin, raised live if inputs arrive late";
	}

	/// The autosave interval row's range.
	inline const char* NetAutosaveRangeHint() {
		return "Every 60 s to 60 min, or off (default)";
	}

	/// What an autosave can cost the other players.
	inline const char* NetAutosaveCostHint() {
		return "Saving may cause a brief pause for other players on slower hosts";
	}

	/// What a match checkpoint is to the players.
	inline const char* NetAutosaveNote() {
		return "Every player takes each checkpoint at the same tick; a player who rejoins starts from one.";
	}

	/// The return window row's consequence.
	inline const char* NetReturnWindowHint() {
		return "A return within the window resumes from the player's held state, a later one loads an image. A longer window keeps more history on the host.";
	}

	/// One return window choice, as the row and the summary name it.
	inline std::string NetReturnWindowText(uint8_t minutes) {
		return std::to_string(minutes) + (minutes == 1 ? " minute" : " minutes");
	}

	/// The world history row's consequence.
	inline const char* NetJoinHistoryHint() {
		return "History this host keeps for joiners and returns, never shorter than the return window. Longer lets slow joiners catch up but uses more disk.";
	}

	/// The catch-up limit row's consequence.
	inline const char* NetJoinLagHint() {
		return "A watcher or returning player who trails the round past this limit without gaining on it starts over from a fresh image.";
	}

	/// One world history or catch-up limit choice, as its row names it.
	inline std::string NetJoinHistoryText(int seconds) {
		if (seconds % 60 != 0) return std::to_string(seconds) + (seconds == 1 ? " second" : " seconds");
		return std::to_string(seconds / 60) + (seconds == 60 ? " minute" : " minutes");
	}

	/// Settings > Network > Connection's hint for each route choice.
	inline const char* NetConnectionModeHint(SettingsMan::NetworkConnectionMode mode) {
		switch (mode) {
			case SettingsMan::NetworkConnectionMode::DirectOnly: return "Direct only: lowest latency; fails when routers block a direct route.";
			case SettingsMan::NetworkConnectionMode::RelayOnly: return "Relay only: every packet uses the relay and adds its round trip.";
			default: return "Direct first: lowest latency; relay adds a round trip if direct fails.";
		}
	}

	inline std::string NetHostNatModeText(const SettingsMan& settings) {
		if (!settings.GetNetworkIceEnableSetting()) return "Port forwarding required";
		if (settings.GetNetworkHostRelayMode() != SettingsMan::NetworkHostRelayMode::Off) return "NAT: STUN + relay";
		return settings.GetNetworkStunServersSetting().empty() ? "Port forwarding required" : "NAT: STUN";
	}

	/// The relay row's hint for the drafted choice: what it does, then what the choice needs.
	inline std::string NetHostRelayHint(const SettingsMan& settings, SettingsMan::NetworkHostRelayMode mode, bool directOn) {
		if (settings.HasNetworkTurnServersOverride()) return "A command-line TURN override applies to this run; this row saves your hosting preference.\nDirect is lowest latency; a relay adds its round trip.";
		if (!directOn) return "Automatic direct connection is off, so this match offers no relay. Turn it on above to offer one.";
		switch (mode) {
			case SettingsMan::NetworkHostRelayMode::Off: return "No relay: a player who cannot connect directly cannot join.\nDirect connections have the lowest latency; some routers need port forwarding.";
			case SettingsMan::NetworkHostRelayMode::Fixed: return "If a direct connection fails, your own relay carries it, adding its round trip.\nAddress: host:port or TURN URLs. Use a login, never a signing secret. Players may pick Direct only.";
			default: return "If a direct connection fails, the game service relays it, adding its round trip.\nUDP relays only in this build; the game service's login renews itself while the session runs.";
		}
	}

	/// The direct-connection row's hint for the drafted choice, then what this computer's settings and the live session mean for it.
	inline std::string NetHostNatTraversalHint(const SettingsMan& settings, bool directOn, bool setup, bool readOnly, const std::string& route) {
		// Two lines fit the row: with no STUN server to ask, what the choice does is reach this network only.
		std::string text = !directOn ? "Players reach you only at your public address and port; many home networks cannot.\n"
		                 : settings.GetNetworkStunServersSetting().empty() ? "The STUN server list is empty, so only players on your network connect directly (Settings - Network - Connection).\n"
		                 : "Tries a direct connection through each player's router first. Recommended.\n";
		if (readOnly) return text + "This is your saved preference; only the host sets up this match.";
		if (settings.GetNetworkIceEnable() != settings.GetNetworkIceEnableSetting() || settings.GetNetworkStunServers() != settings.GetNetworkStunServersSetting()) {
			return text + "Command-line ICE/STUN overrides apply to this run; this row saves your preference.";
		}
		if (!setup) {
			if (route == "ip") return text + "This lobby connects by address: players need your UDP port forwarded, or the same network.";
			if (route == "ice") return text + "This lobby connects players directly. Close it to change this.";
			if (!settings.GetNetworkIceEnableSetting()) return text + "Direct connection is off for this lobby. Close it to change this.";
		}
		return text + (settings.GetSessionDirectoryUrl().empty() ? "A direct connection over the internet needs the online game list service (Settings - Network - Internet)."
		                                                          : !setup ? "The lobby is still setting up its connection." : "Applied when you create the lobby.");
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
			// A receiver has no byte count of its own yet, so it names the transfer instead of reading zero.
			if (bytes == 0) return "Repairing: receiving the host's snapshot " + std::to_string(elapsedMs / 1000) + "s";
			return "Repairing: transfer " + std::to_string(bytes) + " B " + std::to_string(elapsedMs / 1000) + "s";
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
		// Short rows share a line, so the summary fits the smallest panel that shows it.
		line("Difficulty: " + std::to_string(config.difficulty) + "   Starting gold: " +
		     (config.startingGold >= NetMatchConfigUtil::c_InfiniteGold ? std::string("Infinite") : std::to_string(config.startingGold) + " oz"));
		line(std::string("Fog of war: ") + (config.fogOfWar ? "on" : "off") +
		     "   Clear path to orbit: " + (config.requireClearPathToOrbit ? "on" : "off") +
		     "   Deploy units: " + (config.deployUnits ? "on" : "off"));
		// L33's row, in the same words the Rules page's combo uses.
		line(std::string("When every human brain is lost: ") +
		     (config.brainlessHumansSpectate ? "Keep playing, humans spectate" : "End the match"));
		// The live figure drops the service's own row name, which this line already carries.
		std::string live = snapshot.inputDelayText;
		if (live.starts_with("Input delay: ")) live.erase(0, 13);
		line(std::string("Input delay: ") +
		     (config.delayPolicy == NetMatchDelayPolicy::Fixed
		          ? "Fixed " + std::to_string(config.inputDelayFrames) + " ticks"
		          : "Automatic, " + NetAutoDelayText(config.slowPlayerBoundTicks) + (live.empty() ? "" : " - now " + live)));
		line("Frame redundancy: " + std::to_string(config.frameRedundancyTicks) + " ticks   Slow player bound: " +
		     std::to_string(config.slowPlayerBoundTicks) + " ticks");
		line(std::string("When a player falls behind: ") + NetSlowPlayerPolicyText(config.slowPlayerPolicy));
		line(config.autosaveEnabled
		         ? "Autosaves: every " + std::to_string(config.autosaveIntervalSeconds) + " sim seconds"
		         : "Autosaves: off");
		line("Return window: " + NetReturnWindowText(config.returnWindowMinutes));
		line(std::string("Idle wait: ") +
		     (config.idleWaitMinutes == 0 ? "never" : std::to_string(config.idleWaitMinutes) + " minutes") +
		     "   Automatic repair: " + (config.automaticRepair ? "on" : "off"));
		if (!snapshot.lobbyPhase.empty()) {
			line("Phase: " + snapshot.lobbyPhase + (snapshot.statusText.empty() ? "" : " - " + snapshot.statusText));
		}
		return text;
	}

} // namespace RTE
