-- One of the checkpoint cost driver's load objects: a script table holding a count and a native value.
function Create(self)
	self.load = {ticks = 0, uid = self.UniqueID, home = Vector(self.Pos.X, self.Pos.Y)};
end

function ThreadedUpdate(self)
	self.load.ticks = self.load.ticks + 1;
end
