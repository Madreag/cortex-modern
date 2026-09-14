-- Detecting: a module cached by require in Create, its methods reached from a preview OnFire.

local function scriptPath(self)
	local path = "Tests.rte/PreviewModuleCompat.lua"
	if self:HasScript(path) then
		return path
	end
	return "UserScenes.rte/PreviewModuleCompat.lua"
end

function Create(self)
	self.previewModuleUtility = require("Scripts/Utility/ParticleUtility")
	self.previewModuleCalls = 0
end

function Update(self)
	if not IsAHuman(self) then
		return
	end
	local path = scriptPath(self)
	local item = self.EquippedItem
	if item and not item:HasScript(path) then
		item:AddScript(path)
	end
end

function OnFire(self)
	self.previewModuleCalls = (self.previewModuleCalls or 0) + 1
	local cached = self.previewModuleUtility
	local method = type(cached) == "table" and type(cached.CreateDirectionalSmokeEffect) == "function"
	print(string.format("[preview-module-fixture] uid=%s calls=%s cached=%s method=%s", tostring(self.UniqueID), tostring(self.previewModuleCalls), type(cached), tostring(method)))
end
