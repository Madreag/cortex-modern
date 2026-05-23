-- M2OsStubTest.lua — exercises the deterministic os.time / os.clock stubs.
-- The M2 work replaced Lua's wall-clock os.* with stubs that return sim-tick
-- time, so two same-seed runs see identical timestamps. Each tick samples both
-- stubs and folds them; OnEnd confirms the stubs actually advanced (a static
-- or wall-clock os.time would either not move or move non-monotonically).

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioM2OsStubTest = Trust.Extend("M2OsStubTest", { max_ticks = 600 });

function TestScenarioM2OsStubTest:OnStart()
    self._fold = 0;
    self._startTime = nil;
    self._startClock = nil;
    self._endTime = nil;
    self._endClock = nil;
    self:RecordMetric("actor_count", 0);
end

function TestScenarioM2OsStubTest:OnTick(tick)
    local t = os.time();
    local c = os.clock();
    if self._startTime == nil then
        self._startTime = t;
        self._startClock = c;
    end
    self._endTime = t;
    self._endClock = c;

    local sample = math.floor((t + c) * 1000);
    self._fold = (self._fold * 8191 + sample) % 2147483647;
    AIDecisionChannel:EmitWithTarget(-1, "decision", "m2_os_stub", tostring(self._fold), "", -1, 0, 0);

    if tick % 120 == 0 then
        self:RecordMetric("os_time_" .. tostring(tick), t);
        self:RecordMetric("os_clock_" .. tostring(tick), c);
    end

    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioM2OsStubTest:OnEnd()
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("os_fold", self._fold);
    self:RecordMetric("os_time_start", self._startTime or 0);
    self:RecordMetric("os_time_end", self._endTime or 0);
    self:RecordMetric("os_clock_start", self._startClock or 0);
    self:RecordMetric("os_clock_end", self._endClock or 0);
    -- Self-check: stubs returned values, the fold is non-trivial, and the
    -- sim-tick os.time stub advanced over the run.
    self._passed = self:Tick() >= self._maxTicks - 1
                   and self._fold ~= 0
                   and self._startTime ~= nil
                   and self._endTime > self._startTime
                   and self._endClock >= self._startClock;
end
