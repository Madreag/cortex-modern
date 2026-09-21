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
		//!< The object's MovableObject base, worked out from luabind's class metadata at handover
		//!< without reading a single sim field. Null for what is not a MovableObject.
		const RTE::MovableObject* movableObject = nullptr;
		//!< The order the collectors handed objects over in, for what carries no unique ID. One
		//!< global counter, so the per-state queue an object sat in never decides anything.
		uint64_t handoverSerial = 0;
	};

	std::atomic<uint64_t> s_NextHandoverSerial{0};

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

	// A handed-over object's MovableObject base, or null. Pointer arithmetic over luabind's own class
	// metadata: nothing of the sim is read here, on the collecting thread. The unique ID itself is read
	// at the drain, on the sim thread, while the object is still alive.
	const RTE::MovableObject* QueuedDeletionMovableObject(lua_State* luaState, luabind::detail::object_rep* object) {
		luabind::detail::class_registry* registry = luabind::detail::class_registry::get_registry(luaState);
		const luabind::detail::class_rep* movableObject = registry ? registry->find_class(LUABIND_TYPEID(RTE::MovableObject)) : nullptr;
		const int offset = movableObject ? BaseClassOffset(object->crep(), movableObject) : -1;
		if (offset < 0 || !object->ptr()) {
			return nullptr;
		}
		return reinterpret_cast<const RTE::MovableObject*>(static_cast<const char*>(object->ptr()) + offset);
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
					const RTE::MovableObject* asMovableObject = QueuedDeletionMovableObject(luaState, object);
					const uint64_t serial = s_NextHandoverSerial.fetch_add(1, std::memory_order_relaxed);
					std::lock_guard<std::mutex> lock(queue->mutex);
					queue->pending.push_back({object->ptr(), destructor, asMovableObject, serial});
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
	// Every key is read here, on the sim thread, with the object still alive: the sim's unique ID where
	// there is one, the handover serial for the rest. Both are counts of the world, never of the queues.
	struct KeyedDeletion {
		long uniqueID = 0;
		uint64_t handoverSerial = 0;
		OwnedDeletion entry;
	};
	std::vector<KeyedDeletion> keyed;
	keyed.reserve(pending.size());
	for (const OwnedDeletion& entry: pending) {
		keyed.push_back({entry.movableObject ? entry.movableObject->GetUniqueID() : 0, entry.handoverSerial, entry});
	}
	std::sort(keyed.begin(), keyed.end(), [](const KeyedDeletion& lhs, const KeyedDeletion& rhs) {
		if (lhs.uniqueID != rhs.uniqueID) {
			return lhs.uniqueID < rhs.uniqueID;
		}
		return lhs.handoverSerial < rhs.handoverSerial;
	});
	for (const KeyedDeletion& keyedEntry: keyed) {
		keyedEntry.entry.destructor(keyedEntry.entry.object);
		++(s_OnSimThread ? s_SimThreadDeletions : s_OffSimThreadDeletions);
	}
}

namespace {
	std::vector<long> s_QueuedDeletionSelfTestOrder;

	// Stands in for a handed-over object's destructor: records which one ran, and deletes nothing. The
	// pointer it is handed is the object, so the row reads the same identity the drain sorted on.
	void RecordQueuedDeletionSelfTestOrder(void* object) {
		s_QueuedDeletionSelfTestOrder.push_back(static_cast<RTE::MovableObject*>(object)->GetUniqueID());
	}
} // namespace

std::string LuabindObjectWrapper::RunQueuedDeletionOrderSelfTest(int stateCount) {
	// Eight real objects whose unique-ID order is not their queue order, spread over the states the way
	// the collector spreads them: the drain order must come out the same however many queues there are,
	// and the key has to be read here, at the drain, off the object rather than stored at handover.
	constexpr int c_Objects = 8;
	const long savedCounter = RTE::MovableObject::GetUniqueIDCounter();
	std::vector<std::unique_ptr<RTE::MOPixel>> objects;
	objects.reserve(c_Objects);
	for (int object = 0; object < c_Objects; ++object) {
		auto created = std::make_unique<RTE::MOPixel>();
		if (created->Create() < 0) {
			return "";
		}
		objects.push_back(std::move(created));
	}
	const long firstUniqueID = objects.front()->GetUniqueID();
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
		// Handed over highest unique ID first, and spread over the queues by the state count.
		RTE::MOPixel* handed = objects[static_cast<size_t>(c_Objects - 1 - object)].get();
		s_Queues[static_cast<size_t>(object % stateCount)]->pending.push_back({handed, &RecordQueuedDeletionSelfTestOrder, handed, static_cast<uint64_t>(object)});
	}
	s_QueuedDeletionSelfTestOrder.clear();
	const uint64_t savedSimThreadDeletions = s_SimThreadDeletions.load();
	const uint64_t savedOffSimThreadDeletions = s_OffSimThreadDeletions.load();
	ApplyQueuedEntityDeletions();
	s_SimThreadDeletions.store(savedSimThreadDeletions);
	s_OffSimThreadDeletions.store(savedOffSimThreadDeletions);
	std::string order;
	for (long uniqueID: s_QueuedDeletionSelfTestOrder) {
		// Named 1..8 by creation, so the row reads the same however many objects the run made before it.
		order += (order.empty() ? "" : ",") + std::to_string(uniqueID - firstUniqueID + 1);
	}
	s_QueuedDeletionSelfTestOrder.clear();
	{
		std::lock_guard<std::mutex> lock(s_QueuesMutex);
		s_Queues.swap(saved);
	}
	objects.clear();
	RTE::MovableObject::PinUniqueIDCounter(savedCounter);
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
