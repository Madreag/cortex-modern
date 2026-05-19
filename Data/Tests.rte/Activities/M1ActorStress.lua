-- M1ActorStress.lua — Block F (M1) determinism scenario: exercise the
-- stable-iteration sort path from Block C with a population large enough
-- that any sort instability shows up tick-by-tick.
--
-- Design notes:
--   * Spawns 20 Green Dummies in SENTRY mode arranged on a deterministic
--     grid above the Tutorial Bunker roof. The number is bounded by the
--     scene size (we don't want spawn-on-wall failures) but large enough
--     that std::sort's behaviour matters: 20 elements gives plenty of
--     comparator calls per frame.
--   * SENTRY mode — no random patrolling, no random target selection. Every
--     non-determinism risk is structural (RNG, iteration, wall-clock, FP).
--   * 600 ticks (10 sim-seconds). Block C's MOID sort runs three times per
--     tick (m_Actors / m_Items / m_Particles); the actor population stays
--     near 20 throughout, so we get ~36k comparator invocations across the
--     run.
--   * Pass = reach max_ticks.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioM1ActorStress = Trust.Extend("M1ActorStress", { max_ticks = 600 });

local SPAWN_GRID_X_START = 700;
local SPAWN_GRID_X_STEP  = 60;
local SPAWN_COUNT        = 20;

function TestScenarioM1ActorStress:OnStart()
    for i = 0, SPAWN_COUNT - 1 do
        local x = SPAWN_GRID_X_START + i * SPAWN_GRID_X_STEP;
        local team = (i % 2 == 0) and Activity.TEAM_1 or Activity.TEAM_2;
        self:SpawnActor("Green Dummy", "Base.rte", x, 50, team, Actor.AIMODE_SENTRY);
    end
    self:RecordMetric("actor_count", SPAWN_COUNT);
end

function TestScenarioM1ActorStress:OnTick(tick)
    if tick % 120 == 0 then
        self:RecordMetric("alive_at_tick_" .. tostring(tick), self:CountLivingActors());
    end
    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioM1ActorStress:OnEnd()
    self._passed = true;
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("final_alive_count", self:CountLivingActors());
end
