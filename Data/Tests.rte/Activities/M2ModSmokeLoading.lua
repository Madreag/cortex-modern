-- M2ModSmokeLoading.lua — smoke test: actor AI Lua running through the M2-
-- modified Lua environment (deterministic pairs, seeded math.random, os stubs).
-- Two symmetric SENTRY squads on the flat plateau exercise the combat, target-
-- selection and locomotion paths -- enough to assert the AI scripts loaded and
-- ran without errors.
--
-- NOT a strict cross-run-match test: actor AI runs on threaded Lua states,
-- still subject to the threaded-Lua race outside M2's scope. Pass only asserts
-- "the AI scripts ran and engaged".

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioM2ModSmokeLoading = Trust.Extend("M2ModSmokeLoading", { max_ticks = 600 });

local SQUAD_COUNT = 4;

function TestScenarioM2ModSmokeLoading:OnStart()
    self:MaxTeamAISkill(Activity.TEAM_1);
    self:MaxTeamAISkill(Activity.TEAM_2);
    -- Two small SENTRY lines on the flat Grasslands plateau, ~115px no-man's-land.
    self:SpawnSquad{ count = SQUAD_COUNT, x = 900, step = 35, y = 50, team = Activity.TEAM_1,
                     aiMode = Actor.AIMODE_SENTRY, firearm = "Battle Rifle", grenades = 1 };
    self:SpawnSquad{ count = SQUAD_COUNT, x = 1120, step = 35, y = 50, team = Activity.TEAM_2,
                     aiMode = Actor.AIMODE_SENTRY, firearm = "Battle Rifle", grenades = 1 };
    self:RecordMetric("actor_count", SQUAD_COUNT * 2);
end

function TestScenarioM2ModSmokeLoading:OnTick(tick)
    if tick % 120 == 0 then
        self:RecordMetric("team1_alive_" .. tostring(tick), self:CountTeam(Activity.TEAM_1));
        self:RecordMetric("team2_alive_" .. tostring(tick), self:CountTeam(Activity.TEAM_2));
    end
    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioM2ModSmokeLoading:OnEnd()
    local alive = self:CountLivingActors();
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("final_alive_count", alive);
    self:RecordMetric("team1_alive", self:CountTeam(Activity.TEAM_1));
    self:RecordMetric("team2_alive", self:CountTeam(Activity.TEAM_2));
    -- Self-check: AI Lua ran cleanly for the full duration AND the actors
    -- actually engaged (a Lua-broken AI would idle and nobody would die).
    self._passed = self:Tick() >= self._maxTicks - 1
                   and alive < SQUAD_COUNT * 2;
end
