// Make sure that this wrapper file is always set to NOT use pre-compiled headers and conformance mode (/permissive) otherwise everything will be on fire cause luabind is a nightmare!

#include "LuabindObjectWrapper.h"
#include "luabind/object.hpp"
#include "luabind/detail/class_registry.hpp"
#include "luabind/detail/class_rep.hpp"
#include "luabind/detail/object_rep.hpp"
#include "luabind/detail/ref.hpp"

#include "LuaBindingRegisterDefinitions.h"

#include "SoundSet.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

using namespace RTE;

namespace {
	// True only on the thread that owns LuaMan; a Lua-owned engine object may only be destructed there.
	thread_local bool s_OnSimThread = false;

	std::atomic<uint64_t> s_SimThreadDeletions{0};
	std::atomic<uint64_t> s_OffSimThreadDeletions{0};

	using OwnedDeletion = std::pair<void*, void (*)(void*)>;

	struct SimThreadDeletionQueue {
		std::mutex mutex;
		std::vector<OwnedDeletion> pending;
	};

	// One queue per Lua state, kept past the state it belongs to so a handed-over object still gets deleted.
	std::mutex s_QueuesMutex;
	std::vector<std::unique_ptr<SimThreadDeletionQueue>> s_Queues;

	// luabind gives an object_rep the destructor of its own class_rep, except for a Lua-side class, which takes its C++ base's.
	void (*OwnedObjectDestructor(const luabind::detail::class_rep* classRep))(void*) {
		if (classRep && classRep->get_class_type() == luabind::detail::class_rep::lua_class) {
			classRep = classRep->bases().size() == 1 ? classRep->bases().front().base : nullptr;
		}
		return classRep ? classRep->destructor() : nullptr;
	}

	// __gc for every luabind instance metatable: a Lua-owned engine object collected off the sim thread is handed to it instead of deleted here.
	int SimThreadGarbageCollector(lua_State* luaState) {
		luabind::detail::object_rep* object = static_cast<luabind::detail::object_rep*>(lua_touserdata(luaState, -1));
		if (object && (object->flags() & luabind::detail::object_rep::owner)) {
			if (s_OnSimThread) {
				++s_SimThreadDeletions;
			} else {
				auto* queue = static_cast<SimThreadDeletionQueue*>(lua_touserdata(luaState, lua_upvalueindex(1)));
				void (*destructor)(void*) = OwnedObjectDestructor(object->crep());
				// A Lua-side class whose __init never built its base still carries class_rep::allocate's sentinel,
				// one past the userdata. luabind deletes nothing for it, and neither may we.
				const char* storage = static_cast<const char*>(lua_touserdata(luaState, -1));
				const char* held = static_cast<const char*>(object->ptr());
				const bool separate = held < storage || held > storage + lua_objlen(luaState, -1);
				if (queue && destructor && separate) {
					std::lock_guard<std::mutex> lock(queue->mutex);
					queue->pending.emplace_back(object->ptr(), destructor);
					object->set_flags(object->flags() & ~luabind::detail::object_rep::owner);
				} else {
					++s_OffSimThreadDeletions;
				}
			}
		}
		return luabind::detail::object_rep::garbage_collector(luaState);
	}
} // namespace

void LuabindObjectWrapper::SetSimThread() {
	s_OnSimThread = true;
}

void LuabindObjectWrapper::InstallSimThreadDeletion(lua_State* luaState, int stateIndex) {
	SimThreadDeletionQueue* queue = nullptr;
	if (stateIndex >= 0) {
		std::lock_guard<std::mutex> lock(s_QueuesMutex);
		if (static_cast<size_t>(stateIndex) >= s_Queues.size()) {
			s_Queues.resize(stateIndex + 1);
		}
		if (!s_Queues[stateIndex]) {
			s_Queues[stateIndex] = std::make_unique<SimThreadDeletionQueue>();
		}
		queue = s_Queues[stateIndex].get();
	}
	luabind::detail::class_registry* registry = luabind::detail::class_registry::get_registry(luaState);
	const int instanceMetatables[] = {registry->cpp_instance(), registry->lua_instance()};
	for (int metatable: instanceMetatables) {
		luabind::detail::getref(luaState, metatable);
		lua_pushstring(luaState, "__gc");
		lua_pushlightuserdata(luaState, queue);
		lua_pushcclosure(luaState, &SimThreadGarbageCollector, 1);
		lua_rawset(luaState, -3);
		lua_pop(luaState, 1);
	}
}

void LuabindObjectWrapper::ApplyQueuedEntityDeletions() {
	// State index order, then each state's finalizer order, so every peer deletes the same objects in the same order.
	for (size_t index = 0;; ++index) {
		SimThreadDeletionQueue* queue = nullptr;
		{
			std::lock_guard<std::mutex> lock(s_QueuesMutex);
			if (index >= s_Queues.size()) {
				break;
			}
			queue = s_Queues[index].get();
		}
		if (!queue) {
			continue;
		}
		std::vector<OwnedDeletion> pending;
		{
			std::lock_guard<std::mutex> lock(queue->mutex);
			pending.swap(queue->pending);
		}
		for (const auto& [object, destructor]: pending) {
			destructor(object);
			++(s_OnSimThread ? s_SimThreadDeletions : s_OffSimThreadDeletions);
		}
	}
}

uint64_t LuabindObjectWrapper::SimThreadDeletionCount() {
	return s_SimThreadDeletions.load();
}

uint64_t LuabindObjectWrapper::OffSimThreadDeletionCount() {
	return s_OffSimThreadDeletions.load();
}

// With multithreaded Lua, objects can be destructed from multiple threads at once
// This is okay, but LuaBind wants to do some management on the lua state when one of it's objects is deleted
// This means that potentially an object being deleted by one lua state actually exists in another lua state
// And upon deletion, it's unsafe for LuaBind to poke at the state until we're out the multithreaded context
// As such, we don't actually delete the object until we're in a safe environment outside the multithreaded parts
// Note - this is required even though we force objects in multithreaded environments to be within our Lua state
// This is because we may assign an object to another state in a singlethreaded context, before the GC runs in the multithreaded context
static std::vector<luabind::adl::object*> s_QueuedDeletions;

void LuabindObjectWrapper::ApplyQueuedDeletions() {
	for (luabind::adl::object* obj: s_QueuedDeletions) {
		delete obj;
	}

	s_QueuedDeletions.clear();
}

// Set while a preview window tracks the wrappers it hands out, so a wrapper that dies inside one is forgotten.
static void (*s_PreviewDeletionHook)(LuabindObjectWrapper*) = nullptr;

void LuabindObjectWrapper::SetPreviewDeletionHook(void (*hook)(LuabindObjectWrapper*)) {
	s_PreviewDeletionHook = hook;
}

void LuabindObjectWrapper::ResetLuabindObject(luabind::adl::object* newLuabindObject, bool ownsObject) {
	RTEAssert(s_OnSimThread, "A luabind object was replaced off the sim thread, where luabind may not touch the state.");
	if (m_OwnsObject) {
		delete m_LuabindObject;
	}
	m_LuabindObject = newLuabindObject;
	m_OwnsObject = ownsObject;
}

LuabindObjectWrapper::~LuabindObjectWrapper() {
	if (s_PreviewDeletionHook) {
		s_PreviewDeletionHook(this);
	}
	if (m_OwnsObject) {
		static std::mutex mut;
		std::lock_guard<std::mutex> guard(mut);
		s_QueuedDeletions.push_back(m_LuabindObject);
	}
}

luabind::adl::object GetCopyForStateInternal(const luabind::adl::object& obj, lua_State& targetState) {
	if (obj.is_valid()) {
		int type = luabind::type(obj);
		if (type == LUA_TNUMBER) {
			return luabind::adl::object(&targetState, luabind::object_cast<double>(obj));
		} else if (type == LUA_TBOOLEAN) {
			return luabind::adl::object(&targetState, luabind::object_cast<bool>(obj));
		} else if (type == LUA_TSTRING) {
			return luabind::adl::object(&targetState, luabind::object_cast<std::string>(obj));
		} else if (type == LUA_TTABLE) {
			luabind::object table = luabind::newtable(&targetState);
			for (luabind::iterator itr(obj), itrEnd; itr != itrEnd; ++itr) {
				table[GetCopyForStateInternal(itr.key(), targetState)] = GetCopyForStateInternal(*itr, targetState);
			}
			return table;
		} else if (type == LUA_TUSERDATA) {
#define PER_LUA_BINDING(Type) \
	if (boost::optional<Type*> boundObject = luabind::object_cast_nothrow<Type*>(obj)) { \
		return luabind::adl::object(&targetState, boundObject.get()); \
	}

			LIST_OF_LUABOUND_OBJECTS
#undef PER_LUA_BINDING
		}
	}

	// Dear god, I hope this is safe and equivalent to nil, because I can't find another way of doing it.
	return luabind::adl::object();
}

LuabindObjectWrapper LuabindObjectWrapper::GetCopyForState(lua_State& targetState) const {
	luabind::adl::object* copy = new luabind::adl::object(GetCopyForStateInternal(*m_LuabindObject, targetState));
	return LuabindObjectWrapper(copy, m_FilePath, true);
}
