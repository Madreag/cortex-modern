#pragma once

#include "ControllerFrame.h"
#include "NetGameCommand.h"
#include "NetMatchConfig.h"
#include "NetTransport.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
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
		PeerLeft = 7, // A clean leave: the frame field is the FIRST frame without the leaver's data; survivors continue.
		ResyncRequested = 8, // The host ends the round so everyone reconvenes and reloads its snapshot (rejoin/heal).
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
		std::map<uint8_t, uint16_t> peerInputDelayFrames; // Per-sender delay by peerId; empty = every peer uses inputDelayFrames.
		uint32_t timeoutMs = 500;
		uint8_t localPeerId = 0;
		uint8_t remotePeerId = 0; // 2-peer convenience; N-peer derives the remote set from peerCount.
		uint8_t peerCount = 2;
		NetPeerId remoteTransportPeerId = c_InvalidNetPeerId; // 2-peer convenience; see remoteTransportPeerIds.
		std::map<uint8_t, NetPeerId> remoteTransportPeerIds; // Lockstep peerId -> transport id for each remote; empty = derive the 2-peer pair.
		bool relayToOtherPeers = false; // Host-star: this (host) peer forwards each remote's frames/checksums to the other remotes.
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
		static constexpr uint16_t c_Version = 8;
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
		// How far ahead of the committed frame a received frame/checksum may legitimately target
		// (input-delay lead plus jitter); anything beyond is dropped so one peer cannot grow the
		// per-frame maps without bound.
		static constexpr uint64_t c_MaxFutureFrameSkew = 4ULL * c_MaxInputDelayFrames;

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
		/// Starts in playback mode: no remotes, no handshake — every frame commits from the local
		/// queue, which the replay reader feeds through QueueReplayFrame.
		bool StartReplay(INetTransport& transport, const NetLockstepConfig& config, std::string* error = nullptr);
		/// Feeds one recorded tick straight into the commit path: command senders preserved, no
		/// delay math, no wire — the replay's committed frame is exactly the recording's.
		bool QueueReplayFrame(uint64_t frame, std::vector<ControllerFrame> frames, std::vector<NetGameCommand> commands, std::string* error = nullptr);
		/// Rewinds a playback coordinator to re-commit from an earlier frame (the rollback
		/// fidelity gate re-runs a window). Replay mode only — there is no wire to rewind.
		bool RewindReplay(uint64_t firstFrame, std::string* error = nullptr);
		bool QueueLocalInput(uint64_t producedFrame, const std::vector<ControllerFrame>& frames, const std::vector<NetGameCommand>& commands, std::string* error = nullptr);
		bool SubmitLocalChecksum(uint64_t frame, const std::array<uint8_t, 32>& hash, std::string* error = nullptr);
		void Tick(uint64_t nowMs);
		void Complete(const std::string& message = "complete");
		/// Announces a clean local leave: peers keep our frames through the last produced one, then
		/// advance without us. The relay host cannot leave a 3+ match alive (it is the star's hub),
		/// so a host leave completes the match for everyone instead.
		void Leave(const std::string& message = "player left");
		/// Ends the round on every peer so the match reconvenes and reloads the host's snapshot
		/// (a rejoin or an operator-forced heal). Host-initiated.
		void RequestResync(const std::string& message = "resync requested");
		/// Receives the session-protocol traffic (a reconnecting peer's handshake) the coordinator
		/// would otherwise discard while it owns the transport queue.
		void SetSessionEventSink(std::function<void(const NetTransportEvent&)> sink) { m_SessionEventSink = std::move(sink); }
		bool PopReadyFrame(NetLockstepReadyFrame& outFrame);

		NetLockstepState GetState() const { return m_State; }
		bool IsRunning() const { return m_State == NetLockstepState::Running; }
		bool IsFailed() const { return m_State == NetLockstepState::Failed; }
		bool IsStopped() const { return m_State == NetLockstepState::Stopped; }
		const NetLockstepStats& GetStats() const { return m_Stats; }
		const NetLockstepConfig& GetConfig() const { return m_Config; }
		bool IsLocalActor(int64_t actorUniqueID, int actorTeam, bool cpuControlled) const;
		uint8_t ResolveTeamCommandAuthority(int team) const;
		/// Whether a transport peer carries one of this round's lockstep remotes (a NEW transport
		/// peer reaching session-Ready mid-match is a reconnector).
		bool UsesTransportPeer(NetPeerId transportPeerId) const;
		/// Whether the actor's owner peer has left as of the given frame; the lockstep gate means every
		/// survivor answers this identically when consuming that frame, so the stand-down is synced.
		bool IsActorOwnerGone(int64_t actorUniqueID, int actorTeam, bool cpuControlled, uint64_t frame) const;
		/// Whether the peer has cleanly left as of the given frame (never true for the local peer).
		bool IsPeerGoneAtFrame(uint8_t peerId, uint64_t frame) const;
		/// Peers that announced a clean leave, each with the first frame that lacks their data.
		const std::map<uint8_t, uint64_t>& GetPeerLeaveFrames() const { return m_PeerLeaveFrames; }
		/// Names the required peers the next frame still waits on; empty when none are missing.
		std::string DescribeMissingPeers() const;
		/// The peer's roster display name, or "peer N" when the roster has none.
		std::string DescribePeer(uint8_t peerId) const;
		std::string BuildReportJson() const;

		static const char* StateName(NetLockstepState state);

	private:
		bool SendPacket(const NetLockstepPacket& packet, NetTransportLane lane, std::string* error = nullptr);
		void HandleEvent(const NetTransportEvent& event, uint64_t nowMs);
		void HandlePacket(const NetLockstepPacket& packet, uint64_t nowMs, NetPeerId fromTransport);
		void HandleStart(const NetLockstepStart& start, NetPeerId fromTransport);
		void HandleFrame(const NetLockstepFrame& frame, uint64_t nowMs, NetPeerId fromTransport);
		void HandleStop(const NetLockstepStop& stop, uint64_t nowMs, NetPeerId fromTransport);
		void HandleChecksum(const NetLockstepChecksum& checksum, NetPeerId fromTransport);
		/// Whether a packet's claimed sender owns the transport it arrived on. Only the relay host
		/// receives each remote directly; clients get everything via the relay and trust the host.
		bool SenderOwnsTransport(uint8_t claimedPeerId, NetPeerId fromTransport) const;
		void CompareChecksums(uint64_t frame);
		void AdvanceReadyFrames(uint64_t nowMs);
		void ApplyPeerLeave(uint8_t peerId, uint64_t firstFrameWithout, const std::string& message, uint64_t nowMs);
		bool IsRemoteRequiredForFrame(uint8_t peerId, uint64_t frame) const;
		uint16_t PeerInputDelay(uint8_t peerId) const;
		uint64_t EffectiveStartOf(uint8_t peerId) const;
		uint8_t FirstAliveHumanPeerForTeam(uint8_t team, uint64_t frame) const;
		void Fail(NetLockstepStopReason reason, uint64_t frame, const std::string& message);

		INetTransport* m_Transport = nullptr;
		NetLockstepConfig m_Config;
		NetLockstepState m_State = NetLockstepState::Idle;
		NetLockstepStats m_Stats;
		std::vector<uint8_t> m_RemotePeerIds; //!< Every peer except local; derived at Start.
		std::map<uint8_t, NetPeerId> m_RemoteTransports; //!< Lockstep peerId -> transport id for each remote.
		std::set<uint8_t> m_RemoteStartsReceived; //!< Remotes whose matching Start we've accepted; run when all present.
		std::map<uint8_t, uint64_t> m_PeerLeaveFrames; //!< Cleanly-left peers -> the first frame WITHOUT their data.
		std::map<uint8_t, uint64_t> m_PeerEffectiveStart; //!< peerId -> the first frame that carries this sender's input.
		uint64_t m_LastQueuedTargetFrame = UINT64_MAX; //!< Highest produced target frame; UINT64_MAX until the first queue.
		std::function<void(const NetTransportEvent&)> m_SessionEventSink; //!< Forwards session traffic (reconnect handshakes) mid-match.
		bool m_RelayHost = false; //!< Host-star relay: forward each remote's frames/checksums to the other remotes.
		uint64_t m_WaitingFrame = 0;
		uint64_t m_WaitStartMs = 0;
		uint64_t m_LastStallFrame = UINT64_MAX;
		std::map<uint64_t, std::vector<ControllerFrame>> m_LocalFrames;
		std::map<uint64_t, std::map<uint8_t, std::vector<ControllerFrame>>> m_RemoteFrames; //!< frame -> (peerId -> frames)
		std::map<uint64_t, std::vector<NetGameCommand>> m_LocalCommands;
		std::map<uint64_t, std::map<uint8_t, std::vector<NetGameCommand>>> m_RemoteCommands; //!< frame -> (peerId -> commands)
		std::map<uint64_t, std::array<uint8_t, 32>> m_LocalChecksums;
		std::map<uint64_t, std::map<uint8_t, std::array<uint8_t, 32>>> m_RemoteChecksums; //!< frame -> (peerId -> hash)
		std::deque<NetLockstepReadyFrame> m_ReadyFrames;

		bool AllRemoteStartsReceived() const { return m_RemoteStartsReceived.size() == m_RemotePeerIds.size(); }
		bool IsKnownRemotePeer(uint8_t peerId) const;
		void RelayToOtherRemotes(const NetLockstepPacket& packet, uint8_t fromPeerId);
	};

} // namespace RTE
