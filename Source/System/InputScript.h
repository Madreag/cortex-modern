#pragma once

#include <cstdint>
#include <string>

namespace RTE {

	class Vector;

	/// A scripted input source that stands in for the player's devices at the UInputMan boundary, so
	/// gameplay fixtures drive the real controller and wire paths tick by tick.
	///
	/// Script lines: `[player=N] <from_tick> <to_tick> ACTION [ACTION...]`, ticks inclusive, where an ACTION is an
	/// InputElements name without the INPUT_ prefix (L_LEFT, FIRE, WEAPON_PICKUP, ...), `AIM=x,y` for the analog aim
	/// vector, or `MOUSE=dx,dy` for a per-tick mouse movement. `#` starts a comment.
	class InputScript {
	public:
		static bool Load(const std::string& path, std::string* error = nullptr);
		static bool IsActive() { return s_Active; }
		static const std::string& GetPath() { return s_Path; }

		/// Whether the element is held at the sim tick (a range covers the tick).
		static bool HeldAt(int player, int element, uint64_t simTick);
		/// The analog aim vector scripted for the tick; false when none is.
		static bool AimAt(int player, uint64_t simTick, Vector& outAim);
		/// The mouse movement scripted for the tick; false when none is.
		static bool MouseAt(int player, uint64_t simTick, Vector& outMovement);

		/// Whether any line addresses the player: only then does the script replace that player's devices.
		static bool DrivesPlayer(int player);

		static int ElementFromName(const std::string& name);
		static const char* ElementName(int element);

	private:
		static bool s_Active;
		static std::string s_Path;
	};
} // namespace RTE
