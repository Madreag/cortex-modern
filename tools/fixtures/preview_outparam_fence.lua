-- A mod reuses a file-scope Vector as a ray cast's out-parameter. On the predicting peer a preview's stride hook casts
-- the same ray from the preview's copy, so the cast may not write the Vector the real script keeps.
local rayHit = Vector()
PreviewOutparamHandles = PreviewOutparamHandles or {}

-- A preview may not print, so its note waits in the stash the window never rolls back, and the real script prints it.
local NOTES = "preview-outparam:notes"

function Create(self)
	PreviewOutparamHandles[self.UniqueID] = self
end

function OnStride(self)
	local before = rayHit.X
	SceneMan:CastStrengthRay(self.Pos, Vector(0, 200), 10, rayHit, 1, 0, true)
	local after = rayHit.X
	if not rawequal(PreviewOutparamHandles[self.UniqueID], self) and _ScriptFieldsStash then
		_ScriptFieldsStash[NOTES] = (_ScriptFieldsStash[NOTES] or "") .. string.format("[preview-outparam] preview uid=%d hit_before=%.3f hit_after=%.3f\n",
			self.UniqueID, before, after)
	end
end

function Update(self)
	-- The Vector steers the sim: health follows where the last cast hit, so a cast only one peer made shows in the world.
	self.Health = 100 - (math.floor(rayHit.X) % 10) / 10
	local notes = _ScriptFieldsStash and _ScriptFieldsStash[NOTES]
	if notes then
		_ScriptFieldsStash[NOTES] = nil
		for line in string.gmatch(notes, "[^\n]+") do
			print(line)
		end
	end
end
