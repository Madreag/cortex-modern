-- M2LuaRandomStress.lua — heavy load on the deterministic Lua master-state RNG.
-- Each tick draws 200 random numbers and folds them into a checksum-visible
-- value. No actors -- all Lua on the main thread, clear of the threaded-Lua
-- race outside M2's scope.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioM2LuaRandomStress = Trust.Extend("M2LuaRandomStress", { max_ticks = 600 });

local DRAWS_PER_TICK = 200;

function TestScenarioM2LuaRandomStress:OnStart()
    self._fold = 0;
    self._calls = 0;
    self:RecordMetric("actor_count", 0);
    self:RecordMetric("draws_per_tick", DRAWS_PER_TICK);
end

function TestScenarioM2LuaRandomStress:OnTick(tick)
    local fold = self._fold;
    for _ = 1, DRAWS_PER_TICK do
        fold = (fold * 8191 + math.random(0, 65535)) % 2147483647;
    end
    self._fold = fold;
    self._calls = self._calls + DRAWS_PER_TICK;

    if tick % 120 == 0 then
        self:RecordMetric("random_fold_" .. tostring(tick), fold);
    end

    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioM2LuaRandomStress:OnEnd()
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("random_fold", self._fold);
    self:RecordMetric("random_calls", self._calls);
    local expected = DRAWS_PER_TICK * (self._maxTicks - 1);
    -- Self-check: ran full ticks, called RNG the expected number of times, and
    -- the fold is non-trivial (a broken RNG would leave it at zero or stuck).
    self._passed = self:Tick() >= self._maxTicks - 1
                   and self._calls >= expected
                   and self._fold ~= 0;
end
