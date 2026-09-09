#pragma once

#include "NetIdentity.h"
#include "NetProtocol.h"
#include "NetReconnectSession.h"
#include "NetTransport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RTE {

	enum class NetSessionRole : uint8_t {
		None = 0,
		Host = 1,
		Client = 2,
	};

	enum class NetSessionState : uint16_t {
		Stopped = 0,
		Listening = 1,
		Connecting = 2,
		HelloSent = 3,
		Handshake = 4,
		Accepted = 5,
		Ready = 6,
		Rejected = 7,
		Closed = 8,
		Failed = 9,
	};

	struct NetSessionConfig {
		NetIdentityManifest localIdentity;
		std::string displayName = "Player";
		uint16_t port = 41010;
		uint64_t sessionId = 0x5354414745325032ULL;
		uint64_t localNonce = 0x43434D504E4F4E43ULL;
		uint8_t maxPeers = 1;
		uint32_t heartbeatIntervalMs = 100;
		uint32_t timeoutMs = 1000;
		uint16_t minProtocolVersion = NetProtocol::c_Version;
		uint16_t maxProtocolVersion = NetProtocol::c_Version;
		bool rejectUserdataModules = true;
	};

	struct NetSessionStats {
		uint32_t sentMessages = 0;
		uint32_t receivedMessages = 0;
		uint32_t malformedMessages = 0;
		uint32_t ignoredPhasePackets = 0;
		uint32_t timeouts = 0;
		uint32_t timeoutResumptions = 0; //!< Evaluations skipped because the caller stopped feeding the session for longer than the budget.
		uint32_t unboundConnectionFaults = 0; //!< Host: per-connection transport faults ignored so a joiner cannot fail the session for everyone.
		uint32_t unauthenticatedConnectionsRefused = 0; //!< Host: connections refused because the half-open bound was already full.
		uint32_t fencedPackets = 0; //!< Host: packets from a superseded incarnation of a seat, dropped for it.
		uint32_t fencedDisconnects = 0; //!< Host: a dead incarnation timing out, which must not evict the seat.
		uint32_t admissionMessages = 0; //!< H4 admission messages handed to the reconnect plane.
		uint32_t oldWireRejectionsSent = 0; //!< Host: explicit rejections stamped at the peer's own header version (§10).
		uint32_t oldWireDisconnects = 0; //!< Host: old-wire peers whose version we cannot answer in, disconnected with the reason text.
		uint32_t pendingAdmissionJoins = 0; //!< Host: mid-match joiners admitted on a provisional id to prove a ticket on (§6).
	};

	// A connected peer as seen by the match runner: its transport id and session-assigned id.
	struct NetSessionPeerInfo {
		NetPeerId transportPeerId = c_InvalidNetPeerId;
		uint8_t assignedPeerId = 0;
		std::string displayName;
		bool ready = false;
	};

	class NetSession {
	public:
		// Twice the peer cap, so a full lobby plus a reconnect attempt per seat all fit while an
		// unauthenticated connection still cannot make the host track an unbounded number of them.
		static constexpr uint32_t c_MaxUnauthenticatedPeers = 8;
		// §6: a ticket holder can arrive while the incarnation it supersedes still holds the seat's peer
		// id, so a live match keeps this many ids past the peer cap for joiners that have yet to prove.
		static constexpr uint8_t c_MaxPendingAdmissions = 4;

		bool StartHost(INetTransport& transport, NetSessionConfig config, std::string* error = nullptr);
		bool StartClient(INetTransport& transport, const std::string& address, NetSessionConfig config, std::string* error = nullptr);
		void Tick(uint64_t nowMs, bool pollTransport = true);
		uint64_t GetClockMs() const { return m_NowMs; }
		/// Sends session heartbeats without polling the transport or checking timeouts, so another
		/// phase (the lobby) can own the shared event queue while peers still see us alive.
		void TickKeepalive(uint64_t nowMs);
		/// Feeds one transport event when another phase owns the queue (a reconnect handshake the
		/// lockstep coordinator hands over mid-match).
		void InjectEvent(const NetTransportEvent& event, uint64_t nowMs);
		/// Advances the admission plane alone: mid-match the coordinator owns the transport queue and the
		/// heartbeats, so this must not poll either or a peer whose traffic rides the round would time out.
		void TickAdmissionPlane(uint64_t nowMs);
		void Close(const std::string& reason);
		/// Ends the hosted session for every peer with the one reason that lets a client delete its
		/// recovery record. Sent from the same place the seat registry is cleared, and nowhere else.
		void EndHostedSession(const std::string& reason);

		/// Attaches the H4 admission plane. Without one the session behaves exactly as it did before
		/// reconnect existed: an admission message is an unexpected handshake message.
		void SetReconnectHost(NetReconnectHost* host) { m_ReconnectHost = host; }
		void SetReconnectClient(NetReconnectClient* client) { m_ReconnectClient = client; }
		NetReconnectHost* GetReconnectHost() const { return m_ReconnectHost; }
		NetReconnectClient* GetReconnectClient() const { return m_ReconnectClient; }
		/// The frame a seat drop is recorded against; the match runner keeps it current.
		void SetLockstepFrame(uint64_t frame) { m_LockstepFrame = frame; }

		NetSessionRole GetRole() const { return m_Role; }
		NetSessionState GetState() const { return m_State; }
		uint64_t GetSessionId() const { return m_SessionId; }
		uint8_t GetLocalPeerId() const { return m_LocalPeerId; }
		NetPeerId GetRemoteTransportPeerId() const;

		/// Enumerates the peers on the far end of the wire for the match runner.
		/// Host: every Ready client (its transport + session-assigned id). Client: the host as peer id 0.
		std::vector<NetSessionPeerInfo> GetReadyPeers() const;
		/// The number of Ready peers on the far end (host: connected clients; client: 0 or 1).
		uint32_t GetReadyPeerCount() const;
		NetRejectReason GetRejectReason() const { return m_RejectReason; }
		const std::string& GetMismatchKey() const { return m_MismatchKey; }
		bool HasReject() const { return m_HasReject; }
		const std::string& GetRejectSummary() const { return m_RejectSummary; }
		const NetSessionStats& GetStats() const { return m_Stats; }
		/// The number of connections the host is tracking that have not yet passed a ClientHello.
		uint32_t GetUnauthenticatedPeerCount() const;

		/// Builds a one-line human-readable reject/failure reason from the recorded mismatch,
		/// e.g. "deterministic config hash does not match (deterministic_config_hash: 4d31cc89.. vs 77ab01ff..)".
		/// @return The reason text, or an empty string when nothing was rejected.
		std::string BuildRejectText() const;

		bool IsReady() const { return m_State == NetSessionState::Ready; }
		bool IsRejected() const { return m_State == NetSessionState::Rejected; }
		bool IsClosed() const { return m_State == NetSessionState::Closed; }
		bool IsFailed() const { return m_State == NetSessionState::Failed; }

		std::string BuildReportJson() const;

		static const char* RoleName(NetSessionRole role);
		static const char* StateName(NetSessionState state);

	private:
		struct PeerState {
			NetPeerId transportPeerId = c_InvalidNetPeerId;
			uint8_t assignedPeerId = 0;
			uint64_t clientNonce = 0;
			std::string displayName;
			NetSessionState state = NetSessionState::Handshake;
			uint64_t connectedAtMs = 0;
			uint64_t lastReceiveMs = 0;
			uint64_t lastHeartbeatMs = 0;
			NetHash32 identityHash{};
		};

		bool Send(NetPeerId peerId, NetPayload payload, std::string* error = nullptr);
		void SendHeartbeat(NetPeerId peerId);
		void MaybeSendHeartbeats();
		void ProcessEvent(const NetTransportEvent& event);
		void ProcessPacket(NetPeerId peerId, const std::vector<uint8_t>& bytes);
		void HandleMalformed(NetPeerId peerId, const NetProtocolError& decodeError, const std::vector<uint8_t>& bytes);
		/// Host: answers a peer whose header version we do not speak, explicitly when the envelope
		/// allows it and otherwise with a documented best-effort disconnect (§10).
		void RejectOldWirePeer(NetPeerId peerId, const std::vector<uint8_t>& bytes);
		void HandleHostMessage(NetPeerId peerId, const NetMessage& message);
		void HandleClientMessage(NetPeerId peerId, const NetMessage& message);
		void CheckTimeouts();
		void RejectPeer(PeerState& peer, NetRejectReason reason, const std::string& key, const std::string& expected, const std::string& actual, const std::string& summary);
		void RejectConnection(NetPeerId peerId, NetRejectReason reason, const std::string& key, const std::string& expected, const std::string& actual, const std::string& summary);
		void RecordReject(NetRejectReason reason, const std::string& key, const std::string& expected, const std::string& actual, const std::string& summary);
		void SetRejected(NetRejectReason reason, const std::string& key, const std::string& expected, const std::string& actual, const std::string& summary);
		void SetFailed(NetRejectReason reason, const std::string& key, const std::string& expected, const std::string& actual, const std::string& summary);
		PeerState* FindPeer(NetPeerId peerId);
		const PeerState* FindPeer(NetPeerId peerId) const;
		uint32_t ActivePeerCount() const;
		/// Drops connections that never sent a ClientHello within the handshake timeout, counted from the
		/// connection, not from its last packet - a peer that heartbeats without ever saying hello holds a
		/// slot otherwise. A live match runs this and nothing else: a seated player's traffic rides the
		/// round, so its receive clock is stale by design and the heartbeat check would evict it.
		void ExpireSilentHandshakes();
		uint8_t AllocatePeerId() const;
		/// An id past the peer cap for a mid-match joiner to run its admission transaction on; a commit
		/// replaces it with the seat's own id. Zero outside a live match, or when the range is full.
		uint8_t AllocatePendingAdmissionPeerId() const;
		/// Host: closes a peer's transport ourselves. The admission plane only ever hears about a drop
		/// through the transport's own event, so a peer we hang up on must be handed to it here or its
		/// seat is held forever and the next joiner is refused a full lobby.
		void DropPeerTransport(NetPeerId peerId, const std::string& reason);
		void RefreshHostState();

		NetClientHello BuildClientHello() const;
		NetHostHello BuildHostHello(uint8_t assignedPeerId) const;
		NetJoinAccepted BuildJoinAccepted(uint8_t assignedPeerId) const;
		NetReadyState BuildReadyState(bool ready) const;
		/// @return Whether the payload was an admission message the reconnect plane took.
		bool RouteAdmissionMessage(NetPeerId peerId, const NetPayload& payload);
		void FlushReconnectOutbound();
		/// Client: turns a committed admission transaction into Ready on the seat's own peer id, and a
		/// settled-but-uncommitted one into a legible session failure.
		void CompleteClientAdmission();

		NetIdentityMismatch ValidateClientHello(const NetClientHello& hello) const;
		NetIdentityMismatch ValidateHostHello(const NetHostHello& hello) const;

		static uint8_t PlatformId(const std::string& platform);

		INetTransport* m_Transport = nullptr;
		NetSessionConfig m_Config;
		NetSessionRole m_Role = NetSessionRole::None;
		NetSessionState m_State = NetSessionState::Stopped;
		uint64_t m_NowMs = 0;
		uint64_t m_StateStartedMs = 0;
		uint64_t m_LastReceiveMs = 0;
		uint64_t m_LastTimeoutCheckMs = 0;
		bool m_TimeoutsEvaluated = false;
		uint64_t m_NextHeartbeatMs = 0;
		uint64_t m_SessionId = 0;
		NetPeerId m_RemoteTransportPeerId = c_InvalidNetPeerId;
		uint8_t m_LocalPeerId = 0;
		uint32_t m_NextSequence = 0;
		uint32_t m_LastReceivedSequence = 0;
		NetRejectReason m_RejectReason = NetRejectReason::InternalError;
		bool m_HasReject = false;
		std::string m_MismatchKey;
		std::string m_ExpectedValue;
		std::string m_ActualValue;
		std::string m_RejectSummary;
		NetHash32 m_RemoteIdentityHash{};
		bool m_HasRemoteIdentityHash = false;
		NetSessionStats m_Stats;
		NetReconnectHost* m_ReconnectHost = nullptr;
		NetReconnectClient* m_ReconnectClient = nullptr;
		uint64_t m_LockstepFrame = 0;
		std::vector<PeerState> m_Peers;
	};

} // namespace RTE
