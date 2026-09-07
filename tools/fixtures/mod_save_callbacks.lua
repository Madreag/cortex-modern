function SaveActivity(self)
	self.save_callback_count = (self.save_callback_count or 0) + 1;
	ActivityMan:GetActivity():SetTeamFunds(246, 0);
	SceneMan.Scene:SetArea(Area("Activity save callback area"));
end

function OnSave(self)
	self:SetNumberValue("save_callback_count", self:GetNumberValue("save_callback_count") + 1);
	ActivityMan:GetActivity():SetTeamFunds(357, 0);
	SceneMan.Scene.GlobalAcc = Vector(1, 23);
	local area = Area("Save callback area");
	area:AddBox(Box(Vector(30, 40), 10, 20));
	SceneMan.Scene:SetArea(area);
	local added = CreateAHuman("Green Dummy", "Base.rte");
	added.Pos = self.Pos + Vector(100, 0);
	added.Team = self.Team;
	added:SetNumberValue("save_callback_spawn", 1);
	MovableMan:AddActor(added);
	self:EraseFromTerrain();
end
