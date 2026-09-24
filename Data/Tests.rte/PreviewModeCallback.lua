-- Counts every run of the mode-change callback where a preview window's rollback cannot undo it.
function OnControllerInputModeChange(self, previousMode, previousPlayer)
	_ScriptFieldsStash = _ScriptFieldsStash or {}
	_ScriptFieldsStash["preview-mode-callback"] = (_ScriptFieldsStash["preview-mode-callback"] or 0) + 1
end
