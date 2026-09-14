-- Control: same capture as the write fixture, no OnFire/OnStride body.

local SCRIPT = "UserScenes.rte/preview_held_ref_control.lua"

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
end

function OnStride(self)
end
