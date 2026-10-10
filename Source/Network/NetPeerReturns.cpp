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

	NetHash32 NetLockstepCoordinator::PeerFramePrefix(uint64_t frame) const {
		if (const auto prepared = m_PeerPreparedPrefixes.find(frame); prepared != m_PeerPreparedPrefixes.end()) return prepared->second;
		return PeerAppliedFramePrefix(frame);
	}

	NetHash32 NetLockstepCoordinator::PeerAppliedFramePrefix(uint64_t frame) const {
		if (m_Playback) if (const auto applied = m_PeerReplayAppliedPrefixes.find(frame); applied != m_PeerReplayAppliedPrefixes.end()) return applied->second;
		NetHash32 hash{};
		const auto kept = m_MigrationHistory.find(frame);
		if (kept == m_MigrationHistory.end()) return hash;
		uint8_t mac[32]{};
		if (GetNetAuthCrypto().HmacSha256(m_Config.migrationKey.data(), m_Config.migrationKey.size(), kept->second.data(), kept->second.size(), mac))
			std::copy_n(mac, hash.size(), hash.begin());
		return hash;
	}

	void NetLockstepCoordinator::SendPeerFrameWitness(uint64_t frame) {
		if (!m_MigrationHistory.contains(frame)) return;
		auto witness = PeerFrameMessage(NetHostMigrationMessageType::PeerReceipt);
		witness.preparedFrame = UINT64_MAX; witness.frame = witness.appliedFrame = frame;
		const auto group = m_FrameGroupChanges.upper_bound(frame);
		witness.voterMask = group == m_FrameGroupChanges.begin() ? (1U << m_Config.peerCount) - 1 : std::prev(group)->second;
		if ((witness.voterMask & SeatBit(m_Config.localPeerId)) == 0) {
			// A private reader cannot witness this group's frame. Advertise its
			// newly applied prefix on the next pump so return eligibility can
			// follow its catch-up instead of a stale periodic heartbeat.
			m_PeerHeartbeatAtMs = std::min(m_PeerHeartbeatAtMs, m_TimingNowMs);
			return;
		}
		const auto applied = PeerAppliedFramePrefix(frame), prepared = PeerFramePrefix(frame);
		witness.bytes.assign(applied.begin(), applied.end()); witness.bytes.insert(witness.bytes.end(), prepared.begin(), prepared.end());
		m_PeerFrameWitnesses[frame][m_Config.localPeerId] = witness;
		SendPeerFrameMessage(std::move(witness), m_Config.frameLane);
	}

	void NetLockstepCoordinator::SendPeerCommittedTail(uint8_t peer, uint64_t fromFrame) {
		const auto first = m_MigrationHistory.lower_bound(fromFrame);
		if (first == m_MigrationHistory.end() || first->first != fromFrame) {
			auto missing = PeerFrameMessage(NetHostMigrationMessageType::PeerTailMissing);
			missing.frame = fromFrame;
			SendPeerFrameMessage(std::move(missing), NetTransportLane::ControlReliable, peer);
			return;
		}
		for (auto kept = first; kept != m_MigrationHistory.end() && kept->first - fromFrame < 32 && m_LastCompletedSimulationTick && kept->first <= *m_LastCompletedSimulationTick; ++kept) {
			const auto prefix = kept->first == m_Config.startFrame ? NetHash32{} : PeerFramePrefix(kept->first - 1);
			const auto prepared = PeerFramePrefix(kept->first);
			std::vector<uint8_t> body(prefix.begin(), prefix.end()); body.insert(body.end(), prepared.begin(), prepared.end());
			PutSize(body, static_cast<uint32_t>(kept->second.size()));
			body.insert(body.end(), kept->second.begin(), kept->second.end());
			std::map<uint8_t, const NetHostMigrationMessage*> latest;
			for (const auto& [key, decision]: m_PeerBridgeCertificates) if (key.first <= kept->first) latest[key.second] = &decision;
			std::set<PeerDecisionKey> proofs;
			for (const auto& [kind, decision]: latest) proofs.emplace(decision->frame, kind);
			for (const auto& [key, decision]: m_PeerBridgeCertificates) if (key.second == 3) proofs.insert(key);
			for (const auto& [seat, bridge]: m_PeerBridges) if (bridge.fromFrame <= kept->first) proofs.emplace(bridge.fromFrame, 0);
			std::vector<std::vector<uint8_t>> certificates;
			for (const auto& key: proofs) if (const auto proof = m_PeerBridgeCertificates.find(key); proof != m_PeerBridgeCertificates.end()) {
				std::vector<uint8_t> bytes;
				if (NetHostMigrationCodec::Encode(proof->second, m_Config.migrationKey, bytes)) certificates.push_back(std::move(bytes));
			}
			if (certificates.size() > 32) return;
			body.push_back(static_cast<uint8_t>(certificates.size()));
			for (const auto& bytes: certificates) { PutSize(body, static_cast<uint32_t>(bytes.size())); body.insert(body.end(), bytes.begin(), bytes.end()); }
			const auto witnesses = m_PeerFrameWitnesses.find(kept->first);
			body.push_back(witnesses == m_PeerFrameWitnesses.end() ? 0 : static_cast<uint8_t>(witnesses->second.size()));
			if (witnesses != m_PeerFrameWitnesses.end()) for (const auto& [sender, witness]: witnesses->second) {
				std::vector<uint8_t> bytes;
				if (!NetHostMigrationCodec::Encode(witness, m_Config.migrationKey, bytes)) return;
				PutSize(body, static_cast<uint32_t>(bytes.size())); body.insert(body.end(), bytes.begin(), bytes.end());
			}
			if (body.size() > NetHostMigrationCodec::c_MaxPeerTailBytes) return;
			for (size_t offset = 0; offset < body.size(); offset += NetHostMigrationCodec::c_ChunkBytes) {
				auto tail = PeerFrameMessage(NetHostMigrationMessageType::PeerTail);
				tail.frame = kept->first;
				tail.totalBytes = static_cast<uint32_t>(body.size()); tail.offset = static_cast<uint32_t>(offset);
				const auto membership = m_FrameGroupChanges.upper_bound(kept->first);
				tail.voterMask = membership == m_FrameGroupChanges.begin() ? (1U << m_Config.peerCount) - 1 : std::prev(membership)->second;
				const size_t end = std::min(body.size(), offset + NetHostMigrationCodec::c_ChunkBytes);
				tail.bytes.assign(body.begin() + offset, body.begin() + end);
				SendPeerFrameMessage(std::move(tail), NetTransportLane::ControlReliable, peer);
			}
		}
	}

	void NetLockstepCoordinator::RequestPeerCommittedTail(uint64_t nowMs) {
		if (nowMs < m_PeerTailRequestAtMs) return;
		m_PeerTailRequestAtMs = nowMs + 100;
		auto request = PeerFrameMessage(NetHostMigrationMessageType::PeerTailRequest);
		request.frame = m_Stats.nextFrame;
		for (const auto& [peer, applied]: m_PeerAppliedThrough) {
			if (peer == m_Config.localPeerId || applied < m_Stats.nextFrame) continue;
			SendPeerFrameMessage(request, NetTransportLane::ControlReliable, peer);
		}
	}

	bool NetLockstepCoordinator::ApplyPeerCommittedTail(uint64_t frame, uint64_t nowMs) {
		const auto kept = m_PeerCommittedTail.find(frame);
		if (kept == m_PeerCommittedTail.end()) return false;
		if (frame > m_Config.startFrame && (!m_PeerTailPrefixes.contains(frame) || m_PeerTailPrefixes.at(frame) != PeerFramePrefix(frame - 1))) {
			Fail(NetLockstepStopReason::ProtocolError, frame, "the committed tail differs from displayed history at frame " + std::to_string(frame)); return false;
		}
		NetLockstepReadyFrame ready;
		if (const auto decisions = m_PeerTailDecisions.find(frame); decisions != m_PeerTailDecisions.end()) {
			for (const auto& decision: decisions->second) InstallPeerBridge(decision, nowMs);
			m_PeerTailDecisions.erase(decisions);
		}
		PreparePeerBridgeInputs(frame, nowMs);
		if (!DecodeMigrationFrame(kept->second, frame, ready)) {
			Fail(NetLockstepStopReason::ProtocolError, frame, "invalid committed peer tail"); return false;
		}
		StoreMigrationFrame(frame, kept->second);
		// The certified history also fills receipt gaps in the original senders'
		// streams. Without this, reading a tick from the tail leaves their next
		// live input waiting forever behind an already displayed tick.
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if (peer == m_Config.localPeerId || !IsRemoteRequiredForFrame(peer, frame)) continue;
			if (const auto bridge = m_PeerBridges.find(peer); bridge != m_PeerBridges.end() && frame >= bridge->second.fromFrame &&
			    (!bridge->second.returnFrame || frame < *bridge->second.returnFrame)) continue;
			auto& through = m_PeerAcceptedThrough[peer];
			through = std::max(through, frame + 1);
			auto& ahead = m_PeerAcceptedAhead[peer];
			ahead.erase(ahead.begin(), ahead.lower_bound(through));
			while (through != UINT64_MAX && ahead.erase(through) != 0) ++through;
			m_PeerInputReceipts[peer][m_Config.localPeerId] = through;
			auto receipt = PeerFrameMessage(NetHostMigrationMessageType::PeerReceipt);
			receipt.successorPeerId = peer; receipt.frame = through;
			SendPeerFrameMessage(std::move(receipt));
		}
		m_Stats.nextFrame = frame + 1;
		RememberCommittedFrame(ready);
		m_ReadyFrames.push_back(std::move(ready));
		m_LocalFrames.erase(frame); m_LocalCommands.erase(frame); m_LocalObservations.erase(frame); m_LocalValueObservations.erase(frame);
		m_RemoteFrames.erase(frame); m_RemoteCommands.erase(frame); m_RemoteObservations.erase(frame); m_RemoteValueObservations.erase(frame);
		m_PeerCommittedTail.erase(kept);
		m_PeerTailPrefixes.erase(frame); m_PeerTailVotes.erase(frame);
		m_PeerHeartbeatAtMs = std::min(m_PeerHeartbeatAtMs, nowMs);
		return true;
	}

	bool NetLockstepCoordinator::PeerFrameCatchUpActive() const {
		NET_PLANE_CHECK();
		if (!UsesPeerFrameGroups() || !IsRunning() || !m_LastCompletedSimulationTick) return false;
		if (m_PeerTailThrough && *m_LastCompletedSimulationTick < *m_PeerTailThrough) return true;
		if (!m_PeerHadHitch || !m_PeerPaceStartMs || m_TimingNowMs < *m_PeerPaceStartMs || m_Config.simTickMs <= 0) return false;
		const uint64_t due = m_PeerPaceStartFrame + static_cast<uint64_t>((m_TimingNowMs - *m_PeerPaceStartMs) / m_Config.simTickMs);
		return *m_LastCompletedSimulationTick + 1 < due;
	}

	bool NetLockstepCoordinator::ProposePeerReclaim(uint8_t peer, NetPeerId transport, uint32_t incarnation, uint64_t frame, uint64_t trailFrames, std::string* error) {
		if (!IsRunning() || m_Config.localPeerId != GetHostPeerId() || peer == 0 || peer > m_Config.peerCount ||
		    !m_PeerBridges.contains(peer) || transport == c_InvalidNetPeerId || incarnation == 0 || frame <= m_Stats.nextFrame ||
		    frame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew || !PeerGroupHasAuthority(m_FrameGroupMembers)) {
			if (error) *error = "the private return needs an authoritative group and a future frame";
			return false;
		}
		if (HasAgreedSeatReclaim(peer, frame)) return true;
		auto proposal = PeerFrameMessage(NetHostMigrationMessageType::PeerBridge);
		proposal.preparedFrame = 1; proposal.frame = frame; proposal.boundary = m_LastCompletedSimulationTick.value_or(m_Stats.nextFrame - 1);
		proposal.members = {peer}; proposal.connectedMask = m_FrameGroupMembers | SeatBit(peer);
		const auto prefix = PeerAppliedFramePrefix(proposal.boundary); proposal.bytes.assign(prefix.begin(), prefix.end());
		const uint32_t delay = std::max<uint16_t>(InputDelayAt(peer, frame), ReturnDelayFloor(peer, transport));
		const uint64_t transit = static_cast<uint64_t>(std::ceil((m_Stats.peers[peer].pingMs + m_Stats.peers[peer].jitterMs) / m_Config.simTickMs));
		const uint64_t neutral = delay + transit + trailFrames + m_Config.slowPlayerBoundTicks;
		if (neutral > NetLockstepCodec::c_MaxFutureFrameSkew) { if (error) *error = "the private return is still too far behind its activation"; return false; }
		PutSize(proposal.bytes, delay); PutSize(proposal.bytes, static_cast<uint32_t>(neutral)); PutSize(proposal.bytes, incarnation);
		proposal.totalBytes = static_cast<uint32_t>(proposal.bytes.size()); proposal.voterMask = SeatBit(m_Config.localPeerId);
		if (!ValidatePeerBridge(proposal)) { if (error) *error = "the private return does not match the committed boundary"; return false; }
		if (m_Config.peerSessionLinks) m_Config.peerSessionLinks->BindAdmission(peer, transport);
		m_RemoteTransports[peer] = transport;
		auto& votes = m_PeerBridgeVotes[{frame, 1}];
		if (const auto own = votes.find(m_Config.localPeerId); own != votes.end()) return SameBridge(own->second, proposal);
		votes.emplace(m_Config.localPeerId, proposal); SendPeerFrameMessage(std::move(proposal)); TryCommitPeerBridge(frame, m_TimingNowMs, 1);
		return true;
	}

	bool NetLockstepCoordinator::AdoptPeerReturnConfig(NetLockstepConfig& config, const NetLockstepCoordinator& replay, uint64_t activation) const {
		NET_PLANE_CHECK();
		NetLockstepPlane::Check(&replay, "AdoptPeerReturnConfig (the replay)");
		const auto kept = m_PeerReturnProofs.find(activation);
		if (!UsesPeerFrameGroups() || kept == m_PeerReturnProofs.end()) return false;
		NetHostMigrationMessage agreed; uint32_t voters = 0;
		if (!DecodePeerBridgeCertificate(kept->second, agreed, voters) || !PeerGroupHasAuthority(voters) ||
		    agreed.sessionId != config.sessionId || agreed.roundId != config.matchConfig.roundId || agreed.configHash != config.originalRoundConfigHash ||
		    agreed.preparedFrame != 1 || (!replay.m_MigrationHistory.contains(agreed.boundary) && !replay.m_PeerReplayAppliedPrefixes.contains(agreed.boundary))) return false;
		const auto prefix = replay.PeerAppliedFramePrefix(agreed.boundary);
		if (!std::equal(prefix.begin(), prefix.end(), agreed.bytes.begin())) return false;
		size_t offset = prefix.size();
		for (uint8_t peer: agreed.members) {
			uint32_t delay = 0, neutral = 0, incarnation = 0;
			if (!TakeSize(agreed.bytes, offset, delay) || !TakeSize(agreed.bytes, offset, neutral) || !TakeSize(agreed.bytes, offset, incarnation) ||
			    peer == 0 || peer > config.peerCount || delay == 0 || delay > NetLockstepCodec::c_MaxInputDelayFrames ||
			    neutral < delay || neutral > NetLockstepCodec::c_MaxFutureFrameSkew || incarnation == 0 || activation > UINT64_MAX - neutral) return false;
			config.initialSeatReclaims[peer] = {peer, config.migrationGeneration, activation * 8 + peer, incarnation, activation, static_cast<uint16_t>(delay), activation + neutral};
			config.peerIncarnations[peer] = incarnation;
		}
		return offset == agreed.bytes.size() && config.initialSeatReclaims.contains(config.localPeerId);
	}

} // namespace RTE
