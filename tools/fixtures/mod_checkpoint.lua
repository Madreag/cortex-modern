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
	if self:NumberValueExists("CheckpointSpawnChild") then
		local stamp = 0;
		for uid in pairs(math._CheckpointState) do stamp = stamp + uid; end
		self:SetNumberValue("CheckpointSpawnVM", stamp);
		self.Pos = Vector(80 + self.UniqueID % 97, 30 + stamp % 31);
		return;
	end
	self.testCarried = self:GetNumberValue("TestUpdates");
	self.testCreate = (self.testCreate or 0) + 1;
	self.testUpdate = 0;
	self.checkpoint = { count = 0, spawned = {} };
	self.checkpoint.legacy = _ScriptGraph.restoreLegacy([=[{__scriptFieldsId=1,["toggleUpdates"]=249,["tallyUpdates"]=129,["AI"]={__scriptFieldsId=2,__scriptFieldsMeta="NativeHumanAI",["proneState"]=0,["jumpState"]=0,["deviceState"]=0,["lastAIMode"]=0,["teamBlockState"]=0,["minBurstTime"]=140,["SentryFacing"]=false,["fire"]=false,["running"]=false,["flying"]=false,["squadShoot"]=false,["useMedikit"]=false,["AirTimer"]={__scriptFieldsId=3,["simLimit"]=-1,["realStart"]=71618,["realLimit"]=-1,["__scriptFieldsTimer"]=true,["simStart"]=33332},["PickUpTimer"]={__scriptFieldsId=4,["simLimit"]=-1,["realStart"]=71618,["realLimit"]=-1,["__scriptFieldsTimer"]=true,["simStart"]=33332},["ReloadTimer"]={__scriptFieldsId=5,["simLimit"]=-1,["realStart"]=71618,["realLimit"]=-1,["__scriptFieldsTimer"]=true,["simStart"]=33332},["BlockedTimer"]={__scriptFieldsId=6,["simLimit"]=-1,["realStart"]=71618,["realLimit"]=-1,["__scriptFieldsTimer"]=true,["simStart"]=33332},["SquadShootTimer"]={__scriptFieldsId=7,["simLimit"]=-1,["realStart"]=71618,["realLimit"]=-1,["__scriptFieldsTimer"]=true,["simStart"]=33332},["SquadShootDelay"]=60,["RunStateTimer"]={__scriptFieldsId=8,["simLimit"]=4542000,["realStart"]=71618,["realLimit"]=-1,["__scriptFieldsTimer"]=true,["simStart"]=33332},["AlarmTimer"]={__scriptFieldsId=9,["simLimit"]=400000,["realStart"]=71618,["realLimit"]=-1,["__scriptFieldsTimer"]=true,["simStart"]=33332},["TargetLostTimer"]={__scriptFieldsId=10,["simLimit"]=1000000,["realStart"]=71618,["realLimit"]=-1,["__scriptFieldsTimer"]=true,["simStart"]=33332},["idleAimTime"]=500,["PlayerInterferedTimer"]={__scriptFieldsId=11,["simLimit"]=500000,["realStart"]=71618,["realLimit"]=-1,["__scriptFieldsTimer"]=true,["simStart"]=33332},["aimSpeed"]=0.77096108620446435,["aimSkill"]=0.80149960804386922,["skill"]=87.5,["isPlayerOwned"]=true,["lateralMoveState"]=0,["groundContact"]=5}}]=]);
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
	self.checkpoint.sound.PresetName = "Checkpoint Sound";
	self.checkpoint.soundAlias = self.checkpoint.sound;
	self.checkpoint.owned = CreateAHuman("Green Dummy", "Base.rte");
	self.checkpoint.owned.PresetName = "Checkpoint Human";
	self.checkpoint.owned.Description = "A renamed checkpoint actor\nWith two lines";
	self.checkpoint.ownedGroups = {};
	for group in self.checkpoint.owned.Groups do self.checkpoint.ownedGroups[#self.checkpoint.ownedGroups + 1] = group; end
	for _, group in ipairs(self.checkpoint.ownedGroups) do self.checkpoint.owned:RemoveFromGroup(group); end
	self.checkpoint.owned:AddToGroup("Checkpoint Group");
	self.checkpoint.bareHuman = AHuman();
	self.checkpoint.bareCrab = ACrab();
	self.checkpoint.bareHuman.Health = 37;
	self.checkpoint.bareCrab.Health = 53;
	self.checkpoint.bareHuman.Pos = Vector(13, 17);
	self.checkpoint.bareCrab.Pos = Vector(19, 23);
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
	if self:NumberValueExists("CheckpointSpawnChild") then return; end
	local state = self.checkpoint;
	assert(state.legacy.toggleUpdates == 249 and state.legacy.tallyUpdates == 129, "checkpoint legacy saved counters");
	assert(state.legacy.AI.AirTimer.StartSimTimeTicks == 33332 and state.legacy.AI.AirTimer.SimTimeLimitTicks == -1, "checkpoint legacy saved timer");
	assert(getmetatable(state.legacy.AI) == NativeHumanAI, "checkpoint legacy saved metatable");
	assert(state == state.self and state == state.alias, "checkpoint table identity");
	assert(state.globals == getfenv(0) and rawget(state.globals, state.globalKey) == state, "checkpoint global table identity");
	assert(rawget(state.globals, "_ScriptGraphCallbacks") == nil, "checkpoint callback capture escaped");
	assert(rawget(state.globals, self.UniqueID) == self.testUpdate, "checkpoint global numeric key state");
	assert(state.vector == state.vectorAlias and state.vector.shared == state, "checkpoint vector identity");
	assert(state.timer == state.timerAlias, "checkpoint timer identity");
	assert(state.sound == state.soundAlias and state.sound:HasAnySounds() and state.sound.PresetName == "Checkpoint Sound", "checkpoint sound identity");
	assert(state.wrap == state.wrapAlias, "checkpoint closure identity");
	assert(state.engineState == math._CheckpointState and state.engineState[self.UniqueID] == self.testUpdate, "checkpoint engine table state");
	assert(state.owned == state.ownedAlias and state.owned.shared == state, "checkpoint owned object identity");
	assert(state.owned.PresetName == "Checkpoint Human" and state.owned.Description == "A renamed checkpoint actor\nWith two lines", "checkpoint renamed actor");
	assert(state.owned:IsInGroup("Checkpoint Group"), "checkpoint added group");
	for _, group in ipairs(state.ownedGroups) do assert(not state.owned:IsInGroup(group), "checkpoint removed group"); end
	assert(state.bareHuman.Health == 37 and state.bareHuman.Pos.X == 13 and state.bareHuman.Pos.Y == 17, "checkpoint bare human");
	assert(state.bareCrab.Health == 53 and state.bareCrab.Pos.X == 19 and state.bareCrab.Pos.Y == 23, "checkpoint bare crab");
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
	if count == 20 then
		local script = rawget(state.globals, "Userdata/UserScenes.rte/ScriptState/mod_checkpoint.lua");
		assert(type(script) == "table", "checkpoint script function table");
		script.Create = function() error("checkpoint cached Create rebound to global replacement"); end;
		script.Update = function() error("checkpoint cached Update rebound to global replacement"); end;
	end
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
	if self.Team == 0 and self:IsInGroup("Brains") then
		if count == 61 or count == 311 or count == 401 then
			local child = CreateHDFirearm("Old Stock Battle Rifle", "Base.rte");
			child:SetNumberValue("CheckpointSpawnChild", 1);
			child.PinStrength = 10000;
			assert(child:AddScript("UserScenes.rte/ScriptState/mod_checkpoint.lua"), "checkpoint child script load");
			state.spawned[#state.spawned + 1] = child;
			MovableMan:AddItem(child);
			self:SetNumberValue("TestSpawnedUID", child.UniqueID);
		end
		local stamp = 0;
		for _, child in ipairs(state.spawned) do stamp = stamp + child:GetNumberValue("CheckpointSpawnVM"); end
		self:SetNumberValue("TestSpawnedVM", stamp);
	end
	self:SetNumberValue("TestUpdates", count);
	self:SetNumberValue("TestCheckpointState", count + job + wrapped + state.vector.X + #word);
end

Create, Update = checked(Create), checked(Update);
