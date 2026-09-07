#pragma once

#include <cstddef>

struct lua_State;

namespace RTE::LuaThreadCodec {
	/// Visits unfinalized userdata without changing the VM. The callback must not invoke Lua or its collector.
	void VisitUserdata(lua_State* state, void (*visitor)(void* data, size_t size, const void* metatable, void* context), void* context);

	/// Registers capture and restore helpers for coroutine frames and open upvalues.
	void Register(lua_State* state);
} // namespace RTE::LuaThreadCodec
