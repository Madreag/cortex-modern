-- The F72-B arm's activity: a stock three-team skirmish that also seats one AI actor whose pass
-- writes (a message to itself and a gib of a free object) and reports what every peer holds. The
-- writes themselves live in the actor's script; this file only stages them and reads the result.
SkirmishDefense = AIPassWrites;
dofile("Base.rte/Activities/SkirmishDefense.lua");
local stockStart = AIPassWrites.StartActivity;
local stockUpdate = AIPassWrites.UpdateActivity;
local stockEnd = AIPassWrites.EndActivity;

local BRAIN_X = 880;
local WRITER_X = 1240;

local function HumanTeam(activity, team)
	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		if activity:PlayerActive(player) and activity:PlayerHuman(player) and activity:GetTeamOfPlayer(player) == team then
			return true;
		end
	end
	return false;
end

function AIPassWrites:StartActivity(isNewGame)
	if isNewGame ~= false then
		for team = Activity.TEAM_2, Activity.TEAM_3 do
			if not HumanTeam(self, team) then
				self.CPUTeam = team;
				self:SetTeamTech(team, "Coalition.rte");
			end
		end
		self:SetTeamTech(Activity.TEAM_1, "Coalition.rte");
		-- The AI's first wave is late enough that the writer's own calls are what the arm sees first.
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

		-- The object the pass gibs: a sentry actor, which stands still and does not expire, so its
		-- disappearance is the gib and nothing else.
		local victim = CreateAHuman("Green Dummy", "Base.rte");
		local writer = CreateAHuman("Green Dummy", "Base.rte");
		if victim and writer then
			victim.Team = Activity.TEAM_2;
			victim.AIMode = Actor.AIMODE_SENTRY;
			victim.Pos = SceneMan:MovePointToGround(Vector(WRITER_X + 80, 0), 20, 10) + Vector(0, -victim.Radius);
			MovableMan:AddActor(victim);
			writer.Team = Activity.TEAM_2;
			writer.AIMode = Actor.AIMODE_SENTRY;
			writer.Pos = SceneMan:MovePointToGround(Vector(WRITER_X, 0), 20, 10) + Vector(0, -writer.Radius);
			writer:AddScript("UserScenes.rte/AIPassWriter.lua");
			writer:SetNumberValue("F72BVictim", victim.UniqueID);
			MovableMan:AddActor(writer);
			self.f72bWriterUID = writer.UniqueID;
			self.f72bVictimUID = victim.UniqueID;
		end
	end
	self.f72bClock = Timer();
	self.f72bReport = Timer();
	stockStart(self, isNewGame);
end

function AIPassWrites:UpdateActivity()
	if not self.f72bClock then
		self.f72bClock = Timer();
		self.f72bReport = Timer();
	end
	stockUpdate(self);
	if self.f72bReport:IsPastSimMS(1000) then
		self.f72bReport:Reset();
		self:ReportRow();
	end
end

function AIPassWrites:EndActivity()
	self:ReportRow();
	stockEnd(self);
end

function AIPassWrites:ReportRow()
	if not self.f72bClock then
		return;
	end
	local writer = self.f72bWriterUID and MovableMan:FindObjectByUniqueID(self.f72bWriterUID) or nil;
	local victim = self.f72bVictimUID and MovableMan:FindObjectByUniqueID(self.f72bVictimUID) or nil;
	local heard = writer and writer:GetNumberValue("F72BHeard") or -1;
	local vel = writer and (string.format("%.4f", writer.Vel.X) .. "," .. string.format("%.4f", writer.Vel.Y)) or "na";
	-- FindObjectByUniqueID hands back a MovableObject: Health is an Actor property, so cast first.
	local writerActor = writer and MovableMan:IsActor(writer) and ToActor(writer) or nil;
	local health = writerActor and string.format("%.2f", writerActor.Health) or "na";
	local particles = 0;
	for _ in MovableMan.Particles do
		particles = particles + 1;
	end
	print("[f72b-probe] simms=" .. math.floor(self.f72bClock.ElapsedSimTimeMS) ..
		" heard=" .. math.floor(heard) ..
		" vel=" .. vel ..
		" health=" .. health ..
		" victim=" .. (victim and 1 or 0) ..
		" particles=" .. particles);
end
