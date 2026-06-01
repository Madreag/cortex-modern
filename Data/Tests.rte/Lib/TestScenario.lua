-- TestScenario.lua
--
-- Shared base class for the Tests.rte determinism scenarios. Each scenario in
-- Tests.rte/Activities/<Name>.lua extends this with its own setup, per-tick
-- observation, and pass/fail criteria.
--
-- Pattern:
--   local Test = require("Lib/TestScenario");
--   TestScenarioSimBaseline = Test.Extend("SimBaseline", { max_ticks = 600, ... });
--   function TestScenarioSimBaseline:OnStart()    -- spawn actors, set initial state
--   function TestScenarioSimBaseline:OnTick(tick) -- per-tick observation; return done/passed
--   function TestScenarioSimBaseline:OnEnd()      -- record final metrics
--
-- The base class wires the 9-callback GAScripted contract to these three hooks,
-- and ensures MetricsCollector + SimChecksum interactions stay consistent.

local TestScenario = {};

-- Spawn convention: SpawnActor's `y` is height ABOVE THE TERRAIN, not an absolute
-- coordinate. The actor is grounded at its x via SceneMan:MovePointToGround, then
-- lifted `y` px so it drops a short, settling distance onto solid ground -- correct
-- on any scene (Grasslands, Tutorial Bunker, ...) with no per-scene tuning.
TestScenario.DEFAULT_SPAWN_X = 950;
TestScenario.DEFAULT_SPAWN_HEIGHT = 50;

function TestScenario.Extend(scenarioName, defaults)
    local cls = {};
    cls.__index = cls;
    cls._scenarioName = scenarioName;
    cls._defaults = defaults or {};

    -- GAScripted callbacks. The activity instance (`self`) is the Lua "class" defined by the script,
    -- so we attach the standard callbacks on the class and route through the test hooks.

    function cls:StartActivity()
        self._tick = 0;
        self._maxTicks = self._defaults.max_ticks or 1800; -- default 30 sim-seconds
        self._passed = false;
        self._scenarioFinished = false;
        self._finalized = false;
        self._spawnedActors = {};
        self._metrics = {};

        -- Arm MetricsCollector for this run. The seed defaults to 0 here; the C++
        -- side substitutes the CLI -seed value when ScenarioRunner is active, so
        -- the report records the seed actually used.
        MetricsCollector:BeginRun(scenarioName, 0);
        MetricsCollector:RecordString("scenario", scenarioName);

        -- Remove any scene-placed actors so the scenario controls the population.
        self:ClearSceneActors();

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

        self:UpdateWatchCamera();

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

            -- Surface the result on-screen so the human watching the scenario can see PASS/FAIL
            -- before the window closes (CLI mode) or before the activity bounces to the menu.
            if ConsoleMan then
                local resultText = self._passed and "PASS" or "FAIL";
                ConsoleMan:PrintString("[Test] " .. scenarioName .. ": " .. resultText
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
            self._finalized = true;
        end
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
        local spawnX = x or TestScenario.DEFAULT_SPAWN_X;
        local groundY = SceneMan:MovePointToGround(Vector(spawnX, 0), 20, 10).Y;
        actor.Pos = Vector(spawnX, groundY - (y or TestScenario.DEFAULT_SPAWN_HEIGHT));
        actor.Team = team or Activity.TEAM_1;
        if aiMode then actor.AIMode = aiMode; end
        MovableMan:AddActor(actor);
        table.insert(self._spawnedActors, actor);
        return actor;
    end

    function cls:SpawnCrab(presetName, presetModule, x, y, team, aiMode)
        local actor = CreateACrab(presetName, presetModule);
        if not actor then
            return nil;
        end
        local spawnX = x or TestScenario.DEFAULT_SPAWN_X;
        local groundY = SceneMan:MovePointToGround(Vector(spawnX, 0), 20, 10).Y;
        actor.Pos = Vector(spawnX, groundY - (y or TestScenario.DEFAULT_SPAWN_HEIGHT));
        actor.Team = team or Activity.TEAM_1;
        if aiMode then actor.AIMode = aiMode; end
        MovableMan:AddActor(actor);
        table.insert(self._spawnedActors, actor);
        return actor;
    end

    -- Live-actor queries iterate MovableMan.Actors, never a held reference -- a
    -- stored actor ref dangles once the actor dies and can alias a recycled slot.
    function cls:CountLivingActors()
        local count = 0;
        for a in MovableMan.Actors do
            if a.Health > 0 then count = count + 1; end
        end
        return count;
    end

    -- Living actor count on one team.
    function cls:CountTeam(team)
        local n = 0;
        for a in MovableMan.Actors do
            if a.Health > 0 and a.Team == team then n = n + 1; end
        end
        return n;
    end

    function cls:FirstLivingActor()
        for a in MovableMan.Actors do
            if a.Health > 0 then return a; end
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

    -- Determinism-scenario helpers. Each is pcall-guarded so an unknown
    -- preset or a headless run degrades to a no-op rather than aborting the run.

    -- Equips an actor with a firearm from Base.rte.
    function cls:GiveFirearm(actor, preset)
        if not actor then return; end
        pcall(function()
            local gun = CreateHDFirearm(preset, "Base.rte");
            if gun then
                actor:AddInventoryItem(gun);
                actor:EquipNamedDevice("Base.rte", preset, true);
            end
        end);
    end

    -- Adds `count` thrown explosives to an actor's inventory; the AI throws them.
    function cls:GiveGrenades(actor, preset, count)
        if not actor then return; end
        for _ = 1, (count or 1) do
            pcall(function()
                local g = CreateTDExplosive(preset or "Frag Grenade", "Base.rte");
                if g then actor:AddInventoryItem(g); end
            end);
        end
    end

    -- Equips an actor with a digging tool.
    function cls:GiveDigger(actor, preset)
        if not actor then return; end
        local name = preset or "Heavy Digger";
        pcall(function()
            local d = CreateHDFirearm(name, "Base.rte");
            if d then
                actor:AddInventoryItem(d);
                actor:EquipNamedDevice("Base.rte", name, true);
            end
        end);
    end

    -- Sets a team's AI to maximum skill (100 = UnfairSkill).
    function cls:MaxTeamAISkill(team)
        pcall(function() self:SetTeamAISkill(team, 100); end);
    end

    -- Spawns a line of armed actors. opts keys: count, x, step, y, team, aiMode,
    -- preset, firearm, grenades, grenadePreset, digger. Returns the actor list.
    function cls:SpawnSquad(opts)
        local squad = {};
        for i = 0, opts.count - 1 do
            local a = self:SpawnActor(opts.preset or "Green Dummy", "Base.rte",
                opts.x + i * (opts.step or 70), opts.y or 50, opts.team, opts.aiMode);
            if a then
                if opts.firearm then self:GiveFirearm(a, opts.firearm); end
                if opts.digger then self:GiveDigger(a, opts.digger); end
                if opts.grenades and opts.grenades > 0 then
                    self:GiveGrenades(a, opts.grenadePreset, opts.grenades);
                end
                table.insert(squad, a);
            end
        end
        return squad;
    end

    -- Watch camera: smoothly tracks the centroid of living actors so a human
    -- watching the scenario always sees the action. The camera is Presentation,
    -- outside the determinism island -- this never affects the per-tick hash.
    function cls:UpdateWatchCamera()
        pcall(function()
            local sx, sy, n = 0, 0, 0;
            for a in MovableMan.Actors do
                if a.Health > 0 then
                    sx = sx + a.Pos.X; sy = sy + a.Pos.Y; n = n + 1;
                end
            end
            if n > 0 then
                CameraMan:SetScrollTarget(Vector(sx / n, sy / n), 0.3, 0);
            end
        end);
    end

    -- Removes actors the scene file itself placed. Mission and tutorial maps
    -- (Tutorial Bunker bakes in a brain, dummies and crabs) ship actors as part
    -- of the scene; called at StartActivity before OnStart -- when every actor
    -- present is scene-placed -- so the scenario alone controls the population.
    -- Leaves terrain and structure untouched. Deterministic, pcall-guarded.
    function cls:ClearSceneActors()
        pcall(function()
            local toClear = {};
            for actor in MovableMan.Actors do
                if actor.ClassName == "AHuman" or actor.ClassName == "ACrab"
                        or actor:IsInGroup("Brains") then
                    table.insert(toClear, actor);
                end
            end
            for _, actor in ipairs(toClear) do
                MovableMan:RemoveActor(actor);
            end
            MetricsCollector:Record("cleared_scene_actors", #toClear);
        end);
    end

    return cls;
end

return TestScenario;
