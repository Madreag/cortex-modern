SeatFacts = SeatFacts or {};
dofile("UserScenes.rte/SeatFacts.lua");

-- What Void Wanderers does from its activity script, which runs on every peer: clear seat 1's banner every
-- frame (Tactics.lua:1731), ask every seat for its menu, its editor and its controlled actor, and key a sim
-- write on that answer (Panel_Ship.lua:119). A seat this peer does not present must answer with an inert
-- object, never a nil the unchanged mod would have to check.
local readTick = 220;

local function bannerFacts(banner)
    return "text=" .. banner.BannerText .. " anim=" .. banner.AnimState .. " kerning=" .. banner.Kerning
        .. " visible=" .. (banner:IsVisible() and 1 or 0);
end

function VesselBannerFacts:StartActivity(startNewGame)
    SeatFacts.StartActivity(self, startNewGame);
    self.vesselRead = false;
end

function VesselBannerFacts:UpdateActivity()
    SeatFacts.UpdateActivity(self);
    -- Tactics.lua:1731 verbatim in shape: an unguarded method call on seat 1's banner, every frame, every peer.
    self:GetBanner(GUIBanner.RED, Activity.PLAYER_1):ClearText();
    if self.seatFactsTick ~= readTick or self.vesselRead then
        return;
    end
    self.vesselRead = true;
    for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
        local local_seat = self:ScreenOfPlayer(player) >= 0;
        local banner = self:GetBanner(GUIBanner.YELLOW, player);
        local menu = self:GetBuyGUI(player);
        local editor = self:GetEditorGUI(player);
        local actor = self:GetControlledActor(player);
        local uid = actor and actor.UniqueID or 0;
        if actor then
            actor.Vel = Vector(actor.Vel.X + 0.25, actor.Vel.Y);
        end
        if not local_seat then
            -- The mod drives the absent seat's UI exactly as it drives its own; every call must be a no-op.
            banner:ShowText("VESSEL", GUIBanner.FLYBYLEFTWARD, 1000, Vector(640, 480), 0.5, 1500, 500);
            banner:HideText(1500, 100);
            banner.Kerning = 7;
            banner:ClearText();
            menu.ShowOnlyOwnedItems = true;
            menu.EnforceMaxMassConstraint = false;
            menu:SetMetaPlayer(player);
            menu:SetNativeTechModule(0);
            menu:SetForeignCostMultiplier(2.0);
            menu:SetModuleExpanded(0, true);
            menu:LoadAllLoadoutsFromFile();
            menu:AddAllowedItem("Vessel Test Item");
            menu:SetOwnedItemsAmount("Vessel Test Item", 3);
            menu:SetBannerImage("Base.rte/GUIs/BuyMenu/BuyMenuBanner.png");
            menu:SetLogoImage("Base.rte/GUIs/BuyMenu/BuyMenuLogo.png");
            menu:ClearCartList();
            menu:LoadDefaultLoadoutToCart();
            menu:ForceRefresh();
            editor.EditorMode = SceneEditorGUI.PLACINGOBJECT;
            editor:SetCursorPos(Vector(12, 34));
            editor:SetModuleSpace(0);
            editor:SetNativeTechModule(0);
            editor:SetForeignCostMultiplier(2.0);
            editor:TestBrainResidence(false);
            editor:Update();
            assert(bannerFacts(banner) == "text= anim=0 kerning=0 visible=0", "absent seat banner kept state: " .. bannerFacts(banner));
            assert(not menu.ShowOnlyOwnedItems and menu.EnforceMaxMassConstraint, "absent seat menu took a flag");
            assert(menu:GetOwnedItemsAmount("Vessel Test Item") == 0, "absent seat menu kept an owned item");
            assert(menu:GetTotalCartCost() == 0 and menu:GetTotalOrderCost() == 0, "absent seat menu has a cost");
            assert(menu:GetTotalOrderMass() == 0 and menu:GetTotalOrderPassengers() == 0, "absent seat menu has an order");
            assert(editor.EditorMode == SceneEditorGUI.INACTIVE, "absent seat editor changed mode");
            assert(editor:GetCurrentObject() == nil, "absent seat editor holds an object");
            local orders = 0;
            for _ in menu:GetOrderList() do
                orders = orders + 1;
            end
            assert(orders == 0, "absent seat menu has an order list");
        end
        print("[vessel-facts] tick=" .. readTick .. " player=" .. player .. " uid=" .. uid
            .. " screen=" .. self:ScreenOfPlayer(player) .. " " .. bannerFacts(banner)
            .. " menu=" .. (menu and 1 or 0) .. " editor=" .. (editor and 1 or 0)
            .. " buyvisible=" .. (self:IsBuyGUIVisible(player) and 1 or 0));
    end
    -- A seat index outside the roster is not a seat at all and stays nil on every peer.
    assert(self:GetBanner(GUIBanner.RED, Activity.MAXPLAYERCOUNT) == nil, "absent index banner");
    assert(self:GetBuyGUI(Activity.MAXPLAYERCOUNT) == nil, "absent index menu");
    assert(self:GetEditorGUI(Activity.MAXPLAYERCOUNT) == nil, "absent index editor");
    print("[vessel-facts] read seats=" .. Activity.MAXPLAYERCOUNT);
end
