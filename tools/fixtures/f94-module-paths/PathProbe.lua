-- Shared-script path probe: one solved pair and one with no solution, every 100 ticks.

local BRAIN_X = 1600;
local SOLVE_B = Vector(3600, 220);
local FAIL_A = Vector(2000, 2000);
local FAIL_B = Vector(2100, 2100);

local function HumanTeam(activity, team)
	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		if activity:PlayerActive(player) and activity:PlayerHuman(player) and activity:GetTeamOfPlayer(player) == team then
			return true;
		end
	end
	return false;
end

function PathProbe:StartActivity(isNewGame)
	self.ActivityState = Activity.RUNNING;
	self.updates = 0;
	self.nextReq = 0;

	local cpuTeam;
	local lastActive;
	for team = Activity.TEAM_1, Activity.MAXTEAMCOUNT - 1 do
		if self:TeamActive(team) then
			lastActive = team;
			if not HumanTeam(self, team) and cpuTeam == nil then
				cpuTeam = team;
			end
		end
	end
	self.CPUTeam = cpuTeam or lastActive or Activity.TEAM_2;

	if isNewGame ~= false then
		for team = Activity.TEAM_1, Activity.MAXTEAMCOUNT - 1 do
			if self:TeamActive(team) then
				self:SetTeamFunds(0, team);
			end
		end
		for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
			if self:PlayerActive(player) and self:PlayerHuman(player) then
				local team = self:GetTeamOfPlayer(player);
				local brain = CreateAHuman("Brain Robot", "Base.rte");
				brain.Team = team;
				brain.AIMode = Actor.AIMODE_SENTRY;
				brain.Pos = SceneMan:MovePointToGround(Vector(BRAIN_X + player * 120, 0), 20, 10) + Vector(0, -brain.Radius);
				MovableMan:AddActor(brain);
				self:SetPlayerBrain(brain, player);
				self:SwitchToActor(brain, player, team);
				self:SetLandingZone(brain.Pos, player);
				self:SetObservationTarget(brain.Pos, player);
			end
		end
	end
end

function PathProbe:Issue(startPos, endPos)
	self.nextReq = self.nextReq + 1;
	local req = self.nextReq;
	local issued = self.updates;
	local activity = self;
	SceneMan.Scene:CalculatePathAsync(function(pathRequest)
		local nodes = 0;
		if pathRequest.Path then
			for _ in pathRequest.Path do
				nodes = nodes + 1;
			end
		end
		print("[pathprobe] u=" .. req .. " uid=" .. req ..
			" req=" .. req ..
			" issued=" .. issued ..
			" done=" .. activity.updates ..
			" status=" .. pathRequest.Status ..
			" len=" .. pathRequest.PathLength ..
			" nodes=" .. nodes);
	end, startPos, endPos, 20, 1);
end

function PathProbe:UpdateActivity()
	self.updates = self.updates + 1;
	if self.updates >= 100 and self.updates <= 3000 and self.updates % 100 == 0 then
		for x = 200, 3200, 200 do
			self:Issue(Vector(x, 220), SOLVE_B);
		end
		self:Issue(FAIL_A, FAIL_B);
	end
end
