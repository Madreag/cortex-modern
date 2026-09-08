#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace RTE {

	using NetHash32 = std::array<uint8_t, 32>;

	// Fixed-width auth material on the admission wire: transaction ids, the session epoch and client
	// nonces are 16 B; seat credentials, server challenges and proof macs are 32 B.
	using NetAuthBytes16 = std::array<uint8_t, 16>;
	using NetAuthBytes32 = std::array<uint8_t, 32>;

	/// Version of the H4 admission payloads, carried per message so the reconnect handshake can move
	/// without bumping the envelope an old client still has to decode a rejection from.
	constexpr uint16_t c_NetH4Version = 1;

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
		NewJoin = 11,
		TicketOffer = 12,
		TicketStoredAck = 13,
		JoinCommitted = 14,
		Reclaim = 15,
		Challenge = 16,
		Proof = 17,
		LeaveRequest = 18,
		LeaveAck = 19,
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
		// The one signal that lets a client delete its recovery record: the host sends it from the
		// same place it clears the seat registry, so no credential can verify afterwards.
		SessionEnded = 16,
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

	/// The build/config/module identity re-validated on every H4 admission message, ahead of the seat
	/// lookup so a mismatch never reveals whether the seat exists.
	struct NetH4Identity {
		uint16_t controllerFrameVersion = 0;
		uint16_t controllerFrameEncodedSize = 0;
		std::string gameVersion;
		std::string buildId;
		NetHash32 deterministicConfigHash{};
		NetHash32 moduleManifestHash{};
		NetHash32 sessionRulesHash{};
		NetHash32 sessionIdentityHash{};

		bool operator==(const NetH4Identity&) const = default;
	};

	struct NetH4NewJoin {
		uint16_t h4Version = c_NetH4Version;
		NetAuthBytes16 txId{};
		NetH4Identity identity;
		std::string displayName;

		bool operator==(const NetH4NewJoin&) const = default;
	};

	struct NetH4TicketOffer {
		uint16_t h4Version = c_NetH4Version;
		NetAuthBytes16 txId{};
		NetAuthBytes16 epoch{};
		uint16_t stableSeat = 0;
		uint32_t holderGeneration = 0;
		NetAuthBytes32 credential{};
		uint64_t hostSessionId = 0;
		uint32_t provisionalExpiryMs = 0;

		bool operator==(const NetH4TicketOffer&) const = default;
	};

	struct NetH4TicketStoredAck {
		uint16_t h4Version = c_NetH4Version;
		NetAuthBytes16 txId{};
		uint16_t stableSeat = 0;
		uint32_t holderGeneration = 0;
		bool stored = false;

		bool operator==(const NetH4TicketStoredAck&) const = default;
	};

	struct NetH4JoinCommitted {
		uint16_t h4Version = c_NetH4Version;
		NetAuthBytes16 txId{};
		uint16_t stableSeat = 0;
		uint32_t holderGeneration = 0;
		uint32_t incarnation = 0;
		uint8_t assignedPeerId = 0;

		bool operator==(const NetH4JoinCommitted&) const = default;
	};

	struct NetH4Reclaim {
		uint16_t h4Version = c_NetH4Version;
		NetAuthBytes16 txId{};
		NetAuthBytes16 epoch{};
		uint16_t stableSeat = 0;
		uint32_t holderGeneration = 0;
		NetH4Identity identity;
		std::string displayName;

		bool operator==(const NetH4Reclaim&) const = default;
	};

	struct NetH4Challenge {
		uint16_t h4Version = c_NetH4Version;
		NetAuthBytes16 txId{};
		NetAuthBytes32 challenge{};
		uint32_t lifetimeMs = 0;

		bool operator==(const NetH4Challenge&) const = default;
	};

	struct NetH4Proof {
		uint16_t h4Version = c_NetH4Version;
		NetAuthBytes16 txId{};
		NetAuthBytes16 epoch{};
		uint16_t stableSeat = 0;
		uint32_t holderGeneration = 0;
		NetAuthBytes16 clientNonce{};
		NetAuthBytes32 mac{};

		bool operator==(const NetH4Proof&) const = default;
	};

	struct NetH4LeaveRequest {
		uint16_t h4Version = c_NetH4Version;
		NetAuthBytes16 txId{};
		NetAuthBytes16 epoch{};
		uint16_t stableSeat = 0;
		uint32_t holderGeneration = 0;

		bool operator==(const NetH4LeaveRequest&) const = default;
	};

	struct NetH4LeaveAck {
		uint16_t h4Version = c_NetH4Version;
		NetAuthBytes16 txId{};
		uint16_t stableSeat = 0;
		uint32_t holderGeneration = 0;
		bool seatClosed = false;

		bool operator==(const NetH4LeaveAck&) const = default;
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
		NetSessionSummary,
		NetH4NewJoin,
		NetH4TicketOffer,
		NetH4TicketStoredAck,
		NetH4JoinCommitted,
		NetH4Reclaim,
		NetH4Challenge,
		NetH4Proof,
		NetH4LeaveRequest,
		NetH4LeaveAck>;

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
		// An admission message comes from a connection nobody has authenticated yet, so it is refused
		// on size before anything parses it. The largest H4 message is a Reclaim at ~498 B.
		static constexpr size_t c_MaxH4PayloadBytes = 1024;

		/// Whether the type is one of the H4 admission messages, which are size-capped separately.
		static bool IsH4MessageType(NetMessageType type);

		static NetMessageType MessageTypeOf(const NetPayload& payload);
		static const char* MessageTypeName(NetMessageType type);
		static const char* RejectReasonName(NetRejectReason reason);
		static const char* ErrorCodeName(NetProtocolErrorCode code);

		static bool Encode(const NetMessage& message, std::vector<uint8_t>& outBytes, NetProtocolError* error = nullptr);
		/// Encodes at an explicit header version so a peer on an older wire can still decode the
		/// envelope. Refused for any version whose payload schema this build cannot produce, because a
		/// rejection the peer misreads is worse than none (§10).
		static bool EncodeAtVersion(const NetMessage& message, uint16_t headerVersion, std::vector<uint8_t>& outBytes, NetProtocolError* error = nullptr);
		/// Whether a rejection stamped at that header version is one this build can honestly produce.
		static bool CanEncodeAtVersion(uint16_t headerVersion);
		/// Reads the header version out of a buffer whose magic matches, without decoding the payload.
		/// This is the stable negotiation envelope §10's probe asks about: magic and version sit at
		/// fixed offsets in every version of this header.
		static bool PeekHeaderVersion(const uint8_t* data, size_t size, uint16_t& outVersion);
		static NetDecodeResult Decode(const uint8_t* data, size_t size);
		static NetDecodeResult Decode(const std::vector<uint8_t>& bytes);
	};

} // namespace RTE
