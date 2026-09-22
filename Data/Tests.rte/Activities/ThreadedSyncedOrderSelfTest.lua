function SyncedUpdate(self)
	_ThreadedSyncedUpdateAppend(self.UniqueID)
	self:SetNumberValue("threaded_synced_order_length", _ThreadedSyncedUpdateLength())
	-- The three writes Data/Modding/threaded-determinism.md blesses here: a global table, another
	-- object's field, and a spawn. The global is per state, so it is counted and never hashed.
	-- Folded on identity, not counted: the value says WHICH objects share this state's table, which is
	-- what the assignment decides and what a mod's shared counter would carry.
	_ThreadedSyncedGlobalWrites = (_ThreadedSyncedGlobalWrites or 0) + self.UniqueID
	-- Read straight back into this object: the value says which objects share the writer's state.
	self:SetNumberValue("threaded_synced_shared_counter", _ThreadedSyncedGlobalWrites)
	_ThreadedSyncedUpdateWitness(self.UniqueID, _ThreadedSyncedGlobalWrites)
	_ThreadedSyncedUpdateSpawn()
	_ThreadedSyncedUpdateRetire()
end
