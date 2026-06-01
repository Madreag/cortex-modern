-- ThreadStress.lua — determinism scenario: saturate the threaded sim
-- paths so the per-tick hash trace has a demanding workload to compare ACROSS
-- thread counts (the determinism-check `--threads 1,2,4,8,16` matrix).
--
-- Forty Green Dummies in BRAINHUNT mode in two interleaved rows -- the count
-- is well above the 16-state ceiling so every Lua state gets multiple actors
-- and the ThreadedUpdate / ThreadedUpdateAI parallel passes are saturated at
-- every thread count. SMGs and frag grenades load the device hooks; BRAINHUNT
-- keeps the AI pathing and firing every tick. Attrition is intentional -- gibs
-- and dropped inventory exercise the threaded-Lua -> g_SimRNG consumers
-- (GibThis, DropAllInventory) that the per-worker RNG must cover.
--
-- Reinforcement waves keep the population well above the 16-thread ceiling for
-- the full 900 ticks. A one-shot 40-actor brawl wipes to ~8 in 240 ticks and
-- under-saturates the high thread counts for the remaining 660 ticks.
--
-- Spawned across x=420..1200 to stay on usable Grasslands terrain (avoiding
-- the right-side mountain and the chasm at x≈1300).

package.loaded.Constants = nil; require("Constants");
local Test = require("Lib/TestScenario");

TestScenarioThreadStress = Test.Extend("ThreadStress", { max_ticks = 900 });

local SPAWN_COLUMNS = 20;
local SPAWN_ROWS    = 2;
local X_START       = 420;
local X_STEP        = 40;
local ROW_Y         = { 50, 130 };
local WAVE_COUNT    = 16;
local WAVE_TICKS    = { 220, 440, 660 };

local function spawnRows(self, rows, x_start, y_offset)
    local count = 0;
    for row = 0, rows - 1 do
        for col = 0, SPAWN_COLUMNS - 1 do
            local x = x_start + col * X_STEP + row * 20;
            local y = ROW_Y[row + 1] + y_offset;
            local team = (col % 2 == 0) and Activity.TEAM_1 or Activity.TEAM_2;
            local a = self:SpawnActor("Green Dummy", "Base.rte", x, y, team, Actor.AIMODE_BRAINHUNT);
            if a then
                self:GiveFirearm(a, "SMG");
                self:GiveGrenades(a, "Frag Grenade", 1);
                count = count + 1;
            end
        end
    end
    return count;
end

function TestScenarioThreadStress:OnStart()
    self:MaxTeamAISkill(Activity.TEAM_1);
    self:MaxTeamAISkill(Activity.TEAM_2);
    local count = spawnRows(self, SPAWN_ROWS, X_START, 0);
    self._waves = 0;
    self:RecordMetric("actor_count", count);
end

function TestScenarioThreadStress:OnTick(tick)
    -- Reinforcement waves: drop another batch of armed BRAINHUNT actors so the
    -- thread pool stays saturated across the full run.
    for _, wt in ipairs(WAVE_TICKS) do
        if tick == wt then
            self._waves = self._waves + 1;
            -- One row of WAVE_COUNT interleaved actors, dropped from height.
            for col = 0, WAVE_COUNT - 1 do
                local x = X_START + col * X_STEP + 10;
                local team = (col % 2 == 0) and Activity.TEAM_1 or Activity.TEAM_2;
                local a = self:SpawnActor("Green Dummy", "Base.rte", x, 200, team, Actor.AIMODE_BRAINHUNT);
                if a then
                    self:GiveFirearm(a, "SMG");
                    self:GiveGrenades(a, "Frag Grenade", 1);
                end
            end
        end
    end

    if tick % 120 == 0 then
        self:RecordMetric("team1_alive_" .. tostring(tick), self:CountTeam(Activity.TEAM_1));
        self:RecordMetric("team2_alive_" .. tostring(tick), self:CountTeam(Activity.TEAM_2));
        self:RecordMetric("alive_total_" .. tostring(tick), self:CountLivingActors());
    end
    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioThreadStress:OnEnd()
    local spawned = #self._spawnedActors;
    local alive = self:CountLivingActors();
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("total_spawned", spawned);
    self:RecordMetric("final_alive_count", alive);
    self:RecordMetric("waves_done", self._waves);
    self:RecordMetric("team1_alive", self:CountTeam(Activity.TEAM_1));
    self:RecordMetric("team2_alive", self:CountTeam(Activity.TEAM_2));
    -- Self-check: ran full duration, all waves dropped, and heavy attrition
    -- happened (real threaded AI + gib + RNG work, not idle ticks).
    self._passed = self:Tick() >= self._maxTicks - 1
                   and self._waves == #WAVE_TICKS
                   and (spawned - alive) >= 20;
end
