#pragma once

#include "NetProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace RTE {

	enum class NetMatchMode : uint8_t {
		PvPSkirmish = 1,
		CoopPvE = 2,
		PvPvE = 3,
	};

	enum class NetActorOwnershipPolicy : uint8_t {
		UniqueIdModPeerCount = 1,
		TeamOwner = 2,
		HostCpuRemoteHuman = 3,
	};

	struct NetMatchPlayerSlot {
		uint8_t peerId = 0;
		uint8_t team = 0;
		bool cpu = false;
		std::string displayName;

		bool operator==(const NetMatchPlayerSlot&) const = default;
	};

	enum class NetMatchDelayPolicy : uint8_t { Auto = 1, Fixed = 2 };

	struct NetMatchTeamRules {
		std::string technologyIntent = "-All-";
		std::string technologyModule; // Empty resolves -All- to unrestricted factions.
		uint8_t aiSkill = 50;
		bool operator==(const NetMatchTeamRules&) const = default;
	};

	struct NetMatchStandardRules {
		NetMatchMode mode = NetMatchMode::PvPSkirmish;
		std::string activityModule = "Base.rte";
		std::string activityType = "GAScripted";
		std::string activityPreset = "P4 Alpha Duel";
		std::string sceneModule = "Base.rte";
		std::string sceneName = "Grasslands";
		uint8_t difficulty = 50;
		uint32_t startingGold = 0;
		bool fogOfWar = false;
		bool requireClearPathToOrbit = false;
		bool deployUnits = false;
		bool brainlessHumansSpectate = true; // Losing every human brain leaves the humans watching instead of ending the round.
		std::array<NetMatchTeamRules, 4> teamRules;
		bool operator==(const NetMatchStandardRules&) const = default;
	};

	// Inherited rules retain the existing activity/mode member names without duplicate values.
	struct NetMatchConfig : NetMatchStandardRules {
		uint16_t version = 4;
		uint64_t sessionId = 0;
		uint64_t roundId = 1;
		uint64_t configRevision = 1;
		uint8_t hostPeerId = 1;
		bool dedicated = false; // The host keeps lockstep peer hostPeerId but seats no human slot there.
		uint8_t peerCount = 2;
		uint16_t inputDelayFrames = 0;
		std::vector<uint16_t> peerInputDelayFrames; // Per-sender delay by peerId-1 (size 0 or peerCount); empty = uniform inputDelayFrames.
		NetMatchDelayPolicy delayPolicy = NetMatchDelayPolicy::Auto;
		bool autosaveEnabled = false;
		uint32_t autosaveIntervalSeconds = 0;
		uint8_t idleWaitMinutes = 10;
		bool automaticRepair = true;
		// Redundant controller-frame window: each frame packet repeats the last N ticks so one lost
		// datagram costs nothing. 1 sends each tick once.
		uint8_t frameRedundancyTicks = 4;
		NetActorOwnershipPolicy ownershipPolicy = NetActorOwnershipPolicy::TeamOwner;
		std::string modePreset = "PvP";
		std::vector<NetMatchPlayerSlot> players;

		bool operator==(const NetMatchConfig&) const = default;
	};

	class NetMatchConfigUtil {
	public:
		static constexpr uint16_t c_Version = 4; // v4 added the spectate rule; v3 and v2 envelopes stay readable.
		static constexpr uint32_t c_MaxFiniteStartingGold = 29999;
		static constexpr uint32_t c_InfiniteGold = 1000000000;
		static constexpr uint8_t c_MinPeerCount = 2;
		static constexpr uint8_t c_MaxPeerCount = 4;
		static constexpr uint16_t c_MaxInputDelayFrames = 60; // Mirrors NetLockstepCodec::c_MaxInputDelayFrames.
		static constexpr uint8_t c_DefaultFrameRedundancyTicks = 4;
		static constexpr uint8_t c_MaxFrameRedundancyTicks = 8; // Mirrors NetLockstepCodec::c_MaxWindowTicks.
		static constexpr size_t c_MaxPlayers = 7; // Four co-op human peers plus the three peerless CPU teams left.
		static constexpr size_t c_MaxNameBytes = 64;
		static constexpr size_t c_MaxPresetBytes = 128;

		static NetMatchConfig MakeDefault(uint64_t sessionId = 0);
		/// The roster a rematch is played on: the peers still here keep their relative seat order and
		/// close up onto ids 1..N. An intact roster maps to itself, config and hash unchanged.
		/// @param outSeatMap Optional old lockstep peer id -> new lockstep peer id for every survivor.
		static bool DeriveRematchConfig(const NetMatchConfig& previous, const std::vector<uint8_t>& survivingPeerIds, NetMatchConfig& outConfig, std::map<uint8_t, uint8_t>* outSeatMap = nullptr, std::string* error = nullptr);
		static bool ValidateLocalAlpha(const NetMatchConfig& config, std::string* error = nullptr);
		static NetHash32 HashConfig(const NetMatchConfig& config);
		static std::string BuildReportJson(const NetMatchConfig& config);
		/// The peer's input delay: its per-sender entry, or the uniform value when no set rides the config.
		static uint16_t PeerInputDelay(const NetMatchConfig& config, uint8_t peerId);

		static const char* ModeName(NetMatchMode mode);
		/// The mode's menu-facing word, for rows that show a user label instead of the wire token.
		static const char* ModeLabel(NetMatchMode mode);
		static bool ParseMode(const std::string& text, NetMatchMode& outMode);
		static const char* OwnershipPolicyName(NetActorOwnershipPolicy policy);
		static bool ParseOwnershipPolicy(const std::string& text, NetActorOwnershipPolicy& outPolicy);
		/// Puts the host's saved session preferences on the config it publishes. Only a host calls
		/// this: a client adopts these rules with the roster, its own saved copy steers nothing.
		static void ApplySavedHostOptions(NetMatchConfig& config);
	};

} // namespace RTE
