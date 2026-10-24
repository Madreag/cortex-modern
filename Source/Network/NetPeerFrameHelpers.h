#pragma once

#include "NetLockstep.h"

#include <cstdlib>

namespace RTE::NetPeerFrameDetail {
		// Read once: every getenv takes the C runtime's environment lock and scans the whole environment, and this is asked per message.
		inline bool ReturnInputDetails() {
			static const bool details = std::getenv("CC_TEST_RETURN_INPUT_DETAILS") != nullptr;
			return details;
		}
		// Test lever CC_TEST_PEER_INPUT_WIRE: what the peer input path sends, how its receipts come back and when one is owed.
		inline bool PeerInputWireDetails() {
			static const bool details = std::getenv("CC_TEST_PEER_INPUT_WIRE") != nullptr;
			return details;
		}
		inline uint32_t SeatBit(uint8_t peer) { return peer > 0 && peer <= 32 ? 1U << (peer - 1) : 0; }
		inline bool IsHoldDecision(uint8_t kind) { return (kind & 0xF0) == 0x10; }
		inline uint8_t DecisionKind(const NetHostMigrationMessage& message) {
			if (message.preparedFrame == 0) {
				// Independent owners can promise the same cutoff without sharing a decision slot.
				static_assert(NetMatchConfigUtil::c_MaxPeerCount == 4);
				uint8_t kind = 0x10;
				for (uint8_t peer: message.members) kind |= static_cast<uint8_t>(SeatBit(peer));
				return kind;
			}
			return message.preparedFrame == 2 && !message.members.empty() ? static_cast<uint8_t>(8 + message.members.front()) : static_cast<uint8_t>(message.preparedFrame);
		}
		template<class Decisions> bool HasHoldDecision(const Decisions& decisions, uint64_t frame, uint8_t owner = 0) {
			for (auto found = decisions.lower_bound({frame, 0x10}); found != decisions.end() && found->first.first == frame && IsHoldDecision(found->first.second); ++found)
				if (owner == 0 || (found->first.second & SeatBit(owner)) != 0) return true;
			return false;
		}
		template<class Votes, class Certificates> bool OwnHoldAwaitingCertificate(const Votes& votes, const Certificates& certificates, uint64_t frame, uint8_t owner) {
			for (auto found = votes.lower_bound({frame, 0x10}); found != votes.end() && found->first.first == frame && IsHoldDecision(found->first.second); ++found)
				if ((found->first.second & SeatBit(owner)) != 0 && !certificates.contains(found->first))
					if (const auto own = found->second.find(owner); own != found->second.end() && own->second.voterMask == SeatBit(owner)) return true;
			return false;
		}
		inline void PutSize(std::vector<uint8_t>& bytes, uint32_t size) {
			for (unsigned int shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<uint8_t>(size >> shift));
		}
		inline bool TakeSize(const std::vector<uint8_t>& bytes, size_t& offset, uint32_t& size) {
			if (offset > bytes.size() || bytes.size() - offset < 4) return false;
			size = 0;
			for (unsigned int shift = 0; shift < 32; shift += 8) size |= static_cast<uint32_t>(bytes[offset++]) << shift;
			return true;
		}
		inline void PutFrame(std::vector<uint8_t>& bytes, uint64_t frame) {
			for (unsigned int shift = 0; shift < 64; shift += 8) bytes.push_back(static_cast<uint8_t>(frame >> shift));
		}
		inline bool TakeFrame(const std::vector<uint8_t>& bytes, size_t& offset, uint64_t& frame) {
			if (offset > bytes.size() || bytes.size() - offset < 8) return false;
			frame = 0;
			for (unsigned int shift = 0; shift < 64; shift += 8) frame |= static_cast<uint64_t>(bytes[offset++]) << shift;
			return true;
		}
		inline bool SameBridge(const NetHostMigrationMessage& a, const NetHostMigrationMessage& b) {
			return a.sessionId == b.sessionId && a.roundId == b.roundId && a.generation == b.generation && a.frame == b.frame &&
			       a.connectedMask == b.connectedMask && a.preparedFrame == b.preparedFrame && a.boundary == b.boundary && a.completeFrom == b.completeFrom &&
			       a.successorPeerId == b.successorPeerId && a.members == b.members && a.bytes == b.bytes;
		}
		// A return from a common prefix before an agreed departure cannot restore that owner,
		// including when a later admission has already restored ownership at its activation.
		inline bool ReturnCrossesDeparture(const NetHostMigrationMessage& proposal, const std::map<uint8_t, std::map<uint64_t, bool>>& ownership) {
			for (uint8_t peer: proposal.members) if (const auto history = ownership.find(peer); history != ownership.end()) {
				for (auto change = history->second.upper_bound(proposal.boundary); change != history->second.end() && change->first <= proposal.frame; ++change)
					if (!change->second) return true;
			}
			return false;
		}
		inline std::vector<ControllerFrame> FramesForSeat(const NetLockstepReadyFrame& ready, uint8_t peer) {
			if (peer == ready.localPeerId) return ready.localFrames;
			size_t offset = 0;
			for (const auto& [sender, count]: ready.remoteFrameCounts) {
				if (offset > ready.remoteFrames.size() || count > ready.remoteFrames.size() - offset) return {};
				if (sender == peer) return {ready.remoteFrames.begin() + offset, ready.remoteFrames.begin() + offset + count};
				offset += count;
			}
			return {};
		}
		inline std::vector<ControllerFrame> ContinuedInput(const std::vector<ControllerFrame>& last, uint64_t elapsedMs) {
			std::vector<ControllerFrame> frames = last;
			uint64_t repeat = 0;
			for (ControlState state: {MOVE_IDLE, MOVE_RIGHT, MOVE_LEFT, MOVE_UP, MOVE_DOWN, MOVE_FAST, BODY_CROUCH, BODY_PRONE,
			                         AIM_UP, AIM_DOWN, AIM_SHARP, HOLD_RIGHT, HOLD_LEFT, HOLD_UP, HOLD_DOWN}) repeat |= 1ULL << state;
			if (elapsedMs < c_NetHeldActionReleaseMs)
				for (ControlState state: {PRIMARY_ACTION, SECONDARY_ACTION, WEAPON_FIRE, BODY_JUMP}) repeat |= 1ULL << state;
			for (auto& frame: frames) {
				frame.stateMask &= repeat;
				frame.mouseDeltaX = frame.mouseDeltaY = 0;
				frame.analogCursorX = frame.analogCursorY = 0;
				frame.SetFlipIntent(false);
				frame.hatchCommand = static_cast<uint8_t>(ControllerFrame::HatchCommand::None);
				frame.mouseButtons = elapsedMs < c_NetHeldActionReleaseMs ? frame.mouseButtons & 0x7U : 0;
			}
			return frames;
		}

} // namespace RTE::NetPeerFrameDetail
