SeatFacts = SeatFacts or {};
dofile("UserScenes.rte/SeatFacts.lua");

-- The Void Wanderers calls themselves, with nothing added: the activity script clears seat 1's banner every
-- frame (Tactics.lua:1731) and reads every seat's menu, editor and controlled actor once (Panel_Ship.lua:119).
-- The fixture makes no claim about what the engine answers - it only has to run.
local readTick = 220;

function VesselModCalls:StartActivity(startNewGame)
    SeatFacts.StartActivity(self, startNewGame);
    self.vesselRead = false;
end

function VesselModCalls:UpdateActivity()
    SeatFacts.UpdateActivity(self);
    self:GetBanner(GUIBanner.RED, Activity.PLAYER_1):ClearText();
    if self.seatFactsTick ~= readTick or self.vesselRead then
        return;
    end
    self.vesselRead = true;
    for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
        local banner = self:GetBanner(GUIBanner.YELLOW, player);
        local menu = self:GetBuyGUI(player);
        local editor = self:GetEditorGUI(player);
        local actor = self:GetControlledActor(player);
        local uid = actor and actor.UniqueID or 0;
        if actor then
            actor.Vel = Vector(actor.Vel.X + 0.25, actor.Vel.Y);
        end
        banner:ClearText();
        print("[vessel-mod] tick=" .. readTick .. " player=" .. player .. " uid=" .. uid
            .. " screen=" .. self:ScreenOfPlayer(player) .. " banner=" .. (banner and 1 or 0)
            .. " menu=" .. (menu and 1 or 0) .. " editor=" .. (editor and 1 or 0));
    end
    print("[vessel-mod] read seats=" .. Activity.MAXPLAYERCOUNT);
end
