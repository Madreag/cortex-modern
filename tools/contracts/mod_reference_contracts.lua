local factories = {
    owned_controller = function(a) return Controller() end,
    player_controller = function(a) return a:GetPlayerController(0) end,
    cursor_timer = function(a) return a.CursorTimer end,
    game_timer = function(a) return a.GameTimer end,
    game_over_timer = function(a) return a.GameOverTimer end,
    buy_menu = function(a) return a:GetBuyGUI(0) end,
    editor_menu = function(a) return a:GetEditorGUI(0) end,
    yellow_banner = function(a) return a:GetBanner(GUIBanner.YELLOW, 0) end,
    red_banner = function(a) return a:GetBanner(GUIBanner.RED, 0) end,
    background = function(a) for layer in SceneMan.Scene.BackgroundLayers do return layer end end,
    background_iterator = function(a) return SceneMan.Scene.BackgroundLayers end,
    material = function(a) return SceneMan:GetMaterialFromID(1) end,
    owned_emission = function(a) return Emission() end,
    owned_game_activity = function(a) return GameActivity() end,
    owned_scene = function(a) return Scene() end,
    owned_deployment = function(a) return Deployment() end,
    owned_terrain_object = function(a) return TerrainObject() end,
    owned_sound_set = function(a) return SoundSet() end,
    owned_round = function(a) return Round() end,
    owned_limb_path = function(a) return LimbPath() end,
}

function Create(self)
    self.testCreate, self.testUpdate = 1, 0
end

function Update(self)
    self.testUpdate = self.testUpdate + 1
    self:SetNumberValue("TestUpdates", self.testUpdate)
    if self.UniqueID ~= 1048577 or self.referenceContracts then return end
    local key = os.getenv("CC_CONTRACT_REFERENCE")
    assert(factories[key], "unknown reference contract " .. tostring(key))
    local value = factories[key](ToGameActivity(ActivityMan:GetActivity()))
    assert(value ~= nil, "reference contract source unavailable " .. key)
    if key == "owned_game_activity" then value.Difficulty = 77 end
    if key == "owned_sound_set" then value.SoundSelectionCycleMode = SoundSet.FORWARDS end
    if key == "owned_controller" then
        value.AnalogMove = Vector(0.375, -0.625)
        value.AnalogAim = Vector(-0.75, 0.125)
        value.ControlledActor = self
        value:SetState(Controller.WEAPON_FIRE, true)
    end
    if key == "cursor_timer" or key == "game_timer" or key == "game_over_timer" then
        value.SimTimeLimitMS = 12345.75
    end
    self.referenceContracts = {value=value, alias=value, key=key}
    local root = self
    _G._ContractAuditCheck = function(stage)
        local saved = root.referenceContracts
        local failures, checked = 0, 0
        local function check(label, valid)
            checked = checked + 1
            if not valid then
                failures = failures + 1
                print("[reference-contract-mismatch] " .. stage .. " " .. saved.key .. "." .. label)
            end
        end
        check("alias", rawequal(saved.value, saved.alias))
        if string.sub(saved.key, 1, 6) ~= "owned_" and saved.key ~= "background_iterator" then
            local actual = factories[saved.key](ToGameActivity(ActivityMan:GetActivity()))
            check("native-owner", _ScriptGraphNativeAddress(saved.value) == _ScriptGraphNativeAddress(actual))
        end
        if saved.key == "owned_controller" then
            check("AnalogMove", saved.value.AnalogMove.X == 0.375 and saved.value.AnalogMove.Y == -0.625)
            check("AnalogAim", saved.value.AnalogAim.X == -0.75 and saved.value.AnalogAim.Y == 0.125)
            check("WEAPON_FIRE", saved.value:IsState(Controller.WEAPON_FIRE))
            check("actor-cycle", _ScriptGraphNativeAddress(saved.value.ControlledActor) == _ScriptGraphNativeAddress(root))
        end
        if saved.key == "cursor_timer" or saved.key == "game_timer" or saved.key == "game_over_timer" then
            check("SimTimeLimitMS", saved.value.SimTimeLimitMS == 12345.75)
        end
        if saved.key == "owned_game_activity" then check("Difficulty", saved.value.Difficulty == 77) end
        if saved.key == "owned_sound_set" then check("SoundSelectionCycleMode", saved.value.SoundSelectionCycleMode == SoundSet.FORWARDS) end
        if saved.key == "material" then check("material", saved.value.PresetName ~= "") end
        if saved.key == "background_iterator" and stage == "after" then
            local count = 0
            for layer in saved.value do
                check("layer", type(layer) == "userdata")
                count = count + 1
            end
            check("nonempty", count > 0)
        end
        print("[reference-contract-check] " .. stage .. " checked=" .. checked .. " mismatches=" .. failures)
    end
    print("[native-reference-fixture] constructed=" .. key .. " type=" .. type(value))
end
