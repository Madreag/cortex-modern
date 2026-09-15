-- The brainless-spectate arms' activity: it seats the brain the scenario menu would place, marks the
-- AI sides the menu's team panel would mark, destroys every human brain at a fixed sim time and
-- reports the round state once a sim second. The end rule under test is the shipped script's, called
-- unchanged. The reporting lives here because a per-actor test script stops with the actor it rides,
-- and the actor under test is exactly the brain that dies.
SkirmishDefense = SpectateSkirmish;
dofile("Base.rte/Activities/SkirmishDefense.lua");
local stockStart = SpectateSkirmish.StartActivity;
local stockUpdate = SpectateSkirmish.UpdateActivity;
local stockEnd = SpectateSkirmish.EndActivity;

-- The driver stages the arm's options beside this file.
local options = {};
local loadedOk, loaded = pcall(dofile, "UserScenes.rte/SpectateOptions.lua");
if loadedOk and type(loaded) == "table" then
	options = loaded;
end

local BRAIN_X = 880;
local KILL_AT_MS = options.killAtMs or 5000;

local function HumanTeam(activity, team)
	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		if activity:PlayerActive(player) and activity:PlayerHuman(player) and activity:GetTeamOfPlayer(player) == team then
			return true;
		end
	end
	return false;
end

function SpectateSkirmish:StartActivity(isNewGame)
	if isNewGame ~= false then
		-- The scenario menu's team panel marks the AI sides. A preset cannot: the activity is cloned
		-- from it and only the legacy CPUTeam scalar survives the copy, so the flags are set here.
		-- Assigning CPUTeam both activates the team and marks it CPU.
		for team = Activity.TEAM_2, Activity.TEAM_3 do
			-- A net roster seats humans on these teams; only an empty side becomes an AI attacker.
			if not HumanTeam(self, team) then
				self.CPUTeam = team;
				-- Without a faction the AI's RandomACDropShip finds no craft and the side never attacks.
				self:SetTeamTech(team, "Coalition.rte");
			end
		end
		self:SetTeamTech(Activity.TEAM_1, "Coalition.rte");
		-- The AI's first wave is (8000 - 50 * Difficulty) ms in, and it only picks a landing zone while
		-- an enemy actor is on the field: at the shipped default the brain dies before a side attacks.
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
	self.spectateClock = Timer();
	self.spectateReport = Timer();
	self.spectateKilled = false;
	-- Arm the per-tick trace for the single-player arms: RecordTickHash is a no-op until a run is
	-- open, and that trace is what proves the spectator view writes no simulation state. A net peer
	-- must NOT do this: BeginRun re-reads the arming state from the scenario runner, which a match
	-- does not use, and would switch off the recording the match harness already armed.
	if options.armTrace then
		MetricsCollector:BeginRun("Spectate Skirmish", 0);
	end
	stockStart(self, isNewGame);
end

function SpectateSkirmish:UpdateActivity()
	if not self.spectateClock then
		self.spectateClock = Timer();
		self.spectateReport = Timer();
	end

	-- killAtMs = 0 is the control arm: nobody dies, so the round cannot end on the brain rule.
	if KILL_AT_MS > 0 and not self.spectateKilled and self.spectateClock:IsPastSimMS(KILL_AT_MS) then
		for team = Activity.TEAM_1, Activity.TEAM_4 do
			if self:TeamActive(team) and HumanTeam(self, team) then
				-- One pass: a gibbed actor is only removed at the end of the tick, so re-reading the
				-- roster here would hand back the same brain.
				local brain = MovableMan:GetFirstBrainActor(team);
				if brain then
					print("[spectate-probe] brain-kill simms=" .. math.floor(self.spectateClock.ElapsedSimTimeMS) ..
						" team=" .. team .. " uid=" .. brain.UniqueID);
					brain:GibThis();
				end
			end
		end
		self.spectateKilled = true;
	end

	stockUpdate(self);

	if self.spectateReport:IsPastSimMS(1000) then
		self.spectateReport:Reset();
		self:ReportSpectateRow();
	end
end

-- A round that ends stops being updated, so the closing row is written from the end hook.
function SpectateSkirmish:EndActivity()
	self:ReportSpectateRow();
	stockEnd(self);
end

function SpectateSkirmish:ReportSpectateRow()
	if not self.spectateClock then
		return;
	end
	local brainedTeams = 0;
	local teams = "";
	for team = Activity.TEAM_1, Activity.TEAM_4 do
		if self:TeamActive(team) and MovableMan:GetFirstBrainActor(team) then
			brainedTeams = brainedTeams + 1;
		end
		-- active:cpu:brained per team, so every side the round rule counts is visible.
		teams = teams .. (team > Activity.TEAM_1 and "," or "") ..
			(self:TeamActive(team) and 1 or 0) .. ":" .. (self:TeamIsCPU(team) and 1 or 0) .. ":" ..
			(MovableMan:GetFirstBrainActor(team) and 1 or 0);
	end
	local views = "";
	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		views = views .. (player > Activity.PLAYER_1 and "," or "") .. self:GetViewState(player);
	end
	local actors = 0;
	for _ in MovableMan.Actors do
		actors = actors + 1;
	end
	-- The rule in force on THIS machine: in a lockstep match it is the host's, everywhere else the
	-- local setting. A build without the query answers "na", which is the pre-change reference.
	local queried, rule = pcall(function() return self:BrainlessHumansSpectate(); end);
	local scroll = CameraMan:GetScrollTarget(0);
	print("[spectate-probe] simms=" .. math.floor(self.spectateClock.ElapsedSimTimeMS) ..
		" state=" .. self.ActivityState ..
		" views=" .. views ..
		" brained_teams=" .. brainedTeams ..
		" winner=" .. self.WinnerTeam ..
		" rule=" .. (queried and (rule and "1" or "0") or "na") ..
		" scroll=" .. math.floor(scroll.X) .. "," .. math.floor(scroll.Y) ..
		" actors=" .. actors ..
		" teams=" .. teams);
end
