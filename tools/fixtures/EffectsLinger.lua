local Test = dofile("Data/Tests.rte/Lib/TestScenario.lua");
EffectsLinger = Test.Extend("EffectsLinger", { max_ticks = 120 });

function EffectsLinger:OnStart()
	self:SpawnActor("Green Dummy", "Base.rte", 1200, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
end

function EffectsLinger:OnTick(tick)
	return tick >= 2, true;
end
