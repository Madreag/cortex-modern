-- Ordinary mod code covering the SoundContainer/SoundSet surfaces the repair lane's fixture did not
-- measure on the pre-generation reference: mutating Vector methods through the Pos alias, the
-- structural SoundSet mutators, SetTopLevelSoundSet, AudioMan:StopAll, a property read back in a
-- later AI pass, and one SoundContainer written by two different actors' AI hooks.
--
-- Nothing here knows about domains, cohorts or deferral. Each case prints one [extra] line with the
-- observed values; the verdict is the line-by-line comparison against the reference executable.

local SHORT = "UserScenes.rte/ProbeShort.flac"
local LONG = "UserScenes.rte/ProbeLong.flac"

local function num(v)
	if v == nil then return "nil" end
	return string.format("%.17g", v)
end

local function flag(v)
	if v == nil then return "nil" end
	if type(v) == "boolean" then return v and "1" or "0" end
	return tostring(v)
end

local function say(case, detail)
	print(string.format("[extra] case=%s %s", case, detail))
end

local function makeSound(path, loops, overlap, immobile)
	local sound = SoundContainer()
	local set = SoundSet()
	set:AddSound(path)
	sound:SetTopLevelSoundSet(set)
	sound.Immobile = immobile
	sound.Loops = loops
	sound.Volume = 1
	sound.Pitch = 1
	sound.PitchVariation = 0
	sound.AffectedByGlobalPitch = false
	sound.SoundOverlapMode = overlap
	sound.BusRouting = SoundContainer.SFX
	return sound
end

function Create(self)
end

function Update(self)
	if not IsAHuman(self) then return end
	if _XClaim == nil then return end
	if self.UniqueID ~= _XClaim then return end
	if not self.xOwner then return end

	self.xStep = (self.xStep or 0) + 1
	local step = self.xStep

	if step == 1 then
		self.xPos = makeSound(SHORT, 0, SoundContainer.OVERLAP, false)
		self.xMethod = makeSound(SHORT, 0, SoundContainer.OVERLAP, false)
		self.xSet = makeSound(SHORT, 0, SoundContainer.OVERLAP, true)
		self.xCycle = makeSound(SHORT, 0, SoundContainer.OVERLAP, true)
		self.xTop = makeSound(SHORT, 0, SoundContainer.OVERLAP, true)
		self.xStopAll = makeSound(LONG, -1, SoundContainer.OVERLAP, true)
		self.xLater = makeSound(SHORT, 0, SoundContainer.OVERLAP, true)
		_XShared = makeSound(SHORT, 0, SoundContainer.OVERLAP, true)
		_XShared.Volume = 0.5
		print(string.format("[extra] claim actor=%d", self.UniqueID))
		return
	end
	if self.xPos == nil then return end

	-- ---- shared-scope arm: the mutating Vector surface through the Pos alias ------------------
	if step == 4 then
		local s = self.xPos
		s.Pos = Vector(1, 2)
		local ok1 = pcall(function() s.Pos:SetXY(51.5, 52.5) end)
		say("shared_pos_setxy", "call_ok=" .. flag(ok1) ..
			" x=" .. num(s.Pos.X) .. " y=" .. num(s.Pos.Y))
	elseif step == 5 then
		local s = self.xMethod
		s.Pos = Vector(3, 4)
		local p = s.Pos
		local ok1 = pcall(function() p:SetXY(10, 20) end)
		local ok2 = pcall(function() p:FlipX() end)
		local ok3 = pcall(function() p:SetMagnitude(50) end)
		say("shared_pos_methods", "setxy=" .. flag(ok1) .. " flipx=" .. flag(ok2) ..
			" setmag=" .. flag(ok3) .. " x=" .. num(s.Pos.X) .. " y=" .. num(s.Pos.Y) ..
			" mag=" .. num(s.Pos.Magnitude))
	elseif step == 6 then
		local s = self.xPos
		s.Pos = Vector(6, 8)
		local ok1 = pcall(function() s.Pos.AbsRadAngle = 0.0 end)
		say("shared_pos_absradangle", "call_ok=" .. flag(ok1) ..
			" x=" .. num(s.Pos.X) .. " y=" .. num(s.Pos.Y))
	elseif step == 7 then
		-- Structural SoundSet mutation from the shared hook: the control arm for the same calls
		-- made from the AI hook later.
		local s = self.xSet
		local before = s:GetTopLevelSoundSet():HasAnySounds(true)
		s:GetTopLevelSoundSet():AddSound(LONG)
		local removed = s:GetTopLevelSoundSet():RemoveSound(LONG)
		say("shared_soundset_structural", "before=" .. flag(before) ..
			" removed=" .. flag(removed) .. " any=" .. flag(s:HasAnySounds()))
	elseif step == 8 then
		local s = self.xCycle
		s:GetTopLevelSoundSet().SoundSelectionCycleMode = SoundSet.ALL
		say("shared_soundset_cyclemode", "mode=" .. flag(s:GetTopLevelSoundSet().SoundSelectionCycleMode))
	elseif step == 20 then
		self.xStopAllPlayed = self.xStopAll:Play()
		say("shared_stopall_setup", "played=" .. flag(self.xStopAllPlayed) ..
			" playing=" .. flag(self.xStopAll:IsBeingPlayed()))
	end

	if step <= 20 then return end

	-- ---- the AI-hook results, read from the shared hook ---------------------------------------
	if self.xAiSetXY and not self.xAiSetXYReported then
		self.xAiSetXYReported = true
		say("ai_pos_setxy", "ai_ok=" .. flag(self.xAiSetXYOk) .. " ai_x=" .. num(self.xAiSetXYX) ..
			" ai_y=" .. num(self.xAiSetXYY) .. " upd_x=" .. num(self.xPos.Pos.X) ..
			" upd_y=" .. num(self.xPos.Pos.Y))
	end
	if self.xAiAliasMethodLate and not self.xAiAliasMethodReported then
		self.xAiAliasMethodReported = true
		say("ai_pos_retained_alias_method", "ai_x=" .. num(self.xAiAliasMethodX) ..
			" upd_x=" .. num(self.xMethod.Pos.X) .. " upd_y=" .. num(self.xMethod.Pos.Y))
	end
	if self.xAiStructural and not self.xAiStructuralReported then
		self.xAiStructuralReported = true
		say("ai_soundset_addsound", "ai_ok=" .. flag(self.xAiStructuralOk) ..
			" ai_any=" .. flag(self.xAiStructuralAny) .. " upd_any=" .. flag(self.xSet:HasAnySounds()))
	end
	if self.xAiCycle and not self.xAiCycleReported then
		self.xAiCycleReported = true
		say("ai_soundset_cyclemode", "ai_mode=" .. flag(self.xAiCycleMode) ..
			" upd_mode=" .. flag(self.xCycle:GetTopLevelSoundSet().SoundSelectionCycleMode))
	end
	if self.xAiTop and not self.xAiTopReported then
		self.xAiTopReported = true
		say("ai_set_top_level_sound_set", "ai_ok=" .. flag(self.xAiTopOk) ..
			" ai_any=" .. flag(self.xAiTopAny) .. " upd_any=" .. flag(self.xTop:HasAnySounds()))
	end
	if self.xAiLaterWrote and not self.xAiLaterReported and self.xAiLaterRead ~= nil then
		self.xAiLaterReported = true
		say("ai_property_next_pass", "ai_same_pass=" .. num(self.xAiLaterSame) ..
			" ai_next_pass=" .. num(self.xAiLaterRead) .. " upd=" .. num(self.xLater.Volume))
	end
	if self.xAiStopAll and not self.xAiStopAllReported then
		self.xAiStopAllReported = true
		say("ai_audioman_stopall", "ai_playing_after=" .. flag(self.xAiStopAllSeen) ..
			" upd_playing=" .. flag(self.xStopAll:IsBeingPlayed()))
	end
	if step == 130 and not self.xSharedReported then
		self.xSharedReported = true
		say("two_actor_shared_container", "volume=" .. num(_XShared.Volume) ..
			" writers=" .. flag(_XSharedWriters) .. " primary=" .. flag(_XSharedPrimary) ..
			" secondary=" .. flag(_XSharedSecondary))
	end
	if step == 140 then
		say("done", "step=" .. step .. " ai_runs=" .. flag(self.xAiRuns))
	end
end

function UpdateAI(self)
	if not IsAHuman(self) then return end
	if _XClaim == nil then _XClaim = self.UniqueID end
	if _XClaim ~= self.UniqueID then
		-- The second AI actor writes the one shared container too.
		if _XSecond == nil then _XSecond = self.UniqueID end
		if _XSecond ~= self.UniqueID then return end
		if _XShared ~= nil and (_XSharedSecondary or 0) < 40 then
			_XShared.Volume = 0.875
			_XSharedSecondary = (_XSharedSecondary or 0) + 1
			_XSharedWriters = 2
		end
		return
	end
	self.xOwner = true
	self.xAiRuns = (self.xAiRuns or 0) + 1
	if self.xPos == nil then return end
	local step = self.xStep or 0
	if step <= 20 then return end

	if _XShared ~= nil and (_XSharedPrimary or 0) < 40 then
		_XShared.Volume = 0.125
		_XSharedPrimary = (_XSharedPrimary or 0) + 1
	end

	if not self.xAiSetXY then
		self.xAiSetXY = true
		local s = self.xPos
		s.Pos = Vector(5, 6)
		self.xAiSetXYOk = pcall(function() s.Pos:SetXY(61.5, 62.5) end)
		self.xAiSetXYX = s.Pos.X
		self.xAiSetXYY = s.Pos.Y
		return
	end

	if not self.xAiAliasMethod then
		self.xAiAliasMethod = true
		local s = self.xMethod
		s.Pos = Vector(7, 8)
		self.xAiHeldAlias = s.Pos
		self.xAiAliasStep = step
		return
	end

	if not self.xAiStructural then
		self.xAiStructural = true
		local s = self.xSet
		self.xAiStructuralOk = pcall(function() s:GetTopLevelSoundSet():AddSound(LONG) end)
		self.xAiStructuralAny = s:HasAnySounds()
		return
	end

	if not self.xAiCycle then
		self.xAiCycle = true
		local s = self.xCycle
		s:GetTopLevelSoundSet().SoundSelectionCycleMode = SoundSet.FORWARDS
		self.xAiCycleMode = s:GetTopLevelSoundSet().SoundSelectionCycleMode
		return
	end

	if not self.xAiTop then
		self.xAiTop = true
		local s = self.xTop
		local replacement = SoundSet()
		replacement:AddSound(LONG)
		self.xAiTopOk = pcall(function() s:SetTopLevelSoundSet(replacement) end)
		self.xAiTopAny = s:HasAnySounds()
		return
	end

	if not self.xAiLaterWrote then
		self.xAiLaterWrote = true
		self.xLater.Volume = 0.375
		self.xAiLaterSame = self.xLater.Volume
		return
	end
	if self.xAiLaterWrote and self.xAiLaterRead == nil then
		self.xAiLaterRead = self.xLater.Volume
		return
	end

	if not self.xAiAliasMethodLate and self.xAiAliasStep and step >= self.xAiAliasStep + 25 then
		self.xAiAliasMethodLate = true
		pcall(function() self.xAiHeldAlias:SetXY(91.5, 92.5) end)
		self.xAiAliasMethodX = self.xMethod.Pos.X
		return
	end

	if not self.xAiStopAll and step >= 95 then
		self.xAiStopAll = true
		AudioMan:StopAll()
		self.xAiStopAllSeen = self.xStopAll:IsBeingPlayed()
		return
	end
end
