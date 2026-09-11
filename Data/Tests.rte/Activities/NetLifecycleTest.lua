package.loaded.Constants = nil; require("Constants");

local function SpawnUnit(preset, x, team)
	local actor = CreateAHuman(preset, "Base.rte");
	actor.Team = team;
	actor.AIMode = Actor.AIMODE_SENTRY;
	actor.Pos = SceneMan:MovePointToGround(Vector(x, 0), 20, 10) + Vector(0, -actor.Radius);
	MovableMan:AddActor(actor);
	return actor;
end

function NetLifecycleTest:StartActivity(startNewGame)
	self.ActivityState = Activity.RUNNING;
	if startNewGame ~= false then
		self.Brains = {};
		self.Units = {};
		for team = Activity.TEAM_1, Activity.MAXTEAMCOUNT - 1 do
			if self:TeamActive(team) then
				self:SetTeamFunds(0, team);
				local x = 650 + team * 300;
				self.Brains[team] = SpawnUnit("Brain Robot", x, team);
				self.Units[team] = SpawnUnit("Green Dummy", x + 60, team);
			end
		end
	end
	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		if self:PlayerActive(player) and self:PlayerHuman(player) then
			local team = self:GetTeamOfPlayer(player);
			local brain = self.Brains[team];
			local unit = self:GetLockstepHumanSlotIndex(team) == 1 and self.Units[team] or brain;
			self:SetPlayerBrain(brain, player);
			self:SwitchToActor(unit, player, team);
			self:SetLandingZone(unit.Pos, player);
			self:SetObservationTarget(unit.Pos, player);
		end
	end
end

function NetLifecycleTest:UpdateActivity()
	for _, units in ipairs({self.Brains, self.Units}) do
		for _, actor in pairs(units) do
			if not MovableMan:IsActor(actor) or actor.Health <= 0 then
				error("Net Lifecycle Test lost an unarmed sentry");
			end
		end
	end
end
