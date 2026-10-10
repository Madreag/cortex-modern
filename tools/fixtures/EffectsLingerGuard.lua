local Test = dofile("Data/Tests.rte/Lib/TestScenario.lua");
ConnectionGuard = Test.Extend("ConnectionGuard", { max_ticks = 120 });

function ConnectionGuard:OnStart()
	local actor = self:SpawnActor("Green Dummy", "Base.rte", 1200, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
	actor.GlobalAccScalar = 0;
	actor.Vel = Vector(0, 0);
end

function ConnectionGuard:OnTick(tick)
	CameraMan:SetScroll(CameraMan:GetScrollTarget(0), 0);
	if tick == 4 then
		TimerMan.TimeScale = 0;
		self._passed = true;
		local ready = LuaMan:FileOpen("UserScenes.rte/connection-guard-ready.json", "w");
		assert(ready >= 0);
		LuaMan:FileWriteLine(ready, '{"tick":4}\n');
		LuaMan:FileClose(ready);
	end
	return false, false;
end
