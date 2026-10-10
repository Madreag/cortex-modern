#pragma once

#include "NetTransport.h"

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <utility>

namespace RTE {

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
		bool primaryListener = false, sessionAttached = false;
		uint64_t sessionId = 0, nextSequence = 1;
		uint8_t localPeerId = 0, hostPeerId = 1;
		std::array<uint8_t, 32> key{};
		std::map<NetPeerId, uint8_t> sessionRoutes;
		std::map<NetPeerId, std::pair<INetTransport*, NetPeerId>> admissions;
		NetPeerId nextAdmission = 0x80000000U;
		struct Pending { uint64_t sequence = 0; NetTransportLane lane; std::vector<uint8_t> bytes; };
		std::map<uint8_t, std::deque<Pending>> pending;
		size_t pendingBytes = 0;
		std::map<uint8_t, uint64_t> received;
		std::deque<NetTransportEvent> events;
		bool SendTo(uint8_t peer, NetTransportLane lane, const std::vector<uint8_t>& bytes);
		NetPeerId SessionRoute(uint8_t peer) const;
		void ForwardAdmission(INetTransport& wire, const NetTransportEvent& event);
		void BindAdmission(uint8_t peer, NetPeerId sessionRoute);
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
