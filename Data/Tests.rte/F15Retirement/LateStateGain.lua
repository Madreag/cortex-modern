-- Ordinary mod hooks: plant this script on the gun, then make a scripted item from the firing hook.

local SELF_SCRIPT = "Tests.rte/F15Retirement/LateStateGain.lua"
local LATE_SCRIPT = "Tests.rte/PreviewCompat.lua"

function Create(self)
	self.f15Late = 0
end

function Update(self)
	local gun = self.EquippedItem
	if gun and not gun:HasScript(SELF_SCRIPT) then
		gun:AddScript(SELF_SCRIPT)
		print(string.format("[f15late] planted on %s", tostring(gun.UniqueID)))
	end
end

function OnFire(self)
	local item = CreateHDFirearm("Battle Rifle", "Base.rte")
	if not item then
		print("[f15late] no preset for the late item")
		return
	end
	item:AddScript(LATE_SCRIPT)
	print(string.format("[f15late] fire self=%s item=%s scripted=%s", tostring(self.UniqueID), tostring(item.UniqueID), tostring(item:HasScript(LATE_SCRIPT))))
	MovableMan:AddItem(item)
end
