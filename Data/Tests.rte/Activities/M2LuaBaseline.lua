-- M2LuaBaseline.lua — minimal Lua-subsystem determinism scenario.
-- The smallest activity that drives the lua_state checksum: 8 master-state RNG
-- draws per tick, folded into a checksum-visible value. No actors, no threaded
-- Lua. If this scenario diverges, the M2 Lua-determinism foundation is broken.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioM2LuaBaseline = Trust.Extend("M2LuaBaseline", { max_ticks = 600 });

local DRAWS_PER_TICK = 8;

function TestScenarioM2LuaBaseline:OnStart()
    self._fold = 0;
    self:RecordMetric("actor_count", 0);
    self:RecordMetric("draws_per_tick", DRAWS_PER_TICK);
end

function TestScenarioM2LuaBaseline:OnTick(tick)
    -- Drive the master-state RNG and fold into a checksum-visible value.
    local fold = self._fold;
    for _ = 1, DRAWS_PER_TICK do
        fold = (fold * 8191 + math.random(0, 65535)) % 2147483647;
    end
    self._fold = fold;

    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioM2LuaBaseline:OnEnd()
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("lua_fold", self._fold);
    -- Self-check: ran full ticks and the RNG produced a varying stream --
    -- a stuck or broken RNG would leave the fold at 0.
    self._passed = self:Tick() >= self._maxTicks - 1 and self._fold ~= 0;
end
