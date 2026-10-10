local Test = dofile("Data/Tests.rte/Lib/TestScenario.lua");
ConnectionGuard = Test.Extend("ConnectionGuard", { max_ticks = 120 });

function ConnectionGuard:OnStart()
    self:SpawnActor("Green Dummy", "Base.rte", 1200, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
end

function ConnectionGuard:OnTick(tick)
    if tick == 4 then
        ActivityMan:PauseActivity(true);
    end
    return false, false;
end
