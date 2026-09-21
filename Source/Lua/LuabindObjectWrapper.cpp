// Make sure that this wrapper file is always set to NOT use pre-compiled headers and conformance mode (/permissive) otherwise everything will be on fire cause luabind is a nightmare!

#include "LuabindObjectWrapper.h"
#include "luabind/object.hpp"
#include "luabind/detail/class_registry.hpp"
#include "luabind/detail/class_rep.hpp"
#include "luabind/detail/object_rep.hpp"
#include "luabind/detail/ref.hpp"

#include "LuaBindingRegisterDefinitions.h"

#include "SoundSet.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <mutex>
#include <utility>
#include <vector>

using namespace RTE;

namespace {
	// True only on the thread that owns LuaMan; a Lua-owned engine object may only be destructed there.
	thread_local bool s_OnSimThread = false;

	std::atomic<uint64_t> s_SimThreadDeletions{0};
	std::atomic<uint64_t> s_OffSimThreadDeletions{0};

	struct OwnedDeletion {
		void* object = nullptr;
		void (*destructor)(void*) = nullptr;
		long uniqueID = 0; //!< The sim's identity, read while the object is still alive.
	};

	struct SimThreadDeletionQueue {
		std::mutex mutex;
		std::vector<OwnedDeletion> pending;
	};

	// One queue per Lua state, kept past the state it belongs to so a handed-over object still gets deleted.
	std::mutex s_QueuesMutex;
	std::vector<std::unique_ptr<SimThreadDeletionQueue>> s_Queues;

	// The wrapper queue's lock, shared by the destructor that fills it and the drain that empties it.
	std::mutex s_QueuedDeletionsMutex;
	// The states lua_close has already run on. A queued luabind object naming one of these may not be
	// deleted: its destructor unrefs the registry of a lua_State that no longer exists.
	std::vector<lua_State*> s_ClosedStates;
	uint64_t s_QueuedDeletionsDrainedAtStateClose = 0;
	uint64_t s_QueuedDeletionsNamingAClosedState = 0;

	// luabind gives an object_rep the destructor of its own class_rep, except for a Lua-side class, which takes its C++ base's.
	void (*OwnedObjectDestructor(const luabind::detail::class_rep* classRep))(void*) {
		if (classRep && classRep->get_class_type() == luabind::detail::class_rep::lua_class) {
			classRep = classRep->bases().size() == 1 ? classRep->bases().front().base : nullptr;
		}
		return classRep ? classRep->destructor() : nullptr;
	}

	// The offset from an object of `from` to its `target` base, or -1 when `target` is not a base of it.
	int BaseClassOffset(const luabind::detail::class_rep* from, const luabind::detail::class_rep* target) {
		if (!from) {
			return -1;
		}
		if (from == target) {
			return 0;
		}
		for (const luabind::detail::class_rep::base_info& base: from->bases()) {
			const int offset = BaseClassOffset(base.base, target);
			if (offset >= 0) {
				return offset + base.pointer_offset;
			}
		}
		return -1;
	}

	// A handed-over object's unique ID, read while it is still alive: the drain has to run in one order
	// on every peer, and by then the queue holds nothing but a pointer. 0 for what is not a MovableObject.
	long QueuedDeletionKey(lua_State* luaState, luabind::detail::object_rep* object) {
		luabind::detail::class_registry* registry = luabind::detail::class_registry::get_registry(luaState);
		const luabind::detail::class_rep* movableObject = registry ? registry->find_class(LUABIND_TYPEID(RTE::MovableObject)) : nullptr;
		const int offset = movableObject ? BaseClassOffset(object->crep(), movableObject) : -1;
		if (offset < 0 || !object->ptr()) {
			return 0;
		}
		return reinterpret_cast<const RTE::MovableObject*>(static_cast<const char*>(object->ptr()) + offset)->GetUniqueID();
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
					const long uniqueID = QueuedDeletionKey(luaState, object);
					std::lock_guard<std::mutex> lock(queue->mutex);
					queue->pending.push_back({object->ptr(), destructor, uniqueID});
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
	// A new state can open at the address a closed one had; from here that address is live again.
	std::erase(s_ClosedStates, luaState);
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
	// Unique-ID order across every state's queue, so a destructor that writes shared state runs in the
	// same place on a peer with a different state count. A queue index is that machine's own business.
	std::vector<OwnedDeletion> pending;
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
		std::lock_guard<std::mutex> lock(queue->mutex);
		pending.insert(pending.end(), queue->pending.begin(), queue->pending.end());
		queue->pending.clear();
	}
	// Stable, so what carries no unique ID keeps the order it was handed over in.
	std::stable_sort(pending.begin(), pending.end(), [](const OwnedDeletion& lhs, const OwnedDeletion& rhs) { return lhs.uniqueID < rhs.uniqueID; });
	for (const auto& [object, destructor, uniqueID]: pending) {
		destructor(object);
		++(s_OnSimThread ? s_SimThreadDeletions : s_OffSimThreadDeletions);
	}
}

namespace {
	std::vector<long> s_QueuedDeletionSelfTestOrder;

	// Stands in for a handed-over object's destructor: records which one ran, and deletes nothing.
	void RecordQueuedDeletionSelfTestOrder(void* object) {
		s_QueuedDeletionSelfTestOrder.push_back(static_cast<long>(reinterpret_cast<intptr_t>(object)));
	}
} // namespace

std::string LuabindObjectWrapper::RunQueuedDeletionOrderSelfTest(int stateCount) {
	// Eight objects whose unique-ID order is not their queue order, spread over the states the way the
	// collector spreads them: the drain order must come out the same however many queues there are.
	constexpr int c_Objects = 8;
	std::vector<std::unique_ptr<SimThreadDeletionQueue>> saved;
	{
		std::lock_guard<std::mutex> lock(s_QueuesMutex);
		saved.swap(s_Queues);
		s_Queues.resize(static_cast<size_t>(stateCount));
		for (auto& queue: s_Queues) {
			queue = std::make_unique<SimThreadDeletionQueue>();
		}
	}
	for (int object = 0; object < c_Objects; ++object) {
		const long uniqueID = c_Objects - object;
		s_Queues[static_cast<size_t>(object % stateCount)]->pending.push_back({reinterpret_cast<void*>(static_cast<intptr_t>(uniqueID)), &RecordQueuedDeletionSelfTestOrder, uniqueID});
	}
	s_QueuedDeletionSelfTestOrder.clear();
	const uint64_t savedSimThreadDeletions = s_SimThreadDeletions.load();
	const uint64_t savedOffSimThreadDeletions = s_OffSimThreadDeletions.load();
	ApplyQueuedEntityDeletions();
	s_SimThreadDeletions.store(savedSimThreadDeletions);
	s_OffSimThreadDeletions.store(savedOffSimThreadDeletions);
	std::string order;
	for (long uniqueID: s_QueuedDeletionSelfTestOrder) {
		order += (order.empty() ? "" : ",") + std::to_string(uniqueID);
	}
	s_QueuedDeletionSelfTestOrder.clear();
	{
		std::lock_guard<std::mutex> lock(s_QueuesMutex);
		s_Queues.swap(saved);
	}
	return order;
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
	// Deleting one object can destruct engine objects that queue wrappers of their own, so take the
	// queue by swap and keep going until nothing new arrives.
	std::vector<luabind::adl::object*> draining;
	for (;;) {
		{
			std::lock_guard<std::mutex> guard(s_QueuedDeletionsMutex);
			if (s_QueuedDeletions.empty()) {
				return;
			}
			draining.swap(s_QueuedDeletions);
			s_QueuedDeletions.clear();
		}
		for (luabind::adl::object* obj: draining) {
			if (obj && std::find(s_ClosedStates.begin(), s_ClosedStates.end(), obj->interpreter()) != s_ClosedStates.end()) {
				++s_QueuedDeletionsNamingAClosedState;
				continue;
			}
			delete obj;
		}
		draining.clear();
	}
}

uint64_t LuabindObjectWrapper::DrainQueuedDeletionsBeforeStateClose(lua_State* luaState) {
	uint64_t held = 0;
	{
		std::lock_guard<std::mutex> guard(s_QueuedDeletionsMutex);
		for (const luabind::adl::object* obj: s_QueuedDeletions) {
			if (obj && obj->interpreter() == luaState) {
				++held;
			}
		}
	}
	s_QueuedDeletionsDrainedAtStateClose += held;
	ApplyQueuedDeletions();
	// A state's address can be handed out again by the next luaL_newstate, so the record is per address
	// and InstallSimThreadDeletion takes it back off the list when a new state opens there.
	if (std::find(s_ClosedStates.begin(), s_ClosedStates.end(), luaState) == s_ClosedStates.end()) {
		s_ClosedStates.push_back(luaState);
	}
	return held;
}

uint64_t LuabindObjectWrapper::QueuedDeletionsDrainedAtStateClose() {
	return s_QueuedDeletionsDrainedAtStateClose;
}

uint64_t LuabindObjectWrapper::QueuedDeletionsNamingAClosedState() {
	return s_QueuedDeletionsNamingAClosedState;
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
		std::lock_guard<std::mutex> guard(s_QueuedDeletionsMutex);
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
