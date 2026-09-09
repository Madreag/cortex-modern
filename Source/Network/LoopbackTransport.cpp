#include "LoopbackTransport.h"

#include <algorithm>

namespace RTE {

	std::map<uint16_t, LoopbackTransport*> LoopbackTransport::s_Hosts;

	LoopbackTransport::LoopbackTransport() = default;

	LoopbackTransport::~LoopbackTransport() {
		Stop();
	}

	void LoopbackTransport::SetFaultConfig(const LoopbackTransportConfig& config) {
		m_Config = config;
	}

	void LoopbackTransport::AdvanceTimeMs(uint64_t deltaMs) {
		m_NowMs += deltaMs;
		DrainSendQueues(deltaMs);
	}

	void LoopbackTransport::DrainSendQueues(uint64_t deltaMs) {
		if (m_Config.drainBytesPerSecond == 0) {
			return;
		}
		const uint64_t drained = (static_cast<uint64_t>(m_Config.drainBytesPerSecond) * deltaMs) / 1000U;
		for (auto& [peerId, queued] : m_QueuedBytes) {
			queued = queued > drained ? queued - drained : 0;
		}
	}

	bool LoopbackTransport::StartHost(uint16_t port, std::string* error) {
		Stop();
		if (port == 0) {
			SetError(error, "loopback host port must be nonzero");
			return false;
		}
		if (s_Hosts.find(port) != s_Hosts.end()) {
			SetError(error, "loopback host port is already in use");
			return false;
		}
		m_IsHost = true;
		m_IsStarted = true;
		m_Port = port;
		m_NextHostPeerId = 1;
		s_Hosts[port] = this;
		return true;
	}

	bool LoopbackTransport::Connect(const std::string&, uint16_t port, std::string* error) {
		Stop();
		const auto hostIt = s_Hosts.find(port);
		if (hostIt == s_Hosts.end() || !hostIt->second || !hostIt->second->m_IsStarted || !hostIt->second->m_IsHost) {
			SetError(error, "loopback host was not found");
			EnqueueImmediate({NetTransportEventType::ConnectionFailed, c_InvalidNetPeerId, NetTransportLane::ControlReliable, {}, "loopback host was not found"});
			return false;
		}

		LoopbackTransport* host = hostIt->second;
		const NetPeerId hostPeerId = host->m_NextHostPeerId++;
		const NetPeerId clientPeerId = 1;
		host->m_HostPeers[hostPeerId] = HostPeer{this, clientPeerId};

		m_IsHost = false;
		m_IsStarted = true;
		m_Port = port;
		m_ClientHost = host;
		m_ClientHostPeerId = clientPeerId;
		m_HostSidePeerId = hostPeerId;

		host->ScheduleEvent({NetTransportEventType::PeerConnected, hostPeerId, NetTransportLane::ControlReliable, {}, {}}, 0, false);
		ScheduleEvent({NetTransportEventType::PeerConnected, clientPeerId, NetTransportLane::ControlReliable, {}, {}}, 0, false);
		return true;
	}

	bool LoopbackTransport::Send(NetPeerId peerId, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error, bool* congested) {
		if (congested) *congested = false;
		if (!m_IsStarted) {
			SetError(error, "loopback transport is not started");
			return false;
		}
		if (m_Config.refuseSendsToPeer != c_InvalidNetPeerId && peerId == m_Config.refuseSendsToPeer) {
			SetError(error, "loopback peer refuses every send");
			return false;
		}
		if (m_Config.sendBufferBytes > 0) {
			uint64_t& queued = m_QueuedBytes[peerId];
			if (queued + bytes.size() > m_Config.sendBufferBytes) {
				if (congested) *congested = true;
				SetError(error, "loopback send buffer is full (" + std::to_string(queued) + " of " +
				                std::to_string(m_Config.sendBufferBytes) + " bytes queued)");
				return false;
			}
			queued += bytes.size();
		}
		const uint32_t sendOrdinal = ++m_SendCounter;
		if (ShouldDrop(lane, sendOrdinal)) {
			return true;
		}
		const uint64_t delayMs = ComputeDelay(lane, sendOrdinal);
		const bool allowReorder = lane != NetTransportLane::ControlReliable;
		const bool duplicate = ShouldDuplicate(lane, sendOrdinal);

		if (m_IsHost) {
			const auto peerIt = m_HostPeers.find(peerId);
			if (peerIt == m_HostPeers.end() || !peerIt->second.client) {
				SetError(error, "loopback host peer was not found");
				return false;
			}
			peerIt->second.client->ScheduleEvent({NetTransportEventType::PacketReceived, peerIt->second.clientPeerId, lane, bytes, {}}, delayMs, allowReorder);
			if (duplicate) {
				peerIt->second.client->ScheduleEvent({NetTransportEventType::PacketReceived, peerIt->second.clientPeerId, lane, bytes, {}}, delayMs, allowReorder);
			}
			return true;
		}

		if (peerId != m_ClientHostPeerId || !IsConnectedToHost()) {
			SetError(error, "loopback client is not connected to that peer");
			return false;
		}
		m_ClientHost->ScheduleEvent({NetTransportEventType::PacketReceived, m_HostSidePeerId, lane, bytes, {}}, delayMs, allowReorder);
		if (duplicate) {
			m_ClientHost->ScheduleEvent({NetTransportEventType::PacketReceived, m_HostSidePeerId, lane, bytes, {}}, delayMs, allowReorder);
		}
		return true;
	}

	void LoopbackTransport::Disconnect(NetPeerId peerId, const std::string& reason) {
		if (!m_IsStarted) {
			return;
		}
		if (m_IsHost) {
			RemoveHostPeer(peerId, reason, true);
		} else if (peerId == m_ClientHostPeerId) {
			RemoveClientConnection(reason, true);
		}
	}

	void LoopbackTransport::Stop() {
		if (m_IsHost) {
			for (auto& [peerId, peer] : m_HostPeers) {
				if (peer.client) {
					peer.client->m_ClientHost = nullptr;
					peer.client->m_HostSidePeerId = c_InvalidNetPeerId;
					peer.client->ScheduleEvent({NetTransportEventType::PeerDisconnected, peer.clientPeerId, NetTransportLane::ControlReliable, {}, "host stopped"}, 0, false);
				}
			}
			m_HostPeers.clear();
			if (m_Port != 0) {
				const auto hostIt = s_Hosts.find(m_Port);
				if (hostIt != s_Hosts.end() && hostIt->second == this) {
					s_Hosts.erase(hostIt);
				}
			}
		} else if (m_ClientHost) {
			RemoveClientConnection("client stopped", true);
		}

		m_IsHost = false;
		m_IsStarted = false;
		m_Port = 0;
		m_ClientHost = nullptr;
		m_ClientHostPeerId = c_InvalidNetPeerId;
		m_HostSidePeerId = c_InvalidNetPeerId;
		m_Events.clear();
	}

	std::vector<NetTransportEvent> LoopbackTransport::PollEvents() {
		std::stable_sort(m_Events.begin(), m_Events.end(), [](const ScheduledEvent& lhs, const ScheduledEvent& rhs) {
			if (lhs.deliverAtMs != rhs.deliverAtMs) {
				return lhs.deliverAtMs < rhs.deliverAtMs;
			}
			return lhs.order < rhs.order;
		});

		std::vector<NetTransportEvent> ready;
		auto firstPending = m_Events.begin();
		while (firstPending != m_Events.end() && firstPending->deliverAtMs <= m_NowMs) {
			ready.push_back(std::move(firstPending->event));
			++firstPending;
		}
		m_Events.erase(m_Events.begin(), firstPending);
		return ready;
	}

	void LoopbackTransport::ScheduleEvent(NetTransportEvent event, uint64_t delayMs, bool allowReorder) {
		uint64_t deliverAtMs = m_NowMs + delayMs;
		if (allowReorder && m_Config.reorderUnreliable) {
			deliverAtMs += (m_OrderCounter % 2U) == 0U ? 3U : 0U;
		}
		m_Events.push_back({deliverAtMs, m_OrderCounter++, std::move(event)});
	}

	void LoopbackTransport::EnqueueImmediate(NetTransportEvent event) {
		ScheduleEvent(std::move(event), 0, false);
	}

	bool LoopbackTransport::IsConnectedToHost() const {
		return m_ClientHost && m_HostSidePeerId != c_InvalidNetPeerId;
	}

	uint64_t LoopbackTransport::ComputeDelay(NetTransportLane lane, uint32_t sendOrdinal) const {
		uint64_t delay = m_Config.latencyMs;
		if (m_Config.jitterMs > 0 && lane != NetTransportLane::ControlReliable) {
			delay += (sendOrdinal % (m_Config.jitterMs + 1U));
		}
		return delay;
	}

	bool LoopbackTransport::ShouldDrop(NetTransportLane lane, uint32_t sendOrdinal) const {
		return lane == NetTransportLane::InputUnreliable &&
		       m_Config.unreliableDropEveryN > 0 &&
		       (sendOrdinal % m_Config.unreliableDropEveryN) == 0;
	}

	bool LoopbackTransport::ShouldDuplicate(NetTransportLane lane, uint32_t sendOrdinal) const {
		return lane == NetTransportLane::InputUnreliable &&
		       m_Config.unreliableDuplicateEveryN > 0 &&
		       (sendOrdinal % m_Config.unreliableDuplicateEveryN) == 0;
	}

	void LoopbackTransport::RemoveHostPeer(NetPeerId peerId, const std::string& reason, bool notifyClient) {
		const auto peerIt = m_HostPeers.find(peerId);
		if (peerIt == m_HostPeers.end()) {
			return;
		}
		LoopbackTransport* client = peerIt->second.client;
		const NetPeerId clientPeerId = peerIt->second.clientPeerId;
		m_HostPeers.erase(peerIt);
		if (client) {
			client->m_ClientHost = nullptr;
			client->m_HostSidePeerId = c_InvalidNetPeerId;
			if (notifyClient) {
				client->ScheduleEvent({NetTransportEventType::PeerDisconnected, clientPeerId, NetTransportLane::ControlReliable, {}, reason}, 0, false);
			}
		}
		if (!m_Config.silentLocalDisconnect) {
			ScheduleEvent({NetTransportEventType::PeerDisconnected, peerId, NetTransportLane::ControlReliable, {}, reason}, 0, false);
		}
	}

	void LoopbackTransport::RemoveClientConnection(const std::string& reason, bool notifyHost) {
		LoopbackTransport* host = m_ClientHost;
		const NetPeerId hostSidePeerId = m_HostSidePeerId;
		m_ClientHost = nullptr;
		m_HostSidePeerId = c_InvalidNetPeerId;
		if (host) {
			host->m_HostPeers.erase(hostSidePeerId);
			if (notifyHost) {
				host->ScheduleEvent({NetTransportEventType::PeerDisconnected, hostSidePeerId, NetTransportLane::ControlReliable, {}, reason}, 0, false);
			}
		}
		ScheduleEvent({NetTransportEventType::PeerDisconnected, m_ClientHostPeerId, NetTransportLane::ControlReliable, {}, reason}, 0, false);
	}

	void LoopbackTransport::SetError(std::string* error, const std::string& message) {
		if (error) {
			*error = message;
		}
	}

} // namespace RTE
