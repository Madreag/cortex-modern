-- M2ModSmokeLoading.lua — smoke test: real actor AI Lua running through the M2-modified
-- Lua environment (deterministic pairs, seeded math.random, os stubs). Spawns opposing
-- actors so AI behaviours exercise those paths for 600 ticks.
--
-- NOT a cross-run MATCH test — actor AI Lua runs on threaded states, still subject to the
-- threaded-Lua race, so this only asserts the AI scripts run without Lua errors.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioM2ModSmokeLoading = Trust.Extend("M2ModSmokeLoading", { max_ticks = 600 });

function TestScenarioM2ModSmokeLoading:OnStart()
    self:SpawnActor("Green Dummy", "Base.rte", 800,  50, Activity.TEAM_1, Actor.AIMODE_BRAINHUNT);
    self:SpawnActor("Green Dummy", "Base.rte", 1100, 50, Activity.TEAM_2, Actor.AIMODE_BRAINHUNT);
    self:SpawnActor("Green Dummy", "Base.rte", 1400, 50, Activity.TEAM_1, Actor.AIMODE_BRAINHUNT);
    self:SpawnActor("Green Dummy", "Base.rte", 1700, 50, Activity.TEAM_2, Actor.AIMODE_BRAINHUNT);
    self:RecordMetric("actor_count", 4);
end

function TestScenarioM2ModSmokeLoading:OnTick(tick)
    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioM2ModSmokeLoading:OnEnd()
    self._passed = true;
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("final_alive_count", self:CountLivingActors());
end
