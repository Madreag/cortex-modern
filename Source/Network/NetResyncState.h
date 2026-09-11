#pragma once

#include "NetLockstep.h"

#include <map>

namespace RTE {

	struct NetResyncPlayerBindings {
		uint64_t frame = 0;
		NetGamePlayerBindings bindings;
		bool operator==(const NetResyncPlayerBindings&) const = default;
	};

	struct NetResyncPendingCommand {
		uint64_t frame = 0;
		NetGameCommand command;
		bool operator==(const NetResyncPendingCommand&) const = default;
	};

	struct NetResyncState {
		uint64_t sessionId = 0, sourceRound = 0, savedTick = 0;
		std::map<int64_t, uint8_t> controlOwners, droppedControlOwners;
		std::map<uint8_t, NetResyncPlayerBindings> playerBindings;
		std::map<uint8_t, uint64_t> appliedCommands;
		std::vector<NetResyncPendingCommand> pendingCommands;
		std::vector<NetResyncPendingCommand> pendingPlayerBindings;
		std::vector<NetLockstepFrame> pendingInputs;
		std::vector<NetGameCommand> admittedReseats;
		bool operator==(const NetResyncState&) const = default;
	};

	class NetResyncCodec {
	public:
		static constexpr size_t c_MaxPendingInputs = NetLockstepCodec::c_MaxPeerCount * (NetLockstepCodec::c_MaxFutureFrameSkew + 1);
		static constexpr size_t c_MaxAuxiliaryBytes = 8U * 1024U * 1024U;
		static constexpr size_t c_MaxArchiveBytes = 64U * 1024U * 1024U;
		static constexpr size_t c_MaxReferenceBytes = c_MaxPendingInputs * (NetLockstepCodec::c_MaxCommandsPerPacket + 1) * 7;
		static constexpr size_t c_MaxMetadataBytes = c_MaxAuxiliaryBytes + c_MaxReferenceBytes + c_MaxPendingInputs * (NetLockstepCodec::c_MaxRecoveryInputBytes + 9);
		static constexpr size_t c_MaxTotalBytes = c_MaxMetadataBytes + c_MaxArchiveBytes;
		static bool Encode(const NetResyncState& state, const std::vector<uint8_t>& archive, std::vector<uint8_t>& bytes, std::string* error = nullptr);
		static bool Decode(const std::vector<uint8_t>& bytes, uint64_t sessionId, uint64_t startFrame, NetResyncState& state, std::vector<uint8_t>& archive, std::string* error = nullptr);
	};

}
