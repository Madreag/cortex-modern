#include "NetMatchConfig.h"

#include "NetIdentity.h"

#include "nlohmann/json.hpp"

#include <algorithm>

namespace RTE {

	namespace {
		using json = nlohmann::json;

		bool HasControlChars(const std::string& value) {
			return std::any_of(value.begin(), value.end(), [](unsigned char c) {
				return c < 0x20U || c == 0x7FU;
			});
		}

		bool ValidateText(const std::string& value, size_t maxBytes, const char* field, std::string* error) {
			if (value.empty()) {
				if (error) *error = std::string(field) + " must not be empty";
				return false;
			}
			if (value.size() > maxBytes) {
				if (error) *error = std::string(field) + " is too long";
				return false;
			}
			if (HasControlChars(value)) {
				if (error) *error = std::string(field) + " contains control characters";
				return false;
			}
			return true;
		}

		std::vector<NetMatchPlayerSlot> SortedPlayers(const std::vector<NetMatchPlayerSlot>& players) {
			std::vector<NetMatchPlayerSlot> sorted = players;
			std::sort(sorted.begin(), sorted.end(), [](const NetMatchPlayerSlot& lhs, const NetMatchPlayerSlot& rhs) {
				if (lhs.peerId != rhs.peerId) return lhs.peerId < rhs.peerId;
				if (lhs.team != rhs.team) return lhs.team < rhs.team;
				if (lhs.cpu != rhs.cpu) return lhs.cpu < rhs.cpu;
				return lhs.displayName < rhs.displayName;
			});
			return sorted;
		}

		std::string BoolText(bool value) {
			return value ? "1" : "0";
		}

		json PlayerJson(const NetMatchPlayerSlot& player) {
			return {
				{"peer_id", static_cast<int>(player.peerId)},
				{"team", static_cast<int>(player.team)},
				{"cpu", player.cpu},
				{"display_name", player.displayName},
			};
		}
	}

	NetMatchConfig NetMatchConfigUtil::MakeDefault(uint64_t sessionId) {
		NetMatchConfig config;
		config.sessionId = sessionId;
		config.players = {
			NetMatchPlayerSlot{1, 0, false, "Host"},
			NetMatchPlayerSlot{2, 1, false, "Client"},
		};
		return config;
	}

	bool NetMatchConfigUtil::ValidateLocalAlpha(const NetMatchConfig& config, std::string* error) {
		if (config.version != c_Version) {
			if (error) *error = "match config version is unsupported";
			return false;
		}
		if (config.sessionId == 0) {
			if (error) *error = "session_id must be nonzero";
			return false;
		}
		if (config.peerCount < c_MinPeerCount || config.peerCount > c_MaxPeerCount) {
			if (error) *error = "peer_count is out of range";
			return false;
		}
		if (config.hostPeerId == 0 || config.hostPeerId > config.peerCount) {
			if (error) *error = "host_peer_id is out of range";
			return false;
		}
		if (config.inputDelayFrames > c_MaxInputDelayFrames) {
			if (error) *error = "input_delay_frames is out of range";
			return false;
		}
		if (!config.peerInputDelayFrames.empty()) {
			if (config.peerInputDelayFrames.size() != config.peerCount) {
				if (error) *error = "peer_input_delays must cover every peer";
				return false;
			}
			for (uint16_t delay : config.peerInputDelayFrames) {
				// The uniform value is the manual floor; a per-peer pick may only raise it.
				if (delay > c_MaxInputDelayFrames || delay < config.inputDelayFrames) {
					if (error) *error = "peer_input_delay is out of range";
					return false;
				}
			}
		}
		if (!ValidateText(config.activityType, c_MaxPresetBytes, "activity_type", error) ||
		    !ValidateText(config.activityPreset, c_MaxPresetBytes, "activity_preset", error) ||
		    !ValidateText(config.modePreset, c_MaxPresetBytes, "mode_preset", error)) {
			return false;
		}
		if (!config.sceneName.empty() && !ValidateText(config.sceneName, c_MaxPresetBytes, "scene_name", error)) {
			return false;
		}
		if (config.players.empty() || config.players.size() > c_MaxPlayers) {
			if (error) *error = "player slot count is out of range";
			return false;
		}
		std::vector<bool> seen(config.peerCount + 1, false);
		bool sawHost = false;
		bool sawRemoteHuman = false;
		for (const NetMatchPlayerSlot& player : config.players) {
			// A CPU slot has no peer: it marks a machine-run team the host's AI drives over the wire.
			if (player.cpu) {
				if (player.peerId != 0) {
					if (error) *error = "cpu slot must not claim a peer";
					return false;
				}
			} else if (player.peerId == 0 || player.peerId > config.peerCount) {
				if (error) *error = "player peer_id is out of range";
				return false;
			}
			if (player.peerId != 0) {
				if (seen[player.peerId]) {
					if (error) *error = "duplicate player peer_id";
					return false;
				}
				seen[player.peerId] = true;
			}
			sawHost = sawHost || player.peerId == config.hostPeerId;
			sawRemoteHuman = sawRemoteHuman || (!player.cpu && player.peerId >= 2);
			if (player.team >= 4) {
				// Engine teams are 0..3; MaxTeamCount (4) is the exclusive sentinel, so team 4 is invalid.
				if (error) *error = "player team is out of range";
				return false;
			}
			if (!ValidateText(player.displayName, c_MaxNameBytes, "player display_name", error)) {
				return false;
			}
		}
		if (config.dedicated) {
			if (sawHost) {
				if (error) *error = "dedicated config must not seat the host peer";
				return false;
			}
			if (!sawRemoteHuman) {
				if (error) *error = "dedicated config has no client player slot";
				return false;
			}
		} else if (!sawHost) {
			if (error) *error = "host player slot is missing";
			return false;
		}
		return true;
	}

	NetHash32 NetMatchConfigUtil::HashConfig(const NetMatchConfig& config) {
		std::vector<std::pair<std::string, std::string>> fields = {
			{"version", std::to_string(config.version)},
			{"session_id", std::to_string(config.sessionId)},
			{"host_peer_id", std::to_string(config.hostPeerId)},
			{"peer_count", std::to_string(config.peerCount)},
			{"input_delay_frames", std::to_string(config.inputDelayFrames)},
			{"peer_input_delays", [&] {
				std::string csv;
				for (size_t i = 0; i < config.peerInputDelayFrames.size(); ++i) {
					csv += (i == 0 ? "" : ",") + std::to_string(config.peerInputDelayFrames[i]);
				}
				return csv;
			}()},
			{"mode", ModeName(config.mode)},
			{"ownership_policy", OwnershipPolicyName(config.ownershipPolicy)},
			{"activity_type", config.activityType},
			{"activity_preset", config.activityPreset},
			{"scene_name", config.sceneName},
			{"mode_preset", config.modePreset},
		};
		for (const NetMatchPlayerSlot& player : SortedPlayers(config.players)) {
			const std::string prefix = "player." + std::to_string(player.peerId) + ".";
			fields.emplace_back(prefix + "team", std::to_string(player.team));
			fields.emplace_back(prefix + "cpu", BoolText(player.cpu));
			fields.emplace_back(prefix + "display_name", player.displayName);
		}
		// Only the true case rides the hash, so every pre-existing config keeps its value.
		if (config.dedicated) {
			fields.emplace_back("dedicated", "true");
		}
		return NetIdentity::HashCanonicalText("NetMatchConfig/v2", fields);
	}

	std::string NetMatchConfigUtil::BuildReportJson(const NetMatchConfig& config) {
		json players = json::array();
		for (const NetMatchPlayerSlot& player : SortedPlayers(config.players)) {
			players.push_back(PlayerJson(player));
		}
		json report = {
			{"version", config.version},
			{"session_id", config.sessionId},
			{"host_peer_id", static_cast<int>(config.hostPeerId)},
			{"dedicated", config.dedicated},
			{"peer_count", static_cast<int>(config.peerCount)},
			{"input_delay_frames", config.inputDelayFrames},
			{"peer_input_delays", config.peerInputDelayFrames},
			{"mode", ModeName(config.mode)},
			{"ownership_policy", OwnershipPolicyName(config.ownershipPolicy)},
			{"activity_type", config.activityType},
			{"activity_preset", config.activityPreset},
			{"scene_name", config.sceneName},
			{"mode_preset", config.modePreset},
			{"match_config_hash", NetIdentity::HashHex(HashConfig(config))},
			{"players", std::move(players)},
		};
		return report.dump();
	}

	uint16_t NetMatchConfigUtil::PeerInputDelay(const NetMatchConfig& config, uint8_t peerId) {
		if (peerId == 0 || peerId > config.peerInputDelayFrames.size()) {
			return config.inputDelayFrames;
		}
		return config.peerInputDelayFrames[peerId - 1];
	}

	const char* NetMatchConfigUtil::ModeName(NetMatchMode mode) {
		switch (mode) {
			case NetMatchMode::PvPSkirmish: return "pvp-skirmish";
			case NetMatchMode::CoopPvE: return "coop-pve";
			case NetMatchMode::PvPvE: return "pvpve";
		}
		return "unknown";
	}

	bool NetMatchConfigUtil::ParseMode(const std::string& text, NetMatchMode& outMode) {
		if (text == "pvp" || text == "pvp-skirmish") {
			outMode = NetMatchMode::PvPSkirmish;
			return true;
		}
		if (text == "coop-pve" || text == "pve") {
			outMode = NetMatchMode::CoopPvE;
			return true;
		}
		if (text == "pvpve") {
			outMode = NetMatchMode::PvPvE;
			return true;
		}
		return false;
	}

	const char* NetMatchConfigUtil::OwnershipPolicyName(NetActorOwnershipPolicy policy) {
		switch (policy) {
			case NetActorOwnershipPolicy::UniqueIdModPeerCount: return "unique-id-mod-peer-count";
			case NetActorOwnershipPolicy::TeamOwner: return "team-owner";
			case NetActorOwnershipPolicy::HostCpuRemoteHuman: return "host-cpu-remote-human";
		}
		return "unknown";
	}

	bool NetMatchConfigUtil::ParseOwnershipPolicy(const std::string& text, NetActorOwnershipPolicy& outPolicy) {
		if (text == "unique-id-mod-peer-count") {
			outPolicy = NetActorOwnershipPolicy::UniqueIdModPeerCount;
			return true;
		}
		if (text == "team-owner") {
			outPolicy = NetActorOwnershipPolicy::TeamOwner;
			return true;
		}
		if (text == "host-cpu-remote-human") {
			outPolicy = NetActorOwnershipPolicy::HostCpuRemoteHuman;
			return true;
		}
		return false;
	}

} // namespace RTE
