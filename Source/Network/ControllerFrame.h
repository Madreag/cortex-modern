#pragma once

#include "Controller.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace RTE {

	class Actor;

	struct ControllerFrame {
		/// Version 6 carries one-shot actor intents and the owner's control scheme; version 5 frames
		/// (older recordings) apply the actor state absolutely every tick.
		static constexpr uint16_t c_Version = 6;
		static constexpr uint16_t c_LegacyVersion = 5;
		static constexpr size_t c_EncodedSize = 84;
		static constexpr size_t c_LegacyEncodedSize = 80;
		static constexpr int c_AnalogScale = 32767;

		int64_t actorUniqueID = 0;
		uint64_t stateMask = 0;
		int16_t analogMoveX = 0;
		int16_t analogMoveY = 0;
		int16_t analogAimX = 0;
		int16_t analogAimY = 0;
		int16_t analogCursorX = 0;
		int16_t analogCursorY = 0;
		int16_t mouseDeltaX = 0;
		int16_t mouseDeltaY = 0;
		uint8_t inputMode = static_cast<uint8_t>(Controller::CIM_DISABLED);
		int8_t playerRaw = Players::NoPlayer;
		uint8_t flags = 0;
		uint8_t deviceClass = static_cast<uint8_t>(Controller::WireDeviceClass::None);
		float aimAngle = 0.0F;
		float viewPointX = 0.0F;
		float viewPointY = 0.0F;
		int64_t equippedFGUniqueID = 0;
		int64_t equippedBGUniqueID = 0;
		float fgHandPosX = 0.0F;
		float fgHandPosY = 0.0F;
		float bgHandPosX = 0.0F;
		float bgHandPosY = 0.0F;
		float digitalAimSpeed = 1.0F;
		/// The semantics this frame was decoded with; the apply path dispatches on it.
		uint16_t version = c_Version;

		bool IsQuickDisabled() const { return (flags & 0x1U) != 0; }
		void SetQuickDisabled(bool disabled);
		bool IsActorHFlipped() const { return (flags & 0x2U) != 0; }
		void SetActorHFlipped(bool flipped);
		/// One-shot writes the owner's AI made directly to the actor; every peer applies them at this frame's tick.
		bool HasAimIntent() const { return (flags & 0x4U) != 0; }
		void SetAimIntent(bool intent);
		bool HasFlipIntent() const { return (flags & 0x8U) != 0; }
		void SetFlipIntent(bool intent);
		bool IsLegacy() const { return version < c_Version; }
		static constexpr uint8_t c_KnownFlags = 0x0FU;
		static constexpr uint8_t c_LegacyKnownFlags = 0x03U;
	};

	class ControllerFrameCodec {
	public:
		static ControllerFrame Snapshot(int64_t actorUniqueID, const Controller& controller, const Actor* actor = nullptr);
		static bool Apply(const ControllerFrame& frame, Controller& controller, std::string* error = nullptr);
		static bool ApplyActorState(const ControllerFrame& frame, Actor& actor, std::string* error = nullptr);
		/// Applies only the frame's off-wire intents; the sim derives everything else from the controller.
		static bool ApplyActorStateIntents(const ControllerFrame& frame, Actor& actor, std::string* error = nullptr);

		static bool IsSupportedVersion(uint16_t version) { return version == ControllerFrame::c_Version || version == ControllerFrame::c_LegacyVersion; }
		static size_t EncodedSizeFor(uint16_t version) { return version < ControllerFrame::c_Version ? ControllerFrame::c_LegacyEncodedSize : ControllerFrame::c_EncodedSize; }

		static std::vector<uint8_t> Encode(const ControllerFrame& frame);
		static bool Decode(const uint8_t* data, size_t size, ControllerFrame& outFrame, std::string* error = nullptr, uint16_t version = ControllerFrame::c_Version);

		static int16_t QuantizeAnalog(float value);
		static float DequantizeAnalog(int16_t value);
		static int16_t QuantizeMouseDelta(float value);

		static uint32_t PayloadChecksum(const std::vector<uint8_t>& bytes);
		static uint32_t PayloadChecksum(const uint8_t* data, size_t size);
	};

	class ControllerFrameSelfTest {
	public:
		static int Run();
	};

} // namespace RTE
