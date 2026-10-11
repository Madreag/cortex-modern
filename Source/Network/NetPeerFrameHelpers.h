#pragma once

#include "NetLockstep.h"

namespace RTE::NetPeerFrameDetail {
		inline uint8_t DecisionKind(const NetHostMigrationMessage& message) { return message.preparedFrame == 2 && !message.members.empty() ? static_cast<uint8_t>(8 + message.members.front()) : static_cast<uint8_t>(message.preparedFrame); }
		inline uint32_t SeatBit(uint8_t peer) { return peer > 0 && peer <= 32 ? 1U << (peer - 1) : 0; }
		inline void PutSize(std::vector<uint8_t>& bytes, uint32_t size) {
			for (unsigned int shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<uint8_t>(size >> shift));
		}
		inline bool TakeSize(const std::vector<uint8_t>& bytes, size_t& offset, uint32_t& size) {
			if (offset > bytes.size() || bytes.size() - offset < 4) return false;
			size = 0;
			for (unsigned int shift = 0; shift < 32; shift += 8) size |= static_cast<uint32_t>(bytes[offset++]) << shift;
			return true;
		}
		inline bool SameBridge(const NetHostMigrationMessage& a, const NetHostMigrationMessage& b) {
			return a.sessionId == b.sessionId && a.roundId == b.roundId && a.generation == b.generation && a.frame == b.frame &&
			       a.connectedMask == b.connectedMask && a.preparedFrame == b.preparedFrame && a.boundary == b.boundary && a.completeFrom == b.completeFrom &&
			       a.successorPeerId == b.successorPeerId && a.members == b.members && a.bytes == b.bytes;
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
