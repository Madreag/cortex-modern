-- The screen-width arm's activity: the shipped Skirmish Defense, seated the way the scenario menu
-- would seat it, with the AI attacker whose landing zones the shipped LandingZoneMap picks. The
-- stock script is called unchanged; the only additions are the seating, the trace arming and one
-- probe line per AI drop so the landing zone the LZ map chose is readable from the log.
SkirmishDefense = StockSkirmish;
dofile("Base.rte/Activities/SkirmishDefense.lua");
local stockStart = StockSkirmish.StartActivity;
local stockUpdate = StockSkirmish.UpdateActivity;

local BRAIN_X = 880;

local function HumanTeam(activity, team)
	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		if activity:PlayerActive(player) and activity:PlayerHuman(player) and activity:GetTeamOfPlayer(player) == team then
			return true;
		end
	end
	return false;
end

-- Every AI drop goes through one of these; the first argument is always the landing zone's X.
for _, name in ipairs({"CreateHeavyDrop", "CreateMediumDrop", "CreateLightDrop", "CreateScoutDrop", "CreateBreachDrop", "CreateBombDrop"}) do
	local stockCreate = StockSkirmish[name];
	StockSkirmish[name] = function(self, xPosLZ, ...)
		print("[lzprobe] simms=" .. math.floor(self.probeTimer and self.probeTimer.ElapsedSimTimeMS or 0) ..
			" drop=" .. name .. " x=" .. string.format("%.6f", xPosLZ or -1));
		return stockCreate(self, xPosLZ, ...);
	end
end

function StockSkirmish:StartActivity(isNewGame)
	if isNewGame ~= false then
		-- The scenario menu's team panel marks the AI sides; a preset cannot, because the activity is
		-- cloned from it and only the legacy CPUTeam scalar survives the copy.
		for team = Activity.TEAM_2, Activity.TEAM_3 do
			if not HumanTeam(self, team) then
				self.CPUTeam = team;
				-- Without a faction the AI's RandomACDropShip finds no craft and the side never attacks.
				self:SetTeamTech(team, "Coalition.rte");
			end
		end
		self:SetTeamTech(Activity.TEAM_1, "Coalition.rte");
		-- The first AI wave is (8000 - 50 * Difficulty) ms in, so the arm reaches a drop quickly.
		self.Difficulty = 100;
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
	self.probeTimer = Timer();
	-- RecordTickHash is a no-op until a metrics run is open, and the trace is this arm's oracle.
	-- The arm asserts nothing itself, so the run carries no verdict of its own: the comparer is it.
	MetricsCollector:BeginRun("Stock Skirmish", 0);
	MetricsCollector:SetResult(true);
	stockStart(self, isNewGame);
end

function StockSkirmish:UpdateActivity()
	if not self.probeTimer then
		self.probeTimer = Timer();
	end
	stockUpdate(self);
end
