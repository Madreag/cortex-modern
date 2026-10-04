-- A mod sequences its rifle's shots with coroutines: OnFire resumes one made with coroutine.create and one made with
-- coroutine.wrap, each leaving its step on the gun, and the step wears the holder down. On the predicting peer a
-- preview's OnFire runs this code on the preview's copy: the copy's coroutines go on from where the real ones stand,
-- and the real ones move only when the shot is real.
PreviewCoroutineHandles = PreviewCoroutineHandles or {}

-- A preview may not print, so its note waits in the stash the window never rolls back, and the real script prints it.
local NOTES = "preview-coroutine:notes"

local function sequence(gun)
	local step = 0
	while true do
		step = step + 1
		gun.createdStep = step
		coroutine.yield(step)
	end
end

function Create(self)
	PreviewCoroutineHandles[self.UniqueID] = self
	self.createdStep = 0
	self.wrappedStep = 0
	self.sequence = coroutine.create(sequence)
	local shots = 0
	self.wrapped = coroutine.wrap(function()
		while true do
			shots = shots + 1
			self.wrappedStep = shots
			coroutine.yield(shots)
		end
	end)
end

function OnFire(self)
	local real = PreviewCoroutineHandles[self.UniqueID]
	local _, created = coroutine.resume(self.sequence, self)
	local wrapped = self.wrapped()
	local holder = self:GetRootParent()
	if holder and IsActor(holder) and type(created) == "number" then
		ToActor(holder).Health = 100 - created * 0.01
	end
	if not rawequal(real, self) then
		if _ScriptFieldsStash then
			_ScriptFieldsStash[NOTES] = (_ScriptFieldsStash[NOTES] or "") .. string.format("[preview-coroutine] preview uid=%d created=%s wrapped=%s self_created=%s self_wrapped=%s real_created=%s real_wrapped=%s\n",
				self.UniqueID, tostring(created), tostring(wrapped), tostring(self.createdStep), tostring(self.wrappedStep),
				tostring(real and real.createdStep), tostring(real and real.wrappedStep))
		end
		return
	end
	print(string.format("[preview-coroutine] commit uid=%d created=%s wrapped=%s", self.UniqueID, tostring(created), tostring(wrapped)))
end

function Update(self)
	local notes = _ScriptFieldsStash and _ScriptFieldsStash[NOTES]
	if notes then
		_ScriptFieldsStash[NOTES] = nil
		for line in string.gmatch(notes, "[^\n]+") do
			print(line)
		end
	end
end
