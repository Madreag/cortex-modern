local updates = 0;
local orbits = 0;

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
	ActivityMan:GetActivity():SetTeamFunds(10000 + updates + 1000 * orbits, 0);
	if updates == 60 then
		local craft = CreateACDropShip("Dropship MK1", "Base.rte");
		craft.Team = 0;
		craft.Pos = Vector(600, -1000);
		craft.Vel = Vector(0, -10);
		self.expectedOrbitUID = craft.UniqueID;
		MovableMan:AddActor(craft);
		print("[global-callback] craft queued=" .. self.expectedOrbitUID .. " update=" .. updates);
	end
	if updates >= 61 and updates <= 65 then
		local found = false;
		for actor in MovableMan.Actors do
			if actor.UniqueID == self.expectedOrbitUID then
				found = true;
				print("[global-callback] craft update=" .. updates .. " y=" .. actor.Pos.Y .. " delete=" .. tostring(actor.ToDelete) .. " status=" .. actor.Status);
			end
		end
		if not found then print("[global-callback] craft absent update=" .. updates .. " orbits=" .. orbits); end
	end
	if updates == 70 then
		assert(orbits == 1, "global craft orbit callback lost or repeated");
		self:Deactivate();
		print("[global-callback] deactivated calls=" .. updates);
	end
end

function CheckpointGlobalScript:CraftEnteredOrbit(craft)
	orbits = orbits + 1;
	self.orbitUID = craft.UniqueID;
	print("[global-callback] orbit=" .. orbits .. " craft=" .. self.orbitUID);
end
