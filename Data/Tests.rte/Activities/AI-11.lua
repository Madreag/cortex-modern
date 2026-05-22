-- AI-11: Retreat from an unwinnable breach.
-- The actor is given a GOTO waypoint at an unreachable up-left objective.
-- Pass: the actor moves >= 150px to the right -- genuine retreat away from the
-- objective. A stock actor pushing toward the unreachable target, or one stuck
-- against terrain, will not displace +150 right. Result locks in OnTick.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI11 = Trust.Extend("AI-11", { max_ticks = 900 });

function TestScenarioAI11:OnStart()
    local a = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_GOTO);
    if a then
        a:AddAISceneWaypoint(Vector(50, 50));
        self._actor = a;
        self._startX = a.Pos.X;
    end
end

function TestScenarioAI11:OnTick(tick)
    local a = self._actor;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    if tick % 60 == 0 then
        self:RecordMetric("retreat_dx", a.Pos.X - self._startX);
    end
    -- Self-test: genuine retreat away from the unwinnable objective.
    if tick == 60 and self._selfTest then
        a.Pos = Vector(self._startX + 250, a.Pos.Y);
    end
    if tick > 30 then
        local dx = a.Pos.X - self._startX;
        if dx >= 150 then
            self:RecordMetric("retreat_dx", dx);
            self:RecordMetric("retreat_tick", tick);
            return true, true;
        end
        if tick > 450 then
            self:RecordMetric("retreat_dx", dx);
            return true, false;
        end
    end
    return false, false;
end

function TestScenarioAI11:OnEnd()
end
