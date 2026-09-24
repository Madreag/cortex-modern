-- 6.x compatibility bindings: reachable suite fixture for PlayMusic, Queue, Clear, IsMusicPlaying, GetOptionalArea, LimbPath.
local failed = {}
local function check(name, ok, detail)
	local extra = ""
	if not ok and detail ~= nil then extra = " " .. tostring(detail) end
	print(string.format("[mod-api-shims-selftest] %s %s%s", ok and "PASS" or "FAIL", name, extra))
	if not ok then
		failed[#failed + 1] = name .. extra
	end
end

MusicMan:ResetMusicState()
check("ismusicplaying_parity_idle", AudioMan:IsMusicPlaying() == MusicMan:IsMusicPlaying() and AudioMan:IsMusicPlaying() == false, "AudioMan=" .. tostring(AudioMan:IsMusicPlaying()) .. " MusicMan=" .. tostring(MusicMan:IsMusicPlaying()))

local musicOk, musicErr = pcall(function()
	AudioMan:ClearMusicQueue()
	AudioMan:PlayMusic("Base.rte/Music/dBSoundworks/cc2g.ogg", 0, -1)
end)
check("playmusic", musicOk, musicErr)
check("playmusic_starts_stream", AudioMan:IsMusicPlaying() == true, "AudioMan=" .. tostring(AudioMan:IsMusicPlaying()) .. " MusicMan=" .. tostring(MusicMan:IsMusicPlaying()))
check("playmusic_musicman_follows_audible_volume", MusicMan:IsMusicPlaying() == (AudioMan.MusicVolume > 0), "MusicMan=" .. tostring(MusicMan:IsMusicPlaying()) .. " volume=" .. tostring(AudioMan.MusicVolume))

local queueOk, queueErr = pcall(function()
	AudioMan:QueueMusicStream("Base.rte/Music/dBSoundworks/ruinexploration.ogg")
end)
check("queuemusicstream", queueOk, queueErr)
check("clearmusicqueue", pcall(AudioMan.ClearMusicQueue, AudioMan))
check("after_clear_stream_still_playing", AudioMan:IsMusicPlaying() == true, "AudioMan=" .. tostring(AudioMan:IsMusicPlaying()))
check("after_clear_musicman_follows_audible_volume", MusicMan:IsMusicPlaying() == (AudioMan.MusicVolume > 0), "MusicMan=" .. tostring(MusicMan:IsMusicPlaying()) .. " volume=" .. tostring(AudioMan.MusicVolume))

local registered = PresetMan:GetPreset("DynamicSong", "V6CompatMusicQueue", -1)
check("v6compat_preset_absent", registered == nil, registered)

local scene = CreateScene("Alezer Canyon")
check("getoptionalarea_missing_is_nil", scene ~= nil and scene:GetOptionalArea("No Such Area") == nil and scene:HasArea("No Such Area") == false)
check("getoptionalarea_returns_area", scene ~= nil and scene:GetOptionalArea("LZ Team 1") ~= nil and scene:GetOptionalArea("LZ Team 1").Name == "LZ Team 1")

local actor = CreateAHuman("Green Dummy")
local baseSpeed
local limbOk, limbErr = pcall(function()
	baseSpeed = actor:GetLimbPathSpeed(1)
	actor:SetLimbPathSpeed(1, baseSpeed * 0.5)
end)
check("getsetlimpathspeed_methods", limbOk, limbErr)
check("getsetlimpathspeed_roundtrip", limbOk and type(baseSpeed) == "number" and math.abs(actor:GetLimbPathSpeed(1) - baseSpeed * 0.5) < 0.0001, baseSpeed)

-- LuaMan:WriteLine is FileWriteLine under the name Void Wanderers' Lib_Config.lua calls; both lines land in order.
local writePath = "UserSavedGames.rte/mod-api-shims-writeline.txt"
local writeFile = LuaMan:FileOpen(writePath, "w")
local writeOk, writeErr = pcall(function()
	LuaMan:FileWriteLine(writeFile, "first=1\n")
	LuaMan:WriteLine(writeFile, "second=2\n")
end)
LuaMan:FileClose(writeFile)
check("writeline", writeFile >= 0 and writeOk, writeErr)
local firstLine, secondLine
local readFile = LuaMan:FileOpen(writePath, "r")
local readOk, readErr = pcall(function()
	firstLine = LuaMan:FileReadLine(readFile)
	secondLine = LuaMan:FileReadLine(readFile)
end)
LuaMan:FileClose(readFile)
check("writeline_matches_filewriteline", writeOk and readOk and firstLine == "first=1\n" and secondLine == "second=2\n", tostring(readErr) .. " first=" .. tostring(firstLine) .. " second=" .. tostring(secondLine))
if LuaMan:FileExists(writePath) then
	LuaMan:FileRemove(writePath)
end

if actor then
	DeleteEntity(actor)
	actor = nil
end
if scene then
	DeleteEntity(scene)
	scene = nil
end

if #failed > 0 then
	error(table.concat(failed, "; "))
end
