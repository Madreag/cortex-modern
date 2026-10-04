-- Gives each actor's rifle the coroutine-sequenced script (preview_coroutine_gun.lua). A script file serves the hooks of the
-- first class that loads it, so the rifle's script is a file of its own.
function Create(self)
	local gun = self.EquippedItem
	if gun then
		gun:AddScript("UserScenes.rte/preview_coroutine_gun.lua")
	end
end
