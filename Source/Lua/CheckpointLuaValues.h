#pragma once

#include "CheckpointLuaHeap.h"
#include "CheckpointLuaPrototype.h"
#include "CheckpointLuaThread.h"

extern "C" {
#include "lj_tab.h"
}

#include <bit>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace RTE::CheckpointLua {
	// Getters execute with private tables, closures and native values on the saver.
	class SavedValues {
	public:
		using Native = std::function<void(lua_State*, const TValue&)>;
		SavedValues(lua_State* state, Snapshot heap, Native native) : m_State(state), m_Heap(std::move(heap)), m_Prototypes(m_Heap), m_Native(std::move(native)) {
			PrepareStrings();
			const luaJIT_SavedKeys keys{state, this, KeyHash};
			m_PreviousKeys = luaJIT_set_saved_keys(&keys);
		}
		~SavedValues() {
			if (m_State) {
				for (const auto& [source, reference]: m_Values) luaL_unref(m_State, LUA_REGISTRYINDEX, reference);
				for (const auto& [value, reference]: m_Returned) luaL_unref(m_State, LUA_REGISTRYINDEX, reference);
			}
			luaJIT_set_saved_keys(&m_PreviousKeys);
		}
		// Finalizers keep their saved aliases until the private state finishes closing.
		void Closing() noexcept { m_State = nullptr; }
		SavedValues(const SavedValues&) = delete;
		SavedValues& operator=(const SavedValues&) = delete;

		void Remember(const TValue& source, int index) {
			if (!tvisgcv(&source)) throw std::logic_error("saved identity needs an object");
			index = Absolute(index);
			const auto found = m_Values.find(gcval(&source));
			if (found != m_Values.end()) throw std::logic_error("saved identity is already remembered");
			lua_pushvalue(m_State, index);
			const int reference = luaL_ref(m_State, LUA_REGISTRYINDEX);
			const void* identity = lua_topointer(m_State, index);
			bool originalInserted = false;
			try {
				m_Values.emplace(gcval(&source), reference);
				originalInserted = m_Originals.emplace(identity, source).second;
				TValue& target = m_State->base[index - 1];
				if (tvisudata(&source) && tvisudata(&target)) udataV(&target)->serial = m_Heap.Read(&udataV(&source)->serial);
				else if (tvisfunc(&source) && tvisfunc(&target)) funcV(&target)->c.serial = m_Heap.Read(&funcV(&source)->c.serial);
			} catch (...) {
				m_Values.erase(gcval(&source));
				if (originalInserted) m_Originals.erase(identity);
				luaL_unref(m_State, LUA_REGISTRYINDEX, reference);
				throw;
			}
		}
		bool Remembered(const TValue& value) const { return tvisgcv(&value) && m_Values.contains(gcval(&value)); }
		void UseHeap(Snapshot heap) { m_Heap = std::move(heap); m_Prototypes = PrototypeCapture(m_Heap); }
		void FillTable(const TValue& source, int destination) {
			if (!tvistab(&source) || !lua_istable(m_State, destination)) throw std::logic_error("saved table copy needs two tables");
			destination = Absolute(destination);
			if (!Remembered(source)) Remember(source, destination);
			lj_tab_clear(m_State, tabV(m_State->base + destination - 1));
			TableContents(source, destination);
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
				if (const auto found = m_Values.find(gcval(&value)); found != m_Values.end()) { lua_rawgeti(m_State, LUA_REGISTRYINDEX, found->second); return; }
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
			if (!Remembered(value)) Remember(value, -1);
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
				try {
					if (!tvisgcv(&value)) throw std::runtime_error("a saved getter returned a pointer without an owned value");
					m_Heap.Read(&gcval(&value)->gch);
					if (!tvisstr(&value)) Remember(value, -1);
					else if (!m_Returned.contains(gcval(&value))) {
						lua_pushvalue(m_State, -1);
						const int reference = luaL_ref(m_State, LUA_REGISTRYINDEX);
						try { m_Returned.emplace(gcval(&value), reference); }
						catch (...) { luaL_unref(m_State, LUA_REGISTRYINDEX, reference); throw; }
					}
				} catch (...) { lua_settop(m_State, top); throw; }
				lua_settop(m_State, top); return value;
			}
			lua_settop(m_State, top); return *source;
		}
		const Snapshot& Heap() const { return m_Heap; }

		static std::string SelfTestMismatch() {
			auto source = HeapOwner::Create();
			lua_State* const live = source->State();
			ProtectedCall(live, [&] {
				luaL_openlibs(live);
				if (luaL_dostring(live, R"lua(
savedOrder={}
for i=1,48 do savedOrder['slot'..i]=i end
for i=1,12 do savedOrder[3*i]=100+i end
savedOrder.slot7=nil; savedOrder.slot31=nil
savedKey={}; savedFunctionKey=function() return 23 end
savedOrder[savedKey]=777; savedOrder[savedFunctionKey]=778
savedWalk=function() local values={} for _,value in pairs(savedOrder) do values[#values+1]=value end return table.concat(values,',') end
savedRead=function(key) local total=0 for i=1,192 do total=total+savedOrder[key] end return total end
for i=1,80 do assert(savedRead(savedKey)==777*192) end
savedOrderExpected=savedWalk()
savedOrderThread=coroutine.create(function() for _,value in pairs(savedOrder) do coroutine.yield(value) end return 'done' end)
assert(coroutine.resume(savedOrderThread))
)lua") != 0) throw std::runtime_error("could not create saved table traversal");
				if (luaL_dostring(live, "savedTable={value=11}; savedAlias=savedTable; savedGenerated=setmetatable({}, {__index=function(_,key) if key=='text' then return 'private-'..tostring(37) end return {answer=37} end}); local n=7; savedGetter=function() n=n+1; savedTable.value=savedTable.value+n; return n,savedTable,savedAlias end; savedOther=function() return n end; savedThread=coroutine.create(function() local value=23; savedCell=function(n) if n then value=n end return value end; savedCellAlias=function() return value end; coroutine.yield(value); return value end); assert(coroutine.resume(savedThread))") != 0)
					throw std::runtime_error("could not create saved getters");
			});
			const Snapshot heap = source->Freeze([](std::function<void()> work) {
				return std::async(std::launch::deferred, std::move(work));
			});
			const TValue getter = heap.GlobalValue("savedGetter"), other = heap.GlobalValue("savedOther");
			const TValue order = heap.GlobalValue("savedOrderExpected");
			const std::string expected = heap.ReadString(strdata(strV(&order)), heap.Read(&strV(&order)->len));
			ProtectedCall(live, [&] { if (luaL_dostring(live, "savedTable.value=101; savedGetter(); savedGetter=nil; savedOther=nil; collectgarbage('collect')") != 0) throw std::runtime_error("could not change live getters"); });
			source.reset();
			auto worker = HeapOwner::Create();
			lua_State* const state = worker->State();
			if (heap.OriginalBytes(state, sizeof(lua_State))) return "a private VM reused saved source addresses";
			std::string mismatch;
			ProtectedCall(state, [&] {
				luaL_openlibs(state);
				LuaThreadCodec::Register(state);
				SavedValues values(state, heap.WithPrivate(worker->PrivateReader()), [](lua_State*, const TValue&) { throw std::runtime_error("saved getter touched native state"); });
				lua_pushvalue(state, LUA_GLOBALSINDEX); values.RememberLibraries(heap.GlobalValue("_G"), -1); lua_pop(state, 1);
				values.Push(heap.GlobalValue("savedWalk"));
				if (lua_pcall(state, 0, 1, 0) != 0 || !lua_isstring(state, -1) || expected != lua_tostring(state, -1))
					mismatch = "saved table traversal changed its physical order";
				lua_settop(state, 0);
				for (const auto key: {"savedKey", "savedFunctionKey"}) {
					values.Push(heap.GlobalValue("savedRead")); values.Push(heap.GlobalValue(key));
					if (lua_pcall(state, 1, 1, 0) != 0 || lua_tointeger(state, -1) != (std::string_view(key) == "savedKey" ? 777 : 778) * 192)
						mismatch = "saved compiled lookup lost a captured object key";
					lua_settop(state, 0);
				}
				values.Push(heap.GlobalValue("savedOrderThread")); values.Finish();
				lua_setglobal(state, "savedOrderResume");
				if (luaL_dostring(state, "local values={} while coroutine.status(savedOrderResume)~='dead' do local ok,value=coroutine.resume(savedOrderResume); assert(ok,value); if value~='done' then values[#values+1]=value end end savedOrderRest=table.concat(values,',')") != 0)
					throw std::runtime_error("saved table iterator could not resume");
				lua_getglobal(state, "savedOrderRest");
				if (!lua_isstring(state, -1) || expected.substr(expected.find(',') + 1) != lua_tostring(state, -1))
					mismatch = "saved suspended iterator changed its following keys";
				lua_settop(state, 0);
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
				lua_settop(state, 0);
				const auto generated = heap.GlobalValue("savedGenerated");
				const TValue text = values.Lookup(generated, "text"), table = values.Lookup(generated, "table");
				lua_gc(state, LUA_GCCOLLECT, 0);
				const TValue answer = values.Heap().TableValue(tabV(&table), "answer");
				if (!tvisstr(&text) || values.Heap().ReadString(strdata(strV(&text)), values.Heap().Read(&strV(&text)->len)) != "private-37" ||
				    !tvistab(&table) || !tvisnumber(&answer) || numberVnum(&answer) != 37)
					mismatch = "saved getter returned storage that did not survive private collection";
				values.Push(table); lua_setfield(state, LUA_GLOBALSINDEX, "savedPinned");
				if (luaL_dostring(state, "assert(savedPinned.answer==37); savedPinned.answer=41") != 0)
					throw std::runtime_error("the saved getter table lost its retained identity");
				const TValue changed = values.Heap().TableValue(tabV(&table), "answer");
				if (!tvisnumber(&changed) || numberVnum(&changed) != 41) mismatch = "saved getter table changes went to a different value";
			});
			const Snapshot privateHeap = heap.WithPrivate(worker->PrivateReader());
			const auto* closed = state;
			worker.reset();
			bool refused = false;
			try { privateHeap.Read(closed); } catch (const std::runtime_error&) { refused = true; }
			if (!refused) mismatch = "saved getter storage stayed readable after its private VM closed";
			return mismatch;
		}

	private:
		lua_State* m_State;
		Snapshot m_Heap;
		PrototypeCapture m_Prototypes;
		Native m_Native;
		std::unordered_map<const GCobj*, int> m_Values;
		std::unordered_map<const void*, TValue> m_Originals;
		std::unordered_map<const GCobj*, int> m_Returned;
		std::unordered_set<const GCtab*> m_SavedTables;
		luaJIT_SavedKeys m_PreviousKeys{};
		struct Cell { int function; int index; };
		std::unordered_map<const GCupval*, Cell> m_Cells;
		std::unordered_map<const GCupval*, std::vector<Cell>> m_CellUses;
		int Absolute(int index) const { return index < 0 ? lua_gettop(m_State) + index + 1 : index; }
		static uint32_t KeyHash(void* owner, const void* table, uint64_t key, uint32_t ordinary) noexcept {
			const auto& values = *static_cast<const SavedValues*>(owner);
			if (!values.m_SavedTables.contains(static_cast<const GCtab*>(table))) return ordinary;
			TValue token{};
#if LJ_GC64
			token.u64 = key;
#else
			token.gcr.gcptr32 = static_cast<uint32_t>(key);
#endif
			const auto found = values.m_Originals.find(gcval(&token));
			if (found == values.m_Originals.end()) return ordinary;
			const auto original = gcrefu(found->second.gcr);
#if LJ_GC64
			return hashrot(static_cast<uint32_t>(original), static_cast<uint32_t>(original >> 32));
#else
			return hashrot(original, original + HASH_BIAS);
#endif
		}
		void PrepareStrings() {
			const auto state = m_Heap.Read(m_Heap.State());
			const auto* identity = mref(state.glref, global_State);
			const auto global = m_Heap.Read(identity);
			const auto string = [&](const GCstr* source) {
				const auto value = m_Heap.Read(source);
				const auto bytes = m_Heap.ReadBytes(source + 1, value.len);
				lua_pushlstring(m_State, reinterpret_cast<const char*>(bytes.data()), bytes.size());
				strV(m_State->top - 1)->sid = value.sid;
				TValue token; setgcVraw(&token, const_cast<GCobj*>(obj2gco(source)), LJ_TSTR);
				Remember(token, -1); lua_pop(m_State, 1);
			};
			string(&identity->strempty);
			for (MSize slot = 0; slot <= global.str.mask; ++slot) {
				for (const GCobj* item = gcref(m_Heap.Read(global.str.tab + slot)); item;) {
					const auto* source = reinterpret_cast<const GCstr*>(item);
					string(source); item = gcref(m_Heap.Read(&source->nextgc));
				}
			}
			// Existing private library tables must follow the restored string IDs too.
			for (GCobj* item = gcref(G(m_State)->gc.root); item;) {
				GCobj* const next = gcref(item->gch.nextgc);
				if (item->gch.gct == static_cast<uint8_t>(~LJ_TTAB)) {
					auto* table = gco2tab(item);
					lj_tab_resize(m_State, table, table->asize, table->hmask ? std::bit_width(table->hmask) : 0);
				}
				item = next;
			}
			G(m_State)->str.id = global.str.id; G(m_State)->str.idreseed = global.str.idreseed;
		}
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
				} else if (tvisudata(&value) && m_Heap.Read(&udataV(&value)->udtype) == UDTYPE_IO_FILE && lua_isuserdata(m_State, -1)) {
					const TValue target = *(m_State->top - 1);
					if (tvisudata(&target) && udataV(&target)->udtype == UDTYPE_IO_FILE && !Remembered(value)) Remember(value, -1);
				} else if (tvistab(&value)) Libraries(value, lua_gettop(m_State), seen);
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
			lua_createtable(m_State, 0, 0);
			const int destination = lua_gettop(m_State);
			Remember(value, destination);
			TableContents(value, destination);
		}
		void TableContents(const TValue& value, int destination) {
			const GCtab table = m_Heap.Read(tabV(&value));
			if (m_Heap.OriginalBytes(tabV(&value), sizeof(GCtab))) { TableSlots(table, destination); return; }
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
		void TableSlots(const GCtab& source, int destination) {
			GCtab* const target = tabV(m_State->base + destination - 1);
			if (source.hmask && !std::has_single_bit(source.hmask + 1)) throw std::runtime_error("a saved table has an invalid hash size");
			lj_tab_resize(m_State, target, source.asize, source.hmask ? std::bit_width(source.hmask) : 0);
			m_SavedTables.insert(target);
			for (MSize index = 0; index < source.asize; ++index) {
				Push(m_Heap.Read(tvref(source.array) + index));
				arrayslot(target, index)[0] = *(m_State->top - 1); lj_gc_anybarriert(m_State, target); lua_pop(m_State, 1);
			}
			if (source.hmask) {
				const Node* const original = noderef(source.node);
				Node* const nodes = noderef(target->node);
				for (MSize index = 0; index <= source.hmask; ++index) {
					const Node node = m_Heap.Read(original + index);
					if (!tvisnil(&node.val)) {
						Push(node.key); Push(node.val);
						nodes[index].key = *(m_State->top - 2); nodes[index].val = *(m_State->top - 1);
						lj_gc_anybarriert(m_State, target); lua_pop(m_State, 2);
					} else {
						// A nil slot may retain a dead key; it must never be dereferenced.
						nodes[index].key = node.key; setnilV(&nodes[index].val);
						if (Remembered(node.key)) { Push(node.key); nodes[index].key = *(m_State->top - 1); lua_pop(m_State, 1); }
					}
					const Node* const next = nextnode(&node);
					if (next && (next < original || next > original + source.hmask)) throw std::runtime_error("a saved table has an invalid hash chain");
					setmref(nodes[index].next, next ? nodes + (next - original) : nullptr);
				}
				const Node first = m_Heap.Read(original);
				const Node* const top = getfreetop(&source, &first);
				if (top < original || top > original + source.hmask + 1) throw std::runtime_error("a saved table has an invalid free slot");
				setfreetop(target, nodes, nodes + (top - original));
			}
			if (const GCobj* meta = gcref(source.metatable)) {
				TValue token; setgcVraw(&token, const_cast<GCobj*>(meta), LJ_TTAB); Push(token); lua_setmetatable(m_State, destination);
			}
			target->serial = source.serial; target->nomm = source.nomm;
		}
		void Function(const TValue& value) {
			const GCfunc* source = funcV(&value);
			const uint8_t upvalues = m_Heap.Read(&source->c.nupvalues);
			if (m_Heap.Read(&source->c.ffid) != 0) {
				// Native closures need their immutable binding or library owner supplied by the caller.
				m_Native(m_State, value); if (!Remembered(value)) Remember(value, -1); return;
			}
			const auto* prototype = reinterpret_cast<const GCproto*>(mref(m_Heap.Read(&source->l.pc), BCIns) - sizeof(GCproto) / sizeof(BCIns));
			const std::string code = m_Prototypes.Dump(prototype);
			if (luaL_loadbuffer(m_State, code.data(), code.size(), "saved getter") != 0) throw std::runtime_error(lua_tostring(m_State, -1));
			const int destination = lua_gettop(m_State);
			Remember(value, destination);
			PrototypeTables(prototype, funcproto(funcV(m_State->base + destination - 1)));
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
		void PrototypeTables(const GCproto* source, GCproto* target) {
			const auto values = m_Heap.Read(source);
			if (values.sizekgc != target->sizekgc) throw std::runtime_error("a saved function changed its constant count");
			for (ptrdiff_t index = -static_cast<ptrdiff_t>(values.sizekgc); index < 0; ++index) {
				const GCobj* const original = gcref(m_Heap.Read(mref(values.k, GCRef) + index));
				GCobj* const copy = proto_kgc(target, index);
				const auto type = m_Heap.Read(&original->gch.gct);
				if (type != copy->gch.gct) throw std::runtime_error("a saved function changed a constant type");
				if (type == static_cast<uint8_t>(~LJ_TPROTO)) PrototypeTables(gco2pt(original), gco2pt(copy));
				else if (type == static_cast<uint8_t>(~LJ_TTAB)) {
					TValue token; setgcVraw(&token, const_cast<GCobj*>(original), LJ_TTAB);
					settabV(m_State, m_State->top, gco2tab(copy)); incr_top(m_State);
					if (!Remembered(token)) Remember(token, -1);
					TableContents(token, lua_gettop(m_State)); lua_pop(m_State, 1);
				}
			}
		}
	};
}
