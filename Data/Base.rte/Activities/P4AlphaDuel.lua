package.loaded.Constants = nil; require("Constants");

local TEAM_ONE_X = 880;
local TEAM_TWO_X = 1120;
local TEAM_THREE_X = 640;
local TEAM_FOUR_X = 1360;

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
	local dummies = {};
	brains[Activity.TEAM_1] = SpawnHuman("Brain Robot", TEAM_ONE_X, Activity.TEAM_1, Actor.AIMODE_SENTRY);
	brains[Activity.TEAM_2] = SpawnHuman("Brain Robot", TEAM_TWO_X, Activity.TEAM_2, Actor.AIMODE_SENTRY);
	dummies[Activity.TEAM_1] = SpawnHuman("Green Dummy", TEAM_ONE_X - 45, Activity.TEAM_1, Actor.AIMODE_SENTRY);
	dummies[Activity.TEAM_2] = SpawnHuman("Green Dummy", TEAM_TWO_X + 45, Activity.TEAM_2, Actor.AIMODE_SENTRY);
	-- 3/4-player rosters bring extra teams; the added spawns keep the 2-team match untouched.
	if self:TeamActive(Activity.TEAM_3) then
		brains[Activity.TEAM_3] = SpawnHuman("Brain Robot", TEAM_THREE_X, Activity.TEAM_3, Actor.AIMODE_SENTRY);
		dummies[Activity.TEAM_3] = SpawnHuman("Green Dummy", TEAM_THREE_X - 45, Activity.TEAM_3, Actor.AIMODE_SENTRY);
	end
	if self:TeamActive(Activity.TEAM_4) then
		brains[Activity.TEAM_4] = SpawnHuman("Brain Robot", TEAM_FOUR_X, Activity.TEAM_4, Actor.AIMODE_SENTRY);
		dummies[Activity.TEAM_4] = SpawnHuman("Green Dummy", TEAM_FOUR_X + 45, Activity.TEAM_4, Actor.AIMODE_SENTRY);
	end

	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		if self:PlayerActive(player) and self:PlayerHuman(player) then
			local team = self:GetTeamOfPlayer(player);
			local brain = brains[team];
			if brain then
				-- A shared co-op team seats its second player at the dummy; the brain is the team's
				-- shared life. Solo teams (and SP) keep the brain, exactly as before.
				local unit = brain;
				if self:GetLockstepHumanSlotIndex(team) == 1 and dummies[team] then
					unit = dummies[team];
				end
				self:SetPlayerBrain(brain, player);
				self:SwitchToActor(unit, player, team);
				self:SetLandingZone(unit.Pos, player);
				self:SetObservationTarget(unit.Pos, player);
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
