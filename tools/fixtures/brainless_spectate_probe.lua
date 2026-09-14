-- Drives the brainless-spectate arms: destroys every human team's brain at a fixed sim time and
-- prints the activity state, the first seat's view state and the brained teams once a sim second.

local function HumanTeam(activity, team)
	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		if activity:PlayerActive(player) and activity:PlayerHuman(player) and activity:GetTeamOfPlayer(player) == team then
			return true;
		end
	end
	return false;
end

function Create(self)
	SpectateProbe = SpectateProbe or {};
	SpectateProbe.clock = SpectateProbe.clock or Timer();
	SpectateProbe.report = SpectateProbe.report or Timer();
	SpectateProbe.killAtMS = SpectateProbe.killAtMS or 5000;
end

function Update(self)
	local activity = ActivityMan:GetActivity();
	if not activity or not SpectateProbe then
		return;
	end

	if SpectateProbe.report:IsPastSimMS(1000) then
		SpectateProbe.report:Reset();
		local brainedTeams = 0;
		for team = Activity.TEAM_1, Activity.TEAM_4 do
			if activity:TeamActive(team) and MovableMan:GetFirstBrainActor(team) then
				brainedTeams = brainedTeams + 1;
			end
		end
		local views = "";
		for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
			views = views .. (player > Activity.PLAYER_1 and "," or "") .. activity:GetViewState(player);
		end
		print("[spectate-probe] simms=" .. math.floor(SpectateProbe.clock.ElapsedSimTimeMS) ..
			" state=" .. activity.ActivityState ..
			" views=" .. views ..
			" brained_teams=" .. brainedTeams ..
			" winner=" .. activity.WinnerTeam);
	end

	if self:HasObjectInGroup("Brains") and HumanTeam(activity, self.Team) and SpectateProbe.clock:IsPastSimMS(SpectateProbe.killAtMS) then
		print("[spectate-probe] brain-kill simms=" .. math.floor(SpectateProbe.clock.ElapsedSimTimeMS) ..
			" team=" .. self.Team .. " uid=" .. self.UniqueID);
		self:GibThis();
	end
end
