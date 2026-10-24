#include "NetPeerFrameHelpers.h"

#include "DiagnosticLine.h"
#include "NetAuthCrypto.h"
#include "NetLobbyProtocol.h"
#include "NetProtocol.h"

#include "lz4.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <string_view>

namespace RTE {

	using namespace NetPeerFrameDetail;

	bool NetLockstepCoordinator::RequestPrivateBaseCapture() {
		NET_PLANE_CHECK();
		if (!UsesPeerFrameGroups() || !IsRunning() || !IsOwnHostSeatHeld()) return false;
		if (m_PeerCaptureRequested) return true;
		if (m_PeerCaptureSerial == UINT64_MAX) return false;
		auto request = PeerFrameMessage(NetHostMigrationMessageType::PeerCaptureRequest);
		request.boundary = m_Config.migrationGeneration;
		request.completeFrom = ++m_PeerCaptureSerial;
		m_PeerCaptureRequest = request;
		m_PeerCaptureRequested = true;
		m_PeerCaptureRetryAtMs = 0;
		return true;
	}

	void NetLockstepCoordinator::ClearPrivateBaseCaptureRequest() {
		NET_PLANE_CHECK();
		m_PeerCaptureRequested = false;
		if (m_PeerCaptureRequest && m_PeerCaptureRequest->senderPeerId == m_Config.localPeerId) m_PeerCaptureRequest.reset();
	}

	bool NetLockstepCoordinator::ValidatePeerCaptureRequest(const NetHostMigrationMessage& request, uint64_t frame) const {
		uint64_t generation = m_Config.migrationGeneration;
		if (!m_AuthorityEvents.empty()) {
			const auto after = m_AuthorityEvents.upper_bound(frame);
			generation = after == m_AuthorityEvents.begin() ? after->second.generation - 1 : std::prev(after)->second.generation;
		}
		return request.type == NetHostMigrationMessageType::PeerCaptureRequest && request.sessionId == m_Config.sessionId &&
		    request.roundId == m_Config.matchConfig.roundId && request.configHash == m_RoundConfigHash && request.generation == 1 &&
		    request.senderPeerId == HostAuthorityAt(frame) && request.successorPeerId == request.senderPeerId && request.boundary == generation &&
		    request.completeFrom != 0 && request.bytes.empty() && request.totalBytes == 0 && request.members.empty() &&
		    IsSeatUnderAI(request.senderPeerId, frame);
	}

	bool NetLockstepCoordinator::ValidatePeerCaptureBridge(const NetHostMigrationMessage& proposal) const {
		if (proposal.preparedFrame != 4 || proposal.frame > UINT64_MAX - 2 || proposal.boundary >= proposal.frame ||
		    proposal.bytes.size() <= NetHash32{}.size() || proposal.members.size() != 1) return false;
		uint32_t playing = PeerFrameMembersAt(proposal.frame);
		PeerVotingOwners(playing, proposal.frame);
		if (playing == 0 || (playing & ~proposal.connectedMask) != 0) return false;
		NetHostMigrationMessage request;
		return NetHostMigrationCodec::Decode({proposal.bytes.begin() + NetHash32{}.size(), proposal.bytes.end()}, m_Config.migrationKey, request) &&
		    ValidatePeerCaptureRequest(request, proposal.frame) && proposal.members.front() == request.senderPeerId &&
		    proposal.successorPeerId == request.senderPeerId && proposal.completeFrom == request.completeFrom &&
		    (proposal.connectedMask & SeatBit(request.senderPeerId)) == 0;
	}

	void NetLockstepCoordinator::TickPeerCaptureRequest(uint64_t nowMs) {
		if (!UsesPeerFrameGroups() || !IsRunning() || !m_PeerCaptureRequest || m_Config.simTickMs <= 0 || WaitsForPlacement()) return;
		const auto request = *m_PeerCaptureRequest;
		if (!ValidatePeerCaptureRequest(request, m_Stats.nextFrame)) return;
		if (const auto completed = m_PeerCapturesCompleted.find(request.boundary); completed != m_PeerCapturesCompleted.end() && completed->second >= request.completeFrom) return;
		if (request.senderPeerId == m_Config.localPeerId && nowMs >= m_PeerCaptureRetryAtMs) {
			m_PeerCaptureRetryAtMs = nowMs + c_NetPeerVoteResendMs;
			SendPeerFrameMessage(request);
		}
		// The already agreed AI producer proposes; only the playing voters decide the future capture frame.
		if (AiAuthorityAt(m_Stats.nextFrame) != m_Config.localPeerId || !m_LastCompletedSimulationTick || !PeerGroupHasAuthority(m_FrameGroupMembers)) return;
		const auto [hostWire, hostRoute] = PeerFrameRoute(request.senderPeerId);
		const uint64_t freshness = (hostWire && hostWire->IsPeerPingMeasured(hostRoute) ? hostWire->GetPeerPingMs(hostRoute) : 0) + 2 * c_NetPeerHeartbeatMs;
		const uint64_t hostHeard = LastAuthenticatedTraffic(request.senderPeerId);
		if (nowMs > hostHeard && nowMs - hostHeard > freshness) return;
		// A new authority or a return already agreed ahead is applied before another capture is proposed.
		if (m_AuthorityEvents.upper_bound(m_Stats.nextFrame) != m_AuthorityEvents.end()) return;
		// Returners finish their agreed neutral intervals before a new capture asks them to vote.
		for (const auto& [peer, back]: m_ReclaimTransactions) if (back.neutralThroughFrame >= m_Stats.nextFrame) return;
		for (const auto& [key, votes]: m_PeerBridgeVotes) if (key.second == 1 && key.first >= m_Stats.nextFrame && votes.contains(m_Config.localPeerId)) return;
		for (const auto& [key, votes]: m_PeerBridgeVotes) if (key.second == 4 && key.first >= m_Stats.nextFrame) {
			if (votes.contains(m_Config.localPeerId)) return;
		}
		auto proposal = PeerFrameMessage(NetHostMigrationMessageType::PeerBridge);
		proposal.preparedFrame = 4;
		proposal.successorPeerId = request.senderPeerId;
		proposal.members = {request.senderPeerId};
		proposal.completeFrom = request.completeFrom;
		proposal.connectedMask = m_FrameGroupMembers;
		PeerVotingOwners(proposal.connectedMask, m_Stats.nextFrame);
		proposal.boundary = *m_LastCompletedSimulationTick;
		uint64_t prepared = m_Stats.nextFrame;
		uint64_t lead = m_Config.slowPlayerBoundTicks + 1;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if ((proposal.connectedMask & SeatBit(peer)) == 0 || peer == m_Config.localPeerId) continue;
			const auto applied = m_PeerAppliedThrough.find(peer);
			const auto stats = m_Stats.peers.find(peer);
			if (applied == m_PeerAppliedThrough.end() || stats == m_Stats.peers.end() || stats->second.reportedNextFrame == 0) return;
			proposal.boundary = std::min(proposal.boundary, applied->second);
			prepared = std::max(prepared, stats->second.reportedNextFrame);
			uint64_t rttMs = stats->second.pingMs;
			const auto [wire, route] = PeerFrameRoute(peer);
			if (wire && wire->IsPeerPingMeasured(route)) rttMs = std::max<uint64_t>(rttMs, wire->GetPeerPingMs(route));
			lead = std::max<uint64_t>(lead, static_cast<uint64_t>(std::ceil((rttMs + c_NetPeerVoteResendMs) / m_Config.simTickMs)) + m_Config.slowPlayerBoundTicks);
		}
		if (m_LastQueuedTargetFrame != UINT64_MAX) prepared = std::max(prepared, m_LastQueuedTargetFrame + 1);
		if (prepared > UINT64_MAX - lead - 2) return;
		proposal.frame = prepared + lead;
		const auto prefix = PeerAppliedFramePrefix(proposal.boundary);
		proposal.bytes.assign(prefix.begin(), prefix.end());
		std::vector<uint8_t> signedRequest;
		if (!NetHostMigrationCodec::Encode(request, m_Config.migrationKey, signedRequest)) return;
		proposal.bytes.insert(proposal.bytes.end(), signedRequest.begin(), signedRequest.end());
		proposal.totalBytes = static_cast<uint32_t>(proposal.bytes.size());
		proposal.voterMask = SeatBit(m_Config.localPeerId);
		if (!ValidatePeerBridge(proposal)) return;
		m_PeerBridgeVotes[{proposal.frame, 4}].emplace(m_Config.localPeerId, proposal);
		SendPeerFrameMessage(proposal);
		TryCommitPeerBridge(proposal.frame, nowMs, 4);
	}

	void NetLockstepCoordinator::AppendPeerCaptureCommand(NetLockstepReadyFrame& ready) const {
		const auto certificate = m_PeerBridgeCertificates.find({ready.frame, 4});
		if (certificate == m_PeerBridgeCertificates.end()) return;
		NetHostMigrationMessage agreed; uint32_t voters = 0;
		if (!DecodePeerBridgeCertificate(certificate->second, agreed, voters) || !ValidatePeerCaptureBridge(agreed)) return;
		const NetGameCommand command{agreed.members.front(), NetGameCheckpoint{NetGameCheckpoint::Capture, ready.frame + 2}};
		auto& commands = m_Config.localPeerId == agreed.members.front() ? ready.localCommands : ready.remoteCommands;
		if (std::find(commands.begin(), commands.end(), command) == commands.end()) commands.push_back(command);
	}

	uint32_t NetLockstepCoordinator::PeerReplacedInputWitnesses(uint64_t frame, const std::vector<uint8_t>& held, uint32_t candidates) const {
		// From each held owner whose input is missing here, every candidate that accepted an input the hold replaces, its own included.
		const auto sources = m_PeerSourceInputs.find(frame);
		std::vector<uint8_t> replaced;
		for (uint8_t owner: held)
			if ((sources == m_PeerSourceInputs.end() || !sources->second.contains(owner)) && !m_PeerOtherContinuations.contains(owner)) replaced.push_back(owner);
		uint32_t witnesses = 0;
		for (size_t at = 0; at < replaced.size(); ++at) {
			const auto receipts = m_PeerInputReceipts.find(replaced[at]);
			if (receipts == m_PeerInputReceipts.end()) continue;
			for (const auto& [peer, through]: receipts->second)
				if (through > frame && peer != m_Config.localPeerId && (candidates & SeatBit(peer)) != 0 && (witnesses & SeatBit(peer)) == 0) {
					witnesses |= SeatBit(peer);
					replaced.push_back(peer);
				}
		}
		return witnesses;
	}

	bool NetLockstepCoordinator::PeerBridgeHasInputConflict(const NetHostMigrationMessage& proposal) const {
		if (proposal.preparedFrame != 0) return false;
		for (uint8_t owner: proposal.members) {
			// A receipt runs through a seat's neutral interval by itself: no input there was accepted that a hold could contradict.
			if (PeerMissingInputReceipt(owner, proposal.frame) || m_PeerOtherContinuations.contains(owner) || IsSeatReclaimGap(owner, proposal.frame)) continue;
			const auto receipts = m_PeerInputReceipts.find(owner);
			if (receipts == m_PeerInputReceipts.end()) continue;
			for (const auto& [peer, through]: receipts->second)
				if (through > proposal.frame && (proposal.connectedMask & SeatBit(peer)) != 0) return true;
		}
		return false;
	}

	void NetLockstepCoordinator::ResolvePeerBridgeInputConflicts(uint64_t nowMs) {
		std::map<uint64_t, uint32_t> released;
		for (auto& [key, votes]: m_PeerBridgeVotes) {
			if (!IsHoldDecision(key.second) || key.first < m_Stats.nextFrame || m_PeerBridgeCertificates.contains(key)) continue;
			const auto own = votes.find(m_Config.localPeerId);
			if (own != votes.end() && PeerBridgeHasInputConflict(own->second)) {
				// This proposal requires a signer that has already accepted an
				// excluded input. That signer cannot also vote for this bridge,
				// so no certificate can exist for the abandoned proposal.
				for (uint8_t peer: own->second.members) released[key.first] |= SeatBit(peer);
			}
			std::erase_if(votes, [&](const auto& vote) { return PeerBridgeHasInputConflict(vote.second); });
		}
		for (const auto& [frame, owners]: released) {
			uint32_t retained = 0;
			for (auto vote = m_PeerBridgeVotes.lower_bound({frame, 0x10}); vote != m_PeerBridgeVotes.end() && vote->first.first == frame; ++vote)
				if (IsHoldDecision(vote->first.second))
					if (const auto own = vote->second.find(m_Config.localPeerId); own != vote->second.end() && own->second.voterMask == SeatBit(m_Config.localPeerId))
						for (uint8_t held: own->second.members) retained |= SeatBit(held);
			for (auto certificate = m_PeerBridgeCertificates.lower_bound({frame, 0x10}); certificate != m_PeerBridgeCertificates.end() && certificate->first.first == frame; ++certificate)
				if (IsHoldDecision(certificate->first.second)) retained |= certificate->first.second & 0xF;
			m_PeerRejectedInputs[frame] = (m_PeerRejectedInputs[frame] & ~owners) | retained;
		}
		for (auto pending = m_PeerPendingBridgeInputs.begin(); pending != m_PeerPendingBridgeInputs.end();) {
			if (pending->first < m_Stats.nextFrame) {
				pending = m_PeerPendingBridgeInputs.erase(pending);
				continue;
			}
			for (auto input = pending->second.begin(); input != pending->second.end();) {
				if ((m_PeerRejectedInputs[pending->first] & SeatBit(input->first)) != 0) {
					if (HasHoldDecision(m_PeerBridgeCertificates, pending->first, input->first)) input = pending->second.erase(input);
					else ++input;
					continue;
				}
				AcceptRemoteTick(input->second, nowMs, false);
				input = pending->second.erase(input);
			}
			if (pending->second.empty()) pending = m_PeerPendingBridgeInputs.erase(pending);
			else ++pending;
		}
	}

	bool NetLockstepCoordinator::ValidatePeerBridge(const NetHostMigrationMessage& proposal) const {
		if (proposal.preparedFrame == 3) return ValidatePeerAdmin(proposal);
		if (proposal.preparedFrame == 1) {
			if (ReturnCrossesDeparture(proposal, m_PeerOwnershipChanges)) return false;
			for (uint8_t peer: proposal.members) if ((PeerRetainedSeatsAt(proposal.frame) & SeatBit(peer)) == 0) return false;
		}
		// A hold may collect the missing seat's co-sign; its certificate still needs authority from the actual signers.
		const bool cooperativeHold = proposal.preparedFrame == 0 && proposal.members.size() == 1 &&
		    PeerGroupHasAuthority(proposal.connectedMask | SeatBit(proposal.members.front()), proposal.frame);
		if (proposal.members.empty() || (proposal.preparedFrame == 4 ? !ValidatePeerCaptureBridge(proposal) : proposal.bytes.size() != NetHash32{}.size() + (proposal.preparedFrame == 1 ? 12 * proposal.members.size() : 0)) ||
		    (!(proposal.preparedFrame == 0 ? PeerGroupMayHold(proposal.connectedMask, proposal.frame) : PeerGroupHasAuthority(proposal.connectedMask, proposal.frame)) && !cooperativeHold) ||
		    (proposal.preparedFrame > 2 && proposal.preparedFrame != 4) || (proposal.connectedMask & SeatBit(m_Config.localPeerId)) == 0 || PeerBridgeHasInputConflict(proposal)) return false;
		// A hold agreed ahead names a frame past a boundary every voter has applied, as a return does.
		const bool ahead = proposal.preparedFrame != 0 || proposal.boundary < proposal.frame;
		if (!ahead && proposal.frame != m_Stats.nextFrame) return false;
		if (proposal.preparedFrame == 0 && m_MigrationHistory.contains(proposal.frame)) return false;
		// nextFrame is still unprepared. A hold can fence it when its owner has not supplied that input; a frame already
		// prepared is immutable. Returns and other changes retain their strict future-frame requirement.
		if (ahead && (proposal.frame < m_Stats.nextFrame || (proposal.preparedFrame != 0 && proposal.frame == m_Stats.nextFrame) ||
		    proposal.frame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew)) return false;
		const uint64_t prefixFrame = ahead ? proposal.boundary : proposal.frame - 1;
		const NetHash32 prefix = proposal.frame == m_Config.startFrame ? NetHash32{} : ahead ? PeerAppliedFramePrefix(prefixFrame) : PeerFramePrefix(prefixFrame);
		if (proposal.frame != m_Config.startFrame && !m_MigrationHistory.contains(prefixFrame) &&
		    !(prefixFrame < m_Config.startFrame && m_PeerReplayAppliedPrefixes.contains(prefixFrame))) return false;
		if (!std::equal(prefix.begin(), prefix.end(), proposal.bytes.begin())) return false;
		if (proposal.preparedFrame == 1) {
			for (const auto& [key, votes]: m_PeerBridgeVotes) if (key.second == 4 && key.first >= proposal.frame && key.first >= m_Stats.nextFrame && votes.contains(m_Config.localPeerId)) return false;
		}
		if (proposal.preparedFrame == 4) {
			for (const auto& [key, votes]: m_PeerBridgeVotes) if (key.second == 4 && key.first >= m_Stats.nextFrame && key.first != proposal.frame && votes.contains(m_Config.localPeerId)) return false;
			return true;
		}
		if (proposal.preparedFrame == 2) return proposal.members.size() == 1 && proposal.successorPeerId == proposal.members.front() &&
		    proposal.members.front() > 0 && proposal.members.front() <= m_Config.peerCount && proposal.completeFrom > 0 && proposal.completeFrom <= NetLockstepCodec::c_MaxPeerInputDelayFrames &&
		    (proposal.connectedMask & SeatBit(proposal.members.front())) != 0;
		uint32_t named = 0;
		for (uint8_t peer: proposal.members) named |= SeatBit(peer);
		const auto sources = m_PeerSourceInputs.find(proposal.frame);
		// A named seat whose input is here, or that owes none, is held only as the witness of an input this hold replaces.
		const uint32_t witnesses = proposal.preparedFrame == 0 ? PeerReplacedInputWitnesses(proposal.frame, proposal.members, named) : 0;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			const bool present = sources != m_PeerSourceInputs.end() && sources->second.contains(peer);
			const bool neutral = IsSeatReclaimGap(peer, proposal.frame);
			const auto ended = m_PeerRoundEnds.find(peer);
			const bool departed = ended != m_PeerRoundEnds.end() && !ended->second.closedLink && proposal.frame >= ended->second.frame;
			if (proposal.preparedFrame == 0 && (named & SeatBit(peer)) != 0 && !m_PeerOtherContinuations.contains(peer) && (witnesses & SeatBit(peer)) == 0 &&
			    ((present && !PeerMissingInputReceipt(peer, proposal.frame)) || (neutral && !departed)) &&
			    !(proposal.senderPeerId == GetHostPeerId() && present && !neutral && !HasPeerFrameRoute(peer))) return false;
			if (!ahead && (proposal.connectedMask & SeatBit(peer)) != 0 && proposal.frame >= EffectiveStartOf(peer) && !neutral && !present) return false;
			// A hold agreed ahead of a seat the round already holds there has nothing to decide.
			if (ahead && proposal.preparedFrame == 0 && (named & SeatBit(peer)) != 0)
				if (const auto held = m_PeerBridges.find(peer); held != m_PeerBridges.end() && held->second.committedHold && !held->second.returnFrame) return false;
			if (proposal.preparedFrame == 1 && (named & SeatBit(peer)) != 0 &&
			    (!m_PeerBridges.contains(peer) || !m_PeerBridges.at(peer).committedHold || m_PeerBridges.at(peer).returnFrame)) return false;
		}
		return proposal.preparedFrame == 1 ? (named & proposal.connectedMask) == named : (named & proposal.connectedMask) == 0;
	}

	bool NetLockstepCoordinator::CoSignsOwnHold(const NetHostMigrationMessage& proposal) const {
		// The missing seat may supply its own signature from the live plane. A future cutoff is a promise to stop producing there;
		// the other half still needs authority from the actual signatures, so a silent partition grants no new authority.
		const bool ahead = proposal.boundary < proposal.frame;
		if (proposal.preparedFrame != 0 || proposal.members.size() != 1 || proposal.members.front() != m_Config.localPeerId ||
		    proposal.bytes.size() != NetHash32{}.size() || (proposal.connectedMask & SeatBit(m_Config.localPeerId)) != 0 ||
		    (!ahead && proposal.frame != m_Stats.nextFrame) ||
		    (ahead && (proposal.frame < m_Stats.nextFrame || proposal.frame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew)) ||
		    m_MigrationHistory.contains(proposal.frame) || m_LocalFrames.contains(proposal.frame) ||
		    (m_LastQueuedTargetFrame != UINT64_MAX && m_LastQueuedTargetFrame >= proposal.frame)) return false;
		auto withOwnVote = proposal;
		withOwnVote.connectedMask |= SeatBit(m_Config.localPeerId);
		if (PeerBridgeHasInputConflict(withOwnVote)) return false;
		if (proposal.frame != m_Config.startFrame) {
			const uint64_t boundary = ahead ? proposal.boundary : proposal.frame - 1;
			if (!m_MigrationHistory.contains(boundary) && !(boundary < m_Config.startFrame && m_PeerReplayAppliedPrefixes.contains(boundary))) return false;
			const auto prefix = ahead ? PeerAppliedFramePrefix(boundary) : PeerFramePrefix(boundary);
			if (!std::equal(prefix.begin(), prefix.end(), proposal.bytes.begin())) return false;
		}
		return PeerGroupHasAuthority(withOwnVote.connectedMask, proposal.frame);
	}

	bool NetLockstepCoordinator::HoldEmptiesTheRound(const NetHostMigrationMessage& hold, bool signing) const {
		if (hold.preparedFrame != 0 || hold.members.empty()) return false;
		uint64_t last = hold.frame;
		for (const auto& [peer, bridge]: m_PeerBridges) if (bridge.committedHold && !bridge.returnFrame) last = std::max(last, bridge.fromFrame);
		// A hold signed here and not yet installed takes its seat too, at whatever frame it was placed.
		uint32_t signedHolds = 0;
		if (signing) for (auto votes = m_PeerBridgeVotes.lower_bound({m_Stats.nextFrame, 0}); votes != m_PeerBridgeVotes.end(); ++votes) {
			if (!IsHoldDecision(votes->first.second) || m_PeerBridgeCertificates.contains(votes->first)) continue;
			const auto own = votes->second.find(m_Config.localPeerId);
			if (own == votes->second.end() || own->second.voterMask == 0 || SameBridge(own->second, hold)) continue;
			for (uint8_t peer: own->second.members) signedHolds |= SeatBit(peer);
			last = std::max(last, votes->first.first);
		}
		uint32_t named = 0;
		for (uint8_t peer: hold.members) named |= SeatBit(peer);
		const uint32_t held = PeerHeldSeatsAt(last) | signedHolds;
		const uint32_t local = SeatBit(m_Config.localPeerId);
		if (signing && (held & local) != 0 && (named & ~local) != 0) return true;
		return (PeerRetainedSeatsAt(last) & ~(held | named)) == 0;
	}

	bool NetLockstepCoordinator::VotedOtherPeerReturn(const NetHostMigrationMessage& proposal) const {
		if (proposal.preparedFrame != 1) return false;
		uint32_t named = 0;
		for (uint8_t peer: proposal.members) named |= SeatBit(peer);
		for (auto votes = m_PeerBridgeVotes.lower_bound({m_Stats.nextFrame, 0}); votes != m_PeerBridgeVotes.end(); ++votes) {
			if (votes->first.second != 1 || votes->first.first == proposal.frame || m_PeerBridgeCertificates.contains(votes->first)) continue;
			const auto own = votes->second.find(m_Config.localPeerId);
			if (own == votes->second.end()) continue;
			for (uint8_t peer: own->second.members) if ((named & SeatBit(peer)) != 0) return true;
		}
		return false;
	}

	void NetLockstepCoordinator::NotePeerYield(const NetHostMigrationMessage& hold, uint32_t voters, uint64_t nowMs) {
		const uint32_t local = SeatBit(m_Config.localPeerId);
		const uint32_t others = PeerFrameMembersAt(hold.frame) & PeerRetainedSeatsAt(hold.frame) & ~local;
		// A seat every other seat held, holding none of them, is no half of a split for the directory to rule: its play past the hold
		// is its own, so it leaves it rather than hold the others for the receipts their hold withholds. Direct play's heal decides.
		if (!m_Config.frameTieReferee || hold.preparedFrame != 0 || m_Playback || !IsRunning() || std::find(hold.members.begin(), hold.members.end(), m_Config.localPeerId) == hold.members.end() ||
		    others == 0 || (voters & others) != others || (PeerHeldSeatsAt(std::max(hold.frame, m_Stats.nextFrame)) & others) != 0 ||
		    (m_PeerYieldFrame && *m_PeerYieldFrame <= hold.frame)) return;
		m_PeerYieldFrame = hold.frame;
		DiagnosticLine() << "[net-peer-yield] local=" << static_cast<int>(m_Config.localPeerId) << " frame=" << hold.frame << " next=" << m_Stats.nextFrame
		    << " displayed=" << m_LastCompletedSimulationTick.value_or(0) << " signers=" << voters << " now_ms=" << nowMs << std::endl;
	}

	bool NetLockstepCoordinator::ProposePeerBridge(uint64_t frame, uint64_t nowMs, bool returningOnly) {
		// A seat leaving its play for the others' hold of it holds none of them.
		if (m_PeerYieldFrame) return false;
		if (!UsesPeerFrameGroups() || WaitsForPlacement() || frame != m_Stats.nextFrame) return false;
		// Under the details lever, a consumer past its bound whose hold this function never reaches says why, once per frame.
		const auto exited = [&](const char* why) {
			if (ReturnInputDetails() && m_ConsumerWaitingFrame == frame && nowMs >= m_ConsumerWaitStartMs &&
			    nowMs - m_ConsumerWaitStartMs >= static_cast<uint64_t>(std::ceil(m_Config.slowPlayerBoundTicks * m_Config.simTickMs)) && m_ExitedHoldFrame != frame) {
				m_ExitedHoldFrame = frame;
				DiagnosticLine() << "[net-peer-hold-exit] local=" << static_cast<int>(m_Config.localPeerId) << " frame=" << frame << " why=" << why << " now_ms=" << nowMs << std::endl;
			}
			return false;
		};
		// A lagging reader can receive its own hold before the last few preceding inputs. Fetch that committed tail;
		// it must not turn its stale display clock into a new hold of the survivor whose record it is catching up to.
		if (PeerFrameCatchUpActive()) for (const auto& [key, kept]: m_PendingPeerDecisions) {
			const auto& hold = kept.first;
			if (IsHoldDecision(key.second) && key.first > frame && std::find(hold.members.begin(), hold.members.end(), m_Config.localPeerId) != hold.members.end()) {
				RequestPeerCommittedTail(nowMs);
				return exited("own hold pending");
			}
		}
		const bool neutralParticipant = !IsLocalPrivateReaderAt(frame) && IsSeatReclaimGap(m_Config.localPeerId, frame);
		auto sources = m_PeerSourceInputs.find(frame);
		// Activation restores participation before the certified neutral input interval ends. That participant still holds a
		// missing sender at its displayed bound; requiring its own human sample makes the correctly empty interval a deadlock.
		if (sources == m_PeerSourceInputs.end()) {
			if (!neutralParticipant) return exited("no sources");
			sources = m_PeerSourceInputs.try_emplace(frame).first;
		} else if (!sources->second.contains(m_Config.localPeerId) && !neutralParticipant) return exited("no own input");
		auto proposal = PeerFrameMessage(NetHostMigrationMessageType::PeerBridge);
		std::map<uint8_t, uint64_t> returnCatchUpTicks;
		const auto returnLinkDelay = [&](uint8_t peer, uint64_t at) {
			const uint16_t current = InputDelayAt(peer, at);
			if (!m_Config.adaptiveInputDelay) return static_cast<uint32_t>(current);
			const auto [wire, route] = PeerFrameRoute(peer);
			if (!wire || !wire->IsPeerPingMeasured(route)) return static_cast<uint32_t>(current);
			auto& link = m_DelayEstimators[peer];
			link.Observe(nowMs, wire->GetPeerPingMs(route), true);
			return std::max<uint32_t>(current, std::min<uint32_t>(NetLockstepCodec::c_MaxPeerInputDelayFrames,
			    link.RequiredFrames(m_Config.simTickMs, m_Config.matchConfig.inputDelayFrames)));
		};
		proposal.frame = proposal.boundary = frame;
		proposal.connectedMask = 0;
		proposal.preparedFrame = 0;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if (m_RemovedPeers.contains(peer) || IsSeatReleased(peer)) continue;
			const auto bridge = m_PeerBridges.find(peer);
			const bool held = bridge != m_PeerBridges.end() && frame >= bridge->second.fromFrame && (!bridge->second.returnFrame || frame < *bridge->second.returnFrame);
			if (held) continue;
			const auto ended = m_PeerRoundEnds.find(peer);
			if (ended != m_PeerRoundEnds.end() && !ended->second.closedLink && frame >= ended->second.frame) {
				proposal.members.push_back(peer);
				continue;
			}
			if (m_PeerOtherContinuations.contains(peer) || PeerMissingInputReceipt(peer, frame)) proposal.members.push_back(peer);
			else if (frame < EffectiveStartOf(peer) || IsSeatReclaimGap(peer, frame) || sources->second.contains(peer)) proposal.connectedMask |= SeatBit(peer);
			else proposal.members.push_back(peer);
		}
		// A member that already accepted an input this seat lacks plays the continuation that input made: a hold of its owner here
		// holds that witness too, alone if it must, and the split is arbitrated as any other.
		if (const uint32_t witnesses = PeerReplacedInputWitnesses(frame, proposal.members, proposal.connectedMask & ~SeatBit(m_Config.localPeerId))) {
			proposal.connectedMask &= ~witnesses;
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if ((witnesses & SeatBit(peer)) != 0) proposal.members.push_back(peer);
		}
		// A machine that is losing its buffered lead can volunteer its own future cutoff before its survivors wait. Its local
		// clocks and compute samples only propose: the same playing voters and common prefix still certify the hold.
		bool ownYield = false;
		double ownCost = 0;
		if (proposal.members.empty() && (m_FrameGroupMembers & SeatBit(m_Config.localPeerId)) != 0 &&
		    !IsSeatReclaimGap(m_Config.localPeerId, frame) && m_TickCosts.size() >= c_FirstCapacityTicks &&
		    m_Stats.localComputeDebtMs >= m_Config.simTickMs && m_LastCompletedSimulationTick) {
			const auto& costs = m_TickCosts;
			const double shortest = *std::min_element(costs.end() - c_FirstCapacityTicks, costs.end());
			const double capacity = shortest > 0 ? 1000.0 / shortest : 1000.0;
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
				if (peer == m_Config.localPeerId || (m_FrameGroupMembers & SeatBit(peer)) == 0) continue;
				const auto rates = PeerAppliedRate(peer);
				if (!rates) continue;
				const auto [other, own] = *rates;
				if (!SlowAgainst(capacity, other) || !SlowAgainst(own, other)) continue;
				const auto heard = m_PeerAppliedAtMs.find(peer);
				const auto [wire, route] = PeerFrameRoute(peer);
				const uint64_t freshness = wire && wire->IsPeerPingMeasured(route) ? wire->GetPeerPingMs(route) + c_NetPeerHeartbeatMs : c_NetPeerHeartbeatMs;
				if (heard == m_PeerAppliedAtMs.end() || nowMs > heard->second + freshness) continue;
				ownCost = other - own;
				ownYield = true;
				proposal.members = {m_Config.localPeerId};
				proposal.connectedMask &= ~SeatBit(m_Config.localPeerId);
				break;
			}
		}
		if (returningOnly && !proposal.members.empty() && !ownYield) return exited("returning only");
		if (proposal.members.empty()) {
			// A return follows a capture already agreed ahead, so the new voter reads that capture from the committed tail.
			for (const auto& [key, votes]: m_PeerBridgeVotes) if (key.second == 4 && key.first >= frame && votes.contains(m_Config.localPeerId)) return exited("own return vote pending");
			if (m_FrameGroupMembers == 0 || std::countr_zero(m_FrameGroupMembers) + 1 != m_Config.localPeerId) return exited("not the lowest member");
			for (const auto& [peer, bridge]: m_PeerBridges) {
				if (!bridge.committedHold || bridge.returnFrame || peer == m_Config.localPeerId) continue;
				const uint32_t delay = returnLinkDelay(peer, frame);
				const auto [wire, route] = PeerFrameRoute(peer);
				const uint64_t heard = LastAuthenticatedTraffic(peer);
				const auto applied = m_PeerAppliedThrough.find(peer);
				if (nowMs < heard || nowMs - heard > 1000 || applied == m_PeerAppliedThrough.end()) continue;
				// A pre-outage sample can still be inside the buffered runway.
				// Require the reader to have displayed certified bridge history
				// before admitting its next producible input again.
				if (applied->second < bridge.fromFrame) continue;
				// A live opposing half also reports advancing ticks. Only a reader that has actually applied this branch's
				// held history can return without an image; an unresolved split keeps playing with its existing local holds.
				const auto confirmation = m_PeerAppliedPrefixes.find(peer);
				if (confirmation == m_PeerAppliedPrefixes.end() || confirmation->second.first < bridge.fromFrame ||
				    !m_MigrationHistory.contains(confirmation->second.first) ||
				    confirmation->second.second != PeerAppliedFramePrefix(confirmation->second.first)) continue;
				// Compare displayed progress; the remaining trail fits the return's capped neutral interval.
				const uint64_t displayed = m_LastCompletedSimulationTick.value_or(frame - 1);
				const uint64_t allowance = std::max<uint64_t>(delay + m_Config.slowPlayerBoundTicks,
				    static_cast<uint64_t>(std::ceil(c_NetSeatCatchUpAllowanceMs / m_Config.simTickMs)));
				if (displayed > applied->second && displayed - applied->second > allowance) continue;
				// A seat returns once its catch-up shows headroom: a machine slower than this one would only fall behind and be held again.
				if (OwnCapacityTps() > 0) {
					uint32_t lagRttMs = m_Stats.peers[peer].pingMs;
					if (wire && wire->IsPeerPingMeasured(route)) lagRttMs = std::max(lagRttMs, wire->GetPeerPingMs(route));
					// What the seat applied when it reported, this machine displayed one round trip before: its pace is compared with that.
					const auto rates = PeerAppliedRate(peer, lagRttMs);
					if (!rates) continue;
					const auto [rate, own] = *rates;
					// A seat that applies keeps reporting within a heartbeat; an older report is a seat that has stopped.
					const auto reported = m_PeerAppliedAtMs.find(peer);
					const uint64_t reportAgeMs = reported != m_PeerAppliedAtMs.end() && nowMs > reported->second ? nowMs - reported->second : 0;
					if (own <= 0 || reportAgeMs > c_NetPeerHeartbeatMs) continue;
					// It must also close its trail by its first live frame: one still behind there is late at once and held again. A seat that keeps
					// up still trails by the round trip its tail and its report take and by its last report's age; that lag is not trail to close.
					const uint64_t lag = static_cast<uint64_t>(std::ceil((lagRttMs + reportAgeMs) / m_Config.simTickMs));
					const uint64_t behind = displayed > applied->second ? displayed - applied->second : 0;
					const uint64_t trail = behind > lag ? behind - lag : 0;
					const uint64_t horizon = m_LastQueuedTargetFrame != UINT64_MAX && m_LastQueuedTargetFrame >= displayed ? m_LastQueuedTargetFrame + 1 - displayed : 0;
					const double toLive = static_cast<double>(horizon + delay + trail + m_Config.slowPlayerBoundTicks);
					const bool closes = trail <= m_Config.slowPlayerBoundTicks || (rate - own) * toLive >= static_cast<double>(trail - m_Config.slowPlayerBoundTicks) * own;
					// Slow means slower than the round, never slower than its nominal rate: a catch-up burst above it is not the bar.
					const double bar = std::min(own, 1000.0 / m_Config.simTickMs);
					if (rate < bar - (bar >= 59.5 ? 0.5 : bar / 10.0) || !closes) {
						if (m_SlowMachineHolds.insert(peer).second)
							DiagnosticLine() << "[net-lockstep] slow machine peer " << static_cast<int>(peer) << " stays held: it applied " << rate
							                 << " ticks/s against this machine's " << own << " displayed with " << trail << " ticks to close" << std::endl;
						continue;
					}
					// The tail's transit is real display lag when input production resumes. Only the report's trip and age
					// are observation lag. Give the signed neutral interval enough time to close that remaining display lag
					// at the measured surplus; it must not become every later input's permanent delay.
					const uint64_t observationLag = static_cast<uint64_t>(std::ceil((lagRttMs / 2.0 + reportAgeMs) / m_Config.simTickMs));
					const uint64_t displayTrail = behind > observationLag ? behind - observationLag : 0;
					if (displayTrail && rate > own) {
						const double closingTicks = std::ceil(static_cast<double>(displayTrail) * own / (rate - own));
						if (!std::isfinite(closingTicks) || closingTicks > NetLockstepCodec::c_MaxFutureFrameSkew) continue;
						returnCatchUpTicks[peer] = static_cast<uint64_t>(closingTicks);
					}
				}
				proposal.members.push_back(peer); proposal.connectedMask |= SeatBit(peer);
			}
			if (proposal.members.empty()) return exited("no members");
			proposal.preparedFrame = 1;
			if (VotedOtherPeerReturn(proposal) || frame > UINT64_MAX - m_Config.slowPlayerBoundTicks) return exited("voted another return");
			proposal.frame = frame + m_Config.slowPlayerBoundTicks;
			// The activation can follow another return already agreed ahead of this prepared frame. Keep that seat in the
			// later group, and include its displayed prefix and vote lead, even while it is still in its neutral interval now.
			uint32_t returning = 0;
			for (uint8_t peer: proposal.members) returning |= SeatBit(peer);
			bool settled = false;
			for (unsigned pass = 0; pass < NetMatchConfigUtil::c_MaxPeerCount + 1; ++pass) {
				uint32_t members = PeerFrameMembersAt(proposal.frame);
				members |= returning;
				for (const auto& [peer, bridge]: m_PeerBridges)
					if (bridge.returnFrame && *bridge.returnFrame <= proposal.frame && !m_RemovedPeers.contains(peer) && !IsSeatReleased(peer)) members |= SeatBit(peer);
				uint64_t nextPrepared = frame, nextLead = m_Config.slowPlayerBoundTicks;
				proposal.boundary = m_LastCompletedSimulationTick.value_or(frame - 1);
				if (m_LastQueuedTargetFrame != UINT64_MAX) nextPrepared = std::max(nextPrepared, m_LastQueuedTargetFrame + 1);
				for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
					if (peer == m_Config.localPeerId || (members & ~returning & SeatBit(peer)) == 0) continue;
					const auto applied = m_PeerAppliedThrough.find(peer);
					if (applied == m_PeerAppliedThrough.end() || applied->second < m_Config.startFrame) return exited("applied unknown");
					proposal.boundary = std::min(proposal.boundary, applied->second);
					nextPrepared = std::max(nextPrepared, m_Stats.peers[peer].reportedNextFrame);
					uint64_t rtt = m_Stats.peers[peer].pingMs;
					const auto [wire, route] = PeerFrameRoute(peer);
					if (wire && wire->IsPeerPingMeasured(route)) rtt = std::max<uint64_t>(rtt, wire->GetPeerPingMs(route));
					else if (const auto path = m_PeerReceiptDelaySamples.find(peer); path != m_PeerReceiptDelaySamples.end()) rtt = std::max<uint64_t>(rtt, path->second.P95Ms());
					nextLead = std::max<uint64_t>(nextLead, static_cast<uint64_t>(std::ceil((rtt + c_NetPeerVoteResendMs) / m_Config.simTickMs)) + m_Config.slowPlayerBoundTicks);
				}
				if (nextPrepared > UINT64_MAX - nextLead) return exited("lead overflow");
				const uint64_t activation = std::max(proposal.frame, nextPrepared + nextLead);
				if (members == proposal.connectedMask && activation == proposal.frame) { settled = true; break; }
				proposal.connectedMask = members; proposal.frame = activation;
			}
			if (!settled) return exited("not settled");
		}
		// Another continuation's members are held at this frame: no deadline or cutoff ahead applies to a history this group left.
		const bool otherContinuation = !proposal.members.empty() && std::all_of(proposal.members.begin(), proposal.members.end(), [&](uint8_t peer) {
			return m_PeerOtherContinuations.contains(peer);
		});
		const bool declaredDeparture = !proposal.members.empty() && std::all_of(proposal.members.begin(), proposal.members.end(), [&](uint8_t peer) {
			const auto ended = m_PeerRoundEnds.find(peer);
			return ended != m_PeerRoundEnds.end() && !ended->second.closedLink && frame >= ended->second.frame;
		});
		// A machine slower than the others is held when it has cost them more than the round allows, not after a bound none of its
		// late inputs reaches: waiting the bound on each of them would let it slow every other seat for good.
		double slowCost = ownCost;
		const bool slowMachine = ownYield || (proposal.preparedFrame == 0 && !proposal.members.empty() && std::all_of(proposal.members.begin(), proposal.members.end(),
		    [&](uint8_t peer) { return CostsPastTolerance(peer, frame, nowMs, &slowCost); }));
		// With another seat voting, a seat that still feeds - its inputs arrive within the bound - is never held at the frame being waited
		// on: its input reaches some voter before the proposal does, the voters stop at different frames, and no majority forms. So the
		// lowest seat that plays on holds it at a frame past its inputs in flight, agreed ahead, once it costs more than the round allows;
		// the others wait for that hold. A seat silent past the bound stops every voter at one frame and is held there.
		const bool otherVoter = (proposal.connectedMask & ~SeatBit(m_Config.localPeerId)) != 0;
		const uint64_t feedBoundMs = static_cast<uint64_t>(std::ceil(m_Config.slowPlayerBoundTicks * m_Config.simTickMs));
		const bool feeding = ownYield || (proposal.preparedFrame == 0 && !proposal.members.empty() && !declaredDeparture &&
		    std::all_of(proposal.members.begin(), proposal.members.end(), [&](uint8_t peer) {
			    const auto& arrivals = m_Stats.peers[peer].arrivals;
			    return !arrivals.empty() && nowMs <= arrivals.back().first + feedBoundMs;
		    }));
		const bool cooperativeAhead = proposal.members.size() == 1 &&
		    PeerGroupHasAuthority(proposal.connectedMask | SeatBit(proposal.members.front()), proposal.frame);
		const bool localDeadline = PeerConsumerHoldDue(frame, nowMs);
		// Another continuation's member that still feeds is held where the voters agree: each holding it at its own frame would leave
		// the voters on different histories. A seat whose own bound has passed on the frame holds it there and plays on.
		const bool agreedContinuation = otherContinuation && otherVoter && feeding && !localDeadline;
		// Newer arrivals cannot extend the consumer's deadline for an older missing input.
		const bool heldAhead = ((otherVoter || cooperativeAhead) && feeding && (ownYield || !localDeadline) && !otherContinuation) || agreedContinuation;
		if (heldAhead) {
			if ((!slowMachine && !agreedContinuation) || (!ownYield && std::countr_zero(proposal.connectedMask) + 1 != m_Config.localPeerId)) return exited("not a slow-machine or agreed continuation proposer");
			for (const auto& [key, votes]: m_PeerBridgeVotes) {
				if (!IsHoldDecision(key.second) || key.first <= frame || m_PeerBridgeCertificates.contains(key)) continue;
				if (const auto own = votes.find(m_Config.localPeerId); own != votes.end())
					for (uint8_t peer: own->second.members) if (std::find(proposal.members.begin(), proposal.members.end(), peer) != proposal.members.end()) return exited("own vote names a member");
			}
			uint64_t inFlight = frame;
			uint32_t rttMs = 0;
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
				if (peer == m_Config.localPeerId) continue;
				if (std::find(proposal.members.begin(), proposal.members.end(), peer) != proposal.members.end())
					inFlight = std::max<uint64_t>(inFlight, m_Stats.peers[peer].highestTargetFrame + 1);
				uint32_t rtt = m_Stats.peers[peer].pingMs;
				const auto [wire, route] = PeerFrameRoute(peer);
				if (wire && wire->IsPeerPingMeasured(route)) rtt = std::max(rtt, wire->GetPeerPingMs(route));
				rttMs = std::max(rttMs, rtt);
			}
			// Past its inputs in flight by the round trip the proposal and its votes take and the bound, as a return is placed.
			proposal.boundary = m_LastCompletedSimulationTick.value_or(frame - 1);
			const bool needsOwnCoSign = !PeerGroupMayHold(proposal.connectedMask, frame);
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
				if (peer == m_Config.localPeerId || ((proposal.connectedMask & SeatBit(peer)) == 0 &&
				    !(needsOwnCoSign && std::find(proposal.members.begin(), proposal.members.end(), peer) != proposal.members.end()))) continue;
				const auto applied = m_PeerAppliedThrough.find(peer);
				if (applied == m_PeerAppliedThrough.end() || applied->second < m_Config.startFrame) return exited("applied unknown at boundary");
				proposal.boundary = std::min(proposal.boundary, applied->second);
			}
			if (ownYield) {
				// The producer signs and fences its first unqueued future input itself. Do not add another bound of slow inputs before
				// that cutoff: those inputs would consume the surviving players' runway while the voluntary certificate is in flight.
				proposal.frame = std::max(frame + 1, m_LastQueuedTargetFrame == UINT64_MAX ? frame + 1 : m_LastQueuedTargetFrame + 1);
				for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if ((proposal.connectedMask & SeatBit(peer)) != 0)
					proposal.frame = std::max(proposal.frame, m_Stats.peers[peer].reportedNextFrame + 1);
			} else proposal.frame = inFlight + static_cast<uint64_t>(std::ceil(rttMs / m_Config.simTickMs)) + m_Config.slowPlayerBoundTicks;
		}
		// A declared Leave has already ended its input; it needs no silence wait at that boundary.
		if (proposal.preparedFrame == 0 && !declaredDeparture && !heldAhead && !(slowMachine && !otherVoter) && !(otherContinuation && !otherVoter)) {
			const uint64_t boundMs = static_cast<uint64_t>(std::ceil(m_Config.slowPlayerBoundTicks * m_Config.simTickMs));
			// The startup consumer has no established buffered runway to spend.
			// Its first missing inputs get the full bound from the actual wait.
			const uint64_t displayed = m_LastCompletedSimulationTick.value_or(m_Config.startFrame - 1);
			if (displayed < m_Stats.effectiveStartFrame + m_Config.slowPlayerBoundTicks &&
			    (m_ConsumerWaitingFrame != frame || nowMs < m_ConsumerWaitStartMs || nowMs - m_ConsumerWaitStartMs < boundMs)) return exited("startup wait");
			// A local hold needs no advance reservation.
			uint64_t agreementMs = 0;
			// The consumer's bound is already on the deadline below. Charge this co-sign its round trip and a poll, not the bound
			// twice: an early speculative vote would fence a healthy returner's first input while it is still normally in flight.
			if (!PeerGroupMayHold(proposal.connectedMask, proposal.frame)) for (uint8_t peer: proposal.members) {
				uint64_t rtt = m_Stats.peers[peer].pingMs;
				const auto [wire, route] = PeerFrameRoute(peer);
				if (wire && wire->IsPeerPingMeasured(route)) rtt = std::max<uint64_t>(rtt, wire->GetPeerPingMs(route));
				agreementMs = std::max(agreementMs, rtt + static_cast<uint64_t>(std::ceil(m_Config.simTickMs / 2)));
			}
			// Proposal, reciprocal-input loss and local certification spend the same displayed deadline.
			const auto deadline = PeerInputDeadlineMs(frame);
			if (!deadline || nowMs + agreementMs < *deadline) return exited("before the deadline");
		}
		// Past the bound, a hold this seat does not propose names why under the details lever, once per frame.
		const auto refused = [&](const char* why) {
			if (ReturnInputDetails() && m_ConsumerWaitingFrame == frame && nowMs >= m_ConsumerWaitStartMs &&
			    nowMs - m_ConsumerWaitStartMs >= static_cast<uint64_t>(std::ceil(m_Config.slowPlayerBoundTicks * m_Config.simTickMs)) && m_RefusedHoldFrame != frame) {
				m_RefusedHoldFrame = frame;
				std::string members;
				for (uint8_t peer: proposal.members) members += std::to_string(peer);
				const auto sources = m_PeerSourceInputs.find(proposal.frame);
				std::string seats;
				for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer)
					seats += " seat" + std::to_string(peer) + "=" + std::to_string(sources != m_PeerSourceInputs.end() && sources->second.contains(peer)) +
					         std::to_string(PeerMissingInputReceipt(peer, proposal.frame)) + std::to_string(IsSeatReclaimGap(peer, proposal.frame)) +
					         std::to_string(m_PeerOtherContinuations.contains(peer)) + std::to_string(EffectiveStartOf(peer) <= proposal.frame);
				DiagnosticLine() << "[net-peer-hold-refused] local=" << static_cast<int>(m_Config.localPeerId) << " frame=" << frame << " hold_frame=" << proposal.frame
				    << " members=" << members << " connected=" << proposal.connectedMask << " why=" << why << " conflict=" << PeerBridgeHasInputConflict(proposal)
				    << " may_hold=" << PeerGroupMayHold(proposal.connectedMask, proposal.frame) << " history=" << m_MigrationHistory.contains(proposal.frame - 1)
				    << " next=" << m_Stats.nextFrame << " boundary=" << proposal.boundary << seats << " now_ms=" << nowMs << std::endl;
			}
			return false;
		};
		const uint32_t proposers = proposal.preparedFrame == 1 ? m_FrameGroupMembers : proposal.connectedMask;
		if (proposers == 0) return refused("no proposer");
		proposal.successorPeerId = static_cast<uint8_t>(std::countr_zero(proposers) + 1);
		const NetHash32 prefix = frame == m_Config.startFrame ? NetHash32{} : proposal.preparedFrame != 0 || heldAhead ? PeerAppliedFramePrefix(proposal.boundary) : PeerFramePrefix(frame - 1);
		proposal.bytes.assign(prefix.begin(), prefix.end());
		if (proposal.preparedFrame == 1) for (uint8_t peer: proposal.members) {
			const uint64_t displayed = m_LastCompletedSimulationTick.value_or(frame - 1);
			const uint64_t applied = m_PeerAppliedThrough.at(peer);
			const uint32_t trail = static_cast<uint32_t>(displayed - std::min(displayed, applied));
			// The same link estimator sizes every live and returning sender. Catch-up belongs only to the temporary neutral interval.
			const uint32_t delay = returnLinkDelay(peer, proposal.frame);
			const uint32_t transit = delay;
			const uint64_t neutral = std::max<uint64_t>(delay + transit + trail + m_Config.slowPlayerBoundTicks,
			    returnCatchUpTicks.contains(peer) ? delay + returnCatchUpTicks.at(peer) : 0);
			if (neutral > NetLockstepCodec::c_MaxFutureFrameSkew) return false;
			PutSize(proposal.bytes, delay); PutSize(proposal.bytes, neutral);
			PutSize(proposal.bytes, std::max<uint32_t>(1, m_Config.peerIncarnations[peer]));
		}
		proposal.totalBytes = static_cast<uint32_t>(proposal.bytes.size());
		if (!ValidatePeerBridge(proposal) && !(ownYield && CoSignsOwnHold(proposal))) return refused("invalid");
		if (HoldEmptiesTheRound(proposal, true)) return refused("empties the round");
		const PeerDecisionKey decision{proposal.frame, DecisionKind(proposal)};
		if (m_PeerBridgeCertificates.contains(decision)) return refused("certified");
		auto& votes = m_PeerBridgeVotes[decision];
		const auto own = votes.find(m_Config.localPeerId);
		// A matching proposal is still pending until its certificate is installed.
		// Reporting it as progress makes AdvanceReadyFrames spin on the same frame
		// and prevents the wire from delivering the other voters' answers.
		if (own != votes.end()) {
			if (own->second.voterMask != 0 || !SameBridge(own->second, proposal) || !PeerGroupMayHold(proposal.connectedMask, proposal.frame))
				return refused(own->second.voterMask != 0 ? "own vote pending" : !SameBridge(own->second, proposal) ? "own vote differs" : "group may not hold");
		}
		if (VotedOtherPeerReturn(proposal)) return refused("voted another return");
		const bool invitation = proposal.preparedFrame == 0 && !ownYield && !PeerGroupMayHold(proposal.connectedMask, proposal.frame);
		// Asking the missing owner to consent is not a vote. Until it signs, a timely input must remain admissible: the owner
		// may already have produced it and be unable to promise a cutoff. Only actual signatures can fence or certify a hold.
		proposal.voterMask = invitation ? 0 : SeatBit(m_Config.localPeerId);
		votes.insert_or_assign(m_Config.localPeerId, proposal);
		if (proposal.preparedFrame == 1) {
			size_t offset = NetHash32{}.size();
			for (uint8_t peer: proposal.members) {
				uint32_t delay = 0, neutral = 0, incarnation = 0;
				if (!TakeSize(proposal.bytes, offset, delay) || !TakeSize(proposal.bytes, offset, neutral) || !TakeSize(proposal.bytes, offset, incarnation)) return false;
				DiagnosticLine() << "[net-peer-return-runway] peer=" << static_cast<int>(peer) << " activation=" << proposal.frame
				    << " delay_ticks=" << delay << " neutral_through=" << proposal.frame + neutral << std::endl;
			}
		}
		if (invitation && ReturnInputDetails()) DiagnosticLine() << "[net-peer-hold-invitation] local=" << static_cast<int>(m_Config.localPeerId)
		    << " peer=" << static_cast<int>(proposal.members.front()) << " frame=" << proposal.frame << " now_ms=" << nowMs << std::endl;
		if (proposal.preparedFrame == 0 && !invitation) {
			for (uint8_t peer: proposal.members) {
				m_PeerRejectedInputs[proposal.frame] |= SeatBit(peer);
				const auto& stats = m_Stats.peers[peer];
				const auto ended = m_PeerRoundEnds.find(peer);
				const bool declared = ended != m_PeerRoundEnds.end() && !ended->second.closedLink && frame >= ended->second.frame;
				const bool sourcePresent = sources->second.contains(peer);
				const char* cause = declared ? "declared_leave" : m_PeerOtherContinuations.contains(peer) ? "other_continuation" : PeerMissingInputReceipt(peer, frame) ? (HasPeerFrameRoute(peer) ? "receipt_missing" : "link_closed") :
				    slowMachine ? "slow_machine_capacity" : sourcePresent ? "input_acceptance" : "input_missing";
				const uint64_t heard = LastAuthenticatedTraffic(peer);
				const uint64_t silence = nowMs >= heard ? nowMs - heard : 0;
				m_HoldFacts[peer] = {proposal.frame, silence, m_Config.slowPlayerBoundTicks * m_Config.simTickMs, cause};
				const auto [wire, route] = PeerFrameRoute(peer);
				const auto queue = wire ? wire->GetPeerQueueStats(route) : NetTransportQueueStats{};
				const auto receipt = m_PeerReceiptDelaySamples.find(peer);
				DiagnosticLine() << "[net-peer-hold-trigger] local=" << static_cast<int>(m_Config.localPeerId) << " peer=" << static_cast<int>(peer)
				    << " frame=" << proposal.frame << " ai_frame=" << proposal.frame
				    << " observed_frame=" << frame << " now_ms=" << nowMs << " cause=" << cause << " ahead=" << heldAhead
				    << " first_missing_ms=" << m_FirstMissingMs << " last_heard_ms=" << heard << " last_progress_ms=" << stats.lastProgressMs
				    << " displayed=" << m_LastCompletedSimulationTick.value_or(0) << " consumer_at_ms=" << m_PeerConsumerAtMs.value_or(0)
				    << " pace_start_ms=" << m_PeerPaceStartMs.value_or(0) << " pace_start_frame=" << m_PeerPaceStartFrame
				    << " waiting_frame=" << m_ConsumerWaitingFrame.value_or(0) << " waiting_since_ms=" << m_ConsumerWaitStartMs
				    << " received_through=" << stats.highestTargetFrame << " accepted_through=" << stats.acceptedThroughFrame
				    << " input_voters=" << PeerInputVoters(frame, peer) << " delay_ticks=" << InputDelayAt(peer, frame)
				    << " bound_ticks=" << m_Config.slowPlayerBoundTicks << " transport_rtt_ms=" << (wire ? wire->GetPeerPingMs(route) : 0)
				    << " transport_measured=" << (wire && wire->IsPeerPingMeasured(route)) << " receipt_p95_ms=" << (receipt == m_PeerReceiptDelaySamples.end() ? 0 : receipt->second.P95Ms())
				    << " reliable_bytes=" << queue.reliableBytes << " queue_ms=" << queue.queueMs << " send_rate=" << queue.sendRateBytesPerSecond
				    << " lost_tps=" << slowCost << " own_capacity_tps=" << OwnCapacityTps() << " route=" << (wire ? wire->GetConnectedRoute(route) : "none") << std::endl;
				if (!slowMachine) { m_SlowMachineHolds.erase(peer); continue; }
				m_SlowMachineHolds.insert(peer);
				DiagnosticLine() << "[net-lockstep] slow machine peer " << static_cast<int>(peer) << " at frame " << proposal.frame << " cost " << slowCost
				                 << " ticks/s against this machine's " << OwnCapacityTps() << "; the AI takes its seat" << std::endl;
			}
			if (ReturnInputDetails()) {
				DiagnosticLine detail;
				detail << "[net-peer-hold-vote] local=" << static_cast<int>(m_Config.localPeerId) << " frame=" << frame
				    << " displayed=" << m_LastCompletedSimulationTick.value_or(0) << " reference=" << static_cast<int>(m_PeerPaceReference)
				    << " now_ms=" << nowMs << " members=" << proposal.connectedMask;
				for (uint8_t peer: proposal.members) detail << " missing=" << static_cast<int>(peer) << ":" << m_Stats.peers[peer].highestTargetFrame
				    << "/" << m_PeerAppliedThrough[peer];
			}
		}
		SendPeerFrameMessage(proposal);
		TryCommitPeerBridge(proposal.frame, nowMs, DecisionKind(proposal));
		return proposal.preparedFrame == 0 && proposal.frame == frame && m_PeerBridgeCertificates.contains(decision);
	}

	void NetLockstepCoordinator::TryCommitPeerBridge(uint64_t frame, uint64_t nowMs, uint8_t kind) {
		const auto found = m_PeerBridgeVotes.find({frame, kind});
		if (found == m_PeerBridgeVotes.end() || found->second.empty() || m_PeerBridgeCertificates.contains({frame, kind})) return;
		const auto own = found->second.find(m_Config.localPeerId);
		if (own == found->second.end() || own->second.voterMask != SeatBit(m_Config.localPeerId)) return;
		const auto& proposal = own->second;
		const uint32_t required = PeerDecisionRequiredVoters(proposal);
		uint32_t voters = 0;
		std::vector<std::vector<uint8_t>> signedVotes;
		for (const auto& [peer, vote]: found->second) {
			if (vote.voterMask != SeatBit(peer) || !SameBridge(vote, proposal)) continue;
			std::vector<uint8_t> bytes;
			if (!NetHostMigrationCodec::Encode(vote, m_Config.migrationKey, bytes)) return;
			voters |= SeatBit(peer); signedVotes.push_back(std::move(bytes));
		}
		if (proposal.preparedFrame == 0) {
			// A direct host may consent to its own hold without transferring directory authority.
			const bool coSigned = (voters & required) == required && PeerGroupHasAuthority(voters, frame);
			if ((voters & proposal.connectedMask) == 0 || (!PeerGroupMayHold(proposal.connectedMask, frame) && !coSigned)) return;
			if ((voters & required) != required) {
				// Each survivor installs its own hold at the bound. The other survivors' signatures can still be in flight on a
				// healthy long link; waiting a second round trip here would make their controls pay for the missing machine.
				// Another continuation's members are held at once where no other voter shares this group, or at the frame the voters
				// agreed ahead: this group no longer shares their history.
				const bool otherContinuation = std::all_of(proposal.members.begin(), proposal.members.end(), [&](uint8_t peer) { return m_PeerOtherContinuations.contains(peer); }) &&
				    ((proposal.connectedMask & ~SeatBit(m_Config.localPeerId)) == 0 || proposal.boundary < proposal.frame);
				if (frame != m_Stats.nextFrame || (!PeerConsumerHoldDue(frame, nowMs) && !otherContinuation)) return;
			}
		} else if ((voters & required) != required || (proposal.preparedFrame == 3 ? !PeerAdminCertificateValid(proposal, voters) : !PeerGroupHasAuthority(voters, frame))) return;
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

	uint32_t NetLockstepCoordinator::PeerDecisionRequiredVoters(const NetHostMigrationMessage& agreed) const {
		uint32_t required = agreed.connectedMask;
		if (agreed.preparedFrame == 1) for (uint8_t peer: agreed.members) required &= ~SeatBit(peer);
		// A seat the round holds at the decision's frame plays none of it: whether it voted decides nothing, so a voter that knows of the
		// hold certifies without it, and a seat held after voting cannot leave a decision certified on one peer and passed on another.
		if (agreed.preparedFrame != 3) {
			required &= ~PeerHeldSeatsAt(agreed.frame);
			// A certified neutral interval excludes the seat from voting until it plays again.
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer)
				if (IsSeatReclaimGap(peer, agreed.frame)) required &= ~SeatBit(peer);
		}
		return required;
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
		const uint32_t required = PeerDecisionRequiredVoters(agreed);
		// A hold is a surviving signer's bounded local record. Other decisions still require every playing voter. A missing
		// owner's signature alone cannot install a hold: at least one of the seats that continues must have signed it.
		const bool signedByRequired = agreed.preparedFrame == 0 ? (voters & agreed.connectedMask) != 0 : (voters & required) == required;
		return (agreed.preparedFrame != 4 || ValidatePeerCaptureBridge(agreed)) && offset == certificate.bytes.size() && voters == certificate.voterMask && signedByRequired &&
		    agreed.connectedMask == certificate.connectedMask && agreed.members == certificate.members && agreed.preparedFrame == certificate.preparedFrame &&
		    agreed.boundary == certificate.boundary && agreed.completeFrom == certificate.completeFrom && agreed.successorPeerId == certificate.successorPeerId;
	}

	void NetLockstepCoordinator::InstallPeerBridge(const NetHostMigrationMessage& certificate, uint64_t nowMs, bool fromCommittedTail) {
		if (m_PeerBridgeCertificates.contains({certificate.frame, DecisionKind(certificate)})) return;
		// CC_TEST_RETURN_INPUT_DETAILS names each decision this peer sets aside and why.
		const auto refused = [&](const char* reason) {
			if (std::string_view(reason) == "certificate") ++m_Stats.decisionsRefusedForAuthority;
			if (std::string_view(reason) == "not held") ++m_Stats.returnsRefusedNotHeld;
			if (ReturnInputDetails()) DiagnosticLine() << "[net-peer-decision-refused] local=" << static_cast<int>(m_Config.localPeerId)
				<< " frame=" << certificate.frame << " kind=" << static_cast<int>(DecisionKind(certificate)) << " reason=" << reason << " next=" << m_Stats.nextFrame
				<< " displayed=" << m_LastCompletedSimulationTick.value_or(0) << " now_ms=" << nowMs << std::endl;
		};
		// A decision that came before this peer can take it is kept for its frame: a later hold, the hold a return ends, or the
		// referee's answer to a tie this peer has not heard yet will let it in, and the frame waits for it rather than play without it.
		const auto keep = [&] {
			const PeerDecisionKey key{certificate.frame, DecisionKind(certificate)};
			if (certificate.frame >= m_Stats.nextFrame && certificate.frame - m_Stats.nextFrame <= NetLockstepCodec::c_MaxFutureFrameSkew && m_PendingPeerDecisions.size() < 64)
				m_PendingPeerDecisions.try_emplace(key, certificate, nowMs);
		};
		NetHostMigrationMessage agreed; uint32_t voters = 0;
		if (!DecodePeerBridgeCertificate(certificate, agreed, voters)) { refused("certificate"); return; }
		// Foreign decisions cannot govern this active continuation. In direct play that includes another group's migration: only the
		// heal decides between continuations.
		if (agreed.preparedFrame != 0 && (agreed.preparedFrame != 3 || !MigrationUsesDirectory()) &&
		    (agreed.connectedMask & SeatBit(m_Config.localPeerId)) == 0 &&
		    (PeerFrameMembersAt(agreed.frame) & SeatBit(m_Config.localPeerId)) != 0 && !IsLocalPrivateReaderAt(agreed.frame)) {
			auto current = agreed;
			current.connectedMask = PeerFrameMembersAt(agreed.frame);
			// Concurrent returns may omit a neutral participant, but never another playing electorate.
			if (agreed.preparedFrame != 1 || !IsSeatReclaimGap(m_Config.localPeerId, agreed.frame) ||
			    PeerDecisionRequiredVoters(current) != PeerDecisionRequiredVoters(agreed)) {
				refused("foreign continuation"); return;
			}
		}
		if (agreed.preparedFrame == 0 && (agreed.connectedMask & SeatBit(m_Config.localPeerId)) == 0)
			DiagnosticLine() << "[net-peer-foreign-hold] local=" << static_cast<int>(m_Config.localPeerId) << " frame=" << agreed.frame
			    << " next=" << m_Stats.nextFrame << " displayed=" << m_LastCompletedSimulationTick.value_or(0)
			    << " waiting=" << m_ConsumerWaitingFrame.value_or(UINT64_MAX) << " waiting_since=" << m_ConsumerWaitStartMs
			    << " input_voters=" << PeerInputVoters(agreed.frame, m_Config.localPeerId) << " local_source=" << m_LocalFrames.contains(agreed.frame)
			    << " now_ms=" << nowMs << std::endl;
		// A private reader plays the certified history it reads, whose record carries every hold of that history. A hold certified to it
		// directly may be another continuation's, in which the seat it holds still plays. The reader adopts it before it has shown that
		// frame unless the split's ruling names another group or, unruled, the record it has of that frame shows a held seat playing.
		// One ahead is judged again at its frame.
		if (agreed.preparedFrame == 0 && !fromCommittedTail && certificate.frame <= m_Stats.nextFrame && IsLocalPrivateReaderAt(agreed.frame)) {
			CheckPeerFrameTie(certificate.frame, agreed.connectedMask, nowMs);
			const auto ruling = m_PeerFrameTieWinners.find(PeerSplitEpoch(certificate.frame));
			const bool shown = (m_LastCompletedSimulationTick && *m_LastCompletedSimulationTick >= certificate.frame) ||
			    (m_LastDeliveredFrame && *m_LastDeliveredFrame >= certificate.frame) || (m_GrantedSimulationTick && *m_GrantedSimulationTick >= certificate.frame);
			bool contested = false;
			const auto& records = certificate.frame < m_Stats.nextFrame ? m_MigrationHistory : m_PeerCommittedTail;
			if (const auto record = records.find(certificate.frame); record != records.end()) {
				NetLockstepReadyFrame read;
				if (DecodeMigrationFrame(record->second, certificate.frame, read))
					for (uint8_t peer: agreed.members) contested = contested || read.remoteFrameCounts.contains(peer);
			}
			if (shown || (ruling != m_PeerFrameTieWinners.end() ? (ruling->second & agreed.connectedMask) == 0 : contested)) { refused("private reader"); return; }
		}
		// A round keeps a playing seat: a hold that would leave none is not taken, and the seat it names plays on alone. A reader still
		// takes the history it reads and the ruling half's hold: the hold set against that one is the stopped half's.
		if (agreed.preparedFrame == 0 && !fromCommittedTail && HoldEmptiesTheRound(agreed, false)) {
			const auto ruling = m_PeerFrameTieWinners.find(PeerSplitEpoch(certificate.frame));
			if (!IsLocalPrivateReaderAt(agreed.frame) || ruling == m_PeerFrameTieWinners.end() || (ruling->second & agreed.connectedMask) == 0) {
				refused("no playing seat"); return;
			}
		}
		if (agreed.preparedFrame == 1) {
			if (ReturnCrossesDeparture(agreed, m_PeerOwnershipChanges)) { refused("owner changed"); return; }
			for (uint8_t peer: agreed.members) if ((PeerRetainedSeatsAt(agreed.frame) & SeatBit(peer)) == 0) { refused("removed"); return; }
		}
		// A tie the referee decides is asked before the decision is set aside: its answer may already be there.
		if (agreed.preparedFrame != 0 && agreed.preparedFrame != 3 && !PeerGroupHasAuthority(voters, certificate.frame))
			CheckPeerFrameTie(certificate.frame, voters, nowMs);
		const PeerDecisionKey decision{certificate.frame, DecisionKind(certificate)};
		const uint32_t required = PeerDecisionRequiredVoters(agreed);
		const bool coSignedHold = (voters & required) == required && PeerGroupHasAuthority(voters, certificate.frame);
		if (agreed.preparedFrame == 3 ? !PeerAdminCertificateValid(agreed, voters) :
		    agreed.preparedFrame == 0 ? !PeerGroupMayHold(agreed.connectedMask, certificate.frame) && !coSignedHold : !PeerGroupHasAuthority(voters, certificate.frame)) {
			// A hold another group cannot show the authority of here, while this seat plays its own group at that frame, is that group's
			// local hold: it never stops this group's frame.
			if (agreed.preparedFrame == 0 && (agreed.connectedMask & SeatBit(m_Config.localPeerId)) == 0 &&
			    (PeerFrameMembersAt(agreed.frame) & SeatBit(m_Config.localPeerId)) != 0 && !IsLocalPrivateReaderAt(agreed.frame)) {
				NotePeerYield(agreed, voters, nowMs);
				refused("foreign continuation"); return;
			}
			m_AuthorityWaitSinceMs.try_emplace(decision, nowMs);
			refused("certificate");
			keep(); return;
		}
		if (const auto since = m_AuthorityWaitSinceMs.find(decision); since != m_AuthorityWaitSinceMs.end()) {
			m_Stats.longestAuthorityWaitMs = std::max(m_Stats.longestAuthorityWaitMs, nowMs >= since->second ? nowMs - since->second : 0);
			m_AuthorityWaitSinceMs.erase(since);
		}
		const bool heldAhead = agreed.preparedFrame == 0 && agreed.boundary < certificate.frame;
		if (agreed.preparedFrame == 0 && certificate.frame > m_Stats.nextFrame) { refused("ahead"); keep(); if (!heldAhead) RequestPeerCommittedTail(nowMs); return; }
		// No peer is past the frame of a hold agreed ahead when it is certified: every voter fenced that frame, and the held seat plays it
		// only on its own input there, which needs a receipt from a voter - and a voter that took that input could not vote the hold.
		if (heldAhead && certificate.frame < m_Stats.nextFrame) { refused("behind"); return; }
		if (agreed.preparedFrame != 0 && certificate.frame < m_Stats.nextFrame) {
			// Install a private return before its displayed tick, even if preparation has passed it.
			const auto own = m_PeerBridges.find(m_Config.localPeerId);
			const bool privateReader = own != m_PeerBridges.end() && own->second.committedHold && !own->second.returnFrame;
			if (agreed.preparedFrame != 1 || !privateReader ||
			    certificate.frame <= m_LastCompletedSimulationTick.value_or(0)) { refused(privateReader ? "displayed" : "behind"); return; }
		}
		const uint64_t prefixFrame = agreed.preparedFrame != 0 || heldAhead ? agreed.boundary : certificate.frame - 1;
		// Another seat's return whose boundary this seat has not yet played - a returner that started before it - is judged when it has.
		if (agreed.preparedFrame == 1 && prefixFrame >= m_Stats.nextFrame && prefixFrame >= m_Config.startFrame && !m_MigrationHistory.contains(prefixFrame) &&
		    std::find(agreed.members.begin(), agreed.members.end(), m_Config.localPeerId) == agreed.members.end()) {
			refused("boundary ahead"); keep(); return;
		}
		const NetHash32 prefix = certificate.frame == m_Config.startFrame ? NetHash32{} : agreed.preparedFrame != 0 || heldAhead ? PeerAppliedFramePrefix(prefixFrame) : PeerFramePrefix(prefixFrame);
		if ((agreed.preparedFrame != 4 && agreed.bytes.size() != prefix.size() + (agreed.preparedFrame == 1 ? 12 * agreed.members.size() : 0)) || agreed.bytes.size() < prefix.size() || !std::equal(prefix.begin(), prefix.end(), agreed.bytes.begin())) { refused("prefix"); return; }
		size_t returnOffset = prefix.size();
		std::map<uint8_t, std::tuple<uint16_t, uint64_t, uint32_t>> returnDelays;
		if (agreed.preparedFrame == 1) for (uint8_t peer: agreed.members) {
			const auto held = m_PeerBridges.find(peer);
			if (held == m_PeerBridges.end() || !held->second.committedHold || held->second.returnFrame) { refused("not held"); keep(); return; }
			uint32_t delay = 0, neutral = 0, incarnation = 0;
			if (!TakeSize(agreed.bytes, returnOffset, delay) || !TakeSize(agreed.bytes, returnOffset, neutral) || !TakeSize(agreed.bytes, returnOffset, incarnation) || incarnation == 0 ||
			    delay > NetLockstepCodec::c_MaxPeerInputDelayFrames || neutral < delay || neutral > NetLockstepCodec::c_MaxFutureFrameSkew || agreed.frame > UINT64_MAX - neutral) { refused("runway"); return; }
			returnDelays[peer] = {static_cast<uint16_t>(delay), agreed.frame + neutral, incarnation};
		}
		if (agreed.preparedFrame == 0 && certificate.frame < m_Stats.nextFrame && ReturnInputDetails())
			DiagnosticLine() << "[net-peer-hold-past] local=" << static_cast<int>(m_Config.localPeerId) << " frame=" << certificate.frame << " next=" << m_Stats.nextFrame
			    << " members_here=" << PeerFrameMembersAt(agreed.frame) << " connected=" << agreed.connectedMask << " voters=" << voters
			    << " held_here=" << PeerHeldSeatsAt(agreed.frame) << " retained=" << PeerRetainedSeatsAt(agreed.frame) << " resume=" << GetResumeFrame()
			    << " gap=" << IsSeatReclaimGap(m_Config.localPeerId, agreed.frame) << " now_ms=" << nowMs << std::endl;
		const uint32_t heldGroup = agreed.connectedMask & PeerRetainedSeatsAt(agreed.frame) & ~PeerHeldSeatsAt(agreed.frame);
		// A seat in its own return's neutral interval takes its group's hold although it did not sign it, but only a hold of the group it
		// plays in: one that differs by more than this seat is another continuation's.
		const bool neutralSameGroup = IsSeatReclaimGap(m_Config.localPeerId, agreed.frame) &&
		    (PeerFrameMembersAt(agreed.frame) & ~SeatBit(m_Config.localPeerId)) == (heldGroup & ~SeatBit(m_Config.localPeerId));
		if (agreed.preparedFrame == 0 && PeerFrameMembersAt(agreed.frame) != heldGroup &&
		    (voters & SeatBit(m_Config.localPeerId)) == 0 && (PeerFrameMembersAt(agreed.frame) & SeatBit(m_Config.localPeerId)) != 0 &&
		    !neutralSameGroup &&
		    (GetResumeFrame() > agreed.frame || (m_LastDeliveredFrame && *m_LastDeliveredFrame >= agreed.frame) ||
		     (m_GrantedSimulationTick && *m_GrantedSimulationTick >= agreed.frame))) {
			// Every hold is local, including a directory match's. A played or delivered continuation cannot adopt another
			// group's hold as its own history, nor replace its arbitration question. Naming this nonsigner in that group's
			// membership does not make it a participant; an already matching historical hold remains valid.
			const auto arbitration = m_PeerSplitArbitrations.find(PeerSplitEpoch(agreed.frame));
			if (arbitration != m_PeerSplitArbitrations.end()) arbitration->second.offeredHold = certificate;
			NotePeerYield(agreed, voters, nowMs);
			refused("foreign continuation"); return;
		}
		// Two holds certified at one frame by signers that each hold the other are two continuations. A seat that signed neither takes
		// the first it installed there and leaves the other to the heal, or it would part from both.
		if (agreed.preparedFrame == 0 && (voters & SeatBit(m_Config.localPeerId)) == 0) {
			uint32_t held = 0;
			for (uint8_t peer: agreed.members) held |= SeatBit(peer);
			for (auto other = m_PeerBridgeCertificates.lower_bound({certificate.frame, 0}); other != m_PeerBridgeCertificates.end() && other->first.first == certificate.frame; ++other) {
				if (!IsHoldDecision(other->first.second) || other->first.second == DecisionKind(certificate)) continue;
				NetHostMigrationMessage installed; uint32_t installedVoters = 0;
				if (!DecodePeerBridgeCertificate(other->second, installed, installedVoters)) continue;
				uint32_t installedHeld = 0;
				for (uint8_t peer: installed.members) installedHeld |= SeatBit(peer);
				if ((installedHeld & voters) == 0 || (held & installedVoters) == 0) continue;
				const auto arbitration = m_PeerSplitArbitrations.find(PeerSplitEpoch(agreed.frame));
				if (arbitration != m_PeerSplitArbitrations.end()) arbitration->second.offeredHold = certificate;
				refused("other continuation at its frame"); return;
			}
		}
		// A hold, rather than a normally in-flight input or receipt, opens the arbitration question. Asking earlier can assign a
		// healthy startup gap a permanent winner and then reuse that unrelated answer when a machine actually stops.
		if (agreed.preparedFrame == 0) CheckPeerFrameTie(certificate.frame, agreed.connectedMask, nowMs);
		if (agreed.preparedFrame == 0 && !m_Config.frameTieReferee && (agreed.connectedMask & SeatBit(m_Config.localPeerId)) == 0 &&
		    (voters & SeatBit(m_Config.localPeerId)) == 0 && (PeerFrameMembersAt(agreed.frame) & SeatBit(m_Config.localPeerId)) != 0 &&
		    !IsSeatReclaimGap(m_Config.localPeerId, agreed.frame) && GetResumeFrame() >= agreed.frame) {
			const auto arbitration = m_PeerSplitArbitrations.find(PeerSplitEpoch(agreed.frame));
			if (arbitration == m_PeerSplitArbitrations.end() || arbitration->second.readerHoldFrame != agreed.frame) {
				// A foreign local hold cannot suspend an active owner. Keep its history offer outside the frame's decision fence;
				// this group still installs its own hold at the bound. Only the healed histories may make this peer a reader.
				if (arbitration != m_PeerSplitArbitrations.end()) arbitration->second.offeredHold = certificate;
				refused("foreign continuation");
				return;
			}
		}
		if (agreed.preparedFrame == 0 && agreed.frame < m_Stats.nextFrame && GetResumeFrame() <= agreed.frame &&
		    (!m_LastDeliveredFrame || *m_LastDeliveredFrame < agreed.frame) &&
		    (!m_GrantedSimulationTick || *m_GrantedSimulationTick < agreed.frame)) {
			// Every member's unplayed preparation can still contain the held sender's human input. A continuing member
			// rebuilds that suffix from its authenticated originals; a private reader reads the survivors' certified tail.
			// Never recover source commands from the prepared record: it also contains generated authority events.
			if ((agreed.connectedMask & SeatBit(m_Config.localPeerId)) != 0) {
				for (auto sources = m_PeerSourceInputs.lower_bound(agreed.frame); sources != m_PeerSourceInputs.end() && sources->first < m_Stats.nextFrame; ++sources) {
					for (const auto& [owner, input]: sources->second) {
						if (owner == m_Config.localPeerId) {
							m_LocalFrames[sources->first] = input.frames;
							m_LocalCommands[sources->first] = input.commands;
							m_LocalObservations[sources->first] = input.observations;
							m_LocalValueObservations[sources->first] = input.valueObservations;
						} else {
							m_RemoteFrames[sources->first][owner] = input.frames;
							m_RemoteCommands[sources->first][owner] = input.commands;
							m_RemoteObservations[sources->first][owner] = input.observations;
							m_RemoteValueObservations[sources->first][owner] = input.valueObservations;
						}
						m_PendingSourceCommandCounts[sources->first][owner] = input.commands.size();
					}
				}
			}
			// Displayed history and an already delivered/granted tick are never replaced by this path.
			std::erase_if(m_ReadyFrames, [&](const auto& ready) { return ready.frame >= agreed.frame; });
			m_ReadyHistory.erase(m_ReadyHistory.lower_bound(agreed.frame), m_ReadyHistory.end());
			for (auto kept = m_MigrationHistory.lower_bound(agreed.frame); kept != m_MigrationHistory.end();) {
				m_MigrationHistoryBytes -= kept->second.size();
				kept = m_MigrationHistory.erase(kept);
			}
			m_PeerPreparedPrefixes.erase(m_PeerPreparedPrefixes.lower_bound(agreed.frame), m_PeerPreparedPrefixes.end());
			m_Stats.nextFrame = agreed.frame;
			m_LocalPreparedFrame = agreed.frame > m_Config.startFrame ? std::optional(agreed.frame - 1) : std::nullopt;
			m_LastPreparedSentFrame.reset();
			m_FirstMissingFrame.reset();
		}
		m_PeerBridgeCertificates.emplace(PeerDecisionKey{certificate.frame, DecisionKind(certificate)}, certificate);
		if (agreed.preparedFrame == 4) {
			NetHostMigrationMessage request;
			if (!NetHostMigrationCodec::Decode({agreed.bytes.begin() + prefix.size(), agreed.bytes.end()}, m_Config.migrationKey, request)) return;
			auto& completed = m_PeerCapturesCompleted[request.boundary];
			completed = std::max(completed, request.completeFrom);
			while (m_PeerCapturesCompleted.size() > 32) m_PeerCapturesCompleted.erase(m_PeerCapturesCompleted.begin());
			if (m_PeerCaptureRequest && m_PeerCaptureRequest->boundary == request.boundary && m_PeerCaptureRequest->completeFrom <= completed) m_PeerCaptureRequest.reset();
			DiagnosticLine() << "[net-capture] agreed request=" << request.completeFrom << " frame=" << agreed.frame << " tick=" << agreed.frame + 2 << " host=" << static_cast<int>(request.senderPeerId) << " voters=" << voters << std::endl;
			return;
		}
		if (agreed.preparedFrame == 3) { InstallPeerAdmin(agreed, voters, nowMs); return; }
		if (agreed.preparedFrame == 2) {
			if (agreed.members.size() != 1 || agreed.completeFrom == 0 || agreed.completeFrom > NetLockstepCodec::c_MaxPeerInputDelayFrames) return;
			const uint8_t peer = agreed.members.front();
			// A delay the whole group certifies names its split afresh from its frame once a tie under its present name is decided, so the
			// next tie is a new question. With none decided there is nothing to renew, and a certificate one half never installed would
			// name the two halves' split differently.
			if (m_PeerFrameTieWinners.contains(PeerSplitEpoch(agreed.frame))) m_PeerSplitRenewals[agreed.frame] = agreed.connectedMask;
			const bool restated = agreed.completeFrom == InputDelayAt(peer, agreed.frame);
			ApplyInputDelayChange(peer, agreed.frame, static_cast<uint16_t>(agreed.completeFrom), nowMs);
			if (restated) return;
			m_LastDelayResizeMs[peer] = nowMs; ++m_Stats.delayChangesCommitted;
			++m_Stats.peers[peer].delayResizes;
			if (peer == m_Config.localPeerId) PadAheadOfDelayRise();
			DiagnosticLine() << "[net-frame-group] delay peer=" << static_cast<int>(peer) << " frame=" << agreed.frame << " ticks=" << agreed.completeFrom << " voters=" << voters << std::endl;
			return;
		}
		if (agreed.preparedFrame == 0 && (agreed.connectedMask & SeatBit(m_Config.localPeerId)) != 0) {
			const auto arbitration = m_PeerSplitArbitrations.find(PeerSplitEpoch(agreed.frame));
			if (arbitration != m_PeerSplitArbitrations.end() && !arbitration->second.holdFrame)
				arbitration->second.holdFrame = agreed.frame;
		}
		m_FrameGroupChanges[agreed.frame] = agreed.connectedMask;
		for (uint8_t peer: agreed.members) m_PeerFrameHolds[peer][agreed.frame] = agreed.preparedFrame == 0;
		// A returning seat takes this group's history again.
		if (agreed.preparedFrame == 1) for (uint8_t peer: agreed.members) { m_PeerOtherContinuations.erase(peer); m_PeerContinuationEvidence.erase(peer); }
		if (agreed.preparedFrame == 0) {
			m_FrameGroupMembers = PeerFrameMembersAt(agreed.frame);
			// Retain the surviving display clock even when a certificate arrives after preparation.
			if ((m_FrameGroupMembers & SeatBit(m_PeerPaceReference)) == 0)
				m_PeerPaceReference = static_cast<uint8_t>(std::countr_zero(m_FrameGroupMembers) + 1);
			if (ReturnInputDetails()) DiagnosticLine() << "[net-peer-pace] local=" << static_cast<int>(m_Config.localPeerId)
				<< " frame=" << agreed.frame << " members=" << agreed.connectedMask << " reference=" << static_cast<int>(m_PeerPaceReference) << std::endl;
		}
		uint32_t owners = 0;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if (!m_RemovedPeers.contains(peer) && !IsSeatReleased(peer)) owners |= SeatBit(peer);
		if (agreed.preparedFrame == 1 && agreed.connectedMask == owners && agreed.frame <= m_Stats.nextFrame) m_PeerAdminFrameMembers = 0;
		for (uint8_t peer: agreed.members) {
			m_PeerReceiptDelaySamples.erase(peer);
			m_PeerReceiptSampleFloorMs[peer] = nowMs;
			m_PeerOwnReceiptProgress.erase(peer);
			if (agreed.preparedFrame == 1) {
				m_PeerRoundEnds.erase(peer);
				if (peer == m_Config.localPeerId) {
					m_PeerReceiptDelaySamples.clear(); m_PeerOwnReceiptProgress.clear();
					for (uint8_t receiver = 1; receiver <= m_Config.peerCount; ++receiver) m_PeerReceiptSampleFloorMs[receiver] = nowMs;
				}
				if (!m_PeerBridges.contains(peer)) continue;
				m_PeerBridges[peer].returnFrame = agreed.frame;
				m_PeerAcceptedThrough[peer] = agreed.frame;
				m_PeerAcceptedAhead[peer].clear();
				m_PeerInputReceipts[peer].clear();
				const uint64_t revision = agreed.frame * 8 + peer;
				const auto [delay, neutral, incarnation] = returnDelays.at(peer);
				m_ReclaimTransactions[peer] = {peer, m_Config.migrationGeneration, revision, incarnation, agreed.frame, delay, neutral};
				// Every survivor knows the certified terms before the returning process sends its new start.
				m_PeerAdmissions[peer] = {agreed.frame, delay};
				m_Config.initialPeerStartTerms[peer] = {agreed.frame, delay};
				m_PeersPlayedThisRound.erase(peer); m_LastStartAnswerMs.erase(peer);
				if (delay != InputDelayAt(peer, agreed.frame)) m_DelayChanges[peer][agreed.frame] = delay;
				m_PeerEffectiveStart[peer] = agreed.frame + delay;
				m_PeerAcceptedThrough[peer] = neutral + 1;
				// The seat's pace is judged afresh from its return: what it cost and fed before its hold says nothing of it now.
				auto& stats = m_Stats.peers[peer];
				stats.reclaimAdmittedMs = nowMs;
				stats.waitCosts.clear(); stats.arrivals.clear();
				NoteSeatTransition(peer, agreed.frame, SeatTransition::Back);
			} else {
				const auto prior = m_PeerBridges.find(peer);
				const bool alreadyHeld = prior != m_PeerBridges.end() && prior->second.committedHold && !prior->second.returnFrame;
				// A hold agreed ahead of a seat the round already holds changes nothing: the standing hold keeps its frames.
				if (alreadyHeld && heldAhead) continue;
				PeerBridge bridge;
				bridge.fromFrame = agreed.frame;
				bridge.aiFrame = agreed.frame;
				bridge.committedHold = true;
				bridge.members = agreed.connectedMask;
				if (agreed.frame > m_Config.startFrame) {
					NetLockstepReadyFrame previous;
					const auto bytes = m_MigrationHistory.find(agreed.frame - 1);
					if (bytes != m_MigrationHistory.end() && DecodeMigrationFrame(bytes->second, agreed.frame - 1, previous)) bridge.lastInput = FramesForSeat(previous, peer);
				}
				const auto fact = m_HoldFacts.find(peer);
				DiagnosticLine() << "[net-peer-hold-installed] local=" << static_cast<int>(m_Config.localPeerId) << " peer=" << static_cast<int>(peer)
				    << " frame=" << bridge.fromFrame << " ai_frame=" << bridge.aiFrame << " voters=" << voters
				    << " proposer=" << static_cast<int>(agreed.successorPeerId) << " certificate_sender=" << static_cast<int>(certificate.senderPeerId)
				    << " next=" << m_Stats.nextFrame << " displayed=" << m_LastCompletedSimulationTick.value_or(0) << " now_ms=" << nowMs
				    << " cause=" << (fact != m_HoldFacts.end() && fact->second.frame == agreed.frame ? fact->second.cause : "certified_peer_decision") << std::endl;
				m_PeerBridges[peer] = std::move(bridge);
				m_SeatBridgeSinceMs[peer] = nowMs;
				if (!alreadyHeld) {
					++m_Stats.peers[peer].holds;
					// A return the seat made before this hold is over: only a later one brings it back.
					m_ReturningLinks.erase(peer);
				}
				if (peer == m_Config.localPeerId) m_LocalFrames.erase(agreed.frame);
			}
		}
		DiagnosticLine() << "[net-frame-group] " << (agreed.preparedFrame == 1 ? "return" : "bridge") << " frame=" << agreed.frame
		                 << " members=" << agreed.connectedMask << " voters=" << voters << std::endl;
	}

	bool NetLockstepCoordinator::TakePendingPeerDecisions(uint64_t nowMs) {
		auto pending = std::move(m_PendingPeerDecisions);
		m_PendingPeerDecisions.clear();
		for (const auto& [key, kept]: pending) {
			if (m_PeerBridgeCertificates.contains(key) || key.first < m_Stats.nextFrame) continue;
			// A hold is taken at its own frame. Local silence never cancels an unresolved authority decision.
			if (IsHoldDecision(key.second) && key.first > m_Stats.nextFrame) { m_PendingPeerDecisions.emplace(key, kept); continue; }
			InstallPeerBridge(kept.first, nowMs);
			if (const auto again = m_PendingPeerDecisions.find(key); again != m_PendingPeerDecisions.end()) {
				again->second.second = kept.second;
			}
		}
		// A decision this peer cannot yet show the authority of stops its frame; the frame's committed record, asked of the seats that have
		// played it, settles that frame as they played it.
		for (const auto& [key, kept]: m_PendingPeerDecisions) if (key.first <= m_Stats.nextFrame) {
			if (m_PeerCommittedTail.contains(m_Stats.nextFrame)) return false;
			if (ReturnInputDetails()) DiagnosticLine() << "[net-peer-decision-wait] local=" << static_cast<int>(m_Config.localPeerId)
			    << " frame=" << key.first << " kind=" << static_cast<int>(key.second) << " cause=unproven next=" << m_Stats.nextFrame << " now_ms=" << nowMs << std::endl;
			RequestPeerCommittedTail(nowMs);
			return true;
		}
		// A member that voted for a return, a delay or an admission at the next frame takes it before playing that frame, and so does
		// every member that knows of a return proposed there for a seat still held: the others may certify it without this one's vote.
		for (auto votes = m_PeerBridgeVotes.lower_bound({m_Stats.nextFrame, 1}); votes != m_PeerBridgeVotes.end() && votes->first.first == m_Stats.nextFrame; ++votes) {
			if (IsHoldDecision(votes->first.second)) continue;
			if (m_PeerBridgeCertificates.contains(votes->first)) continue;
			bool open = votes->second.contains(m_Config.localPeerId);
			if (!open && votes->first.second == 1)
				for (const auto& [voter, vote]: votes->second)
					for (uint8_t peer: vote.members)
						if (const auto held = m_PeerBridges.find(peer); held != m_PeerBridges.end() && held->second.committedHold && !held->second.returnFrame) open = true;
			if (!open) continue;
			// It is certified only with every seat that plays now: a seat held or silent at this frame, or one that backed another return
			// of these seats, never completes it, so the frame does not wait on it and a silent seat is held within the bound instead.
			const auto& basis = votes->second.contains(m_Config.localPeerId) ? votes->second.at(m_Config.localPeerId) : votes->second.begin()->second;
			const uint32_t required = PeerDecisionRequiredVoters(basis);
			uint32_t returning = 0;
			if (basis.preparedFrame == 1) for (uint8_t peer: basis.members) returning |= SeatBit(peer);
			// One that needs this peer's vote, which it no longer gives at the frame it has reached, is never certified.
			if (basis.preparedFrame != 3 && (required & SeatBit(m_Config.localPeerId)) != 0 && !votes->second.contains(m_Config.localPeerId)) continue;
			const auto remote = m_RemoteFrames.find(m_Stats.nextFrame);
			bool completes = true;
			for (uint8_t peer = 1; completes && peer <= m_Config.peerCount; ++peer) {
				if ((required & SeatBit(peer)) == 0 || peer == m_Config.localPeerId) continue;
				if (const auto vote = votes->second.find(peer); vote != votes->second.end() && SameBridge(vote->second, basis)) continue;
				const auto held = m_PeerBridges.find(peer);
				const bool heldHere = held != m_PeerBridges.end() && held->second.committedHold && m_Stats.nextFrame >= held->second.fromFrame &&
				    (!held->second.returnFrame || m_Stats.nextFrame < *held->second.returnFrame);
				const bool silent = remote == m_RemoteFrames.end() || !remote->second.contains(peer);
				bool backedAnother = false;
				for (auto other = m_PeerBridgeVotes.lower_bound({m_Stats.nextFrame, 0}); returning != 0 && !backedAnother && other != m_PeerBridgeVotes.end(); ++other) {
					if (other->first.second != 1 || other->first == votes->first) continue;
					if (const auto vote = other->second.find(peer); vote != other->second.end())
						for (uint8_t member: vote->second.members) backedAnother = backedAnother || (returning & SeatBit(member)) != 0;
				}
				completes = !heldHere && !silent && !backedAnother;
			}
			if (!completes) continue;
			// Nor does one whose possible signers could never carry the group's authority: it is never certified, so its frame plays on.
			uint32_t signers = required;
			for (const auto& [voter, vote]: votes->second) if (SameBridge(vote, basis)) signers |= SeatBit(voter);
			if (basis.preparedFrame != 3 && !PeerGroupHasAuthority(signers, votes->first.first)) continue;
			// The frame's committed record settles what this peer still waits on: it carries the decision if any peer certified it.
			if (m_PeerCommittedTail.contains(m_Stats.nextFrame)) return false;
			if (ReturnInputDetails()) DiagnosticLine() << "[net-peer-decision-wait] local=" << static_cast<int>(m_Config.localPeerId)
			    << " frame=" << votes->first.first << " kind=" << static_cast<int>(votes->first.second) << " cause=open_vote required=" << required
			    << " voted=" << std::accumulate(votes->second.begin(), votes->second.end(), 0U, [](uint32_t mask, const auto& vote) { return mask | (1U << (vote.first - 1)); })
			    << " connected=" << basis.connectedMask << " held=" << PeerHeldSeatsAt(votes->first.first) << " own_gap=" << IsSeatReclaimGap(m_Config.localPeerId, votes->first.first)
			    << " authority=" << PeerGroupHasAuthority(SeatBit(m_Config.localPeerId), votes->first.first) << " next=" << m_Stats.nextFrame << " now_ms=" << nowMs << std::endl;
			// It is asked at once of the seats that have played the frame, so the wait is one trip to them.
			RequestPeerCommittedTail(nowMs);
			// Only its certificate, a committed frame or a proof that it cannot complete releases this promise.
			return true;
		}
		return false;
	}

	void NetLockstepCoordinator::PreparePeerBridgeInputs(uint64_t frame, uint64_t nowMs) {
		m_FrameGroupMembers = PeerFrameMembersAt(frame);
		if (m_FrameGroupMembers != 0 && (m_FrameGroupMembers & SeatBit(m_PeerPaceReference)) == 0)
			m_PeerPaceReference = static_cast<uint8_t>(std::countr_zero(m_FrameGroupMembers) + 1);
		uint32_t owners = 0;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if (!m_RemovedPeers.contains(peer) && !IsSeatReleased(peer)) owners |= SeatBit(peer);
		if (m_FrameGroupMembers == owners) m_PeerAdminFrameMembers = 0;
		for (const auto& [peer, bridge]: m_PeerBridges) {
			if (bridge.returnFrame && frame >= *bridge.returnFrame) {
				m_PeerLeaveFrames.erase(peer); m_AiHeldSeats.erase(peer); m_SeatBridgeSinceMs.erase(peer);
				m_SlowMachineHolds.erase(peer);
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
				// This hold ends the earlier return while retaining its preceding neutral frames.
				if (const auto back = m_ReclaimTransactions.find(peer); back != m_ReclaimTransactions.end() && back->second.activationFrame < bridge.aiFrame) {
					const uint64_t gapEnd = std::max(back->second.neutralThroughFrame, back->second.activationFrame + back->second.delayFrames);
					m_RetiredReclaimGaps[peer][back->second.activationFrame] = std::min(gapEnd, bridge.aiFrame - 1);
					m_ReclaimTransactions.erase(back);
				}
				m_AiHeldSeats[peer] = bridge.aiFrame;
				m_PeerLeaveFrames[peer] = bridge.aiFrame;
				const uint64_t revision = bridge.fromFrame * 8 + peer;
				m_HoldTransactions[peer] = {peer, m_Config.migrationGeneration, revision, std::max<uint32_t>(1, m_Config.peerIncarnations[peer]), bridge.aiFrame};
				NoteSeatTransition(peer, bridge.aiFrame, SeatTransition::Held);
			}
		}
	}

} // namespace RTE
