-- A mod keeps a thruster in a global. On the preview's copy its stride hook asks the thruster for its burst impulse, a
-- call whose name reads but which fills a cache on the thruster the real script later reads at another burst scale.
PreviewCacheHandles = PreviewCacheHandles or {}

-- A preview may not print, so its note waits in the stash the window never rolls back, and the real script prints it.
local NOTES = "preview-cache:notes"
local CHECK_TICK = 480

function Create(self)
	PreviewCacheHandles[self.UniqueID] = self
	if PreviewCacheThruster == nil then
		PreviewCacheThruster = CreateAEmitter("Rocket MK2 Main Thruster", "Base.rte")
		PreviewCacheThruster.BurstScale = 1
	end
	self.cacheTicks = 0
end

function OnStride(self)
	local thruster = PreviewCacheThruster
	if thruster == nil or rawequal(PreviewCacheHandles[self.UniqueID], self) then
		return
	end
	local impulse = thruster:EstimateImpulse(true)
	if _ScriptFieldsStash then
		_ScriptFieldsStash[NOTES] = (_ScriptFieldsStash[NOTES] or "") .. string.format("[preview-cache] preview uid=%d impulse=%s\n", self.UniqueID, tostring(impulse))
	end
end

function Update(self)
	self.cacheTicks = (self.cacheTicks or 0) + 1
	local thruster = PreviewCacheThruster
	-- The committed read steers the sim: health follows the impulse, so a cache only one peer filled shows.
	if thruster ~= nil and self.cacheTicks == CHECK_TICK then
		thruster.BurstScale = 3
		local impulse = thruster:EstimateImpulse(true)
		self.Health = 50 + impulse % 40
		print(string.format("[preview-cache] commit uid=%d tick=%d impulse=%.4f health=%.4f", self.UniqueID, self.cacheTicks, impulse, self.Health))
	end
	local notes = _ScriptFieldsStash and _ScriptFieldsStash[NOTES]
	if notes then
		_ScriptFieldsStash[NOTES] = nil
		for line in string.gmatch(notes, "[^\n]+") do
			print(line)
		end
	end
end
