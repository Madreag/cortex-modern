#pragma once

#include <cstddef>

struct lua_State;

namespace RTE::LuaThreadCodec {
	/// Visits unfinalized userdata without changing the VM. The callback must not invoke Lua or its collector.
	void VisitUserdata(lua_State* state, void (*visitor)(void* data, size_t size, const void* metatable, void* context), void* context);

	/// Copies each Lua-value slot of every frame onto dest and calls visitor. Frame-link slots are skipped.
	bool VisitThreadStack(lua_State* thread, lua_State* dest, bool (*visitor)(lua_State* dest, void* context), void* context);

	/// Registers capture and restore helpers for coroutine frames and open upvalues.
	void Register(lua_State* state);

	/// Copies the coroutines a preview's script copy holds so each resumes where its original stands, and the closures that
	/// reach their variables, so a preview writes only its own copies.
	class PreviewCopier {
	public:
		/// Replaces the value on top of the stack with what the copy holds for it.
		using MapValue = void (*)(lua_State* state, void* context);

		/// @param seen The absolute index of the copy's original-to-copy table.
		/// @param standIn The body a coroutine that cannot be copied gets instead.
		PreviewCopier(lua_State* state, int seen, MapValue mapValue, void* context, int (*standIn)(lua_State*));
		~PreviewCopier();

		/// Pushes the copy of the coroutine at index, recorded in seen: its frames, its place in them and its own values.
		/// @return False, pushing nothing, when it cannot resume faithfully (running, or inside a continuation the codec cannot rebuild).
		bool PushThread(int index);

		/// Pushes the function at index, or a copy whose variables are the copy's: an upvalue open on a coroutine moves to that
		/// coroutine's copy, one a copied coroutine's code owns is copied once. A coroutine.wrap function gets its coroutine's copy.
		void PushFunction(int index);

		/// Joins the variables the walk left open and moves the closures the copied tables hold onto the copied variables.
		void Finish();

	private:
		struct Impl;
		Impl* m_Impl;
	};

	/// Whether the function at index is a closure a preview copier made whose variables are all its own.
	bool IsOwnPreviewCopy(lua_State* state, int index);

	/// Hands each value a preview copy keeps outside its tables to remap - a copied coroutine's stack, an own closure copy's
	/// variables - which may replace the value on top of the stack; the copy keeps what it leaves. False when remap fails.
	bool RemapPreviewCopyValues(lua_State* state, int index, bool (*remap)(lua_State* state, void* context), void* context);
} // namespace RTE::LuaThreadCodec
