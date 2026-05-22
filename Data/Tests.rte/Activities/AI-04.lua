-- AI-04: Recover from a collapsed tunnel.
-- The actor lands, then a shaft is carved and the actor is dropped to the bottom
-- of it (both modes) -- genuinely trapped at depth, placed by teleport so the
-- drop itself does no damage. Pass: the trapped actor is back at the surface
-- (depth near 0). A held sentry stays in the shaft and fails; the self-test digs
-- an escape route and moves the actor out. Result locks in OnTick.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI04 = Trust.Extend("AI-04", { max_ticks = 1200 });

function TestScenarioAI04:OnStart()
    local a = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
    self._actor = a;
    self._landed = nil;
    self._trapped = false;
end

function TestScenarioAI04:OnTick(tick)
    local a = self._actor;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    if tick == 90 and not self._landed then
        self._landed = Vector(a.Pos.X, a.Pos.Y);
    end
    -- The collapse: carve a shaft and drop the actor to the bottom of it.
    if tick == 100 and self._landed and not self._trapped then
        self:CarveBox(self._landed.X - 40, self._landed.Y + 15,
                      self._landed.X + 40, self._landed.Y + 210);
        a.Pos = Vector(self._landed.X, self._landed.Y + 175);
        self._trapped = true;
        self:RecordMetric("trapped_depth", a.Pos.Y - self._landed.Y);
    end
    -- Self-test: dig an escape route clear of the shaft and move the actor out.
    if tick == 160 and self._selfTest and self._landed then
        self:CarveBox(self._landed.X + 40, self._landed.Y + 30,
                      self._landed.X + 240, self._landed.Y + 210);
        a.Pos = Vector(self._landed.X + 160, self._landed.Y - 10);
    end
    -- Pass: the trapped actor is back at the surface.
    if self._trapped and self._landed and tick > 120 then
        local depth = a.Pos.Y - self._landed.Y;
        if tick % 60 == 0 then
            self:RecordMetric("recover_depth", depth);
        end
        if depth <= 25 then
            self:RecordMetric("recover_depth", depth);
            self:RecordMetric("recover_tick", tick);
            return true, true;
        end
        if tick > 450 then
            self:RecordMetric("recover_depth", depth);
            return true, false;
        end
    end
    return false, false;
end

function TestScenarioAI04:OnEnd()
end
