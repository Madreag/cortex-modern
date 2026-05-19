-- AI-10: Weapon pickup.
-- Spawn actor near a dropped weapon. Pass if actor's equipped weapon
-- changes between StartActivity and end-of-scenario.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI10 = Trust.Extend("AI-10", { max_ticks = 600 });

function TestScenarioAI10:OnStart()
    local a = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_GOTO);
    if a then
        a:AddAISceneWaypoint(Vector(1080, 200));
        self._actor = a;
        local equipped = a.EquippedItem;
        self._startWeaponName = equipped and equipped.PresetName or "";
    end
end

function TestScenarioAI10:OnTick(tick)
    local a = self._actor;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    if tick % 60 == 0 then
        local equipped = a.EquippedItem;
        self:RecordMetric("has_weapon", equipped and 1 or 0);
    end
    return false, false;
end

function TestScenarioAI10:OnEnd()
    local a = self._actor;
    if a and MovableMan:IsActor(a) then
        local equipped = a.EquippedItem;
        local name = equipped and equipped.PresetName or "";
        self:RecordMetric("has_weapon", equipped and 1 or 0);
        self._passed = name ~= "" and name ~= self._startWeaponName;
    else
        self._passed = false;
    end
end
