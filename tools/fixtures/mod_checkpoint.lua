math._CheckpointState = math._CheckpointState or {};

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

local function configure(owned, count)
	local values = {
		Scale = 1.25, GlobalAccScalar = 0.25 + count / 1024,
		AirResistance = 0.25, AirThreshold = 2.5, Sharpness = 3.5,
		HitsMOs = true, GetsHitByMOs = false, IgnoresTeamHits = false,
		IgnoresActorHits = true, IgnoreTerrain = true, ToSettle = true,
		ToDelete = count % 2 == 1, MissionCritical = true, HUDVisible = false,
		PinStrength = 33, RestThreshold = 177, DamageOnCollision = 2.5,
		DamageOnPenetration = 3.75, WoundDamageMultiplier = 0.5,
		ApplyWoundDamageOnCollision = true, ApplyWoundBurstDamageOnCollision = false,
		SimUpdatesBetweenScriptedUpdates = 7, PostEffectEnabled = true,
		EffectRotAngle = 0.125 + count / 1024, EffectAlwaysShows = true,
		EffectStartStrength = 0.75, EffectStopStrength = 0.25,
		SpriteAnimMode = MOSprite.ALWAYSLOOP, SpriteAnimDuration = 1536,
		GibImpulseLimit = 1000 + count, GibWoundLimit = 77,
		GibAtEndOfLifetime = true, OrientToVel = 0.375, DamageMultiplier = 0.625,
	};
	for key, value in pairs(values) do
		owned[key] = value;
	end
	for key in pairs(values) do
		values[key] = owned[key];
	end
	return values;
end

function Create(self)
	self.testCarried = self:GetNumberValue("TestUpdates");
	self.testCreate = (self.testCreate or 0) + 1;
	self.testUpdate = 0;
	self.checkpoint = { count = 0 };
	self.checkpoint.self = self.checkpoint;
	self.checkpoint.alias = self.checkpoint;
	self.checkpoint.engineState = math._CheckpointState;
	math._CheckpointState[self.UniqueID] = 0;
	self.checkpoint.vector = Vector(1, 2);
	self.checkpoint.vector.shared = self.checkpoint;
	self.checkpoint.vectorAlias = self.checkpoint.vector;
	self.checkpoint.timer = Timer();
	self.checkpoint.timerAlias = self.checkpoint.timer;
	self.checkpoint.sound = CreateSoundContainer("Funds Changed", "Base.rte");
	self.checkpoint.soundAlias = self.checkpoint.sound;
	self.checkpoint.owned = CreateAHuman("Green Dummy", "Base.rte");
	self.checkpoint.owned.Pos = Vector(10, 20);
	self.checkpoint.owned.GlobalAccScalar = 0.25;
	self.checkpoint.owned.GibImpulseLimit = 1000;
	self.checkpoint.configuration = configure(self.checkpoint.owned, 0);
	self.checkpoint.owned:SetNumberValue("CheckpointCount", 0);
	self.checkpoint.owned.shared = self.checkpoint;
	self.checkpoint.ownedAlias = self.checkpoint.owned;
	self.checkpoint.ownedUID = self.checkpoint.owned.UniqueID;
	self.checkpoint.ownedPos = self.checkpoint.owned.Pos;
	self.checkpoint.ownedController = self.checkpoint.owned:GetController();
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
	assert(state.engineState == math._CheckpointState and state.engineState[self.UniqueID] == self.testUpdate, "checkpoint engine table state");
	assert(state.owned == state.ownedAlias and state.owned.shared == state, "checkpoint owned object identity");
	assert(state.owned.UniqueID == state.ownedUID and not MovableMan:IsActor(state.owned), "checkpoint owned object lifetime");
	assert(state.owned:GetNumberValue("CheckpointCount") == self.testUpdate, "checkpoint owned object state");
	assert(state.ownedPos.X == self.testUpdate + 10, "checkpoint owned field alias");
	assert(state.ownedController:IsState(Controller.WEAPON_FIRE) == (self.testUpdate % 2 == 1), "checkpoint owned controller state");
	assert(state.owned.GlobalAccScalar == 0.25 + self.testUpdate / 1024, "checkpoint owned acceleration setting");
	assert(state.owned.GibImpulseLimit == 1000 + self.testUpdate, "checkpoint owned gib setting");
	for key, value in pairs(state.configuration) do
		assert(state.owned[key] == value, "checkpoint owned configuration " .. key);
	end
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
	state.engineState[self.UniqueID] = count;
	state.owned:SetNumberValue("CheckpointCount", count);
	state.ownedPos.X = count + 10;
	state.ownedController:SetState(Controller.WEAPON_FIRE, count % 2 == 1);
	state.owned.GlobalAccScalar = 0.25 + count / 1024;
	state.owned.GibImpulseLimit = 1000 + count;
	state.configuration = configure(state.owned, count);
	self:SetNumberValue("TestUpdates", count);
	self:SetNumberValue("TestCheckpointState", count + job + wrapped + state.vector.X + #word);
end
