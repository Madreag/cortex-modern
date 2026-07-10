#include "NetLobbySession.h"

#include "NetIdentity.h"
#include "NetLockstep.h"
#include "NetProtocol.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <iostream>
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

		const std::string& EmptyName() {
			static const std::string empty;
			return empty;
		}
	}

	bool NetLobbySession::Start(INetTransport& transport, const NetLobbySessionConfig& config, std::string* error) {
		if (config.localPeerId == 0) {
			if (error) *error = "lobby local peer id is invalid";
			return false;
		}

		// Derive the remote set from the N-peer map, or the 2-peer convenience pair.
		m_RemotePeerIds.clear();
		m_RemoteTransports.clear();
		m_ConfigAckedByPeer.clear();
		m_RemoteReadyByPeer.clear();
		m_RemoteNamesByPeer.clear();
		m_RemotePingByPeer.clear();
		if (!config.remoteTransportPeerIds.empty()) {
			for (const auto& [peerId, transportId] : config.remoteTransportPeerIds) {
				m_RemotePeerIds.push_back(peerId);
				m_RemoteTransports[peerId] = transportId;
			}
		} else {
			m_RemotePeerIds.push_back(config.remotePeerId);
			m_RemoteTransports[config.remotePeerId] = config.remoteTransportPeerId;
		}
		for (uint8_t peerId : m_RemotePeerIds) {
			if (peerId == 0 || peerId == config.localPeerId) {
				if (error) *error = "lobby peer ids are invalid";
				return false;
			}
			if (m_RemoteTransports[peerId] == c_InvalidNetPeerId) {
				if (error) *error = "lobby remote transport peer id must be valid";
				return false;
			}
			m_ConfigAckedByPeer[peerId] = false;
			m_RemoteReadyByPeer[peerId] = false;
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
		m_LocalReady = config.host || config.autoReady;
		m_ReadySent = false;
		m_StartRequested = config.autoStart;
		m_FailureReason.clear();
		m_OutgoingChunks.clear();
		m_ChunkSendStall = 0;
		m_IncomingStateId = 0;
		m_IncomingTotalBytes = 0;
		m_IncomingReceivedBytes = 0;
		m_IncomingChunks.clear();
		m_IncomingStateComplete = false;
		m_ReceivedState.clear();
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
			SendQueuedStateChunks();
			SendStartIfReady();
		}
		if (m_Config.timeoutMs > 0 && nowMs >= m_LastReceiveMs && nowMs - m_LastReceiveMs > m_Config.timeoutMs) {
			++m_Stats.timeouts;
			Fail("lobby timed out");
		}
	}

	void NetLobbySession::BeginStateTransfer(std::vector<uint8_t> fileBytes) {
		if (!m_Config.host || fileBytes.empty() || IsTerminal(m_State)) {
			return;
		}
		const uint32_t totalBytes = static_cast<uint32_t>(fileBytes.size());
		const uint16_t chunkCount = static_cast<uint16_t>((fileBytes.size() + NetLobbyProtocol::c_MaxStateChunkBytes - 1) / NetLobbyProtocol::c_MaxStateChunkBytes);
		// Steady-clock ns would be nicer but the id only disambiguates transfers within one lobby round.
		const uint64_t transferId = 0x50355354ULL ^ totalBytes ^ (static_cast<uint64_t>(chunkCount) << 32);
		m_OutgoingChunks.clear();
		for (uint16_t index = 0; index < chunkCount; ++index) {
			NetLobbyStateChunk chunk;
			chunk.transferId = transferId;
			chunk.totalBytes = totalBytes;
			chunk.chunkIndex = index;
			chunk.chunkCount = chunkCount;
			const size_t begin = static_cast<size_t>(index) * NetLobbyProtocol::c_MaxStateChunkBytes;
			const size_t end = std::min(fileBytes.size(), begin + NetLobbyProtocol::c_MaxStateChunkBytes);
			chunk.bytes.assign(fileBytes.begin() + begin, fileBytes.begin() + end);
			m_OutgoingChunks.push_back(std::move(chunk));
		}
	}

	std::vector<uint8_t> NetLobbySession::TakeReceivedState() {
		m_IncomingStateComplete = false;
		m_IncomingStateId = 0;
		return std::move(m_ReceivedState);
	}

	void NetLobbySession::SendQueuedStateChunks() {
		// The transport's reliable send buffer backpressures a bulk stream: a refused chunk just
		// waits for the next tick, and only a long stretch of zero progress is a real failure.
		int budget = 2;
		while (!m_OutgoingChunks.empty() && budget-- > 0) {
			std::string error;
			if (!Send(m_OutgoingChunks.front(), &error)) {
				if (++m_ChunkSendStall > 4000) {
					Fail("state transfer stalled: " + error);
				}
				return;
			}
			m_ChunkSendStall = 0;
			m_OutgoingChunks.pop_front();
		}
	}

	void NetLobbySession::HandleStateChunk(const NetLobbyStateChunk& message) {
		if (m_Config.host) {
			return;
		}
		if (m_IncomingStateId != message.transferId) {
			// A new transfer supersedes any partial one.
			m_IncomingStateId = message.transferId;
			m_IncomingTotalBytes = message.totalBytes;
			m_IncomingReceivedBytes = 0;
			m_IncomingChunks.clear();
			m_IncomingStateComplete = false;
			m_ReceivedState.clear();
		}
		if (m_IncomingStateComplete || m_IncomingChunks.find(message.chunkIndex) != m_IncomingChunks.end()) {
			return;
		}
		m_IncomingReceivedBytes += static_cast<uint32_t>(message.bytes.size());
		m_IncomingChunks[message.chunkIndex] = message.bytes;
		if (m_IncomingChunks.size() < message.chunkCount) {
			return;
		}
		m_ReceivedState.clear();
		m_ReceivedState.reserve(m_IncomingTotalBytes);
		for (const auto& [index, bytes]: m_IncomingChunks) {
			m_ReceivedState.insert(m_ReceivedState.end(), bytes.begin(), bytes.end());
		}
		m_IncomingChunks.clear();
		if (m_ReceivedState.size() != m_IncomingTotalBytes) {
			Fail("state transfer size mismatch");
			return;
		}
		m_IncomingStateComplete = true;
		std::cout << "[net-match] state transfer complete: " << m_ReceivedState.size() << " bytes" << std::endl;
	}

	bool NetLobbySession::IsRemoteReady(uint8_t peerId) const {
		const auto it = m_RemoteReadyByPeer.find(peerId);
		return it != m_RemoteReadyByPeer.end() && it->second;
	}

	bool NetLobbySession::HasHeardFrom(uint8_t peerId) const {
		return m_RemoteNamesByPeer.find(peerId) != m_RemoteNamesByPeer.end();
	}

	uint32_t NetLobbySession::GetRemotePingMs(uint8_t peerId) const {
		const auto it = m_RemotePingByPeer.find(peerId);
		return it != m_RemotePingByPeer.end() ? it->second : 0;
	}

	const std::string& NetLobbySession::GetRemoteName() const {
		if (m_RemotePeerIds.empty()) {
			return EmptyName();
		}
		return GetRemoteName(m_RemotePeerIds.front());
	}

	const std::string& NetLobbySession::GetRemoteName(uint8_t peerId) const {
		const auto it = m_RemoteNamesByPeer.find(peerId);
		return it != m_RemoteNamesByPeer.end() ? it->second : EmptyName();
	}

	std::string NetLobbySession::BuildReportJson() const {
		json remotePeers = json::array();
		for (uint8_t peerId : m_RemotePeerIds) {
			remotePeers.push_back(json{
				{"peer_id", static_cast<int>(peerId)},
				{"config_acked", AllConfigAcked() || (m_ConfigAckedByPeer.count(peerId) && m_ConfigAckedByPeer.at(peerId))},
				{"ready", IsRemoteReady(peerId)},
				{"display_name", GetRemoteName(peerId)},
			});
		}
		json report{
			{"state", StateName(m_State)},
			{"role", m_Config.host ? "host" : "client"},
			{"local_peer_id", static_cast<int>(m_Config.localPeerId)},
			{"remote_peer_id", static_cast<int>(m_RemotePeerIds.empty() ? 0 : m_RemotePeerIds.front())},
			{"remote_peers", remotePeers},
			{"match_config_hash", HashText(m_MatchConfigHash)},
			{"start_frame", m_StartFrame},
			{"failure_reason", m_FailureReason},
			{"config_acked", AllConfigAcked()},
			{"local_ready", m_LocalReady},
			{"remote_ready", AllRemoteReady()},
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

	bool NetLobbySession::IsKnownRemote(uint8_t peerId) const {
		return m_RemoteTransports.find(peerId) != m_RemoteTransports.end();
	}

	bool NetLobbySession::AllConfigAcked() const {
		for (uint8_t peerId : m_RemotePeerIds) {
			const auto it = m_ConfigAckedByPeer.find(peerId);
			if (it == m_ConfigAckedByPeer.end() || !it->second) {
				return false;
			}
		}
		return !m_RemotePeerIds.empty();
	}

	bool NetLobbySession::AllRemoteReady() const {
		for (uint8_t peerId : m_RemotePeerIds) {
			const auto it = m_RemoteReadyByPeer.find(peerId);
			if (it == m_RemoteReadyByPeer.end() || !it->second) {
				return false;
			}
		}
		return !m_RemotePeerIds.empty();
	}

	bool NetLobbySession::SendTo(NetPeerId transport, const NetLobbyPayload& payload, std::string* error) {
		if (!m_Transport) {
			if (error) *error = "lobby has no transport";
			return false;
		}
		std::vector<uint8_t> bytes;
		NetLobbyError encodeError;
		if (!NetLobbyProtocol::Encode({payload}, bytes, &encodeError)) {
			if (error) *error = encodeError.message;
			return false;
		}
		if (!m_Transport->Send(transport, NetTransportLane::ControlReliable, bytes, error)) {
			return false;
		}
		++m_Stats.messagesSent;
		return true;
	}

	bool NetLobbySession::Send(const NetLobbyPayload& payload, std::string* error) {
		for (uint8_t peerId : m_RemotePeerIds) {
			if (!SendTo(m_RemoteTransports[peerId], payload, error)) {
				return false;
			}
		}
		return true;
	}

	void NetLobbySession::SendConfigIfDue(uint64_t nowMs) {
		if (!m_Config.host || AllConfigAcked() || m_State != NetLobbyState::WaitingForConfigAck) {
			return;
		}
		if (m_Stats.configPacketsSent > 0 && nowMs < m_LastConfigSentMs + m_Config.resendIntervalMs) {
			return;
		}
		bool sentAny = false;
		for (uint8_t peerId : m_RemotePeerIds) {
			if (m_ConfigAckedByPeer[peerId]) {
				continue;
			}
			std::string error;
			if (SendTo(m_RemoteTransports[peerId], NetLobbyMatchConfig{m_Config.matchConfig}, &error)) {
				sentAny = true;
			} else {
				Fail(error);
				return;
			}
		}
		if (sentAny) {
			++m_Stats.configPacketsSent;
			m_LastConfigSentMs = nowMs;
		}
	}

	void NetLobbySession::SendPeerState() {
		NetLobbyPeerState state;
		state.peerId = m_Config.localPeerId;
		state.ready = m_LocalReady;
		state.displayName = m_Config.displayName;
		state.platform = m_Config.platform;
		std::string error;
		if (!Send(state, &error)) {
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
		// The Start rides the same ordered lane as the state chunks, so it must queue behind them.
		if (!m_Config.host || !AllConfigAcked() || !AllRemoteReady() || !m_StartRequested || !m_OutgoingChunks.empty() || IsTerminal(m_State)) {
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
			case NetTransportEventType::LocalTransportFault:
				Fail(event.reason.empty() ? "transport error" : event.reason);
				break;
			case NetTransportEventType::PacketReceived: {
				m_LastReceiveMs = nowMs;
				const NetLobbyDecodeResult decoded = NetLobbyProtocol::Decode(event.bytes);
				if (!decoded.ok) {
					// Another phase's packet on the shared wire: session leftovers, or the prior
					// match's in-flight lockstep frames when a rematch lobby round starts.
					if (decoded.error.code == NetLobbyErrorCode::BadMagic &&
					    (NetProtocol::Decode(event.bytes).ok || NetLockstepCodec::Decode(event.bytes).ok)) {
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
				if (IsKnownRemote(payload.peerId)) {
					Reject(payload.reason);
				}
			} else if constexpr (std::is_same_v<Payload, NetLobbyPeerState>) {
				HandlePeerState(payload);
			} else if constexpr (std::is_same_v<Payload, NetLobbyStateChunk>) {
				HandleStateChunk(payload);
			}
		}, message.payload);
	}

	void NetLobbySession::HandleMatchConfig(const NetLobbyMatchConfig& message) {
		if (m_Config.host) {
			return;
		}
		const NetHash32 incomingHash = NetMatchConfigUtil::HashConfig(message.config);
		// The host resends config until every client acks; a repeat of the accepted config just re-acks.
		if (m_State != NetLobbyState::WaitingForConfig && incomingHash == m_MatchConfigHash) {
			Send(NetLobbyConfigAck{m_Config.localPeerId, true, m_MatchConfigHash, ""});
			return;
		}
		std::string validateError;
		if (!NetMatchConfigUtil::ValidateLocalAlpha(message.config, &validateError)) {
			Send(NetLobbyConfigAck{m_Config.localPeerId, false, incomingHash, validateError});
			Reject(validateError);
			return;
		}
		m_Config.matchConfig = message.config;
		m_MatchConfigHash = incomingHash;
		m_StartFrame = 0;
		Send(NetLobbyConfigAck{m_Config.localPeerId, true, m_MatchConfigHash, ""});
		m_State = NetLobbyState::WaitingForReady;
		SendReadyIfNeeded();
	}

	void NetLobbySession::HandleConfigAck(const NetLobbyConfigAck& message) {
		if (!m_Config.host || !IsKnownRemote(message.peerId)) {
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
		m_ConfigAckedByPeer[message.peerId] = true;
		if (AllConfigAcked() && m_State == NetLobbyState::WaitingForConfigAck) {
			m_State = NetLobbyState::WaitingForReady;
		}
	}

	void NetLobbySession::HandleReady(const NetLobbyReady& message) {
		if (!m_Config.host || !IsKnownRemote(message.peerId)) {
			return;
		}
		++m_Stats.readyPacketsReceived;
		m_RemoteReadyByPeer[message.peerId] = message.ready;
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

	void NetLobbySession::HandlePeerState(const NetLobbyPeerState& message) {
		// Accept any roster peer, not just direct remotes: a client hears its SIBLINGS through the
		// host's relay, so every lobby shows real names and readies for the whole roster.
		if (message.peerId == 0 || message.peerId == m_Config.localPeerId) {
			return;
		}
		m_RemoteNamesByPeer[message.peerId] = message.displayName;
		// The explicit Ready message is the authoritative edge; the periodic state keeps views live.
		m_RemoteReadyByPeer[message.peerId] = message.ready;
		m_RemotePingByPeer[message.peerId] = message.pingMs;
		// The host forwards each client's state to the others, stamped with its measured ping so
		// everyone sees an honest star-hub-relative connection quality.
		if (m_Config.host && m_Transport && IsKnownRemote(message.peerId)) {
			NetLobbyPeerState relayed = message;
			relayed.pingMs = m_Transport->GetPeerPingMs(m_RemoteTransports[message.peerId]);
			m_RemotePingByPeer[message.peerId] = relayed.pingMs;
			for (uint8_t peerId: m_RemotePeerIds) {
				if (peerId != message.peerId) {
					std::string ignored;
					(void)SendTo(m_RemoteTransports[peerId], relayed, &ignored);
				}
			}
		}
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
