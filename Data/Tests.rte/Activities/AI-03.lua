-- AI-03: Dig to an objective.
-- Pass: terrain was genuinely carved (>= 600 air pixels appeared in the dig
-- column below the actor's landed feet, which gravity alone cannot do) AND the
-- actor descended >= 55px into it. Both must hold -- a falling actor with no
-- digging fails. Geometry is measured relative to the landed position.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI03 = Trust.Extend("AI-03", { max_ticks = 1800 });

function TestScenarioAI03:OnStart()
    local a = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_GOTO);
    if a then
        a:AddAISceneWaypoint(Vector(950, 600));
        self:GiveDigger(a, "Heavy Digger");
        self._actor = a;
    end
    self._landed = nil;
    self._airBefore = 0;
end

function TestScenarioAI03:OnTick(tick)
    local a = self._actor;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    -- Capture the landed position once the fall has settled, then sample the
    -- solid dig column directly below the actor's feet.
    if tick == 90 and not self._landed then
        self._landed = Vector(a.Pos.X, a.Pos.Y);
        self._airBefore = self:CountAirPixels(self._landed.X - 15, self._landed.Y + 15,
                                              self._landed.X + 15, self._landed.Y + 135);
        self:RecordMetric("air_before", self._airBefore);
    end
    -- Self-test: carve the dig column, then drop the actor down it.
    if tick == 110 and self._selfTest and self._landed then
        self:CarveBox(self._landed.X - 15, self._landed.Y + 12,
                      self._landed.X + 15, self._landed.Y + 140);
        a.Pos = Vector(self._landed.X, self._landed.Y + 110);
    end
    if tick > 130 and self._landed then
        local removed = self:CountAirPixels(self._landed.X - 15, self._landed.Y + 15,
                                            self._landed.X + 15, self._landed.Y + 135) - self._airBefore;
        if tick % 60 == 0 then
            self:RecordMetric("terrain_removed", removed);
            self:RecordMetric("digger_dy", a.Pos.Y - self._landed.Y);
        end
        if removed >= 600 and a.Pos.Y >= self._landed.Y + 55 then
            self:RecordMetric("terrain_removed", removed);
            self:RecordMetric("digger_dy", a.Pos.Y - self._landed.Y);
            return true, true;
        end
        if tick > 450 then
            self:RecordMetric("terrain_removed", removed);
            return true, false;
        end
    end
    return false, false;
end

function TestScenarioAI03:OnEnd()
end
