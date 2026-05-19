-- AI-02: Breach a door.
-- Pass: actor moves at least 50 pixels (toward target) within max_ticks.
-- Stock AI under GOTO should move; the baseline measures how far it actually gets.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI02 = Trust.Extend("AI-02", { max_ticks = 1200 });

function TestScenarioAI02:OnStart()
    local a = self:SpawnActor("Green Dummy", "Base.rte", 800, 50, Activity.TEAM_1, Actor.AIMODE_GOTO);
    if a then
        a:AddAISceneWaypoint(Vector(1400, 200));
        self._startPos = Vector(a.Pos.X, a.Pos.Y);
        self._attacker = a;
    end
end

function TestScenarioAI02:OnTick(tick)
    local a = self._attacker;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    if tick % 60 == 0 then
        local dx = a.Pos.X - self._startPos.X;
        self:RecordMetric("attacker_dx", dx);
    end
    return false, false;
end

function TestScenarioAI02:OnEnd()
    local a = self._attacker;
    if a and MovableMan:IsActor(a) then
        local dx = a.Pos.X - self._startPos.X;
        self:RecordMetric("attacker_dx", dx);
        self._passed = dx >= 50;
    else
        self._passed = false;
    end
end
