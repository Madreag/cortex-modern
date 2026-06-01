-- TerrainCarveStress.lua — determinism scenario: sustained combat-driven
-- terrain interaction. The concern is the `terrain` and `carve_math`
-- subsystems -- every projectile-terrain contact runs WillPenetrate even when
-- no pixel removes, so high-frequency projectile fire saturates the path.
--
-- Two SMG squads on the flat plateau open the fight; five reinforcement waves
-- drop in over the run so the firefight is sustained for the full 900 ticks --
-- a brief wipeout would leave 700 idle ticks and gut the test. SMGs and
-- grenades maximise both bullet contacts (WillPenetrate) and blast carves.
--
-- Distinct from TerrainStress: there a scripted carve cascade drives the
-- carve removals; here organic combat does. Same subsystem, different load.
--
-- Determinism: every spawn, wave tick, position and loadout is fixed; the only
-- randomness is the sim's seeded RNG.

package.loaded.Constants = nil; require("Constants");
local Test = require("Lib/TestScenario");

TestScenarioTerrainCarveStress = Test.Extend("TerrainCarveStress", { max_ticks = 900 });

local LINE_COUNT = 6;
local WAVE_COUNT = 5;
local WAVE_TICKS = { 150, 300, 450, 600, 750 };
local T1_BAND_X = 880;
local T2_BAND_X = 1120;
-- Subsurface sample box across the no-man's-land. Bullets and blasts chew it
-- open and the air-pixel delta measures real destruction.
local CARVE_SAMPLE = { 1000, 740, 1140, 830 };

function TestScenarioTerrainCarveStress:OnStart()
    self:MaxTeamAISkill(Activity.TEAM_1);
    self:MaxTeamAISkill(Activity.TEAM_2);
    self:SpawnSquad{ count = LINE_COUNT, x = T1_BAND_X, step = 28, y = 50, team = Activity.TEAM_1,
                     aiMode = Actor.AIMODE_SENTRY, firearm = "SMG", grenades = 4 };
    self:SpawnSquad{ count = LINE_COUNT, x = T2_BAND_X, step = 28, y = 50, team = Activity.TEAM_2,
                     aiMode = Actor.AIMODE_SENTRY, firearm = "SMG", grenades = 4 };

    self._waves = 0;
    self._airStart = self:CountAirPixels(CARVE_SAMPLE[1], CARVE_SAMPLE[2], CARVE_SAMPLE[3], CARVE_SAMPLE[4]);
    self:RecordMetric("initial_actors", #self._spawnedActors);
    self:RecordMetric("terrain_air_start", self._airStart);
end

function TestScenarioTerrainCarveStress:OnTick(tick)
    -- Reinforcements keep the firefight alive for the full run.
    for _, wt in ipairs(WAVE_TICKS) do
        if tick == wt then
            self._waves = self._waves + 1;
            self:SpawnSquad{ count = WAVE_COUNT, x = T1_BAND_X, step = 35, y = 150, team = Activity.TEAM_1,
                             aiMode = Actor.AIMODE_SENTRY, firearm = "SMG", grenades = 2 };
            self:SpawnSquad{ count = WAVE_COUNT, x = T2_BAND_X, step = 35, y = 150, team = Activity.TEAM_2,
                             aiMode = Actor.AIMODE_SENTRY, firearm = "SMG", grenades = 2 };
        end
    end

    if tick % 120 == 0 then
        local air = self:CountAirPixels(CARVE_SAMPLE[1], CARVE_SAMPLE[2], CARVE_SAMPLE[3], CARVE_SAMPLE[4]);
        self:RecordMetric("terrain_destroyed_" .. tostring(tick), air - self._airStart);
        self:RecordMetric("team1_alive_" .. tostring(tick), self:CountTeam(Activity.TEAM_1));
        self:RecordMetric("team2_alive_" .. tostring(tick), self:CountTeam(Activity.TEAM_2));
    end
    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioTerrainCarveStress:OnEnd()
    local airEnd = self:CountAirPixels(CARVE_SAMPLE[1], CARVE_SAMPLE[2], CARVE_SAMPLE[3], CARVE_SAMPLE[4]);
    local destroyed = airEnd - self._airStart;
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("final_alive_count", self:CountLivingActors());
    self:RecordMetric("waves_done", self._waves);
    self:RecordMetric("terrain_destroyed_total", destroyed);
    -- Self-check: ran full ticks, all reinforcement waves dropped, and combat-
    -- driven terrain damage genuinely happened.
    self._passed = self:Tick() >= self._maxTicks - 1
                   and self._waves == #WAVE_TICKS
                   and destroyed > 200;
end
