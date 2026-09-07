#pragma once

struct lua_State;

namespace RTE::LuaThreadCodec {
	/// Registers capture and restore helpers for coroutine frames and open upvalues.
	void Register(lua_State* state);
} // namespace RTE::LuaThreadCodec
