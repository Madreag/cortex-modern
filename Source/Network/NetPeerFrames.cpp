#include "NetPeerFrameHelpers.h"

#include "DiagnosticLine.h"
#include "NetAuthCrypto.h"
#include "NetHttpClient.h"
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
		// Votes and certificates are independently authenticated and resent. An unreliable copy reaches a live plane even
		// while its reliable lane is recovering a lost packet; the reliable copy still guarantees eventual delivery.
		const bool expedited = lane == NetTransportLane::ControlReliable &&
		    (message.type == NetHostMigrationMessageType::PeerBridge || message.type == NetHostMigrationMessageType::PeerBridgeCommit) &&
		    SendPeerFrameMessage(message, NetTransportLane::InputUnreliable, onlyPeer);
		const bool liveInput = message.type == NetHostMigrationMessageType::PeerInput || message.type == NetHostMigrationMessageType::PeerForwardInput;
		const auto back = m_ReclaimTransactions.find(m_Config.localPeerId);
		const bool details = std::getenv("CC_TEST_RETURN_INPUT_DETAILS") && message.type == NetHostMigrationMessageType::PeerInput &&
		    back != m_ReclaimTransactions.end() && message.frame > back->second.neutralThroughFrame && message.frame <= back->second.neutralThroughFrame + 3;
		const auto wantsInput = [&](uint8_t peer) { return !liveInput || PeerWantsLiveInput(peer); };
		if (m_Config.peerSessionLinks) {
			bool sent = false;
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
				if (peer == m_Config.localPeerId) continue;
				const bool wanted = !m_RemovedPeers.contains(peer) && wantsInput(peer) && (onlyPeer == 0 || onlyPeer == peer);
				const bool delivered = wanted && m_Config.peerSessionLinks->SendTo(peer, lane, bytes, true);
				sent |= delivered;
				if (details) DiagnosticLine() << "[net-return-input-send] local=" << static_cast<int>(m_Config.localPeerId) << " peer=" << static_cast<int>(peer)
				    << " frame=" << message.frame << " wanted=" << wanted << " sent=" << delivered << " prepared=" << m_Stats.nextFrame
				    << " displayed=" << m_LastCompletedSimulationTick.value_or(0) << " reference=" << static_cast<int>(m_PeerPaceReference)
				    << " catching_up=" << PeerFrameCatchUpActive() << " now_ms=" << m_TimingNowMs << std::endl;
			}
			return sent || expedited;
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
		return !sent.empty() || expedited;
	}

	bool NetLockstepCoordinator::PeerWantsLiveInput(uint8_t peer) const {
		const auto held = m_PeerBridges.find(peer);
		return held == m_PeerBridges.end() || !held->second.committedHold || held->second.returnFrame.has_value();
	}

	uint32_t NetLockstepCoordinator::PeerHeldSeatsAt(uint64_t frame) const {
		uint32_t held = 0;
		// Restored/private-tail bridges can precede the first decision certificate received here.
		for (const auto& [peer, bridge]: m_PeerBridges)
			if (bridge.committedHold && frame >= bridge.fromFrame && (!bridge.returnFrame || frame < *bridge.returnFrame)) held |= SeatBit(peer);
		for (const auto& [peer, changes]: m_PeerFrameHolds) {
			const auto after = changes.upper_bound(frame);
			if (after != changes.begin() && std::prev(after)->second) held |= SeatBit(peer);
			else held &= ~SeatBit(peer);
		}
		return held;
	}

	uint32_t NetLockstepCoordinator::PeerRetainedSeatsAt(uint64_t frame) const {
		uint32_t retained = (1U << m_Config.peerCount) - 1;
		for (const auto& [peer, changes]: m_PeerOwnershipChanges) {
			const auto after = changes.upper_bound(frame);
			if (after != changes.begin() && !std::prev(after)->second) retained &= ~SeatBit(peer);
		}
		return retained;
	}

	uint32_t NetLockstepCoordinator::PeerFrameMembersAt(uint64_t frame) const {
		const auto group = m_FrameGroupChanges.upper_bound(frame);
		const uint32_t members = group == m_FrameGroupChanges.begin() ? (1U << m_Config.peerCount) - 1 : std::prev(group)->second;
		// A future return can have been signed before another seat's intervening hold.
		// Its snapshot cannot return that other seat: only a return naming it ends its hold.
		return members & PeerRetainedSeatsAt(frame) & ~PeerHeldSeatsAt(frame);
	}

	void NetLockstepCoordinator::PrunePeerFrameSeatChanges(uint64_t floorFrame) {
		for (auto* histories: {&m_PeerFrameHolds, &m_PeerOwnershipChanges}) for (auto& [peer, history]: *histories) {
			const auto floor = history.lower_bound(floorFrame);
			if (floor != history.begin()) history.erase(history.begin(), std::prev(floor));
		}
	}

	uint32_t NetLockstepCoordinator::PeerVotingOwners(uint32_t& voters, uint64_t frame) const {
		const uint64_t at = frame == UINT64_MAX ? m_Stats.nextFrame : frame;
		uint32_t owners = PeerRetainedSeatsAt(at);
		if (voters == 0 || (voters & ~owners) != 0) return 0;
		// A declared Leave keeps its AI seat but answers nothing more from the moment it is declared, its last inputs ahead included.
		for (const auto& [peer, ended]: m_PeerRoundEnds) if (!ended.closedLink) owners &= ~SeatBit(peer);
		// A returning seat answers nothing until it plays, so it votes only after its agreed neutral interval.
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer)
			if (IsSeatReclaimGap(peer, at)) owners &= ~SeatBit(peer);
		// A seat the round holds answers nothing until it returns: the seats that play decide without it, so a second seat lost while
		// the first is held is held too instead of leaving the rest without a majority.
		owners &= ~PeerHeldSeatsAt(at);
		voters &= owners;
		return voters == 0 ? 0 : owners;
	}

	bool NetLockstepCoordinator::PeerTieRefereed(uint64_t frame) const {
		// The directory referees an even split of two or four retained seats; any other tie goes to the host's half. Three retained
		// seats never split evenly for it: with one held, the two that play tie one to one and the host's half keeps the round.
		const int retained = std::popcount(PeerRetainedSeatsAt(frame == UINT64_MAX ? m_Stats.nextFrame : frame));
		return m_Config.frameTieReferee && (retained == 2 || retained == 4);
	}

	uint8_t NetLockstepCoordinator::PeerTieHalfHolder(uint32_t owners) const {
		// An unrefereed tie goes to the host's half. A host the round holds or has lost has no half, so the first playing seat in
		// successor order takes its place, the same seat on every peer.
		if ((owners & SeatBit(GetHostPeerId())) != 0 || owners == 0) return GetHostPeerId();
		for (uint8_t peer: m_Config.matchConfig.successorOrder) if ((owners & SeatBit(peer)) != 0) return peer;
		return static_cast<uint8_t>(std::countr_zero(owners) + 1);
	}

	bool NetLockstepCoordinator::PeerGroupIsRefereedHalf(uint32_t voters, uint64_t frame) const {
		const uint32_t owners = PeerTieRefereed(frame) ? PeerVotingOwners(voters, frame) : 0;
		return owners != 0 && 2 * std::popcount(voters) == std::popcount(owners);
	}

	bool NetLockstepCoordinator::PeerGroupHasAuthority(uint32_t voters, uint64_t frame) const {
		// A decision is judged at its own frame: a peer still behind the split it names would otherwise look up another split's answer.
		const uint64_t at = frame == UINT64_MAX ? m_Stats.nextFrame : frame;
		const uint32_t owners = PeerVotingOwners(voters, at);
		if (owners == 0) return false;
		if (m_PeerAdminFrameMembers != 0 && voters == m_PeerAdminFrameMembers) return true;
		const int votes = std::popcount(voters), seats = std::popcount(owners);
		if (2 * votes != seats) return 2 * votes > seats;
		if (!PeerTieRefereed(at)) return (voters & SeatBit(PeerTieHalfHolder(owners))) != 0;
		const auto decision = m_PeerFrameTieWinners.find(PeerSplitEpoch(at));
		return decision != m_PeerFrameTieWinners.end() && decision->second == voters;
	}

	bool NetLockstepCoordinator::PeerFrameGroupConfirms(uint32_t agreeing, uint32_t members, uint64_t frame) const {
		if ((agreeing & ~members) != 0) return false;
		const int votes = std::popcount(agreeing), seats = std::popcount(members);
		if (2 * votes != seats) return 2 * votes > seats;
		if (!PeerTieRefereed(frame)) return (agreeing & SeatBit(PeerTieHalfHolder(members))) != 0;
		const auto decision = m_PeerFrameTieWinners.find(PeerSplitEpoch(frame));
		return decision != m_PeerFrameTieWinners.end() && decision->second == agreeing;
	}

	uint64_t NetLockstepCoordinator::PeerSplitEpoch(uint64_t frame) const {
		// A seat held at the frame plays in neither half, so each new set of held seats names a new split for the referee.
		uint32_t whole = PeerRetainedSeatsAt(frame);
		whole &= ~PeerHeldSeatsAt(frame);
		uint64_t name = m_Config.startFrame;
		for (auto change = m_FrameGroupChanges.upper_bound(frame); change != m_FrameGroupChanges.begin();)
			if ((PeerFrameMembersAt((--change)->first) & whole) == whole) { name = change->first; break; }
		// A split the whole group has played past since its tie was decided is named afresh, so the next tie is a new question.
		for (auto renewal = m_PeerSplitRenewals.upper_bound(frame); renewal != m_PeerSplitRenewals.begin();)
			if (((--renewal)->second & whole) == whole) { name = std::max(name, renewal->first); break; }
		return name;
	}

	void NetLockstepCoordinator::CheckPeerFrameTie(uint64_t frame, uint32_t members, uint64_t nowMs) {
		// Both halves name a split by the last frame the whole group played, so every check-in of one split meets one decision.
		const uint64_t split = PeerSplitEpoch(frame);
		if (!PeerTieRefereed(frame) || m_PeerFrameTieWinners.contains(split)) return;
		NetFrameTieRequest request;
		request.generation = m_Config.migrationGeneration; request.roundId = m_Config.matchConfig.roundId;
		request.frame = split; request.configHash = m_RoundConfigHash; request.host = GetHostPeerId();
		request.queryOnly = (members & SeatBit(m_Config.localPeerId)) == 0;
		// The tie is between the seats that vote now, and the referee decides an even split of every retained seat: the seats that
		// do not vote are dealt to the halves in seat order, the lower ones to the half that holds the lowest voting seat.
		uint32_t voting = members;
		const uint32_t owners = PeerVotingOwners(voting, frame);
		if (owners == 0 || 2 * std::popcount(voting) != std::popcount(owners)) return;
		uint32_t retained = 0;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer)
			if (!m_RemovedPeers.contains(peer) && !IsSeatReleased(peer)) retained |= SeatBit(peer);
		const bool lowerHalf = (voting & (owners & (~owners + 1))) != 0;
		uint32_t half = voting;
		int dealt = 0;
		const int share = std::popcount(retained & ~owners) / 2;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if ((retained & ~owners & SeatBit(peer)) == 0) continue;
			if ((dealt++ < share) == lowerHalf) half |= SeatBit(peer);
		}
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if ((retained & SeatBit(peer)) == 0) continue;
			request.owners.push_back(peer);
			if ((half & SeatBit(peer)) != 0) request.members.push_back(peer);
		}
		if (request.members.size() * 2 != request.owners.size()) return;
		// A seat that heard the other half play on keeps asking what was decided: the referee holds one request per seat, and
		// checking in again would replace the question and lose its answer.
		if (m_PeerFrameTie && m_PeerFrameTie->queryOnly) {
			auto asked = *m_PeerFrameTie;
			asked.members = request.members; asked.queryOnly = request.queryOnly;
			if (asked == request) request = *m_PeerFrameTie;
		}
		if (!m_PeerFrameTie || *m_PeerFrameTie != request) { m_PeerFrameTie = request; m_PeerFrameTieSinceMs = nowMs; }
		const auto reply = m_Config.frameTieReferee(request);
		if (reply.state == NetFrameTieReply::State::Decided) {
			uint32_t winner = 0;
			for (uint16_t peer: reply.members) {
				if (peer == 0 || peer > m_Config.peerCount || std::find(request.owners.begin(), request.owners.end(), peer) == request.owners.end()) return;
				winner |= SeatBit(static_cast<uint8_t>(peer));
			}
			// The decided half is kept as the voting seats it holds, the form every authority check compares.
			if (std::popcount(winner) * 2 == static_cast<int>(request.owners.size()) && 2 * std::popcount(winner & owners) == std::popcount(owners))
				m_PeerFrameTieWinners.emplace(split, winner & owners);
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
			    << " now_ms=" << nowMs << " unix_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()
			    << " directory=" << (m_TestBlackoutDirectoryDark ? "dark" : "up") << std::endl;
		}
		if (!m_TestBlackoutAtMs) return false;
		const bool active = nowMs >= *m_TestBlackoutAtMs && nowMs - *m_TestBlackoutAtMs < m_TestBlackoutDurationMs;
		if (m_Config.peerSessionLinks) m_Config.peerSessionLinks->frameBlackout = active;
		if (const bool offline = active && m_TestBlackoutDirectoryDark; offline != m_TestBlackoutOffline) {
			m_TestBlackoutOffline = offline;
			NetHttpClient::SetOfflineForTest(offline);
		}
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
			m_PeerHeartbeatAtMs = nowMs + c_NetPeerHeartbeatMs;
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
		if (retryVotes) m_PeerVoteRetryAtMs = nowMs + c_NetPeerVoteResendMs;
		// A vote received ahead of our prepared frame becomes eligible as
		// soon as that frame is ready. The retransmission timer must not add
		// up to 100 ms to the first answer of an otherwise healthy voter.
		{
			for (auto& [key, votes]: m_PeerBridgeVotes) {
				if (key.first < m_Stats.nextFrame || m_PeerBridgeCertificates.contains(key)) continue;
				bool newVote = false;
				const auto previous = votes.find(m_Config.localPeerId);
				if (previous == votes.end() || previous->second.voterMask == 0) for (const auto& [peer, vote]: votes) {
					if (peer == m_Config.localPeerId ||
					    (vote.voterMask == 0 && !(vote.preparedFrame == 0 && vote.members.size() == 1 && vote.members.front() == m_Config.localPeerId)) ||
					    (vote.voterMask != 0 && vote.voterMask != SeatBit(peer))) continue;
					if (!ValidatePeerBridge(vote) && !CoSignsOwnHold(vote)) continue;
					// A stored proposal keeps the same single-return promise as an
					// immediate reply; waiting for a later plane tick cannot add a second signature.
					if (VotedOtherPeerReturn(vote)) continue;
					auto own = vote;
					own.senderPeerId = m_Config.localPeerId; own.voterMask = SeatBit(m_Config.localPeerId);
					own.appliedFrame = m_LastCompletedSimulationTick.value_or(0);
					if (own.preparedFrame == 3) m_PeerAdminOwnVote = own;
					votes.insert_or_assign(m_Config.localPeerId, std::move(own));
					newVote = true;
					if (vote.preparedFrame == 0) for (uint8_t held: vote.members) {
						m_PeerRejectedInputs[vote.frame] |= SeatBit(held);
						m_HoldFacts[held] = {vote.frame, 0, m_Config.slowPlayerBoundTicks * m_Config.simTickMs, "peer_hold_proposal"};
						DiagnosticLine() << "[net-peer-hold-trigger] local=" << static_cast<int>(m_Config.localPeerId) << " peer=" << static_cast<int>(held)
						    << " frame=" << vote.frame << " ai_frame=" << vote.frame + static_cast<uint64_t>(std::ceil(c_NetInputContinuityMs / m_Config.simTickMs))
						    << " now_ms=" << nowMs << " cause=peer_hold_proposal requested_by=" << static_cast<int>(peer)
						    << " last_heard_ms=" << LastAuthenticatedTraffic(held) << " received_through=" << m_Stats.peers[held].highestTargetFrame << std::endl;
					}
					break;
				}
				if (const auto own = votes.find(m_Config.localPeerId); own != votes.end() && (newVote || retryVotes)) SendPeerFrameMessage(own->second);
				TryCommitPeerBridge(key.first, nowMs, key.second);
			}
		}
		if (retryVotes) {
			// A partition can lose the first roster notice. Repeat that exact authority and frame;
			// elapsed time only schedules its publication, never a different removal.
			if (m_RelayHost && m_Config.localPeerId == GetHostPeerId()) for (const auto& [peer, notice]: m_PeerRemovalNotices)
				if (m_RemovedPeers.contains(peer)) (void)SendPacket({notice}, NetTransportLane::ControlReliable, nullptr);
			for (const auto& [key, certificate]: m_PeerBridgeCertificates) {
				if (key.first + NetLockstepCodec::c_MaxFutureFrameSkew < m_Stats.nextFrame) continue;
				SendPeerFrameMessage(certificate);
			}
		}
		RepairPeerReceiptGaps(nowMs);
		RenewSettledPeerSplit(nowMs);
		TickPeerInputDelay(nowMs);
		TickPeerAdmin(nowMs);
		TickPeerCaptureRequest(nowMs);
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
		m_PeerTailVotes.erase(m_PeerTailVotes.begin(), m_PeerTailVotes.lower_bound(m_Stats.nextFrame));
		// The split in force keeps its name and its decision however long it lasts.
		const uint64_t historyFloor = std::min(kept, PeerSplitEpoch(kept));
		for (auto* history: {&m_PeerFrameTieWinners, &m_FrameGroupChanges, &m_PeerSplitRenewals}) {
			const auto floor = history->lower_bound(historyFloor);
			if (floor != history->begin()) history->erase(history->begin(), std::prev(floor));
		}
		PrunePeerFrameSeatChanges(historyFloor);
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

	void NetLockstepCoordinator::RenewSettledPeerSplit(uint64_t nowMs) {
		// A tie decided in this split outlives it unless the group names a new split. Once every member feeds again, the lowest playing
		// seat asks the whole group for one by restating its own delay, which only the whole group, its split healed, can certify.
		if (WaitsForPlacement() || m_Config.simTickMs <= 0 || m_FrameGroupMembers == 0 || std::countr_zero(m_FrameGroupMembers) + 1 != m_Config.localPeerId ||
		    !m_PeerFrameTieWinners.contains(PeerSplitEpoch(m_Stats.nextFrame))) return;
		const uint64_t boundMs = static_cast<uint64_t>(std::ceil(m_Config.slowPlayerBoundTicks * m_Config.simTickMs));
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if (peer == m_Config.localPeerId || (m_FrameGroupMembers & SeatBit(peer)) == 0) continue;
			const auto& arrivals = m_Stats.peers[peer].arrivals;
			if (arrivals.empty() || nowMs > arrivals.back().first + boundMs) return;
		}
		(void)ProposePeerInputDelay(m_Config.localPeerId, InputDelayAt(m_Config.localPeerId, m_Stats.nextFrame), FutureTimingFrame());
	}

	void NetLockstepCoordinator::TickPeerInputDelay(uint64_t nowMs) {
		if (!m_Config.adaptiveInputDelay || WaitsForPlacement() || !std::isfinite(m_Config.simTickMs) || m_Config.simTickMs <= 0) return;
		if ((m_FrameGroupMembers & SeatBit(m_Config.localPeerId)) == 0) return;
		std::vector<std::pair<uint32_t, uint32_t>> paths;
		bool transportsMeasured = true;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if (peer == m_Config.localPeerId || m_RemovedPeers.contains(peer) || IsSeatReleased(peer) || (m_FrameGroupMembers & SeatBit(peer)) == 0) continue;
			auto& sample = m_PeerReceiptDelaySamples[peer];
			sample.Observe(nowMs, 0);
			const auto [wire, route] = PeerFrameRoute(peer);
			// A receipt percentile already measures the complete round trip through the input and acceptance pumps. Its jitter and
			// scheduling time are inside that measurement. Raw transport RTT alone needs one input-production tick beside it.
			const bool transportMeasured = wire && wire->IsPeerPingMeasured(route);
			const uint32_t transportMs = transportMeasured ? wire->GetPeerPingMs(route) : 0;
			if (sample.P95Ms() != 0 || transportMeasured) {
				transportsMeasured &= transportMeasured;
				const uint32_t receiptFrames = static_cast<uint32_t>(std::ceil(sample.P95Ms() / m_Config.simTickMs));
				const uint32_t transportFrames = !transportMeasured ? 0U : static_cast<uint32_t>(std::ceil(transportMs / m_Config.simTickMs)) + 1;
				paths.emplace_back(std::max(receiptFrames, transportFrames), transportMs);
			}
		}
		const size_t needed = std::popcount(m_FrameGroupMembers) - 1;
		if (needed == 0 || paths.size() < needed) return;
		std::sort(paths.begin(), paths.end());
		// Every active receiver needs the sender's buffered input. Sizing to
		// only a quorum slowly drains the runway of a healthy, longer path.
		const auto [bufferedFrames, transportMs] = paths.back();
		const uint32_t rtt = transportMs;
		auto& local = m_Stats.peers[m_Config.localPeerId];
		local.pingMs = rtt; local.pingMeasured = transportsMeasured;
		const uint16_t current = InputDelayAt(m_Config.localPeerId, m_Stats.nextFrame);
		const uint16_t required = static_cast<uint16_t>(std::min<uint32_t>(NetLockstepCodec::c_MaxPeerInputDelayFrames,
		    std::max<uint32_t>(m_Config.matchConfig.inputDelayFrames, bufferedFrames)));
		local.jitterMs = static_cast<uint32_t>(std::ceil(required * m_Config.simTickMs)) - std::min<uint32_t>(rtt, static_cast<uint32_t>(std::ceil(required * m_Config.simTickMs)));
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
