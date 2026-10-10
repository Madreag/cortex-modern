#include "NetLockstep.h"

#include "DiagnosticLine.h"

#include <cmath>

namespace RTE {

	namespace {
		uint32_t AdminBit(uint8_t peer) { return peer > 0 && peer <= 4 ? 1U << (peer - 1) : 0; }
		uint32_t AdminMask(const std::vector<uint8_t>& peers) { uint32_t mask = 0; for (uint8_t peer: peers) mask |= AdminBit(peer); return mask; }
	}

	uint32_t NetLockstepCoordinator::PeerAdminSurvivors(uint64_t nowMs) const {
		uint32_t live = 0;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if (peer == GetHostPeerId() || m_RemovedPeers.contains(peer) || IsSeatReleased(peer)) continue;
			const uint64_t heard = LastAuthenticatedTraffic(peer);
			if (peer == m_Config.localPeerId || (nowMs >= heard && nowMs - heard < c_NetHostLossSilenceMs)) live |= AdminBit(peer);
		}
		return live;
	}

	bool NetLockstepCoordinator::PeerAdminCertificateValid(const NetHostMigrationMessage& proposal, uint32_t voters) const {
		if (proposal.preparedFrame != 3 || proposal.completeFrom != m_Config.migrationGeneration + 1 || proposal.successorPeerId == GetHostPeerId() ||
		    voters != proposal.connectedMask || voters != AdminMask(proposal.members) || (voters & AdminBit(proposal.successorPeerId)) == 0 ||
		    (voters & AdminBit(GetHostPeerId())) != 0 || (voters >> m_Config.peerCount) != 0 || std::popcount(voters) != proposal.members.size()) return false;
		if (MigrationUsesDirectory()) {
			uint32_t selected = 0; for (uint16_t member: m_MigrationChoice->members) selected |= AdminBit(static_cast<uint8_t>(member));
			return m_MigrationChoice->generation == proposal.completeFrom && m_MigrationChoice->host == proposal.successorPeerId &&
			    m_MigrationChoice->boundary == proposal.boundary && selected == voters;
		}
		// Each signer checks silence and the complete survivor set before voting.
		// An already signed certificate remains valid when the old host returns.
		// A lone direct joiner cannot distinguish a dead host from a partitioned
		// host that still owns the frame tie. Only the directory can fence it.
		uint32_t owners = 0;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer)
			if (!m_RemovedPeers.contains(peer) && !IsSeatReleased(peer)) owners |= AdminBit(peer);
		return std::popcount(voters) >= 2 && (voters & ~owners) == 0 && 2 * std::popcount(voters) > std::popcount(owners);
	}

	bool NetLockstepCoordinator::ValidatePeerAdmin(const NetHostMigrationMessage& proposal) const {
		if (m_PlaneTicking || proposal.frame <= m_Stats.nextFrame || proposal.frame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew ||
		    proposal.bytes.size() != NetHash32{}.size() || proposal.boundary == UINT64_MAX || !m_LastCompletedSimulationTick ||
		    *m_LastCompletedSimulationTick < proposal.boundary || m_GrantedSimulationTick ||
		    !m_MigrationHistory.contains(proposal.boundary) || (proposal.connectedMask & AdminBit(m_Config.localPeerId)) == 0 ||
		    !PeerAdminCertificateValid(proposal, proposal.connectedMask)) return false;
		if (!MigrationUsesDirectory() && !m_PeerAdminOwnVote &&
		    (m_TimingNowMs < m_AuthorityLastHeardMs || m_TimingNowMs - m_AuthorityLastHeardMs < c_NetHostLossSilenceMs ||
		     proposal.connectedMask != PeerAdminSurvivors(m_TimingNowMs))) return false;
		if (m_PeerAdminOwnVote && m_PeerAdminOwnVote->completeFrom == proposal.completeFrom &&
		    (m_PeerAdminOwnVote->frame != proposal.frame || m_PeerAdminOwnVote->boundary != proposal.boundary || m_PeerAdminOwnVote->successorPeerId != proposal.successorPeerId ||
		     m_PeerAdminOwnVote->connectedMask != proposal.connectedMask || m_PeerAdminOwnVote->bytes != proposal.bytes)) return false;
		const auto prefix = PeerAppliedFramePrefix(proposal.boundary);
		return std::equal(prefix.begin(), prefix.end(), proposal.bytes.begin());
	}

	void NetLockstepCoordinator::TickPeerAdmin(uint64_t nowMs) {
		if (!UsesPeerFrameGroups() || !IsRunning() || m_PlaneTicking || m_GrantedSimulationTick || !m_LastCompletedSimulationTick) return;
		const bool superseded = m_CertifiedSupersedingGeneration > m_Config.migrationGeneration;
		if (m_Config.localPeerId == GetHostPeerId() && !superseded) return;
		if (!superseded && !m_PeerAdminOwnVote && !MigrationUsesDirectory() &&
		    (nowMs < m_AuthorityLastHeardMs || nowMs - m_AuthorityLastHeardMs < c_NetHostLossSilenceMs)) return;
		if (!m_PeerAdminRequest) {
			m_MigrationChoice.reset();
			m_PeerAdminRequest = NetHostChangeRequest{};
			m_PeerAdminRequest->generation = m_Config.migrationGeneration; m_PeerAdminRequest->roundId = m_Config.matchConfig.roundId;
			m_PeerAdminRequest->configHash = m_RoundConfigHash; m_PeerAdminRequest->appliedFrame = *m_LastCompletedSimulationTick;
			m_PeerAdminRequest->preparedFrame = m_Stats.nextFrame - 1;
			m_MigrationPhase = NetHostMigrationPhase::Contacting; m_MigrationSinceMs = nowMs;
		}
		if (m_MigrationChoice && !MigrationUsesDirectory() && !m_PeerAdminOwnVote) {
			uint32_t chosen = 0;
			for (uint16_t peer: m_MigrationChoice->members) chosen |= AdminBit(static_cast<uint8_t>(peer));
			if (chosen != PeerAdminSurvivors(nowMs)) m_MigrationChoice.reset();
		}
		if (!m_MigrationChoice) {
			auto choice = m_Config.hostChangeReferee ? m_Config.hostChangeReferee(*m_PeerAdminRequest) : NetHostChangeReply{};
			if (choice.state == NetHostChangeReply::State::Waiting) return;
			if (choice.state == NetHostChangeReply::State::Decided) {
				if (choice.generation != m_Config.migrationGeneration + 1 || choice.host == 0 || choice.host > m_Config.peerCount || choice.boundary == UINT64_MAX || choice.members.empty()) return;
			} else {
				const uint32_t survivors = PeerAdminSurvivors(nowMs);
				if (std::popcount(survivors) < 2) return;
				choice.generation = m_Config.migrationGeneration + 1; choice.boundary = *m_LastCompletedSimulationTick;
				for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if ((survivors & AdminBit(peer)) != 0) {
					choice.members.push_back(peer);
					if (peer != m_Config.localPeerId && m_PeerAppliedThrough.contains(peer)) choice.boundary = std::min(choice.boundary, m_PeerAppliedThrough.at(peer));
				}
				choice.host = choice.members.front();
			}
			m_MigrationChoice = std::move(choice);
		}
		m_MigrationSuccessor = static_cast<uint8_t>(m_MigrationChoice->host);
		if (m_MigrationSuccessor != m_Config.localPeerId || m_PeerAdminOwnVote) return;
		// A survivor whose last progress predates this request may still be
		// inside the liveness window. Do not fix an activation while that
		// reader is frozen: the first vote is immutable once it can certify.
		for (uint16_t member: m_MigrationChoice->members) if (member != m_Config.localPeerId) {
			const auto progress = m_PeerAppliedAtMs.find(static_cast<uint8_t>(member));
			if (progress == m_PeerAppliedAtMs.end() || progress->second < m_MigrationSinceMs) return;
		}
		auto proposal = PeerFrameMessage(NetHostMigrationMessageType::PeerBridge);
		proposal.preparedFrame = 3; proposal.completeFrom = m_MigrationChoice->generation; proposal.boundary = m_MigrationChoice->boundary;
		proposal.successorPeerId = m_MigrationSuccessor; proposal.connectedMask = 0;
		for (uint16_t peer: m_MigrationChoice->members) { proposal.members.push_back(static_cast<uint8_t>(peer)); proposal.connectedMask |= AdminBit(static_cast<uint8_t>(peer)); }
		uint64_t lead = m_Config.slowPlayerBoundTicks;
		for (uint8_t peer: proposal.members) lead = std::max<uint64_t>(lead, InputDelayAt(peer, m_Stats.nextFrame) + m_Config.slowPlayerBoundTicks);
		uint64_t prepared = std::max(m_Stats.nextFrame, proposal.boundary + 1);
		for (uint8_t peer: proposal.members) if (const auto report = m_Stats.peers.find(peer); report != m_Stats.peers.end())
			prepared = std::max(prepared, report->second.reportedNextFrame);
		if (prepared > UINT64_MAX - lead) return;
		proposal.frame = prepared + lead;
		// Every signer must be able to accept this future frame before any
		// immutable vote exists. A fresh but distant tail reader is not ready.
		for (uint8_t peer: proposal.members) if (peer != m_Config.localPeerId) {
			const auto report = m_Stats.peers.find(peer);
			if (report == m_Stats.peers.end() || report->second.reportedNextFrame == 0 ||
			    proposal.frame <= report->second.reportedNextFrame ||
			    proposal.frame - report->second.reportedNextFrame > NetLockstepCodec::c_MaxFutureFrameSkew) return;
		}
		if (!MigrationUsesDirectory()) {
			// No vote has been published. Select the current common displayed
			// prefix so a long catch-up cannot age an unvoted choice out of history.
			proposal.boundary = *m_LastCompletedSimulationTick;
			for (uint8_t peer: proposal.members) if (peer != m_Config.localPeerId) {
				const auto applied = m_PeerAppliedThrough.find(peer);
				if (applied == m_PeerAppliedThrough.end()) return;
				proposal.boundary = std::min(proposal.boundary, applied->second);
			}
			m_MigrationChoice->boundary = proposal.boundary;
		}
		const auto prefix = PeerAppliedFramePrefix(proposal.boundary); proposal.bytes.assign(prefix.begin(), prefix.end()); proposal.totalBytes = static_cast<uint32_t>(proposal.bytes.size());
		proposal.voterMask = AdminBit(m_Config.localPeerId);
		if (!ValidatePeerAdmin(proposal)) { RequestPeerCommittedTail(nowMs); return; }
		m_PeerAdminOwnVote = proposal;
		m_PeerBridgeVotes[{proposal.frame, 3}].emplace(m_Config.localPeerId, proposal);
		SendPeerFrameMessage(proposal); TryCommitPeerBridge(proposal.frame, nowMs, 3);
	}

	void NetLockstepCoordinator::InstallPeerAdmin(const NetHostMigrationMessage& proposal, uint32_t voters, uint64_t nowMs) {
		NetGameHostAuthority authority{GetHostPeerId(), proposal.successorPeerId, proposal.completeFrom, proposal.frame, voters, voters, voters};
		if (!authority.IsValid()) return;
		m_AuthorityEvents.emplace(proposal.frame, authority);
		if (2 * std::popcount(voters) >= m_Config.peerCount) m_PeerAdminFrameMembers = voters;
		m_MigrationBoundary = proposal.boundary;
		m_MigrationSuccessor = proposal.successorPeerId;
		m_MigrationGeneration = proposal.completeFrom;
		m_MigrationPhase = NetHostMigrationPhase::Recovering;
		DiagnosticLine() << "[net-admin] validated boundary=" << proposal.boundary << " activation=" << proposal.frame << " host=" << static_cast<int>(proposal.successorPeerId) << " voters=" << voters << std::endl;
		(void)nowMs;
	}

	void NetLockstepCoordinator::DeliverPeerAdmin(const NetLockstepReadyFrame& ready) {
		for (const auto* commands: {&ready.localCommands, &ready.remoteCommands}) for (const auto& command: *commands) {
			const auto* authority = std::get_if<NetGameHostAuthority>(&command.payload);
			if (!authority || !authority->IsValid() || authority->applyFrame != ready.frame || authority->generation <= m_Config.migrationGeneration) continue;
			m_Config.authorityPeerId = authority->peerId; m_Config.migrationGeneration = authority->generation;
			m_AuthorityEvents.emplace(ready.frame, *authority);
			m_MigrationResult = {}; m_MigrationResult.generation = authority->generation; m_MigrationResult.boundary = m_MigrationBoundary;
			m_MigrationResult.activationFrame = ready.frame; m_MigrationResult.hostPeerId = authority->peerId;
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if ((authority->members & AdminBit(peer)) != 0) {
				m_MigrationResult.members.push_back(peer);
			}
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if (peer != m_Config.localPeerId && !m_RemovedPeers.contains(peer) && !IsSeatReleased(peer)) m_MigrationResult.transports[peer] = peer;
			m_MigrationNotice = true; m_MigrationPhase = NetHostMigrationPhase::Complete;
			m_PeerAdminRequest.reset(); m_PeerAdminOwnVote.reset();
			if (m_Config.peerSessionLinks) {
				m_Config.peerSessionLinks->hostPeerId = authority->peerId;
				m_Config.peerSessionLinks->sessionRoutes.clear();
				for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if (peer != m_Config.localPeerId) m_Config.peerSessionLinks->sessionRoutes[peer] = peer;
			}
		}
	}

} // namespace RTE
