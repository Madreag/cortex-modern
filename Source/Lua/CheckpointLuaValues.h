#pragma once

#include "CheckpointLuaHeap.h"
#include "CheckpointLuaPrototype.h"
#include "CheckpointLuaThread.h"

#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace RTE::CheckpointLua {
	// Getters execute with private tables, closures and native values on the saver.
	class SavedValues {
	public:
		using Native = std::function<void(lua_State*, const TValue&)>;
		SavedValues(lua_State* state, Snapshot heap, Native native) : m_State(state), m_Heap(std::move(heap)), m_Prototypes(m_Heap), m_Native(std::move(native)) {}
		~SavedValues() { for (const auto& [source, reference]: m_Values) luaL_unref(m_State, LUA_REGISTRYINDEX, reference); }
		SavedValues(const SavedValues&) = delete;
		SavedValues& operator=(const SavedValues&) = delete;

		void Remember(const TValue& source, int index) {
			if (!tvisgcv(&source) || tvisstr(&source)) throw std::logic_error("saved identity needs an object");
			index = Absolute(index);
			const auto found = m_Values.find(gcval(&source));
			if (found != m_Values.end()) throw std::logic_error("saved identity is already remembered");
			lua_pushvalue(m_State, index);
			const int reference = luaL_ref(m_State, LUA_REGISTRYINDEX);
			try {
				m_Values.emplace(gcval(&source), reference);
				m_Originals.emplace(lua_topointer(m_State, index), source);
			} catch (...) { m_Values.erase(gcval(&source)); luaL_unref(m_State, LUA_REGISTRYINDEX, reference); throw; }
		}
		std::optional<TValue> Original(int index) const {
			const auto found = m_Originals.find(lua_topointer(m_State, index));
			return found == m_Originals.end() ? std::nullopt : std::optional<TValue>{found->second};
		}
		void RememberLibraries(const TValue& source, int destination) {
			std::unordered_set<const GCtab*> seen;
			Libraries(source, Absolute(destination), seen);
		}
		void Push(const TValue& value) {
			if (tvisnil(&value)) { lua_pushnil(m_State); return; }
			if (tvisbool(&value)) { lua_pushboolean(m_State, tvistrue(&value)); return; }
			if (tvisint(&value)) { lua_pushinteger(m_State, intV(&value)); return; }
			if (tvisnum(&value)) { lua_pushnumber(m_State, numV(&value)); return; }
			if (tvisstr(&value)) {
				const auto* string = strV(&value);
				const auto bytes = m_Heap.ReadBytes(string + 1, m_Heap.Read(&string->len));
				lua_pushlstring(m_State, reinterpret_cast<const char*>(bytes.data()), bytes.size()); return;
			}
			if (tvislightud(&value)) { m_Native(m_State, value); return; }
			if (const auto found = m_Values.find(gcval(&value)); found != m_Values.end()) {
				lua_rawgeti(m_State, LUA_REGISTRYINDEX, found->second); return;
			}
			if (tvistab(&value)) { Table(value); return; }
			if (tvisfunc(&value)) { Function(value); return; }
			if (tvisthread(&value)) { Thread(value); return; }
			m_Native(m_State, value);
			Remember(value, -1);
		}
		void Push(lua_State* destination, const TValue& value) {
			if (destination != m_State) throw std::logic_error("saved values belong to a different Lua state");
			Push(value);
		}
		void Finish() {
			const auto state = m_Heap.Read(m_Heap.State());
			const auto global = m_Heap.Read(mref(state.glref, global_State));
			struct Open { const lua_State* thread; ptrdiff_t slot; };
			std::unordered_map<const GCupval*, Open> owners;
			for (const GCobj* object = gcref(global.gc.root); object;) {
				const auto head = m_Heap.Read(&object->gch);
				if (head.gct == static_cast<uint8_t>(~LJ_TTHREAD)) {
					const auto* original = reinterpret_cast<const lua_State*>(object);
					const auto thread = m_Heap.Read(original);
					for (const GCobj* item = gcref(thread.openupval); item;) {
						const auto* identity = reinterpret_cast<const GCupval*>(item);
						const auto cell = m_Heap.Read(identity);
						owners.emplace(identity, Open{original, mref(cell.v, TValue) - tvref(thread.stack)});
						item = gcref(cell.nextgc);
					}
				}
				object = gcref(head.nextgc);
			}
			std::unordered_set<const GCupval*> joined;
			for (;;) {
				std::vector<const GCupval*> pending;
				for (const auto& [identity, cell]: m_Cells) if (owners.contains(identity) && !joined.contains(identity)) pending.push_back(identity);
				if (pending.empty()) break;
				for (const GCupval* identity: pending) {
					const Open owner = owners.at(identity);
					TValue token; setgcVraw(&token, const_cast<GCobj*>(obj2gco(owner.thread)), LJ_TTHREAD);
					Push(token); lua_pop(m_State, 1);
					for (const Cell cell: m_CellUses.at(identity)) {
						lua_getglobal(m_State, "_ScriptGraphJoinOpenUpvalue");
						lua_rawgeti(m_State, LUA_REGISTRYINDEX, cell.function); lua_pushinteger(m_State, cell.index);
						lua_rawgeti(m_State, LUA_REGISTRYINDEX, m_Values.at(obj2gco(owner.thread))); lua_pushinteger(m_State, static_cast<lua_Integer>(owner.slot));
						Call(4, 2);
						if (!lua_toboolean(m_State, -2)) throw std::runtime_error(lua_tostring(m_State, -1) ? lua_tostring(m_State, -1) : "saved open upvalue could not be joined");
						lua_pop(m_State, 2);
						lua_rawgeti(m_State, LUA_REGISTRYINDEX, cell.function);
						luaJIT_set_upvalue_serial(m_State, -1, cell.index, m_Heap.Read(&identity->serial)); lua_pop(m_State, 1);
					}
					joined.insert(identity);
				}
			}
		}
		TValue Lookup(const TValue& table, std::string_view name) {
			const int top = lua_gettop(m_State);
			Push(table); lua_pushlstring(m_State, name.data(), name.size()); lua_gettable(m_State, -2);
			const auto source = Original(-1);
			if (!source) {
				const TValue value = *(m_State->top - 1);
				if (tvispri(&value) || tvisnumber(&value)) { lua_settop(m_State, top); return value; }
				lua_settop(m_State, top); throw std::runtime_error("saved lookup returned a new value without recorded storage");
			}
			lua_settop(m_State, top); return *source;
		}
		const Snapshot& Heap() const { return m_Heap; }

		static std::string SelfTestMismatch() {
			auto source = HeapOwner::Create();
			lua_State* const live = source->State();
			ProtectedCall(live, [&] {
				luaL_openlibs(live);
				if (luaL_dostring(live, "savedTable={value=11}; savedAlias=savedTable; local n=7; savedGetter=function() n=n+1; savedTable.value=savedTable.value+n; return n,savedTable,savedAlias end; savedOther=function() return n end; savedThread=coroutine.create(function() local value=23; savedCell=function(n) if n then value=n end return value end; savedCellAlias=function() return value end; coroutine.yield(value); return value end); assert(coroutine.resume(savedThread))") != 0)
					throw std::runtime_error("could not create saved getters");
			});
			const Snapshot heap = source->Freeze([](std::function<void()> work) {
				return std::async(std::launch::deferred, std::move(work));
			});
			const TValue getter = heap.GlobalValue("savedGetter"), other = heap.GlobalValue("savedOther");
			ProtectedCall(live, [&] { if (luaL_dostring(live, "savedTable.value=101; savedGetter(); savedGetter=nil; savedOther=nil; collectgarbage('collect')") != 0) throw std::runtime_error("could not change live getters"); });
			source.reset();
			auto worker = HeapOwner::Create();
			lua_State* const state = worker->State();
			std::string mismatch;
			ProtectedCall(state, [&] {
				luaL_openlibs(state);
				LuaThreadCodec::Register(state);
				SavedValues values(state, heap, [](lua_State*, const TValue&) { throw std::runtime_error("saved getter touched native state"); });
				lua_pushvalue(state, LUA_GLOBALSINDEX); values.RememberLibraries(heap.GlobalValue("_G"), -1); lua_pop(state, 1);
				values.Push(getter); values.Push(other); lua_pop(state, 1);
				if (lua_pcall(state, 0, 3, 0) != 0) throw std::runtime_error(lua_tostring(state, -1));
				if (lua_tointeger(state, -3) != 8 || !lua_rawequal(state, -2, -1)) mismatch = "saved getter lost an upvalue or table alias";
				lua_getfield(state, -1, "value");
				if (lua_tointeger(state, -1) != 19) mismatch = "saved getter read the later live table";
				lua_settop(state, 0); values.Push(other);
				if (lua_pcall(state, 0, 1, 0) != 0) throw std::runtime_error(lua_tostring(state, -1));
				if (lua_tointeger(state, -1) != 8) mismatch = "saved closures lost their shared upvalue";
				lua_settop(state, 0); values.Push(heap.GlobalValue("savedCell")); values.Push(heap.GlobalValue("savedCellAlias")); lua_pop(state, 1); values.Finish();
				lua_pushinteger(state, 31);
				if (lua_pcall(state, 1, 1, 0) != 0 || lua_tointeger(state, -1) != 31) mismatch = "saved open upvalue could not be changed";
				lua_settop(state, 0); values.Push(heap.GlobalValue("savedCellAlias"));
				if (lua_pcall(state, 0, 1, 0) != 0 || lua_tointeger(state, -1) != 31) mismatch = "saved open upvalue lost a closure alias";
				lua_settop(state, 0); lua_getglobal(state, "coroutine"); lua_getfield(state, -1, "resume"); values.Push(heap.GlobalValue("savedThread"));
				if (lua_pcall(state, 1, 2, 0) != 0 || !lua_toboolean(state, -2) || lua_tointeger(state, -1) != 31) mismatch = "saved coroutine lost its shared open upvalue";
			});
			return mismatch;
		}

	private:
		lua_State* m_State;
		Snapshot m_Heap;
		PrototypeCapture m_Prototypes;
		Native m_Native;
		std::unordered_map<const GCobj*, int> m_Values;
		std::unordered_map<const void*, TValue> m_Originals;
		struct Cell { int function; int index; };
		std::unordered_map<const GCupval*, Cell> m_Cells;
		std::unordered_map<const GCupval*, std::vector<Cell>> m_CellUses;
		int Absolute(int index) const { return index < 0 ? lua_gettop(m_State) + index + 1 : index; }
		void Call(int arguments, int results) {
			const int status = lua_pcall(m_State, arguments, results, 0);
			if (status == LUA_ERRMEM) throw std::bad_alloc();
			if (status) throw std::runtime_error(lua_tostring(m_State, -1) ? lua_tostring(m_State, -1) : "saved Lua call failed");
		}
		void Thread(const TValue& value) {
			const auto* source = threadV(&value);
			lua_State* const thread = lua_newthread(m_State);
			const int destination = lua_gettop(m_State);
			Remember(value, destination);
			lua_getglobal(m_State, "_ScriptGraphThreadRestore");
			if (PushThreadDescription(m_State, *this, source, true) != 1) throw std::runtime_error(lua_tostring(m_State, -1));
			lua_pushvalue(m_State, destination);
			Call(2, 2);
			if (!lua_isthread(m_State, -2)) throw std::runtime_error(lua_tostring(m_State, -1) ? lua_tostring(m_State, -1) : "saved coroutine could not be rebuilt");
			lua_settop(m_State, destination);
			thread->serial = m_Heap.Read(&source->serial);
		}
		void Libraries(const TValue& source, int destination, std::unordered_set<const GCtab*>& seen) {
			if (!tvistab(&source) || !lua_istable(m_State, destination) || !seen.insert(tabV(&source)).second) return;
			const auto table = m_Heap.Read(tabV(&source));
			const auto item = [&](const TValue& key, const TValue& value) {
				if (!tvisstr(&key) && !tvisnumber(&key)) return;
				Push(key); lua_rawget(m_State, destination);
				if (tvisfunc(&value) && m_Heap.Read(&funcV(&value)->c.ffid) != 0 && lua_iscfunction(m_State, -1)) {
					const auto* target = funcV(m_State->top - 1);
					if (target->c.f == m_Heap.Read(&funcV(&value)->c.f) && !m_Values.contains(gcval(&value))) Remember(value, -1);
				} else if (tvisudata(&value) && lua_isuserdata(m_State, -1) && !m_Values.contains(gcval(&value))) Remember(value, -1);
				else if (tvistab(&value)) Libraries(value, lua_gettop(m_State), seen);
				lua_pop(m_State, 1);
			};
			const auto* array = tvref(table.array);
			for (MSize index = 0; index < table.asize; ++index) {
				const TValue value = m_Heap.Read(array + index);
				if (!tvisnil(&value)) { TValue key; setintV(&key, static_cast<int32_t>(index)); item(key, value); }
			}
			const auto* nodes = noderef(table.node);
			for (MSize index = 0; index <= table.hmask; ++index) {
				const Node node = m_Heap.Read(nodes + index);
				if (!tvisnil(&node.val)) item(node.key, node.val);
			}
		}
		void Table(const TValue& value) {
			const GCtab table = m_Heap.Read(tabV(&value));
			if (table.asize > static_cast<MSize>((std::numeric_limits<int>::max)()) || table.hmask > static_cast<MSize>((std::numeric_limits<int>::max)())) throw std::length_error("saved table is too large");
			lua_createtable(m_State, static_cast<int>(table.asize), static_cast<int>(table.hmask));
			const int destination = lua_gettop(m_State);
			Remember(value, destination);
			const TValue* array = tvref(table.array);
			for (MSize index = 0; index < table.asize; ++index) {
				const TValue item = m_Heap.Read(array + index);
				if (!tvisnil(&item)) { Push(item); lua_rawseti(m_State, destination, static_cast<int>(index)); }
			}
			const Node* nodes = noderef(table.node);
			for (MSize index = 0; index <= table.hmask; ++index) {
				const Node node = m_Heap.Read(nodes + index);
				if (!tvisnil(&node.val)) { Push(node.key); Push(node.val); lua_rawset(m_State, destination); }
			}
			if (const GCobj* meta = gcref(table.metatable)) {
				TValue token; setgcVraw(&token, const_cast<GCobj*>(meta), LJ_TTAB); Push(token); lua_setmetatable(m_State, destination);
			}
			const TValue result = *(m_State->base + destination - 1);
			tabV(&result)->serial = table.serial;
		}
		void Function(const TValue& value) {
			const GCfunc* source = funcV(&value);
			const uint8_t upvalues = m_Heap.Read(&source->c.nupvalues);
			if (m_Heap.Read(&source->c.ffid) != 0) {
				// Native closures need their immutable binding or library owner supplied by the caller.
				m_Native(m_State, value); Remember(value, -1); return;
			}
			const auto* prototype = reinterpret_cast<const GCproto*>(mref(m_Heap.Read(&source->l.pc), BCIns) - sizeof(GCproto) / sizeof(BCIns));
			const std::string code = m_Prototypes.Dump(prototype);
			if (luaL_loadbuffer(m_State, code.data(), code.size(), "saved getter") != 0) throw std::runtime_error(lua_tostring(m_State, -1));
			const int destination = lua_gettop(m_State);
			Remember(value, destination);
			const int reference = m_Values.at(gcval(&value));
			funcV(m_State->base + destination - 1)->c.serial = m_Heap.Read(&source->c.serial);
			for (int index = 1; index <= upvalues; ++index) {
				const auto* identity = gco2uv(gcref(m_Heap.Read(&source->l.uvptr[index - 1])));
				m_CellUses[identity].push_back({reference, index});
				if (const auto found = m_Cells.find(identity); found != m_Cells.end()) {
					lua_rawgeti(m_State, LUA_REGISTRYINDEX, found->second.function);
					lua_upvaluejoin(m_State, destination, index, -1, found->second.index); lua_pop(m_State, 1);
				} else {
					m_Cells.emplace(identity, Cell{reference, index});
					const GCupval cell = m_Heap.Read(identity);
					Push(cell.closed ? cell.tv : m_Heap.Read(mref(cell.v, TValue)));
					if (!lua_setupvalue(m_State, destination, index)) throw std::runtime_error("saved function lost an upvalue");
					luaJIT_set_upvalue_serial(m_State, destination, index, cell.serial);
				}
			}
			TValue environment; setgcVraw(&environment, gcref(m_Heap.Read(&source->c.env)), LJ_TTAB);
			Push(environment); lua_setfenv(m_State, destination);
		}
	};
}
