#include "NetPeerFrameHelpers.h"
#include "SteadyClock.h"

#include "DiagnosticLine.h"
#include "NetAuthCrypto.h"
#include "NetLobbyProtocol.h"
#include "NetProtocol.h"
#include "NetWorldJoin.h"

#include "lz4.h"

#include <algorithm>
#include <bit>
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
		if (m_Playback || frame < m_Config.startFrame)
			if (const auto applied = m_PeerReplayAppliedPrefixes.find(frame); applied != m_PeerReplayAppliedPrefixes.end()) return applied->second;
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
		for (const auto& [peer, other]: m_PeerFrameWitnesses[frame]) if (peer != m_Config.localPeerId && other.bytes.size() >= NetHash32{}.size()) {
			NetHash32 prefix{};
			std::copy_n(other.bytes.begin(), prefix.size(), prefix.begin());
			NotePeerRecord(peer, frame, prefix);
			NotePeerContinuation(peer, frame, prefix, other.voterMask);
		}
		if (!m_DeferredChecksumFrames.empty()) RecheckDeferredChecksums();
	}

	void NetLockstepCoordinator::SendPeerCommittedTail(uint8_t peer, uint64_t fromFrame) {
		if (fromFrame == UINT64_MAX) return;
		// Standalone old-input requests retain a bounded one-frame reply too.
		auto range = m_PeerTailSendRanges.try_emplace(peer, fromFrame, fromFrame + 1).first;
		range->second.first = std::min(range->second.first, fromFrame);
		range->second.second = std::max(range->second.second, fromFrame + 1);
		// A frame the reader has applied is never sent again, so a range left from before its last read starts past it.
		if (const auto applied = m_PeerAppliedThrough.find(peer); applied != m_PeerAppliedThrough.end())
			range->second.first = std::max(range->second.first, applied->second + 1);
		if (range->second.first >= range->second.second) return;
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
			if ((m_PeerAppliedThrough.contains(peer) && kept->first <= m_PeerAppliedThrough.at(peer)) || queued.contains(kept->first) ||
			    (sent.contains(kept->first) && !repeats.contains(kept->first))) {
				range->second.first = std::max(range->second.first, kept->first + 1);
				continue;
			}
			// A repeat marker is an authenticated reader's explicit missing-range request, not a resend timer. Honor that
			// repair through the ordinary frame/byte work caps; a prior send cannot prove that the reader received it.
			// Bound application work even when several pipelined requests
			// overlap. A large record resumes at its last queued chunk.
			if (queued.size() >= 128 || queuedBytes >= 4 * 1024 * 1024) break;
			const uint32_t members = PeerFrameMembersAt(kept->first);
			// The frame goes as soon as this donor displays it, with the witnesses it holds: the reader confirms the frame's group with
			// the witnesses every member sends it too, so its trail is the transit alone.
			const auto prefix = kept->first == m_Config.startFrame ? NetHash32{} : PeerFramePrefix(kept->first - 1);
			const auto prepared = PeerFramePrefix(kept->first);
			std::vector<const NetHostMigrationMessage*> witnesses;
			if (const auto heard = m_PeerFrameWitnesses.find(kept->first); heard != m_PeerFrameWitnesses.end()) {
				const auto digest = PeerAppliedFramePrefix(kept->first);
				// A different continuation's witness cannot certify this record, even at the same frame number.
				for (const auto& [sender, witness]: heard->second)
					if (sender > 0 && sender <= m_Config.peerCount && (members & SeatBit(sender)) != 0 && witness.voterMask == members &&
					    witness.frame == kept->first && witness.appliedFrame == kept->first && witness.senderPeerId == sender &&
					    witness.bytes.size() == digest.size() + prepared.size() && std::equal(digest.begin(), digest.end(), witness.bytes.begin()) &&
					    std::equal(prepared.begin(), prepared.end(), witness.bytes.begin() + digest.size())) witnesses.push_back(&witness);
			}
			std::vector<uint8_t> body(prefix.begin(), prefix.end()); body.insert(body.end(), prepared.begin(), prepared.end());
			PutSize(body, static_cast<uint32_t>(kept->second.size()));
			body.insert(body.end(), kept->second.begin(), kept->second.end());
			std::map<uint8_t, const NetHostMigrationMessage*> latest;
			for (const auto& [key, decision]: m_PeerBridgeCertificates) if (key.first <= kept->first) latest[key.second] = &decision;
			std::set<PeerDecisionKey> proofs;
			for (const auto& [kind, decision]: latest) proofs.emplace(decision->frame, kind);
			for (const auto& [key, decision]: m_PeerBridgeCertificates) if (key.second == 3) proofs.insert(key);
			for (const auto& [seat, bridge]: m_PeerBridges) if (bridge.fromFrame <= kept->first)
				for (auto proof = m_PeerBridgeCertificates.lower_bound({bridge.fromFrame, 0x10}); proof != m_PeerBridgeCertificates.end() && proof->first.first == bridge.fromFrame; ++proof)
					if (IsHoldDecision(proof->first.second) && (proof->first.second & SeatBit(seat)) != 0) proofs.insert(proof->first);
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
			body.push_back(static_cast<uint8_t>(witnesses.size()));
			for (const auto* witness: witnesses) {
				std::vector<uint8_t> bytes;
				if (!NetHostMigrationCodec::Encode(*witness, m_Config.migrationKey, bytes)) { unavailable(kept->first); return; }
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
				if (smallRepeat && ReturnInputDetails()) DiagnosticLine() << "[net-peer-tail-repeat] donor=" << static_cast<int>(m_Config.localPeerId)
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
		m_PeerTailRequestedOf.erase(m_PeerTailRequestedOf.begin(), m_PeerTailRequestedOf.lower_bound(m_Stats.nextFrame));
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
		// A playing donor reports its applied frame about every tick; one silent for a few ticks and half a round trip has stopped serving.
		const auto serving = [&](uint8_t peer) {
			const auto at = m_PeerAppliedAtMs.find(peer);
			return at != m_PeerAppliedAtMs.end() && (nowMs < at->second || nowMs - at->second <= static_cast<uint64_t>(std::ceil(4 * m_Config.simTickMs)) + roundTripMs / 2);
		};
		uint32_t requestsInBatch = 0;
		for (uint64_t frame = m_Stats.nextFrame; frame < horizon && requestsInBatch < 8;) {
			const auto pending = m_PeerTailRequestedUntil.find(frame);
			const auto askedOf = m_PeerTailRequestedOf.find(frame);
			// A request to a donor that has since stopped is asked again of another at once.
			const bool askedServing = askedOf == m_PeerTailRequestedOf.end() || serving(askedOf->second);
			if (m_PeerCommittedTail.contains(frame) || (pending != m_PeerTailRequestedUntil.end() && nowMs < pending->second && askedServing)) { ++frame; continue; }
			uint8_t donor = 0; uint64_t through = 0, retryMs = 500;
			bool donorServing = false;
			for (const auto& [peer, applied]: m_PeerAppliedThrough) {
				if (peer == m_Config.localPeerId || applied < frame || !HasPeerFrameRoute(peer)) continue;
				const bool peerServing = serving(peer);
				if (donor == 0 || (peerServing && !donorServing) || (peerServing == donorServing && applied > through)) { donor = peer; through = applied; donorServing = peerServing; }
				const auto [wire, route] = PeerFrameRoute(peer);
				if (wire && wire->IsPeerPingMeasured(route)) retryMs = std::max<uint64_t>(retryMs, 2ULL * wire->GetPeerPingMs(route) + 200);
				if (const auto path = m_PeerReceiptDelaySamples.find(peer); path != m_PeerReceiptDelaySamples.end())
					retryMs = std::max<uint64_t>(retryMs, 2 * path->second.P95Ms() + 100);
			}
			if (donor == 0) break;
			if (m_PeerTailDonor != 0 && m_PeerAppliedThrough.contains(m_PeerTailDonor) &&
			    m_PeerAppliedThrough.at(m_PeerTailDonor) >= frame && HasPeerFrameRoute(m_PeerTailDonor) && (serving(m_PeerTailDonor) || !donorServing)) donor = m_PeerTailDonor;
			// A retry asks another donor than the one that did not deliver this frame, not merely another than the last one asked.
			if (const auto asked = m_PeerTailRequestedOf.find(frame); pending != m_PeerTailRequestedUntil.end() && asked != m_PeerTailRequestedOf.end() && asked->second == donor)
				for (const auto& [peer, applied]: m_PeerAppliedThrough)
					if (peer != donor && peer != m_Config.localPeerId && applied >= frame && HasPeerFrameRoute(peer)) { donor = peer; break; }
			m_PeerTailDonor = donor;
			auto request = PeerFrameMessage(NetHostMigrationMessageType::PeerTailRequest);
			request.frame = frame;
			uint64_t end = std::min(horizon, frame + 128);
			if (const auto ready = m_PeerCommittedTail.lower_bound(frame); ready != m_PeerCommittedTail.end()) end = std::min(end, ready->first);
			request.completeFrom = end;
			if (!SendPeerFrameMessage(request, NetTransportLane::BulkUnreliable, donor)) break;
			if (ReturnInputDetails() && frame == m_Stats.nextFrame)
				DiagnosticLine() << "[net-peer-tail-request] reader=" << static_cast<int>(m_Config.localPeerId)
				    << " donor=" << static_cast<int>(donor) << " from=" << frame << " until=" << end
				    << " retry_ms=" << retryMs << " now_ms=" << nowMs << std::endl;
			++requestsInBatch;
			++m_PeerTailRequests;
			for (; frame < end; ++frame) { m_PeerTailRequestedUntil[frame] = nowMs + retryMs; m_PeerTailRequestedOf[frame] = donor; }
		}
		if ((m_FrameGroupMembers & SeatBit(m_Config.localPeerId)) == 0 && nowMs >= m_PeerTailLogAtMs) {
			m_PeerTailLogAtMs = nowMs + 1000;
			DiagnosticLine() << "[net-peer-recovery] peer=" << static_cast<int>(m_Config.localPeerId)
			    << " now_ms=" << nowMs << " applied=" << m_LastCompletedSimulationTick.value_or(0) << " prepared=" << m_Stats.nextFrame
			    << " donor_applied=" << m_PeerTailThrough.value_or(0) << " certified_ahead=" << m_PeerCommittedTail.size()
			    << " requests=" << m_PeerTailRequests << " next_committed=" << m_PeerCommittedTail.contains(m_Stats.nextFrame)
			    << " next_records=" << (m_PeerTailVotes.contains(m_Stats.nextFrame) ? m_PeerTailVotes.at(m_Stats.nextFrame).size() : 0)
			    << " block=" << m_AdvanceBlock << std::endl;
		}
	}

	bool NetLockstepCoordinator::TakeLoneSeat(uint64_t nowMs) {
		const uint64_t frame = m_Stats.nextFrame;
		const uint8_t local = m_Config.localPeerId;
		if (!IsLocalPrivateReaderAt(frame) || !m_LastCompletedSimulationTick || m_GrantedSimulationTick || m_PeerCommittedTail.contains(frame) ||
		    !m_MigrationHistory.contains(*m_LastCompletedSimulationTick)) { m_PeerNoDonorSinceMs.reset(); return false; }
		// A seat heard within the host-loss window gives this frame when it has applied it, still plays, or is still reading toward its
		// own return: wait for it. The rest are gone, or held and waiting like this one.
		uint32_t waiting = SeatBit(local);
		uint64_t roundTripMs = 0;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if (peer == local || m_RemovedPeers.contains(peer) || IsSeatReleased(peer)) continue;
			const uint64_t heard = LastAuthenticatedTraffic(peer);
			if (nowMs >= heard && nowMs - heard >= c_NetHostLossSilenceMs) continue;
			const auto applied = m_PeerAppliedThrough.find(peer);
			const auto reported = m_PeerReportedMembers.find(peer);
			const auto rose = m_PeerAppliedAtMs.find(peer);
			const uint64_t settleMs = 2 * c_NetPeerHeartbeatMs + m_Stats.peers[peer].pingMs;
			if ((applied != m_PeerAppliedThrough.end() && applied->second >= frame) || reported == m_PeerReportedMembers.end() || (reported->second & SeatBit(peer)) != 0 ||
			    (rose != m_PeerAppliedAtMs.end() && (nowMs < rose->second || nowMs - rose->second < settleMs))) {
				m_PeerNoDonorSinceMs.reset();
				return false;
			}
			waiting |= SeatBit(peer);
			roundTripMs = std::max<uint64_t>(roundTripMs, m_Stats.peers[peer].pingMs);
		}
		if (!m_PeerNoDonorSinceMs) m_PeerNoDonorSinceMs = nowMs;
		// One waiting seat plays on, the one the heal would rule for: the original host, else the lowest seat. The others read its history.
		const uint8_t host = m_Config.matchConfig.hostPeerId;
		const uint8_t chosen = (waiting & SeatBit(host)) != 0 ? host : static_cast<uint8_t>(std::countr_zero(waiting) + 1);
		// The others' reports are a round trip and two heartbeats old at most before this seat acts on them.
		if (chosen != local || nowMs - *m_PeerNoDonorSinceMs < 2 * c_NetPeerHeartbeatMs + roundTripMs) return false;
		const uint64_t displayed = *m_LastCompletedSimulationTick;
		const auto certify = [&](NetHostMigrationMessage decision) {
			decision.successorPeerId = local; decision.voterMask = SeatBit(local); decision.totalBytes = static_cast<uint32_t>(decision.bytes.size());
			std::vector<uint8_t> vote;
			if (!NetHostMigrationCodec::Encode(decision, m_Config.migrationKey, vote)) return false;
			auto certificate = decision;
			certificate.type = NetHostMigrationMessageType::PeerBridgeCommit;
			certificate.bytes = {1};
			PutSize(certificate.bytes, static_cast<uint32_t>(vote.size()));
			certificate.bytes.insert(certificate.bytes.end(), vote.begin(), vote.end());
			certificate.totalBytes = static_cast<uint32_t>(certificate.bytes.size());
			InstallPeerBridge(certificate, nowMs);
			return m_PeerBridgeCertificates.contains({frame, DecisionKind(certificate)});
		};
		// Its own return at this frame, its input neutral through its delay and the bound.
		auto back = PeerFrameMessage(NetHostMigrationMessageType::PeerBridge);
		back.frame = frame; back.boundary = displayed; back.preparedFrame = 1; back.members = {local}; back.connectedMask = SeatBit(local);
		const auto applied = PeerAppliedFramePrefix(displayed);
		back.bytes.assign(applied.begin(), applied.end());
		const uint16_t delay = InputDelayAt(local, frame);
		PutSize(back.bytes, delay); PutSize(back.bytes, delay + m_Config.slowPlayerBoundTicks);
		PutSize(back.bytes, std::max<uint32_t>(1, m_Config.peerIncarnations[local]));
		m_LoneSeatInstalling = true;
		const bool returned = certify(back);
		m_LoneSeatInstalling = false;
		if (!returned) return false;
		// And a hold of every seat still playing here: none of them can reach it.
		auto hold = PeerFrameMessage(NetHostMigrationMessageType::PeerBridge);
		hold.frame = hold.boundary = frame; hold.preparedFrame = 0; hold.connectedMask = SeatBit(local);
		const uint32_t others = PeerRetainedSeatsAt(frame) & ~PeerHeldSeatsAt(frame) & ~SeatBit(local);
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if ((others & SeatBit(peer)) != 0) hold.members.push_back(peer);
		const auto prepared = PeerFramePrefix(frame - 1);
		hold.bytes.assign(prepared.begin(), prepared.end());
		if (!hold.members.empty()) certify(hold);
		DiagnosticLine() << "[net-peer-recovery] peer=" << static_cast<int>(local) << " now_ms=" << nowMs << " frame=" << frame
		                 << " lone=1 waiting=" << waiting << " held=" << others << ": no seat can give this frame; this seat plays on alone" << std::endl;
		m_PeerNoDonorSinceMs.reset();
		return true;
	}

	bool NetLockstepCoordinator::ConfirmPeerTail(uint64_t frame, uint64_t nowMs) {
		if (frame < m_Stats.nextFrame || m_PeerCommittedTail.contains(frame)) return false;
		const auto votes = m_PeerTailVotes.find(frame);
		if (votes == m_PeerTailVotes.end()) return false;
		const auto heard = m_PeerFrameWitnesses.find(frame);
		const auto same = [](const PeerTailVote& a, const PeerTailVote& b) { return a.members == b.members && a.prefix == b.prefix && a.prepared == b.prepared && a.bytes == b.bytes; };
		// A record that continues the history this seat displays, or the record it already took for the frame before, comes first.
		std::optional<NetHash32> continued;
		if (frame > m_Config.startFrame && frame == m_Stats.nextFrame) continued = PeerFramePrefix(frame - 1);
		else if (const auto before = m_PeerPreparedPrefixes.find(frame - 1); frame > m_Config.startFrame && before != m_PeerPreparedPrefixes.end()) continued = before->second;
		for (const bool continuing: {true, false}) for (const auto& [donor, vote]: votes->second) {
			if (continued && (vote.prefix == *continued) != continuing) continue;
			if (!continued && !continuing) break;
			// A remote group's tail is history, not authority to turn an active excluded owner into a private reader.
			// Its installed standing hold (or the heal's no-continuation exchange) must establish that role first.
			if ((vote.members & SeatBit(m_Config.localPeerId)) == 0 &&
			    (PeerFrameMembersAt(frame) & SeatBit(m_Config.localPeerId)) != 0) continue;
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

	bool NetLockstepCoordinator::PrivateTailUnusable(uint64_t frame, uint64_t nowMs, const char* reason) {
		// A private reader that holds records of its next frame it cannot take - none confirmed, or each playing a seat it holds - has
		// read another continuation: past the referee's tie deadline it returns through a private image instead of asking again.
		if (m_PeerTailUnconfirmed.first != frame) {
			m_PeerTailUnconfirmed = {frame, nowMs};
			return false;
		}
		const auto own = m_PeerBridges.find(m_Config.localPeerId);
		if (nowMs < m_PeerTailUnconfirmed.second + c_NetFrameTieDeadlineMs || own == m_PeerBridges.end() || !own->second.committedHold) return false;
		m_PeerCommittedTail.erase(frame); m_PeerTailPrefixes.erase(frame); m_PeerPreparedPrefixes.erase(frame);
		m_PeerTailVotes.erase(frame); m_PeerTailDecisions.erase(frame);
		m_Stats.nextFrame = GetResumeFrame();
		m_ReadyFrames.clear();
		m_LocalSeatHeld = true; m_LocalHoldFrame = std::min(m_Stats.nextFrame, own->second.fromFrame);
		m_State = NetLockstepState::Stopped;
		m_Stats.timeoutReason = reason;
		DiagnosticLine() << "[net-peer-tail-continuation] local=" << static_cast<int>(m_Config.localPeerId) << " frame=" << frame
		                 << " hold_frame=" << m_LocalHoldFrame << " now_ms=" << nowMs << " action=image_return reason=unusable" << std::endl;
		return true;
	}

	bool NetLockstepCoordinator::ApplyPeerCommittedTail(uint64_t frame, uint64_t nowMs) {
		const auto kept = m_PeerCommittedTail.find(frame);
		if (kept == m_PeerCommittedTail.end()) return false;
		if (frame > m_Config.startFrame && !m_PeerTailPrefixes.contains(frame)) {
			Fail(NetLockstepStopReason::ProtocolError, frame, "the committed tail differs from displayed history at frame " + std::to_string(frame)); return false;
		}
		if (frame > m_Config.startFrame && m_PeerTailPrefixes.at(frame) != PeerFramePrefix(frame - 1)) {
			// No donor gives this frame on the history this seat displays: it read another continuation's records before this one. It returns
			// through a private image from the frame it began reading, never by playing a second continuation over the first.
			if (m_GrantedSimulationTick) return false;
			if (ReturnInputDetails()) {
				DiagnosticLine detail;
				detail << "[net-peer-tail-other-history] local=" << static_cast<int>(m_Config.localPeerId) << " frame=" << frame << " start=" << m_Config.startFrame
				    << " effective_start=" << m_Stats.effectiveStartFrame << " displayed=" << m_LastCompletedSimulationTick.value_or(0) << " members=" << m_FrameGroupMembers;
				if (const auto votes = m_PeerTailVotes.find(frame); votes != m_PeerTailVotes.end())
					for (const auto& [donor, vote]: votes->second) detail << " donor" << static_cast<int>(donor) << "=" << vote.members << "/" << vote.confirmed << "/" << vote.through
					    << "/" << (vote.prefix == PeerFramePrefix(frame - 1));
			}
			m_PeerCommittedTail.erase(kept); m_PeerTailPrefixes.erase(frame); m_PeerPreparedPrefixes.erase(frame);
			m_PeerTailVotes.erase(frame); m_PeerTailDecisions.erase(frame);
			const auto own = m_PeerBridges.find(m_Config.localPeerId);
			m_Stats.nextFrame = GetResumeFrame();
			m_ReadyFrames.clear();
			m_LocalSeatHeld = true; m_LocalHoldFrame = std::min(m_Stats.nextFrame, own != m_PeerBridges.end() ? own->second.fromFrame : m_Stats.nextFrame);
			m_State = NetLockstepState::Stopped;
			m_Stats.timeoutReason = "PeerHeld:the committed tail continues another history; returning through a private image";
			DiagnosticLine() << "[net-peer-tail-continuation] local=" << static_cast<int>(m_Config.localPeerId) << " frame=" << frame
			                 << " hold_frame=" << m_LocalHoldFrame << " now_ms=" << nowMs << " action=image_return" << std::endl;
			return false;
		}
		NetLockstepReadyFrame ready;
		if (!DecodeMigrationFrame(kept->second, frame, ready)) {
			Fail(NetLockstepStopReason::ProtocolError, frame, "invalid committed peer tail"); return false;
		}
		// The record is the history this seat plays. One in which a seat it holds at this frame plays is another continuation's:
		// it is set aside unapplied and the frame asked of another donor.
		for (const auto& [peer, count]: ready.remoteFrameCounts) {
			if (peer == m_Config.localPeerId || !IsSeatUnderAI(peer, frame)) continue;
			if (PrivateTailUnusable(frame, nowMs, "PeerHeld:no record of this frame plays the seats this seat holds; returning through a private image")) return false;
			if (const auto votes = m_PeerTailVotes.find(frame); votes != m_PeerTailVotes.end())
				for (const auto& [donor, vote]: votes->second) if (vote.bytes == kept->second) { m_PeerTailRequestedOf[frame] = donor; m_PeerTailRequestedUntil[frame] = 0; break; }
			m_PeerCommittedTail.erase(kept); m_PeerTailPrefixes.erase(frame); m_PeerPreparedPrefixes.erase(frame);
			m_PeerTailVotes.erase(frame); m_PeerTailDecisions.erase(frame);
			m_PeerTailRequestAtMs = 0; RequestPeerCommittedTail(nowMs);
			return false;
		}
		if (const auto decisions = m_PeerTailDecisions.find(frame); decisions != m_PeerTailDecisions.end()) {
			for (const auto& decision: decisions->second) InstallPeerBridge(decision, nowMs, true);
			m_PeerTailDecisions.erase(decisions);
		}
		PreparePeerBridgeInputs(frame, nowMs);
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

	bool NetLockstepCoordinator::PeerFrameCatchUpNeeded() const {
		return PeerFrameCatchUp(false);
	}

	bool NetLockstepCoordinator::PeerFrameCatchUpActive(double nextOrdinaryTickInMs) const {
		return PeerFrameCatchUp(true, nextOrdinaryTickInMs);
	}

	bool NetLockstepCoordinator::PeerFrameCatchUp(bool requireReceiptRunway, double nextOrdinaryTickInMs) const {
		NET_PLANE_CHECK();
		if (!IsRunning() || !m_LastCompletedSimulationTick) return false;
		if (const auto phase = InputDelayRetimingTarget(); phase && *m_LastCompletedSimulationTick < *phase) return true;
		if (!UsesPeerFrameGroups()) return false;
		const uint64_t next = *m_LastCompletedSimulationTick + 1;
		const auto playingClockAhead = [&](uint8_t peer) -> std::optional<bool> {
			const auto applied = m_PeerAppliedThrough.find(peer), observed = m_PeerAppliedAtMs.find(peer);
			const auto [wire, route] = PeerFrameRoute(peer);
			const bool measured = wire && wire->IsPeerPingMeasured(route);
			const uint64_t freshnessMs = measured ? std::max<uint64_t>(250, wire->GetPeerPingMs(route) + 250ULL) : 250;
			if (m_Config.simTickMs <= 0 || applied == m_PeerAppliedThrough.end() || observed == m_PeerAppliedAtMs.end() ||
			    m_TimingNowMs < observed->second || m_TimingNowMs - observed->second > freshnessMs) return std::nullopt;
			const double transit = measured ? wire->GetPeerPingMs(route) / 2.0 : 0.0;
			const uint64_t elapsed = static_cast<uint64_t>((m_TimingNowMs - observed->second + transit) / m_Config.simTickMs);
			return applied->second > next || elapsed > next - std::min(applied->second, next);
		};
		// A future return certificate does not end private replay before its activation tick.
		if (m_PeerTailThrough && *m_LastCompletedSimulationTick < *m_PeerTailThrough && IsLocalPrivateReaderAt(next)) return true;
		// Neutral ticks can close the remaining image lag, but cannot put the returner ahead of every survivor. Spending
		// the entire neutral interval at 3x after alignment steals other senders' receipt runway on the first human tick.
		if (const auto back = m_ReclaimTransactions.find(m_Config.localPeerId); back != m_ReclaimTransactions.end() &&
		    next >= back->second.activationFrame && next <= back->second.neutralThroughFrame) {
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer)
				if (peer != m_Config.localPeerId && (PeerFrameMembersAt(next) & SeatBit(peer)) != 0 && playingClockAhead(peer).value_or(false)) return true;
			return false;
		}
		// An extrapolated display clock is a phase goal, not receipt credit. An extra tick must leave the first outstanding
		// own target enough ordinary tick slots for its full trip to every playing reader. Otherwise catching up spends the
		// delay queue faster than production/receipts can refill it and invents a missing peer on a healthy steady link.
		if (requireReceiptRunway) {
			// An extra grant preserves fractional ordinary time until the caller's next slot.
			const double ordinaryWaitMs = std::isfinite(nextOrdinaryTickInMs) ? std::clamp(nextOrdinaryTickInMs, 0.0, m_Config.simTickMs) : 0;
			const auto receipts = m_PeerInputReceipts.find(m_Config.localPeerId);
			for (uint8_t reader = 1; reader <= m_Config.peerCount; ++reader) {
				if (reader == m_Config.localPeerId || (PeerFrameMembersAt(next) & SeatBit(reader)) == 0) continue;
				uint64_t outstanding = EffectiveStartOf(m_Config.localPeerId);
				if (receipts != m_PeerInputReceipts.end()) if (const auto through = receipts->second.find(reader); through != receipts->second.end())
					outstanding = std::max(outstanding, through->second);
				if (const auto back = m_ReclaimTransactions.find(m_Config.localPeerId); back != m_ReclaimTransactions.end())
					outstanding = std::max(outstanding, back->second.neutralThroughFrame + 1);
				if (outstanding <= next) return false;
				const auto [wire, route] = PeerFrameRoute(reader);
				if (!wire || !wire->IsPeerPingMeasured(route)) return false;
				const auto sent = m_PeerInputSentAtMs.find(outstanding);
				const uint64_t sentAt = sent == m_PeerInputSentAtMs.end() ? m_TimingNowMs : sent->second;
				const double receiptAt = static_cast<double>(sentAt) + wire->GetPeerPingMs(route);
				const double consumeAt = static_cast<double>(m_TimingNowMs) + ordinaryWaitMs + (outstanding - next - 1) * m_Config.simTickMs;
				if (consumeAt < receiptAt) return false;
			}
		}
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
			if (reference == m_Config.localPeerId) {
				// A stall of its own leaves members playing past it, though: it catches up to its own clock while one shows ahead by more
				// than that member's jitter, or every member that follows it feels its machine.
				if (!m_PeerPaceStartMs || m_TimingNowMs < *m_PeerPaceStartMs ||
				    next >= m_PeerPaceStartFrame + static_cast<uint64_t>((m_TimingNowMs - *m_PeerPaceStartMs) / m_Config.simTickMs)) return false;
				bool ahead = false;
				for (uint32_t members = m_FrameGroupMembers & ~SeatBit(m_Config.localPeerId); members; members &= members - 1) {
					const auto peer = static_cast<uint8_t>(std::countr_zero(members) + 1);
					const auto applied = m_PeerAppliedThrough.find(peer), observed = m_PeerAppliedAtMs.find(peer);
					const auto stats = m_Stats.peers.find(peer);
					const auto [wire, route] = PeerFrameRoute(peer);
					if (applied == m_PeerAppliedThrough.end() || observed == m_PeerAppliedAtMs.end() || m_TimingNowMs < observed->second ||
					    !wire || !wire->IsPeerPingMeasured(route)) continue;
					// Only a member still reporting every tick shows where it plays now: a paused one's last report says nothing of the pause.
					const double jitterMs = stats == m_Stats.peers.end() ? 0.0 : stats->second.jitterMs;
					if (m_TimingNowMs - observed->second > 2 * m_Config.simTickMs + jitterMs) continue;
					const double transit = wire->GetPeerPingMs(route) / 2.0 / m_Config.simTickMs;
					const double shown = applied->second + (m_TimingNowMs - observed->second) / m_Config.simTickMs + transit;
					// A stall past this seat's delay leaves a member waiting on its input: the bound holds this seat, and a burst of late
					// input would reach some members in time and others not.
					if (static_cast<double>(m_LastQueuedTargetFrame) < shown + transit) return false;
					ahead |= shown > next + std::ceil(jitterMs / m_Config.simTickMs) + 1;
				}
				return ahead;
			}
			if (const auto ahead = playingClockAhead(reference)) return *ahead;
		}
		if (!m_PeerHadHitch || !m_PeerPaceStartMs || m_TimingNowMs < *m_PeerPaceStartMs || m_Config.simTickMs <= 0) return false;
		const uint64_t due = m_PeerPaceStartFrame + static_cast<uint64_t>((m_TimingNowMs - *m_PeerPaceStartMs) / m_Config.simTickMs);
		return *m_LastCompletedSimulationTick + 1 < due;
	}

	uint64_t NetLockstepCoordinator::PeerReturnPipeline(uint8_t peer, NetPeerId transport, uint64_t frame, uint32_t* outDelay, uint64_t* outTransit) const {
		const uint32_t delay = m_Config.adaptiveInputDelay ? std::min<uint16_t>(NetLockstepCodec::c_MaxPeerInputDelayFrames,
		    std::max<uint16_t>(InputDelayAt(peer, frame), ReturnDelayFloor(peer, transport))) : PeerInputDelay(peer);
		const auto link = m_Stats.peers.find(peer);
		const uint64_t transit = link == m_Stats.peers.end() ? 0 : static_cast<uint64_t>(std::ceil((link->second.pingMs + link->second.jitterMs) / m_Config.simTickMs));
		if (outDelay) *outDelay = delay;
		if (outTransit) *outTransit = transit;
		return delay + transit + m_Config.slowPlayerBoundTicks;
	}

	uint64_t NetLockstepCoordinator::PeerReturnTrailRoom(uint8_t peerId, NetPeerId transport, uint64_t frame) const {
		NET_PLANE_CHECK();
		// The ordinary activation carries any trail in its neutral frame; only a frame-group return names its interval on the wire.
		if (!UsesPeerFrameGroups()) return UINT64_MAX;
		const uint64_t pipeline = PeerReturnPipeline(peerId, transport, frame);
		return pipeline >= NetLockstepCodec::c_MaxFutureFrameSkew ? 0 : NetLockstepCodec::c_MaxFutureFrameSkew - pipeline;
	}

	uint8_t NetLockstepCoordinator::PeerReturnLeader() const {
		NET_PLANE_CHECK();
		// The first playing survivor serves the private return even while the administrative host is held. A seat in its own
		// return's neutral interval votes on nothing and may already be reading privately again, so it serves no return.
		for (uint32_t members = m_FrameGroupMembers; members; members &= members - 1) {
			const uint8_t member = static_cast<uint8_t>(std::countr_zero(members) + 1);
			if (!IsSeatReclaimGap(member, m_Stats.nextFrame)) return member;
		}
		return m_FrameGroupMembers ? static_cast<uint8_t>(std::countr_zero(m_FrameGroupMembers) + 1) : 0;
	}

	bool NetLockstepCoordinator::ProposePeerReclaim(uint8_t peer, NetPeerId transport, uint32_t incarnation, uint64_t frame, uint64_t trailFrames, std::string* error) {
		if (!IsRunning() || m_Config.localPeerId != PeerReturnLeader() || peer == 0 || peer > m_Config.peerCount ||
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
		const uint32_t required = PeerDecisionRequiredVoters(proposal);
		for (uint8_t voter = 1; voter <= m_Config.peerCount; ++voter) {
			if (voter == m_Config.localPeerId || (required & SeatBit(voter)) == 0) continue;
			const auto applied = m_PeerAppliedThrough.find(voter);
			if (applied == m_PeerAppliedThrough.end() || applied->second < m_Config.startFrame) { if (error) *error = "the private return awaits a playing voter's common prefix"; return false; }
			proposal.boundary = std::min(proposal.boundary, applied->second);
		}
		const auto prefix = PeerAppliedFramePrefix(proposal.boundary); proposal.bytes.assign(prefix.begin(), prefix.end());
		uint32_t delay = 0; uint64_t transit = 0;
		const uint64_t neutral = PeerReturnPipeline(peer, transport, frame, &delay, &transit) + trailFrames;
		if (neutral > NetLockstepCodec::c_MaxFutureFrameSkew) {
			if (error) *error = "the private return is still too far behind its activation: delay=" + std::to_string(delay) + " transit=" + std::to_string(transit) + " trail=" + std::to_string(trailFrames);
			return false;
		}
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
