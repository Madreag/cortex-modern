#pragma once

#include "CheckpointLuaPrototype.h"
#include "CheckpointLuaNative.h"
#include "CheckpointLuaThread.h"
#include "CheckpointImage.h"

#include <set>

extern "C" {
#include "lualib.h"
}

namespace RTE::CheckpointLua {
	struct GraphWorker {
		std::mutex mutex;
		std::unique_ptr<lua_State, decltype(&lua_close)> state{nullptr, lua_close};
		std::shared_ptr<void> context;
		~GraphWorker() { state.reset(); context.reset(); }
	};

	struct GraphImage {
		std::shared_ptr<GraphWorker> worker;
		std::shared_ptr<GraphDirt> observations = std::make_shared<GraphDirt>();
		size_t stateIndex = 0;
		int64_t nativeUs = 0;
		Snapshot heap;
		std::shared_ptr<const NativeImage> native;
		TValue roots, globals, baseline, package, callbacks;
		uint64_t liveSerial = 0;
		CheckpointText rng;
		std::unordered_set<const void*> scratch;
		std::optional<TValue> labels;

		GraphImage() {
			setnilV(&roots);
			setnilV(&callbacks);
			setnilV(&globals);
			setnilV(&baseline);
			setnilV(&package);
		}

		/// A non-empty peerMark brackets each fragment only this machine holds.
		std::string Serialize(const char* helperSource, std::unordered_set<uint64_t>& carried, const std::string& peerMark = {}) const {
			if (!helperSource || !heap.State() || !native) throw std::runtime_error("a frozen Lua graph is incomplete");
			if (!tvistab(&roots) || !tvistab(&globals)) throw std::runtime_error("a frozen Lua graph has invalid roots");
			if (!labels || !tvistab(&*labels)) throw std::runtime_error("a frozen Lua graph has no captured key-label table");
			// The worker VM holds no gameplay state, so its writes must not move the barrier's counts.
			LuaCheckpointBarrierIgnore quiet;
			const auto owner = this->worker ? this->worker : std::make_shared<GraphWorker>();
			std::lock_guard lock(owner->mutex);
			const bool first = !owner->state;
			if (first) {
				owner->state.reset(luaL_newstate());
				if (!owner->state) throw std::runtime_error("could not create the frozen graph worker");
				luaL_openlibs(owner->state.get());
				owner->context = std::make_shared<Context>(*this, carried);
			}
			lua_State* worker = owner->state.get();
			Context& context = *std::static_pointer_cast<Context>(owner->context);
			struct ClearStack { lua_State* state; ~ClearStack() { lua_settop(state, 0); } } clear{worker};
			context.Update(worker, *this, carried);
			if (first) context.view.Install(worker);
			context.view.scratch.insert(scratch.begin(), scratch.end());
			native->Bind(worker, context.view);
			lua_pushboolean(worker, true); lua_setglobal(worker, "_ScriptGraphFrozenDependencies");
			Bind(worker, context, "_ScriptGraphThreadCapture", Guard<ThreadCapture>);
			Bind(worker, context, "_ScriptGraphOpenUpvalues", Guard<OpenUpvalues>);
			Bind(worker, context, "_ScriptGraphRandomState", Guard<RandomState>);
			Bind(worker, context, "_ScriptGraphDirtyRoots", Guard<DirtyRoots>);
			Bind(worker, context, "_ScriptGraphClock", Clock);
			Bind(worker, context, "_ScriptGraphNoteCarried", Guard<NoteCarried>);
			Bind(worker, context, "_ScriptGraphBeginCapture", Guard<BeginCapture>);
			Bind(worker, context, "_ScriptGraphEndCapture", Guard<EndCapture>);
			Bind(worker, context, "_ScriptGraphBeginRoot", Guard<BeginRoot>);
			Bind(worker, context, "_ScriptGraphReuseRoot", Guard<ReuseRoot>);
			Bind(worker, context, "_ScriptGraphNoteTable", Guard<NoteValue>);
			Bind(worker, context, "_ScriptGraphNoteValue", Guard<NoteValue>);
			Bind(worker, context, "_ScriptGraphNoteUncacheable", Guard<NoteUncacheable>);
			Bind(worker, context, "_ScriptGraphNoteRootReuse", Guard<NoteRootReuse>);
			Bind(worker, context, "_ScriptGraphWalkPart", Guard<WalkPart>);
			// The capture's descriptor was taken out of the live globals before the freeze; the walk reads it under its name.
			if (tvistab(&callbacks)) context.view.Inject(tabV(&globals), "_ScriptGraphCallbacks", callbacks);
			context.view.Push(worker, globals); lua_setglobal(worker, "_G");
			context.view.Push(worker, baseline); lua_setglobal(worker, "_ScriptGraphBaseline");
			context.view.Push(worker, package); lua_setglobal(worker, "package");
			if (first) {
				Check(worker, luaL_loadstring(worker, helperSource), "could not load the frozen graph helper");
				lua_newtable(worker);
				lua_pushcfunction(worker, NotCapturing); lua_setfield(worker, -2, "active");
				if (!peerMark.empty()) { lua_pushlstring(worker, peerMark.data(), peerMark.size()); lua_setfield(worker, -2, "peerMark"); }
				context.view.Push(worker, *labels); lua_setfield(worker, -2, "keyLabels");
				Check(worker, lua_pcall(worker, 1, 0, 0), "could not initialize the frozen graph helper");
			}
			lua_getglobal(worker, "_ScriptGraph");
			if (!lua_istable(worker, -1)) throw std::runtime_error("the frozen graph helper supplied no graph interface");
			const int graph = lua_gettop(worker);
			lua_getfield(worker, graph, "replaceCaptureKeyLabels"); context.view.Push(worker, *labels);
			Check(worker, lua_pcall(worker, 1, 0, 0), "could not refresh frozen graph key labels");
			lua_getfield(worker, graph, "captureKeyLabels");
			if (!lua_isfunction(worker, -1)) throw std::runtime_error("the frozen graph helper has no key-label handoff");
			Check(worker, lua_pcall(worker, 0, 1, 0), "could not inspect frozen graph key labels");
			VerifyLabels(worker, context.view, lua_gettop(worker));
			lua_pop(worker, 1);
			lua_getfield(worker, graph, "serialize");
			if (!lua_isfunction(worker, -1)) throw std::runtime_error("the frozen graph helper supplied no serializer");
			context.view.Push(worker, roots);
			lua_pushnumber(worker, static_cast<lua_Number>(liveSerial));
			Check(worker, lua_pcall(worker, 2, 2, 0), "could not serialize the frozen Lua graph");
			const auto nativeFinish = std::chrono::steady_clock::now();
			if (!lua_istable(worker, -1)) throw std::runtime_error("the frozen graph serializer supplied no refusal list");
			std::vector<std::string> problems;
			const int refusals = lua_gettop(worker);
			lua_pushnil(worker);
			while (lua_next(worker, refusals)) {
				if (lua_type(worker, -1) != LUA_TSTRING) throw std::runtime_error("the frozen graph serializer supplied an invalid refusal");
				size_t size = 0;
				const char* message = lua_tolstring(worker, -1, &size);
				problems.emplace_back(message, size);
				lua_pop(worker, 1);
			}
			for (std::string& problem: native->UnreachedOwners(context.carriedObjects)) problems.push_back(std::move(problem));
			if (!problems.empty()) { context.walked = false; context.dependencies.clear(); throw ScriptGraphRefusal(std::move(problems)); }
			if (lua_type(worker, -2) != LUA_TSTRING) throw std::runtime_error("the frozen graph serializer supplied no archive text");
			size_t size = 0;
			const char* bytes = lua_tolstring(worker, -2, &size);
			context.Part("native_finish", 0, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - nativeFinish).count(), false, {});
			*observations = context.stats;
			return std::string(bytes, size);
		}

	private:
		struct Context {
			const GraphImage* image;
			std::unordered_set<uint64_t>* carried;
			std::unordered_set<uintptr_t> carriedObjects;
			PrototypeCapture prototypes;
			View view;
			using Root = std::pair<std::string, std::string>;
			struct Dependency { TValue value; uint64_t serial; std::string bytes, helper, argument; };
			using DependencyKey = std::tuple<const void*, std::string, std::string>;
			using Dependencies = std::map<DependencyKey, Dependency>;
			std::map<Root, Dependencies> dependencies, nextDependencies;
			std::optional<Root> current;
			GraphDirt stats;
			bool walked = false, noting = false;
			Context(const GraphImage& source, std::unordered_set<uint64_t>& sounds) : image(&source), carried(&sounds), prototypes(source.heap),
			    view(source.heap,
			        [this](lua_State* state, View& observer, std::string_view name) {
				        if (const auto value = observer.Value(state, 1)) {
					        std::string argument;
					        if (name == "_ScriptGraphNative") argument = lua_toboolean(state, 2) ? "1" : "0";
					        else if (name == "__index" && lua_type(state, 2) == LUA_TSTRING) {
						        size_t size = 0; const char* text = lua_tolstring(state, 2, &size); argument.assign(text, size);
					        }
					        if (tvisudata(&*value) || tvisfunc(&*value)) Note(*value, name, argument);
					        else Note(*value);
				        }
				        return image->native->Call(state, observer, name, carried, &carriedObjects);
			        }, [this](const GCproto* prototype) { return prototypes.Dump(prototype); }) {
				view.observe = [this](const TValue& value) { Note(value); };
			}
			void Update(lua_State* state, const GraphImage& source, std::unordered_set<uint64_t>& sounds) {
				image = &source; carried = &sounds; carriedObjects.clear(); current.reset();
				prototypes = PrototypeCapture(source.heap); view.Reset(state, source.heap);
			}
			void Part(std::string name, uint64_t root, int64_t us, bool reused, std::string unwatched) {
				stats.walkParts.push_back({image->stateIndex, std::move(name), root, us, reused, std::move(unwatched)});
			}
			std::string Fingerprint(const TValue& value, std::string_view helper = {}, std::string_view argument = {}, std::vector<TValue>* tables = nullptr) {
				if (!view.Alive(value)) return "dead";
				std::string bytes = helper.empty() ? view.Fingerprint(value) : view.Token(value);
				if (!helper.empty()) bytes += image->native->Fingerprint(gcval(&value), view, helper, argument,
					[&](const TValue& table) { if (tables) tables->push_back(table); });
				return bytes;
			}
			void Note(const TValue& value, std::string_view helper = {}, std::string_view argument = {}) {
				if (!current || noting || !tvisgcv(&value) || tvisstr(&value)) return;
				auto& values = nextDependencies[*current];
				const DependencyKey key{gcval(&value), std::string(helper), std::string(argument)};
				if (values.contains(key)) return;
				std::vector<TValue> tables;
				noting = true;
				std::string bytes;
				try { bytes = Fingerprint(value, helper, argument, &tables); } catch (...) { noting = false; throw; }
				noting = false;
				values.emplace(key, Dependency{value, view.SerialOf(value), std::move(bytes), std::string(helper), std::string(argument)});
				for (const TValue& table: tables) Note(table);
			}
		};

		static Context& Self(lua_State* state) { return *static_cast<Context*>(lua_touserdata(state, lua_upvalueindex(1))); }

		template<int (*Function)(lua_State*)> static int Guard(lua_State* state) {
			try { return Function(state); }
			catch (const std::exception& error) { lua_pushstring(state, error.what()); }
			catch (...) { lua_pushliteral(state, "unknown frozen graph worker failure"); }
			return lua_error(state);
		}

		static void Bind(lua_State* state, Context& context, const char* name, lua_CFunction function) {
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, function, 1);
			lua_setglobal(state, name);
		}

		static void Check(lua_State* state, int status, const char* operation) {
			if (status == 0) return;
			size_t size = 0;
			const char* bytes = lua_tolstring(state, -1, &size);
			std::string message = operation;
			if (bytes) { message += ": "; message.append(bytes, size); }
			lua_pop(state, 1);
			throw std::runtime_error(message);
		}

		static int BeginCapture(lua_State* state) {
			Context& context = Self(state);
			context.current.reset(); context.nextDependencies.clear(); context.stats = {};
			context.Part("native_setup", 0, context.image->nativeUs, false, {});
			return 0;
		}
		static int DirtyRoots(lua_State* state) {
			Context& context = Self(state);
			const auto start = std::chrono::steady_clock::now();
			std::set<std::string> roots, parts;
			context.noting = true;
			try {
				for (const auto& [root, dependencies]: context.dependencies) for (const auto& [key, saved]: dependencies) {
					bool same = false;
					try {
						same = context.view.Alive(saved.value) && context.view.SerialOf(saved.value) == saved.serial &&
						       context.Fingerprint(saved.value, saved.helper, saved.argument) == saved.bytes;
					} catch (const std::runtime_error&) {
						// A dependency retained by an older chunk may have expired. It
						// invalidates that chunk; fresh serialization still reports errors.
					}
					if (!same) {
						if (root.first == "0") parts.insert(root.second); else roots.insert(root.first);
						break;
					}
				}
			} catch (...) { context.noting = false; throw; }
			context.noting = false;
			context.Part("cache", 0, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count(), false, {});
			lua_newtable(state); lua_newtable(state);
			for (const auto& root: roots) { lua_pushboolean(state, true); lua_setfield(state, -2, root.c_str()); }
			lua_setfield(state, -2, "roots"); lua_newtable(state);
			for (const auto& part: parts) { lua_pushboolean(state, true); lua_setfield(state, -2, part.c_str()); }
			lua_setfield(state, -2, "parts"); lua_pushboolean(state, context.walked); lua_setfield(state, -2, "walked");
			return 1;
		}
		static Context::Root RootAt(lua_State* state) { return {luaL_optstring(state, 1, "0"), luaL_optstring(state, 2, "")}; }
		static int BeginRoot(lua_State* state) { auto& context = Self(state); context.current = RootAt(state); context.nextDependencies[*context.current].clear(); return 0; }
		static int ReuseRoot(lua_State* state) {
			auto& context = Self(state); context.current.reset(); const auto root = RootAt(state);
			context.nextDependencies[root] = context.dependencies.at(root); return 0;
		}
		static int NoteValue(lua_State* state) { auto& context = Self(state); if (const auto value = context.view.Value(state, 1)) context.Note(*value); return 0; }
		static int NoteUncacheable(lua_State* state) { Self(state).stats.uncacheableRoots = static_cast<size_t>(luaL_checknumber(state, 1)); return 0; }
		static int NoteRootReuse(lua_State* state) {
			auto& stats = Self(state).stats; stats.rootsReused = static_cast<size_t>(luaL_checknumber(state, 1));
			stats.rootsRewritten = static_cast<size_t>(luaL_checknumber(state, 2)); stats.roots = stats.rootsReused + stats.rootsRewritten; return 0;
		}
		static int WalkPart(lua_State* state) {
			Self(state).Part(luaL_checkstring(state, 1), std::strtoull(luaL_optstring(state, 2, "0"), nullptr, 10),
				static_cast<int64_t>(luaL_checknumber(state, 3)), lua_toboolean(state, 4) != 0, luaL_optstring(state, 5, "")); return 0;
		}
		static int EndCapture(lua_State* state) {
			auto& context = Self(state); const auto start = std::chrono::steady_clock::now();
			context.current.reset(); context.dependencies = std::move(context.nextDependencies); context.walked = true;
			for (const auto& [root, dependencies]: context.dependencies) for (const auto& [address, value]: dependencies) {
				if (tvistab(&value.value)) ++context.stats.tables; else ++context.stats.values;
			}
			context.Part("index_finish", 0, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count(), false, {});
			return 0;
		}
		static int NotCapturing(lua_State* state) { lua_pushboolean(state, false); return 1; }
		static int Clock(lua_State* state) {
			lua_pushnumber(state, static_cast<lua_Number>(std::chrono::duration_cast<std::chrono::microseconds>(
			    std::chrono::steady_clock::now().time_since_epoch()).count()));
			return 1;
		}
		static int RandomState(lua_State* state) {
			if (lua_gettop(state) != 0) throw std::runtime_error("a frozen graph cannot change its random state");
			const std::string& text = Self(state).image->rng.Text();
			lua_pushlstring(state, text.data(), text.size());
			return 1;
		}
		static int ThreadCapture(lua_State* state) {
			auto& view = Self(state).view;
			const auto value = view.Value(state, 1);
			if (!value || !tvisthread(&*value)) return ThreadDetail::Failure(state, "not a coroutine");
			return PushThreadDescription(state, view, reinterpret_cast<const lua_State*>(gcval(&*value)), lua_toboolean(state, 2) != 0);
		}
		static int OpenUpvalues(lua_State* state) { return PushOpenUpvalues(state, Self(state).view); }
		static int NoteCarried(lua_State* state) {
			Context& context = Self(state);
			context.image->native->NoteCarried(state, context.view, context.carriedObjects);
			return 0;
		}

		void VerifyLabels(lua_State* state, View& view, int actual) const {
			if (!lua_istable(state, actual)) throw std::runtime_error("the frozen graph key labels are not a table");
			size_t count = 0;
			const auto check = [&](const TValue& key, const TValue& value) {
				view.Push(state, key);
				lua_rawget(state, actual);
				view.Push(state, value);
				const bool equal = lua_rawequal(state, -1, -2) != 0;
				lua_pop(state, 2);
				if (!equal) throw std::runtime_error("the frozen graph helper did not import its key labels");
				++count;
			};
			const auto* source = reinterpret_cast<const GCtab*>(gcval(&*labels));
			const GCtab table = heap.Read(source);
			const TValue* array = mref(table.array, TValue);
			for (MSize index = 0; index < table.asize; ++index) {
				const TValue value = heap.Read(array + index);
				if (!tvisnil(&value)) { TValue key; setintV(&key, static_cast<int32_t>(index)); check(key, value); }
			}
			const Node* nodes = mref(table.node, Node);
			for (size_t index = 0; index <= table.hmask; ++index) {
				const Node node = heap.Read(nodes + index);
				if (!tvisnil(&node.val)) check(node.key, node.val);
			}
			size_t imported = 0;
			lua_pushnil(state);
			while (lua_next(state, actual)) { ++imported; lua_pop(state, 1); }
			if (imported != count) throw std::runtime_error("the frozen graph helper imported a different key-label set");
		}
	};
}
