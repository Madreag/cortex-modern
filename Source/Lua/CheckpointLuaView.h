#pragma once

#include "CheckpointLuaHeap.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_set>

namespace RTE::CheckpointLua {

	class View {
	public:
		using NativeCall = std::function<int(lua_State*, View&, std::string_view)>;
		using Dump = std::function<std::string(const GCproto*)>;
		View(Snapshot heap, NativeCall native, Dump dump) : m_Heap(std::move(heap)), m_Native(std::move(native)), m_Dump(std::move(dump)) {}
		const Snapshot& Heap() const { return m_Heap; }
		std::unordered_set<const void*> scratch;
		static const void* ObjectAddress(const TValue& value) {
			if (tvisudata(&value)) return uddata(udataV(&value));
			if (tviscdata(&value)) return cdataptr(cdataV(&value));
			return tvisgcv(&value) ? gcval(&value) : nullptr;
		}

		std::optional<TValue> Value(lua_State* state, int index) const {
			if (lua_type(state, index) != LUA_TUSERDATA || !lua_getmetatable(state, index)) return std::nullopt;
			lua_pushlightuserdata(state, const_cast<View*>(this));
			lua_rawget(state, LUA_REGISTRYINDEX);
			const bool ours = lua_rawequal(state, -1, -2) != 0;
			lua_pop(state, 2);
			if (!ours) return std::nullopt;
			return static_cast<Proxy*>(lua_touserdata(state, index))->value;
		}

		void Push(lua_State* state, const TValue& value) {
			if (tvisnil(&value)) { lua_pushnil(state); return; }
			if (tvisbool(&value)) { lua_pushboolean(state, tvistrue(&value)); return; }
			if (tvisnumber(&value)) { lua_pushnumber(state, numberVnum(&value)); return; }
			if (tvislightud(&value)) { lua_pushlightuserdata(state, LightUserdata(value)); return; }
			if (tvisstr(&value)) {
				const auto* original = strV(&value);
				const auto text = m_Heap.Read(original);
				const auto bytes = m_Heap.ReadBytes(original + 1, text.len);
				lua_pushlstring(state, reinterpret_cast<const char*>(bytes.data()), bytes.size());
				return;
			}
			const void* address = gcval(&value);
			lua_rawgeti(state, LUA_REGISTRYINDEX, m_Proxies);
			lua_pushlightuserdata(state, const_cast<void*>(address));
			lua_rawget(state, -2);
			if (!lua_isnil(state, -1)) { lua_remove(state, -2); return; }
			lua_pop(state, 1);
			auto* proxy = static_cast<Proxy*>(lua_newuserdata(state, sizeof(Proxy)));
			proxy->value = value;
			lua_pushlightuserdata(state, this);
			lua_rawget(state, LUA_REGISTRYINDEX);
			lua_setmetatable(state, -2);
			lua_pushlightuserdata(state, const_cast<void*>(address));
			lua_pushvalue(state, -2);
			lua_rawset(state, -4);
			lua_remove(state, -2);
		}

		void PushTable(lua_State* state, const GCtab* table) {
			if (!table) { lua_pushnil(state); return; }
			TValue value;
			setgcVraw(&value, reinterpret_cast<GCobj*>(const_cast<GCtab*>(table)), LJ_TTAB);
			Push(state, value);
		}

		void Install(lua_State* state) {
			lua_newtable(state);
			m_Proxies = luaL_ref(state, LUA_REGISTRYINDEX);
			lua_newtable(state);
			Bind(state, "__index", Guard<Index>);
			Bind(state, "__len", Guard<Length>);
			lua_pushlightuserdata(state, this);
			lua_pushvalue(state, -2);
			lua_rawset(state, LUA_REGISTRYINDEX);
			lua_pop(state, 1);
			Override(state, "type", Guard<Type>);
			Override(state, "pairs", Guard<Pairs>);
			Override(state, "ipairs", Guard<IPairs>);
			Override(state, "next", Guard<Next>);
			Override(state, "rawget", Guard<RawGet>);
			Override(state, "getfenv", Guard<Environment>);
			lua_getglobal(state, "debug");
			OverrideField(state, "getmetatable", Guard<Metatable>);
			OverrideField(state, "getinfo", Guard<FunctionInfo>);
			OverrideField(state, "getupvalue", Guard<Upvalue>);
			OverrideField(state, "upvalueid", Guard<UpvalueId>);
			lua_pop(state, 1);
			lua_getglobal(state, "string");
			OverrideField(state, "dump", Guard<Bytecode>);
			lua_pop(state, 1);
			lua_getglobal(state, "table");
			OverrideField(state, "concat", Guard<Concat>);
			lua_pop(state, 1);
			BindGlobal(state, "_ScriptGraphValueSerial", Guard<Serial>);
			BindGlobal(state, "_ScriptGraphUpvalueSerial", Guard<CellSerial>);
			BindGlobal(state, "_ScriptGraphStateSerial", Guard<StateSerial>);
			BindGlobal(state, "_ScriptGraphScratchValue", Guard<Scratch>);
			BindGlobal(state, "_ScriptGraphObjectAddress", Guard<Address>);
			BindGlobal(state, "_ScriptGraphSameNativeFunction", Guard<SameNative>);
			BindGlobal(state, "_ScriptGraphGmatchPosition", Guard<GmatchPosition>);
		}

		void BindNative(lua_State* state, const char* name) {
			lua_pushlightuserdata(state, this);
			lua_pushstring(state, name);
			lua_pushcclosure(state, Guard<CallNative>, 2);
			lua_setglobal(state, name);
		}

		void BindGlobal(lua_State* state, const char* name, lua_CFunction function) {
			lua_pushlightuserdata(state, this);
			lua_pushcclosure(state, function, 1);
			lua_setglobal(state, name);
		}

	private:
		struct Proxy { TValue value; };
		Snapshot m_Heap;
		NativeCall m_Native;
		Dump m_Dump;
		int m_Proxies = LUA_NOREF;
		struct Table {
			std::vector<std::pair<TValue, TValue>> entries;
			std::unordered_map<std::string, size_t> positions;
		};
		std::unordered_map<const GCtab*, Table> m_Tables;
		std::string Key(const TValue& value) const {
			std::string key;
			const auto append = [&key](const auto& bits) { key.append(reinterpret_cast<const char*>(&bits), sizeof(bits)); };
			if (tvisstr(&value)) {
				const auto* source = strV(&value); const auto text = m_Heap.Read(source);
				key = "s"; key += m_Heap.ReadString(reinterpret_cast<const char*>(source + 1), text.len);
			} else if (tvisnumber(&value)) {
				double number = numberVnum(&value); if (number == 0) number = 0;
				key = "n"; append(number);
			} else if (tvisbool(&value)) key = tvistrue(&value) ? "t" : "f";
			else if (tvislightud(&value)) { key = "l"; const void* pointer = LightUserdata(value); append(pointer); }
			else if (tvisnil(&value)) key = "z";
			else { key = "p"; append(itype(&value)); const void* pointer = gcval(&value); append(pointer); }
			return key;
		}
		std::string Key(lua_State* state, int index) const {
			if (auto value = Value(state, index)) return Key(*value);
			std::string key;
			switch (lua_type(state, index)) {
				case LUA_TSTRING: { size_t size; const char* bytes = lua_tolstring(state, index, &size); key = "s"; key.append(bytes, size); break; }
				case LUA_TNUMBER: { double number = lua_tonumber(state, index); if (number == 0) number = 0; key = "n"; key.append(reinterpret_cast<const char*>(&number), sizeof(number)); break; }
				case LUA_TBOOLEAN: key = lua_toboolean(state, index) ? "t" : "f"; break;
				case LUA_TLIGHTUSERDATA: { const void* pointer = lua_touserdata(state, index); key = "l"; key.append(reinterpret_cast<const char*>(&pointer), sizeof(pointer)); break; }
				case LUA_TNIL: case LUA_TNONE: key = "z"; break;
				default: throw std::runtime_error("a worker object cannot index a frozen table");
			}
			return key;
		}
		const Table& ReadTable(const GCtab* original) {
			if (auto found = m_Tables.find(original); found != m_Tables.end()) return found->second;
			Table saved;
			const auto table = m_Heap.Read(original);
			const auto add = [&](const TValue& key, const TValue& value) {
				if (tvisnil(&value)) return;
				saved.positions.emplace(Key(key), saved.entries.size()); saved.entries.emplace_back(key, value);
			};
			const TValue* array = mref(table.array, TValue);
			for (MSize index = 0; index < table.asize; ++index) { TValue key; setintV(&key, static_cast<int32_t>(index)); add(key, m_Heap.Read(array + index)); }
			const Node* nodes = mref(table.node, Node);
			for (MSize index = 0; index <= table.hmask; ++index) { const auto node = m_Heap.Read(nodes + index); add(node.key, node.val); }
			return m_Tables.emplace(original, std::move(saved)).first->second;
		}
		void* LightUserdata(const TValue& value) const {
#if LJ_64
			const auto state = m_Heap.Read(m_Heap.State());
			const auto globals = m_Heap.Read(mref(state.glref, global_State));
			const uint64_t segment = lightudseg(value.u64);
			if (segment == (1 << LJ_LIGHTUD_BITS_SEG) - 1) return nullptr;
			if (segment > globals.gc.lightudnum) throw std::runtime_error("invalid frozen lightuserdata segment");
			const uint32_t upper = m_Heap.Read(mref(globals.gc.lightudseg, uint32_t) + segment);
			return reinterpret_cast<void*>((static_cast<uint64_t>(upper) << 32) | lightudlo(value.u64));
#else
			return gcrefp(value.gcr, void);
#endif
		}

		static View& Self(lua_State* state) { return *static_cast<View*>(lua_touserdata(state, lua_upvalueindex(1))); }
		template<int (*Function)(lua_State*)> static int Guard(lua_State* state) {
			try { return Function(state); }
			catch (const std::exception& error) { lua_pushstring(state, error.what()); }
			return lua_error(state);
		}
		void Bind(lua_State* state, const char* name, lua_CFunction function) {
			lua_pushlightuserdata(state, this); lua_pushcclosure(state, function, 1); lua_setfield(state, -2, name);
		}
		void Override(lua_State* state, const char* name, lua_CFunction function) {
			lua_pushlightuserdata(state, this); lua_getglobal(state, name); lua_pushcclosure(state, function, 2); lua_setglobal(state, name);
		}
		void OverrideField(lua_State* state, const char* name, lua_CFunction function) {
			lua_pushlightuserdata(state, this); lua_getfield(state, -2, name); lua_pushcclosure(state, function, 2); lua_setfield(state, -2, name);
		}
		static int Original(lua_State* state) {
			const int count = lua_gettop(state);
			lua_pushvalue(state, lua_upvalueindex(2)); lua_insert(state, 1); lua_call(state, count, LUA_MULTRET);
			return lua_gettop(state);
		}

		bool Equal(lua_State* state, const TValue& value, int index) {
			if (auto other = Value(state, index)) return itype(&value) == itype(&*other) && gcval(&value) == gcval(&*other);
			if (tvisnil(&value)) return lua_isnil(state, index);
			if (tvisbool(&value)) return lua_type(state, index) == LUA_TBOOLEAN && (lua_toboolean(state, index) != 0) == (tvistrue(&value) != 0);
			if (tvisnumber(&value)) return lua_type(state, index) == LUA_TNUMBER && lua_tonumber(state, index) == numberVnum(&value);
			if (tvislightud(&value)) return lua_type(state, index) == LUA_TLIGHTUSERDATA && lua_touserdata(state, index) == LightUserdata(value);
			if (tvisstr(&value) && lua_type(state, index) == LUA_TSTRING) {
				size_t length;
				const char* bytes = lua_tolstring(state, index, &length);
				const auto* source = strV(&value);
				const auto text = m_Heap.Read(source);
				return length == text.len && m_Heap.ReadString(reinterpret_cast<const char*>(source + 1), text.len) == std::string_view(bytes, length);
			}
			return false;
		}

		template<class Visitor> void Entries(const GCtab* original, Visitor visit) {
			for (const auto& [key, value]: ReadTable(original).entries) if (!visit(key, value)) return;
		}

		void Lookup(lua_State* state, const GCtab* table, int key) {
			const std::string name = Key(state, key);
			if (table == m_InjectTable && name == m_InjectKey) { Push(state, m_InjectValue); return; }
			const auto& saved = ReadTable(table);
			const auto found = saved.positions.find(name);
			if (found == saved.positions.end()) lua_pushnil(state);
			else Push(state, saved.entries[found->second].second);
		}

	public:
		// One value answered under a key of one frozen table, for a value the capture kept out of the live heap's table.
		void Inject(const GCtab* table, const char* key, const TValue& value) {
			m_InjectTable = table;
			m_InjectKey = std::string("s") + key;
			m_InjectValue = value;
		}

	private:
		const GCtab* m_InjectTable = nullptr;
		std::string m_InjectKey;
		TValue m_InjectValue{};

		static int Type(lua_State* state) {
			auto value = Self(state).Value(state, 1);
			if (!value) return Original(state);
			const char* name = tvisfunc(&*value) ? "function" : tvistab(&*value) ? "table" : tvisthread(&*value) ? "thread" : tviscdata(&*value) ? "cdata" : "userdata";
			lua_pushstring(state, name); return 1;
		}
		static int Index(lua_State* state) {
			auto& view = Self(state);
			const auto value = view.Value(state, 1);
			if (value && tvistab(&*value)) { view.Lookup(state, tabV(&*value), 2); return 1; }
			return view.m_Native(state, view, "__index");
		}
		static int RawGet(lua_State* state) {
			auto value = Self(state).Value(state, 1);
			return value ? Index(state) : Original(state);
		}
		static int Concat(lua_State* state) {
			auto& view = Self(state);
			const auto value = view.Value(state, 1);
			if (!value) return Original(state);
			if (!tvistab(&*value)) throw std::runtime_error("frozen concat requires a table");
			const std::string separator = luaL_optstring(state, 2, "");
			const int first = luaL_optint(state, 3, 1);
			int last;
			if (lua_isnoneornil(state, 4)) { Length(state); last = static_cast<int>(lua_tointeger(state, -1)); lua_pop(state, 1); }
			else last = luaL_checkint(state, 4);
			std::string result;
			for (int index = first; index <= last; ++index) {
				lua_pushinteger(state, index); view.Lookup(state, tabV(&*value), lua_gettop(state));
				size_t size; const char* bytes = lua_tolstring(state, -1, &size);
				if (!bytes) throw std::runtime_error("invalid value in frozen table.concat");
				if (index != first) result += separator;
				result.append(bytes, size); lua_pop(state, 2);
			}
			lua_pushlstring(state, result.data(), result.size()); return 1;
		}
		static int Length(lua_State* state) {
			auto& view = Self(state);
			const auto value = view.Value(state, 1);
			if (!value || !tvistab(&*value)) throw std::runtime_error("frozen length requires a table");
			const auto table = view.m_Heap.Read(tabV(&*value));
			MSize end = table.asize ? table.asize - 1 : 0, begin = 0;
			const auto at = [&](MSize index) {
				TValue result; setnilV(&result);
				view.Entries(tabV(&*value), [&](const TValue& key, const TValue& item) {
					if (tvisnumber(&key) && numberVnum(&key) == index) { result = item; return false; } return true;
				});
				return !tvisnil(&result);
			};
			if (end && !at(end)) {
				while (end - begin > 1) { const MSize middle = (begin + end) / 2; if (at(middle)) begin = middle; else end = middle; }
				lua_pushnumber(state, begin); return 1;
			}
			if (!table.hmask) { lua_pushnumber(state, end); return 1; }
			begin = end; end++;
			while (at(end)) { begin = end; if (end > 0x3fffffffu) throw std::runtime_error("frozen table length overflow"); end *= 2; }
			while (end - begin > 1) { const MSize middle = (begin + end) / 2; if (at(middle)) begin = middle; else end = middle; }
			lua_pushnumber(state, begin); return 1;
		}
		static int Next(lua_State* state) {
			auto& view = Self(state);
			const auto value = view.Value(state, 1);
			if (!value) return Original(state);
			if (!tvistab(&*value)) throw std::runtime_error("frozen next requires a table");
			const auto& table = view.ReadTable(tabV(&*value));
			size_t index = 0;
			if (!lua_isnoneornil(state, 2)) {
				const auto found = table.positions.find(view.Key(state, 2));
				if (found == table.positions.end()) throw std::runtime_error("invalid key to frozen next");
				index = found->second + 1;
			}
			if (index < table.entries.size()) { view.Push(state, table.entries[index].first); view.Push(state, table.entries[index].second); return 2; }
			lua_pushnil(state); return 1;
		}
		static int Pairs(lua_State* state) {
			if (!Self(state).Value(state, 1)) return Original(state);
			lua_pushlightuserdata(state, &Self(state)); lua_pushcclosure(state, Guard<Next>, 1);
			lua_pushvalue(state, 1); lua_pushnil(state); return 3;
		}
		static int INext(lua_State* state) {
			const int next = static_cast<int>(lua_tointeger(state, 2)) + 1;
			lua_settop(state, 1); lua_pushinteger(state, next);
			Index(state); return lua_isnil(state, -1) ? 0 : 2;
		}
		static int IPairs(lua_State* state) {
			if (!Self(state).Value(state, 1)) return Original(state);
			lua_pushlightuserdata(state, &Self(state)); lua_pushcclosure(state, Guard<INext>, 1);
			lua_pushvalue(state, 1); lua_pushinteger(state, 0); return 3;
		}
		static int Environment(lua_State* state) {
			auto& view = Self(state); auto value = view.Value(state, 1);
			if (!value) return Original(state);
			if (!tvisfunc(&*value)) throw std::runtime_error("frozen environment requires a function");
			const auto function = view.ReadFunction(funcV(&*value));
			view.PushTable(state, gcref(function.c.env) ? gco2tab(gcref(function.c.env)) : nullptr); return 1;
		}
		static int Metatable(lua_State* state) {
			auto& view = Self(state); auto value = view.Value(state, 1);
			if (!value) return Original(state);
			GCRef meta{};
			if (tvistab(&*value)) meta = view.m_Heap.Read(tabV(&*value)).metatable;
			else if (tvisudata(&*value)) meta = view.m_Heap.Read(udataV(&*value)).metatable;
			view.PushTable(state, gcref(meta) ? gco2tab(gcref(meta)) : nullptr); return 1;
		}
		GCfunc ReadFunction(const GCfunc* original) const {
			GCfunc function{};
			const bool lua = m_Heap.Read(&original->c.ffid) == FF_LUA;
			const auto bytes = m_Heap.ReadBytes(original, lua ? offsetof(GCfuncL, uvptr) : offsetof(GCfuncC, upvalue));
			std::memcpy(&function, bytes.data(), bytes.size());
			return function;
		}
		GCfunc Function(lua_State* state, int index) {
			const auto value = Value(state, index);
			if (value) { if (!tvisfunc(&*value)) throw std::runtime_error("frozen function required"); return ReadFunction(funcV(&*value)); }
			if (!lua_isfunction(state, index)) throw std::runtime_error("function required");
			return *static_cast<const GCfunc*>(lua_topointer(state, index));
		}
		static int FunctionInfo(lua_State* state) {
			auto& view = Self(state);
			if (!view.Value(state, 1)) return Original(state);
			const auto function = view.Function(state, 1);
			lua_newtable(state);
			lua_pushstring(state, isluafunc(&function) ? "Lua" : "C"); lua_setfield(state, -2, "what");
			lua_pushinteger(state, function.c.nupvalues); lua_setfield(state, -2, "nups"); return 1;
		}
		const GCupval* UpvalueAddress(lua_State* state, int functionIndex, int number) {
			const auto value = Value(state, functionIndex);
			if (!value || !tvisfunc(&*value)) return nullptr;
			const auto* original = funcV(&*value);
			const auto function = ReadFunction(original);
			if (!isluafunc(&function) || number < 1 || number > function.c.nupvalues) return nullptr;
			return gco2uv(gcref(m_Heap.Read(&original->l.uvptr[number - 1])));
		}
		static int Upvalue(lua_State* state) {
			auto& view = Self(state); const auto value = view.Value(state, 1);
			if (!value) return Original(state);
			const auto function = view.Function(state, 1);
			const int number = static_cast<int>(luaL_checkinteger(state, 2));
			if (number < 1 || number > function.c.nupvalues) return 0;
			if (!isluafunc(&function)) {
				lua_pushliteral(state, ""); view.Push(state, view.m_Heap.Read(&funcV(&*value)->c.upvalue[number - 1])); return 2;
			}
			const auto prototype = view.m_Heap.Read(funcproto(&function));
			const char* cursor = reinterpret_cast<const char*>(proto_uvinfo(&prototype));
			std::string name;
			if (cursor) {
				for (int index = 0; index < number; ++index) {
					name.clear(); char ch;
					while ((ch = view.m_Heap.Read(cursor++)) != 0) name.push_back(ch);
				}
			}
			lua_pushlstring(state, name.data(), name.size());
			const auto cell = view.m_Heap.Read(view.UpvalueAddress(state, 1, number));
			view.Push(state, cell.closed ? cell.tv : view.m_Heap.Read(mref(cell.v, TValue))); return 2;
		}
		static int UpvalueId(lua_State* state) {
			auto& view = Self(state); if (!view.Value(state, 1)) return Original(state);
			const auto* cell = view.UpvalueAddress(state, 1, static_cast<int>(luaL_checkinteger(state, 2)));
			if (!cell) throw std::runtime_error("frozen upvalue is missing");
			lua_pushlightuserdata(state, const_cast<GCupval*>(cell)); return 1;
		}
		static int Bytecode(lua_State* state) {
			auto& view = Self(state); if (!view.Value(state, 1)) return Original(state);
			const auto function = view.Function(state, 1);
			if (!isluafunc(&function)) throw std::runtime_error("unable to dump frozen native function");
			const auto text = view.m_Dump(funcproto(&function));
			lua_pushlstring(state, text.data(), text.size()); return 1;
		}
		static int Serial(lua_State* state) {
			auto& view = Self(state); auto value = view.Value(state, 1); uint64_t serial = 0;
			if (value) {
				if (tvistab(&*value)) serial = view.m_Heap.Read(tabV(&*value)).serial;
				else if (tvisfunc(&*value)) serial = view.ReadFunction(funcV(&*value)).c.serial;
				else if (tvisudata(&*value)) serial = view.m_Heap.Read(udataV(&*value)).serial;
				else if (tvisthread(&*value)) serial = view.m_Heap.Read(threadV(&*value)).serial;
			}
			lua_pushnumber(state, static_cast<lua_Number>(serial)); return 1;
		}
		static int CellSerial(lua_State* state) {
			auto& view = Self(state); const auto* cell = view.UpvalueAddress(state, 1, static_cast<int>(luaL_checkinteger(state, 2)));
			lua_pushnumber(state, cell ? static_cast<lua_Number>(view.m_Heap.Read(cell).serial) : 0); return 1;
		}
		static int StateSerial(lua_State* state) { lua_pushnumber(state, static_cast<lua_Number>(Self(state).m_Heap.StateSerial())); return 1; }
		static int Scratch(lua_State* state) {
			auto& view = Self(state); auto value = view.Value(state, 1);
			lua_pushboolean(state, value && (view.scratch.contains(gcval(&*value)) || view.scratch.contains(ObjectAddress(*value)))); return 1;
		}
		static int Address(lua_State* state) {
			auto value = Self(state).Value(state, 1);
			lua_pushnumber(state, static_cast<lua_Number>(reinterpret_cast<uintptr_t>(value ? ObjectAddress(*value) : lua_topointer(state, 1)))); return 1;
		}
		static int SameNative(lua_State* state) {
			auto& view = Self(state); const auto first = view.Function(state, 1), second = view.Function(state, 2);
			lua_pushboolean(state, !isluafunc(&first) && first.c.ffid == second.c.ffid && (!iscfunc(&first) || first.c.f == second.c.f)); return 1;
		}
		static int GmatchPosition(lua_State* state) {
			auto& view = Self(state); auto value = view.Value(state, 1);
			if (!value || !tvisfunc(&*value)) throw std::runtime_error("frozen match iterator required");
			const auto position = view.m_Heap.Read(&funcV(&*value)->c.upvalue[2]);
			lua_pushinteger(state, position.u32.lo); return 1;
		}
		static int CallNative(lua_State* state) { auto& view = Self(state); return view.m_Native(state, view, lua_tostring(state, lua_upvalueindex(2))); }
	};
}
