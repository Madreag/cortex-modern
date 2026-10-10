local Test = dofile("Data/Tests.rte/Lib/TestScenario.lua");
ConnectionGuard = Test.Extend("ConnectionGuard", { max_ticks = 120 });

function ConnectionGuard:OnStart()
    self.PresetName = "ConnectionGuard";
    self:SpawnActor("Green Dummy", "Base.rte", 1200, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
end

function ConnectionGuard:OnTick(tick)
    if tick == 4 then
        ActivityMan:PauseActivity(true, true);
        assert(self.PresetName == "ConnectionGuard");
        local ready = LuaMan:FileOpen("UserScenes.rte/connection-guard-ready.json", "w");
        assert(ready >= 0);
        LuaMan:FileWriteLine(ready, '{"activity_preset":"' .. self.PresetName .. '","tick":4}\n');
        LuaMan:FileClose(ready);
    end
    return false, false;
end
