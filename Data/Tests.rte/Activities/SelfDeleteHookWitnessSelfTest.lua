-- The second script of the same object: the hook loop reaches it after the first one deleted the object.
function SyncedUpdate(self)
	_ThreadedSyncedUpdateAliveHere(self.UniqueID)
	_ThreadedSyncedUpdateAppend(-self.UniqueID)
end
