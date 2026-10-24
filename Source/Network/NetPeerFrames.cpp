#include "NetPeerFrameHelpers.h"

#include "DiagnosticLine.h"
#include "NetAuthCrypto.h"
#include "NetHttpClient.h"
#include "NetLobbyProtocol.h"
#include "NetProtocol.h"

#include "lz4.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <utility>

namespace RTE {

	using namespace NetPeerFrameDetail;
	// The low seat bits acknowledge committed continuations; the next seat bits acknowledge pre-hold reader reports.
	// Heartbeats publish those distinct masks in voterMask and offset, respectively. A reader receipt cannot decide a split.
	static constexpr unsigned c_DirectReaderReceiptShift = NetMatchConfigUtil::c_MaxPeerCount;
	static constexpr uint32_t c_OtherContinuationFrames = 3;

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
		// A certificate proves itself by the votes it carries. A seat that passes one on sends it as its own, with its own progress: the
		// receiver reads a message only from the seat its link is bound to, and takes its applied frame as that seat's.
		if (message.type == NetHostMigrationMessageType::PeerBridgeCommit) {
			message.senderPeerId = m_Config.localPeerId;
			message.appliedFrame = m_LastCompletedSimulationTick.value_or(0);
		}
		std::vector<uint8_t> bytes;
		if (!NetHostMigrationCodec::Encode(message, m_Config.migrationKey, bytes)) return false;
		// Votes and certificates are independently authenticated and resent. An unreliable copy reaches a live plane even
		// while its reliable lane is recovering a lost packet; the reliable copy still guarantees eventual delivery.
		const bool expedited = lane == NetTransportLane::ControlReliable &&
		    (message.type == NetHostMigrationMessageType::PeerBridge || message.type == NetHostMigrationMessageType::PeerBridgeCommit) &&
		    SendPeerFrameMessage(message, NetTransportLane::InputUnreliable, onlyPeer);
		const bool liveInput = message.type == NetHostMigrationMessageType::PeerInput || message.type == NetHostMigrationMessageType::PeerForwardInput;
		const auto back = m_ReclaimTransactions.find(m_Config.localPeerId);
		const bool details = ReturnInputDetails() && message.type == NetHostMigrationMessageType::PeerInput &&
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
		// A private image reconstructs seat authority from committed replay records, without live bridge votes. Those held
		// seats still cannot vote when this reader validates the survivors' return certificate.
		if (m_Playback) {
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if (IsSeatUnderAI(peer, frame)) held |= SeatBit(peer);
			return held;
		}
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
		uint32_t members = group == m_FrameGroupChanges.begin() ? (1U << m_Config.peerCount) - 1 : std::prev(group)->second;
		// A group snapshot cannot override another seat's independently certified hold or return.
		for (const auto& [peer, changes]: m_PeerFrameHolds) {
			const auto after = changes.upper_bound(frame);
			if (after != changes.begin() && !std::prev(after)->second) members |= SeatBit(peer);
		}
		return members & PeerRetainedSeatsAt(frame) & ~PeerHeldSeatsAt(frame);
	}

	void NetLockstepCoordinator::PrunePeerFrameSeatChanges(uint64_t floorFrame) {
		for (auto* histories: {&m_PeerFrameHolds, &m_PeerOwnershipChanges}) for (auto& [peer, history]: *histories) {
			const auto floor = history.lower_bound(floorFrame);
			if (floor != history.begin()) history.erase(history.begin(), std::prev(floor));
		}
	}

	uint32_t NetLockstepCoordinator::PeerVotingOwners(uint32_t& voters, uint64_t frame, PeerVotingScope scope) const {
		const uint64_t at = frame == UINT64_MAX ? m_Stats.nextFrame : frame;
		uint32_t owners = PeerRetainedSeatsAt(at);
		if (voters == 0 || (voters & ~owners) != 0) return 0;
		// A declared Leave keeps its AI seat but answers nothing more from the moment it is declared, its last inputs ahead included.
		for (const auto& [peer, ended]: m_PeerRoundEnds) if (!ended.closedLink) owners &= ~SeatBit(peer);
		// A participating neutral seat owes no input receipt, but can witness its continuation's history.
		if (scope == PeerVotingScope::Input) for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer)
			if (IsSeatReclaimGap(peer, at)) owners &= ~SeatBit(peer);
		// A seat the round holds answers nothing until it returns: the seats that play decide without it, so a second seat lost while
		// the first is held is held too instead of leaving the rest without a majority.
		owners &= ~PeerHeldSeatsAt(at);
		voters &= owners;
		return voters == 0 ? 0 : owners;
	}

	bool NetLockstepCoordinator::PeerTieRefereed(uint64_t frame) const {
		// Every internet split is arbitrated, including an unequal split. The decision never gates a survivor's local hold.
		const int retained = std::popcount(PeerRetainedSeatsAt(frame == UINT64_MAX ? m_Stats.nextFrame : frame));
		return m_Config.frameTieReferee && retained >= 2 && retained <= 4;
	}

	uint8_t NetLockstepCoordinator::PeerTieHalfHolder(uint32_t owners) const {
		// An unrefereed tie goes to the host's half. A host the round holds or has lost has no half, so the first playing seat in
		// successor order takes its place, the same seat on every peer.
		if ((owners & SeatBit(GetHostPeerId())) != 0 || owners == 0) return GetHostPeerId();
		for (uint8_t peer: m_Config.matchConfig.successorOrder) if ((owners & SeatBit(peer)) != 0) return peer;
		return static_cast<uint8_t>(std::countr_zero(owners) + 1);
	}

	bool NetLockstepCoordinator::PeerGroupNeedsArbitration(uint32_t voters, uint64_t frame) const {
		const uint32_t owners = PeerTieRefereed(frame) ? PeerVotingOwners(voters, frame) : 0;
		return owners != 0 && voters != owners;
	}

	bool NetLockstepCoordinator::PeerGroupHasAuthority(uint32_t voters, uint64_t frame, PeerVotingScope scope) const {
		// A decision is judged at its own frame: a peer still behind the split it names would otherwise look up another split's answer.
		const uint64_t at = frame == UINT64_MAX ? m_Stats.nextFrame : frame;
		// A seat no other seat can reach answers alone for its own return as the round's lone player.
		if (m_LoneSeatInstalling && voters == SeatBit(m_Config.localPeerId)) return true;
		const uint32_t owners = PeerVotingOwners(voters, at, scope);
		if (owners == 0) return false;
		if (m_PeerAdminFrameMembers != 0 && voters == m_PeerAdminFrameMembers) return true;
		const auto decision = m_PeerFrameTieWinners.find(PeerSplitEpoch(at));
		if (PeerTieRefereed(at) && decision != m_PeerFrameTieWinners.end()) return decision->second == voters;
		const int votes = std::popcount(voters), seats = std::popcount(owners);
		if (2 * votes != seats) return 2 * votes > seats;
		if (!PeerTieRefereed(at)) return (voters & SeatBit(PeerTieHalfHolder(owners))) != 0;
		return false;
	}

	bool NetLockstepCoordinator::PeerFrameGroupConfirms(uint32_t agreeing, uint32_t members, uint64_t frame) const {
		if ((agreeing & ~members) != 0) return false;
		const int votes = std::popcount(agreeing), seats = std::popcount(members);
		if (2 * votes != seats) return 2 * votes > seats;
		if (!PeerTieRefereed(frame)) return (agreeing & SeatBit(PeerTieHalfHolder(members))) != 0;
		const auto decision = m_PeerFrameTieWinners.find(PeerSplitEpoch(frame));
		return decision != m_PeerFrameTieWinners.end() && decision->second == agreeing;
	}

	bool NetLockstepCoordinator::PeerGroupMayHold(uint32_t voters, uint64_t frame) const {
		return PeerVotingOwners(voters, frame, PeerVotingScope::History) != 0;
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

	void NetLockstepCoordinator::NotePeerContinuation(uint8_t peer, uint64_t frame, const NetHash32& applied, uint32_t peerMembers) {
		if (!UsesPeerFrameGroups() || m_Playback || !IsRunning() || peer == 0 || peer > m_Config.peerCount || peer == m_Config.localPeerId ||
		    m_PeerOtherContinuations.contains(peer) || applied == NetHash32{} || !m_LastCompletedSimulationTick || frame > *m_LastCompletedSimulationTick ||
		    !m_MigrationHistory.contains(frame) || IsLocalPrivateReaderAt(frame)) return;
		// Both seats showed the frame in one group, each counting the other in it, and the member still plays here: a record that
		// differs is another history inside this group. A seat that had held this one plays a split, which the heal decides.
		const uint32_t members = PeerFrameMembersAt(frame);
		if ((members & SeatBit(peer)) == 0 || (members & SeatBit(m_Config.localPeerId)) == 0 || (peerMembers & SeatBit(m_Config.localPeerId)) == 0 ||
		    (PeerFrameMembersAt(m_Stats.nextFrame) & SeatBit(peer)) == 0) return;
		const auto own = PeerAppliedFramePrefix(frame);
		if (own == NetHash32{}) return;
		auto& evidence = m_PeerContinuationEvidence[peer];
		if (own == applied) {
			if (evidence.frames.empty() || frame >= *evidence.frames.rbegin()) evidence.frames.clear();
			return;
		}
		// One frame can carry a seat's own transition facts; another continuation differs on every frame it plays, whatever order
		// its records arrive in.
		evidence.frames.insert(frame);
		if (evidence.frames.size() < c_OtherContinuationFrames) return;
		m_PeerOtherContinuations[peer] = *evidence.frames.begin();
		m_PeerContinuationEvidence.erase(peer);
		DiagnosticLine() << "[net-peer-continuation] local=" << static_cast<int>(m_Config.localPeerId) << " peer=" << static_cast<int>(peer) << " frame=" << m_PeerOtherContinuations.at(peer)
		    << " next=" << m_Stats.nextFrame << " displayed=" << *m_LastCompletedSimulationTick << " now_ms=" << m_TimingNowMs << std::endl;
	}

	void NetLockstepCoordinator::NotePeerRecord(uint8_t peer, uint64_t frame, const NetHash32& applied) {
		if (!UsesPeerFrameGroups() || peer == 0 || peer > m_Config.peerCount || peer == m_Config.localPeerId || applied == NetHash32{} ||
		    !m_LastCompletedSimulationTick || frame > *m_LastCompletedSimulationTick || !m_MigrationHistory.contains(frame)) return;
		const auto own = PeerAppliedFramePrefix(frame);
		if (own != NetHash32{}) m_PeerRecordMatches[peer][frame] = own == applied;
	}

	NetLockstepCoordinator::ChecksumCause NetLockstepCoordinator::PeerChecksumCause(uint8_t peer, uint64_t frame, uint64_t& firstDiffering) const {
		const uint64_t verified = m_VerifiedAppliedFrames.contains(peer) ? m_VerifiedAppliedFrames.at(peer) : 0;
		const auto records = m_PeerRecordMatches.find(peer);
		if (records != m_PeerRecordMatches.end())
			for (auto record = records->second.upper_bound(verified); record != records->second.end() && record->first <= frame; ++record)
				if (!record->second) { firstDiffering = record->first; return ChecksumCause::Continuation; }
		// Every frame still kept since the last match is known to be the same record before the hash is called a desync.
		return MissingPeerRecords(peer, frame, 1).empty() ? ChecksumCause::Desync : ChecksumCause::Undecided;
	}

	std::vector<uint64_t> NetLockstepCoordinator::MissingPeerRecords(uint8_t peer, uint64_t frame, size_t limit) const {
		std::vector<uint64_t> missing;
		const uint64_t verified = m_VerifiedAppliedFrames.contains(peer) ? m_VerifiedAppliedFrames.at(peer) : 0;
		const auto records = m_PeerRecordMatches.find(peer);
		const uint64_t kept = m_Stats.nextFrame > NetHostMigrationCodec::c_PeerHistoryFrames ? m_Stats.nextFrame - NetHostMigrationCodec::c_PeerHistoryFrames : 0;
		// A side's state at the first frame it holds a record of came with an image, so the records from there on decide.
		const uint64_t own = m_MigrationHistory.empty() ? 0 : m_MigrationHistory.begin()->first;
		const auto theirs = m_PeerRecordFloors.find(peer);
		for (uint64_t at = std::max({verified + 1, kept, m_Config.startFrame, own, theirs == m_PeerRecordFloors.end() ? 0 : theirs->second}); at <= frame && missing.size() < limit; ++at)
			if (records == m_PeerRecordMatches.end() || !records->second.contains(at)) missing.push_back(at);
		return missing;
	}

	void NetLockstepCoordinator::RequestPeerRecords(uint64_t nowMs) {
		// A witness travels once on the frame lane: a differing hash whose records were lost asks the member for them until they arrive.
		std::map<uint8_t, std::vector<uint64_t>> wanted;
		for (const uint64_t frame: m_DeferredChecksumFrames) {
			const auto local = m_LocalChecksums.find(frame);
			const auto remote = m_RemoteChecksums.find(frame);
			if (local == m_LocalChecksums.end() || remote == m_RemoteChecksums.end()) continue;
			for (const auto& [peer, hash]: remote->second) {
				if (hash == local->second || wanted.contains(peer) || IsSeatUnderAI(peer, frame)) continue;
				if (auto missing = MissingPeerRecords(peer, frame, c_NetPeerRecordBatch); !missing.empty()) wanted[peer] = std::move(missing);
			}
		}
		std::erase_if(m_PeerRecordRequestAtMs, [&](const auto& entry) { return !wanted.contains(entry.first); });
		for (const auto& [peer, frames]: wanted) {
			// A record still in flight gets one resend interval to land before it is asked for.
			auto& askAt = m_PeerRecordRequestAtMs.try_emplace(peer, nowMs + c_NetPeerVoteResendMs).first->second;
			if (nowMs < askAt) continue;
			auto request = PeerFrameMessage(NetHostMigrationMessageType::PeerReceipt);
			request.preparedFrame = UINT64_MAX; request.boundary = c_NetPeerRecordRequest; request.frame = frames.front();
			for (const uint64_t frame: frames) PutFrame(request.bytes, frame);
			SendPeerFrameMessage(std::move(request), NetTransportLane::ControlReliable, peer);
			++m_PeerRecordRequests;
			const auto stats = m_Stats.peers.find(peer);
			askAt = nowMs + c_NetPeerVoteResendMs + (stats == m_Stats.peers.end() ? 0 : stats->second.pingMs);
			DiagnosticLine() << "[net-peer-record-request] local=" << static_cast<int>(m_Config.localPeerId) << " peer=" << static_cast<int>(peer)
			    << " first=" << frames.front() << " last=" << frames.back() << " frames=" << frames.size() << " now_ms=" << nowMs << std::endl;
		}
	}

	void NetLockstepCoordinator::AnswerPeerRecordRequest(const NetHostMigrationMessage& request) {
		if (request.bytes.empty() || request.bytes.size() % 8 != 0 || request.bytes.size() / 8 > c_NetPeerRecordBatch) return;
		auto answer = PeerFrameMessage(NetHostMigrationMessageType::PeerReceipt);
		answer.preparedFrame = UINT64_MAX; answer.boundary = c_NetPeerRecordAnswer; answer.frame = request.frame;
		uint64_t floor = m_MigrationHistory.empty() ? m_Stats.nextFrame : m_MigrationHistory.begin()->first;
		if (!m_PeerReplayAppliedPrefixes.empty() && m_PeerReplayAppliedPrefixes.begin()->first < m_Config.startFrame) floor = std::min(floor, m_PeerReplayAppliedPrefixes.begin()->first);
		answer.completeFrom = floor;
		size_t offset = 0;
		for (uint64_t frame = 0; TakeFrame(request.bytes, offset, frame);) {
			if (!m_LastCompletedSimulationTick || frame > *m_LastCompletedSimulationTick) continue;
			const auto applied = PeerAppliedFramePrefix(frame);
			if (applied == NetHash32{}) continue;
			PutFrame(answer.bytes, frame);
			answer.bytes.insert(answer.bytes.end(), applied.begin(), applied.end());
		}
		SendPeerFrameMessage(std::move(answer), NetTransportLane::ControlReliable, request.senderPeerId);
	}

	void NetLockstepCoordinator::RecheckDeferredChecksums() {
		for (const uint64_t frame: std::vector<uint64_t>(m_DeferredChecksumFrames.begin(), m_DeferredChecksumFrames.end())) {
			if (!m_LocalChecksums.contains(frame) || !m_RemoteChecksums.contains(frame)) { m_DeferredChecksumFrames.erase(frame); continue; }
			CompareChecksums(frame);
		}
	}

	bool NetLockstepCoordinator::HoldsOtherContinuation(uint64_t frame) const {
		const uint32_t members = PeerFrameMembersAt(frame);
		return std::any_of(m_PeerOtherContinuations.begin(), m_PeerOtherContinuations.end(), [&](const auto& other) { return (members & SeatBit(other.first)) != 0; });
	}

	void NetLockstepCoordinator::CheckPeerFrameTie(uint64_t frame, uint32_t members, uint64_t nowMs) {
		// The groups name a split by the last frame the whole group played, so every check-in of one split meets one decision.
		AskPeerFrameTie(PeerSplitEpoch(frame), frame, members, nowMs);
	}

	void NetLockstepCoordinator::AskPeerFrameTie(uint64_t split, uint64_t frame, uint32_t members, uint64_t nowMs) {
		// Under the details lever, a split this seat does not ask about says why, once per split.
		const auto unasked = [&](const char* why) {
			if (ReturnInputDetails() && m_TieUnaskedNamed != split) {
				m_TieUnaskedNamed = split;
				DiagnosticLine() << "[net-peer-tie-unasked] local=" << unsigned(m_Config.localPeerId) << " frame=" << frame << " split=" << split << " members=" << members
				    << " why=" << why << " retained=" << PeerRetainedSeatsAt(frame) << " held=" << PeerHeldSeatsAt(frame) << " group=" << PeerFrameMembersAt(frame)
				    << " next=" << m_Stats.nextFrame << " now_ms=" << nowMs << std::endl;
			}
		};
		if (m_Config.frameTieReferee && !PeerTieRefereed(frame)) return;
		if (m_PeerFrameTieWinners.contains(split)) { unasked("decided"); return; }
		NetFrameTieRequest request;
		request.generation = m_Config.migrationGeneration; request.roundId = m_Config.matchConfig.roundId;
		request.frame = split; request.configHash = m_RoundConfigHash; request.host = GetHostPeerId();
		// Name every retained owner, but only the seats actually playing in this group. Held owners are not invented voters
		// to make the groups equal: the directory arbitrates a split of any size.
		uint32_t voting = members;
		uint32_t voters = PeerVotingOwners(voting, frame);
		// A seat in its own return's neutral interval owes no input there yet plays on: its half asks as a History-scope owner, or
		// the other half's ruling never reaches it.
		if (voters == 0 && IsSeatReclaimGap(m_Config.localPeerId, frame) && (members & SeatBit(m_Config.localPeerId)) != 0) {
			voting = members;
			voters = PeerVotingOwners(voting, frame, PeerVotingScope::History);
		}
		if (voters == 0 || voting == voters) { unasked(voters == 0 ? "no voters" : "the group votes alone"); return; }
		request.queryOnly = (voting & SeatBit(m_Config.localPeerId)) == 0;
		// The directory decides at the first group to check in, so a group asks only once its split has settled - the bound and its
		// longest round trip after its first hold - with the seats it still plays with: a seat cut from everyone holds them a frame or two
		// apart and would otherwise ask, as a larger half, for seats already gone from it. Until then it queries; nobody waits on it.
		if (!request.queryOnly && m_Config.frameTieReferee) {
			uint64_t roundTripMs = 0;
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if (peer != m_Config.localPeerId) roundTripMs = std::max<uint64_t>(roundTripMs, m_Stats.peers[peer].pingMs);
			const uint64_t settleAt = m_PeerTieSettles.try_emplace(split, nowMs + static_cast<uint64_t>(std::ceil(m_Config.slowPlayerBoundTicks * m_Config.simTickMs)) + roundTripMs, frame).first->second.first;
			if (nowMs < settleAt) request.queryOnly = true;
		}
		// A group smaller than half of the seats that played together before the split asks the referee to rule for it only after the
		// referee's tie deadline: the larger half's question rules first, so the seat with the problem never decides for the others.
		// Meanwhile it queries and plays on; it asks for itself only if no larger half did.
		bool minorityAsks = false;
		if (const uint32_t whole = PeerFrameMembersAt(split); !request.queryOnly && 2 * std::popcount(members & whole) < std::popcount(whole)) {
			const auto& ask = m_PeerMinorityTieAsks.try_emplace(split, nowMs + c_NetMinorityTieGraceMs, voting).first->second;
			if (nowMs < ask.first) request.queryOnly = true; else minorityAsks = true;
		}
		// The halves compare and decide in History scope: a returner still in its neutral interval on one half is an owner on both.
		uint32_t named = members;
		uint32_t owners = PeerVotingOwners(named, frame, PeerVotingScope::History);
		if (owners == 0) owners = voters;
		uint32_t retained = 0;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer)
			if (!m_RemovedPeers.contains(peer) && !IsSeatReleased(peer)) retained |= SeatBit(peer);
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if ((retained & SeatBit(peer)) == 0) continue;
			request.owners.push_back(peer);
			if ((voting & SeatBit(peer)) != 0) request.members.push_back(peer);
		}
		if (request.members.empty() || request.members.size() >= request.owners.size()) { unasked("no other owner"); return; }
		// A seat that heard the other half play on keeps asking what was decided: the referee holds one request per seat, and
		// checking in again would replace the question and lose its answer. A settled group replaces only its own unsettled query.
		const auto settled = m_PeerTieSettles.find(split);
		const bool settledAsks = settled != m_PeerTieSettles.end() && settled->second.first == 0 && !request.queryOnly && m_PeerFrameTie &&
		    std::find(m_PeerFrameTie->members.begin(), m_PeerFrameTie->members.end(), m_Config.localPeerId) != m_PeerFrameTie->members.end();
		if (m_Config.frameTieReferee && m_PeerFrameTie && m_PeerFrameTie->queryOnly && !minorityAsks && !settledAsks) {
			auto asked = *m_PeerFrameTie;
			asked.members = request.members; asked.queryOnly = request.queryOnly;
			if (asked == request) request = *m_PeerFrameTie;
		}
		if (!m_PeerFrameTie || *m_PeerFrameTie != request) {
			if (ReturnInputDetails()) DiagnosticLine() << "[net-peer-tie-question] local=" << unsigned(m_Config.localPeerId)
			    << " frame=" << frame << " split=" << split << " members=" << voting << " owners=" << owners << " query_only=" << request.queryOnly
			    << " generation=" << request.generation << " host=" << request.host << " superseding=" << m_CertifiedSupersedingGeneration
			    << " next=" << m_Stats.nextFrame << " displayed=" << m_LastCompletedSimulationTick.value_or(0) << " whole=" << (PeerRetainedSeatsAt(frame) & ~PeerHeldSeatsAt(frame))
			    << " renewal=" << (m_PeerSplitRenewals.upper_bound(frame) == m_PeerSplitRenewals.begin() ? 0 : std::prev(m_PeerSplitRenewals.upper_bound(frame))->first)
			    << " now_ms=" << nowMs << std::endl;
			m_PeerFrameTie = request; m_PeerFrameTieSinceMs = nowMs;
		}
		// Preserve the pre-hold electorate. Once the local hold is installed its absent seats no longer vote, but the outstanding
		// directory question still refers to the original split and must keep being polled while the survivors play.
		auto [question, inserted] = m_PeerSplitArbitrations.try_emplace(split, PeerSplitArbitration{request, owners, {}});
		if (question->second.asked == 0 && !request.queryOnly) question->second.asked = voting;
		// Poll the current request for this split, including a reader's query of the other half. Retain the original
		// electorate and installed hold boundary: changing the request must neither displace its answer nor rename history.
		if (!inserted) question->second.request = request;
		PollPeerFrameTies(nowMs);
	}

	void NetLockstepCoordinator::PollPeerFrameTies(uint64_t nowMs) {
		// A split whose group has settled asks for the seats it plays with now.
		for (auto settle = m_PeerTieSettles.begin(); settle != m_PeerTieSettles.end();) {
			const uint64_t split = settle->first;
			if (m_PeerFrameTieWinners.contains(split) || !m_PeerSplitArbitrations.contains(split)) { settle = m_PeerTieSettles.erase(settle); continue; }
			if (settle->second.first == 0 || nowMs < settle->second.first || !IsRunning() || IsLocalPrivateReaderAt(m_Stats.nextFrame)) { ++settle; continue; }
			settle->second.first = 0;
			// The owners are the split's, on the frame before its hold, where the seats it held still voted; the members are the seats
			// still played with now.
			const uint64_t asked = settle->second.second;
			AskPeerFrameTie(split, asked > m_Config.startFrame ? asked - 1 : asked, PeerFrameMembersAt(m_Stats.nextFrame), nowMs);
			return;
		}
		// A smaller group whose grace has passed with its split still open asks for itself.
		for (auto ask = m_PeerMinorityTieAsks.begin(); ask != m_PeerMinorityTieAsks.end();) {
			const auto arbitration = m_PeerSplitArbitrations.find(ask->first);
			if (arbitration == m_PeerSplitArbitrations.end() || m_PeerFrameTieWinners.contains(ask->first)) { ask = m_PeerMinorityTieAsks.erase(ask); continue; }
			if (nowMs >= ask->second.first && arbitration->second.request.queryOnly) {
				arbitration->second.request.queryOnly = false;
				if (arbitration->second.asked == 0) arbitration->second.asked = ask->second.second;
				if (ReturnInputDetails()) DiagnosticLine() << "[net-peer-tie-question] local=" << unsigned(m_Config.localPeerId)
				    << " split=" << ask->first << " minority_asks=1 members=" << ask->second.second << " now_ms=" << nowMs << std::endl;
			}
			++ask;
		}
		for (const auto& [split, arbitration]: m_PeerSplitArbitrations) {
			const auto& request = arbitration.request;
			const uint32_t owners = arbitration.owners;
			if (!m_PeerFrameTieWinners.contains(split)) {
				if (!m_Config.frameTieReferee) continue;
				const auto reply = m_Config.frameTieReferee(request);
				if (reply.state != NetFrameTieReply::State::Decided) continue;
				uint32_t winner = 0;
				bool valid = true;
				for (uint16_t peer: reply.members) {
					if (peer == 0 || peer > m_Config.peerCount || (winner & SeatBit(static_cast<uint8_t>(peer))) != 0 ||
					    std::find(request.owners.begin(), request.owners.end(), peer) == request.owners.end()) { valid = false; break; }
					winner |= SeatBit(static_cast<uint8_t>(peer));
				}
				// Keep the original voting seats even after the local hold changes the live electorate.
				if (!valid || winner == 0 || std::popcount(winner) >= static_cast<int>(request.owners.size()) || (winner & owners) == 0) continue;
				m_PeerFrameTieWinners.emplace(split, winner & owners);
			}
			const uint32_t winner = m_PeerFrameTieWinners.at(split), local = SeatBit(m_Config.localPeerId);
			// A reader that never continued its own branch can still consume the committed tail. Only a live losing branch
			// has history to replace, and it enters the existing private-image return without stopping any winning survivor.
			if (!arbitration.holdFrame || m_Stats.nextFrame <= *arbitration.holdFrame) continue;
			// Later local holds can shrink this continuation before the heal. Its first hold's membership may still name a
			// peer that never joined its history; judge the actual completed continuation, as the history exchange does.
			const uint32_t playing = PeerFrameMembersAt(m_LastCompletedSimulationTick.value_or(*arbitration.holdFrame)) & owners;
			// A ruling every winner of which plays in this seat's group past the split's hold is taken; a later split here asks a question
			// of its own. A ruling that names other winners is judged afresh.
			if (const auto settled = m_PeerSplitsSettled.find(split); settled != m_PeerSplitsSettled.end() && settled->second == winner) continue;
			if (m_LastCompletedSimulationTick && *m_LastCompletedSimulationTick >= *arbitration.holdFrame && (winner & ~playing) == 0) {
				m_PeerSplitsSettled[split] = winner;
				continue;
			}
			// A decision naming members of two continuations is the question one of them asked: a seat held while it still counted a
			// seat of this group asks with that seat named. A seat whose group plays some it names and holds others, and did not ask
			// that question itself, lost it.
			uint32_t other = owners & ~playing, elsewhere = 0;
			for (const auto& [peer, since]: m_PeerOtherContinuations) elsewhere |= SeatBit(peer);
			other |= elsewhere;
			// A ruling that names only seats of this group as it stood at its own hold is this group's, whatever seat it held since: a seat
			// held later for a fault of its own never played the other continuation.
			const uint32_t atHold = PeerFrameMembersAt(*arbitration.holdFrame) & owners;
			uint8_t action = 0;
			uint32_t left = 0;
			const bool ours = (winner & local) != 0 && (atHold & local) != 0 && (winner & ~atHold) == 0 && (winner & elsewhere) == 0;
			const bool askerElsewhere = (winner & other) != 0 && (winner & playing) != 0 && arbitration.asked != winner;
			if (!ours && ((winner & local) == 0 || askerElsewhere) && (playing & local) != 0 && ((playing & winner) == 0 || askerElsewhere)) {
				// A ruling for a continuation every player of which has since left names no machine to return to: this one plays on.
				for (uint8_t peer: m_AnnouncedLeavers) left |= SeatBit(peer);
				// A winner already returning into this continuation through its image has left its own.
				for (const auto& [peer, link]: m_ReturningLinks) left |= SeatBit(peer);
				action = (winner & ~playing) != 0 && (winner & ~playing & ~left) == 0 ? 2 : 1;
			}
			// One seat decides for the continuation from what it knows, and every other seat of it takes that decision, so a ruling
			// never stops some of its seats while the others play on.
			const uint32_t group = PeerFrameMembersAt(m_LastCompletedSimulationTick.value_or(*arbitration.holdFrame));
			if ((group & local) == 0) continue;
			const auto decided = m_PeerSplitDecisions.find(split);
			const uint8_t decider = PeerSplitDecider(group);
			// A decision holds only for the ruling it took and the continuation it was taken in.
			const auto* decision = decided == m_PeerSplitDecisions.end() || !decided->second.contains(decider) ? nullptr : &decided->second.at(decider);
			if (decision && decision->winners == winner && (decision->group & local) != 0) action = decision->action;
			else if (decider != m_Config.localPeerId) continue;
			else if (action != 0) AnnouncePeerSplitDecision(split, winner, group, action, nowMs);
			if (action == 2) {
				if (m_PeerSplitsStanding.insert(split).second)
					DiagnosticLine() << "[net-split-arbitration] local=" << static_cast<int>(m_Config.localPeerId) << " split=" << split << " frame=" << m_Stats.nextFrame
					    << " winners=" << winner << " left=" << left << " decider=" << static_cast<int>(decider) << " now_ms=" << nowMs << " action=stand" << std::endl;
				continue;
			}
			if (action == 1) {
				// Preparation may lead the displayed world by several ticks. Finish any granted tick, then discard that
				// unplayed preparation: the private image replaces this branch at its actual completed boundary.
				if (m_GrantedSimulationTick) continue;
				// The decision served the continuation this seat now leaves; the one it returns into has its own.
				m_PeerSplitDecisions.erase(split); m_PeerSplitDecisionsSent.erase(split);
				m_Stats.nextFrame = GetResumeFrame();
				m_ReadyFrames.clear();
				// This branch's world parted from the ruling half's after the split's last common frame, so it returns through the
				// image, never by replaying the ruling half's frames over its own.
				m_LocalSeatHeld = true; m_LocalHoldFrame = std::min<uint64_t>(m_Stats.nextFrame, split + 1);
				m_State = NetLockstepState::Stopped;
				m_Stats.timeoutReason = m_Config.frameTieReferee ? "PeerHeld:the directory selected the other split; returning through a private image" :
				    "PeerHeld:the healed histories selected the other split; returning through a private image";
				DiagnosticLine() << "[net-split-arbitration] local=" << static_cast<int>(m_Config.localPeerId)
				    << " split=" << split << " frame=" << m_Stats.nextFrame << " winners=" << winner << " decider=" << static_cast<int>(decider) << " now_ms=" << nowMs << " action=image_return" << std::endl;
				return;
			}
		}
	}

	uint8_t NetLockstepCoordinator::PeerSplitDecider(uint32_t group) const {
		const uint8_t host = GetHostPeerId();
		if ((group & SeatBit(host)) != 0) return host;
		return group == 0 ? 0 : static_cast<uint8_t>(std::countr_zero(group) + 1);
	}

	void NetLockstepCoordinator::AnnouncePeerSplitDecision(uint64_t split, uint32_t winners, uint32_t group, uint8_t action, uint64_t nowMs) {
		auto notice = PeerFrameMessage(NetHostMigrationMessageType::PeerReceipt);
		notice.preparedFrame = UINT64_MAX; notice.boundary = c_NetPeerSplitDecision; notice.frame = split; notice.completeFrom = action; notice.connectedMask = group;
		for (uint8_t seat = 1; seat <= m_Config.peerCount; ++seat) if ((winners & SeatBit(seat)) != 0) notice.members.push_back(seat);
		m_PeerSplitDecisions[split][m_Config.localPeerId] = {action, winners, group};
		// A seat alone in its continuation has no seat to tell.
		if ((group & ~SeatBit(m_Config.localPeerId)) == 0) return;
		m_PeerSplitDecisionsSent[split] = {notice, nowMs + c_NetSeatDisconnectSilenceMs};
		SendPeerFrameMessage(std::move(notice), NetTransportLane::ControlReliable);
	}

	uint32_t NetLockstepCoordinator::DirectPeerWinner(uint32_t first, uint32_t second) const {
		uint32_t humans = 0;
		for (const auto& player: m_Config.matchConfig.players)
			if (!player.cpu && !(m_Config.matchConfig.dedicated && player.peerId == m_Config.matchConfig.hostPeerId)) humans |= SeatBit(player.peerId);
		const auto firstHumans = std::popcount(first & humans), secondHumans = std::popcount(second & humans);
		if (firstHumans != secondHumans) return firstHumans > secondHumans ? first : second;
		const uint32_t originalHost = SeatBit(m_Config.matchConfig.hostPeerId);
		if ((first & originalHost) != (second & originalHost)) return (first & originalHost) ? first : second;
		return std::countr_zero(first) < std::countr_zero(second) ? first : second;
	}

	void NetLockstepCoordinator::DerivePeerCommonSplit(uint8_t senderPeer, uint64_t remoteSplit, uint64_t remoteBoundary, uint64_t nowMs) {
		const uint32_t sender = SeatBit(senderPeer);
		for (const auto& [split, arbitration]: m_PeerSplitArbitrations) {
			if (!arbitration.holdFrame || *arbitration.holdFrame <= m_Config.startFrame || arbitration.derived) continue;
			const uint64_t hold = *arbitration.holdFrame;
			// This seat's hold that parted it from the sender, and the sender's own hold, part the same two histories - unless the halves
			// parted earlier, holding a third seat at different frames: their last groups before those holds then differ, and the earlier
			// of the two names it. A group either half formed after the other was held names nothing both played.
			if ((PeerFrameMembersAt(hold - 1) & sender) == 0 || (PeerFrameMembersAt(hold) & sender) != 0) continue;
			uint64_t parted = std::min(hold, remoteBoundary);
			const uint64_t own = split > m_Config.startFrame ? split : 0, other = remoteSplit > m_Config.startFrame ? remoteSplit : 0;
			if (own != other && std::max(own, other) < parted) parted = own == 0 ? other : other == 0 ? own : std::min(own, other);
			const uint64_t common = parted - 1;
			if (common < m_Config.startFrame || m_PeerSplitArbitrations.contains(common)) return;
			uint32_t self = SeatBit(m_Config.localPeerId);
			const uint32_t owners = PeerVotingOwners(self, common, PeerVotingScope::History);
			const uint32_t playing = PeerFrameMembersAt(*m_LastCompletedSimulationTick) & owners;
			if (owners == 0 || (playing & SeatBit(m_Config.localPeerId)) == 0 || (playing & sender) != 0) return;
			DiagnosticLine() << "[net-split-common] local=" << static_cast<int>(m_Config.localPeerId) << " peer=" << static_cast<int>(senderPeer)
			    << " local_split=" << split << " remote_split=" << remoteSplit << " local_hold=" << hold << " remote_hold=" << remoteBoundary
			    << " common=" << common << " owners=" << owners << " playing=" << playing << std::endl;
			if (m_Config.frameTieReferee) AskPeerFrameTie(common, common, playing, nowMs);
			else {
				NetFrameTieRequest request;
				request.generation = m_Config.migrationGeneration; request.roundId = m_Config.matchConfig.roundId;
				request.frame = common; request.configHash = m_RoundConfigHash; request.host = GetHostPeerId();
				m_PeerSplitArbitrations.try_emplace(common, PeerSplitArbitration{request, owners, {}});
			}
			if (const auto derived = m_PeerSplitArbitrations.find(common); derived != m_PeerSplitArbitrations.end()) {
				derived->second.holdFrame = hold; derived->second.derived = true;
				derived->second.historiesSeen |= sender;
			}
			return;
		}
	}

	void NetLockstepCoordinator::CheckDirectPeerHistory(const NetHostMigrationMessage& message, uint64_t nowMs) {
		if (!m_LastCompletedSimulationTick || message.members.empty() ||
		    message.connectedMask == 0 ||
		    (message.connectedMask & SeatBit(message.senderPeerId)) == 0 || message.bytes.size() != 8 + NetHash32{}.size()) return;
		// Halves that named the split from different later certificates meet at the last frame both applied: the earlier hold less one.
		// The sender rotates its questions; only when a whole rotation names none of this seat's is the earliest of them the common one.
		if (message.boundary != 0 && message.appliedFrame >= message.boundary) {
			auto& heard = m_PeerQuestionsHeard[message.senderPeerId];
			auto& question = heard[message.frame];
			question.boundary = message.boundary; ++question.times;
			while (heard.size() > NetMatchConfigUtil::c_MaxPeerCount * 2) heard.erase(heard.begin());
			const bool named = std::any_of(heard.begin(), heard.end(), [&](const auto& asked) { return m_PeerSplitArbitrations.contains(asked.first); });
			const bool rotated = std::any_of(heard.begin(), heard.end(), [](const auto& asked) { return asked.second.times >= 2; });
			if (!named && rotated) {
				const auto earliest = std::min_element(heard.begin(), heard.end(), [](const auto& a, const auto& b) { return a.second.boundary < b.second.boundary; });
				DerivePeerCommonSplit(message.senderPeerId, earliest->first, earliest->second.boundary, nowMs);
			}
		}
		if (m_Config.frameTieReferee) return;
		// A reader of this history has no competing continuation, whatever its current link or roster revision.
		bool sameHistory = false;
		if (m_MigrationHistory.contains(message.appliedFrame)) {
			const auto prefix = PeerAppliedFramePrefix(message.appliedFrame);
			sameHistory = std::equal(prefix.begin(), prefix.end(), message.bytes.begin() + 8);
		}
		for (auto& [split, arbitration]: m_PeerSplitArbitrations) {
			uint32_t remoteOwners = 0;
			for (uint8_t owner: message.members) remoteOwners |= SeatBit(owner);
			// The other half may already play a seat that returned after this split; its owners and acknowledgements are this split's.
			if (split != message.frame || remoteOwners != arbitration.owners || ((message.voterMask | message.offset) & ~arbitration.owners) != 0) continue;
			const uint32_t remotePlaying = message.connectedMask & arbitration.owners;
			arbitration.historiesSeen |= SeatBit(message.senderPeerId) << (message.appliedFrame >= message.boundary ? 0 : c_DirectReaderReceiptShift);
			if (arbitration.offeredHold && *m_LastCompletedSimulationTick < arbitration.offeredHold->frame &&
			    message.appliedFrame >= message.boundary && message.boundary == arbitration.offeredHold->frame &&
			    message.connectedMask == arbitration.offeredHold->connectedMask && ((message.voterMask | message.offset) & SeatBit(m_Config.localPeerId))) {
				// The heal has one continuation. This reader committed nothing past its cutoff and adopts that standing hold.
				arbitration.readerHoldFrame = arbitration.offeredHold->frame;
				const auto certificate = std::exchange(arbitration.offeredHold, {});
				InstallPeerBridge(*certificate, nowMs);
				return;
			}
			if (sameHistory || !arbitration.holdFrame || *m_LastCompletedSimulationTick < *arbitration.holdFrame || message.appliedFrame < message.boundary) continue;
			const uint32_t playing = PeerFrameMembersAt(*m_LastCompletedSimulationTick) & arbitration.owners;
			if ((playing & SeatBit(m_Config.localPeerId)) == 0 || remotePlaying == 0 || (playing & remotePlaying) != 0) continue;
			// Incoming traffic can flow throughout a one-way cut. Only the sender's authenticated receipt of this branch's
			// committed history completes the exchange at the heal; hearing its continuation alone cannot stop this one.
			if ((message.voterMask & SeatBit(m_Config.localPeerId)) == 0) continue;
			const uint32_t chosen = DirectPeerWinner(playing, remotePlaying);
			auto [decision, inserted] = m_PeerFrameTieWinners.try_emplace(split, chosen);
			if (!inserted) decision->second = DirectPeerWinner(decision->second, chosen);
			DiagnosticLine() << "[net-direct-history] local=" << static_cast<int>(m_Config.localPeerId) << " peer=" << static_cast<int>(message.senderPeerId)
			    << " split=" << split << " local_hold=" << *arbitration.holdFrame << " remote_hold=" << message.boundary
			    << " remote_applied=" << message.appliedFrame << " roster_revision=" << message.rosterRevision << " winners=" << decision->second << std::endl;
		}
		PollPeerFrameTies(nowMs);
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
		if (!UsesPeerFrameGroups() || (!IsRunning() && m_State != NetLockstepState::WaitingForStart)) return;
		if (IsRunning()) PollPeerFrameTies(nowMs);
		if (!IsRunning() && m_State != NetLockstepState::WaitingForStart) return;
		// No input clock starts while a cold mesh is still establishing a direct route.
		if (m_State == NetLockstepState::WaitingForStart && m_Config.migrationTransportFactory && !m_Config.joinsRunningRound && !m_Config.continuesMatch &&
		    std::any_of(m_RemotePeerIds.begin(), m_RemotePeerIds.end(), [&](uint8_t peer) { return !HasPeerFrameRoute(peer); })) return;
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
			heartbeat.connectedMask = PeerFrameMembersAt(heartbeat.appliedFrame);
			const auto* roster = m_RosterReader ? m_RosterReader() : nullptr;
			heartbeat.rosterRevision = roster ? roster->revision : m_Config.matchConfig.seatRosterRevision;
			// A new local hold must not hide the older split still named by another continuation. Rotate the retained
			// questions on the existing heartbeat, so nested holds can complete the same bilateral history exchange.
			if (!m_PeerSplitArbitrations.empty()) {
				auto split = m_PeerHistoryQuestion ? m_PeerSplitArbitrations.upper_bound(*m_PeerHistoryQuestion) : m_PeerSplitArbitrations.begin();
				if (split == m_PeerSplitArbitrations.end()) split = m_PeerSplitArbitrations.begin();
				for (size_t remaining = m_PeerSplitArbitrations.size(); remaining; --remaining) {
					const auto boundary = split->second.holdFrame ? split->second.holdFrame :
					    split->second.offeredHold ? std::optional(split->second.offeredHold->frame) : std::nullopt;
					if (!boundary) { if (++split == m_PeerSplitArbitrations.end()) split = m_PeerSplitArbitrations.begin(); continue; }
					heartbeat.frame = split->first; heartbeat.boundary = *boundary;
					heartbeat.voterMask = split->second.historiesSeen & split->second.owners;
					heartbeat.offset = (split->second.historiesSeen >> c_DirectReaderReceiptShift) & split->second.owners;
					for (uint8_t seat = 1; seat <= m_Config.peerCount; ++seat)
						if (split->second.owners & SeatBit(seat)) heartbeat.members.push_back(seat);
					m_PeerHistoryQuestion = split->first;
					break;
				}
			}
			// Restored survivors can receive queued host traffic on a later local
			// clock. Advertise eligibility before anyone fixes an immutable vote;
			// this report grants no authority and does not replace voter validation.
			if (m_Config.localPeerId != GetHostPeerId() && m_LastCompletedSimulationTick &&
			    nowMs >= m_AuthorityLastHeardMs && nowMs - m_AuthorityLastHeardMs >= c_NetHostLossSilenceMs &&
			    m_Config.migrationGeneration != UINT64_MAX)
				heartbeat.completeFrom = m_Config.migrationGeneration + 1;
			PutSize(heartbeat.bytes, m_Stats.peers[m_Config.localPeerId].pingMs); PutSize(heartbeat.bytes, m_Stats.peers[m_Config.localPeerId].jitterMs);
			// Progress alone cannot distinguish a private reader from a live split with different history.
			const auto appliedPrefix = PeerAppliedFramePrefix(heartbeat.appliedFrame);
			heartbeat.bytes.insert(heartbeat.bytes.end(), appliedPrefix.begin(), appliedPrefix.end());
			heartbeat.totalBytes = static_cast<uint32_t>(heartbeat.bytes.size());
			// A newer progress report replaces an older one without waiting on the ordered stream.
			SendPeerFrameMessage(std::move(heartbeat), NetTransportLane::InputUnreliable);
			// The last receipt must survive a lost publication even when no new input follows it.
			for (const auto& [owner, receipts]: m_PeerInputReceipts) {
				const auto accepted = receipts.find(m_Config.localPeerId);
				if (owner == m_Config.localPeerId || accepted == receipts.end() || accepted->second == 0) continue;
				auto receipt = PeerFrameMessage(NetHostMigrationMessageType::PeerReceipt);
				receipt.successorPeerId = owner; receipt.frame = accepted->second;
				SendPeerFrameMessage(std::move(receipt), NetTransportLane::InputUnreliable);
			}
		}
		if (m_State == NetLockstepState::WaitingForStart) {
			if (!m_RequirePublishedStart && !m_AgreedStartApplied && AllRemoteStartsReceived()) {
				m_State = NetLockstepState::Running;
				auto timing = std::move(m_PreStartTiming); m_PreStartTiming.clear();
				for (const auto& [decision, source]: timing) HandleTiming(decision, nowMs, source);
				m_WaitingFrame = std::numeric_limits<uint64_t>::max();
			}
			return;
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
					if (HoldEmptiesTheRound(vote, true)) continue;
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
						    << " frame=" << vote.frame << " ai_frame=" << vote.frame
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
				const bool standingHold = IsHoldDecision(key.second) && std::any_of(m_PeerBridges.begin(), m_PeerBridges.end(), [&](const auto& entry) {
					return (key.second & SeatBit(entry.first)) != 0 && entry.second.committedHold && entry.second.fromFrame == key.first && !entry.second.returnFrame;
				});
				// A suspended owner still needs its standing hold when it wakes. Its input horizon is not a lifetime
				// for the seat's authority record; otherwise it can resume old human input before hearing its hold.
				if (key.first + NetLockstepCodec::c_MaxFutureFrameSkew < m_Stats.nextFrame && !standingHold) continue;
				SendPeerFrameMessage(certificate);
			}
			std::erase_if(m_PeerSplitDecisionsSent, [&](const auto& sent) { return nowMs >= sent.second.second; });
			for (const auto& [split, sent]: m_PeerSplitDecisionsSent) SendPeerFrameMessage(sent.first, NetTransportLane::ControlReliable);
		}
		RepairPeerReceiptGaps(nowMs);
		RequestPeerRecords(nowMs);
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
		for (auto certificate = m_PeerBridgeCertificates.begin(); certificate != m_PeerBridgeCertificates.end() && certificate->first.first < kept;) {
			const bool standingHold = IsHoldDecision(certificate->first.second) && std::any_of(m_PeerBridges.begin(), m_PeerBridges.end(), [&](const auto& entry) {
				return (certificate->first.second & SeatBit(entry.first)) != 0 && entry.second.committedHold && entry.second.fromFrame == certificate->first.first && !entry.second.returnFrame;
			});
			certificate = standingHold ? std::next(certificate) : m_PeerBridgeCertificates.erase(certificate);
		}
		m_PeerInputSentAtMs.erase(m_PeerInputSentAtMs.begin(), m_PeerInputSentAtMs.lower_bound(kept));
		m_PeerPreparedPrefixes.erase(m_PeerPreparedPrefixes.begin(), m_PeerPreparedPrefixes.lower_bound(kept));
		m_PeerFrameWitnesses.erase(m_PeerFrameWitnesses.begin(), m_PeerFrameWitnesses.lower_bound(kept));
		for (auto& [peer, records]: m_PeerRecordMatches) records.erase(records.begin(), records.lower_bound(kept));
		RecheckDeferredChecksums();
		m_PeerTailVotes.erase(m_PeerTailVotes.begin(), m_PeerTailVotes.lower_bound(m_Stats.nextFrame));
		// The split in force keeps its name and its decision however long it lasts.
		const uint64_t historyFloor = std::min(kept, PeerSplitEpoch(kept));
		std::erase_if(m_PeerSplitArbitrations, [&](const auto& question) {
			return question.first < historyFloor && m_PeerFrameTieWinners.contains(question.first);
		});
		std::erase_if(m_PeerSplitDecisions, [&](const auto& decision) { return decision.first < historyFloor && !m_PeerSplitArbitrations.contains(decision.first); });
		std::erase_if(m_PeerSplitsSettled, [&](const auto& settled) { return settled.first < historyFloor && !m_PeerSplitArbitrations.contains(settled.first); });
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
			const bool transportMeasured = wire && wire->IsPeerPingMeasured(route);
			transportsMeasured &= transportMeasured;
			if (transportMeasured) {
				const uint32_t transportMs = wire->GetPeerPingMs(route);
				auto& link = m_DelayEstimators[peer];
				link.Observe(nowMs, transportMs, true);
				// Receipt time includes both machines' simulation and catch-up. Only transport samples size a measured link;
				// a machine that cannot feed that runway is handled by the seat's hold decision.
				const uint32_t frames = link.RequiredFrames(m_Config.simTickMs);
				paths.emplace_back(frames, transportMs);
			}
		}
		const size_t needed = std::popcount(m_FrameGroupMembers) - 1;
		m_Stats.peers[m_Config.localPeerId].pingMeasured = transportsMeasured;
		if (needed == 0 || paths.size() < needed) {
			// With no current transport reading, keep the agreed link-derived delay. Missing data cannot prove five settled seconds.
			m_PeerDelayBelowSinceMs.erase(m_Config.localPeerId);
			return;
		}
		std::sort(paths.begin(), paths.end());
		// Every active receiver needs the sender's buffered input. Sizing to
		// only a quorum slowly drains the runway of a healthy, longer path.
		const auto [bufferedFrames, transportMs] = paths.back();
		const uint32_t rtt = transportMs;
		auto& local = m_Stats.peers[m_Config.localPeerId];
		local.pingMs = rtt; local.pingMeasured = transportsMeasured;
		const uint16_t localRequired = static_cast<uint16_t>(std::min<uint32_t>(NetLockstepCodec::c_MaxPeerInputDelayFrames,
		    std::max<uint32_t>(m_Config.matchConfig.inputDelayFrames, bufferedFrames)));
		local.jitterMs = static_cast<uint32_t>(std::ceil(localRequired * m_Config.simTickMs)) - std::min<uint32_t>(rtt, static_cast<uint32_t>(std::ceil(localRequired * m_Config.simTickMs)));
		uint32_t groupFrames = localRequired;
		// Receipts and repaired input also cross the measured links between this sender's readers.
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if (peer == m_Config.localPeerId || (m_FrameGroupMembers & SeatBit(peer)) == 0) continue;
			const auto& link = m_Stats.peers[peer];
			if (link.pingMeasured && link.pingMs != 0)
				groupFrames = std::max<uint32_t>(groupFrames, static_cast<uint32_t>(std::floor((link.pingMs + link.jitterMs) / m_Config.simTickMs)));
		}
		const uint16_t current = InputDelayAt(m_Config.localPeerId, m_Stats.nextFrame);
		const uint16_t required = static_cast<uint16_t>(std::min<uint32_t>(NetLockstepCodec::c_MaxPeerInputDelayFrames, groupFrames));
		if (required > current) {
			m_PeerDelayBelowSinceMs.erase(m_Config.localPeerId);
			(void)ProposePeerInputDelay(m_Config.localPeerId, required, FutureTimingFrame());
		} else if (required < current) {
			const auto [since, inserted] = m_PeerDelayBelowSinceMs.try_emplace(m_Config.localPeerId, nowMs);
			if (nowMs < since->second) since->second = nowMs;
			const uint16_t gap = current - required;
			const uint64_t interval = NetInputDelayEstimator::c_WindowMs;
			const uint16_t delay = gap > 2 ? std::min<uint16_t>(NetLockstepCodec::c_MaxPeerInputDelayFrames, required + std::max<uint16_t>(2, gap / 2))
			                             : current - std::min(current, c_DelayShrinkStep);
			if (nowMs >= since->second + interval && ProposePeerInputDelay(m_Config.localPeerId, delay, FutureTimingFrame())) since->second = nowMs;
		} else m_PeerDelayBelowSinceMs.erase(m_Config.localPeerId);
	}

} // namespace RTE
