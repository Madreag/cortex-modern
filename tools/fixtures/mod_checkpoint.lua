math._CheckpointState = math._CheckpointState or {};

local function counter(shared)
	local count = 0;
	return function()
		count = count + 1;
		shared.count = count;
		return count;
	end, function() return count; end;
end

local function checked(fn)
	return function(self)
		local ok, message = pcall(fn, self);
		if not ok then
			ConsoleMan:PrintString("ERROR: " .. tostring(message));
			ConsoleMan:SaveAllText("Userdata/CheckpointErrors.txt");
			error(message);
		end
	end;
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

local function configureActor(owned, count)
	local values = {
		PlayerControllable = false, ImpulseDamageThreshold = 2400 + count,
		StableRecoveryDelay = 177, CanRun = true, CrouchWalkSpeedMultiplier = 0.625,
		PassengerSlots = 4, Perceptiveness = 0.375, PainThreshold = 23,
		CanRevealUnseen = false, AimRange = 0.75, AimDistance = 245,
		SightDistance = 333, AIBaseDigStrength = 123, MoveProximityLimit = 12.5,
		LimbPushForcesAndCollisionsDisabled = true, HolsterOffset = Vector(7, 9),
		ReloadOffset = Vector(-3, 6), ArmSwingRate = 1.25, DeviceArmSwayRate = 0.625,
		ThrowPrepTime = 444, MaxWalkPathCrouchShift = 3.75,
		CrouchAmountOverride = 0.375, ProneState = AHuman.GOPRONE,
		UpperBodyState = AHuman.AIMING_SHARP, MovementState = Actor.CROUCH,
		Status = Actor.UNSTABLE, Health = 77, MaxHealth = 222,
		GoldCarried = 12.5 + count / 16, ViewPoint = Vector(123, 234), AIMode = Actor.AIMODE_SQUAD,
	};
	for key, value in pairs(values) do
		owned[key] = value;
	end
	for key in pairs(values) do
		local value = owned[key];
		values[key] = type(value) == "userdata" and Vector(value.X, value.Y) or value;
	end
	return values;
end

local function configureDevice(owned, count)
	local values = {
		RateOfFire = 777 + count, FullAuto = true, Reloadable = true, DualReloadable = false,
		OneHandedReloadTimeMultiplier = 1.75, ReloadAngle = 0.25, OneHandedReloadAngle = 0.375,
		ActivationDelay = 55, DeactivationDelay = 77, BaseReloadTime = 987,
		ShakeRange = 0.125, SharpShakeRange = 0.375, NoSupportFactor = 2.5,
		ParticleSpreadRange = 0.75, ShellVelVariation = 0.25, IsAnimatedManually = true,
		RecoilTransmission = 0.25, ReloadEndOffset = 0.375, MuzzleOffset = Vector(7, 9),
		EjectionOffset = Vector(3, -2), StanceOffset = Vector(4, 5), SharpStanceOffset = Vector(6, 7),
		SupportOffset = Vector(-4, 3), Supportable = true, Supported = true, SharpLength = 123,
		UseSupportOffsetWhileReloading = true, UnPickupable = true, GripStrengthMultiplier = 1.5,
		GetsHitByMOsWhenHeld = true, VisualRecoilMultiplier = 0.875,
		ApplyTransferredForcesAtOffset = false, IgnoresParticlesWhileAttached = true,
		InheritsVelWhenDetached = 0.625, InheritsAngularVelWhenDetached = 0.875,
	};
	for key, value in pairs(values) do
		owned[key] = value;
	end
	for key in pairs(values) do
		local value = owned[key];
		values[key] = type(value) == "userdata" and Vector(value.X, value.Y) or value;
	end
	return values;
end

local function verifyConfiguration(owned, values)
	for key, value in pairs(values) do
		local actual = owned[key];
		if type(value) == "userdata" then
			assert(actual.X == value.X and actual.Y == value.Y, "checkpoint owned configuration " .. key);
		else
			assert(actual == value, "checkpoint owned configuration " .. key);
		end
	end
end

function Create(self)
	self.testCarried = self:GetNumberValue("TestUpdates");
	self.testCreate = (self.testCreate or 0) + 1;
	self.testUpdate = 0;
	self.checkpoint = { count = 0 };
	self.checkpoint.self = self.checkpoint;
	self.checkpoint.alias = self.checkpoint;
	self.checkpoint.globals = getfenv(0);
	self.checkpoint.globalKey = {};
	rawset(self.checkpoint.globals, self.checkpoint.globalKey, self.checkpoint);
	rawset(self.checkpoint.globals, self.UniqueID, 0);
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
	self.checkpoint.actorConfiguration = configureActor(self.checkpoint.owned, 0);
	self.checkpoint.device = CreateHDFirearm("Old Stock Battle Rifle", "Base.rte");
	self.checkpoint.deviceConfiguration = configureDevice(self.checkpoint.device, 0);
	self.checkpoint.device.Pos = Vector(32 + self.UniqueID % 96, 32);
	self.checkpoint.device.PinStrength = 10000;
	self.checkpoint.deviceResident = true;
	MovableMan:AddItem(self.checkpoint.device);
	self.checkpoint.owned:SetNumberValue("CheckpointCount", 0);
	self.checkpoint.owned.shared = self.checkpoint;
	self.checkpoint.ownedAlias = self.checkpoint.owned;
	self.checkpoint.ownedUID = self.checkpoint.owned.UniqueID;
	self.checkpoint.ownedPos = self.checkpoint.owned.Pos;
	self.checkpoint.ownedController = self.checkpoint.owned:GetController();
	self.checkpoint.owned:ClearAIWaypoints();
	self.checkpoint.owned:AddAISceneWaypoint(Vector(31, 47));
	self.checkpoint.owned:AddAISceneWaypoint(Vector(53, 61));
	self.checkpoint.owned:AddAIMOWaypoint(self);
	self.checkpoint.owned:AddToMovePathBeginning(Vector(13, 17));
	self.checkpoint.owned:AddToMovePathEnd(Vector(19, 23));
	self.checkpoint.owned.MOMoveTarget = self;
	self.checkpoint.step, self.checkpoint.peek = counter(self.checkpoint);
	self.checkpoint.job = coroutine.create(work(self.checkpoint, 2));
	self.checkpoint.wrap = coroutine.wrap(work(self.checkpoint, 3));
	self.checkpoint.wrapAlias = self.checkpoint.wrap;
	self.checkpoint.match = string.gmatch(string.rep("alpha beta gamma ", 2048), "%a+");
end

function Update(self)
	local state = self.checkpoint;
	assert(state == state.self and state == state.alias, "checkpoint table identity");
	assert(state.globals == getfenv(0) and rawget(state.globals, state.globalKey) == state, "checkpoint global table identity");
	assert(rawget(state.globals, self.UniqueID) == self.testUpdate, "checkpoint global numeric key state");
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
	assert(state.owned:GetWaypointListSize() == 3 and state.owned:GetAIMOWaypointID() == self.ID, "checkpoint waypoint object identity");
	assert(state.owned.MovePathSize == 2 and state.owned.MovePathEnd.X == 19 and state.owned.MovePathEnd.Y == 23, "checkpoint move path");
	assert(state.owned.MOMoveTarget.UniqueID == self.UniqueID, "checkpoint move target identity");
	verifyConfiguration(state.owned, state.configuration);
	verifyConfiguration(state.owned, state.actorConfiguration);
	verifyConfiguration(state.device, state.deviceConfiguration);
	assert(MovableMan:IsDevice(state.device) == state.deviceResident, "checkpoint device residency");
	local registeredDevice = MovableMan:FindObjectByUniqueID(state.device.UniqueID);
	assert(registeredDevice and registeredDevice.PinStrength == 10000 + self.testUpdate, "checkpoint device registry");
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
	rawset(state.globals, self.UniqueID, count);
	state.engineState[self.UniqueID] = count;
	state.owned:SetNumberValue("CheckpointCount", count);
	state.ownedPos.X = count + 10;
	state.ownedController:SetState(Controller.WEAPON_FIRE, count % 2 == 1);
	state.owned.GlobalAccScalar = 0.25 + count / 1024;
	state.owned.GibImpulseLimit = 1000 + count;
	state.configuration = configure(state.owned, count);
	state.actorConfiguration = configureActor(state.owned, count);
	state.deviceConfiguration = configureDevice(state.device, count);
	state.device.PinStrength = 10000 + count;
	if count == 10 or count == 360 then
		state.deviceOwner = MovableMan:RemoveItem(state.device);
		assert(state.deviceOwner and state.deviceOwner.UniqueID == state.device.UniqueID, "checkpoint device ownership transfer");
		state.deviceResident = false;
	elseif count == 350 then
		MovableMan:AddMO(state.deviceOwner);
		state.deviceOwner = nil;
		state.deviceResident = true;
	end
	self:SetNumberValue("TestUpdates", count);
	self:SetNumberValue("TestCheckpointState", count + job + wrapped + state.vector.X + #word);
end

Create, Update = checked(Create), checked(Update);
