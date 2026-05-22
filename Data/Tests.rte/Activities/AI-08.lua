-- AI-08: Hold formation through a bunker.
-- Spawn a leader + 2 squadmates. Pass: all three alive AND the leader genuinely
-- travelled >= 250px from its landed start AND every sampled second (>= 4) had
-- a mean member-to-leader distance <= 120. The travel requirement closes the
-- "passes by standing still in formation" loophole. Result locks in OnTick.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI08 = Trust.Extend("AI-08", { max_ticks = 1200 });

function TestScenarioAI08:OnStart()
    local leader = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_GOTO);
    if leader then
        leader:AddAISceneWaypoint(Vector(1500, 200));
    end
    self._leader = leader;
    self._squad = {
        self:SpawnActor("Green Dummy", "Base.rte", 920, 50, Activity.TEAM_1, Actor.AIMODE_SQUAD),
        self:SpawnActor("Green Dummy", "Base.rte", 980, 50, Activity.TEAM_1, Actor.AIMODE_SQUAD),
    };
    self._leaderStart = nil;
    self._samples = 0;
    self._allTight = true;
end

function TestScenarioAI08:OnTick(tick)
    local leader = self._leader;
    if not leader or not MovableMan:IsActor(leader) then
        return true, false;
    end

    -- Capture the leader's landed start once the fall has settled.
    if tick == 90 and not self._leaderStart then
        self._leaderStart = Vector(leader.Pos.X, leader.Pos.Y);
    end

    -- Self-test: genuine formation movement -- advance the leader and snap both
    -- squadmates to its flanks each tick. ~250px in ~85 ticks.
    if tick >= 95 and self._selfTest then
        leader.Pos = Vector(leader.Pos.X + 3, leader.Pos.Y);
        for i, m in ipairs(self._squad) do
            if m and MovableMan:IsActor(m) then
                m.Pos = Vector(leader.Pos.X + (i == 1 and -25 or 25), leader.Pos.Y);
            end
        end
    end

    if tick > 90 and self._leaderStart and tick % 30 == 0 then
        local total = 0;
        local count = 0;
        for _, m in ipairs(self._squad) do
            if m and MovableMan:IsActor(m) then
                local dx = m.Pos.X - leader.Pos.X;
                local dy = m.Pos.Y - leader.Pos.Y;
                total = total + math.sqrt(dx * dx + dy * dy);
                count = count + 1;
            end
        end
        local mean = count > 0 and (total / count) or 1e9;
        self:RecordMetric("squad_mean_dist", mean);
        self._samples = self._samples + 1;
        if mean > 120 then
            self._allTight = false;
        end

        local ldx = leader.Pos.X - self._leaderStart.X;
        local ldy = leader.Pos.Y - self._leaderStart.Y;
        local travel = math.sqrt(ldx * ldx + ldy * ldy);
        self:RecordMetric("leader_travel", travel);

        local allAlive = true;
        for _, m in ipairs(self._squad) do
            if not m or not MovableMan:IsActor(m) then allAlive = false; end
        end

        if travel >= 250 and self._allTight and self._samples >= 4 and allAlive then
            self:RecordMetric("formation_tick", tick);
            return true, true;
        end
    end
    if tick > 450 then
        return true, false;
    end
    return false, false;
end

function TestScenarioAI08:OnEnd()
end
