// Copyright (c) 2003 Daniel Wallin and Arvid Norberg

// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
// OR OTHER DEALINGS IN THE SOFTWARE.


#ifndef LUABIND_OBJECT_REP_HPP_INCLUDED
#define LUABIND_OBJECT_REP_HPP_INCLUDED

#include <luabind/config.hpp>
#include <luabind/detail/ref.hpp>

namespace luabind { namespace detail
{
	class class_rep;

	void finalize(lua_State* L, class_rep* crep);

	// this class is allocated inside lua for each pointer.
	// it contains the actual c++ object-pointer.
	// it also tells if it is const or not.
	class LUABIND_API object_rep
	{
	public:
		// checkpoint_trap: a checkpoint walk wrote this object's values into a cached chunk and wants
		// the next mutation reported, once.
		enum { constant = 1, owner = 2, lua_class = 4, call_super = 8, checkpoint_trap = 16 };

		// dest is a function that is called to delete the c++ object this struct holds
		object_rep(void* obj, class_rep* crep, int flags, void(*dest)(void*));
		object_rep(class_rep* crep, int flags, detail::lua_reference const& table_ref);
		~object_rep();

		void* ptr() const { return m_object; }

		void* ptr(int pointer_offset) const
		{
			return reinterpret_cast<char*>(m_object) + pointer_offset;
		}

		const class_rep* crep() const { return m_classrep; }
		class_rep* crep() { return m_classrep; }
		int flags() const { return m_flags; }
		void set_flags(int flags) { m_flags = flags; }

		detail::lua_reference& get_lua_table() { return m_lua_table_ref; }
		detail::lua_reference const& get_lua_table() const { return m_lua_table_ref; }

		void remove_ownership();
		void set_destructor(void(*ptr)(void*));

		void set_object(void* p) { m_object = p; }

		void add_dependency(lua_State* L, int index);
		detail::lua_reference const& get_dependencies() const { return m_dependency_ref; }
		void set_checkpoint_parent(object_rep* parent) { m_checkpoint_parent = parent; }
		object_rep* checkpoint_parent() const { return m_checkpoint_parent; }
		void set_checkpoint_owner(void* owner, void (*write)(void*)) { m_checkpoint_owner = owner; m_checkpoint_owner_write = write; }
		void checkpoint_owner_written() { if (m_checkpoint_owner_write) m_checkpoint_owner_write(m_checkpoint_owner); }
		// The preview window this handle may write in, or 0.
		unsigned preview_window() const { return m_preview_window; }
		void set_preview_window(unsigned window) { m_preview_window = window; }

		static int garbage_collector(lua_State* L);

	private:

		void* m_object; // pointer to the c++ object or holder / if lua class, this is a pointer the the instance of the
									// c++ base or 0.
		class_rep* m_classrep; // the class information about this object's type
		int m_flags;
		detail::lua_reference m_lua_table_ref; // reference to lua table if this is a lua class
		void(*m_destructor)(void*); // this could be in class_rep? it can't: see intrusive_ptr
		int m_dependency_cnt; // counts dependencies
		detail::lua_reference m_dependency_ref; // reference to lua table holding dependency references
		object_rep* m_checkpoint_parent = 0;
		void* m_checkpoint_owner = 0;
		void (*m_checkpoint_owner_write)(void*) = 0;
		unsigned m_preview_window = 0;

		// ======== the new way, separate object_rep from the holder
//		instance_holder* m_instance;
	};

	template<class T>
	struct delete_s
	{
		static void apply(void* ptr)
		{
			delete static_cast<T*>(ptr);
		}
	};

	template<class T>
	struct destruct_only_s
	{
		static void apply(void* ptr)
		{
			// Removes unreferenced formal parameter warning on VC7.
			(void)ptr;
#ifndef NDEBUG
			int completeness_check[sizeof(T)];
			(void)completeness_check;
#endif
			static_cast<T*>(ptr)->~T();
		}
	};


	typedef void (*checkpoint_object_write_cb)(void*);
	extern LUABIND_API checkpoint_object_write_cb checkpoint_object_write;

	// The trap only has to fire once per armed window: the mark is the answer. An object with no trap
	// is in no cached chunk, so its mutations cost nothing but the flag test.
	inline void checkpoint_object_mutated(object_rep* obj)
	{
		if (obj && (obj->flags() & object_rep::checkpoint_trap))
		{
			obj->set_flags(obj->flags() & ~object_rep::checkpoint_trap);
			if (checkpoint_object_write) checkpoint_object_write(obj->ptr());
		}
	}

	inline void checkpoint_alias_mutated(object_rep* obj)
	{
		for (object_rep* parent = obj ? obj->checkpoint_parent() : 0; parent; parent = parent->checkpoint_parent())
		{
			checkpoint_object_mutated(parent);
			parent->checkpoint_owner_written();
		}
	}

	inline object_rep* is_class_object(lua_State* L, int index)
	{
		object_rep* obj = static_cast<detail::object_rep*>(lua_touserdata(L, index));
		if (!obj) return 0;
		if (lua_getmetatable(L, index) == 0) return 0;

		lua_pushstring(L, "__luabind_class");
		lua_gettable(L, -2);
		bool confirmation = lua_toboolean(L, -1) != 0;
		lua_pop(L, 2);
		if (!confirmation) return 0;
		return obj;

	}

	// A preview window's fence at the binding boundary. It is open only on the thread that runs the window, only while
	// the window lasts; outside it every handle reads and writes exactly as before.
	extern LUABIND_API thread_local unsigned preview_fence_window; // The open window's serial on this thread, or 0.
	extern LUABIND_API thread_local int preview_fence_parent; // What the call being converted runs on: -1 nothing, 0 the world's, 1 the window's.

	struct LUABIND_API preview_fence
	{
		// Opens a window on this thread; a nested window shares the outermost one's serial.
		static void open();
		static void close();
		// Points a converted handle at the window's own copy of the world object it names, when the window holds one.
		static void (*substitute)(object_rep* obj);
		// 1 when the object behind a handle is the window's own, 0 when it is the world's, -1 when only the handle can tell.
		static int (*owns)(const object_rep* obj);
		// Whether a non-const method may run on an object the window does not own: a read-only call does, a write is dropped.
		static bool (*runs)(const char* class_name, const char* method_name);
	};

	// Whether a write through this handle may land: always, outside a window.
	inline bool preview_fence_writes(const object_rep* obj)
	{
		if (!preview_fence_window || !obj) return true;
		const int owned = preview_fence::owns ? preview_fence::owns(obj) : -1;
		return owned >= 0 ? owned != 0 : obj->preview_window() == preview_fence_window;
	}

	// A handle the window makes for an object it did not create takes the standing of the call that produced it.
	inline void preview_fence_converted(object_rep* obj)
	{
		if (!preview_fence_window) return;
		if (preview_fence_parent == 1) obj->set_preview_window(preview_fence_window);
		if (preview_fence::substitute) preview_fence::substitute(obj);
	}

	// Inside a window, a getter or method run on the world's object hands back a copy of what it reads, never an alias.
	inline bool preview_fence_detaches()
	{
		return preview_fence_window && preview_fence_parent == 0;
	}

	// The results a getter or method converts take the standing of the object it runs on, for as long as it runs.
	struct preview_fence_call
	{
		int previous;

		explicit preview_fence_call(const object_rep* self) : previous(preview_fence_parent)
		{
			if (preview_fence_window) preview_fence_parent = self && preview_fence_writes(self) ? 1 : 0;
		}

		preview_fence_call(lua_State* L, int index) : previous(preview_fence_parent)
		{
			if (!preview_fence_window) return;
			const object_rep* self = is_class_object(L, index);
			preview_fence_parent = self && preview_fence_writes(self) ? 1 : 0;
		}

		~preview_fence_call() { preview_fence_parent = previous; }
	};

}}

#endif // LUABIND_OBJECT_REP_HPP_INCLUDED
