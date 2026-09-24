-- A mod keeps a handle to its actor and also finds it in the world's actor list. On the predicting peer a
-- preview's edge hook runs this same code on the preview's copy, so nothing it writes may reach the world's actor.
PreviewFenceHandles = PreviewFenceHandles or {}

function Create(self)
	PreviewFenceHandles[self.UniqueID] = self
end

function OnStride(self)
	local kept = PreviewFenceHandles[self.UniqueID]
	if kept == nil then
		return
	end
	local listed = nil
	for actor in MovableMan.Actors do
		if actor.UniqueID == self.UniqueID then
			listed = actor
			break
		end
	end
	if listed == nil then
		return
	end
	-- A native property through the binding, then a write through the alias the Pos getter hands out.
	local health, x = self.Health, self.Pos.X
	listed.Health = listed.Health - 0.125
	listed.Pos.X = listed.Pos.X + 0.125
	local took = (self.Health == health - 0.125 and 1 or 0) + (self.Pos.X == x + 0.125 and 2 or 0)
	-- The same two writes through the handle kept from before any preview.
	local keptHealth, keptX = kept.Health, kept.Pos.X
	kept.Health = kept.Health - 0.0625
	kept.Pos.X = kept.Pos.X + 0.0625
	if not rawequal(kept, self) then
		print(string.format("[preview-fence] preview uid=%d took=%d kept_health=%.4f->%.4f kept_x=%.4f->%.4f",
			self.UniqueID, took, keptHealth, kept.Health, keptX, kept.Pos.X))
	end
end
