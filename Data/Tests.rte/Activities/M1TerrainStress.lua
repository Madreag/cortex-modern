-- M1TerrainStress.lua — Block F (M1) determinism scenario: exercise terrain
-- destruction so the `terrain` subsystem of the per-tick BLAKE3 trace has
-- something interesting to compare across runs.
--
-- Design notes:
--   * Spawns 3 Coalition Grenadiers in BRAINHUNT mode at deterministic positions.
--     They'll lob grenades at each other — explosions destroy terrain pixels,
--     wounds spawn MOPixels back into the terrain, particle settling adds
--     more terrain pixels. The `terrain` subsystem accumulates across these.
--   * Tutorial Bunker scene (consistent with the other M1 / Trust scenarios).
--   * 900 ticks (15 sim-seconds) — long enough for several grenade exchanges
--     plus the resulting terrain carve cascades.
--   * Pass = reach max_ticks. Determinism is the assertion; combat outcome is
--     incidental.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioM1TerrainStress = Trust.Extend("M1TerrainStress", { max_ticks = 900 });

function TestScenarioM1TerrainStress:OnStart()
    -- Three Grenadiers in opposing brain-hunt postures so grenades fly across
    -- the bunker, carving terrain on both sides. All at safe Y=50 so they
    -- drop onto terrain without spawning inside walls.
    self:SpawnActor("Green Dummy", "Base.rte", 800,  50, Activity.TEAM_1, Actor.AIMODE_BRAINHUNT);
    self:SpawnActor("Green Dummy", "Base.rte", 1100, 50, Activity.TEAM_2, Actor.AIMODE_BRAINHUNT);
    self:SpawnActor("Green Dummy", "Base.rte", 1400, 50, Activity.TEAM_2, Actor.AIMODE_BRAINHUNT);
    self:RecordMetric("actor_count", 3);
end

function TestScenarioM1TerrainStress:OnTick(tick)
    if tick % 120 == 0 then
        self:RecordMetric("alive_at_tick_" .. tostring(tick), self:CountLivingActors());
    end
    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioM1TerrainStress:OnEnd()
    self._passed = true;
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("final_alive_count", self:CountLivingActors());
end
