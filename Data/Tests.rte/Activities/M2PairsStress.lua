-- M2PairsStress.lua — heavy load on deterministic pairs() iteration.
-- Builds a 200-entry table with mixed string and integer keys and folds the
-- pairs() iteration order into a checksum-visible value every tick. CC's M2
-- work overrode Lua's randomised hash so pairs() yields keys in a fixed order
-- across runs -- divergence here means that ordering is broken.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioM2PairsStress = Trust.Extend("M2PairsStress", { max_ticks = 600 });

local TABLE_SIZE = 200;

function TestScenarioM2PairsStress:OnStart()
    self._table = {};
    for i = 1, 100 do
        self._table["key_" .. i] = i;
        self._table[i * 1000] = "val_" .. i;
    end
    self._fold = 0;
    self._iterations = 0;
    self:RecordMetric("actor_count", 0);
    self:RecordMetric("table_size", TABLE_SIZE);
end

function TestScenarioM2PairsStress:OnTick(tick)
    local fold = self._fold;
    local iters = 0;
    for k in pairs(self._table) do
        iters = iters + 1;
        local ks = tostring(k);
        for c = 1, #ks do
            fold = (fold * 31 + string.byte(ks, c)) % 2147483647;
        end
    end
    self._fold = fold;
    self._iterations = self._iterations + iters;

    if tick % 120 == 0 then
        self:RecordMetric("pairs_fold_" .. tostring(tick), fold);
    end

    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioM2PairsStress:OnEnd()
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("pairs_fold", self._fold);
    self:RecordMetric("total_iterations", self._iterations);
    local expected = TABLE_SIZE * (self._maxTicks - 1);
    -- Self-check: ran full ticks, iterated the table the expected number of
    -- times, and the fold is non-trivial.
    self._passed = self:Tick() >= self._maxTicks - 1
                   and self._iterations >= expected
                   and self._fold ~= 0;
end
