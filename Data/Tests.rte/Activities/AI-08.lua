-- AI-08: Hold formation through a bunker (squad).
-- Spawn a leader + 2 squadmates. Pass if mean pairwise distance stays under
-- a threshold for >= 50% of sampled seconds.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI08 = Trust.Extend("AI-08", { max_ticks = 1200 });

function TestScenarioAI08:OnStart()
    local leader = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_GOTO);
    if leader then
        leader:AddAISceneWaypoint(Vector(1400, 200));
    end
    local s1 = self:SpawnActor("Green Dummy", "Base.rte", 920, 50, Activity.TEAM_1, Actor.AIMODE_SQUAD);
    local s2 = self:SpawnActor("Green Dummy", "Base.rte", 980, 50, Activity.TEAM_1, Actor.AIMODE_SQUAD);
    self._leader = leader;
    self._squad = { s1, s2 };
    self._tightSeconds = 0;
    self._totalSeconds = 0;
end

function TestScenarioAI08:OnTick(tick)
    if tick % 60 ~= 0 then return false, false; end

    local leader = self._leader;
    if not leader or not MovableMan:IsActor(leader) then
        return true, false;
    end

    local total = 0;
    local pairs_count = 0;
    for _, m in ipairs(self._squad) do
        if m and MovableMan:IsActor(m) then
            local dx = m.Pos.X - leader.Pos.X;
            local dy = m.Pos.Y - leader.Pos.Y;
            total = total + math.sqrt(dx*dx + dy*dy);
            pairs_count = pairs_count + 1;
        end
    end
    local mean = pairs_count > 0 and (total / pairs_count) or 1e9;
    self:RecordMetric("squad_mean_dist", mean);
    self._totalSeconds = self._totalSeconds + 1;
    if mean <= 100 then
        self._tightSeconds = self._tightSeconds + 1;
    end
    return false, false;
end

function TestScenarioAI08:OnEnd()
    self:RecordMetric("tight_seconds", self._tightSeconds);
    self:RecordMetric("total_seconds", self._totalSeconds);
    if self._totalSeconds > 0 then
        local ratio = self._tightSeconds / self._totalSeconds;
        self:RecordMetric("tight_ratio", ratio);
        self._passed = ratio >= 0.5;
    else
        self._passed = false;
    end
end
