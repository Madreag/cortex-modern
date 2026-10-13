#pragma once

#include "Vector.h"
#include "CheckpointNativeContainers.h"
#include <string>
#include <string_view>
#include <SDL3/SDL_gamepad.h>

namespace RTE {

	/// Structure for storing SDL_Gamepad or SDL_Joystick states.
	struct Gamepad {
		std::string SaveCheckpoint() const;
		bool LoadCheckpoint(std::string_view text, bool validateOnly = false);

		int m_DeviceIndex = -1; //!< The internal device index.
		SDL_JoystickID m_JoystickID = -1; //!< The joystick ID for event handling.
		CheckpointVector<int> m_Axis; //!< Array of analog axis states.
		CheckpointVector<int> m_DigitalAxis; //!< Array of digital axis states. Should be updated when analog axis crosses half value 8192.
		CheckpointVector<bool> m_Buttons; //!< Array of button states.
		CheckpointVector<bool> m_ButtonsPressedSinceSim; //!< Per-button press events accumulated since last sim-tick clear.
		CheckpointVector<bool> m_ButtonsReleasedSinceSim; //!< Per-button release events accumulated since last sim-tick clear.

#pragma region Creation
		/// Constructor method used to instantiate a Gamepad object in system memory and make it ready for use.
		Gamepad() = default;

		/// Constructor method used to instantiate a Gamepad object in system memory and make it ready for use.
		/// @param deviceIndex The SDL device index.
		/// @param id The joystick ID for event handling.
		/// @param numAxis Number of analog axis.
		/// @param numButtons Number of buttons.
		Gamepad(int deviceIndex, SDL_JoystickID id, int numAxis, int numButtons) :
		    m_DeviceIndex(deviceIndex), m_JoystickID(id), m_Axis(numAxis), m_DigitalAxis(numAxis), m_Buttons(numButtons), m_ButtonsPressedSinceSim(numButtons), m_ButtonsReleasedSinceSim(numButtons) {}
#pragma endregion

#pragma region Operator Overloads
		/// Equality operator for testing if any two Gamepads are equal by ID.
		/// @param joystickID The ID to check equality with.
		/// @return A boolean indicating whether the two operands are equal or not.
		bool operator==(int joystickID) const { return m_JoystickID == joystickID; }

		/// Equality operator for testing if any two Gamepads are equal.
		/// @param rhs A Gamepad reference as the right hand side operand.
		/// @return A boolean indicating whether the two operands are equal or not.
		bool operator==(const Gamepad& rhs) const { return m_JoystickID == rhs.m_JoystickID; }

		/// Comparison operator for sorting Gamepads by ID.
		/// @param rhs A Gamepad reference as the right hand side operand.
		/// @return A boolean indicating the comparison result.
		bool operator<(const Gamepad& rhs) const { return m_JoystickID < rhs.m_JoystickID; }
	};
} // namespace RTE
