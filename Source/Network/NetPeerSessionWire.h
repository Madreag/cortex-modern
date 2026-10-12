#pragma once

#include "NetTransport.h"

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace RTE {
	struct NetHostMigrationMessage;

	struct NetPeerProbe {
		std::unique_ptr<INetTransport> transport;
		size_t nextAddress = 0;
		uint64_t lastDialMs = 0;
		std::string address;
		bool answered = false;
		NetPeerId connection = c_InvalidNetPeerId;
		uint64_t lastHelloMs = 0;
	};

	// Native links belong to the session, so changing the administrator cannot close a frame path.
	struct NetPeerSessionLinks {
		std::unique_ptr<INetTransport> listener;
		std::map<uint8_t, NetPeerProbe> probes;
		std::map<NetPeerId, uint8_t> listenerBindings, primaryBindings;
		INetTransport* primary = nullptr;
		const void* frameReader = nullptr;
		std::function<uint64_t()> sessionClock;
		bool primaryListener = false, sessionAttached = false;
		bool frameBlackout = false;
		uint64_t sessionId = 0, nextSequence = 1;
		uint8_t localPeerId = 0, hostPeerId = 1;
		std::array<uint8_t, 32> key{};
		std::deque<std::array<uint8_t, 32>> earlierKeys;
		std::map<NetPeerId, uint8_t> sessionRoutes;
		std::map<NetPeerId, std::pair<INetTransport*, NetPeerId>> admissions;
		std::map<NetPeerId, std::pair<INetTransport*, NetPeerId>> admittedRoutes;
		// An admitted seat still reads its raw link until it speaks on this wire.
		std::set<uint8_t> rawReaders;
		struct FrameFlow { uint64_t sent = 0, received = 0, sentAtMs = 0, heardAtMs = 0; };
		std::map<const INetTransport*, std::map<NetPeerId, FrameFlow>> frameFlow;
		NetPeerId nextAdmission = 0x80000000U;
		struct Pending { uint64_t sequence = 0; NetTransportLane lane; std::vector<uint8_t> bytes; uint64_t sendAfterMs = 0; std::array<uint8_t, 32> sendKey{}; uint64_t previous = 0; bool acknowledged = false; uint64_t lastSentMs = 0; uint8_t repeats = 0; };
		std::map<uint8_t, std::deque<Pending>> pending;
		size_t pendingBytes = 0;
		std::map<uint8_t, uint64_t> queuedThrough, acknowledgedThrough, progressAt;
		std::map<uint8_t, uint64_t> received;
		struct Incoming { uint64_t previous = 0; std::vector<uint8_t> bytes; std::array<uint8_t, 32> readKey{}; };
		std::map<uint8_t, std::map<uint64_t, Incoming>> incoming;
		size_t incomingBytes = 0;
		std::deque<NetTransportEvent> events;
		struct Closing { std::string reason; uint64_t deadlineMs = 0; };
		std::map<uint8_t, Closing> closing;
		bool SendTo(uint8_t peer, NetTransportLane lane, const std::vector<uint8_t>& bytes, bool framePacket = false);
		std::vector<std::pair<INetTransport*, NetPeerId>> RoutesTo(uint8_t peer) const;
		std::pair<INetTransport*, NetPeerId> RouteTo(uint8_t peer) const;
		void NoteFrameRead(const INetTransport& wire, NetPeerId native);
		void RetireSpareRoutes(uint8_t peer);
		NetPeerId SessionRoute(uint8_t peer) const;
		uint8_t PeerOn(const INetTransport& wire, NetPeerId native) const;
		bool IsNativeRoute(NetPeerId route, const INetTransport* wire, NetPeerId native) const;
		void ForwardAdmission(INetTransport& wire, const NetTransportEvent& event);
		void BindAdmission(uint8_t peer, NetPeerId sessionRoute);
		void SetSessionKey(const std::array<uint8_t, 32>& next);
		bool ReadSessionPacket(const std::vector<uint8_t>& bytes, NetHostMigrationMessage& message, std::array<uint8_t, 32>* readKey = nullptr) const;
		bool HandleSessionMessage(const NetHostMigrationMessage& message, const std::array<uint8_t, 32>* readKey = nullptr);
		void FlushSession(uint64_t nowMs);
		void PollSession(uint64_t nowMs);
		uint64_t ClockMs() const;
	};

	class NetPeerSessionWire final : public INetTransport {
	public:
		explicit NetPeerSessionWire(std::shared_ptr<NetPeerSessionLinks> links): m_Links(std::move(links)) {}
		bool StartHost(uint16_t, std::string* = nullptr) override;
		bool Connect(const std::string&, uint16_t, std::string* error = nullptr) override;
		bool Send(NetPeerId peer, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error = nullptr, bool* congested = nullptr) override;
		void Disconnect(NetPeerId peer, const std::string& reason) override;
		void Stop() override;
		std::vector<NetTransportEvent> PollEvents() override;
		uint32_t GetPeerPingMs(NetPeerId peer) const override;
		bool IsPeerPingMeasured(NetPeerId peer) const override;
		std::string GetConnectedRoute(NetPeerId peer) const override;
	private:
		std::shared_ptr<NetPeerSessionLinks> m_Links;
	};

} // namespace RTE
