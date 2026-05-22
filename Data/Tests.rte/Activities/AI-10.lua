-- AI-10: Weapon pickup.
-- Spawn an actor near a dropped weapon. Pass: the actor's equipped weapon
-- differs from what it started with -- a genuine pickup. Result locks in OnTick.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI10 = Trust.Extend("AI-10", { max_ticks = 600 });

function TestScenarioAI10:OnStart()
    local a = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_GOTO);
    if a then
        a:AddAISceneWaypoint(Vector(1080, 200));
        self._actor = a;
        self._startWeapon = (a.EquippedItem and a.EquippedItem.PresetName) or "";
    end
    -- A loose firearm on the ground near the actor, for the AI to pick up.
    local gun = CreateHDFirearm("SMG", "Base.rte");
    if gun then
        gun.Pos = Vector(1010, 355);
        MovableMan:AddItem(gun);
    end
    self:RecordMetric("start_weapon", self._startWeapon == "" and 0 or 1);
end

function TestScenarioAI10:OnTick(tick)
    local a = self._actor;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    if tick % 60 == 0 then
        self:RecordMetric("has_weapon", a.EquippedItem and 1 or 0);
    end
    -- Self-test: genuinely give the actor a different weapon and equip it.
    if tick == 60 and self._selfTest then
        local gun = CreateHDFirearm("Pistol", "Base.rte");
        if gun then
            a:AddInventoryItem(gun);
            a:EquipNamedDevice("Base.rte", "Pistol", true);
        end
    end
    if tick > 30 then
        local equipped = a.EquippedItem;
        if equipped ~= nil and equipped.PresetName ~= self._startWeapon then
            self:RecordMetric("has_weapon", 1);
            self:RecordMetric("pickup_tick", tick);
            return true, true;
        end
        if tick > 450 then
            self:RecordMetric("has_weapon", equipped and 1 or 0);
            return true, false;
        end
    end
    return false, false;
end

function TestScenarioAI10:OnEnd()
end
