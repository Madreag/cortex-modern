-- AI-04: Recover from a collapsed tunnel (stuck-recovery).
-- Pass: actor's per-second position delta has been non-zero for at least
--       half of the run's sampled seconds (i.e. the actor has been moving,
--       not pinned forever).

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI04 = Trust.Extend("AI-04", { max_ticks = 1200 });

function TestScenarioAI04:OnStart()
    local a = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_GOTO);
    if a then
        a:AddAISceneWaypoint(Vector(1200, 200));
        self._lastPos = Vector(a.Pos.X, a.Pos.Y);
        self._actor = a;
        self._movingSeconds = 0;
        self._totalSeconds = 0;
    end
end

function TestScenarioAI04:OnTick(tick)
    local a = self._actor;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    if tick % 60 == 0 then
        local dx = a.Pos.X - self._lastPos.X;
        local dy = a.Pos.Y - self._lastPos.Y;
        local dist = math.sqrt(dx*dx + dy*dy);
        if dist > 1.0 then
            self._movingSeconds = self._movingSeconds + 1;
        end
        self._totalSeconds = self._totalSeconds + 1;
        self._lastPos = Vector(a.Pos.X, a.Pos.Y);
    end
    return false, false;
end

function TestScenarioAI04:OnEnd()
    self:RecordMetric("moving_seconds", self._movingSeconds);
    self:RecordMetric("total_seconds", self._totalSeconds);
    if self._totalSeconds > 0 then
        local ratio = self._movingSeconds / self._totalSeconds;
        self:RecordMetric("moving_ratio", ratio);
        self._passed = ratio >= 0.5;
    else
        self._passed = false;
    end
end
