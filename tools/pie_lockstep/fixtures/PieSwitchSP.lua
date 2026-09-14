package.loaded.Constants = nil; require("Constants");
local Test = dofile("Data/Tests.rte/Lib/TestScenario.lua");
PieSwitchSP = Test.Extend("PieSwitchSP", { max_ticks = 320 });

function PieSwitchSP:OnStart()
    self._first = self:SpawnActor("Green Dummy", "Base.rte", 1200, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
    self._second = self:SpawnActor("Green Dummy", "Base.rte", 1260, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
end

function PieSwitchSP:OnTick(tick)
    if not MovableMan:IsActor(self._first) or not MovableMan:IsActor(self._second) then
        return true, false;
    end
    if tick >= self._maxTicks then
        return true, true;
    end
    return false, false;
end
