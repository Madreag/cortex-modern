#include "NetPeerFrameHelpers.h"

#include "DiagnosticLine.h"
#include "NetAuthCrypto.h"
#include "NetLobbyProtocol.h"
#include "NetProtocol.h"

#include "lz4.h"

#include <cmath>
#include <chrono>
#include <cstdlib>
#include <limits>

namespace RTE {

	using namespace NetPeerFrameDetail;

	std::pair<INetTransport*, NetPeerId> NetLockstepCoordinator::PeerFrameRoute(uint8_t peer) const {
		if (m_Config.peerSessionLinks) return m_Config.peerSessionLinks->RouteTo(peer);
		if (const auto dial = m_MigrationProbes.find(peer); dial != m_MigrationProbes.end() && dial->second.transport &&
		    dial->second.answered && dial->second.connection != c_InvalidNetPeerId) return {dial->second.transport.get(), dial->second.connection};
		if (m_MigrationListener) for (const auto& [connection, owner]: m_PeerListenerBindings)
			if (owner == peer) return {m_MigrationListener.get(), connection};
		if (m_Transport) {
			if (const auto connection = m_RemoteTransports.find(peer); connection != m_RemoteTransports.end()) return {m_Transport, connection->second};
			for (const auto& [connection, owner]: m_PeerPrimaryBindings) if (owner == peer) return {m_Transport, connection};
		}
		return {nullptr, c_InvalidNetPeerId};
	}

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

	bool NetLockstepCoordinator::SendPeerFrameMessage(NetHostMigrationMessage message, NetTransportLane lane, uint8_t onlyPeer) {
		if (!UsesPeerFrameGroups() || PeerFrameBlackout(m_TimingNowMs)) return false;
		std::vector<uint8_t> bytes;
		if (!NetHostMigrationCodec::Encode(message, m_Config.migrationKey, bytes)) return false;
		const bool liveInput = message.type == NetHostMigrationMessageType::PeerInput || message.type == NetHostMigrationMessageType::PeerForwardInput;
		const auto back = m_ReclaimTransactions.find(m_Config.localPeerId);
		const bool details = std::getenv("CC_TEST_RETURN_INPUT_DETAILS") && message.type == NetHostMigrationMessageType::PeerInput &&
		    back != m_ReclaimTransactions.end() && message.frame > back->second.neutralThroughFrame && message.frame <= back->second.neutralThroughFrame + 3;
		const auto wantsInput = [&](uint8_t peer) {
			const auto held = m_PeerBridges.find(peer);
			return !liveInput || held == m_PeerBridges.end() || !held->second.committedHold || held->second.returnFrame.has_value();
		};
		if (m_Config.peerSessionLinks) {
			bool sent = false;
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
				if (peer == m_Config.localPeerId) continue;
				const bool wanted = !m_RemovedPeers.contains(peer) && wantsInput(peer) && (onlyPeer == 0 || onlyPeer == peer);
				const bool delivered = wanted && m_Config.peerSessionLinks->SendTo(peer, lane, bytes);
				sent |= delivered;
				if (details) DiagnosticLine() << "[net-return-input-send] local=" << static_cast<int>(m_Config.localPeerId) << " peer=" << static_cast<int>(peer)
				    << " frame=" << message.frame << " wanted=" << wanted << " sent=" << delivered << " prepared=" << m_Stats.nextFrame
				    << " displayed=" << m_LastCompletedSimulationTick.value_or(0) << " reference=" << static_cast<int>(m_PeerPaceReference)
				    << " catching_up=" << PeerFrameCatchUpActive() << " now_ms=" << m_TimingNowMs << std::endl;
			}
			return sent;
		}
		std::set<uint8_t> sent;
		for (auto& [peer, dial]: PeerProbes()) {
			if ((onlyPeer != 0 && onlyPeer != peer) || !wantsInput(peer) || !dial.transport || !dial.answered || dial.connection == c_InvalidNetPeerId) continue;
			if (dial.transport->Send(dial.connection, lane, bytes)) sent.insert(peer);
		}
		if (PeerListener()) for (const auto& [connection, peer]: PeerListenerBindings()) {
			if ((onlyPeer != 0 && onlyPeer != peer) || !wantsInput(peer) || sent.contains(peer)) continue;
			if (PeerListener()->Send(connection, lane, bytes)) sent.insert(peer);
		}
		if (m_Transport) for (const auto& [peer, connection]: m_RemoteTransports)
			if ((onlyPeer == 0 || onlyPeer == peer) && wantsInput(peer) && !sent.contains(peer) && m_Transport->Send(connection, lane, bytes)) sent.insert(peer);
		if (m_Transport) for (const auto& [connection, peer]: PeerPrimaryBindings())
			if ((onlyPeer == 0 || onlyPeer == peer) && wantsInput(peer) && !sent.contains(peer) && m_Transport->Send(connection, lane, bytes)) sent.insert(peer);
		return !sent.empty();
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
			m_PeerReceiptDelaySamples.clear(); m_PeerOwnReceiptProgress.clear();
			for (uint8_t receiver = 1; receiver <= m_Config.peerCount; ++receiver) m_PeerReceiptSampleFloorMs[receiver] = nowMs;
			DiagnosticLine() << "[net-link-blackout] begin peer=" << static_cast<int>(m_Config.localPeerId) << " frame=" << *m_LastCompletedSimulationTick << " duration_ms=" << m_TestBlackoutDurationMs
			    << " now_ms=" << nowMs << " unix_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << std::endl;
		}
		if (!m_TestBlackoutAtMs) return false;
		const bool active = nowMs >= *m_TestBlackoutAtMs && nowMs - *m_TestBlackoutAtMs < m_TestBlackoutDurationMs;
		if (m_Config.peerSessionLinks) m_Config.peerSessionLinks->frameBlackout = active;
		if (!active && !m_TestBlackoutEnded) {
			m_TestBlackoutEnded = true;
			m_PeerReceiptDelaySamples.clear(); m_PeerOwnReceiptProgress.clear();
			for (uint8_t receiver = 1; receiver <= m_Config.peerCount; ++receiver) m_PeerReceiptSampleFloorMs[receiver] = nowMs;
			DiagnosticLine() << "[net-link-blackout] end peer=" << static_cast<int>(m_Config.localPeerId) << " elapsed_ms=" << nowMs - *m_TestBlackoutAtMs
			    << " now_ms=" << nowMs << " unix_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << std::endl;
		}
		return active;
	}

	bool NetLockstepCoordinator::ReadPeerMessage(const std::vector<uint8_t>& bytes, NetHostMigrationMessage& message, NetHash32* readKey) const {
		if (NetHostMigrationCodec::Decode(bytes, m_Config.migrationKey, message)) {
			if (readKey) *readKey = m_Config.migrationKey;
			return true;
		}
		return m_Config.peerSessionLinks && m_Config.peerSessionLinks->ReadSessionPacket(bytes, message, readKey);
	}

	bool NetLockstepCoordinator::HandlePeerSessionMessage(const NetHostMigrationMessage& message, uint64_t nowMs, const NetHash32* readKey) {
		if (message.type != NetHostMigrationMessageType::PeerSession && message.type != NetHostMigrationMessageType::PeerSessionAck) return false;
		if (!UsesPeerFrameGroups() || !m_Config.peerSessionLinks || PeerFrameBlackout(nowMs)) return true;
		auto& links = *m_Config.peerSessionLinks;
		(void)links.HandleSessionMessage(message, readKey);
		if (links.sessionAttached && message.sessionId == links.sessionId && message.roundId == 1 && message.generation == 1 &&
		    message.configHash == NetHash32{} && message.senderPeerId > 0 && message.senderPeerId <= m_Config.peerCount &&
		    message.senderPeerId != m_Config.localPeerId && message.successorPeerId == m_Config.localPeerId &&
		    !m_RemovedPeers.contains(message.senderPeerId) && links.SessionRoute(message.senderPeerId) != c_InvalidNetPeerId) {
			m_PeerLastHeardMs[message.senderPeerId] = nowMs; m_PeerLinkHeardMs[message.senderPeerId] = nowMs;
			if (message.senderPeerId == GetHostPeerId()) NoteAuthorityHeard(nowMs);
		}
		return true;
	}

	void NetLockstepCoordinator::TickPeerSessionWire(uint64_t nowMs) {
		if (!UsesPeerFrameGroups() || !m_Config.peerSessionLinks || !m_Config.peerSessionLinks->sessionAttached) return;
		auto& links = *m_Config.peerSessionLinks;
		if (m_SessionEventSink) for (auto next = links.events.begin(); next != links.events.end();) {
			// The next round reads its own start packets through the retained wire.
			if (next->type == NetTransportEventType::PacketReceived && NetLockstepCodec::LooksLikePacket(next->bytes)) { ++next; continue; }
			const auto event = std::move(*next); next = links.events.erase(next);
			if (!m_RelayHost && event.peerId == links.SessionRoute(GetHostPeerId()) && event.type == NetTransportEventType::PacketReceived) {
				const auto decoded = NetProtocol::Decode(event.bytes);
				if (decoded.ok) {
					if (const auto* removal = std::get_if<NetParticipantRemoval>(&decoded.message.payload)) m_RemovalBoundary = removal->boundaryFrame;
					if (const auto* end = std::get_if<NetDisconnect>(&decoded.message.payload); end &&
					    (end->disconnectReason == static_cast<uint16_t>(NetRejectReason::ParticipantRemoved) || end->disconnectReason == static_cast<uint16_t>(NetRejectReason::ParticipantBanned))) {
						m_RemovedByHost = true; m_State = NetLockstepState::Stopped;
						m_Stats.timeoutReason = "PeerRemoved:" + end->message;
					}
				}
			}
			m_SessionEventSink(event);
		}
		links.FlushSession(nowMs);
	}

	void NetLockstepCoordinator::TickPeerFrameGroups(uint64_t nowMs) {
		TickPeerSessionWire(nowMs);
		if (!UsesPeerFrameGroups() || !IsRunning()) return;
		if (m_Config.peerSessionLinks && nowMs >= m_PeerPathLogAtMs) {
			m_PeerPathLogAtMs = nowMs + 5000;
			for (const auto& [peer, path]: m_PeerReceiptDelaySamples) {
				const auto [wire, native] = m_Config.peerSessionLinks->RouteTo(peer);
				const auto queue = wire ? wire->GetPeerQueueStats(native) : NetTransportQueueStats{};
				DiagnosticLine() << "[net-peer-path] local=" << static_cast<int>(m_Config.localPeerId) << " peer=" << static_cast<int>(peer)
				    << " now_ms=" << nowMs << " frame=" << m_LastCompletedSimulationTick.value_or(0)
				    << " transport_rtt_ms=" << (wire && wire->IsPeerPingMeasured(native) ? static_cast<int64_t>(wire->GetPeerPingMs(native)) : -1)
				    << " receipt_p95_ms=" << path.P95Ms() << " input_delay_ticks=" << InputDelayAt(m_Config.localPeerId, m_Stats.nextFrame)
				    << " accepted_through=" << GetLocalInputAcceptedThrough().value_or(0) << " reliable_bytes=" << queue.reliableBytes
				    << " queue_ms=" << queue.queueMs << " send_rate_bytes_s=" << queue.sendRateBytesPerSecond << std::endl;
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
			// A newer progress report replaces an older one without waiting on the ordered stream.
			SendPeerFrameMessage(std::move(heartbeat), NetTransportLane::InputUnreliable);
		}
		const bool retryVotes = nowMs >= m_PeerVoteRetryAtMs;
		if (retryVotes) m_PeerVoteRetryAtMs = nowMs + 100;
		// A vote received ahead of our prepared frame becomes eligible as
		// soon as that frame is ready. The retransmission timer must not add
		// up to 100 ms to the first answer of an otherwise healthy voter.
		{
			for (auto& [key, votes]: m_PeerBridgeVotes) {
				if (key.first < m_Stats.nextFrame || m_PeerBridgeCertificates.contains(key)) continue;
				bool newVote = false;
				if (!votes.contains(m_Config.localPeerId)) for (const auto& [peer, vote]: votes) {
					if (!ValidatePeerBridge(vote)) continue;
					auto own = vote;
					own.senderPeerId = m_Config.localPeerId; own.voterMask = SeatBit(m_Config.localPeerId);
					own.appliedFrame = m_LastCompletedSimulationTick.value_or(0);
					if (own.preparedFrame == 3) m_PeerAdminOwnVote = own;
					votes.emplace(m_Config.localPeerId, std::move(own));
					newVote = true;
					if (vote.preparedFrame == 0) for (uint8_t held: vote.members) m_PeerRejectedInputs[vote.frame] |= SeatBit(held);
					break;
				}
				if (const auto own = votes.find(m_Config.localPeerId); own != votes.end() && (newVote || retryVotes)) SendPeerFrameMessage(own->second);
				TryCommitPeerBridge(key.first, nowMs, key.second);
			}
		}
		if (retryVotes) {
			for (const auto& [key, certificate]: m_PeerBridgeCertificates) {
				if (key.first + NetLockstepCodec::c_MaxFutureFrameSkew < m_Stats.nextFrame) continue;
				SendPeerFrameMessage(certificate);
			}
		}
		RepairPeerReceiptGaps(nowMs);
		TickPeerInputDelay(nowMs);
		TickPeerAdmin(nowMs);
		FlushPeerCommittedTails(nowMs);

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
		if (peer != m_Config.localPeerId || delay == 0 || delay > NetLockstepCodec::c_MaxPeerInputDelayFrames || frame <= m_Stats.nextFrame || !PeerGroupHasAuthority(m_FrameGroupMembers)) return false;
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
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if (peer == m_Config.localPeerId || m_RemovedPeers.contains(peer) || IsSeatReleased(peer)) continue;
			auto& sample = m_PeerReceiptDelaySamples[peer];
			sample.Observe(nowMs, 0);
			const auto [wire, route] = PeerFrameRoute(peer);
			// Size a held reader's future live runway from transport, outside its catch-up receipts.
			if ((m_FrameGroupMembers & SeatBit(peer)) != 0 && sample.P95Ms() != 0) paths.emplace_back(sample.P95Ms(), sample.JitterMs());
			else if (wire && wire->IsPeerPingMeasured(route)) paths.emplace_back(wire->GetPeerPingMs(route), 0);
		}
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
		const uint16_t required = static_cast<uint16_t>(std::min<uint32_t>(NetLockstepCodec::c_MaxPeerInputDelayFrames,
		    std::max<uint32_t>(m_Config.matchConfig.inputDelayFrames, static_cast<uint32_t>(std::ceil((rtt + margin) / m_Config.simTickMs)))));
		local.jitterMs = std::max<uint32_t>(margin, static_cast<uint32_t>(std::ceil(required * m_Config.simTickMs)) - std::min<uint32_t>(rtt, static_cast<uint32_t>(std::ceil(required * m_Config.simTickMs))));
		const auto last = m_LastDelayResizeMs.find(m_Config.localPeerId);
		if (last != m_LastDelayResizeMs.end() && nowMs < last->second + c_DelayResizeIntervalMs) return;
		if (required > current) {
			m_PeerDelayBelowSinceMs.erase(m_Config.localPeerId);
			(void)ProposePeerInputDelay(m_Config.localPeerId, required, FutureTimingFrame());
		} else if (required < current) {
			const auto [since, inserted] = m_PeerDelayBelowSinceMs.try_emplace(m_Config.localPeerId, nowMs);
			const uint16_t gap = current - required;
			const uint64_t interval = gap > 2 ? std::max<uint64_t>(c_DelayResizeIntervalMs, c_DelayShrinkIntervalMs / gap) : c_DelayShrinkIntervalMs;
			const uint16_t delay = gap > 2 ? std::min<uint16_t>(NetLockstepCodec::c_MaxPeerInputDelayFrames, required + std::max<uint16_t>(2, gap / 2))
			                             : current - std::min(current, c_DelayShrinkStep);
			if (nowMs >= since->second + interval && ProposePeerInputDelay(m_Config.localPeerId, delay, FutureTimingFrame())) since->second = nowMs;
		} else m_PeerDelayBelowSinceMs.erase(m_Config.localPeerId);
	}

} // namespace RTE
