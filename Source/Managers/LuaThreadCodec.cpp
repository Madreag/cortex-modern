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
#include <cstring>
#include <limits>
#include <string>
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
		if (!lua_checkstack(co, static_cast<int>(top) + 16)) {
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
		lua_settop(L, thread);
		return 1;
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
	bool VisitThreadStack(lua_State* thread, lua_State* dest, bool (*visitor)(lua_State* dest, void* context), void* context) {
		if (!thread || !dest || !visitor) return false;
		TValue* stack = tvref(thread->stack);
		const ptrdiff_t topIndex = thread->top - stack;
		if (topIndex <= 1 + LJ_FR2) return false;
		std::vector<char> kind(static_cast<size_t>(topIndex) + 1, 0);
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
		lua_pushcfunction(state, OpenUpvalues);
		lua_setglobal(state, "_ScriptGraphOpenUpvalues");
		lua_pushcfunction(state, JoinOpenUpvalue);
		lua_setglobal(state, "_ScriptGraphJoinOpenUpvalue");
	}
} // namespace RTE::LuaThreadCodec
