-- AI-01: Defend the brain room.
-- Pass: at least 1 defender remains living after max_ticks (stock AI baseline:
-- defenders set to SENTRY should hold position unless they actively die).

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI01 = Trust.Extend("AI-01", { max_ticks = 1200 });

function TestScenarioAI01:OnStart()
    -- Two defenders set to SENTRY. Y=50 spawns them in open sky above the Tutorial
    -- Bunker roof (Y=176) so they fall onto solid terrain rather than spawning
    -- inside a wall.
    self:SpawnActor("Green Dummy", "Base.rte", 900,  50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
    self:SpawnActor("Green Dummy", "Base.rte", 1000, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
    self._startCount = 2;
    self:RecordMetric("defender_start_count", self._startCount);
end

function TestScenarioAI01:OnTick(tick)
    local alive = self:CountLivingActors();
    if tick % 60 == 0 then
        self:RecordMetric("defender_alive_count", alive);
    end
    if alive == 0 then
        return true, false;
    end
    return false, false;
end

function TestScenarioAI01:OnEnd()
    local alive = self:CountLivingActors();
    self:RecordMetric("defender_alive_count", alive);
    self._passed = alive >= 1;
end
