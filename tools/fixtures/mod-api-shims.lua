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

local musicOk, musicErr = pcall(function()
	AudioMan:ClearMusicQueue()
	AudioMan:PlayMusic("Base.rte/Music/dBSoundworks/cc2g.ogg", 0, -1)
end)
check("playmusic", musicOk, musicErr)
check("ismusicplaying_parity_playing", AudioMan:IsMusicPlaying() == MusicMan:IsMusicPlaying(), "AudioMan=" .. tostring(AudioMan:IsMusicPlaying()) .. " MusicMan=" .. tostring(MusicMan:IsMusicPlaying()))

local queueOk, queueErr = pcall(function()
	AudioMan:QueueMusicStream("Base.rte/Music/dBSoundworks/ruinexploration.ogg")
end)
check("queuemusicstream", queueOk, queueErr)
check("clearmusicqueue", pcall(AudioMan.ClearMusicQueue, AudioMan))
check("ismusicplaying_parity_after_clear", AudioMan:IsMusicPlaying() == MusicMan:IsMusicPlaying(), "AudioMan=" .. tostring(AudioMan:IsMusicPlaying()) .. " MusicMan=" .. tostring(MusicMan:IsMusicPlaying()))

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

if #failed > 0 then
	error(table.concat(failed, "; "))
end
