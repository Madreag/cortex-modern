-- A mod reuses a file-scope Vector and chains on a call that changes it. Inside a preview that call is dropped and
-- returns nil, so the chain fails: the preview's hook stops there, reported once, and the real hook runs it at commit.
local scratch = Vector(1, 0)
PreviewNilHandles = PreviewNilHandles or {}

-- A preview may not print, so its note waits in the stash the window never rolls back, and the real script prints it.
local NOTES = "preview-nil:notes"

function Create(self)
	PreviewNilHandles[self.UniqueID] = self
end

function OnStride(self)
	if not rawequal(PreviewNilHandles[self.UniqueID], self) and _ScriptFieldsStash then
		_ScriptFieldsStash[NOTES] = (_ScriptFieldsStash[NOTES] or "") .. string.format("[preview-nil] preview uid=%d\n", self.UniqueID)
	end
	self.strideReach = scratch:SetMagnitude(3).X
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
