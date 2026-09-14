P4AlphaDuel = CpuWriteFacts;
dofile("Base.rte/Activities/P4AlphaDuel.lua");
local start = CpuWriteFacts.StartActivity;
local update = CpuWriteFacts.UpdateActivity;
local observe = dofile("UserScenes.rte/CpuObservation.lua");

function CpuWriteFacts:StartActivity(isNewGame)
    observe(self, "roster", 0);
    start(self, isNewGame);
    local cputeam = Activity.NOTEAM;
    for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
        if self:PlayerActive(player) and self:PlayerHuman(player) then
            cputeam = self:GetTeamOfPlayer(player) + 1;
            if cputeam > Activity.TEAM_4 then cputeam = Activity.TEAM_1; end
        end
    end
    self.CPUTeam = cputeam;
    print("[cpu-write] derived=" .. cputeam .. " actual=" .. self.CPUTeam);
    self.cpuFactsTick = 0;
    observe(self, "started", 0);
end

function CpuWriteFacts:UpdateActivity()
    self.cpuFactsTick = self.cpuFactsTick + 1;
    if self.cpuFactsTick == 1 then observe(self, "first_tick", 1); end
    update(self);
end
