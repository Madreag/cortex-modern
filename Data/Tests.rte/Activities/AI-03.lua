-- AI-03: Dig to an objective.
-- Pass: actor moves downward at least 20 pixels (proxy for digging) by max_ticks.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI03 = Trust.Extend("AI-03", { max_ticks = 1800 });

function TestScenarioAI03:OnStart()
    local a = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_GOTO);
    if a then
        a:AddAISceneWaypoint(Vector(950, 500));
        self._startY = a.Pos.Y;
        self._digger = a;
    end
end

function TestScenarioAI03:OnTick(tick)
    local a = self._digger;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    if tick % 60 == 0 then
        local dy = a.Pos.Y - self._startY;
        self:RecordMetric("digger_dy", dy);
    end
    return false, false;
end

function TestScenarioAI03:OnEnd()
    local a = self._digger;
    if a and MovableMan:IsActor(a) then
        local dy = a.Pos.Y - self._startY;
        self:RecordMetric("digger_dy", dy);
        self._passed = dy >= 20;
    else
        self._passed = false;
    end
end
