#pragma once

#include "CheckpointLuaValues.h"
#include "CheckpointLuaBindings.h"

#include <luabind/detail/class_registry.hpp>
#include <luabind/detail/object_rep.hpp>

namespace RTE::CheckpointLua {
	// Saved handles keep their Lua aliases and point only at private native values.
	class SavedObjects {
	public:
		using Copy = std::function<void*(const luabind::detail::class_rep*, const void*)>;
		SavedObjects(lua_State* state, Snapshot heap, const BindingMetadata::Lease& bindings, Copy copy) :
		    m_State(state), m_Heap(std::move(heap)), m_Bindings(m_Heap, &bindings), m_Copy(std::move(copy)),
		    m_Values(state, m_Heap, [this](lua_State*, const TValue& value) { PushNative(value); }) {}
		SavedValues& Values() { return m_Values; }
		void Prepare() {
			const auto state = m_Heap.Read(m_Heap.State());
			const auto global = m_Heap.Read(mref(state.glref, global_State));
			for (const GCobj* object = gcref(global.gc.root); object;) {
				const auto head = m_Heap.Read(&object->gch);
				if (head.gct == static_cast<uint8_t>(~LJ_TUDATA)) {
					const auto* data = reinterpret_cast<const GCudata*>(object);
					if (DeclaredClass(data)) { TValue value; setgcVraw(&value, const_cast<GCobj*>(object), LJ_TUDATA); PushClass(value); lua_pop(m_State, 1); }
				}
				object = gcref(head.nextgc);
			}
			lua_pushvalue(m_State, LUA_GLOBALSINDEX);
			m_Values.RememberLibraries(m_Heap.GlobalValue("_G"), -1); lua_pop(m_State, 1);
			for (const auto& [source, target]: m_Classes) {
				lua_rawgeti(m_State, LUA_REGISTRYINDEX, target->metatable_ref());
				m_Values.RememberLibraries(m_Heap.RegistryValue(m_Bindings.Class(source)->metatable_ref()), -1); lua_pop(m_State, 1);
				target->get_table(m_State);
				m_Values.RememberLibraries(m_Bindings.Members(source), -1); lua_pop(m_State, 1);
				target->get_default_table(m_State);
				m_Values.RememberLibraries(DefaultMembers(source), -1); lua_pop(m_State, 1);
			}
			for (const auto& [source, target]: m_Classes) {
				target->get_table(m_State);
				m_Values.FillTable(m_Bindings.Members(source), -1); lua_pop(m_State, 1);
				target->get_default_table(m_State);
				m_Values.FillTable(DefaultMembers(source), -1); lua_pop(m_State, 1);
			}
		}
	private:
		lua_State* m_State;
		Snapshot m_Heap;
		FrozenBindings m_Bindings;
		Copy m_Copy;
		SavedValues m_Values;
		std::unordered_map<const luabind::detail::class_rep*, luabind::detail::class_rep*> m_Classes;
		TValue DefaultMembers(const luabind::detail::class_rep* source) const { return m_Heap.RegistryValue(m_Bindings.Class(source)->checkpoint_default_table_reference()); }
		bool DeclaredClass(const GCudata* data) const {
			if (m_Heap.Read(&data->len) != sizeof(luabind::detail::class_rep)) return false;
			const auto* source = static_cast<const luabind::detail::class_rep*>(uddata(const_cast<GCudata*>(data)));
			const auto* view = m_Bindings.Class(source);
			if (!view->checkpoint_metadata()) return false;
			const TValue registered = m_Heap.RegistryValue(view->checkpoint_self_reference());
			return tvisudata(&registered) && udataV(&registered) == data;
		}
		void PushClass(const TValue& value) {
			if (m_Values.Remembered(value)) { m_Values.Push(value); return; }
			const auto* source = static_cast<const luabind::detail::class_rep*>(uddata(udataV(&value)));
			const auto* view = m_Bindings.Class(source);
			const auto& metadata = m_Bindings.Metadata(source);
			luabind::detail::class_rep* target;
			if (view->get_class_type() == luabind::detail::class_rep::cpp_class) {
				target = luabind::detail::class_registry::get_registry(m_State)->find_class(view->type());
				if (!target) throw std::runtime_error("a saved native class has no game binding");
				lua_rawgeti(m_State, LUA_REGISTRYINDEX, target->checkpoint_self_reference());
			} else {
				void* storage = lua_newuserdata(m_State, sizeof(luabind::detail::class_rep));
				target = new(storage) luabind::detail::class_rep(m_State, metadata.name.c_str());
			}
			const int destination = lua_gettop(m_State);
			m_Values.Remember(value, destination);
			m_Classes.emplace(source, target);
			if (view->get_class_type() == luabind::detail::class_rep::lua_class) {
				for (const auto& base: CheckpointValues(metadata.bases)) {
					const TValue token = m_Heap.RegistryValue(m_Bindings.Class(base.base)->checkpoint_self_reference());
					PushClass(token);
					auto* parent = static_cast<luabind::detail::class_rep*>(lua_touserdata(m_State, -1));
					target->add_base_class({base.pointer_offset, parent}); target->derived_from(parent); target->set_type(parent->type());
					lua_pop(m_State, 1);
				}
			}
			target->get_table(m_State);
			if (!m_Values.Remembered(m_Bindings.Members(source))) m_Values.Remember(m_Bindings.Members(source), -1);
			lua_pop(m_State, 1);
			target->get_default_table(m_State);
			if (!m_Values.Remembered(DefaultMembers(source))) m_Values.Remember(DefaultMembers(source), -1);
			lua_settop(m_State, destination);
		}
		void PushObject(const TValue& value) {
			const auto* source = m_Bindings.Object(udataV(&value));
			if (!source || !source->crep()) throw std::runtime_error("a saved native handle has no class");
			const TValue type = m_Heap.RegistryValue(m_Bindings.Class(source->crep())->checkpoint_self_reference());
			PushClass(type);
			auto* targetClass = static_cast<luabind::detail::class_rep*>(lua_touserdata(m_State, -1));
			lua_pop(m_State, 1);
			void* native = source->ptr() ? m_Copy(source->crep(), source->ptr()) : nullptr;
			if (source->ptr() && !native) throw std::runtime_error("a saved native handle has no private value");
			void* storage = lua_newuserdata(m_State, sizeof(luabind::detail::object_rep));
			auto* target = new(storage) luabind::detail::object_rep(native, targetClass, source->flags() & ~luabind::detail::object_rep::checkpoint_trap, [](void*) {});
			const int destination = lua_gettop(m_State);
			m_Values.Remember(value, destination);
			const auto data = m_Heap.Read(udataV(&value));
			if (const GCobj* meta = gcref(data.metatable)) {
				TValue token; setgcVraw(&token, const_cast<GCobj*>(meta), LJ_TTAB);
				m_Values.Push(token); lua_setmetatable(m_State, destination);
			}
			const TValue fields = m_Bindings.Instance(udataV(&value));
			if (!tvisnil(&fields)) { m_Values.Push(fields); target->get_lua_table().set(m_State); }
			const TValue dependencies = m_Heap.RegistryValue(source->get_dependencies().checkpoint_reference());
			if (tvistab(&dependencies)) {
				m_Values.Push(dependencies);
				const int table = lua_gettop(m_State); lua_pushnil(m_State);
				while (lua_next(m_State, table)) { target->add_dependency(m_State, -1); lua_pop(m_State, 1); }
				lua_pop(m_State, 1);
			}
			lua_settop(m_State, destination);
		}
		void PushNative(const TValue& value) {
			if (tvisudata(&value)) {
				if (DeclaredClass(udataV(&value))) PushClass(value);
				else PushObject(value);
				return;
			}
			if (tvisfunc(&value) && m_Heap.Read(&funcV(&value)->c.ffid) == FF_C) {
				const auto* source = funcV(&value);
				const int count = m_Heap.Read(&source->c.nupvalues);
				for (int index = 0; index < count; ++index) lua_pushnil(m_State);
				lua_pushcclosure(m_State, m_Heap.Read(&source->c.f), count);
				const int destination = lua_gettop(m_State);
				m_Values.Remember(value, destination);
				for (int index = 0; index < count; ++index) {
					m_Values.Push(m_Heap.Read(&source->c.upvalue[index]));
					if (!lua_setupvalue(m_State, destination, index + 1)) throw std::runtime_error("a saved native closure lost an upvalue");
				}
				return;
			}
			throw std::runtime_error("a saved native closure has no retained game binding");
		}
	};
}
