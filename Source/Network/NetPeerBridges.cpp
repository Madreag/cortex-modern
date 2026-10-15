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
#include <string_view>

namespace RTE {

	using namespace NetPeerFrameDetail;

	bool NetLockstepCoordinator::PeerBridgeHasInputConflict(const NetHostMigrationMessage& proposal) const {
		if (proposal.preparedFrame != 0) return false;
		for (uint8_t owner: proposal.members) {
			const auto receipts = m_PeerInputReceipts.find(owner);
			if (receipts == m_PeerInputReceipts.end()) continue;
			for (const auto& [peer, through]: receipts->second)
				if (through > proposal.frame && (proposal.connectedMask & SeatBit(peer)) != 0) return true;
		}
		return false;
	}

	void NetLockstepCoordinator::ResolvePeerBridgeInputConflicts(uint64_t nowMs) {
		for (auto& [key, votes]: m_PeerBridgeVotes) {
			if (key.second != 0 || key.first < m_Stats.nextFrame || m_PeerBridgeCertificates.contains(key)) continue;
			const auto own = votes.find(m_Config.localPeerId);
			if (own != votes.end() && PeerBridgeHasInputConflict(own->second)) {
				// This proposal requires a signer that has already accepted an
				// excluded input. That signer cannot also vote for this bridge,
				// so no certificate can exist for the abandoned proposal.
				for (uint8_t peer: own->second.members) m_PeerRejectedInputs[key.first] &= ~SeatBit(peer);
			}
			std::erase_if(votes, [&](const auto& vote) { return PeerBridgeHasInputConflict(vote.second); });
		}
		for (auto pending = m_PeerPendingBridgeInputs.begin(); pending != m_PeerPendingBridgeInputs.end();) {
			if (pending->first < m_Stats.nextFrame || m_PeerBridgeCertificates.contains({pending->first, 0})) {
				pending = m_PeerPendingBridgeInputs.erase(pending);
				continue;
			}
			for (auto input = pending->second.begin(); input != pending->second.end();) {
				if ((m_PeerRejectedInputs[pending->first] & SeatBit(input->first)) != 0) { ++input; continue; }
				AcceptRemoteTick(input->second, nowMs, false);
				input = pending->second.erase(input);
			}
			if (pending->second.empty()) pending = m_PeerPendingBridgeInputs.erase(pending);
			else ++pending;
		}
	}

	bool NetLockstepCoordinator::ValidatePeerBridge(const NetHostMigrationMessage& proposal) const {
		if (proposal.preparedFrame == 3) return ValidatePeerAdmin(proposal);
		if (proposal.members.empty() || proposal.bytes.size() != NetHash32{}.size() + (proposal.preparedFrame == 1 ? 12 * proposal.members.size() : 0) || !PeerGroupHasAuthority(proposal.connectedMask) ||
		    proposal.preparedFrame > 2 || (proposal.connectedMask & SeatBit(m_Config.localPeerId)) == 0 || PeerBridgeHasInputConflict(proposal)) return false;
		// A hold agreed ahead names a frame past a boundary every voter has applied, as a return does.
		const bool ahead = proposal.preparedFrame != 0 || proposal.boundary < proposal.frame;
		if (!ahead && proposal.frame != m_Stats.nextFrame) return false;
		if (ahead && (proposal.frame <= m_Stats.nextFrame || proposal.frame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew)) return false;
		const uint64_t prefixFrame = ahead ? proposal.boundary : proposal.frame - 1;
		const NetHash32 prefix = proposal.frame == m_Config.startFrame ? NetHash32{} : ahead ? PeerAppliedFramePrefix(prefixFrame) : PeerFramePrefix(prefixFrame);
		if (proposal.frame != m_Config.startFrame && !m_MigrationHistory.contains(prefixFrame)) return false;
		if (!std::equal(prefix.begin(), prefix.end(), proposal.bytes.begin())) return false;
		if (proposal.preparedFrame == 2) return proposal.members.size() == 1 && proposal.successorPeerId == proposal.members.front() &&
		    proposal.members.front() > 0 && proposal.members.front() <= m_Config.peerCount && proposal.completeFrom > 0 && proposal.completeFrom <= NetLockstepCodec::c_MaxPeerInputDelayFrames &&
		    (proposal.connectedMask & SeatBit(proposal.members.front())) != 0;
		uint32_t named = 0;
		for (uint8_t peer: proposal.members) named |= SeatBit(peer);
		const auto sources = m_PeerSourceInputs.find(proposal.frame);
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			const bool present = sources != m_PeerSourceInputs.end() && sources->second.contains(peer);
			const bool neutral = IsSeatReclaimGap(peer, proposal.frame);
			const auto ended = m_PeerRoundEnds.find(peer);
			const bool departed = ended != m_PeerRoundEnds.end() && !ended->second.closedLink && proposal.frame >= ended->second.frame;
			if (proposal.preparedFrame == 0 && (named & SeatBit(peer)) != 0 && (present || (neutral && !departed)) &&
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
			const auto ended = m_PeerRoundEnds.find(peer);
			if (ended != m_PeerRoundEnds.end() && !ended->second.closedLink && frame >= ended->second.frame) {
				proposal.members.push_back(peer);
				continue;
			}
			if (PeerLinkLostBeforeOwnInput(peer, frame)) proposal.members.push_back(peer);
			else if (frame < EffectiveStartOf(peer) || IsSeatReclaimGap(peer, frame) || sources->second.contains(peer)) proposal.connectedMask |= SeatBit(peer);
			else proposal.members.push_back(peer);
		}
		if (returningOnly && !proposal.members.empty()) return false;
		if (!proposal.members.empty()) {
			// Inputs are prepared ahead of play. An ordinary packet still in
			// transit must not spend a directory request for every future tick:
			// that leaves the real disconnected player waiting for the minute's
			// allowance to renew. Allow the sender's normal transit and the
			// scheduling bound first, then use the remaining buffered time for
			// the referee so its decision does not postpone the AI stages.
			std::optional<double> dueMs;
			if (m_PeerPaceStartMs && frame >= m_PeerPaceStartFrame)
				dueMs = *m_PeerPaceStartMs + (frame - m_PeerPaceStartFrame) * m_Config.simTickMs;
			if (m_PeerConsumerAtMs && m_LastCompletedSimulationTick && frame > *m_LastCompletedSimulationTick)
				dueMs = *m_PeerConsumerAtMs + (frame - *m_LastCompletedSimulationTick) * m_Config.simTickMs;
			const double boundMs = m_Config.slowPlayerBoundTicks * m_Config.simTickMs;
			double leadMs = std::numeric_limits<double>::max();
			for (uint8_t peer: proposal.members) {
				const auto stats = m_Stats.peers.find(peer);
				uint32_t transitMs = stats != m_Stats.peers.end() ? stats->second.pingMs : 0;
				const auto [wire, route] = PeerFrameRoute(peer);
				if (wire && wire->IsPeerPingMeasured(route)) transitMs = std::max(transitMs, wire->GetPeerPingMs(route));
				leadMs = std::min(leadMs, std::max(0.0, InputDelayAt(peer, frame) * m_Config.simTickMs - transitMs - boundMs));
			}
			if (m_ConsumerWaitingFrame == frame || (dueMs && nowMs + leadMs >= *dueMs))
				CheckPeerFrameTie(frame, proposal.connectedMask, nowMs);
		}
		if (proposal.members.empty() && !returningOnly && !PeerInputAccepted(frame)) {
			// A seat whose own input the other half stopped confirming is in the same split as the half missing input:
			// once the confirmation is overdue and that half has stopped answering past the bound, it checks in too.
			const uint32_t confirming = PeerInputVoters(frame, m_Config.localPeerId);
			const auto sent = m_PeerInputSentAtMs.find(frame);
			const uint64_t boundMs = static_cast<uint64_t>(std::ceil(m_Config.slowPlayerBoundTicks * m_Config.simTickMs));
			bool overdue = sent != m_PeerInputSentAtMs.end() && PeerGroupIsRefereedHalf(confirming);
			for (uint8_t peer = 1; overdue && peer <= m_Config.peerCount; ++peer) {
				if ((confirming & SeatBit(peer)) != 0 || m_RemovedPeers.contains(peer) || IsSeatReleased(peer)) continue;
				const auto stats = m_Stats.peers.find(peer);
				uint64_t transitMs = stats != m_Stats.peers.end() ? stats->second.pingMs : 0;
				const auto [wire, route] = PeerFrameRoute(peer);
				if (wire && wire->IsPeerPingMeasured(route)) transitMs = std::max<uint64_t>(transitMs, wire->GetPeerPingMs(route));
				// A stopped main loop keeps its link alive from another thread: neither its receipts of this machine's input nor its own
				// inputs advance. Receipts alone stop too when this seat's inputs stop reaching it, and then it is this seat that went quiet.
				uint64_t heard = LastAuthenticatedTraffic(peer);
				if (const auto progress = m_PeerOwnReceiptProgress.find(peer); progress != m_PeerOwnReceiptProgress.end() && progress->second.through <= frame) {
					const auto& arrivals = m_Stats.peers[peer].arrivals;
					heard = std::min(heard, std::max(progress->second.advancedAtMs, arrivals.empty() ? 0 : arrivals.back().first));
				}
				overdue = nowMs >= sent->second + transitMs + boundMs && nowMs >= heard && nowMs - heard >= boundMs;
			}
			if (overdue) CheckPeerFrameTie(frame, confirming, nowMs);
		}
		if (proposal.members.empty()) {
			if (m_FrameGroupMembers == 0 || std::countr_zero(m_FrameGroupMembers) + 1 != m_Config.localPeerId) return false;
			for (const auto& [peer, bridge]: m_PeerBridges) {
				if (!bridge.committedHold || bridge.returnFrame || peer == m_Config.localPeerId) continue;
				uint64_t delay = InputDelayAt(peer, frame);
				const auto [wire, route] = PeerFrameRoute(peer);
				if (wire && wire->IsPeerPingMeasured(route)) {
					const uint32_t margin = std::max(c_NetInputJitterReserveMs +
					    static_cast<uint32_t>(std::ceil((m_Config.slowPlayerBoundTicks + 1) * m_Config.simTickMs)), m_Stats.peers[peer].jitterMs);
					delay = std::max<uint64_t>(delay, std::min<uint64_t>(NetLockstepCodec::c_MaxPeerInputDelayFrames,
					    static_cast<uint64_t>(std::ceil((wire->GetPeerPingMs(route) + margin) / m_Config.simTickMs))));
				}
				const uint64_t heard = LastAuthenticatedTraffic(peer);
				const auto applied = m_PeerAppliedThrough.find(peer);
				if (nowMs < heard || nowMs - heard > 1000 || applied == m_PeerAppliedThrough.end()) continue;
				// A pre-outage sample can still be inside the buffered runway.
				// Require the reader to have displayed certified bridge history
				// before admitting its next producible input again.
				if (applied->second < bridge.fromFrame) continue;
				// Compare displayed progress; the remaining trail fits the return's capped neutral interval.
				const uint64_t displayed = m_LastCompletedSimulationTick.value_or(frame - 1);
				const uint64_t allowance = std::max<uint64_t>(delay + m_Config.slowPlayerBoundTicks,
				    static_cast<uint64_t>(std::ceil(c_NetSeatCatchUpAllowanceMs / m_Config.simTickMs)));
				if (displayed > applied->second && displayed - applied->second > allowance) continue;
				// A seat returns once its catch-up shows headroom: a machine slower than this one would only fall behind and be held again.
				const double own = OwnCapacityTps();
				if (own > 0) {
					const auto rate = PeerAppliedRate(peer, nowMs);
					if (!rate) continue;
					// It must also close its trail by its first live frame: one still behind there is late at once and held again.
					const uint64_t trail = displayed > applied->second ? displayed - applied->second : 0;
					const uint64_t horizon = m_LastQueuedTargetFrame != UINT64_MAX && m_LastQueuedTargetFrame >= displayed ? m_LastQueuedTargetFrame + 1 - displayed : 0;
					const double toLive = static_cast<double>(horizon + delay + trail + m_Config.slowPlayerBoundTicks);
					const bool closes = trail <= m_Config.slowPlayerBoundTicks || (*rate - own) * toLive >= static_cast<double>(trail - m_Config.slowPlayerBoundTicks) * own;
					if (*rate < own - (own >= 59.5 ? 0.5 : own / 10.0) || !closes) {
						if (m_SlowMachineHolds.insert(peer).second)
							DiagnosticLine() << "[net-lockstep] slow machine peer " << static_cast<int>(peer) << " stays held: it applied " << *rate
							                 << " ticks/s against this machine's " << own << " with " << trail << " ticks to close" << std::endl;
						continue;
					}
				}
				proposal.members.push_back(peer); proposal.connectedMask |= SeatBit(peer);
			}
			if (proposal.members.empty()) return false;
			proposal.preparedFrame = 1;
			proposal.boundary = m_LastCompletedSimulationTick.value_or(frame - 1);
			uint64_t lead = m_Config.slowPlayerBoundTicks;
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
				if (peer == m_Config.localPeerId || (m_FrameGroupMembers & SeatBit(peer)) == 0) continue;
				// Only surviving voters size the return's agreement lead.
				uint32_t rtt = 0;
				const auto [wire, route] = PeerFrameRoute(peer);
				if (wire && wire->IsPeerPingMeasured(route)) rtt = wire->GetPeerPingMs(route);
				else if (const auto path = m_PeerReceiptDelaySamples.find(peer); path != m_PeerReceiptDelaySamples.end()) rtt = path->second.P95Ms();
				lead = std::max<uint64_t>(lead, static_cast<uint64_t>(std::ceil(rtt / m_Config.simTickMs)) + m_Config.slowPlayerBoundTicks);
			}
			uint64_t prepared = frame;
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if ((m_FrameGroupMembers & SeatBit(peer)) != 0)
				prepared = std::max(prepared, m_Stats.peers[peer].reportedNextFrame);
			// No voter prepares a frame this machine has not sent its input for, so a return past those reaches every voter before its frame.
			if (m_LastQueuedTargetFrame != UINT64_MAX) prepared = std::max(prepared, m_LastQueuedTargetFrame + 1);
			proposal.frame = prepared + lead;
		}
		const bool declaredDeparture = !proposal.members.empty() && std::all_of(proposal.members.begin(), proposal.members.end(), [&](uint8_t peer) {
			const auto ended = m_PeerRoundEnds.find(peer);
			return ended != m_PeerRoundEnds.end() && !ended->second.closedLink && frame >= ended->second.frame;
		});
		// A machine slower than the others is held when it has cost them more than the round allows, not after a bound none of its
		// late inputs reaches: waiting the bound on each of them would let it slow every other seat for good.
		double slowCost = 0;
		const bool slowMachine = proposal.preparedFrame == 0 && !proposal.members.empty() && std::all_of(proposal.members.begin(), proposal.members.end(),
		    [&](uint8_t peer) { return CostsPastTolerance(peer, frame, nowMs, &slowCost); });
		// With another seat voting, a seat that still feeds is never held at the frame being waited on: its input reaches some voter before
		// the proposal does, the voters stop at different frames, and no majority forms. Only a silent seat stops every voter at one frame.
		// So the lowest seat that plays on holds a feeding seat at a frame past its inputs in flight, agreed ahead, once it costs more than
		// the round allows or a wait on it passes the bound; the others wait for that hold.
		const bool otherVoter = (proposal.connectedMask & ~SeatBit(m_Config.localPeerId)) != 0;
		const uint64_t feedBoundMs = static_cast<uint64_t>(std::ceil(m_Config.slowPlayerBoundTicks * m_Config.simTickMs));
		const bool feeding = proposal.preparedFrame == 0 && !proposal.members.empty() && !declaredDeparture &&
		    std::all_of(proposal.members.begin(), proposal.members.end(), [&](uint8_t peer) {
			    const auto& arrivals = m_Stats.peers[peer].arrivals;
			    return !arrivals.empty() && nowMs <= arrivals.back().first + 2 * feedBoundMs;
		    });
		const bool heldAhead = otherVoter && feeding;
		if (heldAhead) {
			const bool waitedPastBound = m_ConsumerWaitingFrame == frame && nowMs >= m_ConsumerWaitStartMs && nowMs - m_ConsumerWaitStartMs >= feedBoundMs;
			if (!(slowMachine || waitedPastBound) || std::countr_zero(proposal.connectedMask) + 1 != m_Config.localPeerId) return false;
			for (const auto& [key, votes]: m_PeerBridgeVotes) {
				if (key.second != 0 || key.first <= frame || m_PeerBridgeCertificates.contains(key)) continue;
				if (const auto own = votes.find(m_Config.localPeerId); own != votes.end())
					for (uint8_t peer: own->second.members) if (std::find(proposal.members.begin(), proposal.members.end(), peer) != proposal.members.end()) return false;
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
			proposal.frame = inFlight + static_cast<uint64_t>(std::ceil(rttMs / m_Config.simTickMs)) + m_Config.slowPlayerBoundTicks;
		}
		// A declared Leave has already ended its input; it needs no silence wait at that boundary.
		if (proposal.preparedFrame == 0 && !declaredDeparture && !heldAhead && !(slowMachine && !otherVoter)) {
			const uint64_t boundMs = static_cast<uint64_t>(std::ceil(m_Config.slowPlayerBoundTicks * m_Config.simTickMs));
			// The startup consumer has no established buffered runway to spend.
			// Its first missing inputs get the full bound from the actual wait.
			const uint64_t displayed = m_LastCompletedSimulationTick.value_or(m_Config.startFrame - 1);
			if (displayed < m_Stats.effectiveStartFrame + m_Config.slowPlayerBoundTicks &&
			    (m_ConsumerWaitingFrame != frame || nowMs < m_ConsumerWaitStartMs || nowMs - m_ConsumerWaitStartMs < boundMs)) return false;
			uint64_t agreementMs = 0;
			bool anotherVoter = false;
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if (peer != m_Config.localPeerId && (proposal.connectedMask & SeatBit(peer)) != 0)
			{
				anotherVoter = true;
				// Size the vote from the surviving signers' transport, outside another seat's input wait.
				const auto [wire, route] = PeerFrameRoute(peer);
				if (wire && wire->IsPeerPingMeasured(route)) agreementMs = std::max<uint64_t>(agreementMs, wire->GetPeerPingMs(route));
				else if (const auto path = m_PeerReceiptDelaySamples.find(peer); path != m_PeerReceiptDelaySamples.end())
					agreementMs = std::max<uint64_t>(agreementMs, path->second.P95Ms());
			}
			// Spend the voters' scheduling reserve inside the consumer's three-tick bound.
			if (anotherVoter) agreementMs = std::max<uint64_t>(1, agreementMs) + boundMs;
			// A lone voter commits at once; half a tick covers the commit and the consumer's next poll, so its wait ends inside the bound.
			else agreementMs = static_cast<uint64_t>(std::ceil(m_Config.simTickMs / 2));
			// Votes spend the buffered runway, so a healthy relay round trip fits inside the consumer's bound.
			if (m_PeerPaceStartMs && frame >= m_PeerPaceStartFrame) {
				double dueMs = *m_PeerPaceStartMs + (frame - m_PeerPaceStartFrame) * m_Config.simTickMs;
					// The current displayed slot includes pauses and startup synchronization.
				if (m_PeerConsumerAtMs && m_LastCompletedSimulationTick && frame > *m_LastCompletedSimulationTick)
						dueMs = *m_PeerConsumerAtMs + (frame - *m_LastCompletedSimulationTick) * m_Config.simTickMs;
				// Catch-up can ask for this tick before its original pacing slot.
				// Its actual consumer wait still has the same three-tick bound.
				if (m_ConsumerWaitingFrame == frame) dueMs = std::min(dueMs, static_cast<double>(m_ConsumerWaitStartMs));
				if (nowMs + agreementMs < dueMs + boundMs) return false;
			} else if (m_ConsumerWaitingFrame != frame || nowMs < m_ConsumerWaitStartMs ||
			    nowMs - m_ConsumerWaitStartMs < boundMs - std::min(boundMs, agreementMs)) return false;
		}
		const uint32_t proposers = proposal.preparedFrame == 1 ? m_FrameGroupMembers : proposal.connectedMask;
		if (proposers == 0) return false;
		proposal.successorPeerId = static_cast<uint8_t>(std::countr_zero(proposers) + 1);
		const NetHash32 prefix = frame == m_Config.startFrame ? NetHash32{} : proposal.preparedFrame != 0 || heldAhead ? PeerAppliedFramePrefix(proposal.boundary) : PeerFramePrefix(frame - 1);
		proposal.bytes.assign(prefix.begin(), prefix.end());
		if (proposal.preparedFrame == 1) for (uint8_t peer: proposal.members) {
			uint32_t rtt = m_Stats.peers[peer].pingMs;
			const auto [wire, route] = PeerFrameRoute(peer);
			if (wire && wire->IsPeerPingMeasured(route)) rtt = std::max(rtt, wire->GetPeerPingMs(route));
			const uint64_t displayed = m_LastCompletedSimulationTick.value_or(frame - 1);
			const uint64_t applied = m_PeerAppliedThrough.at(peer);
			const uint32_t trail = static_cast<uint32_t>(displayed - std::min(displayed, applied));
			// Advertised jitter already includes the scheduling reserve.
			const uint32_t margin = std::max(c_NetInputJitterReserveMs +
			    static_cast<uint32_t>(std::ceil((m_Config.slowPlayerBoundTicks + 1) * m_Config.simTickMs)), m_Stats.peers[peer].jitterMs);
			const uint32_t transit = static_cast<uint32_t>(std::ceil((rtt + margin) / m_Config.simTickMs));
			// The private reader's trail belongs in its temporary neutral
			// interval. It must not become every human input's permanent delay.
			// Fixed rounds keep their agreed delay; return transit stays in the neutral interval.
			const uint32_t delay = m_Config.adaptiveInputDelay ? std::min<uint32_t>(NetLockstepCodec::c_MaxPeerInputDelayFrames,
			    std::max<uint32_t>(InputDelayAt(peer, proposal.frame), transit)) : PeerInputDelay(peer);
			const uint32_t neutral = delay + transit + trail + m_Config.slowPlayerBoundTicks;
			if (neutral > NetLockstepCodec::c_MaxFutureFrameSkew) return false;
			PutSize(proposal.bytes, delay); PutSize(proposal.bytes, neutral);
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
		if (VotedOtherPeerReturn(proposal)) return false;
		proposal.voterMask = SeatBit(m_Config.localPeerId);
		votes.emplace(m_Config.localPeerId, proposal);
		m_PeerVotedAtMs.try_emplace({proposal.frame, DecisionKind(proposal)}, nowMs);
		if (proposal.preparedFrame == 1) {
			size_t offset = NetHash32{}.size();
			for (uint8_t peer: proposal.members) {
				uint32_t delay = 0, neutral = 0, incarnation = 0;
				if (!TakeSize(proposal.bytes, offset, delay) || !TakeSize(proposal.bytes, offset, neutral) || !TakeSize(proposal.bytes, offset, incarnation)) return false;
				DiagnosticLine() << "[net-peer-return-runway] peer=" << static_cast<int>(peer) << " activation=" << proposal.frame
				    << " delay_ticks=" << delay << " neutral_through=" << proposal.frame + neutral << std::endl;
			}
		}
		if (proposal.preparedFrame == 0) {
			for (uint8_t peer: proposal.members) {
				m_PeerRejectedInputs[proposal.frame] |= SeatBit(peer);
				if (!slowMachine) { m_SlowMachineHolds.erase(peer); continue; }
				m_SlowMachineHolds.insert(peer);
				DiagnosticLine() << "[net-lockstep] slow machine peer " << static_cast<int>(peer) << " at frame " << proposal.frame << " cost " << slowCost
				                 << " ticks/s against this machine's " << OwnCapacityTps() << "; the AI takes its seat" << std::endl;
			}
			if (std::getenv("CC_TEST_RETURN_INPUT_DETAILS")) {
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
		if ((voters & required) != required || (proposal.preparedFrame == 3 ? !PeerAdminCertificateValid(proposal, voters) : !PeerGroupHasAuthority(voters, frame))) return;
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
		// CC_TEST_RETURN_INPUT_DETAILS names each decision this peer sets aside and why.
		const auto refused = [&](const char* reason) {
			if (std::string_view(reason) == "certificate") ++m_Stats.decisionsRefusedForAuthority;
			if (std::string_view(reason) == "not held") ++m_Stats.returnsRefusedNotHeld;
			if (std::getenv("CC_TEST_RETURN_INPUT_DETAILS")) DiagnosticLine() << "[net-peer-decision-refused] local=" << static_cast<int>(m_Config.localPeerId)
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
		// A tie the referee decides is asked before the decision is set aside: its answer may already be there.
		if (agreed.preparedFrame != 3 && !PeerGroupHasAuthority(voters, certificate.frame)) CheckPeerFrameTie(certificate.frame, voters, nowMs);
		if (agreed.preparedFrame == 3 ? !PeerAdminCertificateValid(agreed, voters) : !PeerGroupHasAuthority(voters, certificate.frame)) {
			refused("certificate");
			keep(); return;
		}
		const bool heldAhead = agreed.preparedFrame == 0 && agreed.boundary < certificate.frame;
		if (agreed.preparedFrame == 0 && certificate.frame > m_Stats.nextFrame) { refused("ahead"); keep(); if (!heldAhead) RequestPeerCommittedTail(nowMs); return; }
		// A hold agreed ahead that this peer has already played past would rewrite history: it was taken without this peer.
		if (heldAhead && certificate.frame < m_Stats.nextFrame) { refused("behind"); return; }
		if (agreed.preparedFrame != 0 && certificate.frame < m_Stats.nextFrame) {
			// Install a private return before its displayed tick, even if preparation has passed it.
			const auto own = m_PeerBridges.find(m_Config.localPeerId);
			const bool privateReader = own != m_PeerBridges.end() && own->second.committedHold && !own->second.returnFrame;
			if (agreed.preparedFrame != 1 || !privateReader ||
			    certificate.frame <= m_LastCompletedSimulationTick.value_or(0)) { refused(privateReader ? "displayed" : "behind"); return; }
		}
		const uint64_t prefixFrame = agreed.preparedFrame != 0 || heldAhead ? agreed.boundary : certificate.frame - 1;
		const NetHash32 prefix = certificate.frame == m_Config.startFrame ? NetHash32{} : agreed.preparedFrame != 0 || heldAhead ? PeerAppliedFramePrefix(prefixFrame) : PeerFramePrefix(prefixFrame);
		if (agreed.bytes.size() != prefix.size() + (agreed.preparedFrame == 1 ? 12 * agreed.members.size() : 0) || !std::equal(prefix.begin(), prefix.end(), agreed.bytes.begin())) { refused("prefix"); return; }
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
		m_PeerBridgeCertificates.emplace(PeerDecisionKey{certificate.frame, DecisionKind(certificate)}, certificate);
		if (agreed.preparedFrame == 3) { InstallPeerAdmin(agreed, voters, nowMs); return; }
		if (agreed.preparedFrame == 2) {
			if (agreed.members.size() != 1 || agreed.completeFrom == 0 || agreed.completeFrom > NetLockstepCodec::c_MaxPeerInputDelayFrames) return;
			const uint8_t peer = agreed.members.front();
			m_DelayChanges[peer][agreed.frame] = static_cast<uint16_t>(agreed.completeFrom);
			m_LastDelayResizeMs[peer] = nowMs; ++m_Stats.delayChangesCommitted;
			++m_Stats.peers[peer].delayResizes;
			if (peer == m_Config.localPeerId) PadAheadOfDelayRise();
			DiagnosticLine() << "[net-frame-group] delay peer=" << static_cast<int>(peer) << " frame=" << agreed.frame << " ticks=" << agreed.completeFrom << " voters=" << voters << std::endl;
			return;
		}
		m_FrameGroupChanges[agreed.frame] = agreed.connectedMask;
		if (agreed.preparedFrame == 0) {
			m_FrameGroupMembers = agreed.connectedMask;
			// Retain the surviving display clock even when a certificate arrives after preparation.
			if ((agreed.connectedMask & SeatBit(m_PeerPaceReference)) == 0)
				m_PeerPaceReference = static_cast<uint8_t>(std::countr_zero(agreed.connectedMask) + 1);
			if (std::getenv("CC_TEST_RETURN_INPUT_DETAILS")) DiagnosticLine() << "[net-peer-pace] local=" << static_cast<int>(m_Config.localPeerId)
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
				if (delay != InputDelayAt(peer, agreed.frame)) m_DelayChanges[peer][agreed.frame] = delay;
				m_PeerEffectiveStart[peer] = agreed.frame + delay;
				m_PeerAcceptedThrough[peer] = neutral + 1;
				NoteSeatTransition(peer, agreed.frame, SeatTransition::Back);
			} else {
				const auto prior = m_PeerBridges.find(peer);
				const bool alreadyHeld = prior != m_PeerBridges.end() && prior->second.committedHold && !prior->second.returnFrame;
				// A hold agreed ahead of a seat the round already holds changes nothing: the standing hold keeps its frames.
				if (alreadyHeld && heldAhead) continue;
				PeerBridge bridge;
				bridge.fromFrame = agreed.frame;
				bridge.aiFrame = agreed.frame + static_cast<uint64_t>(std::ceil(c_NetInputContinuityMs / m_Config.simTickMs));
				bridge.committedHold = true;
				bridge.members = agreed.connectedMask;
				if (agreed.frame > m_Config.startFrame) {
					NetLockstepReadyFrame previous;
					const auto bytes = m_MigrationHistory.find(agreed.frame - 1);
					if (bytes != m_MigrationHistory.end() && DecodeMigrationFrame(bytes->second, agreed.frame - 1, previous)) bridge.lastInput = FramesForSeat(previous, peer);
				}
				m_PeerBridges[peer] = std::move(bridge);
				m_SeatBridgeSinceMs[peer] = nowMs;
				if (!alreadyHeld) ++m_Stats.peers[peer].holds;
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
			// A hold is taken at its own frame; a decision still refused past the silence the round allows a seat is let go, named.
			if (key.second == 0 && key.first > m_Stats.nextFrame) { m_PendingPeerDecisions.emplace(key, kept); continue; }
			InstallPeerBridge(kept.first, nowMs);
			if (const auto again = m_PendingPeerDecisions.find(key); again != m_PendingPeerDecisions.end()) {
				again->second.second = kept.second;
				if (nowMs >= kept.second + c_NetSeatDisconnectSilenceMs) {
					DiagnosticLine() << "[net-peer-decision-dropped] local=" << static_cast<int>(m_Config.localPeerId) << " frame=" << key.first
					    << " kind=" << static_cast<int>(key.second) << " kept_ms=" << nowMs - kept.second << std::endl;
					m_PendingPeerDecisions.erase(again);
				}
			}
		}
		for (const auto& [key, kept]: m_PendingPeerDecisions) if (key.first <= m_Stats.nextFrame) return true;
		// A member that voted for a return, a delay or an admission at the next frame takes it before playing that frame, and so does
		// every member that knows of a return proposed there for a seat still held: the others may certify it without this one's vote.
		std::erase_if(m_PeerVotedAtMs, [this](const auto& voted) { return voted.first.first < m_Stats.nextFrame || m_PeerBridgeCertificates.contains(voted.first); });
		for (auto votes = m_PeerBridgeVotes.lower_bound({m_Stats.nextFrame, 1}); votes != m_PeerBridgeVotes.end() && votes->first.first == m_Stats.nextFrame; ++votes) {
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
			uint32_t required = basis.connectedMask, returning = 0;
			if (basis.preparedFrame == 1) for (uint8_t peer: basis.members) { required &= ~SeatBit(peer); returning |= SeatBit(peer); }
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
			const auto seen = m_PeerVotedAtMs.try_emplace(votes->first, nowMs).first;
			if (nowMs < seen->second + c_NetSeatCatchUpAllowanceMs) return true;
		}
		return false;
	}

	void NetLockstepCoordinator::PreparePeerBridgeInputs(uint64_t frame, uint64_t nowMs) {
		if (const auto change = m_FrameGroupChanges.upper_bound(frame); change != m_FrameGroupChanges.begin()) m_FrameGroupMembers = std::prev(change)->second;
		if (m_FrameGroupMembers != 0 && (m_FrameGroupMembers & SeatBit(m_PeerPaceReference)) == 0)
			m_PeerPaceReference = static_cast<uint8_t>(std::countr_zero(m_FrameGroupMembers) + 1);
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
