#include "NetPeerFrameHelpers.h"

#include "DiagnosticLine.h"
#include "NetAuthCrypto.h"
#include "NetLobbyProtocol.h"
#include "NetProtocol.h"

#include "lz4.h"

#include <cmath>
#include <limits>

namespace RTE {

	using namespace NetPeerFrameDetail;

	bool NetLockstepCoordinator::ValidatePeerBridge(const NetHostMigrationMessage& proposal) const {
		if (proposal.preparedFrame == 3) return ValidatePeerAdmin(proposal);
		if (proposal.members.empty() || proposal.bytes.size() != NetHash32{}.size() + (proposal.preparedFrame == 1 ? 12 * proposal.members.size() : 0) || !PeerGroupHasAuthority(proposal.connectedMask) ||
		    proposal.preparedFrame > 2 || (proposal.connectedMask & SeatBit(m_Config.localPeerId)) == 0) return false;
		if (proposal.preparedFrame == 0 && proposal.frame != m_Stats.nextFrame) return false;
		if (proposal.preparedFrame != 0 && (proposal.frame <= m_Stats.nextFrame || proposal.frame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew)) return false;
		const uint64_t prefixFrame = proposal.preparedFrame != 0 ? proposal.boundary : proposal.frame - 1;
		const NetHash32 prefix = proposal.frame == m_Config.startFrame ? NetHash32{} : proposal.preparedFrame != 0 ? PeerAppliedFramePrefix(prefixFrame) : PeerFramePrefix(prefixFrame);
		if (proposal.frame != m_Config.startFrame && !m_MigrationHistory.contains(prefixFrame)) return false;
		if (!std::equal(prefix.begin(), prefix.end(), proposal.bytes.begin())) return false;
		if (proposal.preparedFrame == 2) return proposal.members.size() == 1 && proposal.successorPeerId == proposal.members.front() &&
		    proposal.members.front() > 0 && proposal.members.front() <= m_Config.peerCount && proposal.completeFrom > 0 && proposal.completeFrom <= NetLockstepCodec::c_MaxInputDelayFrames &&
		    (proposal.connectedMask & SeatBit(proposal.members.front())) != 0;
		uint32_t named = 0;
		for (uint8_t peer: proposal.members) named |= SeatBit(peer);
		const auto sources = m_PeerSourceInputs.find(proposal.frame);
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			const bool present = sources != m_PeerSourceInputs.end() && sources->second.contains(peer);
			if (proposal.preparedFrame == 0 && (named & SeatBit(peer)) != 0 && present) return false;
			if (proposal.preparedFrame == 0 && (proposal.connectedMask & SeatBit(peer)) != 0 && proposal.frame >= EffectiveStartOf(peer) && !present) return false;
			if (proposal.preparedFrame == 1 && (named & SeatBit(peer)) != 0 && (!m_PeerBridges.contains(peer) || m_PeerBridges.at(peer).returnFrame)) return false;
		}
		return proposal.preparedFrame == 1 ? (named & proposal.connectedMask) == named : (named & proposal.connectedMask) == 0;
	}

	bool NetLockstepCoordinator::ProposePeerBridge(uint64_t frame, uint64_t nowMs, bool returningOnly) {
		if (!UsesPeerFrameGroups() || WaitsForPlacement() || frame != m_Stats.nextFrame || m_PeerBridgeCertificates.contains({frame, 0})) return false;
		const auto sources = m_PeerSourceInputs.find(frame);
		if (sources == m_PeerSourceInputs.end() || !sources->second.contains(m_Config.localPeerId)) return false;
		auto proposal = PeerFrameMessage(NetHostMigrationMessageType::PeerBridge);
		proposal.frame = proposal.boundary = frame;
		proposal.connectedMask = 0;
		proposal.preparedFrame = 0;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if (m_RemovedPeers.contains(peer) || IsSeatReleased(peer)) continue;
			const auto bridge = m_PeerBridges.find(peer);
			const bool held = bridge != m_PeerBridges.end() && frame >= bridge->second.fromFrame && (!bridge->second.returnFrame || frame < *bridge->second.returnFrame);
			if (held) continue;
			if (frame < EffectiveStartOf(peer) || sources->second.contains(peer)) proposal.connectedMask |= SeatBit(peer);
			else proposal.members.push_back(peer);
		}
		if (returningOnly && !proposal.members.empty()) return false;
		if (!proposal.members.empty()) CheckPeerFrameTie(frame, proposal.connectedMask, nowMs);
		if (proposal.members.empty()) {
			if (m_FrameGroupMembers == 0 || std::countr_zero(m_FrameGroupMembers) + 1 != m_Config.localPeerId) return false;
			for (const auto& [peer, bridge]: m_PeerBridges) {
				if (bridge.returnFrame || peer == m_Config.localPeerId) continue;
				const uint64_t delay = InputDelayAt(peer, frame);
				const uint64_t heard = LastAuthenticatedTraffic(peer);
				if (nowMs < heard || nowMs - heard > 1000 || !m_PeerAppliedThrough.contains(peer) || m_PeerAppliedThrough.at(peer) + delay < frame) continue;
				proposal.members.push_back(peer); proposal.connectedMask |= SeatBit(peer);
			}
			if (proposal.members.empty()) return false;
			proposal.preparedFrame = 1;
			proposal.boundary = m_LastCompletedSimulationTick.value_or(frame - 1);
			uint64_t lead = m_Config.slowPlayerBoundTicks;
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer)
				lead = std::max<uint64_t>(lead, static_cast<uint64_t>(std::ceil(m_Stats.peers[peer].pingMs / m_Config.simTickMs)) + m_Config.slowPlayerBoundTicks);
			proposal.frame = frame + lead;
		}
		if (proposal.preparedFrame == 0) {
			const uint64_t boundMs = static_cast<uint64_t>(std::ceil(m_Config.slowPlayerBoundTicks * m_Config.simTickMs));
			uint64_t agreementMs = 1;
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if (peer != m_Config.localPeerId && (proposal.connectedMask & SeatBit(peer)) != 0)
				if (const auto path = m_PeerReceiptDelaySamples.find(peer); path != m_PeerReceiptDelaySamples.end()) agreementMs = std::max<uint64_t>(agreementMs, path->second.P95Ms());
			// Votes spend the buffered runway, so a healthy relay round trip fits inside the consumer's bound.
			if (m_PeerPaceStartMs && frame >= m_PeerPaceStartFrame) {
				const double dueMs = *m_PeerPaceStartMs + (frame - m_PeerPaceStartFrame) * m_Config.simTickMs;
				if (nowMs + agreementMs < dueMs + boundMs) return false;
			} else if (m_ConsumerWaitingFrame != frame || nowMs < m_ConsumerWaitStartMs ||
			    nowMs - m_ConsumerWaitStartMs < boundMs - std::min(boundMs, agreementMs)) return false;
		}
		const uint32_t proposers = proposal.preparedFrame == 1 ? m_FrameGroupMembers : proposal.connectedMask;
		if (proposers == 0) return false;
		proposal.successorPeerId = static_cast<uint8_t>(std::countr_zero(proposers) + 1);
		const NetHash32 prefix = frame == m_Config.startFrame ? NetHash32{} : proposal.preparedFrame != 0 ? PeerAppliedFramePrefix(proposal.boundary) : PeerFramePrefix(frame - 1);
		proposal.bytes.assign(prefix.begin(), prefix.end());
		if (proposal.preparedFrame == 1) for (uint8_t peer: proposal.members) {
			const uint32_t delay = InputDelayAt(peer, proposal.frame);
			const uint32_t transit = static_cast<uint32_t>(std::ceil(m_Stats.peers[peer].pingMs / m_Config.simTickMs));
			PutSize(proposal.bytes, delay); PutSize(proposal.bytes, delay + transit + m_Config.slowPlayerBoundTicks);
			PutSize(proposal.bytes, std::max<uint32_t>(1, m_Config.peerIncarnations[peer]));
		}
		proposal.totalBytes = static_cast<uint32_t>(proposal.bytes.size());
		if (!ValidatePeerBridge(proposal)) return false;
		auto& votes = m_PeerBridgeVotes[{proposal.frame, DecisionKind(proposal)}];
		const auto own = votes.find(m_Config.localPeerId);
		// A matching proposal is still pending until its certificate is installed.
		// Reporting it as progress makes AdvanceReadyFrames spin on the same frame
		// and prevents the wire from delivering the other voters' answers.
		if (own != votes.end()) return m_PeerBridgeCertificates.contains({frame, 0});
		proposal.voterMask = SeatBit(m_Config.localPeerId);
		votes.emplace(m_Config.localPeerId, proposal);
		if (proposal.preparedFrame == 0) for (uint8_t peer: proposal.members) m_PeerRejectedInputs[frame] |= SeatBit(peer);
		SendPeerFrameMessage(proposal);
		TryCommitPeerBridge(proposal.frame, nowMs, DecisionKind(proposal));
		return m_PeerBridgeCertificates.contains({frame, 0});
	}

	void NetLockstepCoordinator::TryCommitPeerBridge(uint64_t frame, uint64_t nowMs, uint8_t kind) {
		const auto found = m_PeerBridgeVotes.find({frame, kind});
		if (found == m_PeerBridgeVotes.end() || found->second.empty() || m_PeerBridgeCertificates.contains({frame, kind})) return;
		const auto own = found->second.find(m_Config.localPeerId);
		if (own == found->second.end()) return;
		const auto& proposal = own->second;
		uint32_t required = proposal.connectedMask;
		if (proposal.preparedFrame == 1) for (uint8_t peer: proposal.members) required &= ~SeatBit(peer);
		uint32_t voters = 0;
		std::vector<std::vector<uint8_t>> signedVotes;
		for (const auto& [peer, vote]: found->second) {
			if (!SameBridge(vote, proposal)) continue;
			std::vector<uint8_t> bytes;
			if (!NetHostMigrationCodec::Encode(vote, m_Config.migrationKey, bytes)) return;
			voters |= SeatBit(peer); signedVotes.push_back(std::move(bytes));
		}
		if ((voters & required) != required || (proposal.preparedFrame == 3 ? !PeerAdminCertificateValid(proposal, voters) : !PeerGroupHasAuthority(voters))) return;
		auto certificate = proposal;
		certificate.type = NetHostMigrationMessageType::PeerBridgeCommit;
		certificate.voterMask = voters;
		certificate.bytes = {static_cast<uint8_t>(signedVotes.size())};
		for (const auto& bytes: signedVotes) {
			PutSize(certificate.bytes, static_cast<uint32_t>(bytes.size()));
			certificate.bytes.insert(certificate.bytes.end(), bytes.begin(), bytes.end());
		}
		certificate.totalBytes = static_cast<uint32_t>(certificate.bytes.size());
		InstallPeerBridge(certificate, nowMs);
		SendPeerFrameMessage(std::move(certificate));
	}

	bool NetLockstepCoordinator::DecodePeerBridgeCertificate(const NetHostMigrationMessage& certificate, NetHostMigrationMessage& agreed, uint32_t& voters) const {
		if (certificate.bytes.empty() || certificate.frame < m_Config.startFrame) return false;
		const uint8_t count = certificate.bytes.front();
		if (count == 0 || count > m_Config.peerCount) return false;
		size_t offset = 1;
		voters = 0;
		for (uint8_t n = 0; n < count; ++n) {
			uint32_t size = 0;
			if (!TakeSize(certificate.bytes, offset, size) || offset > certificate.bytes.size() || size > certificate.bytes.size() - offset) return false;
			NetHostMigrationMessage vote;
			if (!NetHostMigrationCodec::Decode({certificate.bytes.begin() + offset, certificate.bytes.begin() + offset + size}, m_Config.migrationKey, vote) ||
			    vote.type != NetHostMigrationMessageType::PeerBridge || vote.sessionId != m_Config.sessionId || vote.roundId != m_Config.matchConfig.roundId ||
			    vote.configHash != m_RoundConfigHash || vote.generation != certificate.generation || vote.frame != certificate.frame ||
			    vote.senderPeerId == 0 || vote.senderPeerId > m_Config.peerCount || (voters & SeatBit(vote.senderPeerId)) != 0 || vote.voterMask != SeatBit(vote.senderPeerId)) return false;
			if (vote.preparedFrame == 3 && vote.appliedFrame < vote.boundary) return false;
			if (n == 0) agreed = vote;
			else if (!SameBridge(agreed, vote)) return false;
			voters |= SeatBit(vote.senderPeerId); offset += size;
		}
		uint32_t required = agreed.connectedMask;
		if (agreed.preparedFrame == 1) for (uint8_t peer: agreed.members) required &= ~SeatBit(peer);
		return offset == certificate.bytes.size() && voters == certificate.voterMask && (voters & required) == required &&
		    agreed.connectedMask == certificate.connectedMask && agreed.members == certificate.members && agreed.preparedFrame == certificate.preparedFrame &&
		    agreed.boundary == certificate.boundary && agreed.completeFrom == certificate.completeFrom && agreed.successorPeerId == certificate.successorPeerId;
	}

	void NetLockstepCoordinator::InstallPeerBridge(const NetHostMigrationMessage& certificate, uint64_t nowMs) {
		if (m_PeerBridgeCertificates.contains({certificate.frame, DecisionKind(certificate)})) return;
		NetHostMigrationMessage agreed; uint32_t voters = 0;
		if (!DecodePeerBridgeCertificate(certificate, agreed, voters) || (agreed.preparedFrame == 3 ? !PeerAdminCertificateValid(agreed, voters) : !PeerGroupHasAuthority(voters))) return;
		if (agreed.preparedFrame == 0 && certificate.frame > m_Stats.nextFrame) { RequestPeerCommittedTail(nowMs); return; }
		if (agreed.preparedFrame != 0 && certificate.frame < m_Stats.nextFrame) return;
		const uint64_t prefixFrame = agreed.preparedFrame != 0 ? agreed.boundary : certificate.frame - 1;
		const NetHash32 prefix = certificate.frame == m_Config.startFrame ? NetHash32{} : agreed.preparedFrame != 0 ? PeerAppliedFramePrefix(prefixFrame) : PeerFramePrefix(prefixFrame);
		if (agreed.bytes.size() != prefix.size() + (agreed.preparedFrame == 1 ? 12 * agreed.members.size() : 0) || !std::equal(prefix.begin(), prefix.end(), agreed.bytes.begin())) return;
		size_t returnOffset = prefix.size();
		std::map<uint8_t, std::tuple<uint16_t, uint64_t, uint32_t>> returnDelays;
		if (agreed.preparedFrame == 1) for (uint8_t peer: agreed.members) {
			uint32_t delay = 0, neutral = 0, incarnation = 0;
			if (!TakeSize(agreed.bytes, returnOffset, delay) || !TakeSize(agreed.bytes, returnOffset, neutral) || !TakeSize(agreed.bytes, returnOffset, incarnation) || incarnation == 0 ||
			    delay > NetLockstepCodec::c_MaxInputDelayFrames || neutral < delay || neutral > NetLockstepCodec::c_MaxFutureFrameSkew || agreed.frame > UINT64_MAX - neutral) return;
			returnDelays[peer] = {static_cast<uint16_t>(delay), agreed.frame + neutral, incarnation};
		}
		m_PeerBridgeCertificates.emplace(PeerDecisionKey{certificate.frame, DecisionKind(certificate)}, certificate);
		if (agreed.preparedFrame == 3) { InstallPeerAdmin(agreed, voters, nowMs); return; }
		if (agreed.preparedFrame == 2) {
			if (agreed.members.size() != 1 || agreed.completeFrom == 0 || agreed.completeFrom > NetLockstepCodec::c_MaxInputDelayFrames) return;
			const uint8_t peer = agreed.members.front();
			m_DelayChanges[peer][agreed.frame] = static_cast<uint16_t>(agreed.completeFrom);
			m_LastDelayResizeMs[peer] = nowMs; ++m_Stats.delayChangesCommitted;
			++m_Stats.peers[peer].delayResizes;
			if (peer == m_Config.localPeerId) PadAheadOfDelayRise();
			DiagnosticLine() << "[net-frame-group] delay peer=" << static_cast<int>(peer) << " frame=" << agreed.frame << " ticks=" << agreed.completeFrom << " voters=" << voters << std::endl;
			return;
		}
		m_FrameGroupChanges[agreed.frame] = agreed.connectedMask;
		if (agreed.preparedFrame == 0) m_FrameGroupMembers = agreed.connectedMask;
		uint32_t owners = 0;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if (!m_RemovedPeers.contains(peer) && !IsSeatReleased(peer)) owners |= SeatBit(peer);
		if (agreed.preparedFrame == 1 && agreed.connectedMask == owners && agreed.frame <= m_Stats.nextFrame) m_PeerAdminFrameMembers = 0;
		for (uint8_t peer: agreed.members) {
			if (agreed.preparedFrame == 1) {
				if (!m_PeerBridges.contains(peer)) continue;
				m_PeerBridges[peer].returnFrame = agreed.frame;
				m_PeerAcceptedThrough[peer] = agreed.frame;
				m_PeerAcceptedAhead[peer].clear();
				m_PeerInputReceipts[peer].clear();
				const uint64_t revision = agreed.frame * 8 + peer;
				const auto [delay, neutral, incarnation] = returnDelays.at(peer);
				m_ReclaimTransactions[peer] = {peer, m_Config.migrationGeneration, revision, incarnation, agreed.frame, delay, neutral};
				m_PeerEffectiveStart[peer] = agreed.frame + delay;
				m_PeerAcceptedThrough[peer] = neutral + 1;
				NoteSeatTransition(peer, agreed.frame, SeatTransition::Back);
			} else {
				PeerBridge bridge;
				bridge.fromFrame = agreed.frame;
				bridge.aiFrame = agreed.frame + static_cast<uint64_t>(std::ceil(c_NetInputContinuityMs / m_Config.simTickMs));
				bridge.members = agreed.connectedMask;
				if (agreed.frame > m_Config.startFrame) {
					NetLockstepReadyFrame previous;
					const auto bytes = m_MigrationHistory.find(agreed.frame - 1);
					if (bytes != m_MigrationHistory.end() && DecodeMigrationFrame(bytes->second, agreed.frame - 1, previous)) bridge.lastInput = FramesForSeat(previous, peer);
				}
				m_PeerBridges[peer] = std::move(bridge);
				m_SeatBridgeSinceMs[peer] = nowMs;
				if (peer == m_Config.localPeerId) m_LocalFrames.erase(agreed.frame);
			}
		}
		DiagnosticLine() << "[net-frame-group] " << (agreed.preparedFrame == 1 ? "return" : "bridge") << " frame=" << agreed.frame
		                 << " members=" << agreed.connectedMask << " voters=" << voters << std::endl;
	}

	void NetLockstepCoordinator::PreparePeerBridgeInputs(uint64_t frame, uint64_t nowMs) {
		if (const auto change = m_FrameGroupChanges.upper_bound(frame); change != m_FrameGroupChanges.begin()) m_FrameGroupMembers = std::prev(change)->second;
		uint32_t owners = 0;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if (!m_RemovedPeers.contains(peer) && !IsSeatReleased(peer)) owners |= SeatBit(peer);
		if (m_FrameGroupMembers == owners) m_PeerAdminFrameMembers = 0;
		for (const auto& [peer, bridge]: m_PeerBridges) {
			if (bridge.returnFrame && frame >= *bridge.returnFrame) {
				m_PeerLeaveFrames.erase(peer); m_AiHeldSeats.erase(peer); m_SeatBridgeSinceMs.erase(peer);
			}
			if (frame < bridge.fromFrame || (bridge.returnFrame && frame >= *bridge.returnFrame)) continue;
			if (peer == m_Config.localPeerId) continue;
			if (frame < bridge.aiFrame) {
				const uint64_t elapsed = static_cast<uint64_t>(std::llround((frame - bridge.fromFrame) * m_Config.simTickMs));
				m_RemoteFrames[frame][peer] = ContinuedInput(bridge.lastInput, elapsed);
				m_RemoteCommands[frame][peer].clear();
				m_RemoteObservations[frame][peer].clear();
				m_RemoteValueObservations[frame][peer].clear();
			} else if (!m_AiHeldSeats.contains(peer)) {
				m_AiHeldSeats[peer] = bridge.aiFrame;
				m_PeerLeaveFrames[peer] = bridge.aiFrame;
				const uint64_t revision = bridge.fromFrame * 8 + peer;
				m_HoldTransactions[peer] = {peer, m_Config.migrationGeneration, revision, std::max<uint32_t>(1, m_Config.peerIncarnations[peer]), bridge.aiFrame};
				NoteSeatTransition(peer, bridge.aiFrame, SeatTransition::Held);
			}
		}
	}

} // namespace RTE
