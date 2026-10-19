-- Staged after FeelBaseline by test_pie_checkpoint_holders.py.
local shape = "__SHAPE__";
local function wants(name) return shape == "all" or shape == name end
local start = FeelBaseline.StartActivity;
function FeelBaseline:StartActivity(startNewGame)
    start(self, startNewGame);
    if startNewGame == false then return end
    local actor = CreateAHuman("Green Dummy", "Base.rte");
    actor.Pos = Vector(650, -500);
    actor.Team = -1;
    actor.GetsHitByMOs = false;
    actor.PinStrength = 1000000;
    actor.GibWoundLimit = 1000000;
    actor.GibImpulseLimit = 1000000000;
    actor.MissionCritical = true;
    actor:SetControllerMode(Controller.CIM_DISABLED, -1);
    actor:AddInventoryItem(CreateHDFirearm("Constructor", "Base.rte"));
    MovableMan:AddActor(actor);
    actor:EquipFirearm(true);
    self.pieHolderFixture = {actor = actor, updates = 0, sources = {}, handles = {}};
end
local update = FeelBaseline.UpdateActivity;
function FeelBaseline:UpdateActivity()
    update(self);
    local fixture = self.pieHolderFixture;
    assert(fixture and fixture.actor, "pie holder fixture lost its actor");
    fixture.actor.Health = fixture.actor.MaxHealth;
    local menu = fixture.actor.PieMenu;
    local options = menu:GetFirstPieSliceByPresetName("Constructor Options");
    assert(options and options.SubPieMenu, "pie holder fixture has no device submenu");
    if not fixture.planted then
        local function add(name, source)
            local slice = CreatePieSlice("Land", "Base.rte");
            slice.PresetName = "Checkpoint " .. name;
            slice.Direction = Directions.Any;
            slice.Enabled = false;
            assert(menu:AddPieSlice(slice, source), "could not plant pie source " .. name);
            fixture.sources[name] = source;
        end
        if wants("actor") then add("actor", fixture.actor) end
        if wants("activity") then add("activity", self) end
        if wants("global") then
            assert(CheckpointSourceScript, "the source global script did not start");
            add("global", CheckpointSourceScript);
        end
        if wants("scene") then add("scene", SceneMan.Scene) end
        if wants("external-object") then
            local source = CreateAHuman("Green Dummy", "Base.rte");
            source.Pos = Vector(850, -500); source.PinStrength = 1000000;
            source.Team = -1; source.MissionCritical = true;
            MovableMan:AddActor(source);
            fixture.sourceActor = source;
            add("external-object", source);
        end
        if wants("preset") then add("preset", PresetMan:GetPreset("PieSlice", "Menu", "Base.rte")) end
        if wants("borrowed-menu") then
            fixture.rootMenu = menu;
            fixture.rootAlias = fixture.rootMenu;
            fixture.subMenu = options.SubPieMenu;
            fixture.subAlias = fixture.subMenu;
            fixture.rootMenu.FullInnerRadius = 83;
            fixture.subMenu.FullInnerRadius = 47;
        end
        if wants("device") then
            fixture.deviceSlice = options.SubPieMenu:GetFirstPieSliceByPresetName("Constructor Order Construction");
            fixture.deviceAlias = fixture.deviceSlice;
            fixture.deviceSlice.Enabled = false;
        end
        if wants("owned-menu") then
            fixture.ownedMenu = CreatePieMenu("Constructor Pie Menu", "Base.rte");
            fixture.ownedMenuSlice = fixture.ownedMenu:GetFirstPieSliceByPresetName("Constructor Spray Mode");
            fixture.ownedMenuSlice.Enabled = false;
            fixture.ownedSub = fixture.ownedMenu:GetFirstPieSliceByPresetName("Constructor Options");
            -- Constructor Pie Menu's preset does not require a submenu; the separate owned slice below does.
            local slice = CreatePieSlice("Land", "Base.rte");
            slice.PresetName = "Checkpoint owned-menu"; slice.Enabled = false;
            assert(fixture.ownedMenu:AddPieSlice(slice, fixture.ownedMenu));
        end
        if wants("owned-slice") then
            fixture.ownedSlice = CreatePieSlice("Constructor Options", "Base.rte");
            fixture.ownedSubMenu = fixture.ownedSlice.SubPieMenu;
            fixture.ownedSubSlice = fixture.ownedSubMenu:GetFirstPieSliceByPresetName("Constructor Cancel Construction");
            fixture.ownedSubSlice.Enabled = false;
        end
        if wants("preset") then
            fixture.presetMenu = ToPieMenu(PresetMan:GetPreset("PieMenu", "Constructor Pie Menu", "Base.rte"));
            fixture.presetSlice = ToPieSlice(PresetMan:GetPreset("PieSlice", "Menu", "Base.rte"));
        end
        fixture.planted = true;
        print("[pie-holder-fixture] planted shape=" .. shape);
    end
    local function same(a, b)
        assert(a and b and _ScriptGraphNativeAddress(a) == _ScriptGraphNativeAddress(b), "pie holder alias changed");
    end
    -- The external source must still be in the world, so the shape saves a live source and not a gone one.
    if fixture.sourceActor then assert(MovableMan:IsActor(fixture.sourceActor), "pie holder source actor left the world") end
    if _ScriptGraphNativeAddress then
        for name, source in pairs(fixture.sources) do
            local slice = menu:GetFirstPieSliceByPresetName("Checkpoint " .. name);
            assert(slice and slice.Enabled == false, "pie source slice values changed: " .. name);
            same(slice.OriginalSource, source);
            if name == "activity" then same(source, ActivityMan:GetActivity()) end
            if name == "global" then same(source, CheckpointSourceScript) end
            if name == "scene" then same(source, SceneMan.Scene) end
        end
        if wants("borrowed-menu") then
            same(fixture.rootMenu, menu); same(fixture.rootMenu, fixture.rootAlias);
            same(fixture.subMenu, options.SubPieMenu); same(fixture.subMenu, fixture.subAlias);
            assert(menu.FullInnerRadius == 83 and options.SubPieMenu.FullInnerRadius == 47, "pie menu values changed");
            fixture.subMenu.FullInnerRadius = 49;
            assert(options.SubPieMenu.FullInnerRadius == 49, "borrowed menu stopped aliasing its owner");
            fixture.subMenu.FullInnerRadius = 47;
        end
        if wants("device") then
            same(fixture.deviceSlice, options.SubPieMenu:GetFirstPieSliceByPresetName("Constructor Order Construction"));
            same(fixture.deviceSlice, fixture.deviceAlias);
            assert(fixture.deviceSlice.Enabled == false, "device slice values changed");
        end
        if wants("owned-menu") then
            same(fixture.ownedMenuSlice, fixture.ownedMenu:GetFirstPieSliceByPresetName("Constructor Spray Mode"));
            assert(fixture.ownedMenuSlice.Enabled == false, "owned menu slice values changed");
            local added = fixture.ownedMenu:GetFirstPieSliceByPresetName("Checkpoint owned-menu");
            assert(added and added.Enabled == false, "owned menu dropped its contributed slice");
            same(added.OriginalSource, fixture.ownedMenu);
        end
        if wants("owned-slice") then
            same(fixture.ownedSubMenu, fixture.ownedSlice.SubPieMenu);
            same(fixture.ownedSubSlice, fixture.ownedSubMenu:GetFirstPieSliceByPresetName("Constructor Cancel Construction"));
            assert(fixture.ownedSubSlice.Enabled == false, "owned submenu slice values changed");
        end
        if wants("preset") then
            same(fixture.presetMenu, PresetMan:GetPreset("PieMenu", "Constructor Pie Menu", "Base.rte"));
            same(fixture.presetSlice, PresetMan:GetPreset("PieSlice", "Menu", "Base.rte"));
        end
        fixture.updates = fixture.updates + 1;
        if fixture.updates % 30 == 0 then print("[pie-holder-fixture] verified shape=" .. shape .. " updates=" .. fixture.updates) end
    end
end
P4AlphaDuel = FeelBaseline;
