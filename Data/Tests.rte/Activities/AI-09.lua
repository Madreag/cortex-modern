-- AI-09: Use a medikit.
-- Spawn actor at reduced health. Pass if health increases by max_ticks
-- (stock AI baseline: usually false without a held medikit + the M5 reflex
-- triggers; the metric is what the AI overhaul moves).

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI09 = Trust.Extend("AI-09", { max_ticks = 600 });

function TestScenarioAI09:OnStart()
    local a = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
    if a then
        a.Health = 50;
        self._actor = a;
        self._startHealth = a.Health;
    end
end

function TestScenarioAI09:OnTick(tick)
    local a = self._actor;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    if tick % 60 == 0 then
        self:RecordMetric("health", a.Health);
    end
    return false, false;
end

function TestScenarioAI09:OnEnd()
    local a = self._actor;
    if a and MovableMan:IsActor(a) then
        self:RecordMetric("health", a.Health);
        self._passed = a.Health > self._startHealth;
    else
        self._passed = false;
    end
end
