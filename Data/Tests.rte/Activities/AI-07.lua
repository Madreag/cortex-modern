-- AI-07: Avoid a dropship crush (reflex layer).
-- A heavy actor free-falls onto the sentry's column -- a genuine descending
-- threat. Pass: the sentry stays alive AND moves >= 100px laterally out of the
-- danger column (landing noise is well under 30px). Result locks in OnTick.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI07 = Trust.Extend("AI-07", { max_ticks = 600 });

function TestScenarioAI07:OnStart()
    local a = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
    self._actor = a;
    self._dangerX = 950;
    -- Genuine descending threat directly above the danger column. AIMODE_NONE so
    -- it just free-falls under gravity onto the spawn point.
    self._threat = self:SpawnActor("Green Dummy", "Base.rte", 950, -300, Activity.TEAM_2, Actor.AIMODE_NONE);
end

function TestScenarioAI07:OnTick(tick)
    local a = self._actor;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    if tick % 60 == 0 then
        self:RecordMetric("displacement_x", math.abs(a.Pos.X - self._dangerX));
    end
    -- Self-test: genuine lateral dodge out of the danger column.
    if tick == 60 and self._selfTest then
        a.Pos = Vector(a.Pos.X + 170, a.Pos.Y);
    end
    if tick > 80 then
        local dx = math.abs(a.Pos.X - self._dangerX);
        if dx >= 100 then
            self:RecordMetric("displacement_x", dx);
            self:RecordMetric("dodge_tick", tick);
            return true, true;
        end
        if tick > 450 then
            self:RecordMetric("displacement_x", dx);
            return true, false;
        end
    end
    return false, false;
end

function TestScenarioAI07:OnEnd()
end
