-- ScreenBounds.lua — the presentation shape Void Wanderers uses: every player slot flashes and
-- writes its own screen, and a slot nobody sits in answers ScreenOfPlayer with -1. The scenario
-- reports what the in-range screens hold afterwards; it never asserts, so the same file runs on a
-- reference build and on a repaired one.

package.loaded.Constants = nil; require("Constants");
local Test = require("Lib/TestScenario");

TestScenarioScreenBounds = Test.Extend("ScreenBounds", { max_ticks = 120 });

local function zero(vector)
    return vector.X == 0 and vector.Y == 0;
end

local function same(left, right)
    return left.X == right.X and left.Y == right.Y;
end

local function show(vector)
    return string.format("%.2f/%.2f", vector.X, vector.Y);
end

local function copy(vector)
    return Vector(vector.X, vector.Y);
end

function TestScenarioScreenBounds:Report(name, passed, detail)
    if not passed then
        self._failures = self._failures + 1;
    end
    print("[screen-bounds] " .. (passed and "PASS " or "FAIL ") .. name .. ": " .. detail);
end

function TestScenarioScreenBounds:BadScreens()
    return {-1, -2, Activity.MAXPLAYERCOUNT, Activity.MAXPLAYERCOUNT + 1};
end

function TestScenarioScreenBounds:OnStart()
    self._failures = 0;
    self._remoteSlots = 0;

    -- A different value on every screen, so a write through a bad index has somewhere to land.
    for screen = 0, Activity.MAXPLAYERCOUNT - 1 do
        FrameMan:SetScreenText("screen " .. screen, screen, 0, 60000, false);
        FrameMan:SetHudDisabled(true, screen);
        FrameMan:FlashScreen(screen, 20 + screen, 1000);
    end

    -- Encounters_Basic.lua: one flash per player slot, whatever ScreenOfPlayer answered.
    for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
        local screen = self:ScreenOfPlayer(player);
        if screen < 0 then
            self._remoteSlots = self._remoteSlots + 1;
        end
        FrameMan:FlashScreen(screen, 13, 1000);
    end

    for _, screen in ipairs(self:BadScreens()) do
        FrameMan:FlashScreen(screen, 13, 1000);
        FrameMan:SetScreenText("out of range", screen, 250, 60000, true);
        FrameMan:ClearScreenText(screen);
        FrameMan:SetHudDisabled(true, screen);
        self:Report("screen_" .. screen .. "_hud_reads_neutral", not FrameMan:IsHudDisabled(screen),
            "IsHudDisabled " .. tostring(FrameMan:IsHudDisabled(screen)));
    end

    for screen = 0, Activity.MAXPLAYERCOUNT - 1 do
        self:Report("screen_" .. screen .. "_hud_unchanged", FrameMan:IsHudDisabled(screen),
            "IsHudDisabled " .. tostring(FrameMan:IsHudDisabled(screen)) .. " expected true");
    end

    self:RecordMetric("remote_slots", self._remoteSlots);
    print("[screen-bounds] frameman stage done, slots with no screen " .. self._remoteSlots);
end

function TestScenarioScreenBounds:OnTick(tick)
    if tick < 2 then
        return false, false;
    end

    -- One tick's worth of camera state: nothing else moves it between the two reads below.
    local offsets, targets, occlusions = {}, {}, {};
    for screen = 0, Activity.MAXPLAYERCOUNT - 1 do
        CameraMan:SetScreenOcclusion(Vector(screen + 1, -1 - screen), screen);
        offsets[screen] = copy(CameraMan:GetOffset(screen));
        targets[screen] = copy(CameraMan:GetScrollTarget(screen));
        occlusions[screen] = copy(CameraMan:GetScreenOcclusion(screen));
    end

    for _, screen in ipairs(self:BadScreens()) do
        CameraMan:SetOffset(Vector(100, 100), screen);
        CameraMan:SetScroll(Vector(100, 100), screen);
        CameraMan:SetScrollTarget(Vector(100, 100), 1, screen);
        CameraMan:SetScreenOcclusion(Vector(100, 100), screen);
        CameraMan:CheckOffset(screen);
        CameraMan:AddScreenShake(10, screen);
        self:Report("screen_" .. screen .. "_camera_reads_neutral",
            zero(CameraMan:GetOffset(screen)) and zero(CameraMan:GetRenderOffset(screen)) and
            zero(CameraMan:GetScrollTarget(screen)) and zero(CameraMan:GetScreenOcclusion(screen)),
            "offset " .. show(CameraMan:GetOffset(screen)) .. " render " .. show(CameraMan:GetRenderOffset(screen)) ..
            " target " .. show(CameraMan:GetScrollTarget(screen)) .. " occlusion " .. show(CameraMan:GetScreenOcclusion(screen)));
    end

    for screen = 0, Activity.MAXPLAYERCOUNT - 1 do
        self:Report("screen_" .. screen .. "_camera_unchanged",
            same(CameraMan:GetOffset(screen), offsets[screen]) and same(CameraMan:GetScrollTarget(screen), targets[screen]) and
            same(CameraMan:GetScreenOcclusion(screen), occlusions[screen]),
            "offset " .. show(offsets[screen]) .. " became " .. show(CameraMan:GetOffset(screen)) ..
            ", target " .. show(targets[screen]) .. " became " .. show(CameraMan:GetScrollTarget(screen)) ..
            ", occlusion " .. show(occlusions[screen]) .. " became " .. show(CameraMan:GetScreenOcclusion(screen)));
        self:Report("screen_" .. screen .. "_hud_still_unchanged", FrameMan:IsHudDisabled(screen),
            "IsHudDisabled " .. tostring(FrameMan:IsHudDisabled(screen)) .. " expected true");
    end

    return true, self._failures == 0;
end

function TestScenarioScreenBounds:OnEnd()
    self:RecordMetric("screen_bound_failures", self._failures);
    self._passed = self._failures == 0;
    print("[screen-bounds] " .. (self._passed and "PASS" or "FAIL") .. " failures=" .. self._failures);
end
