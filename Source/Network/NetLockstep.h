#pragma once

#include "ControllerFrame.h"
#include "NetGameCommand.h"
#include "NetMatchConfig.h"
#include "NetTransport.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace RTE {

	enum class NetLockstepPacketType : uint16_t {
		Start = 1,
		Frame = 2,
		Ack = 3,
		Stop = 4,
		Checksum = 5,
	};

	enum class NetLockstepStopReason : uint16_t {
		Complete = 1,
		MissingFrameTimeout = 2,
		Desync = 3,
		ProtocolError = 4,
		PeerDisconnected = 5,
		InternalError = 6,
	};

	enum class NetLockstepErrorCode {
		None,
		NullBuffer,
		ShortHeader,
		BadMagic,
		UnsupportedVersion,
		BadHeaderSize,
		UnknownFlags,
		UnknownPacketType,
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

	struct NetLockstepError {
		NetLockstepErrorCode code = NetLockstepErrorCode::None;
		size_t offset = 0;
		std::string message;
	};

	struct NetLockstepStart {
		uint64_t sessionId = 0;
		uint64_t startFrame = 0;
		uint16_t inputDelayFrames = 0;
		uint16_t controllerFrameVersion = 0;
		uint16_t controllerFrameEncodedSize = 0;
		uint8_t localPeerId = 0;
		uint8_t peerCount = 0;
		std::string scenario;
		std::string ownershipPolicy;

		bool operator==(const NetLockstepStart&) const = default;
	};

	struct NetLockstepFrame {
		uint8_t senderPeerId = 0;
		uint64_t targetFrame = 0;
		std::vector<ControllerFrame> frames;
		std::vector<NetGameCommand> commands;

		bool operator==(const NetLockstepFrame& rhs) const;
	};

	struct NetLockstepAck {
		uint8_t senderPeerId = 0;
		uint64_t highestContiguousFrame = 0;
		uint32_t receivedMask = 0;

		bool operator==(const NetLockstepAck&) const = default;
	};

	struct NetLockstepStop {
		uint8_t senderPeerId = 0;
		NetLockstepStopReason reason = NetLockstepStopReason::InternalError;
		uint64_t frame = 0;
		std::string message;

		bool operator==(const NetLockstepStop&) const = default;
	};

	// A sim-gated state hash for one tick, exchanged periodically so the peers detect a silent divergence.
	struct NetLockstepChecksum {
		uint8_t senderPeerId = 0;
		uint64_t frame = 0;
		std::array<uint8_t, 32> hash{};

		bool operator==(const NetLockstepChecksum&) const = default;
	};

	using NetLockstepPayload = std::variant<NetLockstepStart, NetLockstepFrame, NetLockstepAck, NetLockstepStop, NetLockstepChecksum>;

	struct NetLockstepPacket {
		NetLockstepPayload payload;

		bool operator==(const NetLockstepPacket&) const = default;
	};

	struct NetLockstepDecodeResult {
		bool ok = false;
		NetLockstepPacket packet;
		NetLockstepError error;
	};

	enum class NetLockstepState {
		Idle,
		WaitingForStart,
		Running,
		Stopped,
		Failed,
	};

	struct NetLockstepConfig {
		uint64_t sessionId = 0;
		uint64_t startFrame = 0;
		uint16_t inputDelayFrames = 0;
		uint32_t timeoutMs = 500;
		uint8_t localPeerId = 0;
		uint8_t remotePeerId = 0;
		uint8_t peerCount = 2;
		NetPeerId remoteTransportPeerId = c_InvalidNetPeerId;
		NetTransportLane frameLane = NetTransportLane::ControlReliable;
		std::string scenario = "lockstep";
		std::string ownershipPolicy = "unique-id-split";
		NetMatchConfig matchConfig;
	};

	struct NetLockstepReadyFrame {
		uint64_t frame = 0;
		std::vector<ControllerFrame> localFrames;
		std::vector<ControllerFrame> remoteFrames;
		std::vector<NetGameCommand> localCommands;
		std::vector<NetGameCommand> remoteCommands;
	};

	struct NetLockstepStats {
		uint64_t sessionId = 0;
		uint64_t configuredStartFrame = 0;
		uint64_t effectiveStartFrame = 0;
		uint16_t inputDelayFrames = 0;
		uint8_t localPeerId = 0;
		uint8_t remotePeerId = 0;
		uint32_t startPacketsSent = 0;
		uint32_t startPacketsReceived = 0;
		uint32_t framePacketsSent = 0;
		uint32_t framePacketsReceived = 0;
		uint32_t ignoredSessionPackets = 0;
		uint64_t localControllerFramesSent = 0;
		uint64_t remoteControllerFramesReceived = 0;
		uint64_t remoteControllerFramesAccepted = 0;
		uint32_t framesAccepted = 0;
		uint32_t duplicateFrames = 0;
		uint32_t outOfOrderFrames = 0;
		uint32_t missingFrameStalls = 0;
		uint32_t timeouts = 0;
		uint64_t nextFrame = 0;
		std::string timeoutReason;
	};

	class NetLockstepCodec {
	public:
		static constexpr uint32_t c_Magic = 0x334C4343U;
		static constexpr uint16_t c_Version = 4;
		static constexpr uint16_t c_HeaderBytes = 16;
		static constexpr size_t c_MaxPayloadBytes = 64U * 1024U;
		static constexpr size_t c_MaxScenarioBytes = 128;
		static constexpr size_t c_MaxOwnershipPolicyBytes = 128;
		static constexpr size_t c_MaxDiagnosticBytes = 512;
		static constexpr size_t c_MaxFramesPerPacket = 512;
		static constexpr size_t c_MaxCommandsPerPacket = 256;
		static constexpr size_t c_MaxCargoPerDelivery = 64;
		static constexpr uint16_t c_MaxInputDelayFrames = 60;
		static constexpr uint8_t c_MaxPeerCount = 16;

		static NetLockstepPacketType PacketTypeOf(const NetLockstepPayload& payload);
		static const char* PacketTypeName(NetLockstepPacketType type);
		static const char* StopReasonName(NetLockstepStopReason reason);
		static const char* ErrorCodeName(NetLockstepErrorCode code);

		static bool Encode(const NetLockstepPacket& packet, std::vector<uint8_t>& outBytes, NetLockstepError* error = nullptr);
		static NetLockstepDecodeResult Decode(const uint8_t* data, size_t size);
		static NetLockstepDecodeResult Decode(const std::vector<uint8_t>& bytes);
	};

	class NetLockstepCoordinator {
	public:
		bool Start(INetTransport& transport, const NetLockstepConfig& config, std::string* error = nullptr);
		bool QueueLocalInput(uint64_t producedFrame, const std::vector<ControllerFrame>& frames, const std::vector<NetGameCommand>& commands, std::string* error = nullptr);
		bool SubmitLocalChecksum(uint64_t frame, const std::array<uint8_t, 32>& hash, std::string* error = nullptr);
		void Tick(uint64_t nowMs);
		void Complete(const std::string& message = "complete");
		bool PopReadyFrame(NetLockstepReadyFrame& outFrame);

		NetLockstepState GetState() const { return m_State; }
		bool IsRunning() const { return m_State == NetLockstepState::Running; }
		bool IsFailed() const { return m_State == NetLockstepState::Failed; }
		bool IsStopped() const { return m_State == NetLockstepState::Stopped; }
		const NetLockstepStats& GetStats() const { return m_Stats; }
		const NetLockstepConfig& GetConfig() const { return m_Config; }
		bool IsLocalActor(int64_t actorUniqueID, int actorTeam, bool cpuControlled) const;
		uint8_t ResolveTeamCommandAuthority(int team) const;
		std::string BuildReportJson() const;

		static const char* StateName(NetLockstepState state);

	private:
		bool SendPacket(const NetLockstepPacket& packet, NetTransportLane lane, std::string* error = nullptr);
		void HandleEvent(const NetTransportEvent& event, uint64_t nowMs);
		void HandlePacket(const NetLockstepPacket& packet, uint64_t nowMs);
		void HandleStart(const NetLockstepStart& start);
		void HandleFrame(const NetLockstepFrame& frame, uint64_t nowMs);
		void HandleStop(const NetLockstepStop& stop);
		void HandleChecksum(const NetLockstepChecksum& checksum);
		void CompareChecksums(uint64_t frame);
		void AdvanceReadyFrames(uint64_t nowMs);
		void Fail(NetLockstepStopReason reason, uint64_t frame, const std::string& message);

		INetTransport* m_Transport = nullptr;
		NetLockstepConfig m_Config;
		NetLockstepState m_State = NetLockstepState::Idle;
		NetLockstepStats m_Stats;
		bool m_RemoteStartReceived = false;
		uint64_t m_WaitingFrame = 0;
		uint64_t m_WaitStartMs = 0;
		uint64_t m_LastStallFrame = UINT64_MAX;
		std::map<uint64_t, std::vector<ControllerFrame>> m_LocalFrames;
		std::map<uint64_t, std::vector<ControllerFrame>> m_RemoteFrames;
		std::map<uint64_t, std::vector<NetGameCommand>> m_LocalCommands;
		std::map<uint64_t, std::vector<NetGameCommand>> m_RemoteCommands;
		std::map<uint64_t, std::array<uint8_t, 32>> m_LocalChecksums;
		std::map<uint64_t, std::array<uint8_t, 32>> m_RemoteChecksums;
		std::deque<NetLockstepReadyFrame> m_ReadyFrames;
	};

} // namespace RTE
