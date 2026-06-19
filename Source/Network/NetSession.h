#pragma once

#include "NetIdentity.h"
#include "NetProtocol.h"
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
		uint32_t timeouts = 0;
	};

	class NetSession {
	public:
		bool StartHost(INetTransport& transport, NetSessionConfig config, std::string* error = nullptr);
		bool StartClient(INetTransport& transport, const std::string& address, NetSessionConfig config, std::string* error = nullptr);
		void Tick(uint64_t nowMs);
		void Close(const std::string& reason);

		NetSessionRole GetRole() const { return m_Role; }
		NetSessionState GetState() const { return m_State; }
		uint64_t GetSessionId() const { return m_SessionId; }
		uint8_t GetLocalPeerId() const { return m_LocalPeerId; }
		NetRejectReason GetRejectReason() const { return m_RejectReason; }
		const std::string& GetMismatchKey() const { return m_MismatchKey; }
		const NetSessionStats& GetStats() const { return m_Stats; }

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
		void HandleMalformed(NetPeerId peerId, const NetProtocolError& decodeError);
		void HandleHostMessage(NetPeerId peerId, const NetMessage& message);
		void HandleClientMessage(NetPeerId peerId, const NetMessage& message);
		void CheckTimeouts();
		void RejectPeer(PeerState& peer, NetRejectReason reason, const std::string& key, const std::string& expected, const std::string& actual, const std::string& summary);
		void RecordReject(NetRejectReason reason, const std::string& key, const std::string& expected, const std::string& actual, const std::string& summary);
		void SetRejected(NetRejectReason reason, const std::string& key, const std::string& expected, const std::string& actual, const std::string& summary);
		void SetFailed(NetRejectReason reason, const std::string& key, const std::string& expected, const std::string& actual, const std::string& summary);
		PeerState* FindPeer(NetPeerId peerId);
		const PeerState* FindPeer(NetPeerId peerId) const;
		uint32_t ActivePeerCount() const;
		uint8_t AllocatePeerId() const;
		void RefreshHostState(NetSessionState terminalState);

		NetClientHello BuildClientHello() const;
		NetHostHello BuildHostHello(uint8_t assignedPeerId) const;
		NetJoinAccepted BuildJoinAccepted(uint8_t assignedPeerId) const;
		NetReadyState BuildReadyState(bool ready) const;
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
		std::vector<PeerState> m_Peers;
	};

} // namespace RTE
