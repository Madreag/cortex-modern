-- AI-01: Defend the brain room.
-- Pass: the BRAINHUNT attacker is neutralised (dead / no longer a valid actor)
-- while the brain actor is still alive. The threat going down with the brain
-- intact is the genuine "defence held" outcome. Result locks in OnTick.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI01 = Trust.Extend("AI-01", { max_ticks = 1200 });

function TestScenarioAI01:OnStart()
    -- Max AI skill so the armed defender lands its Battle Rifle hits.
    self:MaxTeamAISkill(Activity.TEAM_1);
    self:MaxTeamAISkill(Activity.TEAM_2);
    -- Brain + armed SENTRY defender; an armed attacker with a GOTO waypoint at
    -- the brain. The base Activity doesn't auto-mark the brain dummy, so a
    -- BRAINHUNT attacker wanders -- GOTO + waypoint gives the deterministic
    -- advance the test needs.
    self._brain = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
    local defender = self:SpawnActor("Green Dummy", "Base.rte", 870, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
    self:GiveFirearm(defender, "Battle Rifle");
    self._attacker = self:SpawnActor("Green Dummy", "Base.rte", 650, 50, Activity.TEAM_2, Actor.AIMODE_GOTO);
    if self._attacker and self._brain then
        self._attacker:AddAISceneWaypoint(Vector(self._brain.Pos.X, self._brain.Pos.Y));
        self:GiveFirearm(self._attacker, "SMG");
    end
    self:RecordMetric("brain_start_health", self._brain and self._brain.Health or 0);
end

function TestScenarioAI01:OnTick(tick)
    local brain = self._brain;
    if not brain or not MovableMan:IsActor(brain) or brain.Health <= 0 then
        return true, false;
    end
    if tick % 60 == 0 then
        self:RecordMetric("brain_health", brain.Health);
    end
    -- Self-test: genuinely remove the threat so the defence succeeds.
    if tick == 25 and self._selfTest and self._attacker and MovableMan:IsActor(self._attacker) then
        self._attacker:GibThis();
    end
    -- Threat neutralised with the brain still standing -- defence held.
    local attacker = self._attacker;
    if not attacker or not MovableMan:IsActor(attacker) or attacker.Health <= 0 then
        self:RecordMetric("brain_health", brain.Health);
        self:RecordMetric("threat_down_tick", tick);
        return true, true;
    end
    if tick > 450 then
        return true, false;
    end
    return false, false;
end

function TestScenarioAI01:OnEnd()
    local brain = self._brain;
    local alive = brain and MovableMan:IsActor(brain) and brain.Health > 0;
    self:RecordMetric("brain_health", alive and brain.Health or 0);
end
