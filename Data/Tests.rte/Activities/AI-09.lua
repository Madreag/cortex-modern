-- AI-09: Use a medikit.
-- Spawn an actor at reduced health with a medikit on the ground beside it.
-- Pass: the actor's health rises above its 50 starting value -- the genuine
-- "healed" outcome. Result locks in OnTick the moment health climbs.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI09 = Trust.Extend("AI-09", { max_ticks = 600 });

function TestScenarioAI09:OnStart()
    -- Medikit goes straight into the actor's inventory so the test isolates
    -- the "AI uses a medikit on low health" behaviour from any pickup mechanic.
    local a = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
    if a then
        a.Health = 50;
        self._actor = a;
        self._startHealth = 50;
        local kit = CreateHDFirearm("Medikit", "Base.rte");
        if kit then
            a:AddInventoryItem(kit);
        end
    end
    self:RecordMetric("start_health", self._startHealth);
end

function TestScenarioAI09:OnTick(tick)
    local a = self._actor;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    if tick % 60 == 0 then
        self:RecordMetric("health", a.Health);
    end
    -- Self-test: genuine healing -- the "used a medikit" outcome.
    if tick == 60 and self._selfTest then
        a.Health = 90;
    end
    if tick > 30 then
        if a.Health > self._startHealth then
            self:RecordMetric("health", a.Health);
            self:RecordMetric("healed_tick", tick);
            return true, true;
        end
        if tick > 450 then
            self:RecordMetric("health", a.Health);
            return true, false;
        end
    end
    return false, false;
end

function TestScenarioAI09:OnEnd()
end
