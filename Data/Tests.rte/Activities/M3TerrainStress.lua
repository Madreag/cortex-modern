-- M3TerrainStress.lua — MP M3 determinism scenario: drive heavy terrain
-- destruction so the `carve_math` and `terrain` subsystems of the per-tick
-- BLAKE3 trace have a demanding workload to compare across runs.
--
-- Design notes:
--   * Six Green Dummies in BRAINHUNT mode at deterministic positions across
--     the bunker. They fight; bullets, grenades and impacts carve terrain in
--     several regions at once. Every atom-terrain contact also drives
--     WillPenetrate, so `carve_math` is exercised every tick regardless of
--     whether a pixel is actually removed.
--   * Tutorial Bunker scene (consistent with the M1 / Trust scenarios) — its
--     mixed bunker-wall and dirt materials exercise the IsScrap / scrap-column
--     branches of the penetration path.
--   * 900 ticks (15 sim-seconds) — long enough for sustained terrain carve.
--   * Pass = reach max_ticks. Determinism is the assertion; combat outcome is
--     incidental.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioM3TerrainStress = Trust.Extend("M3TerrainStress", { max_ticks = 900 });

function TestScenarioM3TerrainStress:OnStart()
    -- Two opposing lines of brain-hunters so terrain is carved on both sides.
    -- Y=50 = safe drop onto the bunker roof (per TrustScenario's spawn note).
    self:SpawnActor("Green Dummy", "Base.rte", 800,  50, Activity.TEAM_1, Actor.AIMODE_BRAINHUNT);
    self:SpawnActor("Green Dummy", "Base.rte", 920,  50, Activity.TEAM_1, Actor.AIMODE_BRAINHUNT);
    self:SpawnActor("Green Dummy", "Base.rte", 1040, 50, Activity.TEAM_1, Actor.AIMODE_BRAINHUNT);
    self:SpawnActor("Green Dummy", "Base.rte", 1160, 50, Activity.TEAM_2, Actor.AIMODE_BRAINHUNT);
    self:SpawnActor("Green Dummy", "Base.rte", 1280, 50, Activity.TEAM_2, Actor.AIMODE_BRAINHUNT);
    self:SpawnActor("Green Dummy", "Base.rte", 1400, 50, Activity.TEAM_2, Actor.AIMODE_BRAINHUNT);
    self:RecordMetric("actor_count", 6);
end

function TestScenarioM3TerrainStress:OnTick(tick)
    if tick % 120 == 0 then
        self:RecordMetric("alive_at_tick_" .. tostring(tick), self:CountLivingActors());
    end
    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioM3TerrainStress:OnEnd()
    self._passed = true;
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("final_alive_count", self:CountLivingActors());
end
