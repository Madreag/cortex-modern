#include "NetMatchConfig.h"

#include "NetIdentity.h"
#include "SettingsMan.h"

#include "nlohmann/json.hpp"

#include <algorithm>

namespace RTE {

	namespace {
		using json = nlohmann::json;

		// The saved preference and the wire policy are separate enumerations whose values differ by
		// one, so they are mapped, never cast.
		NetMatchDelayPolicy DelayPolicyFromSetting(SettingsMan::NetworkHostDelayPolicy saved) {
			switch (saved) {
				case SettingsMan::NetworkHostDelayPolicy::Fixed:
					return NetMatchDelayPolicy::Fixed;
				case SettingsMan::NetworkHostDelayPolicy::Auto:
					return NetMatchDelayPolicy::Auto;
			}
			return NetMatchDelayPolicy::Auto;
		}

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

		// A display name is the one field a player types freely, so it is also held to valid UTF-8.
		bool ValidateName(const std::string& value, const char* field, std::string* error) {
			if (!ValidateText(value, NetMatchConfigUtil::c_MaxNameBytes, field, error)) {
				return false;
			}
			if (!NetProtocol::IsValidUtf8(value)) {
				if (error) *error = std::string(field) + " is not valid UTF-8";
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

		json RulesJson(const NetMatchConfig& config) {
			json teams = json::array();
			for (const auto& team : config.teamRules) {
				teams.push_back({{"technology_intent", team.technologyIntent}, {"technology_module", team.technologyModule}, {"ai_skill", team.aiSkill}});
			}
			return {{"round_id", config.roundId}, {"config_revision", config.configRevision},
			        {"activity_module", config.activityModule}, {"scene_module", config.sceneModule},
			        {"difficulty", config.difficulty}, {"starting_gold", config.startingGold},
			        {"fog_of_war", config.fogOfWar}, {"require_clear_path_to_orbit", config.requireClearPathToOrbit},
			        {"deploy_units", config.deployUnits}, {"brainless_humans_spectate", config.brainlessHumansSpectate},
			        {"teams", std::move(teams)},
			        {"autosave_enabled", config.autosaveEnabled}, {"autosave_interval_seconds", config.autosaveIntervalSeconds},
			        {"idle_wait_minutes", config.idleWaitMinutes}, {"automatic_repair", config.automaticRepair},
			        {"path_horizon_ticks", config.pathHorizonTicks},
			        {"delay_policy", static_cast<uint8_t>(config.delayPolicy)},
			        {"frame_redundancy_ticks", config.frameRedundancyTicks}};
		}

		std::vector<std::pair<std::string, std::string>> RuleFields(const NetMatchConfig& config) {
			std::vector<std::pair<std::string, std::string>> fields = {
				{"round_id", std::to_string(config.roundId)}, {"config_revision", std::to_string(config.configRevision)},
				{"activity_module", config.activityModule}, {"scene_module", config.sceneModule},
				{"difficulty", std::to_string(config.difficulty)}, {"starting_gold", std::to_string(config.startingGold)},
				{"fog_of_war", BoolText(config.fogOfWar)}, {"require_clear_path_to_orbit", BoolText(config.requireClearPathToOrbit)},
				{"deploy_units", BoolText(config.deployUnits)},
			};
			// The spectate rule reached the wire in v4, so an older config hashes without its field.
			if (config.version >= 4) {
				fields.emplace_back("brainless_humans_spectate", BoolText(config.brainlessHumansSpectate));
			}
			const std::vector<std::pair<std::string, std::string>> tail = {
				{"autosave_enabled", BoolText(config.autosaveEnabled)},
				{"autosave_interval_seconds", std::to_string(config.autosaveIntervalSeconds)},
				{"idle_wait_minutes", std::to_string(config.idleWaitMinutes)}, {"automatic_repair", BoolText(config.automaticRepair)},
				{"delay_policy", std::to_string(static_cast<uint8_t>(config.delayPolicy))},
			};
			fields.insert(fields.end(), tail.begin(), tail.end());
			if (config.pathHorizonTicks != 0) {
				fields.emplace_back("path_horizon_ticks", std::to_string(config.pathHorizonTicks));
			}
			for (size_t i = 0; i < config.teamRules.size(); ++i) {
				const std::string prefix = "team." + std::to_string(i) + ".";
				fields.emplace_back(prefix + "technology_intent", config.teamRules[i].technologyIntent);
				fields.emplace_back(prefix + "technology_module", config.teamRules[i].technologyModule);
				fields.emplace_back(prefix + "ai_skill", std::to_string(config.teamRules[i].aiSkill));
			}
			return fields;
		}

		bool ValidateModule(const std::string& module, const char* field, std::string* error) {
			if (!ValidateText(module, NetMatchConfigUtil::c_MaxPresetBytes, field, error)) return false;
			if (module.size() <= 4 || !module.ends_with(".rte") || !NetProtocol::IsValidUtf8(module) || module.find_first_of("/\\:*?\"<>|") != std::string::npos) {
				if (error) *error = std::string(field) + " must be a UTF-8 module name ending in .rte";
				return false;
			}
			return true;
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

	void NetMatchConfigUtil::ApplySavedHostOptions(NetMatchConfig& config) {
		config.delayPolicy = DelayPolicyFromSetting(g_SettingsMan.GetNetworkHostDelayPolicy());
		config.idleWaitMinutes = static_cast<uint8_t>(std::clamp(g_SettingsMan.GetNetworkHostIdleWaitMinutes(), 0, 60));
		config.automaticRepair = g_SettingsMan.GetNetworkHostAutoRepair();
		config.pathHorizonTicks = static_cast<uint16_t>(g_SettingsMan.GetNetworkPathHorizonTicks());
	}

	bool NetMatchConfigUtil::DeriveRematchConfig(const NetMatchConfig& previous, const std::vector<uint8_t>& survivingPeerIds, NetMatchConfig& outConfig, std::map<uint8_t, uint8_t>* outSeatMap, std::string* error) {
		std::vector<uint8_t> survivors = survivingPeerIds;
		std::sort(survivors.begin(), survivors.end());
		survivors.erase(std::unique(survivors.begin(), survivors.end()), survivors.end());
		if (previous.persistentWorld) {
			if (error) *error = "a persistent world does not rematch";
			return false;
		}
		if (survivors.empty() || survivors.size() > previous.peerCount) {
			if (error) *error = "surviving peer count is out of range";
			return false;
		}
		for (uint8_t peerId : survivors) {
			if (peerId == 0 || peerId > previous.peerCount) {
				if (error) *error = "a surviving peer id is outside the match config";
				return false;
			}
		}
		if (!std::binary_search(survivors.begin(), survivors.end(), previous.hostPeerId)) {
			if (error) *error = "the host is not among the surviving peers";
			return false;
		}
		std::map<uint8_t, uint8_t> seatMap;
		for (size_t index = 0; index < survivors.size(); ++index) {
			seatMap[survivors[index]] = static_cast<uint8_t>(index + 1);
		}
		NetMatchConfig config = previous;
		config.peerCount = static_cast<uint8_t>(survivors.size());
		config.hostPeerId = seatMap.at(previous.hostPeerId);
		config.players.clear();
		for (const NetMatchPlayerSlot& slot : previous.players) {
			NetMatchPlayerSlot seat = slot;
			if (!slot.cpu) {
				const auto moved = seatMap.find(slot.peerId);
				if (moved == seatMap.end()) {
					continue;
				}
				seat.peerId = moved->second;
			}
			config.players.push_back(seat);
		}
		if (!previous.peerInputDelayFrames.empty()) {
			std::vector<uint16_t> delays(config.peerCount, previous.inputDelayFrames);
			for (const auto& [was, now] : seatMap) {
				delays[now - 1] = PeerInputDelay(previous, was);
			}
			config.peerInputDelayFrames = std::move(delays);
		}
		if (!ValidateLocalAlpha(config, error)) {
			return false;
		}
		if (outSeatMap) *outSeatMap = std::move(seatMap);
		outConfig = std::move(config);
		return true;
	}

	bool NetMatchConfigUtil::IsWorldId(const std::string& text) {
		if (text.size() != c_WorldIdBytes) {
			return false;
		}
		for (size_t index = 0; index < text.size(); ++index) {
			const char c = text[index];
			if (index == 8 || index == 13 || index == 18 || index == 23) {
				if (c != '-') return false;
			} else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
				return false;
			}
		}
		return true;
	}

	bool NetMatchConfigUtil::ValidateLocalAlpha(const NetMatchConfig& config, std::string* error) {
		if (config.version != 2 && config.version != 3 && config.version != c_Version && config.version != c_PersistentWorldVersion) {
			if (error) *error = "match config version is unsupported";
			return false;
		}
		auto refuse = [&](const char* reason) { if (error) *error = reason; return false; };
		if (config.version == 2) {
			// A v2 config predates the rules block, so it carries the pre-rules end rule too.
			NetMatchConfig legacyDefaults;
			legacyDefaults.version = config.version;
			legacyDefaults.brainlessHumansSpectate = false;
			if (RuleFields(config) != RuleFields(legacyDefaults)) return refuse("legacy config cannot carry extended rules");
		}
		// Every pre-v4 config predates the spectate byte, so it cannot carry anything but the pre-spectate rule.
		if (config.version < 4 && config.brainlessHumansSpectate) return refuse("pre-spectate config cannot carry the spectate rule");
		// The persistent world's fields reached the wire in v5; an ordinary match stays on v4 and hashes as it always did.
		if (config.version < c_PersistentWorldVersion) {
			if (config.persistentWorld) return refuse("pre-world config cannot carry the persistent world rule");
			if (!config.worldId.empty() || config.worldBoot != 0) return refuse("pre-world config cannot carry a world identity");
		} else if (config.persistentWorld) {
			if (!IsWorldId(config.worldId)) return refuse("a persistent world needs a canonical world id");
			if (config.worldBoot == 0) return refuse("a persistent world needs a nonzero host boot incarnation");
			// The world ticks on whether or not a human is seated, so the brain rules never end it.
			if (!config.dedicated) return refuse("a persistent world is hosted by a dedicated host");
		} else if (!config.worldId.empty() || config.worldBoot != 0) {
			return refuse("an ordinary match cannot carry a world identity");
		}
		if (config.version < 3 && config.pathHorizonTicks != 0) return refuse("legacy config cannot carry a path horizon");
		if (config.pathHorizonTicks > c_MaxPathHorizonTicks) return refuse("path_horizon_ticks is out of range");
		if (config.roundId == 0 || config.configRevision == 0) return refuse("round_id and config_revision must be nonzero");
		if (config.difficulty > 100) return refuse("difficulty is out of range");
		if (config.startingGold > c_MaxFiniteStartingGold && config.startingGold != c_InfiniteGold) return refuse("starting_gold is out of range");
		if (config.autosaveEnabled && config.autosaveIntervalSeconds == 0) return refuse("enabled autosave requires a nonzero interval");
		if (config.idleWaitMinutes > 60) return refuse("idle_wait_minutes is out of range");
		if (config.frameRedundancyTicks < 1 || config.frameRedundancyTicks > c_MaxFrameRedundancyTicks) return refuse("frame_redundancy_ticks is out of range");
		if (config.delayPolicy != NetMatchDelayPolicy::Auto && config.delayPolicy != NetMatchDelayPolicy::Fixed) return refuse("delay_policy is invalid");
		if (config.mode != NetMatchMode::PvPSkirmish && config.mode != NetMatchMode::CoopPvE && config.mode != NetMatchMode::PvPvE) return refuse("match mode is invalid");
		if (!ValidateModule(config.activityModule, "activity_module", error) || !ValidateModule(config.sceneModule, "scene_module", error)) return false;
		for (const auto& team : config.teamRules) {
			if (team.aiSkill < 1 || team.aiSkill > 100) return refuse("team AI skill is out of range");
			if (team.technologyIntent == "-All-") {
				if (!team.technologyModule.empty()) return refuse("all technology must resolve to unrestricted factions");
			} else {
				if (!ValidateModule(team.technologyModule, "team technology module", error)) return false;
				if (team.technologyIntent != "-Random-" && team.technologyIntent != team.technologyModule) return refuse("team technology intent does not match its resolved module");
			}
		}
		if (config.sessionId == 0) {
			if (error) *error = "session_id must be nonzero";
			return false;
		}
		const size_t humanCount = std::count_if(config.players.begin(), config.players.end(), [](const auto& slot) { return !slot.cpu; });
		const bool soleCPUHost = config.dedicated && humanCount == 0 && !config.players.empty();
		if (config.peerCount < (soleCPUHost ? 1 : c_MinPeerCount) || config.peerCount > c_MaxPeerCount) {
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
		if ((config.version >= 3 || !config.sceneName.empty()) && !ValidateText(config.sceneName, c_MaxPresetBytes, "scene_name", error)) {
			return false;
		}
		if (config.players.empty() || config.players.size() > c_MaxPlayers) {
			if (error) *error = "player slot count is out of range";
			return false;
		}
		std::vector<bool> seen(config.peerCount + 1, false);
		bool sawHost = false;
		std::array<bool, 4> cpuTeams{}, humanTeams{};
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
			if (player.team >= 4) {
				// Engine teams are 0..3; MaxTeamCount (4) is the exclusive sentinel, so team 4 is invalid.
				if (error) *error = "player team is out of range";
				return false;
			}
			if (player.cpu) {
				if (cpuTeams[player.team]) return refuse("duplicate cpu team");
				cpuTeams[player.team] = true;
			} else {
				humanTeams[player.team] = true;
			}
			if (!ValidateName(player.displayName, "player display_name", error)) {
				return false;
			}
		}
		if (config.dedicated) {
			if (sawHost) {
				if (error) *error = "dedicated config must not seat the host peer";
				return false;
			}
		} else if (!sawHost) {
			if (error) *error = "host player slot is missing";
			return false;
		}
		if (humanCount > config.peerCount - (config.dedicated ? 1 : 0)) return refuse("human seats exceed peer capacity");
		if (config.mode == NetMatchMode::PvPvE && humanCount == 4) return refuse("four-human PvPvE exceeds team capacity");
		for (size_t team = 0; team < cpuTeams.size(); ++team) {
			if (cpuTeams[team] && humanTeams[team]) return refuse("cpu slot shares a human team");
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
			// A CPU slot has no peer id, so its team is its key: under a shared player.0 prefix the
			// canonical sort merges every CPU slot's fields and two rosters that swap their teams hash alike.
			// That key reached the hash in v4, so a config recorded before it keeps its peer-id key.
			const std::string prefix = (player.cpu && config.version >= 4) ? ("player.cpu" + std::to_string(player.team) + ".")
			                                                              : ("player." + std::to_string(player.peerId) + ".");
			fields.emplace_back(prefix + "team", std::to_string(player.team));
			fields.emplace_back(prefix + "cpu", BoolText(player.cpu));
			fields.emplace_back(prefix + "display_name", player.displayName);
		}
		// Only the true case rides the hash, so every pre-existing config keeps its value.
		if (config.dedicated) {
			fields.emplace_back("dedicated", "true");
		}
		// Same for the redundancy window: only a host's non-default choice rides it.
		if (config.frameRedundancyTicks != c_DefaultFrameRedundancyTicks) {
			fields.emplace_back("frame_redundancy_ticks", std::to_string(config.frameRedundancyTicks));
		// The world identity is frozen for the world's life: its members' rosters change under a
		// separate revision, so what a joiner validates against stays the same string every boot. Only
		// a world takes the v5 domain, so every ordinary roster hashes as it always did.
		if (config.persistentWorld) {
			fields.emplace_back("persistent_world", "true");
			fields.emplace_back("world_id", config.worldId);
		}
		if (config.version >= 3) {
			const auto rules = RuleFields(config);
			fields.insert(fields.end(), rules.begin(), rules.end());
		}
		const char* domain = config.persistentWorld ? "NetMatchConfig/v5"
		                                            : (config.version >= 4 ? "NetMatchConfig/v4" : (config.version >= 3 ? "NetMatchConfig/v3" : "NetMatchConfig/v2"));
		return NetIdentity::HashCanonicalText(domain, fields);
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
			{"persistent_world", config.persistentWorld},
			{"world_id", config.worldId},
			{"world_boot", config.worldBoot},
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
		report["rules"] = RulesJson(config);
		// The roster takes a session-handshake name without revalidating it, so a stray byte is replaced
		// here instead of throwing: the host builds this report while a match runs.
		return report.dump(-1, ' ', false, json::error_handler_t::replace);
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

	const char* NetMatchConfigUtil::ModeLabel(NetMatchMode mode) {
		switch (mode) {
			case NetMatchMode::PvPSkirmish: return "PvP";
			case NetMatchMode::CoopPvE: return "Co-op PvE";
			case NetMatchMode::PvPvE: return "PvPvE";
		}
		return "Unknown";
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
