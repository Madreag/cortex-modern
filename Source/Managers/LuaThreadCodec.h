#pragma once

#include <cstddef>
#include <string>

struct lua_State;

namespace RTE::LuaThreadCodec {
	/// Visits unfinalized userdata without changing the VM. The callback must not invoke Lua or its collector.
	void VisitUserdata(lua_State* state, void (*visitor)(void* data, size_t size, const void* metatable, void* context), void* context);

	/// Copies each Lua-value slot of every frame onto dest and calls visitor. Frame-link slots are skipped.
	bool VisitThreadStack(lua_State* thread, lua_State* dest, bool (*visitor)(lua_State* dest, void* context), void* context);

	/// Registers capture and restore helpers for coroutine frames and open upvalues.
	void Register(lua_State* state);

	/// Copies what a preview's script copy holds through closures and coroutines, so the copy keeps every identity and
	/// shared variable the original has and a preview writes only its own copies.
	class PreviewCopier {
	public:
		/// Replaces the value on top of the stack with what the copy holds for it.
		using MapValue = void (*)(lua_State* state, void* context);

		/// Whether the copy, or the remap that follows it, gives the userdata at index another value.
		using Replaced = bool (*)(lua_State* state, int index, void* context);

		/// @param seen The absolute index of the copy's original-to-copy table.
		/// @param standIn The body a coroutine that cannot be copied gets instead.
		PreviewCopier(lua_State* state, int seen, MapValue mapValue, Replaced replaced, void* context, int (*standIn)(lua_State*));
		~PreviewCopier();

		/// Points the copier at the original-to-copy table and the map context of the call it serves next.
		void Rebind(int seen, void* context);

		/// Pushes the copy of the coroutine at index, recorded in seen: its frames, its place in them and its own values.
		/// @return False, pushing nothing, when it cannot resume faithfully (running, or inside a continuation the codec cannot rebuild).
		bool PushThread(int index);

		/// Pushes the function at index, or its copy when a variable of it is written, holds a value the copy replaces or is
		/// open on a coroutine the copy restores; decided before the function is handed out, so it is one value everywhere.
		/// Copies that share a variable share its copy. A coroutine.wrap function gets its coroutine's copy.
		void PushFunction(int index);

		/// Joins the variables the walk left open to the copied coroutines' stacks.
		void Finish();

		/// Why the values copied since the last call cannot be copied faithfully, or empty.
		std::string TakeRefusal();

	private:
		struct Impl;
		Impl* m_Impl;
	};

	/// Whether the function at index is a closure a preview copier made that holds values of its own.
	bool IsOwnPreviewCopy(lua_State* state, int index);

	/// Hands each value a preview copy keeps outside its tables to remap - a copied coroutine's stack, a closure copy's own
	/// variables - which may replace the value on top of the stack; the copy keeps what it leaves. False when remap fails.
	bool RemapPreviewCopyValues(lua_State* state, int index, bool (*remap)(lua_State* state, void* context), void* context);
} // namespace RTE::LuaThreadCodec
