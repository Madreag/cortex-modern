#pragma once

#include "NetMatchConfig.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace RTE {

	enum class NetLobbyMessageType : uint16_t {
		Hello = 1,
		PeerState = 2,
		MatchConfig = 3,
		ConfigAck = 4,
		Ready = 5,
		Start = 6,
		Abort = 7,
		StateChunk = 8,
		SeatAssign = 9,
	};

	enum class NetLobbyErrorCode {
		None,
		NullBuffer,
		ShortHeader,
		BadMagic,
		UnsupportedVersion,
		BadHeaderSize,
		UnknownFlags,
		UnknownMessageType,
		ReservedFieldNonZero,
		PayloadLengthMismatch,
		PayloadTooLarge,
		TruncatedPayload,
		TrailingBytes,
		StringTooLong,
		InvalidString,
		InvalidValue,
		EncodeFailed,
	};

	struct NetLobbyError {
		NetLobbyErrorCode code = NetLobbyErrorCode::None;
		size_t offset = 0;
		std::string message;
	};

	struct NetLobbyHello {
		uint16_t minProtocolVersion = 1;
		uint16_t maxProtocolVersion = 1;
		uint8_t peerId = 0;
		std::string displayName;
		std::string desiredRole;

		bool operator==(const NetLobbyHello&) const = default;
	};

	struct NetLobbyPeerState {
		uint8_t peerId = 0;
		bool ready = false;
		uint32_t pingMs = 0;
		uint32_t jitterMs = 0;
		std::string displayName;
		std::string platform;
		bool connected = true;

		bool operator==(const NetLobbyPeerState&) const = default;
	};

	struct NetLobbyMatchConfig {
		NetMatchConfig config;

		bool operator==(const NetLobbyMatchConfig&) const = default;
	};

	struct NetLobbyConfigAck {
		uint8_t peerId = 0;
		bool accepted = false;
		NetHash32 matchConfigHash{};
		std::string reason;

		bool operator==(const NetLobbyConfigAck&) const = default;
	};

	struct NetLobbyReady {
		uint8_t peerId = 0;
		bool ready = false;

		bool operator==(const NetLobbyReady&) const = default;
	};

	struct NetLobbyStart {
		uint64_t sessionId = 0;
		uint64_t startFrame = 0;
		uint16_t inputDelayFrames = 0;
		NetHash32 matchConfigHash{};

		bool operator==(const NetLobbyStart&) const = default;
	};

	struct NetLobbyAbort {
		uint8_t peerId = 0;
		std::string reason;

		bool operator==(const NetLobbyAbort&) const = default;
	};

	// One chunk of a match-state file (a resync/rejoin snapshot) streaming host -> peer.
	struct NetLobbyStateChunk {
		uint64_t transferId = 0;
		uint32_t totalBytes = 0;
		uint16_t chunkIndex = 0;
		uint16_t chunkCount = 0;
		std::vector<uint8_t> bytes;

		bool operator==(const NetLobbyStateChunk&) const = default;
	};

	// The lockstep id the host bound to this connection, host -> that peer, ahead of the config.
	struct NetLobbySeatAssign {
		uint8_t assignedPeerId = 0;

		bool operator==(const NetLobbySeatAssign&) const = default;
	};

	using NetLobbyPayload = std::variant<
		NetLobbyHello,
		NetLobbyPeerState,
		NetLobbyMatchConfig,
		NetLobbyConfigAck,
		NetLobbyReady,
		NetLobbyStart,
		NetLobbyAbort,
		NetLobbyStateChunk,
		NetLobbySeatAssign>;

	struct NetLobbyMessage {
		NetLobbyPayload payload;

		bool operator==(const NetLobbyMessage&) const = default;
	};

	struct NetLobbyDecodeResult {
		bool ok = false;
		NetLobbyMessage message;
		NetLobbyError error;
	};

	class NetLobbyProtocol {
	public:
		static constexpr uint32_t c_Magic = 0x344C4343U;
		static constexpr uint16_t c_Version = 3;
		static constexpr uint16_t c_HeaderBytes = 16;
		static constexpr size_t c_MaxPayloadBytes = 64U * 1024U;
		static constexpr size_t c_MaxShortTextBytes = 128;
		static constexpr size_t c_MaxDisplayNameBytes = 64;
		static constexpr size_t c_MaxStateChunkBytes = 48U * 1024U;
		static constexpr uint32_t c_MaxTotalStateBytes = 2U * 1024U * 1024U * 1024U;
		static constexpr uint32_t c_MaxStateChunkCount = (c_MaxTotalStateBytes - 1U) / c_MaxStateChunkBytes + 1U;
		static_assert(c_MaxStateChunkCount <= 65535U);
		static constexpr size_t c_MaxPlayers = NetMatchConfigUtil::c_MaxPlayers;

		static constexpr uint16_t GetStateChunkCount(size_t totalBytes) {
			return totalBytes == 0 || totalBytes > c_MaxTotalStateBytes ? 0 : static_cast<uint16_t>((totalBytes - 1) / c_MaxStateChunkBytes + 1);
		}

		static NetLobbyMessageType MessageTypeOf(const NetLobbyPayload& payload);
		static const char* MessageTypeName(NetLobbyMessageType type);
		static const char* ErrorCodeName(NetLobbyErrorCode code);

		static bool Encode(const NetLobbyMessage& message, std::vector<uint8_t>& outBytes, NetLobbyError* error = nullptr);
		static NetLobbyDecodeResult Decode(const uint8_t* data, size_t size);
		static NetLobbyDecodeResult Decode(const std::vector<uint8_t>& bytes);
	};

} // namespace RTE
