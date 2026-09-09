#pragma once

#include "NetTransport.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace RTE {

	struct LoopbackTransportConfig {
		uint32_t latencyMs = 0;
		uint32_t jitterMs = 0;
		bool reorderUnreliable = false;
		uint32_t unreliableDropEveryN = 0;
		uint32_t unreliableDuplicateEveryN = 0;
		// Every send to this peer is refused, as a full reliable send buffer does: the message is
		// never queued, so on an ordered lane the receiver has a gap it can never fill.
		NetPeerId refuseSendsToPeer = c_InvalidNetPeerId;
		// Sends to that peer go through until this many have: a buffer that fits a start and not the
		// frame behind it refuses in the middle of a batch, which an all-or-nothing knob cannot do.
		uint32_t acceptedSendsBeforeRefusing = 0;
		// A local Disconnect tells us nothing back, the way GNS behaved before it was made to report
		// its own closes: state keyed on that peer only gets cleaned up if the caller does it itself.
		bool silentLocalDisconnect = false;
	};

	class LoopbackTransport : public INetTransport {
	public:
		LoopbackTransport();
		~LoopbackTransport() override;

		LoopbackTransport(const LoopbackTransport&) = delete;
		LoopbackTransport& operator=(const LoopbackTransport&) = delete;

		void SetFaultConfig(const LoopbackTransportConfig& config);
		void AdvanceTimeMs(uint64_t deltaMs);
		uint64_t NowMs() const { return m_NowMs; }

		bool StartHost(uint16_t port, std::string* error = nullptr) override;
		bool Connect(const std::string& address, uint16_t port, std::string* error = nullptr) override;
		bool Send(NetPeerId peerId, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error = nullptr) override;
		void Disconnect(NetPeerId peerId, const std::string& reason) override;
		void Stop() override;
		std::vector<NetTransportEvent> PollEvents() override;

	private:
		struct ScheduledEvent {
			uint64_t deliverAtMs = 0;
			uint64_t order = 0;
			NetTransportEvent event;
		};

		struct HostPeer {
			LoopbackTransport* client = nullptr;
			NetPeerId clientPeerId = c_InvalidNetPeerId;
		};

		void ScheduleEvent(NetTransportEvent event, uint64_t delayMs, bool allowReorder);
		void EnqueueImmediate(NetTransportEvent event);
		bool IsConnectedToHost() const;
		uint64_t ComputeDelay(NetTransportLane lane, uint32_t sendOrdinal) const;
		bool ShouldDrop(NetTransportLane lane, uint32_t sendOrdinal) const;
		bool ShouldDuplicate(NetTransportLane lane, uint32_t sendOrdinal) const;
		void RemoveHostPeer(NetPeerId peerId, const std::string& reason, bool notifyClient);
		void RemoveClientConnection(const std::string& reason, bool notifyHost);

		static void SetError(std::string* error, const std::string& message);

		uint64_t m_NowMs = 0;
		uint64_t m_OrderCounter = 0;
		uint32_t m_SendCounter = 0;
		uint32_t m_AcceptedBeforeRefusing = 0;
		bool m_IsHost = false;
		bool m_IsStarted = false;
		uint16_t m_Port = 0;
		LoopbackTransportConfig m_Config;
		std::vector<ScheduledEvent> m_Events;
		std::map<NetPeerId, HostPeer> m_HostPeers;
		LoopbackTransport* m_ClientHost = nullptr;
		NetPeerId m_ClientHostPeerId = c_InvalidNetPeerId;
		NetPeerId m_HostSidePeerId = c_InvalidNetPeerId;
		NetPeerId m_NextHostPeerId = 1;

		static std::map<uint16_t, LoopbackTransport*> s_Hosts;
	};

} // namespace RTE
