P4AlphaDuel = CrossCombat;
dofile("Data/Base.rte/Activities/P4AlphaDuel.lua");
local startDuel = CrossCombat.StartActivity;
local updateDuel = CrossCombat.UpdateActivity;

function CrossCombat:StartActivity(startNewGame)
    startDuel(self, startNewGame);
    if startNewGame == false then return; end
    self.crossTicks = 0;
    self.crossPurchases = {};
    for team = Activity.TEAM_1, Activity.MAXTEAMCOUNT - 1 do
        if self:TeamActive(team) then
            self:SetTeamFunds(10000, team);
            for index = 1, 3 do
                local actor = CreateAHuman("Green Dummy", "Base.rte");
                actor.Team = team;
                actor.AIMode = Actor.AIMODE_BRAINHUNT;
                actor.Pos = SceneMan:MovePointToGround(Vector(400 + team * 500 + index * 30, 0), 20, 10) + Vector(0, -actor.Radius);
                actor:AddInventoryItem(CreateHDFirearm("Battle Rifle", "Base.rte"));
                actor:AddInventoryItem(CreateHDFirearm("Light Digger", "Base.rte"));
                MovableMan:AddActor(actor);
            end
        end
    end
    print("[cross-fixture] seeded funds=10000 allies_per_team=3 win=last_brain");
end

function CrossCombat:UpdateActivity()
    updateDuel(self);
    if self.ActivityState ~= Activity.RUNNING then return; end
    self.crossTicks = (self.crossTicks or 0) + 1;
    if self.crossTicks % 600 == 0 then
        print("[cross-fixture] callback_completed tick=" .. self.crossTicks);
    end
end
