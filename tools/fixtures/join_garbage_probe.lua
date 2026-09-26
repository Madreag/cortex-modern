-- The FeelBaseline duel as a mod's activity that drops one Lua-owned object every update and then asks, by unique ID, which of
-- the last 64 it dropped still exist. Every peer drops the same objects and collects them on the same ticks, so every peer
-- must give the same answer at every tick, a peer that joined from an image included.
dofile("Userdata/UserScenes.rte/FeelBaseline.lua");
local updateBaseline = FeelBaseline.UpdateActivity;

function FeelBaseline:UpdateActivity()
	updateBaseline(self);
	local dropped = CreateMOPixel("Spark Yellow 1", "Base.rte");
	self.droppedIDs = self.droppedIDs or {};
	table.insert(self.droppedIDs, dropped.UniqueID);
	if #self.droppedIDs > 64 then
		table.remove(self.droppedIDs, 1);
	end
	dropped = nil;
	self.probeTicks = (self.probeTicks or 0) + 1;
	local found = 0;
	for _, id in ipairs(self.droppedIDs) do
		if MovableMan:FindObjectByUniqueID(id) ~= nil then
			found = found + 1;
		end
	end
	print("[uidprobe] update=" .. tostring(self.probeTicks) .. " found=" .. tostring(found));
end
