#pragma once

#include "CheckpointLuaView.h"

#include <array>
#include <map>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

// Include after the live ScriptGraph helpers in LuaMan.cpp.
namespace RTE::CheckpointLua {

	class CaptureScope;

	class NativeImage {
	public:
		struct Value {
			TValue token{};
			std::optional<CheckpointText> text;
			void Push(lua_State* destination, View& view) const {
				if (text) {
					const std::string& bytes = text->Text();
					lua_pushlstring(destination, bytes.data(), bytes.size());
				} else view.Push(destination, token);
			}
		};

		struct Result {
			std::vector<Value> values;
			std::vector<uint64_t> carriedSounds;
			std::string error;
			int Push(lua_State* destination, View& view, std::unordered_set<uint64_t>* carried) const {
				if (!error.empty()) throw std::runtime_error(error);
				if (!carriedSounds.empty() && !carried) throw std::runtime_error("frozen native sound observations have no collector");
				if (carried) carried->insert(carriedSounds.begin(), carriedSounds.end());
				for (const Value& value: values) value.Push(destination, view);
				return static_cast<int>(values.size());
			}
		};

		void Bind(lua_State* destination, View& view) const {
			for (const char* name: {
			         "_ScriptGraphNative", "_ScriptGraphNativeAddress", "_ScriptGraphMembers", "_ScriptGraphInstance",
			         "_ScriptGraphNativeSave", "_ScriptGraphAreaBoxes", "_ScriptGraphGibOwner", "_ScriptGraphSoundSetOwner",
			         "_ScriptGraphLimbOwner", "_ScriptGraphPropertyOwner", "_ScriptGraphLimbVectorOwner",
			         "_ScriptGraphSceneBoxOwner", "_ScriptGraphIteratorSnapshot", "_ScriptGraphGibReferences"}) {
				view.BindNative(destination, name);
			}
			view.scratch.insert(m_Scratch.begin(), m_Scratch.end());
		}

		int Call(lua_State* destination, View& view, std::string_view name, std::unordered_set<uint64_t>* carried = nullptr, std::unordered_set<uintptr_t>* carriedObjects = nullptr) const {
			if (name == "_ScriptGraphGibReferences") return GibReferences(destination, view);
			const auto value = view.Value(destination, 1);
			if (name == "_ScriptGraphIteratorSnapshot") {
				if (!value) return 0;
				if (!tvisfunc(&*value)) throw std::runtime_error("frozen iterator snapshot requires a function");
				// Only iterators were described; the live helper answers any other function with nothing.
				const auto found = m_Iterators.find(gcval(&*value));
				return found == m_Iterators.end() ? 0 : found->second.Push(destination, view, carried);
			}
			if (!value || !tvisudata(&*value)) {
				if (name == "_ScriptGraphMembers" || name == "_ScriptGraphInstance" || name == "_ScriptGraphNative" || name == "_ScriptGraphNativeAddress") {
					lua_pushnil(destination);
					return 1;
				}
				throw std::runtime_error("frozen native helper requires captured userdata: " + std::string(name));
			}
			const auto found = m_Entries.find(gcval(&*value));
			if (found == m_Entries.end()) throw std::runtime_error("frozen userdata has no native descriptor");
			const Entry& entry = found->second;
			if (name == "_ScriptGraphNative") {
				if (carriedObjects && entry.carriesCopy) carriedObjects->insert(entry.movable);
				return entry.native[lua_toboolean(destination, 2) ? 1 : 0].Push(destination, view, carried);
			}
			if (name == "_ScriptGraphMembers") return Members(destination, view, entry.members);
			if (name == "__index") {
				if (lua_type(destination, 2) != LUA_TSTRING) throw std::runtime_error("frozen native property name is not a string");
				size_t size = 0;
				const char* bytes = lua_tolstring(destination, 2, &size);
				const auto property = entry.properties.find(std::string(bytes, size));
				if (property == entry.properties.end()) throw std::runtime_error("frozen native property was not captured: " + std::string(bytes, size));
				return property->second.Push(destination, view, carried);
			}
			const auto result = entry.helpers.find(std::string(name));
			if (result == entry.helpers.end()) throw std::runtime_error("frozen native helper was not captured: " + std::string(name));
			return result->second.Push(destination, view, carried);
		}

		void NoteCarried(lua_State* destination, View& view, std::unordered_set<uintptr_t>& carriedObjects) const {
			const auto value = view.Value(destination, 1);
			if (!value || !tvisudata(&*value)) return;
			if (const auto found = m_Entries.find(gcval(&*value)); found != m_Entries.end() && found->second.movable) carriedObjects.insert(found->second.movable);
		}

		// The live walk's last check: a registered script-owned object with borrowed references that no root reached.
		std::vector<std::string> UnreachedOwners(const std::unordered_set<uintptr_t>& carriedObjects) const {
			std::vector<std::string> problems;
			for (const auto& [address, entry]: m_Entries) {
				if (!entry.ownedRegistered || !entry.borrows || carriedObjects.contains(entry.movable)) continue;
				problems.push_back("a script-owned " + entry.className + " (" + entry.presetName + ") that no script graph root reaches");
			}
			std::sort(problems.begin(), problems.end());
			return problems;
		}

		size_t EntryCount() const { return m_Entries.size(); }
		size_t IteratorCount() const { return m_Iterators.size(); }
		size_t OwnedCount() const { return m_Owned.size(); }

	private:
		using NativeId = uintptr_t;
		struct Entry {
			std::array<Result, 2> native;
			Result members;
			std::unordered_map<std::string, Result> helpers;
			std::unordered_map<std::string, Result> properties;
			NativeId movable = 0;
			bool carriesCopy = false;
			// A script-owned object the live world registers must be reached by a root or refused.
			bool ownedRegistered = false;
			bool borrows = false;
			std::string className;
			std::string presetName;
		};
		struct GibLink { int index = 0; NativeId particle = 0; };
		struct Object {
			long uid = 0;
			bool rotating = false;
			std::vector<NativeId> children;
			std::vector<GibLink> gibs;
		};
		using Topology = std::unordered_map<NativeId, Object>;
		// The world's trees are walked once per capture and shared by every state's image.
		struct World {
			std::vector<NativeId> objects;
			Topology topology;
		};
		std::unordered_map<const void*, Entry> m_Entries;
		std::unordered_map<const void*, Result> m_Iterators;
		std::unordered_set<const void*> m_Scratch;
		std::shared_ptr<const World> m_World;
		Topology m_Owned;
		const Object* Find(NativeId identity) const {
			if (const auto owned = m_Owned.find(identity); owned != m_Owned.end()) return &owned->second;
			if (m_World) {
				if (const auto world = m_World->topology.find(identity); world != m_World->topology.end()) return &world->second;
			}
			return nullptr;
		}

		template<class Visit> static void TableEntries(const Snapshot& heap, const GCtab* source, Visit visit) {
			const GCtab table = heap.Read(source);
			const TValue* array = mref(table.array, TValue);
			for (MSize index = 0; index < table.asize; ++index) {
				const TValue value = heap.Read(array + index);
				if (!tvisnil(&value)) { TValue key; setintV(&key, static_cast<int32_t>(index)); visit(key, value); }
			}
			const Node* nodes = mref(table.node, Node);
			for (MSize index = 0; index <= table.hmask; ++index) {
				const Node node = heap.Read(nodes + index);
				if (!tvisnil(&node.val)) visit(node.key, node.val);
			}
		}

		static int Members(lua_State* destination, View& view, const Result& sources) {
			if (!sources.error.empty()) throw std::runtime_error(sources.error);
			if (sources.values.empty()) { lua_pushnil(destination); return 1; }
			lua_newtable(destination);
			const int result = lua_gettop(destination);
			for (const Value& source: sources.values) {
				if (source.text || !tvistab(&source.token)) throw std::runtime_error("frozen native members contain a non-table source");
				TableEntries(view.Heap(), tabV(&source.token), [&](const TValue& key, const TValue& value) {
					view.Push(destination, key);
					view.Push(destination, value);
					lua_rawset(destination, result);
				});
			}
			return 1;
		}

		int GibReferences(lua_State* destination, View& view) const {
			luaL_checktype(destination, 1, LUA_TTABLE);
			std::unordered_set<NativeId> seen;
			std::map<long, NativeId> owners;
			std::function<void(NativeId)> collect = [&](NativeId identity) {
				if (!identity || !seen.insert(identity).second) return;
				const Object* object = Find(identity);
				if (!object) throw std::runtime_error("frozen gib owner has no native topology");
				if (object->rotating && object->uid > 0) owners.emplace(object->uid, identity);
				for (NativeId child: object->children) collect(child);
			};
			if (m_World) for (NativeId identity: m_World->objects) collect(identity);
			lua_pushnil(destination);
			while (lua_next(destination, 1)) {
				const auto value = view.Value(destination, -1);
				if (value && tvisudata(&*value)) {
					const auto entry = m_Entries.find(gcval(&*value));
					if (entry == m_Entries.end()) throw std::runtime_error("frozen gib target has no native descriptor");
					collect(entry->second.movable);
				}
				lua_pop(destination, 1);
			}
			lua_newtable(destination);
			const int result = lua_gettop(destination);
			int count = 0;
			for (const auto& [uid, identity]: owners) {
				for (const GibLink& link: Find(identity)->gibs) {
					lua_pushlightuserdata(destination, reinterpret_cast<void*>(link.particle));
					lua_rawget(destination, 1);
					if (!lua_isnil(destination, -1)) {
						lua_newtable(destination);
						lua_pushnumber(destination, static_cast<lua_Number>(uid)); lua_setfield(destination, -2, "owner");
						lua_pushinteger(destination, link.index); lua_setfield(destination, -2, "index");
						lua_pushvalue(destination, -2); lua_setfield(destination, -2, "target");
						lua_rawseti(destination, result, ++count);
					}
					lua_pop(destination, 1);
				}
			}
			return 1;
		}

		friend class CaptureScope;
	};

	// The caller holds the VM lock and keeps carried sound observations enabled.
	class CaptureScope {
	public:
		explicit CaptureScope(lua_State* state) : m_References(state), m_Capture(true), m_Thread(std::this_thread::get_id()) {}
		CaptureScope(const CaptureScope&) = delete;
		CaptureScope& operator=(const CaptureScope&) = delete;

		void Capture() {
			CheckThread();
			if (m_Captured || !m_Image) throw std::logic_error("native image capture cannot be repeated");
			if (!s_GraphNativeCapture) m_NativeScope.emplace();
			std::vector<TValue> roots;
			LuaThreadCodec::VisitUserdata(State(), [](void* data, size_t, const void*, void* opaque) {
				TValue value;
				setgcVraw(&value, reinterpret_cast<GCobj*>(static_cast<GCudata*>(data) - 1), LJ_TUDATA);
				static_cast<std::vector<TValue>*>(opaque)->push_back(value);
			}, &roots);
			for (GCobj* object = gcref(G(State())->gc.root); object; object = gcnext(object)) {
				if (object->gch.gct == ~LJ_TFUNC && IteratorCandidate(&object->fn)) {
					TValue value; setgcVraw(&value, object, LJ_TFUNC); roots.push_back(value);
				}
			}
			for (const TValue& value: roots) Enqueue(value);
			auto& shared = s_GraphNativeCapture->frozenWorld;
			if (!shared) {
				auto world = std::make_shared<NativeImage::World>();
				for (const MovableObject* object: s_GraphNativeCapture->knownObjects) {
					if (!g_MovableMan.ValidMO(object)) continue;
					world->objects.push_back(reinterpret_cast<uintptr_t>(object));
					Describe(object, world->topology, nullptr);
				}
				shared = std::move(world);
			}
			m_Image->m_World = std::static_pointer_cast<const NativeImage::World>(shared);
			for (size_t index = 0; index < m_Queue.size(); ++index) {
				const TValue value = m_Queue[index];
				if (tvisudata(&value)) CaptureUserdata(value);
				else CaptureIterator(value);
			}
			m_Captured = true;
		}

		std::shared_ptr<const NativeImage> Finish(const Snapshot& heap) {
			CheckThread();
			if (!m_Captured || !m_Image || heap.State() != State()) throw std::logic_error("native results require the same frozen Lua heap");
			std::shared_ptr<const NativeImage> result = std::move(m_Image);
			m_References.Release();
			return result;
		}

	private:
		struct References {
			lua_State* state;
			decltype(std::declval<global_State&>().gc.threshold) threshold;
			uint64_t serial;
			int table = LUA_NOREF;
			int count = 0;
			explicit References(lua_State* source) : state(source), threshold(G(source)->gc.threshold), serial(luaJIT_state_serial(source)) {
				G(state)->gc.threshold = std::numeric_limits<decltype(threshold)>::max();
				lua_newtable(state);
				table = luaL_ref(state, LUA_REGISTRYINDEX);
			}
			~References() { Release(); }
			void Release() {
				if (!state) return;
				luaL_unref(state, LUA_REGISTRYINDEX, table);
				G(state)->gc.threshold = threshold;
				luaJIT_set_state_serial(state, serial);
				state = nullptr;
			}
		} m_References;

		struct NativeEffects {
			RandomGenerator sim = g_SimRNG, render = g_RenderRNG;
			long uid = MovableObject::GetUniqueIDCounter();
			int cursor = g_LuaMan.GetScriptStateCursor();
			CheckpointSoundRegistry sounds = g_AudioMan.CaptureCheckpointSoundRegistry();
			uint64_t soundCursor = g_AudioMan.GetCheckpointSoundContainerCursor();
			std::unordered_set<uint64_t> carried = g_AudioMan.LastCarriedSoundIdentities();
			~NativeEffects() {
				g_SimRNG = sim; g_RenderRNG = render;
				MovableObject::PinUniqueIDCounter(uid); g_LuaMan.SetScriptStateCursor(cursor);
				g_AudioMan.RestoreCheckpointSoundRegistry(std::move(sounds));
				g_AudioMan.SetCheckpointSoundContainerCursor(soundCursor);
				g_AudioMan.RememberCarriedSoundIdentities(std::move(carried));
			}
		} m_Effects;

		ScriptGraphCaptureScope m_Capture;
		std::optional<LuaScriptGraphNativeCaptureScope> m_NativeScope;
		std::thread::id m_Thread;
		std::shared_ptr<NativeImage> m_Image = std::make_shared<NativeImage>();
		std::unordered_set<const void*> m_Queued;
		std::vector<TValue> m_Queue;
		bool m_Captured = false;

		lua_State* State() const { return m_References.state; }
		void CheckThread() const {
			if (std::this_thread::get_id() != m_Thread || !State()) throw std::logic_error("native image builder left its simulation thread");
		}
		void Push(const TValue& value) {
			if (!lua_checkstack(State(), 1)) throw std::runtime_error("native capture exhausted the Lua stack");
			copyTV(State(), State()->top, &value);
			incr_top(State());
		}
		TValue At(int index) const { return *(index > 0 ? State()->base + index - 1 : State()->top + index); }
		void Keep(int index) {
			if (index < 0) index += lua_gettop(State()) + 1;
			lua_rawgeti(State(), LUA_REGISTRYINDEX, m_References.table);
			lua_pushvalue(State(), index);
			lua_rawseti(State(), -2, ++m_References.count);
			lua_pop(State(), 1);
		}
		void Enqueue(const TValue& value) {
			if ((!tvisudata(&value) && !tvisfunc(&value)) || !m_Queued.insert(gcval(&value)).second) return;
			Push(value); Keep(-1); lua_pop(State(), 1);
			m_Queue.push_back(value);
		}
		void NewResults(int index, uint64_t before, uint64_t after, std::unordered_set<const void*>& seen) {
			if (index < 0) index += lua_gettop(State()) + 1;
			const TValue value = At(index);
			if (!tvisudata(&value) && !tvistab(&value) && !tvisfunc(&value) && !tvisthread(&value)) return;
			const uint64_t serial = luaJIT_value_serial(State(), index);
			const bool fresh = serial > before && serial <= after;
			if (fresh) {
				NoteScriptGraphScratch(State(), index);
				m_Image->m_Scratch.insert(gcval(&value));
			}
			if (tvisudata(&value) && !ScriptGraphCapturedText(State(), index)) Enqueue(value);
			if (tvisfunc(&value) && IteratorCandidate(funcV(&value))) Enqueue(value);
			if (!fresh || !tvistab(&value) || !seen.insert(gcval(&value)).second) return;
			lua_pushnil(State());
			while (lua_next(State(), index)) {
				NewResults(-2, before, after, seen);
				NewResults(-1, before, after, seen);
				lua_pop(State(), 1);
			}
		}

		template<class Arguments> NativeImage::Result Invoke(lua_CFunction function, const char* name, Arguments arguments, bool observesNative = false) {
			const int top = lua_gettop(State());
			struct RestoreStack { lua_State* state; int top; ~RestoreStack() { lua_settop(state, top); } } stack{State(), top};
			std::optional<AudioMan::SoundCheckpointSaveScope> notes;
			if (observesNative) notes.emplace();
			const uint64_t before = luaJIT_state_serial(State());
			lua_pushcfunction(State(), function);
			const int count = arguments();
			NativeImage::Result result;
			if (lua_pcall(State(), count, LUA_MULTRET, 0) != 0) {
				const char* reason = lua_tostring(State(), -1);
				result.error = std::string("frozen native helper ") + name + ": " + (reason ? reason : "unknown failure");
				return result;
			}
			const uint64_t after = luaJIT_state_serial(State());
			const int last = lua_gettop(State());
			std::unordered_set<const void*> seen;
			for (int index = top + 1; index <= last; ++index) {
				NativeImage::Value value;
				value.token = At(index);
				if (const auto* text = ScriptGraphCapturedText(State(), index)) value.text = *text;
				Keep(index);
				NewResults(index, before, after, seen);
				result.values.push_back(std::move(value));
			}
			if (notes) result.carriedSounds.assign(notes->Carried().begin(), notes->Carried().end());
			return result;
		}

		NativeImage::Result One(lua_CFunction function, const char* name, const TValue& value, bool observesNative = false) {
			return Invoke(function, name, [&] { Push(value); return 1; }, observesNative);
		}
		static std::string Kind(const NativeImage::Result& result) {
			if (!result.error.empty() || result.values.empty() || result.values.front().text || !tvisstr(&result.values.front().token)) return {};
			const GCstr* text = strV(&result.values.front().token);
			return std::string(strdata(text), text->len);
		}
		static int MemberSources(lua_State* state) {
			if (const auto* object = luabind::detail::is_class_object(state, 1); object && object->crep()) {
				object->crep()->get_table(state);
				if (object->get_lua_table().is_valid()) { object->get_lua_table().get(state); return 2; }
				return 1;
			}
			if (luabind::detail::is_class_rep(state, 1)) {
				static_cast<luabind::detail::class_rep*>(lua_touserdata(state, 1))->get_table(state);
				return 1;
			}
			return 0;
		}
		static int Property(lua_State* state) {
			lua_pushvalue(state, 2);
			lua_gettable(state, 1);
			return 1;
		}
		static int IsIterator(lua_State* state) {
			const bool iterator = lua_tocfunction(state, 1) == ScriptGraphValueIteratorNext || ScriptGraphIteratorHook(state, 1, "__iterator_snapshot");
			lua_pushboolean(state, iterator);
			return 1;
		}

		void CaptureUserdata(const TValue& value) {
			auto& entry = m_Image->m_Entries[gcval(&value)];
			std::string className;
			bool detached = false;
			Push(value);
			if (const auto* object = luabind::detail::is_class_object(State(), -1); object && object->crep()) {
				className = object->crep()->name();
				const bool movable = ClassDerivesFrom(object->crep(), "MovableObject");
				const bool owned = (object->flags() & luabind::detail::object_rep::owner) != 0;
				detached = !object->ptr() && (!movable || owned);
				if (movable && object->ptr()) {
					const auto* mo = static_cast<const MovableObject*>(object->ptr());
					entry.movable = reinterpret_cast<uintptr_t>(mo);
					CaptureObject(mo);
					if (owned) {
						entry.carriesCopy = true;
						entry.ownedRegistered = g_MovableMan.FindObjectByUniqueID(mo->GetUniqueID()) == mo;
						if (entry.ownedRegistered) {
							const std::vector<long> links = mo->GetCheckpointBorrowedReferences();
							entry.borrows = std::any_of(links.begin(), links.end(), [](long target) { return target != 0; });
							entry.className = mo->GetClassName();
							entry.presetName = mo->GetPresetName();
						}
					}
				}
			}
			lua_pop(State(), 1);
			entry.members = One(MemberSources, "Members", value);
			entry.helpers.emplace("_ScriptGraphInstance", One(ScriptGraphInstance, "Instance", value));
			entry.helpers.emplace("_ScriptGraphNativeAddress", One(ScriptGraphNativeAddress, "NativeAddress", value));
			if (detached) {
				for (auto& descriptor: entry.native) descriptor.error = "frozen native capture found detached " + className + " userdata";
				return;
			}
			std::unordered_set<std::string> kinds;
			for (int named = 0; named < 2; ++named) {
				entry.native[named] = Invoke(ScriptGraphNative, "Native", [&] { Push(value); lua_pushboolean(State(), named); return 2; }, true);
				kinds.insert(Kind(entry.native[named]));
			}
			const auto helper = [&](const char* name, lua_CFunction function) { entry.helpers.emplace(name, One(function, name, value)); };
			if (kinds.contains("copy")) entry.helpers.emplace("_ScriptGraphNativeSave", One(ScriptGraphNativeSave, "NativeSave", value, true));
			if (kinds.contains("area-ref") || (kinds.contains("copy") && className == "Area")) helper("_ScriptGraphAreaBoxes", ScriptGraphAreaBoxes);
			if (kinds.contains("gib-ref")) helper("_ScriptGraphGibOwner", ScriptGraphGibOwner);
			if (kinds.contains("soundset-ref")) helper("_ScriptGraphSoundSetOwner", ScriptGraphSoundSetOwner);
			if (kinds.contains("limb-ref")) helper("_ScriptGraphLimbOwner", ScriptGraphLimbOwner);
			if (kinds.contains("box-ref")) helper("_ScriptGraphSceneBoxOwner", ScriptGraphSceneBoxOwner);
			if (kinds.contains("vector-ref-unresolved") || kinds.contains("timer-ref")) {
				helper("_ScriptGraphPropertyOwner", ScriptGraphPropertyOwner);
				helper("_ScriptGraphLimbVectorOwner", ScriptGraphLimbVectorOwner);
			}
			std::vector<const char*> properties;
			if (className == "Vector") properties = {"X", "Y"};
			else if (className == "Timer") properties = {"StartSimTimeTicks", "SimTimeLimitTicks", "StartRealTimeTicks", "RealTimeLimitTicks"};
			else if (className == "AlarmEvent") properties = {"ScenePos", "Team", "Range"};
			for (const char* property: properties) {
				entry.properties.emplace(property, Invoke(Property, property, [&] { Push(value); lua_pushstring(State(), property); return 2; }));
			}
		}

		// The only native functions the walk asks about are the value iterators and the hooks with a userdata upvalue.
		static bool IteratorCandidate(const GCfunc* function) {
			return iscfunc(function) && (function->c.f == ScriptGraphValueIteratorNext || (function->c.nupvalues && tvisudata(&function->c.upvalue[0])));
		}

		void CaptureIterator(const TValue& value) {
			NativeImage::Result result = One(IsIterator, "IteratorProbe", value);
			if (result.error.empty()) {
				if (result.values.empty() || !tvistrue(&result.values.front().token)) return;
				result = One(ScriptGraphIteratorSnapshot, "IteratorSnapshot", value);
			}
			m_Image->m_Iterators.emplace(gcval(&value), std::move(result));
		}

		void CaptureObject(const MovableObject* source) {
			Describe(source, m_Image->m_Owned, m_Image->m_World ? &m_Image->m_World->topology : nullptr);
		}

		static void Describe(const MovableObject* source, NativeImage::Topology& into, const NativeImage::Topology* shared) {
			if (!source) return;
			const uintptr_t identity = reinterpret_cast<uintptr_t>(source);
			if (into.contains(identity) || (shared && shared->contains(identity))) return;
			into.emplace(identity, NativeImage::Object{});
			NativeImage::Object object;
			object.uid = source->GetUniqueID();
			const auto child = [&](const MovableObject* part) {
				object.children.push_back(reinterpret_cast<uintptr_t>(part));
				Describe(part, into, shared);
			};
			if (const auto* rotating = dynamic_cast<const MOSRotating*>(source)) {
				object.rotating = true;
				for (const Attachable* part: rotating->GetAttachables()) child(part);
				for (const AEmitter* wound: rotating->GetWoundList()) child(wound);
				int index = 0;
				for (const Gib* gib: *rotating->GetGibList()) {
					const MovableObject* particle = gib->GetParticlePreset();
					if (particle && !particle->IsOriginalPreset() && particle->GetUniqueID() == 0) object.gibs.push_back({index, reinterpret_cast<uintptr_t>(particle)});
					++index;
				}
			}
			if (const auto* actor = dynamic_cast<const Actor*>(source)) for (const MovableObject* part: *actor->GetInventory()) child(part);
			if (const auto* craft = dynamic_cast<const ACraft*>(source)) for (const MovableObject* part: craft->GetCollectedInventory()) child(part);
			into.at(identity) = std::move(object);
		}
	};
}
