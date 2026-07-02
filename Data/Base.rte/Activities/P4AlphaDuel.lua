package.loaded.Constants = nil; require("Constants");

local TEAM_ONE_X = 880;
local TEAM_TWO_X = 1120;

local function SpawnHuman(presetName, x, team, aimode)
	local actor = CreateAHuman(presetName, "Base.rte");
	if not actor then
		return nil;
	end
	actor.Team = team;
	actor.AIMode = aimode or Actor.AIMODE_SENTRY;
	actor.Pos = SceneMan:MovePointToGround(Vector(x, 0), 20, 10) + Vector(0, -actor.Radius);
	local weapon = CreateHDFirearm("Battle Rifle", "Base.rte");
	if weapon then
		actor:AddInventoryItem(weapon);
		actor:EquipNamedDevice("Base.rte", "Battle Rifle", true);
	end
	-- Two carried spares give the inventory-op sync (swap/reorder/drop) something to work on.
	for _ = 1, 2 do
		local spare = CreateHDFirearm("Battle Rifle", "Base.rte");
		if spare then
			actor:AddInventoryItem(spare);
		end
	end
	MovableMan:AddActor(actor);
	return actor;
end

function P4AlphaDuel:StartActivity()
	self.ActivityState = Activity.RUNNING;
	self:SetTeamFunds(0, Activity.TEAM_1);
	self:SetTeamFunds(0, Activity.TEAM_2);

	local brains = {};
	brains[Activity.TEAM_1] = SpawnHuman("Brain Robot", TEAM_ONE_X, Activity.TEAM_1, Actor.AIMODE_SENTRY);
	brains[Activity.TEAM_2] = SpawnHuman("Brain Robot", TEAM_TWO_X, Activity.TEAM_2, Actor.AIMODE_SENTRY);
	SpawnHuman("Green Dummy", TEAM_ONE_X - 45, Activity.TEAM_1, Actor.AIMODE_SENTRY);
	SpawnHuman("Green Dummy", TEAM_TWO_X + 45, Activity.TEAM_2, Actor.AIMODE_SENTRY);

	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		if self:PlayerActive(player) and self:PlayerHuman(player) then
			local team = self:GetTeamOfPlayer(player);
			local brain = brains[team];
			if brain then
				self:SetPlayerBrain(brain, player);
				self:SwitchToActor(brain, player, team);
				self:SetLandingZone(brain.Pos, player);
				self:SetObservationTarget(brain.Pos, player);
			end
		end
	end
end

function P4AlphaDuel:UpdateActivity()
	if self.ActivityState ~= Activity.RUNNING then
		return;
	end
	-- The win check reads only team-level sim state; player bindings differ per peer in a net match.
	self.TeamHadBrain = self.TeamHadBrain or {};
	local liveBrains = {};
	for team = Activity.TEAM_1, Activity.MAXTEAMCOUNT - 1 do
		if self:TeamActive(team) then
			liveBrains[team] = MovableMan:GetFirstBrainActor(team) ~= nil and 1 or 0;
			if liveBrains[team] > 0 then
				self.TeamHadBrain[team] = true;
			end
		end
	end
	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		if self:PlayerActive(player) and self:PlayerHuman(player) then
			local team = self:GetTeamOfPlayer(player);
			if self.TeamHadBrain[team] and (liveBrains[team] or 0) < 1 then
				self:ResetMessageTimer(player);
				local screen = self:ScreenOfPlayer(player);
				FrameMan:ClearScreenText(screen);
				FrameMan:SetScreenText("Your brain has been destroyed!", screen, 2000, -1, false);
			end
		end
	end
	-- A team is out only once a brain it HAD is gone; the match ends when at most one team stands.
	local aliveTeams = {};
	local anyTeamOut = false;
	for team = Activity.TEAM_1, Activity.MAXTEAMCOUNT - 1 do
		if self.TeamHadBrain[team] then
			if (liveBrains[team] or 0) < 1 then
				anyTeamOut = true;
			else
				table.insert(aliveTeams, team);
			end
		end
	end
	if anyTeamOut and #aliveTeams <= 1 then
		-- The last brained team wins; a double kill or a sole-brained world ends as a draw.
		if #aliveTeams == 1 then
			self.WinnerTeam = aliveTeams[1];
			MovableMan:KillAllEnemyActors(self.WinnerTeam);
		end
		ActivityMan:EndActivity();
	end
end
