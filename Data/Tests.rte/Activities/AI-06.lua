-- AI-06: Rescue a downed unit.
-- Spawn rescuer + "downed" actor (one set to STAY) on the same team.
-- Pass if rescuer reaches within rescue_range of downed within max_ticks.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI06 = Trust.Extend("AI-06", { max_ticks = 1200 });

function TestScenarioAI06:OnStart()
    local downed = self:SpawnActor("Green Dummy", "Base.rte", 1200, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
    local rescuer = self:SpawnActor("Green Dummy", "Base.rte", 850, 50, Activity.TEAM_1, Actor.AIMODE_GOTO);
    if rescuer and downed then
        rescuer:AddAISceneWaypoint(Vector(downed.Pos.X, downed.Pos.Y));
        self._rescuer = rescuer;
        self._downed = downed;
        self._rescueRange = 60;
    end
end

function TestScenarioAI06:OnTick(tick)
    local r = self._rescuer;
    local d = self._downed;
    if not r or not MovableMan:IsActor(r) or not d or not MovableMan:IsActor(d) then
        return true, false;
    end
    local dx = r.Pos.X - d.Pos.X;
    local dy = r.Pos.Y - d.Pos.Y;
    local dist = math.sqrt(dx*dx + dy*dy);
    if tick % 60 == 0 then
        self:RecordMetric("rescuer_distance", dist);
    end
    if dist <= self._rescueRange then
        self:RecordMetric("rescue_tick", tick);
        return true, true;
    end
    return false, false;
end

function TestScenarioAI06:OnEnd()
    if not self._passed then
        self._passed = false;
    end
end
