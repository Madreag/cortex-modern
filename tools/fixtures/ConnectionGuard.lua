local Test = dofile("Data/Tests.rte/Lib/TestScenario.lua");
ConnectionGuard = Test.Extend("ConnectionGuard", { max_ticks = 120 });

function ConnectionGuard:OnStart()
    ActivityMan:GetActivity().PresetName = "ConnectionGuard";
    TimerMan.TimeScale = 0.1;
    local actor = self:SpawnActor("Green Dummy", "Base.rte", 1200, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
    actor.PinStrength = 1000000;
    actor.HUDVisible = false;
end

function ConnectionGuard:UpdateWatchCamera()
    CameraMan:SetScrollTarget(Vector(1200, 500), 1, 0);
end

function ConnectionGuard:PauseActivity(pause)
    if pause then
        -- Release the capture clock so the pause loop can run its exit.
        TimerMan.TimeScale = 0.1;
    end
end

function ConnectionGuard:OnTick(tick)
    if tick == 4 then
        TimerMan.TimeScale = 0;
        local name = ActivityMan:GetActivity().PresetName;
        assert(name == "ConnectionGuard");
        local ready = LuaMan:FileOpen("UserScenes.rte/connection-guard-ready.json", "w");
        assert(ready >= 0);
        LuaMan:FileWriteLine(ready, '{"activity_preset":"' .. name .. '","tick":4}\n');
        LuaMan:FileClose(ready);
        MetricsCollector:SetResult(true);
    end
    return false, false;
end
