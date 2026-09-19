-- Drives GameIntensityCalculator and DummyAssault's AddObjectivePoint line.
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
	-- Two peers keep different local cameras; the intensity box must still match.
	if os.getenv("CCCP_INTENSITY_CAMERA") == "client" then
		CameraMan:SetOffset(Vector(8000, 8000), 0);
	else
		CameraMan:SetOffset(Vector(0, 0), 0);
	end
	if not self.hurt then
		for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
			if self:PlayerActive(player) and self:PlayerHuman(player) then
				local actor = self:GetPlayerBrain(player) or self:GetControlledActor(player);
				if actor then
					actor.Health = actor.Health - 40;
				end
			end
		end
		self.hurt = true;
	end
	self.GameIntensityCalculator:UpdateGameIntensityCalculator();
	self:ClearObjectivePoints();
	local headX, headY = 0, 0;
	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		if self:PlayerActive(player) and self:PlayerHuman(player) then
			local actor = self:GetPlayerBrain(player) or self:GetControlledActor(player);
			if actor then
				-- DummyAssault.lua:181
				self.CPUBrain = actor;
				self:AddObjectivePoint("Destroy!", self.CPUBrain.AboveHeadPos+Vector(0,-16), Activity.TEAM_1, GameActivity.ARROWDOWN);
				headX = self.CPUBrain.AboveHeadPos.X + 0;
				headY = self.CPUBrain.AboveHeadPos.Y - 16;
			end
		end
	end
	self:SaveString("GameIntensityCalculatorMainTable", tostring(self.GameIntensityCalculator.saveTable.CurrentIntensity));
	local intensity = self:LoadString("GameIntensityCalculatorMainTable");
	local cam = CameraMan:GetOffset(0);
	print("[intensity-objectives] saved=" .. intensity .. " head=" .. headX .. "," .. headY .. " cam=" .. cam.X .. "," .. cam.Y);
end

function IntensityObjectives:EndActivity()
end
