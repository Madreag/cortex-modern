-- A mod times its actor's strides with a Timer it keeps in self and marks each stride with a particle. On the
-- predicting peer a preview's stride hook runs this code on the preview's copy, whose self shares that Timer.
PreviewMethodHandles = PreviewMethodHandles or {}

-- A preview may not print, so its note waits in the stash the window never rolls back, and the real script prints it.
local NOTES = "preview-method:notes"

function Create(self)
	PreviewMethodHandles[self.UniqueID] = self
	self.strideTimer = Timer()
end

function OnStride(self)
	local before = self.strideTimer.ElapsedSimTimeMS
	self.strideTimer:Reset()
	local after = self.strideTimer.ElapsedSimTimeMS
	local mark = CreateMOPixel("Spark Yellow 1", "Base.rte")
	mark.Pos = Vector(self.Pos.X, self.Pos.Y - 60)
	mark.Vel = Vector(0, 0)
	mark.PinStrength = 1000
	mark.Lifetime = 400
	MovableMan:AddParticle(mark)
	if not rawequal(PreviewMethodHandles[self.UniqueID], self) and _ScriptFieldsStash then
		_ScriptFieldsStash[NOTES] = (_ScriptFieldsStash[NOTES] or "") .. string.format("[preview-method] preview uid=%d timer_before=%.3f timer_after=%.3f\n",
			self.UniqueID, before, after)
	end
end

function Update(self)
	-- The Timer steers the sim: health follows it, so a reset only one peer made shows in the world.
	self.Health = 100 - math.min(self.strideTimer.ElapsedSimTimeMS, 2000) / 1000
	local notes = _ScriptFieldsStash and _ScriptFieldsStash[NOTES]
	if notes then
		_ScriptFieldsStash[NOTES] = nil
		for line in string.gmatch(notes, "[^\n]+") do
			print(line)
		end
	end
end
