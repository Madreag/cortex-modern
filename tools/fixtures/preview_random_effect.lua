-- A mod jolts its actor by a random amount on every stride. On the predicting peer a preview's stride hook runs this code
-- on the preview's copy: the jolt shows there at once, and the draws the committed hooks make stay what they were.
PreviewRandomHandles = PreviewRandomHandles or {}

-- A preview may not print, so its note waits in the stash the window never rolls back, and the real script prints it.
local NOTES = "preview-random:notes"

function Create(self)
	PreviewRandomHandles[self.UniqueID] = self
	self.randomStrides = 0
end

function OnStride(self)
	local jolt = RangeRand(0.05, 0.15)
	local pick = math.random(1, 100)
	local spin = NormalRand()
	local chance = PosRand()
	local before = self.Health
	if type(jolt) == "number" then
		self.Health = self.Health - jolt
	end
	if not rawequal(PreviewRandomHandles[self.UniqueID], self) then
		if _ScriptFieldsStash then
			_ScriptFieldsStash[NOTES] = (_ScriptFieldsStash[NOTES] or "") .. string.format("[preview-random] preview uid=%d jolt=%s pick=%s spin=%s chance=%s health_before=%.4f health_after=%.4f\n",
				self.UniqueID, tostring(jolt), tostring(pick), tostring(spin), tostring(chance), before, self.Health)
		end
		return
	end
	-- The committed draws steer the sim, so a stream only one peer advanced shows in the world.
	self.randomStrides = (self.randomStrides or 0) + 1
	print(string.format("[preview-random] commit uid=%d stride=%d jolt=%.6f pick=%d spin=%.6f chance=%.6f health=%.4f", self.UniqueID, self.randomStrides, jolt, pick, spin, chance, self.Health))
end

function Update(self)
	local notes = _ScriptFieldsStash and _ScriptFieldsStash[NOTES]
	if notes then
		_ScriptFieldsStash[NOTES] = nil
		for line in string.gmatch(notes, "[^\n]+") do
			print(line)
		end
	end
end
