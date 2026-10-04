#include "LuaThreadCodec.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_frame.h"
#include "lj_gc.h"
#include "lj_state.h"
#include "lj_func.h"
#include "lj_bc.h"
#include "lj_vm.h"
}

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Return addresses are offsets into Lua functions; native continuations have stable names.
namespace {
	struct ContSymbol {
		const char* name;
		ASMFunction function;
	};

	const ContSymbol c_ContSymbols[] = {
	    {"cat", reinterpret_cast<ASMFunction>(lj_cont_cat)},
	    {"ra", reinterpret_cast<ASMFunction>(lj_cont_ra)},
	    {"nop", reinterpret_cast<ASMFunction>(lj_cont_nop)},
	    {"condt", reinterpret_cast<ASMFunction>(lj_cont_condt)},
	    {"condf", reinterpret_cast<ASMFunction>(lj_cont_condf)},
	    {"hook", reinterpret_cast<ASMFunction>(lj_cont_hook)},
	    {"stitch", reinterpret_cast<ASMFunction>(lj_cont_stitch)},
	};

	const char* ContName(ASMFunction function) {
		for (const ContSymbol& symbol: c_ContSymbols) {
			if (symbol.function == function) {
				return symbol.name;
			}
		}
		return nullptr;
	}

	ASMFunction ContFunction(const char* name) {
		for (const ContSymbol& symbol: c_ContSymbols) {
			if (name && std::strcmp(symbol.name, name) == 0) {
				return symbol.function;
			}
		}
		return nullptr;
	}

	int Failure(lua_State* L, const std::string& message) {
		lua_pushnil(L);
		lua_pushlstring(L, message.data(), message.size());
		return 2;
	}

	int SameNativeFunction(lua_State* L) {
		bool same = false;
		if (lua_isfunction(L, 1) && lua_isfunction(L, 2)) {
			const GCfunc* first = funcV(L->base);
			const GCfunc* second = funcV(L->base + 1);
			same = !isluafunc(first) && first->c.ffid == second->c.ffid && (!iscfunc(first) || first->c.f == second->c.f);
		}
		lua_pushboolean(L, same);
		return 1;
	}

	int GmatchPosition(lua_State* L) {
		luaL_checktype(L, 1, LUA_TFUNCTION);
		GCfunc* function = funcV(L->base);
		if (isluafunc(function) || function->c.nupvalues != 3 || !tvisstr(&function->c.upvalue[0]) || !tvisstr(&function->c.upvalue[1])) {
			return luaL_error(L, "not a string match iterator");
		}
		TValue* cursor = &function->c.upvalue[2];
		if (lua_gettop(L) > 1) {
			const lua_Integer position = luaL_checkinteger(L, 2);
			if (position < 0 || position > static_cast<lua_Integer>(strV(&function->c.upvalue[0])->len) + 1) {
				return luaL_error(L, "string match cursor is out of range");
			}
			cursor->u64 = 0;
			cursor->u32.lo = static_cast<uint32_t>(position);
		}
		lua_pushinteger(L, cursor->u32.lo);
		return 1;
	}

	const char* ThreadStatus(lua_State* L, lua_State* co) {
		const TValue* stack = tvref(co->stack);
		if (co == L) {
			return "running";
		} else if (co->status == LUA_YIELD) {
			return "suspended";
		} else if (co->status != LUA_OK) {
			return "dead";
		} else if (co->base > stack + 1 + LJ_FR2) {
			return "normal";
		} else if (co->top == co->base) {
			return "dead";
		}
		return "notstarted";
	}

	// (coroutine [, raw]) -> { status, base, top, slots = {[i] = value}, links = {[i] = {type, ftsz | pcslot, pos}}, conts = {[i] = name} }, or nil, message.
	// A yield from a compiled trace sits above LuaJIT's stitch continuation; unless raw is set, the description carries the plain call the trace stitched.
	int ThreadCapture(lua_State* L) {
		if (!lua_isthread(L, 1)) {
			return Failure(L, "not a coroutine");
		}
		const bool raw = lua_toboolean(L, 2) != 0;
		lua_State* co = lua_tothread(L, 1);
		const std::string status = ThreadStatus(L, co);
		if (status == "running" || status == "normal") {
			return Failure(L, "a " + status + " coroutine cannot be captured");
		}
		TValue* stack = tvref(co->stack);
		const ptrdiff_t topIndex = co->top - stack;
		const ptrdiff_t baseIndex = co->base - stack;
		struct Frame {
			ptrdiff_t index;
			int64_t ftsz;
			const BCIns* pc;
			const char* cont;
			bool lua;
			bool collapsed;
		};
		std::vector<Frame> frames;
		if (status == "suspended") {
			int guard = 0;
			for (TValue* frame = co->base - 1; frame > stack + LJ_FR2;) {
				if (++guard > 100000 || frame - stack >= topIndex) {
					return Failure(L, "the coroutine's frame chain is corrupt");
				}
				Frame info{frame - stack, static_cast<int64_t>(frame_ftsz(frame)), nullptr, nullptr, frame_islua(frame) != 0, false};
				if (info.lua) {
					info.pc = frame_pc(frame);
				} else if (frame_iscont(frame)) {
					if (frame_iscont_fficb(frame) || frame_contv(frame) == LJ_CONT_TAILCALL) {
						return Failure(L, "the coroutine is suspended inside an unsupported continuation");
					}
					info.cont = ContName(frame_contf(frame));
					if (!info.cont) {
						return Failure(L, "the coroutine is suspended inside an unknown continuation");
					}
					info.pc = frame_contpc(frame);
					info.collapsed = !raw && std::strcmp(info.cont, "stitch") == 0;
					if (info.collapsed && (info.index - 4 < 1 + LJ_FR2 || !tvisfunc(frame - 1))) {
						return Failure(L, "the coroutine's stitch frame is corrupt");
					}
				}
				frames.push_back(info);
				frame = info.lua ? frame_prevl(frame) : frame_prevd(frame);
			}
		}
		// The three slots a stitch inserted below its callee go away and everything above moves down.
		std::vector<ptrdiff_t> removed;
		for (const Frame& info: frames) {
			if (info.collapsed) {
				removed.insert(removed.end(), {info.index - 4, info.index - 3, info.index - 2});
			}
		}
		std::sort(removed.begin(), removed.end());
		const auto remap = [&removed](ptrdiff_t slot) { return slot - (std::lower_bound(removed.begin(), removed.end(), slot) - removed.begin()); };
		std::vector<ptrdiff_t> funcSlots;
		for (const Frame& info: frames) {
			funcSlots.push_back(info.index - 1);
		}
		// Each return address as a position inside a function that sits on this stack.
		const auto locate = [&](const BCIns* pc, ptrdiff_t& funcSlot, ptrdiff_t& pos) {
			for (const ptrdiff_t candidate: funcSlots) {
				const TValue* funcValue = stack + candidate;
				if (!tvisfunc(funcValue) || !isluafunc(funcV(funcValue))) {
					continue;
				}
				GCproto* proto = funcproto(funcV(funcValue));
				const BCIns* code = proto_bc(proto);
				if (pc >= code && pc <= code + proto->sizebc) {
					funcSlot = candidate;
					pos = pc - code;
					return true;
				}
			}
			return false;
		};
		struct Link {
			ptrdiff_t index;
			ptrdiff_t funcSlot;
			ptrdiff_t pos;
			int64_t ftsz;
			int type;
		};
		std::vector<Link> resolved;
		std::vector<std::pair<ptrdiff_t, const char*>> contNames;
		std::vector<char> kind(static_cast<size_t>(topIndex) + 1, 0);
		for (const Frame& info: frames) {
			kind[info.index] = 1;
			Link link{info.index, -1, 0, info.ftsz, info.collapsed ? FRAME_LUA : static_cast<int>(info.ftsz & FRAME_TYPEP)};
			if (info.lua || info.collapsed) {
				if (!locate(info.pc, link.funcSlot, link.pos)) {
					return Failure(L, "a frame's return address lies in no function on the coroutine's stack");
				}
			}
			resolved.push_back(link);
			if (info.collapsed) {
				kind[info.index - 4] = kind[info.index - 3] = kind[info.index - 2] = 5;
			} else if (info.cont) {
				kind[info.index - 3] = 2;
				kind[info.index - 2] = 3;
				if (std::strcmp(info.cont, "stitch") == 0) {
					kind[info.index - 4] = 4;
				}
				contNames.emplace_back(info.index - 3, info.cont);
				Link contPc{info.index - 2, -1, 0, 0, -1};
				if (!locate(info.pc, contPc.funcSlot, contPc.pos)) {
					return Failure(L, "a frame's return address lies in no function on the coroutine's stack");
				}
				resolved.push_back(contPc);
			}
		}
		lua_newtable(L);
		const int desc = lua_gettop(L);
		lua_pushstring(L, status.c_str());
		lua_setfield(L, desc, "status");
		lua_pushinteger(L, static_cast<lua_Integer>(remap(baseIndex)));
		lua_setfield(L, desc, "base");
		lua_pushinteger(L, static_cast<lua_Integer>(remap(topIndex)));
		lua_setfield(L, desc, "top");
		lua_pushinteger(L, 1 + LJ_FR2);
		lua_setfield(L, desc, "first");
		lua_newtable(L);
		const int slots = lua_gettop(L);
		lua_newtable(L);
		const int links = lua_gettop(L);
		lua_newtable(L);
		const int conts = lua_gettop(L);
		for (const Link& link: resolved) {
			lua_newtable(L);
			if (link.type >= 0) {
				lua_pushinteger(L, link.type);
				lua_setfield(L, -2, "type");
			}
			if (link.funcSlot >= 0) {
				lua_pushinteger(L, static_cast<lua_Integer>(remap(link.funcSlot)));
				lua_setfield(L, -2, "pcslot");
				lua_pushinteger(L, static_cast<lua_Integer>(link.pos));
				lua_setfield(L, -2, "pos");
			} else {
				lua_pushinteger(L, static_cast<lua_Integer>(link.ftsz));
				lua_setfield(L, -2, "ftsz");
			}
			lua_rawseti(L, links, static_cast<int>(remap(link.index)));
		}
		for (const auto& [index, name]: contNames) {
			lua_pushstring(L, name);
			lua_rawseti(L, conts, static_cast<int>(remap(index)));
		}
		for (ptrdiff_t i = 1 + LJ_FR2; i < topIndex; ++i) {
			if (kind[i] == 4) {
				// A raw stitch keeps a zero trace so the interpreter continues it.
				lua_pushnumber(L, 0);
				lua_rawseti(L, slots, static_cast<int>(remap(i)));
				continue;
			}
			if (kind[i] != 0 || tvisnil(stack + i)) {
				continue;
			}
			copyTV(L, L->top, stack + i);
			incr_top(L);
			lua_rawseti(L, slots, static_cast<int>(remap(i)));
		}
		lua_setfield(L, desc, "conts");
		lua_setfield(L, desc, "links");
		lua_setfield(L, desc, "slots");
		return 1;
	}

	void EmptyThread(lua_State* co) {
		TValue* stack = tvref(co->stack);
		co->base = co->top = stack + 1 + LJ_FR2;
		co->status = LUA_OK;
		co->cframe = nullptr;
	}

	// (description) -> coroutine, or nil, message.
	int ThreadRestore(lua_State* L) {
		luaL_checktype(L, 1, LUA_TTABLE);
		lua_getfield(L, 1, "status");
		const std::string status = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		lua_pop(L, 1);
		lua_State* co;
		if (lua_isthread(L, 2)) {
			co = lua_tothread(L, 2);
			const std::string currentStatus = ThreadStatus(L, co);
			if (currentStatus == "running" || currentStatus == "normal") {
				return Failure(L, "an active coroutine cannot be replaced");
			}
			lua_pushvalue(L, 2);
			lua_settop(co, 0);
		} else {
			co = lua_newthread(L);
		}
		const int thread = lua_gettop(L);
		if (status == "dead") {
			return 1;
		}
		lua_getfield(L, 1, "slots");
		const int slots = lua_gettop(L);
		if (status == "notstarted") {
			lua_rawgeti(L, slots, 1 + LJ_FR2);
			if (!lua_isfunction(L, -1)) {
				lua_settop(L, thread - 1);
				return Failure(L, "the coroutine's function is missing");
			}
			lua_xmove(L, co, 1);
			lua_settop(L, thread);
			return 1;
		}
		if (status != "suspended") {
			lua_settop(L, thread - 1);
			return Failure(L, "unknown coroutine status '" + status + "'");
		}
		lua_getfield(L, 1, "base");
		const ptrdiff_t base = static_cast<ptrdiff_t>(lua_tointeger(L, -1));
		lua_pop(L, 1);
		lua_getfield(L, 1, "top");
		const ptrdiff_t top = static_cast<ptrdiff_t>(lua_tointeger(L, -1));
		lua_pop(L, 1);
		if (base < 1 + LJ_FR2 + 2 || top < base || top > 1000000) {
			lua_settop(L, thread - 1);
			return Failure(L, "the coroutine's stack bounds are invalid");
		}
		// A yielded top sits below base+framesize; size every frame from the function in its own slot.
		ptrdiff_t needed = top;
		lua_getfield(L, 1, "links");
		if (lua_istable(L, -1)) {
			lua_pushnil(L);
			while (lua_next(L, -2) != 0) {
				const ptrdiff_t index = static_cast<ptrdiff_t>(lua_tointeger(L, -2));
				if (index < top && lua_istable(L, -1)) {
					lua_rawgeti(L, slots, static_cast<int>(index - 1));
					if (tvisfunc(L->top - 1) && isluafunc(funcV(L->top - 1))) {
						needed = std::max(needed, index + 1 + static_cast<ptrdiff_t>(funcproto(funcV(L->top - 1))->framesize));
					}
					lua_pop(L, 1);
				}
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);
		if (needed > 1000000) {
			lua_settop(L, thread - 1);
			return Failure(L, "the coroutine's stack bounds are invalid");
		}
		if (!lua_checkstack(co, static_cast<int>(top) + 16)) {
			lua_settop(L, thread - 1);
			return Failure(L, "the coroutine's stack exceeds the VM limit");
		}
		// Frames above the top grow past the C API reservation limit, as the VM grows a called frame.
		const ptrdiff_t missing = needed - (tvref(co->maxstack) - tvref(co->stack));
		if (missing > 0 && lj_state_cpgrowstack(co, static_cast<MSize>(missing)) != LUA_OK) {
			co->top--;
			lua_settop(L, thread - 1);
			return Failure(L, "the coroutine's stack exceeds the VM limit");
		}
		TValue* stack = tvref(co->stack);
		for (ptrdiff_t i = 1 + LJ_FR2; i < top; ++i) {
			setnilV(stack + i);
		}
		lua_pushnil(L);
		while (lua_next(L, slots) != 0) {
			const ptrdiff_t index = static_cast<ptrdiff_t>(lua_tointeger(L, -2));
			if (index >= 1 + LJ_FR2 && index < top) {
				copyTV(co, stack + index, L->top - 1);
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
		lua_getfield(L, 1, "links");
		const int links = lua_gettop(L);
		std::vector<ptrdiff_t> linkSlots;
		lua_pushnil(L);
		while (lua_next(L, links) != 0) {
			const ptrdiff_t index = static_cast<ptrdiff_t>(lua_tointeger(L, -2));
			if (!lua_istable(L, -1) || index < 1 + LJ_FR2 + 1 || index >= top) {
				EmptyThread(co);
				lua_settop(L, thread - 1);
				return Failure(L, "a frame link is out of the coroutine's stack");
			}
			lua_getfield(L, -1, "pcslot");
			if (!lua_isnil(L, -1)) {
				const ptrdiff_t funcSlot = static_cast<ptrdiff_t>(lua_tointeger(L, -1));
				lua_pop(L, 1);
				lua_getfield(L, -1, "pos");
				const ptrdiff_t pos = static_cast<ptrdiff_t>(lua_tointeger(L, -1));
				lua_pop(L, 1);
				const TValue* funcValue = funcSlot >= 0 && funcSlot < top ? stack + funcSlot : nullptr;
				if (!funcValue || !tvisfunc(funcValue) || !isluafunc(funcV(funcValue))) {
					EmptyThread(co);
					lua_settop(L, thread - 1);
					return Failure(L, "a frame's function is not a Lua function");
				}
				GCproto* proto = funcproto(funcV(funcValue));
				if (pos < 1 || pos > static_cast<ptrdiff_t>(proto->sizebc)) {
					EmptyThread(co);
					lua_settop(L, thread - 1);
					return Failure(L, "a frame's return address lies outside its function");
				}
				setframe_pc(stack + index, proto_bc(proto) + pos);
			} else {
				lua_pop(L, 1);
				lua_getfield(L, -1, "ftsz");
				const int64_t ftsz = static_cast<int64_t>(lua_tointeger(L, -1));
				lua_pop(L, 1);
				const int64_t bytes = ftsz & ~FRAME_TYPEP;
				if ((ftsz & FRAME_TYPE) == FRAME_LUA || bytes <= 0 || bytes % sizeof(TValue) != 0 || bytes / sizeof(TValue) > index - LJ_FR2) {
					EmptyThread(co);
					lua_settop(L, thread - 1);
					return Failure(L, "a frame's size is outside the coroutine's stack");
				}
				setframe_ftsz(stack + index, ftsz);
			}
			linkSlots.push_back(index);
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
		lua_getfield(L, 1, "conts");
		const int conts = lua_gettop(L);
		lua_pushnil(L);
		while (lua_next(L, conts) != 0) {
			const ptrdiff_t index = static_cast<ptrdiff_t>(lua_tointeger(L, -2));
			const ASMFunction function = ContFunction(lua_tostring(L, -1));
			if (!function || index < 1 + LJ_FR2 || index >= top) {
				EmptyThread(co);
				lua_settop(L, thread - 1);
				return Failure(L, "a continuation slot is invalid");
			}
			setcont(stack + index, function);
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
		co->base = stack + base;
		co->top = stack + top;
		co->status = LUA_YIELD;
		co->cframe = nullptr;
		// The chain must walk down to the bottom through function slots, or the GC and the resume would not.
		int guard = 0;
		for (TValue* frame = co->base - 1; frame > stack + LJ_FR2;) {
			const ptrdiff_t index = frame - stack;
			bool listed = false;
			for (const ptrdiff_t slot: linkSlots) {
				if (slot == index) {
					listed = true;
					break;
				}
			}
			if (!listed || !tvisfunc(frame - 1) || ++guard > 100000) {
				EmptyThread(co);
				lua_settop(L, thread - 1);
				return Failure(L, "the rebuilt frame chain does not reach the bottom of the stack");
			}
			const ptrdiff_t step = frame_islua(frame) ? 1 + LJ_FR2 + bc_a(frame_pc(frame)[-1]) : frame_sized(frame) / sizeof(TValue);
			if (step <= 0 || step > index - LJ_FR2) {
				EmptyThread(co);
				lua_settop(L, thread - 1);
				return Failure(L, "the rebuilt frame chain runs below the stack");
			}
			frame = stack + index - step;
		}
		guard = 0;
		for (TValue* frame = co->base - 1; frame > stack + LJ_FR2;) {
			if (tvisfunc(frame - 1) && isluafunc(funcV(frame - 1))) {
				if ((frame - stack) + 1 + static_cast<ptrdiff_t>(funcproto(funcV(frame - 1))->framesize) > tvref(co->maxstack) - stack) {
					EmptyThread(co);
					lua_settop(L, thread - 1);
					return Failure(L, "a frame needs more stack than the coroutine has");
				}
			}
			const ptrdiff_t index = frame - stack;
			if (++guard > 100000) {
				break;
			}
			const ptrdiff_t step = frame_islua(frame) ? 1 + LJ_FR2 + bc_a(frame_pc(frame)[-1]) : frame_sized(frame) / sizeof(TValue);
			if (step <= 0 || step > index - LJ_FR2) {
				break;
			}
			frame = stack + index - step;
		}
		lua_settop(L, thread);
		return 1;
	}

	// (coroutine) -> fits, needed, maxstack.
	int ThreadStackFits(lua_State* L) {
		if (!lua_isthread(L, 1)) {
			return Failure(L, "not a coroutine");
		}
		lua_State* co = lua_tothread(L, 1);
		TValue* stack = tvref(co->stack);
		const ptrdiff_t maxstack = tvref(co->maxstack) - stack;
		const std::string status = ThreadStatus(L, co);
		if (status != "suspended") {
			lua_pushboolean(L, 1);
			lua_pushinteger(L, 0);
			lua_pushinteger(L, static_cast<lua_Integer>(maxstack));
			return 3;
		}
		ptrdiff_t needed = 0;
		const ptrdiff_t topIndex = co->top - stack;
		int guard = 0;
		// Every Lua frame on the chain must fit its own function: base + framesize <= maxstack.
		for (TValue* frame = co->base - 1; frame > stack + LJ_FR2;) {
			if (++guard > 100000 || frame - stack >= topIndex) {
				return Failure(L, "the coroutine's frame chain is corrupt");
			}
			if (tvisfunc(frame - 1) && isluafunc(funcV(frame - 1))) {
				needed = std::max(needed, (frame - stack) + 1 + static_cast<ptrdiff_t>(funcproto(funcV(frame - 1))->framesize));
			}
			frame = frame_islua(frame) ? frame_prevl(frame) : frame_prevd(frame);
		}
		lua_pushboolean(L, needed <= maxstack);
		lua_pushinteger(L, static_cast<lua_Integer>(needed));
		lua_pushinteger(L, static_cast<lua_Integer>(maxstack));
		return 3;
	}

	// () -> { [upvalue id] = { thread = coroutine, slot = index } } for every open upvalue of every coroutine.
	int OpenUpvalues(lua_State* L) {
		global_State* g = G(L);
		const auto threshold = g->gc.threshold;
		// The tables allocated during this walk must not collect the list being walked.
		g->gc.threshold = std::numeric_limits<decltype(g->gc.threshold)>::max();
		lua_newtable(L);
		const int result = lua_gettop(L);
		for (GCobj* object = gcref(g->gc.root); object != nullptr; object = gcnext(object)) {
			if (object->gch.gct != ~LJ_TTHREAD) {
				continue;
			}
			lua_State* thread = &object->th;
			const TValue* stack = tvref(thread->stack);
			for (GCobj* entry = gcref(thread->openupval); entry != nullptr; entry = gcnext(entry)) {
				GCupval* upvalue = &entry->uv;
				lua_pushlightuserdata(L, upvalue);
				lua_newtable(L);
				setthreadV(L, L->top, thread);
				incr_top(L);
				lua_setfield(L, -2, "thread");
				lua_pushinteger(L, static_cast<lua_Integer>(uvval(upvalue) - stack));
				lua_setfield(L, -2, "slot");
				lua_settable(L, result);
			}
		}
		g->gc.threshold = threshold;
		return 1;
	}

	GCupval* FindOpenUpvalue(lua_State* L, lua_State* co, TValue* slot) {
		global_State* g = G(L);
		GCRef* link = &co->openupval;
		while (gcref(*link) != nullptr) {
			GCupval* candidate = &gcref(*link)->uv;
			if (uvval(candidate) < slot) {
				break;
			}
			if (uvval(candidate) == slot) {
				if (isdead(g, obj2gco(candidate))) {
					flipwhite(obj2gco(candidate));
				}
				return candidate;
			}
			link = &candidate->nextgc;
		}
		GCupval* upvalue = static_cast<GCupval*>(lj_mem_realloc(L, nullptr, 0, sizeof(GCupval)));
		newwhite(g, upvalue);
		upvalue->gct = ~LJ_TUPVAL;
		upvalue->closed = 0;
		upvalue->immutable = 0;
		upvalue->dhash = 0;
		setmref(upvalue->v, slot);
		setgcrefr(upvalue->nextgc, *link);
		setgcref(*link, obj2gco(upvalue));
		setgcref(upvalue->prev, obj2gco(&g->uvhead));
		setgcrefr(upvalue->next, g->uvhead.next);
		setgcref(uvnext(upvalue)->prev, obj2gco(upvalue));
		setgcref(g->uvhead.next, obj2gco(upvalue));
		return upvalue;
	}

	// (function, index, coroutine, slot): the closure's upvalue becomes the coroutine's open upvalue on that slot.
	int JoinOpenUpvalue(lua_State* L) {
		if (!lua_isfunction(L, 1) || !lua_isthread(L, 3)) {
			return Failure(L, "a Lua function and a coroutine are needed");
		}
		GCfunc* function = funcV(L->base);
		const int index = static_cast<int>(luaL_checkinteger(L, 2));
		lua_State* co = lua_tothread(L, 3);
		const ptrdiff_t slot = static_cast<ptrdiff_t>(luaL_checkinteger(L, 4));
		if (!isluafunc(function) || index < 1 || index > function->l.nupvalues) {
			return Failure(L, "the upvalue index is out of range");
		}
		TValue* stack = tvref(co->stack);
		if (slot < 1 + LJ_FR2 || slot >= co->top - stack) {
			return Failure(L, "the upvalue slot is outside the coroutine's stack");
		}
		GCupval* upvalue = FindOpenUpvalue(L, co, stack + slot);
		setgcref(function->l.uvptr[index - 1], obj2gco(upvalue));
		lj_gc_objbarrier(L, function, upvalue);
		lua_pushboolean(L, 1);
		return 1;
	}
} // namespace

namespace RTE::LuaThreadCodec {
	// What each slot of a coroutine's stack holds: 0 a Lua value, otherwise a frame link or a continuation's own slot.
	std::vector<char> ThreadSlotKinds(lua_State* thread) {
		TValue* stack = tvref(thread->stack);
		const ptrdiff_t topIndex = thread->top - stack;
		std::vector<char> kind(static_cast<size_t>(std::max<ptrdiff_t>(topIndex, 0)) + 1, 0);
		int guard = 0;
		for (TValue* frame = thread->base - 1; frame > stack + LJ_FR2;) {
			const ptrdiff_t index = frame - stack;
			if (++guard > 100000 || index <= 0 || index >= topIndex) break;
			kind[static_cast<size_t>(index)] = 1;
			const bool lua = frame_islua(frame) != 0;
			if (!lua && frame_iscont(frame) && !frame_iscont_fficb(frame) && frame_contv(frame) != LJ_CONT_TAILCALL) {
				if (index >= 3) {
					kind[static_cast<size_t>(index - 3)] = 2;
					kind[static_cast<size_t>(index - 2)] = 3;
				}
				if (index >= 4 && frame_contf(frame) == reinterpret_cast<ASMFunction>(lj_cont_stitch)) {
					kind[static_cast<size_t>(index - 4)] = 4;
				}
			}
			frame = lua ? frame_prevl(frame) : frame_prevd(frame);
		}
		return kind;
	}

	bool VisitThreadStack(lua_State* thread, lua_State* dest, bool (*visitor)(lua_State* dest, void* context), void* context) {
		if (!thread || !dest || !visitor) return false;
		TValue* stack = tvref(thread->stack);
		const ptrdiff_t topIndex = thread->top - stack;
		if (topIndex <= 1 + LJ_FR2) return false;
		const std::vector<char> kind = ThreadSlotKinds(thread);
		for (ptrdiff_t i = 1 + LJ_FR2; i < topIndex; ++i) {
			if (!lua_checkstack(dest, 1)) return false;
			stack = tvref(thread->stack);
			if (kind[static_cast<size_t>(i)] != 0 || tvisnil(stack + i)) continue;
			copyTV(dest, dest->top, stack + i);
			incr_top(dest);
			const bool found = visitor(dest, context);
			dest->top--;
			if (found) return true;
		}
		return false;
	}

	void VisitUserdata(lua_State* state, void (*visitor)(void*, size_t, const void*, void*), void* context) {
		auto visit = [&](GCobj* object) {
			if (object->gch.gct == ~LJ_TUDATA) {
				GCudata* data = gco2ud(object);
				visitor(uddata(data), data->len, tabref(data->metatable), context);
			}
		};
		global_State* global = G(state);
		for (GCobj* object = gcref(global->gc.root); object; object = gcnext(object)) {
			if (!(object->gch.marked & LJ_GC_FINALIZED)) visit(object);
		}
		// Queued finalizers have the finalized bit set, but their native payload is still alive.
		if (GCobj* last = gcref(global->gc.mmudata)) {
			GCobj* object = last;
			do {
				object = gcnext(object);
				visit(object);
			} while (object != last);
		}
	}

	void Register(lua_State* state) {
		lua_pushcfunction(state, GmatchPosition);
		lua_setglobal(state, "_ScriptGraphGmatchPosition");
		lua_pushcfunction(state, SameNativeFunction);
		lua_setglobal(state, "_ScriptGraphSameNativeFunction");
		lua_pushcfunction(state, ThreadCapture);
		lua_setglobal(state, "_ScriptGraphThreadCapture");
		lua_pushcfunction(state, ThreadRestore);
		lua_setglobal(state, "_ScriptGraphThreadRestore");
		lua_pushcfunction(state, ThreadStackFits);
		lua_setglobal(state, "_ScriptGraphThreadStackFits");
		lua_pushcfunction(state, OpenUpvalues);
		lua_setglobal(state, "_ScriptGraphOpenUpvalues");
		lua_pushcfunction(state, JoinOpenUpvalue);
		lua_setglobal(state, "_ScriptGraphJoinOpenUpvalue");
	}

	namespace {
		int EmptyFunction(lua_State*) { return 0; }

		// The fast function id of coroutine.wrap's functions, read once from one the library makes.
		int WrapFunctionId(lua_State* L) {
			static std::atomic<int> id{-1};
			if (id.load() < 0) {
				const int top = lua_gettop(L);
				lua_getglobal(L, "coroutine");
				if (lua_istable(L, -1)) {
					lua_getfield(L, -1, "wrap");
					lua_pushcfunction(L, EmptyFunction);
					if (lua_pcall(L, 1, 1, 0) == 0 && lua_isfunction(L, -1) && !isluafunc(funcV(L->top - 1))) {
						id.store(funcV(L->top - 1)->c.ffid);
					}
				}
				lua_settop(L, top);
			}
			return id.load();
		}

		GCupval* NewClosedUpvalue(lua_State* L, const GCupval* like) {
			GCupval* upvalue = static_cast<GCupval*>(lj_mem_newgco(L, sizeof(GCupval)));
			upvalue->gct = ~LJ_TUPVAL;
			upvalue->serial = ++G(L)->objserial;
			upvalue->closed = 1;
			upvalue->immutable = like->immutable;
			upvalue->dhash = like->dhash;
			setnilV(&upvalue->tv);
			setmref(upvalue->v, &upvalue->tv);
			return upvalue;
		}
	} // namespace

	namespace {
		const char* const c_OwnPreviewCopiesKey = "cccp.preview_own_copies";
		// Past this many functions deep the decision copies rather than recurse further: a copy reads what the original does.
		constexpr int c_MaxDecisionDepth = 400;
	}

	bool IsOwnPreviewCopy(lua_State* L, int index) {
		index = index < 0 ? lua_gettop(L) + index + 1 : index;
		if (!lua_isfunction(L, index)) return false;
		lua_getfield(L, LUA_REGISTRYINDEX, c_OwnPreviewCopiesKey);
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			return false;
		}
		lua_pushvalue(L, index);
		lua_rawget(L, -2);
		const bool own = lua_toboolean(L, -1) != 0;
		lua_pop(L, 2);
		return own;
	}

	bool RemapPreviewCopyValues(lua_State* L, int index, bool (*remap)(lua_State*, void*), void* context) {
		index = index < 0 ? lua_gettop(L) + index + 1 : index;
		if (lua_isthread(L, index)) {
			lua_State* co = lua_tothread(L, index);
			if (co == L) return true;
			const ptrdiff_t topIndex = co->top - tvref(co->stack);
			const std::vector<char> kind = ThreadSlotKinds(co);
			for (ptrdiff_t slot = 1 + LJ_FR2; slot < topIndex; ++slot) {
				if (kind[static_cast<size_t>(slot)] != 0 || tvisnil(tvref(co->stack) + slot)) continue;
				if (!lua_checkstack(L, 2)) return false;
				copyTV(L, L->top, tvref(co->stack) + slot);
				incr_top(L);
				// A coroutine's stack is traversed again at every collection's end, so writing it needs no barrier.
				const bool remapped = remap(L, context);
				if (remapped) copyTV(co, tvref(co->stack) + slot, L->top - 1);
				L->top--;
				if (!remapped) return false;
			}
			return true;
		}
		if (!IsOwnPreviewCopy(L, index)) return true;
		GCfunc* function = funcV(L->base + index - 1);
		if (!isluafunc(function)) {
			// A native closure's copy holds every one of its values as its own.
			for (uint32_t slot = 0; slot < function->c.nupvalues; ++slot) {
				if (!lua_checkstack(L, 2)) return false;
				copyTV(L, L->top, &function->c.upvalue[slot]);
				incr_top(L);
				const bool remapped = remap(L, context);
				if (remapped) {
					copyTV(L, &function->c.upvalue[slot], L->top - 1);
					if (tvisgcv(L->top - 1)) lj_gc_objbarrier(L, function, gcV(L->top - 1));
				}
				L->top--;
				if (!remapped) return false;
			}
			return true;
		}
		// The variables the copy holds of its own: every one, or the slots its entry lists; the rest are the original's.
		std::vector<uint32_t> slots;
		lua_getfield(L, LUA_REGISTRYINDEX, c_OwnPreviewCopiesKey);
		lua_pushvalue(L, index);
		lua_rawget(L, -2);
		if (lua_istable(L, -1)) {
			const size_t count = lua_objlen(L, -1);
			for (size_t at = 1; at <= count; ++at) {
				lua_rawgeti(L, -1, static_cast<int>(at));
				slots.push_back(static_cast<uint32_t>(lua_tointeger(L, -1) - 1));
				lua_pop(L, 1);
			}
		} else {
			for (uint32_t slot = 0; slot < function->l.nupvalues; ++slot) {
				slots.push_back(slot);
			}
		}
		lua_pop(L, 2);
		for (const uint32_t slot: slots) {
			if (slot >= function->l.nupvalues) continue;
			GCupval* upvalue = &gcref(function->l.uvptr[slot])->uv;
			// One joined to a coroutine's copy lives on that copy's stack, which the coroutine's own remap reaches.
			if (!upvalue->closed) continue;
			if (!lua_checkstack(L, 2)) return false;
			copyTV(L, L->top, uvval(upvalue));
			incr_top(L);
			const bool remapped = remap(L, context);
			if (remapped) {
				copyTV(L, uvval(upvalue), L->top - 1);
				if (tvisgcv(L->top - 1)) lj_gc_objbarrier(L, upvalue, gcV(L->top - 1));
			}
			L->top--;
			if (!remapped) return false;
		}
		return true;
	}

	struct PreviewCopier::Impl {
		struct Join {
			GCfunc* function;
			uint32_t index;
			GCupval* original;
			lua_State* thread;
			ptrdiff_t slot;
		};

		// A function's place in the walk that decides, a cycle at a time, which functions the copy copies.
		struct Visit {
			int index = -1;
			int low = -1;
			bool onStack = false;
			bool reason = false;
			bool decided = false;
			bool copied = false;
		};

		lua_State* L;
		int seen;
		MapValue mapValue;
		Replaced replaced;
		void* context;
		int (*standIn)(lua_State*);
		std::unordered_map<GCupval*, GCupval*> cells; //!< A closed variable the copied closures hold, and the copy's own.
		std::unordered_map<GCupval*, GCupval*> held; //!< A variable open on a coroutine, and the one the copies hold until it is joined.
		std::unordered_map<lua_State*, lua_State*> threads; //!< Each coroutine restored, and its copy.
		std::unordered_map<const GCfunc*, Visit> visits; //!< Every function the decision walk reached.
		std::vector<const GCfunc*> pending; //!< The decision walk's functions whose cycle is still open.
		int visitCount = 0;
		std::vector<Join> joins; //!< Variables open on a coroutine, moved to its copy's stack once it is restored.
		std::unordered_map<GCupval*, lua_State*> owners;
		bool ownersRead = false;
		std::string refusal;

		TValue* Slot(int index) const { return L->base + ((index < 0 ? lua_gettop(L) + index + 1 : index) - 1); }

		// The coroutine an open variable lives on; read from every coroutine's open list the first time one is needed.
		lua_State* OwnerOf(GCupval* upvalue) {
			if (!ownersRead) {
				ownersRead = true;
				for (GCobj* object = gcref(G(L)->gc.root); object != nullptr; object = gcnext(object)) {
					if (object->gch.gct != ~LJ_TTHREAD) {
						continue;
					}
					for (GCobj* entry = gcref(object->th.openupval); entry != nullptr; entry = gcnext(entry)) {
						owners.emplace(&entry->uv, &object->th);
					}
				}
			}
			const auto found = owners.find(upvalue);
			return found == owners.end() ? nullptr : found->second;
		}

		// A coroutine at rest whose copy can own a variable, not the state running the copy.
		bool Copyable(const lua_State* thread) const { return thread && thread != L && thread != mainthread(G(L)); }

		bool IsWrap(const GCfunc* function) {
			const int wrap = WrapFunctionId(L);
			return !isluafunc(function) && wrap >= 0 && function->c.ffid == wrap && function->c.nupvalues == 1 && tvisthread(&function->c.upvalue[0]);
		}

		// Whether the copy gives a value that is not a function another value: tables and coroutines are copied, and the
		// map says which userdata it or the remap after it replaces.
		bool Replaces(const TValue* value) {
			if (tvistab(value) || tvisthread(value)) {
				return true;
			}
			if (!tvisudata(value)) {
				return false;
			}
			if (!lua_checkstack(L, 1)) {
				return true;
			}
			copyTV(L, L->top, value);
			incr_top(L);
			const bool result = replaced(L, -1, context);
			L->top--;
			return result;
		}

		// A closure is copied when a variable of it is written, holds a value the copy replaces, or is open on a coroutine
		// the copy restores; a native closure when a value of it is replaced. A cycle of unwritten variables is decided whole.
		void Connect(const GCfunc* function, int depth) {
			Visit& visit = visits[function];
			visit.index = visit.low = visitCount++;
			visit.onStack = true;
			pending.push_back(function);
			const auto follow = [&](const TValue* value) {
				if (!tvisfunc(value)) {
					visit.reason = visit.reason || Replaces(value);
					return;
				}
				const GCfunc* next = funcV(value);
				auto found = visits.find(next);
				if (found == visits.end()) {
					if (depth >= c_MaxDecisionDepth) {
						visit.reason = true;
						return;
					}
					Connect(next, depth + 1);
					found = visits.find(next);
				} else if (found->second.onStack) {
					visit.low = std::min(visit.low, found->second.index);
					return;
				}
				if (found->second.decided) {
					visit.reason = visit.reason || found->second.copied;
				} else {
					visit.low = std::min(visit.low, found->second.low);
				}
			};
			if (isluafunc(function)) {
				for (uint32_t slot = 0; slot < function->l.nupvalues; ++slot) {
					GCupval* upvalue = &gcref(function->l.uvptr[slot])->uv;
					if (!upvalue->closed) {
						if (Copyable(OwnerOf(upvalue))) {
							visit.reason = true;
						} else if (refusal.empty()) {
							refusal = "a closure's variable is open on the state running the copy";
						}
					} else if (!upvalue->immutable) {
						visit.reason = true;
					} else {
						follow(&upvalue->tv);
					}
				}
			} else if (IsWrap(function)) {
				visit.reason = true;
			} else {
				for (uint32_t slot = 0; slot < function->c.nupvalues; ++slot) {
					follow(&function->c.upvalue[slot]);
				}
			}
			if (visit.low != visit.index) {
				return;
			}
			size_t first = pending.size();
			bool copied = false;
			do {
				--first;
				copied = copied || visits[pending[first]].reason;
			} while (pending[first] != function);
			for (size_t at = first; at < pending.size(); ++at) {
				Visit& member = visits[pending[at]];
				member.onStack = false;
				member.decided = true;
				member.copied = copied;
			}
			pending.resize(first);
		}

		bool Decide(const GCfunc* function) {
			auto found = visits.find(function);
			if (found == visits.end()) {
				Connect(function, 0);
				found = visits.find(function);
			}
			return found->second.copied;
		}

		// Whether a copied closure takes its own copy of this closed variable: one written, or holding a value the copy replaces.
		bool OwnsVariable(const GCupval* upvalue) {
			if (!upvalue->immutable) {
				return true;
			}
			return tvisfunc(&upvalue->tv) ? Decide(funcV(&upvalue->tv)) : Replaces(&upvalue->tv);
		}

		void SetUpvalue(GCfunc* function, uint32_t index, GCupval* upvalue) {
			setgcref(function->l.uvptr[index], obj2gco(upvalue));
			lj_gc_objbarrier(L, function, upvalue);
		}

		// Replaces the value on top of the stack with what the copy holds for it and stores that into the variable.
		void StoreMapped(GCupval* variable) {
			mapValue(L, context);
			copyTV(L, uvval(variable), L->top - 1);
			if (tvisgcv(L->top - 1)) {
				lj_gc_objbarrier(L, variable, gcV(L->top - 1));
			}
			lua_pop(L, 1);
		}

		// The copy's own variable for the original's, made once for every closure that shares it; the closure takes it
		// before its value is mapped, so the mapping's allocations cannot collect it.
		void TakeOwnVariable(GCfunc* copy, uint32_t slot, GCupval* upvalue) {
			if (const auto found = cells.find(upvalue); found != cells.end()) {
				SetUpvalue(copy, slot, found->second);
				return;
			}
			GCupval* own = NewClosedUpvalue(L, upvalue);
			SetUpvalue(copy, slot, own);
			cells.emplace(upvalue, own);
			copyTV(L, L->top, uvval(upvalue));
			incr_top(L);
			StoreMapped(own);
		}

		// A variable open on a coroutine: every copy that shares it holds one copy, the original's value mapped, until the
		// coroutine's copy is restored and the variable moves onto its stack.
		void HoldOpenVariable(GCfunc* copy, uint32_t slot, GCupval* upvalue, lua_State* owner) {
			if (const auto found = held.find(upvalue); found != held.end()) {
				SetUpvalue(copy, slot, found->second);
			} else {
				GCupval* variable = NewClosedUpvalue(L, upvalue);
				SetUpvalue(copy, slot, variable);
				held.emplace(upvalue, variable);
				copyTV(L, L->top, uvval(upvalue));
				incr_top(L);
				StoreMapped(variable);
			}
			joins.push_back({copy, slot, upvalue, owner, uvval(upvalue) - tvref(owner->stack)});
		}

		// Records which values of a copy are its own, so the remap after the copy reaches those and never the original's.
		void MarkOwn(GCfunc* copy, const std::vector<uint32_t>& own, uint32_t count) {
			if (count > 0 && own.empty()) {
				return;
			}
			lua_getfield(L, LUA_REGISTRYINDEX, c_OwnPreviewCopiesKey);
			if (!lua_istable(L, -1)) {
				lua_pop(L, 1);
				lua_newtable(L);
				lua_newtable(L);
				lua_pushliteral(L, "k");
				lua_setfield(L, -2, "__mode");
				lua_setmetatable(L, -2);
				lua_pushvalue(L, -1);
				lua_setfield(L, LUA_REGISTRYINDEX, c_OwnPreviewCopiesKey);
			}
			setfuncV(L, L->top, copy);
			incr_top(L);
			if (own.size() == count) {
				lua_pushboolean(L, 1);
			} else {
				lua_createtable(L, static_cast<int>(own.size()), 0);
				for (size_t at = 0; at < own.size(); ++at) {
					lua_pushinteger(L, static_cast<lua_Integer>(own[at]) + 1);
					lua_rawseti(L, -2, static_cast<int>(at) + 1);
				}
			}
			lua_rawset(L, -3);
			lua_pop(L, 1);
		}

		// A native closure's copy holds each of its values as the copy holds it; coroutine.wrap's resumes its coroutine's copy.
		void PushNativeCopy(int index, const GCfunc* original) {
			const uint32_t count = original->c.nupvalues;
			GCfunc* copy = lj_func_newC(L, count, tabref(original->c.env));
			for (uint32_t slot = 0; slot < count; ++slot) {
				setnilV(&copy->c.upvalue[slot]);
			}
			copy->c.ffid = original->c.ffid;
			copy->c.f = original->c.f;
			copy->c.pc = original->c.pc;
			setfuncV(L, L->top, copy);
			incr_top(L);
			const int copyIndex = lua_gettop(L);
			MarkOwn(copy, {}, 0);
			lua_pushvalue(L, index);
			lua_pushvalue(L, copyIndex);
			lua_rawset(L, seen);
			for (uint32_t slot = 0; slot < count; ++slot) {
				copyTV(L, L->top, &original->c.upvalue[slot]);
				incr_top(L);
				mapValue(L, context);
				copyTV(L, &copy->c.upvalue[slot], L->top - 1);
				if (tvisgcv(L->top - 1)) {
					lj_gc_objbarrier(L, copy, gcV(L->top - 1));
				}
				lua_pop(L, 1);
			}
			lua_settop(L, copyIndex);
		}

		void PushClosure(int index) {
			index = index < 0 ? lua_gettop(L) + index + 1 : index;
			GCfunc* original = funcV(Slot(index));
			lua_pushvalue(L, index);
			lua_rawget(L, seen);
			if (!lua_isnil(L, -1)) {
				return;
			}
			lua_pop(L, 1);
			// Decided from the function's own variables before any reference is handed out, so every place that holds
			// the function holds the same value.
			if (!Decide(original)) {
				lua_pushvalue(L, index);
				return;
			}
			if (!isluafunc(original)) {
				PushNativeCopy(index, original);
				return;
			}
			GCfunc* copy = lj_func_newL_empty(L, funcproto(original), tabref(original->l.env));
			setfuncV(L, L->top, copy);
			incr_top(L);
			const int copyIndex = lua_gettop(L);
			lua_pushvalue(L, index);
			lua_pushvalue(L, copyIndex);
			lua_rawset(L, seen);
			std::vector<uint32_t> own;
			for (uint32_t slot = 0; slot < original->l.nupvalues; ++slot) {
				GCupval* upvalue = &gcref(original->l.uvptr[slot])->uv;
				if (!upvalue->closed) {
					lua_State* owner = OwnerOf(upvalue);
					if (Copyable(owner)) {
						HoldOpenVariable(copy, slot, upvalue, owner);
						own.push_back(slot);
					} else {
						SetUpvalue(copy, slot, upvalue);
					}
				} else if (OwnsVariable(upvalue)) {
					TakeOwnVariable(copy, slot, upvalue);
					own.push_back(slot);
				} else {
					// Never written and holding what the copy holds: the original's variable reads the same.
					SetUpvalue(copy, slot, upvalue);
				}
			}
			MarkOwn(copy, own, original->l.nupvalues);
			lua_settop(L, copyIndex);
		}

		void JoinOpenVariables(PreviewCopier& copier) {
			for (size_t at = 0; at < joins.size(); ++at) {
				const Join join = joins[at];
				// A coroutine the collector freed closed its variables first: the copy keeps the value it holds.
				if (join.original->closed) {
					continue;
				}
				auto copied = threads.find(join.thread);
				if (copied == threads.end()) {
					// The walk never reached this coroutine: it is copied now, so the closure writes the copy's variable.
					setthreadV(L, L->top, join.thread);
					incr_top(L);
					lua_pushvalue(L, -1);
					lua_rawget(L, seen);
					const bool known = !lua_isnil(L, -1);
					lua_pop(L, 1);
					if (!known && copier.PushThread(-1)) {
						lua_pop(L, 1);
					}
					lua_pop(L, 1);
					copied = threads.find(join.thread);
					if (copied == threads.end()) {
						continue;
					}
				}
				TValue* stack = tvref(copied->second->stack);
				if (join.slot >= 1 + LJ_FR2 && join.slot < copied->second->top - stack) {
					SetUpvalue(join.function, join.index, FindOpenUpvalue(L, copied->second, stack + join.slot));
				}
			}
			joins.clear();
		}
	};

	PreviewCopier::PreviewCopier(lua_State* state, int seen, MapValue mapValue, Replaced replaced, void* context, int (*standIn)(lua_State*)) :
	    m_Impl(new Impl{state, seen < 0 ? lua_gettop(state) + seen + 1 : seen, mapValue, replaced, context, standIn}) {}

	PreviewCopier::~PreviewCopier() { delete m_Impl; }

	void PreviewCopier::Rebind(int seen, void* context) {
		m_Impl->seen = seen < 0 ? lua_gettop(m_Impl->L) + seen + 1 : seen;
		m_Impl->context = context;
	}

	std::string PreviewCopier::TakeRefusal() {
		return std::exchange(m_Impl->refusal, std::string());
	}

	bool PreviewCopier::PushThread(int index) {
		lua_State* L = m_Impl->L;
		index = index < 0 ? lua_gettop(L) + index + 1 : index;
		lua_State* original = lua_tothread(L, index);
		const std::string status = ThreadStatus(L, original);
		if (status == "running" || status == "normal") {
			return false;
		}
		const int top = lua_gettop(L);
		lua_State* copy = lua_newthread(L);
		const int copyIndex = lua_gettop(L);
		lua_pushvalue(L, index);
		lua_pushvalue(L, copyIndex);
		lua_rawset(L, m_Impl->seen);
		// An empty coroutine reads as dead, as the original does.
		if (status == "dead") {
			return true;
		}
		lua_pushcfunction(L, ThreadCapture);
		lua_pushvalue(L, index);
		if (lua_pcall(L, 1, 1, 0) != 0 || !lua_istable(L, -1)) {
			lua_pushvalue(L, index);
			lua_pushnil(L);
			lua_rawset(L, m_Impl->seen);
			lua_settop(L, top);
			return false;
		}
		const int description = lua_gettop(L);
		lua_getfield(L, description, "slots");
		const int slots = lua_gettop(L);
		std::vector<lua_Integer> keys;
		lua_pushnil(L);
		while (lua_next(L, slots) != 0) {
			keys.push_back(lua_tointeger(L, -2));
			lua_pop(L, 1);
		}
		for (const lua_Integer key: keys) {
			lua_rawgeti(L, slots, static_cast<int>(key));
			m_Impl->mapValue(L, m_Impl->context);
			lua_rawseti(L, slots, static_cast<int>(key));
		}
		lua_pushcfunction(L, ThreadRestore);
		lua_pushvalue(L, description);
		lua_pushvalue(L, copyIndex);
		if (lua_pcall(L, 2, 1, 0) == 0 && lua_isthread(L, -1)) {
			m_Impl->threads[original] = copy;
		} else {
			// A copy that cannot run as the original would stands in for it: resuming it reruns the preview with the scripts frozen.
			EmptyThread(copy);
			lua_pushcfunction(copy, m_Impl->standIn);
		}
		lua_settop(L, copyIndex);
		return true;
	}

	void PreviewCopier::PushFunction(int index) {
		m_Impl->PushClosure(index);
	}

	void PreviewCopier::Finish() {
		m_Impl->JoinOpenVariables(*this);
	}
} // namespace RTE::LuaThreadCodec
