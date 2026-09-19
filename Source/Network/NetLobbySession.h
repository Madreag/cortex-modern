#pragma once

#include "NetLobbyProtocol.h"
#include "NetTransport.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace RTE {
	class NetSession;

	/// What a targeted state transfer did with the blob it was handed.
	enum class NetLobbyStateTransfer : uint8_t {
		Refused = 0, //!< Not taken and not kept: an unknown remote, an empty or oversize blob.
		Queued = 1,  //!< Held until the transfer in flight drains.
		Started = 2, //!< On the pump now.
	};

	enum class NetLobbyState {
		Idle,
		WaitingForConfig,
		WaitingForConfigAck,
		WaitingForReady,
		Started,
		Rejected,
		Failed,
	};

	struct NetLobbySessionConfig {
		std::vector<NetTransportEvent> pendingEvents; //!< Earlier packets on the same bound transport.
		bool host = false;
		uint8_t localPeerId = 0;
		uint8_t remotePeerId = 0; // 2-peer convenience; N-peer derives the remote set from remoteTransportPeerIds.
		NetPeerId remoteTransportPeerId = c_InvalidNetPeerId; // 2-peer convenience; see remoteTransportPeerIds.
		std::map<uint8_t, NetPeerId> remoteTransportPeerIds; // Lockstep peerId -> transport id for each remote; empty = derive the 2-peer pair.
		NetMatchConfig matchConfig;
		uint64_t startFrame = 0;
		uint32_t resendIntervalMs = 250;
		uint32_t peerStateIntervalMs = 1000;
		uint32_t timeoutMs = 5000;
		std::string displayName = "Player";
		std::string platform = "unknown";
		bool autoReady = true;
		bool autoStart = true;
		NetSession* session = nullptr; //!< Shared admission state while the lobby owns transport events.
		std::function<uint64_t()> sessionNowMs; //!< Shared session clock; absent callers use the round's captured base.
		bool autoInputDelay = false;
		// A rematch may reseat a client the host knows more drops than; the host names each connection's
		// lockstep id and the client says nothing until it has adopted the one meant for it.
		bool assignSeats = false;
	};

	struct NetLobbyStats {
		uint32_t messagesSent = 0;
		uint32_t messagesReceived = 0;
		uint32_t malformedMessages = 0;
		uint32_t ignoredSessionPackets = 0;
		uint32_t configPacketsSent = 0;
		uint32_t configAcksReceived = 0;
		uint32_t readyPacketsReceived = 0;
		uint32_t startPacketsSent = 0;
		uint32_t startPacketsReceived = 0;
		uint32_t timeouts = 0;
		uint32_t unconfiguredPeerStates = 0; //!< Roster states for a peer this config has no slot for, ignored.
		uint32_t unboundConnectionFaults = 0; //!< Host: faults from a transport this round never bound.
		uint32_t unboundDisconnects = 0; //!< Host: disconnects from a transport this round never bound.
		uint32_t seatAssignmentsSent = 0;     //!< Host: seat bindings handed to remotes.
		uint32_t seatAssignmentsAdopted = 0;  //!< Client: seat bindings it took from the host.
	};

	class NetLobbySession {
	public:
		bool Start(INetTransport& transport, const NetLobbySessionConfig& config, std::string* error = nullptr);
		void Tick(uint64_t nowMs);

		NetLobbyState GetState() const { return m_State; }
		bool IsStarted() const { return m_State == NetLobbyState::Started; }
		bool IsFailed() const { return m_State == NetLobbyState::Failed; }
		bool IsRejected() const { return m_State == NetLobbyState::Rejected; }
		bool IsLocalReady() const { return m_LocalReady; }
		bool IsRemoteReady() const { return AllRemoteReady(); }
		bool IsRemoteReady(uint8_t peerId) const;
		const std::string& GetRemoteName() const;
		const std::string& GetRemoteName(uint8_t peerId) const;
		/// Whether this peer's periodic state has been heard at all (directly or host-relayed).
		bool HasHeardFrom(uint8_t peerId) const;
		/// The peer's last reported ping in ms (the host stamps relayed states with its measurement).
		uint32_t GetRemotePingMs(uint8_t peerId) const;
		bool IsStartRequested() const { return m_StartRequested; }
		const NetMatchConfig& GetMatchConfig() const { return m_Config.matchConfig; }
		const NetHash32& GetMatchConfigHash() const { return m_MatchConfigHash; }
		uint64_t GetStartFrame() const { return m_StartFrame; }
		const std::string& GetFailureReason() const { return m_FailureReason; }
		const NetLobbyStats& GetStats() const { return m_Stats; }

		void SetLocalReady(bool ready);
		void RequestStart();

		/// Queues a match-state file (a resync/rejoin snapshot) to stream to every remote before the
		/// lobby starts: chunks pace out through Tick and the Start rides the same ordered lane, so a
		/// started client always holds the complete state.
		void BeginStateTransfer(std::vector<uint8_t> fileBytes);
		/// The same StateChunk pump, aimed at one late remote after the lobby has Started.
		/// @return Whether this call took the pump; a queued blob starts when the one in flight drains.
		bool BeginStateTransferTo(uint8_t peerId, std::vector<uint8_t> fileBytes);
		/// The same call, with the refusal told apart from the queue: a refused blob was never taken,
		/// so its caller must retry it rather than wait for a transfer that will not happen.
		NetLobbyStateTransfer BeginStateTransferToPeer(uint8_t peerId, std::vector<uint8_t> fileBytes);
		/// Whether a joiner's image is waiting for the pump.
		size_t QueuedStateTransfers() const { return m_QueuedStateTransfers.size(); }
		/// Binds a session-Ready joiner so the host can send it config and StateChunks.
		bool BindLateRemote(uint8_t peerId, NetPeerId transport, std::string* error = nullptr);
		/// Binds a world bootstrap's own lobby id. Its chunks go out by transport, without the lobby-up
		/// gate: a reserved world id is never named in a lobby payload, so the client cannot mark it up.
		bool BindWorldTransferRemote(uint8_t peerId, NetPeerId transport, std::string* error = nullptr);
		/// Whether that peer is a world bootstrap this host bound itself.
		bool IsWorldTransferPeer(uint8_t peerId) const { return m_WorldTransferPeers.find(peerId) != m_WorldTransferPeers.end(); }
		/// Whether that remote's own lobby has spoken; before it does, its session discards lobby packets.
		bool IsRemoteLobbyUp(uint8_t peerId) const { return m_RemoteLobbyUp.find(peerId) != m_RemoteLobbyUp.end(); }
		void SendMatchConfigTo(uint8_t peerId);
		/// Sends queued chunks after Started; Tick itself stops once the lobby is terminal.
		void PumpOutgoingChunks();
		void HandleTransportEvent(const NetTransportEvent& event, uint64_t nowMs);
		bool SendPayloadTo(uint8_t peerId, const NetLobbyPayload& payload, std::string* error = nullptr);
		bool SendPayload(const NetLobbyPayload& payload, std::string* error = nullptr);
		const std::vector<uint8_t>& PeekReceivedState() const { return m_ReceivedState; }
		uint64_t GetOutgoingStateId() const { return m_OutgoingStateId; }
		uint16_t GetOutgoingChunkCount() const { return m_OutgoingChunkCount; }
		uint16_t GetOutgoingChunkIndex(uint8_t peerId) const { return OutgoingChunkIndex(peerId); }
		struct WorldJoinReport {
			uint8_t kind = 0;
			uint64_t value = 0;
			uint8_t fromPeer = 0;
			bool pending = false;
		};
		WorldJoinReport TakeWorldJoinReport();
		std::vector<uint8_t> TakePendingTailBytes();
		void SetStartFrame(uint64_t startFrame) { m_StartFrame = startFrame; }
		bool HasCompleteStateTransfer() const { return m_IncomingStateComplete; }
		/// Gets whether any remote still lacks a chunk of the queued state file.
		bool HasPendingStateChunks() const;
		/// Takes the fully received state file (empties the buffer).
		std::vector<uint8_t> TakeReceivedState();
		/// Received/total byte progress of an incoming transfer (0/0 when none).
		std::pair<uint32_t, uint32_t> GetStateTransferProgress() const { return {m_IncomingReceivedBytes, m_IncomingTotalBytes}; }
		bool IsStateTransferOutgoing() const { return HasPendingStateChunks(); }
		uint64_t GetStateTransferProgressSerial() const { return m_StateTransferProgressSerial; }

		std::string BuildReportJson() const;

		static const char* StateName(NetLobbyState state);

	private:
		bool SendTo(NetPeerId transport, const NetLobbyPayload& payload, std::string* error = nullptr);
		bool Send(const NetLobbyPayload& payload, std::string* error = nullptr); // Broadcast to every remote transport.
		void SendConfigIfDue(uint64_t nowMs);
		void SendPeerState();
		void SendPeerStateIfDue(uint64_t nowMs);
		void SendReadyIfNeeded();
		void SendStartIfReady();
		void HandleEvent(const NetTransportEvent& event, uint64_t nowMs);
		void HandleMessage(const NetLobbyMessage& message);
		void HandleMatchConfig(const NetLobbyMatchConfig& message);
		void HandleConfigAck(const NetLobbyConfigAck& message);
		void HandleReady(const NetLobbyReady& message);
		void HandleStart(const NetLobbyStart& message);
		void HandlePeerState(const NetLobbyPeerState& message);
		void SyncSessionPeers();
		void RemoveRemote(NetPeerId transportPeerId);
		void RejectRemote(NetPeerId transportPeerId, const std::string& reason);
		void HandleStateChunk(const NetLobbyStateChunk& message);
		void HandleSeatAssign(const NetLobbySeatAssign& message);
		void SendSeatAssign(uint8_t peerId);
		void RestartStateTransfer();
		/// Takes the free pump for the next waiting joiner image. Returns whether one started.
		bool StartNextQueuedStateTransfer();
		void SendQueuedStateChunks();
		void Reject(const std::string& reason);
		void Fail(const std::string& reason);

		bool IsKnownRemote(uint8_t peerId) const;
		bool IsCommittedTransport(NetPeerId transportPeerId) const;
		uint16_t OutgoingChunkIndex(uint8_t peerId) const;
		/// Whether the round seats a human on a peer other than this host's own.
		bool SeatsRemoteHuman() const;
		bool AllConfigAcked() const;
		bool AllRemoteReady() const;

		INetTransport* m_Transport = nullptr;
		NetLobbySessionConfig m_Config;
		NetLobbyState m_State = NetLobbyState::Idle;
		NetHash32 m_MatchConfigHash{};
		uint64_t m_StartFrame = 0;
		uint64_t m_LastConfigSentMs = 0;
		uint64_t m_LastPeerStateSentMs = 0;
		uint64_t m_LastReceiveMs = 0;
		uint64_t m_SessionClockBaseMs = 0;
		bool m_PeerStatePending = false;
		bool m_LocalReady = false;
		bool m_ReadySent = false;
		bool m_StartRequested = false;
		std::string m_FailureReason;
		std::vector<uint8_t> m_RemotePeerIds; //!< Every remote lockstep peerId; derived at Start.
		std::map<uint8_t, NetPeerId> m_RemoteTransports; //!< Lockstep peerId -> transport id for each remote.
		std::map<uint8_t, bool> m_ConfigAckedByPeer; //!< Host: which remotes accepted the config.
		std::map<uint8_t, bool> m_RemoteReadyByPeer; //!< Which peers are ready, from direct or relayed peer-state.
		std::map<uint8_t, std::string> m_RemoteNamesByPeer; //!< Peer display names from periodic peer-state.
		std::map<uint8_t, uint32_t> m_RemotePingByPeer; //!< Peer pings; the host stamps relayed states with its measurement.
		std::map<uint8_t, std::string> m_RemotePlatformsByPeer;
		std::set<uint8_t> m_RemoteLobbyUp; //!< Remotes that have sent a lobby message of their own.
		std::set<uint8_t> m_WorldTransferPeers; //!< World bootstraps the host bound; no lobby-up gate.
		bool m_SeatAssigned = false;       //!< Client: the host has named the id it bound to this connection.
		std::vector<uint8_t> m_StateBytesToSend;
		uint64_t m_OutgoingStateId = 0;
		uint8_t m_StateTransferOnlyPeer = 0; //!< 0 = every remote (resync); else the one world joiner.
		std::map<uint8_t, uint16_t> m_OutgoingChunkIndexByPeer; //!< Next chunk each remote still needs.
		std::vector<std::pair<uint8_t, std::vector<uint8_t>>> m_QueuedStateTransfers; //!< Joiner images waiting for the pump.
		uint16_t m_OutgoingChunkCount = 0;
		uint32_t m_ChunkSendStall = 0; //!< Consecutive ticks the transport refused a chunk (backpressure).
		uint64_t m_StateTransferProgressSerial = 0;
		uint64_t m_IncomingStateId = 0; //!< The active incoming transfer, 0 = none.
		uint64_t m_LastIncomingStateId = 0;
		uint32_t m_IncomingTotalBytes = 0;
		uint32_t m_IncomingReceivedBytes = 0;
		uint16_t m_IncomingChunkCount = 0;
		uint16_t m_IncomingNextChunkIndex = 0;
		bool m_IncomingStateComplete = false;
		std::vector<uint8_t> m_ReceivedState;
		WorldJoinReport m_WorldJoinReport;
		std::vector<uint8_t> m_PendingTailBytes;
		NetLobbyStats m_Stats;
	};

} // namespace RTE
