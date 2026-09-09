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
	if self.starts >= 2 then
		-- The mod drops its cached object when the game it saved comes back, and the collector takes it.
		_ScriptedObjects["setaside_park"] = nil;
		collectgarbage("collect");
		print("[setaside-park] dropped start=" .. self.starts .. " parked=" .. tostring(self.parked));
	end
end

function CheckpointGlobalScript:UpdateScript()
	updates = updates + 1;
	self.calls = updates;
	if updates == 20 and not self.parked then
		-- A script-owned object no graph root reaches: the class a set-aside world keeps naming.
		_ScriptedObjects = _ScriptedObjects or {};
		_ScriptedObjects["setaside_park"] = { held = CreateMOPixel("Spark Yellow 1", "Base.rte") };
		self.parked = _ScriptedObjects["setaside_park"].held.UniqueID;
		print("[setaside-park] parked uid=" .. self.parked);
	end
	if self.parked and (updates >= 45 and updates <= 52 or updates >= 95 and updates <= 105) then
		-- After the probe put the original world back: is the object the mod still holds still known?
		local park = _ScriptedObjects["setaside_park"];
		local alive = -1;
		if park and park.held then alive = park.held.UniqueID; end
		local found = MovableMan:FindObjectByUniqueID(self.parked);
		print("[setaside-park] check update=" .. updates .. " parked=" .. self.parked .. " alive_uid=" .. alive .. " findable=" .. tostring(found ~= nil));
	end
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
		print("[global-callback] deactivated calls=" .. updates);
	end
end

function CheckpointGlobalScript:CraftEnteredOrbit(craft)
	orbits = orbits + 1;
	self.orbitUID = craft.UniqueID;
	print("[global-callback] orbit=" .. orbits .. " craft=" .. self.orbitUID);
end
