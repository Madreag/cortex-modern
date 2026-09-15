package.loaded.Constants = nil; require("Constants")
local Test = dofile("Data/Tests.rte/Lib/TestScenario.lua")
PreviewBarrier240 = Test.Extend("PreviewBarrier240", {max_ticks = 221})

function PreviewBarrier240:OnStart()
    self._brains = {}
    for i = 1, 240 do
        local team = (i - 1) % 2
        local unit = self:SpawnActor(i <= 2 and "Brain Robot" or "Green Dummy", "Base.rte",
            100 + ((i - 1) % 24) * 80, 80 + math.floor((i - 1) / 24) * 55, team, Actor.AIMODE_SENTRY)
        assert(unit, "actor creation failed")
        unit.PinStrength = 100000
        unit.HitsMOs = false
        unit.GetsHitByMOs = false
        if i <= 2 then
            self._brains[team] = unit
            self:GiveFirearm(unit, "Battle Rifle")
            unit:AddScript("UserScenes.rte/preview_write_barrier.lua")
        end
    end
    self:RecordMetric("barrier_initial_actors", #self._spawnedActors)
end

function PreviewBarrier240:OnTick(tick)
    if tick == 3 then
        for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
            if self:PlayerActive(player) and self:PlayerHuman(player) then
                local team = self:GetTeamOfPlayer(player)
                local brain = self._brains[team]
                assert(brain, "missing player brain")
                self:SetPlayerBrain(brain, player)
                self:SwitchToActor(brain, player, team)
                self:SetObservationTarget(brain.Pos, player)
            end
        end
    end
    if tick >= 145 and tick <= 165 then
        local count = self:CountLivingActors()
        assert(count == 240, "actor census changed: " .. count)
        print("[preview-barrier-census] tick=" .. tick .. " actors=" .. count)
    end
    if tick >= self._maxTicks - 1 then return true, true end
    return false, false
end

function PreviewBarrier240:OnEnd()
    self:RecordMetric("barrier_final_actors", self:CountLivingActors())
end
