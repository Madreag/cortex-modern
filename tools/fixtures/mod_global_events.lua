local score = 0;
local starts = 0;

function CheckpointGlobalScript:StartScript()
	starts = starts + 1;
	self.alias = self;
	print("[global-events] started=" .. starts);
end

function CheckpointGlobalScript:ResetEvents()
	score = 0;
	self.lastCraft = nil;
end

function CheckpointGlobalScript:PauseScript(paused)
	score = score + (paused and 1 or 10);
end

function CheckpointGlobalScript:CraftEnteredOrbit(craft)
	assert(craft.UniqueID == self.expectedCraft.UniqueID, "global event craft alias changed");
	self.lastCraft = craft;
	score = score + 100;
end

function CheckpointGlobalScript:EndScript()
	score = score + 1000;
end

function CheckpointGlobalScript:VerifyEvents(expected)
	assert(starts == 1, "global event startup repeated");
	assert(score == expected, "global event score " .. score .. " expected " .. expected);
	assert(rawequal(self.alias, self), "global event self alias changed");
	if expected > 0 then
		assert(self.lastCraft.UniqueID == self.expectedCraft.UniqueID, "global event retained craft changed");
	end
end
