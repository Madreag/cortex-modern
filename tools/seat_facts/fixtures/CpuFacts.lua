P4AlphaDuel = CpuFacts;
dofile("Base.rte/Activities/P4AlphaDuel.lua");
local start = CpuFacts.StartActivity;
local update = CpuFacts.UpdateActivity;
local observe = dofile("UserScenes.rte/CpuObservation.lua");

function CpuFacts:StartActivity(isNewGame)
    observe(self, "roster", 0);
    start(self, isNewGame);
    self.cpuFactsTick = 0;
    observe(self, "started", 0);
end

function CpuFacts:UpdateActivity()
    self.cpuFactsTick = self.cpuFactsTick + 1;
    if self.cpuFactsTick == 1 then observe(self, "first_tick", 1); end
    update(self);
end
