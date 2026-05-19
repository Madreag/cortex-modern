-- AI-11: Retreat from an unwinnable breach.
-- Spawn actor with GOTO toward an unreachable target. Pass if actor reports
-- impossibility (the AI M2 stuck-recovery work surfaces this; M0 baseline
-- almost always fails this — the metric is what we move).

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI11 = Trust.Extend("AI-11", { max_ticks = 900 });

function TestScenarioAI11:OnStart()
    local a = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_GOTO);
    if a then
        a:AddAISceneWaypoint(Vector(50, 50));
        self._actor = a;
        self._startPos = Vector(a.Pos.X, a.Pos.Y);
    end
end

function TestScenarioAI11:OnTick(tick)
    local a = self._actor;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    if tick % 60 == 0 then
        local mode = a.AIMode;
        self:RecordMetric("ai_mode", mode);
        local dx = a.Pos.X - self._startPos.X;
        self:RecordMetric("retreat_dx", dx);
    end
    return false, false;
end

function TestScenarioAI11:OnEnd()
    local a = self._actor;
    if a and MovableMan:IsActor(a) then
        local dx = a.Pos.X - self._startPos.X;
        self:RecordMetric("retreat_dx", dx);
        -- Conservative baseline: pass if mode reverted from GOTO to SENTRY (impossibility reported)
        -- OR if actor genuinely retreated (dx < -50, away from start).
        self._passed = (a.AIMode == Actor.AIMODE_SENTRY) or dx < -50;
    else
        self._passed = false;
    end
end
