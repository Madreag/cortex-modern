#include "NetPeerFrameHelpers.h"
#include "SteadyClock.h"

#include "DiagnosticLine.h"
#include "NetAuthCrypto.h"
#include "NetLobbyProtocol.h"
#include "NetProtocol.h"
#include "NetWorldJoin.h"

#include "lz4.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace RTE {

	using namespace NetPeerFrameDetail;

	NetHash32 NetLockstepCoordinator::PeerFramePrefix(uint64_t frame) const {
		if (const auto prepared = m_PeerPreparedPrefixes.find(frame); prepared != m_PeerPreparedPrefixes.end()) return prepared->second;
		return PeerAppliedFramePrefix(frame);
	}

	NetHash32 NetLockstepCoordinator::PeerAppliedFramePrefix(uint64_t frame) const {
		if (m_Playback) if (const auto applied = m_PeerReplayAppliedPrefixes.find(frame); applied != m_PeerReplayAppliedPrefixes.end()) return applied->second;
		const auto kept = m_MigrationHistory.find(frame);
		if (kept == m_MigrationHistory.end()) return {};
		NetLockstepReadyFrame ready;
		if (DecodeMigrationFrame(kept->second, frame, ready)) return CommittedFramePrefix(ready);
		NetHash32 hash{};
		uint8_t mac[32]{};
		if (GetNetAuthCrypto().HmacSha256(m_Config.migrationKey.data(), m_Config.migrationKey.size(), kept->second.data(), kept->second.size(), mac))
			std::copy_n(mac, hash.size(), hash.begin());
		return hash;
	}

	NetHash32 NetLockstepCoordinator::CommittedFramePrefix(const NetLockstepReadyFrame& ready) const {
		// The frame as the round's committed record carries it, the same bytes on every peer: a seat that only replayed that
		// record, with no peer's own split of the inputs, derives the same prefix as the seats that played it.
		std::vector<uint8_t> bytes{ready.authorityPeerId, ready.updateAuthorityPeerId, static_cast<uint8_t>(ready.departedPeerIds.size())};
		bytes.insert(bytes.end(), ready.departedPeerIds.begin(), ready.departedPeerIds.end());
		for (const auto* facts: {&ready.committedPeerLeaves, &ready.committedFrameWaivers}) {
			bytes.push_back(static_cast<uint8_t>(facts->size()));
			for (const auto& [peer, boundary]: *facts) {
				bytes.push_back(peer);
				for (int shift = 0; shift < 64; shift += 8) bytes.push_back(static_cast<uint8_t>(boundary >> shift));
			}
		}
		std::vector<uint8_t> committed;
		NetHash32 hash{};
		uint8_t mac[32]{};
		// A frame the record cannot carry keeps its own encoding, which no other frame's prefix can match.
		if (!EncodeCommittedJoinFrame(PackWorldJoinReadyFrame(ready), committed) && !EncodeMigrationFrame(ready, committed)) return hash;
		bytes.insert(bytes.end(), committed.begin(), committed.end());
		if (GetNetAuthCrypto().HmacSha256(m_Config.migrationKey.data(), m_Config.migrationKey.size(), bytes.data(), bytes.size(), mac))
			std::copy_n(mac, hash.size(), hash.begin());
		return hash;
	}

	void NetLockstepCoordinator::SendPeerFrameWitness(uint64_t frame) {
		if (!m_MigrationHistory.contains(frame)) return;
		auto witness = PeerFrameMessage(NetHostMigrationMessageType::PeerReceipt);
		witness.preparedFrame = UINT64_MAX; witness.frame = witness.appliedFrame = frame;
		witness.voterMask = PeerFrameMembersAt(frame);
		if ((witness.voterMask & SeatBit(m_Config.localPeerId)) == 0) {
			// A private reader cannot witness this group's frame. Advertise its
			// newly applied prefix on the next pump so return eligibility can
			// follow its catch-up instead of a stale periodic heartbeat.
			m_PeerHeartbeatAtMs = std::min(m_PeerHeartbeatAtMs, m_TimingNowMs + 25);
			return;
		}
		const auto applied = PeerAppliedFramePrefix(frame), prepared = PeerFramePrefix(frame);
		witness.bytes.assign(applied.begin(), applied.end()); witness.bytes.insert(witness.bytes.end(), prepared.begin(), prepared.end());
		m_PeerFrameWitnesses[frame][m_Config.localPeerId] = witness;
		SendPeerFrameMessage(std::move(witness), m_Config.frameLane);
	}

	void NetLockstepCoordinator::SendPeerCommittedTail(uint8_t peer, uint64_t fromFrame) {
		if (fromFrame == UINT64_MAX) return;
		// Standalone old-input requests retain a bounded one-frame reply too.
		auto range = m_PeerTailSendRanges.try_emplace(peer, fromFrame, fromFrame + 1).first;
		range->second.first = std::min(range->second.first, fromFrame);
		range->second.second = std::max(range->second.second, fromFrame + 1);
		// Resume the earliest pending record when a later request extends the range.
		const uint64_t firstNeeded = range->second.first;
		// A future subscription waits until this donor has displayed its first frame.
		if (!m_LastCompletedSimulationTick || firstNeeded > *m_LastCompletedSimulationTick) return;
		auto& queued = m_PeerTailOutgoing[peer];
		auto& sent = m_PeerTailLastSentMs[peer];
		auto& repeats = m_PeerTailResendFrames[peer];
		uint64_t queuedBytes = 0;
		for (const auto& [frame, item]: queued) queuedBytes += item.bytes.size();
		// A frame this donor cannot send is named to the reader, which returns through a private image instead of waiting on it.
		const auto unavailable = [&](uint64_t frame) {
			auto missing = PeerFrameMessage(NetHostMigrationMessageType::PeerTailMissing);
			missing.frame = frame;
			SendPeerFrameMessage(std::move(missing), NetTransportLane::ControlReliable, peer);
		};
		const auto first = m_MigrationHistory.lower_bound(firstNeeded);
		if (first == m_MigrationHistory.end() || first->first != firstNeeded) { unavailable(firstNeeded); return; }
		const auto workEnd = RTE::SteadyClock::now() + std::chrono::milliseconds(2);
		uint32_t preparedFrames = 0;
		for (auto kept = first; kept != m_MigrationHistory.end() && kept->first < range->second.second && m_LastCompletedSimulationTick && kept->first <= *m_LastCompletedSimulationTick; ++kept) {
			// The donor's live seat keeps its next update while a reader catches up.
			if (preparedFrames >= 4 || (preparedFrames != 0 && RTE::SteadyClock::now() >= workEnd)) break;
			const auto [wire, route] = PeerFrameRoute(peer);
			const uint64_t retryMs = wire && wire->IsPeerPingMeasured(route) ? std::max<uint64_t>(500, wire->GetPeerPingMs(route)) : 1000;
			if ((m_PeerAppliedThrough.contains(peer) && kept->first <= m_PeerAppliedThrough.at(peer)) || queued.contains(kept->first) ||
			    (sent.contains(kept->first) && (!repeats.contains(kept->first) || m_TimingNowMs < sent.at(kept->first) + retryMs))) {
				range->second.first = std::max(range->second.first, kept->first + 1);
				continue;
			}
			// Bound application work even when several pipelined requests
			// overlap. A large record resumes at its last queued chunk.
			if (queued.size() >= 128 || queuedBytes >= 4 * 1024 * 1024) break;
			const uint32_t members = PeerFrameMembersAt(kept->first);
			// The frame goes as soon as this donor displays it, with the witnesses it holds: the reader confirms the frame's group with
			// the witnesses every member sends it too, so its trail is the transit alone.
			const auto witnesses = m_PeerFrameWitnesses.find(kept->first);
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
				// The reader's confirmed prefix omits decisions it has already applied.
				if (key.second != 3 && m_PeerAppliedThrough.contains(peer) && key.first <= m_PeerAppliedThrough.at(peer)) continue;
				std::vector<uint8_t> bytes;
				if (NetHostMigrationCodec::Encode(proof->second, m_Config.migrationKey, bytes)) certificates.push_back(std::move(bytes));
			}
			if (certificates.size() > 32) { unavailable(kept->first); return; }
			body.push_back(static_cast<uint8_t>(certificates.size()));
			for (const auto& bytes: certificates) { PutSize(body, static_cast<uint32_t>(bytes.size())); body.insert(body.end(), bytes.begin(), bytes.end()); }
			body.push_back(witnesses == m_PeerFrameWitnesses.end() ? 0 : static_cast<uint8_t>(witnesses->second.size()));
			if (witnesses != m_PeerFrameWitnesses.end()) for (const auto& [sender, witness]: witnesses->second) {
				std::vector<uint8_t> bytes;
				if (!NetHostMigrationCodec::Encode(witness, m_Config.migrationKey, bytes)) { unavailable(kept->first); return; }
				PutSize(body, static_cast<uint32_t>(bytes.size())); body.insert(body.end(), bytes.begin(), bytes.end());
			}
			if (body.size() > NetHostMigrationCodec::c_MaxPeerTailBytes) { unavailable(kept->first); return; }
			m_PeerTailRawBytesSent += body.size();
			bool compressedBody = false;
			std::vector<uint8_t> compressed(LZ4_compressBound(static_cast<int>(body.size())));
			const int size = LZ4_compress_default(reinterpret_cast<const char*>(body.data()), reinterpret_cast<char*>(compressed.data()), static_cast<int>(body.size()), static_cast<int>(compressed.size()));
			if (size > 0 && static_cast<size_t>(size) + 4 < body.size()) {
				std::vector<uint8_t> packed; PutSize(packed, static_cast<uint32_t>(body.size()));
				packed.insert(packed.end(), compressed.begin(), compressed.begin() + size); body = std::move(packed); compressedBody = true;
			}
			if (!queued.empty() && queuedBytes + body.size() > 4 * 1024 * 1024) break;
			queuedBytes += body.size();
			queued.emplace(kept->first, PeerTailOutgoing{std::move(body), members, 0, compressedBody});
			repeats.erase(kept->first);
			++preparedFrames;
			range->second.first = std::max(range->second.first, kept->first + 1);
		}
	}

	void NetLockstepCoordinator::FlushPeerCommittedTails(uint64_t nowMs) {
		constexpr uint64_t reliableQueueCap = 256 * 1024;
		for (auto range = m_PeerTailSendRanges.begin(); range != m_PeerTailSendRanges.end();) {
			if (m_RemovedPeers.contains(range->first) || range->second.first >= range->second.second) {
				range = m_PeerTailSendRanges.erase(range);
				continue;
			}
			if (m_LastCompletedSimulationTick && range->second.first <= *m_LastCompletedSimulationTick)
				SendPeerCommittedTail(range->first, range->second.first);
			++range;
		}
		for (auto& [peer, queued]: m_PeerTailOutgoing) {
			const uint64_t applied = m_PeerAppliedThrough.contains(peer) ? m_PeerAppliedThrough.at(peer) : 0;
			queued.erase(queued.begin(), queued.upper_bound(applied));
			auto& sent = m_PeerTailLastSentMs[peer];
			sent.erase(sent.begin(), sent.upper_bound(applied));
			auto& repeats = m_PeerTailResendFrames[peer];
			repeats.erase(repeats.begin(), repeats.upper_bound(applied));
			uint64_t handedOver = 0;
			const auto sendEnd = RTE::SteadyClock::now() + std::chrono::milliseconds(2);
			for (auto item = queued.begin(); item != queued.end() && handedOver < 64 * 1024;) {
				if (handedOver != 0 && RTE::SteadyClock::now() >= sendEnd) break;
				auto& body = item->second;
				// Small records arrive independently and repeat within the measured request interval.
				const bool independentRecord = body.bytes.size() <= 4096;
				const bool smallRepeat = sent.contains(item->first) && independentRecord;
				const size_t end = std::min(body.bytes.size(), body.offset + NetHostMigrationCodec::c_ChunkBytes);
				NetTransportQueueStats queue;
				const auto [wire, route] = PeerFrameRoute(peer);
				if (wire) queue = wire->GetPeerQueueStats(route);
				if (!independentRecord && queue.reliableBytes + end - body.offset + 512 > reliableQueueCap) { ++m_PeerTailDeferredSends; break; }
				auto tail = PeerFrameMessage(NetHostMigrationMessageType::PeerTail);
				tail.frame = item->first; tail.voterMask = body.members; tail.completeFrom = body.compressed ? 1 : 0;
				tail.totalBytes = static_cast<uint32_t>(body.bytes.size()); tail.offset = body.offset;
				tail.bytes.assign(body.bytes.begin() + body.offset, body.bytes.begin() + end);
				if (!SendPeerFrameMessage(std::move(tail), independentRecord ? NetTransportLane::BulkUnreliable : NetTransportLane::ControlReliable, peer)) break;
				if (smallRepeat && std::getenv("CC_TEST_RETURN_INPUT_DETAILS")) DiagnosticLine() << "[net-peer-tail-repeat] donor=" << static_cast<int>(m_Config.localPeerId)
				    << " reader=" << static_cast<int>(peer) << " frame=" << item->first << " bytes=" << body.bytes.size()
				    << " age_ms=" << nowMs - sent.at(item->first) << " now_ms=" << nowMs << std::endl;
				const size_t bytes = end - body.offset;
				handedOver += bytes; m_PeerTailBytesSent += bytes; body.offset = static_cast<uint32_t>(end);
				if (end == body.bytes.size()) {
					++m_PeerTailFramesSent; sent[item->first] = nowMs; item = queued.erase(item);
				}
			}
		}
	}

	void NetLockstepCoordinator::RequestPeerCommittedTail(uint64_t nowMs) {
		if (nowMs < m_PeerTailRequestAtMs) return;
		uint64_t roundTripMs = 100;
		for (const auto& [peer, applied]: m_PeerAppliedThrough) {
			if (peer == m_Config.localPeerId || !HasPeerFrameRoute(peer)) continue;
			const auto [wire, route] = PeerFrameRoute(peer);
			if (wire && wire->IsPeerPingMeasured(route)) roundTripMs = std::max<uint64_t>(roundTripMs, wire->GetPeerPingMs(route));
		}
		// Two batches of at most eight needed ranges fit in one measured round trip.
		m_PeerTailRequestAtMs = nowMs + std::max<uint64_t>(100, roundTripMs / 2);
		m_PeerTailRequestedUntil.erase(m_PeerTailRequestedUntil.begin(), m_PeerTailRequestedUntil.lower_bound(m_Stats.nextFrame));
		// Repair missing buffered ranges before they reach the consumer.
		if (!m_PeerCommittedTail.empty()) {
			const uint64_t lastBuffered = m_PeerCommittedTail.rbegin()->first;
			for (auto pending = m_PeerTailRequestedUntil.lower_bound(m_Stats.nextFrame);
			     pending != m_PeerTailRequestedUntil.end() && pending->first < lastBuffered; ++pending)
				if (!m_PeerCommittedTail.contains(pending->first) && pending->second <= nowMs + roundTripMs + 200)
					pending->second = std::min(pending->second, nowMs);
		}
		// A bounded subscription includes future history, sent only after the donor displays it.
		const uint64_t horizon = m_Stats.nextFrame + NetLockstepCodec::c_MaxFutureFrameSkew;
		uint32_t requestsInBatch = 0;
		for (uint64_t frame = m_Stats.nextFrame; frame < horizon && requestsInBatch < 8;) {
			const auto pending = m_PeerTailRequestedUntil.find(frame);
			if (m_PeerCommittedTail.contains(frame) || (pending != m_PeerTailRequestedUntil.end() && nowMs < pending->second)) { ++frame; continue; }
			uint8_t donor = 0; uint64_t through = 0, retryMs = 500;
			for (const auto& [peer, applied]: m_PeerAppliedThrough) {
				if (peer == m_Config.localPeerId || applied < frame || !HasPeerFrameRoute(peer)) continue;
				if (donor == 0 || applied > through) { donor = peer; through = applied; }
				const auto [wire, route] = PeerFrameRoute(peer);
				if (wire && wire->IsPeerPingMeasured(route)) retryMs = std::max<uint64_t>(retryMs, 2ULL * wire->GetPeerPingMs(route) + 200);
				if (const auto path = m_PeerReceiptDelaySamples.find(peer); path != m_PeerReceiptDelaySamples.end())
					retryMs = std::max<uint64_t>(retryMs, 2 * path->second.P95Ms() + 100);
			}
			if (donor == 0) break;
			if (m_PeerTailDonor != 0 && m_PeerAppliedThrough.contains(m_PeerTailDonor) &&
			    m_PeerAppliedThrough.at(m_PeerTailDonor) >= frame && HasPeerFrameRoute(m_PeerTailDonor)) {
				donor = m_PeerTailDonor;
				if (pending != m_PeerTailRequestedUntil.end())
					for (const auto& [peer, applied]: m_PeerAppliedThrough)
						if (peer != donor && peer != m_Config.localPeerId && applied >= frame && HasPeerFrameRoute(peer)) { donor = peer; break; }
			}
			m_PeerTailDonor = donor;
			auto request = PeerFrameMessage(NetHostMigrationMessageType::PeerTailRequest);
			request.frame = frame;
			uint64_t end = std::min(horizon, frame + 128);
			if (const auto ready = m_PeerCommittedTail.lower_bound(frame); ready != m_PeerCommittedTail.end()) end = std::min(end, ready->first);
			request.completeFrom = end;
			if (!SendPeerFrameMessage(request, NetTransportLane::BulkUnreliable, donor)) break;
			if (std::getenv("CC_TEST_RETURN_INPUT_DETAILS") && frame == m_Stats.nextFrame)
				DiagnosticLine() << "[net-peer-tail-request] reader=" << static_cast<int>(m_Config.localPeerId)
				    << " donor=" << static_cast<int>(donor) << " from=" << frame << " until=" << end
				    << " retry_ms=" << retryMs << " now_ms=" << nowMs << std::endl;
			++requestsInBatch;
			++m_PeerTailRequests;
			for (; frame < end; ++frame) m_PeerTailRequestedUntil[frame] = nowMs + retryMs;
		}
		if ((m_FrameGroupMembers & SeatBit(m_Config.localPeerId)) == 0 && nowMs >= m_PeerTailLogAtMs) {
			m_PeerTailLogAtMs = nowMs + 1000;
			DiagnosticLine() << "[net-peer-recovery] peer=" << static_cast<int>(m_Config.localPeerId)
			    << " now_ms=" << nowMs << " applied=" << m_LastCompletedSimulationTick.value_or(0) << " prepared=" << m_Stats.nextFrame
			    << " donor_applied=" << m_PeerTailThrough.value_or(0) << " certified_ahead=" << m_PeerCommittedTail.size()
			    << " requests=" << m_PeerTailRequests << std::endl;
		}
	}

	bool NetLockstepCoordinator::ConfirmPeerTail(uint64_t frame, uint64_t nowMs) {
		if (frame < m_Stats.nextFrame || m_PeerCommittedTail.contains(frame)) return false;
		const auto votes = m_PeerTailVotes.find(frame);
		if (votes == m_PeerTailVotes.end()) return false;
		const auto heard = m_PeerFrameWitnesses.find(frame);
		const auto same = [](const PeerTailVote& a, const PeerTailVote& b) { return a.members == b.members && a.prefix == b.prefix && a.prepared == b.prepared && a.bytes == b.bytes; };
		for (const auto& [donor, vote]: votes->second) {
			uint32_t agreeing = 0;
			for (const auto& [peer, other]: votes->second) if (same(other, vote)) agreeing |= SeatBit(peer) | other.confirmed;
			// A member's witness this peer received itself confirms the record as one its donor carried does.
			if (heard != m_PeerFrameWitnesses.end()) for (const auto& [sender, witness]: heard->second)
				if (witness.voterMask == vote.members && witness.bytes.size() == vote.digest.size() + vote.prepared.size() &&
				    std::equal(vote.digest.begin(), vote.digest.end(), witness.bytes.begin()) && std::equal(vote.prepared.begin(), vote.prepared.end(), witness.bytes.begin() + vote.digest.size()))
					agreeing |= SeatBit(sender);
			// An authenticated donor cannot commit a minority's tail by claiming somebody else's votes.
			if (!PeerFrameGroupConfirms(agreeing, vote.members, frame)) continue;
			m_PeerCommittedTail.try_emplace(frame, vote.bytes);
			m_PeerPreparedPrefixes.try_emplace(frame, vote.prepared);
			m_PeerTailPrefixes.try_emplace(frame, vote.prefix); m_PeerTailDecisions.try_emplace(frame, vote.proofs);
			m_PeerTailThrough = std::max(m_PeerTailThrough.value_or(0), vote.through);
			m_FrameGroupMembers = vote.members;
			if ((vote.members & SeatBit(m_Config.localPeerId)) == 0 && !m_PeerBridges.contains(m_Config.localPeerId)) {
				PeerBridge own;
				own.fromFrame = m_Stats.nextFrame;
				own.aiFrame = own.fromFrame;
				own.members = vote.members;
				// The matching majority's committed history already excludes this seat.
				own.committedHold = true;
				DiagnosticLine() << "[net-peer-hold-trigger] local=" << static_cast<int>(m_Config.localPeerId)
				    << " peer=" << static_cast<int>(m_Config.localPeerId) << " frame=" << own.fromFrame << " ai_frame=" << own.aiFrame
				    << " now_ms=" << nowMs << " cause=certified_committed_tail donor=" << static_cast<int>(donor)
				    << " tail_frame=" << frame << " members=" << vote.members << " voters=" << agreeing << std::endl;
				m_PeerBridges.emplace(m_Config.localPeerId, std::move(own));
				m_PeerFrameHolds[m_Config.localPeerId][m_Stats.nextFrame] = true;
				++m_Stats.peers[m_Config.localPeerId].holds;
				m_SeatBridgeSinceMs[m_Config.localPeerId] = nowMs;
			}
			return true;
		}
		if (frame == m_Stats.nextFrame && nowMs >= m_PeerTailLogAtMs) {
			m_PeerTailLogAtMs = nowMs + 1000;
			DiagnosticLine() << "[net-peer-tail-wait] local=" << static_cast<int>(m_Config.localPeerId) << " frame=" << frame
			    << " donors=" << votes->second.size() << " witnesses=" << (heard == m_PeerFrameWitnesses.end() ? 0 : heard->second.size()) << std::endl;
		}
		return false;
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
		const auto own = m_PeerBridges.find(m_Config.localPeerId);
		const bool privateReader = own != m_PeerBridges.end() && own->second.committedHold &&
		    (!own->second.returnFrame || frame < *own->second.returnFrame);
		// Certified history advances receipts for the original inputs it contains.
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
			// An excluded reader acknowledges history through its displayed progress.
			if (privateReader) continue;
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
		m_PeerHeartbeatAtMs = std::min(m_PeerHeartbeatAtMs, nowMs + 25);
		return true;
	}

	bool NetLockstepCoordinator::IsLocalPrivateReaderAt(uint64_t frame) const {
		NET_PLANE_CHECK();
		const auto own = m_PeerBridges.find(m_Config.localPeerId);
		return UsesPeerFrameGroups() && own != m_PeerBridges.end() && own->second.committedHold && frame >= own->second.fromFrame &&
		    (!own->second.returnFrame || frame < *own->second.returnFrame);
	}

	bool NetLockstepCoordinator::PeerFrameCatchUpActive() const {
		NET_PLANE_CHECK();
		if (!UsesPeerFrameGroups() || !IsRunning() || !m_LastCompletedSimulationTick) return false;
		const auto own = m_PeerBridges.find(m_Config.localPeerId);
		if (own != m_PeerBridges.end() && own->second.committedHold && !own->second.returnFrame &&
		    m_PeerTailThrough && *m_LastCompletedSimulationTick < *m_PeerTailThrough) return true;
		// The certified neutral interval is the returner's remaining catch-up runway. Keep using its existing bounded replay
		// budget through that interval; matching one survivor's display clock can still leave it behind a faster survivor's phase.
		if (const auto back = m_ReclaimTransactions.find(m_Config.localPeerId); back != m_ReclaimTransactions.end() &&
		    *m_LastCompletedSimulationTick >= back->second.activationFrame && *m_LastCompletedSimulationTick < back->second.neutralThroughFrame) return true;
		// The initial handshake reaches seats at different times. Follow one
		// stable member's displayed clock so that difference cannot become a
		// permanent phase offset. This only selects the existing 3x/six-tick
		// consumer budget; every frame still needs its ordinary certificate.
		if (m_FrameGroupMembers != 0 && m_Config.simTickMs > 0) {
			// Keep the surviving display clock when its former host returns.
			const uint8_t reference = (m_FrameGroupMembers & SeatBit(m_PeerPaceReference)) != 0 ? m_PeerPaceReference :
			    static_cast<uint8_t>(std::countr_zero(m_FrameGroupMembers) + 1);
			// The pace leader advances at the ordinary rate after a shared pause.
			// No displayed peer clock justifies replaying the group's paused time.
			if (reference == m_Config.localPeerId) return false;
			const auto applied = m_PeerAppliedThrough.find(reference), observed = m_PeerAppliedAtMs.find(reference);
			const auto [wire, route] = PeerFrameRoute(reference);
			const bool measured = wire && wire->IsPeerPingMeasured(route);
			const uint64_t freshnessMs = measured ? std::max<uint64_t>(250, wire->GetPeerPingMs(route) + 250ULL) : 250;
			if (reference != m_Config.localPeerId && applied != m_PeerAppliedThrough.end() && observed != m_PeerAppliedAtMs.end() &&
			    m_TimingNowMs >= observed->second && m_TimingNowMs - observed->second <= freshnessMs) {
				double transit = 0.0;
				if (measured) transit = wire->GetPeerPingMs(route) / 2.0;
				const uint64_t elapsed = static_cast<uint64_t>((m_TimingNowMs - observed->second + transit) / m_Config.simTickMs);
				if (applied->second > *m_LastCompletedSimulationTick + 1 ||
				    elapsed > *m_LastCompletedSimulationTick + 1 - std::min(applied->second, *m_LastCompletedSimulationTick + 1)) return true;
				return false;
			}
		}
		if (!m_PeerHadHitch || !m_PeerPaceStartMs || m_TimingNowMs < *m_PeerPaceStartMs || m_Config.simTickMs <= 0) return false;
		const uint64_t due = m_PeerPaceStartFrame + static_cast<uint64_t>((m_TimingNowMs - *m_PeerPaceStartMs) / m_Config.simTickMs);
		return *m_LastCompletedSimulationTick + 1 < due;
	}

	bool NetLockstepCoordinator::ProposePeerReclaim(uint8_t peer, NetPeerId transport, uint32_t incarnation, uint64_t frame, uint64_t trailFrames, std::string* error) {
		if (!IsRunning() || m_Config.localPeerId != GetHostPeerId() || peer == 0 || peer > m_Config.peerCount ||
		    !m_PeerBridges.contains(peer) || !m_PeerBridges.at(peer).committedHold || transport == c_InvalidNetPeerId || incarnation == 0 || frame <= m_Stats.nextFrame ||
		    frame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew || !PeerGroupHasAuthority(m_FrameGroupMembers)) {
			if (error) *error = "the private return needs an authoritative group and a future frame";
			return false;
		}
		if (HasAgreedSeatReclaim(peer, frame)) return true;
		auto proposal = PeerFrameMessage(NetHostMigrationMessageType::PeerBridge);
		proposal.preparedFrame = 1; proposal.frame = frame; proposal.boundary = m_LastCompletedSimulationTick.value_or(m_Stats.nextFrame - 1);
		proposal.members = {peer}; proposal.connectedMask = PeerFrameMembersAt(frame) | SeatBit(peer);
		for (const auto& [returned, bridge]: m_PeerBridges)
			if (bridge.returnFrame && *bridge.returnFrame <= frame && !m_RemovedPeers.contains(returned) && !IsSeatReleased(returned)) proposal.connectedMask |= SeatBit(returned);
		for (uint8_t voter = 1; voter <= m_Config.peerCount; ++voter) {
			if (voter == peer || voter == m_Config.localPeerId || (proposal.connectedMask & SeatBit(voter)) == 0) continue;
			const auto applied = m_PeerAppliedThrough.find(voter);
			if (applied == m_PeerAppliedThrough.end() || applied->second < m_Config.startFrame) { if (error) *error = "the private return awaits a playing voter's common prefix"; return false; }
			proposal.boundary = std::min(proposal.boundary, applied->second);
		}
		const auto prefix = PeerAppliedFramePrefix(proposal.boundary); proposal.bytes.assign(prefix.begin(), prefix.end());
		const uint32_t delay = m_Config.adaptiveInputDelay ? std::min<uint16_t>(NetLockstepCodec::c_MaxPeerInputDelayFrames,
		    std::max<uint16_t>(InputDelayAt(peer, frame), ReturnDelayFloor(peer, transport))) : PeerInputDelay(peer);
		const uint64_t transit = static_cast<uint64_t>(std::ceil((m_Stats.peers[peer].pingMs + m_Stats.peers[peer].jitterMs) / m_Config.simTickMs));
		const uint64_t neutral = delay + transit + trailFrames + m_Config.slowPlayerBoundTicks;
		if (neutral > NetLockstepCodec::c_MaxFutureFrameSkew) { if (error) *error = "the private return is still too far behind its activation"; return false; }
		PutSize(proposal.bytes, delay); PutSize(proposal.bytes, static_cast<uint32_t>(neutral)); PutSize(proposal.bytes, incarnation);
		proposal.totalBytes = static_cast<uint32_t>(proposal.bytes.size()); proposal.voterMask = SeatBit(m_Config.localPeerId);
		if (!ValidatePeerBridge(proposal)) { if (error) *error = "the private return does not match the committed boundary"; return false; }
		if (m_Config.peerSessionLinks) m_Config.peerSessionLinks->BindAdmission(peer, transport);
		m_RemoteTransports[peer] = transport;
		if (m_Config.peerSessionLinks) {
			const auto [wire, native] = m_Config.peerSessionLinks->RouteTo(peer);
			DiagnosticLine() << "[net-frame-group] return proposed peer=" << static_cast<int>(peer) << " frame=" << frame << " routes=" << m_Config.peerSessionLinks->RoutesTo(peer).size()
			    << " native=" << native << " route=" << (wire ? wire->GetConnectedRoute(native) : std::string("none")) << std::endl;
		}
		m_PeerAdmissions[peer] = {frame, static_cast<uint16_t>(delay)};
		m_RemoteStartsReceived.erase(peer); m_RemoteStarts.erase(peer);
		m_PeersPlayedThisRound.erase(peer); m_LastStartAnswerMs.erase(peer);
		auto& votes = m_PeerBridgeVotes[{frame, 1}];
		if (const auto own = votes.find(m_Config.localPeerId); own != votes.end()) return SameBridge(own->second, proposal);
		if (VotedOtherPeerReturn(proposal)) { if (error) *error = "another return of the seat is still being decided"; return false; }
		votes.emplace(m_Config.localPeerId, proposal); SendPeerFrameMessage(std::move(proposal)); TryCommitPeerBridge(frame, m_TimingNowMs, 1);
		return true;
	}

	bool NetLockstepCoordinator::AdoptPeerReturnConfig(NetLockstepConfig& config, const NetLockstepCoordinator& replay, uint64_t activation) const {
		NET_PLANE_CHECK();
		NetLockstepPlane::Check(&replay, "AdoptPeerReturnConfig (the replay)");
		const auto kept = m_PeerReturnProofs.find(activation);
		if (!UsesPeerFrameGroups() || kept == m_PeerReturnProofs.end()) return false;
		return AdoptPeerReturn(config, replay, kept->second, activation);
	}

	bool NetLockstepCoordinator::AdoptPeerReturn(NetLockstepConfig& config, const NetLockstepCoordinator& replay, const NetHostMigrationMessage& proof, uint64_t activation, std::string* why) const {
		NET_PLANE_CHECK();
		NetLockstepPlane::Check(&replay, "AdoptPeerReturn (the replay)");
		NetHostMigrationMessage agreed; uint32_t voters = 0;
		const auto refuse = [&](const char* reason) { if (why) *why = reason; return false; };
		if (proof.type != NetHostMigrationMessageType::PeerBridgeCommit || proof.frame != activation) return refuse("not this return's certificate");
		if (!DecodePeerBridgeCertificate(proof, agreed, voters)) return refuse("certificate unreadable");
		if (!PeerGroupHasAuthority(voters, activation)) return refuse("voters lack authority");
		if (agreed.sessionId != config.sessionId) return refuse("other session");
		if (agreed.roundId != config.matchConfig.roundId) return refuse("other round");
		if (agreed.configHash != config.originalRoundConfigHash) return refuse("other round configuration");
		if (agreed.preparedFrame != 1) return refuse("not committed");
		if (ReturnCrossesDeparture(agreed, m_PeerOwnershipChanges)) return refuse("owner changed before return");
		for (uint8_t peer: agreed.members) if ((PeerRetainedSeatsAt(activation) & SeatBit(peer)) == 0) return refuse("seat removed before return");
		if (!replay.m_MigrationHistory.contains(agreed.boundary) && !replay.m_PeerReplayAppliedPrefixes.contains(agreed.boundary)) return refuse("boundary not replayed");
		const auto prefix = replay.PeerAppliedFramePrefix(agreed.boundary);
		if (!std::equal(prefix.begin(), prefix.end(), agreed.bytes.begin())) return refuse("applied prefix differs");
		for (const auto& [peer, terms]: replay.m_Config.initialPeerStartTerms) config.initialPeerStartTerms[peer] = terms;
		size_t offset = prefix.size();
		for (uint8_t peer: agreed.members) {
			uint32_t delay = 0, neutral = 0, incarnation = 0;
			if (!TakeSize(agreed.bytes, offset, delay) || !TakeSize(agreed.bytes, offset, neutral) || !TakeSize(agreed.bytes, offset, incarnation) ||
			    peer == 0 || peer > config.peerCount || delay == 0 || delay > NetLockstepCodec::c_MaxInputDelayFrames ||
			    neutral < delay || neutral > NetLockstepCodec::c_MaxFutureFrameSkew || incarnation == 0 || activation > UINT64_MAX - neutral) return false;
			config.initialSeatReclaims[peer] = {peer, config.migrationGeneration, activation * 8 + peer, incarnation, activation, static_cast<uint16_t>(delay), activation + neutral};
			config.peerIncarnations[peer] = incarnation;
			config.initialPeerStartTerms[peer] = {activation, static_cast<uint16_t>(delay)};
		}
		return offset == agreed.bytes.size() && config.initialSeatReclaims.contains(config.localPeerId);
	}

} // namespace RTE
