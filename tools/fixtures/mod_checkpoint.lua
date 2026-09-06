local function counter(shared)
	local count = 0;
	return function()
		count = count + 1;
		shared.count = count;
		return count;
	end, function() return count; end;
end

local function work(shared, scale)
	return function()
		local count = 0;
		while true do
			count = count + 1;
			coroutine.yield(count * scale + shared.count);
		end
	end;
end

function Create(self)
	self.testCarried = self:GetNumberValue("TestUpdates");
	self.testCreate = (self.testCreate or 0) + 1;
	self.testUpdate = 0;
	self.checkpoint = { count = 0 };
	self.checkpoint.self = self.checkpoint;
	self.checkpoint.alias = self.checkpoint;
	self.checkpoint.vector = Vector(1, 2);
	self.checkpoint.vector.shared = self.checkpoint;
	self.checkpoint.vectorAlias = self.checkpoint.vector;
	self.checkpoint.timer = Timer();
	self.checkpoint.timerAlias = self.checkpoint.timer;
	self.checkpoint.sound = CreateSoundContainer("Funds Changed", "Base.rte");
	self.checkpoint.soundAlias = self.checkpoint.sound;
	self.checkpoint.step, self.checkpoint.peek = counter(self.checkpoint);
	self.checkpoint.job = coroutine.create(work(self.checkpoint, 2));
	self.checkpoint.wrap = coroutine.wrap(work(self.checkpoint, 3));
	self.checkpoint.wrapAlias = self.checkpoint.wrap;
	self.checkpoint.match = string.gmatch(string.rep("alpha beta gamma ", 2048), "%a+");
end

function Update(self)
	local state = self.checkpoint;
	assert(state == state.self and state == state.alias, "checkpoint table identity");
	assert(state.vector == state.vectorAlias and state.vector.shared == state, "checkpoint vector identity");
	assert(state.timer == state.timerAlias, "checkpoint timer identity");
	assert(state.sound == state.soundAlias and state.sound:HasAnySounds(), "checkpoint sound identity");
	assert(state.wrap == state.wrapAlias, "checkpoint closure identity");
	local count = state.step();
	assert(count == self.testUpdate + 1 and state.peek() == count, "checkpoint shared upvalue");
	local ok, job = coroutine.resume(state.job);
	assert(ok and job == count * 3, "checkpoint coroutine continuation");
	local wrapped = state.wrap();
	assert(wrapped == count * 4, "checkpoint wrapped continuation");
	local word = state.match();
	assert(word == ({ "alpha", "beta", "gamma" })[(count - 1) % 3 + 1], "checkpoint iterator continuation");
	state.vector.X = state.vector.X + 1;
	assert(state.vectorAlias.X == count + 1, "checkpoint vector mutation");
	self.testUpdate = count;
	self:SetNumberValue("TestUpdates", count);
	self:SetNumberValue("TestCheckpointState", count + job + wrapped + state.vector.X + #word);
end
