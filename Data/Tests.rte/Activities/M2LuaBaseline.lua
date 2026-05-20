-- M2LuaBaseline.lua — minimal determinism scenario for the lua_state checksum subsystem.
-- No actors, so all Lua runs in the Activity script (main thread), clear of the threaded
-- Lua race. OnTick advances the master state's RNG so the subsystem hashes live movement.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioM2LuaBaseline = Trust.Extend("M2LuaBaseline", { max_ticks = 600 });

function TestScenarioM2LuaBaseline:OnStart()
    self:RecordMetric("actor_count", 0);
end

function TestScenarioM2LuaBaseline:OnTick(tick)
    for _ = 1, 8 do
        math.random();
    end
    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioM2LuaBaseline:OnEnd()
    self._passed = true;
    self:RecordMetric("final_tick", self:Tick());
end
