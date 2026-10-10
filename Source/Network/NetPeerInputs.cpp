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
			SendPeerFrameMessage(std::move(message), NetTransportLane::ControlReliable, onlyPeer);
		}
	}

	void NetLockstepCoordinator::SendPeerInput(const NetLockstepFrame& frame) {
		if (!UsesPeerFrameGroups()) return;
		std::vector<std::vector<uint8_t>> inputs;
		NetLockstepFrame current = frame;
		current.priorWindow.clear();
		std::vector<uint8_t> encoded;
		if (!NetLockstepCodec::EncodeRecoveryInput(current, encoded)) return;
		size_t total = 5 + encoded.size();
		inputs.push_back(std::move(encoded));
		for (uint32_t back = 1; back < ConfiguredWindowTicks() && frame.targetFrame >= back; ++back) {
			NetLockstepFrame prior;
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
		for (size_t offset = 0; offset < body.size(); offset += NetHostMigrationCodec::c_ChunkBytes) {
			auto message = PeerFrameMessage(NetHostMigrationMessageType::PeerInput);
			message.frame = frame.targetFrame;
			message.totalBytes = static_cast<uint32_t>(body.size());
			message.offset = static_cast<uint32_t>(offset);
			const size_t end = std::min(body.size(), offset + NetHostMigrationCodec::c_ChunkBytes);
			message.bytes.assign(body.begin() + offset, body.begin() + end);
			SendPeerFrameMessage(std::move(message), body.size() > NetHostMigrationCodec::c_ChunkBytes ? NetTransportLane::ControlReliable : m_Config.frameLane);
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
		SendPeerFrameMessage(std::move(receipt));
	}

	bool NetLockstepCoordinator::PeerInputAccepted(uint64_t frame, uint8_t owner) const {
		if (owner == 0) owner = m_Config.localPeerId;
		if (frame < EffectiveStartOf(owner) || IsSeatReclaimGap(owner, frame)) return true;
		if (const auto bridge = m_PeerBridges.find(owner); bridge != m_PeerBridges.end() &&
		    frame >= bridge->second.fromFrame && (!bridge->second.returnFrame || frame < *bridge->second.returnFrame))
			return owner != m_Config.localPeerId || m_PeerCommittedTail.contains(frame);
		uint32_t accepted = SeatBit(owner);
		if (const auto receipts = m_PeerInputReceipts.find(owner); receipts != m_PeerInputReceipts.end())
			for (const auto& [peer, through]: receipts->second) if (through > frame) accepted |= SeatBit(peer);
		return PeerGroupHasAuthority(accepted);
	}

	bool NetLockstepCoordinator::HandlePeerFrameMessage(const NetHostMigrationMessage& message, uint64_t nowMs) {
		if (HandlePeerSessionMessage(message, nowMs)) return true;
		if (!UsesPeerFrameGroups() || message.type < NetHostMigrationMessageType::PeerInput ||
		    (message.type > NetHostMigrationMessageType::PeerControl && message.type != NetHostMigrationMessageType::PeerForwardInput)) return false;
		if (PeerFrameBlackout(nowMs)) return true;
		if (message.sessionId != m_Config.sessionId || message.roundId != m_Config.matchConfig.roundId || message.configHash != m_RoundConfigHash ||
		    message.senderPeerId == 0 || message.senderPeerId == m_Config.localPeerId || message.senderPeerId > m_Config.peerCount || message.generation != 1 || m_RemovedPeers.contains(message.senderPeerId)) return true;
		const auto heard = [&] {
			m_PeerLastHeardMs[message.senderPeerId] = nowMs;
			m_PeerLinkHeardMs[message.senderPeerId] = nowMs;
			m_Stats.peers[message.senderPeerId].lastHeardMs = nowMs;
			m_Stats.peers[m_Config.localPeerId].lastHeardMs = nowMs;
			auto& applied = m_PeerAppliedThrough[message.senderPeerId];
			if (!m_PeerAppliedAtMs.contains(message.senderPeerId) || message.appliedFrame > applied) {
				applied = std::max(applied, message.appliedFrame);
				m_PeerAppliedAtMs[message.senderPeerId] = nowMs;
			}
			if (message.senderPeerId == GetHostPeerId()) NoteAuthorityHeard(nowMs);
		};
		if (message.type == NetHostMigrationMessageType::PeerReceipt) {
			if (message.successorPeerId == message.senderPeerId && message.preparedFrame == UINT64_MAX && message.bytes.size() == 2 * NetHash32{}.size() && message.appliedFrame == message.frame &&
			    message.frame >= m_Config.startFrame && message.frame <= m_Stats.nextFrame + NetLockstepCodec::c_MaxFutureFrameSkew &&
			    (message.voterMask & SeatBit(message.senderPeerId)) != 0 && (message.voterMask >> m_Config.peerCount) == 0) {
				heard();
				m_PeerFrameWitnesses[message.frame].try_emplace(message.senderPeerId, message);
				return true;
			}
			if (message.successorPeerId > 0 && message.successorPeerId <= m_Config.peerCount && message.bytes.empty() &&
			    message.frame >= m_Config.startFrame && message.frame <= m_Stats.nextFrame + NetLockstepCodec::c_MaxFutureFrameSkew + 1) {
				heard();
				if (message.successorPeerId == m_Config.localPeerId && message.frame > 0) if (const auto sent = m_PeerInputSentAtMs.find(message.frame - 1); sent != m_PeerInputSentAtMs.end() && nowMs >= sent->second)
				{
					const auto group = m_FrameGroupChanges.upper_bound(message.frame - 1);
					const uint32_t members = group == m_FrameGroupChanges.begin() ? (1U << m_Config.peerCount) - 1 : std::prev(group)->second;
					const auto bridge = m_PeerBridges.find(message.senderPeerId);
					const bool live = bridge == m_PeerBridges.end() || (bridge->second.returnFrame && message.appliedFrame >= *bridge->second.returnFrame);
					// A private tail reader's delayed receipt measures its catch-up,
					// not the live sender's transport round trip.
					if (live && (members & m_FrameGroupMembers & SeatBit(message.senderPeerId)) != 0) {
						const auto delay = static_cast<uint32_t>(std::min<uint64_t>(nowMs - sent->second, UINT32_MAX));
						m_PeerReceiptDelaySamples[message.senderPeerId].Observe(nowMs, delay);
						m_PeerArrivalLatencyMs[m_Config.localPeerId] = {nowMs, delay};
					}
				}
				m_PeerInputReceipts[message.successorPeerId][message.senderPeerId] = std::max(m_PeerInputReceipts[message.successorPeerId][message.senderPeerId], message.frame);
				ResolvePeerBridgeInputConflicts(nowMs);
			}
			return true;
		}
		if (message.type == NetHostMigrationMessageType::PeerBridge) {
			if (message.preparedFrame == 0 && message.frame > m_Stats.nextFrame && message.frame - m_Stats.nextFrame <= NetLockstepCodec::c_MaxFutureFrameSkew) {
				m_PeerBridgeVotes[{message.frame, DecisionKind(message)}].try_emplace(message.senderPeerId, message);
				return true;
			}
			if (!ValidatePeerBridge(message)) return true;
			heard();
			auto& votes = m_PeerBridgeVotes[{message.frame, DecisionKind(message)}];
			const auto own = votes.find(m_Config.localPeerId);
			if (own != votes.end() && !SameBridge(own->second, message)) return true;
			votes.try_emplace(message.senderPeerId, message);
			if (own == votes.end()) {
				auto reply = message;
				reply.senderPeerId = m_Config.localPeerId;
				reply.appliedFrame = m_LastCompletedSimulationTick.value_or(0);
				reply.voterMask = SeatBit(m_Config.localPeerId);
				if (reply.preparedFrame == 3) m_PeerAdminOwnVote = reply;
				votes.emplace(m_Config.localPeerId, reply);
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
			if (message.bytes.empty()) { heard(); SendPeerCommittedTail(message.senderPeerId, message.frame); }
			return true;
		}
		if (message.type == NetHostMigrationMessageType::PeerTailMissing) {
			const bool longHitch = m_ConsumerWaitingFrame && nowMs >= m_ConsumerWaitStartMs && nowMs - m_ConsumerWaitStartMs >= c_NetSeatDisconnectSilenceMs;
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
			m_Stats.peers[message.senderPeerId].pingMeasured = rtt != 0;
			if (PeerGroupHasAuthority(message.connectedMask) && message.appliedFrame >= m_Stats.nextFrame)
				RequestPeerCommittedTail(nowMs);
			return true;
		}
		if (message.type == NetHostMigrationMessageType::PeerControl) {
			const auto decoded = NetLockstepCodec::Decode(message.bytes, ControllerFrame::c_Version);
			if (decoded.ok && std::visit([&](const auto& payload) {
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
		const size_t byteCap = inputMessage ? NetLockstepCodec::c_InputWindowByteCap : NetHostMigrationCodec::c_MaxPeerTailBytes;
		if (message.totalBytes == 0 || message.totalBytes > byteCap || message.offset > message.totalBytes ||
		    message.bytes.size() > message.totalBytes - message.offset) return true;
		if (message.frame < m_Stats.nextFrame || message.frame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew ||
		    (message.type == NetHostMigrationMessageType::PeerInput && message.totalBytes > NetLockstepCodec::c_InputWindowByteCap)) return true;
		if (!m_PeerFrameIncoming.contains(key) && m_PeerFrameIncoming.size() >= 2 * m_Config.peerCount) return true;
		auto& incoming = m_PeerFrameIncoming[key];
		incoming.lastReceivedMs = nowMs;
		if (message.offset == 0) { incoming.totalBytes = message.totalBytes; incoming.bytes.clear(); }
		if (incoming.totalBytes != message.totalBytes || message.offset != incoming.bytes.size()) { m_PeerFrameIncoming.erase(key); return true; }
		incoming.bytes.insert(incoming.bytes.end(), message.bytes.begin(), message.bytes.end());
		if (incoming.bytes.size() != incoming.totalBytes) return true;
		std::vector<uint8_t> body = std::move(incoming.bytes); m_PeerFrameIncoming.erase(key);
		if (message.type == NetHostMigrationMessageType::PeerTail) {
			if ((message.voterMask & SeatBit(message.senderPeerId)) == 0 || message.appliedFrame < message.frame || body.size() < 70) return true;
			NetHash32 prefix{}, prepared{}; std::copy_n(body.begin(), prefix.size(), prefix.begin());
			std::copy_n(body.begin() + prefix.size(), prepared.size(), prepared.begin());
			size_t at = prefix.size() + prepared.size(); uint32_t size = 0;
			if (!TakeSize(body, at, size) || size > NetHostMigrationCodec::c_MaxFrameBytes || at > body.size() || size > body.size() - at) return true;
			std::vector<uint8_t> record(body.begin() + at, body.begin() + at + size); at += size;
			NetLockstepReadyFrame decoded;
			if (!DecodeMigrationFrame(record, message.frame, decoded) || at == body.size()) return true;
			const uint8_t count = body[at++]; if (count > 32) return true;
			std::vector<NetHostMigrationMessage> proofs;
			for (uint8_t n = 0; n < count; ++n) {
				if (!TakeSize(body, at, size) || at > body.size() || size > body.size() - at) return true;
				NetHostMigrationMessage proof, agreed; uint32_t voters = 0;
				if (!NetHostMigrationCodec::Decode({body.begin() + at, body.begin() + at + size}, m_Config.migrationKey, proof) ||
				    proof.type != NetHostMigrationMessageType::PeerBridgeCommit || !DecodePeerBridgeCertificate(proof, agreed, voters) ||
				    (proof.frame > message.frame && agreed.preparedFrame != 3)) return true;
				if (agreed.preparedFrame == 3 && agreed.completeFrom > m_Config.migrationGeneration) {
					NoteSuperseded(agreed.completeFrom);
					if (PeerAdminCertificateValid(agreed, voters) && 2 * std::popcount(voters) >= static_cast<int>(m_Config.peerCount)) m_PeerAdminFrameMembers = voters;
				} else if (agreed.connectedMask == message.voterMask && !PeerGroupHasAuthority(voters)) CheckPeerFrameTie(agreed.frame, voters, nowMs);
				proofs.push_back(std::move(proof)); at += size;
			}
			if (at == body.size()) return true;
			const uint8_t witnessCount = body[at++];
			if (witnessCount > m_Config.peerCount) return true;
			uint8_t digest[32]{};
			if (!GetNetAuthCrypto().HmacSha256(m_Config.migrationKey.data(), m_Config.migrationKey.size(), record.data(), record.size(), digest)) return true;
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
				    !std::equal(digest, digest + 32, witness.bytes.begin()) || !std::equal(prepared.begin(), prepared.end(), witness.bytes.begin() + 32)) return true;
				witnessed |= SeatBit(witness.senderPeerId); at += size;
			}
			if (at != body.size()) return true;
			heard();
			if (!PeerGroupHasAuthority(message.voterMask)) return true;
			auto& votes = m_PeerTailVotes[message.frame];
			votes.try_emplace(message.senderPeerId, PeerTailVote{message.voterMask, prefix, prepared, record});
			uint32_t agreeing = witnessed;
			for (const auto& [peer, vote]: votes) if (vote.members == message.voterMask && vote.prefix == prefix && vote.prepared == prepared && vote.bytes == record) agreeing |= SeatBit(peer);
			if ((message.voterMask & SeatBit(m_Config.localPeerId)) != 0) {
				const auto own = m_PeerSourceInputs.find(message.frame);
				if (own != m_PeerSourceInputs.end() && own->second.contains(m_Config.localPeerId)) {
					auto input = own->second.at(m_Config.localPeerId), retained = input;
					input.commands.clear(); input.observations.clear(); input.valueObservations.clear(); input.priorWindow.clear();
					retained = input; retained.frames = FramesForSeat(decoded, m_Config.localPeerId);
					std::vector<uint8_t> a, b;
					if (NetLockstepCodec::EncodeRecoveryInput(input, a) && NetLockstepCodec::EncodeRecoveryInput(retained, b) && a == b) agreeing |= SeatBit(m_Config.localPeerId);
				}
			}
			// An authenticated donor cannot commit a minority's tail by claiming somebody else's votes.
			if ((agreeing & ~message.voterMask) != 0 || 2 * std::popcount(agreeing) <= std::popcount(message.voterMask)) return true;
			m_PeerCommittedTail.try_emplace(message.frame, std::move(record));
			m_PeerPreparedPrefixes.try_emplace(message.frame, prepared);
			m_PeerTailPrefixes.try_emplace(message.frame, prefix); m_PeerTailDecisions.try_emplace(message.frame, std::move(proofs));
			m_PeerTailThrough = std::max(m_PeerTailThrough.value_or(0), message.appliedFrame);
			m_FrameGroupMembers = message.voterMask;
			if ((message.voterMask & SeatBit(m_Config.localPeerId)) == 0 && !m_PeerBridges.contains(m_Config.localPeerId)) {
				PeerBridge own;
				own.fromFrame = m_Stats.nextFrame;
				own.aiFrame = own.fromFrame + static_cast<uint64_t>(std::ceil(c_NetInputContinuityMs / m_Config.simTickMs));
				own.members = message.voterMask;
				m_PeerBridges.emplace(m_Config.localPeerId, std::move(own));
				m_SeatBridgeSinceMs[m_Config.localPeerId] = nowMs;
			}
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
		std::vector<NetLockstepFrame> inputs;
		for (uint8_t n = 0; n < count; ++n) {
			uint32_t size = 0;
			if (!TakeSize(body, offset, size) || offset > body.size() || size > body.size() - offset) return true;
			NetLockstepFrame input;
			if (!NetLockstepCodec::DecodeRecoveryInput({body.begin() + offset, body.begin() + offset + size}, input) || input.senderPeerId != owner ||
			    input.roundId != m_RoundId || input.targetFrame > message.frame || (n > 0 && input.targetFrame <= inputs.back().targetFrame)) return true;
			inputs.push_back(std::move(input)); offset += size;
		}
		if (offset != body.size() || inputs.back().targetFrame != message.frame) return true;
		heard();
		if (owner == m_Config.localPeerId) return true;
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
				SendPeerFrameMessage(std::move(forwarded), wireBody.size() > NetHostMigrationCodec::c_ChunkBytes ? NetTransportLane::ControlReliable : m_Config.frameLane);
			}
		}
		for (const auto& input: inputs) {
			if (input.targetFrame < m_Stats.nextFrame || input.targetFrame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew) continue;
			if ((m_PeerRejectedInputs[input.targetFrame] & SeatBit(input.senderPeerId)) != 0) {
				if (!m_PeerBridgeCertificates.contains({input.targetFrame, 0}))
					m_PeerPendingBridgeInputs[input.targetFrame].try_emplace(input.senderPeerId, input);
				continue;
			}
			const auto bridge = m_PeerBridges.find(input.senderPeerId);
			if (bridge != m_PeerBridges.end() && (!bridge->second.returnFrame || input.targetFrame < *bridge->second.returnFrame))
				m_PeerSourceInputs[input.targetFrame].try_emplace(input.senderPeerId, input);
			else AcceptRemoteTick(input, nowMs, false);
		}
		return true;
	}

} // namespace RTE
