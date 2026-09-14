dofile("Base.rte/Activities/P4AlphaDuel.lua");

function SeatFacts:StartActivity(startNewGame)
    P4AlphaDuel.StartActivity(self, startNewGame);
    for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
        if self:PlayerActive(player) and self:PlayerHuman(player) then
            local brain = self:GetPlayerBrain(player);
            if brain then
                brain:SetNumberValue("HumanSeat" .. player, 101 + player);
                print("[seat-branch] player=" .. player .. " team=" .. self:GetTeamOfPlayer(player)
                    .. " brain=" .. brain.UniqueID .. " mark=" .. (101 + player));
            end
        end
    end
    self.seatFactsTick = 0;
end

function SeatFacts:UpdateActivity()
    P4AlphaDuel.UpdateActivity(self);
    self.seatFactsTick = self.seatFactsTick + 1;
    if self.seatFactsTick == 200 then
        for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
            local brain = self:GetPlayerBrain(player);
            print("[seat-facts] player=" .. player
                .. " active=" .. (self:PlayerActive(player) and 1 or 0)
                .. " human=" .. (self:PlayerHuman(player) and 1 or 0)
                .. " team=" .. self:GetTeamOfPlayer(player)
                .. " brain=" .. (brain and brain.UniqueID or 0)
                .. " mark=" .. (brain and brain:GetNumberValue("HumanSeat" .. player) or 0)
                .. " screen=" .. self:ScreenOfPlayer(player));
        end
    end
end
