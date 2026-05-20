-- M4ThreadStress.lua — MP M4 determinism scenario: saturate the threaded sim
-- paths so the per-tick BLAKE3 trace has a demanding workload to compare
-- ACROSS thread counts (the determinism-check `--threads 1,2,4,8,16` matrix).
--
-- Design notes:
--   * 40 Green Dummies in BRAINHUNT mode on a deterministic two-row grid. The
--     count is well above the 16-state ceiling of the thread matrix, so every
--     Lua state gets several registered actors — the ThreadedUpdate /
--     ThreadedUpdateAI parallel passes are genuinely saturated at every count.
--   * BRAINHUNT — the actors actively path, look and fight. That drives the
--     three parallel passes M4 makes deterministic: ThreadedUpdateAI (the AI
--     itself), CastSeeRays (40 actors casting vision rays every tick), and the
--     ThreadedUpdate device hooks of whatever the dummies carry.
--   * Attrition is intentional: as dummies die they gib and drop inventory,
--     which exercises the threaded-Lua -> C++ g_SimRNG consumers (GibThis,
--     DropAllInventory) that Block C's per-worker RNG redirect must cover.
--   * Tutorial Bunker scene — consistent with the M1 / M3 stress scenarios.
--   * 900 ticks (15 sim-seconds) — long enough for sustained threaded load.
--   * Pass = reach max_ticks. Determinism across thread counts is the assertion;
--     combat outcome is incidental.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioM4ThreadStress = Trust.Extend("M4ThreadStress", { max_ticks = 900 });

local SPAWN_COLUMNS = 20;
local SPAWN_ROWS    = 2;
local X_START       = 700;
local X_STEP        = 60;
local ROW_Y         = { 50, 18 };

function TestScenarioM4ThreadStress:OnStart()
    local count = 0;
    for row = 0, SPAWN_ROWS - 1 do
        for col = 0, SPAWN_COLUMNS - 1 do
            -- Stagger the second row half a step so the two rows don't land stacked.
            local x = X_START + col * X_STEP + row * 30;
            local y = ROW_Y[row + 1];
            local team = (col % 2 == 0) and Activity.TEAM_1 or Activity.TEAM_2;
            self:SpawnActor("Green Dummy", "Base.rte", x, y, team, Actor.AIMODE_BRAINHUNT);
            count = count + 1;
        end
    end
    self:RecordMetric("actor_count", count);
end

function TestScenarioM4ThreadStress:OnTick(tick)
    if tick % 120 == 0 then
        self:RecordMetric("alive_at_tick_" .. tostring(tick), self:CountLivingActors());
    end
    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioM4ThreadStress:OnEnd()
    self._passed = true;
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("final_alive_count", self:CountLivingActors());
end
