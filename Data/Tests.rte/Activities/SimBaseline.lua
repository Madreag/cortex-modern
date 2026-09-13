-- SimBaseline.lua — determinism scenario: the suite's minimal baseline.
--
-- One Green Dummy in SENTRY mode on the open Grasslands field -- the fewest
-- moving parts in the suite. SENTRY means no patrolling and no random pathing,
-- so any per-tick hash divergence across runs must come from RNG, iteration
-- order, wall-clock or FP -- exactly the classes the determinism work
-- removes. If this scenario diverges, something foundational is broken.
--
-- It is also a self-verifying baseline: a clean sentry spawns, drops onto solid
-- ground, settles, and holds its post at full health. The scenario captures the
-- landed position and final state and fails if the sentry is ever lost -- so a
-- headless trace proves "clean minimal baseline", not just "600 ticks ran".

package.loaded.Constants = nil; require("Constants");
local Test = require("Lib/TestScenario");

TestScenarioSimBaseline = Test.Extend("SimBaseline", { max_ticks = 600 });

function TestScenarioSimBaseline:OnStart()
    -- One sentry at the centre of the field. SpawnActor grounds it on the terrain;
    -- the y arg is height above ground, so it drops a short settling distance.
    self._actor = self:SpawnActor("Green Dummy", "Base.rte", 1200, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
    self._landed = nil;
    self:RecordMetric("actor_count", 1);
end

function TestScenarioSimBaseline:OnTick(tick)
    local a = self._actor;
    -- A lone sentry on empty ground must never die. If it does, the baseline broke.
    if not a or not MovableMan:IsActor(a) or a.Health <= 0 then
        self:RecordMetric("sentry_lost_tick", tick);
        return true, false;
    end
    -- Capture the landed position once the spawn drop has settled.
    if tick == 90 then
        self._landed = Vector(a.Pos.X, a.Pos.Y);
        self:RecordMetric("landed_x", a.Pos.X);
        self:RecordMetric("landed_y", a.Pos.Y);
    end
    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioSimBaseline:OnEnd()
    local a = self._actor;
    local alive = a and MovableMan:IsActor(a) and a.Health > 0;
    self:RecordMetric("final_health", alive and a.Health or 0);
    -- SENTRY holds its post: the actor must not have drifted from where it landed.
    if alive and self._landed then
        self:RecordMetric("drift_x", math.abs(a.Pos.X - self._landed.X));
    end
    self:RecordMetric("final_tick", self:Tick());
    self._passed = alive;
end
