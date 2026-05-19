-- M1Baseline.lua — Block A (M1): minimal repeatable scenario used by the determinism
-- CI scaffold (cccp-determinism-check). The aim is *not* a pass/fail test in the
-- AI-trust sense — it is to exercise the sim with as few moving parts as possible
-- so the per-tick BLAKE3 hash trace converges to zero divergence as the M1 blocks
-- (B-F) land.
--
-- Design notes:
--   * Single Green Dummy in SENTRY mode. SENTRY = no patrolling / no random pathing,
--     so any divergence across runs must come from RNG, iteration-order, wall-clock,
--     or FP — exactly the four classes the M1 blocks remove.
--   * Deterministic spawn position. No CreateRandom*, no random factions, no
--     procedural terrain.
--   * Tutorial Bunker scene (same as the AI-NN trust scenarios) so we reuse a known
--     scene rather than introducing M1-specific terrain churn.
--   * Default max_ticks = 600 (10 sim-seconds at 60 Hz). cccp-determinism-check
--     can override via -ticks.
--   * `passed` is set to true on reaching max_ticks. The point of the scenario is
--     to produce a stable hash trace, not to grade behaviour.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioM1Baseline = Trust.Extend("M1Baseline", { max_ticks = 600 });

function TestScenarioM1Baseline:OnStart()
    -- One sentry. Deterministic Y=50 spawn (per TrustScenario convention: the actor
    -- drops onto Tutorial Bunker's roof at Y=176, no spawn-inside-wall risk).
    self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
    self:RecordMetric("actor_count", 1);
end

function TestScenarioM1Baseline:OnTick(tick)
    -- Reaching max_ticks is the scenario's "pass" condition for determinism purposes.
    -- Returning done=true,passed=true *just before* the Trust base class's hard cap
    -- avoids the cap-triggered fail branch in TrustScenario.UpdateActivity.
    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioM1Baseline:OnEnd()
    self._passed = true;
    self:RecordMetric("final_tick", self:Tick());
end
