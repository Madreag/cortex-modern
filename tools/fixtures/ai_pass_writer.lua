-- The F72-B end-to-end fixture's actor script. Its AI pass - owner-only, per machine - sends the
-- actor a message whose receiver writes simulation state (the pattern BrowncoatBoss.lua:103-113 and
-- :177-201 use), and gibs a second object. Both calls must land on every peer at the committed tick.
-- It also makes the two calls no wire can carry, to show they still behave as they always did: a
-- table context, which delivers on the machine that made it, and a gib read back in the same pass.
-- The one-argument SendMessage is the opposite case: no context is nameable, so it has to cross.
function Create(self)
	self.f72bClock = Timer();
	self.f72bSent = false;
	self.f72bTableSent = false;
	self.f72bGibbed = false;
	self.f72bOneArgSent = false;
	self.f72bOneArg = false;
	self.f72bOneArgSeen = false;
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
			-- What the same pass sees of its own gib. The reference gibbed inside the call, so it reads
			-- false there; under lockstep the gib is a command for the committed tick and the object is
			-- still here. Printed, never asserted, so the row reads the same way on every executable.
			print("[f72b-gib] samepass uid=" .. self.UniqueID .. " valid=" .. tostring(MovableMan:ValidMO(found)));
		end
	end
	if not self.f72bOneArgSent and self.f72bClock:IsPastSimMS(5000) then
		-- No context at all. That is a context the wire can name, so this must cross like the two-argument
		-- form; on a build where it does not, only the peer whose pass ran ever sets the flag below.
		print("[f72b-pass] onearg uid=" .. self.UniqueID);
		self:SendMessage("F72B_OneArg");
		self.f72bOneArgSent = true;
	end
end

function ThreadedUpdate(self)
	-- A non-AI script: it runs on EVERY peer, which is why a flag its OnMessage sets has to be set on
	-- every peer too. The BrowncoatBoss.lua:177-201 shape - read the flag here, write simulation state.
	if not self.f72bClock then
		self.f72bClock = Timer();
	end
	if self.f72bOneArg and not self.f72bOneArgSeen then
		self.f72bOneArgSeen = true;
		self.Health = self.Health - 3;
		print("[f72b-onearg] uid=" .. self.UniqueID .. " ms=" .. math.floor(self.f72bClock.ElapsedSimTimeMS));
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
	if message == "F72B_OneArg" then
		-- Script state only here; the simulation write is ThreadedUpdate's, gated on this flag.
		self.f72bOneArg = true;
		print("[f72b-onearg-heard] uid=" .. self.UniqueID);
	end
	if message == "F72B_Table" then
		-- Script state only: this delivery is the sending peer's alone, so a simulation write here would
		-- be the desync the wire path exists to stop.
		self.f72bTableHeard = (self.f72bTableHeard or 0) + 1;
		print("[f72b-table-heard] uid=" .. self.UniqueID .. " kind=" .. type(context));
	end
end
