-- AI-12: Cross a newly-opened route.
-- A horizontal tunnel is genuinely carved at tick 120 (both modes) through the
-- solid terrain at the actor's landed level -- provable by a pixel-count change.
-- Pass: the route was genuinely opened (>= 300 air pixels appeared) AND the
-- actor crossed >= 460px through it. Geometry is relative to the landed pos.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI12 = Trust.Extend("AI-12", { max_ticks = 1800 });

function TestScenarioAI12:OnStart()
    local a = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_GOTO);
    if a then
        a:AddAISceneWaypoint(Vector(1500, 200));
        self._actor = a;
    end
    self._landed = nil;
    self._tunnelBox = nil;
    self._airBefore = 0;
    self._carved = 0;
    self._carveDone = false;
end

function TestScenarioAI12:OnTick(tick)
    local a = self._actor;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    -- Capture the landed position, then define a horizontal band through the
    -- solid terrain at the actor's landed level.
    if tick == 90 and not self._landed then
        self._landed = Vector(a.Pos.X, a.Pos.Y);
        self._tunnelBox = { self._landed.X + 30, self._landed.Y - 25,
                            self._landed.X + 520, self._landed.Y + 15 };
        self._airBefore = self:CountAirPixels(self._tunnelBox[1], self._tunnelBox[2],
                                              self._tunnelBox[3], self._tunnelBox[4]);
        self:RecordMetric("tunnel_air_before", self._airBefore);
    end
    -- The route opens: a genuine terrain carve, in both modes.
    if tick == 120 and not self._carveDone and self._tunnelBox then
        self:CarveBox(self._tunnelBox[1], self._tunnelBox[2],
                      self._tunnelBox[3], self._tunnelBox[4]);
        self._carved = self:CountAirPixels(self._tunnelBox[1], self._tunnelBox[2],
                                           self._tunnelBox[3], self._tunnelBox[4]) - self._airBefore;
        self._carveDone = true;
        self:RecordMetric("terrain_carved", self._carved);
    end
    -- Self-test: walk the actor through the opened tunnel.
    if tick == 150 and self._selfTest and self._landed then
        a.Pos = Vector(self._landed.X + 500, self._landed.Y);
    end
    if tick % 60 == 0 then
        self:RecordMetric("actor_x", a.Pos.X);
    end
    if tick > 130 and self._carveDone and self._landed then
        if self._carved >= 300 and a.Pos.X >= self._landed.X + 460 then
            self:RecordMetric("actor_x", a.Pos.X);
            self:RecordMetric("goal_tick", tick);
            return true, true;
        end
        if tick > 900 then
            self:RecordMetric("actor_x", a.Pos.X);
            return true, false;
        end
    end
    return false, false;
end

function TestScenarioAI12:OnEnd()
end
