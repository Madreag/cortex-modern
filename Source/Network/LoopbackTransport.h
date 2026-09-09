#pragma once

#include "NetTransport.h"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace RTE {

	struct LoopbackTransportConfig {
		uint32_t latencyMs = 0;
		uint32_t jitterMs = 0;
		bool reorderUnreliable = false;
		uint32_t unreliableDropEveryN = 0;
		uint32_t unreliableDuplicateEveryN = 0;
		// Every send to this peer is refused outright, the way a connection that cannot carry anything
		// behaves: the message is never queued, and no amount of waiting will change that.
		NetPeerId refuseSendsToPeer = c_InvalidNetPeerId;
		// A local Disconnect tells us nothing back, the way GNS behaved before it was made to report
		// its own closes: state keyed on that peer only gets cleaned up if the caller does it itself.
		bool silentLocalDisconnect = false;
		// A metered per-peer send queue, as a real socket has: bytes accumulate on send and drain at
		// this rate as time advances, and a send past the budget is refused as congestion. Zero is the
		// unmetered queue every other test wants.
		uint32_t sendBufferBytes = 0;
		uint32_t drainBytesPerSecond = 0;
		// Meter only this peer's queue, leaving every other link healthy.
		NetPeerId meterOnlyPeer = c_InvalidNetPeerId;
	};

	/// What a transport did to its connections' send rate, and when.
	struct LoopbackRateChange {
		uint64_t atMs = 0;
		bool bulk = false;
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
		bool Send(NetPeerId peerId, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error = nullptr, bool* congested = nullptr) override;
		void Disconnect(NetPeerId peerId, const std::string& reason) override;
		void Stop() override;
		std::vector<NetTransportEvent> PollEvents() override;
		void SetBulkTransferMode(bool on) override;

		/// Gets whether the connections are still carrying the bulk rate.
		bool IsBulkTransferMode() const { return m_BulkTransfer; }
		/// Gets every raise and drop of the bulk rate, in order, for a test to read the timing off.
		const std::vector<LoopbackRateChange>& GetRateChanges() const { return m_RateChanges; }
		/// Gets bytes this transport has sent but not yet put in front of the far end.
		uint64_t InFlightBytes() const;

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

		void DrainSendQueues(uint64_t deltaMs);
		void RetireInFlight();
		void LowerBulkRateWhenDrained();
		void NoteInFlight(NetPeerId peerId, uint64_t deliverAtMs, size_t bytes);

		uint64_t m_NowMs = 0;
		uint64_t m_OrderCounter = 0;
		std::map<NetPeerId, uint64_t> m_QueuedBytes; //!< The metered queue's depth per peer.
		std::map<NetPeerId, std::deque<std::pair<uint64_t, size_t>>> m_InFlight; //!< peerId -> (arrival, bytes) not yet delivered.
		std::vector<LoopbackRateChange> m_RateChanges;
		bool m_BulkTransfer = false;
		bool m_BulkDrainPending = false;
		uint32_t m_SendCounter = 0;
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
