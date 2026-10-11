#include "NetPeerFrameHelpers.h"

#include "DiagnosticLine.h"
#include "NetAuthCrypto.h"
#include "NetLobbyProtocol.h"
#include "NetProtocol.h"

#include "lz4.h"

#include <cmath>
#include <chrono>
#include <limits>

namespace RTE {

	using namespace NetPeerFrameDetail;

	NetHostMigrationMessage NetLockstepCoordinator::PeerFrameMessage(NetHostMigrationMessageType type) const {
		NetHostMigrationMessage message;
		message.type = type;
		message.sessionId = m_Config.sessionId;
		message.roundId = m_Config.matchConfig.roundId;
		message.generation = 1;
		message.senderPeerId = message.successorPeerId = m_Config.localPeerId;
		message.configHash = m_RoundConfigHash;
		message.appliedFrame = m_LastCompletedSimulationTick.value_or(m_Config.startFrame == 0 ? 0 : m_Config.startFrame - 1);
		message.preparedFrame = m_Stats.nextFrame > 0 ? m_Stats.nextFrame - 1 : 0;
		message.connectedMask = m_FrameGroupMembers;
		return message;
	}

	void NetLockstepCoordinator::SendPeerFrameMessage(NetHostMigrationMessage message, NetTransportLane lane, uint8_t onlyPeer) {
		if (!UsesPeerFrameGroups() || PeerFrameBlackout(m_TimingNowMs)) return;
		std::vector<uint8_t> bytes;
		if (!NetHostMigrationCodec::Encode(message, m_Config.migrationKey, bytes)) return;
		if (m_Config.peerSessionLinks) {
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer)
				if (peer != m_Config.localPeerId && !m_RemovedPeers.contains(peer) && (onlyPeer == 0 || onlyPeer == peer)) (void)m_Config.peerSessionLinks->SendTo(peer, lane, bytes);
			return;
		}
		std::set<uint8_t> sent;
		for (auto& [peer, dial]: PeerProbes()) {
			if ((onlyPeer != 0 && onlyPeer != peer) || !dial.transport || !dial.answered || dial.connection == c_InvalidNetPeerId) continue;
			if (dial.transport->Send(dial.connection, lane, bytes)) sent.insert(peer);
		}
		if (PeerListener()) for (const auto& [connection, peer]: PeerListenerBindings()) {
			if ((onlyPeer != 0 && onlyPeer != peer) || sent.contains(peer)) continue;
			if (PeerListener()->Send(connection, lane, bytes)) sent.insert(peer);
		}
		if (m_Transport) for (const auto& [peer, connection]: m_RemoteTransports)
			if ((onlyPeer == 0 || onlyPeer == peer) && !sent.contains(peer)) (void)m_Transport->Send(connection, lane, bytes);
		if (m_Transport) for (const auto& [connection, peer]: PeerPrimaryBindings())
			if ((onlyPeer == 0 || onlyPeer == peer) && !sent.contains(peer)) (void)m_Transport->Send(connection, lane, bytes);
	}

	bool NetLockstepCoordinator::PeerGroupHasAuthority(uint32_t voters) const {
		uint32_t owners = 0;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer)
			if (!m_RemovedPeers.contains(peer) && !IsSeatReleased(peer)) owners |= SeatBit(peer);
		if (voters == 0 || (voters & ~owners) != 0) return false;
		if (m_PeerAdminFrameMembers != 0 && voters == m_PeerAdminFrameMembers) return true;
		const int votes = std::popcount(voters), seats = std::popcount(owners);
		if (2 * votes != seats) return 2 * votes > seats;
		if (!m_Config.frameTieReferee) return (voters & SeatBit(GetHostPeerId())) != 0;
		const auto decision = m_PeerFrameTieWinners.upper_bound(m_Stats.nextFrame);
		return decision != m_PeerFrameTieWinners.begin() && std::prev(decision)->second == voters;
	}

	void NetLockstepCoordinator::CheckPeerFrameTie(uint64_t frame, uint32_t members, uint64_t nowMs) {
		if (!m_Config.frameTieReferee || m_PeerFrameTieWinners.contains(frame)) return;
		NetFrameTieRequest request;
		request.generation = m_Config.migrationGeneration; request.roundId = m_Config.matchConfig.roundId;
		request.frame = frame; request.configHash = m_RoundConfigHash; request.host = GetHostPeerId();
		request.queryOnly = (members & SeatBit(m_Config.localPeerId)) == 0;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if (m_RemovedPeers.contains(peer) || IsSeatReleased(peer)) continue;
			request.owners.push_back(peer);
			if ((members & SeatBit(peer)) != 0) request.members.push_back(peer);
		}
		if (request.members.size() * 2 != request.owners.size()) return;
		if (!m_PeerFrameTie || *m_PeerFrameTie != request) { m_PeerFrameTie = request; m_PeerFrameTieSinceMs = nowMs; }
		const auto reply = m_Config.frameTieReferee(request);
		if (reply.state == NetFrameTieReply::State::Decided) {
			uint32_t winner = 0;
			for (uint16_t peer: reply.members) {
				if (peer == 0 || peer > m_Config.peerCount || std::find(request.owners.begin(), request.owners.end(), peer) == request.owners.end()) return;
				winner |= SeatBit(static_cast<uint8_t>(peer));
			}
			if (std::popcount(winner) * 2 == static_cast<int>(request.owners.size())) m_PeerFrameTieWinners.emplace(frame, winner);
		}
		// Silence from an internet referee grants neither side permission to display new history.
	}

	bool NetLockstepCoordinator::PeerFrameBlackout(uint64_t nowMs) {
		if (m_TestBlackoutDurationMs == 0 || m_TestBlackoutFrame == UINT64_MAX) {
			if (m_Config.peerSessionLinks) m_Config.peerSessionLinks->frameBlackout = false;
			return false;
		}
		if (!m_TestBlackoutAtMs && m_LastCompletedSimulationTick && *m_LastCompletedSimulationTick >= m_TestBlackoutFrame) {
			m_TestBlackoutAtMs = nowMs; m_TestBlackoutEnded = false;
			DiagnosticLine() << "[net-link-blackout] begin peer=" << static_cast<int>(m_Config.localPeerId) << " frame=" << *m_LastCompletedSimulationTick << " duration_ms=" << m_TestBlackoutDurationMs
			    << " now_ms=" << nowMs << " unix_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << std::endl;
		}
		if (!m_TestBlackoutAtMs) return false;
		const bool active = nowMs >= *m_TestBlackoutAtMs && nowMs - *m_TestBlackoutAtMs < m_TestBlackoutDurationMs;
		if (m_Config.peerSessionLinks) m_Config.peerSessionLinks->frameBlackout = active;
		if (!active && !m_TestBlackoutEnded) {
			m_TestBlackoutEnded = true;
			DiagnosticLine() << "[net-link-blackout] end peer=" << static_cast<int>(m_Config.localPeerId) << " elapsed_ms=" << nowMs - *m_TestBlackoutAtMs
			    << " now_ms=" << nowMs << " unix_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << std::endl;
		}
		return active;
	}

	bool NetLockstepCoordinator::HandlePeerSessionMessage(const NetHostMigrationMessage& message, uint64_t nowMs) {
		if (message.type != NetHostMigrationMessageType::PeerSession && message.type != NetHostMigrationMessageType::PeerSessionAck) return false;
		if (!UsesPeerFrameGroups() || !m_Config.peerSessionLinks || PeerFrameBlackout(nowMs)) return true;
		auto& links = *m_Config.peerSessionLinks;
		if (!links.sessionAttached || message.sessionId != links.sessionId || message.roundId != 1 || message.generation != 1 || message.configHash != NetHash32{} ||
		    message.senderPeerId == 0 || message.senderPeerId > m_Config.peerCount || message.senderPeerId == m_Config.localPeerId || message.successorPeerId != m_Config.localPeerId || m_RemovedPeers.contains(message.senderPeerId)) return true;
		if (message.type == NetHostMigrationMessageType::PeerSessionAck) {
			if (!message.bytes.empty()) return true;
			auto& pending = links.pending[message.senderPeerId];
			if (!pending.empty() && pending.front().sequence == message.frame) {
				links.pendingBytes -= pending.front().bytes.size(); pending.pop_front(); m_PeerSessionSendAtMs = nowMs;
				m_PeerLastHeardMs[message.senderPeerId] = nowMs; m_PeerLinkHeardMs[message.senderPeerId] = nowMs;
				if (message.senderPeerId == GetHostPeerId()) NoteAuthorityHeard(nowMs);
			}
			return true;
		}
		if (message.frame == 0 || message.bytes.empty() || (!NetProtocol::Decode(message.bytes).ok && !NetLobbyProtocol::Decode(message.bytes).ok)) return true;
		const NetPeerId route = links.SessionRoute(message.senderPeerId);
		if (route == c_InvalidNetPeerId) return true;
		if (message.frame > links.received[message.senderPeerId]) {
			if (links.events.size() >= 512) return true;
			links.received[message.senderPeerId] = message.frame;
			links.events.push_back({NetTransportEventType::PacketReceived, route, NetTransportLane::ControlReliable, message.bytes, {}});
		}
		m_PeerLastHeardMs[message.senderPeerId] = nowMs; m_PeerLinkHeardMs[message.senderPeerId] = nowMs;
		if (message.senderPeerId == GetHostPeerId()) NoteAuthorityHeard(nowMs);
		auto ack = message; ack.type = NetHostMigrationMessageType::PeerSessionAck; ack.senderPeerId = m_Config.localPeerId; ack.successorPeerId = message.senderPeerId;
		ack.bytes.clear(); ack.totalBytes = 0;
		SendPeerFrameMessage(std::move(ack), NetTransportLane::ControlReliable, message.senderPeerId);
		return true;
	}

	void NetLockstepCoordinator::TickPeerSessionWire(uint64_t nowMs) {
		if (!UsesPeerFrameGroups() || !m_Config.peerSessionLinks || !m_Config.peerSessionLinks->sessionAttached) return;
		auto& links = *m_Config.peerSessionLinks;
		if (m_SessionEventSink) while (!links.events.empty()) { const auto event = std::move(links.events.front()); links.events.pop_front(); m_SessionEventSink(event); }
		if (nowMs < m_PeerSessionSendAtMs) return;
		m_PeerSessionSendAtMs = nowMs + 50;
		for (const auto& [peer, pending]: links.pending) {
			if (pending.empty()) continue;
			auto message = PeerFrameMessage(NetHostMigrationMessageType::PeerSession);
			message.roundId = message.generation = 1; message.configHash = {}; message.successorPeerId = peer;
			message.frame = pending.front().sequence; message.bytes = pending.front().bytes; message.totalBytes = static_cast<uint32_t>(message.bytes.size());
			SendPeerFrameMessage(std::move(message), NetTransportLane::ControlReliable, peer);
		}
	}

	void NetLockstepCoordinator::TickPeerFrameGroups(uint64_t nowMs) {
		TickPeerSessionWire(nowMs);
		if (!UsesPeerFrameGroups() || !IsRunning()) return;
		if (m_Config.peerSessionLinks && nowMs >= m_PeerPathLogAtMs) {
			m_PeerPathLogAtMs = nowMs + 5000;
			for (const auto& [peer, path]: m_PeerReceiptDelaySamples) {
				const auto [wire, native] = m_Config.peerSessionLinks->RouteTo(peer);
				DiagnosticLine() << "[net-peer-path] local=" << static_cast<int>(m_Config.localPeerId) << " peer=" << static_cast<int>(peer)
				    << " now_ms=" << nowMs << " frame=" << m_LastCompletedSimulationTick.value_or(0)
				    << " transport_rtt_ms=" << (wire && wire->IsPeerPingMeasured(native) ? static_cast<int64_t>(wire->GetPeerPingMs(native)) : -1)
				    << " receipt_p95_ms=" << path.P95Ms() << " input_delay_ticks=" << InputDelayAt(m_Config.localPeerId, m_Stats.nextFrame) << std::endl;
			}
		}
		ResolvePeerBridgeInputConflicts(nowMs);
		if (m_FrameGroupChanges.empty()) { m_FrameGroupMembers = (1U << m_Config.peerCount) - 1; m_FrameGroupChanges[m_Config.startFrame] = m_FrameGroupMembers; }
		if (nowMs >= m_PeerHeartbeatAtMs) {
			m_PeerHeartbeatAtMs = nowMs + 250;
			auto heartbeat = PeerFrameMessage(NetHostMigrationMessageType::PeerHeartbeat);
			// Restored survivors can receive queued host traffic on a later local
			// clock. Advertise eligibility before anyone fixes an immutable vote;
			// this report grants no authority and does not replace voter validation.
			if (m_Config.localPeerId != GetHostPeerId() && m_LastCompletedSimulationTick &&
			    nowMs >= m_AuthorityLastHeardMs && nowMs - m_AuthorityLastHeardMs >= c_NetHostLossSilenceMs &&
			    m_Config.migrationGeneration != UINT64_MAX)
				heartbeat.completeFrom = m_Config.migrationGeneration + 1;
			PutSize(heartbeat.bytes, m_Stats.peers[m_Config.localPeerId].pingMs); PutSize(heartbeat.bytes, m_Stats.peers[m_Config.localPeerId].jitterMs);
			heartbeat.totalBytes = static_cast<uint32_t>(heartbeat.bytes.size());
			SendPeerFrameMessage(std::move(heartbeat));
		}
		if (nowMs >= m_PeerVoteRetryAtMs) {
			m_PeerVoteRetryAtMs = nowMs + 100;
			for (auto& [key, votes]: m_PeerBridgeVotes) {
				if (key.first < m_Stats.nextFrame || m_PeerBridgeCertificates.contains(key)) continue;
				if (!votes.contains(m_Config.localPeerId)) for (const auto& [peer, vote]: votes) {
					if (!ValidatePeerBridge(vote)) continue;
					auto own = vote;
					own.senderPeerId = m_Config.localPeerId; own.voterMask = SeatBit(m_Config.localPeerId);
					own.appliedFrame = m_LastCompletedSimulationTick.value_or(0);
					if (own.preparedFrame == 3) m_PeerAdminOwnVote = own;
					votes.emplace(m_Config.localPeerId, std::move(own));
					if (vote.preparedFrame == 0) for (uint8_t held: vote.members) m_PeerRejectedInputs[vote.frame] |= SeatBit(held);
					break;
				}
				if (const auto own = votes.find(m_Config.localPeerId); own != votes.end()) SendPeerFrameMessage(own->second);
				TryCommitPeerBridge(key.first, nowMs, key.second);
			}
			for (const auto& [key, certificate]: m_PeerBridgeCertificates) {
				if (key.first + NetLockstepCodec::c_MaxFutureFrameSkew < m_Stats.nextFrame) continue;
				SendPeerFrameMessage(certificate);
			}
		}
		TickPeerInputDelay(nowMs);
		TickPeerAdmin(nowMs);

		if (m_PeerTailThrough && m_Stats.nextFrame <= *m_PeerTailThrough) RequestPeerCommittedTail(nowMs);
		const uint64_t kept = m_Stats.nextFrame > NetHostMigrationCodec::c_PeerHistoryFrames ? m_Stats.nextFrame - NetHostMigrationCodec::c_PeerHistoryFrames : 0;
		const uint64_t inputWindow = ConfiguredWindowTicks();
		const uint64_t inputKept = m_Stats.nextFrame > inputWindow ? m_Stats.nextFrame - inputWindow : 0;
		m_PeerSourceInputs.erase(m_PeerSourceInputs.begin(), m_PeerSourceInputs.lower_bound(inputKept));
		m_PeerRejectedInputs.erase(m_PeerRejectedInputs.begin(), m_PeerRejectedInputs.lower_bound(kept));
		m_PeerBridgeVotes.erase(m_PeerBridgeVotes.begin(), m_PeerBridgeVotes.lower_bound({kept, 0}));
		m_PeerBridgeCertificates.erase(m_PeerBridgeCertificates.begin(), m_PeerBridgeCertificates.lower_bound({kept, 0}));
		m_PeerInputSentAtMs.erase(m_PeerInputSentAtMs.begin(), m_PeerInputSentAtMs.lower_bound(kept));
		m_PeerPreparedPrefixes.erase(m_PeerPreparedPrefixes.begin(), m_PeerPreparedPrefixes.lower_bound(kept));
		m_PeerFrameWitnesses.erase(m_PeerFrameWitnesses.begin(), m_PeerFrameWitnesses.lower_bound(kept));
		for (auto* history: {&m_PeerFrameTieWinners, &m_FrameGroupChanges}) {
			const auto floor = history->lower_bound(kept);
			if (floor != history->begin()) history->erase(history->begin(), std::prev(floor));
		}
		std::erase_if(m_PeerFrameIncoming, [&](const auto& entry) { return nowMs >= entry.second.lastReceivedMs && nowMs - entry.second.lastReceivedMs > c_NetHostLossSilenceMs; });
	}

	bool NetLockstepCoordinator::ProposePeerInputDelay(uint8_t peer, uint16_t delay, uint64_t frame) {
		if (peer != m_Config.localPeerId || delay == 0 || delay > NetLockstepCodec::c_MaxInputDelayFrames || frame <= m_Stats.nextFrame || !PeerGroupHasAuthority(m_FrameGroupMembers)) return false;
		for (const auto& [key, votes]: m_PeerBridgeVotes) if (key.second == 8 + peer && key.first >= m_Stats.nextFrame) return false;
		auto proposal = PeerFrameMessage(NetHostMigrationMessageType::PeerBridge);
		proposal.preparedFrame = 2; proposal.frame = frame; proposal.boundary = m_LastCompletedSimulationTick.value_or(m_Stats.nextFrame - 1);
		proposal.members = {peer}; proposal.completeFrom = delay;
		const NetHash32 prefix = PeerAppliedFramePrefix(proposal.boundary);
		proposal.bytes.assign(prefix.begin(), prefix.end()); proposal.totalBytes = static_cast<uint32_t>(proposal.bytes.size());
		proposal.voterMask = SeatBit(peer);
		if (!ValidatePeerBridge(proposal)) return false;
		m_PeerBridgeVotes[{frame, DecisionKind(proposal)}].emplace(peer, proposal);
		++m_Stats.delayChangesProposed;
		SendPeerFrameMessage(std::move(proposal));
		TryCommitPeerBridge(frame, m_TimingNowMs, static_cast<uint8_t>(8 + peer));
		return true;
	}

	void NetLockstepCoordinator::TickPeerInputDelay(uint64_t nowMs) {
		if (!m_Config.adaptiveInputDelay || WaitsForPlacement() || !std::isfinite(m_Config.simTickMs) || m_Config.simTickMs <= 0) return;
		if ((m_FrameGroupMembers & SeatBit(m_Config.localPeerId)) == 0) return;
		std::vector<std::pair<uint32_t, uint32_t>> paths;
		for (const auto& [peer, sample]: m_PeerReceiptDelaySamples) if ((m_FrameGroupMembers & SeatBit(peer)) != 0) paths.emplace_back(sample.P95Ms(), sample.JitterMs());
		const size_t needed = std::popcount(m_FrameGroupMembers) - 1;
		if (needed == 0 || paths.size() < needed) return;
		std::sort(paths.begin(), paths.end());
		// Every active receiver needs the sender's buffered input. Sizing to
		// only a quorum slowly drains the runway of a healthy, longer path.
		const auto [rtt, jitter] = paths.back();
		const uint32_t margin = std::max(c_NetInputJitterReserveMs, jitter) + static_cast<uint32_t>(std::ceil((m_Config.slowPlayerBoundTicks + 1) * m_Config.simTickMs));
		auto& local = m_Stats.peers[m_Config.localPeerId];
		local.pingMs = rtt; local.pingMeasured = true;
		const uint16_t current = InputDelayAt(m_Config.localPeerId, m_Stats.nextFrame);
		const uint16_t required = static_cast<uint16_t>(std::min<uint32_t>(NetLockstepCodec::c_MaxInputDelayFrames,
		    std::max<uint32_t>(m_Config.matchConfig.inputDelayFrames, static_cast<uint32_t>(std::ceil((rtt + margin) / m_Config.simTickMs)))));
		local.jitterMs = std::max<uint32_t>(margin, static_cast<uint32_t>(std::ceil(required * m_Config.simTickMs)) - std::min<uint32_t>(rtt, static_cast<uint32_t>(std::ceil(required * m_Config.simTickMs))));
		const auto last = m_LastDelayResizeMs.find(m_Config.localPeerId);
		if (last != m_LastDelayResizeMs.end() && nowMs < last->second + c_DelayResizeIntervalMs) return;
		if (required > current) {
			m_PeerDelayBelowSinceMs.erase(m_Config.localPeerId);
			(void)ProposePeerInputDelay(m_Config.localPeerId, required, FutureTimingFrame());
		} else if (required + c_DelayShrinkStep < current) {
			const auto [since, inserted] = m_PeerDelayBelowSinceMs.try_emplace(m_Config.localPeerId, nowMs);
			if (nowMs >= since->second + c_DelayShrinkIntervalMs && ProposePeerInputDelay(m_Config.localPeerId, current - c_DelayShrinkStep, FutureTimingFrame())) since->second = nowMs;
		} else m_PeerDelayBelowSinceMs.erase(m_Config.localPeerId);
	}

} // namespace RTE
