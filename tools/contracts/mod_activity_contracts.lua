local function readState()
    local a = ToGameActivity(ActivityMan:GetActivity())
    local b = a:GetBuyGUI(0)
    local e = a:GetEditorGUI(0)
    local banner = a:GetBanner(GUIBanner.YELLOW, 0)
    local lz = a:GetLandingZone(0)
    return {
        InCampaignStage=a.InCampaignStage,
        Difficulty=a.Difficulty,
        AllowsUserSaving=a.AllowsUserSaving,
        DeliveryDelay=a.DeliveryDelay,
        BuyMenuEnabled=a.BuyMenuEnabled,
        CraftsOrbitAtTheEdge=a.CraftsOrbitAtTheEdge,
        GameOverPeriod=a.GameOverPeriod,
        CursorTimerStart=a.CursorTimer.StartSimTimeTicks,
        CursorTimerLimit=a.CursorTimer.SimTimeLimitTicks,
        GameTimerStart=a.GameTimer.StartSimTimeTicks,
        GameTimerLimit=a.GameTimer.SimTimeLimitTicks,
        GameOverTimerStart=a.GameOverTimer.StartSimTimeTicks,
        GameOverTimerLimit=a.GameOverTimer.SimTimeLimitTicks,
        TeamFunds=a:GetTeamFunds(0),
        TeamAISkill=a:GetTeamAISkill(0),
        TeamDeaths=a:GetTeamDeathCount(0),
        LandingZoneX=lz.X,
        LandingZoneY=lz.Y,
        BrainLZWidth=a:GetBrainLZWidth(0),
        TeamTech=a:GetTeamTech(0),
        ShowOnlyOwnedItems=b.ShowOnlyOwnedItems,
        EnforceMaxPassengers=b.EnforceMaxPassengersConstraint,
        EnforceMaxMass=b.EnforceMaxMassConstraint,
        OwnedItemCount=b:GetOwnedItemsAmount("Base.rte/Battle Rifle"),
        EditorMode=e.EditorMode,
        BannerKerning=banner.Kerning,
        MaxDroppedItems=MovableMan.MaxDroppedItems,
        ParticleSettling=MovableMan:IsParticleSettlingEnabled(),
    }
end

function Create(self)
    self.testCreate, self.testUpdate = 1, 0
end

function Update(self)
    self.testUpdate = self.testUpdate + 1
    self:SetNumberValue("TestUpdates", self.testUpdate)
    if self.UniqueID ~= 1048577 or self.activityContracts or os.getenv("CC_CONTRACT_FRESH_DEFAULTS") then return end
    local a = ToGameActivity(ActivityMan:GetActivity())
    a.InCampaignStage = 3
    a.Difficulty = 77
    a.AllowsUserSaving = true
    a.DeliveryDelay = 5278
    a.BuyMenuEnabled = false
    a.CraftsOrbitAtTheEdge = true
    a.GameOverPeriod = 8421
    a.CursorTimer.StartSimTimeTicks = -123400
    a.CursorTimer.SimTimeLimitTicks = 6789000
    a.GameTimer.StartSimTimeTicks = -234500
    a.GameTimer.SimTimeLimitTicks = 7890000
    a.GameOverTimer.StartSimTimeTicks = -345600
    a.GameOverTimer.SimTimeLimitTicks = 8901000
    a:SetTeamFunds(33337, 0)
    a:SetTeamAISkill(0, 55)
    a:ReportDeath(0, 1)
    a:SetObservationTarget(Vector(812, 612), 0)
    a:SetDeathViewTarget(Vector(913, 513), 0)
    a:SetLandingZone(Vector(714, 414), 0)
    a:SetActorSelectCursor(Vector(615, 315), 0)
    a:SetBrainLZWidth(0, 123)
    a:SetTeamTech(0, "Base.rte")
    local b = a:GetBuyGUI(0)
    b.ShowOnlyOwnedItems = true
    b.EnforceMaxPassengersConstraint = false
    b.EnforceMaxMassConstraint = false
    b:SetOwnedItemsAmount("Base.rte/Battle Rifle", 7)
    b:AddAllowedItem("Base.rte/Battle Rifle")
    b:AddAlwaysAllowedItem("Base.rte/Light Digger")
    b:AddProhibitedItem("Base.rte/Heavy Digger")
    local e = a:GetEditorGUI(0)
    e.EditorMode = SceneEditorGUI.PICKINGOBJECT
    a:GetBanner(GUIBanner.YELLOW, 0).Kerning = 7
    MovableMan.MaxDroppedItems = 247
    MovableMan:EnableParticleSettling(false)
    self.activityContracts = readState()
    local n=0; for _ in pairs(self.activityContracts) do n=n+1 end
    print("[activity-contract-fixture] constructed checks=" .. n)
end

_G._ContractAuditCheck = function(stage)
    for _, self in pairs(_ScriptedObjects or {}) do
        if self.activityContracts then
            local actual, failures, checked = readState(), 0, 0
            local keys={}; for key in pairs(self.activityContracts) do keys[#keys+1]=key end
            table.sort(keys)
            for _, key in ipairs(keys) do
                local expected=self.activityContracts[key]
                checked=checked+1
                if expected~=actual[key] then
                    failures=failures+1
                    print("[activity-contract-mismatch] " .. stage .. " " .. key .. " expected=" .. tostring(expected) .. " actual=" .. tostring(actual[key]))
                end
            end
            print("[activity-contract-check] " .. stage .. " checked=" .. checked .. " mismatches=" .. failures)
        end
    end
end
