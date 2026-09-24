-- A mod keeps a spare attachable in a global. On the preview's copy its stride hook attaches that spare to the actor,
-- so the call the preview makes on its own copy would take an object the real script keeps.
PreviewArgumentHandles = PreviewArgumentHandles or {}

-- A preview may not print, so its note waits in the stash the window never rolls back, and the real script prints it.
local NOTES = "preview-argument:notes"

function Create(self)
	PreviewArgumentHandles[self.UniqueID] = self
	if PreviewArgumentSpare == nil then
		PreviewArgumentSpare = CreateAttachable("Grenade Bandolier", "Base.rte")
	end
end

function OnStride(self)
	local spare = PreviewArgumentSpare
	if spare == nil or rawequal(PreviewArgumentHandles[self.UniqueID], self) then
		return
	end
	self:AddAttachable(spare)
	if _ScriptFieldsStash then
		_ScriptFieldsStash[NOTES] = (_ScriptFieldsStash[NOTES] or "") .. string.format("[preview-argument] preview uid=%d attached=%s\n",
			self.UniqueID, tostring(spare:IsAttached()))
	end
end

function Update(self)
	-- The spare steers the sim: health says whether it is attached anywhere, so an attach only one peer made shows.
	local spare = PreviewArgumentSpare
	self.Health = (spare ~= nil and spare:IsAttached()) and 99 or 100
	local notes = _ScriptFieldsStash and _ScriptFieldsStash[NOTES]
	if notes then
		_ScriptFieldsStash[NOTES] = nil
		for line in string.gmatch(notes, "[^\n]+") do
			print(line)
		end
	end
end
