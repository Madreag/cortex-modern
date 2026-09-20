P4AlphaDuel = FeelBaseline;
dofile("Data/Base.rte/Activities/P4AlphaDuel.lua");
local startDuel = FeelBaseline.StartActivity;
local updateDuel = FeelBaseline.UpdateActivity;
-- The driver rewrites this line with the run's tick target before it stages the script.
local WINDOW_TICKS = 1200;

local function CollectTeamActors(team)
    local actors = {};
    local actor = MovableMan:GetFirstTeamActor(team, Activity.PLAYER_NONE);
    if not actor then
        return actors;
    end
    local first = actor;
    repeat
        table.insert(actors, actor);
        actor = MovableMan:GetNextTeamActor(team, actor);
    until not actor or actor.UniqueID == first.UniqueID;
    return actors;
end

local function KeepAlive(actor)
    actor.MissionCritical = true;
    actor.Health = actor.MaxHealth;
    if actor:GetWoundCount(true, true, true) > 0 then
        actor:RemoveWounds(1000, true, true, true);
    end
    local rotating = ToMOSRotating(actor);
    if rotating then
        for attachable in rotating.Attachables do
            attachable.MissionCritical = true;
        end
    end
end

local function ClearTeam(activity, team)
    activity:SetTeamFunds(0, team);
    for _, actor in ipairs(CollectTeamActors(team)) do
        MovableMan:RemoveActor(actor);
    end
end

local function ParkTeam(activity, team, x, fromY)
    local actors = CollectTeamActors(team);
    for i, actor in ipairs(actors) do
        local ground = SceneMan:MovePointToGround(Vector(x + (i - 1) * 45, fromY), 20, 10);
        actor.Pos = ground + Vector(0, -actor.Radius);
    end
    local brain = MovableMan:GetFirstBrainActor(team);
    if not brain then
        return;
    end
    for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
        if activity:PlayerActive(player) and activity:GetTeamOfPlayer(player) == team then
            activity:SetLandingZone(brain.Pos, player);
            activity:SetObservationTarget(brain.Pos, player);
        end
    end
end

function FeelBaseline:StartActivity(startNewGame)
    MetricsCollector:BeginRun("FeelBaseline", 0);
    self.feelTicks = 0;
    startDuel(self, startNewGame);
    if startNewGame == false then
        return;
    end
    for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
        if self:PlayerActive(player) and not self:PlayerHuman(player) then
            self:DeactivatePlayer(player);
        end
    end
    for team = Activity.TEAM_1, Activity.MAXTEAMCOUNT - 1 do
        if not self:IsHumanTeam(team) then
            ClearTeam(self, team);
        end
    end
    -- Grasslands wraps X; park each team on the surface a third of the scene apart, out of each other's reach.
    for team = Activity.TEAM_1, Activity.MAXTEAMCOUNT - 1 do
        if self:IsHumanTeam(team) then
            ParkTeam(self, team, 400 + team * SceneMan.SceneWidth / 3, 0);
        end
    end
    self:DisableAIs(true, Activity.NOTEAM);
    print("[feel-baseline] parked brains out of reach");
end

function FeelBaseline:UpdateActivity()
    self.feelTicks = self.feelTicks + 1;
    -- The window measures feel, so the duellists never die: the scene's cast and cost stay fixed.
    for team = Activity.TEAM_1, Activity.MAXTEAMCOUNT - 1 do
        if self:IsHumanTeam(team) then
            for _, actor in ipairs(CollectTeamActors(team)) do
                KeepAlive(actor);
            end
        end
    end
    -- The measurement window is the objective here; a lost brain must not decide the duel inside it.
    self.TeamHadBrain = {};
    updateDuel(self);
    if self.feelTicks >= WINDOW_TICKS then
        MetricsCollector:Record("final_tick", self.feelTicks);
        MetricsCollector:SetResult(true);
        MetricsCollector:EndRun();
        self.ActivityState = Activity.OVER;
    end
end

function FeelBaseline:EndActivity()
    if self.feelTicks < WINDOW_TICKS then
        MetricsCollector:Record("final_tick", self.feelTicks);
        MetricsCollector:SetResult(false);
        MetricsCollector:EndRun();
    end
end
