-- M2OsStubTest.lua — exercises the deterministic os.time / os.clock stubs. OnTick reads
-- both and folds them into a checksum-visible value. The stubs return sim-tick time, so
-- the fold is reproducible across runs.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioM2OsStubTest = Trust.Extend("M2OsStubTest", { max_ticks = 600 });

function TestScenarioM2OsStubTest:OnStart()
    self._fold = 0;
    self:RecordMetric("actor_count", 0);
end

function TestScenarioM2OsStubTest:OnTick(tick)
    local sample = math.floor((os.time() + os.clock()) * 1000);
    self._fold = (self._fold * 8191 + sample) % 2147483647;
    AIDecisionChannel:EmitWithTarget(-1, "decision", "m2_os_stub", tostring(self._fold), "", -1, 0, 0);

    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioM2OsStubTest:OnEnd()
    self._passed = true;
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("os_fold", self._fold);
end
