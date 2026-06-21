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
