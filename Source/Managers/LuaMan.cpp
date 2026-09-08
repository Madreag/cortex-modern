#include "LuaMan.h"

#include "LuabindObjectWrapper.h"
#include "LuaBindingRegisterDefinitions.h"
#include "ThreadMan.h"
#include "System.h"
#include "MetricsCollector.h"
#include "SimChecksum.h"
#include "RTETools.h"
#include "LuaThreadCodec.h"
#include "ContentFile.h"
#include "MovableMan.h"
#include "ActivityMan.h"
#include "GAScripted.h"
#include "SceneMan.h"
#include "Scene.h"
#include "Entity.h"
#include "Attachable.h"
#include "AEmitter.h"
#include "AHuman.h"
#include "ACrab.h"
#include "ACraft.h"
#include "Gib.h"
#include "DataModule.h"
#include "PathFinder.h"
#include "SoundContainer.h"
#include "SoundSet.h"
#include "AudioMan.h"
#include "MusicMan.h"
#include "GUISound.h"
#include "UInputMan.h"
#include "PostProcessMan.h"
#include "FrameMan.h"
#include "BitmapCheckpoint.h"
#include "BuyMenuGUI.h"
#include "SceneEditorGUI.h"
#include "GUIBanner.h"
#include "GUICheckpoint.h"
#include "SLBackground.h"
#include "Writer.h"
#include "Reader.h"
#include "Base64/base64.h"

#include "luabind/detail/object_rep.hpp"

#include <cmath>
#include <cstring>
#include <future>
#include <map>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "tracy/Tracy.hpp"
#include "tracy/TracyLua.hpp"

using namespace RTE;

struct RTE::LuaPathCallbackContext {
	struct Request {
		lua_State* state;
		int id;
		Scene* scene;
		Vector start;
		Vector end;
		float jumpHeight;
		float digStrength;
		Activity::Teams team;
		bool submitted;
	};
	struct Callback {
		lua_State* state;
		int id;
		uint64_t order;
		std::shared_ptr<const PathRequest> result;
	};
	std::mutex mutex;
	std::unordered_map<lua_State*, int> nextId;
	uint64_t nextOrder = 0;
	bool orderPending = false;
	std::vector<Callback> callbacks;
	std::vector<Callback> incoming;
	std::vector<Request> pending;
};

const std::unordered_set<std::string> LuaMan::c_FileAccessModes = {"r", "r+", "w", "w+", "a", "a+", "rt", "wt"};

namespace {
	// os.time / os.clock replacements returning sim-tick seconds instead of the wall clock.
	int det_os_time(lua_State* L) {
		lua_pushnumber(L, static_cast<lua_Number>(g_TimerMan.GetSimUpdateCount()) / 60.0);
		return 1;
	}

	int det_os_clock(lua_State* L) {
		lua_pushnumber(L, static_cast<lua_Number>(g_TimerMan.GetSimUpdateCount()) / 60.0);
		return 1;
	}

	void RegisterDeterministicOsStubs(lua_State* L) {
		lua_getglobal(L, "os");
		lua_pushcfunction(L, det_os_time);
		lua_setfield(L, -2, "time");
		lua_pushcfunction(L, det_os_clock);
		lua_setfield(L, -2, "clock");
		lua_pop(L, 1);
	}

	// Route math.atan/atan2 through the cross-platform poly — AI ballistics aim through these and the platform libm atan2 diverges cross-toolchain.
	int det_math_atan(lua_State* L) {
		lua_pushnumber(L, DeterministicAtan2(luaL_checknumber(L, 1), luaL_optnumber(L, 2, 1.0)));
		return 1;
	}

	int det_math_atan2(lua_State* L) {
		lua_pushnumber(L, DeterministicAtan2(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
		return 1;
	}

	// Route math.exp through the cross-platform poly — combat AI aim-skill uses it and the platform libm exp diverges cross-toolchain.
	int det_math_exp(lua_State* L) {
		lua_pushnumber(L, DeterministicExp(luaL_checknumber(L, 1)));
		return 1;
	}

	// Route math.pow through the cross-platform poly — combat AI aim-skill (pow feeds exp) and ballistics use it; the platform libm pow diverges cross-toolchain.
	int det_math_pow(lua_State* L) {
		lua_pushnumber(L, DeterministicPow(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
		return 1;
	}

	// Route the remaining transcendentals through the polys — a LuaJIT/libm probe found sin/cos/tan/asin/acos/tanh/sinh/cosh all diverge cross-toolchain (only log was identical).
	int det_math_sin(lua_State* L) {
		lua_pushnumber(L, DeterministicSin(luaL_checknumber(L, 1)));
		return 1;
	}

	int det_math_cos(lua_State* L) {
		lua_pushnumber(L, DeterministicCos(luaL_checknumber(L, 1)));
		return 1;
	}

	int det_math_tan(lua_State* L) {
		const double x = luaL_checknumber(L, 1);
		lua_pushnumber(L, DeterministicSin(x) / DeterministicCos(x));
		return 1;
	}

	int det_math_asin(lua_State* L) {
		const double x = luaL_checknumber(L, 1);
		const double t = 1.0 - x * x;
		lua_pushnumber(L, DeterministicAtan2(x, std::sqrt(t < 0.0 ? 0.0 : t)));
		return 1;
	}

	int det_math_acos(lua_State* L) {
		const double x = luaL_checknumber(L, 1);
		const double t = 1.0 - x * x;
		lua_pushnumber(L, DeterministicAtan2(std::sqrt(t < 0.0 ? 0.0 : t), x));
		return 1;
	}

	int det_math_tanh(lua_State* L) {
		const double x = luaL_checknumber(L, 1);
		const double e = DeterministicExp(-2.0 * (x < 0.0 ? -x : x));
		const double t = (1.0 - e) / (1.0 + e);
		lua_pushnumber(L, x < 0.0 ? -t : t);
		return 1;
	}

	int det_math_sinh(lua_State* L) {
		const double x = luaL_checknumber(L, 1);
		lua_pushnumber(L, (DeterministicExp(x) - DeterministicExp(-x)) * 0.5);
		return 1;
	}

	int det_math_cosh(lua_State* L) {
		const double x = luaL_checknumber(L, 1);
		lua_pushnumber(L, (DeterministicExp(x) + DeterministicExp(-x)) * 0.5);
		return 1;
	}

	void RegisterDeterministicMathOverrides(lua_State* L) {
		lua_getglobal(L, "math");
		lua_pushcfunction(L, det_math_atan);
		lua_setfield(L, -2, "atan");
		lua_pushcfunction(L, det_math_atan2);
		lua_setfield(L, -2, "atan2");
		lua_pushcfunction(L, det_math_exp);
		lua_setfield(L, -2, "exp");
		lua_pushcfunction(L, det_math_pow);
		lua_setfield(L, -2, "pow");
		lua_pushcfunction(L, det_math_sin);
		lua_setfield(L, -2, "sin");
		lua_pushcfunction(L, det_math_cos);
		lua_setfield(L, -2, "cos");
		lua_pushcfunction(L, det_math_tan);
		lua_setfield(L, -2, "tan");
		lua_pushcfunction(L, det_math_asin);
		lua_setfield(L, -2, "asin");
		lua_pushcfunction(L, det_math_acos);
		lua_setfield(L, -2, "acos");
		lua_pushcfunction(L, det_math_tanh);
		lua_setfield(L, -2, "tanh");
		lua_pushcfunction(L, det_math_sinh);
		lua_setfield(L, -2, "sinh");
		lua_pushcfunction(L, det_math_cosh);
		lua_setfield(L, -2, "cosh");
		lua_pop(L, 1);
	}
} // namespace

// Per-MO RNG generator and its Lua-side override pointer; a threaded per-MO hook
// points both sim-RNG overrides here so its draws depend only on the MO and tick.
thread_local RandomGenerator s_workerMORNG;
thread_local RandomGenerator* s_luaRNGOverride = nullptr;

// splitmix64-style mix so an MO's successive hooks and ticks don't correlate.
static uint64_t DeriveMORNGSeed(long uniqueID, uint64_t tick, uint64_t phase) {
	uint64_t h = static_cast<uint64_t>(uniqueID) * 0x9E3779B97F4A7C15ULL;
	h = (h ^ tick) * 0xBF58476D1CE4E5B9ULL;
	h = (h ^ phase) * 0x94D049BB133111EBULL;
	h ^= h >> 31;
	return h;
}

DeterministicMORNGScope::DeterministicMORNGScope(long uniqueID, uint64_t phase, bool enabled) :
    m_Installed(enabled), m_PrevSimOverride(nullptr), m_PrevLuaOverride(nullptr) {
	if (!enabled) {
		return;
	}
	s_workerMORNG.Seed(DeriveMORNGSeed(uniqueID, g_TimerMan.GetSimUpdateCount(), phase));
	m_PrevSimOverride = t_simRNGOverride;
	m_PrevLuaOverride = s_luaRNGOverride;
	t_simRNGOverride = &s_workerMORNG;
	s_luaRNGOverride = &s_workerMORNG;
}

DeterministicMORNGScope::~DeterministicMORNGScope() {
	if (m_Installed) {
		t_simRNGOverride = m_PrevSimOverride;
		s_luaRNGOverride = m_PrevLuaOverride;
	}
}

std::string LuaStateWrapper::DescribeScriptObjectIdentity(long uniqueID) {
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);
	lua_getglobal(m_State, "_ScriptedObjects");
	std::string identity = "-";
	if (lua_istable(m_State, -1)) {
		lua_pushstring(m_State, std::to_string(uniqueID).c_str());
		lua_gettable(m_State, -2);
		if (const void* address = lua_topointer(m_State, -1)) {
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%p", address);
			identity = buffer;
		}
		lua_pop(m_State, 1);
	}
	lua_pop(m_State, 1);
	return identity;
}

namespace {
	// Pushes the table luabind keeps for the fields set on _ScriptedObjects[uid], or nil when the object never set one.
	void PushScriptObjectInstanceTable(lua_State* L, long uniqueID) {
		lua_getglobal(L, "_ScriptedObjects");
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			lua_pushnil(L);
			return;
		}
		lua_pushstring(L, std::to_string(uniqueID).c_str());
		lua_gettable(L, -2);
		lua_remove(L, -2);
		if (!lua_isuserdata(L, -1)) {
			lua_pop(L, 1);
			lua_pushnil(L);
			return;
		}
		luabind::detail::object_rep* rep = static_cast<luabind::detail::object_rep*>(lua_touserdata(L, -1));
		lua_pop(L, 1);
		if (rep && rep->get_lua_table().is_valid()) {
			rep->get_lua_table().get(L);
		} else {
			lua_pushnil(L);
		}
	}

	// Tables copy by structure (cycles kept), entities by unique id (looked up in the live world at restore), the rest by value or reference.
	constexpr const char* c_ScriptGraphHelper = R"lua(
-- The script state of one Lua VM as one graph: every scripted object's instance table and every
-- script-made global, with identity kept for shared tables, closures rebuilt from their bytecode
-- with their captured variables and the cells they share, resident functions and engine objects
-- by name, entities by unique id. serialize/deserialize round-trip the text; the text is canonical,
-- so serializing again after a restore reproduces it byte for byte.
_ScriptGraph = _ScriptGraph or {}
local Graph = _ScriptGraph

-- An older graph can hold a yield that ran through a compiled trace: three continuation slots
-- below the callee. The runtime only ever rebuilds the plain call the trace stitched.
function Graph.canonicalThread(desc)
	local stitched = {}
	for index, name in pairs(desc.conts) do
		if name == "stitch" then stitched[#stitched + 1] = index end
	end
	if #stitched == 0 then return desc end
	table.sort(stitched)
	for _, cont in ipairs(stitched) do
		local pc, link = desc.links[cont + 1], desc.links[cont + 3]
		if not (pc and pc.pcslot and link and link.ftsz and desc.slots[cont + 2] ~= nil) then return nil, "malformed stitch continuation" end
	end
	local function dropped(slot)
		for _, cont in ipairs(stitched) do if slot >= cont - 1 and slot <= cont + 1 then return true end end
		return false
	end
	local function remap(slot)
		local shift = 0
		for _, cont in ipairs(stitched) do if slot > cont + 1 then shift = shift + 3 end end
		return slot - shift
	end
	local out = { status = desc.status, first = desc.first, base = remap(desc.base), top = remap(desc.top), slots = {}, links = {}, conts = {} }
	for index, value in pairs(desc.slots) do
		if not dropped(index) then out.slots[remap(index)] = value end
	end
	for index, link in pairs(desc.links) do
		local callee = nil
		for _, cont in ipairs(stitched) do if index == cont + 3 then callee = cont end end
		if callee then
			local pc = desc.links[callee + 1]
			out.links[remap(index)] = { pcslot = remap(pc.pcslot), pos = pc.pos }
		elseif not dropped(index) then
			out.links[remap(index)] = link.pcslot and { pcslot = remap(link.pcslot), pos = link.pos } or { ftsz = link.ftsz }
		end
	end
	for index, name in pairs(desc.conts) do
		if name ~= "stitch" then out.conts[remap(index)] = name end
	end
	return out
end
local SKIP_GLOBALS = { _ScriptedObjects = true, _ScriptGraph = true, _ScriptGraphBaseline = true, _ScriptGraphNative = true, _ScriptGraphProgress = true, _G = true, _ScriptFieldsStash = true }
local _G, type, pairs, ipairs, next, rawget, rawset, rawequal = _G, type, pairs, ipairs, next, rawget, rawset, rawequal
local tonumber, tostring, error, pcall, xpcall, getfenv, setfenv, loadstring = tonumber, tostring, error, pcall, xpcall, getfenv, setfenv, loadstring
local function libraryCopy(source)
	local copy = {}
	for key, value in pairs(source) do copy[key] = value end
	return copy
end
local math, string, table, debug, coroutine = libraryCopy(math), libraryCopy(string), libraryCopy(table), libraryCopy(debug), libraryCopy(coroutine)
local getmetatable = debug.getmetatable
local function setmetatable(object, meta) debug.setmetatable(object, meta); return object end
local keyLabels = setmetatable({}, { __mode = "k" })
local liveOwned = setmetatable({}, { __mode = "v" })
local lastObjects = setmetatable({}, { __mode = "v" })
local heldObjects
function Graph.restoreLegacy(text)
	local chunk, message = loadstring("return " .. text)
	if not chunk then error(message) end
	setfenv(chunk, {})
	local root = chunk()
	if type(root) ~= "table" then error("legacy script fields are not a table") end
	local ids, seen = {}, {}
	local function index(value)
		if type(value) ~= "table" or seen[value] then return end
		seen[value] = true
		if value.__scriptFieldsId then ids[value.__scriptFieldsId] = value end
		for key, item in pairs(value) do index(key); index(item) end
	end
	index(root)
	seen = {}
	local function restore(value)
		if type(value) ~= "table" then return value end
		if value.__scriptFieldsRef then return restore(ids[value.__scriptFieldsRef]) end
		if value.__scriptFieldsModule then return require(value.__scriptFieldsModule) end
		if seen[value] then return seen[value] end
		local copy
		if value.__scriptFieldsEntity then
			local mo = MovableMan:FindObjectByUniqueID(value.__scriptFieldsEntity)
			local cast = _G["To" .. (value.__scriptFieldsClass or "MovableObject")]
			copy = mo and (cast and cast(mo) or mo)
		elseif value.__scriptFieldsVector then
			copy = Vector(value.x, value.y)
		elseif value.__scriptFieldsTimer then
			copy = Timer()
			copy.StartSimTimeTicks = value.simStart
			copy.SimTimeLimitTicks = value.simLimit
			copy.StartRealTimeTicks = value.realStart
			copy.RealTimeLimitTicks = value.realLimit
		else
			copy = {}
			seen[value] = copy
			for key, item in pairs(value) do
				if key ~= "__scriptFieldsId" and key ~= "__scriptFieldsMeta" and key ~= "__scriptFieldsMetaIndex" then
					copy[restore(key)] = restore(item)
				end
			end
			local meta = value.__scriptFieldsMeta and _G[value.__scriptFieldsMeta]
			local metaIndex = value.__scriptFieldsMetaIndex and _G[value.__scriptFieldsMetaIndex]
			if type(meta) == "table" then setmetatable(copy, meta)
			elseif type(metaIndex) == "table" then setmetatable(copy, { __index = metaIndex }) end
		end
		seen[value] = copy
		return copy
	end
	return restore(root)
end

function Graph.stashObjects()
	heldObjects = {}
	for id, value in pairs(lastObjects) do heldObjects[id] = value end
end
function Graph.releaseObjects() heldObjects = nil end
local nativeClosures = {
	wrap = function() return coroutine.wrap(function() end) end,
	gmatch = function() return string.gmatch("", ".") end
}
local nativePrototypes = {}
for name, create in pairs(nativeClosures) do nativePrototypes[name] = create() end

local function numberText(value)
	if value ~= value then return "nan" end
	if value == math.huge then return "inf" end
	if value == -math.huge then return "-inf" end
	if value == 0 then return 1 / value < 0 and "-0" or "0" end
	if value == math.floor(value) and math.abs(value) < 2^53 then return string.format("%d", value) end
	return string.format("%.17g", value)
end

local function stringToken(s)
	return "s" .. #s .. ":" .. s
end

local function carriesKey(saved, key)
	return not saved.global or type(key) ~= "string" or key == "_G"
end

function Graph.captureBaseline()
	local baseline = { globals = {}, loaded = {}, values = {}, symbols = {}, paths = {}, tables = {}, globalTable = _G }
	local seen = {}
	local function remember(value, name)
		local kind = type(value)
		if (kind ~= "function" and kind ~= "table" and kind ~= "userdata") or value == _G or seen[value] then return end
		seen[value] = true
		baseline.symbols[name] = value
		baseline.paths[value] = { "_ScriptGraphBaseline", "symbols", name }
		local members = kind == "table" and value or (kind == "userdata" and _ScriptGraphMembers(value))
		if type(members) == "table" then
			local keys, entries = {}, {}
			for key, item in pairs(members) do
				entries[key] = item
				if type(key) == "string" or type(key) == "number" then keys[#keys + 1] = key end
			end
			if kind == "table" then baseline.tables[#baseline.tables + 1] = { object = value, entries = entries, meta = getmetatable(value) } end
			table.sort(keys, function(a, b) if type(a) ~= type(b) then return type(a) < type(b) end return a < b end)
			for _, key in ipairs(keys) do remember(members[key], name .. "/" .. type(key) .. stringToken(tostring(key))) end
		elseif kind == "function" and debug.getinfo(value, "S").what ~= "C" then
			for index = 1, debug.getinfo(value, "u").nups do
				local _, item = debug.getupvalue(value, index)
				remember(item, name .. "/upvalue" .. index)
			end
		end
	end
	local names = {}
	for name in pairs(_G) do if type(name) == "string" then names[#names + 1] = name end end
	table.sort(names)
	for _, name in ipairs(names) do
		baseline.globals[name] = true
		if not SKIP_GLOBALS[name] and string.sub(name, 1, 12) ~= "_ScriptGraph" then
			baseline.values[name] = _G[name]
			remember(_G[name], stringToken(name))
		end
	end
	for name in pairs(package.loaded) do baseline.loaded[name] = true end
	baseline.paths[_G] = { "_ScriptGraphBaseline", "globalTable" }
	baseline.tables[#baseline.tables + 1] = { object = _G, entries = { _G = rawget(_G, "_G") }, meta = getmetatable(_G), global = true }
	_ScriptGraphBaseline = baseline
end
)lua"
	    R"lua(
-- Every function, table and userdata reachable by name from the globals or the loaded modules, keyed by value.
local function buildPaths(baseline)
	local paths, engine = {}, {}
	for value, path in pairs(baseline.paths or {}) do paths[value], engine[value] = path, true end
	paths[_G], engine[_G] = { "_ScriptGraphBaseline", "globalTable" }, true
	local function note(value, segments, isEngine)
		local kind = type(value)
		if (kind == "function" or kind == "table" or kind == "userdata") and paths[value] == nil then
			paths[value] = segments
			engine[value] = (baseline.paths and baseline.paths[value] ~= nil) or (not baseline.paths and isEngine) or nil
		end
	end
	local names = {}
	for name in pairs(_G) do if type(name) == "string" then names[#names + 1] = name end end
	table.sort(names)
	for _, name in ipairs(names) do
		local value = _G[name]
		local isEngine = baseline.globals[name] == true
		if not SKIP_GLOBALS[name] then note(value, { name }, isEngine) end
	end
	for _, name in ipairs(names) do
		local value = _G[name]
		local members = type(value) == "table" and value or (_ScriptGraphMembers and _ScriptGraphMembers(value))
		if type(members) == "table" and not SKIP_GLOBALS[name] and value ~= _G then
			local isEngine = baseline.globals[name] == true
			local keys = {}
			for key in pairs(members) do if type(key) == "string" then keys[#keys + 1] = key end end
			table.sort(keys)
			for _, key in ipairs(keys) do note(members[key], { name, key }, isEngine) end
		end
	end
	if type(package) == "table" and type(package.loaded) == "table" then
		local names2 = {}
		for name in pairs(package.loaded) do if type(name) == "string" then names2[#names2 + 1] = name end end
		table.sort(names2)
		for _, name in ipairs(names2) do
			local module = package.loaded[name]
			local isEngine = baseline.loaded[name] == true
			if paths[module] == nil then note(module, { "package", "loaded", name }, isEngine) end
			if type(module) == "table" and module ~= _G then
				local keys = {}
				for key in pairs(module) do if type(key) == "string" then keys[#keys + 1] = key end end
				table.sort(keys)
				for _, key in ipairs(keys) do
					if paths[module[key]] == nil then note(module[key], { "package", "loaded", name, key }, isEngine) end
				end
			end
		end
	end
	return paths, engine
end

local function pathToken(segments)
	local parts = { "g", #segments, ";" }
	for _, segment in ipairs(segments) do parts[#parts + 1] = stringToken(segment) end
	return table.concat(parts)
end

local function keyOrder(key, ctx)
	local kind = type(key)
	if kind == "number" then return 0, key
	elseif kind == "string" then return 1, key
	elseif kind == "boolean" then return 2, key and 1 or 0
	else
		local id = ctx.ids[key]
		if id then return 3, id end
		if ctx.paths[key] then return 4, pathToken(ctx.paths[key]) end
		if keyLabels[key] then return 5, keyLabels[key] end
		return 6, _ScriptGraphObjectAddress(key)
	end
end

local function sortedKeys(t, ctx)
	local keys = {}
	for key in pairs(t) do keys[#keys + 1] = key end
	table.sort(keys, function(a, b)
		local ra, va = keyOrder(a, ctx)
		local rb, vb = keyOrder(b, ctx)
		if ra ~= rb then return ra < rb end
		return va < vb
	end)
	return keys
end

local visit

local function problem(ctx, message)
	ctx.problems[#ctx.problems + 1] = message .. " at " .. (ctx.location or "graph")
end

local function visitAt(value, ctx, location)
	local previous = ctx.location
	ctx.location = location
	local token = visit(value, ctx)
	ctx.location = previous
	return token
end

local function newId(ctx)
	ctx.count = ctx.count + 1
	return ctx.count
end

-- Owned values are nodes, so two fields holding one Vector share it again after the restore.
local function userdataNode(value, ctx, payload)
	local id = newId(ctx)
	ctx.ids[value] = id
	local instance = _ScriptGraphInstance(value)
	ctx.nodes[id] = "U" .. id .. ";" .. payload .. "I" .. visit(instance, ctx)
	return "#" .. id .. ";"
end

local function visitUserdata(value, ctx)
	local id = ctx.ids[value]
	if id then return "#" .. id .. ";" end
	local native = _ScriptGraphNative and { _ScriptGraphNative(value, ctx.paths[value] ~= nil) } or {}
	local kind = native[1]
	if kind == "copy" and native[6] then ctx.ownedPointers[native[6]] = value end
	if kind == "area-ref" or (kind == "copy" and native[2] == "Area") then
		for index, address in ipairs(_ScriptGraphAreaBoxes(value)) do ctx.areaBoxes[address] = { owner = value, index = index } end
	end
	if kind == "vector" then
		return userdataNode(value, ctx, "v" .. numberText(value.X) .. "," .. numberText(value.Y) .. ";")
	elseif kind == "alarm" then
		return userdataNode(value, ctx, "c" .. "n" .. numberText(value.ScenePos.X) .. ";n" .. numberText(value.ScenePos.Y) .. ";n" .. value.Team .. ";n" .. numberText(value.Range) .. ";")
	elseif kind == "module-ref" then
		return userdataNode(value, ctx, "d" .. stringToken(native[2]))
	elseif kind == "material-ref" then
		return userdataNode(value, ctx, "M" .. native[2] .. ";")
	elseif kind == "path-request" then
		local fields = {}
		for _, number in ipairs(native[2]) do fields[#fields + 1] = "n" .. numberText(number) .. ";" end
		return userdataNode(value, ctx, "P" .. #fields .. ";" .. table.concat(fields))
	elseif kind == "timer" then
		return userdataNode(value, ctx, "m" .. numberText(value.StartSimTimeTicks) .. "," .. numberText(value.SimTimeLimitTicks) .. "," .. numberText(value.StartRealTimeTicks) .. "," .. numberText(value.RealTimeLimitTicks) .. ";")
	elseif kind == "vector-ref" then
		return userdataNode(value, ctx, "w" .. numberText(native[2]) .. ":" .. stringToken(native[3]))
	elseif kind == "controller-ref" then
		return userdataNode(value, ctx, "q" .. numberText(native[2]) .. ";")
	elseif kind == "controller-value" or kind == "owner-ref" then
		local id = newId(ctx)
		ctx.ids[value] = id
		local payload
		if kind == "controller-value" then payload = "Q" .. stringToken(native[2]) .. visit(native[3], ctx)
		else
			payload = (native[6] and "Y" or "x") .. visit(native[2], ctx) .. stringToken(native[3]) .. "n" .. native[4] .. ";" .. (native[5] and "t;" or "f;")
			if native[6] then payload = payload .. stringToken(native[6]) .. stringToken(native[7]) end
		end
		ctx.nodes[id] = "U" .. id .. ";" .. payload .. "I" .. visit(_ScriptGraphInstance(value), ctx)
		return "#" .. id .. ";"
	elseif kind == "gib-ref" then
		local owner, index = _ScriptGraphGibOwner(value)
		if not owner then problem(ctx, "a Gib whose owner is missing") return "z;" end
		local id = newId(ctx)
		ctx.ids[value] = id
		ctx.nodes[id] = "U" .. id .. ";i" .. visit(owner, ctx) .. "n" .. index .. ";I" .. visit(_ScriptGraphInstance(value), ctx)
		return "#" .. id .. ";"
	elseif kind == "soundset-ref" then
		local owner, index = _ScriptGraphSoundSetOwner(value)
		if not owner then problem(ctx, "a SoundSet whose owner is missing") return "z;" end
		local id = newId(ctx)
		ctx.ids[value] = id
		ctx.nodes[id] = "U" .. id .. ";j" .. visit(owner, ctx) .. "n" .. index .. ";I" .. visit(_ScriptGraphInstance(value), ctx)
		return "#" .. id .. ";"
	elseif kind == "limb-ref" then
		local owner, index = _ScriptGraphLimbOwner(value)
		if not owner then problem(ctx, "a LimbPath whose owning actor is missing") return "z;" end
		local id = newId(ctx)
		ctx.ids[value] = id
		ctx.nodes[id] = "U" .. id .. ";l" .. visit(owner, ctx) .. "n" .. index .. ";I" .. visit(_ScriptGraphInstance(value), ctx)
		return "#" .. id .. ";"
	elseif kind == "entity" then
		return userdataNode(value, ctx, "e" .. numberText(native[2]) .. ":" .. stringToken(native[3]))
	elseif kind == "activity" then
		return userdataNode(value, ctx, "A;")
	elseif kind == "global-script" then
		return userdataNode(value, ctx, "G" .. native[2] .. ";")
	elseif kind == "scene" then
		return userdataNode(value, ctx, "S;")
	elseif kind == "area-ref" then
		return userdataNode(value, ctx, "a" .. stringToken(native[2]))
	elseif kind == "box-ref" then
		local id = newId(ctx)
		ctx.ids[value] = id
		local instance = visit(_ScriptGraphInstance(value), ctx)
		ctx.boxRefs[#ctx.boxRefs + 1] = { value = value, id = id, address = native[2], constant = native[3], instance = instance }
		return "#" .. id .. ";"
	elseif kind == "preset" then
		return userdataNode(value, ctx, "p" .. stringToken(native[2]) .. stringToken(native[3]) .. stringToken(native[4]))
	elseif kind == "named" then
		return userdataNode(value, ctx, pathToken(ctx.paths[value]))
	elseif kind == "copy" then
		local ini, message = _ScriptGraphNativeSave(value)
		if not ini then
			problem(ctx, "a " .. tostring(native[2]) .. " could not be written: " .. tostring(message))
			return "z;"
		end
		local header = "o"
		if native[5] then
			header = "O" .. numberText(native[5]) .. ";"
			liveOwned[native[5]] = value
		end
		return userdataNode(value, ctx, header .. stringToken(native[2]) .. stringToken(native[3]) .. stringToken(native[4]) .. stringToken(ini))
	elseif kind == "invalid" then
		problem(ctx, "a reference to a " .. tostring(native[2]) .. " that no longer exists")
		return "z;"
	elseif kind == "vector-ref-unresolved" or kind == "timer-ref" then
		local propertyOwner, property, isConst = _ScriptGraphPropertyOwner(value)
		if propertyOwner then
			local id = newId(ctx)
			ctx.ids[value] = id
			ctx.nodes[id] = "U" .. id .. ";h" .. visit(propertyOwner, ctx) .. stringToken(property) .. (isConst and "t;" or "f;") .. "I" .. visit(_ScriptGraphInstance(value), ctx)
			return "#" .. id .. ";"
		end
		local owner, index, constant = _ScriptGraphLimbVectorOwner(value)
		if owner then
			local id = newId(ctx)
			ctx.ids[value] = id
			ctx.nodes[id] = "U" .. id .. ";k" .. visit(owner, ctx) .. "n" .. index .. ";" .. (constant and "t;" or "f;") .. "I" .. visit(_ScriptGraphInstance(value), ctx)
			return "#" .. id .. ";"
		end
		problem(ctx, "a " .. kind .. " into an engine object that no known property exposes")
		return "z;"
	end
	local path = ctx.paths[value]
	if path and ctx.engine[value] then return userdataNode(value, ctx, pathToken(path)) end
	problem(ctx, "an unsupported userdata (" .. tostring(native[2] or type(value)) .. ")")
	return "z;"
end

)lua"
	    R"lua(
local function visitFunction(value, ctx)
	local id = ctx.ids[value]
	if id then return "#" .. id .. ";" end
	local path = ctx.paths[value]
	local info = debug.getinfo(value, "Su")
	if info.what == "C" then
		local range = _ScriptGraphIteratorSnapshot(value)
		if range then
			id = newId(ctx)
			ctx.ids[value] = id
			local fields = { "J" .. id .. ";" .. (range.owned and "o" or "r"), visit(range.owner, ctx), "n" .. range.first .. ";" }
			if range.owned then
				fields[#fields + 1] = "u" .. range.count .. ";"
				for index = 1, range.count do fields[#fields + 1] = visit(range.values[index], ctx) end
			else
				fields[#fields + 1] = "n" .. range.last .. ";" .. visit(range.creator, ctx)
				fields[#fields + 1] = "u" .. #(range.args or {}) .. ";"
				for _, argument in ipairs(range.args or {}) do fields[#fields + 1] = visit(argument, ctx) end
			end
			ctx.nodes[id] = table.concat(fields)
			return "#" .. id .. ";"
		end
		for name, prototype in pairs(nativePrototypes) do
			if _ScriptGraphSameNativeFunction(value, prototype) then
				id = newId(ctx)
				ctx.ids[value] = id
				local upvalues = {}
				for i = 1, info.nups do
					local _, upvalue = debug.getupvalue(value, i)
					if name == "gmatch" and i == 3 then upvalue = _ScriptGraphGmatchPosition(value) end
					upvalues[#upvalues + 1] = visitAt(upvalue, ctx, (ctx.location or "function") .. ".native_upvalue[" .. i .. "]")
				end
				ctx.nodes[id] = "B" .. id .. ";" .. stringToken(name) .. "u" .. #upvalues .. ";" .. table.concat(upvalues)
				return "#" .. id .. ";"
			end
		end
		if path then return pathToken(path) end
		problem(ctx, "a native function with no global name")
		return "z;"
	end
	local ok, code = pcall(string.dump, value, "d")
	if not ok then
		problem(ctx, "a function could not be dumped: " .. tostring(code))
		return "z;"
	end
	id = newId(ctx)
	ctx.ids[value] = id
	local parts = {}
	if path then
		parts[#parts + 1] = "R" .. pathToken(path)
	end
	parts[#parts + 1] = "D" .. stringToken(code)
	local env = getfenv(value)
	parts[#parts + 1] = "E" .. ((env == _G or env == nil) and "z;" or visit(env, ctx))
	local cells = {}
	for i = 1, info.nups do
		local name, upvalue = debug.getupvalue(value, i)
		if name == nil then break end
		local cellKey = debug.upvalueid(value, i)
		local cellId = ctx.cells[cellKey]
		if not cellId then
			cellId = newId(ctx)
			ctx.cells[cellKey] = cellId
			local open = ctx.openUpvalues[cellKey]
			if open then
				ctx.nodes[cellId] = "C" .. cellId .. ";O" .. visit(open.thread, ctx) .. "n" .. open.slot .. ";"
			else
				ctx.nodes[cellId] = "C" .. cellId .. ";" .. visitAt(upvalue, ctx, (ctx.location or "function") .. ".upvalue[" .. name .. "]")
			end
		end
		cells[#cells + 1] = "c" .. cellId .. ";"
	end
	parts[#parts + 1] = "u" .. #cells .. ";" .. table.concat(cells)
	ctx.nodes[id] = "F" .. id .. ";" .. table.concat(parts)
	return "#" .. id .. ";"
end

local function visitTable(value, ctx)
	local id = ctx.ids[value]
	if id then return "#" .. id .. ";" end
	local path = ctx.paths[value]
	if path and ctx.engine[value] then return pathToken(path) end
	id = newId(ctx)
	ctx.ids[value] = id
	local parts = { path and ("P" .. pathToken(path)) or "P-;" }
	local meta = getmetatable(value)
	parts[#parts + 1] = "M" .. (type(meta) == "table" and visit(meta, ctx) or "z;")
	local keys = sortedKeys(value, ctx)
	local body = {}
	for _, key in ipairs(keys) do
		body[#body + 1] = visit(key, ctx)
		local label = (type(key) == "string" or type(key) == "number" or type(key) == "boolean") and tostring(key) or type(key)
		body[#body + 1] = visitAt(rawget(value, key), ctx, (ctx.location or "table") .. "[" .. label .. "]")
	end
	parts[#parts + 1] = "k" .. #keys .. ";" .. table.concat(body)
	ctx.nodes[id] = "T" .. id .. ";" .. table.concat(parts)
	return "#" .. id .. ";"
end

local function visitThread(value, ctx)
	local id = ctx.ids[value]
	if id then return "#" .. id .. ";" end
	local desc, message = nil, "no coroutine codec"
	if _ScriptGraphThreadCapture then desc, message = _ScriptGraphThreadCapture(value) end
	if not desc then
		problem(ctx, "a coroutine cannot be carried: " .. tostring(message))
		return "z;"
	end
	id = newId(ctx)
	ctx.ids[value] = id
	local letter = desc.status == "suspended" and "s" or (desc.status == "notstarted" and "n" or "d")
	local entries = {}
	for i = desc.first, desc.top - 1 do
		local link, cont = desc.links[i], desc.conts[i]
		if link and link.pcslot then entries[#entries + 1] = "P" .. link.pcslot .. ":" .. link.pos .. ";"
		elseif link then entries[#entries + 1] = "L" .. link.ftsz .. ";"
		elseif cont then entries[#entries + 1] = "K" .. stringToken(cont)
		else entries[#entries + 1] = "V" .. visitAt(desc.slots[i], ctx, (ctx.location or "coroutine") .. ".slot[" .. i .. "]") end
	end
	ctx.nodes[id] = "H" .. id .. ";" .. letter .. ";" .. desc.first .. ";" .. desc.base .. ";" .. desc.top .. ";" .. table.concat(entries)
	return "#" .. id .. ";"
end

visit = function(value, ctx)
	local kind = type(value)
	if kind == "nil" then return "z;"
	elseif kind == "boolean" then return value and "t;" or "f;"
	elseif kind == "number" then return "n" .. numberText(value) .. ";"
	elseif kind == "string" then return stringToken(value)
	elseif kind == "table" then return visitTable(value, ctx)
	elseif kind == "function" then return visitFunction(value, ctx)
	elseif kind == "userdata" then return visitUserdata(value, ctx)
	elseif kind == "thread" then return visitThread(value, ctx)
	end
	problem(ctx, "an unsupported value (" .. kind .. ")")
	return "z;"
end

-- roots: { [uidString] = instanceTable }. Returns the text and the list of problems (any problem means the capture is unfaithful).
local function serializeGraph(roots)
	local baseline = _ScriptGraphBaseline or { globals = {}, loaded = {} }
	local paths, engine = buildPaths(baseline)
	if _ScriptGraphBeginCapture then _ScriptGraphBeginCapture() end
	local ctx = { ids = {}, cells = {}, nodes = {}, count = 0, problems = {}, paths = paths, engine = engine, areaBoxes = {}, boxRefs = {}, ownedPointers = {}, openUpvalues = _ScriptGraphOpenUpvalues and _ScriptGraphOpenUpvalues() or {} }
	local rootIds = {}
	local uids = {}
	for uid in pairs(roots) do uids[#uids + 1] = uid end
	table.sort(uids, function(a, b) return tonumber(a) < tonumber(b) end)
	for _, uid in ipairs(uids) do
		rootIds[#rootIds + 1] = stringToken(uid) .. visitAt(roots[uid], ctx, "object[" .. uid .. "]")
	end
	local globals = {}
	local names = {}
	local allNames = {}
	for name in pairs(_G) do allNames[name] = true end
	for name in pairs(baseline.values or {}) do allNames[name] = true end
	for name in pairs(allNames) do
		if type(name) == "string" and not SKIP_GLOBALS[name] and (string.sub(name, 1, 12) ~= "_ScriptGraph" or name == "_ScriptGraphCallbacks") and (not baseline.globals[name] or (baseline.values and not rawequal(rawget(_G, name), baseline.values[name]))) then names[#names + 1] = name end
	end
	table.sort(names)
	for _, name in ipairs(names) do
		globals[#globals + 1] = stringToken(name) .. visitAt(rawget(_G, name), ctx, "global[" .. name .. "]")
	end
	local loaded = {}
	if type(package) == "table" and type(package.loaded) == "table" then
		local moduleNames = {}
		for name in pairs(package.loaded) do
			if type(name) == "string" and not baseline.loaded[name] then moduleNames[#moduleNames + 1] = name end
		end
		table.sort(moduleNames)
		for _, name in ipairs(moduleNames) do
			loaded[#loaded + 1] = stringToken(name) .. visitAt(package.loaded[name], ctx, "package.loaded[" .. name .. "]")
		end
	end
	local enginePatches = {}
	for _, saved in ipairs(baseline.tables or {}) do
		local keys, changes = {}, {}
		for key in pairs(saved.entries) do if carriesKey(saved, key) then keys[key] = true end end
		for key in pairs(saved.object) do if carriesKey(saved, key) then keys[key] = true end end
		for _, key in ipairs(sortedKeys(keys, ctx)) do
			local value = rawget(saved.object, key)
			if not rawequal(value, saved.entries[key]) then changes[#changes + 1] = { key, value } end
		end
		local meta = getmetatable(saved.object)
		if #changes > 0 or not rawequal(meta, saved.meta) then
			enginePatches[#enginePatches + 1] = pathToken(baseline.paths[saved.object]) .. visitAt(changes, ctx, "engine table changes") .. visit(meta, ctx)
		end
	end
)lua"
    R"lua(	for _, ref in ipairs(ctx.boxRefs) do
		local link = ctx.areaBoxes[ref.address]
		if not link then
			local owner, index = _ScriptGraphSceneBoxOwner(ref.value)
			if owner then link = { owner = owner, index = index } end
		end
		if link then
			ctx.nodes[ref.id] = "U" .. ref.id .. ";b" .. visit(link.owner, ctx) .. "n" .. link.index .. ";" .. (ref.constant and "t;" or "f;") .. "I" .. ref.instance
		else
			problem(ctx, "a Box reference whose owning Area is missing")
			ctx.nodes[ref.id] = "U" .. ref.id .. ";z;I" .. ref.instance
		end
	end
	local rng = _ScriptGraphRandomState and stringToken(_ScriptGraphRandomState()) or "z;"
	local gibReferences = {}
	for _, link in ipairs(_ScriptGraphGibReferences(ctx.ownedPointers)) do
		gibReferences[#gibReferences + 1] = "n" .. link.owner .. ";n" .. link.index .. ";" .. visit(link.target, ctx)
	end
	local out = { "SG3;", "r", #rootIds, ";", table.concat(rootIds), "G", #globals, ";", table.concat(globals), "L", #loaded, ";", table.concat(loaded), "E", #enginePatches, ";", table.concat(enginePatches), "R", rng, "X", #gibReferences, ";", table.concat(gibReferences), "N", ctx.count, ";" }
	for id = 1, ctx.count do out[#out + 1] = ctx.nodes[id] end
	lastObjects = setmetatable({}, { __mode = "v" })
	for value, id in pairs(ctx.ids) do keyLabels[value], lastObjects[id] = id, value end
	if _ScriptGraphEndCapture then _ScriptGraphEndCapture() end
	return table.concat(out), ctx.problems
end

function Graph.serialize(roots)
	local ok, text, problems = xpcall(function() return serializeGraph(roots) end, debug.traceback)
	if not ok then
		if _ScriptGraphEndCapture then _ScriptGraphEndCapture() end
		return "", { text }
	end
	return text, problems
end

-- Validate the complete graph before allocating or changing any native object.
local function newReader(text)
	local reader = { text = text, pos = 1, references = {} }
	function reader:bad(message)
		error("script graph: " .. message .. " at " .. self.pos)
	end
	function reader:peek() return string.sub(self.text, self.pos, self.pos) end
	function reader:expect(tag)
		if string.sub(self.text, self.pos, self.pos + #tag - 1) ~= tag then self:bad("expected '" .. tag .. "'") end
		self.pos = self.pos + #tag
	end
	function reader:readUntil(ch)
		local stop = string.find(self.text, ch, self.pos, true)
		if not stop then self:bad("unterminated token") end
		local value = string.sub(self.text, self.pos, stop - 1)
		self.pos = stop + 1
		return value
	end
	function reader:integer(value, minimum, maximum)
		local n = type(value) == "number" and value or (type(value) == "string" and string.match(value, "^%-?%d+$") and tonumber(value))
		if not n or n ~= math.floor(n) or n < (minimum or 0) or n > (maximum or 9007199254740991) then self:bad("invalid integer") end
		return n
	end
	function reader:count(delimiter)
		return self:integer(self:readUntil(delimiter or ";"), 0, #self.text - self.pos + 1)
	end
	function reader:number(repr)
		if repr == "nan" then return 0 / 0 end
		if repr == "inf" then return math.huge end
		if repr == "-inf" then return -math.huge end
		local value = tonumber(repr)
		if value == nil then self:bad("invalid number") end
		return value
	end
	function reader:readString(body)
		if not body then self:expect("s") end
		local length = self:count(":")
		if length > #self.text - self.pos + 1 then self:bad("truncated string") end
		local value = string.sub(self.text, self.pos, self.pos + length - 1)
		self.pos = self.pos + length
		return value
	end
	function reader:typed(kind)
		local token = self:readToken()
		if token.t ~= kind then self:bad("expected " .. kind .. " value") end
		return token
	end
	function reader:index(minimum)
		return self:integer(self:typed("num").v, minimum)
	end
	function reader:readToken()
		local c = self:peek()
		self.pos = self.pos + 1
		if c == "z" then self:expect(";") return { t = "nil" }
		elseif c == "t" or c == "f" then self:expect(";") return { t = "bool", v = c == "t" }
		elseif c == "n" then return { t = "num", v = self:number(self:readUntil(";")) }
		elseif c == "s" then return { t = "str", v = self:readString(true) }
		elseif c == "M" then return { t = "material", index = self:integer(self:readUntil(";"), 0, 255) }
		elseif c == "#" then
			local token = { t = "ref", id = self:integer(self:readUntil(";"), 1) }
			self.references[#self.references + 1] = token
			return token
		elseif c == "v" then
			local x, y = string.match(self:readUntil(";"), "^([^,]+),([^,]+)$")
			if not x then self:bad("invalid vector") end
			return { t = "vector", x = self:number(x), y = self:number(y) }
		elseif c == "m" then
			local a, b, c2, d = string.match(self:readUntil(";"), "^([^,]+),([^,]+),([^,]+),([^,]+)$")
			if not a then self:bad("invalid timer") end
			return { t = "timer", v = { self:number(a), self:number(b), self:number(c2), self:number(d) } }
		elseif c == "c" then
			return { t = "alarm", x = self:typed("num").v, y = self:typed("num").v, team = self:index(-2147483648), range = self:typed("num").v }
		elseif c == "d" then return { t = "module", name = self:readString() }
		elseif c == "P" then
			local fields = {}
			local count = self:count()
			if count < 8 or count % 2 ~= 0 then self:bad("invalid path request length") end
			for index = 1, count do fields[index] = self:typed("num").v end
			return { t = "path-request", fields = fields }
		elseif c == "e" or c == "w" then
			local uid = self:integer(self:readUntil(":"), 0)
			local name = self:readString()
			if c == "e" then return { t = "entity", uid = uid, class = name } end
			return { t = "field", uid = uid, property = name }
		elseif c == "q" then return { t = "controller", uid = self:integer(self:readUntil(";"), 0) }
		elseif c == "Q" then return { t = "controller-value", checkpoint = self:readString(), actor = self:readToken() }
		elseif c == "x" or c == "Y" then
			local token = { t = "owner-ref", owner = self:readToken(), property = self:readString(), index = self:index(0), constant = self:typed("bool").v }
			if c == "Y" then token.class = self:readString(); token.checkpoint = self:readString() end
			return token
		elseif c == "g" then
			local segments = {}
			local count = self:count()
			if count == 0 then self:bad("empty native path") end
			for i = 1, count do segments[i] = self:readString() end
			return { t = "path", segments = segments }
		elseif c == "p" then return { t = "preset", class = self:readString(), preset = self:readString(), module = self:readString() }
		elseif c == "A" then self:expect(";") return { t = "activity" }
		elseif c == "G" then return { t = "global-script", index = self:integer(self:readUntil(";"), 1) }
		elseif c == "S" then self:expect(";") return { t = "scene" }
		elseif c == "a" then return { t = "area", name = self:readString() }
		elseif c == "b" then return { t = "box", owner = self:readToken(), index = self:index(0), constant = self:typed("bool").v }
		elseif c == "i" then return { t = "gib", owner = self:readToken(), index = self:index(0) }
		elseif c == "h" then return { t = "property", owner = self:readToken(), property = self:readString(), constant = self:typed("bool").v }
		elseif c == "j" then return { t = "soundset", owner = self:readToken(), index = self:index(-1) }
		elseif c == "l" then return { t = "limb", owner = self:readToken(), index = self:index(0) }
		elseif c == "k" then return { t = "limb-vector", owner = self:readToken(), index = self:index(-1), constant = self:typed("bool").v }
		end
		self:bad("unknown token '" .. c .. "'")
	end
	return reader
end

local function parse(text)
	local reader = newReader(text)
	local version = reader:readUntil(";")
	if version ~= "SG1" and version ~= "SG2" and version ~= "SG3" then reader:bad("bad header") end
	local graph = { roots = {}, globals = {}, loaded = {}, nodes = {}, enginePatches = {}, gibReferences = {}, nativeReferences = {} }
	local function namedList(tag, into, nameKey)
		reader:expect(tag)
		local names = {}
		for _ = 1, reader:count() do
			local name = reader:readString()
			if names[name] then reader:bad("duplicate " .. tag .. " entry") end
			names[name] = true
			into[#into + 1] = { [nameKey] = name, value = reader:readToken() }
		end
	end
	namedList("r", graph.roots, "uid")
	namedList("G", graph.globals, "name")
	namedList("L", graph.loaded, "name")
	if version ~= "SG1" then
		reader:expect("E")
		for _ = 1, reader:count() do
			graph.enginePatches[#graph.enginePatches + 1] = { target = reader:typed("path"), changes = reader:readToken(), meta = reader:readToken() }
		end
	end
	if version == "SG3" then
		reader:expect("R")
		graph.rng = reader:readToken()
		if graph.rng.t ~= "nil" and graph.rng.t ~= "str" then reader:bad("invalid random state") end
	end
	if reader:peek() == "X" then
		reader:expect("X")
		for _ = 1, reader:count() do
			graph.gibReferences[#graph.gibReferences + 1] = { owner = reader:index(1), index = reader:index(0), target = reader:readToken() }
		end
	end
	if reader:peek() == "Y" then
		reader:expect("Y")
		for _ = 1, reader:count() do
			graph.nativeReferences[#graph.nativeReferences + 1] = { owner = reader:readToken(), property = reader:readString(), target = reader:readToken() }
		end
	end
	reader:expect("N")
	local count = reader:count()
	for expected = 1, count do
		local kind = reader:peek()
		reader.pos = reader.pos + 1
		local id = reader:integer(reader:readUntil(";"), 1, count)
		if id ~= expected then reader:bad("noncontiguous or duplicate node ID") end
		local node = { kind = kind, id = id }
		if kind == "T" then
			reader:expect("P")
			if reader:peek() == "-" then reader:expect("-;") else node.path = reader:typed("path") end
			reader:expect("M")
			node.meta = reader:readToken()
			reader:expect("k")
			node.pairs = {}
			for i = 1, reader:count() do
				local key = reader:readToken()
				if key.t == "nil" or (key.t == "num" and key.v ~= key.v) then reader:bad("invalid table key") end
				node.pairs[i] = { key, reader:readToken() }
			end
		elseif kind == "F" then
			if reader:peek() == "R" then
				reader:expect("R")
				node.path = reader:typed("path")
				if reader:peek() == "D" then reader:expect("D"); node.code = reader:readString() end
			else
				reader:expect("D")
				node.code = reader:readString()
			end
			reader:expect("E")
			node.env = reader:readToken()
			reader:expect("u")
			node.cells = {}
			for i = 1, reader:count() do
				reader:expect("c")
				node.cells[i] = reader:integer(reader:readUntil(";"), 1, count)
			end
		elseif kind == "B" then
			node.factory = reader:readString()
			reader:expect("u")
			node.upvalues = {}
			for i = 1, reader:count() do node.upvalues[i] = reader:readToken() end
		elseif kind == "J" then
			local ownership = reader:peek()
			if ownership ~= "o" and ownership ~= "r" then reader:bad("invalid iterator ownership") end
			reader.pos = reader.pos + 1
			node.owned = ownership == "o"
			node.owner = reader:readToken()
			node.first = reader:index(0)
			if not node.owned then
				node.last = reader:index(node.first)
				node.creator = reader:readToken()
			end
			reader:expect("u")
			node.values = {}
			for i = 1, reader:count() do node.values[i] = reader:readToken() end
		elseif kind == "U" then
			local c = reader:peek()
			if c == "o" or c == "O" then
				reader.pos = reader.pos + 1
				local uid = c == "O" and reader:integer(reader:readUntil(";"), 0) or nil
				node.copy = { uid = uid, class = reader:readString(), preset = reader:readString(), module = reader:readString(), ini = reader:readString() }
			else node.value = reader:readToken() end
			if reader:peek() == "I" then reader:expect("I"); node.instance = reader:readToken() end
		elseif kind == "C" then
			if reader:peek() == "O" then
				reader:expect("O")
				local thread = reader:typed("ref")
				node.open = { thread = thread.id, slot = reader:index(0) }
			else node.value = reader:readToken() end
		elseif kind == "H" then
			node.status = reader:readUntil(";")
			if node.status ~= "s" and node.status ~= "n" and node.status ~= "d" then reader:bad("invalid coroutine status") end
			node.first = reader:integer(reader:readUntil(";"), 0, #text)
			node.base = reader:integer(reader:readUntil(";"), node.first, #text)
			node.top = reader:integer(reader:readUntil(";"), node.base, #text)
			if node.top - node.first > #text - reader.pos + 1 then reader:bad("truncated coroutine stack") end
			node.entries = {}
			for i = 1, node.top - node.first do
				local c = reader:peek()
				reader.pos = reader.pos + 1
				if c == "P" then node.entries[i] = { kind = "P", pcslot = reader:integer(reader:readUntil(":"), 0, node.top - 1), pos = reader:integer(reader:readUntil(";"), 0) }
				elseif c == "L" then node.entries[i] = { kind = "L", ftsz = reader:integer(reader:readUntil(";"), 0) }
				elseif c == "K" then node.entries[i] = { kind = "K", name = reader:readString() }
				elseif c == "V" then node.entries[i] = { kind = "V", value = reader:readToken() }
				else reader:bad("invalid coroutine slot") end
			end
		else reader:bad("unknown node kind '" .. kind .. "'") end
		graph.nodes[id] = node
	end
	if reader.pos ~= #text + 1 then reader:bad("trailing data") end
	for _, ref in ipairs(reader.references) do
		if not graph.nodes[ref.id] or graph.nodes[ref.id].kind == "C" then reader:bad("invalid object reference " .. ref.id) end
	end
	for _, node in ipairs(graph.nodes) do
		if node.cells then
			for _, id in ipairs(node.cells) do
				if not graph.nodes[id] or graph.nodes[id].kind ~= "C" then reader:bad("invalid upvalue cell reference") end
			end
		elseif node.open then
			local thread = graph.nodes[node.open.thread]
			if not thread or thread.kind ~= "H" or node.open.slot < thread.first or node.open.slot >= thread.top then reader:bad("invalid open upvalue") end
		end
	end
	return graph
end

function Graph.validate(text)
	local graph = parse(text)
	if graph.rng and graph.rng.t ~= "nil" and not _ScriptGraphRandomState(graph.rng.v, true) then error("script graph: invalid random state") end
	for _, node in ipairs(graph.nodes) do
		if node.value and node.value.t == "controller-value" and not _ScriptGraphControllerState(nil, node.value.checkpoint) then error("script graph: invalid controller checkpoint") end
		if node.value and node.value.t == "owner-ref" and node.value.checkpoint and not _ScriptGraphOwnerState(nil, node.value.class, node.value.checkpoint) then error("script graph: invalid owner checkpoint") end
		if node.code then
			local fn, message = (loadstring or load)(node.code)
			if not fn then error("script graph: invalid closure: " .. tostring(message)) end
			if debug.getinfo(fn, "u").nups ~= #node.cells then error("script graph: closure upvalue count differs") end
		elseif node.kind == "B" and not nativeClosures[node.factory] then
			error("script graph: unknown native closure factory " .. node.factory)
		elseif node.copy then
			local copy = node.copy
			local bare = copy.preset == "" or copy.preset == "None"
			local values = (_ScriptGraphBaseline or {}).values or _G
			if not values[bare and copy.class or ("Create" .. copy.class)] then error("script graph: unknown native class " .. copy.class) end
		end
	end
	return {}
end

local function resolvePath(segments)
	local value = _G
	for _, segment in ipairs(segments) do
		if type(value) ~= "table" and type(value) ~= "userdata" then return nil end
		value = value[segment]
	end
	return value
end

local function assignPath(segments, object)
	local parent = _G
	for i = 1, #segments - 1 do
		if type(parent) ~= "table" and type(parent) ~= "userdata" then return false end
		parent = parent[segments[i]]
	end
	if type(parent) ~= "table" and type(parent) ~= "userdata" then return false end
	parent[segments[#segments]] = object
	return true
end

local function pathText(segments)
	return table.concat(segments, ".")
end

)lua"
	    R"lua(
local preparedGraph
function Graph.prepare(text, reuseHeld)
	local graph = parse(text)
	local objects, problems = {}, {}
	if reuseHeld and heldObjects then for id, value in pairs(heldObjects) do objects[id] = value end end
	preparedGraph = { text = text, graph = graph, objects = objects, problems = problems }
	local baseline = _ScriptGraphBaseline or {}
	for id = 1, #graph.nodes do
		local copy = graph.nodes[id].copy
		if copy then
			local bare = copy.preset == "" or copy.preset == "None"
			local create = (baseline.values or _G)[bare and copy.class or ("Create" .. copy.class)]
			if reuseHeld and heldObjects and heldObjects[id] then
				objects[id] = heldObjects[id]
			elseif create == nil then
				problems[#problems + 1] = "no Create" .. copy.class .. " to rebuild a " .. copy.preset
			else
				if copy.uid and liveOwned[copy.uid] then _ScriptGraphNativeRelease(liveOwned[copy.uid]) end
				local ok, object
				if string.sub(copy.ini, 1, 19) == "15 PrimitiveValue1 " then ok, object = pcall(_ScriptGraphPrimitiveCreate, copy.class)
				elseif bare then ok, object = pcall(create) else ok, object = pcall(create, copy.preset, copy.module) end
				if not ok or object == nil then
					problems[#problems + 1] = "the " .. copy.class .. " " .. copy.preset .. " of " .. copy.module .. " could not be rebuilt"
				else
					local loaded, message = _ScriptGraphNativeLoad(object, copy.ini)
					if not loaded then problems[#problems + 1] = "the " .. copy.class .. " " .. copy.preset .. " did not take its saved state: " .. tostring(message) end
					objects[id] = object
				end
			end
		end
	end
	return problems
end

function Graph.prepareRoots()
	local problems = {}
	if not preparedGraph then return { "no script graph was prepared" } end
	for _, root in ipairs(preparedGraph.graph.roots) do
		local ok, message = _ScriptGraphAdoptRoot(tonumber(root.uid))
		if not ok then problems[#problems + 1] = message end
	end
	preparedGraph.rootsReady = true
	return problems
end

function Graph.clearPrepared() preparedGraph = nil end

-- Restores the graph into this VM; returns { [uidString] = instanceTable } and the list of problems (any problem means the restore is unfaithful).
function Graph.deserialize(text, reuseHeld, adoptRoots)
	if not preparedGraph or preparedGraph.text ~= text then Graph.prepare(text, reuseHeld) end
	local graph, objects, problems = preparedGraph.graph, preparedGraph.objects, preparedGraph.problems
	if adoptRoots and not preparedGraph.rootsReady then
		for _, message in ipairs(Graph.prepareRoots()) do problems[#problems + 1] = message end
	end
	preparedGraph = nil
	local baseline = _ScriptGraphBaseline or { globals = {}, loaded = {} }
	for name, value in pairs(baseline.values or {}) do rawset(_G, name, value) end
	for _, saved in ipairs(baseline.tables or {}) do
		for key in pairs(saved.object) do if carriesKey(saved, key) then rawset(saved.object, key, nil) end end
		for key, value in pairs(saved.entries) do rawset(saved.object, key, value) end
		setmetatable(saved.object, saved.meta)
	end
	local function fail(message) problems[#problems + 1] = message end
	local note = fail
	local resolve
	resolve = function(token, held)
		local t = token.t
		if t == "nil" then return nil
		elseif t == "bool" or t == "num" or t == "str" then return token.v
		elseif t == "ref" then return objects[token.id]
		elseif t == "path" then
			local value = resolvePath(token.segments)
			if value == nil then note("the named value " .. pathText(token.segments) .. " is missing") end
			return value
		elseif t == "entity" then
			local mo = MovableMan:FindObjectByUniqueID(token.uid)
			if mo == nil then note("the object " .. token.uid .. " (" .. token.class .. ") is not in the world") return nil end
			local cast = _G["To" .. token.class]
			return cast and cast(mo) or mo
		elseif t == "field" then
			local mo = MovableMan:FindObjectByUniqueID(token.uid)
			if mo == nil then note("the object " .. token.uid .. " whose " .. token.property .. " a field aliases is not in the world") return nil end
			local ok, value = pcall(function() return mo[token.property] end)
			if not ok or value == nil then note("the object " .. token.uid .. " has no " .. token.property .. " to alias") return nil end
			return value
		elseif t == "controller" then
			local mo = MovableMan:FindObjectByUniqueID(token.uid)
			if mo == nil or not IsActor(mo) then note("the actor " .. token.uid .. " whose controller a field aliases is not in the world") return nil end
			return ToActor(mo):GetController()
		elseif t == "controller-value" then
			local controller = held or Controller()
			if not _ScriptGraphControllerState(controller, token.checkpoint) then note("a Controller state is invalid") end
			return controller
		elseif t == "vector" then
			local vector = held or Vector()
			vector.X, vector.Y = token.x, token.y
			return vector
		elseif t == "alarm" then
			local alarm = held or AlarmEvent()
			alarm.ScenePos = Vector(token.x, token.y)
			alarm.Team = token.team
			alarm.Range = token.range
			return alarm
		elseif t == "module" then
			local id = PresetMan:GetModuleID(token.name)
			if id < 0 then note("the module " .. token.name .. " is missing") return nil end
			return PresetMan:GetDataModule(id)
		elseif t == "path-request" then return _ScriptGraphPathRequest(token.fields, held)
		elseif t == "timer" then
			local timer = held or Timer()
			timer.StartSimTimeTicks = token.v[1]
			timer.SimTimeLimitTicks = token.v[2]
			timer.StartRealTimeTicks = token.v[3]
			timer.RealTimeLimitTicks = token.v[4]
			return timer
		elseif t == "preset" then
			local preset = PresetMan:GetPreset(token.class, token.preset, token.module)
			if preset == nil then note("the preset " .. token.class .. " " .. token.preset .. " of " .. token.module .. " is missing") end
			return preset
		elseif t == "material" then return SceneMan:GetMaterialFromID(token.index)
		elseif t == "activity" then
			local activity = ActivityMan:GetActivity()
			return activity and IsGameActivity(activity) and ToGameActivity(activity) or activity
		elseif t == "global-script" then
			local script = _ScriptGraphGlobalScript(token.index)
			if not script then note("the global script at index " .. token.index .. " is missing") end
			return script
		elseif t == "scene" then return SceneMan.Scene
		elseif t == "area" then
			local area = SceneMan.Scene and SceneMan.Scene:GetArea(token.name)
			if not area then note("the scene Area " .. token.name .. " is missing") end
			return area
		elseif t == "box" then
			local area = resolve(token.owner)
			if not area and token.owner.t == "ref" then
				local owner = graph.nodes[token.owner.id]
				if owner and owner.value then area = resolve(owner.value); objects[token.owner.id] = area end
			end
			local box = area and _ScriptGraphAreaBox(area, token.index, token.constant)
			if not box then note("an Area box at index " .. token.index .. " is missing") end
			return box
		elseif t == "gib" or t == "property" or t == "owner-ref" then
			local owner = resolve(token.owner)
			if not owner and token.owner.t == "ref" then
				local saved = graph.nodes[token.owner.id]
				if saved and saved.value then owner = resolve(saved.value); objects[token.owner.id] = owner end
			end
			local value
			if owner then
				if t == "gib" then value = _ScriptGraphGib(owner, token.index)
				elseif t == "owner-ref" then
					value = _ScriptGraphOwnerReference(owner, token.property, token.index, token.constant)
					if value and token.checkpoint and not _ScriptGraphOwnerState(value, token.class, token.checkpoint) then note("a borrowed owner checkpoint could not be restored") end
				else value = _ScriptGraphProperty(owner, token.property, token.constant) end
			end
			if not value then note("a borrowed " .. t .. " is missing") end
			return value
		elseif t == "soundset" then
			local owner = resolve(token.owner)
			if not owner and token.owner.t == "ref" then
				local saved = graph.nodes[token.owner.id]
				if saved and saved.value then owner = resolve(saved.value); objects[token.owner.id] = owner end
			end
			local soundSet = owner and _ScriptGraphSoundSet(owner, token.index)
			if not soundSet then note("a SoundSet is missing") end
			return soundSet
		elseif t == "limb" then
			local actor = resolve(token.owner)
			if not actor and token.owner.t == "ref" then
				local owner = graph.nodes[token.owner.id]
				if owner and owner.value then actor = resolve(owner.value); objects[token.owner.id] = actor end
			end
			local limb = actor and _ScriptGraphActorLimb(actor, token.index)
			if not limb then note("an actor's LimbPath is missing") end
			return limb
		elseif t == "limb-vector" then
			local limb = resolve(token.owner)
			if not limb and token.owner.t == "ref" then
				local owner = graph.nodes[token.owner.id]
				if owner and owner.value then limb = resolve(owner.value); objects[token.owner.id] = limb end
			end
			local vector = limb and _ScriptGraphLimbVector(limb, token.index, token.constant)
			if not vector then note("a LimbPath vector is missing") end
			return vector
		end
		fail("unknown token type " .. tostring(t))
		return nil
	end
	-- Allocate every table and function first, so references resolve in any order.
	local ids = {}
	for id in pairs(graph.nodes) do ids[#ids + 1] = id end
	table.sort(ids)
	for _, id in ipairs(ids) do
		local node = graph.nodes[id]
		if node.kind == "T" then
			local object = reuseHeld and objects[id] or nil
			if object then
				if node.path and not assignPath(node.path.segments, object) then fail("cannot restore a held table path") end
			elseif node.path then
				object = resolvePath(node.path.segments)
				if type(object) ~= "table" or (baseline.paths and baseline.paths[object]) or object == _G then
					object = {}
					if not assignPath(node.path.segments, object) then fail("cannot place the table " .. pathText(node.path.segments)) end
				end
			else
				object = {}
			end
			objects[id] = object
		elseif node.kind == "H" then
			objects[id] = (reuseHeld and objects[id]) or coroutine.create(function() end)
		elseif node.kind == "B" then
			local create = nativeClosures[node.factory]
			if not (reuseHeld and objects[id]) then
				if create then objects[id] = create() else fail("unknown native closure factory " .. node.factory) end
			end
		end
	end
	for _, id in ipairs(ids) do
		local node = graph.nodes[id]
		if node.kind == "U" then
			if node.copy then
				if node.copy.uid and objects[id] then
					liveOwned[node.copy.uid] = objects[id]
				end
				if not reuseHeld and objects[id] and not _ScriptGraphNativeResolve(objects[id]) then fail("a native runtime reference is missing") end
			elseif objects[id] == nil then
				objects[id] = resolve(node.value)
			elseif reuseHeld and node.value and (node.value.t == "vector" or node.value.t == "timer" or node.value.t == "alarm" or node.value.t == "controller-value" or node.value.t == "path-request") then
				objects[id] = resolve(node.value, objects[id])
			end
		end
	end
	for _, id in ipairs(ids) do
		local node = graph.nodes[id]
		if node.kind == "U" and node.value and node.value.t == "controller-value" then
			if not _ScriptGraphControllerActor(objects[id], resolve(node.value.actor)) then fail("a Controller actor reference is invalid") end
		end
		if node.copy and objects[id] and not reuseHeld and not _ScriptGraphPrimitiveResolve(objects[id]) then fail("a primitive sprite reference is missing") end
		if node.kind == "J" then
			local values = {}
			for index, token in ipairs(node.values) do values[index] = resolve(token) end
			if reuseHeld and objects[id] then
				if not _ScriptGraphIteratorRewind(objects[id], node.first, node.owned and (node.first + #node.values) or node.last) then fail("a held native iterator could not rewind") end
			elseif node.owned then
				objects[id] = _ScriptGraphIteratorFromValues(values, resolve(node.owner), node.first, #node.values)
			else
				local iterator, message = _ScriptGraphIteratorRestore(resolve(node.owner), resolve(node.creator), node.first, node.last, values)
				if not iterator then fail("a native iterator could not be restored: " .. tostring(message)) end
				objects[id] = iterator
			end
		end
	end
	for _, link in ipairs(graph.gibReferences) do
		if not _ScriptGraphRestoreGibReference(link.owner, link.index, resolve(link.target)) then
			fail("the Gib at index " .. link.index .. " on object " .. link.owner .. " could not restore its particle reference")
		end
	end
	for _, id in ipairs(ids) do
		local node = graph.nodes[id]
		if node.kind == "F" then
			local fn = (reuseHeld and objects[id]) or (node.path and resolvePath(node.path.segments))
			if type(fn) == "function" and (not node.code or (debug.getinfo(fn, "S").what ~= "C" and string.dump(fn, "d") == node.code)) then
				objects[id] = fn
			elseif node.code then
				local chunk, message = (loadstring or load)(node.code)
				if not chunk then fail("a closure could not be rebuilt: " .. tostring(message)) end
				objects[id] = chunk
				if node.path and chunk and not assignPath(node.path.segments, chunk) then fail("cannot place the function " .. pathText(node.path.segments)) end
			else
				fail("the resident function " .. pathText(node.path.segments) .. " is missing")
			end
		end
	end
	-- Coroutines are rebuilt slot for slot once every table and function they hold exists.
	for _, id in ipairs(ids) do
		local node = graph.nodes[id]
		if node.kind == "H" then
			if not _ScriptGraphThreadRestore then
				fail("no coroutine codec in this state")
			else
				local desc = { status = node.status == "s" and "suspended" or (node.status == "n" and "notstarted" or "dead"), first = node.first, base = node.base, top = node.top, slots = {}, links = {}, conts = {} }
				for i, entry in ipairs(node.entries) do
					local index = node.first + i - 1
					if entry.kind == "P" then desc.links[index] = { pcslot = entry.pcslot, pos = entry.pos }
)lua"
	    R"lua(					elseif entry.kind == "L" then desc.links[index] = { ftsz = entry.ftsz }
					elseif entry.kind == "K" then desc.conts[index] = entry.name
					else desc.slots[index] = resolve(entry.value) end
				end
				local canonical, problem = Graph.canonicalThread(desc)
				if not canonical then fail("a coroutine could not be rebuilt: " .. tostring(problem)) end
				local thread, message = _ScriptGraphThreadRestore(canonical or desc, objects[id])
				if thread then objects[id] = thread else fail("a coroutine could not be rebuilt: " .. tostring(message)) end
			end
		end
	end
	for _, id in ipairs(ids) do
		local node = graph.nodes[id]
		if node.kind == "B" and objects[id] then
			for i, token in ipairs(node.upvalues) do
				if node.factory == "gmatch" and i == 3 then
					_ScriptGraphGmatchPosition(objects[id], resolve(token))
				elseif debug.setupvalue(objects[id], i, resolve(token)) == nil then
					fail("a native closure has fewer captured variables than its saved state")
				end
			end
		end
	end
	-- Fill the tables, then the closures' captured variables.
	for _, id in ipairs(ids) do
		local node = graph.nodes[id]
		if node.kind == "T" then
			local object = objects[id]
			if node.path or reuseHeld then
				for key in pairs(object) do rawset(object, key, nil) end
			end
			local meta = resolve(node.meta)
			setmetatable(object, meta)
			for _, pair in ipairs(node.pairs) do
				local key = resolve(pair[1])
				if key ~= nil then rawset(object, key, resolve(pair[2])) end
			end
		end
	end
	local cellOwners = {}
	for _, id in ipairs(ids) do
		local node = graph.nodes[id]
		if node.kind == "U" and node.instance and objects[id] then
			local instance = resolve(node.instance)
			if not _ScriptGraphSetInstance(objects[id], instance) then fail("a bound object's Lua fields could not be restored") end
		end
	end
	for _, id in ipairs(ids) do
		local node = graph.nodes[id]
		if node.kind == "F" and objects[id] then
			local fn = objects[id]
			if node.code then
				local env = resolve(node.env)
				if type(env) == "table" then setfenv(fn, env) end
			end
			for i, cellId in ipairs(node.cells) do
				local name = debug.getupvalue(fn, i)
				if name == nil then
					note("a function has fewer captured variables than its saved state")
					break
				end
				local cell = graph.nodes[cellId]
				local owner = cellOwners[cellId]
				if cell and cell.open then
					local thread = objects[cell.open.thread]
					if type(thread) ~= "thread" then
						fail("an open upvalue's coroutine is missing")
					else
						local ok, message = _ScriptGraphJoinOpenUpvalue(fn, i, thread, cell.open.slot)
						if not ok then fail("an open upvalue could not be joined: " .. tostring(message)) end
					end
				elseif owner then
					debug.upvaluejoin(fn, i, owner[1], owner[2])
				else
					cellOwners[cellId] = { fn, i }
					debug.setupvalue(fn, i, cell and resolve(cell.value) or nil)
				end
			end
		end
	end
	local baseline = _ScriptGraphBaseline or { globals = {}, loaded = {} }
	local globals, loaded = {}, {}
	for _, entry in ipairs(graph.globals) do globals[entry.name] = true end
	for name in pairs(_G) do
		if type(name) == "string" and not baseline.globals[name] and not SKIP_GLOBALS[name] and not globals[name] then _G[name] = nil end
	end
	for _, entry in ipairs(graph.loaded) do loaded[entry.name] = true end
	if type(package) == "table" and type(package.loaded) == "table" then
		for name in pairs(package.loaded) do
			if type(name) == "string" and not baseline.loaded[name] and not loaded[name] then package.loaded[name] = nil end
		end
	end
	for _, entry in ipairs(graph.globals) do _G[entry.name] = resolve(entry.value) end
	for _, entry in ipairs(graph.loaded) do
		if type(package) == "table" and type(package.loaded) == "table" then package.loaded[entry.name] = resolve(entry.value) end
	end
	for _, patch in ipairs(graph.enginePatches) do
		local object = resolve(patch.target)
		for _, change in ipairs(resolve(patch.changes)) do rawset(object, change[1], change[2]) end
		setmetatable(object, resolve(patch.meta))
	end
	local roots = {}
	for _, root in ipairs(graph.roots) do roots[root.uid] = resolve(root.value) end
	for id, value in pairs(objects) do keyLabels[value] = id end
	if graph.rng and (graph.rng.t ~= "str" or not _ScriptGraphRandomState or not _ScriptGraphRandomState(graph.rng.v)) then
		fail("the Lua state's random generator could not be restored")
	end
	return roots, problems
end

-- The unique ids of the objects the text holds a root for.
function Graph.roots(text)
	local reader = newReader(text)
	local version = reader:readUntil(";")
	if version ~= "SG1" and version ~= "SG2" and version ~= "SG3" then error("script graph: bad header") end
	reader.pos = reader.pos + 1
	local uids = {}
	for _ = 1, tonumber(reader:readUntil(";")) do
		uids[#uids + 1] = reader:readString()
		reader:readToken()
	end
	return uids
end

)lua";

	constexpr const char* c_ScriptGraphSelfTest = R"lua(
-- The script graph's contracts, run inside the engine's master state by -script-graph-selftest.
local results = {}
local function check(name, ok, detail)
	results[#results + 1] = string.format("[script-graph-selftest] %s %s%s", ok and "PASS" or "FAIL", name, detail and (" " .. tostring(detail)) or "")
	if _ScriptGraphProgress then _ScriptGraphProgress(results[#results]) end
end
local function resumed(co, ...)
	local ok, value = coroutine.resume(co, ...)
	return tostring(ok) .. "/" .. tostring(value) .. "/" .. coroutine.status(co)
end

_SelfTestShared = { count = 7 }
_SelfTestVector = Vector(11, 12)
_SelfTestTimer = Timer()
_SelfTestTimer.StartSimTimeTicks = 4567
math._Checkpoint = { count = 7 }
local function wrapAbs(original) return function(value) return original(value) end end
math.abs = wrapAbs(math.abs)
_VERSION = "snapshot version"
_SelfTestMod = {}
do
	local count = 0
	function _SelfTestMod.f() count = count + 1 return count end
	function _SelfTestMod.job(x) coroutine.yield(x) return x * 2 end
	function _SelfTestMod.opener()
		local n = 0
		local get = function() return n end
		coroutine.yield(get)
		n = n + 1
		coroutine.yield(get())
		return n
	end
	function _SelfTestMod.protected()
		local ok, v = pcall(function() coroutine.yield("in") return "out" end)
		return ok, v
	end
	_SelfTestMod.meta = { __index = function(t, k) coroutine.yield("idx:" .. k) return k .. "!" end }
	function _SelfTestMod.indexer()
		local t = setmetatable({}, _SelfTestMod.meta)
		local v = t.foo
		return v
	end
	function _SelfTestMod.varargs(...)
		coroutine.yield(select("#", ...))
		return ...
	end
end
_SelfTestKlass = {}
_SelfTestKlass.__index = _SelfTestKlass
function _SelfTestKlass.hello(self) return "hi " .. tostring(self.name) end
local env = setmetatable({}, { __index = _G })
_G["selftest.lua"] = env
local chunk = loadstring("fileLocal = (fileLocal or 0) + 10; return function() fileLocal = fileLocal + 1; return fileLocal end")
setfenv(chunk, env)
local envFn = chunk()

local a, b = {}, {}
do
	local count = 0
	a.step = function() count = count + 1 return count end
	a.peek = function() return count end
end
a.shared = _SelfTestShared
a.globalVector = _SelfTestVector
a.globalTimer = _SelfTestTimer
a.engineTableState = math._Checkpoint
a.engineAbs = math.abs
a.protectedTable = setmetatable({ stored = 7 }, { __metatable = "locked", __index = function() return 42 end })
a.pair = { _SelfTestShared, _SelfTestShared }
a.obj = setmetatable({ name = "x" }, _SelfTestKlass)
a.envFn = envFn
a.helper = function() return _SelfTestMod.f() end
a.nums = { p = 0.1, q = 1 / 3, r = -0.0, s = 2 ^ 53, t = 1e300, u = 0 / 0, v = math.huge, w = -math.huge, x = 42, y = -7.5 }
a.str = "bin\0\1\2;:\n\r\255end"
a.keys = { [true] = 1, [false] = 2, [1.5] = 3, [{}] = 4 }
a.keyAlias = setmetatable({}, { __tostring = function() error("serializer ran a key metamethod") end })
a.keys[a.keyAlias] = 5
a.self = a
-- Owned engine values: a Vector and a Timer each held under two names, an entity copy with an instance change.
a.vecA = Vector(3.25, -4.5)
a.vecB = a.vecA
a.vecA.customShared = a.shared
a.vecA.loop = a.vecA
a.timerA = Timer()
a.timerA.StartSimTimeTicks = 123456
a.timerA.SimTimeLimitTicks = 5000
a.timerB = a.timerA
a.sound = CreateSoundContainer("Funds Changed", "Base.rte")
a.sound.Volume = 4
a.soundAlias = a.sound
a.soundSet = SoundSet()
a.soundSet.SoundSelectionCycleMode = SoundSet.FORWARDS
a.soundSetAlias = a.soundSet
for mode = 0, 2 do
	local child = SoundSet()
	child.SoundSelectionCycleMode = mode
	a.soundSet:AddSoundSet(child)
end
a.soundIterator = a.soundSet.SubSoundSets
a.soundIteratorAlias = a.soundIterator
a.soundIterator()
a.aSoundTop = a.sound:GetTopLevelSoundSet()
a.aSoundTop.SoundSelectionCycleMode = SoundSet.FORWARDS
a.aSoundTop:SelectNextSounds()
a.aSoundTop.shared = a.shared
-- A reference into an engine object: the position of a known object.
a.mo = CreateAHuman("Green Dummy", "Base.rte")
a.mo.Pos = Vector(640, 480)
a.mo:SetSpritePixelIndex(1, 1, 0, 19, -1, false)
a.primitiveLine = LinePrimitive(2, Vector(3.25, 7.5), Vector(51, 63), 3.75, 47)
a.primitiveLineAlias = a.primitiveLine
a.primitiveText = TextPrimitive(1, Vector(15, 19), "native graph 42", true, 1, 0.375)
a.primitiveBitmap = BitmapPrimitive(3, Vector(23, 31), a.mo, 0.25, 0, 1.75, true, false)
a.primitiveFile = BitmapPrimitive(0, Vector(27, 39), "Base.rte/GUIs/Skins/Cursor.png", -0.25, false, true)
a.primitiveStates = {_ScriptGraphNativeSave(a.primitiveLine), _ScriptGraphNativeSave(a.primitiveText), _ScriptGraphNativeSave(a.primitiveBitmap), _ScriptGraphNativeSave(a.primitiveFile)}
a.limb = a.mo:GetLimbPath(AHuman.FGROUND, Actor.WALK)
a.limb.StartOffset = Vector(17, 29)
a.limbStart = a.limb.StartOffset
a.limbStartAlias = a.limbStart
a.gib = a.mo.Gibs()
a.gib.Count = 0
a.gib.ParticlePreset = nil
a.gibOffset = a.gib.Offset
a.gibOffset.X = 17
a.alarm = AlarmEvent()
a.alarm.ScenePos = Vector(3.25, -7.5)
a.alarm.Range = 27.5
a.alarm.Team = Activity.TEAM_2
a.alarmPosition = a.alarm.ScenePos
a.alarm.shared = a
a.onlyAlarmPosition = AlarmEvent().ScenePos
a.onlyAlarmPosition.X = 17
a.module = PresetMan:GetDataModule(PresetMan:GetModuleID("Base.rte"))
a.pathRequest = _ScriptGraphPathRequest({1, 0, 3, 7.25, 1, 2, 13, 17, 1, 2, 7, 8, 13, 17})
a.pathRequest.shared = a
a.pathIterator = a.pathRequest.Path
a.pathIterator()
a.posRef = a.mo.Pos
a.ctrl = a.mo:GetController()
a.ctrlAlias = a.ctrl
a.cachedMethod = MovableMan.AddParticle
local function captureMethod(method) return function() return method end end
a.methodClosure = captureMethod(a.cachedMethod)
do
	local saved = '{__scriptFieldsId=1,["testCreate"]=1,["testUpdate"]=249,["self"]={__scriptFieldsRef=1},["later"]={__scriptFieldsRef=2},["shared"]={__scriptFieldsId=2,["count"]=129},["vector"]={__scriptFieldsVector=true,x=3.25,y=-4.5},["timer"]={__scriptFieldsTimer=true,simStart=123,simLimit=456,realStart=789,realLimit=1000},["target"]={__scriptFieldsEntity=' .. a.mo.UniqueID .. ',__scriptFieldsClass="AHuman"}}'
	local fields = _ScriptGraph.restoreLegacy(saved)
	check("legacy_field_values", fields.testCreate == 1 and fields.testUpdate == 249 and fields.shared.count == 129)
	check("legacy_table_aliases", fields.self == fields and fields.later == fields.shared)
	check("legacy_native_values", fields.vector.X == 3.25 and fields.vector.Y == -4.5 and fields.timer.StartSimTimeTicks == 123 and fields.timer.RealTimeLimitTicks == 1000 and fields.target.UniqueID == a.mo.UniqueID)
	local legacy = CreateAHuman("Green Dummy", "Base.rte")
	local ini = _ScriptGraphNativeSave(legacy) .. "\n\tScriptState = " .. saved .. "\n"
	local loaded, message = _ScriptGraphNativeLoad(legacy, ini)
	check("legacy_field_reader", loaded, message)
	local adopted, errorText = _ScriptGraphAdoptRoot(legacy.UniqueID)
	check("legacy_object_initialized", adopted, errorText)
	local instance = _ScriptGraphInstance(_ScriptedObjects[tostring(legacy.UniqueID)])
	check("legacy_object_fields_installed", instance and instance.testCreate == 1 and instance.testUpdate == 249 and instance.self == instance)
end
)lua"
	    R"lua(
-- Coroutines: suspended mid-body, holding an open upvalue, inside pcall, inside a metamethod, with varargs, not started, dead.
a.job = coroutine.create(_SelfTestMod.job)
coroutine.resume(a.job, 21)
a.opener = coroutine.create(_SelfTestMod.opener)
local _, getter = coroutine.resume(a.opener)
a.getter = getter
a.protected = coroutine.create(_SelfTestMod.protected)
coroutine.resume(a.protected)
a.indexer = coroutine.create(_SelfTestMod.indexer)
local _, indexYield = coroutine.resume(a.indexer)
a.varargs = coroutine.create(_SelfTestMod.varargs)
coroutine.resume(a.varargs, 1, 2, 3)
a.fresh = coroutine.create(_SelfTestMod.job)
a.wrapped = coroutine.wrap(_SelfTestMod.job)
check("wrapped_yield_reached", a.wrapped(12) == 12)
a.wrappedAlias = a.wrapped
a.wrappedFresh = coroutine.wrap(_SelfTestMod.job)
a.matcher = string.gmatch("one two three", "%a+")
check("gmatch_started", a.matcher() == "one")
do
	local first, second
	first = coroutine.create(function() coroutine.yield() return second end)
	second = coroutine.create(function() coroutine.yield() return first end)
	coroutine.resume(first)
	coroutine.resume(second)
	a.threadPair = { first, second }
end
a.dead = coroutine.create(function() return 1 end)
coroutine.resume(a.dead)
-- A yield from a compiled trace sits behind LuaJIT's stitch frame; its checkpoint must equal the interpreter's.
do
	local source = "return function() local n = 0 while true do n = n + 1 coroutine.yield(n) end end"
	local stitchedStep, interpretedStep = assert(loadstring(source))(), assert(loadstring(source))()
	jit.off(interpretedStep, true)
	a.stitched = coroutine.create(stitchedStep)
	a.interpreted = coroutine.create(interpretedStep)
	jit.opt.start("hotloop=1")
	for _ = 1, 8 do coroutine.resume(a.stitched); coroutine.resume(a.interpreted) end
	jit.opt.start("hotloop=56")
	local raw = _ScriptGraphThreadCapture(a.stitched, true)
	local canonical = _ScriptGraphThreadCapture(a.stitched)
	local plain = _ScriptGraphThreadCapture(a.interpreted)
	local rawStitched = false
	for _, name in pairs(raw and raw.conts or {}) do if name == "stitch" then rawStitched = true end end
	check("coroutine_stitch_control_reached", jit.status() and rawStitched and raw.base == plain.base + 3, raw and plain and (raw.base .. " vs " .. plain.base) or "no capture")
	local function sameLayout(x, y)
		if not x or not y then return false, "no capture" end
		if x.status ~= y.status or x.first ~= y.first or x.base ~= y.base or x.top ~= y.top then return false, "bounds " .. x.base .. "/" .. x.top .. " vs " .. y.base .. "/" .. y.top end
		for i = x.first, x.top - 1 do
			local lx, ly = x.links[i], y.links[i]
			if (lx == nil) ~= (ly == nil) or x.conts[i] ~= y.conts[i] then return false, "slot " .. i end
			if lx and (lx.pcslot ~= ly.pcslot or lx.pos ~= ly.pos or lx.ftsz ~= ly.ftsz) then return false, "link " .. i end
			if not lx and type(x.slots[i]) ~= type(y.slots[i]) then return false, "value " .. i end
			if not lx and type(x.slots[i]) == "number" and x.slots[i] ~= y.slots[i] then return false, "number " .. i end
		end
		if next(x.conts) ~= nil then return false, "continuation left" end
		return true
	end
	check("coroutine_stitch_capture_canonical", sameLayout(canonical, plain))
	local collapsed = raw and _ScriptGraph.canonicalThread(raw)
	check("coroutine_stitch_raw_collapses", sameLayout(collapsed, canonical))
	local fromCanonical = canonical and _ScriptGraphThreadRestore(canonical)
	local fromRaw = collapsed and _ScriptGraphThreadRestore(collapsed)
	check("coroutine_stitch_canonical_resumes", fromCanonical and resumed(fromCanonical) == "true/9/suspended" and resumed(fromCanonical) == "true/10/suspended")
	check("coroutine_stitch_raw_resumes", fromRaw and resumed(fromRaw) == "true/9/suspended" and resumed(fromRaw) == "true/10/suspended")
	local malformed = raw and _ScriptGraph.canonicalThread({ status = raw.status, first = raw.first, base = raw.base, top = raw.top, slots = raw.slots, links = {}, conts = raw.conts })
	check("coroutine_stitch_malformed_refused", malformed == nil)
end
b.shared = _SelfTestShared
b.other = a
check("metamethod_yield_reached", indexYield == "idx:foo", indexYield)

local roots = { ["11"] = a, ["22"] = b }
local text, problems = _ScriptGraph.serialize(roots)
check("serialize_no_problems", #problems == 0, table.concat(problems, " | "))
if #problems ~= 0 then return table.concat(results, "\n") end
check("roots_listed", table.concat(_ScriptGraph.roots(text), ",") == "11,22")

-- Reference continuations on the originals, then advance the rest of the live state past the capture.
local jobReference = resumed(a.job, 5)
local stitchedReference = resumed(a.stitched)
local interpretedReference = resumed(a.interpreted)
local openerReference = resumed(a.opener)
local openerGetterReference = tostring(getter())
local protectedReference = resumed(a.protected)
local indexerReference = resumed(a.indexer)
local varargsReference = resumed(a.varargs)
a.step(); a.step()
_SelfTestMod.f(); _SelfTestMod.f()
_SelfTestShared.count = 99
math._Checkpoint.count = 88
math._AddedAfterCheckpoint = { bad = true }
math.abs = math.floor
_VERSION = "advanced version"
envFn(); envFn()
_SelfTestVector.X = 100
_SelfTestTimer.StartSimTimeTicks = 8000
a.vecA.customShared = { count = 100 }
a.vecA.afterCapture = true
_SelfTestAfterCapture = { count = 9 }
package.loaded._SelfTestAfterCapture = { count = 8 }
local wrappedReference = a.wrapped()
local matcherReference = a.matcher()
local liveModF = _SelfTestMod.f
local liveEnv = env
local liveShared = _SelfTestShared

local restored, restoreProblems = _ScriptGraph.deserialize(text)
check("restore_no_problems", #restoreProblems == 0, table.concat(restoreProblems, " | "))
if #restoreProblems ~= 0 then return table.concat(results, "\n") end
local ra, rb = restored["11"], restored["22"]
check("roots_present", ra ~= nil and rb ~= nil)
local text2, problems2 = _ScriptGraph.serialize({ ["11"] = ra, ["22"] = rb })
check("canonical_reserialize", text2 == text, string.format("len %d vs %d, problems %d", #text2, #text, #problems2))
if text2 ~= text then
	local f1 = io.open("script_graph_selftest_capture.txt", "wb")
	if f1 then f1:write(text) f1:close() end
	local f2 = io.open("script_graph_selftest_restored.txt", "wb")
	if f2 then f2:write(text2) f2:close() end
	for i = 1, math.min(#text, #text2) do
		if string.byte(text, i) ~= string.byte(text2, i) then
			check("first_difference", false, string.format("at %d: %q vs %q", i, string.sub(text, i, i + 80), string.sub(text2, i, i + 80)))
			break
		end
	end
end
check("closure_counter_rewound", ra.step() == 1)
check("shared_upvalue_between_closures", ra.peek() == 1)
check("cross_object_alias", ra.shared == rb.shared and ra.shared == _SelfTestShared and _SelfTestShared == liveShared)
check("global_table_rewound", _SelfTestShared.count == 7, _SelfTestShared.count)
check("engine_table_fields_rewound", math._Checkpoint ~= nil and math._Checkpoint == ra.engineTableState and math._Checkpoint.count == 7)
check("engine_table_new_fields_removed", math._AddedAfterCheckpoint == nil)
check("engine_function_override_rewound", math.abs == ra.engineAbs and math.abs(-2.5) == 2.5)
check("engine_global_override_rewound", _VERSION == "snapshot version")
check("protected_metatable_restored", getmetatable(ra.protectedTable) == "locked" and ra.protectedTable.missing == 42 and ra.protectedTable.stored == 7)
check("global_vector_rewound_and_aliased", _SelfTestVector.X == 11 and rawequal(_SelfTestVector, ra.globalVector))
check("global_timer_rewound_and_aliased", _SelfTestTimer.StartSimTimeTicks == 4567 and rawequal(_SelfTestTimer, ra.globalTimer))
check("userdata_lua_fields_rewound", ra.vecA.customShared == ra.shared and rawequal(ra.vecA.loop, ra.vecA) and ra.vecA.afterCapture == nil)
check("new_global_removed", _SelfTestAfterCapture == nil)
check("new_module_removed", package.loaded._SelfTestAfterCapture == nil)
check("within_object_alias", ra.pair[1] == ra.pair[2] and ra.pair[1] == ra.shared)
check("module_function_resident", _SelfTestMod.f == liveModF)
check("module_upvalue_rewound", _SelfTestMod.f() == 1)
check("helper_calls_live_resident", ra.helper() == 2)
check("metatable_by_name", getmetatable(ra.obj) == _SelfTestKlass and ra.obj:hello() == "hi x")
check("sandbox_env_same_table", _G["selftest.lua"] == liveEnv)
check("sandbox_env_value_rewound", liveEnv.fileLocal == 10, liveEnv.fileLocal)
check("sandbox_closure_uses_env", ra.envFn() == 11)
local n = ra.nums
check("numbers_roundtrip", n.p == 0.1 and n.q == 1 / 3 and n.r == 0 and 1 / n.r == -math.huge and n.s == 2 ^ 53 and n.t == 1e300 and n.u ~= n.u and n.v == math.huge and n.w == -math.huge and n.x == 42 and n.y == -7.5)
check("binary_string_roundtrip", ra.str == a.str)
local keyKinds = {}
for k, v in pairs(ra.keys) do keyKinds[#keyKinds + 1] = type(k) .. "=" .. v end
table.sort(keyKinds)
check("mixed_keys_roundtrip", table.concat(keyKinds, ",") == "boolean=1,boolean=2,number=3,table=4,table=5", table.concat(keyKinds, ","))
check("key_alias_without_metamethod", ra.keys[ra.keyAlias] == 5)
check("self_cycle", ra.self == ra)
check("cached_native_method", ra.cachedMethod == MovableMan.AddParticle and ra.methodClosure() == MovableMan.AddParticle)
check("native_gib_nullable_preset", ra.gib.Count == 0 and ra.gib.ParticlePreset == nil and ra.gibOffset.X == 17 and ra.mo.Gibs().Offset.X == 17)
check("native_alarm_values_and_owner", ra.alarm.ScenePos.X == 3.25 and ra.alarmPosition.Y == -7.5 and ra.alarm.Range == 27.5 and ra.alarm.Team == Activity.TEAM_2 and ra.alarm.shared == ra and ra.onlyAlarmPosition.X == 17)
check("native_module_reference", ra.module.FileName == "Base.rte")
local restoredPathPoint = ra.pathIterator()
check("native_path_request_and_iterator", ra.pathRequest.shared == ra and ra.pathRequest.PathLength == 3 and ra.pathRequest.TotalCost == 7.25 and ra.pathRequest.Status == 0 and restoredPathPoint.X == 7 and restoredPathPoint.Y == 8)
check("native_iterator_alias", ra.soundIterator == ra.soundIteratorAlias)
check("native_iterator_continuation", ra.soundIterator().SoundSelectionCycleMode == SoundSet.FORWARDS and ra.soundIterator().SoundSelectionCycleMode == SoundSet.ALL and ra.soundIterator() == nil)
check("controller_reference_alias", rawequal(ra.ctrl, ra.ctrlAlias))
ra.ctrl:SetState(Controller.WEAPON_FIRE, true)
check("controller_reference_targets_actor", ra.mo:GetController():IsState(Controller.WEAPON_FIRE))
ra.ctrl:SetState(Controller.WEAPON_FIRE, false)
check("cross_root_reference", rb.other == ra)
-- Owned values: state by value, one object per node, independent of the originals (Vector equality compares coordinates, so mutate).
check("vector_state", type(ra.vecA) == "userdata" and ra.vecA.X == 3.25 and ra.vecA.Y == -4.5, type(ra.vecA) == "userdata" and (ra.vecA.X .. "," .. ra.vecA.Y) or type(ra.vecA))
ra.vecA.X = 100
check("vector_alias_kept", ra.vecB.X == 100, ra.vecB.X)
check("vector_storage_independent", a.vecA.X == 3.25, a.vecA.X)
check("timer_state", ra.timerA.StartSimTimeTicks == 123456 and ra.timerA.SimTimeLimitTicks == 5000)
ra.timerA.SimTimeLimitTicks = 777
check("timer_alias_kept", ra.timerB.SimTimeLimitTicks == 777, ra.timerB.SimTimeLimitTicks)
check("timer_storage_independent", a.timerA.SimTimeLimitTicks == 5000, a.timerA.SimTimeLimitTicks)
check("entity_copy_instance_state", type(ra.sound) == "userdata" and ra.sound.Volume == 4, type(ra.sound) == "userdata" and ra.sound.Volume or type(ra.sound))
if type(ra.sound) == "userdata" then
	ra.sound.Volume = 2
	check("entity_copy_alias_kept", ra.soundAlias.Volume == 2, ra.soundAlias.Volume)
	check("entity_copy_storage_independent", a.sound.Volume == 4, a.sound.Volume)
	check("entity_copy_has_sounds", ra.sound:HasAnySounds() and a.sound:HasAnySounds())
end
check("owned_entity_identity", ra.mo ~= nil and ra.mo.UniqueID == a.mo.UniqueID and not rawequal(ra.mo, a.mo))
check("owned_primitive_alias", rawequal(ra.primitiveLine, ra.primitiveLineAlias))
check("owned_primitive_state", _ScriptGraphNativeSave(ra.primitiveLine) == ra.primitiveStates[1] and _ScriptGraphNativeSave(ra.primitiveText) == ra.primitiveStates[2] and _ScriptGraphNativeSave(ra.primitiveBitmap) == ra.primitiveStates[3] and _ScriptGraphNativeSave(ra.primitiveFile) == ra.primitiveStates[4])
check("owned_sprite_pixel_state", ra.mo:GetSpritePixelIndex(1, 1, 0) == 19 and a.mo:GetSpritePixelIndex(1, 1, 0) == 19)
ra.mo:SetSpritePixelIndex(1, 1, 0, 73, -1, false)
check("primitive_sprite_pixel_alias", _ScriptGraphNativeSave(ra.primitiveBitmap) ~= ra.primitiveStates[3] and _ScriptGraphNativeSave(a.primitiveBitmap) == a.primitiveStates[3])
if ra.posRef ~= nil then
	ra.posRef.X = 77
	check("engine_field_alias_kept", ra.mo.Pos.X == 77, ra.mo.Pos.X)
	check("owned_entity_storage_independent", a.mo.Pos.X == 640, a.mo.Pos.X)
else
	check("engine_field_alias_kept", false, "no reference restored")
end
-- The coroutines continue where they yielded, exactly as the originals did.
local jobRestored = resumed(ra.job, 5)
check("coroutine_continues", jobRestored == jobReference and jobRestored == "true/42/dead", jobRestored .. " vs " .. jobReference)
local openerRestored = resumed(ra.opener)
check("coroutine_open_upvalue_continues", openerRestored == openerReference and openerRestored == "true/1/suspended", openerRestored .. " vs " .. openerReference)
check("coroutine_open_upvalue_shared", tostring(ra.getter()) == openerGetterReference and ra.getter() == 1, tostring(ra.getter()))
check("coroutine_pcall_frame", resumed(ra.protected) == protectedReference and protectedReference == "true/true/dead", protectedReference)
check("coroutine_metamethod_frame", resumed(ra.indexer) == indexerReference and indexerReference == "true/foo!/dead", indexerReference)
check("coroutine_vararg_frame", resumed(ra.varargs) == varargsReference and varargsReference == "true/1/dead", varargsReference)
check("coroutine_not_started", coroutine.status(ra.fresh) == "suspended" and resumed(ra.fresh, 4) == "true/4/suspended")
check("coroutine_dead", coroutine.status(ra.dead) == "dead")
check("coroutine_wrap_continues", ra.wrapped() == wrappedReference and wrappedReference == 24)
check("coroutine_wrap_alias", ra.wrapped == ra.wrappedAlias)
check("coroutine_wrap_not_started", ra.wrappedFresh(5) == 5 and ra.wrappedFresh() == 10)
check("gmatch_continues", ra.matcher() == matcherReference and matcherReference == "two" and ra.matcher() == "three" and ra.matcher() == nil)
local okFirst, otherSecond = coroutine.resume(ra.threadPair[1])
local okSecond, otherFirst = coroutine.resume(ra.threadPair[2])
check("coroutine_cross_references", okFirst and okSecond and otherSecond == ra.threadPair[2] and otherFirst == ra.threadPair[1])
local stitchedRestored = resumed(ra.stitched)
check("coroutine_stitch_continues", stitchedRestored == stitchedReference and stitchedRestored == "true/9/suspended" and resumed(ra.stitched) == "true/10/suspended", stitchedRestored .. " vs " .. stitchedReference)
local interpretedRestored = resumed(ra.interpreted)
check("coroutine_interpreted_continues", interpretedRestored == interpretedReference and interpretedRestored == "true/9/suspended", interpretedRestored .. " vs " .. interpretedReference)

-- Every dumped closure in the captured graph must load and re-dump to the same bytes in this engine.
do
	local unstable, first, total = 0, nil, 0
	local pos = 1
	while true do
		local s, e, id, len = string.find(text, "F(%d+);Ds(%d+):", pos)
		if not s then break end
		local code = string.sub(text, e + 1, e + tonumber(len))
		local fn = loadstring(code)
		total = total + 1
		if fn and string.dump(fn, "d") ~= code then
			unstable = unstable + 1
			if not first then first = "F" .. id .. " " .. tostring(string.match(code, "@[^%z]+%.lua") or "?") end
		end
		pos = e + tonumber(len) + 1
	end
	check("dumps_roundtrip_in_engine", unstable == 0 and total > 0, tostring(unstable) .. " of " .. total .. " unstable; first " .. tostring(first))
end

-- Negative controls: a value that cannot be carried must fail the capture, never pass silently.
)lua"
    R"lua(
do
	local tooLarge, sizeError = _ScriptGraphThreadRestore({ status = "suspended", base = 10, top = 999999, slots = {}, links = {}, conts = {} })
	check("negative_coroutine_stack_limit", tooLarge == nil and type(sizeError) == "string")
	local badFrame, frameError = _ScriptGraphThreadRestore({ status = "suspended", base = 10, top = 10, slots = {}, links = { [9] = { ftsz = 0 } }, conts = {} })
	check("negative_coroutine_frame_size", badFrame == nil and type(frameError) == "string")
	local _, p1 = _ScriptGraph.serialize({ ["1"] = { bad = newproxy(true) } })
	check("negative_unsupported_userdata_fails", #p1 > 0, table.concat(p1, " | "))
	local constructed, message = pcall(function() return MOPixel() end)
	check("negative_unregistered_constructor_fails", not constructed and string.find(tostring(message), "has no Lua constructor", 1, true) ~= nil)
	local file = io.tmpfile()
	local _, p2 = _ScriptGraph.serialize({ ["1"] = { bad = file:lines() } })
	file:close()
	check("negative_nameless_native_function_fails", #p2 > 0, table.concat(p2, " | "))
	local _, p3 = _ScriptGraph.serialize({ ["1"] = { bad = debug.upvalueid(a.step, 1) } })
	check("negative_light_userdata_fails", #p3 > 0, table.concat(p3, " | "))
	local _, p4 = _ScriptGraph.deserialize("SG1;r1;s1:1#1;G0;L0;N1;T1;P-;Mz;k1;s3:bade99999999:s6:AHuman")
	check("negative_missing_entity_fails_restore", #p4 > 0, table.concat(p4, " | "))
end

-- A failed candidate restore reuses the original Lua objects. Value bytes and
-- native iterator cursors must rewind while every pre-existing alias survives.
do
	local saved = { vector = Vector(3.25, -7.5), timer = Timer(), alarm = AlarmEvent(), controller = Controller() }
	saved.vectorAlias = saved.vector; saved.timerAlias = saved.timer; saved.alarmPosition = saved.alarm.ScenePos
	saved.timer.StartSimTimeTicks = 13579; saved.timer.SimTimeLimitTicks = 24680
	saved.alarm.ScenePos = Vector(17, -29); saved.alarm.Range = 53.5; saved.alarm.Team = 1
	saved.controller:SetState(Controller.WEAPON_FIRE, true)
	saved.request = _ScriptGraphPathRequest({1, 0, 3, 7.25, 1, 2, 13, 17, 1, 2, 7, 8, 13, 17})
	saved.path = saved.request.Path; saved.path(); saved.pathAlias = saved.path
	saved.values = _ScriptGraphIteratorFromValues({11, 23, 37}, nil, 0, 3); saved.values(); saved.valuesAlias = saved.values
	local originalVector, originalTimer, originalPath, originalValues = saved.vector, saved.timer, saved.path, saved.values
	local capture, problems = _ScriptGraph.serialize({ ["held"] = saved })
	check("held_capture", #problems == 0, table.concat(problems, " | "))
	_ScriptGraph.stashObjects()
	saved.vector.X = 99; saved.timer.SimTimeLimitTicks = 999
	saved.alarmPosition.X = 99; saved.alarm.Range = 999; saved.alarm.Team = 3
	saved.controller:SetState(Controller.WEAPON_FIRE, false)
	saved.path(); saved.values(); saved.values()
	_ScriptGraphPathRequest({1, 1, 2, 99, 31, 32, 33, 34, 31, 32, 33, 34}, saved.request)
	local restored, errors = _ScriptGraph.deserialize(capture, true)
	local result = restored.held
	check("held_restore", #errors == 0 and rawequal(result, saved), table.concat(errors, " | "))
	check("held_vector_identity_and_value", rawequal(result.vector, originalVector) and result.vector.X == 3.25 and result.vector.Y == -7.5)
	check("held_timer_identity_and_value", rawequal(result.timer, originalTimer) and result.timer.StartSimTimeTicks == 13579 and result.timer.SimTimeLimitTicks == 24680)
	check("held_alarm_alias_and_value", result.alarmPosition.X == 17 and result.alarmPosition.Y == -29 and result.alarm.Range == 53.5 and result.alarm.Team == 1)
	check("held_controller_value", result.controller:IsState(Controller.WEAPON_FIRE))
	local point = result.path()
	check("held_path_request_and_iterator", result.request.PathLength == 3 and result.request.TotalCost == 7.25 and rawequal(result.path, originalPath) and point.X == 7 and point.Y == 8)
	check("held_owned_iterator_continuation", rawequal(result.values, originalValues) and result.values() == 23 and result.values() == 37 and result.values() == nil)
	_ScriptGraph.releaseObjects()
end

-- The reviewer's within-object alias sweep.
local sweepOk, sweepDetail = true, ""
for i = 1, 128 do
	local shared = { count = i }
	local keyA, keyB = "first" .. i, "second" .. i
	local original = { [keyA] = shared, [keyB] = shared }
	local t = _ScriptGraph.serialize({ ["1"] = original })
	local r = _ScriptGraph.deserialize(t)["1"]
	if r[keyA] == nil or r[keyB] == nil or r[keyA] ~= r[keyB] then
		sweepOk, sweepDetail = false, "case " .. i
		break
	end
end
check("alias_sweep_128", sweepOk, sweepDetail)
do
	local codec, actual = _ScriptGraph, getfenv(0)
	local oldGlobal, oldMeta = rawget(actual, "_G"), debug.getmetatable(actual)
	local key, laterKey = {}, {}
	local state = { shared = { value = 67 }, key = key, global = actual }
	rawset(actual, key, state.shared)
	rawset(actual, 654321, state.shared)
	rawset(actual, false, state.shared)
	rawset(actual, "_G", { shared = state.shared })
	debug.setmetatable(actual, { __metatable = "protected", __index = function(_, name)
		if name == "_CheckpointGlobalLookup" then return 67 end
		error("codec invoked a global metamethod for " .. tostring(name))
	end, __newindex = function() error("codec invoked global __newindex") end })
	local text, captureProblems = codec.serialize({ ["1"] = state })
	check("global_environment_capture", #captureProblems == 0, table.concat(captureProblems, " | "))
	rawset(actual, key, nil)
	rawset(actual, 654321, nil)
	rawset(actual, false, nil)
	rawset(actual, laterKey, "added later")
	rawset(actual, "_G", { changed = true })
	debug.setmetatable(actual, nil)
	local roots, problems = codec.deserialize(text)
	local restored = roots["1"]
	check("global_environment_restore", #problems == 0, table.concat(problems, " | "))
	check("global_environment_identity", restored.global == actual)
	check("global_nonstring_keys", rawget(actual, restored.key) == restored.shared and rawget(actual, 654321) == restored.shared and rawget(actual, false) == restored.shared)
	check("global_added_keys_removed", rawget(actual, laterKey) == nil)
	check("global_binding_rewound", rawget(actual, "_G").shared == restored.shared)
	check("global_metatable_rewound", getmetatable(actual) == "protected" and actual._CheckpointGlobalLookup == 67)
	local again, againProblems = codec.serialize(roots)
	check("global_environment_reserialize", text == again and #againProblems == 0)
	debug.setmetatable(actual, oldMeta)
	rawset(actual, "_G", oldGlobal)
	rawset(actual, key, nil)
	rawset(actual, restored.key, nil)
	rawset(actual, 654321, nil)
	rawset(actual, false, nil)
	rawset(actual, laterKey, nil)
end
do
	local text, captureProblems = _ScriptGraph.serialize({})
	check("strict_graph_valid_current", pcall(_ScriptGraph.validate, text))
	check("strict_graph_valid_start_offset_index", pcall(_ScriptGraph.validate, "SG1;r0;G0;L0;N1;U1;kz;n-1;t;"))
	check("strict_graph_valid_top_soundset_index", pcall(_ScriptGraph.validate, "SG1;r0;G0;L0;N1;U1;jz;n-1;"))
	local invalid = {
		truncated = string.sub(text, 1, #text - 1),
		trailing = text .. "x",
		wrong_section = "SG3;r0;Q0;L0;E0;Rz;N0;",
		negative_length = "SG1;r1;s-1:1n12;G0;L0;N0;",
		truncated_string = "SG1;r1;s999999:1",
		fractional_count = "SG1;r0.5;G0;L0;N0;",
		missing_node = "SG1;r1;s1:1#1;G0;L0;N0;",
		duplicate_node = "SG1;r0;G0;L0;N2;T1;P-;Mz;k0;T1;P-;Mz;k0;",
		wrong_cell_kind = "SG1;r0;G0;L0;N1;F1;Rg1;s4:typeEz;u1;c1;",
		unknown_factory = "SG1;r0;G0;L0;N1;B1;s7:missingu0;",
		wrong_scalar_tag = "SG1;r0;G0;L0;N1;U1;cz;n0;n0;n0;",
		invalid_limb_vector_index = "SG1;r0;G0;L0;N1;U1;kz;n-2;t;",
		invalid_soundset_index = "SG1;r0;G0;L0;N1;U1;jz;n-2;",
		bad_rng = "SG3;r0;G0;L0;E0;Rs1:xN0;"
	}
	for name, graph in pairs(invalid) do check("strict_graph_reject_" .. name, not pcall(_ScriptGraph.validate, graph)) end
	for _, version in ipairs({ "SG1", "SG2" }) do
		local old = version .. ";r1;s1:1n12;G0;L0;" .. (version == "SG2" and "E0;" or "") .. "N0;"
		check("strict_graph_valid_" .. version, pcall(_ScriptGraph.validate, old))
		local roots, problems = _ScriptGraph.deserialize(old)
		check("legacy_graph_" .. version, roots["1"] == 12 and #problems == 0)
	end
	local _, badRNG = _ScriptGraph.deserialize("SG3;r0;G0;L0;E0;Rs1:xN0;")
	check("negative_vm_rng_checkpoint", #badRNG > 0)
	local expected = {}
	for i = 1, 16 do expected[i] = LuaMan:SelectRand(0, 1000000) end
	for i = 1, 2000 do LuaMan:SelectRand(0, 1000000) end
	local _, restoreProblems = _ScriptGraph.deserialize(text)
	local same = #captureProblems == 0 and #restoreProblems == 0
	for i = 1, #expected do same = LuaMan:SelectRand(0, 1000000) == expected[i] and same end
	check("native_vm_rng_rewound", same)
end
_SelfTestShared, _SelfTestMod, _SelfTestKlass = nil, nil, nil
_G["selftest.lua"] = nil
return table.concat(results, "\n")

)lua";

// Whether a luabind class or one of its bases carries the name.
static bool ClassDerivesFrom(const luabind::detail::class_rep* rep, const char* name) {
	if (!rep) {
		return false;
	}
	if (std::strcmp(rep->name(), name) == 0) {
		return true;
	}
	for (const luabind::detail::class_rep::base_info& base: rep->bases()) {
		if (ClassDerivesFrom(base.base, name)) {
			return true;
		}
	}
	return false;
}

// The engine Vectors a script can alias through a property, by address, for the capture in progress.
struct VectorField {
	long uid;
	const char* property;
};
static std::unordered_map<const void*, VectorField> s_VectorFields;
static std::unordered_map<const void*, long> s_ControllerOwners;

static int ScriptGraphBeginCapture(lua_State* L) {
	s_VectorFields.clear();
	s_ControllerOwners.clear();
	for (MovableObject* mo: g_MovableMan.SnapshotKnownObjects()) {
		const long uid = mo->GetUniqueID();
		if (Actor* actor = dynamic_cast<Actor*>(mo)) {
			s_ControllerOwners[actor->GetController()] = uid;
		}
		s_VectorFields[&mo->GetPos()] = {uid, "Pos"};
		s_VectorFields[&mo->GetVel()] = {uid, "Vel"};
		s_VectorFields[&mo->GetPrevPos()] = {uid, "PrevPos"};
		s_VectorFields[&mo->GetPrevVel()] = {uid, "PrevVel"};
		if (const MOSRotating* rotating = dynamic_cast<const MOSRotating*>(mo)) {
			s_VectorFields[&rotating->GetRecoilForce()] = {uid, "RecoilForce"};
			s_VectorFields[&rotating->GetRecoilOffset()] = {uid, "RecoilOffset"};
		}
		if (const Attachable* attachable = dynamic_cast<const Attachable*>(mo)) {
			s_VectorFields[&attachable->GetParentOffset()] = {uid, "ParentOffset"};
			s_VectorFields[&attachable->GetJointOffset()] = {uid, "JointOffset"};
			s_VectorFields[&attachable->GetJointPos()] = {uid, "JointPos"};
		}
	}
	return 0;
}

static int ScriptGraphEndCapture(lua_State* L) {
	s_VectorFields.clear();
	s_ControllerOwners.clear();
	return 0;
}

static int FindActorLimb(MovableObject* object, const LimbPath* path) {
	if (AHuman* human = dynamic_cast<AHuman*>(object)) {
		return human->GetLimbPathIndex(path);
	}
	if (ACrab* crab = dynamic_cast<ACrab*>(object)) {
		return crab->GetLimbPathIndex(path);
	}
	return -1;
}

static int ScriptGraphValueIteratorNext(lua_State* L) {
	const int index = lua_tointeger(L, lua_upvalueindex(3)) + 1;
	lua_getfield(L, lua_upvalueindex(1), "count");
	const int count = lua_tointeger(L, -1);
	lua_pop(L, 1);
	if (index <= count) {
		lua_rawgeti(L, lua_upvalueindex(1), index);
		lua_pushinteger(L, index);
		lua_replace(L, lua_upvalueindex(3));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

static int ScriptGraphIteratorFromValues(lua_State* L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_pushvalue(L, 4);
	lua_setfield(L, 1, "count");
	lua_pushvalue(L, 1);
	lua_pushvalue(L, 2);
	lua_pushinteger(L, 0);
	lua_pushvalue(L, 3);
	lua_pushcclosure(L, ScriptGraphValueIteratorNext, 4);
	return 1;
}

static lua_CFunction ScriptGraphIteratorHook(lua_State* L, int index, const char* name) {
	const int top = lua_gettop(L);
	if (lua_iscfunction(L, index) && lua_getupvalue(L, index, 1) && lua_isuserdata(L, -1) && lua_getmetatable(L, -1)) {
		lua_getfield(L, -1, name);
		lua_CFunction hook = lua_tocfunction(L, -1);
		lua_pop(L, 2);
		if (hook) return hook;
	}
	lua_settop(L, top);
	return nullptr;
}

static int ScriptGraphIteratorSnapshot(lua_State* L) {
	if (lua_tocfunction(L, 1) == ScriptGraphValueIteratorNext) {
		lua_getupvalue(L, 1, 1);
		const int values = lua_gettop(L);
		lua_getfield(L, values, "count");
		const int count = lua_tointeger(L, -1);
		lua_getupvalue(L, 1, 3);
		const int index = lua_tointeger(L, -1);
		lua_getupvalue(L, 1, 4);
		const int first = lua_tointeger(L, -1);
		lua_newtable(L);
		lua_pushboolean(L, true); lua_setfield(L, -2, "owned");
		lua_pushinteger(L, first + index); lua_setfield(L, -2, "first");
		lua_pushinteger(L, count - index); lua_setfield(L, -2, "count");
		lua_newtable(L);
		for (int item = index + 1; item <= count; ++item) {
			lua_rawgeti(L, values, item);
			lua_rawseti(L, -2, item - index);
		}
		lua_setfield(L, -2, "values");
	} else if (lua_CFunction snapshot = ScriptGraphIteratorHook(L, 1, "__iterator_snapshot")) {
		const int range = lua_gettop(L);
		lua_pushcfunction(L, snapshot);
		lua_pushvalue(L, range);
		if (!lua_getupvalue(L, 1, 2)) lua_pushnil(L);
		lua_call(L, 2, 1);
		if (!lua_getupvalue(L, 1, 3)) lua_pushnil(L);
		lua_setfield(L, -2, "creator");
		if (!lua_getupvalue(L, 1, 4)) lua_pushnil(L);
		lua_setfield(L, -2, "args");
	} else {
		return 0;
	}
	if (!lua_getupvalue(L, 1, 2)) lua_pushnil(L);
	lua_setfield(L, -2, "owner");
	return 1;
}

static int ScriptGraphIteratorRewind(lua_State* L) {
	const int first = luaL_checkinteger(L, 2), last = luaL_checkinteger(L, 3);
	if (lua_tocfunction(L, 1) == ScriptGraphValueIteratorNext) {
		lua_getupvalue(L, 1, 4);
		const int origin = lua_tointeger(L, -1);
		lua_getupvalue(L, 1, 1); lua_getfield(L, -1, "count");
		const int count = lua_tointeger(L, -1);
		const bool valid = first >= origin && last == origin + count && first <= last;
		if (valid) { lua_pushinteger(L, first - origin); lua_setupvalue(L, 1, 3); }
		lua_pushboolean(L, valid);
	} else if (lua_CFunction seek = ScriptGraphIteratorHook(L, 1, "__iterator_seek")) {
		const int range = lua_gettop(L);
		lua_pushcfunction(L, seek); lua_pushvalue(L, range); lua_pushinteger(L, first); lua_pushinteger(L, last); lua_call(L, 3, 1);
	} else lua_pushboolean(L, false);
	return 1;
}

static int ScriptGraphIteratorRestore(lua_State* L) {
	const int first = lua_tointeger(L, 3), last = lua_tointeger(L, 4);
	const auto fail = [L](const char* message) { lua_pushnil(L); lua_pushstring(L, message); return 2; };
	auto* owner = luabind::detail::is_class_object(L, 1);
	if (!owner || !owner->crep()) return fail("the range owner is missing");
	if (lua_type(L, 2) == LUA_TSTRING) {
		const auto& properties = owner->crep()->properties();
		const auto property = properties.find(lua_tostring(L, 2));
		if (property == properties.end()) return fail("the range property is missing");
		property->second.func(L, property->second.pointer_offset);
	} else if (lua_iscfunction(L, 2)) {
		const int count = lua_istable(L, 5) ? static_cast<int>(lua_objlen(L, 5)) : 0;
		lua_pushvalue(L, 2);
		lua_pushvalue(L, 1);
		for (int index = 1; index <= count; ++index) lua_rawgeti(L, 5, index);
		lua_call(L, count + 1, 1);
	} else {
		return fail("the range getter is missing");
	}
	const int function = lua_gettop(L);
	lua_CFunction seek = ScriptGraphIteratorHook(L, function, "__iterator_seek");
	if (!seek) return fail("the getter returned no native iterator");
	const int range = lua_gettop(L);
	lua_pushcfunction(L, seek);
	lua_pushvalue(L, range);
	lua_pushinteger(L, first);
	lua_pushinteger(L, last);
	lua_call(L, 3, 1);
	if (!lua_toboolean(L, -1)) return fail("the saved position exceeds the range");
	lua_pushvalue(L, 2); lua_setupvalue(L, function, 3);
	lua_pushvalue(L, 5); lua_setupvalue(L, function, 4);
	lua_pushvalue(L, function);
	return 1;
}

static int ScriptGraphGibOwner(lua_State* L) {
	auto* rep = luabind::detail::is_class_object(L, 1);
	if (!rep || !rep->get_dependencies().is_valid()) return 0;
	rep->get_dependencies().get(L);
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		auto* owner = luabind::detail::is_class_object(L, -1);
		if (owner && ClassDerivesFrom(owner->crep(), "MOSRotating")) {
			const auto& gibs = *static_cast<MOSRotating*>(owner->ptr())->GetGibList();
			const auto found = std::find(gibs.begin(), gibs.end(), rep->ptr());
			if (found != gibs.end()) {
				lua_pushinteger(L, std::distance(gibs.begin(), found));
				return 2;
			}
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int ScriptGraphGib(lua_State* L) {
	auto* owner = luabind::detail::is_class_object(L, 1);
	const int index = lua_tointeger(L, 2);
	if (owner && ClassDerivesFrom(owner->crep(), "MOSRotating")) {
		const auto& gibs = *static_cast<MOSRotating*>(owner->ptr())->GetGibList();
		if (index >= 0 && static_cast<size_t>(index) < gibs.size()) {
			auto found = gibs.begin();
			std::advance(found, index);
			luabind::object(L, *found).push(L);
			luabind::detail::is_class_object(L, -1)->add_dependency(L, 1);
			return 1;
		}
	}
	lua_pushnil(L);
	return 1;
}

static int ScriptGraphProperty(lua_State* L) {
	auto* owner = luabind::detail::is_class_object(L, 1);
	const bool constant = lua_toboolean(L, 3);
	if (owner && owner->crep() && lua_type(L, 2) == LUA_TSTRING) {
		const auto& properties = owner->crep()->properties();
		const auto property = properties.find(lua_tostring(L, 2));
		if (property != properties.end()) {
			property->second.func(L, property->second.pointer_offset);
			if (auto* value = luabind::detail::is_class_object(L, -1); value && !(value->flags() & luabind::detail::object_rep::owner)) {
				value->set_flags((value->flags() & ~luabind::detail::object_rep::constant) | (constant ? luabind::detail::object_rep::constant : 0));
				value->add_dependency(L, 1);
				return 1;
			}
		}
	}
	lua_pushnil(L);
	return 1;
}

static int ScriptGraphPropertyOwner(lua_State* L) {
	auto* rep = luabind::detail::is_class_object(L, 1);
	if (!rep) return 0;
	lua_newtable(L);
	const int candidates = lua_gettop(L);
	int candidateCount = 0;
	if (rep->get_dependencies().is_valid()) {
		rep->get_dependencies().get(L);
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			lua_pushvalue(L, -1);
			lua_rawseti(L, candidates, ++candidateCount);
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	if (auto* activity = dynamic_cast<GameActivity*>(g_ActivityMan.GetActivity())) {
		luabind::object(L, activity).push(L);
		lua_rawseti(L, candidates, ++candidateCount);
		for (int player = 0; player < Players::MaxPlayerCount; ++player) {
			if (auto* editor = activity->GetEditorGUI(player); editor && editor->GetCurrentObject()) {
				luabind::object(L, editor->GetCurrentObject()).push(L);
				lua_rawseti(L, candidates, ++candidateCount);
			}
		}
	}
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		auto* owner = luabind::detail::is_class_object(L, -1);
		std::vector<const char*> names;
		if (owner && owner->crep()) {
			if (std::strcmp(owner->crep()->name(), "Gib") == 0) names = {"Offset"};
			else if (std::strcmp(owner->crep()->name(), "AlarmEvent") == 0) names = {"ScenePos"};
			else if (ClassDerivesFrom(owner->crep(), "GameActivity")) names = {"CursorTimer", "GameTimer", "GameOverTimer"};
			else if (ClassDerivesFrom(owner->crep(), "SceneObject")) names = {"Pos"};
		}
		for (const char* propertyName: names) {
			const int ownerIndex = lua_gettop(L);
			lua_pushcfunction(L, ScriptGraphProperty);
			lua_pushvalue(L, ownerIndex);
			lua_pushstring(L, propertyName);
			lua_pushboolean(L, (rep->flags() & luabind::detail::object_rep::constant) != 0);
			lua_call(L, 3, 1);
			auto* property = luabind::detail::is_class_object(L, -1);
			const bool same = property && property->ptr() == rep->ptr();
			lua_pop(L, 1);
			if (same) {
				lua_pushstring(L, propertyName);
				lua_pushboolean(L, (rep->flags() & luabind::detail::object_rep::constant) != 0);
				return 3;
			}
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int ScriptGraphGibReferences(lua_State* L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	std::unordered_set<const MovableObject*> seen;
	std::map<long, const MOSRotating*> owners;
	std::function<void(const MovableObject*)> collect = [&](const MovableObject* object) {
		if (!object || !seen.insert(object).second) return;
		if (const auto* rotating = dynamic_cast<const MOSRotating*>(object)) {
			if (object->GetUniqueID() > 0) owners.emplace(object->GetUniqueID(), rotating);
			for (const Attachable* part: rotating->GetAttachables()) collect(part);
			for (const AEmitter* wound: rotating->GetWoundList()) collect(wound);
		}
		if (const auto* actor = dynamic_cast<const Actor*>(object)) {
			for (const MovableObject* item: *actor->GetInventory()) collect(item);
		}
		if (const auto* craft = dynamic_cast<const ACraft*>(object)) for (const MovableObject* item: craft->GetCollectedInventory()) collect(item);
	};
	for (const MovableObject* object: g_MovableMan.SnapshotKnownObjects()) {
		if (g_MovableMan.ValidMO(object)) collect(object);
	}
	lua_pushnil(L);
	while (lua_next(L, 1)) {
		const auto* rep = luabind::detail::is_class_object(L, -1);
		if (rep && ClassDerivesFrom(rep->crep(), "MovableObject")) collect(static_cast<const MovableObject*>(rep->ptr()));
		lua_pop(L, 1);
	}
	lua_newtable(L);
	const int references = lua_gettop(L);
	int count = 0;
	for (const auto& [uid, owner]: owners) {
		int index = 0;
		for (const Gib* gib: *owner->GetGibList()) {
			const MovableObject* particle = gib->GetParticlePreset();
			if (particle && !particle->IsOriginalPreset() && particle->GetUniqueID() == 0) {
				lua_pushlightuserdata(L, const_cast<MovableObject*>(particle));
				lua_rawget(L, 1);
				if (!lua_isnil(L, -1)) {
					lua_newtable(L);
					lua_pushnumber(L, static_cast<lua_Number>(uid));
					lua_setfield(L, -2, "owner");
					lua_pushinteger(L, index);
					lua_setfield(L, -2, "index");
					lua_pushvalue(L, -2);
					lua_setfield(L, -2, "target");
					lua_rawseti(L, references, ++count);
				}
				lua_pop(L, 1);
			}
			++index;
		}
	}
	return 1;
}

static int ScriptGraphRestoreGibReference(lua_State* L) {
	const auto* target = luabind::detail::is_class_object(L, 3);
	auto* owner = dynamic_cast<MOSRotating*>(g_MovableMan.FindObjectByUniqueID(static_cast<long>(lua_tonumber(L, 1))));
	const int index = lua_tointeger(L, 2);
	if (owner && target && ClassDerivesFrom(target->crep(), "MovableObject") && index >= 0 && static_cast<size_t>(index) < owner->GetGibList()->size()) {
		(*std::next(owner->GetGibList()->begin(), index))->SetParticlePreset(static_cast<const MovableObject*>(target->ptr()));
		lua_pushboolean(L, true);
	} else {
		lua_pushboolean(L, false);
	}
	return 1;
}

static int ScriptGraphSoundSetOwner(lua_State* L) {
	auto* rep = luabind::detail::is_class_object(L, 1);
	if (!rep || !rep->get_dependencies().is_valid()) {
		return 0;
	}
	rep->get_dependencies().get(L);
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		auto* owner = luabind::detail::is_class_object(L, -1);
		if (owner && owner->crep()) {
			if (std::strcmp(owner->crep()->name(), "SoundContainer") == 0 && &static_cast<SoundContainer*>(owner->ptr())->GetTopLevelSoundSet() == rep->ptr()) {
				lua_pushinteger(L, -1);
				return 2;
			}
			if (std::strcmp(owner->crep()->name(), "SoundSet") == 0) {
				const auto& sets = static_cast<SoundSet*>(owner->ptr())->GetSubSoundSets();
				const auto found = std::find(sets.begin(), sets.end(), rep->ptr());
				if (found != sets.end()) {
					lua_pushinteger(L, std::distance(sets.begin(), found));
					return 2;
				}
			}
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int ScriptGraphSoundSet(lua_State* L) {
	auto* owner = luabind::detail::is_class_object(L, 1);
	const int index = lua_tointeger(L, 2);
	SoundSet* soundSet = nullptr;
	if (owner && owner->crep()) {
		if (index == -1 && std::strcmp(owner->crep()->name(), "SoundContainer") == 0) {
			soundSet = &static_cast<SoundContainer*>(owner->ptr())->GetTopLevelSoundSet();
		} else if (index >= 0 && std::strcmp(owner->crep()->name(), "SoundSet") == 0) {
			const auto& sets = static_cast<SoundSet*>(owner->ptr())->GetSubSoundSets();
			if (static_cast<size_t>(index) < sets.size()) soundSet = sets[index];
		}
	}
	if (soundSet) {
		luabind::object(L, soundSet).push(L);
		luabind::detail::is_class_object(L, -1)->add_dependency(L, 1);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

static int ScriptGraphLimbOwner(lua_State* L) {
	auto* rep = luabind::detail::is_class_object(L, 1);
	if (!rep || !rep->crep() || std::strcmp(rep->crep()->name(), "LimbPath") != 0) {
		return 0;
	}
	const auto* path = static_cast<const LimbPath*>(rep->ptr());
	if (rep->get_dependencies().is_valid()) {
		rep->get_dependencies().get(L);
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			auto* owner = luabind::detail::is_class_object(L, -1);
			if (owner && ClassDerivesFrom(owner->crep(), "MovableObject")) {
				if (const int index = FindActorLimb(static_cast<MovableObject*>(owner->ptr()), path); index >= 0) {
					lua_pushinteger(L, index);
					return 2;
				}
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	for (MovableObject* object: g_MovableMan.SnapshotKnownObjects()) {
		if (const int index = FindActorLimb(object, path); index >= 0) {
			luabind::object(L, object).push(L);
			lua_pushinteger(L, index);
			return 2;
		}
	}
	return 0;
}

static int ScriptGraphActorLimb(lua_State* L) {
	auto* owner = luabind::detail::is_class_object(L, 1);
	const int index = lua_tointeger(L, 2);
	LimbPath* path = nullptr;
	if (owner && ClassDerivesFrom(owner->crep(), "MovableObject")) {
		auto* object = static_cast<MovableObject*>(owner->ptr());
		if (AHuman* human = dynamic_cast<AHuman*>(object)) {
			path = human->GetLimbPathByIndex(index);
		} else if (ACrab* crab = dynamic_cast<ACrab*>(object)) {
			path = crab->GetLimbPathByIndex(index);
		}
	}
	if (path) {
		luabind::object(L, path).push(L);
		luabind::detail::is_class_object(L, -1)->add_dependency(L, 1);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

static int ScriptGraphLimbVectorOwner(lua_State* L) {
	auto* rep = luabind::detail::is_class_object(L, 1);
	if (!rep || !rep->get_dependencies().is_valid()) {
		return 0;
	}
	rep->get_dependencies().get(L);
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		auto* owner = luabind::detail::is_class_object(L, -1);
		if (owner && owner->crep() && std::strcmp(owner->crep()->name(), "LimbPath") == 0) {
			auto* path = static_cast<LimbPath*>(owner->ptr());
			int index = -1;
			if (&path->GetStartOffset() != rep->ptr()) {
				for (index = 0; index < path->GetSegCount(); ++index) {
					if (path->GetSegment(index) == rep->ptr()) {
						break;
					}
				}
			}
			if (index < path->GetSegCount()) {
				lua_pushinteger(L, index);
				lua_pushboolean(L, (rep->flags() & luabind::detail::object_rep::constant) != 0);
				return 3;
			}
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int ScriptGraphLimbVector(lua_State* L) {
	auto* owner = luabind::detail::is_class_object(L, 1);
	const int index = lua_tointeger(L, 2);
	Vector* vector = nullptr;
	if (owner && owner->crep() && std::strcmp(owner->crep()->name(), "LimbPath") == 0) {
		auto* path = static_cast<LimbPath*>(owner->ptr());
		if (index == -1) {
			vector = const_cast<Vector*>(&path->GetStartOffset());
		} else if (index >= 0 && index < path->GetSegCount()) {
			vector = path->GetSegment(index);
		}
	}
	if (vector) {
		if (lua_toboolean(L, 3)) {
			luabind::object(L, static_cast<const Vector*>(vector)).push(L);
		} else {
			luabind::object(L, vector).push(L);
		}
		luabind::detail::is_class_object(L, -1)->add_dependency(L, 1);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

static int ScriptGraphObjectAddress(lua_State* L) {
	lua_pushnumber(L, static_cast<lua_Number>(reinterpret_cast<uintptr_t>(lua_topointer(L, 1))));
	return 1;
}

static int ScriptGraphNativeAddress(lua_State* L) {
	const auto* object = luabind::detail::is_class_object(L, 1);
	if (object) lua_pushlightuserdata(L, object->ptr());
	else lua_pushnil(L);
	return 1;
}

static int ScriptGraphMembers(lua_State* L) {
	if (const auto* object = luabind::detail::is_class_object(L, 1); object && object->crep()) {
		lua_newtable(L);
		const int result = lua_gettop(L);
		auto merge = [&]() {
			const int source = lua_gettop(L);
			lua_pushnil(L);
			while (lua_next(L, source) != 0) {
				lua_pushvalue(L, -2);
				lua_pushvalue(L, -2);
				lua_rawset(L, result);
				lua_pop(L, 1);
			}
			lua_pop(L, 1);
		};
		object->crep()->get_table(L);
		merge();
		if (object->get_lua_table().is_valid()) {
			object->get_lua_table().get(L);
			merge();
		}
	} else if (luabind::detail::is_class_rep(L, 1)) {
		static_cast<luabind::detail::class_rep*>(lua_touserdata(L, 1))->get_table(L);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

static int ScriptGraphInstance(lua_State* L) {
	const auto* object = luabind::detail::is_class_object(L, 1);
	if (object && object->get_lua_table().is_valid()) {
		object->get_lua_table().get(L);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

static int ScriptGraphSetInstance(lua_State* L) {
	auto* object = luabind::detail::is_class_object(L, 1);
	const bool valid = lua_isnil(L, 2) || (object && lua_istable(L, 2));
	if (object && valid) {
		if (lua_isnil(L, 2)) {
			object->get_lua_table().reset();
		} else {
			lua_pushvalue(L, 2);
			object->get_lua_table().set(L);
		}
	}
	lua_pushboolean(L, valid);
	return 1;
}

static int ScriptGraphAreaBoxes(lua_State* L) {
	auto* rep = luabind::detail::is_class_object(L, 1);
	lua_newtable(L);
	if (rep && rep->crep() && std::strcmp(rep->crep()->name(), "Area") == 0) {
		int index = 0;
		for (Box* box: static_cast<Scene::Area*>(rep->ptr())->GetBoxes()) {
			lua_pushlightuserdata(L, box);
			lua_rawseti(L, -2, ++index);
		}
	}
	return 1;
}

static int ScriptGraphAreaBox(lua_State* L) {
	auto* rep = luabind::detail::is_class_object(L, 1);
	const int index = lua_tointeger(L, 2);
	if (rep && rep->crep() && std::strcmp(rep->crep()->name(), "Area") == 0) {
		const auto& boxes = static_cast<Scene::Area*>(rep->ptr())->GetBoxes();
		if (index > 0 && static_cast<size_t>(index) <= boxes.size()) {
			if (lua_toboolean(L, 3)) {
				luabind::object(L, static_cast<const Box*>(boxes[index - 1])).push(L);
			} else {
				luabind::object(L, boxes[index - 1]).push(L);
			}
			luabind::detail::is_class_object(L, -1)->add_dependency(L, 1);
			return 1;
		}
	}
	lua_pushnil(L);
	return 1;
}

static int ScriptGraphSceneBoxOwner(lua_State* L) {
	auto* rep = luabind::detail::is_class_object(L, 1);
	if (!rep || !rep->crep() || std::strcmp(rep->crep()->name(), "Box") != 0) {
		return 0;
	}
	const auto find = [rep](const Scene::Area* area) {
		const auto& boxes = area->GetBoxes();
		const auto box = std::find(boxes.begin(), boxes.end(), rep->ptr());
		return box == boxes.end() ? 0 : static_cast<int>(std::distance(boxes.begin(), box) + 1);
	};
	if (rep->get_dependencies().is_valid()) {
		rep->get_dependencies().get(L);
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			auto* owner = luabind::detail::is_class_object(L, -1);
			if (owner && owner->crep() && std::strcmp(owner->crep()->name(), "Area") == 0) {
				if (const int index = find(static_cast<Scene::Area*>(owner->ptr()))) {
					lua_pushinteger(L, index);
					return 2;
				}
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	if (const Scene* scene = g_SceneMan.GetScene()) {
		for (Scene::Area* area: scene->GetAreas()) {
			if (const int index = find(area)) {
				luabind::object(L, area).push(L);
				lua_pushinteger(L, index);
				return 2;
			}
		}
	}
	return 0;
}

// What a bound object is to the script graph: a Lua-owned Vector or Timer by value, a Vector reference by the
// object and property it aliases, a known object by unique id, the activity, the scene, a preset by name, a
// Lua-owned entity copy by class and preset; a reference to an object that is gone reports as invalid.
static int ScriptGraphPathQueueSelfTest(lua_State* L) {
	Scene* scene = g_SceneMan.GetScene();
	if (!scene) return luaL_error(L, "path queue test requires a scene");
	auto& pool = g_ThreadMan.GetBackgroundThreadPool();
	pool.pause();
	pool.wait_for_tasks();
	auto& finder = scene->GetPathFinder(Activity::Teams::NoTeam);
	std::atomic<int> callbacks{0};
	const auto request = finder.CalculatePathAsync(Vector(32, 32), Vector(160, 32), 0, 1,
	    [&callbacks](std::shared_ptr<volatile PathRequest>) { ++callbacks; });
	const bool queued = !request->complete && finder.GetCurrentPathingRequests() == 1;
	pool.unpause();
	pool.wait_for_tasks();
	const bool completed = request->complete && callbacks.load() == 1 && finder.GetCurrentPathingRequests() == 0;
	std::cout << "[path-queue-selftest] " << (queued && completed ? "PASS" : "FAIL") << " queued=" << queued << " completed=" << completed << std::endl;
	auto* heldFinder = new PathFinder(96);
	pool.pause();
	pool.wait_for_tasks();
	const auto heldRequest = heldFinder->CalculatePathAsync(Vector(32, 32), Vector(160, 32), 0, 1);
	std::promise<void> entered;
	std::promise<void> destroyed;
	auto destroyedFuture = destroyed.get_future();
	std::thread destroyer([heldFinder, &entered, &destroyed]() { entered.set_value(); delete heldFinder; destroyed.set_value(); });
	entered.get_future().wait();
	if (destroyedFuture.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready) {
		std::cout << "[path-queue-selftest] FAIL destroyed_with_queued_request" << std::endl;
		std::_Exit(1);
	}
	pool.unpause();
	destroyer.join();
	pool.wait_for_tasks();
	const bool lifetime = heldRequest->complete;
	std::cout << "[path-queue-selftest] " << (lifetime ? "PASS" : "FAIL") << " queued_request_lifetime" << std::endl;
	lua_pushboolean(L, queued && completed && lifetime);
	return 1;
}

static int ScriptGraphPathRequest(lua_State* L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	const size_t count = lua_objlen(L, 1);
	if (count < 8 || count % 2 != 0) return luaL_error(L, "invalid path request checkpoint");
	const auto number = [L](int index) {
		lua_rawgeti(L, 1, index);
		const lua_Number value = luaL_checknumber(L, -1);
		lua_pop(L, 1);
		return value;
	};
	PathRequest request;
	request.complete = number(1) != 0;
	request.status = static_cast<int>(number(2));
	request.pathLength = static_cast<float>(number(3));
	request.totalCost = static_cast<float>(number(4));
	request.startPos.m_X = static_cast<float>(number(5));
	request.startPos.m_Y = static_cast<float>(number(6));
	request.targetPos.m_X = static_cast<float>(number(7));
	request.targetPos.m_Y = static_cast<float>(number(8));
	for (int index = 9; static_cast<size_t>(index) < count; index += 2) {
		const float x = static_cast<float>(number(index));
		const float y = static_cast<float>(number(index + 1));
		request.path.emplace_back(x, y);
	}
	auto* held = luabind::detail::is_class_object(L, 2);
	if (held && held->crep() && std::strcmp(held->crep()->name(), "PathRequest") == 0) {
		*static_cast<PathRequest*>(held->ptr()) = std::move(request);
		lua_pushvalue(L, 2);
	} else luabind::object(L, request).push(L);
	return 1;
}

static int ScriptGraphControllerState(lua_State* L) {
	auto* rep = luabind::detail::is_class_object(L, 1);
	Controller* controller = rep && rep->crep() && std::strcmp(rep->crep()->name(), "Controller") == 0 ? static_cast<Controller*>(rep->ptr()) : nullptr;
	if (lua_type(L, 2) == LUA_TSTRING) {
		size_t length;
		const char* text = lua_tolstring(L, 2, &length);
		Controller validation;
		lua_pushboolean(L, (controller ? controller : &validation)->LoadCheckpoint(std::string_view(text, length)));
	} else if (controller) {
		const std::string text = controller->SaveCheckpoint();
		lua_pushlstring(L, text.data(), text.size());
	} else lua_pushnil(L);
	return 1;
}

static int ScriptGraphControllerActor(lua_State* L) {
	auto* rep = luabind::detail::is_class_object(L, 1);
	auto* actor = luabind::detail::is_class_object(L, 2);
	const bool valid = rep && rep->crep() && std::strcmp(rep->crep()->name(), "Controller") == 0 &&
		(lua_isnil(L, 2) || (actor && ClassDerivesFrom(actor->crep(), "Actor")));
	if (valid) static_cast<Controller*>(rep->ptr())->SetControlledActor(actor ? static_cast<Actor*>(actor->ptr()) : nullptr);
	lua_pushboolean(L, valid);
	return 1;
}

template <class T> static bool ApplyOwnerCheckpoint(void* pointer, std::string_view checkpoint) {
	if (!pointer) { T validator; return validator.LoadCheckpoint(checkpoint, true); }
	auto& value = *static_cast<T*>(pointer);
	return value.SaveCheckpoint() == checkpoint || value.LoadCheckpoint(checkpoint);
}

static int ScriptGraphOwnerState(lua_State* L) {
	auto* rep = luabind::detail::is_class_object(L, 1);
	const char* type = luaL_checkstring(L, 2);
	size_t length;
	const char* saved = luaL_checklstring(L, 3, &length);
	const std::string_view checkpoint(saved, length);
	void* pointer = rep && rep->crep() && std::strcmp(rep->crep()->name(), type) == 0 ? rep->ptr() : nullptr;
	if (rep && !pointer) { lua_pushboolean(L, false); return 1; }
	bool valid = false;
	try {
		if (std::strcmp(type, "Controller") == 0) valid = ApplyOwnerCheckpoint<Controller>(pointer, checkpoint);
		else if (std::strcmp(type, "BuyMenuGUI") == 0) valid = ApplyOwnerCheckpoint<BuyMenuGUI>(pointer, checkpoint);
		else if (std::strcmp(type, "SceneEditorGUI") == 0) valid = ApplyOwnerCheckpoint<SceneEditorGUI>(pointer, checkpoint);
		else if (std::strcmp(type, "GUIBanner") == 0) valid = ApplyOwnerCheckpoint<GUIBanner>(pointer, checkpoint);
		else if (std::strcmp(type, "SLBackground") == 0) valid = ApplyOwnerCheckpoint<SLBackground>(pointer, checkpoint);
	} catch (const std::exception&) { valid = false; }
	lua_pushboolean(L, valid);
	return 1;
}

static int ScriptGraphOwnerReference(lua_State* L) {
	auto* owner = luabind::detail::is_class_object(L, 1);
	const char* property = luaL_checkstring(L, 2);
	const int index = luaL_checkinteger(L, 3);
	const bool constant = lua_toboolean(L, 4);
	const auto push = [&](auto* value) {
		if (!value) { lua_pushnil(L); return 1; }
		luabind::object(L, value).push(L);
		if (auto* rep = luabind::detail::is_class_object(L, -1)) {
			rep->set_flags((rep->flags() & ~luabind::detail::object_rep::constant) | (constant ? luabind::detail::object_rep::constant : 0));
			rep->add_dependency(L, 1);
		}
		return 1;
	};
	if (owner && owner->crep() && index >= 0) {
		if (std::strcmp(owner->crep()->name(), "PrimitiveManager") == 0) {
			if (std::strcmp(property, "primitive") == 0) {
				auto* primitive = g_PrimitiveMan.GetCheckpointPrimitive(index);
				if (!primitive) return push(primitive);
				switch (primitive->GetPrimitiveType()) {
#define PUSH_QUEUED_PRIMITIVE(name) case GraphicalPrimitive::PrimitiveType::name: return push(static_cast<name##Primitive*>(primitive))
					PUSH_QUEUED_PRIMITIVE(Line); PUSH_QUEUED_PRIMITIVE(Arc); PUSH_QUEUED_PRIMITIVE(Spline);
					PUSH_QUEUED_PRIMITIVE(Box); PUSH_QUEUED_PRIMITIVE(BoxFill); PUSH_QUEUED_PRIMITIVE(RoundedBox); PUSH_QUEUED_PRIMITIVE(RoundedBoxFill);
					PUSH_QUEUED_PRIMITIVE(Circle); PUSH_QUEUED_PRIMITIVE(CircleFill); PUSH_QUEUED_PRIMITIVE(Ellipse); PUSH_QUEUED_PRIMITIVE(EllipseFill);
					PUSH_QUEUED_PRIMITIVE(Triangle); PUSH_QUEUED_PRIMITIVE(TriangleFill); PUSH_QUEUED_PRIMITIVE(Text); PUSH_QUEUED_PRIMITIVE(Bitmap);
#undef PUSH_QUEUED_PRIMITIVE
					default: return push(primitive);
				}
			}
			if (std::strcmp(property, "primitive-vertex") == 0) return push(g_PrimitiveMan.GetCheckpointVertex(index));
		}
		if (ClassDerivesFrom(owner->crep(), "Activity") && index < Players::MaxPlayerCount) {
			auto* activity = static_cast<Activity*>(owner->ptr());
			if (std::strcmp(property, "player-controller") == 0) return push(activity->GetPlayerController(index));
			if (auto* game = dynamic_cast<GameActivity*>(activity)) {
				if ((game->GetBuyGUI(index) && game->GetBuyGUI(index)->HasPendingCheckpoint()) || (game->GetEditorGUI(index) && game->GetEditorGUI(index)->HasPendingCheckpoint())) {
					if (!game->PrepareCheckpointUI()) { lua_pushnil(L); return 1; }
				}
				if (std::strcmp(property, "buy-menu") == 0) return push(game->GetBuyGUI(index));
				if (std::strcmp(property, "editor-menu") == 0) return push(game->GetEditorGUI(index));
				if (std::strcmp(property, "yellow-banner") == 0) return push(game->GetBanner(GameActivity::YELLOW, index));
				if (std::strcmp(property, "red-banner") == 0) return push(game->GetBanner(GameActivity::RED, index));
			}
		} else if (std::strcmp(owner->crep()->name(), "SceneEditorGUI") == 0) {
			auto* editor = static_cast<SceneEditorGUI*>(owner->ptr());
			if (std::strcmp(property, "current-object") == 0) return push(editor->GetCurrentObject());
			if (std::strcmp(property, "editor-pie") == 0) return push(editor->GetCheckpointPieMenu());
		} else if (std::strcmp(owner->crep()->name(), "Scene") == 0 && std::strcmp(property, "background") == 0) {
			const auto& layers = static_cast<Scene*>(owner->ptr())->GetBackLayers();
			if (static_cast<size_t>(index) < layers.size()) return push(*std::next(layers.begin(), index));
		}
	}
	lua_pushnil(L);
	return 1;
}

static int ScriptGraphOwnerReferenceDescriptor(lua_State* L, const luabind::detail::object_rep* rep) {
	const auto found = [&](auto* owner, const char* property, int index) {
		lua_pushliteral(L, "owner-ref");
		if (static_cast<const void*>(owner) == static_cast<const void*>(&g_PrimitiveMan)) {
			const int top = lua_gettop(L);
			lua_getglobal(L, "_ScriptGraphBaseline");
			if (lua_istable(L, -1)) {
				lua_getfield(L, -1, "values");
				if (lua_istable(L, -1)) lua_getfield(L, -1, "PrimitiveMan");
			}
			auto* manager = luabind::detail::is_class_object(L, -1);
			if (!manager || manager->ptr() != &g_PrimitiveMan) {
				lua_settop(L, top);
				lua_getglobal(L, "PrimitiveMan");
				manager = luabind::detail::is_class_object(L, -1);
			}
			if (!manager || manager->ptr() != &g_PrimitiveMan) { lua_settop(L, top); lua_pushnil(L); }
			else { if (lua_gettop(L) > top + 1) lua_replace(L, top + 1); lua_settop(L, top + 1); }
		} else luabind::object(L, owner).push(L);
		lua_pushstring(L, property);
		lua_pushinteger(L, index);
		lua_pushboolean(L, (rep->flags() & luabind::detail::object_rep::constant) != 0);
		std::string checkpoint;
		const std::string type = rep->crep()->name();
		if (type == "Controller") checkpoint = static_cast<const Controller*>(rep->ptr())->SaveCheckpoint();
		else if (type == "BuyMenuGUI") checkpoint = static_cast<const BuyMenuGUI*>(rep->ptr())->SaveCheckpoint();
		else if (type == "SceneEditorGUI") checkpoint = static_cast<const SceneEditorGUI*>(rep->ptr())->SaveCheckpoint();
		else if (type == "GUIBanner") checkpoint = static_cast<const GUIBanner*>(rep->ptr())->SaveCheckpoint();
		else if (type == "SLBackground") checkpoint = static_cast<const SLBackground*>(rep->ptr())->SaveCheckpoint();
		if (checkpoint.empty()) return 5;
		lua_pushlstring(L, type.data(), type.size());
		lua_pushlstring(L, checkpoint.data(), checkpoint.size());
		return 7;
	};
	if (const int index = g_PrimitiveMan.FindCheckpointPrimitive(rep->ptr()); index >= 0) return found(&g_PrimitiveMan, "primitive", index);
	if (const int index = g_PrimitiveMan.FindCheckpointVertex(rep->ptr()); index >= 0) return found(&g_PrimitiveMan, "primitive-vertex", index);
	const auto activityMember = [&](Activity* activity) {
		if (!activity) return 0;
		for (int index = 0; index < Players::MaxPlayerCount; ++index) {
			if (rep->ptr() == activity->GetPlayerController(index)) return found(activity, "player-controller", index);
			if (auto* game = dynamic_cast<GameActivity*>(activity)) {
				if (rep->ptr() == game->GetBuyGUI(index)) return found(game, "buy-menu", index);
				if (rep->ptr() == game->GetEditorGUI(index)) return found(game, "editor-menu", index);
				if (auto* editor = game->GetEditorGUI(index)) {
					if (rep->ptr() == editor->GetCurrentObject()) return found(editor, "current-object", 0);
					if (rep->ptr() == editor->GetCheckpointPieMenu()) return found(editor, "editor-pie", 0);
				}
				if (rep->ptr() == game->GetBanner(GameActivity::YELLOW, index)) return found(game, "yellow-banner", index);
				if (rep->ptr() == game->GetBanner(GameActivity::RED, index)) return found(game, "red-banner", index);
			}
		}
		return 0;
	};
	if (const int count = activityMember(g_ActivityMan.GetActivity())) return count;
	if (rep->get_dependencies().is_valid()) {
		rep->get_dependencies().get(L);
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			const auto* owner = luabind::detail::is_class_object(L, -1);
			if (owner && ClassDerivesFrom(owner->crep(), "Activity")) {
				if (const int count = activityMember(static_cast<Activity*>(owner->ptr()))) return count;
			} else if (owner && std::strcmp(owner->crep()->name(), "SceneEditorGUI") == 0) {
				auto* editor = static_cast<SceneEditorGUI*>(owner->ptr());
				if (rep->ptr() == editor->GetCurrentObject()) return found(editor, "current-object", 0);
				if (rep->ptr() == editor->GetCheckpointPieMenu()) return found(editor, "editor-pie", 0);
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	if (Scene* scene = g_SceneMan.GetScene()) {
		int index = 0;
		for (SLBackground* layer: scene->GetBackLayers()) {
			if (rep->ptr() == layer) return found(scene, "background", index);
			++index;
		}
	}
	return 0;
}

// Set while a graph is serialized: the script-owned objects the walk actually reached.
static thread_local std::unordered_set<const MovableObject*>* s_CarriedScriptOwnedObjects = nullptr;

static int ScriptGraphNative(lua_State* L) {
	luabind::detail::object_rep* rep = luabind::detail::is_class_object(L, 1);
	if (!rep || !rep->crep()) {
		lua_pushnil(L);
		return 1;
	}
	const luabind::detail::class_rep* crep = rep->crep();
	const bool owned = (rep->flags() & luabind::detail::object_rep::owner) != 0;
	const std::string className = crep->name();
	if (!owned && className == "Material") {
		const auto& palette = g_SceneMan.GetMaterialPalette();
		for (size_t index = 0; index < palette.size(); ++index) {
			if (palette[index] == rep->ptr()) {
				lua_pushliteral(L, "material-ref");
				lua_pushinteger(L, index);
				return 2;
			}
		}
	}
	if (!owned && rep->ptr()) {
		if (const int count = ScriptGraphOwnerReferenceDescriptor(L, rep)) return count;
	}
	if (owned && ClassDerivesFrom(crep, "GraphicalPrimitive")) {
		const auto* primitive = static_cast<const GraphicalPrimitive*>(rep->ptr());
		lua_pushliteral(L, "copy");
		lua_pushstring(L, GraphicalPrimitive::CheckpointTypeName(primitive->GetPrimitiveType()));
		lua_pushliteral(L, ""); lua_pushliteral(L, ""); lua_pushnil(L);
		lua_pushlightuserdata(L, rep->ptr());
		return 6;
	}
	if (className == "Controller" && owned) {
		const auto* controller = static_cast<const Controller*>(rep->ptr());
		const std::string state = controller->SaveCheckpoint();
		lua_pushliteral(L, "controller-value");
		lua_pushlstring(L, state.data(), state.size());
		luabind::object(L, controller->GetControlledActor()).push(L);
		return 3;
	}
	if (className == "PathRequest" && owned) {
		const auto* request = static_cast<const PathRequest*>(rep->ptr());
		lua_pushliteral(L, "path-request");
		lua_newtable(L);
		int index = 0;
		const auto number = [L, &index](lua_Number value) { lua_pushnumber(L, value); lua_rawseti(L, -2, ++index); };
		number(request->complete);
		number(request->status);
		number(request->pathLength);
		number(request->totalCost);
		number(request->startPos.m_X);
		number(request->startPos.m_Y);
		number(request->targetPos.m_X);
		number(request->targetPos.m_Y);
		for (const Vector& point: request->path) { number(point.m_X); number(point.m_Y); }
		return 2;
	}
	if (className == "AlarmEvent" && owned) { lua_pushliteral(L, "alarm"); return 1; }
	if (className == "DataModule") {
		lua_pushliteral(L, "module-ref");
		lua_pushstring(L, static_cast<const DataModule*>(rep->ptr())->GetFileName().c_str());
		return 2;
	}
	if (className == "Gib") { lua_pushliteral(L, "gib-ref"); return 1; }
	if (className == "SoundSet" && !owned) {
		lua_pushliteral(L, "soundset-ref");
		return 1;
	}
	if (className == "LimbPath") {
		lua_pushliteral(L, "limb-ref");
		return 1;
	}
	if (className == "Box" && !owned) {
		lua_pushstring(L, "box-ref");
		lua_pushlightuserdata(L, rep->ptr());
		lua_pushboolean(L, (rep->flags() & luabind::detail::object_rep::constant) != 0);
		return 3;
	}
	if ((className == "Box" || className == "Area" || className == "SoundSet") && owned) {
		lua_pushstring(L, "copy");
		lua_pushstring(L, className.c_str());
		lua_pushliteral(L, "");
		lua_pushliteral(L, "");
		return 4;
	}
	if (className == "Area" && g_SceneMan.GetScene()) {
		const auto& areas = g_SceneMan.GetScene()->GetAreas();
		const auto found = std::find(areas.begin(), areas.end(), rep->ptr());
		if (found != areas.end()) {
			lua_pushstring(L, "area-ref");
			lua_pushstring(L, (*found)->GetName().c_str());
			return 2;
		}
	}
	if (className == "Controller") {
		if (const auto actor = s_ControllerOwners.find(rep->ptr()); actor != s_ControllerOwners.end()) {
			lua_pushstring(L, "controller-ref");
			lua_pushnumber(L, static_cast<lua_Number>(actor->second));
			return 2;
		}
	}
	if (className == "Vector") {
		if (owned) {
			lua_pushstring(L, "vector");
			return 1;
		}
		if (const auto field = s_VectorFields.find(rep->ptr()); field != s_VectorFields.end()) {
			lua_pushstring(L, "vector-ref");
			lua_pushnumber(L, static_cast<lua_Number>(field->second.uid));
			lua_pushstring(L, field->second.property);
			return 3;
		}
		lua_pushstring(L, "vector-ref-unresolved");
		return 1;
	}
	if (className == "Timer") {
		lua_pushstring(L, owned ? "timer" : "timer-ref");
		return 1;
	}
	if (ClassDerivesFrom(crep, "MovableObject")) {
		const MovableObject* mo = static_cast<const MovableObject*>(rep->ptr());
		if (owned) {
			const Entity* preset = mo->GetPresetForCopy();
			if (s_CarriedScriptOwnedObjects) s_CarriedScriptOwnedObjects->insert(mo);
			lua_pushstring(L, "copy");
			lua_pushstring(L, mo->GetClassName().c_str());
			lua_pushstring(L, preset ? preset->GetPresetName().c_str() : "");
			lua_pushstring(L, preset ? preset->GetModuleName().c_str() : "");
			lua_pushnumber(L, static_cast<lua_Number>(mo->GetUniqueID()));
			lua_pushlightuserdata(L, rep->ptr());
			return 6;
		}
		if (g_MovableMan.IsKnownObject(mo)) {
			lua_pushstring(L, "entity");
			lua_pushnumber(L, static_cast<lua_Number>(mo->GetUniqueID()));
			lua_pushstring(L, className.c_str());
			return 3;
		}
		lua_pushstring(L, "invalid");
		lua_pushstring(L, className.c_str());
		return 2;
	}
	if (className == "GlobalScript") {
		if (const auto* activity = dynamic_cast<const GAScripted*>(g_ActivityMan.GetActivity())) {
			const auto& scripts = activity->GetGlobalScripts();
			for (size_t index = 0; index < scripts.size(); ++index) {
				if (scripts[index] == rep->ptr()) {
					lua_pushliteral(L, "global-script");
					lua_pushinteger(L, index + 1);
					return 2;
				}
			}
		}
	}
	if (ClassDerivesFrom(crep, "Activity") && rep->ptr() == g_ActivityMan.GetActivity()) {
		lua_pushstring(L, "activity");
		return 1;
	}
	if (className == "Scene" && rep->ptr() == g_SceneMan.GetScene()) {
		lua_pushstring(L, "scene");
		return 1;
	}
	if (!owned && lua_toboolean(L, 2) && (ClassDerivesFrom(crep, "Activity") || className == "GlobalScript")) {
		lua_pushstring(L, "named");
		return 1;
	}
	if (ClassDerivesFrom(crep, "Entity")) {
		const Entity* entity = static_cast<const Entity*>(rep->ptr());
		if (owned || entity->IsOriginalPreset()) {
			const Entity* preset = entity->GetPresetForCopy();
			lua_pushstring(L, owned ? "copy" : "preset");
			lua_pushstring(L, entity->GetClassName().c_str());
			lua_pushstring(L, preset ? preset->GetPresetName().c_str() : "");
			lua_pushstring(L, preset ? preset->GetModuleName().c_str() : "");
			return 4;
		}
	}
	lua_pushnil(L);
	lua_pushstring(L, className.c_str());
	return 2;
}

static int ScriptGraphGlobalScript(lua_State* L) {
	const auto* activity = dynamic_cast<const GAScripted*>(g_ActivityMan.GetActivity());
	const int index = luaL_checkinteger(L, 1);
	if (!activity || index < 1 || static_cast<size_t>(index) > activity->GetGlobalScripts().size()) {
		lua_pushnil(L);
	} else {
		luabind::object(L, activity->GetGlobalScripts()[index - 1]).push(L);
	}
	return 1;
}

// (entity) -> the entity's INI text, or nil, message.
static Serializable* ScriptGraphSerializable(luabind::detail::object_rep* rep) {
	if (rep && rep->crep()) {
		if (ClassDerivesFrom(rep->crep(), "Entity")) return static_cast<Entity*>(rep->ptr());
		if (std::strcmp(rep->crep()->name(), "Box") == 0) return static_cast<Box*>(rep->ptr());
		if (std::strcmp(rep->crep()->name(), "Area") == 0) return static_cast<Scene::Area*>(rep->ptr());
		if (std::strcmp(rep->crep()->name(), "SoundSet") == 0) return static_cast<SoundSet*>(rep->ptr());
	}
	return nullptr;
}

static int ScriptGraphNativeSave(lua_State* L) {
	const auto* native = luabind::detail::is_class_object(L, 1);
	if (native && native->crep() && ClassDerivesFrom(native->crep(), "GraphicalPrimitive")) {
		try {
			const std::string text = static_cast<const GraphicalPrimitive*>(native->ptr())->SaveCheckpoint();
			lua_pushlstring(L, text.data(), text.size()); return 1;
		} catch (const std::exception& error) { lua_pushnil(L); lua_pushstring(L, error.what()); return 2; }
	}
	const Serializable* object = ScriptGraphSerializable(luabind::detail::is_class_object(L, 1));
	if (!object) {
		lua_pushnil(L);
		lua_pushstring(L, "not a supported serializable object");
		return 2;
	}
	auto stream = std::make_unique<std::stringstream>();
	std::stringstream* raw = stream.get();
	Writer writer(std::move(stream));
	Writer::SnapshotScope snapshotScope(writer);
	if (const MovableObject* mo = dynamic_cast<const MovableObject*>(object)) {
		writer.NewProperty("ScriptEntity");
		Scene::SaveSceneObject(writer, mo, false, true);
	} else {
		writer.NewProperty("ScriptEntity");
		if (const Scene::Area* area = dynamic_cast<const Scene::Area*>(object)) {
			area->SaveSnapshot(writer);
		} else {
			object->Save(writer);
		}
		writer.ObjectEnd();
	}
	const std::string text = raw->str();
	lua_pushlstring(L, text.data(), text.size());
	return 1;
}

static int ScriptGraphPrimitiveCreate(lua_State* L) {
	const std::string type = luaL_checkstring(L, 1);
	for (int index = 1; index <= static_cast<int>(GraphicalPrimitive::PrimitiveType::Bitmap); ++index) {
		const auto value = static_cast<GraphicalPrimitive::PrimitiveType>(index);
		if (type != GraphicalPrimitive::CheckpointTypeName(value)) continue;
		auto primitive = GraphicalPrimitive::CreateCheckpointType(value);
		switch (value) {
#define COPY_PRIMITIVE(name) case GraphicalPrimitive::PrimitiveType::name: luabind::object(L, static_cast<name##Primitive&>(*primitive)).push(L); return 1
			COPY_PRIMITIVE(Line); COPY_PRIMITIVE(Arc); COPY_PRIMITIVE(Spline); COPY_PRIMITIVE(Box); COPY_PRIMITIVE(BoxFill);
			COPY_PRIMITIVE(RoundedBox); COPY_PRIMITIVE(RoundedBoxFill); COPY_PRIMITIVE(Circle); COPY_PRIMITIVE(CircleFill);
			COPY_PRIMITIVE(Ellipse); COPY_PRIMITIVE(EllipseFill); COPY_PRIMITIVE(Triangle); COPY_PRIMITIVE(TriangleFill);
			COPY_PRIMITIVE(Text); COPY_PRIMITIVE(Bitmap);
#undef COPY_PRIMITIVE
			default: break;
		}
	}
	lua_pushnil(L); return 1;
}

// (entity, ini) -> true, or nil, message: the entity takes the saved state in place of its preset copy.
static int ScriptGraphNativeLoad(lua_State* L) {
	auto* native = luabind::detail::is_class_object(L, 1);
	if (native && native->crep() && ClassDerivesFrom(native->crep(), "GraphicalPrimitive")) {
		size_t length; const char* data = luaL_checklstring(L, 2, &length);
		lua_pushboolean(L, static_cast<GraphicalPrimitive*>(native->ptr())->LoadCheckpoint(std::string_view(data, length))); return 1;
	}
	Serializable* object = ScriptGraphSerializable(luabind::detail::is_class_object(L, 1));
	if (!object || !lua_isstring(L, 2)) {
		lua_pushnil(L);
		lua_pushstring(L, "an entity and its INI text are needed");
		return 2;
	}
	try {
	const Entity* entity = dynamic_cast<Entity*>(object);
	struct RestoreScope {
		bool previous = g_MovableMan.IsRestoringSnapshot();
		RestoreScope() { g_MovableMan.SetRestoringSnapshot(true); }
		~RestoreScope() { g_MovableMan.SetRestoringSnapshot(previous); }
	} restoreScope;
	size_t length = 0;
	const char* data = lua_tolstring(L, 2, &length);
	const std::string module = !entity || entity->GetModuleName().empty() ? "Base.rte" : entity->GetModuleName();
	Reader reader(std::make_unique<std::stringstream>(std::string(data, length)), module + "/ScriptGraph.ini", false, nullptr, true);
	reader.SetCheckpoint(true);
	reader.SetThrowOnError(true);
	reader.SetSkipIncludes(true);
	if (!reader.NextProperty() || reader.ReadPropName() != "ScriptEntity") {
		lua_pushnil(L);
		lua_pushstring(L, "the INI text carries no entity");
		return 2;
	}
	if (Entity* ownedEntity = dynamic_cast<Entity*>(object)) ownedEntity->Destroy();
	else object->Reset();
	if (object->Create(reader, true, dynamic_cast<MovableObject*>(object) == nullptr) < 0) {
		lua_pushnil(L);
		lua_pushstring(L, "the entity did not read its saved state");
		return 2;
	}
	if (auto* activity = dynamic_cast<Activity*>(object); activity && !activity->ApplyPendingCheckpoint()) {
		lua_pushnil(L);
		lua_pushliteral(L, "the activity runtime state is invalid");
		return 2;
	}
	if (MovableObject* mo = dynamic_cast<MovableObject*>(object)) {
		mo->AdoptPersistedUniqueID();
		if (Actor* actor = dynamic_cast<Actor*>(mo)) {
			actor->ApplyPersistedControllerMode();
		}
	}
	lua_pushboolean(L, 1);
	return 1;
	} catch (const std::exception& error) {
		lua_pushnil(L);
		lua_pushstring(L, error.what());
		return 2;
	}
}

static void ReleaseScriptOwnedTree(MovableObject* mo) {
	if (g_MovableMan.FindObjectByUniqueID(mo->GetUniqueID()) == mo && mo->GetLuaState()) {
		mo->GetLuaState()->UnregisterMO(mo);
	}
	g_MovableMan.UnregisterObject(mo);
	if (auto* part = dynamic_cast<Attachable*>(mo)) {
		if (part->GetOwnedBreakWound()) ReleaseScriptOwnedTree(part->GetOwnedBreakWound());
		if (part->GetOwnedParentBreakWound()) ReleaseScriptOwnedTree(part->GetOwnedParentBreakWound());
	}
	if (MOSRotating* rotating = dynamic_cast<MOSRotating*>(mo)) {
		for (Attachable* part: rotating->GetAttachables()) {
			ReleaseScriptOwnedTree(part);
		}
		for (AEmitter* wound: rotating->GetWoundList()) {
			ReleaseScriptOwnedTree(wound);
		}
	}
	if (Actor* actor = dynamic_cast<Actor*>(mo)) {
		for (MovableObject* item: *actor->GetInventory()) {
			ReleaseScriptOwnedTree(item);
		}
	}
	if (ACraft* craft = dynamic_cast<ACraft*>(mo)) for (MovableObject* item: craft->GetCollectedInventory()) ReleaseScriptOwnedTree(item);
}

// Every script-owned MovableObject in this state's heap. A capture and the restore's release
// both run this walk, so what a graph has to carry and what a restore detaches cannot disagree.
static void VisitScriptOwnedObjects(lua_State* state, const std::function<void(MovableObject*)>& visit) {
	auto* registry = luabind::detail::class_registry::get_registry(state);
	struct Walk {
		const void* cpp;
		const void* lua;
		const std::function<void(MovableObject*)>* visit;
	} walk;
	lua_rawgeti(state, LUA_REGISTRYINDEX, registry->cpp_instance());
	walk.cpp = lua_topointer(state, -1);
	lua_pop(state, 1);
	lua_rawgeti(state, LUA_REGISTRYINDEX, registry->lua_instance());
	walk.lua = lua_topointer(state, -1);
	lua_pop(state, 1);
	walk.visit = &visit;
	LuaThreadCodec::VisitUserdata(state, [](void* data, size_t size, const void* metatable, void* raw) {
		const auto& context = *static_cast<const Walk*>(raw);
		if (size < sizeof(luabind::detail::object_rep) || (metatable != context.cpp && metatable != context.lua)) return;
		const auto* rep = static_cast<const luabind::detail::object_rep*>(data);
		if (rep->ptr() && rep->crep() && (rep->flags() & luabind::detail::object_rep::owner) && ClassDerivesFrom(rep->crep(), "MovableObject")) {
			(*context.visit)(static_cast<MovableObject*>(rep->ptr()));
		}
	}, &walk);
}

static int ScriptGraphNativeRelease(lua_State* L) {
	auto* rep = luabind::detail::is_class_object(L, 1);
	if (rep && rep->crep() && (rep->flags() & luabind::detail::object_rep::owner) && ClassDerivesFrom(rep->crep(), "MovableObject")) {
		ReleaseScriptOwnedTree(static_cast<MovableObject*>(rep->ptr()));
	}
	return 0;
}

static int ScriptGraphNativeResolve(lua_State* L) {
	auto* rep = luabind::detail::is_class_object(L, 1);
	bool complete = true;
	if (rep && rep->crep() && ClassDerivesFrom(rep->crep(), "MovableObject")) {
		static_cast<MovableObject*>(rep->ptr())->ResolveFaithfulLinks();
	} else if (rep && rep->crep() && ClassDerivesFrom(rep->crep(), "Activity")) {
		complete = static_cast<Activity*>(rep->ptr())->ResolveCheckpointReferences();
	}
	lua_pushboolean(L, complete);
	return 1;
}

static int ScriptGraphPrimitiveResolve(lua_State* L) {
	auto* rep = luabind::detail::is_class_object(L, 1);
	lua_pushboolean(L, !rep || !rep->crep() || !ClassDerivesFrom(rep->crep(), "GraphicalPrimitive") || static_cast<GraphicalPrimitive*>(rep->ptr())->ResolveCheckpointReferences());
	return 1;
}

static int ScriptGraphAdoptRoot(lua_State* L) {
	auto* state = static_cast<LuaStateWrapper*>(lua_touserdata(L, lua_upvalueindex(1)));
	const long uid = static_cast<long>(lua_tonumber(L, 1));
	MovableObject* object = g_MovableMan.FindObjectByUniqueID(uid);
	std::string error;
	if (!object) {
		error = "the script state names object " + std::to_string(uid) + ", which is not in the world";
	} else if (object->ObjectScriptsInitialized()) {
		if (object->GetLuaState() != state) {
			error = "object " + std::to_string(uid) + " started its scripts in another Lua state";
		}
	} else {
		object->MoveScriptsToState(*state);
		if (object->GetLuaState() != state || object->AdoptScriptObject() < 0) {
			error = "object " + std::to_string(uid) + " could not adopt its saved Lua state";
		}
	}
	lua_pushboolean(L, error.empty());
	lua_pushlstring(L, error.data(), error.size());
	return 2;
}

// Appends every string in the table at the index.
static void CollectStrings(lua_State* L, int index, std::vector<std::string>& out) {
	const int absolute = index < 0 ? lua_gettop(L) + index + 1 : index;
	if (!lua_istable(L, absolute)) {
		return;
	}
	lua_pushnil(L);
	while (lua_next(L, absolute) != 0) {
		if (lua_isstring(L, -1)) {
			out.emplace_back(lua_tostring(L, -1));
		}
		lua_pop(L, 1);
	}
}

struct ScriptCallbackRootScope {
	lua_State* state;
	~ScriptCallbackRootScope() {
		lua_pushliteral(state, "_ScriptGraphCallbacks");
		lua_pushnil(state);
		lua_rawset(state, LUA_GLOBALSINDEX);
	}
};

static int ScriptGraphRandomState(lua_State* L) {
	auto* state = static_cast<LuaStateWrapper*>(lua_touserdata(L, lua_upvalueindex(1)));
	if (lua_type(L, 1) == LUA_TSTRING) {
		size_t length;
		const char* text = lua_tolstring(L, 1, &length);
		if (lua_toboolean(L, 2)) {
			RandomGenerator candidate;
			lua_pushboolean(L, candidate.RestoreCheckpoint(std::string_view(text, length)));
		} else {
			lua_pushboolean(L, state->RestoreRandomGeneratorCheckpoint(std::string_view(text, length)));
		}
	} else {
		const std::string text = state->GetRandomGeneratorCheckpoint();
		lua_pushlstring(L, text.data(), text.size());
	}
	return 1;
}
} // namespace

void LuaStateWrapper::LoadScriptGraphHelper() {
	if (!m_ScriptGraphHelperLoaded) {
		lua_pushcfunction(m_State, ScriptGraphObjectAddress);
		lua_setglobal(m_State, "_ScriptGraphObjectAddress");
		lua_pushcfunction(m_State, ScriptGraphNativeAddress);
		lua_setglobal(m_State, "_ScriptGraphNativeAddress");
		lua_pushcfunction(m_State, ScriptGraphInstance);
		lua_setglobal(m_State, "_ScriptGraphInstance");
		lua_pushcfunction(m_State, ScriptGraphSetInstance);
		lua_setglobal(m_State, "_ScriptGraphSetInstance");
		lua_pushcfunction(m_State, ScriptGraphMembers);
		lua_setglobal(m_State, "_ScriptGraphMembers");
		lua_pushcfunction(m_State, ScriptGraphNative);
		lua_setglobal(m_State, "_ScriptGraphNative");
		lua_pushcfunction(m_State, ScriptGraphOwnerState);
		lua_setglobal(m_State, "_ScriptGraphOwnerState");
		lua_pushcfunction(m_State, ScriptGraphOwnerReference);
		lua_setglobal(m_State, "_ScriptGraphOwnerReference");
		lua_pushcfunction(m_State, ScriptGraphControllerState);
		lua_setglobal(m_State, "_ScriptGraphControllerState");
		lua_pushcfunction(m_State, ScriptGraphControllerActor);
		lua_setglobal(m_State, "_ScriptGraphControllerActor");
		lua_pushcfunction(m_State, ScriptGraphPathRequest);
		lua_setglobal(m_State, "_ScriptGraphPathRequest");
		lua_pushcfunction(m_State, ScriptGraphPathQueueSelfTest);
		lua_setglobal(m_State, "_ScriptGraphPathQueueSelfTest");
		lua_pushcfunction(m_State, ScriptGraphIteratorRewind);
		lua_setglobal(m_State, "_ScriptGraphIteratorRewind");
		lua_pushcfunction(m_State, ScriptGraphIteratorSnapshot);
		lua_setglobal(m_State, "_ScriptGraphIteratorSnapshot");
		lua_pushcfunction(m_State, ScriptGraphIteratorRestore);
		lua_setglobal(m_State, "_ScriptGraphIteratorRestore");
		lua_pushcfunction(m_State, ScriptGraphIteratorFromValues);
		lua_setglobal(m_State, "_ScriptGraphIteratorFromValues");
		lua_pushcfunction(m_State, ScriptGraphGibOwner);
		lua_setglobal(m_State, "_ScriptGraphGibOwner");
		lua_pushcfunction(m_State, ScriptGraphGib);
		lua_setglobal(m_State, "_ScriptGraphGib");
		lua_pushcfunction(m_State, ScriptGraphGibReferences);
		lua_setglobal(m_State, "_ScriptGraphGibReferences");
		lua_pushcfunction(m_State, ScriptGraphRestoreGibReference);
		lua_setglobal(m_State, "_ScriptGraphRestoreGibReference");
		lua_pushcfunction(m_State, ScriptGraphPropertyOwner);
		lua_setglobal(m_State, "_ScriptGraphPropertyOwner");
		lua_pushcfunction(m_State, ScriptGraphProperty);
		lua_setglobal(m_State, "_ScriptGraphProperty");
		lua_pushcfunction(m_State, ScriptGraphSoundSetOwner);
		lua_setglobal(m_State, "_ScriptGraphSoundSetOwner");
		lua_pushcfunction(m_State, ScriptGraphSoundSet);
		lua_setglobal(m_State, "_ScriptGraphSoundSet");
		lua_pushcfunction(m_State, ScriptGraphLimbOwner);
		lua_setglobal(m_State, "_ScriptGraphLimbOwner");
		lua_pushcfunction(m_State, ScriptGraphActorLimb);
		lua_setglobal(m_State, "_ScriptGraphActorLimb");
		lua_pushcfunction(m_State, ScriptGraphLimbVectorOwner);
		lua_setglobal(m_State, "_ScriptGraphLimbVectorOwner");
		lua_pushcfunction(m_State, ScriptGraphLimbVector);
		lua_setglobal(m_State, "_ScriptGraphLimbVector");
		lua_pushcfunction(m_State, ScriptGraphGlobalScript);
		lua_setglobal(m_State, "_ScriptGraphGlobalScript");
		lua_pushcfunction(m_State, ScriptGraphNativeSave);
		lua_setglobal(m_State, "_ScriptGraphNativeSave");
		lua_pushcfunction(m_State, ScriptGraphPrimitiveCreate);
		lua_setglobal(m_State, "_ScriptGraphPrimitiveCreate");
		lua_pushcfunction(m_State, ScriptGraphPrimitiveResolve);
		lua_setglobal(m_State, "_ScriptGraphPrimitiveResolve");
		lua_pushcfunction(m_State, ScriptGraphAreaBoxes);
		lua_setglobal(m_State, "_ScriptGraphAreaBoxes");
		lua_pushcfunction(m_State, ScriptGraphAreaBox);
		lua_setglobal(m_State, "_ScriptGraphAreaBox");
		lua_pushcfunction(m_State, ScriptGraphSceneBoxOwner);
		lua_setglobal(m_State, "_ScriptGraphSceneBoxOwner");
		lua_pushcfunction(m_State, ScriptGraphNativeLoad);
		lua_setglobal(m_State, "_ScriptGraphNativeLoad");
		lua_pushcfunction(m_State, ScriptGraphNativeRelease);
		lua_setglobal(m_State, "_ScriptGraphNativeRelease");
		lua_pushcfunction(m_State, ScriptGraphNativeResolve);
		lua_setglobal(m_State, "_ScriptGraphNativeResolve");
		lua_pushlightuserdata(m_State, this);
		lua_pushcclosure(m_State, ScriptGraphAdoptRoot, 1);
		lua_setglobal(m_State, "_ScriptGraphAdoptRoot");
		lua_pushlightuserdata(m_State, this);
		lua_pushcclosure(m_State, ScriptGraphRandomState, 1);
		lua_setglobal(m_State, "_ScriptGraphRandomState");
		lua_pushcfunction(m_State, ScriptGraphBeginCapture);
		lua_setglobal(m_State, "_ScriptGraphBeginCapture");
		lua_pushcfunction(m_State, ScriptGraphEndCapture);
		lua_setglobal(m_State, "_ScriptGraphEndCapture");
		LuaThreadCodec::Register(m_State);
		RunScriptString(c_ScriptGraphHelper);
		m_ScriptGraphHelperLoaded = true;
	}
}

void LuaStateWrapper::CaptureScriptGraphBaseline() {
	LoadScriptGraphHelper();
	RunScriptString("_ScriptGraph.captureBaseline()");
}

bool LuaStateWrapper::SerializeScriptGraph(std::string& text, std::vector<std::string>& problems) {
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);
	ScriptCallbackRootScope callbackRoot{m_State};
	LoadScriptGraphHelper();
	CaptureScriptCallbacks();
	const int top = lua_gettop(m_State);
	text.clear();
	lua_newtable(m_State);
	std::unordered_set<MovableObject*> objects = m_RegisteredMOs;
	objects.insert(m_AddedRegisteredMOs.begin(), m_AddedRegisteredMOs.end());
	for (const MovableObject* mo: objects) {
		if (!mo->ObjectScriptsInitialized()) {
			continue;
		}
		lua_pushstring(m_State, std::to_string(mo->GetUniqueID()).c_str());
		PushScriptObjectInstanceTable(m_State, mo->GetUniqueID());
		if (lua_isnil(m_State, -1)) {
			lua_pop(m_State, 1);
			lua_newtable(m_State);
		}
		lua_settable(m_State, -3);
	}
	lua_getglobal(m_State, "_ScriptGraph");
	lua_getfield(m_State, -1, "serialize");
	lua_pushvalue(m_State, -3);
	std::unordered_set<const MovableObject*> carried;
	struct CarriedScope {
		explicit CarriedScope(std::unordered_set<const MovableObject*>& objects) { s_CarriedScriptOwnedObjects = &objects; }
		~CarriedScope() { s_CarriedScriptOwnedObjects = nullptr; }
	} carriedScope{carried};
	if (lua_pcall(m_State, 1, 2, 0) != 0) {
		problems.push_back(std::string("script graph serialize failed: ") + (lua_tostring(m_State, -1) ? lua_tostring(m_State, -1) : "?"));
		lua_settop(m_State, top);
		return false;
	}
	size_t length = 0;
	const char* data = lua_tolstring(m_State, -2, &length);
	text = data ? std::string(data, length) : std::string();
	const size_t before = problems.size();
	CollectStrings(m_State, -1, problems);
	// The restore detaches every script-owned tree so this graph's copies can adopt their saved
	// identities. One the roots never reached comes back as nothing, so a checkpoint that still
	// has to rebind its borrowed pointers names an owner no peer can produce.
	VisitScriptOwnedObjects(m_State, [&carried, &problems](MovableObject* mo) {
		if (carried.contains(mo)) return;
		// A world set aside for a restore leaves its script-owned trees in the heap while the
		// restored copies hold their identities, and only the live registration reaches the
		// reference map, so a shadowed copy is not an owner a restore can fail to find.
		if (g_MovableMan.FindObjectByUniqueID(mo->GetUniqueID()) != mo) return;
		const std::vector<long> links = mo->GetCheckpointBorrowedReferences();
		if (std::none_of(links.begin(), links.end(), [](long target) { return target != 0; })) return;
		problems.push_back("a script-owned " + mo->GetClassName() + " (" + mo->GetPresetName() + ") that no script graph root reaches");
	});
	lua_settop(m_State, top);
	return problems.size() == before;
}

std::vector<long> LuaStateWrapper::ListScriptGraphRoots(const std::string& text) {
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);
	LoadScriptGraphHelper();
	const int top = lua_gettop(m_State);
	std::vector<long> roots;
	lua_getglobal(m_State, "_ScriptGraph");
	lua_getfield(m_State, -1, "roots");
	lua_pushlstring(m_State, text.data(), text.size());
	if (lua_pcall(m_State, 1, 1, 0) != 0) {
		g_ConsoleMan.PrintString(std::string("ERROR: script graph roots failed: ") + (lua_tostring(m_State, -1) ? lua_tostring(m_State, -1) : "?"));
		lua_settop(m_State, top);
		return roots;
	}
	if (lua_istable(m_State, -1)) {
		const int count = static_cast<int>(lua_objlen(m_State, -1));
		for (int i = 1; i <= count; ++i) {
			lua_rawgeti(m_State, -1, i);
			if (lua_isstring(m_State, -1)) {
				roots.push_back(static_cast<long>(std::strtol(lua_tostring(m_State, -1), nullptr, 10)));
			}
			lua_pop(m_State, 1);
		}
	}
	lua_settop(m_State, top);
	return roots;
}

bool LuaStateWrapper::ValidateScriptGraph(const std::string& text, std::vector<std::string>& problems) {
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);
	LoadScriptGraphHelper();
	const int top = lua_gettop(m_State);
	const size_t before = problems.size();
	lua_getglobal(m_State, "_ScriptGraph");
	lua_getfield(m_State, -1, "validate");
	lua_pushlstring(m_State, text.data(), text.size());
	if (lua_pcall(m_State, 1, 1, 0) != 0) {
		problems.push_back(std::string("script graph validation failed: ") + (lua_tostring(m_State, -1) ? lua_tostring(m_State, -1) : "?"));
	} else {
		CollectStrings(m_State, -1, problems);
	}
	lua_settop(m_State, top);
	return problems.size() == before;
}

bool LuaStateWrapper::PrepareScriptGraph(const std::string* text, std::vector<std::string>& problems, bool reuseHeld) {
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);
	LoadScriptGraphHelper();
	const int top = lua_gettop(m_State);
	const size_t before = problems.size();
	lua_getglobal(m_State, "_ScriptGraph");
	lua_getfield(m_State, -1, text ? "prepare" : "prepareRoots");
	if (text) {
		lua_pushlstring(m_State, text->data(), text->size());
		lua_pushboolean(m_State, reuseHeld);
	}
	if (lua_pcall(m_State, text ? 2 : 0, 1, 0) != 0) {
		problems.push_back(std::string("script graph preparation failed: ") + (lua_tostring(m_State, -1) ? lua_tostring(m_State, -1) : "?"));
	} else {
		CollectStrings(m_State, -1, problems);
	}
	lua_settop(m_State, top);
	return problems.size() == before;
}

void LuaStateWrapper::ReleaseScriptOwnedObjects() {
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);
	VisitScriptOwnedObjects(m_State, ReleaseScriptOwnedTree);
}

bool LuaStateWrapper::RestoreScriptGraph(const std::string& text, std::vector<std::string>& problems, bool reuseHeld) {
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);
	ScriptCallbackRootScope callbackRoot{m_State};
	LoadScriptGraphHelper();
	const int top = lua_gettop(m_State);
	const size_t before = problems.size();
	lua_getglobal(m_State, "_ScriptGraph");
	lua_getfield(m_State, -1, "deserialize");
	lua_pushlstring(m_State, text.data(), text.size());
	lua_pushboolean(m_State, reuseHeld);
	lua_pushboolean(m_State, true);
	if (lua_pcall(m_State, 3, 2, 0) != 0) {
		problems.push_back(std::string("script graph deserialize failed: ") + (lua_tostring(m_State, -1) ? lua_tostring(m_State, -1) : "?"));
		lua_settop(m_State, top);
		return false;
	}
	CollectStrings(m_State, -1, problems);
	const int roots = lua_gettop(m_State) - 1;
	if (lua_istable(m_State, roots)) {
		lua_pushnil(m_State);
		while (lua_next(m_State, roots) != 0) {
			const long uid = lua_isstring(m_State, -2) ? static_cast<long>(std::strtol(lua_tostring(m_State, -2), nullptr, 10)) : 0;
			luabind::detail::object_rep* rep = nullptr;
			lua_getglobal(m_State, "_ScriptedObjects");
			if (lua_istable(m_State, -1)) {
				lua_pushstring(m_State, std::to_string(uid).c_str());
				lua_gettable(m_State, -2);
				rep = lua_isuserdata(m_State, -1) ? static_cast<luabind::detail::object_rep*>(lua_touserdata(m_State, -1)) : nullptr;
				lua_pop(m_State, 1);
			}
			lua_pop(m_State, 1);
			if (rep && lua_istable(m_State, -1)) {
				lua_pushvalue(m_State, -1);
				rep->get_lua_table().set(m_State);
			} else {
				problems.push_back("the script state of object " + std::to_string(uid) + " has no live object to land on");
			}
			lua_pop(m_State, 1);
		}
	}
	RestoreScriptCallbacks(problems, !reuseHeld);
	lua_settop(m_State, top);
	return problems.size() == before;
}

bool LuaStateWrapper::RestoreLegacyScriptObjectFields(long uniqueID, const std::string& text) {
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);
	LoadScriptGraphHelper();
	const int top = lua_gettop(m_State);
	lua_getglobal(m_State, "_ScriptedObjects");
	if (!lua_istable(m_State, -1)) {
		lua_settop(m_State, top);
		return false;
	}
	lua_getfield(m_State, -1, std::to_string(uniqueID).c_str());
	luabind::detail::object_rep* rep = luabind::detail::is_class_object(m_State, -1);
	lua_getglobal(m_State, "_ScriptGraph");
	lua_getfield(m_State, -1, "restoreLegacy");
	lua_pushlstring(m_State, text.data(), text.size());
	const int status = lua_pcall(m_State, 1, 1, 0);
	const bool restored = status == 0 && rep && lua_istable(m_State, -1);
	if (restored) {
		rep->get_lua_table().set(m_State);
	} else {
		g_ConsoleMan.PrintString("ERROR: legacy script field restore failed for object " + std::to_string(uniqueID) + ": " + (lua_tostring(m_State, -1) ? lua_tostring(m_State, -1) : "missing object fields"));
	}
	lua_settop(m_State, top);
	return restored;
}

void LuaStateWrapper::CaptureScriptCallbacks() {
	const int top = lua_gettop(m_State);
	lua_newtable(m_State);
	const int callbacks = lua_gettop(m_State);
	if (this == &g_LuaMan.GetMasterScriptState()) {
		if (const auto* activity = dynamic_cast<const GAScripted*>(g_ActivityMan.GetActivity())) {
			lua_newtable(m_State);
			lua_pushstring(m_State, activity->GetLuaClassName().c_str());
			lua_setfield(m_State, -2, "class");
			lua_newtable(m_State);
			for (const auto& [name, function]: activity->m_ScriptFunctions) {
				function->GetLuabindObject()->push(m_State);
				lua_setfield(m_State, -2, name.c_str());
			}
			lua_setfield(m_State, -2, "functions");
			lua_setfield(m_State, callbacks, "activity");
			lua_newtable(m_State);
			int index = 1;
			for (const GlobalScript* script: activity->GetGlobalScripts()) {
				lua_newtable(m_State);
				lua_pushstring(m_State, script->m_LuaClassName.c_str());
				lua_setfield(m_State, -2, "class");
				lua_pushboolean(m_State, script->m_IsActive);
				lua_setfield(m_State, -2, "active");
				lua_pushboolean(m_State, script->m_HasStarted);
				lua_setfield(m_State, -2, "started");
				lua_pushboolean(m_State, script->m_LateUpdate);
				lua_setfield(m_State, -2, "late");
				lua_rawseti(m_State, -2, index++);
			}
			lua_setfield(m_State, callbacks, "globals");
		}
	}
	g_LuaMan.PushPathCallbacks(m_State);
	lua_setfield(m_State, callbacks, "async");
	lua_newtable(m_State);
	for (const auto& [path, cached]: m_ScriptCache) {
		lua_newtable(m_State);
		for (const auto& [name, function]: cached.functionNamesAndObjects) {
			function->GetLuabindObject()->push(m_State);
			lua_setfield(m_State, -2, name.c_str());
		}
		lua_setfield(m_State, -2, path.c_str());
	}
	lua_setfield(m_State, callbacks, "cache");
	lua_newtable(m_State);
	for (const MovableObject* mo: g_MovableMan.SnapshotKnownObjects()) {
		if (mo->GetLuaState() != this || mo->IsOriginalPreset() || mo->GetPendingPersistedUniqueID() > 0 || mo->m_FunctionsAndScripts.empty()) {
			continue;
		}
		lua_newtable(m_State);
		for (const auto& [name, functions]: mo->m_FunctionsAndScripts) {
			lua_newtable(m_State);
			int index = 1;
			for (const auto& function: functions) {
				lua_newtable(m_State);
				function.m_LuaFunction->GetLuabindObject()->push(m_State);
				lua_setfield(m_State, -2, "function");
				lua_pushstring(m_State, function.m_LuaFunction->GetFilePath().c_str());
				lua_setfield(m_State, -2, "path");
				lua_pushboolean(m_State, function.m_ScriptIsEnabled);
				lua_setfield(m_State, -2, "enabled");
				lua_rawseti(m_State, -2, index++);
			}
			lua_setfield(m_State, -2, name.c_str());
		}
		lua_setfield(m_State, -2, std::to_string(mo->GetUniqueID()).c_str());
	}
	lua_setfield(m_State, callbacks, "objects");
	lua_pushliteral(m_State, "_ScriptGraphCallbacks");
	lua_pushvalue(m_State, callbacks);
	lua_rawset(m_State, LUA_GLOBALSINDEX);
	lua_settop(m_State, top);
}

void LuaStateWrapper::RestoreScriptCallbacks(std::vector<std::string>& problems, bool restoreAsync) {
	const int top = lua_gettop(m_State);
	lua_pushliteral(m_State, "_ScriptGraphCallbacks");
	lua_rawget(m_State, LUA_GLOBALSINDEX);
	if (!lua_istable(m_State, -1)) {
		lua_settop(m_State, top);
		return;
	}
	const int callbacks = lua_gettop(m_State);
	if (this == &g_LuaMan.GetMasterScriptState()) {
		if (auto* activity = dynamic_cast<GAScripted*>(g_ActivityMan.GetActivity())) {
			lua_getfield(m_State, callbacks, "activity");
			if (lua_istable(m_State, -1)) {
				lua_getfield(m_State, -1, "class");
				const bool sameClass = lua_isstring(m_State, -1) && activity->GetLuaClassName() == lua_tostring(m_State, -1);
				lua_pop(m_State, 1);
				lua_getfield(m_State, -1, "functions");
				if (sameClass && lua_istable(m_State, -1)) {
					activity->m_ScriptFunctions.clear();
					lua_pushnil(m_State);
					while (lua_next(m_State, -2)) {
						if (lua_isstring(m_State, -2) && lua_isfunction(m_State, -1)) {
							const std::string name = lua_tostring(m_State, -2);
							activity->m_ScriptFunctions.emplace(name, std::make_unique<LuabindObjectWrapper>(
							    new luabind::object(luabind::from_stack(m_State, -1)), activity->m_ScriptPath));
						} else {
							problems.push_back("invalid cached activity callback");
						}
						lua_pop(m_State, 1);
					}
				} else {
					problems.push_back("invalid activity callback checkpoint");
				}
				lua_pop(m_State, 1);
			} else {
				// Older checkpoints only contain the activity's Lua fields.
				activity->RefreshActivityFunctions();
			}
			lua_pop(m_State, 1);
			lua_getfield(m_State, callbacks, "globals");
			if (lua_istable(m_State, -1)) {
				const auto& scripts = activity->GetGlobalScripts();
				if (lua_objlen(m_State, -1) != scripts.size()) problems.push_back("the global script count changed");
				for (size_t index = 0; index < scripts.size(); ++index) {
					GlobalScript* script = scripts[index];
					lua_rawgeti(m_State, -1, index + 1);
					if (lua_istable(m_State, -1)) {
						lua_getfield(m_State, -1, "class");
						if (!lua_isstring(m_State, -1) || script->m_LuaClassName != lua_tostring(m_State, -1)) problems.push_back("the global script class changed");
						lua_pop(m_State, 1);
						const auto restoreFlag = [&](const char* name, bool& flag) {
							lua_getfield(m_State, -1, name);
							if (lua_isboolean(m_State, -1)) flag = lua_toboolean(m_State, -1);
							else problems.push_back(std::string("invalid global script flag ") + name);
							lua_pop(m_State, 1);
						};
						restoreFlag("active", script->m_IsActive);
						restoreFlag("started", script->m_HasStarted);
						restoreFlag("late", script->m_LateUpdate);
					} else {
						problems.push_back("missing global script checkpoint");
					}
					lua_pop(m_State, 1);
				}
			}
			lua_pop(m_State, 1);
		}
	}
	if (restoreAsync) {
		lua_getfield(m_State, callbacks, "async");
		if (lua_istable(m_State, -1) && !g_LuaMan.RestorePathCallbacks(m_State, -1)) problems.push_back("invalid asynchronous path callbacks");
		lua_pop(m_State, 1);
	}
	lua_getfield(m_State, callbacks, "cache");
	if (lua_istable(m_State, -1)) {
		ClearLuaScriptCache();
		lua_pushnil(m_State);
		while (lua_next(m_State, -2)) {
			if (lua_isstring(m_State, -2) && lua_istable(m_State, -1)) {
				const std::string path = lua_tostring(m_State, -2);
				auto& cached = m_ScriptCache[path];
				lua_pushnil(m_State);
				while (lua_next(m_State, -2)) {
					if (lua_isstring(m_State, -2) && lua_isfunction(m_State, -1)) {
						const std::string name = lua_tostring(m_State, -2);
						cached.functionNamesAndObjects.emplace(name, new LuabindObjectWrapper(new luabind::object(luabind::from_stack(m_State, -1)), path));
					} else {
						problems.push_back("invalid cached function in " + path);
					}
					lua_pop(m_State, 1);
				}
			} else {
				problems.push_back("invalid script cache entry");
			}
			lua_pop(m_State, 1);
		}
	}
	lua_pop(m_State, 1);
	lua_getfield(m_State, callbacks, "objects");
	if (lua_istable(m_State, -1)) {
		lua_pushnil(m_State);
		while (lua_next(m_State, -2)) {
			const long uid = lua_isstring(m_State, -2) ? static_cast<long>(std::strtol(lua_tostring(m_State, -2), nullptr, 10)) : 0;
			MovableObject* mo = g_MovableMan.FindObjectByUniqueID(uid);
			if (mo && mo->GetLuaState() == this && lua_istable(m_State, -1)) {
				mo->m_FunctionsAndScripts.clear();
				lua_pushnil(m_State);
				while (lua_next(m_State, -2)) {
					if (lua_isstring(m_State, -2) && lua_istable(m_State, -1)) {
						const std::string name = lua_tostring(m_State, -2);
						auto& functions = mo->m_FunctionsAndScripts[name];
						const int count = static_cast<int>(lua_objlen(m_State, -1));
						for (int index = 1; index <= count; ++index) {
							lua_rawgeti(m_State, -1, index);
							if (lua_istable(m_State, -1)) {
								lua_getfield(m_State, -1, "path");
								const std::string path = lua_isstring(m_State, -1) ? lua_tostring(m_State, -1) : "";
								lua_pop(m_State, 1);
								lua_getfield(m_State, -1, "enabled");
								const bool enabled = lua_toboolean(m_State, -1);
								lua_pop(m_State, 1);
								lua_getfield(m_State, -1, "function");
								if (!path.empty() && lua_isfunction(m_State, -1)) {
									auto& function = functions.emplace_back();
									function.m_ScriptIsEnabled = enabled;
									function.m_LuaFunction = std::make_unique<LuabindObjectWrapper>(new luabind::object(luabind::from_stack(m_State, -1)), path);
								} else {
									problems.push_back("invalid callback for object " + std::to_string(uid));
								}
								lua_pop(m_State, 1);
							} else {
								problems.push_back("invalid callback record for object " + std::to_string(uid));
							}
							lua_pop(m_State, 1);
						}
					} else {
						problems.push_back("invalid callback list for object " + std::to_string(uid));
					}
					lua_pop(m_State, 1);
				}
			}
			lua_pop(m_State, 1);
		}
	}
	lua_settop(m_State, top);
}

void LuaStateWrapper::StashScriptObject(long uniqueID) {
	const std::string key = std::to_string(uniqueID);
	RunScriptString("_ScriptFieldsStash = _ScriptFieldsStash or {}; _ScriptedObjects = _ScriptedObjects or {}; _ScriptFieldsStash[\"" + key + "\"] = _ScriptedObjects[\"" + key + "\"];");
}

void LuaStateWrapper::UnstashScriptObject(long uniqueID) {
	const std::string key = std::to_string(uniqueID);
	RunScriptString("if _ScriptFieldsStash then _ScriptedObjects[\"" + key + "\"] = _ScriptFieldsStash[\"" + key + "\"]; _ScriptFieldsStash[\"" + key + "\"] = nil; end");
}

double LuaStateWrapper::GetScriptObjectNumberField(long uniqueID, const std::string& field, double fallback) {
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);
	PushScriptObjectInstanceTable(m_State, uniqueID);
	double value = fallback;
	if (lua_istable(m_State, -1)) {
		lua_getfield(m_State, -1, field.c_str());
		if (lua_isnumber(m_State, -1)) {
			value = lua_tonumber(m_State, -1);
		}
		lua_pop(m_State, 1);
	}
	lua_pop(m_State, 1);
	return value;
}

LuaStateWrapper::LuaStateWrapper() {
	Clear();
}

LuaStateWrapper::~LuaStateWrapper() {
	Destroy();
}

void LuaStateWrapper::Clear() {
	m_State = nullptr;
	m_TempEntity = nullptr;
	m_TempEntityVector.clear();
	m_LastError.clear();
	m_CurrentlyRunningScriptPath = "";
}

void LuaStateWrapper::Initialize() {
	m_State = luaL_newstate();
	luabind::open(m_State);
	tracy::LuaRegister(m_State);

	// We do async GC, but we still keep the normal GC on so it can catch any big spikes or runaway allocs
	//lua_gc(m_State, LUA_GCSTOP, 0);

	const luaL_Reg libsToLoad[] = {
	    // Basic Lua libraries
	    {LUA_COLIBNAME, luaopen_base},
	    {LUA_LOADLIBNAME, luaopen_package},
	    {LUA_TABLIBNAME, luaopen_table},
	    {LUA_STRLIBNAME, luaopen_string},
	    {LUA_MATHLIBNAME, luaopen_math},
	    {LUA_DBLIBNAME, luaopen_debug},

		// These were removed for "security reasons" but we need them for debugger integration
	    {LUA_IOLIBNAME, luaopen_io},
	    {LUA_OSLIBNAME, luaopen_os},

		// LuaJIT libraries
	    {LUA_BITLIBNAME, luaopen_bit},
	    {LUA_FFILIBNAME, luaopen_ffi},
	    {LUA_JITLIBNAME, luaopen_jit},

	    {NULL, NULL} // End of array
	};

	for (const luaL_Reg* lib = libsToLoad; lib->func; lib++) {
		if (g_SettingsMan.DisableLuaJIT() && strcmp(lib->name, LUA_JITLIBNAME) == 0) {
			continue;
		}
		lua_pushcfunction(m_State, lib->func);
		lua_pushstring(m_State, lib->name);
		lua_call(m_State, 1, 0);
	}

	// LuaJIT should start automatically after we load the library (if we loaded it) but we're making sure it did anyway.
	if (!g_SettingsMan.DisableLuaJIT() && !luaJIT_setmode(m_State, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_ON)) {
		RTEAbort("Failed to initialize LuaJIT!\nIf this error persists, please disable LuaJIT with \"Settings.ini\" property \"DisableLuaJIT\".");
	}

	// Replace os.time / os.clock with sim-tick stubs so sim Lua can't read the wall clock.
	RegisterDeterministicOsStubs(m_State);

	// Route math.atan/atan2 through the cross-platform poly so AI ballistics don't diverge on the platform libm.
	RegisterDeterministicMathOverrides(m_State);

	// From LuaBind documentation:
	// As mentioned in the Lua documentation, it is possible to pass an error handler function to lua_pcall(). LuaBind makes use of lua_pcall() internally when calling member functions and free functions.
	// It is possible to set the error handler function that LuaBind will use globally:
	// set_pcall_callback(&AddFileAndLineToError); // NOTE: this seems to do nothing because retrieving the error from the lua stack wasn't done correctly. The current error handling works just fine but might look into doing this properly sometime later.

	// Register all relevant bindings to the state. Note that the order of registration is important, as bindings can't derive from an unregistered type (inheritance and all that).
	luabind::module(m_State)[luabind::class_<LuaStateWrapper>("LuaManager")
	                             .property("TempEntity", &LuaStateWrapper::GetTempEntity)
	                             .property("TempEntities", &LuaStateWrapper::GetTempEntityVector, luabind::return_stl_iterator)
	                             .def("SelectRand", &LuaStateWrapper::SelectRand)
	                             .def("RangeRand", &LuaStateWrapper::RangeRand)
	                             .def("PosRand", &LuaStateWrapper::PosRand)
	                             .def("NormalRand", &LuaStateWrapper::NormalRand)
	                             .def("GetDirectoryList", &LuaStateWrapper::DirectoryList, luabind::return_stl_iterator_owned)
	                             .def("GetFileList", &LuaStateWrapper::FileList, luabind::return_stl_iterator_owned)
	                             .def("FileExists", &LuaStateWrapper::FileExists)
	                             .def("DirectoryExists", &LuaStateWrapper::DirectoryExists)
	                             .def("FileOpen", &LuaStateWrapper::FileOpen)
	                             .def("FileClose", &LuaStateWrapper::FileClose)
	                             .def("FileRemove", &LuaStateWrapper::FileRemove)
	                             .def("DirectoryCreate", &LuaStateWrapper::DirectoryCreate1)
	                             .def("DirectoryCreate", &LuaStateWrapper::DirectoryCreate2)
	                             .def("DirectoryRemove", &LuaStateWrapper::DirectoryRemove1)
	                             .def("DirectoryRemove", &LuaStateWrapper::DirectoryRemove2)
	                             .def("FileRename", &LuaStateWrapper::FileRename)
	                             .def("DirectoryRename", &LuaStateWrapper::DirectoryRename)
	                             .def("FileReadLine", &LuaStateWrapper::FileReadLine)
	                             .def("FileWriteLine", &LuaStateWrapper::FileWriteLine)
	                             .def("FileEOF", &LuaStateWrapper::FileEOF),

	                         luabind::def("DeleteEntity", &LuaAdaptersUtility::DeleteEntity, luabind::adopt(_1)), // NOT a member function, so adopting _1 instead of the _2 for the first param, since there's no "this" pointer!!
	                         luabind::def("LERP", (float (*)(float, float, float, float, float))&Lerp),
	                         luabind::def("Lerp", (float (*)(float, float, float, float, float))&Lerp),
	                         luabind::def("Lerp", (Vector(*)(float, float, Vector, Vector, float))&Lerp),
	                         luabind::def("Lerp", (Matrix(*)(float, float, const Matrix&, const Matrix&, float))&Lerp),
	                         luabind::def("EaseIn", &EaseIn),
	                         luabind::def("EaseOut", &EaseOut),
	                         luabind::def("EaseInOut", &EaseInOut),
	                         luabind::def("Clamp", &Limit),
	                         luabind::def("NormalizeAngleBetween0And2PI", &NormalizeAngleBetween0And2PI),
	                         luabind::def("NormalizeAngleBetweenNegativePIAndPI", &NormalizeAngleBetweenNegativePIAndPI),
	                         luabind::def("AngleWithinRange", &AngleWithinRange),
	                         luabind::def("ClampAngle", &ClampAngle),
	                         luabind::def("GetPPM", &LuaAdaptersUtility::GetPPM),
	                         luabind::def("GetMPP", &LuaAdaptersUtility::GetMPP),
	                         luabind::def("GetPPL", &LuaAdaptersUtility::GetPPL),
	                         luabind::def("GetLPP", &LuaAdaptersUtility::GetLPP),
	                         luabind::def("GetPathFindingFlyingJumpHeight", &LuaAdaptersUtility::GetPathFindingFlyingJumpHeight),
	                         luabind::def("GetPathFindingDefaultDigStrength", &LuaAdaptersUtility::GetPathFindingDefaultDigStrength),
	                         luabind::def("RoundFloatToPrecision", &RoundFloatToPrecision),
	                         luabind::def("RoundToNearestMultiple", &RoundToNearestMultiple),

	                         RegisterLuaBindingsOfType(SystemLuaBindings, Vector),
	                         RegisterLuaBindingsOfType(SystemLuaBindings, Box),
	                         RegisterLuaBindingsOfType(EntityLuaBindings, Entity),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, SoundContainer),
	                         RegisterLuaBindingsOfType(EntityLuaBindings, SoundSet),
	                         RegisterLuaBindingsOfType(EntityLuaBindings, LimbPath),
	                         RegisterLuaBindingsOfAbstractType(EntityLuaBindings, SceneObject),
	                         RegisterLuaBindingsOfAbstractType(EntityLuaBindings, MovableObject),
	                         RegisterLuaBindingsOfType(EntityLuaBindings, Material),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, MOPixel),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, TerrainObject),
	                         RegisterLuaBindingsOfAbstractType(EntityLuaBindings, MOSprite),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, MOSParticle),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, MOSRotating),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, Attachable),
	                         RegisterLuaBindingsOfAbstractType(EntityLuaBindings, Emission),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, AEmitter),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, AEJetpack),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, PEmitter),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, Actor),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, ADoor),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, Arm),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, Leg),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, AHuman),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, ACrab),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, Turret),
	                         RegisterLuaBindingsOfAbstractType(EntityLuaBindings, ACraft),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, ACDropShip),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, ACRocket),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, HeldDevice),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, Magazine),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, Round),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, HDFirearm),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, ThrownDevice),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, TDExplosive),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, PieSlice),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, PieMenu),
	                         RegisterLuaBindingsOfType(EntityLuaBindings, Gib),
	                         RegisterLuaBindingsOfType(SystemLuaBindings, Controller),
	                         RegisterLuaBindingsOfType(SystemLuaBindings, Timer),
	                         RegisterLuaBindingsOfType(SystemLuaBindings, PathRequest),
	                         RegisterLuaBindingsOfConcreteType(EntityLuaBindings, Scene),
	                         RegisterLuaBindingsOfType(EntityLuaBindings, SceneArea),
	                         RegisterLuaBindingsOfType(EntityLuaBindings, StaticSceneLayer),
	                         RegisterLuaBindingsOfType(EntityLuaBindings, SLBackground),
	                         RegisterLuaBindingsOfAbstractType(EntityLuaBindings, Deployment),
	                         RegisterLuaBindingsOfType(SystemLuaBindings, DataModule),
	                         RegisterLuaBindingsOfType(ActivityLuaBindings, Activity),
	                         RegisterLuaBindingsOfAbstractType(ActivityLuaBindings, GameActivity),
	                         RegisterLuaBindingsOfAbstractType(EntityLuaBindings, GlobalScript),
	                         RegisterLuaBindingsOfType(EntityLuaBindings, MetaPlayer),
	                         RegisterLuaBindingsOfType(GUILuaBindings, GUIBanner),
	                         RegisterLuaBindingsOfType(GUILuaBindings, BuyMenuGUI),
	                         RegisterLuaBindingsOfType(GUILuaBindings, SceneEditorGUI),
	                         RegisterLuaBindingsOfType(ManagerLuaBindings, ActivityMan),
	                         RegisterLuaBindingsOfType(ManagerLuaBindings, AudioMan),
	                         RegisterLuaBindingsOfType(ManagerLuaBindings, MusicMan),
	                         RegisterLuaBindingsOfType(ManagerLuaBindings, CameraMan),
	                         RegisterLuaBindingsOfType(ManagerLuaBindings, ConsoleMan),
	                         RegisterLuaBindingsOfType(ManagerLuaBindings, FrameMan),
	                         RegisterLuaBindingsOfType(ManagerLuaBindings, MetaMan),
	                         RegisterLuaBindingsOfType(ManagerLuaBindings, MovableMan),
	                         RegisterLuaBindingsOfType(ManagerLuaBindings, PerformanceMan),
	                         RegisterLuaBindingsOfType(ManagerLuaBindings, PostProcessMan),
	                         RegisterLuaBindingsOfType(ManagerLuaBindings, PresetMan),
	                         RegisterLuaBindingsOfType(ManagerLuaBindings, PrimitiveMan),
	                         RegisterLuaBindingsOfType(ManagerLuaBindings, SceneMan),
	                         RegisterLuaBindingsOfType(ManagerLuaBindings, SettingsMan),
	                         RegisterLuaBindingsOfType(ManagerLuaBindings, MetricsCollector),
	                         RegisterLuaBindingsOfType(ManagerLuaBindings, TimerMan),
	                         RegisterLuaBindingsOfType(ManagerLuaBindings, UInputMan),
	                         RegisterLuaBindingsOfType(PrimitiveLuaBindings, GraphicalPrimitive),
	                         RegisterLuaBindingsOfType(PrimitiveLuaBindings, LinePrimitive),
	                         RegisterLuaBindingsOfType(PrimitiveLuaBindings, ArcPrimitive),
	                         RegisterLuaBindingsOfType(PrimitiveLuaBindings, SplinePrimitive),
	                         RegisterLuaBindingsOfType(PrimitiveLuaBindings, BoxPrimitive),
	                         RegisterLuaBindingsOfType(PrimitiveLuaBindings, BoxFillPrimitive),
	                         RegisterLuaBindingsOfType(PrimitiveLuaBindings, RoundedBoxPrimitive),
	                         RegisterLuaBindingsOfType(PrimitiveLuaBindings, RoundedBoxFillPrimitive),
	                         RegisterLuaBindingsOfType(PrimitiveLuaBindings, CirclePrimitive),
	                         RegisterLuaBindingsOfType(PrimitiveLuaBindings, CircleFillPrimitive),
	                         RegisterLuaBindingsOfType(PrimitiveLuaBindings, EllipsePrimitive),
	                         RegisterLuaBindingsOfType(PrimitiveLuaBindings, EllipseFillPrimitive),
	                         RegisterLuaBindingsOfType(PrimitiveLuaBindings, TrianglePrimitive),
	                         RegisterLuaBindingsOfType(PrimitiveLuaBindings, TriangleFillPrimitive),
	                         RegisterLuaBindingsOfType(PrimitiveLuaBindings, TextPrimitive),
	                         RegisterLuaBindingsOfType(PrimitiveLuaBindings, BitmapPrimitive),
	                         RegisterLuaBindingsOfType(InputLuaBindings, InputDevice),
	                         RegisterLuaBindingsOfType(InputLuaBindings, InputElements),
	                         RegisterLuaBindingsOfType(InputLuaBindings, JoyButtons),
	                         RegisterLuaBindingsOfType(InputLuaBindings, JoyDirections),
	                         RegisterLuaBindingsOfType(InputLuaBindings, MouseButtons),
	                         RegisterLuaBindingsOfType(InputLuaBindings, SDL_Keycode),
	                         RegisterLuaBindingsOfType(InputLuaBindings, SDL_Scancode),
	                         RegisterLuaBindingsOfType(InputLuaBindings, SDL_GamepadButton),
	                         RegisterLuaBindingsOfType(InputLuaBindings, SDL_GamepadAxis),
	                         RegisterLuaBindingsOfType(MiscLuaBindings, AlarmEvent),
	                         RegisterLuaBindingsOfType(MiscLuaBindings, Directions),
	                         RegisterLuaBindingsOfType(MiscLuaBindings, DrawBlendMode),
	                         RegisterLuaBindingsOfType(MiscLuaBindings, DrawDepth)];

	// Assign the manager instances to globals in the lua master state
	luabind::globals(m_State)["TimerMan"] = &g_TimerMan;
	luabind::globals(m_State)["FrameMan"] = &g_FrameMan;
	luabind::globals(m_State)["PerformanceMan"] = &g_PerformanceMan;
	luabind::globals(m_State)["PostProcessMan"] = &g_PostProcessMan;
	luabind::globals(m_State)["PrimitiveMan"] = &g_PrimitiveMan;
	luabind::globals(m_State)["PresetMan"] = &g_PresetMan;
	luabind::globals(m_State)["AudioMan"] = &g_AudioMan;
	luabind::globals(m_State)["MusicMan"] = &g_MusicMan;
	luabind::globals(m_State)["UInputMan"] = &g_UInputMan;
	luabind::globals(m_State)["SceneMan"] = &g_SceneMan;
	luabind::globals(m_State)["ActivityMan"] = &g_ActivityMan;
	luabind::globals(m_State)["MetaMan"] = &g_MetaMan;
	luabind::globals(m_State)["MovableMan"] = &g_MovableMan;
	luabind::globals(m_State)["CameraMan"] = &g_CameraMan;
	luabind::globals(m_State)["ConsoleMan"] = &g_ConsoleMan;
	luabind::globals(m_State)["LuaMan"] = this;
	luabind::globals(m_State)["SettingsMan"] = &g_SettingsMan;
	luabind::globals(m_State)["MetricsCollector"] = &g_MetricsCollector;

	// Don't draw from the sim RNG here — it would couple the RNG stream to thread count (SeedAllLuaRNGs re-seeds per state at activity start)
	m_RandomGenerator.Seed(0);

	luaL_dostring(m_State,
	              "package.path = package.path .. \";Data/Base.rte/LuaIntegration/?.lua;Data/Base.rte/LuaIntegration/?/?.lua;\"\n"
	              "package.cpath = package.cpath .. \";Data/Base.rte/LuaIntegration/?.dll;Data/Base.rte/LuaIntegration/?/?.dll;\"\n"
	              "package.cpath = package.cpath .. \";Data/Base.rte/LuaIntegration/?.so;Data/Base.rte/LuaIntegration/?/?.so;\"\n"
	              // Add cls() as a shortcut to ConsoleMan:Clear().
	              "cls = function() ConsoleMan:Clear(); end\n"
	              // Override "print" in the lua state to output to the console.
	              "print = function(stringToPrint) ConsoleMan:PrintString(\"PRINT: \" .. tostring(stringToPrint)); end\n"
	              // Override random functions to appear global instead of under LuaMan
	              "SelectRand = function(lower, upper) return LuaMan:SelectRand(lower, upper); end;\n"
	              "RangeRand = function(lower, upper) return LuaMan:RangeRand(lower, upper); end;\n"
	              "PosRand = function() return LuaMan:PosRand(); end;\n"
	              "NormalRand = function() return LuaMan:NormalRand(); end;\n"
	              // Override "math.random" in the lua state to use RTETools MT19937 implementation. Preserve return types of original to not break all the things.
	              "math.random = function(lower, upper) if lower ~= nil and upper ~= nil then return LuaMan:SelectRand(lower, upper); elseif lower ~= nil then return LuaMan:SelectRand(1, lower); else return LuaMan:PosRand(); end end\n"
	              // Override "dofile"/"loadfile" to be able to account for Data/ or Mods/ directory.
	              "do local OriginalDoFile = dofile; dofile = function(filePath) filePath = PresetMan:GetFullModulePath(filePath); if filePath ~= '' then return OriginalDoFile(filePath); end end; end\n"
	              "do local OriginalLoadFile = loadfile; loadfile = function(filePath) filePath = PresetMan:GetFullModulePath(filePath); if filePath ~= '' then return OriginalLoadFile(filePath); end end; end\n"
	              // Override "require" to be able to track loaded packages so we can clear them when scripts are reloaded.
	              "_RequiredPackages = {};\n"
	              "do local OriginalRequire = require; require = function(filePath) _RequiredPackages[filePath] = true; return OriginalRequire(filePath); end; end\n"
	              "_ClearRequiredPackages = function() for k, v in pairs(_RequiredPackages) do package.loaded[k] = nil; end; _RequiredPackages = {}; end;\n"
	              // Internal helper functions to add callbacks for async pathing requests
	              "_AsyncPathCallbacks = {};\n"
	              "_AddAsyncPathCallback = function(id, callback) _AsyncPathCallbacks[id] = callback; end\n"
	              "_TriggerAsyncPathCallback = function(id, param) if _AsyncPathCallbacks[id] ~= nil then _AsyncPathCallbacks[id](param); _AsyncPathCallbacks[id] = nil; end end\n");

	LoadScriptGraphHelper();
	CaptureScriptGraphBaseline();

	if (g_SettingsMan.EnableLuaDebugging()) {
		luaL_dostring(m_State, "require(\"mobdebug\").coro(); require(\"mobdebug\").start();");
	}
}

void LuaStateWrapper::Destroy() {
	lua_close(m_State);
}

// During a threaded per-MO hook these draw from the per-MO generator; else this state's RNG.
int LuaStateWrapper::SelectRand(int minInclusive, int maxInclusive) {
	RandomGenerator& rng = s_luaRNGOverride ? *s_luaRNGOverride : m_RandomGenerator;
	return rng.RandomNum<int>(minInclusive, maxInclusive);
}

double LuaStateWrapper::RangeRand(double minInclusive, double maxInclusive) {
	RandomGenerator& rng = s_luaRNGOverride ? *s_luaRNGOverride : m_RandomGenerator;
	return rng.RandomNum<double>(minInclusive, maxInclusive);
}

double LuaStateWrapper::NormalRand() {
	RandomGenerator& rng = s_luaRNGOverride ? *s_luaRNGOverride : m_RandomGenerator;
	return rng.RandomNormalNum<double>();
}

double LuaStateWrapper::PosRand() {
	RandomGenerator& rng = s_luaRNGOverride ? *s_luaRNGOverride : m_RandomGenerator;
	return rng.RandomNum<double>();
}

void LuaStateWrapper::SeedRandomGenerator(uint64_t seed) {
	m_RandomGenerator.Seed(seed);
}

std::string LuaStateWrapper::GetRandomGeneratorStateForHashing() const {
	return m_RandomGenerator.SerializeStateForHashing();
}

// Passthrough LuaMan Functions
const std::vector<std::string>* LuaStateWrapper::DirectoryList(const std::string& path) { return g_LuaMan.DirectoryList(path); }
const std::vector<std::string>* LuaStateWrapper::FileList(const std::string& path) { return g_LuaMan.FileList(path); }
bool LuaStateWrapper::FileExists(const std::string& path) { return g_LuaMan.FileExists(path); }
bool LuaStateWrapper::DirectoryExists(const std::string& path) { return g_LuaMan.DirectoryExists(path); }
int LuaStateWrapper::FileOpen(const std::string& path, const std::string& accessMode) { return g_LuaMan.FileOpen(path, accessMode); }
void LuaStateWrapper::FileClose(int fileIndex) { return g_LuaMan.FileClose(fileIndex); }
void LuaStateWrapper::FileCloseAll() { return g_LuaMan.FileCloseAll(); }
bool LuaStateWrapper::FileRemove(const std::string& path) { return g_LuaMan.FileRemove(path); }
bool LuaStateWrapper::DirectoryCreate1(const std::string& path) { return g_LuaMan.DirectoryCreate(path, false); }
bool LuaStateWrapper::DirectoryCreate2(const std::string& path, bool recursive) { return g_LuaMan.DirectoryCreate(path, recursive); }
bool LuaStateWrapper::DirectoryRemove1(const std::string& path) { return g_LuaMan.DirectoryRemove(path, false); }
bool LuaStateWrapper::DirectoryRemove2(const std::string& path, bool recursive) { return g_LuaMan.DirectoryRemove(path, recursive); }
bool LuaStateWrapper::FileRename(const std::string& oldPath, const std::string& newPath) { return g_LuaMan.FileRename(oldPath, newPath); }
bool LuaStateWrapper::DirectoryRename(const std::string& oldPath, const std::string& newPath) { return g_LuaMan.DirectoryRename(oldPath, newPath); }
std::string LuaStateWrapper::FileReadLine(int fileIndex) { return g_LuaMan.FileReadLine(fileIndex); }
void LuaStateWrapper::FileWriteLine(int fileIndex, const std::string& line) { return g_LuaMan.FileWriteLine(fileIndex, line); }
bool LuaStateWrapper::FileEOF(int fileIndex) { return g_LuaMan.FileEOF(fileIndex); }

void LuaMan::Clear() {
	m_OpenedFiles.fill(nullptr);
	ResetPathCallbacks();
}

void LuaMan::Initialize() {
	m_MasterScriptState.Initialize();

	int luaStateCount = std::thread::hardware_concurrency();
	if (g_SettingsMan.EnableLuaDebugging()) {
		luaStateCount = 0;
	} else if (g_SettingsMan.GetNumberOfLuaStatesOverride() != -1) {
		luaStateCount = g_SettingsMan.GetNumberOfLuaStatesOverride();
	}

	m_ScriptStates = std::vector<LuaStateWrapper>(luaStateCount);
	for (LuaStateWrapper& luaState: m_ScriptStates) {
		luaState.Initialize();
	}
}

LuaStateWrapper& LuaMan::GetMasterScriptState() {
	return m_MasterScriptState;
}

LuaStatesArray& LuaMan::GetThreadedScriptStates() {
	return m_ScriptStates;
}

int LuaMan::GetStateIndex(const LuaStateWrapper* state) const {
	if (!state) {
		return -1;
	}
	if (state == &m_MasterScriptState) {
		return 0;
	}
	for (size_t i = 0; i < m_ScriptStates.size(); ++i) {
		if (&m_ScriptStates[i] == state) {
			return static_cast<int>(i) + 1;
		}
	}
	return -1;
}

LuaStateWrapper& LuaMan::GetStateByIndex(int index) {
	if (index <= 0 || m_ScriptStates.empty()) {
		return m_MasterScriptState;
	}
	return m_ScriptStates[static_cast<size_t>(index - 1) % m_ScriptStates.size()];
}

bool LuaMan::RunScriptGraphSelfTest() {
	lua_State* state = m_MasterScriptState.GetLuaState();
	const int id = AllocatePathCallback(m_PathCallbacks, state);
	m_MasterScriptState.RunScriptString("_PathCallbackPurgeTest = 0; _AddAsyncPathCallback(" + std::to_string(id) + ", function(result) _PathCallbackPurgeTest = _PathCallbackPurgeTest + result.PathLength end)");
	PathRequest result;
	result.pathLength = 7;
	CompletePathCallback(m_PathCallbacks, state, id, result);
	g_MovableMan.PurgeAllMOs();
	ExecuteLuaScriptCallbacks();
	lua_getglobal(state, "_PathCallbackPurgeTest");
	const bool purgePreserved = lua_tonumber(state, -1) == 7;
	lua_pop(state, 1);
	m_MasterScriptState.RunScriptString("_PathCallbackPurgeTest = nil");
	ResetPathCallbacks(true);
	std::cout << "[script-graph-selftest] " << (purgePreserved ? "PASS" : "FAIL") << " native_path_callback_survives_purge" << std::endl;
	return m_MasterScriptState.RunScriptGraphSelfTest() && purgePreserved;
}

bool LuaStateWrapper::RunScriptGraphSelfTest() {
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);
	LoadScriptGraphHelper();
	bool checkpointValues = GUICheckpoint::RunSelfTest();
	checkpointValues = g_AudioMan.RunCheckpointSelfTest() && checkpointValues;
	checkpointValues = g_AudioMan.RunLogicalPlaybackSelfTest() && checkpointValues;
	checkpointValues = g_MusicMan.RunCheckpointSelfTest() && checkpointValues;
	checkpointValues = g_GUISound.RunCheckpointSelfTest() && checkpointValues;
	checkpointValues = g_UInputMan.RunCheckpointSelfTest() && checkpointValues;
	checkpointValues = System::RunPathCaseSelfTest() && checkpointValues;
	checkpointValues = ContentFile::RunImageLoadSelfTest() && checkpointValues;
	checkpointValues = Reader::RunUnknownPropertySelfTest() && checkpointValues;
	checkpointValues = g_PostProcessMan.RunCheckpointSelfTest() && checkpointValues;
	checkpointValues = g_FrameMan.RunPaletteCheckpointSelfTest() && checkpointValues;
	checkpointValues = BitmapCheckpoint::RunSelfTest() && checkpointValues;
	checkpointValues = PieMenu::RunCheckpointSelfTest() && checkpointValues;
	checkpointValues = Actor::RunBorrowedReferenceSelfTest() && checkpointValues;
	checkpointValues = MOSprite::RunCheckpointSelfTest() && checkpointValues;
	checkpointValues = g_PrimitiveMan.RunCheckpointSelfTest() && checkpointValues;
	{
		PrimitiveMan::QueuesSetAside original;
		g_PrimitiveMan.SetAsideQueues(original);
		const bool primitivesPassed = RunScriptString(R"lua(
local line = LinePrimitive(0, Vector(11, 13), Vector(31, 37), 2.5, 43)
local text = TextPrimitive(1, Vector(17, 23), "public queue", true, 1, 0.375)
local bitmap = BitmapPrimitive(2, Vector(29, 41), "Base.rte/GUIs/Skins/Cursor.png", 0.25, false, true)
assert(not pcall(function() PrimitiveMan:DrawPrimitives(73, {line, "invalid"}) end), "mixed table accepted")
assert(({_ScriptGraphNative(line)})[1] == "copy", "mixed table stole native ownership")
assert(not pcall(function() PrimitiveMan:DrawPrimitives(999, 73, {text}) end), "invalid blend accepted")
assert(({_ScriptGraphNative(text)})[1] == "copy", "invalid blend stole native ownership")
assert(not pcall(function() PrimitiveMan:DrawPrimitives(73, {line, line}) end), "duplicate primitive accepted")
assert(({_ScriptGraphNative(line)})[1] == "copy", "duplicate primitive stole native ownership")
assert(_ScriptGraphOwnerReference(PrimitiveMan, "primitive", 0, false) == nil, "rejected call changed queue")
PrimitiveMan:DrawPrimitives(73, {line, text, bitmap})
for _, value in ipairs({line, text, bitmap}) do
    local descriptor = {_ScriptGraphNative(value)}
    assert(descriptor[1] == "owner-ref", "public table submission lost owner")
    local queued = _ScriptGraphOwnerReference(PrimitiveMan, "primitive", descriptor[4], false)
    assert(_ScriptGraphNativeAddress(value) == _ScriptGraphNativeAddress(queued), "public table submission changed pointer")
    assert(getmetatable(value) == getmetatable(queued), "queue resolver changed concrete class")
end
local shared = Vector(19.25, -31.5)
PrimitiveMan:DrawPolygonPrimitive(2, Vector(211, 229), 107, {shared, Vector(5, 7), shared})
PrimitiveMan:DrawPolygonFillPrimitive(3, Vector(223, 233), 109, {shared, Vector(11, 13), Vector(17, 19)})
local descriptor = {_ScriptGraphNative(shared)}
assert(descriptor[1] == "owner-ref", "shared polygon vertex lost owner")
local queued = _ScriptGraphOwnerReference(PrimitiveMan, "primitive-vertex", descriptor[4], false)
assert(_ScriptGraphNativeAddress(shared) == _ScriptGraphNativeAddress(queued), "shared polygon vertex changed identity")
assert(shared.X == 19.25 and shared.Y == -31.5, "shared polygon vertex changed value")
line.contractLabel = "retained queued line"
_PrimitiveQueueCapture = {line=line, alias=line, text=text, bitmap=bitmap, vertex=shared, vertexAlias=shared}
local bytes, problems = _ScriptGraph.serialize({})
assert(#problems == 0, table.concat(problems, "; "))
local restored, failures = _ScriptGraph.deserialize(bytes)
assert(restored and #failures == 0, table.concat(failures, "; "))
assert(rawequal(_PrimitiveQueueCapture.line, _PrimitiveQueueCapture.alias), "queued userdata alias lost")
assert(rawequal(_PrimitiveQueueCapture.vertex, _PrimitiveQueueCapture.vertexAlias), "queued vertex alias lost")
assert(_ScriptGraphNativeAddress(_PrimitiveQueueCapture.line) == _ScriptGraphNativeAddress(line), "queued graph owner changed")
assert(_PrimitiveQueueCapture.line.contractLabel == "retained queued line", "queued instance field lost")
assert(getmetatable(_PrimitiveQueueCapture.line) == getmetatable(line), "queued graph class changed")
_PrimitiveQueueCapture = nil
)lua") == 0;
		g_PrimitiveMan.ClearPrimitivesQueue();
		g_PrimitiveMan.ReinstateQueues(original);
		std::cout << "[primitive-lua-selftest] " << (primitivesPassed ? "PASS" : "FAIL") << " public_tables_atomic_adoption_shared_vertices_concrete_types" << std::endl;
		checkpointValues = primitivesPassed && checkpointValues;
	}
	checkpointValues = g_SceneMan.RunMaterialCheckpointSelfTest() && checkpointValues;
	{
		Controller source, restored;
		source.SetInputMode(Controller::CIM_AI);
		source.SetPlayerRaw(-1);
		source.SetAnalogMove(Vector(0.375F, -0.625F));
		source.SetAnalogAim(Vector(-0.75F, 0.125F));
		source.SetState(ControlState::WEAPON_FIRE, true);
		const std::string checkpoint = source.SaveCheckpoint();
		checkpointValues = restored.LoadCheckpoint(checkpoint) && restored.SaveCheckpoint() == checkpoint && checkpointValues;
		for (const std::string& invalid: {checkpoint.substr(0, checkpoint.size() - 2), checkpoint + "trailing"}) {
			checkpointValues = !restored.LoadCheckpoint(invalid) && restored.SaveCheckpoint() == checkpoint && checkpointValues;
		}
		Controller clone;
		clone.CopyCheckpointFrom(source);
		checkpointValues = clone.SaveCheckpoint() == checkpoint && checkpointValues;
		GameActivity activity;
		activity.SetDifficulty(77);
		const std::string activityState = activity.SaveCheckpoint();
		activity.SetDifficulty(33);
		checkpointValues = activity.LoadCheckpoint(activityState) && activity.GetDifficulty() == 77 && activity.SaveCheckpoint() == activityState && checkpointValues;
	}
	std::cout << "[script-graph-selftest] " << (checkpointValues ? "PASS" : "FAIL") << " native_runtime_checkpoint_values" << std::endl;
	// A script-owned sound that has lost its last Lua reference keeps its playing voice until the
	// collector sweeps it, so a capture taken before that names an owner no restore can produce.
	bool settledSoundOwner = false;
	{
		const std::string originalAudio = g_AudioMan.SaveCheckpoint();
		RunScriptString("_CheckpointSoundOwner = CreateSoundContainer(\"Funds Changed\", \"Base.rte\"); _CheckpointSoundOwner.Loops = -1; _CheckpointSoundOwner.Immobile = true; _CheckpointSoundOwner.Paused = true; _CheckpointSoundOwnerPlayed = _CheckpointSoundOwner:Play()");
		lua_getglobal(m_State, "_CheckpointSoundOwnerPlayed");
		const bool played = lua_toboolean(m_State, -1) != 0;
		lua_pop(m_State, 1);
		RunScriptString("_CheckpointSoundOwner = nil; _CheckpointSoundOwnerPlayed = nil");
		g_ActivityMan.CaptureRuntimeGlobals();
		const std::string captured = g_AudioMan.SaveCheckpoint();
		g_LuaMan.CollectGarbageForCheckpoint();
		settledSoundOwner = played && g_AudioMan.LoadCheckpoint(captured);
		settledSoundOwner = g_AudioMan.LoadCheckpoint(originalAudio) && settledSoundOwner;
	}
	std::cout << "[script-graph-selftest] " << (settledSoundOwner ? "PASS" : "FAIL") << " checkpoint_settles_script_owned_sound" << std::endl;
	// A script-owned object parked where no graph root reaches it is detached by the restore and
	// never rebuilt, so the checkpoint must not demand it back and must refuse to promise it.
	bool unreachableOwner = false;
	{
		MovableMan::ConstructionRegistryScope registryScope;
		const bool created = RunScriptString(
			"_ScriptedObjects = _ScriptedObjects or {};"
			"_ScriptedObjects[\"scriptgraphselftest\"] = { held = CreateMOPixel(\"Spark Yellow 1\", \"Base.rte\") };"
			"_ScriptGraphSelfTestUID = _ScriptedObjects[\"scriptgraphselftest\"].held.UniqueID") == 0;
		lua_getglobal(m_State, "_ScriptGraphSelfTestUID");
		MovableObject* hidden = g_MovableMan.FindObjectByUniqueID(static_cast<long>(lua_tonumber(m_State, -1)));
		lua_pop(m_State, 1);
		std::vector<std::string> graphs;
		std::vector<std::string> graphProblems;
		if (created && hidden) {
			const std::string references = g_MovableMan.SaveCheckpoint();
			g_MovableMan.UnregisterObject(hidden);
			unreachableOwner = g_MovableMan.LoadCheckpoint(references);
			g_MovableMan.RegisterObject(hidden);
			unreachableOwner = g_MovableMan.SerializeScriptGraphs(graphs, graphProblems) && unreachableOwner;
			MOPixel target;
			target.Create();
			hidden->SetWhichMOToNotHit(&target, 10.0F);
			graphs.clear();
			graphProblems.clear();
			unreachableOwner = !g_MovableMan.SerializeScriptGraphs(graphs, graphProblems) && unreachableOwner;
			unreachableOwner = graphProblems.size() == 1 && graphProblems.front().starts_with("a script-owned MOPixel") && unreachableOwner;
			hidden->SetWhichMOToNotHit(nullptr, 0.0F);
		}
		RunScriptString("_ScriptedObjects[\"scriptgraphselftest\"] = nil; _ScriptGraphSelfTestUID = nil");
		g_LuaMan.CollectGarbageForCheckpoint();
	}
	std::cout << "[script-graph-selftest] " << (unreachableOwner ? "PASS" : "FAIL") << " checkpoint_drops_unreachable_script_owner" << std::endl;
	checkpointValues = unreachableOwner && checkpointValues;
	// The collector can sweep a script-owned object while a construction scope holds a registry copy,
	// so putting that copy back must not name the object the sweep destroyed.
	bool scopeForgetsDestroyed = false;
	{
		auto* object = new MOPixel;
		object->Create();
		const long identity = object->GetUniqueID();
		{
			MovableMan::ConstructionRegistryScope registryScope;
			delete object;
		}
		scopeForgetsDestroyed = identity > 0 && g_MovableMan.FindObjectByUniqueID(identity) == nullptr;
	}
	std::cout << "[script-graph-selftest] " << (scopeForgetsDestroyed ? "PASS" : "FAIL") << " construction_scope_forgets_destroyed_owner" << std::endl;
	// A world set aside for an in-memory restore leaves its script-owned trees in the heap while the
	// restored copies hold their identities. Only the live registration reaches the reference map,
	// so the shadowed original is not an owner any restore can fail to find.
	bool shadowedOwner = false;
	{
		MovableMan::ConstructionRegistryScope registryScope;
		const bool created = RunScriptString(
			"_ScriptedObjects = _ScriptedObjects or {};"
			"_ScriptedObjects[\"scriptgraphselftest\"] = { held = CreateMOPixel(\"Spark Yellow 1\", \"Base.rte\") };"
			"_ScriptGraphSelfTestUID = _ScriptedObjects[\"scriptgraphselftest\"].held.UniqueID") == 0;
		lua_getglobal(m_State, "_ScriptGraphSelfTestUID");
		MovableObject* hidden = g_MovableMan.FindObjectByUniqueID(static_cast<long>(lua_tonumber(m_State, -1)));
		lua_pop(m_State, 1);
		std::vector<std::string> graphs;
		std::vector<std::string> graphProblems;
		if (created && hidden) {
			MOPixel target;
			target.Create();
			hidden->SetWhichMOToNotHit(&target, 10.0F);
			shadowedOwner = !g_MovableMan.SerializeScriptGraphs(graphs, graphProblems);
			MOPixel replacement;
			MovableObject::PinUniqueIDCounter(hidden->GetUniqueID() - 1);
			replacement.Create();
			shadowedOwner = replacement.GetUniqueID() == hidden->GetUniqueID() && shadowedOwner;
			shadowedOwner = g_MovableMan.FindObjectByUniqueID(hidden->GetUniqueID()) == &replacement && shadowedOwner;
			graphs.clear();
			graphProblems.clear();
			shadowedOwner = g_MovableMan.SerializeScriptGraphs(graphs, graphProblems) && shadowedOwner;
			hidden->SetWhichMOToNotHit(nullptr, 0.0F);
		}
		RunScriptString("_ScriptedObjects[\"scriptgraphselftest\"] = nil; _ScriptGraphSelfTestUID = nil");
		g_LuaMan.CollectGarbageForCheckpoint();
	}
	std::cout << "[script-graph-selftest] " << (shadowedOwner ? "PASS" : "FAIL") << " checkpoint_ignores_shadowed_script_owner" << std::endl;
	checkpointValues = shadowedOwner && checkpointValues;
	bool nativeLifetime = true;
	{
		MOPixel object;
		object.Create();
		const long original = object.GetUniqueID();
		object.Create();
		nativeLifetime = g_MovableMan.FindObjectByUniqueID(original) == nullptr && g_MovableMan.FindObjectByUniqueID(object.GetUniqueID()) == &object;
		const long replaced = object.GetUniqueID();
		object.MovableObject::Reset();
		nativeLifetime = g_MovableMan.FindObjectByUniqueID(replaced) == nullptr && nativeLifetime;
		Turret turret;
		nativeLifetime = turret.GetFirstMountedDevice() == nullptr && nativeLifetime;
		auto* mounted = static_cast<HeldDevice*>(g_PresetMan.GetEntityPreset("HDFirearm", "SMG")->Clone());
		const long mountedUID = mounted->GetUniqueID();
		turret.SetFirstMountedDevice(mounted);
		turret.Destroy();
		nativeLifetime = turret.GetFirstMountedDevice() == nullptr && g_MovableMan.FindObjectByUniqueID(mountedUID) == nullptr && nativeLifetime;
		ACDropShip ship;
		const float lateralControl = ship.GetLateralControl();
		ship.SetLateralControlSpeed(3.75F);
		nativeLifetime = ship.GetLateralControl() == lateralControl && ship.GetLateralControlSpeed() == 3.75F && nativeLifetime;
	}
	std::cout << "[script-graph-selftest] " << (nativeLifetime ? "PASS" : "FAIL") << " native_recreate_and_turret_lifetime" << std::endl;
	bool registryLifetime = false;
	{
		const int top = lua_gettop(m_State);
		lua_State* coroutine = lua_newthread(m_State);
		luabind::weak_ref coroutineWitness(m_State, -1);
		{
			lua_newtable(coroutine);
			lua_pushinteger(coroutine, 73);
			lua_setfield(coroutine, -2, "value");
			luabind::weak_ref weak(coroutine, -1);
			luabind::detail::lua_reference strong;
			strong.set(coroutine);
			const bool stable = strong.state() == m_State && weak.state() == m_State;
			if (stable) {
				lua_pop(m_State, 1);
				lua_gc(m_State, LUA_GCCOLLECT, 0);
				coroutineWitness.get(m_State);
				registryLifetime = lua_isnil(m_State, -1);
				lua_pop(m_State, 1);
				luabind::detail::lua_reference copy(strong);
				copy.get(m_State);
				weak.get(m_State);
				registryLifetime = lua_rawequal(m_State, -1, -2) && registryLifetime;
				lua_getfield(m_State, -1, "value");
				registryLifetime = lua_tointeger(m_State, -1) == 73 && registryLifetime;
				lua_pop(m_State, 3);
			}
		}
		lua_settop(m_State, top);
	}
	std::cout << "[script-graph-selftest] " << (registryLifetime ? "PASS" : "FAIL") << " native_reference_outlives_coroutine" << std::endl;
	bool randomRoundtrip = true;
	int randomCases = 0;
	for (uint64_t seed: {0ULL, 42ULL, 0xffffffffULL, 0xfedcba9876543210ULL}) {
		for (int skip: {0, 1, 226, 227, 396, 397, 623, 624, 625, 9999, 100000}) {
			RandomGenerator source;
			source.Seed(seed);
			for (int i = 0; i < skip; ++i) {
				source.RandomNum<float>();
			}
			const std::string saved = source.SerializeCheckpoint();
			RandomGenerator restored;
			randomRoundtrip = restored.RestoreCheckpoint(saved) && restored.SerializeCheckpoint() == saved && randomRoundtrip;
			auto expected = source.GetEngineState();
			auto actual = restored.GetEngineState();
			for (int i = 0; i < 10000; ++i) {
				randomRoundtrip = actual() == expected() && randomRoundtrip;
			}
			for (const std::string& bad: {std::string(), "MT2" + saved.substr(3), saved.substr(0, saved.rfind(' ')), saved + " extra"}) {
				randomRoundtrip = !restored.RestoreCheckpoint(bad) && restored.SerializeCheckpoint() == saved && randomRoundtrip;
			}
			++randomCases;
		}
	}
	std::cout << "[script-graph-selftest] " << (randomRoundtrip ? "PASS" : "FAIL") << " portable_rng_continuation " << randomCases << " cases, 10000 outputs each" << std::endl;
	bool soundSetCopies = true;
	{
		SoundSet leaf;
		leaf.SetSoundSelectionCycleMode(SoundSet::ALL);
		SoundSet branch;
		branch.AddSoundSet(leaf);
		SoundSet source;
		source.AddSoundSet(branch);
		SoundSet copy(source);
		copy.GetSubSoundSets()[0]->GetSubSoundSets()[0]->SetSoundSelectionCycleMode(SoundSet::FORWARDS);
		soundSetCopies = source.GetSubSoundSets()[0]->GetSubSoundSets()[0]->GetSoundSelectionCycleMode() == SoundSet::ALL;
		SoundSet assigned;
		assigned.AddSoundSet(leaf);
		assigned = source;
		assigned.GetSubSoundSets()[0]->GetSubSoundSets()[0]->SetSoundSelectionCycleMode(SoundSet::RANDOM);
		soundSetCopies = assigned.GetSubSoundSets().size() == 1 && source.GetSubSoundSets()[0]->GetSubSoundSets()[0]->GetSoundSelectionCycleMode() == SoundSet::ALL && soundSetCopies;
		assigned = *assigned.GetSubSoundSets()[0];
		soundSetCopies = assigned.GetSubSoundSets().size() == 1 && assigned.GetSubSoundSets()[0]->GetSoundSelectionCycleMode() == SoundSet::RANDOM && soundSetCopies;
		assigned.Reset();
		soundSetCopies = assigned.GetSubSoundSets().empty() && assigned.GetSoundSelectionCycleMode() == SoundSet::RANDOM && soundSetCopies;
	}
	std::cout << "[script-graph-selftest] " << (soundSetCopies ? "PASS" : "FAIL") << " native_sound_set_deep_copy" << std::endl;
	std::string bytes;
	for (int value = 0; value < 256; ++value) {
		bytes.push_back(static_cast<char>(value));
	}
	bytes.append(3, '\xff');
	auto stream = std::make_unique<std::ostringstream>();
	auto* written = stream.get();
	Writer writer(std::move(stream));
	writer.NewPropertyWithValue("LuaStateGraph", base64_encode(bytes, true));
	writer.NewLine(false);
	Reader reader(std::make_unique<std::istringstream>(written->str()), "script-graph-selftest.ini", false, nullptr, true);
	const bool textRoundtrip = reader.ReadPropName() == "LuaStateGraph" && base64_decode(reader.ReadPropValue()) == bytes;
	std::cout << "[script-graph-selftest] " << (textRoundtrip ? "PASS" : "FAIL") << " save_text_binary_roundtrip" << std::endl;
	lua_State* L = m_State;
	// Each check echoes as it runs so a crash mid-chunk still names the last check reached.
	lua_pushcfunction(L, [](lua_State* state) -> int {
		std::cout << "[script-graph-progress] " << luaL_checkstring(state, 1) << std::endl;
		return 0;
	});
	lua_setglobal(L, "_ScriptGraphProgress");
	if (luaL_loadstring(L, c_ScriptGraphSelfTest) != 0 || lua_pcall(L, 0, 1, 0) != 0) {
		std::cout << "[script-graph-selftest] ERROR: " << (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?") << std::endl;
		lua_pop(L, 1);
		return false;
	}
	const std::string report = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
	lua_pop(L, 1);
	std::cout << report << std::endl;
	const bool pass = checkpointValues && settledSoundOwner && scopeForgetsDestroyed && nativeLifetime && registryLifetime && randomRoundtrip && soundSetCopies && textRoundtrip && !report.empty() && report.find("FAIL") == std::string::npos;
	std::cout << "[script-graph-selftest] " << (pass ? "PASS" : "FAIL") << std::endl;
	return pass;
}

thread_local LuaStateWrapper* s_luaStateOverride = nullptr;
LuaStateWrapper* LuaMan::GetThreadLuaStateOverride() const {
	return s_luaStateOverride;
}

void LuaMan::SetThreadLuaStateOverride(LuaStateWrapper* luaState) {
	s_luaStateOverride = luaState;
}

thread_local LuaStateWrapper* s_currentLuaState = nullptr;
LuaStateWrapper* LuaMan::GetThreadCurrentLuaState() const {
	return s_currentLuaState;
}

LuaStateWrapper* LuaMan::GetAndLockFreeScriptState() {
	if (s_luaStateOverride) {
		// We're creating this object in a multithreaded environment, ensure that it's assigned to the same script state as us
		bool success = s_luaStateOverride->GetMutex().try_lock();
		RTEAssert(success, "Our lua state override for our thread already belongs to another thread!") return s_luaStateOverride;
	}

	// TODO
	// It would be nice to assign to least-saturated state, but that's a bit tricky with MO registering...
	/*auto itr = std::min_element(m_ScriptStates.begin(), m_ScriptStates.end(),
	    [](const LuaStateWrapper& lhs, const LuaStateWrapper& rhs) { return lhs.GetRegisteredMOs().size() < rhs.GetRegisteredMOs().size(); }
	);

	bool success = itr->GetMutex().try_lock();
	RTEAssert(success, "Script mutex was already locked while in a non-multithreaded environment!");

	return &(*itr);*/

	int ourState = m_LastAssignedLuaState;
	m_LastAssignedLuaState = (m_LastAssignedLuaState + 1) % m_ScriptStates.size();

	bool success = m_ScriptStates[ourState].GetMutex().try_lock();
	RTEAssert(success, "Script mutex was already locked while in a non-multithreaded environment!");

	return &m_ScriptStates[ourState];
}

void LuaMan::ClearUserModuleCache() {
	m_GarbageCollectionTask.wait();

	m_MasterScriptState.ClearLuaScriptCache();
	for (LuaStateWrapper& luaState: m_ScriptStates) {
		luaState.ClearLuaScriptCache();
	}

	m_MasterScriptState.ClearUserModuleCache();
	for (LuaStateWrapper& luaState: m_ScriptStates) {
		luaState.ClearUserModuleCache();
	}
}

int LuaMan::AllocatePathCallback(const std::shared_ptr<LuaPathCallbackContext>& context, lua_State* state) {
	std::scoped_lock lock(context->mutex);
	return context->nextId[state]++;
}

static void DispatchPathCallback(const std::shared_ptr<LuaPathCallbackContext>& context, const LuaPathCallbackContext::Request& request) {
	request.scene->CalculatePathAsync(request.start, request.end, request.jumpHeight, request.digStrength, request.team,
	    [context, state = request.state, id = request.id](std::shared_ptr<volatile PathRequest> result) {
		    LuaMan::CompletePathCallback(context, state, id, const_cast<const PathRequest&>(*result));
	    });
}

void LuaMan::StartPathCallback(const std::shared_ptr<LuaPathCallbackContext>& context, lua_State* state, int id, Scene* scene, const Vector& start, const Vector& end, float jumpHeight, float digStrength, int team) {
	const LuaPathCallbackContext::Request request{state, id, scene, start, end, jumpHeight, digStrength, static_cast<Activity::Teams>(team), true};
	{
		std::scoped_lock lock(context->mutex);
		context->pending.push_back(request);
	}
	DispatchPathCallback(context, request);
}

void LuaMan::CompletePathCallback(const std::shared_ptr<LuaPathCallbackContext>& context, lua_State* state, int id, const PathRequest& result) {
	auto copy = std::make_shared<PathRequest>(result);
	copy->complete = true;
	std::scoped_lock lock(context->mutex);
	context->incoming.push_back({state, id, 0, std::move(copy)});
}

void LuaMan::ResetPathCallbacks(bool clearLua) {
	m_PathCallbacks = std::make_shared<LuaPathCallbackContext>();
	if (clearLua) {
		const auto clear = [](LuaStateWrapper& state) { if (state.GetLuaState()) state.RunScriptString("_AsyncPathCallbacks = {}"); };
		clear(m_MasterScriptState);
		for (LuaStateWrapper& state: m_ScriptStates) clear(state);
	}
}

void LuaMan::SwapPathCallbacks(std::shared_ptr<LuaPathCallbackContext>& context) {
	m_PathCallbacks.swap(context);
	if (!m_PathCallbacks) ResetPathCallbacks();
}

void LuaMan::BeginPathCallbackCapture() {
	m_PathCallbackCapture = std::make_shared<LuaPathCallbackContext>();
	std::scoped_lock lock(m_PathCallbacks->mutex);
	m_PathCallbackCapture->nextId = m_PathCallbacks->nextId;
	m_PathCallbackCapture->nextOrder = m_PathCallbacks->nextOrder;
	m_PathCallbackCapture->callbacks = m_PathCallbacks->callbacks;
	m_PathCallbackCapture->pending = m_PathCallbacks->pending;
}

void LuaMan::EndPathCallbackCapture() {
	m_PathCallbackCapture.reset();
}

void LuaMan::PushPathCallbacks(lua_State* state) {
	const auto context = m_PathCallbackCapture ? m_PathCallbackCapture : m_PathCallbacks;
	int nextId;
	uint64_t nextOrder;
	std::vector<LuaPathCallbackContext::Callback> callbacks;
	std::vector<LuaPathCallbackContext::Request> pending;
	{
		std::scoped_lock lock(context->mutex);
		const auto found = context->nextId.find(state);
		nextId = found == context->nextId.end() ? 0 : found->second;
		nextOrder = context->nextOrder;
		for (const auto& callback: context->callbacks) if (callback.state == state) callbacks.push_back(callback);
		for (const auto& request: context->pending) if (request.state == state) pending.push_back(request);
	}
	lua_newtable(state);
	lua_pushinteger(state, nextId);
	lua_setfield(state, -2, "nextId");
	lua_pushnumber(state, static_cast<lua_Number>(nextOrder));
	lua_setfield(state, -2, "nextOrder");
	lua_newtable(state);
	int index = 0;
	for (const auto& callback: callbacks) {
		lua_newtable(state);
		lua_pushinteger(state, callback.id);
		lua_setfield(state, -2, "id");
		lua_pushnumber(state, static_cast<lua_Number>(callback.order));
		lua_setfield(state, -2, "order");
		luabind::object(state, *callback.result).push(state);
		lua_setfield(state, -2, "result");
		lua_rawseti(state, -2, ++index);
	}
	lua_setfield(state, -2, "entries");
	lua_newtable(state);
	index = 0;
	for (const auto& request: pending) {
		lua_newtable(state);
		lua_pushinteger(state, request.id);
		lua_setfield(state, -2, "id");
		luabind::object(state, request.scene).push(state);
		lua_setfield(state, -2, "scene");
		const auto number = [state](const char* name, lua_Number value) { lua_pushnumber(state, value); lua_setfield(state, -2, name); };
		number("startX", request.start.m_X);
		number("startY", request.start.m_Y);
		number("endX", request.end.m_X);
		number("endY", request.end.m_Y);
		number("jumpHeight", request.jumpHeight);
		number("digStrength", request.digStrength);
		number("team", request.team);
		lua_rawseti(state, -2, ++index);
	}
	lua_setfield(state, -2, "pending");
}

bool LuaMan::RestorePathCallbacks(lua_State* state, int index) {
	const int top = lua_gettop(state);
	if (index < 0) index += top + 1;
	lua_getfield(state, index, "nextId");
	const int nextId = lua_tointeger(state, -1);
	lua_pop(state, 1);
	lua_getfield(state, index, "nextOrder");
	const uint64_t nextOrder = static_cast<uint64_t>(lua_tonumber(state, -1));
	lua_pop(state, 1);
	lua_getfield(state, index, "entries");
	bool valid = nextId >= 0 && lua_istable(state, -1);
	std::vector<LuaPathCallbackContext::Callback> callbacks;
	const int count = valid ? static_cast<int>(lua_objlen(state, -1)) : 0;
	for (int entry = 1; entry <= count && valid; ++entry) {
		lua_rawgeti(state, -1, entry);
		if (!lua_istable(state, -1)) { valid = false; break; }
		lua_getfield(state, -1, "id");
		const int id = lua_tointeger(state, -1);
		lua_pop(state, 1);
		lua_getfield(state, -1, "order");
		const uint64_t order = static_cast<uint64_t>(lua_tonumber(state, -1));
		lua_pop(state, 1);
		lua_getfield(state, -1, "result");
		const auto* value = luabind::detail::is_class_object(state, -1);
		valid = id >= 0 && id < nextId && order < nextOrder && value && std::strcmp(value->crep()->name(), "PathRequest") == 0;
		if (valid) callbacks.push_back({state, id, order, std::make_shared<PathRequest>(*static_cast<const PathRequest*>(value->ptr()))});
		lua_pop(state, 2);
	}
	lua_settop(state, top);
	lua_getfield(state, index, "pending");
	valid = valid && (lua_isnil(state, -1) || lua_istable(state, -1));
	std::vector<LuaPathCallbackContext::Request> pending;
	const int pendingCount = valid && lua_istable(state, -1) ? static_cast<int>(lua_objlen(state, -1)) : 0;
	for (int entry = 1; entry <= pendingCount && valid; ++entry) {
		lua_rawgeti(state, -1, entry);
		if (!lua_istable(state, -1)) { valid = false; break; }
		const auto number = [state, &valid](const char* name) {
			lua_getfield(state, -1, name);
			valid = valid && lua_isnumber(state, -1);
			const lua_Number result = lua_tonumber(state, -1);
			lua_pop(state, 1);
			return result;
		};
		LuaPathCallbackContext::Request request{};
		request.state = state;
		request.id = static_cast<int>(number("id"));
		request.start.m_X = static_cast<float>(number("startX"));
		request.start.m_Y = static_cast<float>(number("startY"));
		request.end.m_X = static_cast<float>(number("endX"));
		request.end.m_Y = static_cast<float>(number("endY"));
		request.jumpHeight = static_cast<float>(number("jumpHeight"));
		request.digStrength = static_cast<float>(number("digStrength"));
		request.team = static_cast<Activity::Teams>(static_cast<int>(number("team")));
		lua_getfield(state, -1, "scene");
		const auto* value = luabind::detail::is_class_object(state, -1);
		valid = valid && request.id >= 0 && request.id < nextId && request.team >= Activity::Teams::NoTeam && request.team <= Activity::Teams::TeamFour && value && std::strcmp(value->crep()->name(), "Scene") == 0 && value->ptr();
		if (valid) {
			request.scene = static_cast<Scene*>(value->ptr());
			pending.push_back(request);
		}
		lua_pop(state, 2);
	}
	lua_settop(state, top);
	if (!valid) return false;
	std::scoped_lock lock(m_PathCallbacks->mutex);
	std::erase_if(m_PathCallbacks->callbacks, [state](const auto& callback) { return callback.state == state; });
	m_PathCallbacks->callbacks.insert(m_PathCallbacks->callbacks.end(), callbacks.begin(), callbacks.end());
	std::erase_if(m_PathCallbacks->pending, [state](const auto& request) { return request.state == state; });
	m_PathCallbacks->pending.insert(m_PathCallbacks->pending.end(), pending.begin(), pending.end());
	m_PathCallbacks->nextId[state] = nextId;
	m_PathCallbacks->nextOrder = std::max(m_PathCallbacks->nextOrder, nextOrder);
	m_PathCallbacks->orderPending = true;
	return true;
}

void LuaMan::ResumePathCallbacks() {
	std::vector<LuaPathCallbackContext::Request> pending;
	{
		std::scoped_lock lock(m_PathCallbacks->mutex);
		for (auto& request: m_PathCallbacks->pending) {
			if (!request.submitted) {
				request.submitted = true;
				pending.push_back(request);
			}
		}
	}
	for (const auto& request: pending) DispatchPathCallback(m_PathCallbacks, request);
}

void LuaMan::ExecuteLuaScriptCallbacks() {
	static const long publishAfter = []() { const char* value = std::getenv("CC_TEST_ASYNC_PATH_PUBLICATION_TICK"); return value ? std::strtol(value, nullptr, 10) : 0L; }();
	if (publishAfter <= 0 || g_TimerMan.GetSimUpdateCount() >= publishAfter) {
		std::scoped_lock lock(m_PathCallbacks->mutex);
		for (auto& callback: m_PathCallbacks->incoming) {
			std::erase_if(m_PathCallbacks->pending, [&callback](const auto& request) { return request.state == callback.state && request.id == callback.id; });
			callback.order = m_PathCallbacks->nextOrder++;
			m_PathCallbacks->callbacks.push_back(std::move(callback));
		}
		m_PathCallbacks->incoming.clear();
	}
	static const long holdUntil = []() { const char* value = std::getenv("CC_TEST_ASYNC_PATH_DELIVERY_TICK"); return value ? std::strtol(value, nullptr, 10) : 0L; }();
	if (holdUntil > 0 && g_TimerMan.GetSimUpdateCount() < holdUntil) return;
	std::vector<LuaPathCallbackContext::Callback> callbacks;

	{
		std::scoped_lock lock(m_PathCallbacks->mutex);
		if (m_PathCallbacks->orderPending) {
			std::stable_sort(m_PathCallbacks->callbacks.begin(), m_PathCallbacks->callbacks.end(), [](const auto& a, const auto& b) { return a.order < b.order; });
			m_PathCallbacks->orderPending = false;
		}
		callbacks.swap(m_PathCallbacks->callbacks);
	}

	for (const auto& callback: callbacks) {
		luabind::call_function<void>(callback.state, "_TriggerAsyncPathCallback", callback.id, *callback.result);
	}
}

const std::unordered_map<std::string, PerformanceMan::ScriptTiming> LuaMan::GetScriptTimings() const {
	std::unordered_map<std::string, PerformanceMan::ScriptTiming> timings = m_MasterScriptState.GetScriptTimings();
	for (const LuaStateWrapper& luaState: m_ScriptStates) {
		for (auto&& [functionName, timing]: luaState.GetScriptTimings()) {
			auto& existing = timings[functionName];
			existing.m_CallCount += timing.m_CallCount;
			existing.m_Time = std::max(existing.m_Time, timing.m_Time);
		}
	}
	return timings;
}

void LuaMan::Destroy() {
	for (int i = 0; i < c_MaxOpenFiles; ++i) {
		FileClose(i);
	}
	Clear();
}

void LuaStateWrapper::ClearUserModuleCache() {
	luaL_dostring(m_State, "_ClearRequiredPackages();");
}

void LuaStateWrapper::ClearLuaScriptCache() {
	for (const auto& [path, cached]: m_ScriptCache) {
		for (const auto& [name, function]: cached.functionNamesAndObjects) {
			delete function;
		}
	}
	m_ScriptCache.clear();
}

Entity* LuaStateWrapper::GetTempEntity() const {
	return m_TempEntity;
}

void LuaStateWrapper::SetTempEntity(Entity* entity) {
	m_TempEntity = entity;
}

const std::vector<Entity*>& LuaStateWrapper::GetTempEntityVector() const {
	return m_TempEntityVector;
}

void LuaStateWrapper::SetTempEntityVector(const std::vector<const Entity*>& entityVector) {
	m_TempEntityVector.reserve(entityVector.size());
	for (const Entity* entity: entityVector) {
		m_TempEntityVector.push_back(const_cast<Entity*>(entity));
	}
}

void LuaStateWrapper::SetLuaPath(const std::string& filePath) {
	const std::string moduleName = g_PresetMan.GetModuleNameFromPath(filePath);
	// A bundled non-official module (the determinism Tests.rte) ships in Data/, not Mods/, so its
	// require() path must resolve there too — mirror PresetMan::GetFullModulePath.
	const std::string moduleFolder = (g_PresetMan.IsModuleOfficial(moduleName) || std::filesystem::exists(System::GetWorkingDirectory() + System::GetDataDirectory() + moduleName)) ? System::GetDataDirectory() : System::GetModDirectory();
	const std::string scriptPath = moduleFolder + moduleName + "/?.lua";

	lua_getglobal(m_State, "package");
	lua_getfield(m_State, -1, "path"); // get field "path" from table at top of stack (-1).
	std::string currentPath = lua_tostring(m_State, -1); // grab path string from top of stack.

	// check if scriptPath is already in there, if not add it.
	if (currentPath.find(scriptPath) == std::string::npos) {
		currentPath.append(";" + scriptPath);
	}

	lua_pop(m_State, 1); // get rid of the string on the stack we just pushed previously.
	lua_pushstring(m_State, currentPath.c_str()); // push the new one.
	lua_setfield(m_State, -2, "path"); // set the field "path" in table at -2 with value at top of stack.
	lua_pop(m_State, 1); // get rid of package table from top of stack.
}

const std::unordered_map<std::string, PerformanceMan::ScriptTiming>& LuaStateWrapper::GetScriptTimings() const {
	return m_ScriptTimings;
}

int LuaStateWrapper::RunScriptFunctionString(const std::string& functionName, const std::string& selfObjectName, const std::vector<std::string_view>& variablesToSafetyCheck, const std::vector<const Entity*>& functionEntityArguments, const std::vector<std::string_view>& functionLiteralArguments) {
	std::stringstream scriptString;
	if (!variablesToSafetyCheck.empty()) {
		scriptString << "if ";
		for (const std::string_view& variableToSafetyCheck: variablesToSafetyCheck) {
			if (&variableToSafetyCheck != &variablesToSafetyCheck[0]) {
				scriptString << " and ";
			}
			scriptString << variableToSafetyCheck;
		}
		scriptString << " then ";
	}
	if (!functionEntityArguments.empty()) {
		scriptString << "local entityArguments = LuaMan.TempEntities; ";
	}

	// Lock here, even though we also lock in RunScriptString(), to ensure that the temp entity vector isn't stomped by separate threads.
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);
	s_currentLuaState = this;

	scriptString << functionName + "(";
	if (!selfObjectName.empty()) {
		scriptString << selfObjectName;
	}
	bool isFirstFunctionArgument = selfObjectName.empty();
	if (!functionEntityArguments.empty()) {
		SetTempEntityVector(functionEntityArguments);
		for (const Entity* functionEntityArgument: functionEntityArguments) {
			if (!isFirstFunctionArgument) {
				scriptString << ", ";
			}
			scriptString << "(To" + functionEntityArgument->GetClassName() + " and To" + functionEntityArgument->GetClassName() + "(entityArguments()) or entityArguments())";
			isFirstFunctionArgument = false;
		}
	}
	if (!functionLiteralArguments.empty()) {
		for (const std::string_view& functionLiteralArgument: functionLiteralArguments) {
			if (!isFirstFunctionArgument) {
				scriptString << ", ";
			}
			scriptString << std::string(functionLiteralArgument);
			isFirstFunctionArgument = false;
		}
	}
	scriptString << ");";

	if (!variablesToSafetyCheck.empty()) {
		scriptString << " end;";
	}

	int result = RunScriptString(scriptString.str());
	m_TempEntityVector.clear();
	return result;
}

int LuaStateWrapper::RunScriptString(const std::string& scriptString, bool consoleErrors) {
	if (scriptString.empty()) {
		return -1;
	}
	int error = 0;

	std::lock_guard<std::recursive_mutex> lock(m_Mutex);
	s_currentLuaState = this;

	lua_pushcfunction(m_State, &AddFileAndLineToError);
	// Load the script string onto the stack and then execute it with pcall. Pcall will call the file and line error handler if there's an error by pointing 2 up the stack to it.
	if (luaL_loadstring(m_State, scriptString.c_str()) || lua_pcall(m_State, 0, LUA_MULTRET, -2)) {
		// Retrieve the error message then pop it off the stack to clean it up
		m_LastError = lua_tostring(m_State, -1);
		lua_pop(m_State, 1);
		if (consoleErrors) {
			g_ConsoleMan.PrintString("ERROR: " + m_LastError);
			ClearErrors();
		}
		error = -1;
	}
	// Pop the file and line error handler off the stack to clean it up
	lua_pop(m_State, 1);

	return error;
}

int LuaStateWrapper::RunScriptFunctionObject(const LuabindObjectWrapper* functionObject, const std::string& selfGlobalTableName, const std::string& selfGlobalTableKey, const std::vector<const Entity*>& functionEntityArguments, const std::vector<std::string_view>& functionLiteralArguments, const std::vector<LuabindObjectWrapper*>& functionObjectArguments) {
	int status = 0;

	std::lock_guard<std::recursive_mutex> lock(m_Mutex);
	s_currentLuaState = this;
	m_CurrentlyRunningScriptPath = functionObject->GetFilePath();

	lua_pushcfunction(m_State, &AddFileAndLineToError);
	functionObject->GetLuabindObject()->push(m_State);

	int argumentCount = functionEntityArguments.size() + functionLiteralArguments.size() + functionObjectArguments.size();
	if (!selfGlobalTableName.empty() && TableEntryIsDefined(selfGlobalTableName, selfGlobalTableKey)) {
		lua_getglobal(m_State, selfGlobalTableName.c_str());
		lua_getfield(m_State, -1, selfGlobalTableKey.c_str());
		lua_remove(m_State, -2);
		argumentCount++;
	}

	for (const Entity* functionEntityArgument: functionEntityArguments) {
		std::unique_ptr<LuabindObjectWrapper> downCastEntityAsLuabindObjectWrapper(LuaAdaptersEntityCast::s_EntityToLuabindObjectCastFunctions.at(functionEntityArgument->GetClassName())(const_cast<Entity*>(functionEntityArgument), m_State));
		downCastEntityAsLuabindObjectWrapper->GetLuabindObject()->push(m_State);
	}

	for (const std::string_view& functionLiteralArgument: functionLiteralArguments) {
		char* stringToDoubleConversionFailed = nullptr;
		if (functionLiteralArgument == "nil") {
			lua_pushnil(m_State);
		} else if (functionLiteralArgument == "true" || functionLiteralArgument == "false") {
			lua_pushboolean(m_State, functionLiteralArgument == "true" ? 1 : 0);
		} else if (double argumentAsNumber = std::strtod(functionLiteralArgument.data(), &stringToDoubleConversionFailed); !*stringToDoubleConversionFailed) {
			lua_pushnumber(m_State, argumentAsNumber);
		} else {
			lua_pushlstring(m_State, functionLiteralArgument.data(), functionLiteralArgument.size());
		}
	}

	for (const LuabindObjectWrapper* functionObjectArgument: functionObjectArguments) {
		if (functionObjectArgument->GetLuabindObject()->interpreter() != m_State) {
			LuabindObjectWrapper copy = functionObjectArgument->GetCopyForState(*m_State);
			copy.GetLuabindObject()->push(m_State);
		} else {
			functionObjectArgument->GetLuabindObject()->push(m_State);
		}
	}
	const std::string& path = functionObject->GetFilePath();

	// Function object may be deleted during the Lua call, making `path` above invalid.
	// Find and store the script timings entry now and write to it afterward.
	PerformanceMan::ScriptTiming* timing = nullptr;

	// only track time in non-MT scripts, for now
	if (&g_LuaMan.GetMasterScriptState() == this) {
		timing = &m_ScriptTimings[path];
	}

	std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
	{
		ZoneScoped;
		ZoneName(path.c_str(), path.length());

		if (lua_pcall(m_State, argumentCount, LUA_MULTRET, -argumentCount - 2) > 0) {
			m_LastError = lua_tostring(m_State, -1);
			lua_pop(m_State, 1);
			g_ConsoleMan.PrintString("ERROR: " + m_LastError);
			ClearErrors();
			status = -1;
		}
	}
	std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

	if (timing) {
		timing->m_Time += std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
		timing->m_CallCount++;
	}

	lua_pop(m_State, 1);

	m_CurrentlyRunningScriptPath = "";
	return status;
}

int LuaStateWrapper::RunScriptConditionalTestFunctionObject(const LuabindObjectWrapper* functionObject, const std::string& selfGlobalTableName, const std::string& selfGlobalTableKey, bool& returnParam, const std::vector<const Entity*>& functionEntityArguments, const std::vector<std::string_view>& functionLiteralArguments, const std::vector<LuabindObjectWrapper*>& functionObjectArguments) {
	int status = 0;

	std::lock_guard<std::recursive_mutex> lock(m_Mutex);
	s_currentLuaState = this;
	m_CurrentlyRunningScriptPath = functionObject->GetFilePath();

	lua_pushcfunction(m_State, &AddFileAndLineToError);
	functionObject->GetLuabindObject()->push(m_State);

	int argumentCount = functionEntityArguments.size() + functionLiteralArguments.size() + functionObjectArguments.size();
	if (!selfGlobalTableName.empty() && TableEntryIsDefined(selfGlobalTableName, selfGlobalTableKey)) {
		lua_getglobal(m_State, selfGlobalTableName.c_str());
		lua_getfield(m_State, -1, selfGlobalTableKey.c_str());
		lua_remove(m_State, -2);
		argumentCount++;
	}

	for (const Entity* functionEntityArgument: functionEntityArguments) {
		std::unique_ptr<LuabindObjectWrapper> downCastEntityAsLuabindObjectWrapper(LuaAdaptersEntityCast::s_EntityToLuabindObjectCastFunctions.at(functionEntityArgument->GetClassName())(const_cast<Entity*>(functionEntityArgument), m_State));
		downCastEntityAsLuabindObjectWrapper->GetLuabindObject()->push(m_State);
	}

	for (const std::string_view& functionLiteralArgument: functionLiteralArguments) {
		char* stringToDoubleConversionFailed = nullptr;
		if (functionLiteralArgument == "nil") {
			lua_pushnil(m_State);
		} else if (functionLiteralArgument == "true" || functionLiteralArgument == "false") {
			lua_pushboolean(m_State, functionLiteralArgument == "true" ? 1 : 0);
		} else if (double argumentAsNumber = std::strtod(functionLiteralArgument.data(), &stringToDoubleConversionFailed); !*stringToDoubleConversionFailed) {
			lua_pushnumber(m_State, argumentAsNumber);
		} else {
			lua_pushlstring(m_State, functionLiteralArgument.data(), functionLiteralArgument.size());
		}
	}

	for (const LuabindObjectWrapper* functionObjectArgument: functionObjectArguments) {
		if (functionObjectArgument->GetLuabindObject()->interpreter() != m_State) {
			LuabindObjectWrapper copy = functionObjectArgument->GetCopyForState(*m_State);
			copy.GetLuabindObject()->push(m_State);
		} else {
			functionObjectArgument->GetLuabindObject()->push(m_State);
		}
	}

	const std::string& path = functionObject->GetFilePath();
	std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
	{
		ZoneScoped;
		ZoneName(path.c_str(), path.length());

		if (lua_pcall(m_State, argumentCount, 1, -argumentCount - 2) > 0) {
			m_LastError = lua_tostring(m_State, -1);
			lua_pop(m_State, 1);
			g_ConsoleMan.PrintString("ERROR: " + m_LastError);
			ClearErrors();
			status = -1;
		} else {
			returnParam = 1 == lua_toboolean(m_State, -1);
			lua_pop(m_State, 1);
		}
	}
	std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

	// only track time in non-MT scripts, for now
	if (&g_LuaMan.GetMasterScriptState() == this) {
		m_ScriptTimings[path].m_Time += std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
		m_ScriptTimings[path].m_CallCount++;
	}

	lua_pop(m_State, 1);

	m_CurrentlyRunningScriptPath = "";
	return status;
}

int LuaStateWrapper::RunScriptFile(const std::string& filePath, bool consoleErrors, bool doInSandboxedEnvironment) {
	const std::string fullScriptPath = g_PresetMan.GetFullModulePath(filePath);
	if (fullScriptPath.empty()) {
		m_LastError = "Can't run a script file with an empty filepath!";
		return -1;
	}

	if (!System::PathExistsCaseSensitive(fullScriptPath)) {
		m_LastError = "Script file: " + filePath + " doesn't exist!";
		if (consoleErrors) {
			g_ConsoleMan.PrintString("ERROR: " + m_LastError);
			ClearErrors();
		}
		return -1;
	}

	int error = 0;

	std::lock_guard<std::recursive_mutex> lock(m_Mutex);
	s_currentLuaState = this;
	m_CurrentlyRunningScriptPath = filePath;

	const int stackStart = lua_gettop(m_State);

	lua_pushcfunction(m_State, &AddFileAndLineToError);
	SetLuaPath(fullScriptPath);

	// Load the script file's contents onto the stack
	if (luaL_loadfile(m_State, fullScriptPath.c_str())) {
		m_LastError = lua_tostring(m_State, -1);
		lua_pop(m_State, 1);
		if (consoleErrors) {
			g_ConsoleMan.PrintString("ERROR: " + m_LastError);
			ClearErrors();
		}
		error = -1;
	}

	if (error == 0) {
		if (doInSandboxedEnvironment) {
			// create a new environment table
			lua_getglobal(m_State, filePath.c_str());
			if (lua_isnil(m_State, -1)) {
				lua_pop(m_State, 1);
				lua_newtable(m_State);
				lua_newtable(m_State);
				lua_getglobal(m_State, "_G");
				lua_setfield(m_State, -2, "__index");
				lua_setmetatable(m_State, -2);
				lua_setglobal(m_State, filePath.c_str());
				lua_getglobal(m_State, filePath.c_str());
			}

			lua_setfenv(m_State, -2);
		}

		// execute script file with pcall. Pcall will call the file and line error handler if there's an error by pointing 2 up the stack to it.
		if (lua_pcall(m_State, 0, LUA_MULTRET, -2)) {
			m_LastError = lua_tostring(m_State, -1);
			lua_pop(m_State, 1);
			if (consoleErrors) {
				g_ConsoleMan.PrintString("ERROR: " + m_LastError);
				ClearErrors();
			}
			error = -1;
		}
	}

	// Pop the line error handler off the stack to clean it up
	lua_pop(m_State, 1);

	m_CurrentlyRunningScriptPath = "";
	RTEAssert(lua_gettop(m_State) == stackStart, "Malformed lua stack!");
	return error;
}

bool LuaStateWrapper::RetrieveFunctions(const std::string& funcObjectName, const std::vector<std::string>& functionNamesToLookFor, std::unordered_map<std::string, LuabindObjectWrapper*>& outFunctionNamesAndObjects) {
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);
	s_currentLuaState = this;

	luabind::object funcHoldingObject = luabind::globals(m_State)[funcObjectName.c_str()];
	if (luabind::type(funcHoldingObject) == LUA_TNIL) {
		return false;
	}

	auto& newScript = m_ScriptCache[funcObjectName.c_str()];
	for (auto& pair: newScript.functionNamesAndObjects) {
		delete pair.second;
	}
	newScript.functionNamesAndObjects.clear();
	for (const std::string& functionName: functionNamesToLookFor) {
		luabind::object functionObject = funcHoldingObject[functionName];
		if (luabind::type(functionObject) == LUA_TFUNCTION) {
			luabind::object* functionObjectCopyForStoring = new luabind::object(functionObject);
			newScript.functionNamesAndObjects.try_emplace(functionName, new LuabindObjectWrapper(functionObjectCopyForStoring, funcObjectName));
		}
	}

	for (auto& pair: newScript.functionNamesAndObjects) {
		luabind::object* functionObjectCopyForStoring = new luabind::object(*pair.second->GetLuabindObject());
		outFunctionNamesAndObjects.try_emplace(pair.first, new LuabindObjectWrapper(functionObjectCopyForStoring, funcObjectName));
	}

	return true;
}

int LuaStateWrapper::RunScriptFileAndRetrieveFunctions(const std::string& filePath, const std::vector<std::string>& functionNamesToLookFor, std::unordered_map<std::string, LuabindObjectWrapper*>& outFunctionNamesAndObjects, bool forceReload) {
	static bool disableCaching = false;
	forceReload = forceReload || disableCaching;

	// If it's already cached, we don't need to run it again
	// TODO - fix activity restarting needing to force reload
	auto cachedScript = m_ScriptCache.find(filePath);
	if (!forceReload && cachedScript != m_ScriptCache.end()) {
		for (auto& pair: cachedScript->second.functionNamesAndObjects) {
			if (std::find(functionNamesToLookFor.begin(), functionNamesToLookFor.end(), pair.first) != functionNamesToLookFor.end()) {
				luabind::object* functionObjectCopyForStoring = new luabind::object(*pair.second->GetLuabindObject());
				outFunctionNamesAndObjects.try_emplace(pair.first, new LuabindObjectWrapper(functionObjectCopyForStoring, filePath));
			}
		}

		return 0;
	}

	std::lock_guard<std::recursive_mutex> lock(m_Mutex);
	s_currentLuaState = this;

	if (int error = RunScriptFile(filePath); error < 0) {
		return error;
	}

	if (!RetrieveFunctions(filePath, functionNamesToLookFor, outFunctionNamesAndObjects)) {
		return -1;
	}

	return 0;
}

void LuaStateWrapper::Update() {
	for (MovableObject* mo: m_AddedRegisteredMOs) {
		m_RegisteredMOs.insert(mo);
	}
	m_AddedRegisteredMOs.clear();
}

void LuaStateWrapper::ClearScriptTimings() {
	m_ScriptTimings.clear();
}

bool LuaStateWrapper::ExpressionIsTrue(const std::string& expression, bool consoleErrors) {
	if (expression.empty()) {
		return false;
	}
	bool result = false;

	std::lock_guard<std::recursive_mutex> lock(m_Mutex);

	// Push the script string onto the stack so we can execute it, and then actually try to run it. Assign the result to a dedicated temp global variable.
	if (luaL_dostring(m_State, std::string("ExpressionResult = " + expression + ";").c_str())) {
		m_LastError = std::string("When evaluating Lua expression: ") + lua_tostring(m_State, -1);
		lua_pop(m_State, 1);
		if (consoleErrors) {
			g_ConsoleMan.PrintString("ERROR: " + m_LastError);
			ClearErrors();
		}
		return false;
	}
	// Put the result of the expression on the lua stack and check its value. Need to pop it off the stack afterwards so it leaves the stack unchanged.
	lua_getglobal(m_State, "ExpressionResult");
	result = lua_toboolean(m_State, -1);
	lua_pop(m_State, 1);

	return result;
}

void LuaStateWrapper::SavePointerAsGlobal(void* objectToSave, const std::string& globalName) {
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);

	// Push the pointer onto the Lua stack.
	lua_pushlightuserdata(m_State, objectToSave);
	// Pop and assign that pointer to a global var in the Lua state.
	lua_setglobal(m_State, globalName.c_str());
}

bool LuaStateWrapper::GlobalIsDefined(const std::string& globalName) {
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);

	// Get the var you want onto the stack so we can check it.
	lua_getglobal(m_State, globalName.c_str());
	// Now report if it is nil/null or not.
	bool isDefined = !lua_isnil(m_State, -1);
	// Pop the var so this operation is balanced and leaves the stack as it was.
	lua_pop(m_State, 1);

	return isDefined;
}

bool LuaStateWrapper::TableEntryIsDefined(const std::string& tableName, const std::string& indexName) {
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);

	// Push the table onto the stack, checking if it even exists.
	lua_getglobal(m_State, tableName.c_str());
	if (!lua_istable(m_State, -1)) {
		// Clean up and report that there was nothing properly defined here.
		lua_pop(m_State, 1);
		return false;
	}
	// Push the value at the requested index onto the stack so we can check if it's anything.
	lua_getfield(m_State, -1, indexName.c_str());
	// Now report if it is nil/null or not
	bool isDefined = !lua_isnil(m_State, -1);
	// Pop both the var and the table so this operation is balanced and leaves the stack as it was.
	lua_pop(m_State, 2);

	return isDefined;
}

bool LuaStateWrapper::ErrorExists() const {
	return !m_LastError.empty();
	;
}

std::string LuaStateWrapper::GetLastError() const {
	return m_LastError;
}

void LuaStateWrapper::ClearErrors() {
	m_LastError.clear();
}

std::string LuaStateWrapper::DescribeLuaStack() {
	int indexOfTopOfStack = lua_gettop(m_State);
	if (indexOfTopOfStack == 0) {
		return "The Lua stack is empty.";
	}
	std::stringstream stackDescription;
	stackDescription << "The Lua stack contains " + std::to_string(indexOfTopOfStack) + " elements. From top to bottom, they are:\n";

	for (int i = indexOfTopOfStack; i > 0; --i) {
		switch (int type = lua_type(m_State, i)) {
			case LUA_TBOOLEAN:
				stackDescription << (lua_toboolean(m_State, i) ? "true" : "false");
				break;
			case LUA_TNUMBER:
				stackDescription << std::to_string(lua_tonumber(m_State, i));
				break;
			case LUA_TSTRING:
				stackDescription << lua_tostring(m_State, i);
				break;
			default:
				stackDescription << lua_typename(m_State, type);
				break;
		}
		if (i - 1 > 0) {
			stackDescription << "\n";
		}
	}
	return stackDescription.str();
}

LuaMan::LuaMan() {
	Clear();
}

LuaMan::~LuaMan() {
	Destroy();
}

const std::vector<std::string>* LuaMan::DirectoryList(const std::string& path) {
	std::string fullPath = System::GetWorkingDirectory() + path;
	auto* directoryPaths = new std::vector<std::string>();

	if (fullPath.find("..") == std::string::npos) {
#ifndef _WIN32
		fullPath = GetCaseInsensitiveFullPath(fullPath);
#endif
		if (std::filesystem::exists(fullPath)) {
			for (const auto& entry: std::filesystem::directory_iterator(fullPath)) {
				if (entry.is_directory()) {
					directoryPaths->emplace_back(entry.path().filename().generic_string());
				}
			}
		}
	}
	return directoryPaths;
}

const std::vector<std::string>* LuaMan::FileList(const std::string& path) {
	std::string fullPath = System::GetWorkingDirectory() + path;
	auto* filePaths = new std::vector<std::string>();

	if (fullPath.find("..") == std::string::npos) {
#ifndef _WIN32
		fullPath = GetCaseInsensitiveFullPath(fullPath);
#endif
		if (std::filesystem::exists(fullPath)) {
			for (const auto& entry: std::filesystem::directory_iterator(fullPath)) {
				if (entry.is_regular_file()) {
					filePaths->emplace_back(entry.path().filename().generic_string());
				}
			}
		}
	}
	return filePaths;
}

bool LuaMan::FileExists(const std::string& path) {
	std::string fullPath = System::GetWorkingDirectory() + g_PresetMan.GetFullModulePath(path);
	if (fullPath.find("..") == std::string::npos) {
#ifndef _WIN32
		fullPath = GetCaseInsensitiveFullPath(fullPath);
#endif
		return std::filesystem::is_regular_file(fullPath);
	}
	return false;
}

bool LuaMan::DirectoryExists(const std::string& path) {
	std::string fullPath = System::GetWorkingDirectory() + g_PresetMan.GetFullModulePath(path);
	if (fullPath.find("..") == std::string::npos) {
#ifndef _WIN32
		fullPath = GetCaseInsensitiveFullPath(fullPath);
#endif
		return std::filesystem::is_directory(fullPath);
	}
	return false;
}

// TODO: Move to ModuleMan, once the ModuleMan PR has been merged
bool LuaMan::IsValidModulePath(const std::string& path) {
	return (path.find("..") == std::string::npos) && (path.find(System::GetModulePackageExtension()) != std::string::npos);
}

int LuaMan::FileOpen(const std::string& path, const std::string& accessMode) {
	if (c_FileAccessModes.find(accessMode) == c_FileAccessModes.end()) {
		g_ConsoleMan.PrintString("ERROR: Cannot open file, invalid file access mode specified.");
		return -1;
	}

	int fileIndex = -1;
	for (int i = 0; i < c_MaxOpenFiles; ++i) {
		if (!m_OpenedFiles[i]) {
			fileIndex = i;
			break;
		}
	}
	if (fileIndex == -1) {
		g_ConsoleMan.PrintString("ERROR: Cannot open file, maximum number of files already open.");
		return -1;
	}

	std::string fullPath = System::GetWorkingDirectory() + g_PresetMan.GetFullModulePath(path);
	if (IsValidModulePath(fullPath)) {
#ifdef _WIN32
		FILE* file = fopen(fullPath.c_str(), accessMode.c_str());
#else
		FILE* file = [&fullPath, &accessMode]() -> FILE* {
			if (std::filesystem::exists(fullPath)) {
				return fopen(fullPath.c_str(), accessMode.c_str());
			}

			std::filesystem::path inspectedPath = System::GetWorkingDirectory();
			const std::filesystem::path relativeFilePath = std::filesystem::path(fullPath).lexically_relative(inspectedPath);

			// Iterate over all path parts
			for (std::filesystem::path::const_iterator relativeFilePathIterator = relativeFilePath.begin(); relativeFilePathIterator != relativeFilePath.end(); ++relativeFilePathIterator) {
				bool pathPartExists = false;

				// Iterate over all entries in the path part's directory,
				// to check if the path part is in there case insensitively
				for (const std::filesystem::path& filesystemEntryPath: std::filesystem::directory_iterator(inspectedPath)) {
					if (StringsEqualCaseInsensitive(filesystemEntryPath.filename().generic_string(), relativeFilePathIterator->generic_string())) {
						inspectedPath = filesystemEntryPath;

						// If the path part is found, stop looking for it
						pathPartExists = true;
						break;
					}
				}

				if (!pathPartExists) {
					// If this is the last part, then all directories in relativeFilePath exist, but the file doesn't
					if (std::next(relativeFilePathIterator) == relativeFilePath.end()) {
						return fopen((inspectedPath / relativeFilePath.filename()).generic_string().c_str(), accessMode.c_str());
					}

					// Some directory in relativeFilePath doesn't exist, so the file can't be created
					return nullptr;
				}
			}

			// If the file exists, open it
			return fopen(inspectedPath.generic_string().c_str(), accessMode.c_str());
		}();
#endif
		if (file) {
			m_OpenedFiles[fileIndex] = file;
			return fileIndex;
		}
	}
	g_ConsoleMan.PrintString("ERROR: Failed to open file " + path);
	return -1;
}

void LuaMan::FileClose(int fileIndex) {
	if (fileIndex > -1 && fileIndex < c_MaxOpenFiles && m_OpenedFiles.at(fileIndex)) {
		fclose(m_OpenedFiles[fileIndex]);
		m_OpenedFiles[fileIndex] = nullptr;
	}
}

void LuaMan::FileCloseAll() {
	for (int file = 0; file < c_MaxOpenFiles; ++file) {
		FileClose(file);
	}
}

bool LuaMan::FileRemove(const std::string& path) {
	std::string fullPath = System::GetWorkingDirectory() + g_PresetMan.GetFullModulePath(path);
	if (IsValidModulePath(fullPath)) {
#ifndef _WIN32
		fullPath = GetCaseInsensitiveFullPath(fullPath);
#endif
		if (std::filesystem::is_regular_file(fullPath)) {
			return std::filesystem::remove(fullPath);
		}
	}
	g_ConsoleMan.PrintString("ERROR: Failed to remove file " + path);
	return false;
}

bool LuaMan::DirectoryCreate(const std::string& path, bool recursive) {
	std::string fullPath = System::GetWorkingDirectory() + g_PresetMan.GetFullModulePath(path);
	if (fullPath.find("..") == std::string::npos) {
#ifndef _WIN32
		fullPath = GetCaseInsensitiveFullPath(fullPath);
#endif
		try {
			if (recursive) {
				return std::filesystem::create_directories(fullPath);
			} else {
				return std::filesystem::create_directory(fullPath);
			}
		} catch (const std::filesystem::filesystem_error& e) {}
	}
	g_ConsoleMan.PrintString("ERROR: Failed to remove directory " + path);
	return false;
}

bool LuaMan::DirectoryRemove(const std::string& path, bool recursive) {
	std::string fullPath = System::GetWorkingDirectory() + g_PresetMan.GetFullModulePath(path);
	if (fullPath.find("..") == std::string::npos) {
#ifndef _WIN32
		fullPath = GetCaseInsensitiveFullPath(fullPath);
#endif
		if (std::filesystem::is_directory(fullPath)) {
			try {
				if (recursive) {
					return std::filesystem::remove_all(fullPath) > 0;
				} else {
					return std::filesystem::remove(fullPath);
				}
			} catch (const std::filesystem::filesystem_error& e) {}
		}
	}
	g_ConsoleMan.PrintString("ERROR: Failed to remove directory " + path);
	return false;
}

bool LuaMan::FileRename(const std::string& oldPath, const std::string& newPath) {
	std::string fullOldPath = System::GetWorkingDirectory() + g_PresetMan.GetFullModulePath(oldPath);
	std::string fullNewPath = System::GetWorkingDirectory() + g_PresetMan.GetFullModulePath(newPath);
	if (IsValidModulePath(fullOldPath) && IsValidModulePath(fullNewPath)) {
#ifndef _WIN32
		fullOldPath = GetCaseInsensitiveFullPath(fullOldPath);
		fullNewPath = GetCaseInsensitiveFullPath(fullNewPath);
#endif
		// Ensures parity between Linux which can overwrite an empty directory, while Windows can't
		// Ensures parity between Linux which can't rename a directory to a newPath that is a file in order to overwrite it, while Windows can
		if (std::filesystem::is_regular_file(fullOldPath) && !std::filesystem::exists(fullNewPath)) {
			try {
				std::filesystem::rename(fullOldPath, fullNewPath);
				return true;
			} catch (const std::filesystem::filesystem_error& e) {}
		}
	}
	g_ConsoleMan.PrintString("ERROR: Failed to rename oldPath " + oldPath + " to newPath " + newPath);
	return false;
}

bool LuaMan::DirectoryRename(const std::string& oldPath, const std::string& newPath) {
	std::string fullOldPath = System::GetWorkingDirectory() + g_PresetMan.GetFullModulePath(oldPath);
	std::string fullNewPath = System::GetWorkingDirectory() + g_PresetMan.GetFullModulePath(newPath);
	if (fullOldPath.find("..") == std::string::npos && fullNewPath.find("..") == std::string::npos) {
#ifndef _WIN32
		fullOldPath = GetCaseInsensitiveFullPath(fullOldPath);
		fullNewPath = GetCaseInsensitiveFullPath(fullNewPath);
#endif
		// Ensures parity between Linux which can overwrite an empty directory, while Windows can't
		// Ensures parity between Linux which can't rename a directory to a newPath that is a file in order to overwrite it, while Windows can
		if (std::filesystem::is_directory(fullOldPath) && !std::filesystem::exists(fullNewPath)) {
			try {
				std::filesystem::rename(fullOldPath, fullNewPath);
				return true;
			} catch (const std::filesystem::filesystem_error& e) {}
		}
	}
	g_ConsoleMan.PrintString("ERROR: Failed to rename oldPath " + oldPath + " to newPath " + newPath);
	return false;
}

std::string LuaMan::FileReadLine(int fileIndex) {
	if (fileIndex > -1 && fileIndex < c_MaxOpenFiles && m_OpenedFiles.at(fileIndex)) {
		char buf[4096];
		if (fgets(buf, sizeof(buf), m_OpenedFiles[fileIndex]) != nullptr) {
			return buf;
		}
	} else {
		g_ConsoleMan.PrintString("ERROR: Tried to read an invalid or closed file.");
	}
	return "";
}

void LuaMan::FileWriteLine(int fileIndex, const std::string& line) {
	if (fileIndex > -1 && fileIndex < c_MaxOpenFiles && m_OpenedFiles.at(fileIndex)) {
		if (fputs(line.c_str(), m_OpenedFiles[fileIndex]) == EOF) {
			g_ConsoleMan.PrintString("ERROR: Failed to write to file. File might have been opened without writing permissions or is corrupt.");
		}
	} else {
		g_ConsoleMan.PrintString("ERROR: Tried to write to an invalid or closed file.");
	}
}

bool LuaMan::FileEOF(int fileIndex) {
	if (fileIndex > -1 && fileIndex < c_MaxOpenFiles && m_OpenedFiles.at(fileIndex)) {
		return feof(m_OpenedFiles[fileIndex]);
	}
	g_ConsoleMan.PrintString("ERROR: Tried to check EOF for an invalid or closed file.");
	return false;
}

void LuaMan::Update() {
	ZoneScoped;

	m_MasterScriptState.Update();
	for (LuaStateWrapper& luaState: m_ScriptStates) {
		luaState.Update();
	}

	// Make sure a GC run isn't happening while we try to apply deletions
	m_GarbageCollectionTask.wait();

	// Apply all deletions queued from lua
	LuabindObjectWrapper::ApplyQueuedDeletions();
}

void LuaMan::WaitForAsyncGarbageCollection() {
	m_GarbageCollectionTask.wait();
}

void LuaMan::CollectGarbageForCheckpoint() {
	m_GarbageCollectionTask.wait();
	// A finalizer keeps its own object, and anything only it reaches, alive for the cycle that runs
	// it, so one pass does not settle a chain. Repeat while a full collection still frees something.
	const auto collect = [](LuaStateWrapper& luaState) {
		std::lock_guard<std::recursive_mutex> lock(luaState.GetMutex());
		lua_State* state = luaState.GetLuaState();
		const auto bytes = [state] { return static_cast<long long>(lua_gc(state, LUA_GCCOUNT, 0)) * 1024 + lua_gc(state, LUA_GCCOUNTB, 0); };
		long long before = 0;
		do {
			before = bytes();
			lua_gc(state, LUA_GCCOLLECT, 0);
		} while (bytes() < before);
		lua_gc(state, LUA_GCSTOP, 0);
	};
	collect(m_MasterScriptState);
	for (LuaStateWrapper& luaState: m_ScriptStates) {
		collect(luaState);
	}
	LuabindObjectWrapper::ApplyQueuedDeletions();
}

void LuaMan::StartAsyncGarbageCollection() {
	ZoneScoped;

	std::vector<LuaStateWrapper*> allStates;
	allStates.reserve(m_ScriptStates.size() + 1);

	allStates.push_back(&m_MasterScriptState);
	for (LuaStateWrapper& wrapper: m_ScriptStates) {
		allStates.push_back(&wrapper);
	}

	m_GarbageCollectionTask = BS::multi_future<void>();
	for (LuaStateWrapper* luaState: allStates) {
		m_GarbageCollectionTask.push_back(
		    g_ThreadMan.GetPriorityThreadPool().submit([luaState]() {
			    ZoneScopedN("Lua Garbage Collection");
			    std::lock_guard<std::recursive_mutex> lock(luaState->GetMutex());
			    lua_gc(luaState->GetLuaState(), LUA_GCSTEP, 100);
			    lua_gc(luaState->GetLuaState(), LUA_GCSTOP, 0);
		    }));
	}
}

void LuaMan::SeedAllLuaRNGs(uint64_t baseSeed) {
	// Derive an independent per-state seed so states don't share a math.random sequence.
	std::mt19937_64 derive(baseSeed);
	m_MasterScriptState.SeedRandomGenerator(derive());
	for (LuaStateWrapper& luaState: m_ScriptStates) {
		luaState.SeedRandomGenerator(derive());
	}
}

void LuaMan::HashAllLuaStatesIntoSimChecksum() {
	if (!g_SimChecksum.IsActive()) {
		return;
	}
	// Hash the master state only — threaded per-MO Lua work is redirected to per-MO RNGs, so the
	// threaded states carry no sim-observable RNG state. Master is the thread-count-invariant,
	// sim-authoritative Lua RNG.
	const std::string masterState = m_MasterScriptState.GetRandomGeneratorStateForHashing();
	g_SimChecksum.Update("lua_state", masterState.data(), masterState.size());
}

void LuaMan::ClearScriptTimings() {
	m_MasterScriptState.ClearScriptTimings();
	for (LuaStateWrapper& luaState: m_ScriptStates) {
		luaState.ClearScriptTimings();
	}
}

void LuaStateWrapper::DiscardStashedScriptObject(long uniqueID) {
	RunScriptString("if _ScriptFieldsStash then _ScriptFieldsStash[\"" + std::to_string(uniqueID) + "\"] = nil; end");
}
