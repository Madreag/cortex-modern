-- The F72-B end-to-end fixture's actor script. Its AI pass - owner-only, per machine - sends the
-- actor a message whose receiver writes simulation state (the pattern BrowncoatBoss.lua:103-113 and
-- :177-201 use), and gibs a second object. Both calls must land on every peer at the committed tick.
-- It also makes the two calls no wire can carry, to show they still behave as they always did: a
-- table context, which delivers on the machine that made it.
function Create(self)
	self.f72bClock = Timer();
	self.f72bSent = false;
	self.f72bTableSent = false;
	self.f72bGibbed = false;
end

function ThreadedUpdateAI(self)
	if not self.f72bClock then
		self.f72bClock = Timer();
	end
	if not self.f72bSent and self.f72bClock:IsPastSimMS(2000) then
		-- One line per peer whose pass actually runs: the arm reads it to tell a wire delivery from two
		-- machines each deciding for themselves.
		print("[f72b-pass] sent uid=" .. self.UniqueID);
		self:SendMessage("F72B_Flying", true);
		self.f72bSent = true;
	end
	if not self.f72bTableSent and self.f72bClock:IsPastSimMS(3000) then
		-- A table context is not a thing the wire can name, and never was. The reference simply delivered
		-- it; so must this build, on this peer, with no boundary line in the log.
		self.f72bTableHeard = 0;
		print("[f72b-pass] table uid=" .. self.UniqueID);
		self:SendMessage("F72B_Table", {});
		print("[f72b-table] uid=" .. self.UniqueID .. " heard=" .. self.f72bTableHeard);
		self.f72bTableSent = true;
	end
	if not self.f72bGibbed and self.f72bClock:IsPastSimMS(4000) then
		-- FindObjectByUniqueID hands back a MovableObject; GibThis lives on MOSRotating, so cast first.
		local found = MovableMan:FindObjectByUniqueID(self:GetNumberValue("F72BVictim"));
		local victim = found and ToMOSRotating(found) or nil;
		self.f72bGibbed = true;
		if victim then
			print("[f72b-pass] gib uid=" .. self.UniqueID);
			victim:GibThis();
		end
	end
end

function OnMessage(self, message, context)
	if message == "F72B_Flying" and context == true then
		-- Two simulation writes the per-tick hash carries, gated on a message the AI pass sent. Health
		-- is the one a probe row can still see a second later; velocity washes out against the ground.
		self.Vel = Vector(self.Vel.X + 3, self.Vel.Y - 2);
		self.Health = self.Health - 5;
		self:SetNumberValue("F72BHeard", self:GetNumberValue("F72BHeard") + 1);
		print("[f72b-heard] uid=" .. self.UniqueID);
	end
	if message == "F72B_Table" then
		-- Script state only: this delivery is the sending peer's alone, so a simulation write here would
		-- be the desync the wire path exists to stop.
		self.f72bTableHeard = (self.f72bTableHeard or 0) + 1;
		print("[f72b-table-heard] uid=" .. self.UniqueID .. " kind=" .. type(context));
	end
end
