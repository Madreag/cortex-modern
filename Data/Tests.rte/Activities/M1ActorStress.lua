-- M1ActorStress.lua — determinism scenario: large, churning population.
-- The concern is stable MOID-ordered iteration -- m_Actors / m_Items /
-- m_Particles are re-sorted every tick. This scenario keeps those containers big
-- AND continuously churning: a dense infantry battle on the flat plateau, fed by
-- scripted reinforcement waves, so the actor-add path (MOID assignment) and the
-- per-tick sort run under a live, already-populated list -- a far harder
-- iteration-order test than a one-shot spawn of idle sentries.
--
-- Grasslands is hilly: a valley on the left, a mountain on the right, and one
-- flat plateau around x=1070. The whole fight is staged on that plateau across a
-- tight 100px no-man's-land, so line-of-sight and Battle Rifle range are
-- guaranteed and the firefight is real. Five reinforcement waves keep the
-- population high and the plateau a churning meat-grinder for the full run.
--
-- Determinism: every spawn, wave tick, position and loadout is fixed; the only
-- randomness is the sim's seeded RNG. Reproducibility under a large, churning
-- actor population is the test.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioM1ActorStress = Trust.Extend("M1ActorStress", { max_ticks = 900 });

-- Opening line size and reinforcement wave size, per side, plus the ticks the
-- waves drop on. 6 + 5*5 = 31 actors per side, 62 over the run.
local LINE_COUNT = 6;
local WAVE_COUNT = 5;
local WAVE_TICKS = { 150, 300, 450, 600, 750 };

-- Plateau geometry: two firing lines either side of a 100px no-man's-land,
-- every actor on the flat ground. Waves drop along the same two bands.
local T1_BAND_X = 880;
local T2_BAND_X = 1120;
local LINE_STEP = 28;
local WAVE_STEP = 35;

function TestScenarioM1ActorStress:OnStart()
    self:MaxTeamAISkill(Activity.TEAM_1);
    self:MaxTeamAISkill(Activity.TEAM_2);

    -- Two facing SENTRY lines on the plateau, 100px no-man's-land between them.
    self:SpawnSquad{ count = LINE_COUNT, x = T1_BAND_X, step = LINE_STEP, y = 50, team = Activity.TEAM_1,
                     aiMode = Actor.AIMODE_SENTRY, firearm = "Battle Rifle", grenades = 2 };
    self:SpawnSquad{ count = LINE_COUNT, x = T2_BAND_X, step = LINE_STEP, y = 50, team = Activity.TEAM_2,
                     aiMode = Actor.AIMODE_SENTRY, firearm = "Battle Rifle", grenades = 2 };

    self._waves = 0;
    self._peakPop = self:CountLivingActors();
    self:RecordMetric("initial_actors", #self._spawnedActors);

    -- Terrain profile across the plateau -- confirms the fight is on flat ground.
    for px = 800, 1320, 40 do
        self:RecordMetric("g_" .. tostring(px),
                          SceneMan:MovePointToGround(Vector(px, 0), 20, 10).Y);
    end
end

function TestScenarioM1ActorStress:OnTick(tick)
    -- Reinforcement waves: a fresh squad drops into each line on a fixed cadence,
    -- exercising the actor-add path and MOID sort under an already-churning list.
    for _, wt in ipairs(WAVE_TICKS) do
        if tick == wt then
            self._waves = self._waves + 1;
            self:SpawnSquad{ count = WAVE_COUNT, x = T1_BAND_X, step = WAVE_STEP, y = 150, team = Activity.TEAM_1,
                             aiMode = Actor.AIMODE_SENTRY, firearm = "Battle Rifle", grenades = 1 };
            self:SpawnSquad{ count = WAVE_COUNT, x = T2_BAND_X, step = WAVE_STEP, y = 150, team = Activity.TEAM_2,
                             aiMode = Actor.AIMODE_SENTRY, firearm = "Battle Rifle", grenades = 1 };
        end
    end

    -- Track the high-water population and sample the battle per team.
    local alive = self:CountLivingActors();
    if alive > self._peakPop then self._peakPop = alive; end
    if tick % 120 == 0 then
        self:RecordMetric("team1_alive_" .. tostring(tick), self:CountTeam(Activity.TEAM_1));
        self:RecordMetric("team2_alive_" .. tostring(tick), self:CountTeam(Activity.TEAM_2));
        self:RecordMetric("alive_total_" .. tostring(tick), alive);
    end

    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioM1ActorStress:OnEnd()
    local spawned = #self._spawnedActors;
    local alive = self:CountLivingActors();
    local deaths = spawned - alive;
    local perTeam = LINE_COUNT + WAVE_COUNT * #WAVE_TICKS;
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("total_spawned", spawned);
    self:RecordMetric("final_alive_count", alive);
    self:RecordMetric("peak_population", self._peakPop);
    self:RecordMetric("total_deaths", deaths);
    self:RecordMetric("waves_done", self._waves);
    -- Self-check: the iteration path was genuinely stressed only if a big,
    -- churning population ran -- all waves in, a high peak, heavy attrition,
    -- and both lines bleeding.
    self._passed = self._waves == #WAVE_TICKS
                   and self._peakPop >= 20
                   and deaths >= 20
                   and self:CountTeam(Activity.TEAM_1) <= perTeam - 8
                   and self:CountTeam(Activity.TEAM_2) <= perTeam - 8;
end
