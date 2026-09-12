#include "ControllerFrame.h"

#include "Actor.h"
#include "AHuman.h"
#include "HeldDevice.h"
#include "TimerMan.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

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

		void AppendU32LE(std::vector<uint8_t>& out, uint32_t value) {
			for (int i = 0; i < 4; ++i) {
				out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFFU));
			}
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

		void AppendF32LE(std::vector<uint8_t>& out, float value) {
			uint32_t bits;
			std::memcpy(&bits, &value, sizeof(bits));
			AppendU32LE(out, bits);
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

		uint32_t ReadU32LE(const uint8_t*& p) {
			uint32_t value = 0;
			for (int i = 0; i < 4; ++i) {
				value |= static_cast<uint32_t>(p[i]) << (i * 8);
			}
			p += 4;
			return value;
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

		float ReadF32LE(const uint8_t*& p) {
			const uint32_t bits = ReadU32LE(p);
			float value;
			std::memcpy(&value, &bits, sizeof(value));
			return value;
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

	void ControllerFrame::SetActorHFlipped(bool flipped) {
		if (flipped) {
			flags |= 0x2U;
		} else {
			flags &= static_cast<uint8_t>(~0x2U);
		}
	}

	void ControllerFrame::SetAimIntent(bool intent) {
		if (intent) {
			flags |= 0x4U;
		} else {
			flags &= static_cast<uint8_t>(~0x4U);
		}
	}

	void ControllerFrame::SetFlipIntent(bool intent) {
		if (intent) {
			flags |= 0x8U;
		} else {
			flags &= static_cast<uint8_t>(~0x8U);
		}
	}

	ControllerFrame ControllerFrameCodec::Snapshot(int64_t actorUniqueID, const Controller& controller, const Actor* actor) {
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
		// The producer's seat is what every peer's sim adopts at this frame's tick.
		frame.inputMode = static_cast<uint8_t>(controller.GetSeatMode());
		frame.playerRaw = static_cast<int8_t>(std::clamp(controller.GetSeatPlayerRaw(), -128, 127));
		frame.SetQuickDisabled(controller.IsQuickDisabled());
		frame.deviceClass = static_cast<uint8_t>(controller.GetLocalDeviceClass());
		frame.digitalAimSpeed = controller.GetLocalDigitalAimSpeed();
		if (actor) {
			// A direct AI write this tick rides as the intent value; the live state stays what the wire last applied.
			const long long simTick = static_cast<long long>(g_TimerMan.GetSimUpdateCount());
			const bool aimIntent = actor->GetOffWireAimTick() == simTick;
			const bool flipIntent = actor->GetOffWireFlipTick() == simTick;
			frame.SetActorHFlipped(flipIntent ? actor->GetOffWireFlip() : actor->IsHFlipped());
			frame.aimAngle = aimIntent ? actor->GetOffWireAim() : actor->GetAimAngle(false);
			frame.SetAimIntent(aimIntent);
			frame.SetFlipIntent(flipIntent);
			const Vector viewPoint = actor->GetViewPoint();
			frame.viewPointX = viewPoint.m_X;
			frame.viewPointY = viewPoint.m_Y;
			if (const AHuman* human = dynamic_cast<const AHuman*>(actor)) {
				if (const Arm* fgArm = human->GetFGArm()) {
					const Vector fgHandPos = fgArm->GetHandPos();
					frame.fgHandPosX = fgHandPos.m_X;
					frame.fgHandPosY = fgHandPos.m_Y;
				}
				if (const Arm* bgArm = human->GetBGArm()) {
					const Vector bgHandPos = bgArm->GetHandPos();
					frame.bgHandPosX = bgHandPos.m_X;
					frame.bgHandPosY = bgHandPos.m_Y;
				}
				if (const HeldDevice* equippedFG = human->GetEquippedItem()) {
					frame.equippedFGUniqueID = static_cast<int64_t>(equippedFG->GetUniqueID());
				}
				if (const HeldDevice* equippedBG = human->GetEquippedBGItem()) {
					frame.equippedBGUniqueID = static_cast<int64_t>(equippedBG->GetUniqueID());
				}
			}
		}
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
		if ((frame.flags & static_cast<uint8_t>(~(frame.IsLegacy() ? ControllerFrame::c_LegacyKnownFlags : ControllerFrame::c_KnownFlags))) != 0) {
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
		if (!frame.IsLegacy()) {
			controller.ApplyWireScheme(static_cast<Controller::WireDeviceClass>(frame.deviceClass), frame.digitalAimSpeed);
		}
		return true;
	}

	bool ControllerFrameCodec::ApplyActorState(const ControllerFrame& frame, Actor& actor, std::string* error) {
		if (!std::isfinite(frame.aimAngle) || !std::isfinite(frame.viewPointX) || !std::isfinite(frame.viewPointY) ||
		    !std::isfinite(frame.fgHandPosX) || !std::isfinite(frame.fgHandPosY) ||
		    !std::isfinite(frame.bgHandPosX) || !std::isfinite(frame.bgHandPosY)) {
			SetError(error, "ControllerFrame actor state fields must be finite.");
			return false;
		}
		actor.SetHFlipped(frame.IsActorHFlipped());
		actor.SetAimAngle(frame.aimAngle);
		actor.SetViewPoint(Vector(frame.viewPointX, frame.viewPointY));
		if (AHuman* human = dynamic_cast<AHuman*>(&actor)) {
			// The referenced item can be deterministically destroyed while this frame is in flight, so a
			// failed equip sync is skipped; both peers make the same call on the same state, and real
			// divergence is policed by the sim-hash exchange.
			human->SyncEquippedItemsByUniqueID(frame.equippedFGUniqueID, frame.equippedBGUniqueID);
			if (Arm* fgArm = human->GetFGArm()) {
				fgArm->SetHandPos(Vector(frame.fgHandPosX, frame.fgHandPosY));
			}
			if (Arm* bgArm = human->GetBGArm()) {
				bgArm->SetHandPos(Vector(frame.bgHandPosX, frame.bgHandPosY));
			}
		} else if (frame.equippedFGUniqueID != 0 || frame.equippedBGUniqueID != 0) {
			SetError(error, "ControllerFrame equipped item state targets a non-AHuman actor.");
			return false;
		}
		return true;
	}

	bool ControllerFrameCodec::ApplyActorStateIntents(const ControllerFrame& frame, Actor& actor, std::string* error) {
		if (frame.HasAimIntent()) {
			if (!std::isfinite(frame.aimAngle)) {
				SetError(error, "ControllerFrame aim intent must be finite.");
				return false;
			}
			actor.SetAimAngle(frame.aimAngle);
		}
		if (frame.HasFlipIntent()) {
			actor.SetHFlipped(frame.IsActorHFlipped());
		}
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
		AppendU8(out, frame.deviceClass);
		AppendF32LE(out, frame.aimAngle);
		AppendF32LE(out, frame.viewPointX);
		AppendF32LE(out, frame.viewPointY);
		AppendI64LE(out, frame.equippedFGUniqueID);
		AppendI64LE(out, frame.equippedBGUniqueID);
		AppendF32LE(out, frame.fgHandPosX);
		AppendF32LE(out, frame.fgHandPosY);
		AppendF32LE(out, frame.bgHandPosX);
		AppendF32LE(out, frame.bgHandPosY);
		AppendF32LE(out, frame.digitalAimSpeed);
		return out;
	}

	bool ControllerFrameCodec::Decode(const uint8_t* data, size_t size, ControllerFrame& outFrame, std::string* error, uint16_t version) {
		if (!IsSupportedVersion(version)) {
			SetError(error, "ControllerFrame version " + std::to_string(version) + " is not supported.");
			return false;
		}
		if (!data || size != EncodedSizeFor(version)) {
			SetError(error, "ControllerFrame encoded size mismatch.");
			return false;
		}
		const uint8_t* p = data;
		ControllerFrame frame;
		frame.version = version;
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
		frame.deviceClass = ReadU8(p);
		frame.aimAngle = ReadF32LE(p);
		frame.viewPointX = ReadF32LE(p);
		frame.viewPointY = ReadF32LE(p);
		frame.equippedFGUniqueID = ReadI64LE(p);
		frame.equippedBGUniqueID = ReadI64LE(p);
		frame.fgHandPosX = ReadF32LE(p);
		frame.fgHandPosY = ReadF32LE(p);
		frame.bgHandPosX = ReadF32LE(p);
		frame.bgHandPosY = ReadF32LE(p);
		if (frame.IsLegacy()) {
			// The legacy layout kept this byte reserved and carried no scheme facts.
			if (frame.deviceClass != 0) {
				SetError(error, "ControllerFrame reserved byte must be zero.");
				return false;
			}
		} else {
			frame.digitalAimSpeed = ReadF32LE(p);
			if (frame.deviceClass >= static_cast<uint8_t>(Controller::WireDeviceClass::Count)) {
				SetError(error, "ControllerFrame device class is out of range.");
				return false;
			}
			if (!std::isfinite(frame.digitalAimSpeed) || frame.digitalAimSpeed < 0.0F) {
				SetError(error, "ControllerFrame digital aim speed must be finite and non-negative.");
				return false;
			}
		}
		if (frame.inputMode >= static_cast<uint8_t>(Controller::CIM_INPUTMODECOUNT)) {
			SetError(error, "ControllerFrame input_mode is out of range.");
			return false;
		}
		if (HasUnknownControlStateBits(frame.stateMask)) {
			SetError(error, "ControllerFrame has state bits beyond CONTROLSTATECOUNT.");
			return false;
		}
		if ((frame.flags & static_cast<uint8_t>(~(frame.IsLegacy() ? ControllerFrame::c_LegacyKnownFlags : ControllerFrame::c_KnownFlags))) != 0) {
			SetError(error, "ControllerFrame reserved flags must be zero.");
			return false;
		}
		if (!std::isfinite(frame.aimAngle) || !std::isfinite(frame.viewPointX) || !std::isfinite(frame.viewPointY) ||
		    !std::isfinite(frame.fgHandPosX) || !std::isfinite(frame.fgHandPosY) ||
		    !std::isfinite(frame.bgHandPosX) || !std::isfinite(frame.bgHandPosY)) {
			SetError(error, "ControllerFrame actor state fields must be finite.");
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
		frame.SetActorHFlipped(true);
		frame.aimAngle = 0.125F;
		frame.viewPointX = 123.5F;
		frame.viewPointY = -456.25F;
		frame.equippedFGUniqueID = 22222;
		frame.equippedBGUniqueID = 33333;
		frame.fgHandPosX = 10.5F;
		frame.fgHandPosY = 20.5F;
		frame.bgHandPosX = -30.5F;
		frame.bgHandPosY = -40.5F;
		frame.deviceClass = static_cast<uint8_t>(Controller::WireDeviceClass::Gamepad);
		frame.digitalAimSpeed = 1.75F;

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
		    decoded.mouseDeltaY != frame.mouseDeltaY ||
		    decoded.aimAngle != frame.aimAngle ||
		    decoded.viewPointX != frame.viewPointX ||
		    decoded.viewPointY != frame.viewPointY ||
		    decoded.equippedFGUniqueID != frame.equippedFGUniqueID ||
		    decoded.equippedBGUniqueID != frame.equippedBGUniqueID ||
		    decoded.fgHandPosX != frame.fgHandPosX ||
		    decoded.fgHandPosY != frame.fgHandPosY ||
		    decoded.bgHandPosX != frame.bgHandPosX ||
		    decoded.bgHandPosY != frame.bgHandPosY ||
		    decoded.deviceClass != frame.deviceClass ||
		    decoded.digitalAimSpeed != frame.digitalAimSpeed ||
		    decoded.version != ControllerFrame::c_Version) {
			return fail("decoded scalar fields differ");
		}

		Controller applied(Controller::CIM_DISABLED, Players::NoPlayer);
		if (!ControllerFrameCodec::Apply(decoded, applied, &error)) {
			return fail(error);
		}
		if (!applied.HasWireScheme() || !applied.IsGamepadControlled() || applied.IsMouseControlled() || applied.GetDigitalAimSpeed() != 1.75F) {
			return fail("wire scheme facts failed to apply");
		}

		// A legacy frame is the first 80 bytes with the scheme byte reserved; it decodes only as version 5 and applies no scheme.
		std::vector<uint8_t> legacyBytes(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(ControllerFrame::c_LegacyEncodedSize));
		legacyBytes[35] = 0;
		legacyBytes[34] &= 0x3U;
		ControllerFrame legacy;
		if (ControllerFrameCodec::Decode(legacyBytes.data(), legacyBytes.size(), legacy, nullptr)) {
			return fail("a legacy-sized frame decoded as the current version");
		}
		if (!ControllerFrameCodec::Decode(legacyBytes.data(), legacyBytes.size(), legacy, &error, ControllerFrame::c_LegacyVersion)) {
			return fail("legacy frame decode failed: " + error);
		}
		if (legacy.version != ControllerFrame::c_LegacyVersion || !legacy.IsLegacy() || legacy.actorUniqueID != frame.actorUniqueID || legacy.aimAngle != frame.aimAngle || legacy.equippedBGUniqueID != frame.equippedBGUniqueID) {
			return fail("legacy frame fields differ");
		}
		Controller legacyApplied(Controller::CIM_DISABLED, Players::NoPlayer);
		if (!ControllerFrameCodec::Apply(legacy, legacyApplied, &error) || legacyApplied.HasWireScheme()) {
			return fail("a legacy frame must apply without scheme facts");
		}
		legacyBytes[35] = 1;
		if (ControllerFrameCodec::Decode(legacyBytes.data(), legacyBytes.size(), legacy, nullptr, ControllerFrame::c_LegacyVersion)) {
			return fail("a legacy frame with a reserved byte set was accepted");
		}
		ControllerFrame badDevice = frame;
		badDevice.deviceClass = static_cast<uint8_t>(Controller::WireDeviceClass::Count);
		const std::vector<uint8_t> badDeviceBytes = ControllerFrameCodec::Encode(badDevice);
		if (ControllerFrameCodec::Decode(badDeviceBytes.data(), badDeviceBytes.size(), decoded, nullptr)) {
			return fail("an out-of-range device class was accepted");
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
		malformed.flags |= 0x20U;
		const std::vector<uint8_t> malformedFlagsBytes = ControllerFrameCodec::Encode(malformed);
		if (ControllerFrameCodec::Decode(malformedFlagsBytes.data(), malformedFlagsBytes.size(), decoded, nullptr)) {
			return fail("malformed flags were accepted");
		}

		// Real time past the 250 ms debounce must not re-arm; sim time must.
		g_TimerMan.ResetTime();
		Controller releaseDelay(Controller::CIM_PLAYER, Players::PlayerOne);
		std::string clock = g_TimerMan.SaveCheckpoint();
		const std::string from = "9 TimerMan1 1000000 0 ";
		const auto at = clock.find(from);
		if (at == std::string::npos) {
			return fail("TimerMan checkpoint prefix missing");
		}
		clock.replace(at, from.size(), "9 TimerMan1 1000000 500000 ");
		if (!g_TimerMan.LoadCheckpoint(clock) || g_TimerMan.GetRealTickCount() != 500000 || g_TimerMan.GetSimTickCount() != 0) {
			return fail("TimerMan real time did not advance without a sim tick");
		}
		if (releaseDelay.ReleaseDelayPassed()) {
			return fail("release delay passed on real time with no sim tick");
		}
		int steps = 0;
		while (g_TimerMan.GetSimTimeMS() <= 250 && steps < 64) {
			g_TimerMan.AdvanceSimTickForPreview();
			++steps;
		}
		if (g_TimerMan.GetSimTimeMS() <= 250) {
			return fail("sim time did not advance past 250 ms");
		}
		if (!releaseDelay.ReleaseDelayPassed()) {
			return fail("release delay still closed after sim time passed 250 ms");
		}
		// A Go-To order disables the ordered actor's controller on the tick the synced pie command lands;
		// the owner's own sample of that disable is still inputDelayFrames out, so the frames committed
		// in the meantime were sampled before the order and must not enable the actor again.
		{
			Controller ordered(Controller::CIM_PLAYER, Players::PlayerOne);
			const ControllerFrame preOrder = ControllerFrameCodec::Snapshot(4242, ordered);
			ordered.HoldDisabledForSyncedOrder(305);
			if (!ControllerFrameCodec::Apply(preOrder, ordered, &error)) {
				return fail(error);
			}
			if (!ordered.IsDisabled()) {
				return fail("a frame sampled before the Go-To order re-enabled the ordered controller");
			}
			ControllerFrame ownerDisable = preOrder;
			ownerDisable.SetQuickDisabled(true);
			if (!ControllerFrameCodec::Apply(ownerDisable, ordered, &error) || !ordered.IsDisabled() || ordered.IsSyncedOrderDisableHeld()) {
				return fail("the owner's own disable failed to keep the controller disabled and take the hold back");
			}
			if (!ControllerFrameCodec::Apply(preOrder, ordered, &error) || ordered.IsDisabled()) {
				return fail("the wire did not own the disable again after the owner's sample landed");
			}
			// A hold the owner's disable never caught up with expires at the input-delay cap.
			Controller stranded(Controller::CIM_PLAYER, Players::PlayerOne);
			stranded.HoldDisabledForSyncedOrder(305);
			stranded.ExpireSyncedOrderDisable(364);
			if (!stranded.IsSyncedOrderDisableHeld()) {
				return fail("the order hold expired before the input-delay cap");
			}
			stranded.ExpireSyncedOrderDisable(365);
			if (stranded.IsSyncedOrderDisableHeld()) {
				return fail("the order hold outlived the input-delay cap");
			}
			if (!ControllerFrameCodec::Apply(preOrder, stranded, &error) || stranded.IsDisabled()) {
				return fail("an expired order hold still blocked the wire");
			}
		}

		std::cout << "[controller-frame-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
