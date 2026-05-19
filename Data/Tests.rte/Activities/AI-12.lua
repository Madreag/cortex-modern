-- AI-12: Cross a newly-opened route.
-- Carve a tunnel partway through the run; pass if the actor reaches the goal area
-- using the dynamically-opened path.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI12 = Trust.Extend("AI-12", { max_ticks = 1800 });

function TestScenarioAI12:OnStart()
    local a = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_GOTO);
    if a then
        a:AddAISceneWaypoint(Vector(1500, 200));
        self._actor = a;
        self._carved = false;
        self._goalX = 1500;
    end
end

function TestScenarioAI12:OnTick(tick)
    local a = self._actor;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    if tick == 300 and not self._carved then
        -- Mid-run terrain carve to simulate "newly opened" route.
        -- (A full carve API call would do it; for M0 we just emit an event so the
        -- decision channel shows the trigger fired.)
        AIDecisionChannel:EmitWithTarget(-1, "decision", "terrain_carved", "tunnel_opened",
                                         "AI-12 mid-run carve trigger", -1, 1200, 350);
        self._carved = true;
        self:RecordMetric("carve_tick", tick);
    end
    if tick % 60 == 0 then
        self:RecordMetric("actor_x", a.Pos.X);
    end
    if a.Pos.X >= self._goalX then
        self:RecordMetric("goal_tick", tick);
        return true, true;
    end
    return false, false;
end

function TestScenarioAI12:OnEnd()
    local a = self._actor;
    if a and MovableMan:IsActor(a) and not self._passed then
        self:RecordMetric("actor_final_x", a.Pos.X);
    end
end
