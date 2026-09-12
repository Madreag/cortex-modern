#pragma once

#include "GnsTransport.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace RTE {

	/// A host's direct-IP listen and its ICE listen behind one INetTransport. Peer ids from the ICE
	/// half carry c_P2PTag, so Send, Disconnect and every event route back to the half they came from.
	/// Everything that touches a half runs on the thread that calls PollEvents: GnsTransport has no
	/// locks, so other threads hand work over with Post().
	class NetMuxTransport final : public INetTransport {
	public:
		/// What a session-id join dials instead of an address.
		struct JoinSpec {
			std::string peerIdentity;
			int remoteVirtualPort = 0;
			GnsP2PConfig p2p;
			std::function<ISteamNetworkingConnectionSignaling*()> makeSignaling;
		};

		static constexpr NetPeerId c_P2PTag = 0x80000000u;
		static bool IsP2P(NetPeerId peerId) { return (peerId & c_P2PTag) != 0; }
		static NetPeerId Tag(NetPeerId peerId) { return peerId | c_P2PTag; }
		static NetPeerId Untag(NetPeerId peerId) { return peerId & ~c_P2PTag; }

		NetMuxTransport();
		/// Test seam: halves that open no socket. The ICE-only entry points are unavailable on these.
		NetMuxTransport(std::unique_ptr<INetTransport> ip, std::unique_ptr<INetTransport> p2p);
		~NetMuxTransport() override;

		NetMuxTransport(const NetMuxTransport&) = delete;
		NetMuxTransport& operator=(const NetMuxTransport&) = delete;

		INetTransport& Ip() { return *m_Ip; }
		INetTransport& P2P() { return *m_P2P; }
		/// Null on the test seam's halves.
		GnsTransport* P2PGns() { return m_P2PGns; }
		GnsTransport* IpGns() { return m_IpGns; }

		/// Host: the virtual port the ICE listen opens on and the identity bound before it does.
		/// StartHost then opens the ICE listen FIRST, because GNS refuses an identity change once any
		/// listen socket of the process is up.
		void SetHostP2P(int virtualPort, const GnsP2PConfig& config);
		bool HostP2PArmed() const { return m_HostP2PArmed; }
		const GnsP2PConfig& HostP2PConfig() const { return m_HostP2PConfig; }
		int HostVirtualPort() const { return m_HostVirtualPort; }
		/// Joiner: Connect() ignores its address and dials this.
		void SetJoinSpec(JoinSpec spec);
		bool HasJoinSpec() const { return m_HasJoinSpec; }
		const JoinSpec& GetJoinSpec() const { return m_JoinSpec; }

		/// Runs at the top of every PollEvents, on the transport-owner thread.
		void SetPump(std::function<void()> pump);
		/// Queues work for that thread. Safe from any thread.
		void Post(std::function<void()> task);
		size_t PendingTasks() const;

		bool StartHost(uint16_t port, std::string* error = nullptr) override;
		bool Connect(const std::string& address, uint16_t port, std::string* error = nullptr) override;
		bool Send(NetPeerId peerId, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error = nullptr, bool* congested = nullptr) override;
		void Disconnect(NetPeerId peerId, const std::string& reason) override;
		void Stop() override;
		std::vector<NetTransportEvent> PollEvents() override;
		uint32_t GetPeerPingMs(NetPeerId peerId) const override;

		/// How many events each half has produced, for the report.
		uint64_t IpEvents() const { return m_IpEvents; }
		uint64_t P2PEvents() const { return m_P2PEvents; }

	private:
		std::unique_ptr<INetTransport> m_Ip;
		std::unique_ptr<INetTransport> m_P2P;
		GnsTransport* m_IpGns = nullptr;
		GnsTransport* m_P2PGns = nullptr;

		bool m_HostP2PArmed = false;
		int m_HostVirtualPort = 0;
		GnsP2PConfig m_HostP2PConfig;
		bool m_HasJoinSpec = false;
		JoinSpec m_JoinSpec;

		mutable std::mutex m_TaskMutex;
		std::vector<std::function<void()>> m_Tasks;
		std::function<void()> m_Pump;

		uint64_t m_IpEvents = 0;
		uint64_t m_P2PEvents = 0;
	};

} // namespace RTE
