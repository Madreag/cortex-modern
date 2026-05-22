-- MPerfBench: heavy compute-bound performance-measurement scenario.
--
-- Spawns a large brain-hunting melee (120 actors) so one sim tick's compute far
-- exceeds the real-time frame budget. The headless sim is real-time-paced
-- (while TimerMan::TimeForSimUpdate()), so a light scenario finishes in
-- ~sim-duration regardless of build speed; this scenario is heavy enough that
-- the machine cannot keep real-time, so the run is compute-bound and
-- __wall_seconds reflects the build's actual per-tick sim cost. Run the same
-- scenario+seed on each milestone build and compare __wall_seconds for the
-- per-milestone performance regression. Not a determinism scenario.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioMPerfBench = Trust.Extend("MPerfBench", { max_ticks = 1200 });

function TestScenarioMPerfBench:OnStart()
    local n = 0;
    for row = 0, 7 do
        for col = 0, 14 do
            local team = (col % 2 == 0) and Activity.TEAM_1 or Activity.TEAM_2;
            local x = 300 + col * 90;
            local y = 50 + row * 22;
            if self:SpawnActor("Green Dummy", "Base.rte", x, y, team, Actor.AIMODE_BRAINHUNT) then
                n = n + 1;
            end
        end
    end
    self:RecordMetric("perf_actor_count", n);
end

function TestScenarioMPerfBench:OnTick(tick)
    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioMPerfBench:OnEnd()
    self._passed = true;
    self:RecordMetric("final_alive_count", self:CountLivingActors());
end
