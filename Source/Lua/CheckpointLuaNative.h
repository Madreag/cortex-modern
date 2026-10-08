#pragma once

#include "CheckpointLuaView.h"
#include "CheckpointLuaAddresses.h"
#include "CaptureSentinel.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <exception>
#include <functional>
#include <map>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Include after the live ScriptGraph helpers in LuaMan.cpp.
namespace RTE::CheckpointLua {

	class CaptureScope;

	// The counters a native capture must leave as it found them, put back when it ends.
	struct NativeEffects {
		RandomGenerator sim = g_SimRNG, render = g_RenderRNG;
		long uid = MovableObject::GetUniqueIDCounter();
		// The sound registry is restored once per world capture, as the live walk has it.
		uint64_t soundCursor = g_AudioMan.GetCheckpointSoundContainerCursor();
		std::unordered_set<uint64_t> carried = g_AudioMan.LastCarriedSoundIdentities();
		~NativeEffects() {
			g_SimRNG = sim; g_RenderRNG = render;
			MovableObject::PinUniqueIDCounter(uid);
			g_AudioMan.SetCheckpointSoundContainerCursor(soundCursor);
			g_AudioMan.RememberCarriedSoundIdentities(std::move(carried));
		}
	};

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
			const Entry* found = FindEntry(gcval(&*value));
			if (!found) throw std::runtime_error("frozen userdata has no native descriptor");
			const Entry& entry = *found;
			if (name == "_ScriptGraphNative") {
				if (carriedObjects && entry.carriesCopy) carriedObjects->insert(entry.movable);
				return entry.native[lua_toboolean(destination, 2) ? 1 : 0].Push(destination, view, carried);
			}
			if (name == "_ScriptGraphMembers") return Members(destination, view, entry.members);
			if (name == "__index") {
				if (lua_type(destination, 2) != LUA_TSTRING) throw std::runtime_error("frozen native property name is not a string");
				size_t size = 0;
				const char* bytes = lua_tolstring(destination, 2, &size);
				const auto property = entry.properties.find(std::string_view(bytes, size));
				if (property == entry.properties.end()) throw std::runtime_error("frozen native property was not captured: " + std::string(bytes, size));
				return property->second.Push(destination, view, carried);
			}
			const auto result = entry.helpers.find(name);
			if (result == entry.helpers.end()) throw std::runtime_error("frozen native helper was not captured: " + std::string(name));
			return result->second.Push(destination, view, carried);
		}

		void NoteCarried(lua_State* destination, View& view, std::unordered_set<uintptr_t>& carriedObjects) const {
			const auto value = view.Value(destination, 1);
			if (!value || !tvisudata(&*value)) return;
			if (const Entry* found = FindEntry(gcval(&*value)); found && found->movable) carriedObjects.insert(found->movable);
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

		size_t EntryCount() const { return m_Entries.size() + m_Scalars.size() + (m_Classes ? m_Classes->Size() : 0); }
		// Compare the actual frozen native answers, including deferred text, before
		// reusing a chunk. No live writer barrier is borrowed by the saver VM.
		std::string Fingerprint(const void* address, View& view, std::string_view helper, std::string_view argument,
		                        const std::function<void(const TValue&)>& noteTable = {}) const {
			std::string bytes;
			const auto word = [&bytes](const auto& value) { bytes.append(reinterpret_cast<const char*>(&value), sizeof(value)); };
			const auto text = [&bytes, &word](const std::string& value) { word(value.size()); bytes += value; };
			const auto result = [&](const Result& answer) {
				text(answer.error); word(answer.values.size());
				for (const Value& value: answer.values) {
					word(value.text.has_value());
					text(value.text ? value.text->Text() : view.Token(value.token));
					if (!value.text && tvistab(&value.token) && noteTable) noteTable(value.token);
				}
				word(answer.carriedSounds.size()); for (uint64_t identity: answer.carriedSounds) word(identity);
			};
			if (const Entry* entry = FindEntry(address)) {
				word(entry->serial); word(entry->movable); word(entry->carriesCopy); word(entry->ownedRegistered); word(entry->borrows);
				text(entry->className); text(entry->presetName);
				// Track the answer this root actually consumed. A borrowed actor
				// reference must not become dirty when an unused controller changes.
				if (helper == "_ScriptGraphNative") result(entry->native[argument == "1" ? 1 : 0]);
				else if (helper == "_ScriptGraphMembers") result(entry->members);
				else {
					const auto& answers = helper == "__index" ? entry->properties : entry->helpers;
					const auto answer = answers.find(helper == "__index" ? argument : helper);
					if (answer == answers.end()) bytes += "missing"; else result(answer->second);
				}
			} else if (helper == "_ScriptGraphIteratorSnapshot") {
				if (const auto iterator = m_Iterators.find(address); iterator != m_Iterators.end()) result(iterator->second);
				else bytes = "missing";
			}
			else bytes = "missing";
			return bytes;
		}
		size_t IteratorCount() const { return m_Iterators.size(); }
		size_t OwnedCount() const { return m_Owned.size(); }

		using NativeId = uintptr_t;
		class Answers {
		public:
			using Item = std::pair<std::string_view, Result>;
			using Iterator = std::vector<Item>::iterator;
			using ConstIterator = std::vector<Item>::const_iterator;
			Iterator begin() { return m_Items.begin(); }
			Iterator end() { return m_Items.end(); }
			ConstIterator begin() const { return m_Items.begin(); }
			ConstIterator end() const { return m_Items.end(); }
			size_t size() const { return m_Items.size(); }
			ConstIterator find(std::string_view name) const { return std::find_if(begin(), end(), [name](const Item& item) { return item.first == name; }); }
			std::pair<Iterator, bool> emplace(std::string_view name, Result value) {
				const auto existing = std::find_if(begin(), end(), [name](const Item& item) { return item.first == name; });
				if (existing != end()) return std::pair(existing, false);
				if (m_Items.empty()) m_Items.reserve(4);
				m_Items.emplace_back(name, std::move(value));
				return std::pair(std::prev(end()), true);
			}
		private:
			std::vector<Item> m_Items;
		};
		struct Entry {
			std::array<Result, 2> native;
			Result members;
			// Names are the static helper/property literals below. Do not allocate
			// another copy of them for every userdata in every captured state.
			Answers helpers;
			Answers properties;
			NativeId movable = 0;
			bool carriesCopy = false;
			// A script-owned object the live world registers must be reached by a root or refused.
			bool ownedRegistered = false;
			bool borrows = false;
			std::string className;
			std::string presetName;
			uint64_t serial = 0; // The userdata's birth number: a reused address with another serial is another object.
		};
		// Plain owned scalar bindings have no callbacks or native ownership links.
		// Freeze only their tokens; build the helper result containers on the saver.
		struct ScalarEntry {
			uint64_t serial = 0;
			TValue kind{}, instance{}, address{};
			std::array<TValue, 2> members{};
			std::array<TValue, 4> properties{};
			unsigned memberCount = 0;
			bool timer = false;
			mutable std::unique_ptr<Entry> expanded;
			const Entry* Expand() const {
				if (!expanded) {
					auto entry = std::make_unique<Entry>();
					entry->serial = serial;
					const auto answer = [](const TValue& token) { Result result; result.values.push_back(Value{token}); return result; };
					entry->native[0] = answer(kind);
					entry->native[1] = entry->native[0];
					for (unsigned index = 0; index < memberCount; ++index) entry->members.values.push_back(Value{members[index]});
					entry->helpers.emplace("_ScriptGraphInstance", answer(instance));
					entry->helpers.emplace("_ScriptGraphNativeAddress", answer(address));
					static constexpr std::array vectorNames{"X", "Y"};
					static constexpr std::array timerNames{"StartSimTimeTicks", "SimTimeLimitTicks", "StartRealTimeTicks", "RealTimeLimitTicks"};
					const std::span<const char* const> names = timer ? std::span<const char* const>(timerNames) : std::span<const char* const>(vectorNames);
					for (size_t index = 0; index < names.size(); ++index) entry->properties.emplace(names[index], answer(properties[index]));
					expanded = std::move(entry);
				}
				return expanded.get();
			}
		};
		// The class descriptors and plain userdata a state holds never change; every capture shares one map of them.
		struct ImmutableEntry {
			uint64_t serial = 0;
			TValue members{};
			std::array<TValue, 3> native{};
			unsigned memberCount = 0, nativeCount = 0;
			Entry Expand() const {
				Entry entry;
				entry.serial = serial;
				if (memberCount) entry.members.values.push_back(Value{members});
				TValue nil; setnilV(&nil);
				Result missing; missing.values.push_back(Value{nil});
				entry.helpers.emplace("_ScriptGraphInstance", missing);
				entry.helpers.emplace("_ScriptGraphNativeAddress", std::move(missing));
				for (unsigned index = 0; index < nativeCount; ++index) entry.native[0].values.push_back(Value{native[index]});
				entry.native[1] = entry.native[0];
				return entry;
			}
		};
		struct ClassEntries {
			std::unordered_map<const void*, Entry> entries;
			std::unordered_map<const void*, ImmutableEntry> compact;
			size_t Size() const { return entries.size() + compact.size(); }
		};
		size_t CachedCount() const { return m_CachedClasses; }
		/// Fresh references another reference's answer fits, and of those checked against a fresh answer, how many differed.
		size_t SharedCount() const { return m_SharedAnswers; }
		size_t SharedMismatchCount() const { return m_SharedMismatches; }
		int64_t EnumUs() const { return m_EnumUs; }
		int64_t WorldUs() const { return m_WorldUs; }
		int64_t AnswerUs() const { return m_AnswerUs; }

	private:
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
		using ScalarRecord = std::pair<const void*, ScalarEntry>;
		std::vector<ScalarRecord> m_Scalars;
		mutable std::optional<std::vector<const ScalarRecord*>> m_ScalarIndex;
		std::shared_ptr<const ClassEntries> m_Classes;
		// Expansion belongs to this image, rather than the shared class cache.
		// A later capture can therefore reuse the cache while this image is saved.
		mutable std::unordered_map<const void*, Entry> m_ExpandedClasses;
		size_t m_CachedClasses = 0;
		size_t m_SharedAnswers = 0, m_SharedMismatches = 0;
		int64_t m_EnumUs = 0, m_WorldUs = 0, m_AnswerUs = 0;
		std::unordered_map<const void*, Result> m_Iterators;
		const Entry* FindEntry(const void* address) const {
			if (const auto own = m_Entries.find(address); own != m_Entries.end()) return &own->second;
			if (!m_Scalars.empty()) {
				// The saver builds the index after the captured records stop growing.
				if (!m_ScalarIndex) {
					std::vector<const ScalarRecord*> index;
					index.reserve(m_Scalars.size());
					for (const auto& record: m_Scalars) index.push_back(&record);
					std::sort(index.begin(), index.end(), [](const auto* first, const auto* second) { return std::less<const void*>{}(first->first, second->first); });
					m_ScalarIndex = std::move(index);
				}
				const auto& index = *m_ScalarIndex;
				const auto scalar = std::lower_bound(index.begin(), index.end(), address, [](const auto* record, const void* key) { return std::less<const void*>{}(record->first, key); });
				if (scalar != index.end() && (*scalar)->first == address) return (*scalar)->second.Expand();
			}
			if (m_Classes) {
				if (const auto shared = m_Classes->entries.find(address); shared != m_Classes->entries.end()) return &shared->second;
				if (const auto shared = m_Classes->compact.find(address); shared != m_Classes->compact.end()) {
					if (const auto expanded = m_ExpandedClasses.find(address); expanded != m_ExpandedClasses.end()) return &expanded->second;
					return &m_ExpandedClasses.emplace(address, shared->second.Expand()).first->second;
				}
			}
			return nullptr;
		}
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
					const Entry* entry = FindEntry(gcval(&*value));
					if (!entry) throw std::runtime_error("frozen gib target has no native descriptor");
					collect(entry->movable);
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

	// What a state's captures keep between freezes: the class descriptors and the heap values they name.
	class NativeCache {
	public:
		explicit NativeCache(lua_State* state) : m_State(state) {
			// Keyed by this cache's own address, never by a registry ref: luabind hands out registry
			// refs from a counter of its own, so a ref number can be given to a second owner whose
			// release then puts a free-list number where this table was.
			lua_pushlightuserdata(state, this);
			lua_newtable(state);
			// Each userdata keys its own answers, and the table holds its keys weakly, so what a
			// descriptor named is collected with the object it describes and not one cycle later.
			lua_newtable(state);
			lua_pushliteral(state, "k");
			lua_setfield(state, -2, "__mode");
			lua_setmetatable(state, -2);
			lua_rawset(state, LUA_REGISTRYINDEX);
		}
		~NativeCache() {
			if (!m_State) return;
			lua_pushlightuserdata(m_State, this);
			lua_pushnil(m_State);
			lua_rawset(m_State, LUA_REGISTRYINDEX);
		}
		NativeCache(const NativeCache&) = delete;
		NativeCache& operator=(const NativeCache&) = delete;
		std::shared_ptr<const NativeImage::ClassEntries> classes = std::make_shared<NativeImage::ClassEntries>();
		// A reference to a live object answers the same while the object it names is the same live object.
		struct Reference {
			NativeImage::Entry entry;
			const void* pointer = nullptr;
			long uid = 0;
			bool entity = false;
		};
		std::unordered_map<const void*, Reference> references;
		// Pushes the table this cache keeps its descriptors' answers in.
		void PushRetained(lua_State* state) const {
			lua_pushlightuserdata(state, const_cast<NativeCache*>(this));
			lua_rawget(state, LUA_REGISTRYINDEX);
		}

	private:
		lua_State* m_State;
	};

	// The caller holds the VM lock and keeps carried sound observations enabled.
	class CaptureScope {
	public:
		CaptureScope(lua_State* state, NativeCache& cache) : m_References(state), m_Capture(true), m_Thread(std::this_thread::get_id()), m_Cache(cache) {
			// A world capture puts the counters back once, after every state; a state captured alone does it here.
			if (!s_GraphNativeCapture) m_Effects.emplace();
		}
		CaptureScope(const CaptureScope&) = delete;
		CaptureScope& operator=(const CaptureScope&) = delete;

		void Capture() {
			CheckThread();
			if (m_Captured || !m_Image) throw std::logic_error("native image capture cannot be repeated");
			if (!s_GraphNativeCapture) m_NativeScope.emplace();
			const auto enumStarted = std::chrono::steady_clock::now();
			// The class descriptors were answered by an earlier capture; everything else is asked again.
			const auto& classes = m_Cache.classes->entries;
			const auto& compactClasses = m_Cache.classes->compact;
			auto& references = m_Cache.references;
			ForEachCapturedUserdata(State(), [&](GCudata* data) {
				GCobj* object = obj2gco(data);
				if (const auto known = classes.find(object); known != classes.end() && known->second.serial == data->serial) {
					m_SeenClasses.Insert(object);
					++m_Image->m_CachedClasses;
					return;
				}
				if (const auto known = compactClasses.find(object); known != compactClasses.end() && known->second.serial == data->serial) {
					m_SeenClasses.Insert(object);
					++m_Image->m_CachedClasses;
					return;
				}
				if (const auto known = references.find(object); known != references.end() && known->second.entry.serial == data->serial) {
					const auto& reference = known->second;
					const auto* rep = static_cast<const luabind::detail::object_rep*>(uddata(data));
					bool same = rep->ptr() == reference.pointer;
					if (same && reference.entity) {
						const auto* mo = static_cast<const MovableObject*>(reference.pointer);
						same = g_MovableMan.ValidMO(mo) && mo->GetUniqueID() == reference.uid;
					}
					if (same) {
						const NativeImage::Entry& kept = m_Image->m_Entries.emplace(object, reference.entry).first->second;
						if (reference.entity && SharesAnswer(rep)) m_Shared.emplace(SharedKey::Of(rep), SharedAnswer{&kept, reference.uid});
						m_Kept.Insert(object);
						++m_Image->m_CachedClasses;
						return;
					}
				}
				TValue value; setgcVraw(&value, object, LJ_TUDATA); Enqueue(value);
			});
			// A suspended script can hold a native range iterator which was not
			// constructed by IteratorFromValues. Describe reachable stack closures too.
			ForEachCapturedFunction(State(), [&](const GCfunc* function) {
				if (!IteratorCandidate(function)) return;
				TValue value; setgcVraw(&value, reinterpret_cast<GCobj*>(const_cast<GCfunc*>(function)), LJ_TFUNC); Enqueue(value);
			});
			// The value iterators registered themselves when made; nothing else the walk asks about is a function.
			lua_getfield(State(), LUA_REGISTRYINDEX, "_ScriptGraphIterators");
			if (lua_istable(State(), -1)) {
				const int table = lua_gettop(State());
				lua_pushnil(State());
				while (lua_next(State(), table)) {
					if (lua_isfunction(State(), -2)) Enqueue(At(-2));
					lua_pop(State(), 1);
				}
			}
			lua_pop(State(), 1);
			const auto worldStarted = std::chrono::steady_clock::now();
			m_Image->m_EnumUs = std::chrono::duration_cast<std::chrono::microseconds>(worldStarted - enumStarted).count();
			if (CheckpointWriter::BatchEnabled()) m_Image->m_Scalars.reserve(m_Queue.size());
			{
				// The states of one world capture may run side by side; the first to get here walks the world.
				std::lock_guard worldLock(s_GraphNativeCapture->frozenWorldMutex);
				auto& shared = s_GraphNativeCapture->frozenWorld;
				if (!shared) shared = BuildWorld(s_GraphNativeCapture->KnownObjects());
				m_Image->m_World = std::static_pointer_cast<const NativeImage::World>(shared);
			}
			const auto answerStarted = std::chrono::steady_clock::now();
			m_Image->m_WorldUs = std::chrono::duration_cast<std::chrono::microseconds>(answerStarted - worldStarted).count();
			for (size_t index = 0; index < m_Queue.size(); ++index) {
				const TValue value = m_Queue[index];
				if (tvisudata(&value)) CaptureUserdata(value);
				else CaptureIterator(value);
			}
			m_Image->m_AnswerUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - answerStarted).count();
			if (m_RetainedReport) System::PrintDiagnosticLine(std::format("[checkpoint-retained] thread={} calls={} missing={} us={}", std::hash<std::thread::id>{}(std::this_thread::get_id()), m_RetainedCalls, m_RetainedMissing, m_RetainedUs));
			m_Captured = true;
		}

		std::shared_ptr<const NativeImage> Finish(lua_State* state) {
			CheckThread();
			if (!m_Captured || !m_Image || state != State()) throw std::logic_error("native results require the same frozen Lua heap");
			// Descriptors this walk neither reused nor made again describe userdata that are gone.
			const bool retired = m_SeenClasses.Size() != m_Cache.classes->Size();
			if (!m_NewClasses.empty() || !m_NewCompactClasses.empty() || retired) {
				auto merged = std::make_shared<NativeImage::ClassEntries>(*m_Cache.classes);
				if (retired) {
					std::erase_if(merged->entries, [this](const auto& entry) { return !m_SeenClasses.Contains(entry.first); });
					std::erase_if(merged->compact, [this](const auto& entry) { return !m_SeenClasses.Contains(entry.first); });
				}
				for (auto& [address, entry]: m_NewClasses) {
					merged->compact.erase(address);
					merged->entries[address] = std::move(entry);
				}
				for (auto& [address, entry]: m_NewCompactClasses) {
					merged->entries.erase(address);
					merged->compact[address] = std::move(entry);
				}
				m_Cache.classes = std::move(merged);
			}
			// References this walk neither reused nor made again name objects that are gone.
			std::erase_if(m_Cache.references, [this](const auto& entry) { return !m_Kept.Contains(entry.first); });
			for (auto& [address, reference]: m_NewReferences) m_Cache.references[address] = std::move(reference);
			m_Image->m_Classes = m_Cache.classes;
			std::shared_ptr<const NativeImage> result = std::move(m_Image);
			m_References.Release();
			return result;
		}

	private:
		struct References {
			lua_State* state;
			decltype(std::declval<global_State&>().gc.threshold) threshold;
			uint64_t serial;
			int count = 0;
			explicit References(lua_State* source) : state(source), threshold(G(source)->gc.threshold), serial(luaJIT_state_serial(source)) {
				G(state)->gc.threshold = std::numeric_limits<decltype(threshold)>::max();
				// This capture's own address keys its table, for the reason the cache's does.
				lua_pushlightuserdata(state, this);
				lua_newtable(state);
				lua_rawset(state, LUA_REGISTRYINDEX);
			}
			~References() { Release(); }
			void Push() const {
				lua_pushlightuserdata(state, const_cast<References*>(this));
				lua_rawget(state, LUA_REGISTRYINDEX);
			}
			void Release() {
				if (!state) return;
				lua_pushlightuserdata(state, this);
				lua_pushnil(state);
				lua_rawset(state, LUA_REGISTRYINDEX);
				G(state)->gc.threshold = threshold;
				luaJIT_set_state_serial(state, serial);
				state = nullptr;
			}
		} m_References;

		std::optional<NativeEffects> m_Effects;

		ScriptGraphCaptureScope m_Capture;
		std::optional<LuaScriptGraphNativeCaptureScope> m_NativeScope;
		std::thread::id m_Thread;
		NativeCache& m_Cache;
		// The capture owns prepared backing until its temporary containers die.
		std::shared_ptr<std::pmr::memory_resource> m_TransientBacking = CheckpointWriter::BatchEnabled() ? CheckpointBuffer::LeaseCaptureStorage() : nullptr;
		std::pmr::monotonic_buffer_resource m_Transient{m_TransientBacking ? m_TransientBacking.get() : std::pmr::get_default_resource()};
		std::pmr::memory_resource* m_TransientResource = CheckpointWriter::BatchEnabled() ? &m_Transient : std::pmr::get_default_resource();
		std::pmr::unordered_map<const void*, NativeImage::Entry> m_NewClasses{m_TransientResource};
		std::pmr::unordered_map<const void*, NativeImage::ImmutableEntry> m_NewCompactClasses{m_TransientResource};
		std::pmr::unordered_map<const void*, NativeCache::Reference> m_NewReferences{m_TransientResource};
		CaptureAddressSet m_Kept{m_TransientResource};
		struct SharedKey {
			const void* crep = nullptr;
			const void* pointer = nullptr;
			bool constant = false;
			static SharedKey Of(const luabind::detail::object_rep* rep) { return {rep->crep(), rep->ptr(), (rep->flags() & luabind::detail::object_rep::constant) != 0}; }
			bool operator==(const SharedKey&) const = default;
		};
		struct SharedKeyHash {
			size_t operator()(const SharedKey& key) const noexcept { return std::hash<const void*>{}(key.pointer) ^ (std::hash<const void*>{}(key.crep) << 1) ^ static_cast<size_t>(key.constant); }
		};
		struct SharedAnswer { const NativeImage::Entry* entry = nullptr; long uid = 0; };
		struct SharedHit { NativeImage::Entry entry; long uid = 0; };
		std::pmr::unordered_map<SharedKey, SharedAnswer, SharedKeyHash> m_Shared{m_TransientResource};
		CaptureAddressSet m_SeenClasses{m_TransientResource};
		TValue m_Subject{};
		bool m_Persist = false;
		std::shared_ptr<NativeImage> m_Image = std::make_shared<NativeImage>();
		CaptureAddressSet m_Queued{m_TransientResource};
		CaptureAddressSet m_Pinned{m_TransientResource};
		std::pmr::vector<TValue> m_Queue{m_TransientResource};
		bool m_Captured = false;
		struct ScalarKeys {
			GCstr* index = nullptr;
			std::array<GCstr*, 4> properties{};
			GCstr* kind = nullptr;
			const luabind::detail::class_rep* type = nullptr;
			TValue members{};
		};
		std::array<ScalarKeys, 2> m_ScalarKeys;
		const bool m_RetainedReport = [] { const char* value = std::getenv("CCCP_CHECKPOINT_PHASES"); return value && std::string_view(value) == "1"; }();
		size_t m_RetainedCalls = 0, m_RetainedMissing = 0;
		int64_t m_RetainedUs = 0;

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
			const TValue value = At(index);
			// One strong reference keeps every repeated token alive for this image.
			if (!CheckpointWriter::BatchEnabled() || !tvisgcv(&value) || m_Pinned.Insert(gcval(&value))) {
				m_References.Push();
				lua_pushvalue(State(), index);
				lua_rawseti(State(), -2, ++m_References.count);
				lua_pop(State(), 1);
			}
			// A cached descriptor's values must outlive this capture: the subject keeps them for every later freeze.
			if (m_Persist) {
				const int bucket = PushRetainedBucket();
				lua_pushvalue(State(), index);
				lua_rawseti(State(), bucket, static_cast<int>(lua_objlen(State(), bucket)) + 1);
				lua_pop(State(), 2);
			}
		}
		// The subject's own bucket in the retained table; it and every answer in it die with the subject.
		int PushRetainedBucket() {
			m_Cache.PushRetained(State());
			Push(m_Subject);
			lua_rawget(State(), -2);
			if (lua_isnil(State(), -1)) {
				lua_pop(State(), 1);
				lua_newtable(State());
				Push(m_Subject);
				lua_pushvalue(State(), -2);
				lua_rawset(State(), -4);
			}
			return lua_gettop(State());
		}
		// A subject answered again names new values; what the last capture kept for it goes.
		void ReleaseRetained(const TValue& subject) {
			const auto started = m_RetainedReport ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			m_Cache.PushRetained(State());
			const TValue* previous = (m_RetainedReport || CheckpointWriter::BatchEnabled()) ? lj_tab_get(State(), tabV(&State()->top[-1]), &subject) : nullptr;
			if (m_RetainedReport) {
				++m_RetainedCalls;
				if (!previous || tvisnil(previous)) ++m_RetainedMissing;
			}
			// Setting an absent key to nil still grows Lua's hash chains.
			if (CheckpointWriter::BatchEnabled() && (!previous || tvisnil(previous))) {
				lua_pop(State(), 1);
				if (m_RetainedReport) m_RetainedUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
				return;
			}
			Push(subject);
			lua_pushnil(State());
			lua_rawset(State(), -3);
			lua_pop(State(), 1);
			if (m_RetainedReport) m_RetainedUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
		}
		// The heap values a cached answer names stay alive in the subject's bucket of the retained table.
		void Retain(const NativeImage::Entry& entry) {
			const auto keep = [&](const NativeImage::Result& result) {
				for (const NativeImage::Value& item: result.values) {
					if (item.text || !tvisgcv(&item.token)) continue;
					const int bucket = PushRetainedBucket();
					Push(item.token);
					lua_rawseti(State(), bucket, static_cast<int>(lua_objlen(State(), bucket)) + 1);
					lua_pop(State(), 2);
				}
			};
			for (const auto& result: entry.native) keep(result);
			// The members are the class's table and the subject's own: both are reachable from the subject
			// and live exactly as long as it does, so keeping them here would only pin the subject's table
			// to itself and outlive it.
			for (const auto& [name, result]: entry.helpers) keep(result);
			for (const auto& [name, result]: entry.properties) keep(result);
		}
		void Enqueue(const TValue& value) {
			if ((!tvisudata(&value) && !tvisfunc(&value)) || !m_Queued.Insert(gcval(&value))) return;
			Push(value); Keep(-1); lua_pop(State(), 1);
			m_Queue.push_back(value);
		}
		using ResultTables = std::optional<std::pmr::unordered_set<const void*>>;
		void NewResults(int index, uint64_t before, uint64_t after, ResultTables& seen) {
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
			if (!fresh || !tvistab(&value)) return;
			// Only newly returned tables need a cycle set.
			if (!seen) seen.emplace(m_TransientResource);
			if (!seen->insert(gcval(&value)).second) return;
			lua_pushnil(State());
			while (lua_next(State(), index)) {
				NewResults(-2, before, after, seen);
				NewResults(-1, before, after, seen);
				lua_pop(State(), 1);
			}
		}

		// Binding-table and constant descriptors read no Lua property or native visitor.
		// Keep their returned values exactly as the protected helper call does.
		template<class PushValues> NativeImage::Result RecordPushed(PushValues push, bool keep = true) {
			const int top = lua_gettop(State());
			struct RestoreStack { lua_State* state; int top; ~RestoreStack() { lua_settop(state, top); } } stack{State(), top};
			const uint64_t before = luaJIT_state_serial(State());
			push();
			const uint64_t after = luaJIT_state_serial(State());
			NativeImage::Result result;
			const int last = lua_gettop(State());
			if (CheckpointWriter::BatchEnabled()) result.values.reserve(last - top);
			ResultTables seen;
			for (int index = top + 1; index <= last; ++index) {
				NativeImage::Value value;
				value.token = At(index);
				if (const auto* text = ScriptGraphCapturedText(State(), index)) value.text = *text;
				if (keep) {
					if (!CheckpointWriter::BatchEnabled() || tvisgcv(&value.token)) Keep(index);
					NewResults(index, before, after, seen);
				}
				result.values.push_back(std::move(value));
			}
			return result;
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
			if (CheckpointWriter::BatchEnabled()) result.values.reserve(last - top);
			ResultTables seen;
			for (int index = top + 1; index <= last; ++index) {
				NativeImage::Value value;
				value.token = At(index);
				if (const auto* text = ScriptGraphCapturedText(State(), index)) value.text = *text;
				// Numbers, booleans, nil and light userdata have no GC owner to
				// pin. Avoid growing a scratch Lua table for those scalar answers.
				if (!CheckpointWriter::BatchEnabled() || tvisgcv(&value.token)) Keep(index);
				NewResults(index, before, after, seen);
				result.values.push_back(std::move(value));
			}
			if (notes) result.carriedSounds.assign(notes->Carried().begin(), notes->Carried().end());
			return result;
		}

		NativeImage::Result One(lua_CFunction function, const char* name, const TValue& value, bool observesNative = false) {
			return Invoke(function, name, [&] { Push(value); return 1; }, observesNative);
		}
		static std::string_view Kind(const NativeImage::Result& result) {
			if (!result.error.empty() || result.values.empty() || result.values.front().text || !tvisstr(&result.values.front().token)) return {};
			const GCstr* text = strV(&result.values.front().token);
			return std::string_view(strdata(text), text->len);
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
		// Overrides keep the ordinary Lua lookup; plain scalar getters need no call closure.
		bool PlainScalarProperties(const TValue& subject, const luabind::detail::object_rep* object) {
			if (!CheckpointWriter::BatchEnabled() || !object || !object->ptr() || !object->crep()) return false;
			const auto* type = object->crep();
			if (type->get_class_type() != luabind::detail::class_rep::cpp_class ||
			    (type->type() != LUABIND_TYPEID(Vector) && type->type() != LUABIND_TYPEID(Timer))) return false;
			static constexpr std::array vectorNames{"X", "Y"};
			static constexpr std::array timerNames{"StartSimTimeTicks", "SimTimeLimitTicks", "StartRealTimeTicks", "RealTimeLimitTicks"};
			const std::span<const char* const> names = type->type() == LUABIND_TYPEID(Timer) ? std::span<const char* const>(timerNames) : std::span<const char* const>(vectorNames);
			const int top = lua_gettop(State());
			struct Restore { lua_State* state; int top; ~Restore() { lua_settop(state, top); } } restore{State(), top};
			auto& keys = m_ScalarKeys[type->type() == LUABIND_TYPEID(Timer) ? 1 : 0];
			if (!keys.index) {
				const auto key = [&](const char* name) {
					lua_pushstring(State(), name);
					GCstr* string = strV(&State()->top[-1]);
					Keep(-1);
					lua_pop(State(), 1);
					return string;
				};
				keys.index = key("__index");
				for (size_t index = 0; index < names.size(); ++index) keys.properties[index] = key(names[index]);
			}
			const GCtab* meta = tabref(udataV(&subject)->metatable);
			if (!meta) return false;
			const TValue* dispatcher = lj_tab_getstr(const_cast<GCtab*>(meta), keys.index);
			if (!dispatcher || !tvisfunc(dispatcher)) return false;
			const GCfunc* function = funcV(dispatcher);
			const BCOp operation = bc_op(*mref(function->c.pc, BCIns));
			if ((operation != BC_FUNCC && operation != BC_FUNCCW) || function->c.f != luabind::detail::class_rep::gettable_dispatcher) return false;
			const auto plain = [&] {
				const TValue& value = State()->top[-1];
				if (!tvistab(&value) || tabref(tabV(&value)->metatable)) return false;
				// Read every override again; only the interned keys are capture-local.
				for (size_t index = 0; index < names.size(); ++index) {
					const TValue* field = lj_tab_getstr(tabV(&value), keys.properties[index]);
					if (field && !tvisnil(field)) return false;
				}
				return true;
			};
			if (object->get_lua_table().is_valid()) {
				object->get_lua_table().get(State());
				if (!plain()) return false;
				lua_pop(State(), 1);
			}
			type->get_table(State());
			return plain();
		}
		TValue ScalarPropertyToken(const luabind::detail::object_rep* object, const char* name) {
			lua_Number number;
			if (object->crep()->type() == LUABIND_TYPEID(Vector)) {
				const auto& value = *static_cast<const Vector*>(object->ptr());
				number = std::strcmp(name, "X") == 0 ? value.GetX() : value.GetY();
			} else {
				const auto& value = *static_cast<const Timer*>(object->ptr());
				if (std::strcmp(name, "StartSimTimeTicks") == 0) number = value.GetStartSimTimeTicksNumber();
				else if (std::strcmp(name, "SimTimeLimitTicks") == 0) number = value.GetSimTimeLimitTicksNumber();
				else if (std::strcmp(name, "StartRealTimeTicks") == 0) number = value.GetStartRealTimeTicksNumber();
				else number = value.GetRealTimeLimitTicksNumber();
			}
			// Match lua_pushnumber, including its NaN canonicalization, without
			// changing the live VM stack for each already-owned scalar field.
			TValue token; setnumV(&token, number);
			if (tvisnan(&token)) setnanV(&token);
			return token;
		}
		NativeImage::Result ScalarProperty(const luabind::detail::object_rep* object, const char* name) {
			NativeImage::Result result;
			NativeImage::Value value;
			value.token = ScalarPropertyToken(object, name);
			result.values.push_back(std::move(value));
			return result;
		}
		template<class PushToken> TValue ScalarToken(PushToken push) {
			const int top = lua_gettop(State());
			struct Restore { lua_State* state; int top; ~Restore() { lua_settop(state, top); } } restore{State(), top};
			const uint64_t before = luaJIT_state_serial(State());
			push();
			const uint64_t after = luaJIT_state_serial(State());
			const TValue token = At(-1);
			if (tvisgcv(&token)) Keep(-1);
			ResultTables seen;
			NewResults(-1, before, after, seen);
			return token;
		}
		bool CaptureOwnedScalar(const TValue& subject, const luabind::detail::object_rep* object) {
			if (!CheckpointWriter::BatchEnabled() || !object || !object->ptr() || !object->crep() ||
			    !(object->flags() & luabind::detail::object_rep::owner)) return false;
			const auto* type = object->crep();
			if (type->get_class_type() != luabind::detail::class_rep::cpp_class) return false;
			const bool timer = type->type() == LUABIND_TYPEID(Timer) && std::strcmp(type->name(), "Timer") == 0;
			const bool vector = type->type() == LUABIND_TYPEID(Vector) && std::strcmp(type->name(), "Vector") == 0;
			if (!timer && !vector) return false;
			static constexpr std::array vectorNames{"X", "Y"};
			static constexpr std::array timerNames{"StartSimTimeTicks", "SimTimeLimitTicks", "StartRealTimeTicks", "RealTimeLimitTicks"};
			const std::span<const char* const> names = timer ? std::span<const char* const>(timerNames) : std::span<const char* const>(vectorNames);
			if (!PlainScalarProperties(subject, object)) return false;
			CaptureTrace::Span span("answer_scalar", type->name());
			NativeImage::ScalarEntry entry;
			entry.serial = luaJIT_value_serial(State(), -1);
			entry.timer = timer;
			auto& tokens = m_ScalarKeys[timer ? 1 : 0];
			if (tokens.type != type) {
				type->get_table(State()); tokens.members = At(-1); Keep(-1); lua_pop(State(), 1); tokens.type = type;
			}
			entry.members[entry.memberCount++] = tokens.members;
			if (object->get_lua_table().is_valid()) {
				object->get_lua_table().get(State()); entry.instance = At(-1); Keep(-1); lua_pop(State(), 1);
				entry.members[entry.memberCount++] = entry.instance;
			} else setnilV(&entry.instance);
			lua_pushlightuserdata(State(), object->ptr()); entry.address = At(-1); lua_pop(State(), 1);
			if (!tokens.kind) {
				lua_pushstring(State(), timer ? "timer" : "vector"); tokens.kind = strV(&State()->top[-1]); Keep(-1); lua_pop(State(), 1);
			}
			setgcVraw(&entry.kind, obj2gco(tokens.kind), LJ_TSTR);
			for (size_t index = 0; index < names.size(); ++index) entry.properties[index] = ScalarPropertyToken(object, names[index]);
			m_Image->m_Scalars.emplace_back(gcval(&subject), std::move(entry));
			return true;
		}
		static int IsIterator(lua_State* state) {
			const bool iterator = lua_tocfunction(state, 1) == ScriptGraphValueIteratorNext || ScriptGraphIteratorHook(state, 1, "__iterator_snapshot");
			lua_pushboolean(state, iterator);
			return 1;
		}

		void CaptureUserdata(const TValue& value) {
			std::string className;
			bool detached = false;
			m_Subject = value;
			ReleaseRetained(value);
			Push(value);
			// Only a luabind instance can change its answers; a class descriptor or a plain userdata is answered once.
			const auto* object = luabind::detail::is_class_object(State(), -1);
			if (CaptureOwnedScalar(value, object)) { lua_pop(State(), 1); return; }
			const bool immutable = luabind::detail::is_class_rep(State(), -1) || !object;
			// A fresh reference to a live object that another reference of its class already answered for answers the same.
			std::optional<SharedHit> shared;
			if (!immutable) shared = SharedAnswerFor(object);
			if (shared) ++m_Image->m_SharedAnswers;
			CaptureTrace::Span span(shared ? "answer_shared" : "answer", CaptureTrace::Active() && object && object->crep() ? std::string(object->crep()->name()) : std::string());
			if (shared && !VerifySharedAnswers()) {
				CommitShared(value, object, std::move(*shared));
				lua_pop(State(), 1);
				return;
			}
			struct Persist {
				bool& flag;
				Persist(bool& value, bool on) : flag(value) { flag = on; }
				~Persist() { flag = false; }
			} persist{m_Persist, immutable};
			if (immutable && CheckpointWriter::BatchEnabled()) {
				auto* classRep = luabind::detail::is_class_rep(State(), -1) ? static_cast<luabind::detail::class_rep*>(lua_touserdata(State(), -1)) : nullptr;
				NativeImage::ImmutableEntry compact;
				compact.serial = luaJIT_value_serial(State(), -1);
				lua_pop(State(), 1);
				CaptureTrace::Span immutableSpan("answer_class");
				if (classRep) {
					compact.members = ScalarToken([&] { classRep->get_table(State()); });
					compact.memberCount = 1;
				}
				if (classRep && classRep->get_class_type() == luabind::detail::class_rep::lua_class) {
					compact.native[compact.nativeCount++] = ScalarToken([&] { lua_pushliteral(State(), "lua-class"); });
					compact.native[compact.nativeCount++] = ScalarToken([&] { lua_pushstring(State(), classRep->name()); });
					compact.native[compact.nativeCount++] = ScalarToken([&] { lua_pushstring(State(), !classRep->bases().empty() && classRep->bases()[0].base ? classRep->bases()[0].base->name() : ""); });
				} else {
					setnilV(&compact.native[compact.nativeCount++]);
					if (classRep) compact.native[compact.nativeCount++] = ScalarToken([&] { lua_pushstring(State(), classRep->name()); });
				}
				if (VerifySharedAnswers()) {
					NativeImage::Entry original;
					original.serial = compact.serial;
					AnswerImmutable(original, classRep, false);
					CompareShared(original, compact.Expand(), classRep ? classRep->name() : "");
				}
				m_NewCompactClasses.emplace(gcval(&value), std::move(compact));
				return;
			}
			auto& entry = immutable ? m_NewClasses[gcval(&value)] : m_Image->m_Entries[gcval(&value)];
			entry.serial = luaJIT_value_serial(State(), -1);
			bool movable = false, owned = false;
			if (object && object->crep()) {
				className = object->crep()->name();
				movable = ClassDerivesFrom(object->crep(), "MovableObject");
				owned = (object->flags() & luabind::detail::object_rep::owner) != 0;
				// The values read below through their own properties are never read once a script outlived their owner (a
				// gone movable object's field, a past frame's alarm).
				const bool readsProperties = className == "Vector" || className == "Timer" || className == "AlarmEvent";
				// A reference a script kept past its object's end, restored or not, is named as gone.
				const bool namedWhenGone = movable || className == "Area";
				detached = (!object->ptr() && (!namedWhenGone || owned)) ||(readsProperties && object->ptr() && !owned && !ScriptGraphNativeAlive(State(), object));
				if (movable && object->ptr() && ScriptGraphNativeAlive(State(), object)) {
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
			auto* classRep = luabind::detail::is_class_rep(State(), -1) ? static_cast<luabind::detail::class_rep*>(lua_touserdata(State(), -1)) : nullptr;
			lua_pop(State(), 1);
			if (immutable) {
				if (!VerifySharedAnswers()) {
					AnswerImmutable(entry, classRep, true);
					return;
				}
				NativeImage::Entry quick;
				quick.serial = entry.serial;
				AnswerImmutable(quick, classRep, false);
				shared.emplace(SharedHit{std::move(quick), 0});
			}
			const bool ownedScalar = CheckpointWriter::BatchEnabled() && owned && !detached && object && object->crep() &&
			    object->crep()->get_class_type() == luabind::detail::class_rep::cpp_class &&
			    ((className == "Vector" && object->crep()->type() == LUABIND_TYPEID(Vector)) ||
			     (className == "Timer" && object->crep()->type() == LUABIND_TYPEID(Timer)));
			if (ownedScalar) {
				entry.members = RecordPushed([&] {
					object->crep()->get_table(State());
					if (object->get_lua_table().is_valid()) object->get_lua_table().get(State());
				});
				entry.helpers.emplace("_ScriptGraphInstance", RecordPushed([&] {
					if (object->get_lua_table().is_valid()) object->get_lua_table().get(State());
					else lua_pushnil(State());
				}));
				entry.helpers.emplace("_ScriptGraphNativeAddress", RecordPushed([&] { lua_pushlightuserdata(State(), object->ptr()); }));
			} else {
				entry.members = One(MemberSources, "Members", value);
				entry.helpers.emplace("_ScriptGraphInstance", One(ScriptGraphInstance, "Instance", value));
				entry.helpers.emplace("_ScriptGraphNativeAddress", One(ScriptGraphNativeAddress, "NativeAddress", value));
			}
			if (detached) {
				for (auto& descriptor: entry.native) descriptor.error = "frozen native capture found detached " + className + " userdata";
				if (shared) CompareShared(shared->entry, entry, className);
				return;
			}
			std::array<std::string_view, 2> kinds;
			for (int named = 0; named < 2; ++named) {
				if (ownedScalar) {
					if (named == 0) entry.native[0] = RecordPushed([&] { lua_pushstring(State(), className == "Vector" ? "vector" : "timer"); });
					else entry.native[1] = entry.native[0];
				} else entry.native[named] = Invoke(ScriptGraphNative, "Native", [&] { Push(value); lua_pushboolean(State(), named); return 2; }, true);
				kinds[named] = Kind(entry.native[named]);
			}
			const auto contains = [&kinds](std::string_view kind) { return kinds[0] == kind || kinds[1] == kind; };
			const auto helper = [&](const char* name, lua_CFunction function) { entry.helpers.emplace(name, One(function, name, value)); };
			if (contains("copy")) entry.helpers.emplace("_ScriptGraphNativeSave", One(ScriptGraphNativeSave, "NativeSave", value, true));
			if (contains("area-ref") || (contains("copy") && className == "Area")) helper("_ScriptGraphAreaBoxes", ScriptGraphAreaBoxes);
			if (contains("gib-ref")) helper("_ScriptGraphGibOwner", ScriptGraphGibOwner);
			if (contains("soundset-ref")) helper("_ScriptGraphSoundSetOwner", ScriptGraphSoundSetOwner);
			if (contains("limb-ref")) helper("_ScriptGraphLimbOwner", ScriptGraphLimbOwner);
			if (contains("box-ref")) helper("_ScriptGraphSceneBoxOwner", ScriptGraphSceneBoxOwner);
			if (contains("vector-ref-unresolved") || contains("timer-ref")) {
				helper("_ScriptGraphPropertyOwner", ScriptGraphPropertyOwner);
				helper("_ScriptGraphLimbVectorOwner", ScriptGraphLimbVectorOwner);
			}
			static constexpr std::array vectorProperties{"X", "Y"};
			static constexpr std::array timerProperties{"StartSimTimeTicks", "SimTimeLimitTicks", "StartRealTimeTicks", "RealTimeLimitTicks"};
			static constexpr std::array alarmProperties{"ScenePos", "Team", "Range"};
			std::span<const char* const> properties;
			if (className == "Vector") properties = vectorProperties;
			else if (className == "Timer") properties = timerProperties;
			else if (className == "AlarmEvent") properties = alarmProperties;
			const bool scalarProperties = !properties.empty() && PlainScalarProperties(value, object);
			for (const char* property: properties) {
				entry.properties.emplace(property, scalarProperties ? ScalarProperty(object, property) :
				                         Invoke(Property, property, [&] { Push(value); lua_pushstring(State(), property); return 2; }));
			}
			// A reference to a live entity or to a manager singleton answers the same next time if it still names the same object.
			const std::string_view kind = Kind(entry.native[0]);
			const bool entity = kind == "entity" && object && object->ptr();
			const bool singleton = kind.empty() && object && object->ptr() && !owned && !movable && entry.helpers.size() == 2;
			if (entity || singleton) {
				NativeCache::Reference reference{entry, object->ptr(), entity ? static_cast<const MovableObject*>(object->ptr())->GetUniqueID() : 0, entity};
				m_NewReferences[gcval(&value)] = std::move(reference);
				m_Kept.Insert(gcval(&value));
				Retain(entry);
			}
			if (entity && SharesAnswer(object)) m_Shared.emplace(SharedKey::Of(object), SharedAnswer{&entry, static_cast<const MovableObject*>(object->ptr())->GetUniqueID()});
			if (shared) CompareShared(shared->entry, entry, classRep ? classRep->name() : className);
		}

		// A class descriptor or a plain userdata answers with its class table and its name alone: the helpers' answers, pushed
		// and recorded here as Invoke records them, without calling them.
		void AnswerImmutable(NativeImage::Entry& entry, luabind::detail::class_rep* crep, bool keep) {
			const auto record = [this, keep](const std::function<void()>& push) {
				const int top = lua_gettop(State());
				const uint64_t before = luaJIT_state_serial(State());
				push();
				const uint64_t after = luaJIT_state_serial(State());
				NativeImage::Result result;
				ResultTables seen;
				for (int index = top + 1; index <= lua_gettop(State()); ++index) {
					NativeImage::Value value;
					value.token = At(index);
					if (const auto* text = ScriptGraphCapturedText(State(), index)) value.text = *text;
					if (keep) {
						Keep(index);
						NewResults(index, before, after, seen);
					}
					result.values.push_back(std::move(value));
				}
				lua_settop(State(), top);
				return result;
			};
			entry.members = record([&] { if (crep) crep->get_table(State()); });
			entry.helpers.emplace("_ScriptGraphInstance", record([&] { lua_pushnil(State()); }));
			entry.helpers.emplace("_ScriptGraphNativeAddress", record([&] { lua_pushnil(State()); }));
			const auto native = [&] {
				if (!crep) {
					lua_pushnil(State());
				} else if (crep->get_class_type() == luabind::detail::class_rep::lua_class) {
					lua_pushliteral(State(), "lua-class");
					lua_pushstring(State(), crep->name());
					lua_pushstring(State(), !crep->bases().empty() && crep->bases()[0].base ? crep->bases()[0].base->name() : "");
				} else {
					lua_pushnil(State());
					lua_pushstring(State(), crep->name());
				}
			};
			for (int named = 0; named < 2; ++named) entry.native[named] = record(native);
		}

		// What a reference to a live world object answers is its class's and its object's, unless the userdata carries a table of its
		// own: its dependencies name an owner only for activity and editor members, which are never world objects.
		static bool SharesAnswer(const luabind::detail::object_rep* rep) {
			return rep && rep->crep() && rep->ptr() && !(rep->flags() & luabind::detail::object_rep::owner) && !rep->get_lua_table().is_valid();
		}
		static bool VerifySharedAnswers() {
			static const bool verify = [] {
				const char* value = std::getenv("CCCP_CHECKPOINT_VERIFY_SHARED");
				return value && *value && std::strcmp(value, "0") != 0;
			}();
			return verify;
		}
		// The top of the stack is the subject.
		std::optional<SharedHit> SharedAnswerFor(const luabind::detail::object_rep* object) {
			if (m_Shared.empty() || !SharesAnswer(object)) return std::nullopt;
			const auto hit = m_Shared.find(SharedKey::Of(object));
			if (hit == m_Shared.end() || !ScriptGraphNativeAlive(State(), object)) return std::nullopt;
			const auto* mo = static_cast<const MovableObject*>(object->ptr());
			if (mo->GetUniqueID() != hit->second.uid) return std::nullopt;
			SharedHit shared{*hit->second.entry, hit->second.uid};
			shared.entry.serial = luaJIT_value_serial(State(), -1);
			return shared;
		}
		void CommitShared(const TValue& value, const luabind::detail::object_rep* object, SharedHit shared) {
			CaptureObject(static_cast<const MovableObject*>(object->ptr()));
			NativeImage::Entry& entry = m_Image->m_Entries[gcval(&value)] = std::move(shared.entry);
			m_NewReferences[gcval(&value)] = NativeCache::Reference{entry, object->ptr(), shared.uid, true};
			m_Kept.Insert(gcval(&value));
			Retain(entry);
		}
		static bool SameResult(const NativeImage::Result& a, const NativeImage::Result& b) {
			if (a.error != b.error || a.carriedSounds != b.carriedSounds || a.values.size() != b.values.size()) return false;
			for (size_t index = 0; index < a.values.size(); ++index) {
				const NativeImage::Value& left = a.values[index];
				const NativeImage::Value& right = b.values[index];
				if (left.token.u64 != right.token.u64 || left.text.has_value() != right.text.has_value()) return false;
				if (left.text && left.text->Text() != right.text->Text()) return false;
			}
			return true;
		}
		// The check CCCP_CHECKPOINT_VERIFY_SHARED asks for: the answer a shared reference took against the one it would have made.
		void CompareShared(const NativeImage::Entry& shared, const NativeImage::Entry& fresh, const std::string& className) {
			const char* field = nullptr;
			if (!SameResult(shared.native[0], fresh.native[0]) || !SameResult(shared.native[1], fresh.native[1])) field = "native";
			else if (!SameResult(shared.members, fresh.members)) field = "members";
			else if (shared.helpers.size() != fresh.helpers.size() || shared.properties.size() != fresh.properties.size()) field = "helper-set";
			else if (shared.movable != fresh.movable || shared.carriesCopy != fresh.carriesCopy || shared.ownedRegistered != fresh.ownedRegistered ||
			         shared.borrows != fresh.borrows || shared.className != fresh.className || shared.presetName != fresh.presetName || shared.serial != fresh.serial) field = "fields";
			for (const auto& [name, result]: shared.helpers) {
				if (field) break;
				const auto other = fresh.helpers.find(name);
				if (other == fresh.helpers.end() || !SameResult(result, other->second)) field = "helpers";
			}
			for (const auto& [name, result]: shared.properties) {
				if (field) break;
				const auto other = fresh.properties.find(name);
				if (other == fresh.properties.end() || !SameResult(result, other->second)) field = "properties";
			}
			if (!field) return;
			++m_Image->m_SharedMismatches;
			static std::atomic<int> printed{0};
			if (printed.fetch_add(1, std::memory_order_relaxed) < 16) std::cout << "[native-share] mismatch class=" << className << " field=" << field << std::endl;
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

	public:
		// The world's trees, walked in chunks side by side; an object two chunks reach is described the same by both.
		static std::shared_ptr<const void> BuildWorld(const std::vector<MovableObject*>& known) {
			CheckpointBuffer::AllocationScope allocation(CheckpointWriter::BatchEnabled());
			auto world = std::make_shared<NativeImage::World>();
			const size_t chunks = CaptureTrace::Serial() ? 1 : std::clamp<size_t>(known.size() / 512, 1, 16);
			std::vector<NativeImage::Topology> topologies(chunks);
			std::vector<std::vector<NativeImage::NativeId>> objects(chunks);
			const auto walk = [&](size_t chunk) {
				CheckpointBuffer::AllocationScope allocation(CheckpointWriter::BatchEnabled());
				for (size_t index = chunk * known.size() / chunks; index < (chunk + 1) * known.size() / chunks; ++index) {
					const MovableObject* object = known[index];
					if (!g_MovableMan.ValidMO(object)) continue;
					objects[chunk].push_back(reinterpret_cast<uintptr_t>(object));
					Describe(object, topologies[chunk], nullptr);
				}
			};
			ParallelWork(g_ThreadMan.GetPriorityThreadPool(), chunks, walk).Finish();
			for (size_t chunk = 0; chunk < chunks; ++chunk) {
				world->objects.insert(world->objects.end(), objects[chunk].begin(), objects[chunk].end());
				for (auto& [identity, object]: topologies[chunk]) world->topology.try_emplace(identity, std::move(object));
			}
			return world;
		}

	private:
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
