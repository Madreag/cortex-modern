function SyncedUpdate(self)
	_ThreadedSyncedUpdateAppend(self.UniqueID)
	self:SetNumberValue("threaded_synced_order_length", _ThreadedSyncedUpdateLength())
end
