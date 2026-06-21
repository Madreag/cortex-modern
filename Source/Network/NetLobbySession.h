#pragma once

#include "NetLobbyProtocol.h"
#include "NetTransport.h"

#include <cstdint>
#include <string>

namespace RTE {

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
		bool host = false;
		uint8_t localPeerId = 0;
		uint8_t remotePeerId = 0;
		NetPeerId remoteTransportPeerId = c_InvalidNetPeerId;
		NetMatchConfig matchConfig;
		uint64_t startFrame = 0;
		uint32_t resendIntervalMs = 250;
		uint32_t peerStateIntervalMs = 1000;
		uint32_t timeoutMs = 5000;
		std::string displayName = "Player";
		std::string platform = "unknown";
		bool autoReady = true;
		bool autoStart = true;
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
		bool IsRemoteReady() const { return m_RemoteReady; }
		bool IsStartRequested() const { return m_StartRequested; }
		const NetMatchConfig& GetMatchConfig() const { return m_Config.matchConfig; }
		const NetHash32& GetMatchConfigHash() const { return m_MatchConfigHash; }
		uint64_t GetStartFrame() const { return m_StartFrame; }
		const std::string& GetFailureReason() const { return m_FailureReason; }
		const NetLobbyStats& GetStats() const { return m_Stats; }

		void SetLocalReady(bool ready);
		void RequestStart();

		std::string BuildReportJson() const;

		static const char* StateName(NetLobbyState state);

	private:
		bool Send(NetLobbyPayload payload, std::string* error = nullptr);
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
		void Reject(const std::string& reason);
		void Fail(const std::string& reason);

		INetTransport* m_Transport = nullptr;
		NetLobbySessionConfig m_Config;
		NetLobbyState m_State = NetLobbyState::Idle;
		NetHash32 m_MatchConfigHash{};
		uint64_t m_StartFrame = 0;
		uint64_t m_LastConfigSentMs = 0;
		uint64_t m_LastPeerStateSentMs = 0;
		uint64_t m_LastReceiveMs = 0;
		bool m_ConfigAcked = false;
		bool m_LocalReady = false;
		bool m_ReadySent = false;
		bool m_RemoteReady = false;
		bool m_StartRequested = false;
		std::string m_FailureReason;
		NetLobbyStats m_Stats;
	};

} // namespace RTE
