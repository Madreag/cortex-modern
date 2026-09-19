-- What a shared script sees of terrain it just dug. Scene:CalculatePath from a script that is not the
-- running AI actor reads the committed horizon under lockstep, so a path asked on the tick the hole was
-- dug still answers against the terrain of T - H; off lockstep the same call sees the hole at once.
-- The fixture asserts nothing: it prints the three lengths so the reference, the broken and the repaired
-- executables each say what they see, and the lag stays visible.
local digTick = 120;
local laterTick = 180;
local SHAFT_X = 900;
local SHAFT_DEPTH = 140;
local DIG_RADIUS = 24;

function DigThenPath:StartActivity(isNewGame)
	self.digTickCount = 0;
	self.digGround = nil;
end

function DigThenPath:Ends()
	-- The pair the path is asked for: just above the ground, and straight down inside the terrain. Only
	-- the dug shaft joins them at this dig strength.
	local ground = self.digGround;
	return ground + Vector(0, -30), ground + Vector(0, SHAFT_DEPTH);
end

function DigThenPath:Measure(phase)
	local scene = SceneMan.Scene;
	local start, finish = self:Ends();
	-- Dig strength 1: the pathfinder will not eat through undisturbed terrain, so the hole is the story.
	local length = scene:CalculatePath(start, finish, 0, 1, Activity.NOTEAM);
	print("[digpath] tick=" .. self.digTickCount .. " phase=" .. phase .. " len=" .. length);
end

function DigThenPath:UpdateActivity()
	self.digTickCount = self.digTickCount + 1;
	if not self.digGround then
		self.digGround = SceneMan:MovePointToGround(Vector(SHAFT_X, 0), 20, 10);
	end
	if self.digTickCount == digTick - 1 then
		self:Measure("before");
	end
	if self.digTickCount == digTick then
		-- Dig the shaft, then ask on the same tick, in that order, from one shared script.
		local dug = 0;
		for step = 0, SHAFT_DEPTH, DIG_RADIUS do
			for _ in SceneMan:DislodgePixelCircle(self.digGround + Vector(0, step), DIG_RADIUS, true) do
				dug = dug + 1;
			end
		end
		print("[digpath] tick=" .. self.digTickCount .. " dug=" .. dug);
		self:Measure("same");
	end
	if self.digTickCount == laterTick then
		self:Measure("later");
	end
end
