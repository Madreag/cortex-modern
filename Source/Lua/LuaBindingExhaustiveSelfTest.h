#pragma once

#include <cstdint>

namespace RTE {

	/// Calls every method and property of every luabind class on a canonical object inside a preview window, and holds the object and the world to their bytes after each call.
	class LuaBindingExhaustiveSelfTest {
	public:
		/// Arms the walk: the missing fixtures join the world at fixtureTick and the walk runs at walkTick.
		static void Arm(uint64_t fixtureTick, uint64_t walkTick);

		/// Runs this tick's part of the walk.
		/// @return -1 while nothing is decided, 0 once the walk passed, 1 once it failed.
		static int OnTick(uint64_t simTick);
	};
} // namespace RTE
