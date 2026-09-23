-- A file-scope local an edge hook counts: a preview's OnFire must leave it as the window found it.
local shots = 0

function OnFire(self)
	shots = shots + 1
end

function ReadShots(self)
	return shots
end
