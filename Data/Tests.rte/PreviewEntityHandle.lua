-- An unchanged mod keeps the activity it runs in, as Void Wanderers' Lazor Rifle and the Scanner do, and reads through it in a
-- hook; a second case writes through it. The self-test calls OnMessage on the live actor and on its preview copy.
function Create(self)
	self.activity = ActivityMan:GetActivity();
	self.fundsTeam = 0;
end

function OnMessage(self, message)
	if message == "read" or message == "live-read" then
		_ScriptFieldsStash = _ScriptFieldsStash or {};
		_ScriptFieldsStash["preview-entity:" .. message] = tostring(self.activity:GetTeamFunds(self.fundsTeam)) .. " screen=" .. tostring(self.activity:ScreenOfPlayer(0));
	elseif message == "write" then
		self.activity:SetTeamFunds(self.activity:GetTeamFunds(self.fundsTeam) + 100, self.fundsTeam);
	end
end
