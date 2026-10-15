package.loaded.Constants = nil; require("Constants");
local Test = dofile("Data/Tests.rte/Lib/TestScenario.lua");
-- A single-player world that grows and shrinks: Lua garbage early, then a particle storm that dies in the air.
MemoryStormSP = Test.Extend("MemoryStormSP", { max_ticks = 1500 });

local c_GarbageFirst, c_GarbageLast, c_GarbageTables = 60, 119, 40000;
local c_StormFirst, c_StormLast, c_StormParticles = 180, 199, 10000;

function MemoryStormSP:OnStart()
	self._spawned = 0;
end

function MemoryStormSP:OnTick(tick)
	if tick >= c_GarbageFirst and tick <= c_GarbageLast then
		local garbage = {};
		for index = 1, c_GarbageTables do
			garbage[index] = { index, index * 2, "storm garbage " .. tick .. " " .. index };
		end
	end
	if tick >= c_StormFirst and tick <= c_StormLast then
		for index = 1, c_StormParticles do
			local particle = CreateMOPixel("Flash Particle Yellow Big", "Base.rte");
			particle.Pos = Vector(200 + (index % 1600), 150 + (index % 60));
			particle.Vel = Vector(((index % 7) - 3) * 0.5, -12 - (index % 5));
			particle.Lifetime = 2000;
			particle.HitsMOs = false;
			particle.GetsHitByMOs = false;
			MovableMan:AddParticle(particle);
		end
		self._spawned = self._spawned + c_StormParticles;
	end
	if tick >= self._maxTicks then
		print("[memory-storm] spawned=" .. self._spawned);
		return true, true;
	end
	return false, false;
end
