#include "NetPeerSessionWire.h"

#include "NetLockstep.h"
#include "DiagnosticLine.h"
#include "NetLobbyProtocol.h"
#include "NetProtocol.h"

#include <algorithm>
#include <set>

namespace RTE {

	std::vector<std::pair<INetTransport*, NetPeerId>> NetPeerSessionLinks::RoutesTo(uint8_t peer) const {
		if (const auto current = admittedRoutes.find(SessionRoute(peer)); current != admittedRoutes.end()) return {current->second};
		std::vector<std::pair<INetTransport*, NetPeerId>> routes;
		const auto add = [&](INetTransport* wire, NetPeerId native) {
			if (std::find(routes.begin(), routes.end(), std::pair{wire, native}) == routes.end()) routes.emplace_back(wire, native);
		};
		// The admitted host route stays in use; other pairs choose the higher seat's outgoing link.
		if (primary) for (const auto& [native, bound]: primaryBindings) if (bound == peer) add(primary, native);
		const auto outgoing = [&] {
			if (const auto dial = probes.find(peer); dial != probes.end() && dial->second.answered && dial->second.transport && dial->second.connection != c_InvalidNetPeerId)
				add(dial->second.transport.get(), dial->second.connection);
		};
		const auto incoming = [&] {
			if (listener) for (const auto& [native, bound]: listenerBindings) if (bound == peer) add(listener.get(), native);
		};
		if (localPeerId > peer) { outgoing(); incoming(); }
		else { incoming(); outgoing(); }
		return routes;
	}

	std::pair<INetTransport*, NetPeerId> NetPeerSessionLinks::RouteTo(uint8_t peer) const {
		const auto routes = RoutesTo(peer);
		return routes.empty() ? std::pair<INetTransport*, NetPeerId>{} : routes.front();
	}

	bool NetPeerSessionLinks::SendTo(uint8_t peer, NetTransportLane lane, const std::vector<uint8_t>& bytes, bool framePacket) {
		for (const auto& [wire, native]: RoutesTo(peer)) if (wire->Send(native, lane, bytes)) {
			if (framePacket) { auto& flow = frameFlow[wire][native]; ++flow.sent; flow.sentAtMs = ClockMs(); }
			return true;
		}
		return false;
	}

	void NetPeerSessionLinks::NoteFrameRead(const INetTransport& wire, NetPeerId native) {
		auto& flow = frameFlow[&wire][native];
		++flow.received; flow.heardAtMs = ClockMs();
	}

	void NetPeerSessionLinks::RetireSpareRoutes(uint8_t peer) {
		if (!sessionAttached || frameBlackout) return;
		const auto retained = RouteTo(peer);
		const auto wire = frameFlow.find(retained.first);
		if (!retained.first || wire == frameFlow.end()) return;
		const auto flow = wire->second.find(retained.second);
		const uint64_t nowMs = ClockMs();
		if (flow == wire->second.end() || flow->second.sent == 0 || flow->second.received == 0 ||
		    nowMs < flow->second.heardAtMs || nowMs - flow->second.heardAtMs >= c_NetLinkTimeoutMs / 2 ||
		    nowMs < flow->second.sentAtMs || nowMs - flow->second.sentAtMs >= c_NetLinkTimeoutMs / 2) return;
		for (const auto& route: RoutesTo(peer)) {
			if (route == retained || route.first == primary) continue;
			if (const auto prior = frameFlow.find(route.first); prior != frameFlow.end()) {
				if (const auto recent = prior->second.find(route.second); recent != prior->second.end() &&
				    ((recent->second.sent != 0 && recent->second.sentAtMs >= flow->second.sentAtMs) ||
				     (recent->second.received != 0 && recent->second.heardAtMs >= flow->second.heardAtMs))) continue;
			}
			bool inAdmission = false;
			for (const auto& [handle, admitted]: admissions) if (admitted == route) inAdmission = true;
			for (const auto& [handle, admitted]: admittedRoutes) if (admitted == route) inAdmission = true;
			if (inAdmission) continue;
			if (route.first == listener.get()) listenerBindings.erase(route.second);
			for (auto& [owner, dial]: probes) if (dial.transport.get() == route.first && dial.connection == route.second) {
				dial.connection = c_InvalidNetPeerId; dial.answered = false;
			}
			DiagnosticLine() << "[net-peer-link] retiring local=" << static_cast<int>(localPeerId) << " peer=" << static_cast<int>(peer)
			    << " retained_sent=" << flow->second.sent << " retained_received=" << flow->second.received
			    << " spare_sent=" << frameFlow[route.first][route.second].sent << " spare_received=" << frameFlow[route.first][route.second].received << " reader_detached=1" << std::endl;
			route.first->RetirePeerLink(route.second);
			frameFlow[route.first].erase(route.second);
		}
	}

	NetPeerId NetPeerSessionLinks::SessionRoute(uint8_t peer) const {
		for (const auto& [route, bound]: sessionRoutes) if (bound == peer) return route;
		return c_InvalidNetPeerId;
	}

	uint64_t NetPeerSessionLinks::ClockMs() const { return sessionClock ? sessionClock() : NetLockstepNowMs(); }

	uint8_t NetPeerSessionLinks::PeerOn(const INetTransport& wire, NetPeerId native) const {
		const auto current = [&](uint8_t peer) {
			const auto route = admittedRoutes.find(SessionRoute(peer));
			return route == admittedRoutes.end() || (route->second.first == &wire && route->second.second == native) ? peer : uint8_t{0};
		};
		if (&wire == primary) if (const auto known = primaryBindings.find(native); known != primaryBindings.end()) return current(known->second);
		if (&wire == listener.get()) if (const auto known = listenerBindings.find(native); known != listenerBindings.end()) return current(known->second);
		for (const auto& [peer, dial]: probes)
			if (dial.transport.get() == &wire && dial.answered && dial.connection == native) return current(peer);
		return 0;
	}

	bool NetPeerSessionLinks::IsNativeRoute(NetPeerId route, const INetTransport* wire, NetPeerId native) const {
		const auto bound = admittedRoutes.find(route);
		return bound != admittedRoutes.end() && bound->second.first == wire && bound->second.second == native;
	}

	void NetPeerSessionLinks::ForwardAdmission(INetTransport& wire, const NetTransportEvent& event) {
		if (!sessionAttached) return;
		const uint8_t peer = PeerOn(wire, event.peerId);
		if (peer != 0 && event.type == NetTransportEventType::PacketReceived) progressAt[peer] = ClockMs();
		NetPeerId route = SessionRoute(peer);
		for (const auto& [handle, native]: admittedRoutes) if (native.first == &wire && native.second == event.peerId) route = handle;
		for (const auto& [handle, native]: admissions) if (native.first == &wire && native.second == event.peerId) route = handle;
		if (event.type == NetTransportEventType::PeerDisconnected || event.type == NetTransportEventType::ConnectionFailed) {
			frameFlow[&wire].erase(event.peerId);
			if (&wire == primary) primaryBindings.erase(event.peerId);
			if (&wire == listener.get()) listenerBindings.erase(event.peerId);
			for (auto& [owner, dial]: probes) if (dial.transport.get() == &wire && dial.connection == event.peerId) {
				dial.connection = c_InvalidNetPeerId; dial.answered = false;
			}
			std::erase_if(admissions, [&](const auto& admitted) { return admitted.second == std::pair{&wire, event.peerId}; });
			std::erase_if(admittedRoutes, [&](const auto& admitted) { return admitted.second == std::pair{&wire, event.peerId}; });
			if (peer != 0) {
				if (RouteTo(peer).first) return;
				for (const auto& queued: pending[peer]) pendingBytes -= queued.bytes.size();
				pending.erase(peer); received.erase(peer); closing.erase(peer);
				queuedThrough.erase(peer); acknowledgedThrough.erase(peer); progressAt.erase(peer);
				for (const auto& [sequence, queued]: incoming[peer]) incomingBytes -= queued.bytes.size();
				incoming.erase(peer);
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
		admittedRoutes[route] = admitted->second;
		admissions.erase(admitted);
		for (const auto& item: pending[peer]) pendingBytes -= item.bytes.size();
		pending.erase(peer); closing.erase(peer);
		received.erase(peer);
		queuedThrough.erase(peer); acknowledgedThrough.erase(peer); progressAt.erase(peer);
		for (const auto& [sequence, queued]: incoming[peer]) incomingBytes -= queued.bytes.size();
		incoming.erase(peer);
	}

	void NetPeerSessionLinks::SetSessionKey(const std::array<uint8_t, 32>& next) {
		if (next == key) return;
		if (std::any_of(key.begin(), key.end(), [](uint8_t byte) { return byte != 0; })) earlierKeys.push_back(key);
		if (earlierKeys.size() > 8) earlierKeys.pop_front();
		key = next;
	}

	bool NetPeerSessionLinks::ReadSessionPacket(const std::vector<uint8_t>& bytes, NetHostMigrationMessage& message, std::array<uint8_t, 32>* readKey) const {
		const auto read = [&](const auto& candidate) {
			if (!NetHostMigrationCodec::Decode(bytes, candidate, message) ||
			    (message.type != NetHostMigrationMessageType::PeerSession && message.type != NetHostMigrationMessageType::PeerSessionAck)) return false;
			if (readKey) *readKey = candidate;
			return true;
		};
		if (read(key)) return true;
		for (const auto& previous: earlierKeys) if (read(previous)) return true;
		return false;
	}

	bool NetPeerSessionLinks::HandleSessionMessage(const NetHostMigrationMessage& message, const std::array<uint8_t, 32>* readKey) {
		if (message.type != NetHostMigrationMessageType::PeerSession && message.type != NetHostMigrationMessageType::PeerSessionAck) return false;
		if (!sessionAttached || frameBlackout || message.sessionId != sessionId || message.roundId != 1 || message.generation != 1 ||
		    message.configHash != NetHash32{} || message.senderPeerId == 0 || message.senderPeerId == localPeerId ||
		    message.successorPeerId != localPeerId || SessionRoute(message.senderPeerId) == c_InvalidNetPeerId) return true;
		progressAt[message.senderPeerId] = ClockMs();
		if (message.type == NetHostMigrationMessageType::PeerSessionAck) {
			if (!message.bytes.empty()) return true;
			auto& queued = pending[message.senderPeerId];
			for (auto& item: queued) if (item.sequence == message.frame) { item.acknowledged = true; break; }
			while (!queued.empty() && queued.front().acknowledged) {
				acknowledgedThrough[message.senderPeerId] = queued.front().sequence;
				pendingBytes -= queued.front().bytes.size(); queued.pop_front();
			}
			return true;
		}
		if (message.frame == 0 || message.completeFrom >= message.frame || message.bytes.empty() ||
		    (!NetProtocol::Decode(message.bytes).ok && !NetLobbyProtocol::Decode(message.bytes).ok &&
		     !NetLockstepCodec::Decode(message.bytes, ControllerFrame::c_Version).ok)) return true;
		const auto acknowledge = [&](uint64_t sequence, const auto& replyKey) {
			auto ack = message;
			ack.type = NetHostMigrationMessageType::PeerSessionAck; ack.senderPeerId = localPeerId; ack.successorPeerId = message.senderPeerId;
			ack.frame = sequence; ack.bytes.clear(); ack.totalBytes = 0;
			std::vector<uint8_t> bytes;
			if (NetHostMigrationCodec::Encode(ack, replyKey, bytes)) (void)SendTo(message.senderPeerId, NetTransportLane::ControlReliable, bytes);
		};
		if (message.frame <= received[message.senderPeerId]) { acknowledge(message.frame, readKey ? *readKey : key); return true; }
		auto& waiting = incoming[message.senderPeerId];
		if (!waiting.contains(message.frame)) {
			if (waiting.size() >= 32 || incomingBytes + message.bytes.size() > 1024 * 1024) return true;
			waiting.emplace(message.frame, Incoming{message.completeFrom, message.bytes, readKey ? *readKey : key});
			incomingBytes += message.bytes.size();
		}
		// A changed native path may deliver the later message first; keep its predecessor before applying it.
		while (!waiting.empty() && events.size() < 512) {
			const auto next = std::find_if(waiting.begin(), waiting.end(), [&](const auto& item) { return item.second.previous <= received[message.senderPeerId]; });
			if (next == waiting.end()) break;
			received[message.senderPeerId] = next->first;
			incomingBytes -= next->second.bytes.size();
			events.push_back({NetTransportEventType::PacketReceived, SessionRoute(message.senderPeerId), NetTransportLane::ControlReliable, std::move(next->second.bytes), {}});
			acknowledge(next->first, next->second.readKey);
			waiting.erase(next);
			for (auto old = waiting.begin(); old != waiting.end() && old->first <= received[message.senderPeerId];) {
				incomingBytes -= old->second.bytes.size(); old = waiting.erase(old);
			}
		}
		return true;
	}

	void NetPeerSessionLinks::FlushSession(uint64_t nowMs) {
		if (!sessionAttached || frameBlackout) return;
		for (auto& [peer, queued]: pending) {
			size_t messages = 0, bytesDue = 0;
			const auto [wire, route] = RouteTo(peer);
			const uint64_t repeatMs = std::clamp<uint64_t>(wire ? 2ULL * wire->GetPeerPingMs(route) + 50 : 100, 100, 2000);
			for (auto& item: queued) {
				if (++messages > 16 || (bytesDue != 0 && bytesDue + item.bytes.size() > 128 * 1024)) break;
				bytesDue += item.bytes.size();
				if (item.acknowledged || nowMs < item.sendAfterMs) continue;
				// A reader that is not polling gets a bounded reliable window until it speaks again.
				if (item.repeats >= 3 && progressAt[peer] <= item.lastSentMs) continue;
				if (progressAt[peer] > item.lastSentMs) item.repeats = 0;
				NetHostMigrationMessage message;
				message.type = NetHostMigrationMessageType::PeerSession;
				message.sessionId = sessionId; message.roundId = message.generation = 1;
				message.senderPeerId = localPeerId; message.successorPeerId = peer;
				message.frame = item.sequence; message.bytes = item.bytes; message.totalBytes = static_cast<uint32_t>(item.bytes.size());
				message.completeFrom = item.previous;
				std::vector<uint8_t> bytes;
				if (NetHostMigrationCodec::Encode(message, item.sendKey, bytes) && SendTo(peer, NetTransportLane::ControlReliable, bytes)) {
					item.lastSentMs = nowMs; ++item.repeats;
				}
				item.sendAfterMs = nowMs + repeatMs;
			}
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
				std::array<uint8_t, 32> readKey;
				if (ReadSessionPacket(event.bytes, message, &readKey) && PeerOn(*wire, event.peerId) == message.senderPeerId)
					(void)HandleSessionMessage(message, &readKey);
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
			const uint64_t sequence = m_Links->nextSequence++;
			m_Links->pending[bound->second].push_back({sequence, lane, bytes, 0, m_Links->key, m_Links->queuedThrough[bound->second]});
			m_Links->queuedThrough[bound->second] = sequence;
			m_Links->pendingBytes += bytes.size();
			return true;
		}
		if (const auto admitted = m_Links->admissions.find(peer); admitted != m_Links->admissions.end())
			return admitted->second.first->Send(admitted->second.second, lane, bytes, error, congested);
		if (const auto admitted = m_Links->admittedRoutes.find(peer); admitted != m_Links->admittedRoutes.end())
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
				uint64_t previous = m_Links->acknowledgedThrough[bound->second];
				for (auto& item: queued) { item.previous = previous; previous = item.sequence; item.sendAfterMs = 0; }
				if (!queued.empty()) queued.front().sendAfterMs = 0;
			}
			m_Links->closing[bound->second] = {reason, m_Links->ClockMs() + 2000};
			return;
		}
		if (const auto admitted = m_Links->admissions.find(peer); admitted != m_Links->admissions.end()) {
			admitted->second.first->Disconnect(admitted->second.second, reason);
			m_Links->admissions.erase(admitted);
		}
		else if (const auto admitted = m_Links->admittedRoutes.find(peer); admitted != m_Links->admittedRoutes.end()) {
			admitted->second.first->Disconnect(admitted->second.second, reason);
			m_Links->admittedRoutes.erase(admitted);
		}
	}

	void NetPeerSessionWire::Stop() {
		const NetLockstepPlaneGuard guard;
		m_Links->sessionAttached = false;
		m_Links->pending.clear(); m_Links->pendingBytes = 0; m_Links->events.clear();
		m_Links->closing.clear();
		m_Links->queuedThrough.clear(); m_Links->acknowledgedThrough.clear(); m_Links->progressAt.clear();
		m_Links->incoming.clear(); m_Links->incomingBytes = 0;
		m_Links->earlierKeys.clear();
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
