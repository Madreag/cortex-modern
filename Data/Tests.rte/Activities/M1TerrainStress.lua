-- M1TerrainStress.lua — determinism scenario: heavy, sustained terrain
-- destruction. Drives the `terrain` and `carve_math` subsystems of the per-tick
-- BLAKE3 trace hard, so any divergence in the destruction path is caught.
--
-- Two seven-actor squads form spread-out SENTRY lines facing each other across
-- a ~300px no-man's-land, armed with rocket launchers and frag grenades. SENTRY
-- holds the line, and because both sides are stationary the rockets actually
-- connect -- a genuine sustained firefight that craters the whole no-man's-land.
-- A scripted, terrain-aware carve cascade marches across the field alongside the
-- fight so the terrain workload is heavy and guaranteed, not left to the battle.
--
-- Determinism: every spawn, loadout and carve is fixed; the only randomness is
-- the sim's seeded RNG. Reproducibility under sustained destruction is the test.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioM1TerrainStress = Trust.Extend("M1TerrainStress", { max_ticks = 900 });

-- Sub-surface sample box in the no-man's-land. Air pixels here start near zero
-- (solid terrain) and climb as the firefight and the cascade chew it open.
local CARVE_SAMPLE = { 1150, 770, 1350, 880 };

function TestScenarioM1TerrainStress:OnStart()
    self:MaxTeamAISkill(Activity.TEAM_1);
    self:MaxTeamAISkill(Activity.TEAM_2);

    -- Two spread lines, 100px between actors, ~300px no-man's-land between lines.
    self:SpawnSquad{ count = 7, x = 500, step = 100, y = 50, team = Activity.TEAM_1,
                     aiMode = Actor.AIMODE_SENTRY, firearm = "Rocket Launcher", grenades = 4 };
    self:SpawnSquad{ count = 7, x = 1400, step = 100, y = 50, team = Activity.TEAM_2,
                     aiMode = Actor.AIMODE_SENTRY, firearm = "Rocket Launcher", grenades = 4 };

    self:RecordMetric("actor_count", 14);
    self._carves = 0;
    self._airStart = self:CountAirPixels(CARVE_SAMPLE[1], CARVE_SAMPLE[2], CARVE_SAMPLE[3], CARVE_SAMPLE[4]);
    self:RecordMetric("terrain_air_start", self._airStart);
end

function TestScenarioM1TerrainStress:OnTick(tick)
    -- Terrain-aware carve cascade: marches a crater box across the battlefield,
    -- always straddling the actual ground surface so it removes real terrain.
    if tick >= 60 and tick % 10 == 0 then
        local s = math.floor(tick / 10);
        local cx = 700 + (s * 51) % 1200;
        local g = SceneMan:MovePointToGround(Vector(cx, 0), 20, 10).Y;
        self:CarveBox(cx, g - 25, cx + 80, g + 110);
        self._carves = self._carves + 1;
    end

    -- Sample the battle: per-team survivors and terrain destroyed.
    if tick % 120 == 0 then
        self:RecordMetric("team1_alive_" .. tostring(tick), self:CountTeam(Activity.TEAM_1));
        self:RecordMetric("team2_alive_" .. tostring(tick), self:CountTeam(Activity.TEAM_2));
        local air = self:CountAirPixels(CARVE_SAMPLE[1], CARVE_SAMPLE[2], CARVE_SAMPLE[3], CARVE_SAMPLE[4]);
        self:RecordMetric("terrain_destroyed_" .. tostring(tick), air - self._airStart);
        self:RecordMetric("carves_done_" .. tostring(tick), self._carves);
    end

    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioM1TerrainStress:OnEnd()
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("final_alive_count", self:CountLivingActors());
    self:RecordMetric("carves_done_total", self._carves);
    local air = self:CountAirPixels(CARVE_SAMPLE[1], CARVE_SAMPLE[2], CARVE_SAMPLE[3], CARVE_SAMPLE[4]);
    local destroyed = air - self._airStart;
    self:RecordMetric("terrain_destroyed_total", destroyed);
    -- Self-check: the scenario did its job only if it genuinely carved heavy
    -- terrain AND both squads actually fought (each took losses).
    self._passed = destroyed > 3000
                   and self:CountTeam(Activity.TEAM_1) < 7
                   and self:CountTeam(Activity.TEAM_2) < 7;
end
