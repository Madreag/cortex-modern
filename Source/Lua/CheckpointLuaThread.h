#pragma once

extern "C" {
#include "lua.h"
#include "lj_obj.h"
#include "lj_frame.h"
#include "lj_bc.h"
#include "lj_vm.h"
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace RTE::CheckpointLua {
namespace ThreadDetail {

template<class T, class Owner> const T* Field(const Owner* owner, size_t offset) {
	return reinterpret_cast<const T*>(reinterpret_cast<const char*>(owner) + offset);
}

inline int Failure(lua_State* destination, const std::string& message) {
	lua_pushnil(destination);
	lua_pushlstring(destination, message.data(), message.size());
	return 2;
}

inline const char* ContinuationName(ASMFunction function) {
	struct Symbol { const char* name; ASMFunction function; };
	const Symbol symbols[] = {
		{"cat", reinterpret_cast<ASMFunction>(lj_cont_cat)},
		{"ra", reinterpret_cast<ASMFunction>(lj_cont_ra)},
		{"nop", reinterpret_cast<ASMFunction>(lj_cont_nop)},
		{"condt", reinterpret_cast<ASMFunction>(lj_cont_condt)},
		{"condf", reinterpret_cast<ASMFunction>(lj_cont_condf)},
		{"hook", reinterpret_cast<ASMFunction>(lj_cont_hook)},
		{"stitch", reinterpret_cast<ASMFunction>(lj_cont_stitch)},
	};
	for (const auto& symbol: symbols) if (symbol.function == function) return symbol.name;
	return nullptr;
}

template<class Heap> const char* Status(const Heap& heap, const lua_State* original, const lua_State& thread) {
	const TValue* stack = tvref(thread.stack);
	if (original == heap.State()) return "running";
	if (thread.status == LUA_YIELD) return "suspended";
	if (thread.status != LUA_OK) return "dead";
	if (thread.base > stack + 1 + LJ_FR2) return "normal";
	if (thread.top == thread.base) return "dead";
	return "notstarted";
}

}

template<class View> int PushThreadDescription(lua_State* destination, View& view, const lua_State* originalThread, bool raw = false) {
	using namespace ThreadDetail;
	if (!originalThread) return Failure(destination, "not a coroutine");
	const auto& heap = view.Heap();
	const lua_State thread = heap.template Read<lua_State>(originalThread);
	if (thread.gct != static_cast<uint8_t>(~LJ_TTHREAD)) return Failure(destination, "not a coroutine");
	const std::string status = Status(heap, originalThread, thread);
	if (status == "running" || status == "normal") return Failure(destination, "a " + status + " coroutine cannot be captured");
	const TValue* stack = tvref(thread.stack);
	const ptrdiff_t topIndex = thread.top - stack;
	const ptrdiff_t baseIndex = thread.base - stack;
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
		for (const TValue* frame = thread.base - 1; frame > stack + LJ_FR2;) {
			if (++guard > 100000 || frame - stack >= topIndex) return Failure(destination, "the coroutine's frame chain is corrupt");
			const TValue value = heap.template Read<TValue>(frame);
			Frame info{frame - stack, static_cast<int64_t>(frame_ftsz(&value)), nullptr, nullptr, frame_islua(&value) != 0, false};
			if (info.lua) {
				info.pc = frame_pc(&value);
			} else if (frame_iscont(&value)) {
				TValue continuation[4]{};
				continuation[3] = value;
				for (int offset = 1; offset <= (LJ_FR2 ? 3 : 1); ++offset) continuation[3 - offset] = heap.template Read<TValue>(frame - offset);
				const TValue* savedFrame = &continuation[3];
				if (frame_iscont_fficb(savedFrame) || frame_contv(savedFrame) == LJ_CONT_TAILCALL) {
					return Failure(destination, "the coroutine is suspended inside an unsupported continuation");
				}
				info.cont = ContinuationName(frame_contf(savedFrame));
				if (!info.cont) return Failure(destination, "the coroutine is suspended inside an unknown continuation");
				info.pc = frame_contpc(savedFrame);
				info.collapsed = !raw && std::strcmp(info.cont, "stitch") == 0;
				if (info.collapsed && (info.index - 4 < 1 + LJ_FR2 || !tvisfunc(&continuation[2]))) {
					return Failure(destination, "the coroutine's stitch frame is corrupt");
				}
			}
			frames.push_back(info);
			if (info.lua) {
				const BCIns call = heap.template Read<BCIns>(info.pc - 1);
				frame -= 1 + LJ_FR2 + bc_a(call);
			} else {
				frame = reinterpret_cast<const TValue*>(reinterpret_cast<const char*>(frame) - frame_sized(&value));
			}
		}
	}
	std::vector<ptrdiff_t> removed;
	for (const auto& info: frames) {
		if (info.collapsed) removed.insert(removed.end(), {info.index - 4, info.index - 3, info.index - 2});
	}
	std::sort(removed.begin(), removed.end());
	const auto remap = [&removed](ptrdiff_t slot) { return slot - (std::lower_bound(removed.begin(), removed.end(), slot) - removed.begin()); };
	std::vector<ptrdiff_t> functionSlots;
	for (const auto& info: frames) functionSlots.push_back(info.index - 1);
	const auto locate = [&](const BCIns* pc, ptrdiff_t& functionSlot, ptrdiff_t& position) {
		for (const ptrdiff_t candidate: functionSlots) {
			const TValue value = heap.template Read<TValue>(stack + candidate);
			if (!tvisfunc(&value)) continue;
			const auto* function = funcV(&value);
			const uint8_t ffid = heap.template Read<uint8_t>(Field<uint8_t>(function, offsetof(GCfuncL, ffid)));
			if (ffid != FF_LUA) continue;
			const MRef bytecode = heap.template Read<MRef>(Field<MRef>(function, offsetof(GCfuncL, pc)));
			const auto* prototype = reinterpret_cast<const GCproto*>(mref(bytecode, const char) - sizeof(GCproto));
			const GCproto saved = heap.template Read<GCproto>(prototype);
			const BCIns* code = proto_bc(prototype);
			if (pc >= code && pc <= code + saved.sizebc) {
				functionSlot = candidate;
				position = pc - code;
				return true;
			}
		}
		return false;
	};
	struct Link { ptrdiff_t index; ptrdiff_t functionSlot; ptrdiff_t position; int64_t ftsz; int type; };
	std::vector<Link> resolved;
	std::vector<std::pair<ptrdiff_t, const char*>> continuations;
	std::vector<char> kind(static_cast<size_t>(topIndex) + 1, 0);
	for (const auto& info: frames) {
		kind[info.index] = 1;
		Link link{info.index, -1, 0, info.ftsz, info.collapsed ? FRAME_LUA : static_cast<int>(info.ftsz & FRAME_TYPEP)};
		if ((info.lua || info.collapsed) && !locate(info.pc, link.functionSlot, link.position)) {
			return Failure(destination, "a frame's return address lies in no function on the coroutine's stack");
		}
		resolved.push_back(link);
		if (info.collapsed) {
			kind[info.index - 4] = kind[info.index - 3] = kind[info.index - 2] = 5;
		} else if (info.cont) {
			kind[info.index - 3] = 2;
			kind[info.index - 2] = 3;
			if (std::strcmp(info.cont, "stitch") == 0) kind[info.index - 4] = 4;
			continuations.emplace_back(info.index - 3, info.cont);
			Link continuationPC{info.index - 2, -1, 0, 0, -1};
			if (!locate(info.pc, continuationPC.functionSlot, continuationPC.position)) {
				return Failure(destination, "a frame's return address lies in no function on the coroutine's stack");
			}
			resolved.push_back(continuationPC);
		}
	}
	lua_newtable(destination);
	const int description = lua_gettop(destination);
	lua_pushlstring(destination, status.data(), status.size());
	lua_setfield(destination, description, "status");
	lua_pushinteger(destination, static_cast<lua_Integer>(remap(baseIndex)));
	lua_setfield(destination, description, "base");
	lua_pushinteger(destination, static_cast<lua_Integer>(remap(topIndex)));
	lua_setfield(destination, description, "top");
	lua_pushinteger(destination, 1 + LJ_FR2);
	lua_setfield(destination, description, "first");
	lua_newtable(destination);
	const int slots = lua_gettop(destination);
	lua_newtable(destination);
	const int links = lua_gettop(destination);
	lua_newtable(destination);
	const int conts = lua_gettop(destination);
	for (const auto& link: resolved) {
		lua_newtable(destination);
		if (link.type >= 0) {
			lua_pushinteger(destination, link.type);
			lua_setfield(destination, -2, "type");
		}
		if (link.functionSlot >= 0) {
			lua_pushinteger(destination, static_cast<lua_Integer>(remap(link.functionSlot)));
			lua_setfield(destination, -2, "pcslot");
			lua_pushinteger(destination, static_cast<lua_Integer>(link.position));
			lua_setfield(destination, -2, "pos");
		} else {
			lua_pushinteger(destination, static_cast<lua_Integer>(link.ftsz));
			lua_setfield(destination, -2, "ftsz");
		}
		lua_rawseti(destination, links, static_cast<int>(remap(link.index)));
	}
	for (const auto& [index, name]: continuations) {
		lua_pushstring(destination, name);
		lua_rawseti(destination, conts, static_cast<int>(remap(index)));
	}
	for (ptrdiff_t index = 1 + LJ_FR2; index < topIndex; ++index) {
		if (kind[index] == 4) {
			lua_pushnumber(destination, 0);
			lua_rawseti(destination, slots, static_cast<int>(remap(index)));
			continue;
		}
		if (kind[index] != 0) continue;
		const TValue value = heap.template Read<TValue>(stack + index);
		if (tvisnil(&value)) continue;
		view.Push(destination, value);
		lua_rawseti(destination, slots, static_cast<int>(remap(index)));
	}
	lua_setfield(destination, description, "conts");
	lua_setfield(destination, description, "links");
	lua_setfield(destination, description, "slots");
	return 1;
}

template<class View> int PushOpenUpvalues(lua_State* destination, View& view) {
	using namespace ThreadDetail;
	const auto& heap = view.Heap();
	const lua_State state = heap.template Read<lua_State>(heap.State());
	const auto* global = mref(state.glref, const global_State);
	const GCRef root = heap.template Read<GCRef>(Field<GCRef>(global, offsetof(global_State, gc) + offsetof(GCState, root)));
	lua_newtable(destination);
	const int result = lua_gettop(destination);
	for (const GCobj* object = gcref(root); object;) {
		const GCRef next = heap.template Read<GCRef>(Field<GCRef>(object, offsetof(GChead, nextgc)));
		const uint8_t type = heap.template Read<uint8_t>(Field<uint8_t>(object, offsetof(GChead, gct)));
		if (type == static_cast<uint8_t>(~LJ_TTHREAD)) {
			const auto* original = reinterpret_cast<const lua_State*>(object);
			const lua_State thread = heap.template Read<lua_State>(original);
			const TValue* stack = tvref(thread.stack);
			for (const GCobj* entry = gcref(thread.openupval); entry;) {
				const auto* originalUpvalue = reinterpret_cast<const GCupval*>(entry);
				const GCupval upvalue = heap.template Read<GCupval>(originalUpvalue);
				lua_pushlightuserdata(destination, const_cast<GCupval*>(originalUpvalue));
				lua_newtable(destination);
				TValue value{};
				setgcVraw(&value, const_cast<GCobj*>(object), LJ_TTHREAD);
				view.Push(destination, value);
				lua_setfield(destination, -2, "thread");
				lua_pushinteger(destination, static_cast<lua_Integer>(uvval(&upvalue) - stack));
				lua_setfield(destination, -2, "slot");
				lua_settable(destination, result);
				entry = gcref(upvalue.nextgc);
			}
		}
		object = gcref(next);
	}
	return 1;
}

}
