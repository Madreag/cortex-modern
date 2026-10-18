
local checkpointStart = FeelBaseline.StartActivity;
function FeelBaseline:StartActivity(startNewGame)
    checkpointStart(self, startNewGame);
    if startNewGame == false then print("[r0-slice-fixture] image start"); return; end
    local holder = CreateAHuman("Green Dummy", "Base.rte");
    holder.Pos = Vector(650, -500);
    holder.Team = -1;
    holder.GetsHitByMOs = false;
    holder.GibWoundLimit = 1000000;
    holder.GibImpulseLimit = 1000000000;
    holder.PinStrength = 1000000;
    holder.MissionCritical = true;
    holder:SetControllerMode(Controller.CIM_DISABLED, -1);
    holder:AddInventoryItem(CreateHDFirearm("Constructor", "Base.rte"));
    MovableMan:AddActor(holder);
    holder:EquipFirearm(true);
    local options = holder.PieMenu:GetFirstPieSliceByPresetName("Constructor Options");
    self.checkpointMenuHolder = holder;
    self.checkpointSlice = options.SubPieMenu:GetFirstPieSliceByPresetName("Constructor Order Construction");
    self.checkpointSliceAlias = self.checkpointSlice;
    assert(self.checkpointSlice ~= nil);
    self.checkpointSliceEnabled = not self.checkpointSlice.Enabled;
    self.checkpointSlice.Enabled = self.checkpointSliceEnabled;
    local grenadeHolder = CreateAHuman("Green Dummy", "Base.rte");
    grenadeHolder.Pos = Vector(850, -500);
    grenadeHolder.Team = -1;
    grenadeHolder.GetsHitByMOs = false;
    grenadeHolder.PinStrength = 1000000;
    grenadeHolder.GibWoundLimit = 1000000;
    grenadeHolder.GibImpulseLimit = 1000000000;
    grenadeHolder.MissionCritical = true;
    grenadeHolder:SetControllerMode(Controller.CIM_DISABLED, -1);
    grenadeHolder:AddInventoryItem(CreateHDFirearm("Grenade Launcher", "Coalition.rte"));
    MovableMan:AddActor(grenadeHolder);
    grenadeHolder:EquipFirearm(true);
    self.checkpointGrenadeHolder = grenadeHolder;
    local menu = grenadeHolder.PieMenu:GetFirstPieSliceByPresetName("Coalition Grenade Launcher Options").SubPieMenu;
    self.checkpointGrenadeSlices = {};
    self.checkpointGrenadeAliases = {};
    self.checkpointGrenadeEnabled = {};
    for _, name in ipairs({"Impact Mode", "Bounce Mode", "Remote Mode", "Remove Grenades", "Detonate Grenades"}) do
        local slice = menu:GetFirstPieSliceByPresetName("Coalition Grenade Launcher " .. name);
        assert(slice ~= nil);
        self.checkpointGrenadeSlices[name] = slice;
        self.checkpointGrenadeAliases[name] = slice;
        self.checkpointGrenadeEnabled[name] = not slice.Enabled;
        slice.Enabled = self.checkpointGrenadeEnabled[name];
    end
end
local checkpointUpdate = FeelBaseline.UpdateActivity;
function FeelBaseline:UpdateActivity()
    checkpointUpdate(self);
    assert(self.checkpointMenuHolder ~= nil, "the image dropped the slice holder");
    local holder = self.checkpointMenuHolder;
    if holder then
        holder.Health = holder.MaxHealth;
        local options = holder.PieMenu:GetFirstPieSliceByPresetName("Constructor Options");
        local slice = options and options.SubPieMenu:GetFirstPieSliceByPresetName("Constructor Order Construction");
        assert(slice and self.checkpointSlice and rawequal(self.checkpointSlice, self.checkpointSliceAlias));
        assert(slice.Enabled == self.checkpointSliceEnabled, "restored device slice lost its saved value");
        local enabled = slice.Enabled;
        self.checkpointSlice.Enabled = not enabled;
        assert(slice.Enabled == not enabled, "restored device slice lost its menu identity");
        self.checkpointSlice.Enabled = enabled;
        assert(self.checkpointGrenadeHolder ~= nil, "the image dropped the grenade slice holder");
        local menu = self.checkpointGrenadeHolder.PieMenu:GetFirstPieSliceByPresetName("Coalition Grenade Launcher Options").SubPieMenu;
        for name, cached in pairs(self.checkpointGrenadeSlices) do
            local current = menu:GetFirstPieSliceByPresetName("Coalition Grenade Launcher " .. name);
            assert(current ~= nil and rawequal(cached, self.checkpointGrenadeAliases[name]));
            assert(current.Enabled == self.checkpointGrenadeEnabled[name], "restored device slice lost its grenade value");
            local was = current.Enabled;
            cached.Enabled = not was;
            assert(current.Enabled == not was, "restored device slice lost its grenade menu identity");
            cached.Enabled = was;
        end
        self.checkpointVerifiedUpdates = (self.checkpointVerifiedUpdates or 0) + 1;
        if self.checkpointVerifiedUpdates % 30 == 0 then
            print("[r0-slice-fixture] verified updates=" .. self.checkpointVerifiedUpdates .. " grenade=5 aliases=6");
        end
    end
end
