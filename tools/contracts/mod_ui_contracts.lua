-- Independent public observations of the activity-owned UI and its retained native values.
local function objectLabel(object)
    return object and object.ClassName .. ":" .. object.ModuleName .. "/" .. object.PresetName or "nil"
end

local function cartItems(buy)
    local values = {}
    for item in buy:GetOrderList() do values[#values + 1] = objectLabel(item) end
    return table.concat(values, "|")
end

local function editorObject(class)
    local candidate
    for preset in PresetMan:GetAllEntities() do
        if preset.ClassName == class and (not candidate or objectLabel(preset) < objectLabel(candidate)) then candidate = preset end
    end
    assert(candidate, "no editor fixture preset for " .. class)
    return _G["Create" .. class](candidate.PresetName, candidate.ModuleName)
end

function Create(self)
    self.testCreate, self.testUpdate = 1, 0
end

function Update(self)
    self.testUpdate = self.testUpdate + 1
    self:SetNumberValue("TestUpdates", self.testUpdate)
    if self.UniqueID ~= 1048577 or self.uiContracts then return end
    local activity = ToGameActivity(ActivityMan:GetActivity())
    local buy, editor = activity:GetBuyGUI(0), activity:GetEditorGUI(0)
    assert(buy and editor, "activity UI is unavailable")
    activity.BuyMenuEnabled = false
    buy:ClearCartList()
    buy:LoadDefaultLoadoutToCart()
    buy:SetForeignCostMultiplier(1.375)
    buy:SetOwnedItemsAmount("Base.rte/Battle Rifle", 13)
    buy:SetModuleExpanded(0, false)
    buy:SetBannerImage("Base.rte/GUIs/Skins/Cursor.png")
    buy:SetLogoImage("Base.rte/GUIs/Skins/Cursor.png")
    local heldOrder = buy:GetOrderList()
    local firstOrder = heldOrder()
    assert(firstOrder, "default loadout has no cart items")
    local expectedRemaining = {}
    local count = 0
    for item in buy:GetOrderList() do
        count = count + 1
        if count > 1 then expectedRemaining[#expectedRemaining + 1] = objectLabel(item) end
    end
    local placed = editorObject(os.getenv("CC_CONTRACT_EDITOR_CLASS") or "TerrainObject")
    placed.PresetName = "Checkpoint editor current object"
    placed.Pos = Vector(412.25, 267.75)
    placed.Team = 1
    assert(editor:SetCurrentObject(placed), "editor refused its owned object")
    editor.EditorMode = SceneEditorGUI.INACTIVE
    editor:SetCursorPos(Vector(489.5, 321.25))
    editor:SetForeignCostMultiplier(1.625)
    local current = editor:GetCurrentObject()
    local background
    for layer in SceneMan.Scene.BackgroundLayers do background = layer; break end
    assert(background, "scene has no background")
    background.IsAnimatedManually = true
    background.SpriteAnimDuration = 1777
    background.AutoScrollX, background.AutoScrollY = false, false
    background.AutoScrollInterval = 139
    background.AutoScrollStep = Vector(1.375, -2.625)
    local ownedVector = Vector(18.25, -29.5)
    local ownedTimer = Timer()
    ownedTimer.StartSimTimeTicks, ownedTimer.SimTimeLimitTicks = -777000, 9876000
    ownedTimer.StartRealTimeTicks, ownedTimer.RealTimeLimitTicks = -888000, 7654000
    local ownedAlarm = AlarmEvent()
    ownedAlarm.ScenePos, ownedAlarm.Team, ownedAlarm.Range = Vector(97.75, -46.125), 2, 351.5
    self.uiContracts = {
        buy=buy, buyAlias=buy, editor=editor, editorAlias=editor,
        current=current, currentAlias=current, currentPos=current.Pos,
        order=heldOrder, orderAlias=heldOrder, firstOrder=firstOrder,
        expectedOrder=cartItems(buy), expectedRemaining=table.concat(expectedRemaining, "|"),
        cartCost=buy:GetTotalCartCost(), orderCost=buy:GetTotalOrderCost(),
        mass=buy:GetTotalOrderMass(), passengers=buy:GetTotalOrderPassengers(),
        currentLabel=objectLabel(current), currentX=current.Pos.X, currentY=current.Pos.Y,
        currentTeam=current.Team, currentPlayer=current.PlacedByPlayer,
        background=background, backgroundAlias=background, frame=background.Frame,
        vector=ownedVector, vectorAlias=ownedVector, timer=ownedTimer, timerAlias=ownedTimer,
        alarm=ownedAlarm, alarmAlias=ownedAlarm, alarmPos=ownedAlarm.ScenePos,
    }
    local root = self
    _G._ContractAuditCheck = function(stage)
        local saved = root.uiContracts
        local failures, checked = 0, 0
        local function check(label, valid)
            checked = checked + 1
            if not valid then failures = failures + 1; print("[reference-contract-mismatch] " .. stage .. " UI." .. label) end
        end
        local actualActivity = ToGameActivity(ActivityMan:GetActivity())
        local actualBuy, actualEditor = actualActivity:GetBuyGUI(0), actualActivity:GetEditorGUI(0)
        local actualCurrent = actualEditor:GetCurrentObject()
        check("buy_alias", rawequal(saved.buy, saved.buyAlias))
        check("editor_alias", rawequal(saved.editor, saved.editorAlias))
        check("buy_owner", _ScriptGraphNativeAddress(saved.buy) == _ScriptGraphNativeAddress(actualBuy))
        check("editor_owner", _ScriptGraphNativeAddress(saved.editor) == _ScriptGraphNativeAddress(actualEditor))
        check("cart_items", cartItems(actualBuy) == saved.expectedOrder)
        check("cart_cost", actualBuy:GetTotalCartCost() == saved.cartCost)
        check("order_cost", actualBuy:GetTotalOrderCost() == saved.orderCost)
        check("order_mass", actualBuy:GetTotalOrderMass() == saved.mass)
        check("order_passengers", actualBuy:GetTotalOrderPassengers() == saved.passengers)
        check("owned_items", actualBuy:GetOwnedItemsAmount("Base.rte/Battle Rifle") == 13)
        check("editor_mode", actualEditor.EditorMode == SceneEditorGUI.INACTIVE)
        check("current_alias", rawequal(saved.current, saved.currentAlias))
        check("current_owner", actualCurrent and _ScriptGraphNativeAddress(saved.current) == _ScriptGraphNativeAddress(actualCurrent))
        check("current_label", objectLabel(actualCurrent) == saved.currentLabel)
        check("current_position", actualCurrent and actualCurrent.Pos.X == saved.currentX and actualCurrent.Pos.Y == saved.currentY)
        check("current_team", actualCurrent and actualCurrent.Team == saved.currentTeam)
        check("current_player", actualCurrent and actualCurrent.PlacedByPlayer == saved.currentPlayer)
        check("current_position_alias", actualCurrent and _ScriptGraphNativeAddress(saved.currentPos) == _ScriptGraphNativeAddress(actualCurrent.Pos))
        check("order_iterator_alias", rawequal(saved.order, saved.orderAlias))
        check("first_order_preset", objectLabel(saved.firstOrder) ~= "nil")
        check("background_alias", rawequal(saved.background, saved.backgroundAlias))
        local actualBackground; for layer in SceneMan.Scene.BackgroundLayers do actualBackground=layer; break end
        check("background_owner", _ScriptGraphNativeAddress(saved.background) == _ScriptGraphNativeAddress(actualBackground))
        check("background_frame", actualBackground.Frame == saved.frame)
        check("background_manual", actualBackground.IsAnimatedManually)
        check("background_duration", actualBackground.SpriteAnimDuration == 1777)
        check("background_scroll_axes", not actualBackground.AutoScrollX and not actualBackground.AutoScrollY)
        check("background_scroll_interval", actualBackground.AutoScrollInterval == 139)
        check("background_scroll_step", actualBackground.AutoScrollStep.X == 1.375 and actualBackground.AutoScrollStep.Y == -2.625)
        check("vector_alias", rawequal(saved.vector, saved.vectorAlias))
        check("vector_value", saved.vector.X == 18.25 and saved.vector.Y == -29.5)
        check("timer_alias", rawequal(saved.timer, saved.timerAlias))
        check("timer_sim", saved.timer.StartSimTimeTicks == -777000 and saved.timer.SimTimeLimitTicks == 9876000)
        check("timer_real", saved.timer.StartRealTimeTicks == -888000 and saved.timer.RealTimeLimitTicks == 7654000)
        check("alarm_alias", rawequal(saved.alarm, saved.alarmAlias))
        check("alarm_value", saved.alarm.ScenePos.X == 97.75 and saved.alarm.ScenePos.Y == -46.125 and saved.alarm.Team == 2 and saved.alarm.Range == 351.5)
        check("alarm_position_alias", _ScriptGraphNativeAddress(saved.alarmPos) == _ScriptGraphNativeAddress(saved.alarm.ScenePos))
        if stage == "after" then
            local remaining = {}; for item in saved.order do remaining[#remaining + 1] = objectLabel(item) end
            check("retained_order_continuation", table.concat(remaining, "|") == saved.expectedRemaining)
        end
        print("[reference-contract-check] " .. stage .. " checked=" .. checked .. " mismatches=" .. failures)
    end
    print("[ui-contract-fixture] constructed editor=" .. current.ClassName .. " cart_items=" .. count)
end
