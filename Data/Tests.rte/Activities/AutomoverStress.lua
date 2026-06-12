-- AutomoverStress.lua — determinism scenario: the automover bunker-rail system.
--
-- The automover node graph is the engine's biggest Lua-side sim system keyed by
-- object identity: node registration, per-direction connection picks, a Dijkstra
-- pass per node, teleporter hops and rider routing. This scenario builds a live
-- network in open air — two rows of three nodes plus a teleporter at each end —
-- and rides four actors across it on GOTO waypoints. Rider 3's shortest route is
-- rail-then-teleporter-hop, so the teleport pick and the equal-cost Dijkstra
-- entries actually run; rider 4 takes the long rail and ends oscillating at a
-- connecting-area boundary — vanilla centring jank, kept as deterministic churn
-- (the pass bar is 3 of 4). A corner node is deleted late in the run to exercise
-- removal and the neighbours' re-add path.
--
-- Determinism: every node position, spawn tick, rider start and target is fixed;
-- the only randomness is the sim's seeded RNG. Reproducibility of the graph
-- build, the path tables and the rider trajectories is the test.

package.loaded.Constants = nil; require("Constants");
local Test = require("Lib/TestScenario");

TestScenarioAutomoverStress = Test.Extend("AutomoverStress", { max_ticks = 900 });

-- Lattice geometry: columns 96px apart (exactly aligned for connections), two
-- rows 96px apart in the air above the plateau, a teleporter on each row's end.
local COL_X = { 860, 956, 1052 };
local TELE_A_X = 1148;
local TELE_B_X = 764;
local ROW_A_LIFT = 100;
local ROW_B_LIFT = 196;

local SPAWN_NODES_TICK = 5;
local SPAWN_RIDERS_TICK = 90;
local DELETE_NODE_TICK = 600;
local ARRIVE_RADIUS = 60;

local function spawnNode(presetName, x, y)
    local node = CreateMOSRotating(presetName, "Base.rte");
    node.Pos = Vector(x, y);
    node.Team = Activity.TEAM_1;
    MovableMan:AddParticle(node);
    return node;
end

function TestScenarioAutomoverStress:OnStart()
    local groundY = SceneMan:MovePointToGround(Vector(COL_X[2], 0), 20, 10).Y;
    self._rowAY = groundY - ROW_A_LIFT;
    self._rowBY = groundY - ROW_B_LIFT;
    self._groundY = groundY;
    self._riders = {};
    self._nodes = {};
    self:RecordMetric("ground_y", groundY);
end

function TestScenarioAutomoverStress:OnTick(tick)
    if tick == SPAWN_NODES_TICK then
        -- The controller powers the team's network and runs the routing script.
        local controller = CreateActor("Automover Controller", "Base.rte");
        controller.Pos = Vector(COL_X[2], self._groundY - 12);
        controller.Team = Activity.TEAM_1;
        MovableMan:AddActor(controller);

        for _, x in ipairs(COL_X) do
            table.insert(self._nodes, spawnNode("Automover Node 1x1", x, self._rowAY));
            table.insert(self._nodes, spawnNode("Automover Node 1x1", x, self._rowBY));
        end
        table.insert(self._nodes, spawnNode("Teleporter Node", TELE_A_X, self._rowAY));
        table.insert(self._nodes, spawnNode("Teleporter Node", TELE_B_X, self._rowBY));
    end

    if tick == SPAWN_RIDERS_TICK then
        self:RecordMetric("node_count_at_riders", AutomoverData[Activity.TEAM_1].nodeDataCount);
        -- Four riders, spawned weightless inside node zones, each with a GOTO
        -- target inside a far zone. Riders 3 and 4 route through the teleporters.
        local routes = {
            { fromX = COL_X[1], fromY = self._rowAY, toX = COL_X[3], toY = self._rowBY },
            { fromX = COL_X[3], fromY = self._rowAY, toX = COL_X[1], toY = self._rowBY },
            { fromX = COL_X[2], fromY = self._rowBY, toX = TELE_A_X, toY = self._rowAY },
            { fromX = TELE_B_X, fromY = self._rowBY, toX = COL_X[3], toY = self._rowAY },
        };
        for i, route in ipairs(routes) do
            local rider = CreateAHuman("Green Dummy", "Base.rte");
            rider.Pos = Vector(route.fromX, route.fromY);
            rider.Team = Activity.TEAM_1;
            rider.AIMode = Actor.AIMODE_GOTO;
            MovableMan:AddActor(rider);
            rider:ClearAIWaypoints();
            rider:AddAISceneWaypoint(Vector(route.toX, route.toY));
            table.insert(self._spawnedActors, rider);
            table.insert(self._riders, { actor = rider, target = Vector(route.toX, route.toY) });
        end
    end

    if tick == DELETE_NODE_TICK then
        -- Remove a corner node: exercises Automovers_RemoveNode and the
        -- neighbours' shouldReaddNode re-registration.
        for _, node in ipairs(self._nodes) do
            if MovableMan:ValidMO(node) and node.Pos.X == COL_X[1] and node.Pos.Y == self._rowAY then
                node.ToDelete = true;
            end
        end
        self:RecordMetric("node_count_before_delete", AutomoverData[Activity.TEAM_1].nodeDataCount);
    end

    -- Finish early once every rider has arrived.
    if tick > SPAWN_RIDERS_TICK + 60 and tick % 30 == 0 then
        local arrived = self:CountArrivedRiders();
        if arrived == #self._riders then
            return true, true;
        end
    end

    return false;
end

function TestScenarioAutomoverStress:CountArrivedRiders()
    local arrived = 0;
    for _, rider in ipairs(self._riders) do
        if MovableMan:ValidMO(rider.actor) and rider.actor.Health > 0 then
            local dist = SceneMan:ShortestDistance(rider.actor.Pos, rider.target, true);
            if dist:MagnitudeIsLessThan(ARRIVE_RADIUS) then
                arrived = arrived + 1;
            end
        end
    end
    return arrived;
end

function TestScenarioAutomoverStress:OnEnd()
    local arrived = self:CountArrivedRiders();
    self:RecordMetric("riders_arrived", arrived);
    self:RecordMetric("node_count_final", AutomoverData[Activity.TEAM_1].nodeDataCount);
    self:RecordMetric("teleporter_count_final", AutomoverData[Activity.TEAM_1].teleporterNodesCount);
    for i, rider in ipairs(self._riders) do
        if MovableMan:ValidMO(rider.actor) then
            self:RecordMetric("rider" .. i .. "_dist",
                              SceneMan:ShortestDistance(rider.actor.Pos, rider.target, true).Magnitude);
        end
    end
    -- Three of four is the bar: rail riding works, with slack for one AI wobble.
    self._passed = arrived >= 3;
end
