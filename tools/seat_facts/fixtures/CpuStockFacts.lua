SkirmishDefense = CpuStockFacts;
dofile("Base.rte/Activities/SkirmishDefense.lua");
local start = CpuStockFacts.StartActivity;
local update = CpuStockFacts.UpdateActivity;
local observe = dofile("UserScenes.rte/CpuObservation.lua");

function CpuStockFacts:StartActivity(isNewGame)
    observe(self, "roster", 0);
    start(self, isNewGame);
    self.cpuFactsTick = 0;
    observe(self, "started", 0);
end

function CpuStockFacts:UpdateActivity()
    self.cpuFactsTick = self.cpuFactsTick + 1;
    if self.cpuFactsTick == 1 then observe(self, "first_tick", 1); end
    update(self);
end
