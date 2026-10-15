#include "NetPeerFrameHelpers.h"

#include "DiagnosticLine.h"
#include "NetAuthCrypto.h"
#include "NetLobbyProtocol.h"
#include "NetProtocol.h"

#include "lz4.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <limits>
#include <optional>
#include <utility>

namespace RTE {

	using namespace NetPeerFrameDetail;

	void NetLockstepCoordinator::SendForwardedPeerInput(const NetLockstepFrame& input, uint8_t onlyPeer) {
		NetLockstepFrame frame = input;
		frame.priorWindow.clear();
		std::vector<uint8_t> encoded;
		if (!NetLockstepCodec::EncodeRecoveryInput(frame, encoded) || encoded.size() + 5 > NetLockstepCodec::c_InputWindowByteCap) return;
		std::vector<uint8_t> body{1};
		PutSize(body, static_cast<uint32_t>(encoded.size()));
		body.insert(body.end(), encoded.begin(), encoded.end());
		for (size_t offset = 0; offset < body.size(); offset += NetHostMigrationCodec::c_ChunkBytes) {
			auto message = PeerFrameMessage(NetHostMigrationMessageType::PeerForwardInput);
			message.successorPeerId = frame.senderPeerId; message.frame = frame.targetFrame;
			message.totalBytes = static_cast<uint32_t>(body.size()); message.offset = static_cast<uint32_t>(offset);
			const size_t end = std::min(body.size(), offset + NetHostMigrationCodec::c_ChunkBytes);
			message.bytes.assign(body.begin() + offset, body.begin() + end);
			// A requested repair must survive the missing input's original path.
			SendPeerFrameMessage(std::move(message), NetTransportLane::ControlReliable, onlyPeer);
		}
	}

	bool NetLockstepCoordinator::PeerReadsInputLive(uint8_t peer, uint64_t nowMs) const {
		const auto progress = m_PeerOwnReceiptProgress.find(peer);
		if (progress == m_PeerOwnReceiptProgress.end()) return false;
		uint32_t rtt = 0;
		const auto [wire, route] = PeerFrameRoute(peer);
		if (wire && wire->IsPeerPingMeasured(route)) rtt = wire->GetPeerPingMs(route);
		const uint64_t retryMs = std::clamp<uint64_t>(rtt + static_cast<uint64_t>(std::ceil(m_Config.slowPlayerBoundTicks * m_Config.simTickMs)), 50, 1000);
		return nowMs < progress->second.advancedAtMs || nowMs - progress->second.advancedAtMs < retryMs;
	}

	void NetLockstepCoordinator::SendPeerInput(const NetLockstepFrame& frame) {
		if (!UsesPeerFrameGroups()) return;
		NetLockstepFrame current = frame;
		current.priorWindow.clear();
		std::vector<uint8_t> newest;
		if (!NetLockstepCodec::EncodeRecoveryInput(current, newest)) return;
		// A receiver is sent the ticks it has not receipted; a live reader never gets a tick twice on the reliable lane.
		std::map<uint64_t, std::vector<uint8_t>> receivers;
		const auto receipts = m_PeerInputReceipts.find(m_Config.localPeerId);
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if (peer == m_Config.localPeerId || m_RemovedPeers.contains(peer) || !PeerWantsLiveInput(peer)) continue;
			uint64_t oldest = 0;
			if (receipts != m_PeerInputReceipts.end())
				if (const auto receipt = receipts->second.find(peer); receipt != receipts->second.end()) oldest = receipt->second;
			if (const auto sent = m_PeerInputReliableThrough.find(peer); sent != m_PeerInputReliableThrough.end() && PeerReadsInputLive(peer, m_TimingNowMs))
				oldest = std::max(oldest, sent->second + 1);
			receivers[std::min(oldest, frame.targetFrame)].push_back(peer);
		}
		for (const auto& [oldest, peers]: receivers) {
			std::vector<std::vector<uint8_t>> inputs{newest};
			size_t total = 5 + newest.size();
			for (uint32_t back = 1; back < ConfiguredWindowTicks() && frame.targetFrame >= back && frame.targetFrame - back >= oldest; ++back) {
				NetLockstepFrame prior;
				std::vector<uint8_t> encoded;
				if (!FindLocalInput(frame.targetFrame - back, prior)) break;
				prior.priorWindow.clear();
				if (!NetLockstepCodec::EncodeRecoveryInput(prior, encoded) || total + 4 + encoded.size() > NetLockstepCodec::c_InputWindowByteCap) break;
				total += 4 + encoded.size();
				inputs.push_back(std::move(encoded));
			}
			std::vector<uint8_t> body{static_cast<uint8_t>(inputs.size())};
			for (auto input = inputs.rbegin(); input != inputs.rend(); ++input) {
				PutSize(body, static_cast<uint32_t>(input->size()));
				body.insert(body.end(), input->begin(), input->end());
			}
			// Repeated ticks compress together so jitter cover does not become a relay backlog.
			if (body.size() > 1024) {
				std::vector<uint8_t> compressed(LZ4_compressBound(static_cast<int>(body.size())));
				const int size = LZ4_compress_default(reinterpret_cast<const char*>(body.data()), reinterpret_cast<char*>(compressed.data()), static_cast<int>(body.size()), static_cast<int>(compressed.size()));
				if (size > 0 && static_cast<size_t>(size) + 5 < body.size()) {
					std::vector<uint8_t> packed{0}; PutSize(packed, static_cast<uint32_t>(body.size()));
					packed.insert(packed.end(), compressed.begin(), compressed.begin() + size); body = std::move(packed);
				}
			}
			// Repeated input windows repair missing small messages without delaying later inputs.
			const NetTransportLane lane = body.size() <= 4096 ? NetTransportLane::InputUnreliable : NetTransportLane::ControlReliable;
			for (uint8_t peer: peers) {
				bool delivered = true;
				for (size_t offset = 0; offset < body.size(); offset += NetHostMigrationCodec::c_ChunkBytes) {
					auto message = PeerFrameMessage(NetHostMigrationMessageType::PeerInput);
					message.frame = frame.targetFrame;
					message.totalBytes = static_cast<uint32_t>(body.size());
					message.offset = static_cast<uint32_t>(offset);
					const size_t end = std::min(body.size(), offset + NetHostMigrationCodec::c_ChunkBytes);
					message.bytes.assign(body.begin() + offset, body.begin() + end);
					delivered = SendPeerFrameMessage(std::move(message), lane, peer) && delivered;
				}
				if (lane == NetTransportLane::ControlReliable && delivered) m_PeerInputReliableThrough[peer] = frame.targetFrame;
			}
		}
		m_PeerSourceInputs[frame.targetFrame][m_Config.localPeerId] = std::move(current);
		m_PeerInputSentAtMs.try_emplace(frame.targetFrame, m_TimingNowMs);
	}

	void NetLockstepCoordinator::NotePeerInputAccepted(const NetLockstepFrame& frame) {
		if (!UsesPeerFrameGroups()) return;
		m_PeerSourceInputs[frame.targetFrame].try_emplace(frame.senderPeerId, frame);
		auto [accepted, inserted] = m_PeerAcceptedThrough.try_emplace(frame.senderPeerId, EffectiveStartOf(frame.senderPeerId));
		auto& ahead = m_PeerAcceptedAhead[frame.senderPeerId];
		if (frame.targetFrame >= accepted->second) ahead.insert(frame.targetFrame);
		while (accepted->second != UINT64_MAX && ahead.erase(accepted->second) != 0) ++accepted->second;
		auto receipt = PeerFrameMessage(NetHostMigrationMessageType::PeerReceipt);
		receipt.successorPeerId = frame.senderPeerId;
		receipt.frame = accepted->second;
		m_PeerInputReceipts[frame.senderPeerId][m_Config.localPeerId] = accepted->second;
		// Each cumulative receipt replaces a missing one without waiting on the ordered stream.
		SendPeerFrameMessage(std::move(receipt), NetTransportLane::InputUnreliable);
	}

	bool NetLockstepCoordinator::PeerInputAccepted(uint64_t frame, uint8_t owner) const {
		if (owner == 0) owner = m_Config.localPeerId;
		if (frame < EffectiveStartOf(owner) || IsSeatReclaimGap(owner, frame)) return true;
		if (const auto bridge = m_PeerBridges.find(owner); bridge != m_PeerBridges.end() &&
		    frame >= bridge->second.fromFrame && (!bridge->second.returnFrame || frame < *bridge->second.returnFrame))
			return owner != m_Config.localPeerId || m_PeerCommittedTail.contains(frame);
		return PeerGroupHasAuthority(PeerInputVoters(frame, owner));
	}

	uint32_t NetLockstepCoordinator::PeerInputVoters(uint64_t frame, uint8_t owner) const {
		// A removed or released owner's input before its boundary is accepted by the seats that remain.
		uint32_t accepted = m_RemovedPeers.contains(owner) || IsSeatReleased(owner) ? 0 : SeatBit(owner);
		if (const auto receipts = m_PeerInputReceipts.find(owner); receipts != m_PeerInputReceipts.end())
			for (const auto& [peer, through]: receipts->second)
				if (!m_RemovedPeers.contains(peer) && !IsSeatReleased(peer) && through > frame) accepted |= SeatBit(peer);
		return accepted;
	}

	std::optional<uint64_t> NetLockstepCoordinator::GetLocalInputAcceptedThrough() const {
		NET_PLANE_CHECK();
		if (!UsesPeerFrameGroups()) return m_HostAcceptedLocalFrames.empty() ? std::nullopt : std::optional(*m_HostAcceptedLocalFrames.rbegin());
		const uint8_t owner = m_Config.localPeerId;
		uint64_t through = EffectiveStartOf(owner);
		if (const auto reclaim = m_ReclaimTransactions.find(owner); reclaim != m_ReclaimTransactions.end())
			through = std::max(through, reclaim->second.neutralThroughFrame + 1);
		if ((m_FrameGroupMembers & SeatBit(owner)) != 0 && PeerGroupHasAuthority(SeatBit(owner)) && m_LastQueuedTargetFrame != UINT64_MAX)
			through = std::max(through, m_LastQueuedTargetFrame + 1);
		if (const auto found = m_PeerInputReceipts.find(owner); found != m_PeerInputReceipts.end()) {
			for (const auto& [receiver, candidate]: found->second) {
				if (m_RemovedPeers.contains(receiver) || IsSeatReleased(receiver)) continue;
				uint32_t voters = SeatBit(owner);
				for (const auto& [peer, accepted]: found->second)
					if (!m_RemovedPeers.contains(peer) && !IsSeatReleased(peer) && accepted >= candidate) voters |= SeatBit(peer);
				if (PeerGroupHasAuthority(voters)) through = std::max(through, candidate);
			}
		}
		return through == 0 ? std::nullopt : std::optional(through - 1);
	}

	void NetLockstepCoordinator::RepairPeerReceiptGaps(uint64_t nowMs) {
		if ((m_FrameGroupMembers & SeatBit(m_Config.localPeerId)) == 0 || m_LastQueuedTargetFrame == UINT64_MAX) return;
		for (auto& [peer, progress]: m_PeerOwnReceiptProgress) {
			if (m_RemovedPeers.contains(peer) || IsSeatReleased(peer) || (m_FrameGroupMembers & SeatBit(peer)) == 0 || progress.through >= m_LastQueuedTargetFrame ||
			    progress.through < EffectiveStartOf(m_Config.localPeerId) || IsSeatReclaimGap(m_Config.localPeerId, progress.through)) continue;
			uint32_t rtt = 0;
			const auto [wire, route] = PeerFrameRoute(peer);
			if (wire && wire->IsPeerPingMeasured(route)) rtt = wire->GetPeerPingMs(route);
			// Start from the original send so the repair arrives inside the buffered runway.
			const uint64_t retryMs = std::clamp<uint64_t>(rtt + static_cast<uint64_t>(std::ceil(m_Config.slowPlayerBoundTicks * m_Config.simTickMs)), 50, 1000);
			const auto sent = m_PeerInputSentAtMs.find(progress.through);
			if (sent == m_PeerInputSentAtMs.end()) continue;
			const uint64_t since = std::max(sent->second, progress.repairedAtMs);
			if (nowMs < since || nowMs - since < retryMs) continue;
			NetLockstepFrame input;
			if (!FindLocalInput(progress.through, input)) continue;
			SendForwardedPeerInput(input, peer);
			progress.repairedAtMs = nowMs;
			++m_PeerReceiptRepairs;
			DiagnosticLine() << "[net-peer-input-repair] owner=" << static_cast<int>(m_Config.localPeerId)
			    << " receiver=" << static_cast<int>(peer) << " frame=" << progress.through << " stagnant_ms=" << nowMs - progress.advancedAtMs << std::endl;
		}
	}

	void NetLockstepCoordinator::RemovePeerFrameSeat(uint8_t peer, uint64_t frame) {
		if (!UsesPeerFrameGroups()) return;
		m_PeerOwnReceiptProgress.erase(peer); m_PeerReceiptDelaySamples.erase(peer); m_PeerInputReliableThrough.erase(peer);
		m_PeerTailSendRanges.erase(peer); m_PeerTailOutgoing.erase(peer);
		m_PeerTailLastSentMs.erase(peer); m_PeerTailResendFrames.erase(peer);
		for (auto& [owner, receipts]: m_PeerInputReceipts) receipts.erase(peer);
		const auto prior = m_FrameGroupChanges.upper_bound(frame);
		const uint32_t members = prior == m_FrameGroupChanges.begin() ? (1U << m_Config.peerCount) - 1 : std::prev(prior)->second;
		m_FrameGroupChanges.try_emplace(frame, members);
		for (auto change = m_FrameGroupChanges.lower_bound(frame); change != m_FrameGroupChanges.end(); ++change) change->second &= ~SeatBit(peer);
		if (frame <= m_Stats.nextFrame) m_FrameGroupMembers &= ~SeatBit(peer);
		m_PeerAdminFrameMembers &= ~SeatBit(peer);
	}

	bool NetLockstepCoordinator::HandlePeerFrameMessage(const NetHostMigrationMessage& message, uint64_t nowMs, const NetHash32* readKey) {
		if (HandlePeerSessionMessage(message, nowMs, readKey)) return true;
		if (!UsesPeerFrameGroups() || message.type < NetHostMigrationMessageType::PeerInput ||
		    (message.type > NetHostMigrationMessageType::PeerControl && message.type != NetHostMigrationMessageType::PeerForwardInput)) return false;
		const uint8_t detailOwner = message.type == NetHostMigrationMessageType::PeerForwardInput ? message.successorPeerId : message.senderPeerId;
		const auto detailBack = m_ReclaimTransactions.find(detailOwner);
		if (std::getenv("CC_TEST_RETURN_INPUT_DETAILS") && (message.type == NetHostMigrationMessageType::PeerInput || message.type == NetHostMigrationMessageType::PeerForwardInput) &&
		    detailBack != m_ReclaimTransactions.end() && message.frame > detailBack->second.neutralThroughFrame && message.frame <= detailBack->second.neutralThroughFrame + 3) {
			DiagnosticLine() << "[net-return-input-read] local=" << static_cast<int>(m_Config.localPeerId) << " owner=" << static_cast<int>(detailOwner)
			    << " frame=" << message.frame << " prepared=" << m_Stats.nextFrame << " session_matches=" << (message.sessionId == m_Config.sessionId)
			    << " round_matches=" << (message.roundId == m_Config.matchConfig.roundId) << " setup_matches=" << (message.configHash == m_RoundConfigHash)
			    << " removed=" << m_RemovedPeers.contains(message.senderPeerId) << " now_ms=" << nowMs << std::endl;
		}
		if (PeerFrameBlackout(nowMs)) return true;
		if (message.sessionId != m_Config.sessionId || message.roundId != m_Config.matchConfig.roundId || message.configHash != m_RoundConfigHash ||
		    message.senderPeerId == 0 || message.senderPeerId == m_Config.localPeerId || message.senderPeerId > m_Config.peerCount || message.generation != 1 || m_RemovedPeers.contains(message.senderPeerId)) return true;
		const auto heard = [&] {
			if (m_Config.peerSessionLinks) m_Config.peerSessionLinks->progressAt[message.senderPeerId] = nowMs;
			m_PeerLastHeardMs[message.senderPeerId] = nowMs;
			m_PeerLinkHeardMs[message.senderPeerId] = nowMs;
			m_Stats.peers[message.senderPeerId].lastHeardMs = nowMs;
			m_Stats.peers[m_Config.localPeerId].lastHeardMs = nowMs;
			auto& applied = m_PeerAppliedThrough[message.senderPeerId];
			if (!m_PeerAppliedAtMs.contains(message.senderPeerId) || message.appliedFrame > applied) {
				applied = std::max(applied, message.appliedFrame);
				m_PeerAppliedAtMs[message.senderPeerId] = nowMs;
				auto& progress = m_PeerAppliedProgress[message.senderPeerId];
				progress.emplace_back(nowMs, applied);
				while (progress.size() > 2 && progress.front().first + 3000 < nowMs) progress.pop_front();
			}
			if (message.senderPeerId == GetHostPeerId()) NoteAuthorityHeard(nowMs);
		};
		if (message.type == NetHostMigrationMessageType::PeerReceipt) {
			if (message.successorPeerId == message.senderPeerId && message.preparedFrame == UINT64_MAX && message.bytes.size() == 2 * NetHash32{}.size() && message.appliedFrame == message.frame &&
			    message.frame >= m_Config.startFrame && message.frame <= m_Stats.nextFrame + NetLockstepCodec::c_MaxFutureFrameSkew &&
			    (message.voterMask & SeatBit(message.senderPeerId)) != 0 && (message.voterMask >> m_Config.peerCount) == 0) {
				heard();
				m_PeerFrameWitnesses[message.frame].try_emplace(message.senderPeerId, message);
				if (m_PeerTailVotes.contains(message.frame)) ConfirmPeerTail(message.frame, nowMs);
				return true;
			}
			if (message.successorPeerId > 0 && message.successorPeerId <= m_Config.peerCount && message.bytes.empty() &&
			    message.frame >= m_Config.startFrame && message.frame <= m_Stats.nextFrame + NetLockstepCodec::c_MaxFutureFrameSkew + 1) {
				heard();
				auto& receipt = m_PeerInputReceipts[message.successorPeerId][message.senderPeerId];
				const bool advanced = message.frame > receipt;
				if (message.successorPeerId == m_Config.localPeerId && advanced) {
					++m_InputAcceptanceReceipts;
					auto& progress = m_PeerOwnReceiptProgress[message.senderPeerId];
					progress = {message.frame, nowMs, 0};
				}
				if (message.successorPeerId == m_Config.localPeerId && advanced && message.frame > 0) if (const auto sent = m_PeerInputSentAtMs.find(message.frame - 1); sent != m_PeerInputSentAtMs.end() && nowMs >= sent->second)
				{
					const auto group = m_FrameGroupChanges.upper_bound(message.frame - 1);
					const uint32_t members = group == m_FrameGroupChanges.begin() ? (1U << m_Config.peerCount) - 1 : std::prev(group)->second;
					const auto bridge = m_PeerBridges.find(message.senderPeerId);
					const bool live = bridge == m_PeerBridges.end() || (bridge->second.returnFrame && message.appliedFrame >= *bridge->second.returnFrame);
					// A private tail reader's delayed receipt measures its catch-up,
					// not the live sender's transport round trip.
					if (live && sent->second >= m_PeerReceiptSampleFloorMs[message.senderPeerId] && (members & m_FrameGroupMembers & SeatBit(message.senderPeerId)) != 0) {
						const auto delay = static_cast<uint32_t>(std::min<uint64_t>(nowMs - sent->second, UINT32_MAX));
						const auto [wire, route] = PeerFrameRoute(message.senderPeerId);
						const uint64_t reserve = c_NetInputJitterReserveMs +
						    static_cast<uint64_t>(std::ceil((m_Config.slowPlayerBoundTicks + 1) * m_Config.simTickMs));
						// Receipts retained through a placement pause measure the pause,
						// even when no combat bridge changed the sample floor.
						if (!wire || !wire->IsPeerPingMeasured(route) || delay <= wire->GetPeerPingMs(route) + reserve) {
							m_PeerReceiptDelaySamples[message.senderPeerId].Observe(nowMs, delay);
							m_PeerArrivalLatencyMs[m_Config.localPeerId] = {nowMs, delay};
						}
					}
				}
				receipt = std::max(receipt, message.frame);
				ResolvePeerBridgeInputConflicts(nowMs);
			}
			return true;
		}
		if (message.type == NetHostMigrationMessageType::PeerBridge) {
			// A hold made now is voted at its frame; one agreed ahead is voted as it arrives.
			if (message.preparedFrame == 0 && message.boundary >= message.frame && message.frame > m_Stats.nextFrame && message.frame - m_Stats.nextFrame <= NetLockstepCodec::c_MaxFutureFrameSkew) {
				m_PeerBridgeVotes[{message.frame, DecisionKind(message)}].try_emplace(message.senderPeerId, message);
				return true;
			}
			if (!ValidatePeerBridge(message) && !CoSignsOwnHold(message)) return true;
			heard();
			auto& votes = m_PeerBridgeVotes[{message.frame, DecisionKind(message)}];
			const auto own = votes.find(m_Config.localPeerId);
			if (own != votes.end() && !SameBridge(own->second, message)) return true;
			votes.try_emplace(message.senderPeerId, message);
			if (own == votes.end() && !VotedOtherPeerReturn(message)) {
				auto reply = message;
				reply.senderPeerId = m_Config.localPeerId;
				reply.appliedFrame = m_LastCompletedSimulationTick.value_or(0);
				reply.voterMask = SeatBit(m_Config.localPeerId);
				if (reply.preparedFrame == 3) m_PeerAdminOwnVote = reply;
				votes.emplace(m_Config.localPeerId, reply);
				m_PeerVotedAtMs.try_emplace({reply.frame, DecisionKind(reply)}, nowMs);
				if (reply.preparedFrame == 0) for (uint8_t peer: reply.members) m_PeerRejectedInputs[reply.frame] |= SeatBit(peer);
				SendPeerFrameMessage(std::move(reply));
			}
			TryCommitPeerBridge(message.frame, nowMs, DecisionKind(message));
			return true;
		}
		if (message.type == NetHostMigrationMessageType::PeerBridgeCommit) {
			NetHostMigrationMessage agreed; uint32_t voters = 0;
			if (message.preparedFrame == 1 && DecodePeerBridgeCertificate(message, agreed, voters)) {
				m_PeerReturnProofs.try_emplace(message.frame, message);
				while (m_PeerReturnProofs.size() > 32) m_PeerReturnProofs.erase(m_PeerReturnProofs.begin());
			}
			InstallPeerBridge(message, nowMs);
			if (m_PeerBridgeCertificates.contains({message.frame, DecisionKind(message)})) heard();
			return true;
		}
		if (message.type == NetHostMigrationMessageType::PeerTailRequest) {
			if (message.bytes.empty() && message.frame <= UINT64_MAX - 128 && message.completeFrom > message.frame &&
			    message.completeFrom - message.frame <= 128) {
				heard();
				auto& repeats = m_PeerTailResendFrames[message.senderPeerId];
				const auto& sent = m_PeerTailLastSentMs[message.senderPeerId];
				for (uint64_t frame = message.frame; frame < message.completeFrom; ++frame)
					if (sent.contains(frame) && repeats.size() < NetLockstepCodec::c_MaxFutureFrameSkew) repeats.insert(frame);
				auto [range, inserted] = m_PeerTailSendRanges.try_emplace(message.senderPeerId, message.frame, message.completeFrom);
				if (!inserted) {
					range->second.first = std::min(range->second.first, message.frame);
					range->second.second = std::max(range->second.second, message.completeFrom);
				}
				SendPeerCommittedTail(message.senderPeerId, message.frame);
			}
			return true;
		}
		if (message.type == NetHostMigrationMessageType::PeerTailMissing) {
			const bool longHitch = m_ConsumerWaitingFrame && nowMs >= m_ConsumerWaitStartMs && nowMs - m_ConsumerWaitStartMs >= c_NetSeatDisconnectSilenceMs;
			// History older than the donor keeps cannot carry the split's certificate, so the split's name asks the referee instead.
			if (message.frame == m_Stats.nextFrame && (message.connectedMask & SeatBit(m_Config.localPeerId)) == 0 && PeerGroupIsRefereedHalf(message.connectedMask) &&
			    !PeerGroupHasAuthority(message.connectedMask)) CheckPeerFrameTie(message.frame, message.connectedMask, nowMs);
			if (message.frame == m_Stats.nextFrame && PeerGroupHasAuthority(message.connectedMask) && longHitch) {
				m_LocalSeatHeld = true; m_LocalHoldFrame = m_Stats.nextFrame;
				m_State = NetLockstepState::Stopped; m_Stats.timeoutReason = "PeerHeld:the committed tail is unavailable; returning through a private image";
			}
			return true;
		}
		if (message.type == NetHostMigrationMessageType::PeerHeartbeat) {
			if (message.bytes.size() != 8 || (message.connectedMask & ~((1U << m_Config.peerCount) - 1)) != 0) return true;
			size_t offset = 0; uint32_t rtt = 0, jitter = 0;
			if (!TakeSize(message.bytes, offset, rtt) || !TakeSize(message.bytes, offset, jitter) || rtt > 60000 || jitter > 60000) return true;
			heard();
			m_Stats.peers[message.senderPeerId].pingMs = rtt; m_Stats.peers[message.senderPeerId].jitterMs = jitter;
			m_PeerAdminReady[message.senderPeerId] = {message.completeFrom, nowMs};
			m_Stats.peers[message.senderPeerId].pingMeasured = rtt != 0;
			// Only this authenticated heartbeat names a prepared horizon. Bridge
			// kinds and receipt witnesses reuse that field for other purposes.
			if (message.preparedFrame != UINT64_MAX && (message.preparedFrame <= m_Stats.nextFrame ||
			    message.preparedFrame - m_Stats.nextFrame <= NetLockstepCodec::c_MaxFutureFrameSkew))
				m_Stats.peers[message.senderPeerId].reportedNextFrame = std::max(m_Stats.peers[message.senderPeerId].reportedNextFrame, message.preparedFrame + 1);
			// The other half of a refereed split holds history only on the referee's word: ask it what it decided for this split.
			const bool otherHalf = (message.connectedMask & SeatBit(m_Config.localPeerId)) == 0 && PeerGroupIsRefereedHalf(message.connectedMask);
			if (otherHalf && !PeerGroupHasAuthority(message.connectedMask)) CheckPeerFrameTie(m_Stats.nextFrame, message.connectedMask, nowMs);
			if ((otherHalf || PeerGroupHasAuthority(message.connectedMask)) && message.appliedFrame >= m_Stats.nextFrame)
				RequestPeerCommittedTail(nowMs);
			return true;
		}
		if (message.type == NetHostMigrationMessageType::PeerControl) {
			const auto decoded = NetLockstepCodec::Decode(message.bytes, ControllerFrame::c_Version);
			if (decoded.ok && std::visit([&](const auto& payload) {
				if constexpr (std::is_same_v<std::decay_t<decltype(payload)>, NetLockstepStop>)
					if (payload.reason == NetLockstepStopReason::PeerRemoved && message.senderPeerId == GetHostPeerId() &&
					    payload.senderPeerId > 0 && payload.senderPeerId <= m_Config.peerCount && payload.senderPeerId != GetHostPeerId()) return true;
				if constexpr (requires { payload.senderPeerId; }) return payload.senderPeerId == message.senderPeerId;
				else return payload.localPeerId == message.senderPeerId;
			}, decoded.packet.payload)) {
				heard();
				m_AuthenticatedPeerMessage = message.senderPeerId;
				HandlePacket(decoded.packet, nowMs, c_InvalidNetPeerId);
				m_AuthenticatedPeerMessage = 0;
			}
			return true;
		}
		const auto key = std::tuple{message.senderPeerId, message.type, message.frame};
		const bool inputMessage = message.type == NetHostMigrationMessageType::PeerInput || message.type == NetHostMigrationMessageType::PeerForwardInput;
		if (message.type == NetHostMigrationMessageType::PeerTail && message.offset == 0 &&
		    (message.frame < m_Stats.nextFrame || m_PeerCommittedTail.contains(message.frame))) ++m_PeerTailDuplicateFrames;
		const size_t byteCap = inputMessage ? NetLockstepCodec::c_InputWindowByteCap : NetHostMigrationCodec::c_MaxPeerTailBytes;
		if (message.totalBytes == 0 || message.totalBytes > byteCap || message.offset > message.totalBytes ||
		    message.bytes.size() > message.totalBytes - message.offset) return true;
		if ((!inputMessage && message.frame < m_Stats.nextFrame) ||
		    (message.frame >= m_Stats.nextFrame && message.frame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew) ||
		    (message.frame < m_Stats.nextFrame && m_Stats.nextFrame - message.frame > NetHostMigrationCodec::c_PeerHistoryFrames) ||
		    (message.type == NetHostMigrationMessageType::PeerInput && message.totalBytes > NetLockstepCodec::c_InputWindowByteCap)) return true;
		if (!m_PeerFrameIncoming.contains(key) && m_PeerFrameIncoming.size() >= 2 * m_Config.peerCount) {
			if (inputMessage) DiagnosticLine() << "[net-peer-input-room] local=" << static_cast<int>(m_Config.localPeerId)
			    << " sender=" << static_cast<int>(message.senderPeerId) << " owner=" << static_cast<int>(message.successorPeerId)
			    << " through=" << message.frame << " partial=" << m_PeerFrameIncoming.size() << std::endl;
			return true;
		}
		auto& incoming = m_PeerFrameIncoming[key];
		incoming.lastReceivedMs = nowMs;
		if (message.offset == 0) { incoming.totalBytes = message.totalBytes; incoming.bytes.clear(); }
		if (incoming.totalBytes != message.totalBytes || message.offset != incoming.bytes.size()) { m_PeerFrameIncoming.erase(key); return true; }
		incoming.bytes.insert(incoming.bytes.end(), message.bytes.begin(), message.bytes.end());
		if (incoming.bytes.size() != incoming.totalBytes) return true;
		std::vector<uint8_t> body = std::move(incoming.bytes); m_PeerFrameIncoming.erase(key);
		if (message.type == NetHostMigrationMessageType::PeerTail) {
			const auto readFailed = [&](const char* reason) {
				if (std::getenv("CC_TEST_RETURN_INPUT_DETAILS")) DiagnosticLine() << "[net-peer-tail-read] local=" << static_cast<int>(m_Config.localPeerId)
				    << " donor=" << static_cast<int>(message.senderPeerId) << " frame=" << message.frame << " prepared=" << m_Stats.nextFrame
				    << " reason=" << reason << " now_ms=" << nowMs << std::endl;
				return true;
			};
			if (message.completeFrom > 1) return true;
			if (message.completeFrom == 1) {
				size_t offset = 0; uint32_t expandedSize = 0;
				if (!TakeSize(body, offset, expandedSize) || expandedSize < 70 || expandedSize > NetHostMigrationCodec::c_MaxPeerTailBytes) return true;
				std::vector<uint8_t> expanded(expandedSize);
				if (LZ4_decompress_safe(reinterpret_cast<const char*>(body.data() + offset), reinterpret_cast<char*>(expanded.data()),
				    static_cast<int>(body.size() - offset), static_cast<int>(expanded.size())) != static_cast<int>(expanded.size())) return true;
				body = std::move(expanded);
			}
			if ((message.voterMask & SeatBit(message.senderPeerId)) == 0 || message.appliedFrame < message.frame || body.size() < 70) return true;
			NetHash32 prefix{}, prepared{}; std::copy_n(body.begin(), prefix.size(), prefix.begin());
			std::copy_n(body.begin() + prefix.size(), prepared.size(), prepared.begin());
			size_t at = prefix.size() + prepared.size(); uint32_t size = 0;
			if (!TakeSize(body, at, size) || size > NetHostMigrationCodec::c_MaxFrameBytes || at > body.size() || size > body.size() - at) return true;
			std::vector<uint8_t> record(body.begin() + at, body.begin() + at + size); at += size;
			NetLockstepReadyFrame decoded;
			if (!DecodeMigrationFrame(record, message.frame, decoded) || at == body.size()) return readFailed("frame-record");
			const uint8_t count = body[at++]; if (count > 32) return true;
			std::vector<NetHostMigrationMessage> proofs;
			// A certified return of this seat carried here: its activation and the end of its neutral gap.
			std::optional<std::pair<uint64_t, uint64_t>> ownReturnGap;
			for (uint8_t n = 0; n < count; ++n) {
				if (!TakeSize(body, at, size) || at > body.size() || size > body.size() - at) return true;
				NetHostMigrationMessage proof, agreed; uint32_t voters = 0;
				if (!NetHostMigrationCodec::Decode({body.begin() + at, body.begin() + at + size}, m_Config.migrationKey, proof) ||
				    proof.type != NetHostMigrationMessageType::PeerBridgeCommit || !DecodePeerBridgeCertificate(proof, agreed, voters) ||
				    (proof.frame > message.frame && agreed.preparedFrame != 3)) return readFailed("bridge-record");
				if (agreed.preparedFrame == 3 && agreed.completeFrom > m_Config.migrationGeneration) {
					NoteSuperseded(agreed.completeFrom);
					if (PeerAdminCertificateValid(agreed, voters) && 2 * std::popcount(voters) >= static_cast<int>(m_Config.peerCount)) m_PeerAdminFrameMembers = voters;
				} else if (agreed.connectedMask == message.voterMask && !PeerGroupHasAuthority(voters)) CheckPeerFrameTie(agreed.frame, voters, nowMs);
				if (agreed.preparedFrame == 1) {
					size_t offset = NetHash32{}.size();
					for (uint8_t member: agreed.members) {
						uint32_t delay = 0, neutral = 0, incarnation = 0;
						if (!TakeSize(agreed.bytes, offset, delay) || !TakeSize(agreed.bytes, offset, neutral) || !TakeSize(agreed.bytes, offset, incarnation)) break;
						if (member == m_Config.localPeerId) ownReturnGap = {agreed.frame, agreed.frame + std::max(neutral, delay)};
					}
				}
				proofs.push_back(std::move(proof)); at += size;
			}
			if (at == body.size()) return true;
			const uint8_t witnessCount = body[at++];
			if (witnessCount > m_Config.peerCount) return true;
			const NetHash32 digest = CommittedFramePrefix(decoded);
			uint32_t witnessed = 0;
			for (uint8_t n = 0; n < witnessCount; ++n) {
				if (!TakeSize(body, at, size) || at > body.size() || size > body.size() - at) return true;
				NetHostMigrationMessage witness;
				if (!NetHostMigrationCodec::Decode({body.begin() + at, body.begin() + at + size}, m_Config.migrationKey, witness) ||
				    witness.type != NetHostMigrationMessageType::PeerReceipt || witness.successorPeerId != witness.senderPeerId || witness.preparedFrame != UINT64_MAX || witness.sessionId != m_Config.sessionId ||
				    witness.roundId != m_Config.matchConfig.roundId || witness.configHash != m_RoundConfigHash || witness.generation != 1 ||
				    witness.frame != message.frame || witness.appliedFrame != message.frame || witness.voterMask != message.voterMask ||
				    witness.senderPeerId == 0 || witness.senderPeerId > m_Config.peerCount || (witnessed & SeatBit(witness.senderPeerId)) != 0 ||
				    (message.voterMask & SeatBit(witness.senderPeerId)) == 0 || witness.bytes.size() != 64 ||
				    !std::equal(digest.begin(), digest.end(), witness.bytes.begin()) || !std::equal(prepared.begin(), prepared.end(), witness.bytes.begin() + 32)) return readFailed("witness-record");
				witnessed |= SeatBit(witness.senderPeerId); at += size;
			}
			if (at != body.size()) return true;
			heard();
			if (!PeerGroupHasAuthority(message.voterMask)) return true;
			uint32_t agreeing = witnessed;
			if ((message.voterMask & SeatBit(m_Config.localPeerId)) != 0) {
				const auto own = m_PeerSourceInputs.find(message.frame);
				if (own != m_PeerSourceInputs.end() && own->second.contains(m_Config.localPeerId)) {
					auto input = own->second.at(m_Config.localPeerId), retained = input;
					input.commands.clear(); input.observations.clear(); input.valueObservations.clear(); input.priorWindow.clear();
					retained = input; retained.frames = FramesForSeat(decoded, m_Config.localPeerId);
					std::vector<uint8_t> a, b;
					if (NetLockstepCodec::EncodeRecoveryInput(input, a) && NetLockstepCodec::EncodeRecoveryInput(retained, b) && a == b) agreeing |= SeatBit(m_Config.localPeerId);
				} else if (IsSeatReclaimGap(m_Config.localPeerId, message.frame) || (ownReturnGap && message.frame >= ownReturnGap->first && message.frame <= ownReturnGap->second)) {
					// Inside its own return's neutral gap this seat sent nothing, so it confirms a record that carries nothing for it.
					const auto sentByThisSeat = [&](const std::vector<NetGameCommand>& commands) {
						return std::any_of(commands.begin(), commands.end(), [&](const NetGameCommand& command) { return command.senderPeerId == m_Config.localPeerId; });
					};
					if (FramesForSeat(decoded, m_Config.localPeerId).empty() && !sentByThisSeat(decoded.localCommands) && !sentByThisSeat(decoded.remoteCommands))
						agreeing |= SeatBit(m_Config.localPeerId);
				}
			}
			// The frame waits with what confirms it until its group does; a repeat from the same donor adds the witnesses it now carries.
			auto& votes = m_PeerTailVotes[message.frame];
			const auto [vote, fresh] = votes.try_emplace(message.senderPeerId, PeerTailVote{message.voterMask, prefix, prepared, digest, record, std::move(proofs), agreeing, message.appliedFrame});
			if (!fresh && vote->second.members == message.voterMask && vote->second.prefix == prefix && vote->second.prepared == prepared && vote->second.bytes == record)
				vote->second.confirmed |= agreeing;
			ConfirmPeerTail(message.frame, nowMs);
			return true;
		}
		if (!inputMessage || body.empty()) return true;
		const uint8_t owner = message.type == NetHostMigrationMessageType::PeerForwardInput ? message.successorPeerId : message.senderPeerId;
		if (owner == 0 || owner > m_Config.peerCount || m_RemovedPeers.contains(owner)) return true;
		const auto wireBody = body;
		if (body.front() == 0) {
			size_t packedOffset = 1; uint32_t size = 0;
			if (!TakeSize(body, packedOffset, size) || size == 0 || size > NetLockstepCodec::c_InputWindowByteCap) return true;
			std::vector<uint8_t> unpacked(size);
			const int decoded = LZ4_decompress_safe(reinterpret_cast<const char*>(body.data() + packedOffset), reinterpret_cast<char*>(unpacked.data()), static_cast<int>(body.size() - packedOffset), static_cast<int>(unpacked.size()));
			if (decoded < 0 || static_cast<uint32_t>(decoded) != size) return true;
			body = std::move(unpacked);
		}
		const uint8_t count = body.front();
		if (count == 0 || count > NetLockstepCodec::c_PeerInputWindowTicks) return true;
		size_t offset = 1;
		std::vector<const NetLockstepFrame*> inputs;
		std::deque<NetLockstepFrame> unshared;
		auto& decoded = m_PeerInputDecodes[owner];
		// The window carries each input again in every message until it leaves it, so an entry this peer decoded before is reused.
		const auto prune = [&] {
			if (decoded.size() > 2U * NetLockstepCodec::c_PeerInputWindowTicks)
				std::erase_if(decoded, [&](const auto& entry) { return entry.second.input.targetFrame + NetLockstepCodec::c_PeerInputWindowTicks < message.frame; });
		};
		for (uint8_t n = 0; n < count; ++n) {
			uint32_t size = 0;
			if (!TakeSize(body, offset, size) || offset > body.size() || size > body.size() - offset) return true;
			const auto first = body.begin() + offset, last = first + size;
			uint64_t hash = 14695981039346656037ULL;
			for (auto byte = first; byte != last; ++byte) hash = (hash ^ *byte) * 1099511628211ULL;
			auto known = decoded.find(hash);
			if (known == decoded.end() || !std::equal(first, last, known->second.bytes.begin(), known->second.bytes.end())) {
				NetLockstepFrame input;
				NetLockstepError readError;
				if (!NetLockstepCodec::DecodeRecoveryInput({first, last}, input, &readError)) {
					DiagnosticLine() << "[net-peer-input-read] local=" << static_cast<int>(m_Config.localPeerId) << " owner=" << static_cast<int>(owner)
					    << " through=" << message.frame << " bytes=" << size << " reason=" << readError.message << std::endl;
					return true;
				}
				if (known == decoded.end()) known = decoded.emplace(hash, PeerInputDecode{{first, last}, std::move(input)}).first;
				else { unshared.push_back(std::move(input)); inputs.push_back(&unshared.back()); offset += size; continue; }
			}
			const NetLockstepFrame& input = known->second.input;
			if (input.senderPeerId != owner || input.roundId != m_RoundId || input.targetFrame > message.frame || (n > 0 && input.targetFrame <= inputs.back()->targetFrame)) {
				DiagnosticLine() << "[net-peer-input-shape] local=" << static_cast<int>(m_Config.localPeerId) << " owner=" << static_cast<int>(owner)
				    << " through=" << message.frame << " input=" << input.targetFrame << " sender=" << static_cast<int>(input.senderPeerId)
				    << " round_matches=" << (input.roundId == m_RoundId) << " index=" << static_cast<int>(n) << std::endl;
				return true;
			}
			inputs.push_back(&input); offset += size;
		}
		if (offset != body.size() || inputs.back()->targetFrame != message.frame) return true;
		heard();
		if (owner == m_Config.localPeerId) { prune(); return true; }
		if (message.frame < m_Stats.nextFrame) {
			if (const auto through = m_PeerAcceptedThrough.find(owner); through != m_PeerAcceptedThrough.end() && through->second > message.frame)
				NotePeerInputAccepted(*inputs.back());
			prune();
			return true;
		}
		if (m_PeerPaceStartMs && m_Config.simTickMs > 0) {
			const uint64_t produced = message.frame - std::min<uint64_t>(message.frame, InputDelayAt(owner, message.frame));
			if (produced >= m_PeerPaceStartFrame) {
				const double expected = *m_PeerPaceStartMs + (produced - m_PeerPaceStartFrame) * m_Config.simTickMs;
				m_PeerArrivalLatencyMs[owner] = {nowMs, nowMs > expected ? static_cast<uint32_t>(std::min<double>(nowMs - expected, UINT32_MAX)) : 0};
			}
		}
		// An input already received on one live path must reach the other voters before a bridge can be agreed.
		if (!m_PeerForwardedInputs.contains(owner) || message.frame > m_PeerForwardedInputs.at(owner)) {
			m_PeerForwardedInputs[owner] = message.frame;
			for (size_t start = 0; start < wireBody.size(); start += NetHostMigrationCodec::c_ChunkBytes) {
				auto forwarded = PeerFrameMessage(NetHostMigrationMessageType::PeerForwardInput);
				forwarded.successorPeerId = owner; forwarded.frame = message.frame;
				forwarded.totalBytes = static_cast<uint32_t>(wireBody.size()); forwarded.offset = static_cast<uint32_t>(start);
				const size_t end = std::min(wireBody.size(), start + NetHostMigrationCodec::c_ChunkBytes);
				forwarded.bytes.assign(wireBody.begin() + start, wireBody.begin() + end);
				SendPeerFrameMessage(std::move(forwarded), wireBody.size() <= 4096 ? NetTransportLane::InputUnreliable : NetTransportLane::ControlReliable);
			}
		}
		for (const NetLockstepFrame* entry: inputs) {
			const NetLockstepFrame& input = *entry;
			if (input.targetFrame < m_Stats.nextFrame || input.targetFrame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew) continue;
			if ((m_PeerRejectedInputs[input.targetFrame] & SeatBit(input.senderPeerId)) != 0) {
				if (!m_PeerBridgeCertificates.contains({input.targetFrame, 0})) {
					const auto pending = m_PeerPendingBridgeInputs[input.targetFrame].try_emplace(input.senderPeerId, input);
					if (pending.second && std::getenv("CC_TEST_RETURN_INPUT_DETAILS")) DiagnosticLine() << "[net-peer-input-at-vote] local=" << static_cast<int>(m_Config.localPeerId)
					    << " owner=" << static_cast<int>(input.senderPeerId) << " frame=" << input.targetFrame << " prepared=" << m_Stats.nextFrame
					    << " displayed=" << m_LastCompletedSimulationTick.value_or(0) << " now_ms=" << nowMs << std::endl;
				}
				continue;
			}
			const auto bridge = m_PeerBridges.find(input.senderPeerId);
			if (bridge != m_PeerBridges.end() && (!bridge->second.returnFrame || input.targetFrame < *bridge->second.returnFrame))
				m_PeerSourceInputs[input.targetFrame].try_emplace(input.senderPeerId, input);
			else AcceptRemoteTick(input, nowMs, false);
		}
		prune();
		return true;
	}

} // namespace RTE
