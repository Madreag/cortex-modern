-- Ends the round on sim time and prints OVER.
-- The detector is the native ApplyObserveLookAround row.
GameOverObserve = {};

local BRAIN_X = 880;

function GameOverObserve:StartActivity(isNewGame)
	if isNewGame ~= false then
		for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
			if self:PlayerActive(player) and self:PlayerHuman(player) and not self:GetPlayerBrain(player) then
				local team = self:GetTeamOfPlayer(player);
				local brain = CreateAHuman("Brain Robot", "Base.rte");
				if brain then
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
	self.clock = Timer();
	self.ended = false;
	self.ticks = 0;
end

function GameOverObserve:UpdateActivity()
	if not self.clock then
		self.clock = Timer();
	end
	self.ticks = (self.ticks or 0) + 1;
	if not self.ended and self.clock:IsPastSimMS(500) then
		self.WinnerTeam = Activity.TEAM_1;
		self.ActivityState = Activity.OVER;
		self.GameOverTimer:Reset();
		for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
			if self:PlayerActive(player) and self:PlayerHuman(player) then
				self:SetViewState(Activity.OBSERVE, player);
			end
		end
		self.ended = true;
		print("[game-over-freeze] over tick=" .. self.ticks);
	end
end

function GameOverObserve:EndActivity()
end
