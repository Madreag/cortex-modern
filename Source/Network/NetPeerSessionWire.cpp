#include "NetPeerSessionWire.h"

#include "NetLockstep.h"
#include "NetLobbyProtocol.h"
#include "NetProtocol.h"

#include <set>

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

	uint64_t NetPeerSessionLinks::ClockMs() const { return sessionClock ? sessionClock() : NetLockstepNowMs(); }

	uint8_t NetPeerSessionLinks::PeerOn(const INetTransport& wire, NetPeerId native) const {
		if (&wire == primary) if (const auto known = primaryBindings.find(native); known != primaryBindings.end()) return known->second;
		if (&wire == listener.get()) if (const auto known = listenerBindings.find(native); known != listenerBindings.end()) return known->second;
		for (const auto& [peer, dial]: probes)
			if (dial.transport.get() == &wire && dial.answered && dial.connection == native) return peer;
		return 0;
	}

	void NetPeerSessionLinks::ForwardAdmission(INetTransport& wire, const NetTransportEvent& event) {
		if (!sessionAttached) return;
		const uint8_t peer = PeerOn(wire, event.peerId);
		NetPeerId route = SessionRoute(peer);
		for (const auto& [handle, native]: admissions) if (native.first == &wire && native.second == event.peerId) route = handle;
		if (event.type == NetTransportEventType::PeerDisconnected || event.type == NetTransportEventType::ConnectionFailed) {
			if (&wire == primary) primaryBindings.erase(event.peerId);
			if (&wire == listener.get()) listenerBindings.erase(event.peerId);
			for (auto& [owner, dial]: probes) if (dial.transport.get() == &wire && dial.connection == event.peerId) {
				dial.connection = c_InvalidNetPeerId; dial.answered = false;
			}
			std::erase_if(admissions, [&](const auto& admitted) { return admitted.second == std::pair{&wire, event.peerId}; });
			if (peer != 0) {
				if (RouteTo(peer).first) return;
				for (const auto& queued: pending[peer]) pendingBytes -= queued.bytes.size();
				pending.erase(peer); received.erase(peer); closing.erase(peer);
			}
		}
		if (route == c_InvalidNetPeerId) {
			if (event.type != NetTransportEventType::PacketReceived) return;
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
		std::erase_if(sessionRoutes, [&](const auto& binding) { return binding.second == peer; });
		sessionRoutes[route] = peer;
		admissions.erase(admitted);
		for (const auto& item: pending[peer]) pendingBytes -= item.bytes.size();
		pending.erase(peer); closing.erase(peer);
		received.erase(peer);
	}

	bool NetPeerSessionLinks::HandleSessionMessage(const NetHostMigrationMessage& message) {
		if (message.type != NetHostMigrationMessageType::PeerSession && message.type != NetHostMigrationMessageType::PeerSessionAck) return false;
		if (!sessionAttached || frameBlackout || message.sessionId != sessionId || message.roundId != 1 || message.generation != 1 ||
		    message.configHash != NetHash32{} || message.senderPeerId == 0 || message.senderPeerId == localPeerId ||
		    message.successorPeerId != localPeerId || SessionRoute(message.senderPeerId) == c_InvalidNetPeerId) return true;
		if (message.type == NetHostMigrationMessageType::PeerSessionAck) {
			if (!message.bytes.empty()) return true;
			auto& queued = pending[message.senderPeerId];
			if (!queued.empty() && queued.front().sequence == message.frame) {
				pendingBytes -= queued.front().bytes.size(); queued.pop_front();
			}
			return true;
		}
		if (message.frame == 0 || message.bytes.empty() ||
		    (!NetProtocol::Decode(message.bytes).ok && !NetLobbyProtocol::Decode(message.bytes).ok &&
		     !NetLockstepCodec::Decode(message.bytes, ControllerFrame::c_Version).ok)) return true;
		if (message.frame > received[message.senderPeerId]) {
			if (events.size() >= 512) return true;
			received[message.senderPeerId] = message.frame;
			events.push_back({NetTransportEventType::PacketReceived, SessionRoute(message.senderPeerId), NetTransportLane::ControlReliable, message.bytes, {}});
		}
		auto ack = message;
		ack.type = NetHostMigrationMessageType::PeerSessionAck; ack.senderPeerId = localPeerId; ack.successorPeerId = message.senderPeerId;
		ack.bytes.clear(); ack.totalBytes = 0;
		std::vector<uint8_t> bytes;
		if (NetHostMigrationCodec::Encode(ack, key, bytes)) (void)SendTo(message.senderPeerId, NetTransportLane::ControlReliable, bytes);
		return true;
	}

	void NetPeerSessionLinks::FlushSession(uint64_t nowMs) {
		if (!sessionAttached || frameBlackout) return;
		for (auto& [peer, queued]: pending) {
			if (queued.empty() || nowMs < queued.front().sendAfterMs) continue;
			auto& item = queued.front();
			NetHostMigrationMessage message;
			message.type = NetHostMigrationMessageType::PeerSession;
			message.sessionId = sessionId; message.roundId = message.generation = 1;
			message.senderPeerId = localPeerId; message.successorPeerId = peer;
			message.frame = item.sequence; message.bytes = item.bytes; message.totalBytes = static_cast<uint32_t>(item.bytes.size());
			std::vector<uint8_t> bytes;
			if (NetHostMigrationCodec::Encode(message, key, bytes)) (void)SendTo(peer, NetTransportLane::ControlReliable, bytes);
			item.sendAfterMs = nowMs + 50;
		}
		for (auto end = closing.begin(); end != closing.end();) {
			const uint8_t peer = end->first;
			if (!pending[peer].empty() && nowMs < end->second.deadlineMs) { ++end; continue; }
			if (primary) for (const auto& [native, owner]: primaryBindings) if (owner == peer) primary->Disconnect(native, end->second.reason);
			if (listener) for (const auto& [native, owner]: listenerBindings) if (owner == peer) listener->Disconnect(native, end->second.reason);
			if (const auto dial = probes.find(peer); dial != probes.end() && dial->second.transport && dial->second.connection != c_InvalidNetPeerId)
				dial->second.transport->Disconnect(dial->second.connection, end->second.reason);
			for (const auto& item: pending[peer]) pendingBytes -= item.bytes.size();
			pending.erase(peer); end = closing.erase(end);
		}
	}

	void NetPeerSessionLinks::PollSession(uint64_t nowMs) {
		if (!sessionAttached || frameReader) return;
		FlushSession(nowMs);
		std::set<INetTransport*> wires;
		if (primary) wires.insert(primary);
		if (listener) wires.insert(listener.get());
		for (const auto& [peer, dial]: probes) if (dial.transport) wires.insert(dial.transport.get());
		for (INetTransport* wire: wires) for (const auto& event: wire->PollEvents()) {
			if (event.type == NetTransportEventType::PacketReceived && NetHostMigrationCodec::LooksLikePacket(event.bytes)) {
				NetHostMigrationMessage message;
				if (NetHostMigrationCodec::Decode(event.bytes, key, message) && PeerOn(*wire, event.peerId) == message.senderPeerId)
					(void)HandleSessionMessage(message);
				continue;
			}
			if (event.type != NetTransportEventType::PacketReceived || NetProtocol::Decode(event.bytes).ok || NetLobbyProtocol::Decode(event.bytes).ok)
				ForwardAdmission(*wire, event);
		}
		FlushSession(nowMs);
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
		if (const auto bound = m_Links->sessionRoutes.find(peer); bound != m_Links->sessionRoutes.end()) {
			if (reason == c_NetRemovedLinkText || reason == c_NetBannedLinkText) {
				auto& queued = m_Links->pending[bound->second];
				std::erase_if(queued, [&](const auto& item) {
					const auto decoded = NetProtocol::Decode(item.bytes);
					if (decoded.ok && (std::holds_alternative<NetParticipantRemoval>(decoded.message.payload) || std::holds_alternative<NetDisconnect>(decoded.message.payload))) return false;
					m_Links->pendingBytes -= item.bytes.size(); return true;
				});
				if (!queued.empty()) queued.front().sendAfterMs = 0;
			}
			m_Links->closing[bound->second] = {reason, m_Links->ClockMs() + 2000};
			return;
		}
		if (const auto admitted = m_Links->admissions.find(peer); admitted != m_Links->admissions.end()) {
			admitted->second.first->Disconnect(admitted->second.second, reason);
			m_Links->admissions.erase(admitted);
		}
	}

	void NetPeerSessionWire::Stop() {
		const NetLockstepPlaneGuard guard;
		m_Links->sessionAttached = false;
		m_Links->pending.clear(); m_Links->pendingBytes = 0; m_Links->events.clear();
		m_Links->closing.clear();
	}

	std::vector<NetTransportEvent> NetPeerSessionWire::PollEvents() {
		const NetLockstepPlaneGuard guard;
		m_Links->PollSession(m_Links->ClockMs());
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
