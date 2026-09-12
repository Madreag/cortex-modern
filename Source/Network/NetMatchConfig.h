#pragma once

#include "NetProtocol.h"

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

	struct NetMatchConfig {
		uint16_t version = 2;
		uint64_t sessionId = 0;
		uint8_t hostPeerId = 1;
		bool dedicated = false; // The host keeps lockstep peer hostPeerId but seats no human slot there.
		uint8_t peerCount = 2;
		uint16_t inputDelayFrames = 0;
		std::vector<uint16_t> peerInputDelayFrames; // Per-sender delay by peerId-1 (size 0 or peerCount); empty = uniform inputDelayFrames.
		NetMatchMode mode = NetMatchMode::PvPSkirmish;
		NetActorOwnershipPolicy ownershipPolicy = NetActorOwnershipPolicy::TeamOwner;
		std::string activityType = "GAScripted";
		std::string activityPreset = "Skirmish Defense";
		std::string sceneName;
		std::string modePreset = "PvP";
		std::vector<NetMatchPlayerSlot> players;

		bool operator==(const NetMatchConfig&) const = default;
	};

	class NetMatchConfigUtil {
	public:
		static constexpr uint16_t c_Version = 2;
		static constexpr uint8_t c_MinPeerCount = 2;
		static constexpr uint8_t c_MaxPeerCount = 4;
		static constexpr uint16_t c_MaxInputDelayFrames = 60; // Mirrors NetLockstepCodec::c_MaxInputDelayFrames.
		static constexpr size_t c_MaxPlayers = 5; // Four human peers plus one peerless CPU slot.
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
		static bool ParseMode(const std::string& text, NetMatchMode& outMode);
		static const char* OwnershipPolicyName(NetActorOwnershipPolicy policy);
		static bool ParseOwnershipPolicy(const std::string& text, NetActorOwnershipPolicy& outPolicy);
	};

} // namespace RTE
