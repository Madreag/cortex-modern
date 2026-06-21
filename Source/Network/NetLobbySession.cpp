#include "NetLobbySession.h"

#include "NetIdentity.h"
#include "NetProtocol.h"

#include "nlohmann/json.hpp"

#include <type_traits>
#include <utility>

namespace RTE {

	namespace {
		using json = nlohmann::json;

		std::string HashText(const NetHash32& hash) {
			return NetIdentity::HashHex(hash);
		}

		bool IsTerminal(NetLobbyState state) {
			return state == NetLobbyState::Started || state == NetLobbyState::Rejected || state == NetLobbyState::Failed;
		}
	}

	bool NetLobbySession::Start(INetTransport& transport, const NetLobbySessionConfig& config, std::string* error) {
		if (config.remoteTransportPeerId == c_InvalidNetPeerId) {
			if (error) *error = "lobby remote transport peer id must be valid";
			return false;
		}
		if (config.localPeerId == 0 || config.remotePeerId == 0 || config.localPeerId == config.remotePeerId) {
			if (error) *error = "lobby peer ids are invalid";
			return false;
		}
		std::string validateError;
		if (!NetMatchConfigUtil::ValidateLocalAlpha(config.matchConfig, &validateError)) {
			if (error) *error = validateError;
			return false;
		}

		m_Transport = &transport;
		m_Config = config;
		m_State = config.host ? NetLobbyState::WaitingForConfigAck : NetLobbyState::WaitingForConfig;
		m_MatchConfigHash = NetMatchConfigUtil::HashConfig(m_Config.matchConfig);
		m_StartFrame = config.startFrame;
		m_LastConfigSentMs = 0;
		m_LastPeerStateSentMs = 0;
		m_LastReceiveMs = 0;
		m_ConfigAcked = false;
		m_LocalReady = config.host || config.autoReady;
		m_ReadySent = false;
		m_RemoteReady = false;
		m_StartRequested = config.autoStart;
		m_FailureReason.clear();
		m_Stats = {};

		SendPeerState();
		if (m_Config.host) {
			SendConfigIfDue(0);
		}
		return true;
	}

	void NetLobbySession::Tick(uint64_t nowMs) {
		if (!m_Transport || m_State == NetLobbyState::Idle || IsTerminal(m_State)) {
			return;
		}
		for (const NetTransportEvent& event : m_Transport->PollEvents()) {
			if (IsTerminal(m_State)) {
				return;
			}
			HandleEvent(event, nowMs);
		}
		if (IsTerminal(m_State)) {
			return;
		}
		SendReadyIfNeeded();
		if (IsTerminal(m_State)) {
			return;
		}
		SendPeerStateIfDue(nowMs);
		if (IsTerminal(m_State)) {
			return;
		}
		if (m_Config.host) {
			SendConfigIfDue(nowMs);
			SendStartIfReady();
		}
		if (m_Config.timeoutMs > 0 && nowMs >= m_LastReceiveMs && nowMs - m_LastReceiveMs > m_Config.timeoutMs) {
			++m_Stats.timeouts;
			Fail("lobby timed out");
		}
	}

	std::string NetLobbySession::BuildReportJson() const {
		json report{
			{"state", StateName(m_State)},
			{"role", m_Config.host ? "host" : "client"},
			{"local_peer_id", static_cast<int>(m_Config.localPeerId)},
			{"remote_peer_id", static_cast<int>(m_Config.remotePeerId)},
			{"match_config_hash", HashText(m_MatchConfigHash)},
			{"start_frame", m_StartFrame},
			{"failure_reason", m_FailureReason},
			{"config_acked", m_ConfigAcked},
			{"local_ready", m_LocalReady},
			{"remote_ready", m_RemoteReady},
			{"start_requested", m_StartRequested},
			{"match_config", json::parse(NetMatchConfigUtil::BuildReportJson(m_Config.matchConfig))},
			{"stats", {
				{"messages_sent", m_Stats.messagesSent},
				{"messages_received", m_Stats.messagesReceived},
				{"malformed_messages", m_Stats.malformedMessages},
				{"ignored_session_packets", m_Stats.ignoredSessionPackets},
				{"config_packets_sent", m_Stats.configPacketsSent},
				{"config_acks_received", m_Stats.configAcksReceived},
				{"ready_packets_received", m_Stats.readyPacketsReceived},
				{"start_packets_sent", m_Stats.startPacketsSent},
				{"start_packets_received", m_Stats.startPacketsReceived},
				{"timeouts", m_Stats.timeouts},
			}},
		};
		return report.dump();
	}

	void NetLobbySession::SetLocalReady(bool ready) {
		if (IsTerminal(m_State)) {
			return;
		}
		m_LocalReady = ready;
		if (!ready) {
			m_ReadySent = false;
			return;
		}
		SendReadyIfNeeded();
	}

	void NetLobbySession::RequestStart() {
		if (IsTerminal(m_State)) {
			return;
		}
		m_StartRequested = true;
		SendStartIfReady();
	}

	const char* NetLobbySession::StateName(NetLobbyState state) {
		switch (state) {
			case NetLobbyState::Idle: return "Idle";
			case NetLobbyState::WaitingForConfig: return "WaitingForConfig";
			case NetLobbyState::WaitingForConfigAck: return "WaitingForConfigAck";
			case NetLobbyState::WaitingForReady: return "WaitingForReady";
			case NetLobbyState::Started: return "Started";
			case NetLobbyState::Rejected: return "Rejected";
			case NetLobbyState::Failed: return "Failed";
		}
		return "Unknown";
	}

	bool NetLobbySession::Send(NetLobbyPayload payload, std::string* error) {
		if (!m_Transport) {
			if (error) *error = "lobby has no transport";
			return false;
		}
		std::vector<uint8_t> bytes;
		NetLobbyError encodeError;
		if (!NetLobbyProtocol::Encode({std::move(payload)}, bytes, &encodeError)) {
			if (error) *error = encodeError.message;
			return false;
		}
		if (!m_Transport->Send(m_Config.remoteTransportPeerId, NetTransportLane::ControlReliable, bytes, error)) {
			return false;
		}
		++m_Stats.messagesSent;
		return true;
	}

	void NetLobbySession::SendConfigIfDue(uint64_t nowMs) {
		if (!m_Config.host || m_ConfigAcked || m_State != NetLobbyState::WaitingForConfigAck) {
			return;
		}
		if (m_Stats.configPacketsSent > 0 && nowMs < m_LastConfigSentMs + m_Config.resendIntervalMs) {
			return;
		}
		std::string error;
		if (Send(NetLobbyMatchConfig{m_Config.matchConfig}, &error)) {
			++m_Stats.configPacketsSent;
			m_LastConfigSentMs = nowMs;
		} else {
			Fail(error);
		}
	}

	void NetLobbySession::SendPeerState() {
		NetLobbyPeerState state;
		state.peerId = m_Config.localPeerId;
		state.ready = m_LocalReady;
		state.displayName = m_Config.displayName;
		state.platform = m_Config.platform;
		std::string error;
		if (!Send(std::move(state), &error)) {
			Fail(error);
		}
	}

	void NetLobbySession::SendPeerStateIfDue(uint64_t nowMs) {
		if (m_Config.peerStateIntervalMs == 0 || nowMs < m_LastPeerStateSentMs + m_Config.peerStateIntervalMs) {
			return;
		}
		SendPeerState();
		m_LastPeerStateSentMs = nowMs;
	}

	void NetLobbySession::SendReadyIfNeeded() {
		if (m_Config.host || !m_LocalReady || m_ReadySent || m_State != NetLobbyState::WaitingForReady) {
			return;
		}
		std::string error;
		if (Send(NetLobbyReady{m_Config.localPeerId, true}, &error)) {
			m_ReadySent = true;
		} else {
			Fail(error);
		}
	}

	void NetLobbySession::SendStartIfReady() {
		if (!m_Config.host || !m_ConfigAcked || !m_RemoteReady || !m_StartRequested || IsTerminal(m_State)) {
			return;
		}
		NetLobbyStart start;
		start.sessionId = m_Config.matchConfig.sessionId;
		start.startFrame = m_StartFrame;
		start.inputDelayFrames = m_Config.matchConfig.inputDelayFrames;
		start.matchConfigHash = m_MatchConfigHash;
		std::string error;
		if (!Send(start, &error)) {
			Fail(error);
			return;
		}
		++m_Stats.startPacketsSent;
		m_State = NetLobbyState::Started;
	}

	void NetLobbySession::HandleEvent(const NetTransportEvent& event, uint64_t nowMs) {
		switch (event.type) {
			case NetTransportEventType::PeerConnected:
				break;
			case NetTransportEventType::PeerDisconnected:
				Fail(event.reason.empty() ? "peer disconnected" : event.reason);
				break;
			case NetTransportEventType::ConnectionFailed:
			case NetTransportEventType::TransportError:
				Fail(event.reason.empty() ? "transport error" : event.reason);
				break;
			case NetTransportEventType::PacketReceived: {
				m_LastReceiveMs = nowMs;
				const NetLobbyDecodeResult decoded = NetLobbyProtocol::Decode(event.bytes);
				if (!decoded.ok) {
					if (decoded.error.code == NetLobbyErrorCode::BadMagic && NetProtocol::Decode(event.bytes).ok) {
						++m_Stats.ignoredSessionPackets;
						return;
					}
					++m_Stats.malformedMessages;
					Fail(decoded.error.message);
					return;
				}
				++m_Stats.messagesReceived;
				HandleMessage(decoded.message);
				break;
			}
		}
	}

	void NetLobbySession::HandleMessage(const NetLobbyMessage& message) {
		std::visit([&](const auto& payload) {
			using Payload = std::decay_t<decltype(payload)>;
			if constexpr (std::is_same_v<Payload, NetLobbyMatchConfig>) {
				HandleMatchConfig(payload);
			} else if constexpr (std::is_same_v<Payload, NetLobbyConfigAck>) {
				HandleConfigAck(payload);
			} else if constexpr (std::is_same_v<Payload, NetLobbyReady>) {
				HandleReady(payload);
			} else if constexpr (std::is_same_v<Payload, NetLobbyStart>) {
				HandleStart(payload);
			} else if constexpr (std::is_same_v<Payload, NetLobbyAbort>) {
				if (payload.peerId == m_Config.remotePeerId) {
					Reject(payload.reason);
				}
			}
		}, message.payload);
	}

	void NetLobbySession::HandleMatchConfig(const NetLobbyMatchConfig& message) {
		if (m_Config.host) {
			return;
		}
		std::string validateError;
		if (!NetMatchConfigUtil::ValidateLocalAlpha(message.config, &validateError)) {
			Send(NetLobbyConfigAck{m_Config.localPeerId, false, NetMatchConfigUtil::HashConfig(message.config), validateError});
			Reject(validateError);
			return;
		}
		m_Config.matchConfig = message.config;
		m_MatchConfigHash = NetMatchConfigUtil::HashConfig(m_Config.matchConfig);
		m_StartFrame = 0;
		Send(NetLobbyConfigAck{m_Config.localPeerId, true, m_MatchConfigHash, ""});
		m_State = NetLobbyState::WaitingForReady;
		SendReadyIfNeeded();
	}

	void NetLobbySession::HandleConfigAck(const NetLobbyConfigAck& message) {
		if (!m_Config.host || message.peerId != m_Config.remotePeerId) {
			return;
		}
		++m_Stats.configAcksReceived;
		if (!message.accepted) {
			Reject(message.reason.empty() ? "match config rejected" : message.reason);
			return;
		}
		if (message.matchConfigHash != m_MatchConfigHash) {
			Reject("match config hash mismatch");
			return;
		}
		m_ConfigAcked = true;
		m_State = NetLobbyState::WaitingForReady;
	}

	void NetLobbySession::HandleReady(const NetLobbyReady& message) {
		if (!m_Config.host || message.peerId != m_Config.remotePeerId) {
			return;
		}
		++m_Stats.readyPacketsReceived;
		m_RemoteReady = message.ready;
	}

	void NetLobbySession::HandleStart(const NetLobbyStart& message) {
		if (m_Config.host) {
			return;
		}
		++m_Stats.startPacketsReceived;
		if (message.sessionId != m_Config.matchConfig.sessionId ||
		    message.inputDelayFrames != m_Config.matchConfig.inputDelayFrames ||
		    message.matchConfigHash != m_MatchConfigHash) {
			Reject("lobby start does not match accepted config");
			return;
		}
		m_StartFrame = message.startFrame;
		m_State = NetLobbyState::Started;
	}

	void NetLobbySession::Reject(const std::string& reason) {
		if (IsTerminal(m_State)) {
			return;
		}
		m_State = NetLobbyState::Rejected;
		m_FailureReason = reason;
		if (m_Transport) {
			std::string ignored;
			(void)Send(NetLobbyAbort{m_Config.localPeerId, reason}, &ignored);
		}
	}

	void NetLobbySession::Fail(const std::string& reason) {
		if (IsTerminal(m_State)) {
			return;
		}
		m_State = NetLobbyState::Failed;
		m_FailureReason = reason;
	}

} // namespace RTE
