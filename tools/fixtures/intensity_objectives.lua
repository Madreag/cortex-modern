-- Drives GameIntensityCalculator and the stock AddObjectivePoint(AboveHeadPos) shape.
IntensityObjectives = {};

local BRAIN_X = 880;

function IntensityObjectives:StartActivity(isNewGame)
	self.GameIntensityCalculator = require("Activities/Utility/GameIntensityCalculator");
	self.GameIntensityCalculator:Initialize(self, isNewGame ~= false, 0.2, 0.01);
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
				end
			end
		end
	end
end

function IntensityObjectives:UpdateActivity()
	if not self.GameIntensityCalculator then
		self.GameIntensityCalculator = require("Activities/Utility/GameIntensityCalculator");
		self.GameIntensityCalculator:Initialize(self, false, 0.2, 0.01);
	end
	self.GameIntensityCalculator:UpdateGameIntensityCalculator();
	self:ClearObjectivePoints();
	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		if self:PlayerActive(player) and self:PlayerHuman(player) then
			local actor = self:GetPlayerBrain(player) or self:GetControlledActor(player);
			if actor then
				self:AddObjectivePoint("Protect!", actor.AboveHeadPos, self:GetTeamOfPlayer(player), GameActivity.ARROWDOWN);
			end
		end
	end
	self:SaveString("GameIntensityCalculatorMainTable", tostring(self.GameIntensityCalculator.saveTable.CurrentIntensity));
	local intensity = self:LoadString("GameIntensityCalculatorMainTable");
	print("[intensity-objectives] saved=" .. intensity);
end

function IntensityObjectives:EndActivity()
end
