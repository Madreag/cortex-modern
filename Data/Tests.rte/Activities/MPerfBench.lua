-- MPerfBench: heavy compute-bound performance-measurement scenario.
-- One hundred-fifty BRAINHUNT actors armed with SMGs and frag grenades open
-- the run, with six reinforcement waves dropping in over the 1200 ticks. The
-- goal is that one sim tick's compute consistently exceeds the real-time frame
-- budget so the sim is compute-bound for most of the run -- only then does
-- __wall_seconds reflect the build's actual per-tick sim cost rather than
-- TimerMan's idle-sleep. Run the same scenario+seed on each build
-- and compare __wall_seconds for the regression.
--
-- A one-shot brawl collapses to a handful in ~240 ticks, after which the sim
-- is far below capacity and __wall_seconds is mostly idle sleep -- useless as
-- a perf signal. Sustained heavy population is the whole point.
--
-- Not a determinism scenario.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioMPerfBench = Trust.Extend("MPerfBench", { max_ticks = 1200 });

local PERF_COLUMNS = 30;
local PERF_ROWS    = 5;
local X_START      = 400;
local X_STEP       = 28;
local WAVE_COUNT   = 30;
local WAVE_TICKS   = { 180, 360, 540, 720, 900, 1080 };

local function spawnGrid(self, x_start, rows, y_offset)
    local n = 0;
    for row = 0, rows - 1 do
        for col = 0, PERF_COLUMNS - 1 do
            local x = x_start + col * X_STEP + row * 8;
            local y = 50 + row * 50 + y_offset;
            local team = (col % 2 == 0) and Activity.TEAM_1 or Activity.TEAM_2;
            local a = self:SpawnActor("Green Dummy", "Base.rte", x, y, team, Actor.AIMODE_BRAINHUNT);
            if a then
                self:GiveFirearm(a, "SMG");
                self:GiveGrenades(a, "Frag Grenade", 1);
                n = n + 1;
            end
        end
    end
    return n;
end

function TestScenarioMPerfBench:OnStart()
    self:MaxTeamAISkill(Activity.TEAM_1);
    self:MaxTeamAISkill(Activity.TEAM_2);
    local n = spawnGrid(self, X_START, PERF_ROWS, 0);
    self._waves = 0;
    self:RecordMetric("perf_actor_count", n);
end

function TestScenarioMPerfBench:OnTick(tick)
    -- Reinforcement waves: a full row of WAVE_COUNT armed BRAINHUNT actors
    -- every ~180 ticks so population stays heavy.
    for _, wt in ipairs(WAVE_TICKS) do
        if tick == wt then
            self._waves = self._waves + 1;
            for col = 0, WAVE_COUNT - 1 do
                local x = X_START + col * X_STEP + 14;
                local team = (col % 2 == 0) and Activity.TEAM_1 or Activity.TEAM_2;
                local a = self:SpawnActor("Green Dummy", "Base.rte", x, 280, team, Actor.AIMODE_BRAINHUNT);
                if a then
                    self:GiveFirearm(a, "SMG");
                    self:GiveGrenades(a, "Frag Grenade", 1);
                end
            end
        end
    end

    if tick % 240 == 0 then
        self:RecordMetric("alive_at_" .. tostring(tick), self:CountLivingActors());
    end
    if tick >= self._maxTicks - 1 then
        return true, true;
    end
    return false, false;
end

function TestScenarioMPerfBench:OnEnd()
    local spawned = #self._spawnedActors;
    local alive = self:CountLivingActors();
    self:RecordMetric("final_tick", self:Tick());
    self:RecordMetric("final_alive_count", alive);
    self:RecordMetric("waves_done", self._waves);
    self:RecordMetric("total_spawned", spawned);
    -- Self-check: ran the full duration, all waves dropped, and the scenario
    -- structure spawned its full population. Whether the sim was compute-bound
    -- is read off __wall_seconds vs sim seconds in the JSON -- not gated here
    -- because os.clock is stubbed to sim time and we cannot read wall from Lua.
    self._passed = self:Tick() >= self._maxTicks - 1
                   and self._waves == #WAVE_TICKS
                   and spawned >= 250;
end
