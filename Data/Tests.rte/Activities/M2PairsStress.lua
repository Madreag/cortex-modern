-- M2PairsStress.lua — exercises deterministic pairs(). OnTick iterates a fixed table of
-- mixed string and integer keys and folds the iteration order into a checksum-visible
-- value. No actors, so iteration stays on the main thread.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioM2PairsStress = Trust.Extend("M2PairsStress", { max_ticks = 600 });

function TestScenarioM2PairsStress:OnStart()
    self._table = {};
    for i = 1, 100 do
        self._table["key_" .. i] = i;
        self._table[i * 1000] = "val_" .. i;
    end
    self._fold = 0;
    self:RecordMetric("actor_count", 0);
end

function TestScenarioM2PairsStress:OnTick(tick)
    local fold = self._fold;
    for k in pairs(self._table) do
        local ks = tostring(k);
        for c = 1, #ks do
            fold = (fold * 31 + string.byte(ks, c)) % 2147483647;
        end
    end
    self._fold = fold;
    AIDecisionChannel:EmitWithTarget(-1, "decision", "m2_pairs_order", tostring(fold), "", -1, 0, 0);

    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioM2PairsStress:OnEnd()
    self._passed = true;
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("pairs_fold", self._fold);
end
