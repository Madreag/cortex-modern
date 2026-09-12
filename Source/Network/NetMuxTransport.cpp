#include "NetMuxTransport.h"

#include <utility>

namespace RTE {

	namespace {
		void SetMuxError(std::string* error, const std::string& message) {
			if (error) {
				*error = message;
			}
		}
	} // namespace

	NetMuxTransport::NetMuxTransport() {
		auto ip = std::make_unique<GnsTransport>();
		auto p2p = std::make_unique<GnsTransport>();
		m_IpGns = ip.get();
		m_P2PGns = p2p.get();
		m_Ip = std::move(ip);
		m_P2P = std::move(p2p);
	}

	NetMuxTransport::NetMuxTransport(std::unique_ptr<INetTransport> ip, std::unique_ptr<INetTransport> p2p) : m_Ip(std::move(ip)), m_P2P(std::move(p2p)) {}

	NetMuxTransport::~NetMuxTransport() {
		Stop();
	}

	void NetMuxTransport::SetHostP2P(int virtualPort, const GnsP2PConfig& config) {
		m_HostVirtualPort = virtualPort;
		m_HostP2PConfig = config;
		m_HostP2PArmed = true;
	}

	void NetMuxTransport::SetJoinSpec(JoinSpec spec) {
		m_JoinSpec = std::move(spec);
		m_HasJoinSpec = true;
	}

	void NetMuxTransport::SetPump(std::function<void()> pump) {
		std::lock_guard<std::mutex> lock(m_TaskMutex);
		m_Pump = std::move(pump);
	}

	void NetMuxTransport::Post(std::function<void()> task) {
		std::lock_guard<std::mutex> lock(m_TaskMutex);
		m_Tasks.push_back(std::move(task));
	}

	size_t NetMuxTransport::PendingTasks() const {
		std::lock_guard<std::mutex> lock(m_TaskMutex);
		return m_Tasks.size();
	}

	bool NetMuxTransport::StartHost(uint16_t port, std::string* error) {
		// The ICE listen goes first: binding the session identity is a process-wide ResetIdentity,
		// which GNS refuses once any listen socket is open.
		if (m_HostP2PArmed) {
			if (m_P2PGns) {
				if (!m_P2PGns->StartHostP2P(m_HostVirtualPort, m_HostP2PConfig, error)) {
					return false;
				}
			} else if (!m_P2P->StartHost(port, error)) {
				return false;
			}
		}
		if (!m_Ip->StartHost(port, error)) {
			m_P2P->Stop();
			return false;
		}
		return true;
	}

	bool NetMuxTransport::Connect(const std::string& address, uint16_t port, std::string* error) {
		if (!m_HasJoinSpec) {
			return m_Ip->Connect(address, port, error);
		}
		if (!m_P2PGns) {
			SetMuxError(error, "the mux's ICE half cannot dial a session id");
			return false;
		}
		ISteamNetworkingConnectionSignaling* signaling = m_JoinSpec.makeSignaling ? m_JoinSpec.makeSignaling() : nullptr;
		if (!signaling) {
			SetMuxError(error, "the signaling channel for the session-id join is not open");
			return false;
		}
		return m_P2PGns->ConnectP2P(signaling, m_JoinSpec.peerIdentity, m_JoinSpec.remoteVirtualPort, m_JoinSpec.p2p, error);
	}

	bool NetMuxTransport::Send(NetPeerId peerId, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error, bool* congested) {
		return IsP2P(peerId) ? m_P2P->Send(Untag(peerId), lane, bytes, error, congested) : m_Ip->Send(peerId, lane, bytes, error, congested);
	}

	void NetMuxTransport::Disconnect(NetPeerId peerId, const std::string& reason) {
		if (IsP2P(peerId)) {
			m_P2P->Disconnect(Untag(peerId), reason);
		} else {
			m_Ip->Disconnect(peerId, reason);
		}
	}

	void NetMuxTransport::Stop() {
		m_Ip->Stop();
		m_P2P->Stop();
		std::lock_guard<std::mutex> lock(m_TaskMutex);
		m_Tasks.clear();
	}

	std::vector<NetTransportEvent> NetMuxTransport::PollEvents() {
		std::vector<std::function<void()>> tasks;
		std::function<void()> pump;
		{
			std::lock_guard<std::mutex> lock(m_TaskMutex);
			tasks.swap(m_Tasks);
			pump = m_Pump;
		}
		for (const std::function<void()>& task : tasks) {
			task();
		}
		if (pump) {
			pump();
		}

		std::vector<NetTransportEvent> events = m_Ip->PollEvents();
		m_IpEvents += events.size();
		for (NetTransportEvent& event : m_P2P->PollEvents()) {
			// A local fault names no peer, and c_InvalidNetPeerId must stay invalid through the tag.
			if (event.peerId != c_InvalidNetPeerId) {
				event.peerId = Tag(event.peerId);
			}
			events.push_back(std::move(event));
			++m_P2PEvents;
		}
		return events;
	}

	uint32_t NetMuxTransport::GetPeerPingMs(NetPeerId peerId) const {
		return IsP2P(peerId) ? m_P2P->GetPeerPingMs(Untag(peerId)) : m_Ip->GetPeerPingMs(peerId);
	}

} // namespace RTE
