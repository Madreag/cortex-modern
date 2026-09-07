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
	self.checkpoint.box = Box(Vector(3, 5), 7, 11);
	self.checkpoint.box.Width = -7;
	self.checkpoint.box.shared = self.checkpoint;
	self.checkpoint.boxAlias = self.checkpoint.box;
	self.checkpoint.emptyArea = Area();
	self.checkpoint.area = Area("Checkpoint Area " .. self.UniqueID .. "\nSecond line");
	self.checkpoint.area:AddBox(Box(Vector(30, 40), 10, 20));
	self.checkpoint.area:AddBox(Box(Vector(80, 90), 5, 7));
	self.checkpoint.area.shared = self.checkpoint;
	self.checkpoint.aBox = self.checkpoint.area:GetBoxInside(Vector(31, 41));
	self.checkpoint.firstBox = self.checkpoint.area.FirstBox;
	self.checkpoint.firstBox.shared = self.checkpoint;
	SceneMan.Scene:SetArea(self.checkpoint.area);
	self.checkpoint.sceneArea = SceneMan.Scene:GetArea(self.checkpoint.area.Name);
	self.checkpoint.sceneBox = self.checkpoint.sceneArea:GetBoxInside(Vector(31, 41));
	self.checkpoint.aBox.Width = -10;
	self.checkpoint.sceneBox.Width = -10;
	do
		local owner = Area("Checkpoint hidden owner");
		owner:AddBox(Box(Vector(4, 6), 13, 17));
		self.checkpoint.onlyBox = owner:GetBoxInside(Vector(5, 7));
	end
	self.checkpoint.sound = CreateSoundContainer("Funds Changed", "Base.rte");
	self.checkpoint.sound.PresetName = "Checkpoint Sound";
	self.checkpoint.soundAlias = self.checkpoint.sound;
	self.checkpoint.soundSet = SoundSet();
	self.checkpoint.soundSet.SoundSelectionCycleMode = SoundSet.FORWARDS;
	self.checkpoint.soundSetAlias = self.checkpoint.soundSet;
	do
		local leaf = SoundSet();
		leaf.SoundSelectionCycleMode = SoundSet.ALL;
		local branch = SoundSet();
		branch:AddSoundSet(leaf);
		self.checkpoint.soundSet:AddSoundSet(branch);
	end
	self.checkpoint.aSoundSubset = self.checkpoint.soundSet.SubSoundSets();
	self.checkpoint.aSoundLeaf = self.checkpoint.aSoundSubset.SubSoundSets();
	self.checkpoint.aSoundLeaf.shared = self.checkpoint;
	do
		local second = SoundSet();
		second.SoundSelectionCycleMode = SoundSet.FORWARDS;
		self.checkpoint.soundSet:AddSoundSet(second);
		local third = SoundSet();
		third.SoundSelectionCycleMode = SoundSet.ALL;
		self.checkpoint.soundSet:AddSoundSet(third);
	end
	self.checkpoint.soundIterator = self.checkpoint.soundSet.SubSoundSets;
	self.checkpoint.soundIteratorAlias = self.checkpoint.soundIterator;
	self.checkpoint.soundUnstartedIterator = self.checkpoint.soundSet.SubSoundSets;
	self.checkpoint.soundExhaustedIterator = self.checkpoint.soundSet.SubSoundSets;
	while self.checkpoint.soundExhaustedIterator() do end
	self.checkpoint.emptyIterator = SoundSet().SubSoundSets;
	assert(self.checkpoint.soundIterator().SoundSelectionCycleMode == SoundSet.RANDOM, "checkpoint iterator first");
	self.checkpoint.soundLoop = coroutine.create(function(owner)
		for child in owner.SubSoundSets do coroutine.yield(child.SoundSelectionCycleMode); end
		return "done";
	end);
	local ok, first = coroutine.resume(self.checkpoint.soundLoop, self.checkpoint.soundSet);
	assert(ok and first == SoundSet.RANDOM, "checkpoint iterator loop first");
	do
		local owner = SoundSet();
		local child = SoundSet();
		child.SoundSelectionCycleMode = SoundSet.ALL;
		owner:AddSoundSet(child);
		self.checkpoint.onlySoundSubset = owner.SubSoundSets();
	end
	do
		local owner = CreateSoundContainer("Funds Changed", "Base.rte");
		self.checkpoint.onlySoundTop = owner:GetTopLevelSoundSet();
		self.checkpoint.onlySoundTop.SoundSelectionCycleMode = SoundSet.FORWARDS;
	end
	self.checkpoint.aSoundTop = self.checkpoint.sound:GetTopLevelSoundSet();
	self.checkpoint.aSoundTop.SoundSelectionCycleMode = SoundSet.FORWARDS;
	self.checkpoint.aSoundTop.shared = self.checkpoint;
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
	for _, x in ipairs({11, 21, 31}) do self.checkpoint.bareHuman:AddAISceneWaypoint(Vector(x, 43)); end
	self.checkpoint.waypointIterator = self.checkpoint.bareHuman.SceneWaypoints;
	assert(self.checkpoint.waypointIterator().X == 11, "checkpoint owned iterator first");
	self.checkpoint.bareHuman:ClearAIWaypoints();
	self.checkpoint.gib = self.checkpoint.owned.Gibs();
	assert(self.checkpoint.gib, "checkpoint gib present");
	self.checkpoint.gibAlias = self.checkpoint.gib;
	self.checkpoint.gib.Offset = Vector(7, -9);
	self.checkpoint.gibOffset = self.checkpoint.gib.Offset;
	self.checkpoint.gibPreset = self.checkpoint.gib.ParticlePreset;
	self.checkpoint.gibPresetName = self.checkpoint.gibPreset.PresetName;
	self.checkpoint.gib.Count = 3;
	self.checkpoint.gib.Spread = 0.75;
	self.checkpoint.gib.MinVelocity = 5.5;
	self.checkpoint.gib.MaxVelocity = 1.5;
	self.checkpoint.gib.LifeVariation = 0.375;
	self.checkpoint.gib.InheritsVel = 0.25;
	self.checkpoint.gib.InheritsAngularVel = 0.875;
	self.checkpoint.gib.IgnoresTeamHits = true;
	self.checkpoint.gib.SpreadMode = Gib.SpreadSpiral;
	do
		local owner = CreateAHuman("Green Dummy", "Base.rte");
		self.checkpoint.customGib = owner.Gibs();
		self.checkpoint.customGibParticle = CreateAHuman("Green Dummy", "Base.rte");
		self.checkpoint.customGibParticle.GlobalAccScalar = 0.375;
		self.checkpoint.customGib.ParticlePreset = self.checkpoint.customGibParticle;
		self.checkpoint.customGib.shared = self.checkpoint;
		self.Gibs().ParticlePreset = self.checkpoint.customGibParticle;
		self.Gibs().Count = 0;
	end
	do
		local owner = CreateAHuman("Green Dummy", "Base.rte");
		self.checkpoint.bareGib = owner.Gibs();
		self.checkpoint.bareGibParticle = AHuman();
		self.checkpoint.bareGibParticle.GlobalAccScalar = 0.625;
		self.checkpoint.bareGib.ParticlePreset = self.checkpoint.bareGibParticle;
		self.checkpoint.bareGib.Count = 0;
	end
	self.checkpoint.gibSpawnSource = CreateHDFirearm("Old Stock Battle Rifle", "Base.rte");
	self.checkpoint.alarm = AlarmEvent();
	self.checkpoint.alarm.ScenePos = Vector(13, -15);
	self.checkpoint.alarm.Team = Activity.TEAM_2;
	self.checkpoint.alarm.Range = 173.5;
	self.checkpoint.alarm.shared = self.checkpoint;
	self.checkpoint.alarmAlias = self.checkpoint.alarm;
	self.checkpoint.alarmPosition = self.checkpoint.alarm.ScenePos;
	self.checkpoint.module = PresetMan:GetDataModule(PresetMan:GetModuleID("Base.rte"));
	self.checkpoint.moduleAlias = self.checkpoint.module;
	self.checkpoint.moduleIterator = self.checkpoint.module.Presets;
	self.checkpoint.moduleIterator();
	self.checkpoint.gibSpawnParticle = CreateMOSRotating("Gib Panel Dark Small A", "Base.rte");
	self.checkpoint.gibSpawnParticle:SetNumberValue("CheckpointGibParticle", self.UniqueID);
	self.checkpoint.gibSpawnParticle.PinStrength = 10000;
	self.checkpoint.gibSpawnParticle.GlobalAccScalar = 0.125;
	for gib in self.checkpoint.gibSpawnSource.Gibs do gib.Count = 0; end
	local spawnGib = self.checkpoint.gibSpawnSource.Gibs();
	spawnGib.ParticlePreset = self.checkpoint.gibSpawnParticle;
	spawnGib.Count = 2;
	spawnGib.LifeVariation = 0;
	spawnGib.MinVelocity = 0.5;
	spawnGib.MaxVelocity = 0.5;
	self.checkpoint.limb = self.checkpoint.owned:GetLimbPath(AHuman.FGROUND, Actor.WALK);
	self.checkpoint.limbAlias = self.checkpoint.limb;
	self.checkpoint.crabLimb = self.checkpoint.bareCrab:GetLimbPath(0, 0, Actor.WALK);
	self.checkpoint.limb.StartOffset = Vector(17, 29);
	self.checkpoint.limb.BaseTravelSpeedMultiplier = 1.25;
	self.checkpoint.limb.TravelSpeed = 2.5;
	self.checkpoint.limb.PushForce = 310;
	self.checkpoint.limbStart = self.checkpoint.limb.StartOffset;
	self.checkpoint.limbSegment = self.checkpoint.limb:GetSegment(0);
	assert(self.checkpoint.limbSegment, "checkpoint limb segment present");
	self.checkpoint.limbSegment.X = 37;
	self.checkpoint.aLimbStart = self.checkpoint.limbStart;
	self.checkpoint.aLimbSegment = self.checkpoint.limbSegment;
	self.checkpoint.limb.shared = self.checkpoint;
	self.checkpoint.limbSegment.parent = self.checkpoint.limb;
	do
		local owner = CreateAHuman("Green Dummy", "Base.rte");
		self.checkpoint.onlyLimb = owner:GetLimbPath(AHuman.BGROUND, Actor.WALK);
		self.checkpoint.onlyLimb.PushForce = 610;
	end
	self.checkpoint.crabLimb.StartOffset = Vector(23, 31);
	self.checkpoint.crabLimb.PushForce = 410;
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
	assert(state.box == state.boxAlias and state.box.shared == state, "checkpoint box identity");
	assert(state.box.Width == -7 - self.testUpdate and state.box.Height == 11, "checkpoint box state");
	assert(state.emptyArea.Name == "" and state.emptyArea:HasNoArea(), "checkpoint empty area");
	assert(state.onlyBox.Width == 13 and state.onlyBox.Height == 17, "checkpoint hidden box owner");
	assert(state.area.shared == state and state.firstBox.shared == state, "checkpoint area fields");
	assert(state.aBox.Width == -10 - self.testUpdate and state.firstBox.Width == state.aBox.Width, "checkpoint area box aliases");
	assert(state.area.FirstBox.Width == state.aBox.Width, "checkpoint borrowed area box");
	local boxes = 0;
	for box in state.area.Boxes do boxes = boxes + 1; if boxes == 2 then assert(box.Width == 5 and box.Height == 7, "checkpoint area box order"); end end
	assert(boxes == 2, "checkpoint area box count");
	assert(state.sceneBox.Width == -10 - 2 * self.testUpdate and state.sceneArea.FirstBox.Width == state.sceneBox.Width, "checkpoint scene box state " .. state.sceneBox.Width .. "/" .. state.sceneArea.FirstBox.Width .. " expected " .. (-10 - 2 * self.testUpdate));
	assert(SceneMan.Scene:GetArea(state.area.Name).FirstBox.Width == state.sceneBox.Width, "checkpoint scene area reference");
	assert(state.sound == state.soundAlias and state.sound:HasAnySounds() and state.sound.PresetName == "Checkpoint Sound", "checkpoint sound identity");
	assert(state.soundSet == state.soundSetAlias and state.soundSet.SoundSelectionCycleMode == SoundSet.FORWARDS, "checkpoint owned sound set");
	assert(state.aSoundTop.SoundSelectionCycleMode == SoundSet.FORWARDS and state.aSoundTop.shared == state, "checkpoint sound set owner");
	assert(state.sound:GetTopLevelSoundSet().SoundSelectionCycleMode == state.aSoundTop.SoundSelectionCycleMode, "checkpoint sound set reference");
	assert(state.aSoundTop:SelectNextSounds(), "checkpoint sound selection");
	assert(state.aSoundLeaf.SoundSelectionCycleMode == SoundSet.ALL and state.aSoundLeaf.shared == state, "checkpoint nested sound set");
	assert(state.onlySoundSubset.SoundSelectionCycleMode == SoundSet.ALL, "checkpoint sound subset lifetime");
	assert(state.soundSet.SubSoundSets().SubSoundSets().SoundSelectionCycleMode == SoundSet.ALL, "checkpoint sound set hierarchy");
	assert(state.onlySoundTop.SoundSelectionCycleMode == SoundSet.FORWARDS and state.onlySoundTop:SelectNextSounds(), "checkpoint sound set lifetime");
	assert(state.wrap == state.wrapAlias, "checkpoint closure identity");
	assert(state.engineState == math._CheckpointState and state.engineState[self.UniqueID] == self.testUpdate, "checkpoint engine table state");
	assert(state.owned == state.ownedAlias and state.owned.shared == state, "checkpoint owned object identity");
	assert(state.owned.PresetName == "Checkpoint Human" and state.owned.Description == "A renamed checkpoint actor\nWith two lines", "checkpoint renamed actor");
	assert(state.owned:IsInGroup("Checkpoint Group"), "checkpoint added group");
	for _, group in ipairs(state.ownedGroups) do assert(not state.owned:IsInGroup(group), "checkpoint removed group"); end
	assert(state.bareHuman.Health == 37 and state.bareHuman.Pos.X == 13 and state.bareHuman.Pos.Y == 17, "checkpoint bare human");
	assert(state.bareCrab.Health == 53 and state.bareCrab.Pos.X == 19 and state.bareCrab.Pos.Y == 23, "checkpoint bare crab");
	assert(rawequal(state.gib, state.gibAlias) and state.gib.Count == 3 + self.testUpdate % 2, "checkpoint gib alias");
	assert(state.gibOffset.X == 7 + self.testUpdate and state.owned.Gibs().Offset.X == state.gibOffset.X, "checkpoint gib offset reference");
	assert(state.gib.ParticlePreset.PresetName == state.gibPresetName and state.gibPreset.PresetName == state.gibPresetName, "checkpoint gib preset reference");
	assert(state.gib.Spread == 0.75 and state.gib.LifeVariation == 0.375 and state.gib.InheritsVel == 0.25 and state.gib.InheritsAngularVel == 0.875, "checkpoint gib configuration");
	assert(state.gib.MinVelocity == 1.5 and state.gib.MaxVelocity == 5.5 + self.testUpdate / 16, "checkpoint gib velocity bounds");
	assert(state.gib.IgnoresTeamHits and state.gib.SpreadMode == Gib.SpreadSpiral, "checkpoint gib flags");
	local customParticle = ToMovableObject(state.customGib.ParticlePreset);
	assert(state.customGib.shared == state and customParticle.UniqueID == state.customGibParticle.UniqueID, "checkpoint custom gib target");
	assert(customParticle.GlobalAccScalar == 0.375 + self.testUpdate / 16, "checkpoint custom gib target state");
	local worldParticle = ToMovableObject(self.Gibs().ParticlePreset);
	assert(worldParticle and worldParticle.UniqueID == state.customGibParticle.UniqueID and worldParticle.GlobalAccScalar == customParticle.GlobalAccScalar, "checkpoint world gib target");
	assert(ToMovableObject(state.bareGib.ParticlePreset).GlobalAccScalar == 0.625 + self.testUpdate / 32, "checkpoint bare gib target");
	assert(rawequal(state.alarm, state.alarmAlias) and state.alarm.shared == state, "checkpoint alarm aliases");
	assert(state.alarm.ScenePos.X == 13 + self.testUpdate and state.alarmPosition.Y == -15, "checkpoint alarm position");
	assert(state.alarm.Team == Activity.TEAM_2 and state.alarm.Range == 173.5 + self.testUpdate / 8, "checkpoint alarm values");
	assert(rawequal(state.module, state.moduleAlias) and state.module.FileName == "Base.rte", "checkpoint module reference");
	assert(state.limb == state.limbAlias and state.limb.StartOffset.X == 17 + self.testUpdate, "checkpoint limb alias");
	assert(state.owned:GetLimbPath(AHuman.FGROUND, Actor.WALK).PushForce == 310 + self.testUpdate, "checkpoint limb owner");
	assert(state.limb.BaseTravelSpeedMultiplier == 1.25 and state.limb.TravelSpeed == 2.5, "checkpoint limb configuration");
	assert(state.limbStart.X == 17 + self.testUpdate and state.limbSegment.X == 37 + self.testUpdate, "checkpoint limb vector references");
	assert(state.limb:GetSegment(0).X == state.limbSegment.X, "checkpoint limb segment owner");
	assert(state.aLimbStart == state.limbStart and state.aLimbSegment == state.limbSegment, "checkpoint limb vector order");
	assert(state.limbSegment.parent == state.limb and state.limb.shared == state, "checkpoint limb userdata cycle");
	assert(state.onlyLimb.PushForce == 610, "checkpoint limb owner lifetime");
	assert(state.crabLimb.StartOffset.X == 23 and state.crabLimb.PushForce == 410 + self.testUpdate, "checkpoint crab limb");
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
	state.gibOffset.X = 7 + count;
	state.gib.Count = 3 + count % 2;
	state.gib.MinVelocity = 5.5 + count / 16;
	state.customGibParticle.GlobalAccScalar = 0.375 + count / 16;
	state.bareGibParticle.GlobalAccScalar = 0.625 + count / 32;
	state.gibSpawnParticle.GlobalAccScalar = 0.125 + count / 64;
	state.alarmPosition.X = 13 + count;
	state.alarm.Range = 173.5 + count / 8;
	state.limb.StartOffset = Vector(17 + count, 29);
	state.limb.PushForce = 310 + count;
	state.limbSegment.X = 37 + count;
	state.crabLimb.PushForce = 410 + count;
	assert(state.soundIterator == state.soundIteratorAlias, "checkpoint native iterator alias");
	assert(state.soundExhaustedIterator() == nil and state.emptyIterator() == nil, "checkpoint empty native iterators");
	if count == 61 or count == 311 or count == 401 then
		local value = state.soundIterator();
		local ok, yielded = coroutine.resume(state.soundLoop);
		local expected = count == 61 and SoundSet.FORWARDS or (count == 311 and SoundSet.ALL or "done");
		assert((value and value.SoundSelectionCycleMode or "done") == expected, "checkpoint native iterator continuation");
		assert(ok and yielded == expected, "checkpoint native iterator coroutine");
		local waypoint = state.waypointIterator();
		assert((waypoint and waypoint.X or -1) == (count == 61 and 21 or (count == 311 and 31 or -1)), "checkpoint owned iterator continuation");
	end
	state.box.Width = -7 - count;
	state.aBox.Width = -10 - count;
	state.sceneBox.Width = -10 - 2 * count;
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
			local gibSource = state.gibSpawnSource:Clone();
			ToMOSRotating(gibSource):GibThis();
			local spawnedGibs = 0;
			for particle in MovableMan.AddedParticles do
				if particle:GetNumberValue("CheckpointGibParticle") == self.UniqueID then
					assert(particle.GlobalAccScalar == state.gibSpawnParticle.GlobalAccScalar, "checkpoint spawned gib state");
					spawnedGibs = spawnedGibs + 1;
				end
			end
			assert(spawnedGibs == 2, "checkpoint gib spawn count");
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
