local updates = 0;

function CheckpointGlobalScript:StartScript()
	self.starts = (self.starts or 0) + 1;
	local actor = CreateAHuman("Green Dummy", "Base.rte");
	actor.Pos = Vector(600, 100);
	actor.Team = -1;
	actor.PinStrength = 10000;
	actor.HitsMOs = false;
	actor.GetsHitByMOs = false;
	actor:SetNumberValue("global_script_spawn", 1);
	self.spawn = actor;
	MovableMan:AddActor(actor);
	print("[global-callback] start=" .. self.starts .. " actor=" .. self.spawn.UniqueID);
end

function CheckpointGlobalScript:UpdateScript()
	updates = updates + 1;
	self.calls = updates;
	ActivityMan:GetActivity():SetTeamFunds(10000 + updates, 0);
	if updates == 70 then
		self:Deactivate();
		print("[global-callback] deactivated calls=" .. updates);
	end
end
