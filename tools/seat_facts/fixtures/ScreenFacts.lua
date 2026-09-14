dofile("UserScenes.rte/SeatFacts.lua");

local function zero(value)
    return value.X == 0 and value.Y == 0;
end

local function same(left, right)
    return left.X == right.X and left.Y == right.Y;
end

function ScreenFacts:StartActivity(startNewGame)
    SeatFacts.StartActivity(self, startNewGame);
    print("[screen-facts] begin");
    local offset = CameraMan:GetOffset(0);
    local target = CameraMan:GetScrollTarget(0);
    local currentOcclusion = CameraMan:GetScreenOcclusion(0);
    local occlusion = Vector(currentOcclusion.X, currentOcclusion.Y);
    local hud = FrameMan:IsHudDisabled(0);
    for _, screen in ipairs({-1, Activity.MAXPLAYERCOUNT}) do
        FrameMan:SetScreenText("absent screen", screen, 0, 1, false);
        FrameMan:ClearScreenText(screen);
        FrameMan:FlashScreen(screen, 13, 1);
        FrameMan:SetHudDisabled(true, screen);
        assert(not FrameMan:IsHudDisabled(screen), "absent screen HUD");
        CameraMan:SetOffset(Vector(100, 100), screen);
        CameraMan:SetScroll(Vector(100, 100), screen);
        CameraMan:SetScrollTarget(Vector(100, 100), 1, screen);
        CameraMan:SetScreenOcclusion(Vector(100, 100), screen);
        CameraMan:CheckOffset(screen);
        CameraMan:AddScreenShake(10, screen);
        assert(zero(CameraMan:GetOffset(screen)), "absent screen offset");
        assert(zero(CameraMan:GetRenderOffset(screen)), "absent screen render offset");
        assert(zero(CameraMan:GetScrollTarget(screen)), "absent screen target");
        assert(zero(CameraMan:GetScreenOcclusion(screen)), "absent screen occlusion");
    end
    assert(same(CameraMan:GetOffset(0), offset), "local offset changed");
    assert(same(CameraMan:GetScrollTarget(0), target), "local target changed");
    assert(same(CameraMan:GetScreenOcclusion(0), occlusion), "local occlusion changed");
    assert(FrameMan:IsHudDisabled(0) == hud, "local HUD changed");
    assert(FrameMan.PlayerScreenWidth > 0 and FrameMan.PlayerScreenHeight > 0, "local dimensions");

    local remoteCount = 0;
    local sound = SoundContainer();
    sound.BusRouting = SoundContainer.UI;
    for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
        if self:PlayerHuman(player) and self:ScreenOfPlayer(player) == -1 then
            remoteCount = remoteCount + 1;
            local brain = self:GetPlayerBrain(player);
            self:SetObservationTarget(Vector(12, 34), player);
            self:SetDeathViewTarget(Vector(12, 34), player);
            self:SetLandingZone(Vector(12, 34), player);
            self:SetActorSelectCursor(Vector(12, 34), player);
            self:SetBrainLZWidth(player, 100);
            self:SetViewState(Activity.NORMAL, player);
            self:ResetMessageTimer(player);
            self:ClearOverridePurchase(player);
            assert(self:GetViewState(player) == Activity.OBSERVE, "remote view");
            assert(zero(self:GetLandingZone(player)), "remote landing zone");
            assert(self:GetBrainLZWidth(player) == 0, "remote landing width");
            assert(self:GetBuyGUI(player) == nil and self:GetEditorGUI(player) == nil, "remote GUI");
            assert(self:GetBanner(0, player) == nil, "remote banner");
            assert(not self:IsBuyGUIVisible(player), "remote buy visibility");
            assert(not self:CreateDelivery(player), "remote delivery input");
            assert(self:GetPlayerBrain(player) == brain and brain ~= nil, "shared brain changed");
            sound:Play(player);
            sound:Play(Vector(0, 0), player);
            sound:Stop(player);
            sound:Restart(player);
        end
    end
    assert(remoteCount == 1, "missing remote human branch");
    print("[screen-facts] pass remote=" .. remoteCount);
end

function ScreenFacts:UpdateActivity()
    SeatFacts.UpdateActivity(self);
end
