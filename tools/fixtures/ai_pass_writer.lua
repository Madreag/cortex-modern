-- The F72-B end-to-end fixture's actor script. Its AI pass - owner-only, per machine - sends the
-- actor a message whose receiver writes simulation state (the pattern BrowncoatBoss.lua:103-113 and
-- :177-201 use), and gibs a second object. Both calls must land on every peer at the committed tick.
function Create(self)
	self.f72bClock = Timer();
	self.f72bSent = false;
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
end
