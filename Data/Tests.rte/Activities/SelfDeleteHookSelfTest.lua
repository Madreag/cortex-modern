function SyncedUpdate(self)
	_ThreadedSyncedUpdateAppend(self.UniqueID)
	-- What a mod does when an object has finished its job: the engine's own script-facing deletion,
	-- reached from inside this object's own hook (Base.rte's Harvester deletes ships this way).
	_ThreadedSyncedUpdateDeleteSelf(self.UniqueID)
end
