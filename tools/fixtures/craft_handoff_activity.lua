package.loaded.Constants = nil; require("Constants");

-- The first player's seat flies a landing craft that carries one passenger; the craft opens its hatch in
-- the air and hands the passenger out, the way an exit hands a player's craft over to the first one out.
local function SpawnBrain(x, team)
	local brain = CreateAHuman("Brain Robot", "Base.rte");
	brain.Team = team;
	brain.AIMode = Actor.AIMODE_SENTRY;
	brain.Pos = SceneMan:MovePointToGround(Vector(x, 0), 20, 10) + Vector(0, -brain.Radius);
	MovableMan:AddActor(brain);
	return brain;
end

function CraftHandoff:StartActivity(startNewGame)
	self.ActivityState = Activity.RUNNING;
	self.fixtureTick = 0;
	local brains = {};
	brains[Activity.TEAM_1] = SpawnBrain(880, Activity.TEAM_1);
	brains[Activity.TEAM_2] = SpawnBrain(1320, Activity.TEAM_2);
	local craft = CreateACRocket("Rocket MK2", "Base.rte");
	craft.Team = Activity.TEAM_1;
	craft.Pos = SceneMan:MovePointToGround(Vector(520, 0), 20, 10) + Vector(0, -320);
	local passenger = CreateAHuman("Green Dummy", "Base.rte");
	passenger.Team = Activity.TEAM_1;
	craft:AddInventoryItem(passenger);
	MovableMan:AddActor(craft);
	self.craft = craft;
	self.passenger = passenger;
	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		if self:PlayerActive(player) and self:PlayerHuman(player) then
			local team = self:GetTeamOfPlayer(player);
			local brain = brains[team];
			if brain then
				local unit = team == Activity.TEAM_1 and craft or brain;
				self:SetPlayerBrain(brain, player);
				self:SwitchToActor(unit, player, team);
				self:SetLandingZone(unit.Pos, player);
				self:SetObservationTarget(unit.Pos, player);
			end
		end
	end
end

function CraftHandoff:UpdateActivity()
	if self.ActivityState ~= Activity.RUNNING then
		return;
	end
	self.fixtureTick = self.fixtureTick + 1;
	-- The seat's switch rides the wire, so every peer sees the passenger played at the same tick.
	if self.passengerOut and not self.passengerPlayed and MovableMan:ValidMO(self.passenger) and self.passenger:IsPlayerControlled() then
		self.passengerPlayed = true;
		print("[craft-fixture] passenger played tick=" .. self.fixtureTick);
	end
	local craft = self.craft;
	if not craft or not MovableMan:ValidMO(craft) then
		return;
	end
	if self.fixtureTick == 60 then
		ToACraft(craft):OpenHatch();
		print("[craft-fixture] hatch opening tick=" .. self.fixtureTick);
	end
	if not self.passengerOut and craft:IsInventoryEmpty() then
		self.passengerOut = true;
		print("[craft-fixture] passenger out tick=" .. self.fixtureTick);
	end
end
