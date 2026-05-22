-- TrustScenario.lua
--
-- Shared base class for the M0 AI-01..AI-12 trust scenarios. Each scenario in
-- Tests.rte/Activities/AI-NN.lua extends this with its own setup, per-tick
-- observation, and pass/fail criteria.
--
-- Pattern:
--   local Trust = require("Lib/TrustScenario");
--   TestScenarioAI01 = Trust.Extend("AI-01", { max_ticks = 1800, ... });
--   function TestScenarioAI01:OnStart()      -- spawn actors, set initial state
--   function TestScenarioAI01:OnTick(tick)   -- per-tick observation; return done/passed
--   function TestScenarioAI01:OnEnd()        -- record final metrics
--
-- The base class wires the 9-callback GAScripted contract to these three hooks,
-- and ensures MetricsCollector + SimChecksum interactions stay consistent.

local TrustScenario = {};

-- Default safe spawn point in Tutorial Bunker:
--   Y = 50 puts actors in the open sky well above the bunker roof (Y=176),
--   so they fall briefly and land on solid roof terrain rather than spawning
--   inside a wall (the cause of "actors die instantly" in the early M0 runs).
-- Scenarios can override via the second arg to SpawnActor.
TrustScenario.DEFAULT_SPAWN_X = 950;
TrustScenario.DEFAULT_SPAWN_Y = 50;

function TrustScenario.Extend(scenarioName, defaults)
    local cls = {};
    cls.__index = cls;
    cls._scenarioName = scenarioName;
    cls._defaults = defaults or {};

    -- GAScripted callbacks. The activity instance (`self`) is the Lua "class" defined by the script,
    -- so we attach the standard callbacks on the class and route through Trust hooks.

    function cls:StartActivity()
        self._tick = 0;
        self._maxTicks = self._defaults.max_ticks or 1800; -- default 30 sim-seconds
        self._passed = false;
        self._scenarioFinished = false;
        self._finalized = false;
        self._spawnedActors = {};
        self._metrics = {};

        -- Positive-control mode (-trust-selftest): the scenario drives its own named
        -- behaviour so the grading criterion can be demonstrated to pass. Without it,
        -- the scenario is the genuine AI test and the stock AI is on its own.
        self._selfTest = MetricsCollector:IsSelfTest();

        -- Arm MetricsCollector for this run. The seed defaults to 0 here; the C++
        -- side substitutes the CLI -seed value when ScenarioRunner is active, so
        -- the report records the seed actually used.
        MetricsCollector:BeginRun(scenarioName, 0);
        MetricsCollector:RecordString("scenario", scenarioName);

        -- Trust scenarios are developer-facing: auto-enable the AI decision overlay so the
        -- user can watch the AI think while the scenario plays.
        AIDebugOverlay.Enabled = true;

        -- Emit a scenario-start decision so the report always has something to count.
        AIDecisionChannel:EmitWithTarget(-1, "decision", "scenario_start", scenarioName, "begin", -1, 0, 0);

        if type(self.OnStart) == "function" then
            self:OnStart();
        end
    end

    function cls:UpdateActivity()
        if self._finalized then
            -- Once finalized, every subsequent tick is a no-op. The C++ outer-loop
            -- guard in RunGameLoop catches ActivityState=OVER and exits cleanly,
            -- so this just prevents the finalize block from re-firing 1000+ times
            -- in the gap between OVER being set and the guard kicking us out.
            return;
        end

        self._tick = self._tick + 1;

        if not self._scenarioFinished and type(self.OnTick) == "function" then
            local done, passed = self:OnTick(self._tick);
            if done then
                self._scenarioFinished = true;
                self._passed = passed and true or false;
            end
        end

        if self._tick >= self._maxTicks and not self._scenarioFinished then
            self._scenarioFinished = true;
            -- Default: not finishing within max_ticks is a fail.
            self._passed = false;
            MetricsCollector:Record("hit_max_ticks", 1);
        end

        if self._scenarioFinished and not self._finalized then
            self._finalized = true;
            if type(self.OnEnd) == "function" then
                self:OnEnd();
            end
            MetricsCollector:Record("final_tick", self._tick);
            MetricsCollector:SetResult(self._passed);
            MetricsCollector:EndRun();
            AIDecisionChannel:EmitWithTarget(-1, "decision", "scenario_end", self._passed and "pass" or "fail",
                                             "tick=" .. tostring(self._tick), -1, 0, 0);

            -- Surface the result on-screen so the human watching the scenario can see PASS/FAIL
            -- before the window closes (CLI mode) or before the activity bounces to the menu.
            if ConsoleMan then
                local resultText = self._passed and "PASS" or "FAIL";
                ConsoleMan:PrintString("[Trust] " .. scenarioName .. ": " .. resultText
                                       .. " (tick=" .. tostring(self._tick) .. ")");
            end

            self.ActivityState = Activity.OVER;
        end
    end

    function cls:PauseActivity(pause) end

    function cls:EndActivity()
        -- If the activity was ended externally (e.g. hard-cap auto-exit before
        -- _scenarioFinished was set), still finalize so the JSON report has a result.
        if not self._finalized then
            if not self._scenarioFinished then
                self._scenarioFinished = true;
                self._passed = false;
                MetricsCollector:Record("ended_externally", 1);
                if type(self.OnEnd) == "function" then
                    self:OnEnd();
                end
            end
            MetricsCollector:Record("final_tick", self._tick or 0);
            MetricsCollector:SetResult(self._passed);
            MetricsCollector:EndRun();
            AIDecisionChannel:EmitWithTarget(-1, "decision", "scenario_end", self._passed and "pass" or "fail",
                                             "EndActivity:tick=" .. tostring(self._tick or 0), -1, 0, 0);
            self._finalized = true;
        end
        AIDebugOverlay.Enabled = false;
    end

    function cls:OnSave() end
    function cls:CraftEnteredOrbit(craft) end
    function cls:OnMessage(message, context) end
    function cls:OnGlobalMessage(message, context) end
    function cls:IsCompatibleScene(scene) return true end

    -- Helpers exposed to the concrete scenario.

    function cls:SpawnActor(presetName, presetModule, x, y, team, aiMode)
        local actor = CreateAHuman(presetName, presetModule);
        if not actor then
            return nil;
        end
        actor.Pos = Vector(x or TrustScenario.DEFAULT_SPAWN_X, y or TrustScenario.DEFAULT_SPAWN_Y);
        actor.Team = team or Activity.TEAM_1;
        if aiMode then actor.AIMode = aiMode; end
        MovableMan:AddActor(actor);
        table.insert(self._spawnedActors, actor);
        -- Auto-watch the first spawned actor in the debug overlay so the user can see
        -- decision events without having to "switch to" an actor in-game.
        if AIDebugOverlay.WatchedActorId == nil or AIDebugOverlay.WatchedActorId < 0 then
            AIDebugOverlay.WatchedActorId = actor.UniqueID;
        end
        return actor;
    end

    function cls:SpawnCrab(presetName, presetModule, x, y, team, aiMode)
        local actor = CreateACrab(presetName, presetModule);
        if not actor then
            return nil;
        end
        actor.Pos = Vector(x or TrustScenario.DEFAULT_SPAWN_X, y or TrustScenario.DEFAULT_SPAWN_Y);
        actor.Team = team or Activity.TEAM_1;
        if aiMode then actor.AIMode = aiMode; end
        MovableMan:AddActor(actor);
        table.insert(self._spawnedActors, actor);
        if AIDebugOverlay.WatchedActorId == nil or AIDebugOverlay.WatchedActorId < 0 then
            AIDebugOverlay.WatchedActorId = actor.UniqueID;
        end
        return actor;
    end

    function cls:CountLivingActors()
        local count = 0;
        for _, a in ipairs(self._spawnedActors) do
            if MovableMan:IsActor(a) and a.Health > 0 then
                count = count + 1;
            end
        end
        return count;
    end

    function cls:FirstLivingActor()
        for _, a in ipairs(self._spawnedActors) do
            if MovableMan:IsActor(a) and a.Health > 0 then
                return a;
            end
        end
        return nil;
    end

    function cls:Tick() return self._tick or 0; end

    function cls:RecordMetric(name, value)
        self._metrics[name] = value;
        MetricsCollector:Record(name, value);
    end

    -- Counts air (material 0) terrain pixels in an inclusive pixel box. Scenarios
    -- sample this before/after to prove terrain was genuinely carved.
    function cls:CountAirPixels(x1, y1, x2, y2)
        local air = 0;
        for px = x1, x2 do
            for py = y1, y2 do
                if SceneMan:GetTerrMatter(px, py) == 0 then
                    air = air + 1;
                end
            end
        end
        return air;
    end

    -- Carves a rectangular hole in the terrain. Used by scenarios as a real route
    -- opener and by the self-test positive control.
    function cls:CarveBox(x1, y1, x2, y2)
        SceneMan:DislodgePixelBox(Vector(x1, y1), Vector(x2, y2), true);
    end

    return cls;
end

return TrustScenario;
