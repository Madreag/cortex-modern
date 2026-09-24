#include "NetLobbySession.h"

#include "NetIdentity.h"
#include "NetLockstep.h"
#include "NetProtocol.h"
#include "NetSession.h"
#include "NetWorldJoin.h"
#include "System.h"
#include "TimerMan.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
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

		std::atomic<uint64_t> g_TransferStartMs{0};
		std::atomic<uint64_t> g_LastStateTransferMs{0};

		uint64_t TransferSteadyMs() {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		uint64_t RelayWallSeconds() {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
		}

		const std::string& EmptyName() {
			static const std::string empty;
			return empty;
		}
	}

	uint64_t NetLobbyLastStateTransferMs() {
		return g_LastStateTransferMs.load();
	}

	bool NetLobbySession::Start(INetTransport& transport, const NetLobbySessionConfig& config, std::string* error) {
		m_InputDelaySamples.clear();
		m_TimingClockMs = 0;
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
		m_RemotePlatformsByPeer.clear();
		if (!config.remoteTransportPeerIds.empty()) {
			for (const auto& [peerId, transportId] : config.remoteTransportPeerIds) {
				m_RemotePeerIds.push_back(peerId);
				m_RemoteTransports[peerId] = transportId;
			}
		} else if (config.remotePeerId != 0 || config.remoteTransportPeerId != c_InvalidNetPeerId) {
			m_RemotePeerIds.push_back(config.remotePeerId);
			m_RemoteTransports[config.remotePeerId] = config.remoteTransportPeerId;
		}
		if (!config.host && m_RemotePeerIds.empty()) {
			if (error) *error = "lobby client has no host connection";
			return false;
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
		if (!m_Config.matchConfig.relay.Usable(RelayWallSeconds())) m_Config.matchConfig.relay = {};
		m_RelaySendPending = false;
		m_MigrationEndpoints.clear();
		m_OpenedMigrationHash = {};
		m_MigrationRequested = false;
		m_LastMigrationRequestMs = UINT64_MAX;
		m_HostLost = false;
		if (config.enableMigration)
			m_MigrationEndpoints[config.localPeerId] = {config.localPeerId, static_cast<uint16_t>(config.migrationListenPort + config.localPeerId), config.migrationListenAddrs};
		m_State = config.host ? NetLobbyState::WaitingForConfigAck : NetLobbyState::WaitingForConfig;
		m_MatchConfigHash = NetMatchConfigUtil::HashConfig(m_Config.matchConfig);
		m_StartFrame = config.startFrame;
		m_LastConfigSentMs = 0;
		m_LastPeerStateSentMs = 0;
		m_LastReceiveMs = 0;
		m_SessionClockBaseMs = config.session ? config.session->GetClockMs() : 0;
		m_PeerStatePending = true;
		m_ConfigResendDue = false;
		m_LocalReady = config.host || config.autoReady;
		m_ReadySent = false;
		m_StartRequested = config.autoStart;
		m_FailureReason.clear();
		m_RemoteLobbyUp.clear();
		m_LobbyUpConnections.clear();
		m_OutOfRosterPeers.clear();
		m_WorldTransferPeers.clear();
		m_SeatAssigned = false;
		m_StateBytesToSend.clear();
		m_QueuedStateTransfers.clear();
		m_OutgoingStateId = 0;
		m_StateTransferOnlyPeer = 0;
		m_WorldJoinReports.clear();
		m_PendingTailBytes.clear();
		m_OutgoingChunkIndexByPeer.clear();
		m_OutgoingChunkCount = 0;
		m_ChunkSendStall = 0;
		m_StateTransferProgressSerial = 0;
		m_IncomingStateId = 0;
		m_LastIncomingStateId = 0;
		m_IncomingTotalBytes = 0;
		m_IncomingReceivedBytes = 0;
		m_IncomingChunkCount = 0;
		m_IncomingNextChunkIndex = 0;
		m_IncomingStateComplete = false;
		m_ReceivedState.clear();
		m_Stats = {};

		if (m_Config.host) {
			if (m_Config.enableMigration) {
				NetLobbyHello hello;
				hello.peerId = m_Config.localPeerId;
				hello.displayName = m_Config.displayName;
				hello.desiredRole = "host";
				(void)Send(hello);
			}
			for (uint8_t peerId : m_RemotePeerIds) {
				SendSeatAssign(peerId);
			}
			SendConfigIfDue(0);
		}
		// A reseated client must not speak under an id the host's lobby does not answer to.
		if (!m_Config.assignSeats || m_Config.host) {
			SendPeerState();
		}
		return true;
	}

	void NetLobbySession::Tick(uint64_t nowMs) {
		if (!m_Config.matchConfig.relay.Empty() && !m_Config.matchConfig.relay.Usable(RelayWallSeconds())) SetRelayOffer({});
		if (!m_Transport || m_State == NetLobbyState::Idle || IsTerminal(m_State)) {
			return;
		}
		m_TimingClockMs = nowMs;
		std::vector<NetTransportEvent> events;
		events.swap(m_Config.pendingEvents);
		for (NetTransportEvent& event : m_Transport->PollEvents()) events.push_back(std::move(event));
		const uint64_t sessionNowMs = m_Config.sessionNowMs ? m_Config.sessionNowMs() : m_SessionClockBaseMs + nowMs;
		if (m_Config.session) {
			for (const NetTransportEvent& event: events) {
				m_Config.session->InjectEvent(event, sessionNowMs);
			}
			if (m_Config.session->IsFailed() || m_Config.session->IsRejected() || m_Config.session->IsClosed()) {
				const std::string reason = m_Config.session->BuildRejectText();
				Fail(reason.empty() ? "session closed" : reason);
				return;
			}
			SyncSessionPeers();
		}
		for (const NetTransportEvent& event : events) {
			if (IsTerminal(m_State)) {
				return;
			}
			HandleEvent(event, nowMs);
		}
		if (IsTerminal(m_State)) {
			return;
		}
		if (m_Config.session) {
			// Valid phase traffic is counted before the session evaluates silence.
			m_Config.session->Tick(sessionNowMs, false);
			if (m_Config.session->IsFailed() || m_Config.session->IsRejected() || m_Config.session->IsClosed()) {
				const std::string reason = m_Config.session->BuildRejectText();
				Fail(reason.empty() ? "session closed" : reason);
				return;
			}
		}
		SampleInputDelays(nowMs);
		SendReadyIfNeeded();
		if (IsTerminal(m_State)) {
			return;
		}
		if (m_Config.host) {
			SendConfigIfDue(nowMs);
		}
		SendPeerStateIfDue(nowMs);
		if (IsTerminal(m_State)) {
			return;
		}
		if (m_Config.host) {
			// A roster no remote has to accept is acked the moment it is authored.
			if (m_State == NetLobbyState::WaitingForConfigAck && AllConfigAcked()) {
				m_State = NetLobbyState::WaitingForReady;
			}
			GiveUpWaitingForResumeAnswers(nowMs);
			SendQueuedStateChunks();
			SendStartIfReady();
		} else {
			if (m_Config.snapshotProviderPeerId == m_Config.localPeerId) {
				SendQueuedStateChunks();
			}
			ReportStartWait();
		}
		if (!m_Config.host && m_Config.timeoutMs > 0 && nowMs >= m_LastReceiveMs && nowMs - m_LastReceiveMs > m_Config.timeoutMs) {
			m_HostLost = true;
			++m_Stats.timeouts;
			Fail("lobby timed out");
		}
	}

	void NetLobbySession::BeginStateTransfer(std::vector<uint8_t> fileBytes) {
		if ((!m_Config.host && m_Config.snapshotProviderPeerId != m_Config.localPeerId) || fileBytes.empty() || IsTerminal(m_State)) {
			return;
		}
		if (NetLobbyProtocol::GetStateChunkCount(fileBytes.size()) == 0) {
			Fail("state transfer exceeds maximum size");
			return;
		}
		m_StateTransferOnlyPeer = 0;
		m_StateBytesToSend = std::move(fileBytes);
		RestartStateTransfer();
	}

	bool NetLobbySession::BeginStateTransferTo(uint8_t peerId, std::vector<uint8_t> fileBytes) {
		return BeginStateTransferToPeer(peerId, std::move(fileBytes)) == NetLobbyStateTransfer::Started;
	}

	NetLobbyStateTransfer NetLobbySession::BeginStateTransferToPeer(uint8_t peerId, std::vector<uint8_t> fileBytes) {
		if (!m_Config.host || fileBytes.empty() || peerId == 0 || !IsKnownRemote(peerId)) {
			return NetLobbyStateTransfer::Refused;
		}
		if (NetLobbyProtocol::GetStateChunkCount(fileBytes.size()) == 0) {
			Fail("state transfer exceeds maximum size");
			return NetLobbyStateTransfer::Refused;
		}
		// There is one outgoing blob: a second joiner's image would replace the one in flight, so it
		// waits its turn instead.
		if (HasPendingStateChunks()) {
			std::erase_if(m_QueuedStateTransfers, [peerId](const std::pair<uint8_t, std::vector<uint8_t>>& queued) { return queued.first == peerId; });
			m_QueuedStateTransfers.emplace_back(peerId, std::move(fileBytes));
			return NetLobbyStateTransfer::Queued;
		}
		m_StateTransferOnlyPeer = peerId;
		m_StateBytesToSend = std::move(fileBytes);
		RestartStateTransfer();
		return NetLobbyStateTransfer::Started;
	}

	bool NetLobbySession::StartNextQueuedStateTransfer() {
		while (!m_QueuedStateTransfers.empty()) {
			std::pair<uint8_t, std::vector<uint8_t>> next = std::move(m_QueuedStateTransfers.front());
			m_QueuedStateTransfers.erase(m_QueuedStateTransfers.begin());
			if (!IsKnownRemote(next.first) || next.second.empty()) {
				continue;
			}
			m_StateTransferOnlyPeer = next.first;
			m_StateBytesToSend = std::move(next.second);
			RestartStateTransfer();
			return true;
		}
		return false;
	}

	NetPeerId NetLobbySession::RemoteTransportOf(uint8_t peerId) const {
		const auto found = m_RemoteTransports.find(peerId);
		return found == m_RemoteTransports.end() ? c_InvalidNetPeerId : found->second;
	}

	bool NetLobbySession::BindLateRemote(uint8_t peerId, NetPeerId transport, std::string* error) {
		if (!m_Config.host || peerId == 0 || peerId == m_Config.localPeerId || transport == c_InvalidNetPeerId) {
			if (error) *error = "late lobby remote is invalid";
			return false;
		}
		if (IsKnownRemote(peerId)) {
			m_RemoteTransports[peerId] = transport;
			return true;
		}
		m_RemotePeerIds.push_back(peerId);
		m_RemoteTransports[peerId] = transport;
		m_ConfigAckedByPeer[peerId] = false;
		m_RemoteReadyByPeer[peerId] = false;
		return true;
	}

	bool NetLobbySession::BindWorldTransferRemote(uint8_t peerId, NetPeerId transport, std::string* error) {
		if (!m_Config.host || peerId == 0 || peerId == m_Config.localPeerId || transport == c_InvalidNetPeerId) {
			if (error) *error = "late lobby remote is invalid";
			return false;
		}
		const bool connectionWasReady = m_LobbyUpConnections.contains(transport);
		if (IsKnownRemote(peerId) && RemoteTransportOf(peerId) != transport) RemoveRemotePeer(peerId);
		std::vector<uint8_t> previous;
		for (const auto& [bound, connection]: m_RemoteTransports)
			if (connection == transport && bound != peerId) previous.push_back(bound);
		if (!BindLateRemote(peerId, transport, error)) {
			return false;
		}
		for (uint8_t bound: previous) RemoveRemotePeer(bound);
		if (connectionWasReady) m_LobbyUpConnections.insert(transport);
		m_WorldTransferPeers.insert(peerId);
		return true;
	}

	bool NetLobbySession::SendMatchConfigTo(uint8_t peerId) {
		if (!m_Config.host || !IsKnownRemote(peerId)) {
			return false;
		}
		SendSeatAssign(peerId);
		std::string error;
		if (!SendTo(m_RemoteTransports.at(peerId), NetLobbyMatchConfig{m_Config.matchConfig}, &error)) return false;
		SendResumeOfferTo(peerId);
		return true;
	}

	void NetLobbySession::PumpOutgoingChunks() {
		if (m_Config.host && m_Config.session) {
			const auto ready = m_Config.session->GetReadyPeers();
			std::vector<uint8_t> disconnected;
			for (uint8_t peer: m_WorldTransferPeers) {
				const NetPeerId connection = RemoteTransportOf(peer);
				if (std::none_of(ready.begin(), ready.end(), [&](const auto& live) { return live.transportPeerId == connection; })) disconnected.push_back(peer);
			}
			for (uint8_t peer: disconnected) RemoveRemotePeer(peer);
		}
		SendQueuedStateChunks();
	}

	void NetLobbySession::HandleTransportEvent(const NetTransportEvent& event, uint64_t nowMs) {
		HandleEvent(event, nowMs);
	}

	bool NetLobbySession::SendPayloadTo(uint8_t peerId, const NetLobbyPayload& payload, std::string* error) {
		if (!IsKnownRemote(peerId)) {
			if (error) *error = "lobby has no remote for that peer";
			return false;
		}
		return SendTo(m_RemoteTransports.at(peerId), payload, error);
	}

	bool NetLobbySession::SendPayload(const NetLobbyPayload& payload, std::string* error) {
		return Send(payload, error);
	}

	void NetLobbySession::RestartStateTransfer() {
		const uint16_t chunkCount = NetLobbyProtocol::GetStateChunkCount(m_StateBytesToSend.size());
		if (chunkCount == 0 || m_OutgoingStateId == std::numeric_limits<uint64_t>::max()) {
			Fail("state transfer bounds are invalid");
			return;
		}
		m_OutgoingStateId = m_OutgoingStateId == 0 ? 0x50355354ULL ^ static_cast<uint32_t>(m_StateBytesToSend.size()) ^ (static_cast<uint64_t>(chunkCount) << 32) : m_OutgoingStateId + 1;
		m_OutgoingChunkIndexByPeer.clear();
		m_OutgoingChunkCount = chunkCount;
		m_ChunkSendStall = 0;
		g_TransferStartMs.store(0);
	}

	uint16_t NetLobbySession::OutgoingChunkIndex(uint8_t peerId) const {
		const auto it = m_OutgoingChunkIndexByPeer.find(peerId);
		return it == m_OutgoingChunkIndexByPeer.end() ? 0 : it->second;
	}

	bool NetLobbySession::HasPendingStateChunks() const {
		return m_OutgoingChunkCount > 0 && std::any_of(m_RemotePeerIds.begin(), m_RemotePeerIds.end(), [this](uint8_t peerId) {
			if (m_StateTransferOnlyPeer != 0 && peerId != m_StateTransferOnlyPeer) {
				return false;
			}
			// A peer that answered with its own copy of the checkpoint is owed no chunk at all; one that
			// has not answered yet is still owed the state, so the start keeps waiting for it.
			if (m_StateTransferOnlyPeer == 0 && ResumeSkipsTransfer(peerId)) {
				return false;
			}
			return OutgoingChunkIndex(peerId) < m_OutgoingChunkCount;
		});
	}

	bool NetLobbySession::ResumeSkipsTransfer(uint8_t peerId) const {
		return !m_Config.resumeMatchId.empty() && m_ResumeHeldPeers.contains(peerId);
	}

	bool NetLobbySession::ResumeAwaitsAnswer(uint8_t peerId) const {
		return !m_Config.resumeMatchId.empty() && !m_ResumeAnsweredPeers.contains(peerId);
	}

	std::vector<uint8_t> NetLobbySession::TakeReceivedState() {
		if (!m_IncomingStateComplete) return {};
		m_IncomingStateComplete = false;
		m_IncomingStateId = 0;
		return std::move(m_ReceivedState);
	}

	void NetLobbySession::SendQueuedStateChunks() {
		int budget = 2;
		while (budget-- > 0) {
			// A remote gets chunks only once its own lobby is up: before that its session discards
			// them as another phase's packets and nothing ever sends them again.
			uint16_t index = m_OutgoingChunkCount;
			for (uint8_t peerId: m_RemotePeerIds) {
				if (m_StateTransferOnlyPeer != 0 && peerId != m_StateTransferOnlyPeer) continue;
				if (m_StateTransferOnlyPeer == 0 && (ResumeSkipsTransfer(peerId) || ResumeAwaitsAnswer(peerId))) continue;
				if (IsRemoteConnectionLobbyUp(peerId)) index = std::min(index, OutgoingChunkIndex(peerId));
			}
			if (index >= m_OutgoingChunkCount) break;
			NetLobbyStateChunk chunk;
			chunk.transferId = m_OutgoingStateId;
			chunk.totalBytes = static_cast<uint32_t>(m_StateBytesToSend.size());
			chunk.chunkIndex = index;
			chunk.chunkCount = m_OutgoingChunkCount;
			const size_t begin = static_cast<size_t>(index) * NetLobbyProtocol::c_MaxStateChunkBytes;
			const size_t end = std::min(m_StateBytesToSend.size(), begin + NetLobbyProtocol::c_MaxStateChunkBytes);
			chunk.bytes.assign(m_StateBytesToSend.begin() + begin, m_StateBytesToSend.begin() + end);
			for (uint8_t peerId: m_RemotePeerIds) {
				if (m_StateTransferOnlyPeer != 0 && peerId != m_StateTransferOnlyPeer) continue;
				if (m_StateTransferOnlyPeer == 0 && (ResumeSkipsTransfer(peerId) || ResumeAwaitsAnswer(peerId))) continue;
				if (!IsRemoteConnectionLobbyUp(peerId) || OutgoingChunkIndex(peerId) != index) continue;
				std::string error;
				if (!SendTo(m_RemoteTransports.at(peerId), chunk, &error)) {
					if (++m_ChunkSendStall > 4000) Fail("state transfer stalled: " + error);
					return;
				}
				m_OutgoingChunkIndexByPeer[peerId] = static_cast<uint16_t>(index + 1);
				++m_StateTransferProgressSerial;
				m_ChunkSendStall = 0;
				if (g_TransferStartMs.load() == 0) {
					g_TransferStartMs.store(TransferSteadyMs());
				}
			}
		}
		if (!HasPendingStateChunks()) {
			const uint64_t start = g_TransferStartMs.exchange(0);
			if (start != 0) {
				g_LastStateTransferMs.store(TransferSteadyMs() - start);
			}
			// The pump is free, so the next joiner's image can take it; its chunks go out next pump.
			(void)StartNextQueuedStateTransfer();
		}
	}

	NetLobbySession::WorldJoinReport NetLobbySession::TakeWorldJoinReport() {
		if (m_WorldJoinReports.empty()) return {};
		WorldJoinReport report = m_WorldJoinReports.front();
		m_WorldJoinReports.pop_front();
		return report;
	}

	std::vector<uint8_t> NetLobbySession::TakePendingTailBytes() {
		return std::move(m_PendingTailBytes);
	}

	void NetLobbySession::HandleStateChunk(const NetLobbyStateChunk& message) {
		if (m_Config.host && m_Config.snapshotProviderPeerId == 0) {
			return;
		}
		if (m_IncomingStateId != message.transferId) {
			if (message.chunkIndex != 0 || message.transferId <= m_LastIncomingStateId) {
				Fail("state transfer does not start with a new first chunk");
				return;
			}
			m_IncomingStateId = message.transferId;
			m_LastIncomingStateId = message.transferId;
			m_IncomingTotalBytes = message.totalBytes;
			m_IncomingReceivedBytes = 0;
			m_IncomingChunkCount = message.chunkCount;
			m_IncomingNextChunkIndex = 0;
			m_IncomingStateComplete = false;
			m_ReceivedState.clear();
			g_TransferStartMs.store(TransferSteadyMs());
		}
		if (message.totalBytes != m_IncomingTotalBytes || message.chunkCount != m_IncomingChunkCount) {
			Fail("state transfer header changed");
			return;
		}
		if (message.chunkIndex < m_IncomingNextChunkIndex) {
			const size_t begin = static_cast<size_t>(message.chunkIndex) * NetLobbyProtocol::c_MaxStateChunkBytes;
			if (!std::equal(message.bytes.begin(), message.bytes.end(), m_ReceivedState.begin() + begin)) Fail("state transfer duplicate differs");
			return;
		}
		if (message.chunkIndex != m_IncomingNextChunkIndex) {
			Fail("state transfer chunk is out of order");
			return;
		}
		const size_t nextSize = m_ReceivedState.size() + message.bytes.size();
		if (nextSize > m_ReceivedState.capacity()) {
			const size_t capacity = std::min(static_cast<size_t>(m_IncomingTotalBytes), std::max(nextSize, m_ReceivedState.capacity() + m_ReceivedState.capacity() / 2));
			m_ReceivedState.reserve(capacity);
		}
		m_ReceivedState.insert(m_ReceivedState.end(), message.bytes.begin(), message.bytes.end());
		m_IncomingReceivedBytes = static_cast<uint32_t>(m_ReceivedState.size());
		++m_IncomingNextChunkIndex;
		++m_StateTransferProgressSerial;
		if (m_Config.matchConfig.persistentWorld || IsWorldJoinImageBlob(m_ReceivedState)) {
			const uint64_t progress = (static_cast<uint64_t>(m_IncomingChunkCount) << 32) | m_IncomingNextChunkIndex;
			std::string sendError;
			(void)Send(MakeWorldJoinReport(c_NetWorldReportProgress, progress), &sendError);
		}
		if (m_IncomingNextChunkIndex != m_IncomingChunkCount) return;
		m_IncomingStateComplete = true;
		if (m_Config.host && m_Config.snapshotProviderPeerId != 0)
			BeginStateTransfer(m_ReceivedState);
		const uint64_t start = g_TransferStartMs.exchange(0);
		if (start != 0) {
			g_LastStateTransferMs.store(TransferSteadyMs() - start);
		}
		System::PrintDiagnosticLine("[net-match] state transfer complete: " + std::to_string(m_ReceivedState.size()) + " bytes");
	}

	bool NetLobbySession::IsRemoteReady(uint8_t peerId) const {
		const auto it = m_RemoteReadyByPeer.find(peerId);
		return it != m_RemoteReadyByPeer.end() && it->second;
	}

	bool NetLobbySession::IsConfigAcked(uint8_t peerId) const {
		const auto it = m_ConfigAckedByPeer.find(peerId);
		return it != m_ConfigAckedByPeer.end() && it->second;
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
				{"config_republishes", m_Stats.configRepublishes},
				{"ready_packets_received", m_Stats.readyPacketsReceived},
				{"start_packets_sent", m_Stats.startPacketsSent},
				{"start_packets_received", m_Stats.startPacketsReceived},
				{"timeouts", m_Stats.timeouts},
				{"unbound_connection_faults", m_Stats.unboundConnectionFaults},
				{"unbound_disconnects", m_Stats.unboundDisconnects},
			}},
		};
		// A remote abort reason is free-form text the codec never held to UTF-8, so a stray byte is
		// replaced rather than thrown.
		return report.dump(-1, ' ', false, json::error_handler_t::replace);
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
	}

	bool NetLobbySession::RepublishMatchConfig(const NetMatchConfig& config, std::string* error) {
		auto refuse = [error](const char* reason) {
			if (error) *error = reason;
			return false;
		};
		if (!m_Config.host) {
			return refuse("only the host republishes the match config");
		}
		if (m_State == NetLobbyState::Idle || IsTerminal(m_State)) {
			return refuse("the lobby round is no longer open");
		}
		// The draft named the revision it was accepted against; anything at or behind the published
		// one is a transaction the round has already moved past.
		if (config.configRevision <= m_Config.matchConfig.configRevision) {
			return refuse("the draft names a stale configuration revision");
		}
		if (config.sessionId != m_Config.matchConfig.sessionId || config.hostPeerId != m_Config.matchConfig.hostPeerId ||
		    config.peerCount != m_Config.matchConfig.peerCount) {
			return refuse("seat capacity and session identity are fixed for the open lobby");
		}
		std::string validateError;
		if (!NetMatchConfigUtil::ValidateLocalAlpha(config, &validateError)) {
			if (error) *error = validateError;
			return false;
		}
		m_Config.matchConfig = config;
		m_Config.autoInputDelay = config.delayPolicy == NetMatchDelayPolicy::Auto;
		m_MatchConfigHash = NetMatchConfigUtil::HashConfig(config);
		// Every peer acknowledges this exact revision before it counts again: HandleConfigAck drops an
		// ack whose hash is the old one, so a delayed ack cannot accept a config its sender never saw.
		for (auto& [peerId, acked] : m_ConfigAckedByPeer) {
			acked = false;
		}
		for (auto& [peerId, ready] : m_RemoteReadyByPeer) {
			ready = false;
		}
		m_State = NetLobbyState::WaitingForConfigAck;
		m_ReadySent = false;
		m_LocalReady = m_Config.host || m_Config.autoReady;
		// A Start pending on the old revision does not carry over; an auto-starting round re-arms it
		// exactly as Start() did, so the new config is what the round begins on.
		m_StartRequested = m_Config.autoStart;
		m_PeerStatePending = true;
		m_ConfigResendDue = true;
		++m_Stats.configRepublishes;
		return true;
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

	bool NetLobbySession::IsCommittedTransport(NetPeerId transportPeerId) const {
		return transportPeerId != c_InvalidNetPeerId && std::any_of(m_RemoteTransports.begin(), m_RemoteTransports.end(), [transportPeerId](const auto& entry) {
			return entry.second == transportPeerId;
		});
	}

	void NetLobbySession::RemoveRemote(NetPeerId transportPeerId) {
		const auto peer = std::find_if(m_RemoteTransports.begin(), m_RemoteTransports.end(), [transportPeerId](const auto& entry) {
			return entry.second == transportPeerId;
		});
		if (peer == m_RemoteTransports.end()) return;
		RemoveRemotePeer(peer->first);
	}

	void NetLobbySession::RemoveRemotePeer(uint8_t peerId) {
		m_LobbyUpConnections.erase(RemoteTransportOf(peerId));
		std::erase_if(m_QueuedStateTransfers, [&](const auto& transfer) { return transfer.first == peerId; });
		if (m_StateTransferOnlyPeer == peerId) {
			m_StateTransferOnlyPeer = 0;
			m_StateBytesToSend.clear();
			m_OutgoingChunkCount = 0;
			m_OutgoingChunkIndexByPeer.clear();
		}
		m_RemoteTransports.erase(peerId);
		std::erase(m_RemotePeerIds, peerId);
		m_RemoteLobbyUp.erase(peerId);
		m_WorldTransferPeers.erase(peerId);
		m_OutgoingChunkIndexByPeer.erase(peerId);
		m_ConfigAckedByPeer.erase(peerId);
		m_RemoteReadyByPeer.erase(peerId);
		m_RemoteNamesByPeer.erase(peerId);
		m_RemotePingByPeer.erase(peerId);
		m_RemotePlatformsByPeer.erase(peerId);
		// The seat is open again, so it carries the unseated name once more: a kicked or departed
		// member's name on a seat nobody holds is a roster row that lies to every peer. Opening it is a
		// live roster change, so it rides the republish a host option edit rides - a new revision the
		// peers that stayed acknowledge, and the panels mirroring the adopted config re-seed from it.
		NetMatchConfig opened = m_Config.matchConfig;
		bool seatOpened = false;
		const std::string unseated = NetMatchConfigUtil::UnseatedSlotName(peerId, opened.persistentWorld);
		for (NetMatchPlayerSlot& slot: opened.players) {
			if (slot.peerId == peerId && !slot.cpu && slot.displayName != unseated) {
				slot.displayName = unseated;
				seatOpened = true;
			}
		}
		if (seatOpened) {
			++opened.configRevision;
			// Only a round that is already closing refuses, and its roster is nobody's view by then.
			(void)RepublishMatchConfig(opened);
		}
		m_PeerStatePending = true;
		m_StartRequested = m_Config.autoStart;
		m_State = NetLobbyState::WaitingForConfigAck;
	}

	void NetLobbySession::RejectRemote(NetPeerId transportPeerId, const std::string& reason) {
		m_Transport->Disconnect(transportPeerId, reason);
		if (m_Config.session) {
			m_Config.session->InjectEvent({NetTransportEventType::PeerDisconnected, transportPeerId, NetTransportLane::ControlReliable, {}, reason}, m_Config.session->GetClockMs());
		}
		RemoveRemote(transportPeerId);
	}

	void NetLobbySession::SampleInputDelays(uint64_t nowMs) {
		if (!m_Config.host || !m_Config.autoInputDelay || IsTerminal(m_State)) return;
		NetMatchConfig next = m_Config.matchConfig;
		if (next.peerInputDelayFrames.empty()) next.peerInputDelayFrames.resize(next.peerCount, std::max<uint16_t>(1, next.inputDelayFrames));
		bool changed = false;
		for (const auto& [peer, transport]: m_RemoteTransports) {
			if (peer == 0 || peer > next.peerCount) continue;
			auto& sample = m_InputDelaySamples[peer];
			sample.Observe(nowMs, m_Transport->GetPeerPingMs(transport));
			if (const auto delay = sample.Change(nowMs, next.peerInputDelayFrames[peer - 1], g_TimerMan.GetDeltaTimeMS(), next.inputDelayFrames)) {
				next.peerInputDelayFrames[peer - 1] = *delay;
				changed = true;
			}
		}
		if (!changed || next.configRevision == UINT64_MAX) return;
		++next.configRevision;
		const bool startPending = m_StartRequested;
		// The host asked for this round to start once, as a player presses it once. A re-size the host
		// never typed re-opens the acknowledgement, never withdraws that request.
		if (RepublishMatchConfig(next) && startPending) {
			m_StartRequested = true;
		}
	}

	void NetLobbySession::SyncSessionPeers() {
		if (!m_Config.host || !m_Config.session) return;
		const std::vector<NetSessionPeerInfo> readyPeers = m_Config.session->GetReadyPeers();
		const auto active = [&](uint8_t peer) {
			return m_Config.matchConfig.activePeerIds.empty() || std::binary_search(m_Config.matchConfig.activePeerIds.begin(), m_Config.matchConfig.activePeerIds.end(), peer);
		};
		const auto worldConnection = [&](NetPeerId connection) {
			return std::any_of(m_WorldTransferPeers.begin(), m_WorldTransferPeers.end(), [&](uint8_t peer) {
				return m_RemoteTransports.contains(peer) && m_RemoteTransports.at(peer) == connection;
			});
		};
		std::map<uint8_t, NetPeerId> transports;
		const auto inRoster = [&](uint8_t peer) { return peer != 0 && peer <= m_Config.matchConfig.peerCount; };
		for (const NetSessionPeerInfo& peer: readyPeers) {
			if (worldConnection(peer.transportPeerId)) continue;
			if (!active(static_cast<uint8_t>(peer.assignedPeerId + 1))) continue;
			const uint8_t peerId = static_cast<uint8_t>(peer.assignedPeerId + 1);
			// A connection the session seated past the roster is no lobby seat: a world admits it through its join plane.
			if (!inRoster(peerId)) {
				if (m_OutOfRosterPeers.insert(peer.transportPeerId).second)
					System::PrintDiagnosticLine("[net-lobby] connection " + std::to_string(peer.transportPeerId) + " holds peer id " + std::to_string(peerId) +
					                            " past the roster's " + std::to_string(m_Config.matchConfig.peerCount) + " seats; the lobby does not seat it");
				continue;
			}
			transports[peerId] = peer.transportPeerId;
		}
		// A world bootstrap's id comes from the join plane, never from the session roster; without this
		// the rebuild below would unbind it and the image in flight would stop.
		for (uint8_t worldPeer: m_WorldTransferPeers) {
			if (const auto bound = m_RemoteTransports.find(worldPeer); bound != m_RemoteTransports.end()) {
				transports[worldPeer] = bound->second;
			}
		}
		if (transports == m_RemoteTransports) return;
		const auto previous = m_RemoteTransports;
		bool addedPeer = false;
		for (const auto& [peerId, transportId]: previous) {
			if (!transports.contains(peerId) || transports.at(peerId) != transportId) RemoveRemote(transportId);
		}
		for (const NetSessionPeerInfo& peer: readyPeers) {
			const uint8_t peerId = static_cast<uint8_t>(peer.assignedPeerId + 1);
			if (worldConnection(peer.transportPeerId)) continue;
			if (!active(peerId)) continue;
			if (IsKnownRemote(peerId)) continue;
			if (!inRoster(peerId)) continue;
			addedPeer = true;
			m_RemoteTransports[peerId] = peer.transportPeerId;
			m_RemotePeerIds.push_back(peerId);
			m_ConfigAckedByPeer[peerId] = false;
			m_RemoteReadyByPeer[peerId] = false;
			m_RemoteNamesByPeer[peerId] = peer.displayName;
			for (NetMatchPlayerSlot& slot: m_Config.matchConfig.players) {
				if (slot.peerId == peerId) slot.displayName = peer.displayName;
			}
			if (m_Config.autoInputDelay) {
				auto& delays = m_Config.matchConfig.peerInputDelayFrames;
				// A restored config may carry fewer delays than seats; every seat it restores has one.
				if (delays.size() < m_Config.matchConfig.peerCount) delays.resize(m_Config.matchConfig.peerCount, std::max<uint16_t>(1, m_Config.matchConfig.inputDelayFrames));
				const uint32_t rttMs = m_Transport->GetPeerPingMs(peer.transportPeerId);
				auto& sample = m_InputDelaySamples[peerId];
				sample.Observe(m_TimingClockMs, rttMs);
				const uint16_t delay = static_cast<uint16_t>(std::min<uint32_t>(sample.RequiredFrames(g_TimerMan.GetDeltaTimeMS(), m_Config.matchConfig.inputDelayFrames), NetMatchConfigUtil::c_MaxInputDelayFrames));
				delays.at(peerId - 1) = std::max(m_Config.matchConfig.inputDelayFrames, delay);
			}
		}
		std::sort(m_RemotePeerIds.begin(), m_RemotePeerIds.end());
		const NetHash32 configHash = NetMatchConfigUtil::HashConfig(m_Config.matchConfig);
		if (configHash != m_MatchConfigHash) {
			for (auto& [peerId, acked]: m_ConfigAckedByPeer) acked = false;
			m_MatchConfigHash = configHash;
		}
		m_State = NetLobbyState::WaitingForConfigAck;
		m_StartRequested = m_Config.autoStart;
		m_PeerStatePending = true;
		if (addedPeer) {
			for (uint8_t peerId: m_RemotePeerIds) {
				if (!previous.contains(peerId)) SendSeatAssign(peerId);
			}
		}
		// A targeted transfer is one joiner's image; another peer arriving must not restart it.
		if (addedPeer && m_StateTransferOnlyPeer == 0 && !m_StateBytesToSend.empty()) RestartStateTransfer();
	}

	bool NetLobbySession::SeatsRemoteHuman() const {
		return std::any_of(m_Config.matchConfig.players.begin(), m_Config.matchConfig.players.end(), [this](const NetMatchPlayerSlot& slot) {
			return !slot.cpu && slot.peerId != m_Config.matchConfig.hostPeerId;
		});
	}

	bool NetLobbySession::HasRequiredOccupancy() const {
		if (m_Config.matchConfig.persistentWorld) {
			return !m_RemotePeerIds.empty() || !m_Config.matchConfig.dedicated;
		}
		if (m_Config.host && m_RemotePeerIds.size() + 1 != (m_Config.activePeerCount ? m_Config.activePeerCount : m_Config.matchConfig.peerCount)) {
			return false;
		}
		return !m_RemotePeerIds.empty() || !SeatsRemoteHuman();
	}

	bool NetLobbySession::AllConfigAcked() const {
		if (!HasRequiredOccupancy()) return false;
		for (uint8_t peerId : m_RemotePeerIds) {
			const auto it = m_ConfigAckedByPeer.find(peerId);
			if (it == m_ConfigAckedByPeer.end() || !it->second) {
				return false;
			}
		}
		return true;
	}

	bool NetLobbySession::AllRemoteReady() const {
		// An unstarted lobby has no remotes to be ready; the empty default config would otherwise say yes.
		if (m_State == NetLobbyState::Idle || !HasRequiredOccupancy()) {
			return false;
		}
		for (uint8_t peerId : m_RemotePeerIds) {
			const auto it = m_RemoteReadyByPeer.find(peerId);
			if (it == m_RemoteReadyByPeer.end() || !it->second) {
				return false;
			}
		}
		return true;
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

	void NetLobbySession::SetRelayOffer(const NetRelayConfig& offer) {
		const NetRelayConfig current = offer.Usable(RelayWallSeconds()) ? offer : NetRelayConfig{};
		if (m_Config.matchConfig.relay != current) {
			m_Config.matchConfig.relay = current;
			m_RelaySendPending = true;
		}
		if (m_Config.host && m_RelaySendPending && m_State != NetLobbyState::Idle) {
			m_RelaySendPending = !Send(NetLobbyMatchConfig{m_Config.matchConfig});
		}
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
		if (m_Config.enableMigration && !PrepareMigrationRoster()) {
			if (m_LastMigrationRequestMs == UINT64_MAX || nowMs >= m_LastMigrationRequestMs + m_Config.resendIntervalMs) {
				NetLobbyMigration request;
				request.peerId = m_Config.localPeerId;
				request.listenPort = static_cast<uint16_t>(m_Config.migrationListenPort + m_Config.localPeerId);
				request.listenAddrs = m_Config.migrationListenAddrs;
				(void)Send(request);
				m_LastMigrationRequestMs = nowMs;
			}
			return;
		}
		if (!m_ConfigResendDue && m_Stats.configPacketsSent > 0 && nowMs < m_LastConfigSentMs + m_Config.resendIntervalMs) {
			return;
		}
		bool sentAny = false;
		for (uint8_t peerId : m_RemotePeerIds) {
			if (m_ConfigAckedByPeer[peerId]) {
				continue;
			}
			// A peer whose lobby came up after the first send has heard neither, so both go again.
			if (m_Config.enableMigration) {
				NetLobbyHello hello;
				hello.peerId = m_Config.localPeerId;
				hello.displayName = m_Config.displayName;
				hello.desiredRole = "host";
				(void)SendTo(m_RemoteTransports[peerId], hello);
			}
			SendSeatAssign(peerId);
			std::string error;
			NetLobbyMigration capsule;
			if (!m_Config.matchConfig.successorOrder.empty()) {
				capsule.kind = 2;
				capsule.peerId = peerId;
				capsule.configHash = m_MatchConfigHash;
				if (!m_Config.sealMigration) {
					Fail("successor credential provider is missing");
					return;
				}
				if (!m_Config.sealMigration(peerId, m_MatchConfigHash, capsule.sealedState))
					continue;
			}
			if (SendTo(m_RemoteTransports[peerId], NetLobbyMatchConfig{m_Config.matchConfig}, &error)) {
				if (!m_Config.matchConfig.successorOrder.empty()) {
					(void)SendTo(m_RemoteTransports[peerId], capsule, &error);
				}
				SendResumeOfferTo(peerId);
				sentAny = true;
			}
		}
		if (sentAny) {
			++m_Stats.configPacketsSent;
			m_LastConfigSentMs = nowMs;
			m_ConfigResendDue = false;
		}
	}

	void NetLobbySession::SendPeerState() {
		if (!m_Config.host && m_Config.enableMigration && m_MigrationRequested) {
			NetLobbyMigration endpoint;
			endpoint.peerId = m_Config.localPeerId;
			endpoint.listenPort = static_cast<uint16_t>(m_Config.migrationListenPort + m_Config.localPeerId);
			endpoint.listenAddrs = m_Config.migrationListenAddrs;
			(void)Send(endpoint);
		}
		NetLobbyPeerState state;
		state.peerId = m_Config.localPeerId;
		state.ready = m_LocalReady;
		state.displayName = m_Config.displayName;
		state.platform = m_Config.platform;
		std::string error;
		if (!Send(state, &error) && !m_Config.host) {
			Fail(error);
		}
		if (m_Config.host) {
			for (const NetMatchPlayerSlot& slot: m_Config.matchConfig.players) {
				if (slot.peerId == m_Config.localPeerId || slot.cpu) continue;
				NetLobbyPeerState remote;
				remote.peerId = slot.peerId;
				remote.connected = IsKnownRemote(slot.peerId);
				remote.ready = remote.connected && IsRemoteReady(slot.peerId);
				remote.displayName = GetRemoteName(slot.peerId).empty() ? slot.displayName : GetRemoteName(slot.peerId);
				remote.pingMs = remote.connected ? m_Transport->GetPeerPingMs(m_RemoteTransports.at(slot.peerId)) : 0;
				if (m_RemotePlatformsByPeer.contains(slot.peerId)) remote.platform = m_RemotePlatformsByPeer.at(slot.peerId);
				(void)Send(remote, &error);
			}
		}
		m_PeerStatePending = false;
	}

	void NetLobbySession::SendPeerStateIfDue(uint64_t nowMs) {
		if (!m_Config.host && m_Config.assignSeats && !m_SeatAssigned) {
			return;
		}
		if (!m_PeerStatePending && (m_Config.peerStateIntervalMs == 0 || nowMs < m_LastPeerStateSentMs + m_Config.peerStateIntervalMs)) {
			return;
		}
		SendPeerState();
		m_LastPeerStateSentMs = nowMs;
	}

	void NetLobbySession::SendSeatAssign(uint8_t peerId) {
		const bool worldSeat = m_WorldTransferPeers.contains(peerId) && peerId <= m_Config.matchConfig.peerCount;
		if (!m_Config.host || (!m_Config.assignSeats && !worldSeat) || !IsKnownRemote(peerId)) {
			return;
		}
		std::string error;
		if (SendTo(m_RemoteTransports.at(peerId), NetLobbySeatAssign{peerId}, &error)) {
			++m_Stats.seatAssignmentsSent;
		}
	}

	void NetLobbySession::HandleSeatAssign(const NetLobbySeatAssign& message) {
		if (m_Config.host || message.assignedPeerId == 0) {
			return;
		}
		m_SeatAssigned = true;
		if (message.assignedPeerId == m_Config.localPeerId) {
			return;
		}
		if (IsKnownRemote(message.assignedPeerId)) {
			Fail("the host bound this connection to a seat it already gave another peer");
			return;
		}
		m_Config.localPeerId = message.assignedPeerId;
		if (m_Config.session) {
			std::string error;
			if (!m_Config.session->AdoptRematchPeerId(static_cast<uint8_t>(message.assignedPeerId - 1), &error)) {
				Fail(error);
				return;
			}
		}
		++m_Stats.seatAssignmentsAdopted;
		m_PeerStatePending = true;
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

	void NetLobbySession::ReportStartWait() {
		if (IsTerminal(m_State) || m_State == NetLobbyState::Idle) {
			return;
		}
		// The first call only starts the clock: a lobby that starts promptly says nothing.
		if (m_LastStartWaitLogMs == 0 || m_TimingClockMs < m_LastStartWaitLogMs + 2000) {
			if (m_LastStartWaitLogMs == 0) m_LastStartWaitLogMs = m_TimingClockMs == 0 ? 1 : m_TimingClockMs;
			return;
		}
		m_LastStartWaitLogMs = m_TimingClockMs == 0 ? 1 : m_TimingClockMs;
		std::ostringstream line;
		line << "[net-lobby] waiting at " << StateName(m_State) << " role=" << (m_Config.host ? "host" : "client")
		     << " hash=" << HashText(m_MatchConfigHash) << " revision=" << m_Config.matchConfig.configRevision
		     << " republishes=" << m_Stats.configRepublishes << " config_sent=" << m_Stats.configPacketsSent
		     << " acks=" << m_Stats.configAcksReceived;
		if (m_Config.host) {
			line << " occupancy=" << (HasRequiredOccupancy() ? 1 : 0) << " start_requested=" << (m_StartRequested ? 1 : 0)
			     << " chunks_pending=" << (HasPendingStateChunks() ? 1 : 0);
			for (uint8_t peerId: m_RemotePeerIds) {
				line << " peer" << static_cast<int>(peerId) << "=[acked=" << (m_ConfigAckedByPeer.count(peerId) && m_ConfigAckedByPeer.at(peerId) ? 1 : 0)
				     << " ready=" << (IsRemoteReady(peerId) ? 1 : 0) << " lobby_up=" << (IsRemoteLobbyUp(peerId) ? 1 : 0)
				     << " delay=" << NetMatchConfigUtil::PeerInputDelay(m_Config.matchConfig, peerId) << "]";
			}
		} else {
			line << " ready_sent=" << (m_ReadySent ? 1 : 0) << " local_ready=" << (m_LocalReady ? 1 : 0)
			     << " seat_assigned=" << (m_SeatAssigned ? 1 : 0) << " starts_seen=" << m_Stats.startPacketsReceived;
		}
		std::cout << line.str() << std::endl;
	}

	void NetLobbySession::SendStartIfReady() {
		if (m_Config.host && m_Config.snapshotProviderPeerId != 0 && !m_IncomingStateComplete)
			return;
		// The Start rides the same ordered lane as the state chunks, so it must queue behind them.
		if (!m_Config.host || !AllConfigAcked() || !AllRemoteReady() || !m_StartRequested || HasPendingStateChunks() || IsTerminal(m_State)) {
			ReportStartWait();
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
				if (m_Config.host) {
					// A joiner this round never bound is not part of it; only a committed remote leaving
					// changes the round.
					if (!IsCommittedTransport(event.peerId)) {
						++m_Stats.unboundDisconnects;
						break;
					}
					RemoveRemote(event.peerId);
				} else {
					m_HostLost = IsCommittedTransport(event.peerId);
					Fail(event.reason.empty() ? "peer disconnected" : event.reason);
				}
				break;
			case NetTransportEventType::ConnectionFailed:
			case NetTransportEventType::TransportError:
				if (m_Config.host) {
					// The transport reports these with no attributable peer, so an unbound joiner's
					// fault must never reach a committed remote's connection or abort the round.
					if (!IsCommittedTransport(event.peerId)) {
						++m_Stats.unboundConnectionFaults;
						break;
					}
					RejectRemote(event.peerId, event.reason.empty() ? "connection failed" : event.reason);
					break;
				}
				[[fallthrough]];
			case NetTransportEventType::LocalTransportFault:
				Fail(event.reason.empty() ? "transport error" : event.reason);
				break;
			case NetTransportEventType::PacketReceived: {
				const auto sender = std::find_if(m_RemoteTransports.begin(), m_RemoteTransports.end(), [&event](const auto& entry) {
					return entry.second == event.peerId;
				});
				if (sender == m_RemoteTransports.end()) return;
				m_LastReceiveMs = nowMs;
				NetLobbyDecodeResult decoded = NetLobbyProtocol::Decode(event.bytes);
				if (!decoded.ok) {
					// Another phase's packet on the shared wire: session leftovers, or the prior
					// match's in-flight lockstep frames when a rematch lobby round starts. A chat line
					// that fails decode is counted and dropped by the session, never cause to eject.
					uint16_t peekedType = 0;
					const bool chatTyped = NetProtocol::PeekMessageType(event.bytes.data(), event.bytes.size(), peekedType) &&
					                       peekedType == static_cast<uint16_t>(NetMessageType::Chat);
					if (decoded.error.code == NetLobbyErrorCode::BadMagic &&
					    (NetProtocol::Decode(event.bytes).ok || NetLockstepCodec::LooksLikePacket(event.bytes) || chatTyped)) {
						++m_Stats.ignoredSessionPackets;
						return;
					}
					++m_Stats.malformedMessages;
					if (m_Config.host) RejectRemote(event.peerId, decoded.error.message);
					else Fail(decoded.error.message);
					return;
				}
				uint8_t sessionSender = sender->first;
				const bool worldSender = m_Config.host && m_WorldTransferPeers.contains(sender->first);
				if (worldSender && m_Config.session) {
					const auto ready = m_Config.session->GetReadyPeers();
					const auto admitted = std::find_if(ready.begin(), ready.end(), [&](const auto& peer) { return peer.transportPeerId == event.peerId; });
					if (admitted == ready.end()) return;
					sessionSender = static_cast<uint8_t>(admitted->assignedPeerId + 1);
				} else if (worldSender) {
					for (const auto& [peer, connection]: m_Config.remoteTransportPeerIds)
						if (connection == event.peerId) sessionSender = peer;
					if (m_Config.remoteTransportPeerIds.empty() && m_Config.remoteTransportPeerId == event.peerId)
						sessionSender = m_Config.remotePeerId;
				}
				const bool allowed = std::visit([&](const auto& payload) {
					using Payload = std::decay_t<decltype(payload)>;
					if constexpr (std::is_same_v<Payload, NetLobbyMigration>)
						return event.lane == NetTransportLane::ControlReliable && (m_Config.host ? payload.kind == 1 && payload.peerId == sender->first : (payload.kind == 2 && payload.peerId == m_Config.localPeerId) || (payload.kind == 1 && payload.peerId == sender->first));
					if constexpr (std::is_same_v<Payload, NetLobbyStateChunk>)
						return event.lane == NetTransportLane::ControlReliable && (worldSender || m_Config.matchConfig.persistentWorld || !m_Config.host || (m_Config.snapshotProviderPeerId != 0 && sender->first == m_Config.snapshotProviderPeerId));
					// Only the hub binds seats; a client offering one is not a peer this round keeps.
					if constexpr (std::is_same_v<Payload, NetLobbySeatAssign>) return !m_Config.host;
					if (m_Config.host) {
						if constexpr (requires { payload.peerId; }) {
							return payload.peerId == sender->first || (worldSender && payload.peerId == sessionSender);
						}
						return false;
					}
					if constexpr (std::is_same_v<Payload, NetLobbyReady> || std::is_same_v<Payload, NetLobbyConfigAck>) return false;
					if constexpr (std::is_same_v<Payload, NetLobbyHello>)
						return payload.peerId == sender->first || (m_Config.enableMigration && payload.peerId != 0 && payload.peerId <= NetMatchConfigUtil::c_MaxPeerCount);
					if constexpr (std::is_same_v<Payload, NetLobbyAbort>)
						return payload.peerId == sender->first;
					// The host is a client's only remote and the roster's sole authority, so a state for
					// a peer we have no slot for yet is an ordering race with its match config, not an
					// impostor: HandlePeerState drops it. Failing the session over a name-and-ping
					// message left a joiner dead in a four-member lobby.
					if constexpr (std::is_same_v<Payload, NetLobbyPeerState>) {
						return true;
					}
					return true;
				}, decoded.message.payload);
				if (!allowed) {
					const uint32_t claimedPeer = std::visit([](const auto& payload) -> uint32_t {
						if constexpr (requires { payload.peerId; }) return payload.peerId;
						return 0;
					}, decoded.message.payload);
					System::PrintDiagnosticLine("[net-lobby] sender mismatch type=" + std::string(NetLobbyProtocol::MessageTypeName(NetLobbyProtocol::MessageTypeOf(decoded.message.payload))) +
					          " connection=" + std::to_string(event.peerId) + " expected=" + std::to_string(sender->first) + " claimed=" + std::to_string(claimedPeer));
					if (m_Config.host) RejectRemote(event.peerId, "lobby message does not match its connection");
					else Fail("invalid host lobby message");
					return;
				}
				++m_Stats.messagesReceived;
				if (m_Config.session) m_Config.session->NotePeerTraffic(event.peerId, m_Config.session->GetClockMs());
				m_LobbyUpConnections.insert(event.peerId);
				if (!worldSender || sender->first <= m_Config.matchConfig.peerCount) m_RemoteLobbyUp.insert(sender->first);
				// A bootstrap's session identity and world slot share one authenticated connection.
				if (worldSender) std::visit([&](auto& payload) {
					if constexpr (requires { payload.peerId; }) payload.peerId = sender->first;
				}, decoded.message.payload);
				if (const NetLobbyStateChunk* chunk = std::get_if<NetLobbyStateChunk>(&decoded.message.payload)) {
					uint8_t kind = 0;
					uint64_t value = 0;
					WorldJoinReport report;
					if (ParseWorldJoinReport(*chunk, kind, value, &report.workTicks, &report.workUs, &report.sentThrough)) {
						report.kind = kind; report.value = value; report.fromPeer = sender->first; report.pending = true;
						std::erase_if(m_WorldJoinReports, [&](const auto& pending) { return pending.kind == kind && pending.fromPeer == sender->first; });
						m_WorldJoinReports.push_back(report);
						break;
					}
					if (chunk->transferId == c_NetWorldTailTransferId) {
						// Only a joiner drains this; on the host it would grow for the world's life.
						if (!m_Config.host) {
							m_PendingTailBytes.insert(m_PendingTailBytes.end(), chunk->bytes.begin(), chunk->bytes.end());
						}
						break;
					}
				}
				HandleMessage(decoded.message);
				break;
			}
		}
	}

	void NetLobbySession::HandleMessage(const NetLobbyMessage& message) {
		std::visit([&](const auto& payload) {
			using Payload = std::decay_t<decltype(payload)>;
			if constexpr (std::is_same_v<Payload, NetLobbyHello>) {
				if (!m_Config.host && m_Config.enableMigration && !m_RemoteTransports.contains(payload.peerId) && !m_RemoteTransports.empty()) {
					const auto transport = m_RemoteTransports.begin()->second;
					m_RemoteTransports = {{payload.peerId, transport}};
					m_RemotePeerIds = {payload.peerId};
					if (m_Config.session)
						m_Config.session->AdoptLobbyHostPeerId(payload.peerId);
				}
			} else if constexpr (std::is_same_v<Payload, NetLobbyMigration>) {
				HandleMigration(payload);
			} else if constexpr (std::is_same_v<Payload, NetLobbyMatchConfig>) {
				HandleMatchConfig(payload);
			} else if constexpr (std::is_same_v<Payload, NetLobbyConfigAck>) {
				HandleConfigAck(payload);
			} else if constexpr (std::is_same_v<Payload, NetLobbyReady>) {
				HandleReady(payload);
			} else if constexpr (std::is_same_v<Payload, NetLobbyStart>) {
				HandleStart(payload);
			} else if constexpr (std::is_same_v<Payload, NetLobbyAbort>) {
				if (IsKnownRemote(payload.peerId)) {
					if (m_Config.host) RejectRemote(m_RemoteTransports.at(payload.peerId), payload.reason);
					else Reject(payload.reason);
				}
			} else if constexpr (std::is_same_v<Payload, NetLobbyPeerState>) {
				HandlePeerState(payload);
			} else if constexpr (std::is_same_v<Payload, NetLobbyStateChunk>) {
				HandleStateChunk(payload);
			} else if constexpr (std::is_same_v<Payload, NetLobbySeatAssign>) {
				HandleSeatAssign(payload);
			} else if constexpr (std::is_same_v<Payload, NetLobbyResume>) {
				HandleResume(payload);
			}
		}, message.payload);
	}

	void NetLobbySession::TimeoutWaitingForStart() {
		++m_Stats.timeouts;
		if (m_Config.host && m_Config.enableMigration && m_Config.activePeerCount == 0) {
			for (uint8_t peer = 1; peer <= m_Config.matchConfig.peerCount; ++peer) {
				if (m_MigrationEndpoints.contains(peer)) {
					continue;
				}
				std::string name = "peer " + std::to_string(peer);
				for (const auto& player: m_Config.matchConfig.players) {
					if (player.peerId == peer && !player.displayName.empty()) {
						name = player.displayName;
						break;
					}
				}
				Fail("waiting for " + name + "'s handover endpoint");
				return;
			}
		}
		Fail("timed out waiting for lobby start");
	}

	bool NetLobbySession::PrepareMigrationRoster() {
		if (m_Config.matchConfig.dedicated || m_Config.matchConfig.persistentWorld)
			return false;
		if (m_Config.activePeerCount != 0 && !m_Config.matchConfig.successorOrder.empty())
			return true;
		for (uint8_t peer = 1; peer <= m_Config.matchConfig.peerCount; ++peer)
			if (!m_MigrationEndpoints.contains(peer))
				return false;
		NetMatchConfig next = m_Config.matchConfig;
		next.migrationPeers.clear();
		for (const auto& [peer, endpoint]: m_MigrationEndpoints)
			if (peer <= next.peerCount)
				next.migrationPeers.push_back(endpoint);
		if (next.successorOrder.empty())
			for (uint8_t peer = 1; peer <= next.peerCount; ++peer)
				if (peer != next.hostPeerId)
					next.successorOrder.push_back(peer);
		std::string error;
		if (!NetMatchConfigUtil::ValidateLocalAlpha(next, &error)) {
			Fail(error);
			return false;
		}
		const auto hash = NetMatchConfigUtil::HashConfig(next);
		if (hash != m_MatchConfigHash) {
			m_Config.matchConfig = std::move(next);
			m_MatchConfigHash = hash;
			for (auto& [peer, acked]: m_ConfigAckedByPeer)
				acked = false;
		}
		return true;
	}

	void NetLobbySession::SendResumeOfferTo(uint8_t peerId) {
		if (!m_Config.host || m_Config.resumeMatchId.empty() || m_Config.resumeTick == 0 || !IsKnownRemote(peerId)) {
			return;
		}
		NetLobbyResume offer;
		offer.kind = 1;
		offer.peerId = m_Config.localPeerId;
		offer.savedTick = m_Config.resumeTick;
		offer.matchId = m_Config.resumeMatchId;
		offer.digest = m_Config.resumeDigest;
		offer.sideStateHash = m_Config.resumeSideStateHash;
		std::string error;
		(void)SendTo(m_RemoteTransports.at(peerId), offer, &error);
	}

	void NetLobbySession::GiveUpWaitingForResumeAnswers(uint64_t nowMs) {
		if (m_Config.resumeMatchId.empty() || m_OutgoingChunkCount == 0) {
			return;
		}
		for (uint8_t peerId: m_RemotePeerIds) {
			if (!ResumeAwaitsAnswer(peerId) || !IsRemoteLobbyUp(peerId)) {
				continue;
			}
			// The wait starts when the remote's own lobby is up: before that it has heard no offer.
			const auto waiting = m_ResumeWaitStartedMs.emplace(peerId, nowMs).first;
			if (nowMs < waiting->second || nowMs - waiting->second < c_ResumeAnswerWaitMs) {
				continue;
			}
			// A remote that never answers is a remote without that checkpoint as far as this round is
			// concerned: it is streamed the state, which is the path a peer without one always takes.
			m_ResumeAnsweredPeers.insert(peerId);
			System::PrintDiagnosticLine("[net-lobby] resume answer timed out for peer " + std::to_string(peerId) + "; streaming the state");
		}
	}

	void NetLobbySession::HandleResume(const NetLobbyResume& message) {
		if (m_Config.host) {
			// Only the peer itself may answer for its own copy, and only about the checkpoint offered.
			if (message.kind != 2 || !IsKnownRemote(message.peerId) || message.matchId != m_Config.resumeMatchId || message.savedTick != m_Config.resumeTick) {
				return;
			}
			m_ResumeAnsweredPeers.insert(message.peerId);
			if (message.held) {
				m_ResumeHeldPeers.insert(message.peerId);
			} else {
				m_ResumeHeldPeers.erase(message.peerId);
			}
			return;
		}
		if (message.kind != 1 || !IsKnownRemote(message.peerId) || message.matchId.empty()) {
			return;
		}
		NetLobbyResume answer;
		answer.kind = 2;
		answer.peerId = m_Config.localPeerId;
		// The store decides; an answer without one is "not held", which costs a transfer and nothing else.
		answer.held = m_Config.resumeHeld ? m_Config.resumeHeld(message) : false;
		answer.savedTick = message.savedTick;
		answer.matchId = message.matchId;
		answer.digest = message.digest;
		answer.sideStateHash = message.sideStateHash;
		m_ResumeAnsweredHeld = answer.held;
		std::string error;
		(void)SendTo(m_RemoteTransports.at(message.peerId), answer, &error);
	}

	void NetLobbySession::HandleMigration(const NetLobbyMigration& message) {
		if (!m_Config.enableMigration)
			return;
		if (m_Config.host) {
			if (message.kind != 1 || message.listenPort == 0 || message.listenAddrs.empty() || !message.sealedState.empty()) {
				Fail("invalid migration endpoint");
				return;
			}
			m_MigrationEndpoints[message.peerId] = {message.peerId, message.listenPort, message.listenAddrs};
			return;
		}
		if (message.kind == 1) {
			m_MigrationRequested = true;
			SendPeerState();
			return;
		}
		if (message.kind != 2 || message.configHash != m_MatchConfigHash || m_Config.matchConfig.successorOrder.empty())
			return;
		if (!m_Config.openMigration || !m_Config.openMigration(message)) {
			Fail("successor credential authentication failed");
			return;
		}
		m_OpenedMigrationHash = message.configHash;
		HandleMatchConfig({m_Config.matchConfig});
	}

	void NetLobbySession::HandleMatchConfig(const NetLobbyMatchConfig& message) {
		if (m_Config.host) {
			return;
		}
		// The ack carries this peer's id, so it waits for the one the host bound to this connection.
		if (m_Config.assignSeats && !m_SeatAssigned) {
			return;
		}
		const NetHash32 incomingHash = NetMatchConfigUtil::HashConfig(message.config);
		if (!message.config.successorOrder.empty() && m_OpenedMigrationHash != incomingHash) {
			m_Config.matchConfig = message.config;
			if (!m_Config.matchConfig.relay.Usable(RelayWallSeconds())) m_Config.matchConfig.relay = {};
			m_MatchConfigHash = incomingHash;
			return;
		}
		// The host resends config until every client acks; a repeat of the accepted config just re-acks.
		if (m_State != NetLobbyState::WaitingForConfig && incomingHash == m_MatchConfigHash) {
			m_Config.matchConfig.relay = message.config.relay.Usable(RelayWallSeconds()) ? message.config.relay : NetRelayConfig{};
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
		if (!m_Config.matchConfig.relay.Usable(RelayWallSeconds())) m_Config.matchConfig.relay = {};
		m_MatchConfigHash = incomingHash;
		m_StartFrame = 0;
		Send(NetLobbyConfigAck{m_Config.localPeerId, true, m_MatchConfigHash, ""});
		m_State = NetLobbyState::WaitingForReady;
		m_ReadySent = false;
		SendReadyIfNeeded();
	}

	void NetLobbySession::HandleConfigAck(const NetLobbyConfigAck& message) {
		if (!m_Config.host || !IsKnownRemote(message.peerId)) {
			return;
		}
		++m_Stats.configAcksReceived;
		if (!message.accepted) {
			RejectRemote(m_RemoteTransports.at(message.peerId), message.reason.empty() ? "match config rejected" : message.reason);
			return;
		}
		if (message.matchConfigHash != m_MatchConfigHash) {
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
		m_PeerStatePending = true;
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
		if (m_IncomingStateId != 0 && !m_IncomingStateComplete) {
			Fail("lobby started before state transfer completed");
			return;
		}
		m_StartFrame = message.startFrame;
		m_State = NetLobbyState::Started;
	}

	void NetLobbySession::HandlePeerState(const NetLobbyPeerState& message) {
		if (m_Config.host && m_WorldTransferPeers.contains(message.peerId) && message.peerId > m_Config.matchConfig.peerCount) return;
		// Accept any roster peer, not just direct remotes: a client hears its SIBLINGS through the
		// host's relay, so every lobby shows real names and readies for the whole roster.
		if (message.peerId == 0 || message.peerId == m_Config.localPeerId) {
			return;
		}
		// Only peers this config seats: a state that outran its match config names a peer we cannot
		// place, and the next periodic state carries it again once the config lands.
		if (!m_Config.host && std::none_of(m_Config.matchConfig.players.begin(), m_Config.matchConfig.players.end(),
		                                   [&](const NetMatchPlayerSlot& slot) { return slot.peerId == message.peerId; })) {
			++m_Stats.unconfiguredPeerStates;
			return;
		}
		if (!message.connected) {
			if (m_Config.host) {
				RejectRemote(m_RemoteTransports.at(message.peerId), "client marked itself disconnected");
			} else {
				m_RemoteNamesByPeer.erase(message.peerId);
				m_RemoteReadyByPeer.erase(message.peerId);
				m_RemotePingByPeer.erase(message.peerId);
				m_RemotePlatformsByPeer.erase(message.peerId);
			}
			return;
		}
		m_RemoteNamesByPeer[message.peerId] = message.displayName;
		m_RemotePlatformsByPeer[message.peerId] = message.platform;
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
