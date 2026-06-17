#include "ControllerFrame.h"

#include "TimerMan.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

namespace RTE {

	namespace {
		void SetError(std::string* error, const std::string& message) {
			if (error) {
				*error = message;
			}
		}

		void AppendU8(std::vector<uint8_t>& out, uint8_t value) {
			out.push_back(value);
		}

		void AppendI8(std::vector<uint8_t>& out, int8_t value) {
			out.push_back(static_cast<uint8_t>(value));
		}

		void AppendU16LE(std::vector<uint8_t>& out, uint16_t value) {
			out.push_back(static_cast<uint8_t>(value & 0xFFU));
			out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
		}

		void AppendI16LE(std::vector<uint8_t>& out, int16_t value) {
			AppendU16LE(out, static_cast<uint16_t>(value));
		}

		void AppendU64LE(std::vector<uint8_t>& out, uint64_t value) {
			for (int i = 0; i < 8; ++i) {
				out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFFU));
			}
		}

		void AppendI64LE(std::vector<uint8_t>& out, int64_t value) {
			AppendU64LE(out, static_cast<uint64_t>(value));
		}

		uint8_t ReadU8(const uint8_t*& p) {
			return *p++;
		}

		int8_t ReadI8(const uint8_t*& p) {
			return static_cast<int8_t>(*p++);
		}

		uint16_t ReadU16LE(const uint8_t*& p) {
			uint16_t value = static_cast<uint16_t>(p[0]) |
			                 static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
			p += 2;
			return value;
		}

		int16_t ReadI16LE(const uint8_t*& p) {
			return static_cast<int16_t>(ReadU16LE(p));
		}

		uint64_t ReadU64LE(const uint8_t*& p) {
			uint64_t value = 0;
			for (int i = 0; i < 8; ++i) {
				value |= static_cast<uint64_t>(p[i]) << (i * 8);
			}
			p += 8;
			return value;
		}

		int64_t ReadI64LE(const uint8_t*& p) {
			return static_cast<int64_t>(ReadU64LE(p));
		}

		bool NearlyEqual(float lhs, float rhs, float epsilon = 1.0F / static_cast<float>(ControllerFrame::c_AnalogScale)) {
			return std::fabs(lhs - rhs) <= epsilon;
		}

		bool HasUnknownControlStateBits(uint64_t stateMask) {
			if constexpr (ControlState::CONTROLSTATECOUNT >= 64) {
				return false;
			} else {
				return (stateMask >> ControlState::CONTROLSTATECOUNT) != 0;
			}
		}
	}

	void ControllerFrame::SetQuickDisabled(bool disabled) {
		if (disabled) {
			flags |= 0x1U;
		} else {
			flags &= static_cast<uint8_t>(~0x1U);
		}
	}

	ControllerFrame ControllerFrameCodec::Snapshot(int64_t actorUniqueID, const Controller& controller) {
		static_assert(ControlState::CONTROLSTATECOUNT <= 64, "ControllerFrame state_mask must grow if ControlState exceeds 64 entries.");

		ControllerFrame frame;
		frame.actorUniqueID = actorUniqueID;
		for (int state = 0; state < ControlState::CONTROLSTATECOUNT; ++state) {
			if (controller.IsState(static_cast<ControlState>(state))) {
				frame.stateMask |= (uint64_t{1} << state);
			}
		}

		const Vector move = controller.GetAnalogMove();
		const Vector aim = controller.GetAnalogAim();
		const Vector cursor = controller.GetAnalogCursor();
		const Vector mouse = controller.GetMouseMovement();
		frame.analogMoveX = QuantizeAnalog(move.m_X);
		frame.analogMoveY = QuantizeAnalog(move.m_Y);
		frame.analogAimX = QuantizeAnalog(aim.m_X);
		frame.analogAimY = QuantizeAnalog(aim.m_Y);
		frame.analogCursorX = QuantizeAnalog(cursor.m_X);
		frame.analogCursorY = QuantizeAnalog(cursor.m_Y);
		frame.mouseDeltaX = QuantizeMouseDelta(mouse.m_X);
		frame.mouseDeltaY = QuantizeMouseDelta(mouse.m_Y);
		frame.inputMode = static_cast<uint8_t>(controller.GetInputMode());
		frame.playerRaw = static_cast<int8_t>(std::clamp(controller.GetPlayerRaw(), -128, 127));
		frame.SetQuickDisabled(controller.IsQuickDisabled());
		return frame;
	}

	bool ControllerFrameCodec::Apply(const ControllerFrame& frame, Controller& controller, std::string* error) {
		if (frame.inputMode >= static_cast<uint8_t>(Controller::CIM_INPUTMODECOUNT)) {
			SetError(error, "ControllerFrame input_mode is out of range.");
			return false;
		}
		if (HasUnknownControlStateBits(frame.stateMask)) {
			SetError(error, "ControllerFrame has state bits beyond CONTROLSTATECOUNT.");
			return false;
		}
		if ((frame.flags & static_cast<uint8_t>(~0x1U)) != 0) {
			SetError(error, "ControllerFrame reserved flags must be zero.");
			return false;
		}

		std::array<bool, ControlState::CONTROLSTATECOUNT> states{};
		for (int state = 0; state < ControlState::CONTROLSTATECOUNT; ++state) {
			states[state] = (frame.stateMask & (uint64_t{1} << state)) != 0;
		}

		controller.ApplyWireState(states,
		                          Vector(DequantizeAnalog(frame.analogMoveX), DequantizeAnalog(frame.analogMoveY)),
		                          Vector(DequantizeAnalog(frame.analogAimX), DequantizeAnalog(frame.analogAimY)),
		                          Vector(DequantizeAnalog(frame.analogCursorX), DequantizeAnalog(frame.analogCursorY)),
		                          Vector(static_cast<float>(frame.mouseDeltaX), static_cast<float>(frame.mouseDeltaY)),
		                          static_cast<Controller::InputMode>(frame.inputMode),
		                          static_cast<int>(frame.playerRaw),
		                          frame.IsQuickDisabled());
		return true;
	}

	std::vector<uint8_t> ControllerFrameCodec::Encode(const ControllerFrame& frame) {
		std::vector<uint8_t> out;
		out.reserve(ControllerFrame::c_EncodedSize);
		AppendI64LE(out, frame.actorUniqueID);
		AppendU64LE(out, frame.stateMask);
		AppendI16LE(out, frame.analogMoveX);
		AppendI16LE(out, frame.analogMoveY);
		AppendI16LE(out, frame.analogAimX);
		AppendI16LE(out, frame.analogAimY);
		AppendI16LE(out, frame.analogCursorX);
		AppendI16LE(out, frame.analogCursorY);
		AppendI16LE(out, frame.mouseDeltaX);
		AppendI16LE(out, frame.mouseDeltaY);
		AppendU8(out, frame.inputMode);
		AppendI8(out, frame.playerRaw);
		AppendU8(out, frame.flags);
		AppendU8(out, 0); // reserved
		return out;
	}

	bool ControllerFrameCodec::Decode(const uint8_t* data, size_t size, ControllerFrame& outFrame, std::string* error) {
		if (!data || size != ControllerFrame::c_EncodedSize) {
			SetError(error, "ControllerFrame encoded size mismatch.");
			return false;
		}
		const uint8_t* p = data;
		ControllerFrame frame;
		frame.actorUniqueID = ReadI64LE(p);
		frame.stateMask = ReadU64LE(p);
		frame.analogMoveX = ReadI16LE(p);
		frame.analogMoveY = ReadI16LE(p);
		frame.analogAimX = ReadI16LE(p);
		frame.analogAimY = ReadI16LE(p);
		frame.analogCursorX = ReadI16LE(p);
		frame.analogCursorY = ReadI16LE(p);
		frame.mouseDeltaX = ReadI16LE(p);
		frame.mouseDeltaY = ReadI16LE(p);
		frame.inputMode = ReadU8(p);
		frame.playerRaw = ReadI8(p);
		frame.flags = ReadU8(p);
		const uint8_t reserved = ReadU8(p);

		if (reserved != 0) {
			SetError(error, "ControllerFrame reserved byte must be zero.");
			return false;
		}
		if (frame.inputMode >= static_cast<uint8_t>(Controller::CIM_INPUTMODECOUNT)) {
			SetError(error, "ControllerFrame input_mode is out of range.");
			return false;
		}
		if (HasUnknownControlStateBits(frame.stateMask)) {
			SetError(error, "ControllerFrame has state bits beyond CONTROLSTATECOUNT.");
			return false;
		}
		if ((frame.flags & static_cast<uint8_t>(~0x1U)) != 0) {
			SetError(error, "ControllerFrame reserved flags must be zero.");
			return false;
		}
		outFrame = frame;
		return true;
	}

	int16_t ControllerFrameCodec::QuantizeAnalog(float value) {
		if (!std::isfinite(value)) {
			return 0;
		}
		const float clamped = std::clamp(value, -1.0F, 1.0F);
		return static_cast<int16_t>(std::lround(clamped * static_cast<float>(ControllerFrame::c_AnalogScale)));
	}

	float ControllerFrameCodec::DequantizeAnalog(int16_t value) {
		return static_cast<float>(value) / static_cast<float>(ControllerFrame::c_AnalogScale);
	}

	int16_t ControllerFrameCodec::QuantizeMouseDelta(float value) {
		if (!std::isfinite(value)) {
			return 0;
		}
		const float clamped = std::clamp(value, static_cast<float>(std::numeric_limits<int16_t>::min()), static_cast<float>(std::numeric_limits<int16_t>::max()));
		return static_cast<int16_t>(std::lround(clamped));
	}

	uint32_t ControllerFrameCodec::PayloadChecksum(const std::vector<uint8_t>& bytes) {
		return PayloadChecksum(bytes.data(), bytes.size());
	}

	uint32_t ControllerFrameCodec::PayloadChecksum(const uint8_t* data, size_t size) {
		uint32_t state = 2166136261U;
		for (size_t i = 0; i < size; ++i) {
			state ^= data[i];
			state *= 16777619U;
		}
		return state;
	}

	int ControllerFrameSelfTest::Run() {
		TimerMan::Construct();

		auto fail = [](const std::string& message) {
			std::cerr << "[controller-frame-selftest] FAIL: " << message << std::endl;
			return 1;
		};

		Controller controller(Controller::CIM_PLAYER, Players::PlayerOne);
		controller.SetState(ControlState::PRIMARY_ACTION, true);
		controller.SetState(ControlState::WEAPON_FIRE, true);
		controller.SetState(ControlState::DEBUG_ONE, true);
		controller.SetAnalogMove(Vector(-1.0F, 1.0F));
		controller.SetAnalogAim(Vector(0.5F, -0.25F));
		controller.SetAnalogCursor(Vector(0.0F, 0.75F));

		ControllerFrame frame = ControllerFrameCodec::Snapshot(12345, controller);
		frame.mouseDeltaX = -7;
		frame.mouseDeltaY = 9;
		frame.SetQuickDisabled(true);

		const std::vector<uint8_t> encoded = ControllerFrameCodec::Encode(frame);
		if (encoded.size() != ControllerFrame::c_EncodedSize) {
			return fail("encoded size mismatch");
		}

		ControllerFrame decoded;
		std::string error;
		if (!ControllerFrameCodec::Decode(encoded.data(), encoded.size(), decoded, &error)) {
			return fail(error);
		}
		if (decoded.actorUniqueID != frame.actorUniqueID || decoded.stateMask != frame.stateMask ||
		    decoded.inputMode != frame.inputMode || decoded.playerRaw != frame.playerRaw ||
		    decoded.flags != frame.flags || decoded.mouseDeltaX != frame.mouseDeltaX ||
		    decoded.mouseDeltaY != frame.mouseDeltaY) {
			return fail("decoded scalar fields differ");
		}

		Controller applied(Controller::CIM_DISABLED, Players::NoPlayer);
		if (!ControllerFrameCodec::Apply(decoded, applied, &error)) {
			return fail(error);
		}
		if (!applied.IsState(ControlState::PRIMARY_ACTION) ||
		    !applied.IsState(ControlState::WEAPON_FIRE) ||
		    !applied.IsState(ControlState::DEBUG_ONE)) {
			return fail("control states failed to apply");
		}
		if (!NearlyEqual(applied.GetAnalogMove().m_X, -1.0F) ||
		    !NearlyEqual(applied.GetAnalogMove().m_Y, 1.0F) ||
		    !NearlyEqual(applied.GetAnalogAim().m_X, 0.5F) ||
		    !NearlyEqual(applied.GetAnalogAim().m_Y, -0.25F) ||
		    !NearlyEqual(applied.GetAnalogCursor().m_Y, 0.75F)) {
			return fail("analog values failed to round-trip");
		}
		if (applied.GetInputMode() != Controller::CIM_PLAYER || applied.GetPlayerRaw() != Players::PlayerOne || !applied.IsQuickDisabled()) {
			return fail("mode/player/disabled fields failed to apply");
		}

		if constexpr (ControlState::CONTROLSTATECOUNT < 64) {
			ControllerFrame malformed = decoded;
			malformed.stateMask |= (uint64_t{1} << ControlState::CONTROLSTATECOUNT);
			const std::vector<uint8_t> malformedBytes = ControllerFrameCodec::Encode(malformed);
			if (ControllerFrameCodec::Decode(malformedBytes.data(), malformedBytes.size(), decoded, nullptr)) {
				return fail("malformed state mask was accepted");
			}
		}

		ControllerFrame malformed = frame;
		malformed.flags |= 0x2U;
		const std::vector<uint8_t> malformedFlagsBytes = ControllerFrameCodec::Encode(malformed);
		if (ControllerFrameCodec::Decode(malformedFlagsBytes.data(), malformedFlagsBytes.size(), decoded, nullptr)) {
			return fail("malformed flags were accepted");
		}

		std::cout << "[controller-frame-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
