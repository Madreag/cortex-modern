local case = dofile("Userdata/UserScenes.rte/PieCase.lua");

function Create(self)
    self.writeStep = 0;
    self.buyMenuOff = false;
end

function Update(self)
    self.writeStep = self.writeStep + 1;
    local activity = ActivityMan:GetActivity();
    if case == "buy_menu" and not self.buyMenuOff and activity then
        local gameActivity = ToGameActivity(activity);
        if gameActivity then
            gameActivity.BuyMenuEnabled = false;
            self.buyMenuOff = true;
            print("[pie-write] buymenu off step=" .. self.writeStep .. " uid=" .. self.UniqueID);
        end
    end
    local pie = self.PieMenu;
    if not pie then
        return;
    end
    local slices = 0;
    for slice in pie.PieSlices do
        slices = slices + 1;
    end
    local values = {pie:IsEnabled(), pie:IsEnabling(), pie:IsDisabling(),
                    pie:IsEnablingOrDisabling(), pie:IsVisible(), pie:HasSubPieMenuOpen()};
    local state = "";
    for _, value in ipairs(values) do
        state = state .. (value and "1" or "0");
    end
    print("[pie-write-observe] uid=" .. self.UniqueID .. " step=" .. self.writeStep
          .. " mode=" .. self:GetController().InputMode .. " state=" .. state
          .. " slices=" .. slices);
end
