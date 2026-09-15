SeatFacts = SeatFacts or {};
dofile("UserScenes.rte/SeatFacts.lua");

-- The F79 residual: the peer that owns a seat answers a script from its own controlled actor the moment it
-- switches, while the other peers learn the switch from the committed frame input-delay ticks later. Both
-- peers print every seat's answer for every tick of the window around the harness's switch at tick 220.
-- Two windows: the harness switches each peer's own seat at 220 and hands it back at 230, then kills the
-- brains at 299 - a controlled actor the owner still names is refused to the other peers once it is dead.
local windows = {{215, 245}, {290, 320}};

function SwitchWindowFacts:StartActivity(startNewGame)
    SeatFacts.StartActivity(self, startNewGame);
end

function SwitchWindowFacts:UpdateActivity()
    SeatFacts.UpdateActivity(self);
    local tick = self.seatFactsTick;
    local inWindow = false;
    for _, window in ipairs(windows) do
        inWindow = inWindow or (tick >= window[1] and tick <= window[2]);
    end
    if not inWindow then
        return;
    end
    for player = Activity.PLAYER_1, Activity.PLAYER_2 do
        local actor = self:GetControlledActor(player);
        print("[switch-window] tick=" .. tick .. " player=" .. player .. " uid=" .. (actor and actor.UniqueID or 0)
            .. " screen=" .. self:ScreenOfPlayer(player));
    end
end
