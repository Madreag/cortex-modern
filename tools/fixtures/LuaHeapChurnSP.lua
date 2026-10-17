package.loaded.Constants = nil; require("Constants");
local Test = dofile("Data/Tests.rte/Lib/TestScenario.lua");
-- Continuous discarded graphs, as in a busy single-player script. No explicit GC:
-- this must exercise the ordinary incremental collector and its allocation limit.
LuaHeapChurnSP = Test.Extend("LuaHeapChurnSP", { max_ticks = 1800 });

function LuaHeapChurnSP:OnTick(tick)
	local garbage = {};
	for index = 1, 12000 do
		garbage[index] = { index, tick, "discarded " .. tick .. " " .. index };
	end
	if tick >= self._maxTicks then
		print("[lua-heap-churn] ticks=" .. tick .. " tables_per_tick=12000");
		return true, true;
	end
	return false, false;
end
