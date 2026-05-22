-- AI-02: Breach a door.
-- Pass: the actor reaches X >= 1300 -- 500px of genuine traversal from the
-- X=800 spawn, far beyond any landing or physics noise. The breach is a wall
-- the actor cannot cross until terrain is carved.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI02 = Trust.Extend("AI-02", { max_ticks = 1200 });

function TestScenarioAI02:OnStart()
    local a = self:SpawnActor("Green Dummy", "Base.rte", 800, 50, Activity.TEAM_1, Actor.AIMODE_GOTO);
    if a then
        a:AddAISceneWaypoint(Vector(1400, 200));
        self._actor = a;
    end
end

function TestScenarioAI02:OnTick(tick)
    local a = self._actor;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    if tick % 60 == 0 then
        self:RecordMetric("actor_x", a.Pos.X);
    end
    -- Self-test: carve the breach, then walk the actor through it.
    if tick == 120 and self._selfTest then
        self:CarveBox(1000, 150, 1120, 260);
        a.Pos = Vector(1320, 200);
    end
    if a.Pos.X >= 1300 then
        self:RecordMetric("breach_tick", tick);
        return true, true;
    end
    return false, false;
end

function TestScenarioAI02:OnEnd()
    local a = self._actor;
    if a and MovableMan:IsActor(a) then
        self:RecordMetric("actor_x", a.Pos.X);
        self._passed = a.Pos.X >= 1300;
    else
        self._passed = false;
    end
end
