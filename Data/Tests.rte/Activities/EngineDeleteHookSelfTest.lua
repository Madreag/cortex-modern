function SyncedUpdate(self)
	_ThreadedSyncedUpdateAppend(self.UniqueID)
	-- Not the script-facing deletion: an engine path that frees the object where the script stands.
	_ThreadedSyncedUpdateEngineDeleteSelf(self.UniqueID)
end
