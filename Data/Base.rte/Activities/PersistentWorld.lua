package.loaded.Constants = nil; require("Constants");

-- A world that outlives its players. It never ends: no last-brain check, no timer, no win. A team that
-- loses its resident gets a new one after a fixed delay, at a fixed place, in the world author's one
-- ordered transition, so every machine spawns the same actor at the same committed tick.

local TEAM_HOME_X = {[Activity.TEAM_1] = 880, [Activity.TEAM_2] = 1120, [Activity.TEAM_3] = 640, [Activity.TEAM_4] = 1360};
local RESPAWN_DELAY_TICKS = 300; -- Five seconds of simulation at the fixed timestep.

-- A team with a seated human is the host's: it authors that seat's respawn as a world transition, so
-- the team loop below must not spawn a second resident for it.
local function TeamHasSeat(self, team)
	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		if self:PlayerActive(player) and self:PlayerHuman(player) and self:GetTeamOfPlayer(player) == team then
			return true;
		end
	end
	return false;
end

local function HomePosition(team)
	local x = TEAM_HOME_X[team] or 880;
	return SceneMan:MovePointToGround(Vector(x, 0), 20, 10) + Vector(0, -20);
end

local function SpawnResident(self, team, startNewGame)
	local position = HomePosition(team);
	if startNewGame then
		-- The world's first build is local: no round is running yet, so there is nothing to order.
		local actor = CreateAHuman("Brain Robot", "Base.rte");
		if not actor then
			return nil;
		end
		actor.Team = team;
		actor.AIMode = Actor.AIMODE_SENTRY;
		actor.Pos = position - Vector(0, actor.Radius - 20);
		local weapon = CreateHDFirearm("Battle Rifle", "Base.rte");
		if weapon then
			actor:AddInventoryItem(weapon);
			actor:EquipNamedDevice("Base.rte", "Battle Rifle", true);
		end
		MovableMan:AddActor(actor);
		return actor;
	end
	self:SubmitWorldRespawn(team, "AHuman", "Brain Robot", "Base.rte", position, Actor.AIMODE_SENTRY);
	return nil;
end

function PersistentWorld:StartActivity(startNewGame)
	self.ActivityState = Activity.RUNNING;
	self.respawnDueAt = self.respawnDueAt or {};
	self.worldTick = self.worldTick or 0;
	if startNewGame == false then
		-- A restored world already holds its actors, funds and terrain; only the local seats are new.
		for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
			if self:PlayerActive(player) and self:PlayerHuman(player) then
				local team = self:GetTeamOfPlayer(player);
				local brain = MovableMan:GetFirstBrainActor(team);
				if brain then
					self:SetPlayerBrain(brain, player);
					self:SwitchToActor(brain, player, team);
					self:SetLandingZone(brain.Pos, player);
					self:SetObservationTarget(brain.Pos, player);
				end
			end
		end
		return;
	end
	for team = Activity.TEAM_1, Activity.MAXTEAMCOUNT - 1 do
		if self:TeamActive(team) then
			self:SetTeamFunds(0, team);
			SpawnResident(self, team, true);
		end
	end
	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		if self:PlayerActive(player) and self:PlayerHuman(player) then
			local team = self:GetTeamOfPlayer(player);
			local brain = MovableMan:GetFirstBrainActor(team);
			if brain then
				self:SetPlayerBrain(brain, player);
				self:SwitchToActor(brain, player, team);
				self:SetLandingZone(brain.Pos, player);
				self:SetObservationTarget(brain.Pos, player);
			end
		end
	end
end

function PersistentWorld:UpdateActivity()
	if self.ActivityState ~= Activity.RUNNING then
		return;
	end
	-- UpdateActivity runs once per simulation tick on every machine, so this counter is the world's own
	-- clock: it rides the checkpoint, and a peer that joined at tick B keeps counting from B.
	self.worldTick = (self.worldTick or 0) + 1;
	local now = self.worldTick;
	self.respawnDueAt = self.respawnDueAt or {};
	local author = self:IsWorldAuthor();
	for team = Activity.TEAM_1, Activity.MAXTEAMCOUNT - 1 do
		if self:TeamActive(team) and not TeamHasSeat(self, team) then
			if MovableMan:GetFirstBrainActor(team) ~= nil then
				self.respawnDueAt[team] = nil;
			elseif self.respawnDueAt[team] == nil then
				self.respawnDueAt[team] = now + RESPAWN_DELAY_TICKS;
			elseif now >= self.respawnDueAt[team] then
				self.respawnDueAt[team] = now + RESPAWN_DELAY_TICKS;
				if author then
					SpawnResident(self, team, false);
				end
			end
		end
	end
	-- No end condition of any kind: this Activity is never Over, whoever is or is not playing it.
end
