-- 6.x compatibility bindings: reachable suite fixture for PlayMusic, Queue, Clear, IsMusicPlaying, GetOptionalArea, LimbPath.
local function check(name, ok, detail)
	local extra = ""
	if not ok and detail ~= nil then extra = " " .. tostring(detail) end
	print(string.format("[mod-api-shims-selftest] %s %s%s", ok and "PASS" or "FAIL", name, extra))
end

local musicOk, musicErr = pcall(function()
	AudioMan:ClearMusicQueue()
	AudioMan:PlayMusic("Base.rte/Music/dBSoundworks/cc2g.ogg", 0, -1)
	AudioMan:QueueMusicStream("Base.rte/Music/dBSoundworks/ruinexploration.ogg")
end)
check("playmusic", musicOk, musicErr)
check("queuemusicstream", musicOk, musicErr)
check("clearmusicqueue", pcall(AudioMan.ClearMusicQueue, AudioMan))

local audioPlaying = AudioMan:IsMusicPlaying()
local musicPlaying = MusicMan:IsMusicPlaying()
check("ismusicplaying_parity", audioPlaying == musicPlaying, "AudioMan=" .. tostring(audioPlaying) .. " MusicMan=" .. tostring(musicPlaying))

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
