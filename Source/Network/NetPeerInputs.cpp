#include "NetPeerFrameHelpers.h"

#include "DiagnosticLine.h"
#include "GnsTransport.h"
#include "NetAuthCrypto.h"
#include "NetLobbyProtocol.h"
#include "NetProtocol.h"

#include "lz4.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <utility>

namespace RTE {

	using namespace NetPeerFrameDetail;

	namespace {
		bool ObservedReceiptFrame(uint64_t frame) {
			static const char* value = std::getenv("CC_TEST_LOCKSTEP_OBSERVE_TARGET");
			static const uint64_t observed = value ? std::strtoull(value, nullptr, 10) : UINT64_MAX;
			return value && frame >= observed && frame - observed <= 1;
		}

		// Test lever CC_TEST_PEER_INPUT_WIRE: once a second, what the peer input path put on each lane and how its receipts came back.
		struct InputWireStats {
			struct Lane { uint64_t sends = 0, bytes = 0, maxBytes = 0, maxTicks = 0; };
			struct Peer { Lane unreliable, reliable; uint64_t repairs = 0, repairBytes = 0, receiptsSent = 0, receiptsRead = 0, maxReceiptMs = 0; };
			struct Gap { uint64_t frame = 0, openedMs = 0, aheadFrame = 0; };
			uint64_t sinceMs = 0;
			std::map<uint8_t, Peer> peers;
			std::map<uint8_t, Gap> gaps; //!< Per owner: the first input missing while a later one is in.
		};
		InputWireStats* InputWire() {
			static const bool on = std::getenv("CC_TEST_PEER_INPUT_WIRE") != nullptr;
			static InputWireStats stats;
			return on ? &stats : nullptr;
		}
		void FlushInputWire(InputWireStats& stats, uint8_t local, uint64_t frame, uint64_t nowMs) {
			if (stats.sinceMs == 0) stats.sinceMs = nowMs;
			if (nowMs < stats.sinceMs + 1000) return;
			for (const auto& [peer, wire]: stats.peers)
				DiagnosticLine() << "[net-peer-input-wire] local=" << unsigned(local) << " peer=" << unsigned(peer) << " frame=" << frame << " ms=" << nowMs - stats.sinceMs
				    << " unreliable=" << wire.unreliable.sends << "/" << wire.unreliable.bytes << "/" << wire.unreliable.maxBytes << "/" << wire.unreliable.maxTicks
				    << " reliable=" << wire.reliable.sends << "/" << wire.reliable.bytes << "/" << wire.reliable.maxBytes << "/" << wire.reliable.maxTicks
				    << " repairs=" << wire.repairs << "/" << wire.repairBytes << " receipts_sent=" << wire.receiptsSent << " receipts_read=" << wire.receiptsRead
				    << " max_receipt_ms=" << wire.maxReceiptMs << " now_ms=" << nowMs << std::endl;
			stats.peers.clear();
			stats.sinceMs = nowMs;
		}
	}

	void NetLockstepCoordinator::SendForwardedPeerInput(const NetLockstepFrame& input, uint8_t onlyPeer) {
		NetLockstepFrame frame = input;
		frame.priorWindow.clear();
		std::vector<uint8_t> encoded;
		if (!NetLockstepCodec::EncodeRecoveryInput(frame, encoded) || encoded.size() + 5 > NetLockstepCodec::c_InputWindowByteCap) return;
		std::vector<uint8_t> body{1};
		PutSize(body, static_cast<uint32_t>(encoded.size()));
		body.insert(body.end(), encoded.begin(), encoded.end());
		if (InputWireStats* wire = InputWire(); wire && onlyPeer != 0) {
			auto& peer = wire->peers[onlyPeer];
			++peer.repairs;
			peer.repairBytes += body.size() * (body.size() <= 4096 ? 2 : 1);
		}
		for (size_t offset = 0; offset < body.size(); offset += NetHostMigrationCodec::c_ChunkBytes) {
			auto message = PeerFrameMessage(NetHostMigrationMessageType::PeerForwardInput);
			message.successorPeerId = frame.senderPeerId; message.frame = frame.targetFrame;
			message.totalBytes = static_cast<uint32_t>(body.size()); message.offset = static_cast<uint32_t>(offset);
			const size_t end = std::min(body.size(), offset + NetHostMigrationCodec::c_ChunkBytes);
			message.bytes.assign(body.begin() + offset, body.begin() + end);
			// Small gap repairs have the live input's independent path as well as eventual reliable delivery. An older
			// lost control packet must not hold this exact missing input behind the ordered stream's retransmission.
			if (body.size() <= 4096) SendPeerFrameMessage(message, NetTransportLane::InputUnreliable, onlyPeer);
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
		GnsTransport::ObserveOutgoingLockstepFrame(frame.targetFrame);
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
			if (InputWireStats* wire = InputWire()) for (uint8_t peer: peers) {
				auto& sent = lane == NetTransportLane::ControlReliable ? wire->peers[peer].reliable : wire->peers[peer].unreliable;
				++sent.sends; sent.bytes += body.size();
				sent.maxBytes = std::max<uint64_t>(sent.maxBytes, body.size()); sent.maxTicks = std::max<uint64_t>(sent.maxTicks, inputs.size());
			}
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
		if (InputWireStats* wire = InputWire()) FlushInputWire(*wire, m_Config.localPeerId, frame.targetFrame, m_TimingNowMs);
	}

	void NetLockstepCoordinator::NotePeerInputAccepted(const NetLockstepFrame& frame) {
		if (!UsesPeerFrameGroups()) return;
		m_PeerSourceInputs[frame.targetFrame].try_emplace(frame.senderPeerId, frame);
		auto [accepted, inserted] = m_PeerAcceptedThrough.try_emplace(frame.senderPeerId, EffectiveStartOf(frame.senderPeerId));
		auto& ahead = m_PeerAcceptedAhead[frame.senderPeerId];
		if (frame.targetFrame >= accepted->second) ahead.insert(frame.targetFrame);
		while (accepted->second != UINT64_MAX) {
			const bool received = ahead.erase(accepted->second) != 0;
			if (!received && !IsSeatReclaimGap(frame.senderPeerId, accepted->second)) break;
			++accepted->second;
		}
		if (InputWireStats* wire = InputWire()) {
			const auto open = wire->gaps.find(frame.senderPeerId);
			if (open != wire->gaps.end() && accepted->second > open->second.frame) {
				if (m_TimingNowMs >= open->second.openedMs + 20)
					DiagnosticLine() << "[net-peer-input-gap] local=" << unsigned(m_Config.localPeerId) << " owner=" << unsigned(frame.senderPeerId) << " missing=" << open->second.frame
					    << " ahead=" << open->second.aheadFrame << " open_ms=" << m_TimingNowMs - open->second.openedMs << " filled_by=" << frame.targetFrame
					    << " through=" << accepted->second << " next=" << m_Stats.nextFrame << " now_ms=" << m_TimingNowMs << std::endl;
				wire->gaps.erase(open);
			}
			if (!ahead.empty() && !wire->gaps.contains(frame.senderPeerId)) wire->gaps[frame.senderPeerId] = {accepted->second, m_TimingNowMs, *ahead.begin()};
		}
		if (ReturnInputDetails()) if (const auto own = m_ReclaimTransactions.find(m_Config.localPeerId);
		    ObservedReceiptFrame(frame.targetFrame) || (own != m_ReclaimTransactions.end() && frame.targetFrame >= own->second.activationFrame &&
		    frame.targetFrame > own->second.neutralThroughFrame && frame.targetFrame - own->second.neutralThroughFrame <= 8))
			DiagnosticLine() << "[net-return-input-receipt] local=" << unsigned(m_Config.localPeerId) << " owner=" << unsigned(frame.senderPeerId)
			    << " target=" << frame.targetFrame << " through=" << accepted->second << " first_ahead=" << (ahead.empty() ? UINT64_MAX : *ahead.begin())
			    << " effective_start=" << EffectiveStartOf(frame.senderPeerId) << " next=" << m_Stats.nextFrame << " now_ms=" << m_TimingNowMs << std::endl;
		auto receipt = PeerFrameMessage(NetHostMigrationMessageType::PeerReceipt);
		receipt.successorPeerId = frame.senderPeerId;
		receipt.frame = accepted->second;
		m_PeerInputReceipts[frame.senderPeerId][m_Config.localPeerId] = accepted->second;
		if (InputWireStats* wire = InputWire()) ++wire->peers[frame.senderPeerId].receiptsSent;
		// Each cumulative receipt replaces a missing one without waiting on the ordered stream.
		SendPeerFrameMessage(std::move(receipt), NetTransportLane::InputUnreliable);
	}

	bool NetLockstepCoordinator::PeerInputAccepted(uint64_t frame, uint8_t owner) const {
		if (owner == 0) owner = m_Config.localPeerId;
		if (frame < EffectiveStartOf(owner) || IsSeatReclaimGap(owner, frame)) return true;
		if (const auto bridge = m_PeerBridges.find(owner); bridge != m_PeerBridges.end() &&
		    frame >= bridge->second.fromFrame && (!bridge->second.returnFrame || frame < *bridge->second.returnFrame))
			return owner != m_Config.localPeerId || m_PeerCommittedTail.contains(frame);
		const uint32_t voters = PeerInputVoters(frame, owner);
		if (PeerGroupHasAuthority(voters, frame)) return true;
		// The bounded hold forms a local group from mutual acceptance. Consuming an unacknowledged input here would leave
		// overlapping human groups on a one-way path and let the foreign hold turn its active owner into a private reader.
		return false;
	}

	uint32_t NetLockstepCoordinator::PeerInputReceiptGroup(uint64_t frame) const {
		const uint32_t members = PeerFrameMembersAt(frame), local = SeatBit(m_Config.localPeerId);
		uint32_t voting = members;
		PeerVotingOwners(voting, frame);
		const auto sources = m_PeerSourceInputs.find(frame);
		const bool neutralParticipant = !IsLocalPrivateReaderAt(frame) && IsSeatReclaimGap(m_Config.localPeerId, frame);
		if (sources == m_PeerSourceInputs.end() ||
		    (!neutralParticipant && ((voting & local) == 0 || !sources->second.contains(m_Config.localPeerId)))) return members;
		// A participating neutral returner owes no own sample, but still observes every supplied sender's acceptance.
		// Its absent own receipt must neither remove the continuing owners nor skip their missing reciprocal receipts.
		uint32_t group = neutralParticipant || PeerInputAccepted(frame, m_Config.localPeerId) ? voting : voting & PeerInputVoters(frame, m_Config.localPeerId);
		// Keep the supplied sender whose acceptance blocks this frame, and the readers that accepted it. An observer outside
		// that group cannot veto its local hold by acknowledging a different input. The stable seat order resolves overlapping
		// receipt sets locally; it selects no winning history. The heal or directory still arbitrates any continuations.
		for (uint8_t owner = 1; owner <= m_Config.peerCount; ++owner) {
			if ((voting & SeatBit(owner)) == 0 || frame < EffectiveStartOf(owner)) continue;
			if (!sources->second.contains(owner)) {
				// A witness that already accepted this missing input cannot share the local hold that replaces it.
				group &= ~(PeerInputVoters(frame, owner) & ~SeatBit(owner) & ~local);
				continue;
			}
			if ((group & SeatBit(owner)) == 0 || PeerInputAccepted(frame, owner)) continue;
			// Possession of this authenticated source is our own receipt even while its cumulative publication catches up.
			group &= PeerInputVoters(frame, owner) | local;
		}
		// Neutral returning seats do not owe input receipts before their certified gap ends.
		return group | (members & ~voting);
	}

	uint32_t NetLockstepCoordinator::PeerInputVoters(uint64_t frame, uint8_t owner) const {
		const uint32_t retained = PeerRetainedSeatsAt(frame);
		if ((retained & SeatBit(owner)) == 0) return 0;
		uint32_t accepted = SeatBit(owner);
		if (const auto receipts = m_PeerInputReceipts.find(owner); receipts != m_PeerInputReceipts.end())
			for (const auto& [peer, through]: receipts->second)
				if ((retained & SeatBit(peer)) != 0 && through > frame) accepted |= SeatBit(peer);
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
				if (candidate == 0) continue;
				if (PeerGroupHasAuthority(PeerInputVoters(candidate - 1, owner), candidate - 1)) through = std::max(through, candidate);
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
			const bool measured = wire && wire->IsPeerPingMeasured(route);
			if (measured) rtt = wire->GetPeerPingMs(route);
			uint64_t retryMs = std::clamp<uint64_t>(static_cast<uint64_t>(std::ceil(m_Config.slowPlayerBoundTicks * m_Config.simTickMs)), 50, 1000);
			if (measured) {
				// Repair replaces a receipt after its link trip, before the buffered target is due. Adding the gameplay hold
				// budget here starts recovery after that runway has expired. Link p95 covers in-flight transport jitter.
				retryMs = std::max<uint64_t>(static_cast<uint64_t>(std::max(1.0, std::ceil(m_Config.simTickMs))), rtt);
				if (const auto link = m_DelayEstimators.find(peer); link != m_DelayEstimators.end() && link->second.HasMeasurement())
					retryMs = std::max<uint64_t>(retryMs, link->second.P95Ms());
			}
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
		m_PeerOwnershipChanges[peer][frame] = false;
		std::set<PeerDecisionKey> retired;
		const auto superseded = [&](const NetHostMigrationMessage& proposal) {
			return proposal.preparedFrame == 1 && proposal.frame >= frame && ReturnCrossesDeparture(proposal, m_PeerOwnershipChanges);
		};
		// A removal before a certified return supersedes the entire named cohort.
		// Retire it on early receivers just as installation rejects it on late ones.
		for (auto cert = m_PeerBridgeCertificates.lower_bound({frame, 0}); cert != m_PeerBridgeCertificates.end(); ++cert) {
			if (cert->first.second != 1) continue;
			NetHostMigrationMessage agreed; uint32_t voters = 0;
			if (!DecodePeerBridgeCertificate(cert->second, agreed, voters) || !superseded(agreed)) continue;
			retired.insert(cert->first);
			for (uint8_t named: agreed.members) {
				if (auto bridge = m_PeerBridges.find(named); bridge != m_PeerBridges.end() && bridge->second.returnFrame == agreed.frame) bridge->second.returnFrame.reset();
				if (auto back = m_ReclaimTransactions.find(named); back != m_ReclaimTransactions.end() && back->second.activationFrame == agreed.frame) {
					m_PeerEffectiveStart.erase(named);
					if (auto admission = m_PeerAdmissions.find(named); admission != m_PeerAdmissions.end() && admission->second.frame == agreed.frame) m_PeerAdmissions.erase(admission);
					if (auto terms = m_Config.initialPeerStartTerms.find(named); terms != m_Config.initialPeerStartTerms.end() && terms->second.first == agreed.frame) m_Config.initialPeerStartTerms.erase(terms);
					m_ReclaimTransactions.erase(back);
				}
				if (auto holds = m_PeerFrameHolds.find(named); holds != m_PeerFrameHolds.end()) {
					const auto back = holds->second.find(agreed.frame);
					if (back != holds->second.end() && !back->second) holds->second.erase(back);
				}
				if (auto transitions = m_SeatTransitions.find(named); transitions != m_SeatTransitions.end()) {
					const auto back = transitions->second.find(agreed.frame);
					if (back != transitions->second.end() && back->second == SeatTransition::Back) transitions->second.erase(back);
				}
				// An independently certified delay at the same frame still stands.
				NetHostMigrationMessage delay; uint32_t delayVoters = 0;
				const auto delayCert = m_PeerBridgeCertificates.find({agreed.frame, static_cast<uint8_t>(8 + named)});
				if (delayCert == m_PeerBridgeCertificates.end() || !DecodePeerBridgeCertificate(delayCert->second, delay, delayVoters) ||
				    std::find(delay.members.begin(), delay.members.end(), named) == delay.members.end()) m_DelayChanges[named].erase(agreed.frame);
			}
		}
		for (const auto& [key, votes]: m_PeerBridgeVotes) if (key.second == 1 && key.first >= frame)
			for (const auto& [voter, proposal]: votes) if (superseded(proposal)) retired.insert(key);
		for (const auto& [key, pending]: m_PendingPeerDecisions) if (key.second == 1 && key.first >= frame) {
			NetHostMigrationMessage proposal; uint32_t voters = 0;
			if (DecodePeerBridgeCertificate(pending.first, proposal, voters) && superseded(proposal)) retired.insert(key);
		}
		for (const auto& [activation, proof]: m_PeerReturnProofs) if (activation >= frame) {
			NetHostMigrationMessage proposal; uint32_t voters = 0;
			if (DecodePeerBridgeCertificate(proof, proposal, voters) && superseded(proposal)) retired.emplace(activation, 1);
		}
		for (const auto& key: retired) {
			m_PeerBridgeCertificates.erase(key); m_PeerBridgeVotes.erase(key); m_PendingPeerDecisions.erase(key);
			m_AuthorityWaitSinceMs.erase(key); m_PeerReturnProofs.erase(key.first);
		}
		m_PeerOwnReceiptProgress.erase(peer); m_PeerReceiptDelaySamples.erase(peer); m_PeerInputReliableThrough.erase(peer);
		m_PeerTailSendRanges.erase(peer); m_PeerTailOutgoing.erase(peer);
		m_PeerTailLastSentMs.erase(peer); m_PeerTailResendFrames.erase(peer);
		// Keep the old owner's real receipts for earlier frames, without authorizing a new incarnation's inputs.
		for (auto& [owner, receipts]: m_PeerInputReceipts) if (auto receipt = receipts.find(peer); receipt != receipts.end()) receipt->second = std::min(receipt->second, frame);
		const uint32_t members = PeerFrameMembersAt(frame);
		m_FrameGroupChanges.try_emplace(frame, members);
		for (auto change = m_FrameGroupChanges.lower_bound(frame); change != m_FrameGroupChanges.end(); ++change) change->second &= ~SeatBit(peer);
		if (frame <= m_Stats.nextFrame) m_FrameGroupMembers &= ~SeatBit(peer);
		m_PeerAdminFrameMembers &= ~SeatBit(peer);
	}

	bool NetLockstepCoordinator::HandlePeerFrameMessage(const NetHostMigrationMessage& message, uint64_t nowMs, const NetHash32* readKey) {
		if (HandlePeerSessionMessage(message, nowMs, readKey)) return true;
		if (!UsesPeerFrameGroups() || message.type < NetHostMigrationMessageType::PeerInput ||
		    (message.type > NetHostMigrationMessageType::PeerControl && message.type != NetHostMigrationMessageType::PeerForwardInput && message.type != NetHostMigrationMessageType::PeerCaptureRequest)) return false;
		const uint8_t detailOwner = message.type == NetHostMigrationMessageType::PeerForwardInput ? message.successorPeerId : message.senderPeerId;
		const auto detailBack = m_ReclaimTransactions.find(detailOwner);
		if (ReturnInputDetails() && (message.type == NetHostMigrationMessageType::PeerInput || message.type == NetHostMigrationMessageType::PeerForwardInput) &&
		    detailBack != m_ReclaimTransactions.end() && message.frame > detailBack->second.neutralThroughFrame && message.frame <= detailBack->second.neutralThroughFrame + 3) {
			DiagnosticLine() << "[net-return-input-read] local=" << static_cast<int>(m_Config.localPeerId) << " owner=" << static_cast<int>(detailOwner)
			    << " frame=" << message.frame << " prepared=" << m_Stats.nextFrame << " session_matches=" << (message.sessionId == m_Config.sessionId)
			    << " round_matches=" << (message.roundId == m_Config.matchConfig.roundId) << " setup_matches=" << (message.configHash == m_RoundConfigHash)
			    << " removed=" << m_RemovedPeers.contains(message.senderPeerId) << " now_ms=" << nowMs << std::endl;
		}
		if (ReturnInputDetails() && message.type == NetHostMigrationMessageType::PeerTail && message.frame == m_Stats.nextFrame &&
		    (m_FrameGroupMembers & SeatBit(m_Config.localPeerId)) == 0 && m_TailArrivalNamed != message.frame) {
			m_TailArrivalNamed = message.frame;
			DiagnosticLine() << "[net-peer-tail-arrival] local=" << static_cast<int>(m_Config.localPeerId) << " donor=" << static_cast<int>(message.senderPeerId)
			    << " frame=" << message.frame << " offset=" << message.offset << " total=" << message.totalBytes << " complete_from=" << message.completeFrom
			    << " voters=" << message.voterMask << " applied=" << message.appliedFrame << " generation=" << message.generation << " session_matches=" << (message.sessionId == m_Config.sessionId)
			    << " round_matches=" << (message.roundId == m_Config.matchConfig.roundId) << " setup_matches=" << (message.configHash == m_RoundConfigHash)
			    << " blackout=" << PeerFrameBlackout(nowMs) << " now_ms=" << nowMs << std::endl;
		}
		if (PeerFrameBlackout(nowMs)) return true;
		if (message.sessionId != m_Config.sessionId || message.roundId != m_Config.matchConfig.roundId || message.configHash != m_RoundConfigHash ||
		    message.senderPeerId == 0 || message.senderPeerId == m_Config.localPeerId || message.senderPeerId > m_Config.peerCount || message.generation != 1) return true;
		if (m_RemovedPeers.contains(message.senderPeerId)) {
			const bool historicalData = message.type == NetHostMigrationMessageType::PeerInput || message.type == NetHostMigrationMessageType::PeerForwardInput ||
			    message.type == NetHostMigrationMessageType::PeerReceipt || message.type == NetHostMigrationMessageType::PeerTail ||
			    message.type == NetHostMigrationMessageType::PeerBridge || message.type == NetHostMigrationMessageType::PeerBridgeCommit;
			const uint64_t at = message.type == NetHostMigrationMessageType::PeerReceipt && message.preparedFrame != UINT64_MAX && message.frame > 0 ? message.frame - 1 : message.frame;
			if (!historicalData || at < m_Config.startFrame || (PeerRetainedSeatsAt(at) & SeatBit(message.senderPeerId)) == 0) return true;
		}
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
			if (message.successorPeerId == message.senderPeerId && message.preparedFrame == UINT64_MAX && message.boundary == c_NetPeerSplitDecision &&
			    (message.completeFrom == 1 || message.completeFrom == 2) && message.frame > 0) {
				// A decision carries no progress of its sender's: the seats that follow it take it at their next poll.
				uint32_t winners = 0;
				for (uint8_t seat: message.members) winners |= SeatBit(seat);
				m_PeerSplitDecisions[message.frame].insert_or_assign(message.senderPeerId, PeerSplitDecision{static_cast<uint8_t>(message.completeFrom), winners, message.connectedMask});
				return true;
			}
			if (message.successorPeerId == message.senderPeerId && message.preparedFrame == UINT64_MAX && message.boundary == c_NetPeerRecordRequest) {
				heard();
				AnswerPeerRecordRequest(message);
				return true;
			}
			if (message.successorPeerId == message.senderPeerId && message.preparedFrame == UINT64_MAX && message.boundary == c_NetPeerRecordAnswer &&
			    message.bytes.size() % (8 + NetHash32{}.size()) == 0 && message.bytes.size() / (8 + NetHash32{}.size()) <= c_NetPeerRecordBatch) {
				heard();
				// Frames before the member's first held record are not its records: it played them before its image.
				auto& floor = m_PeerRecordFloors[message.senderPeerId];
				floor = std::max(floor, std::min(message.completeFrom, message.appliedFrame + 1));
				size_t offset = 0;
				for (uint64_t frame = 0; TakeFrame(message.bytes, offset, frame);) {
					NetHash32 prefix{};
					std::copy_n(message.bytes.begin() + offset, prefix.size(), prefix.begin());
					offset += prefix.size();
					NotePeerRecord(message.senderPeerId, frame, prefix);
				}
				if (!m_DeferredChecksumFrames.empty()) RecheckDeferredChecksums();
				return true;
			}
			if (message.successorPeerId == message.senderPeerId && message.preparedFrame == UINT64_MAX && message.boundary == 0 && message.bytes.size() == 2 * NetHash32{}.size() && message.appliedFrame == message.frame &&
			    message.frame >= m_Config.startFrame && message.frame <= m_Stats.nextFrame + NetLockstepCodec::c_MaxFutureFrameSkew &&
			    (message.voterMask & SeatBit(message.senderPeerId)) != 0 && (message.voterMask >> m_Config.peerCount) == 0) {
				heard();
				m_PeerFrameWitnesses[message.frame].try_emplace(message.senderPeerId, message);
				NetHash32 prefix{};
				std::copy_n(message.bytes.begin(), prefix.size(), prefix.begin());
				if (!m_PeerAppliedPrefixes.contains(message.senderPeerId) || m_PeerAppliedPrefixes.at(message.senderPeerId).first < message.frame)
					m_PeerAppliedPrefixes[message.senderPeerId] = {message.frame, prefix};
				NotePeerRecord(message.senderPeerId, message.frame, prefix);
				NotePeerContinuation(message.senderPeerId, message.frame, prefix, message.voterMask);
				if (!m_DeferredChecksumFrames.empty()) RecheckDeferredChecksums();
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
					const uint32_t members = PeerFrameMembersAt(message.frame - 1);
					const auto bridge = m_PeerBridges.find(message.senderPeerId);
					const bool live = bridge == m_PeerBridges.end() || (bridge->second.returnFrame && message.appliedFrame >= *bridge->second.returnFrame);
					// A private tail reader's delayed receipt measures its catch-up,
					// not the live sender's transport round trip.
					if (InputWireStats* wire = InputWire()) {
						auto& peer = wire->peers[message.senderPeerId];
						++peer.receiptsRead;
						peer.maxReceiptMs = std::max<uint64_t>(peer.maxReceiptMs, nowMs - sent->second);
					}
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
				if (ReturnInputDetails() && ObservedReceiptFrame(message.frame))
					DiagnosticLine() << "[net-input-receipt-read] local=" << unsigned(m_Config.localPeerId) << " owner=" << unsigned(message.successorPeerId)
					    << " receiver=" << unsigned(message.senderPeerId) << " through=" << message.frame << " retained=" << receipt
					    << " next=" << m_Stats.nextFrame << " now_ms=" << nowMs << std::endl;
				ResolvePeerBridgeInputConflicts(nowMs);
			}
			return true;
		}
		if (message.type == NetHostMigrationMessageType::PeerCaptureRequest) {
			if (!ValidatePeerCaptureRequest(message, m_Stats.nextFrame)) return true;
			if (const auto completed = m_PeerCapturesCompleted.find(message.boundary); completed != m_PeerCapturesCompleted.end() && completed->second >= message.completeFrom) return true;
			if (!m_PeerCaptureRequest || message.boundary > m_PeerCaptureRequest->boundary ||
			    (message.boundary == m_PeerCaptureRequest->boundary && message.completeFrom > m_PeerCaptureRequest->completeFrom)) m_PeerCaptureRequest = message;
			heard();
			return true;
		}
		if (message.type == NetHostMigrationMessageType::PeerBridge) {
			const bool invitation = message.voterMask == 0;
			if (invitation) {
				// An authenticated invitation is delivered only to its named owner. It contributes no vote on this or any other
				// peer, and cannot stop an input. The owner's ordinary co-sign checks decide whether it can promise the cutoff.
				if (message.preparedFrame != 0 || message.members.size() != 1 || message.members.front() != m_Config.localPeerId ||
				    (message.connectedMask & SeatBit(message.senderPeerId)) == 0 || (message.connectedMask & SeatBit(m_Config.localPeerId)) != 0) return true;
			} else if (message.voterMask != SeatBit(message.senderPeerId)) return true;
			// A hold made now is voted at its frame; one agreed ahead is voted as it arrives.
			if (message.preparedFrame == 0 && message.boundary >= message.frame && message.frame > m_Stats.nextFrame && message.frame - m_Stats.nextFrame <= NetLockstepCodec::c_MaxFutureFrameSkew) {
				m_PeerBridgeVotes[{message.frame, DecisionKind(message)}].try_emplace(message.senderPeerId, message);
				return true;
			}
			if (!ValidatePeerBridge(message) && !CoSignsOwnHold(message)) {
				if (ReturnInputDetails()) DiagnosticLine() << "[net-peer-vote-refused] local=" << static_cast<int>(m_Config.localPeerId)
				    << " sender=" << static_cast<int>(message.senderPeerId) << " frame=" << message.frame << " boundary=" << message.boundary
				    << " kind=" << static_cast<int>(DecisionKind(message)) << " next=" << m_Stats.nextFrame << " start=" << m_Config.startFrame
				    << " applied_boundary=" << m_MigrationHistory.contains(message.boundary) << " replay_boundary=" << m_PeerReplayAppliedPrefixes.contains(message.boundary)
				    << " now_ms=" << nowMs << std::endl;
				return true;
			}
			heard();
			auto& votes = m_PeerBridgeVotes[{message.frame, DecisionKind(message)}];
			const auto own = votes.find(m_Config.localPeerId);
			if (own != votes.end() && own->second.voterMask != 0 && !SameBridge(own->second, message)) return true;
			const auto previous = votes.find(message.senderPeerId);
			if (previous == votes.end() || (previous->second.voterMask == 0 && !invitation)) votes.insert_or_assign(message.senderPeerId, message);
			if ((own == votes.end() || own->second.voterMask == 0) && !VotedOtherPeerReturn(message) && !HoldEmptiesTheRound(message, true)) {
				auto reply = message;
				reply.senderPeerId = m_Config.localPeerId;
				reply.appliedFrame = m_LastCompletedSimulationTick.value_or(0);
				reply.voterMask = SeatBit(m_Config.localPeerId);
				if (reply.preparedFrame == 3) m_PeerAdminOwnVote = reply;
				votes.insert_or_assign(m_Config.localPeerId, reply);
				if (reply.preparedFrame == 0) for (uint8_t peer: reply.members) {
					m_PeerRejectedInputs[reply.frame] |= SeatBit(peer);
					const char* cause = peer == m_Config.localPeerId ? "own_hold_consent" : "peer_hold_proposal";
					const uint64_t heardAt = LastAuthenticatedTraffic(peer);
					m_HoldFacts[peer] = {reply.frame, nowMs >= heardAt ? nowMs - heardAt : 0, m_Config.slowPlayerBoundTicks * m_Config.simTickMs, cause};
					DiagnosticLine() << "[net-peer-hold-trigger] local=" << static_cast<int>(m_Config.localPeerId) << " peer=" << static_cast<int>(peer)
					    << " frame=" << reply.frame << " ai_frame=" << reply.frame
					    << " now_ms=" << nowMs << " cause=" << cause << " requested_by=" << static_cast<int>(message.senderPeerId)
					    << " last_heard_ms=" << heardAt << " last_progress_ms=" << m_Stats.peers[peer].lastProgressMs
					    << " received_through=" << m_Stats.peers[peer].highestTargetFrame << " delay_ticks=" << InputDelayAt(peer, reply.frame)
					    << " voters=" << reply.voterMask << std::endl;
				}
				SendPeerFrameMessage(std::move(reply));
			}
			TryCommitPeerBridge(message.frame, nowMs, DecisionKind(message));
			return true;
		}
		if (message.type == NetHostMigrationMessageType::PeerBridgeCommit) {
			NetHostMigrationMessage agreed; uint32_t voters = 0;
			if (message.preparedFrame == 1 && DecodePeerBridgeCertificate(message, agreed, voters)) {
				if (ReturnCrossesDeparture(agreed, m_PeerOwnershipChanges)) return true;
				for (uint8_t peer: agreed.members) if ((PeerRetainedSeatsAt(agreed.frame) & SeatBit(peer)) == 0) return true;
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
				if (ReturnInputDetails()) {
					const auto queued = m_PeerTailOutgoing.find(message.senderPeerId);
					DiagnosticLine() << "[net-peer-tail-serve] donor=" << static_cast<int>(m_Config.localPeerId) << " reader=" << static_cast<int>(message.senderPeerId)
					    << " from=" << message.frame << " until=" << message.completeFrom << " displayed=" << m_LastCompletedSimulationTick.value_or(0)
					    << " reader_applied=" << (m_PeerAppliedThrough.contains(message.senderPeerId) ? m_PeerAppliedThrough.at(message.senderPeerId) : 0)
					    << " kept_from=" << m_MigrationHistory.contains(message.frame) << " kept_first=" << (m_MigrationHistory.empty() ? 0 : m_MigrationHistory.begin()->first)
					    << " queued=" << (queued == m_PeerTailOutgoing.end() ? 0 : queued->second.size()) << " sent=" << sent.contains(message.frame)
					    << " repeat=" << repeats.contains(message.frame) << " deferred=" << m_PeerTailDeferredSends << " now_ms=" << nowMs << std::endl;
				}
			}
			return true;
		}
		if (message.type == NetHostMigrationMessageType::PeerTailMissing) {
			const bool longHitch = m_ConsumerWaitingFrame && nowMs >= m_ConsumerWaitStartMs && nowMs - m_ConsumerWaitStartMs >= c_NetSeatDisconnectSilenceMs;
			const bool authority = PeerGroupHasAuthority(message.connectedMask, m_Stats.nextFrame, PeerVotingScope::History);
			// History older than the donor keeps cannot carry the split's certificate, so the split's name asks the referee instead.
			if (message.frame == m_Stats.nextFrame && (message.connectedMask & SeatBit(m_Config.localPeerId)) == 0 && PeerGroupNeedsArbitration(message.connectedMask) &&
			    !authority) CheckPeerFrameTie(message.frame, message.connectedMask, nowMs);
			if (message.frame == m_Stats.nextFrame && authority && longHitch) {
				m_LocalSeatHeld = true; m_LocalHoldFrame = m_Stats.nextFrame;
				m_State = NetLockstepState::Stopped; m_Stats.timeoutReason = "PeerHeld:the committed tail is unavailable; returning through a private image";
			}
			return true;
		}
		if (message.type == NetHostMigrationMessageType::PeerHeartbeat) {
			if ((message.bytes.size() != 8 && message.bytes.size() != 8 + NetHash32{}.size()) || (message.connectedMask & ~((1U << m_Config.peerCount) - 1)) != 0) return true;
			size_t offset = 0; uint32_t rtt = 0, jitter = 0;
			if (!TakeSize(message.bytes, offset, rtt) || !TakeSize(message.bytes, offset, jitter) || rtt > 60000 || jitter > 60000) return true;
			heard();
			if (message.bytes.size() == 8 + NetHash32{}.size() &&
			    (!m_PeerAppliedPrefixes.contains(message.senderPeerId) || m_PeerAppliedPrefixes.at(message.senderPeerId).first <= message.appliedFrame)) {
				NetHash32 prefix{};
				std::copy_n(message.bytes.begin() + 8, prefix.size(), prefix.begin());
				m_PeerAppliedPrefixes[message.senderPeerId] = {message.appliedFrame, prefix};
				NotePeerContinuation(message.senderPeerId, message.appliedFrame, prefix, message.connectedMask);
			}
			m_Stats.peers[message.senderPeerId].pingMs = rtt; m_Stats.peers[message.senderPeerId].jitterMs = jitter;
			m_PeerAdminReady[message.senderPeerId] = {message.completeFrom, nowMs};
			m_PeerReportedMembers[message.senderPeerId] = message.connectedMask;
			m_Stats.peers[message.senderPeerId].pingMeasured = rtt != 0;
			CheckDirectPeerHistory(message, nowMs);
			if (m_State == NetLockstepState::Stopped) return true;
			// Only this authenticated heartbeat names a prepared horizon. Bridge
			// kinds and receipt witnesses reuse that field for other purposes.
			if (message.preparedFrame != UINT64_MAX && (message.preparedFrame <= m_Stats.nextFrame ||
			    message.preparedFrame - m_Stats.nextFrame <= NetLockstepCodec::c_MaxFutureFrameSkew))
				m_Stats.peers[message.senderPeerId].reportedNextFrame = std::max(m_Stats.peers[message.senderPeerId].reportedNextFrame, message.preparedFrame + 1);
			// The other half of a refereed split holds history only on the referee's word: ask it what it decided for this split.
			const bool otherHalf = (message.connectedMask & SeatBit(m_Config.localPeerId)) == 0 && PeerGroupNeedsArbitration(message.connectedMask);
			const bool authority = PeerGroupHasAuthority(message.connectedMask, m_Stats.nextFrame, PeerVotingScope::History);
			if (otherHalf && !authority) CheckPeerFrameTie(m_Stats.nextFrame, message.connectedMask, nowMs);
			if ((otherHalf || authority) && message.appliedFrame >= m_Stats.nextFrame)
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
				// A member can return to another continuation before this peer has adopted it. That start belongs to its
				// named group, not this running peer's handshake. Waiting image returners still take their own member starts.
				if (IsRunning() && std::holds_alternative<NetLockstepStart>(decoded.packet.payload) &&
				    (message.connectedMask & SeatBit(message.senderPeerId)) != 0 && (message.connectedMask & SeatBit(m_Config.localPeerId)) == 0) return true;
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
			if (inputMessage || ReturnInputDetails()) DiagnosticLine() << "[net-peer-input-room] local=" << static_cast<int>(m_Config.localPeerId)
			    << " sender=" << static_cast<int>(message.senderPeerId) << " owner=" << static_cast<int>(message.successorPeerId) << " type=" << static_cast<int>(message.type)
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
				if (ReturnInputDetails()) DiagnosticLine() << "[net-peer-tail-read] local=" << static_cast<int>(m_Config.localPeerId)
				    << " donor=" << static_cast<int>(message.senderPeerId) << " frame=" << message.frame << " prepared=" << m_Stats.nextFrame
				    << " reason=" << reason << " now_ms=" << nowMs << std::endl;
				return true;
			};
			if (message.completeFrom > 1) return readFailed("compression");
			if (message.completeFrom == 1) {
				size_t offset = 0; uint32_t expandedSize = 0;
				if (!TakeSize(body, offset, expandedSize) || expandedSize < 70 || expandedSize > NetHostMigrationCodec::c_MaxPeerTailBytes) return readFailed("malformed-2");
				std::vector<uint8_t> expanded(expandedSize);
				if (LZ4_decompress_safe(reinterpret_cast<const char*>(body.data() + offset), reinterpret_cast<char*>(expanded.data()),
				    static_cast<int>(body.size() - offset), static_cast<int>(expanded.size())) != static_cast<int>(expanded.size())) return readFailed("malformed-3");
				body = std::move(expanded);
			}
			if ((message.voterMask & SeatBit(message.senderPeerId)) == 0 || message.appliedFrame < message.frame || body.size() < 70) return readFailed("sender-group");
			NetHash32 prefix{}, prepared{}; std::copy_n(body.begin(), prefix.size(), prefix.begin());
			std::copy_n(body.begin() + prefix.size(), prepared.size(), prepared.begin());
			size_t at = prefix.size() + prepared.size(); uint32_t size = 0;
			if (!TakeSize(body, at, size) || size > NetHostMigrationCodec::c_MaxFrameBytes || at > body.size() || size > body.size() - at) return readFailed("malformed-4");
			std::vector<uint8_t> record(body.begin() + at, body.begin() + at + size); at += size;
			NetLockstepReadyFrame decoded;
			if (!DecodeMigrationFrame(record, message.frame, decoded) || at == body.size()) return readFailed("frame-record");
			const uint8_t count = body[at++]; if (count > 32) return readFailed("malformed-5");
			std::vector<NetHostMigrationMessage> proofs;
			// A certified return of this seat carried here: its activation and the end of its neutral gap.
			std::optional<std::pair<uint64_t, uint64_t>> ownReturnGap;
			for (uint8_t n = 0; n < count; ++n) {
				if (!TakeSize(body, at, size) || at > body.size() || size > body.size() - at) return readFailed("malformed-6");
				NetHostMigrationMessage proof, agreed; uint32_t voters = 0;
				if (!NetHostMigrationCodec::Decode({body.begin() + at, body.begin() + at + size}, m_Config.migrationKey, proof) ||
				    proof.type != NetHostMigrationMessageType::PeerBridgeCommit || !DecodePeerBridgeCertificate(proof, agreed, voters) ||
				    (proof.frame > message.frame && agreed.preparedFrame != 3)) return readFailed("bridge-record");
				if (agreed.preparedFrame == 3 && agreed.completeFrom > m_Config.migrationGeneration) {
					NoteSuperseded(agreed.completeFrom);
					if (PeerAdminCertificateValid(agreed, voters) && 2 * std::popcount(voters) >= static_cast<int>(m_Config.peerCount)) m_PeerAdminFrameMembers = voters;
				} else if (agreed.connectedMask == message.voterMask) {
					// A local hold can be signed before its other survivors answer. Its signers are evidence for the hold,
					// not a different playing half: only the certificate's actual continuing group can name that split.
					const uint32_t playing = agreed.preparedFrame == 0 ? agreed.connectedMask : voters;
					if (!PeerGroupHasAuthority(playing, agreed.frame, PeerVotingScope::History)) CheckPeerFrameTie(agreed.frame, playing, nowMs);
				}
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
			if (at == body.size()) return readFailed("no-witnesses");
			const uint8_t witnessCount = body[at++];
			if (witnessCount > m_Config.peerCount) return readFailed("malformed-7");
			const NetHash32 digest = CommittedFramePrefix(decoded);
			uint32_t witnessed = 0;
			std::vector<NetHostMigrationMessage> witnesses;
			for (uint8_t n = 0; n < witnessCount; ++n) {
				if (!TakeSize(body, at, size) || at > body.size() || size > body.size() - at) return readFailed("malformed-8");
				NetHostMigrationMessage witness;
				if (!NetHostMigrationCodec::Decode({body.begin() + at, body.begin() + at + size}, m_Config.migrationKey, witness) ||
				    witness.type != NetHostMigrationMessageType::PeerReceipt || witness.successorPeerId != witness.senderPeerId || witness.preparedFrame != UINT64_MAX || witness.sessionId != m_Config.sessionId ||
				    witness.roundId != m_Config.matchConfig.roundId || witness.configHash != m_RoundConfigHash || witness.generation != 1 ||
				    witness.frame != message.frame || witness.appliedFrame != message.frame || witness.voterMask != message.voterMask ||
				    witness.senderPeerId == 0 || witness.senderPeerId > m_Config.peerCount || (witnessed & SeatBit(witness.senderPeerId)) != 0 ||
				    (message.voterMask & SeatBit(witness.senderPeerId)) == 0 || witness.bytes.size() != 64 ||
				    !std::equal(digest.begin(), digest.end(), witness.bytes.begin()) || !std::equal(prepared.begin(), prepared.end(), witness.bytes.begin() + 32)) return readFailed("witness-record");
				witnessed |= SeatBit(witness.senderPeerId); witnesses.push_back(std::move(witness)); at += size;
			}
			if (at != body.size()) return readFailed("malformed-9");
			heard();
			if (!PeerGroupHasAuthority(message.voterMask, message.frame, PeerVotingScope::History)) {
				// A group this history cannot certify at its next frame plays another continuation: a record this reader cannot take.
				if (message.frame == m_Stats.nextFrame) m_PeerTailForeignFrame = message.frame;
				return readFailed("group-authority");
			}
			// Retain complete validated evidence so this reader can certify the same history when it relays it.
			for (auto& witness: witnesses) m_PeerFrameWitnesses[message.frame].try_emplace(witness.senderPeerId, std::move(witness));
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
		if (owner == 0 || owner > m_Config.peerCount) return true;
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
			if ((PeerRetainedSeatsAt(input.targetFrame) & SeatBit(input.senderPeerId)) == 0) continue;
			if (input.targetFrame < m_Stats.nextFrame || input.targetFrame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew) continue;
			if ((m_PeerRejectedInputs[input.targetFrame] & SeatBit(input.senderPeerId)) != 0) {
				if (!HasHoldDecision(m_PeerBridgeCertificates, input.targetFrame, input.senderPeerId)) {
					const auto pending = m_PeerPendingBridgeInputs[input.targetFrame].try_emplace(input.senderPeerId, input);
					if (pending.second && ReturnInputDetails()) DiagnosticLine() << "[net-peer-input-at-vote] local=" << static_cast<int>(m_Config.localPeerId)
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
