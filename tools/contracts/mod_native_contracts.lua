local nativeProperties = {
  ["ACDropShip"] = {"AIBaseDigStrength","AIMode","Age","AimDistance","AimRange","AirResistance","AirThreshold","AlarmSound","AngularVel","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","BodyHitSound","CanEnterOrbit","CanRevealUnseen","CanRun","CrashSound","CrouchWalkSpeedMultiplier","DamageMultiplier","DamageOnCollision","DamageOnPenetration","DeathSound","Description","DeviceSwitchSound","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","ForcedHFlip","Frame","GetsHitByMOs","GibAtEndOfLifetime","GibImpulseLimit","GibSound","GibWoundLimit","GlobalAccScalar","GoldCarried","HFlipped","HUDVisible","HatchCloseSound","HatchDelay","HatchOpenSound","Health","HitsMOs","HolsterOffset","HoverHeightModifier","IgnoreTerrain","IgnoresActorHits","IgnoresTeamHits","ImpulseDamageThreshold","ItemInReach","LateralControlSpeed","LeftEngine","LeftHatch","LeftThruster","Lifetime","LimbPushForcesAndCollisionsDisabled","MOMoveTarget","Mass","MaxEngineAngle","MaxHealth","MissionCritical","MoveProximityLimit","MovementState","OrientToVel","PainSound","PainThreshold","PassengerSlots","Perceptiveness","PieMenu","PinStrength","PlacedByPlayer","PlayerControllable","Pos","PostEffectEnabled","PresetName","ReloadOffset","RestThreshold","RightEngine","RightHatch","RightThruster","RotAngle","Scale","ScuttleOnDeath","Sharpness","SightDistance","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","StableRecoveryDelay","Status","Team","ToDelete","ToSettle","TravelImpulse","Vel","ViewPoint","WoundDamageMultiplier"},
  ["ACRocket"] = {"AIBaseDigStrength","AIMode","Age","AimDistance","AimRange","AirResistance","AirThreshold","AlarmSound","AngularVel","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","BodyHitSound","CanEnterOrbit","CanRevealUnseen","CanRun","CrashSound","CrouchWalkSpeedMultiplier","DamageMultiplier","DamageOnCollision","DamageOnPenetration","DeathSound","Description","DeviceSwitchSound","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","ForcedHFlip","Frame","GetsHitByMOs","GibAtEndOfLifetime","GibImpulseLimit","GibSound","GibWoundLimit","GlobalAccScalar","GoldCarried","HFlipped","HUDVisible","HatchCloseSound","HatchDelay","HatchOpenSound","Health","HitsMOs","HolsterOffset","IgnoreTerrain","IgnoresActorHits","IgnoresTeamHits","ImpulseDamageThreshold","ItemInReach","LeftEngine","LeftLeg","LeftThruster","Lifetime","LimbPushForcesAndCollisionsDisabled","MOMoveTarget","MainEngine","Mass","MaxHealth","MissionCritical","MoveProximityLimit","MovementState","OrientToVel","PainSound","PainThreshold","PassengerSlots","Perceptiveness","PieMenu","PinStrength","PlacedByPlayer","PlayerControllable","Pos","PostEffectEnabled","PresetName","ReloadOffset","RestThreshold","RightEngine","RightLeg","RightThruster","RotAngle","Scale","ScuttleOnDeath","Sharpness","SightDistance","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","StableRecoveryDelay","Status","Team","ToDelete","ToSettle","TravelImpulse","Vel","ViewPoint","WoundDamageMultiplier"},
  ["ACrab"] = {"AIBaseDigStrength","AIMode","Age","AimDistance","AimRange","AimRangeLowerLimit","AimRangeUpperLimit","AirResistance","AirThreshold","AlarmSound","AngularVel","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","BodyHitSound","CanRevealUnseen","CanRun","CrouchWalkSpeedMultiplier","DamageMultiplier","DamageOnCollision","DamageOnPenetration","DeathSound","Description","DeviceSwitchSound","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","ForcedHFlip","Frame","GetsHitByMOs","GibAtEndOfLifetime","GibImpulseLimit","GibSound","GibWoundLimit","GlobalAccScalar","GoldCarried","HFlipped","HUDVisible","Health","HitsMOs","HolsterOffset","IgnoreTerrain","IgnoresActorHits","IgnoresTeamHits","ImpulseDamageThreshold","ItemInReach","Jetpack","LeftBGLeg","LeftFGLeg","Lifetime","LimbPushForcesAndCollisionsDisabled","MOMoveTarget","Mass","MaxHealth","MissionCritical","MoveProximityLimit","MovementState","OrientToVel","PainSound","PainThreshold","PassengerSlots","Perceptiveness","PieMenu","PinStrength","PlacedByPlayer","PlayerControllable","Pos","PostEffectEnabled","PresetName","ReloadOffset","RestThreshold","RightBGLeg","RightFGLeg","RotAngle","Scale","Sharpness","SightDistance","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","StableRecoveryDelay","Status","StrideSound","Team","ToDelete","ToSettle","TravelImpulse","Turret","Vel","ViewPoint","WoundDamageMultiplier"},
  ["ADoor"] = {"AIBaseDigStrength","AIMode","Age","AimDistance","AimRange","AirResistance","AirThreshold","AlarmSound","AngularVel","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","BodyHitSound","CanRevealUnseen","CanRun","CrouchWalkSpeedMultiplier","DamageMultiplier","DamageOnCollision","DamageOnPenetration","DeathSound","Description","DeviceSwitchSound","Door","DoorDirectionChangeSound","DoorMoveEndSound","DoorMoveSound","DoorMoveStartSound","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","ForcedHFlip","Frame","GetsHitByMOs","GibAtEndOfLifetime","GibImpulseLimit","GibSound","GibWoundLimit","GlobalAccScalar","GoldCarried","HFlipped","HUDVisible","Health","HitsMOs","HolsterOffset","IgnoreTerrain","IgnoresActorHits","IgnoresTeamHits","ImpulseDamageThreshold","ItemInReach","Lifetime","LimbPushForcesAndCollisionsDisabled","MOMoveTarget","Mass","MaxHealth","MissionCritical","MoveProximityLimit","MovementState","OrientToVel","PainSound","PainThreshold","PassengerSlots","Perceptiveness","PieMenu","PinStrength","PlacedByPlayer","PlayerControllable","Pos","PostEffectEnabled","PresetName","ReloadOffset","RestThreshold","RotAngle","Scale","Sharpness","SightDistance","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","StableRecoveryDelay","Status","Team","ToDelete","ToSettle","TravelImpulse","Vel","ViewPoint","WoundDamageMultiplier"},
  ["AEmitter"] = {"Age","AirResistance","AirThreshold","AngularVel","ApplyTransferredForcesAtOffset","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","BreakWound","BurstDamage","BurstScale","BurstSound","BurstSpacing","CollidesWithTerrainWhileAttached","DamageMultiplier","DamageOnCollision","DamageOnPenetration","DeleteWhenRemovedFromParent","Description","DrawnAfterParent","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","EmissionSound","EmitAngle","EmitCountLimit","EmitDamage","EmitOffset","EmitterDamageMultiplier","EndSound","Flash","FlashScale","ForcedHFlip","Frame","GetThrottle","GetsHitByMOs","GibAtEndOfLifetime","GibImpulseLimit","GibSound","GibWhenRemovedFromParent","GibWoundLimit","GlobalAccScalar","HFlipped","HUDVisible","HitsMOs","IgnoreTerrain","IgnoresActorHits","IgnoresParticlesWhileAttached","IgnoresTeamHits","InheritedRotAngleOffset","InheritsAngularVelWhenDetached","InheritsFrame","InheritsHFlipped","InheritsRotAngle","InheritsVelWhenDetached","JointOffset","JointStiffness","JointStrength","Lifetime","Mass","MissionCritical","NegativeThrottleMultiplier","OrientToVel","ParentBreakWound","ParentOffset","PinStrength","PlacedByPlayer","PlayBurstSound","Pos","PositiveThrottleMultiplier","PostEffectEnabled","PresetName","RestThreshold","RotAngle","Scale","Sharpness","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","Team","Throttle","ToDelete","ToSettle","TravelImpulse","Vel","WoundDamageMultiplier"},
  ["AEJetpack"] = {"AdjustsThrottleForWeight","Age","AirResistance","AirThreshold","AngularVel","ApplyTransferredForcesAtOffset","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","BreakWound","BurstDamage","BurstScale","BurstSound","BurstSpacing","CanAdjustAngleWhileFiring","CollidesWithTerrainWhileAttached","DamageMultiplier","DamageOnCollision","DamageOnPenetration","DeleteWhenRemovedFromParent","Description","DrawnAfterParent","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","EmissionSound","EmitAngle","EmitCountLimit","EmitDamage","EmitOffset","EmitterDamageMultiplier","EndSound","Flash","FlashScale","ForcedHFlip","Frame","GetThrottle","GetsHitByMOs","GibAtEndOfLifetime","GibImpulseLimit","GibSound","GibWhenRemovedFromParent","GibWoundLimit","GlobalAccScalar","HFlipped","HUDVisible","HitsMOs","IgnoreTerrain","IgnoresActorHits","IgnoresParticlesWhileAttached","IgnoresTeamHits","InheritedRotAngleOffset","InheritsAngularVelWhenDetached","InheritsFrame","InheritsHFlipped","InheritsRotAngle","InheritsVelWhenDetached","JetAngleRange","JetReplenishRate","JetTimeLeft","JetTimeTotal","JetpackType","JointOffset","JointStiffness","JointStrength","Lifetime","Mass","MinimumFuelRatio","MissionCritical","NegativeThrottleMultiplier","OrientToVel","ParentBreakWound","ParentOffset","PinStrength","PlacedByPlayer","PlayBurstSound","Pos","PositiveThrottleMultiplier","PostEffectEnabled","PresetName","RestThreshold","RotAngle","Scale","Sharpness","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","Team","Throttle","ToDelete","ToSettle","TravelImpulse","Vel","WoundDamageMultiplier"},
  ["AHuman"] = {"AIBaseDigStrength","AIMode","Age","AimDistance","AimRange","AirResistance","AirThreshold","AlarmSound","AngularVel","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","ArmSwingRate","BGArm","BGFoot","BGLeg","BodyHitSound","CanRevealUnseen","CanRun","CrouchAmountOverride","CrouchWalkSpeedMultiplier","DamageMultiplier","DamageOnCollision","DamageOnPenetration","DeathSound","Description","DeviceArmSwayRate","DeviceSwitchSound","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","FGArm","FGFoot","FGLeg","ForcedHFlip","Frame","GetsHitByMOs","GibAtEndOfLifetime","GibImpulseLimit","GibSound","GibWoundLimit","GlobalAccScalar","GoldCarried","HFlipped","HUDVisible","Head","Health","HitsMOs","HolsterOffset","IgnoreTerrain","IgnoresActorHits","IgnoresTeamHits","ImpulseDamageThreshold","ItemInReach","Jetpack","Lifetime","LimbPushForcesAndCollisionsDisabled","MOMoveTarget","Mass","MaxHealth","MaxWalkPathCrouchShift","MissionCritical","MoveProximityLimit","MovementState","OrientToVel","PainSound","PainThreshold","PassengerSlots","Perceptiveness","PieMenu","PinStrength","PlacedByPlayer","PlayerControllable","Pos","PostEffectEnabled","PresetName","ProneState","ReloadOffset","RestThreshold","RotAngle","Scale","Sharpness","SightDistance","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","StableRecoveryDelay","Status","StrideSound","Team","ThrowPrepTime","ToDelete","ToSettle","TravelImpulse","UpperBodyState","Vel","ViewPoint","WoundDamageMultiplier"},
  ["Arm"] = {"Age","AirResistance","AirThreshold","AngularVel","ApplyTransferredForcesAtOffset","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","BreakWound","CollidesWithTerrainWhileAttached","DamageMultiplier","DamageOnCollision","DamageOnPenetration","DeleteWhenRemovedFromParent","Description","DrawnAfterParent","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","ForcedHFlip","Frame","GetsHitByMOs","GibAtEndOfLifetime","GibImpulseLimit","GibSound","GibWhenRemovedFromParent","GibWoundLimit","GlobalAccScalar","GripStrength","HFlipped","HUDVisible","HandIdleOffset","HandPos","HeldDevice","HitsMOs","IgnoreTerrain","IgnoresActorHits","IgnoresParticlesWhileAttached","IgnoresTeamHits","InheritedRotAngleOffset","InheritsAngularVelWhenDetached","InheritsFrame","InheritsHFlipped","InheritsRotAngle","InheritsVelWhenDetached","JointOffset","JointStiffness","JointStrength","Lifetime","Mass","MissionCritical","MoveSpeed","OrientToVel","ParentBreakWound","ParentOffset","PinStrength","PlacedByPlayer","Pos","PostEffectEnabled","PresetName","RestThreshold","RotAngle","Scale","Sharpness","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","Team","ThrowStrength","ToDelete","ToSettle","TravelImpulse","Vel","WoundDamageMultiplier"},
  ["Attachable"] = {"Age","AirResistance","AirThreshold","AngularVel","ApplyTransferredForcesAtOffset","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","BreakWound","CollidesWithTerrainWhileAttached","DamageMultiplier","DamageOnCollision","DamageOnPenetration","DeleteWhenRemovedFromParent","Description","DrawnAfterParent","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","ForcedHFlip","Frame","GetsHitByMOs","GibAtEndOfLifetime","GibImpulseLimit","GibSound","GibWhenRemovedFromParent","GibWoundLimit","GlobalAccScalar","HFlipped","HUDVisible","HitsMOs","IgnoreTerrain","IgnoresActorHits","IgnoresParticlesWhileAttached","IgnoresTeamHits","InheritedRotAngleOffset","InheritsAngularVelWhenDetached","InheritsFrame","InheritsHFlipped","InheritsRotAngle","InheritsVelWhenDetached","JointOffset","JointStiffness","JointStrength","Lifetime","Mass","MissionCritical","OrientToVel","ParentBreakWound","ParentOffset","PinStrength","PlacedByPlayer","Pos","PostEffectEnabled","PresetName","RestThreshold","RotAngle","Scale","Sharpness","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","Team","ToDelete","ToSettle","TravelImpulse","Vel","WoundDamageMultiplier"},
  ["Emission"] = {"BurstSize","Description","InheritsAngularVel","InheritsVel","LifeVariation","MaxVelocity","MinVelocity","Offset","ParticleCount","ParticlesPerMinute","PresetName","PushesEmitter","Spread"},
  ["HDFirearm"] = {"ActivationDelay","ActiveSound","Age","AirResistance","AirThreshold","AngularVel","ApplyTransferredForcesAtOffset","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","BaseReloadTime","BreakWound","CollidesWithTerrainWhileAttached","DamageMultiplier","DamageOnCollision","DamageOnPenetration","DeactivationDelay","DeactivationSound","DeleteWhenRemovedFromParent","Description","DrawnAfterParent","DualReloadable","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","EjectionOffset","EmptySound","FireEchoSound","FireSound","Flash","ForcedHFlip","Frame","FullAuto","GetsHitByMOs","GetsHitByMOsWhenHeld","GibAtEndOfLifetime","GibImpulseLimit","GibSound","GibWhenRemovedFromParent","GibWoundLimit","GlobalAccScalar","GripStrengthMultiplier","HFlipped","HUDVisible","HitsMOs","IgnoreTerrain","IgnoresActorHits","IgnoresParticlesWhileAttached","IgnoresTeamHits","InheritedRotAngleOffset","InheritsAngularVelWhenDetached","InheritsFrame","InheritsHFlipped","InheritsRotAngle","InheritsVelWhenDetached","IsAnimatedManually","JointOffset","JointStiffness","JointStrength","Lifetime","Magazine","Mass","MissionCritical","MuzzleOffset","NoSupportFactor","OneHandedReloadAngle","OneHandedReloadTimeMultiplier","OrientToVel","ParentBreakWound","ParentOffset","ParticleSpreadRange","PinStrength","PlacedByPlayer","Pos","PostEffectEnabled","PreFireSound","PresetName","RateOfFire","RecoilTransmission","ReloadAngle","ReloadEndOffset","ReloadEndSound","ReloadProgress","ReloadStartSound","Reloadable","RestThreshold","RotAngle","Scale","ShakeRange","SharpLength","SharpShakeRange","SharpStanceOffset","Sharpness","ShellVelVariation","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","StanceOffset","SupportOffset","Supportable","Supported","Team","ToDelete","ToSettle","TravelImpulse","UnPickupable","UseSupportOffsetWhileReloading","Vel","VisualRecoilMultiplier","WoundDamageMultiplier"},
  ["HeldDevice"] = {"Age","AirResistance","AirThreshold","AngularVel","ApplyTransferredForcesAtOffset","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","BreakWound","CollidesWithTerrainWhileAttached","DamageMultiplier","DamageOnCollision","DamageOnPenetration","DeleteWhenRemovedFromParent","Description","DrawnAfterParent","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","ForcedHFlip","Frame","GetsHitByMOs","GetsHitByMOsWhenHeld","GibAtEndOfLifetime","GibImpulseLimit","GibSound","GibWhenRemovedFromParent","GibWoundLimit","GlobalAccScalar","GripStrengthMultiplier","HFlipped","HUDVisible","HitsMOs","IgnoreTerrain","IgnoresActorHits","IgnoresParticlesWhileAttached","IgnoresTeamHits","InheritedRotAngleOffset","InheritsAngularVelWhenDetached","InheritsFrame","InheritsHFlipped","InheritsRotAngle","InheritsVelWhenDetached","JointOffset","JointStiffness","JointStrength","Lifetime","Mass","MissionCritical","MuzzleOffset","OrientToVel","ParentBreakWound","ParentOffset","PinStrength","PlacedByPlayer","Pos","PostEffectEnabled","PresetName","RestThreshold","RotAngle","Scale","SharpLength","SharpStanceOffset","Sharpness","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","StanceOffset","SupportOffset","Supportable","Supported","Team","ToDelete","ToSettle","TravelImpulse","UnPickupable","UseSupportOffsetWhileReloading","Vel","VisualRecoilMultiplier","WoundDamageMultiplier"},
  ["Leg"] = {"Age","AirResistance","AirThreshold","AngularVel","ApplyTransferredForcesAtOffset","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","BreakWound","CollidesWithTerrainWhileAttached","DamageMultiplier","DamageOnCollision","DamageOnPenetration","DeleteWhenRemovedFromParent","Description","DrawnAfterParent","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","Foot","ForcedHFlip","Frame","GetsHitByMOs","GibAtEndOfLifetime","GibImpulseLimit","GibSound","GibWhenRemovedFromParent","GibWoundLimit","GlobalAccScalar","HFlipped","HUDVisible","HitsMOs","IgnoreTerrain","IgnoresActorHits","IgnoresParticlesWhileAttached","IgnoresTeamHits","InheritedRotAngleOffset","InheritsAngularVelWhenDetached","InheritsFrame","InheritsHFlipped","InheritsRotAngle","InheritsVelWhenDetached","JointOffset","JointStiffness","JointStrength","Lifetime","Mass","MissionCritical","MoveSpeed","OrientToVel","ParentBreakWound","ParentOffset","PinStrength","PlacedByPlayer","Pos","PostEffectEnabled","PresetName","RestThreshold","RotAngle","Scale","Sharpness","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","Team","ToDelete","ToSettle","TravelImpulse","Vel","WoundDamageMultiplier"},
  ["LimbPath"] = {"BaseTravelSpeedMultiplier","PushForce","StartOffset","TravelSpeed"},
  ["Magazine"] = {"Age","AirResistance","AirThreshold","AngularVel","ApplyTransferredForcesAtOffset","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","BreakWound","CollidesWithTerrainWhileAttached","DamageMultiplier","DamageOnCollision","DamageOnPenetration","DeleteWhenRemovedFromParent","Description","DrawnAfterParent","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","ForcedHFlip","Frame","GetsHitByMOs","GibAtEndOfLifetime","GibImpulseLimit","GibSound","GibWhenRemovedFromParent","GibWoundLimit","GlobalAccScalar","HFlipped","HUDVisible","HitsMOs","IgnoreTerrain","IgnoresActorHits","IgnoresParticlesWhileAttached","IgnoresTeamHits","InheritedRotAngleOffset","InheritsAngularVelWhenDetached","InheritsFrame","InheritsHFlipped","InheritsRotAngle","InheritsVelWhenDetached","JointOffset","JointStiffness","JointStrength","Lifetime","Mass","MissionCritical","OrientToVel","ParentBreakWound","ParentOffset","PinStrength","PlacedByPlayer","Pos","PostEffectEnabled","PresetName","RestThreshold","RotAngle","RoundCount","Scale","Sharpness","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","Team","ToDelete","ToSettle","TravelImpulse","Vel","WoundDamageMultiplier"},
  ["MOPixel"] = {"Age","AirResistance","AirThreshold","AngularVel","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","DamageOnCollision","DamageOnPenetration","Description","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","GetsHitByMOs","GlobalAccScalar","HFlipped","HUDVisible","HitsMOs","IgnoreTerrain","IgnoresActorHits","IgnoresTeamHits","Lifetime","Mass","MissionCritical","PinStrength","PlacedByPlayer","Pos","PostEffectEnabled","PresetName","RestThreshold","RotAngle","Scale","Sharpness","SimUpdatesBetweenScriptedUpdates","Staininess","Team","ToDelete","ToSettle","TrailLength","Vel","WoundDamageMultiplier"},
  ["MOSParticle"] = {},
  ["MOSRotating"] = {"Age","AirResistance","AirThreshold","AngularVel","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","DamageMultiplier","DamageOnCollision","DamageOnPenetration","Description","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","ForcedHFlip","Frame","GetsHitByMOs","GibAtEndOfLifetime","GibImpulseLimit","GibSound","GibWoundLimit","GlobalAccScalar","HFlipped","HUDVisible","HitsMOs","IgnoreTerrain","IgnoresActorHits","IgnoresTeamHits","Lifetime","Mass","MissionCritical","OrientToVel","PinStrength","PlacedByPlayer","Pos","PostEffectEnabled","PresetName","RestThreshold","RotAngle","Scale","Sharpness","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","Team","ToDelete","ToSettle","TravelImpulse","Vel","WoundDamageMultiplier"},
  ["PEmitter"] = {"Age","AirResistance","AirThreshold","AngularVel","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","BurstScale","BurstSpacing","DamageOnCollision","DamageOnPenetration","Description","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","EmitAngle","EmitCountLimit","FlashScale","ForcedHFlip","Frame","GetThrottle","GetsHitByMOs","GlobalAccScalar","HFlipped","HUDVisible","HitsMOs","IgnoreTerrain","IgnoresActorHits","IgnoresTeamHits","Lifetime","Mass","MissionCritical","PinStrength","PlacedByPlayer","PlayBurstSound","Pos","PostEffectEnabled","PresetName","RestThreshold","RotAngle","Scale","Sharpness","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","Team","Throttle","ToDelete","ToSettle","Vel","WoundDamageMultiplier"},
  ["PieSlice"] = {"CanBeMiddleSlice","Description","Direction","DrawFlippedToMatchAbsoluteAngle","Enabled","FunctionName","PresetName","ScriptPath","SubPieMenu","Type"},
  ["PieMenu"] = {"Description","FullInnerRadius","PresetName","RotAngle"},
  ["Round"] = {"Description","PresetName"},
  ["SoundContainer"] = {"AffectedByGlobalPitch","AttenuationStartDistance","BusRouting","CustomPanValue","Description","Immobile","Loops","PanningStrengthMultiplier","Paused","Pitch","PitchVariation","Pos","PresetName","Priority","SoundOverlapMode","Volume"},
  ["TDExplosive"] = {"Age","AirResistance","AirThreshold","AngularVel","ApplyTransferredForcesAtOffset","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","BreakWound","CollidesWithTerrainWhileAttached","DamageMultiplier","DamageOnCollision","DamageOnPenetration","DeleteWhenRemovedFromParent","Description","DrawnAfterParent","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","EndThrowOffset","ForcedHFlip","Frame","GetsHitByMOs","GetsHitByMOsWhenHeld","GibAtEndOfLifetime","GibImpulseLimit","GibSound","GibWhenRemovedFromParent","GibWoundLimit","GlobalAccScalar","GripStrengthMultiplier","HFlipped","HUDVisible","HitsMOs","IgnoreTerrain","IgnoresActorHits","IgnoresParticlesWhileAttached","IgnoresTeamHits","InheritedRotAngleOffset","InheritsAngularVelWhenDetached","InheritsFrame","InheritsHFlipped","InheritsRotAngle","InheritsVelWhenDetached","IsAnimatedManually","JointOffset","JointStiffness","JointStrength","Lifetime","Mass","MaxThrowVel","MinThrowVel","MissionCritical","MuzzleOffset","OrientToVel","ParentBreakWound","ParentOffset","PinStrength","PlacedByPlayer","Pos","PostEffectEnabled","PresetName","RestThreshold","RotAngle","Scale","SharpLength","SharpStanceOffset","Sharpness","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","StanceOffset","StartThrowOffset","SupportOffset","Supportable","Supported","Team","ToDelete","ToSettle","TravelImpulse","UnPickupable","UseSupportOffsetWhileReloading","Vel","VisualRecoilMultiplier","WoundDamageMultiplier"},
  ["ThrownDevice"] = {"Age","AirResistance","AirThreshold","AngularVel","ApplyTransferredForcesAtOffset","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","BreakWound","CollidesWithTerrainWhileAttached","DamageMultiplier","DamageOnCollision","DamageOnPenetration","DeleteWhenRemovedFromParent","Description","DrawnAfterParent","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","EndThrowOffset","ForcedHFlip","Frame","GetsHitByMOs","GetsHitByMOsWhenHeld","GibAtEndOfLifetime","GibImpulseLimit","GibSound","GibWhenRemovedFromParent","GibWoundLimit","GlobalAccScalar","GripStrengthMultiplier","HFlipped","HUDVisible","HitsMOs","IgnoreTerrain","IgnoresActorHits","IgnoresParticlesWhileAttached","IgnoresTeamHits","InheritedRotAngleOffset","InheritsAngularVelWhenDetached","InheritsFrame","InheritsHFlipped","InheritsRotAngle","InheritsVelWhenDetached","JointOffset","JointStiffness","JointStrength","Lifetime","Mass","MaxThrowVel","MinThrowVel","MissionCritical","MuzzleOffset","OrientToVel","ParentBreakWound","ParentOffset","PinStrength","PlacedByPlayer","Pos","PostEffectEnabled","PresetName","RestThreshold","RotAngle","Scale","SharpLength","SharpStanceOffset","Sharpness","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","StanceOffset","StartThrowOffset","SupportOffset","Supportable","Supported","Team","ToDelete","ToSettle","TravelImpulse","UnPickupable","UseSupportOffsetWhileReloading","Vel","VisualRecoilMultiplier","WoundDamageMultiplier"},
  ["Turret"] = {"Age","AirResistance","AirThreshold","AngularVel","ApplyTransferredForcesAtOffset","ApplyWoundBurstDamageOnCollision","ApplyWoundDamageOnCollision","BreakWound","CollidesWithTerrainWhileAttached","DamageMultiplier","DamageOnCollision","DamageOnPenetration","DeleteWhenRemovedFromParent","Description","DrawnAfterParent","EffectAlwaysShows","EffectRotAngle","EffectStartStrength","EffectStopStrength","ForcedHFlip","Frame","GetsHitByMOs","GibAtEndOfLifetime","GibImpulseLimit","GibSound","GibWhenRemovedFromParent","GibWoundLimit","GlobalAccScalar","HFlipped","HUDVisible","HitsMOs","IgnoreTerrain","IgnoresActorHits","IgnoresParticlesWhileAttached","IgnoresTeamHits","InheritedRotAngleOffset","InheritsAngularVelWhenDetached","InheritsFrame","InheritsHFlipped","InheritsRotAngle","InheritsVelWhenDetached","JointOffset","JointStiffness","JointStrength","Lifetime","Mass","MissionCritical","MountedDevice","MountedDeviceRotationOffset","OrientToVel","ParentBreakWound","ParentOffset","PinStrength","PlacedByPlayer","Pos","PostEffectEnabled","PresetName","RestThreshold","RotAngle","Scale","Sharpness","SimUpdatesBetweenScriptedUpdates","SpriteAnimDuration","SpriteAnimMode","SpriteOffset","Team","ToDelete","ToSettle","TravelImpulse","Vel","WoundDamageMultiplier"},
}

local enumProperties = {SpriteAnimMode=true, ForcedHFlip=true, JetpackType=true, Type=true, Direction=true, SoundOverlapMode=true, BusRouting=true, ProneState=true, UpperBodyState=true, MovementState=true, Status=true, AIMode=true}
local skip = {Age="advances with simulation clock", ReloadProgress="computed progress has no independent constant value", GetThrottle="alias of Throttle", ScriptPath="requires configured callback case", FunctionName="requires configured callback case"}
local bounded = {EffectStartStrength=0.375, EffectStopStrength=0.625, Volume=0.375, MinimumFuelRatio=0.375, ReloadProgress=0.375}
local nullableClasses = {ItemInReach="HDFirearm", MOMoveTarget="AHuman", Door="Attachable", HeldDevice="HDFirearm", Foot="MOSRotating", SubPieMenu="PieMenu", BreakWound="AEmitter", ParentBreakWound="AEmitter"}
local function snapshot(value)
    if type(value) ~= "userdata" then return value end
    local ok, x, y = pcall(function() return value.X, value.Y end)
    if ok and type(x)=="number" and type(y)=="number" then return {kind="vector", X=x, Y=y} end
    local entity, class, name = pcall(function() return value.ClassName, value.PresetName end)
    if entity and type(class)=="string" and type(name)=="string" then
        local hasUID, uid = pcall(function() return value.UniqueID end)
        return {kind="entity", class=class, name=name, uid=hasUID and uid or nil}
    end
    return value
end
local function same(expected, value)
    if type(expected)=="table" and expected.kind=="vector" then return value.X==expected.X and value.Y==expected.Y end
    if type(expected)=="table" and expected.kind=="entity" then
        local actual = snapshot(value)
        return type(actual)=="table" and actual.class==expected.class and actual.name==expected.name and actual.uid==expected.uid
    end
    if type(expected)=="userdata" or type(value)=="userdata" then return rawequal(expected, value) end
    return expected == value
end
function Create(self)
    self.testCreate, self.testUpdate = 1, 0
    if self.UniqueID ~= 1048577 then return end
    local catalog = {}
    for preset in PresetMan:GetAllEntities() do
        local class = preset.ClassName
        if nativeProperties[class] and _G["Create" .. class] and (not os.getenv("CC_CONTRACT_NATIVE_CLASS") or class==os.getenv("CC_CONTRACT_NATIVE_CLASS")) then
            local label = preset.ModuleName .. "/" .. preset.PresetName
            if not catalog[class] or label < catalog[class].label then
                catalog[class] = {name=preset.PresetName, module=preset.ModuleName, label=label}
            end
        end
    end
    self.nativeContracts = {objects={}, aliases={}, expected={}, coverage={}, gaps={}, targets={}}
    local state = self.nativeContracts
    local classes = {}
    for class in pairs(nativeProperties) do classes[#classes+1] = class end
    table.sort(classes)
    local exercised, constructed = 0, 0
    for _, class in ipairs(classes) do
        local preset = catalog[class]
        if not preset then
            state.gaps[#state.gaps+1] = class .. ": no concrete preset in the loaded module set"
        else
            local object = _G["Create" .. class](preset.name, preset.module)
            assert(object, "native contract construction " .. class)
            constructed = constructed + 1
            state.objects[class], state.aliases[class], state.expected[class] = object, object, {}
            local original = {}
            local assigned = {}
            for _, property in ipairs(nativeProperties[class]) do
                local key = class .. "." .. property
                local blocked = class=="Turret" and property=="MountedDevice" and not os.getenv("CC_CONTRACT_EMPTY_TURRET_CONTROL")
                local ok, old = true, nil
                if not blocked then ok, old = pcall(function() return object[property] end) end
                if blocked then state.gaps[#state.gaps+1] = key .. ": F10 retained empty-turret getter crash control"
                elseif not ok then state.gaps[#state.gaps+1] = key .. ": getter: " .. tostring(old)
                elseif skip[property] then state.gaps[#state.gaps+1] = key .. ": " .. skip[property]
                else
                    original[property] = snapshot(old)
                    local value = old
                    if property=="CanRun" then value = old==0
                    elseif bounded[property] then value = bounded[property]
                    elseif type(old)=="boolean" then value = not old
                    elseif type(old)=="number" then
                        if enumProperties[property] then value = old==0 and 1 or 0
                        elseif property=="Frame" then value=0
                        elseif old==math.floor(old) then value = math.max(1, math.min(250, old + 1))
                        else value=old * 0.75 + 0.5 end
                    elseif type(old)=="string" then
                        if property=="ScriptPath" or property=="FunctionName" then value=old
                        else value="NativeContract" .. class .. property end
                    elseif type(old)=="userdata" then
                        local saved = snapshot(old)
                        if type(saved)=="table" and saved.kind=="vector" then value=Vector(saved.X+13, saved.Y-17)
                        else
                            local cloned, replacement = pcall(function() return old:Clone() end)
                            if cloned and replacement then
                                value=replacement
                                pcall(function() value.PresetName = "NativeContractTarget" .. class .. property end)
                            end
                        end
                    elseif old==nil then
                        local targetClass = nullableClasses[property] or (string.sub(property, -5)=="Sound" and "SoundContainer")
                        local targetPreset = targetClass and catalog[targetClass]
                        if targetPreset then
                            value = _G["Create" .. targetClass](targetPreset.name, targetPreset.module)
                            value.PresetName = "NativeContractTarget" .. class .. property
                        end
                    end
                    if value ~= nil then
                        local set, message = pcall(function() object[property] = value end)
                        if set then
                            assigned[property] = true
                            if type(value)=="userdata" then state.targets[#state.targets+1] = value end
                        else state.gaps[#state.gaps+1] = key .. ": setter: " .. tostring(message) end
                    else state.gaps[#state.gaps+1] = key .. ": null ownership requires typed construction case" end
                end
            end
            for property in pairs(assigned) do
                local value = object[property]
                state.expected[class][property] = snapshot(value)
                if not same(original[property], value) then state.coverage[class .. "." .. property]=true; exercised=exercised+1
                else state.gaps[#state.gaps+1] = class .. "." .. property .. ": setter did not produce a distinct value" end
            end
        end
    end
    for _, gap in ipairs(state.gaps) do print("[native-contract-gap] " .. gap) end
    print("[native-contract-fixture] constructed=" .. constructed .. " distinct_property_cases=" .. exercised .. " gaps=" .. #state.gaps)
end
function Update(self)
    self.testUpdate = self.testUpdate + 1
    self:SetNumberValue("TestUpdates", self.testUpdate)
    local state = self.nativeContracts
    if not state then return end
    for class, object in pairs(state.objects) do
        assert(object == state.aliases[class], "native contract object alias " .. class)
        for property, expected in pairs(state.expected[class]) do
            assert(same(expected, object[property]), "native contract changed " .. class .. "." .. property)
        end
    end
end
_G._ContractAuditCheck = function(stage)
    local checked, failures, owners = 0, 0, 0
    local function orderedKeys(values)
        local keys = {}
        for key in pairs(values) do keys[#keys+1] = key end
        table.sort(keys)
        return keys
    end
    local function describe(value)
        if type(value)=="table" and value.kind=="vector" then return tostring(value.X) .. "," .. tostring(value.Y) end
        if type(value)=="table" and value.kind=="entity" then return value.class .. "/" .. value.name .. "/" .. tostring(value.uid) end
        return tostring(value)
    end
    for _, self in pairs(_ScriptedObjects or {}) do
        local state = self.nativeContracts
        if state then
            for _, class in ipairs(orderedKeys(state.objects)) do
                local object = state.objects[class]
                owners = owners + 1
                if not rawequal(object, state.aliases[class]) then
                    failures = failures + 1
                    print("[native-contract-mismatch] " .. stage .. " " .. class .. ".alias")
                end
                for _, property in ipairs(orderedKeys(state.expected[class])) do
                    local expected = state.expected[class][property]
                    local ok, actual = pcall(function() return object[property] end)
                    checked = checked + 1
                    if not ok or not same(expected, actual) then
                        failures = failures + 1
                        print("[native-contract-mismatch] " .. stage .. " " .. class .. "." .. property .. " expected=" .. describe(expected) .. " actual=" .. (ok and describe(snapshot(actual)) or tostring(actual)))
                    end
                end
            end
        end
    end
    print("[native-contract-check] " .. stage .. " owners=" .. owners .. " checked=" .. checked .. " mismatches=" .. failures)
end
