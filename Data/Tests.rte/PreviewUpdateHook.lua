-- An unchanged mod counts its updates in Update: a preview's copy counts in its own instance, and the live instance's count
-- moves only on its own ticks. The self-test reads both counts through OnMessage.
function Create(self)
	self.updates = 0;
end

function Update(self)
	self.updates = self.updates + 1;
end

function OnMessage(self, message)
	if message ~= "init" then
		_ScriptFieldsStash = _ScriptFieldsStash or {};
		_ScriptFieldsStash["preview-update:" .. message] = self.updates;
	end
end
