-- Detecting: a Create-time non-MO Entity on self freezes preview hooks.

local SCRIPT = "UserScenes.rte/preview_held_ref_fallback.lua"

local function capture(self)
	if self.heldActivity then
		return
	end
	self.heldActivity = ActivityMan:GetActivity()
	if self.heldActivity then
		print(string.format("[held-ref-fallback] capture self=%s class=%s", tostring(self.UniqueID), self.heldActivity.ClassName))
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
	if not self.heldActivity then
		capture(self)
	end
	attachEquipped(self)
end

function OnFire(self)
	self.Vel = self.Vel + Vector(0, -0.01)
	print(string.format("[held-ref-fallback] write self=%s", tostring(self.UniqueID)))
end
