function Create(self)
	self.previewCompatShots = 0
end

function OnFire(self)
	self.previewCompatShots = (self.previewCompatShots or 0) + 1
end

function Update(self)
	local path = "Tests.rte/PreviewCompat.lua"
	if not self:HasScript(path) then
		path = "W104bCompat.rte/PreviewCompat.lua"
	end
	local gun = self.EquippedItem
	if gun and not gun:HasScript(path) then
		gun:AddScript(path)
	end
	ConsoleMan:PrintString(string.format("[preview-compat] class=%s uid=%s shots=%s particles=%s", tostring(self.ClassName), tostring(self.UniqueID), tostring(self.previewCompatShots or 0), tostring(MovableMan:GetParticleCount())))
end
