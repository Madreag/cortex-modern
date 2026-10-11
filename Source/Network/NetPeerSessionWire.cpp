#include "NetPeerSessionWire.h"

#include "NetLockstep.h"

namespace RTE {

	std::pair<INetTransport*, NetPeerId> NetPeerSessionLinks::RouteTo(uint8_t peer) const {
		if (const auto dial = probes.find(peer); dial != probes.end() && dial->second.answered && dial->second.transport && dial->second.connection != c_InvalidNetPeerId)
			return {dial->second.transport.get(), dial->second.connection};
		if (listener) for (const auto& [native, bound]: listenerBindings) if (bound == peer) return {listener.get(), native};
		if (primary) for (const auto& [native, bound]: primaryBindings) if (bound == peer) return {primary, native};
		return {};
	}

	bool NetPeerSessionLinks::SendTo(uint8_t peer, NetTransportLane lane, const std::vector<uint8_t>& bytes) {
		if (const auto dial = probes.find(peer); dial != probes.end() && dial->second.answered && dial->second.transport &&
		    dial->second.connection != c_InvalidNetPeerId && dial->second.transport->Send(dial->second.connection, lane, bytes)) return true;
		if (listener) for (const auto& [native, bound]: listenerBindings) if (bound == peer && listener->Send(native, lane, bytes)) return true;
		if (primary) for (const auto& [native, bound]: primaryBindings) if (bound == peer && primary->Send(native, lane, bytes)) return true;
		return false;
	}

	NetPeerId NetPeerSessionLinks::SessionRoute(uint8_t peer) const {
		for (const auto& [route, bound]: sessionRoutes) if (bound == peer) return route;
		return c_InvalidNetPeerId;
	}

	void NetPeerSessionLinks::ForwardAdmission(INetTransport& wire, const NetTransportEvent& event) {
		if (!sessionAttached || event.type != NetTransportEventType::PacketReceived) return;
		NetPeerId route = c_InvalidNetPeerId;
		if (&wire == primary) if (const auto known = primaryBindings.find(event.peerId); known != primaryBindings.end()) route = SessionRoute(known->second);
		for (const auto& [handle, native]: admissions) if (native.first == &wire && native.second == event.peerId) route = handle;
		if (route == c_InvalidNetPeerId) {
			if (admissions.size() >= 16 || nextAdmission == UINT32_MAX) return;
			route = nextAdmission++;
			admissions.emplace(route, std::pair{&wire, event.peerId});
			events.push_back({NetTransportEventType::PeerConnected, route, NetTransportLane::ControlReliable, {}, {}});
		}
		auto forwarded = event; forwarded.peerId = route;
		if (events.size() < 512) events.push_back(std::move(forwarded));
	}

	void NetPeerSessionLinks::BindAdmission(uint8_t peer, NetPeerId route) {
		const auto admitted = admissions.find(route);
		if (admitted == admissions.end()) return;
		const auto [wire, native] = admitted->second;
		if (wire == primary) primaryBindings[native] = peer;
		else if (wire == listener.get()) listenerBindings[native] = peer;
		sessionRoutes[route] = peer;
	}

	bool NetPeerSessionWire::Connect(const std::string&, uint16_t, std::string* error) {
		const NetLockstepPlaneGuard guard;
		const NetPeerId route = m_Links->SessionRoute(m_Links->hostPeerId);
		if (route == c_InvalidNetPeerId || m_Links->hostPeerId == m_Links->localPeerId) {
			if (error) *error = "the retained session has no authenticated host route";
			return false;
		}
		m_Links->sessionAttached = true;
		m_Links->events.push_back({NetTransportEventType::PeerConnected, route, NetTransportLane::ControlReliable, {}, {}});
		return true;
	}

	bool NetPeerSessionWire::StartHost(uint16_t, std::string*) {
		const NetLockstepPlaneGuard guard;
		m_Links->sessionAttached = true;
		return true;
	}

	bool NetPeerSessionWire::Send(NetPeerId peer, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error, bool* congested) {
		const NetLockstepPlaneGuard guard;
		if (congested) *congested = false;
		if (const auto bound = m_Links->sessionRoutes.find(peer); bound != m_Links->sessionRoutes.end()) {
			if (bytes.empty() || bytes.size() > NetHostMigrationCodec::c_ChunkBytes || m_Links->pendingBytes + bytes.size() > 8 * 1024 * 1024 || m_Links->pending[bound->second].size() >= 256) {
				if (congested) *congested = true;
				if (error) *error = "the retained session outbox is full";
				return false;
			}
			m_Links->pending[bound->second].push_back({m_Links->nextSequence++, lane, bytes});
			m_Links->pendingBytes += bytes.size();
			return true;
		}
		if (const auto admitted = m_Links->admissions.find(peer); admitted != m_Links->admissions.end())
			return admitted->second.first->Send(admitted->second.second, lane, bytes, error, congested);
		if (error) *error = "the session connection has no authenticated seat";
		return false;
	}

	void NetPeerSessionWire::Disconnect(NetPeerId peer, const std::string& reason) {
		const NetLockstepPlaneGuard guard;
		if (const auto admitted = m_Links->admissions.find(peer); admitted != m_Links->admissions.end()) {
			admitted->second.first->Disconnect(admitted->second.second, reason);
			m_Links->admissions.erase(admitted);
		}
	}

	void NetPeerSessionWire::Stop() {
		const NetLockstepPlaneGuard guard;
		m_Links->sessionAttached = false;
		m_Links->pending.clear(); m_Links->pendingBytes = 0; m_Links->events.clear();
	}

	std::vector<NetTransportEvent> NetPeerSessionWire::PollEvents() {
		const NetLockstepPlaneGuard guard;
		std::vector<NetTransportEvent> events;
		while (!m_Links->events.empty()) { events.push_back(std::move(m_Links->events.front())); m_Links->events.pop_front(); }
		return events;
	}

	uint32_t NetPeerSessionWire::GetPeerPingMs(NetPeerId route) const {
		const NetLockstepPlaneGuard guard;
		const auto peer = m_Links->sessionRoutes.find(route);
		if (peer == m_Links->sessionRoutes.end()) return 0;
		const auto [wire, native] = m_Links->RouteTo(peer->second);
		return wire ? wire->GetPeerPingMs(native) : 0;
	}
	bool NetPeerSessionWire::IsPeerPingMeasured(NetPeerId route) const {
		const NetLockstepPlaneGuard guard;
		const auto peer = m_Links->sessionRoutes.find(route);
		if (peer == m_Links->sessionRoutes.end()) return false;
		const auto [wire, native] = m_Links->RouteTo(peer->second);
		return wire && wire->IsPeerPingMeasured(native);
	}
	std::string NetPeerSessionWire::GetConnectedRoute(NetPeerId route) const {
		const NetLockstepPlaneGuard guard;
		const auto peer = m_Links->sessionRoutes.find(route);
		if (peer == m_Links->sessionRoutes.end()) return {};
		const auto [wire, native] = m_Links->RouteTo(peer->second);
		return wire ? wire->GetConnectedRoute(native) : std::string{};
	}

} // namespace RTE
