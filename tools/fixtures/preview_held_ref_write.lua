-- Detecting: a Create-time entity ref written from preview OnFire/OnStride.

local SCRIPT = "UserScenes.rte/preview_held_ref_write.lua"

local function capture(self)
	if self.heldRef then
		return
	end
	if IsActor(self) then
		local brain = MovableMan:GetFirstBrainActor(self.Team)
		if brain and brain.UniqueID ~= self.UniqueID then
			self.heldRef = brain
		elseif self.EquippedItem then
			self.heldRef = self.EquippedItem
		end
	else
		self.heldRef = self:GetParent()
	end
	if self.heldRef then
		print(string.format("[held-ref] capture self=%s held=%s", tostring(self.UniqueID), tostring(self.heldRef.UniqueID)))
	end
end

local function attachEquipped(self)
	if not IsAHuman(self) then
		return
	end
	local item = self.EquippedItem
	if item and not item:HasScript(SCRIPT) then
		item:AddScript(SCRIPT)
	end
end

local function writeThrough(self, hook)
	capture(self)
	if not self.heldRef then
		print("[held-ref] skip hook=" .. hook .. " no heldRef")
		return
	end
	self.heldRef.Vel = self.heldRef.Vel + Vector(0, -0.01)
	self.heldRef.previewWriteProbe = (self.heldRef.previewWriteProbe or 0) + 1
	print(string.format("[held-ref] write hook=%s held=%s probe=%s", hook, tostring(self.heldRef.UniqueID), tostring(self.heldRef.previewWriteProbe)))
end

function Create(self)
	capture(self)
	attachEquipped(self)
end

function Update(self)
	if not self.heldRef then
		capture(self)
	end
	attachEquipped(self)
end

function OnFire(self)
	writeThrough(self, "OnFire")
end

function OnStride(self)
	writeThrough(self, "OnStride")
end
