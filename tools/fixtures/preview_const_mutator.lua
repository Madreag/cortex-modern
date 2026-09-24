-- A hook that kills the enemies and ends the activity from the preview's copy only. The real hook never does either, so
-- they can come only from a preview. Both are const methods: constness alone must not let a manager call run in one.
PreviewConstHandles = PreviewConstHandles or {}

-- A preview may not print, so its note waits in the stash the window never rolls back, and the real script prints it.
local NOTES = "preview-const:notes"

function Create(self)
	PreviewConstHandles[self.UniqueID] = self
end

function OnStride(self)
	if rawequal(PreviewConstHandles[self.UniqueID], self) then
		return
	end
	local killed = MovableMan:KillAllEnemyActors(self.Team)
	ActivityMan:EndActivity()
	if _ScriptFieldsStash then
		_ScriptFieldsStash[NOTES] = (_ScriptFieldsStash[NOTES] or "") .. string.format("[preview-const] preview uid=%d running=%s killed=%s\n",
			self.UniqueID, tostring(ActivityMan:ActivityRunning()), tostring(killed))
	end
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
