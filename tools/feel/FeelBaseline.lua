P4AlphaDuel = FeelBaseline;
dofile("Data/Base.rte/Activities/P4AlphaDuel.lua");
local startDuel = FeelBaseline.StartActivity;
local updateDuel = FeelBaseline.UpdateActivity;

function FeelBaseline:StartActivity(startNewGame)
    MetricsCollector:BeginRun("FeelBaseline", 0);
    self.feelTicks = 0;
    startDuel(self, startNewGame);
end

function FeelBaseline:UpdateActivity()
    self.feelTicks = self.feelTicks + 1;
    updateDuel(self);
    if self.feelTicks >= 1200 then
        MetricsCollector:Record("final_tick", self.feelTicks);
        MetricsCollector:SetResult(true);
        MetricsCollector:EndRun();
        self.ActivityState = Activity.OVER;
    end
end

function FeelBaseline:EndActivity()
    if self.feelTicks < 1200 then
        MetricsCollector:Record("final_tick", self.feelTicks);
        MetricsCollector:SetResult(false);
        MetricsCollector:EndRun();
    end
end
