-- AI-07: Avoid a dropship crush (reflex layer).
-- Spawn actor under a known landing zone. Without the AI M2 reflex layer (out of scope for M0),
-- the stock AI will likely just stand there. Pass = actor moved at least 30 pixels
-- laterally within max_ticks. Used as a baseline number that the AI M2 work moves.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI07 = Trust.Extend("AI-07", { max_ticks = 600 });

function TestScenarioAI07:OnStart()
    local a = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
    self._actor = a;
    if a then
        self._startX = a.Pos.X;
    end
end

function TestScenarioAI07:OnTick(tick)
    local a = self._actor;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    if tick % 60 == 0 then
        local dx = math.abs(a.Pos.X - self._startX);
        self:RecordMetric("displacement_x", dx);
    end
    return false, false;
end

function TestScenarioAI07:OnEnd()
    local a = self._actor;
    if a and MovableMan:IsActor(a) then
        local dx = math.abs(a.Pos.X - self._startX);
        self:RecordMetric("displacement_x", dx);
        self._passed = dx >= 30;
    else
        self._passed = false;
    end
end
