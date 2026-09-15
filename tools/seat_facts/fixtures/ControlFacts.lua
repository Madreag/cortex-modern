SeatFacts = SeatFacts or {};
dofile("UserScenes.rte/SeatFacts.lua");

-- A mod sim script reading the seat's controlled actor: every peer must name the same one, and a sim
-- write keyed on that answer must land on every peer alike.
local readTick = 220;

function ControlFacts:StartActivity(startNewGame)
    SeatFacts.StartActivity(self, startNewGame);
    self.controlFactsRead = false;
end

function ControlFacts:UpdateActivity()
    SeatFacts.UpdateActivity(self);
    if self.seatFactsTick == readTick and not self.controlFactsRead then
        self.controlFactsRead = true;
        local seen = 0;
        for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
            local actor = self:GetControlledActor(player);
            local uid = actor and actor.UniqueID or 0;
            print("[control-facts] tick=" .. readTick .. " player=" .. player .. " uid=" .. uid
                .. " screen=" .. self:ScreenOfPlayer(player));
            if actor then
                seen = seen + 1;
                actor.Vel = Vector(actor.Vel.X + 0.25, actor.Vel.Y);
            end
        end
        print("[control-facts] read seats=" .. seen);
    end
end
