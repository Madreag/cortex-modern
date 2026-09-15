#include "PreviewScriptSelfTest.h"

#include "AEmitter.h"
#include "ACraft.h"
#include "ActivityMan.h"
#include "AHuman.h"
#include "Actor.h"
#include "Arm.h"
#include "Attachable.h"
#include "AudioMan.h"
#include "HDFirearm.h"
#include "HeldDevice.h"
#include "LuaMan.h"
#include "Magazine.h"
#include "MovableMan.h"
#include "MovableObject.h"
#include "OwnedMovableObjects.h"
#include "PieMenu.h"
#include "PostProcessMan.h"
#include "PresetMan.h"
#include "PreviewEventLedger.h"
#include "RTETools.h"
#include "SoundContainer.h"
#include "SoundSimulation.h"
#include "TimerMan.h"

#include "lua.hpp"

#include <iostream>
#include <functional>
#include <memory>
#include <sstream>
#include <unordered_set>

namespace RTE {

	bool PreviewScriptSelfTest::CheckGlobalWriteBarrier() {
		static const std::string setup = R"lua(
local p = {data = {1, 2, 3, x = 4}, list = {4, 1, 3, 2}, capi = {x = 11, [1] = 12}, cmeta = {}}
_PreviewBarrierProbe = p
local priorUtil = package.loaded['jit.util']
local priorClear = package.loaded['table.clear']
-- Opening table.clear writes the key into the live table library, which no baseline holds.
local priorLibClear = rawget(table, 'clear')
local util = jit and require('jit.util')
local clear = require('table.clear')
p.cleanup = function() package.loaded['jit.util'] = priorUtil; package.loaded['table.clear'] = priorClear; rawset(table, 'clear', priorLibClear) end
p.clearData = {1, 2, key = 3}
p.namedRoot = {value = 1}
rawset(_G, '_ScriptFieldsStash\0probe', p.namedRoot)
p.modeNames = setmetatable({keep = {value = 1}}, {['__mode\0extra'] = 'v'})
local hidden = {deep = {value = 31}}
local sink = {}
local mt = {__index = {fallback = 17}, __newindex = function(t, k, v)
  if k == 'trap' then sink[k] = v else rawset(t, k, v) end
end}
setmetatable(p.data, mt)
p.key = {}; p.data[p.key] = 'key'; p.data[false] = 'false'; p.data[1.5] = 'fraction'; p.data[0] = 'zero'
p.alias = p.data; p.cycle = p; p.sink = sink; p.mt = mt
p.colocated = {10, 20, 30}; p.separate = {}
for i = 1, 96 do p.separate[i] = i end
p.hot = {x = 7, raw = 8, [1] = 9}
p.big = {}
for i = 1, 8192 do p.big[i] = i end
p.hookData = {count = 0}
p.weak = setmetatable({}, {__mode = 'v'})
p.weak[1] = {value = 1}
local function sequence(t)
  local result, k, v = {}, nil, nil
  repeat
    k, v = next(t, k)
    if k ~= nil then result[#result+1] = tostring(k)..'='..tostring(v) end
  until k == nil
  return table.concat(result, '|')
end
local order, length = sequence(p.data), #p.data
local originalCMeta = {kind = 'original'}
setmetatable(p.cmeta, originalCMeta)
local function hot(t, n)
  for i = 1, n do
    t.x = i; t[i % 16 + 1] = i; rawset(t, 'raw', i)
  end
end
local jitEnabled = jit and jit.status() or false
local traceStarts, hotTrace, traceReason, traceSeen = {}, nil, 'no trace event', {}
local function traceEvent(what, tr, func, pos, errmsg)
  traceSeen[what] = (traceSeen[what] or 0) + 1
  if what == 'flush' then
    traceStarts, hotTrace, traceReason = {}, nil, 'trace cache flushed'
  elseif what == 'start' then
    -- A flushed trace number is handed out again, so the recorded one only counts while it keeps its number.
    if tr == hotTrace then hotTrace, traceReason = nil, 'trace number reused' end
    traceStarts[tr] = func
  elseif what == 'stop' then
    if traceStarts[tr] == hot or func == hot then hotTrace, traceReason = tr, 'recorded' end
    traceStarts[tr] = nil
  elseif what == 'abort' then
    if traceStarts[tr] == hot or func == hot then traceReason = 'aborted at pc '..tostring(pos)..': '..tostring(errmsg) end
    traceStarts[tr] = nil
  end
end
local function seenCounts()
  return tostring(traceSeen.start or 0)..'/'..tostring(traceSeen.stop or 0)..'/'..tostring(traceSeen.abort or 0)..'/'..tostring(traceSeen.flush or 0)
end
local function liveTraces()
  local live = 0
  for i = 1, 1024 do if util.traceinfo(i) then live = live + 1 end end
  return live
end
local function recordHot()
  traceStarts, hotTrace, traceReason, traceSeen = {}, nil, 'no trace event', {}
  jit.attach(traceEvent, 'trace')
  local scratch = {}
  hot(scratch, 512); hot(scratch, 512)
  jit.attach(traceEvent)
end
-- A live compiled trace for the guarded loop is a precondition of the JIT arm, so say why it is missing.
local armNote = ''
local function armHotTrace(label)
  if not jitEnabled then _PreviewBarrierHotTrace = label..':jit-off'; return end
  local how = 'kept'
  if hotTrace == nil or util.traceinfo(hotTrace) == nil then
    recordHot()
    how = 'recorded'
    if hotTrace == nil then
      -- A full trace cache and an already patched loop are both ignored in silence, so clear them and record again.
      local first = seenCounts()..'@'..liveTraces()
      jit.flush()
      recordHot()
      how = 'flushed('..first..')'
      if hotTrace == nil then
        -- A recorder left mid-trace stops hot counting for the whole state, and only a mode change aborts it.
        local second = seenCounts()..'@'..liveTraces()
        jit.off(); jit.on(); jit.flush()
        recordHot()
        how = 'restarted('..first..','..second..')'
      end
    end
  end
  assert(hotTrace and util.traceinfo(hotTrace), 'no live compiled trace for the guarded store loop at '..label..': '..tostring(traceReason)..' events(start/stop/abort/flush)='..seenCounts()..' live_traces='..liveTraces()..' status='..tostring(jit.status()))
  local note = label..':'..tostring(hotTrace)..':'..how
  if label == 'setup' then armNote = note end
  _PreviewBarrierHotTrace = armNote..'|'..note
end
-- The first live trace of any origin is what the arm used to accept; it is reported to show it is not this loop's.
local firstAnyTrace = 0
if jitEnabled then
  for i = 1, 65535 do
    if util.traceinfo(i) and util.traceir(i, 1) then firstAnyTrace = i; break end
  end
end
_PreviewBarrierFirstTrace = firstAnyTrace
armHotTrace('setup')
local exits = 0
local function onexit() exits = exits + 1 end
local slot = 1
p.prepare = function()
  p.co = coroutine.create(function()
    local value = {deep = {value = 41}}
    coroutine.yield()
    value.deep.value = 91
    coroutine.yield()
    return value.deep.value
  end)
  assert(coroutine.resume(p.co))
  p.weak[1] = {value = 1}
  armHotTrace('prepare')
end
p.mutate = function()
  assert(rawequal(p.data, p.alias) and p.cycle == p)
  assert(getmetatable(p.data) == mt and p.data.fallback == 17)
  assert(#p.data == length and sequence(p.data) == order)
  assert(rawget(p.data, 'x') == 4 and rawget(p.data, 'fallback') == nil)
  p.data.x = 5; p.data.x = 6; p.data.x = nil; p.data.x = 7
  p.data.trap = 51
  assert(rawget(p.data, 'trap') == nil and sink.trap == 51)
  rawset(p.data, 'raw', 61); rawset(p.data, p.key, nil)
  p.data[-0.0] = 'changed'; p.data[false] = nil; p.data[1.5] = 'changed'
  table.insert(p.list, 2, 8); assert(table.remove(p.list, 1) == 4)
  table.sort(p.list); assert(#p.list == 4 and p.list[1] == 1 and p.list[4] == 8)
  table.move(p.list, 1, 3, 2); assert(table.concat(p.list, ',') == '1,1,2,3')
  clear(p.clearData); assert(next(p.clearData) == nil and #p.clearData == 0)
  p.namedRoot.value = 2; p.modeNames.keep = {value = 2}
  for i = 1, 1024 do p.data['new'..i] = i; p.colocated[i] = i; p.separate[i+96] = i end
  setmetatable(p.data, {__index = {fallback = 29}})
  assert(p.data.fallback == 29 and getmetatable(p.data) ~= mt)
  hidden.deep.value = 81
  assert(not jitEnabled or (hotTrace and util.traceinfo(hotTrace)), 'the guarded store loop lost its compiled trace before the window: '..tostring(traceReason))
  local exitsBefore = exits
  if jitEnabled then jit.attach(onexit, 'texit') end
  hot(p.hot, 512)
  if jitEnabled then jit.attach(onexit) end
  assert(p.hot.x == 512 and rawget(p.hot, 'raw') == 512)
  slot = 2
  assert((jit and jit.status() or false) == jitEnabled)
  assert(coroutine.resume(p.co))
  assert(not jitEnabled or exits > exitsBefore, 'no compiled trace ran for the guarded stores')
  rawset(p.weak, 2, {})
  if p.weak[1] then p.weak[1].value = 2 end
  local hook = function() p.hookData.count = p.hookData.count + 1 end
  debug.sethook(hook, '', 1)
  assert(debug.gethook() == hook)
  debug.sethook()
  assert(p.hookData.count > 0)
  p.added = {nested = {value = 71}}
end
p.collected = function()
  collectgarbage('restart'); collectgarbage('collect'); collectgarbage('collect')
  assert(p.weak[1] == nil and p.weak[2] == nil, 'weak references stayed live')
end
p.accounting = function()
  -- Only the beforeimage of p.big is allocated between the two readings.
  local before = collectgarbage('count')
  p.big[1] = -1
  local inside = collectgarbage('count')
  _ScriptFieldsStash['preview:gc'] = inside
  assert(inside - before >= 48, 'beforeimage bytes unaccounted: '..tostring(inside-before))
end
p.released = function()
  assert(p.big[1] == 1, 'the captured array did not come back')
  collectgarbage('collect')
  local inside, after = _ScriptFieldsStash['preview:gc'], collectgarbage('count')
  _ScriptFieldsStash['preview:gc'] = nil
  assert(inside and inside - after >= 48, 'beforeimage bytes not released: '..tostring(inside)..' -> '..tostring(after))
end
p.upvalueSlotReport = function()
  -- Documented limit: the barrier captures tables, so an upvalue slot keeps what the preview wrote.
  _PreviewBarrierUpvalueSlot = slot
  assert(slot == 2, 'upvalue slot write was undone: '..tostring(slot))
  slot = 1
end
p.check = function()
  assert(p.data == p.alias and p.cycle == p)
  assert(sequence(p.data) == order and #p.data == length, 'table layout changed')
  assert(p.data.x == 4 and p.data[0] == 'zero' and p.data[false] == 'false' and p.data[1.5] == 'fraction')
  assert(p.data[p.key] == 'key' and rawget(p.data, 'raw') == nil and rawget(p.data, 'new1') == nil)
  assert(getmetatable(p.data) == mt and p.data.fallback == 17 and sink.trap == nil)
  assert(table.concat(p.list, ',') == '4,1,3,2' and table.concat(p.colocated, ',') == '10,20,30')
  assert(p.clearData[1] == 1 and p.clearData[2] == 2 and p.clearData.key == 3, 'table.clear leaked')
  assert(p.namedRoot.value == 1 and p.modeNames.keep.value == 1, 'embedded zero in a key changed semantics')
  assert(#p.separate == 96 and p.separate[96] == 96 and p.separate[97] == nil)
  assert(hidden.deep.value == 31 and p.added == nil and p.hookData.count == 0)
  assert(p.hot.x == 7 and p.hot.raw == 8 and p.hot[1] == 9 and p.hot[2] == nil, 'compiled store leaked')
  assert(p.capi.x == 11 and p.capi[1] == 12 and p.capi.added == nil, 'native API store leaked')
  assert(getmetatable(p.cmeta) == originalCMeta, 'native metatable store leaked')
  assert(p.weak[1] == nil and p.weak[2] == nil, 'collected weak reference resurrected')
  local ok, value = coroutine.resume(p.co)
  assert(ok and value == 41, 'coroutine table store leaked')
  assert((jit and jit.status() or false) == jitEnabled)
end
)lua";
		std::vector<LuaStateWrapper*> states{&g_LuaMan.GetMasterScriptState()};
		for (LuaStateWrapper& state: g_LuaMan.GetThreadedScriptStates()) states.push_back(&state);
		bool passed = true;
		const auto check = [&](const char* name, int state, int round, int status) {
			std::cout << "[script-graph-selftest] " << (status >= 0 ? "PASS " : "FAIL ") << name
			          << " state=" << state << " round=" << round;
			if (status < 0) std::cout << " " << states[state]->GetLastError();
			std::cout << std::endl;
			passed = status >= 0 && passed;
		};
		for (int index = 0; index < static_cast<int>(states.size()); ++index) {
			states[index]->RunScriptString("collectgarbage('stop')", false);
			check("preview_barrier_fixture", index, 0, states[index]->RunScriptString(setup, false));
		}
		for (int round = 0; round < 2 && passed; ++round) {
			for (int index = 0; index < static_cast<int>(states.size()); ++index) {
				// The stash is a lazily created per-state global; a threaded state has none until something stashes.
				check("preview_barrier_prepared", index, round, states[index]->RunScriptString("_ScriptFieldsStash = _ScriptFieldsStash or {}; collectgarbage('stop'); _PreviewBarrierProbe.prepare(); _ScriptFieldsStash['preview:-7654321'] = {}", false));
				// Only the registry holds this one, so nothing the walk reaches from the globals refers to it.
				lua_State* armed = states[index]->GetLuaState();
				lua_newtable(armed);
				lua_pushinteger(armed, 1);
				lua_setfield(armed, -2, "value");
				lua_setfield(armed, LUA_REGISTRYINDEX, "_PreviewBarrierRegistry");
			}
			if (!passed) break;
			LuaMan::CapturePreviewSelfCopies({}, false);
			for (LuaStateWrapper* state: states) state->CapturePreviewGlobalFence();
			LuaMan::BeginPreviewScripts({}, false);
			for (int index = 0; index < static_cast<int>(states.size()); ++index) {
				LuaStateWrapper* state = states[index];
				check("preview_barrier_vm_semantics", index, round, state->RunScriptString("_PreviewBarrierProbe.mutate()", false));
				lua_State* L = state->GetLuaState();
				const int top = lua_gettop(L);
				lua_getglobal(L, "_PreviewBarrierProbe");
				lua_getfield(L, -1, "capi");
				lua_pushinteger(L, 92);
				lua_rawseti(L, -2, 1);
				lua_pushinteger(L, 93);
				lua_setfield(L, -2, "x");
				lua_pushliteral(L, "added");
				lua_pushinteger(L, 94);
				lua_rawset(L, -3);
				lua_pop(L, 1);
				lua_getfield(L, -1, "cmeta");
				lua_newtable(L);
				lua_setmetatable(L, -2);
				lua_getfield(L, LUA_REGISTRYINDEX, "_PreviewBarrierRegistry");
				lua_pushinteger(L, 2);
				lua_setfield(L, -2, "value");
				lua_settop(L, top);
				check("preview_barrier_double_arm_refused", index, round, luaJIT_preview_begin(L, nullptr, 0) ? -1 : 0);
				check("preview_barrier_native_semantics", index, round, state->RunScriptString("assert(_PreviewBarrierProbe.capi[1] == 92 and _PreviewBarrierProbe.capi.x == 93 and _PreviewBarrierProbe.capi.added == 94)", false));
				check("preview_barrier_fault_injection", index, round, luaJIT_preview_faultcheck(L) ? 0 : -1);
				check("preview_barrier_gc_accounting", index, round, state->RunScriptString("_PreviewBarrierProbe.accounting()", false));
				check("preview_barrier_weak_semantics", index, round, state->RunScriptString("_PreviewBarrierProbe.collected()", false));
				const int error = state->RunScriptString("_ScriptFieldsStash['preview:-7654321'] = nil; _PreviewBarrierProbe.data.x = 97; error('preview barrier error arm')", false);
				check("preview_barrier_error_caught", index, round, error < 0 ? 0 : -1);
			}
			LuaMan::EndPreviewScripts();
			for (int index = 0; index < static_cast<int>(states.size()); ++index) {
				check("preview_barrier_exact_rollback", index, round, states[index]->RunScriptString("_PreviewBarrierProbe.check(); assert(_ScriptFieldsStash['preview:-7654321'] == nil)", false));
				check("preview_barrier_gc_released", index, round, states[index]->RunScriptString("_PreviewBarrierProbe.released()", false));
				check("preview_barrier_upvalue_slot_limit", index, round, states[index]->RunScriptString("_PreviewBarrierProbe.upvalueSlotReport()", false));
				lua_State* observed = states[index]->GetLuaState();
				const int slotTop = lua_gettop(observed);
				lua_getglobal(observed, "_PreviewBarrierUpvalueSlot");
				lua_getfield(observed, LUA_REGISTRYINDEX, "_PreviewBarrierRegistry");
				int registryValue = -1;
				if (lua_istable(observed, -1)) {
					lua_getfield(observed, -1, "value");
					registryValue = lua_isnumber(observed, -1) ? static_cast<int>(lua_tointeger(observed, -1)) : -1;
				}
				lua_getglobal(observed, "_PreviewBarrierHotTrace");
				const char* hotTrace = lua_isstring(observed, -1) ? lua_tostring(observed, -1) : "none";
				lua_getglobal(observed, "_PreviewBarrierFirstTrace");
				const int firstTrace = lua_isnumber(observed, -1) ? static_cast<int>(lua_tointeger(observed, -1)) : -1;
				// Both are documented limits, so the observed values are printed and a contract change shows up here.
				std::cout << "[preview-barrier] state=" << index << " round=" << round
				          << " upvalue_slot_after_end=" << (lua_isnumber(observed, slotTop + 1) ? static_cast<int>(lua_tointeger(observed, slotTop + 1)) : -1)
				          << " registry_table_after_end=" << registryValue
				          << " hot_trace=" << hotTrace << " first_live_trace=" << firstTrace << std::endl;
				lua_settop(observed, slotTop);
				check("preview_barrier_registry_table", index, round, registryValue == 2 ? 0 : -1);
			}
		}
		for (LuaStateWrapper* state: states) {
			state->RunScriptString("_ScriptFieldsStash = _ScriptFieldsStash or {}; debug.sethook(); if _PreviewBarrierProbe and _PreviewBarrierProbe.cleanup then _PreviewBarrierProbe.cleanup() end; _PreviewBarrierProbe = nil; _PreviewBarrierUpvalueSlot = nil; _PreviewBarrierHotTrace = nil; _PreviewBarrierFirstTrace = nil; rawset(_G, '_ScriptFieldsStash\\0probe', nil); _ScriptFieldsStash['preview:-7654321'] = nil; _ScriptFieldsStash['preview:gc'] = nil; collectgarbage('restart'); collectgarbage('collect')", false);
			lua_State* done = state->GetLuaState();
			lua_pushnil(done);
			lua_setfield(done, LUA_REGISTRYINDEX, "_PreviewBarrierRegistry");
		}
		return passed;
	}

	bool PreviewScriptSelfTest::RunRetirementArm(char mode) {
		const std::string label = std::string("overlay-links ") + mode;
		bool passed = true;
		const auto check = [&](const std::string& name, bool ok, const std::string& detail) {
			std::cout << "[lpinv] " << (ok ? "PASS " : "FAIL ") << label << ": " << name << ": " << detail << std::endl;
			passed = passed && ok;
		};
		const auto address = [](const MovableObject* object) {
			std::ostringstream out;
			out << static_cast<const void*>(object);
			return out.str();
		};
		const auto expect = [&](const std::string& name, const MovableObject* actual, const MovableObject* wanted) {
			check(name, actual == wanted, "observed=" + address(actual) + " expected=" + address(wanted));
		};
		const auto armed = [&](const std::string& name, bool ok, const std::string& detail) {
			if (ok) {
				std::cout << "[lpinv] ARMED " << label << ": " << name << ": " << detail << std::endl;
			} else {
				check("not armed " + name, false, detail);
			}
			return ok;
		};
		Activity* activity = g_ActivityMan.GetActivity();
		const AHuman* original = activity ? dynamic_cast<const AHuman*>(activity->GetControlledActor(Players::PlayerOne)) : nullptr;
		const HeldDevice* device = original ? original->GetEquippedItem() : nullptr;
		const AEmitter* wound = nullptr;
		if (original) {
			for (const Attachable* part: original->GetAttachables()) {
				if (part->GetBreakWound() && part->GetBreakWound()->m_AllLoadedScripts.empty()) {
					wound = part->GetBreakWound();
					break;
				}
			}
		}
		std::list<Entity*> crafts;
		g_PresetMan.GetAllOfType(crafts, "ACDropShip");
		const ACraft* craftPreset = crafts.empty() ? nullptr : dynamic_cast<const ACraft*>(crafts.front());
		MovableObject* residentItem = nullptr;
		const HeldDevice* residentChild = nullptr;
		for (MovableObject* mo: g_MovableMan.SnapshotKnownObjects()) {
			if (mo != original && g_MovableMan.IsResident(mo)) {
				if (!residentItem && dynamic_cast<HeldDevice*>(mo) && g_MovableMan.IsDevice(mo)) residentItem = mo;
				if (const AHuman* human = dynamic_cast<const AHuman*>(mo); human && human->GetEquippedItem()) residentChild = human->GetEquippedItem();
			}
		}
		if (!device) device = dynamic_cast<const HeldDevice*>(residentItem);
		if (!armed("presets", original && device && wound && craftPreset, "actor/device/unscripted wound/craft=" + std::to_string(original != nullptr) + "/" + std::to_string(device != nullptr) + "/" + std::to_string(wound != nullptr) + "/" + std::to_string(craftPreset != nullptr))) return false;
		if (mode == 't' && !armed("resident item", residentItem != nullptr, "address=" + address(residentItem))) return false;
		if (mode == 'x' && !armed("resident held child", residentChild != nullptr, "address=" + address(residentChild))) return false;

		const auto rng = g_SimRNG.GetEngineState();
		const uint64_t draws = g_SimRNG.GetDrawCount();
		const long uid = MovableObject::GetUniqueIDCounter();
		const int cursor = g_LuaMan.GetScriptStateCursor();
		const uint64_t soundCursor = g_AudioMan.GetCheckpointSoundContainerCursor();
		Activity::RollbackState activityState;
		activity->CaptureRollbackState(activityState);
		TerrainLayerSnapshot terrain;
		if (!armed("terrain snapshot", terrain.Capture(), "capture")) return false;
		LuaMan::SetScriptsFrozen(true);
		AudioMan::SetPlaybackSuppressed(true);
		PostProcessMan::SetRegistrationSuppressed(true);
		const auto actorCopy = [&]() { MovableObject::ScriptLoadDeferralScope scope; return dynamic_cast<Actor*>(original->Clone()); };
		const auto craftCopy = [&]() { MovableObject::ScriptLoadDeferralScope scope; return dynamic_cast<ACraft*>(craftPreset->Clone()); };
		const auto deviceCopy = [&]() { MovableObject::ScriptLoadDeferralScope scope; return dynamic_cast<HeldDevice*>(device->Clone()); };
		const auto woundCopy = [&]() { MovableObject::ScriptLoadDeferralScope scope; return dynamic_cast<AEmitter*>(wound->Clone()); };
		const auto link = [](MovableObject* holder, MovableObject* target) {
			holder->m_FaithfulMOToNotHitUID = 0;
			holder->SetWhichMOToNotHit(target, -1.0F);
		};
		const int repeats = mode == 't' ? 2 : 1;
		for (int subcase = 0; subcase < repeats; ++subcase) {
			std::vector<std::unique_ptr<MovableObject>> survivors;
			Actor* survivor = actorCopy();
			survivors.emplace_back(survivor);
			Actor* guard = actorCopy();
			survivors.emplace_back(guard);
			HeldDevice* guardCargo = deviceCopy();
			guard->AddInventoryItem(guardCargo);
			ACraft* survivorCraft = craftCopy();
			survivors.emplace_back(survivorCraft);
			survivorCraft->OpenHatch();
			HeldDevice* guardCollected = deviceCopy();
			survivorCraft->AddInventoryItem(guardCollected);
			std::vector<MovableObject*> roots{survivor, guard, survivorCraft};
			LuaStateWrapper* master = &g_LuaMan.GetMasterScriptState();
			if (mode == 'l') {
				// The fixture's globals are canonical state: loading it inside the preview window would hand the fence its own chunk to undo.
				master->RunScriptString("_F15Path = package.path", false);
				const int loaded = master->RunScriptFile("Tests.rte/F15Retirement/HeldRefs.lua", false, false);
				if (!armed("fixture_loaded", loaded >= 0, "Tests.rte/F15Retirement/HeldRefs.lua status=" + std::to_string(loaded) + " " + master->GetLastError())) return false;
				const int captured = master->RunScriptFunctionString("F15Refs.Capture", "", {}, {device}, {});
				if (!armed("fixture_capture", captured >= 0, "canonical device=" + address(device) + " " + master->GetLastError())) return false;
			}
			LuaMan::CapturePreviewSelfCopies({}, false);
			// Each arm starts from an empty ledger: an earlier arm's identical event key would ghost nothing here.
			PreviewEventLedger::ResetBetweenSelfTestArms();
			PreviewEventLedger::Arm(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()), soundCursor, {static_cast<uint64_t>(original->GetUniqueID())});
			g_MovableMan.BeginSpeculation();
			LuaMan::BeginPreviewScripts(roots, false);
			std::vector<std::function<void()>> observations;
			std::vector<std::function<void()>> afterDispose;
			std::vector<MovableObject*> ghostRoots;
			const auto add = [&](MovableObject* root) {
				SoundSimulationScope scope(static_cast<uint64_t>(original->GetUniqueID()), Hash("retirement-arm"));
				g_MovableMan.AddMO(root);
			};
			const auto weak = [&](const std::string& name, MovableObject* target, MovableObject* wanted) {
				AEmitter* holder = woundCopy();
				survivor->AddWound(holder, Vector(), false);
				link(holder, target);
				armed(name, holder->GetWhichMOToNotHit() == target && target, "holder=" + address(holder) + " target=" + address(target));
				observations.push_back([=, &expect]() { expect(name, holder->GetWhichMOToNotHit(), wanted); });
			};
			weak("surviving_actor_inventory", guardCargo, guardCargo);
			weak("surviving_craft_collected", guardCollected, guardCollected);
			armed("surviving_collected_membership", std::find(survivorCraft->GetCollectedInventory().begin(), survivorCraft->GetCollectedInventory().end(), guardCollected) != survivorCraft->GetCollectedInventory().end(), "cargo=" + address(guardCollected));
			if (mode == 'i' || mode == 'x') {
				Actor* actor = actorCopy();
				ACraft* craft = craftCopy();
				HeldDevice* cargo = deviceCopy();
				HeldDevice* collected = deviceCopy();
				AEmitter* actorChild = woundCopy();
				AEmitter* craftChild = woundCopy();
				cargo->AddWound(actorChild, Vector(), false);
				collected->AddWound(craftChild, Vector(), false);
				actor->AddInventoryItem(cargo);
				craft->OpenHatch();
				craft->AddInventoryItem(collected);
				armed("actor_inventory_membership", std::find(actor->GetInventory()->begin(), actor->GetInventory()->end(), cargo) != actor->GetInventory()->end(), "owner=" + address(actor) + " cargo=" + address(cargo));
				armed("craft_collected_membership", std::find(craft->GetCollectedInventory().begin(), craft->GetCollectedInventory().end(), collected) != craft->GetCollectedInventory().end(), "owner=" + address(craft) + " cargo=" + address(collected));
				add(actor);
				add(craft);
				g_MovableMan.HarvestSpeculativeSpawns();
				ghostRoots = {actor, craft};
				if (mode == 'i') {
					weak("actor_inventory_child", actorChild, nullptr);
					weak("craft_collected_child", craftChild, nullptr);
				} else {
					survivor->SetItemInReach(cargo);
					if (armed("craft exit", !survivorCraft->m_Exits.empty(), "count=" + std::to_string(survivorCraft->m_Exits.size()))) {
						auto* exit = &survivorCraft->m_Exits.front();
						exit->m_FaithfulIncomingMOUID = 0;
						exit->m_pIncomingMO = collected;
						armed("raw_craft_child_before_remap", exit->m_pIncomingMO == collected, "target=" + address(collected));
						observations.push_back([=, &expect]() { expect("raw_craft_exit_to_retiring_child", exit->m_pIncomingMO, nullptr); });
						afterDispose.push_back([=, &check]() { exit->m_Clear = true; exit->SuckInMOs(survivorCraft); check("craft_exit_consumer", exit->m_pIncomingMO == nullptr, "incoming=" + address(exit->m_pIncomingMO)); });
					}
					armed("raw_item_before_remap", survivor->GetItemInReach() == cargo, "target=" + address(cargo));
					observations.push_back([=, &expect]() { expect("raw_item_in_reach_to_retiring_child", survivor->GetItemInReach(), nullptr); });
					MovableObject* shadowChild = g_MovableMan.FindObjectByUniqueID(residentChild->GetUniqueID());
					armed("raw_shadow_child", shadowChild && shadowChild != residentChild && dynamic_cast<HeldDevice*>(shadowChild), "shadow=" + address(shadowChild) + " resident=" + address(residentChild));
					guard->SetItemInReach(dynamic_cast<HeldDevice*>(shadowChild));
					observations.push_back([=, &expect]() { expect("raw_item_in_reach_to_shadow_child", guard->GetItemInReach(), residentChild); });
				}
			} else if (mode == 't') {
				MovableObject* shadow = g_MovableMan.FindObjectByUniqueID(residentItem->GetUniqueID());
				MovableObject* taken = g_MovableMan.RemoveItem(shadow);
				Actor* taker = subcase == 0 ? actorCopy() : guard;
				taker->AddInventoryItem(taken);
				if (subcase == 0) {
					add(taker);
					g_MovableMan.HarvestSpeculativeSpawns();
					ghostRoots.push_back(taker);
				}
				const auto found = g_MovableMan.m_Speculation.shadows.find(residentItem);
				armed("taken_shadow_ownership", taken && taken == shadow && found != g_MovableMan.m_Speculation.shadows.end() && !found->second.inWorld && std::find(taker->GetInventory()->begin(), taker->GetInventory()->end(), taken) != taker->GetInventory()->end(), "inWorld=" + std::to_string(found == g_MovableMan.m_Speculation.shadows.end() ? -1 : found->second.inWorld) + " shadow=" + address(shadow) + " taker=" + address(taker) + " retiring=" + std::to_string(subcase == 0));
				weak(subcase == 0 ? "taken_shadow_with_retiring_owner" : "taken_shadow_with_surviving_owner", taken, subcase == 0 ? residentItem : taken);
				afterDispose.push_back([=, &check]() { check("canonical_resident_live", g_MovableMan.IsDevice(residentItem) && g_MovableMan.IsKnownObject(residentItem), "resident=" + address(residentItem)); });
			} else if (mode == 'm') {
				HeldDevice* reacquired = deviceCopy();
				add(reacquired);
				MovableObject* removed = g_MovableMan.RemoveItem(reacquired);
				guard->AddInventoryItem(removed);
				armed("metadata_only_reacquired", removed == reacquired && g_MovableMan.m_Speculation.spawnMeta.count(reacquired) == 1 && g_MovableMan.GetSpeculativeSpawnCount() == 0 && std::find(g_MovableMan.m_AddedItems.begin(), g_MovableMan.m_AddedItems.end(), reacquired) == g_MovableMan.m_AddedItems.end(), "target=" + address(reacquired) + " metadata=" + std::to_string(g_MovableMan.m_Speculation.spawnMeta.count(reacquired)));
				weak("reacquired_spawn_meta_key", reacquired, reacquired);
				HeldDevice* stale = deviceCopy();
				add(stale);
				const bool removedStale = g_MovableMan.RemoveItem(stale) == stale;
				stale->DestroyScriptState();
				delete stale;
				armed("stale_metadata_key", removedStale && g_MovableMan.m_Speculation.spawnMeta.count(stale) == 1 && !g_MovableMan.IsKnownObject(stale), "deleted address=" + address(stale));
				observations.push_back([=, &check]() { check("stale_key_listed_without_traversal", g_MovableMan.RetiringOverlayObjects().count(stale) == 1, "key=" + address(stale)); });
			} else if (mode == 'q') {
				std::vector<MOSRotating*> queued{actorCopy(), deviceCopy(), woundCopy()};
				const char* kinds[] = {"actor", "item", "particle"};
				for (size_t i = 0; i < queued.size(); ++i) {
					AEmitter* child = woundCopy();
					queued[i]->AddWound(child, Vector(), false);
					add(queued[i]);
					weak(std::string("added_tail_") + kinds[i] + "_child", child, nullptr);
				}
				const auto tail = g_MovableMan.MarkAddQueues();
				const auto mark = g_MovableMan.m_Speculation.mark;
				armed("three_unharvested_tails", tail.actors == mark.actors + 1 && tail.items == mark.items + 1 && tail.particles == mark.particles + 1 && g_MovableMan.GetSpeculativeSpawnCount() == 0, "actors=" + std::to_string(tail.actors - mark.actors) + " items=" + std::to_string(tail.items - mark.items) + " particles=" + std::to_string(tail.particles - mark.particles) + " harvested=" + std::to_string(g_MovableMan.GetSpeculativeSpawnCount()));
			} else if (mode == 'o') {
				HeldDevice* retiring = deviceCopy();
				HeldDevice* surviving = deviceCopy();
				guard->AddInventoryItem(surviving);
				for (HeldDevice* owner: {retiring, surviving}) {
					owner->SetOwnedBreakWound(woundCopy());
					owner->SetOwnedParentBreakWound(woundCopy());
					link(owner->GetOwnedBreakWound(), retiring);
					link(owner->GetOwnedParentBreakWound(), retiring);
				}
				add(retiring);
				g_MovableMan.HarvestSpeculativeSpawns();
				ghostRoots.push_back(retiring);
				armed("owned_templates", surviving->GetOwnedBreakWound() && surviving->GetOwnedParentBreakWound() && retiring->GetOwnedBreakWound() && retiring->GetOwnedParentBreakWound(), "owned=4 shared=" + address(wound));
				observations.push_back([=, &expect]() { expect("owned_break_wound_outgoing_link", surviving->GetOwnedBreakWound()->GetWhichMOToNotHit(), nullptr); });
				observations.push_back([=, &expect]() { expect("owned_parent_break_wound_outgoing_link", surviving->GetOwnedParentBreakWound()->GetWhichMOToNotHit(), nullptr); });
				weak("retiring_owned_break_wound_inbound", retiring->GetOwnedBreakWound(), nullptr);
				weak("shared_preset_guard", const_cast<AEmitter*>(wound), const_cast<AEmitter*>(wound));
			} else if (mode == 'l') {
				// A mod-style fixture holds Lua references across the boundary and reads one link back.
				Actor* actor = actorCopy();
				HeldDevice* cargo = deviceCopy();
				AEmitter* cargoChild = woundCopy();
				cargo->AddWound(cargoChild, Vector(), false);
				actor->AddInventoryItem(cargo);
				add(actor);
				g_MovableMan.HarvestSpeculativeSpawns();
				ghostRoots.push_back(actor);
				AEmitter* holder = woundCopy();
				survivor->AddWound(holder, Vector(), false);
				link(holder, cargoChild);
				if (!armed("lua_link_holder", holder->GetWhichMOToNotHit() == cargoChild, "holder=" + address(holder) + " target=" + address(cargoChild))) return false;
				observations.push_back([=, &check]() {
					const int status = master->RunScriptFunctionString("F15Refs.CheckLink", "", {}, {holder}, {});
					check("fixture_link_observation", status >= 0, status >= 0 ? "holder=" + address(holder) : master->GetLastError());
				});
				afterDispose.push_back([=, &check]() {
					const int status = master->RunScriptFunctionString("F15Refs.Verify", "", {}, {device}, {});
					check("fixture_reference_semantics", status >= 0, status >= 0 ? "closure, shared identity, continuation and property alias held" : master->GetLastError());
					master->RunScriptString("F15Refs.Release()", false);
					master->RunScriptString("package.path = _F15Path _F15Path = nil collectgarbage('collect')", false);
				});
			} else if (mode == 'a') {
				// Manufactured producer: the natural late-detach timing is not armed, so the support pointer is set here.
				AHuman* supporter = dynamic_cast<AHuman*>(survivor);
				Arm* backArm = supporter ? supporter->GetBGArm() : nullptr;
				HeldDevice* retiringDevice = deviceCopy();
				HeldDevice* survivingDevice = deviceCopy();
				guard->AddInventoryItem(survivingDevice);
				add(retiringDevice);
				g_MovableMan.HarvestSpeculativeSpawns();
				ghostRoots.push_back(retiringDevice);
				if (armed("background_arm", backArm != nullptr, "arm=" + address(backArm))) {
					backArm->SetHeldDeviceThisArmIsTryingToSupport(retiringDevice);
					armed("support_before_remap", backArm->GetHeldDeviceThisArmIsTryingToSupport() == retiringDevice, "support=" + address(retiringDevice));
					const std::string retiringAddress = address(retiringDevice);
					observations.push_back([=, &check]() {
						const HeldDevice* held = backArm->GetHeldDeviceThisArmIsTryingToSupport();
						check("supported_device_after_retirement", held == nullptr, "observed=" + address(held) + " expected=0000000000000000");
					});
					afterDispose.push_back([=, &check]() {
						// The target is gone; the pointer value is compared, never read through.
						const std::string stale = address(backArm->GetHeldDeviceThisArmIsTryingToSupport());
						check("support_pointer_after_disposal", stale != retiringAddress, "observed=" + stale + " retired=" + retiringAddress);
						backArm->SetHeldDeviceThisArmIsTryingToSupport(nullptr);
					});
					Actor* guardSupporterHolder = guard;
					AHuman* guardSupporter = dynamic_cast<AHuman*>(guardSupporterHolder);
					Arm* guardArm = guardSupporter ? guardSupporter->GetBGArm() : nullptr;
					if (armed("surviving_support_guard", guardArm != nullptr, "arm=" + address(guardArm))) {
						guardArm->SetHeldDeviceThisArmIsTryingToSupport(survivingDevice);
						observations.push_back([=, &check]() {
							const HeldDevice* held = guardArm->GetHeldDeviceThisArmIsTryingToSupport();
							check("surviving_supported_device_retained", held == survivingDevice, "observed=" + address(held) + " expected=" + address(survivingDevice));
						});
						afterDispose.push_back([=]() { guardArm->SetHeldDeviceThisArmIsTryingToSupport(nullptr); });
					}
				}
			} else if (mode == 'p') {
				// Manufactured producer: nothing in the engine sets the affected object today, so the arm sets it.
				HeldDevice* retiringDevice = deviceCopy();
				HeldDevice* survivingDevice = deviceCopy();
				guard->AddInventoryItem(survivingDevice);
				add(retiringDevice);
				g_MovableMan.HarvestSpeculativeSpawns();
				ghostRoots.push_back(retiringDevice);
				PieMenu* previewMenu = survivor->GetPieMenu();
				PieMenu* craftMenu = survivorCraft->GetPieMenu();
				MovableObject* shadowItem = residentItem ? g_MovableMan.FindObjectByUniqueID(residentItem->GetUniqueID()) : nullptr;
				PieMenu* guardMenu = shadowItem && shadowItem != residentItem ? guard->GetPieMenu() : nullptr;
				if (armed("preview_pie_menu", previewMenu != nullptr && craftMenu != nullptr, "owner=" + address(survivor) + " craft=" + address(survivorCraft) + " shadow_case=" + std::to_string(guardMenu != nullptr))) {
					previewMenu->SetAffectedObject(retiringDevice);
					craftMenu->SetAffectedObject(survivingDevice);
					armed("affected_object_before_remap", previewMenu->GetAffectedObject() == retiringDevice && craftMenu->GetAffectedObject() == survivingDevice, "retiring=" + address(retiringDevice) + " surviving=" + address(survivingDevice));
					const std::string retiringAffected = address(retiringDevice);
					observations.push_back([=, &check]() {
						const MovableObject* affected = previewMenu->GetAffectedObject();
						check("affected_object_after_retirement", affected == nullptr, "observed=" + address(affected) + " expected=0000000000000000");
					});
					observations.push_back([=, &check]() {
						const MovableObject* affected = craftMenu->GetAffectedObject();
						check("surviving_affected_object_retained", affected == survivingDevice, "observed=" + address(affected) + " expected=" + address(survivingDevice));
					});
					afterDispose.push_back([=, &check]() {
						// The target is gone; the pointer value is compared, never read through.
						const std::string stale = address(previewMenu->GetAffectedObject());
						check("affected_object_pointer_after_disposal", stale != retiringAffected, "observed=" + stale + " retired=" + retiringAffected);
						previewMenu->SetAffectedObject(nullptr);
						craftMenu->SetAffectedObject(nullptr);
					});
					if (guardMenu) {
						const long residentUID = residentItem->GetUniqueID();
						guardMenu->SetAffectedObject(shadowItem);
						armed("affected_object_shadow_before_remap", guardMenu->GetAffectedObject() == shadowItem, "shadow=" + address(shadowItem) + " resident=" + address(residentItem) + " uid=" + std::to_string(residentUID));
						observations.push_back([=, &check]() {
							const MovableObject* affected = guardMenu->GetAffectedObject();
							// Only the resident is safe to read through; a stale shadow is compared by address alone.
							const long observedUID = affected == residentItem ? affected->GetUniqueID() : -1;
							check("affected_object_to_shadow_item", affected == residentItem && observedUID == residentUID, "observed=" + address(affected) + " uid=" + std::to_string(observedUID) + " expected=" + address(residentItem) + " uid=" + std::to_string(residentUID));
						});
						afterDispose.push_back([=]() { guardMenu->SetAffectedObject(nullptr); });
					}
				}
			} else if (mode == 'n') {
				// A preview part gains a script the supported way; the canonical state assignment must not move.
				AEmitter* retiringPart = woundCopy();
				AEmitter* survivingPart = woundCopy();
				survivor->AddWound(survivingPart, Vector(), false);
				const int cursorBefore = g_LuaMan.GetScriptStateCursor();
				const bool stateless = retiringPart->GetAllLoadedScripts().empty() && survivingPart->GetAllLoadedScripts().empty() && !retiringPart->GetLuaState() && !survivingPart->GetLuaState();
				armed("stateless_preview_parts", stateless, "cursor=" + std::to_string(cursorBefore) + " retiring_scripts=" + std::to_string(retiringPart->GetAllLoadedScripts().size()) + " surviving_scripts=" + std::to_string(survivingPart->GetAllLoadedScripts().size()));
				const int retiringLoad = stateless ? retiringPart->LoadScript(g_PresetMan.GetFullModulePath("Tests.rte/PreviewCompat.lua")) : -99;
				const int survivingLoad = stateless ? survivingPart->LoadScript(g_PresetMan.GetFullModulePath("Tests.rte/PreviewCompat.lua")) : -99;
				armed("late_scripts_loaded", retiringLoad == 0 && survivingLoad == 0, "retiring=" + std::to_string(retiringLoad) + " surviving=" + std::to_string(survivingLoad));
				const int cursorGained = g_LuaMan.GetScriptStateCursor();
				armed("late_state_allocation", true, "retiring_state=" + std::to_string(g_LuaMan.GetStateIndex(retiringPart->GetLuaState())) + " surviving_state=" + std::to_string(g_LuaMan.GetStateIndex(survivingPart->GetLuaState())) + " cursor " + std::to_string(cursorBefore) + "->" + std::to_string(cursorGained));
				add(retiringPart);
                                g_MovableMan.HarvestSpeculativeSpawns();
				ghostRoots.push_back(retiringPart);
				afterDispose.push_back([=, &check]() {
					check("lua_state_cursor_after_preview", g_LuaMan.GetScriptStateCursor() == cursor, "observed=" + std::to_string(g_LuaMan.GetScriptStateCursor()) + " saved=" + std::to_string(cursor));
					check("no_canonical_slot_for_preview_part", !survivingPart->ObjectScriptsInitialized(), "slot='" + survivingPart->m_ScriptObjectName + "' state=" + std::to_string(g_LuaMan.GetStateIndex(survivingPart->GetLuaState())));
				});
			} else {
				check("unknown arm", false, std::string(1, mode));
			}
			const auto retiring = g_MovableMan.RetiringOverlayObjects();
			armed("retirement_boundary", g_MovableMan.IsSpeculative() && (!retiring.empty() || (mode == 't' && subcase == 1)), "retiring=" + std::to_string(retiring.size()) + " harvested=" + std::to_string(g_MovableMan.GetSpeculativeSpawnCount()));
			for (MovableObject* root: roots) root->RemapExternalLinks([&](MovableObject* mo) { return g_MovableMan.OverlaySurvivorOf(mo, retiring); });
			std::cout << "[lpinv] OBSERVE " << label << ": before EndSpeculation" << std::endl;
			for (const auto& observe: observations) observe();
			g_MovableMan.EndSpeculation();
			LuaMan::EndPreviewScripts();
			PreviewEventLedger::Disarm();
			if (mode != 'm') {
				std::cout << "[lpinv] OBSERVE " << label << ": after EndPreviewScripts" << std::endl;
				for (const auto& observe: observations) observe();
			}
			for (MovableObject* ghost: ghostRoots) {
				const bool retained = std::any_of(g_MovableMan.m_PreviewGhosts.begin(), g_MovableMan.m_PreviewGhosts.end(), [=](const auto& entry) { return entry.object == ghost; });
				check("named_ghost_owner_retained", retained, "owner=" + address(ghost));
			}
			g_MovableMan.DropAllPreviewGhosts();
			for (const auto& observe: afterDispose) observe();
			for (const auto& owner: survivors) owner->DestroyScriptState();
		}
		terrain.Restore();
		activity->RestoreRollbackState(activityState);
		g_SimRNG.SetEngineState(rng);
		g_SimRNG.SetDrawCount(draws);
		MovableObject::PinUniqueIDCounter(uid);
		g_LuaMan.SetScriptStateCursor(cursor);
		g_AudioMan.SetCheckpointSoundContainerCursor(soundCursor);
		PostProcessMan::SetRegistrationSuppressed(false);
		AudioMan::SetPlaybackSuppressed(false);
		LuaMan::SetScriptsFrozen(false);
		return passed;
	}

	bool PreviewScriptSelfTest::s_SubtreeProbe = false;
	bool PreviewScriptSelfTest::s_SharedSlot = false;
	bool PreviewScriptSelfTest::s_StrideCounter = false;
	bool PreviewScriptSelfTest::s_PreviewStrideSeen = false;
	bool PreviewScriptSelfTest::s_Probed = false;
	bool PreviewScriptSelfTest::s_Played = false;
	bool PreviewScriptSelfTest::s_ChildWasPreviewed = false;
	uint64_t PreviewScriptSelfTest::s_RootUID = 0;
	uint64_t PreviewScriptSelfTest::s_ChildUID = 0;
	uint64_t PreviewScriptSelfTest::s_CommittedTick = 0;
	PreviewScriptSelfTest::OverlayLinkProbe PreviewScriptSelfTest::s_OverlayLinkProbe;

	void PreviewScriptSelfTest::SetSubtreeProbe(bool enabled) {
		s_SubtreeProbe = enabled;
	}

	bool PreviewScriptSelfTest::SubtreeProbeEnabled() {
		return s_SubtreeProbe;
	}

	void PreviewScriptSelfTest::SetSharedSlot(bool shared) {
		s_SharedSlot = shared;
	}

	bool PreviewScriptSelfTest::SharedSlot() {
		return s_SharedSlot;
	}

	void PreviewScriptSelfTest::ProbeArmedEmitters(const Actor* original) {
		if (!s_SubtreeProbe || s_Played || !original || !g_AudioMan.IsAudioEnabled()) {
			return;
		}
		s_RootUID = static_cast<uint64_t>(original->GetUniqueID());
		s_ChildUID = 0;
		SoundContainer* sound = nullptr;
		if (const AHuman* human = dynamic_cast<const AHuman*>(original)) {
			if (const HDFirearm* gun = dynamic_cast<const HDFirearm*>(human->GetEquippedItem())) {
				s_ChildUID = static_cast<uint64_t>(gun->GetUniqueID());
				sound = gun->GetFireSound();
			}
			if (!sound) {
				sound = human->GetStrideSound();
			}
		}
		if (!s_ChildUID) {
			std::unordered_set<const Entity*> visited;
			std::unordered_set<const MovableObject*> objects;
			CollectOwnedMovableObjects(original, visited, objects);
			for (const MovableObject* mo: objects) {
				if (mo && mo->GetUniqueID() > 0 && static_cast<uint64_t>(mo->GetUniqueID()) != s_RootUID) {
					s_ChildUID = static_cast<uint64_t>(mo->GetUniqueID());
					break;
				}
			}
		}
		s_ChildWasPreviewed = s_ChildUID && PreviewEventLedger::IsPreviewedEmitter(s_ChildUID);
		s_CommittedTick = PreviewEventLedger::CommittedTick();
		s_Probed = true;
		if (!sound || !s_ChildUID || !sound->HasAnySounds()) {
			return;
		}
		SoundSimulationScope scope(s_ChildUID, Hash("subtree-emitter-probe"));
		s_Played = sound->Play();
	}

	bool PreviewScriptSelfTest::CheckSubtreeEmitter(long long pressTick) {
		bool passed = true;
		const auto check = [&passed](const char* name, bool ok, const std::string& detail) {
			std::cout << "[preview-subtree-selftest] " << (ok ? "PASS " : "FAIL ") << name << ": " << detail << std::endl;
			passed = passed && ok;
		};
		if (!s_SubtreeProbe) {
			return true;
		}
		check("a_non_root_part_exists", s_ChildUID != 0 && s_ChildUID != s_RootUID,
		      "root=" + std::to_string(s_RootUID) + " child=" + std::to_string(s_ChildUID) + " probed=" + std::to_string(s_Probed ? 1 : 0));
		check("the_non_root_part_is_a_previewed_emitter", s_ChildWasPreviewed,
		      "child uid=" + std::to_string(s_ChildUID) + " previewed=" + std::to_string(s_ChildWasPreviewed ? 1 : 0));
		const PreviewEventLedger::EventStart* hit = nullptr;
		for (const PreviewEventLedger::EventStart& start: PreviewEventLedger::GetEventStarts()) {
			if (start.kind == PreviewEventLedger::Sound && start.emitterUID == s_ChildUID && start.predicted) {
				hit = &start;
				break;
			}
		}
		if (!hit) {
			for (const PreviewEventLedger::EventStart& start: PreviewEventLedger::GetEventStarts()) {
				if (start.kind == PreviewEventLedger::Sound && start.emitterUID == s_ChildUID) {
					hit = &start;
					break;
				}
			}
		}
		const uint64_t press = pressTick > 0 ? static_cast<uint64_t>(pressTick) : s_CommittedTick;
		check("a_native_sound_starts_on_the_preview_tick", hit && hit->predicted && hit->committedTick <= press + 1,
		      hit ? "child uid=" + std::to_string(s_ChildUID) + " committed=" + std::to_string(hit->committedTick) + " event=" + std::to_string(hit->eventTick) +
		                " predicted=" + std::to_string(hit->predicted ? 1 : 0) + " played=" + std::to_string(s_Played ? 1 : 0)
		          : "no physical voice keyed by child uid " + std::to_string(s_ChildUID) + " played=" + std::to_string(s_Played ? 1 : 0));
		std::cout << "[preview-subtree-selftest] " << (passed ? "PASS" : "FAIL") << std::endl;
		return passed;
	}

	bool PreviewScriptSelfTest::CheckNestedHookScope() {
		LuaMan::SetRunningPreviewHook(false);
		bool nestedKept = false;
		{
			LuaMan::PreviewHookScope outer(true);
			{
				LuaMan::PreviewHookScope inner(true);
			}
			nestedKept = LuaMan::IsRunningPreviewHook();
		}
		const bool outerCleared = !LuaMan::IsRunningPreviewHook();
		const bool ok = nestedKept && outerCleared;
		std::cout << "[preview-hook-scope] " << (ok ? "PASS" : "FAIL") << " nested_inner_does_not_clear_outer kept=" << (nestedKept ? 1 : 0) << " cleared=" << (outerCleared ? 1 : 0) << std::endl;
		return ok;
	}

	void PreviewScriptSelfTest::SetStrideCounter(bool enabled) {
		s_StrideCounter = enabled;
	}

	bool PreviewScriptSelfTest::StrideCounterRequested() {
		return s_StrideCounter;
	}

	void PreviewScriptSelfTest::NotePreviewStride(bool initialized) {
		if (initialized) {
			s_PreviewStrideSeen = true;
		}
	}

	bool PreviewScriptSelfTest::InstallStrideCounter(MovableObject* object) {
		RunOverlayLinkProbe(object);
		if (!object || !object->GetLuaState()) {
			return false;
		}
		return object->GetLuaState()->AttachPreviewInvStride(object);
	}

	void PreviewScriptSelfTest::ArmOverlayLinkProbe(char mode, const Actor* residentActor, const MovableObject* residentItem) {
		s_OverlayLinkProbe = {};
		s_OverlayLinkProbe.mode = mode;
		s_OverlayLinkProbe.residentActor = residentActor;
		s_OverlayLinkProbe.residentItem = residentItem;
	}

	void PreviewScriptSelfTest::DisarmOverlayLinkProbe() {
		s_OverlayLinkProbe = {};
	}

	// Runs at the stride-counter seam: speculation is on, the clones are bound, their faithful links not yet resolved.
	void PreviewScriptSelfTest::RunOverlayLinkProbe(MovableObject* object) {
		OverlayLinkProbe& probe = s_OverlayLinkProbe;
		if (probe.mode == 0 || probe.ran || !object || !g_MovableMan.IsSpeculative() || !LuaMan::IsPreviewClone(object)) {
			return;
		}
		probe.ran = true;
		AHuman* clone = dynamic_cast<AHuman*>(object);
		if (!clone) {
			probe.failure = "the preview clone is not an AHuman";
			return;
		}
		probe.clone = clone;
		probe.cloneUID = clone->GetUniqueID();
		// A link set here has to outlive the faithful link resolution RunPreview does after this seam.
		const auto link = [](MovableObject* holder, MovableObject* target) {
			holder->m_FaithfulMOToNotHitUID = 0;
			holder->SetWhichMOToNotHit(target, -1.0F);
		};
		const auto shadowOf = [](const MovableObject* resident) -> MovableObject* {
			MovableObject* view = resident ? g_MovableMan.FindObjectByUniqueID(resident->GetUniqueID()) : nullptr;
			return view != resident ? view : nullptr;
		};
		// The root's direct parts are siblings, so a link set on one never propagates onto another.
		const std::vector<MovableObject*> parts(clone->GetAttachableList().begin(), clone->GetAttachableList().end());

		if (probe.mode == 'r') {
			if (clone->GetItemInReachUniqueID() != 0) {
				probe.failure = "the clone carries a faithful item in reach that link resolution would put back";
				return;
			}
			HeldDevice* shadow = dynamic_cast<HeldDevice*>(shadowOf(probe.residentItem));
			if (!shadow || !g_MovableMan.IsDevice(shadow)) {
				probe.failure = "no resident item with an in-world shadow";
				return;
			}
			// The shadow is the overlay's own copy; moving it into reach leaves the resident where it lies.
			const Arm* arm = clone->GetFGArm() ? clone->GetFGArm() : clone->GetBGArm();
			shadow->SetPos(arm ? arm->GetJointPos() : clone->GetPos());
			clone->SetItemInReach(shadow);
			return;
		}

		Arm* arm = nullptr;
		for (Arm* candidate: {clone->GetFGArm(), clone->GetBGArm()}) {
			if (candidate && candidate->GetHeldDevice()) {
				arm = candidate;
				break;
			}
		}
		if (!arm) {
			probe.failure = "the preview clone holds no device";
			return;
		}
		if (probe.mode == 'b' && clone->GetItemInReachUniqueID() != 0) {
			probe.failure = "the clone carries a faithful item in reach that link resolution would put back";
			return;
		}
		HeldDevice* held = arm->GetHeldDevice();
		probe.spawn = held;
		probe.spawnUID = held->GetUniqueID();
		probe.spawnPreset = held->GetPresetName();
		{
			// A named drop under a previewed emitter retires as a ghost that outlives EndSpeculation.
			const uint64_t emitterUID = probe.mode == 'c' ? static_cast<uint64_t>(held->GetUniqueID()) : static_cast<uint64_t>(clone->GetUniqueID());
			SoundSimulationScope emitter(emitterUID, Hash("overlay-link-probe"));
			arm->RemoveAttachable(held, true, false);
		}
		link(clone, held);

		if (probe.mode == 'b') {
			MovableObject* shadowActor = shadowOf(probe.residentActor);
			if (!shadowActor || parts.empty()) {
				probe.failure = shadowActor ? "the clone has no parts" : "no other resident actor with an in-world shadow";
				return;
			}
			clone->SetItemInReach(held);
			link(parts[0], shadowActor);
			probe.shadowLinkPart = parts[0];
			return;
		}

		if (probe.mode == 'c') {
			MovableObject* spawnPart = nullptr;
			if (const HDFirearm* firearm = dynamic_cast<const HDFirearm*>(held); firearm && firearm->GetMagazine()) {
				spawnPart = firearm->GetMagazine();
			} else if (!held->GetAttachableList().empty()) {
				spawnPart = held->GetAttachableList().front();
			}
			const MOSRotating* shadowActor = dynamic_cast<const MOSRotating*>(shadowOf(probe.residentActor));
			MovableObject* shadowPart = shadowActor && !shadowActor->GetAttachableList().empty() ? shadowActor->GetAttachableList().front() : nullptr;
			probe.residentPart = shadowPart ? const_cast<Actor*>(probe.residentActor)->FindPartByUniqueID(shadowPart->GetUniqueID()) : nullptr;
			// Wounds as a hit makes them: copies of a part's break wound, which carry no script and so take no Lua state.
			const AEmitter* woundPreset = nullptr;
			for (const MovableObject* part: parts) {
				const Attachable* attachable = dynamic_cast<const Attachable*>(part);
				if (attachable && attachable->GetBreakWound() && static_cast<const MovableObject*>(attachable->GetBreakWound())->m_AllLoadedScripts.empty()) {
					woundPreset = attachable->GetBreakWound();
					break;
				}
			}
			if (parts.size() < 3 || !spawnPart || !probe.residentPart || !woundPreset) {
				probe.failure = parts.size() < 3 ? "the clone has fewer than three parts" : !spawnPart ? "the dropped device has no part" : !probe.residentPart ? "no resident part behind a shadow part" : "no unscripted wound preset on the clone";
				return;
			}
			AEmitter* spawnWound = dynamic_cast<AEmitter*>(woundPreset->Clone());
			AEmitter* cloneWound = dynamic_cast<AEmitter*>(woundPreset->Clone());
			if (!spawnWound || !cloneWound) {
				delete spawnWound;
				delete cloneWound;
				probe.failure = "the wound preset did not clone";
				return;
			}
			held->AddWound(spawnWound, Vector(), false);
			clone->AddWound(cloneWound, Vector(), false);
			link(parts[0], spawnPart);
			link(parts[1], spawnWound);
			link(parts[2], shadowPart);
			link(cloneWound, held);
			probe.spawnPart = spawnPart;
			probe.spawnWound = spawnWound;
			probe.cloneWound = cloneWound;
			probe.spawnPartLinkPart = parts[0];
			probe.spawnWoundLinkPart = parts[1];
			probe.shadowPartLinkPart = parts[2];
			return;
		}
		probe.failure = std::string("unknown probe mode '") + probe.mode + "'";
	}
} // namespace RTE
