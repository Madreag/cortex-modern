-- M2LuaRandomStress.lua — exercises deterministic math.random. OnTick draws a batch of
-- random numbers on the master Lua state and folds them into a checksum-visible value.
-- No actors, so the draws stay on the main thread, clear of the threaded-Lua race.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioM2LuaRandomStress = Trust.Extend("M2LuaRandomStress", { max_ticks = 600 });

function TestScenarioM2LuaRandomStress:OnStart()
    self._fold = 0;
    self:RecordMetric("actor_count", 0);
end

function TestScenarioM2LuaRandomStress:OnTick(tick)
    local fold = self._fold;
    for _ = 1, 200 do
        fold = (fold * 8191 + math.random(0, 65535)) % 2147483647;
    end
    self._fold = fold;
    AIDecisionChannel:EmitWithTarget(-1, "decision", "m2_lua_random", tostring(fold), "", -1, 0, 0);

    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioM2LuaRandomStress:OnEnd()
    self._passed = true;
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("random_fold", self._fold);
end
