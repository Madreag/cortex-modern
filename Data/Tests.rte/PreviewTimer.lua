-- An unchanged mod keeps a Timer in its instance and resets it in Update: a preview's copy resets and reads its own Timer,
-- one Timer held twice stays one, and the live instance's Timer keeps its start. The self-test reads them through OnMessage.
function Create(self)
	self.timer = Timer();
	self.alias = self.timer;
end

function Update(self)
	self.timer:Reset();
end

function OnMessage(self, message)
	if message == "init" then
		self.timer.ElapsedSimTimeMS = 5000;
		return;
	end
	_ScriptFieldsStash = _ScriptFieldsStash or {};
	_ScriptFieldsStash["preview-timer:" .. message] = math.floor(self.timer.ElapsedSimTimeMS + 0.5);
	_ScriptFieldsStash["preview-timer-alias:" .. message] = rawequal(self.alias, self.timer);
end
