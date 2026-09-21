#pragma once

#include "CheckpointLuaPrototype.h"
#include "CheckpointLuaNative.h"
#include "CheckpointLuaThread.h"

extern "C" {
#include "lualib.h"
}

namespace RTE::CheckpointLua {

	struct GraphImage {
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

		std::string Serialize(const char* helperSource, std::unordered_set<uint64_t>& carried) const {
			if (!helperSource || !heap.State() || !native) throw std::runtime_error("a frozen Lua graph is incomplete");
			if (!tvistab(&roots) || !tvistab(&globals)) throw std::runtime_error("a frozen Lua graph has invalid roots");
			if (!labels || !tvistab(&*labels)) throw std::runtime_error("a frozen Lua graph has no captured key-label table");
			Context context(*this, carried);
			// The worker VM holds no gameplay state, so its writes must not move the barrier's counts.
			LuaCheckpointBarrierIgnore quiet;
			std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
			if (!state) throw std::runtime_error("could not create the frozen graph worker");
			lua_State* worker = state.get();
			luaL_openlibs(worker);
			context.view.Install(worker);
			context.view.scratch.insert(scratch.begin(), scratch.end());
			native->Bind(worker, context.view);
			Bind(worker, context, "_ScriptGraphThreadCapture", Guard<ThreadCapture>);
			Bind(worker, context, "_ScriptGraphOpenUpvalues", Guard<OpenUpvalues>);
			Bind(worker, context, "_ScriptGraphRandomState", Guard<RandomState>);
			Bind(worker, context, "_ScriptGraphDirtyRoots", NoDirt);
			Bind(worker, context, "_ScriptGraphClock", Clock);
			Bind(worker, context, "_ScriptGraphNoteCarried", Guard<NoteCarried>);
			for (const char* name: {"_ScriptGraphBeginCapture", "_ScriptGraphEndCapture", "_ScriptGraphBeginRoot",
			         "_ScriptGraphReuseRoot", "_ScriptGraphNoteTable", "_ScriptGraphNoteValue",
			         "_ScriptGraphNoteUncacheable", "_ScriptGraphNoteRootReuse", "_ScriptGraphWalkPart"})
				Bind(worker, context, name, NoOp);
			// The capture's descriptor was taken out of the live globals before the freeze; the walk reads it under its name.
			if (tvistab(&callbacks)) context.view.Inject(tabV(&globals), "_ScriptGraphCallbacks", callbacks);
			context.view.Push(worker, globals); lua_setglobal(worker, "_G");
			context.view.Push(worker, baseline); lua_setglobal(worker, "_ScriptGraphBaseline");
			context.view.Push(worker, package); lua_setglobal(worker, "package");
			Check(worker, luaL_loadstring(worker, helperSource), "could not load the frozen graph helper");
			lua_newtable(worker);
			lua_pushcfunction(worker, NotCapturing); lua_setfield(worker, -2, "active");
			context.view.Push(worker, *labels); lua_setfield(worker, -2, "keyLabels");
			Check(worker, lua_pcall(worker, 1, 0, 0), "could not initialize the frozen graph helper");
			lua_getglobal(worker, "_ScriptGraph");
			if (!lua_istable(worker, -1)) throw std::runtime_error("the frozen graph helper supplied no graph interface");
			const int graph = lua_gettop(worker);
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
			if (!problems.empty()) throw ScriptGraphRefusal(std::move(problems));
			if (lua_type(worker, -2) != LUA_TSTRING) throw std::runtime_error("the frozen graph serializer supplied no archive text");
			size_t size = 0;
			const char* bytes = lua_tolstring(worker, -2, &size);
			return std::string(bytes, size);
		}

	private:
		struct Context {
			const GraphImage& image;
			std::unordered_set<uint64_t>& carried;
			std::unordered_set<uintptr_t> carriedObjects;
			PrototypeCapture prototypes;
			View view;
			Context(const GraphImage& source, std::unordered_set<uint64_t>& sounds) : image(source), carried(sounds), prototypes(source.heap),
			    view(source.heap,
			        [this](lua_State* state, View& observer, std::string_view name) { return image.native->Call(state, observer, name, &carried, &carriedObjects); },
			        [this](const GCproto* prototype) { return prototypes.Dump(prototype); }) {}
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

		static int NoOp(lua_State*) { return 0; }
		static int NoDirt(lua_State* state) { lua_pushnil(state); return 1; }
		static int NotCapturing(lua_State* state) { lua_pushboolean(state, false); return 1; }
		static int Clock(lua_State* state) {
			lua_pushnumber(state, static_cast<lua_Number>(std::chrono::duration_cast<std::chrono::microseconds>(
			    std::chrono::steady_clock::now().time_since_epoch()).count()));
			return 1;
		}
		static int RandomState(lua_State* state) {
			if (lua_gettop(state) != 0) throw std::runtime_error("a frozen graph cannot change its random state");
			const std::string& text = Self(state).image.rng.Text();
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
			context.image.native->NoteCarried(state, context.view, context.carriedObjects);
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
