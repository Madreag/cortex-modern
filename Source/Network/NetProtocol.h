#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace RTE {

	using NetHash32 = std::array<uint8_t, 32>;

	enum class NetMessageType : uint16_t {
		ClientHello = 1,
		HostHello = 2,
		JoinAccepted = 3,
		JoinRejected = 4,
		ReadyState = 5,
		Heartbeat = 6,
		Ping = 7,
		Pong = 8,
		Disconnect = 9,
		SessionSummary = 10,
	};

	enum class NetRejectReason : uint16_t {
		ProtocolMismatch = 1,
		GameVersionMismatch = 2,
		BuildMismatch = 3,
		ControllerFrameVersionMismatch = 4,
		ControllerFrameSizeMismatch = 5,
		DeterministicConfigMismatch = 6,
		ModuleManifestMismatch = 7,
		SessionRulesMismatch = 8,
		UserdataModulesNotAllowed = 9,
		SessionFull = 10,
		HostNotAccepting = 11,
		DuplicateClientNonce = 12,
		MalformedMessage = 13,
		Timeout = 14,
		InternalError = 15,
	};

	enum class NetProtocolErrorCode {
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

	struct NetProtocolError {
		NetProtocolErrorCode code = NetProtocolErrorCode::None;
		size_t offset = 0;
		std::string message;
	};

	struct NetClientHello {
		uint64_t clientNonce = 0;
		uint16_t minProtocolVersion = 0;
		uint16_t maxProtocolVersion = 0;
		uint16_t controllerFrameVersion = 0;
		uint16_t controllerFrameEncodedSize = 0;
		uint8_t platformId = 0;
		std::string displayName;
		std::string gameVersion;
		std::string buildId;
		NetHash32 deterministicConfigHash{};
		NetHash32 moduleManifestHash{};
		NetHash32 sessionRulesHash{};
		NetHash32 sessionIdentityHash{};
		bool hasUserdataModules = false;

		bool operator==(const NetClientHello&) const = default;
	};

	struct NetHostHello {
		uint64_t sessionId = 0;
		uint64_t hostNonce = 0;
		uint16_t selectedProtocolVersion = 0;
		uint16_t controllerFrameVersion = 0;
		uint16_t controllerFrameEncodedSize = 0;
		uint8_t assignedPeerId = 0;
		uint8_t maxPeers = 0;
		uint8_t hostPlatformId = 0;
		std::string gameVersion;
		std::string hostName;
		std::string buildId;
		NetHash32 deterministicConfigHash{};
		NetHash32 moduleManifestHash{};
		NetHash32 sessionRulesHash{};
		NetHash32 sessionIdentityHash{};
		bool hasUserdataModules = false;

		bool operator==(const NetHostHello&) const = default;
	};

	struct NetJoinAccepted {
		uint64_t sessionId = 0;
		uint8_t assignedPeerId = 0;
		uint8_t maxPeers = 0;
		uint16_t selectedProtocolVersion = 0;
		uint32_t heartbeatIntervalMs = 0;
		uint32_t timeoutMs = 0;

		bool operator==(const NetJoinAccepted&) const = default;
	};

	struct NetJoinRejected {
		NetRejectReason rejectReason = NetRejectReason::InternalError;
		std::string humanMessage;
		std::string mismatchKey;
		std::string expected;
		std::string actual;

		bool operator==(const NetJoinRejected&) const = default;
	};

	struct NetReadyState {
		uint8_t peerId = 0;
		bool ready = false;
		NetHash32 deterministicConfigHash{};
		NetHash32 moduleManifestHash{};

		bool operator==(const NetReadyState&) const = default;
	};

	struct NetHeartbeat {
		uint64_t senderTimeMs = 0;
		uint32_t lastReceivedSequence = 0;
		uint32_t sessionState = 0;

		bool operator==(const NetHeartbeat&) const = default;
	};

	struct NetPing {
		uint64_t pingId = 0;
		uint64_t senderTimeMs = 0;

		bool operator==(const NetPing&) const = default;
	};

	struct NetPong {
		uint64_t pingId = 0;
		uint64_t senderTimeMs = 0;

		bool operator==(const NetPong&) const = default;
	};

	struct NetDisconnect {
		uint16_t disconnectReason = 0;
		std::string message;

		bool operator==(const NetDisconnect&) const = default;
	};

	struct NetSessionSummary {
		uint64_t sessionId = 0;
		uint8_t peerCount = 0;
		uint8_t localPeerId = 0;
		uint16_t sessionState = 0;
		NetHash32 deterministicConfigHash{};
		NetHash32 moduleManifestHash{};

		bool operator==(const NetSessionSummary&) const = default;
	};

	using NetPayload = std::variant<
		NetClientHello,
		NetHostHello,
		NetJoinAccepted,
		NetJoinRejected,
		NetReadyState,
		NetHeartbeat,
		NetPing,
		NetPong,
		NetDisconnect,
		NetSessionSummary>;

	struct NetMessage {
		uint32_t sequence = 0;
		uint16_t flags = 0;
		NetPayload payload;

		bool operator==(const NetMessage&) const = default;
	};

	struct NetDecodeResult {
		bool ok = false;
		NetMessage message;
		NetProtocolError error;
	};

	class NetProtocol {
	public:
		static constexpr uint32_t c_Magic = 0x324E4343U;
		static constexpr uint16_t c_Version = 1;
		static constexpr uint16_t c_HeaderBytes = 24;
		static constexpr size_t c_MaxControlPayloadBytes = 64U * 1024U;
		static constexpr size_t c_MaxDisplayNameBytes = 64;
		static constexpr size_t c_MaxShortTextBytes = 128;
		static constexpr size_t c_MaxDiagnosticTextBytes = 512;

		static NetMessageType MessageTypeOf(const NetPayload& payload);
		static const char* MessageTypeName(NetMessageType type);
		static const char* RejectReasonName(NetRejectReason reason);
		static const char* ErrorCodeName(NetProtocolErrorCode code);

		static bool Encode(const NetMessage& message, std::vector<uint8_t>& outBytes, NetProtocolError* error = nullptr);
		static NetDecodeResult Decode(const uint8_t* data, size_t size);
		static NetDecodeResult Decode(const std::vector<uint8_t>& bytes);
	};

} // namespace RTE
