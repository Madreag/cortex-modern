#pragma once

// Read-only audit instrumentation; generated from the retained declaration inventory.
#include "GAScripted.h"
#include "GameActivity.h"
#include "ACDropShip.h"
#include "ACRocket.h"
#include "ACrab.h"
#include "ACraft.h"
#include "ADSensor.h"
#include "ADoor.h"
#include "AEJetpack.h"
#include "AEmitter.h"
#include "AHuman.h"
#include "Activity.h"
#include "Actor.h"
#include "Arm.h"
#include "AtomGroup.h"
#include "Attachable.h"
#include "Emission.h"
#include "Gib.h"
#include "GlobalScript.h"
#include "HDFirearm.h"
#include "HeldDevice.h"
#include "Icon.h"
#include "Leg.h"
#include "LimbPath.h"
#include "MOPixel.h"
#include "MOSParticle.h"
#include "MOSRotating.h"
#include "MOSprite.h"
#include "Magazine.h"
#include "Material.h"
#include "MovableObject.h"
#include "PEmitter.h"
#include "PieMenu.h"
#include "PieSlice.h"
#include "Round.h"
#include "Scene.h"
#include "SceneObject.h"
#include "SoundContainer.h"
#include "SoundSet.h"
#include "GUISound.h"
#include "TDExplosive.h"
#include "ThrownDevice.h"
#include "Turret.h"
#include "ActivityMan.h"
#include "MovableMan.h"
#include "SceneMan.h"
#include "TimerMan.h"
#include "Atom.h"
#include "Box.h"
#include "ContentFile.h"
#include "Controller.h"
#include "Entity.h"
#include "Matrix.h"
#include "RTETools.h"
#include "Timer.h"
#include "Vector.h"
#include "LuaMan.h"
#include "TerrainLayerSnapshot.h"
#include "PathFinder.h"
#include "UInputMan.h"
#include "FrameMan.h"
#include "PostProcessMan.h"
#include "PrimitiveMan.h"
#include <bit>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <type_traits>
#include <typeinfo>
#include <utility>

namespace RTE {
struct ContractAudit {
using State = std::map<std::string, std::string>;
State values;
std::map<const void*, std::string> visited;
std::set<std::string> gaps;


template <class T> std::string Value(const T& value, const std::string& path) {
    using V = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<V, std::string> || std::is_same_v<V, std::string_view>) {
        std::ostringstream out; out << std::quoted(std::string(value)); return out.str();
    } else if constexpr (std::is_arithmetic_v<V> || std::is_enum_v<V>) {
        std::ostringstream out;
        if constexpr (std::is_floating_point_v<V>) {
            out << std::hexfloat << value << " bits:" << std::hex;
            if constexpr (sizeof(V) == 4) out << std::bit_cast<uint32_t>(value);
            else if constexpr (sizeof(V) == 8) out << std::bit_cast<uint64_t>(value);
        }
        else if constexpr (std::is_enum_v<V>) out << static_cast<std::underlying_type_t<V>>(value);
        else out << +value;
        return out.str();
    } else if constexpr (std::is_pointer_v<V>) {
        if (!value) return "null";
        using P = std::remove_pointer_t<V>;
        if constexpr (requires { sizeof(P); } && !std::is_function_v<P>) {
            if constexpr (std::is_base_of_v<MovableObject, P>) return "uid:" + std::to_string(value->GetUniqueID());
            else if constexpr (std::is_base_of_v<Entity, P>) return EntityValue(value, path);
            else if constexpr (requires { Visit(*value, path); }) {
                auto [it, added] = visited.emplace(value, path);
                if (added) Visit(*value, path);
                return "ref:" + it->second;
            }
        }
        gaps.insert(path + " opaque pointer " + typeid(V).name());
        return "opaque:present";
    } else if constexpr (requires { value.get(); }) {
        return Value(value.get(), path);
    } else if constexpr (requires { value.first; value.second; }) {
        return "(" + Value(value.first, path + ".first") + "," + Value(value.second, path + ".second") + ")";
    } else if constexpr (requires { typename V::mapped_type; std::begin(value); std::end(value); }) {
        std::vector<std::pair<std::string, const typename V::value_type*>> entries;
        for (const auto& element: value) entries.emplace_back(Value(element.first, path + ".key"), &element);
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::string out = "[";
        for (const auto& [key, element]: entries) out += key + ":" + Value(element->second, path + "[key=" + key + "]") + ";";
        return out + "]";
    } else if constexpr (requires { std::begin(value); std::end(value); }) {
        std::vector<std::string> elements;
        size_t index = 0;
        for (const auto& element: value) elements.push_back(Value(element, path + "[" + std::to_string(index++) + "]"));
        if constexpr (requires { typename V::hasher; }) std::sort(elements.begin(), elements.end());
        std::string out = "[";
        for (const auto& element: elements) out += element + ";";
        return out + "]";
    } else if constexpr (requires { Visit(value, path); }) {
        Visit(value, path);
        return "fields";
    } else {
        gaps.insert(path + " opaque value " + typeid(V).name());
        return "opaque";
    }
}
template <class T> void Field(const std::string& path, const T& value) { values[path] = Value(value, path); }
std::string EntityValue(const Entity* entity, const std::string& path);
void VisitEntity(const Entity& entity, const std::string& path);

void Visit(const ACDropShip& object, const std::string& path) {
Visit(static_cast<const ACraft&>(object), path);
Field(path + ".ACDropShip.m_pBodyAG", object.m_pBodyAG);
Field(path + ".ACDropShip.m_pRThruster", object.m_pRThruster);
Field(path + ".ACDropShip.m_pLThruster", object.m_pLThruster);
Field(path + ".ACDropShip.m_pURThruster", object.m_pURThruster);
Field(path + ".ACDropShip.m_pULThruster", object.m_pULThruster);
Field(path + ".ACDropShip.m_pRHatch", object.m_pRHatch);
Field(path + ".ACDropShip.m_pLHatch", object.m_pLHatch);
Field(path + ".ACDropShip.m_HatchSwingRange", object.m_HatchSwingRange);
Field(path + ".ACDropShip.m_HatchOpeness", object.m_HatchOpeness);
Field(path + ".ACDropShip.m_LateralControl", object.m_LateralControl);
Field(path + ".ACDropShip.m_LateralControlSpeed", object.m_LateralControlSpeed);
Field(path + ".ACDropShip.m_AutoStabilize", object.m_AutoStabilize);
Field(path + ".ACDropShip.m_MaxEngineAngle", object.m_MaxEngineAngle);
Field(path + ".ACDropShip.m_HoverHeightModifier", object.m_HoverHeightModifier);
}
void Visit(const ACRocket& object, const std::string& path) {
Visit(static_cast<const ACraft&>(object), path);
Field(path + ".ACRocket.m_pRLeg", object.m_pRLeg);
Field(path + ".ACRocket.m_pLLeg", object.m_pLLeg);
Field(path + ".ACRocket.m_pBodyAG", object.m_pBodyAG);
Field(path + ".ACRocket.m_pRFootGroup", object.m_pRFootGroup);
Field(path + ".ACRocket.m_pLFootGroup", object.m_pLFootGroup);
Field(path + ".ACRocket.m_pMThruster", object.m_pMThruster);
Field(path + ".ACRocket.m_pRThruster", object.m_pRThruster);
Field(path + ".ACRocket.m_pLThruster", object.m_pLThruster);
Field(path + ".ACRocket.m_pURThruster", object.m_pURThruster);
Field(path + ".ACRocket.m_pULThruster", object.m_pULThruster);
Field(path + ".ACRocket.m_GearState", object.m_GearState);
Field(path + ".ACRocket.m_PersistedRFootResidue", object.m_PersistedRFootResidue);
Field(path + ".ACRocket.m_PersistedLFootResidue", object.m_PersistedLFootResidue);
Field(path + ".ACRocket.m_PersistedLimbPathStates", object.m_PersistedLimbPathStates);
Field(path + ".ACRocket.m_PersistedLimbGroupPositions", object.m_PersistedLimbGroupPositions);
Field(path + ".ACRocket.m_PersistedLimbGroupInertia", object.m_PersistedLimbGroupInertia);
Field(path + ".ACRocket.m_Paths", object.m_Paths);
Field(path + ".ACRocket.m_MaxGimbalAngle", object.m_MaxGimbalAngle);
}
void Visit(const ACrab& object, const std::string& path) {
Visit(static_cast<const Actor&>(object), path);
Field(path + ".ACrab.m_pTurret", object.m_pTurret);
Field(path + ".ACrab.m_pLFGLeg", object.m_pLFGLeg);
Field(path + ".ACrab.m_pLBGLeg", object.m_pLBGLeg);
Field(path + ".ACrab.m_pRFGLeg", object.m_pRFGLeg);
Field(path + ".ACrab.m_pRBGLeg", object.m_pRBGLeg);
Field(path + ".ACrab.m_pLFGFootGroup", object.m_pLFGFootGroup);
Field(path + ".ACrab.m_BackupLFGFootGroup", object.m_BackupLFGFootGroup);
Field(path + ".ACrab.m_pLBGFootGroup", object.m_pLBGFootGroup);
Field(path + ".ACrab.m_BackupLBGFootGroup", object.m_BackupLBGFootGroup);
Field(path + ".ACrab.m_pRFGFootGroup", object.m_pRFGFootGroup);
Field(path + ".ACrab.m_BackupRFGFootGroup", object.m_BackupRFGFootGroup);
Field(path + ".ACrab.m_pRBGFootGroup", object.m_pRBGFootGroup);
Field(path + ".ACrab.m_BackupRBGFootGroup", object.m_BackupRBGFootGroup);
Field(path + ".ACrab.m_PersistedLFGFootResidue", object.m_PersistedLFGFootResidue);
Field(path + ".ACrab.m_PersistedLBGFootResidue", object.m_PersistedLBGFootResidue);
Field(path + ".ACrab.m_PersistedRFGFootResidue", object.m_PersistedRFGFootResidue);
Field(path + ".ACrab.m_PersistedRBGFootResidue", object.m_PersistedRBGFootResidue);
Field(path + ".ACrab.m_PersistedLimbPathStates", object.m_PersistedLimbPathStates);
Field(path + ".ACrab.m_PersistedLimbPathStatesFromFile", object.m_PersistedLimbPathStatesFromFile);
Field(path + ".ACrab.m_PersistedLimbGroupPositions", object.m_PersistedLimbGroupPositions);
Field(path + ".ACrab.m_PersistedLimbGroupInertia", object.m_PersistedLimbGroupInertia);
Field(path + ".ACrab.m_StrideSound", object.m_StrideSound);
Field(path + ".ACrab.m_pJetpack", object.m_pJetpack);
Field(path + ".ACrab.m_IconBlinkTimer", object.m_IconBlinkTimer);
Field(path + ".ACrab.m_StrideFrame", object.m_StrideFrame);
Field(path + ".ACrab.m_Paths", object.m_Paths);
Field(path + ".ACrab.m_Aiming", object.m_Aiming);
Field(path + ".ACrab.m_StrideStart", object.m_StrideStart);
Field(path + ".ACrab.m_StrideTimer", object.m_StrideTimer);
Field(path + ".ACrab.m_AimRangeUpperLimit", object.m_AimRangeUpperLimit);
Field(path + ".ACrab.m_AimRangeLowerLimit", object.m_AimRangeLowerLimit);
Field(path + ".ACrab.m_LockMouseAimInput", object.m_LockMouseAimInput);
}
void Visit(const ACraft& object, const std::string& path) {
Visit(static_cast<const Actor&>(object), path);
Field(path + ".ACraft.m_HatchState", object.m_HatchState);
Field(path + ".ACraft.m_HatchTimer", object.m_HatchTimer);
Field(path + ".ACraft.m_HatchDelay", object.m_HatchDelay);
Field(path + ".ACraft.m_HatchOpenSound", object.m_HatchOpenSound);
Field(path + ".ACraft.m_HatchCloseSound", object.m_HatchCloseSound);
Field(path + ".ACraft.m_CollectedInventory", object.m_CollectedInventory);
Field(path + ".ACraft.m_Exits", object.m_Exits);
Field(path + ".ACraft.m_CurrentExit", object.m_CurrentExit);
Field(path + ".ACraft.m_ExitInterval", object.m_ExitInterval);
Field(path + ".ACraft.m_ExitTimer", object.m_ExitTimer);
Field(path + ".ACraft.m_PersistedHatchTimerAnchor", object.m_PersistedHatchTimerAnchor);
Field(path + ".ACraft.m_PersistedExitTimerAnchor", object.m_PersistedExitTimerAnchor);
Field(path + ".ACraft.m_ReadExitIncomingCursor", object.m_ReadExitIncomingCursor);
Field(path + ".ACraft.m_ExitLinePhase", object.m_ExitLinePhase);
Field(path + ".ACraft.m_HasDelivered", object.m_HasDelivered);
Field(path + ".ACraft.m_LandingCraft", object.m_LandingCraft);
Field(path + ".ACraft.m_FlippedTimer", object.m_FlippedTimer);
Field(path + ".ACraft.m_CrashTimer", object.m_CrashTimer);
Field(path + ".ACraft.m_CrashSound", object.m_CrashSound);
Field(path + ".ACraft.m_CanEnterOrbit", object.m_CanEnterOrbit);
Field(path + ".ACraft.m_MaxPassengers", object.m_MaxPassengers);
Field(path + ".ACraft.m_ScuttleIfFlippedTime", object.m_ScuttleIfFlippedTime);
Field(path + ".ACraft.m_ScuttleOnDeath", object.m_ScuttleOnDeath);
Field(path + ".ACraft.m_DeliveryState", object.m_DeliveryState);
Field(path + ".ACraft.m_AltitudeMoveState", object.m_AltitudeMoveState);
Field(path + ".ACraft.m_AltitudeControl", object.m_AltitudeControl);
Field(path + ".ACraft.m_DeliveryDelayMultiplier", object.m_DeliveryDelayMultiplier);
Field(path + ".ACraft.m_NetworkDelivery", object.m_NetworkDelivery);
Field(path + ".ACraft.m_NetworkDeliveryTimer", object.m_NetworkDeliveryTimer);
}
void Visit(const ACraft::Exit& object, const std::string& path) {
Field(path + ".ACraft::Exit.m_Offset", object.m_Offset);
Field(path + ".ACraft::Exit.m_Velocity", object.m_Velocity);
Field(path + ".ACraft::Exit.m_VelSpread", object.m_VelSpread);
Field(path + ".ACraft::Exit.m_Radius", object.m_Radius);
Field(path + ".ACraft::Exit.m_Range", object.m_Range);
Field(path + ".ACraft::Exit.m_Clear", object.m_Clear);
Field(path + ".ACraft::Exit.m_pIncomingMO", object.m_pIncomingMO);
Field(path + ".ACraft::Exit.m_FaithfulIncomingMOUID", object.m_FaithfulIncomingMOUID);
}
void Visit(const ADSensor& object, const std::string& path) {
Field(path + ".ADSensor.m_StartOffset", object.m_StartOffset);
Field(path + ".ADSensor.m_SensorRay", object.m_SensorRay);
Field(path + ".ADSensor.m_Skip", object.m_Skip);
}
void Visit(const ADoor& object, const std::string& path) {
Visit(static_cast<const Actor&>(object), path);
Field(path + ".ADoor.m_InitialSpriteAnimDuration", object.m_InitialSpriteAnimDuration);
Field(path + ".ADoor.m_Sensors", object.m_Sensors);
Field(path + ".ADoor.m_SensorTimer", object.m_SensorTimer);
Field(path + ".ADoor.m_SensorInterval", object.m_SensorInterval);
Field(path + ".ADoor.m_Door", object.m_Door);
Field(path + ".ADoor.m_DoorState", object.m_DoorState);
Field(path + ".ADoor.m_DoorStateOnStop", object.m_DoorStateOnStop);
Field(path + ".ADoor.m_ClosedByDefault", object.m_ClosedByDefault);
Field(path + ".ADoor.m_OpenOffset", object.m_OpenOffset);
Field(path + ".ADoor.m_ClosedOffset", object.m_ClosedOffset);
Field(path + ".ADoor.m_OpenAngle", object.m_OpenAngle);
Field(path + ".ADoor.m_ClosedAngle", object.m_ClosedAngle);
Field(path + ".ADoor.m_DoorMoveTimer", object.m_DoorMoveTimer);
Field(path + ".ADoor.m_DoorMoveTime", object.m_DoorMoveTime);
Field(path + ".ADoor.m_ResumeAfterStop", object.m_ResumeAfterStop);
Field(path + ".ADoor.m_ChangedDirectionAfterStop", object.m_ChangedDirectionAfterStop);
Field(path + ".ADoor.m_DoorMoveStopTime", object.m_DoorMoveStopTime);
Field(path + ".ADoor.m_ResetToDefaultStateTimer", object.m_ResetToDefaultStateTimer);
Field(path + ".ADoor.m_ResetToDefaultStateDelay", object.m_ResetToDefaultStateDelay);
Field(path + ".ADoor.m_DrawMaterialLayerWhenOpen", object.m_DrawMaterialLayerWhenOpen);
Field(path + ".ADoor.m_DrawMaterialLayerWhenClosed", object.m_DrawMaterialLayerWhenClosed);
Field(path + ".ADoor.m_DoorMaterialID", object.m_DoorMaterialID);
Field(path + ".ADoor.m_DoorMaterialDrawn", object.m_DoorMaterialDrawn);
Field(path + ".ADoor.m_DoorMaterialTempErased", object.m_DoorMaterialTempErased);
Field(path + ".ADoor.m_DoorMaterialRedrawTimer", object.m_DoorMaterialRedrawTimer);
Field(path + ".ADoor.m_LastDoorMaterialPos", object.m_LastDoorMaterialPos);
Field(path + ".ADoor.m_DoorMoveStartSound", object.m_DoorMoveStartSound);
Field(path + ".ADoor.m_DoorMoveSound", object.m_DoorMoveSound);
Field(path + ".ADoor.m_DoorDirectionChangeSound", object.m_DoorDirectionChangeSound);
Field(path + ".ADoor.m_DoorMoveEndSound", object.m_DoorMoveEndSound);
}
void Visit(const AEJetpack& object, const std::string& path) {
Visit(static_cast<const AEmitter&>(object), path);
Field(path + ".AEJetpack.m_JetpackType", object.m_JetpackType);
Field(path + ".AEJetpack.m_JetTimeTotal", object.m_JetTimeTotal);
Field(path + ".AEJetpack.m_JetTimeLeft", object.m_JetTimeLeft);
Field(path + ".AEJetpack.m_JetThrustBonusMultiplier", object.m_JetThrustBonusMultiplier);
Field(path + ".AEJetpack.m_JetReplenishRate", object.m_JetReplenishRate);
Field(path + ".AEJetpack.m_MinimumFuelRatio", object.m_MinimumFuelRatio);
Field(path + ".AEJetpack.m_JetAngleRange", object.m_JetAngleRange);
Field(path + ".AEJetpack.m_CanAdjustAngleWhileFiring", object.m_CanAdjustAngleWhileFiring);
Field(path + ".AEJetpack.m_AdjustsThrottleForWeight", object.m_AdjustsThrottleForWeight);
}
void Visit(const AEmitter& object, const std::string& path) {
Visit(static_cast<const Attachable&>(object), path);
Field(path + ".AEmitter.m_EmissionList", object.m_EmissionList);
Field(path + ".AEmitter.m_EmissionSound", object.m_EmissionSound);
Field(path + ".AEmitter.m_BurstSound", object.m_BurstSound);
Field(path + ".AEmitter.m_EndSound", object.m_EndSound);
Field(path + ".AEmitter.m_EmitEnabled", object.m_EmitEnabled);
Field(path + ".AEmitter.m_WasEmitting", object.m_WasEmitting);
Field(path + ".AEmitter.m_EmitCount", object.m_EmitCount);
Field(path + ".AEmitter.m_EmitCountLimit", object.m_EmitCountLimit);
Field(path + ".AEmitter.m_NegativeThrottleMultiplier", object.m_NegativeThrottleMultiplier);
Field(path + ".AEmitter.m_PositiveThrottleMultiplier", object.m_PositiveThrottleMultiplier);
Field(path + ".AEmitter.m_Throttle", object.m_Throttle);
Field(path + ".AEmitter.m_EmissionsIgnoreThis", object.m_EmissionsIgnoreThis);
Field(path + ".AEmitter.m_BurstScale", object.m_BurstScale);
Field(path + ".AEmitter.m_BurstDamage", object.m_BurstDamage);
Field(path + ".AEmitter.m_EmitterDamageMultiplier", object.m_EmitterDamageMultiplier);
Field(path + ".AEmitter.m_BurstTriggered", object.m_BurstTriggered);
Field(path + ".AEmitter.m_BurstSpacing", object.m_BurstSpacing);
Field(path + ".AEmitter.m_BurstTimer", object.m_BurstTimer);
Field(path + ".AEmitter.m_PersistedBurstTimerAnchor", object.m_PersistedBurstTimerAnchor);
Field(path + ".AEmitter.m_PersistedEmissionAccumulators", object.m_PersistedEmissionAccumulators);
Field(path + ".AEmitter.m_PersistedEmissionTimers", object.m_PersistedEmissionTimers);
Field(path + ".AEmitter.m_PlayBurstSound", object.m_PlayBurstSound);
Field(path + ".AEmitter.m_EmitAngle", object.m_EmitAngle);
Field(path + ".AEmitter.m_EmissionOffset", object.m_EmissionOffset);
Field(path + ".AEmitter.m_EmitDamage", object.m_EmitDamage);
Field(path + ".AEmitter.m_LastEmitTmr", object.m_LastEmitTmr);
Field(path + ".AEmitter.m_PersistedLastEmitTimerAnchor", object.m_PersistedLastEmitTimerAnchor);
Field(path + ".AEmitter.m_pFlash", object.m_pFlash);
Field(path + ".AEmitter.m_FlashScale", object.m_FlashScale);
Field(path + ".AEmitter.m_AvgBurstImpulse", object.m_AvgBurstImpulse);
Field(path + ".AEmitter.m_AvgImpulse", object.m_AvgImpulse);
Field(path + ".AEmitter.m_LoudnessOnEmit", object.m_LoudnessOnEmit);
Field(path + ".AEmitter.m_FlashOnlyOnBurst", object.m_FlashOnlyOnBurst);
Field(path + ".AEmitter.m_SustainBurstSound", object.m_SustainBurstSound);
Field(path + ".AEmitter.m_BurstSoundFollowsEmitter", object.m_BurstSoundFollowsEmitter);
}
void Visit(const AHuman& object, const std::string& path) {
Visit(static_cast<const Actor&>(object), path);
Field(path + ".AHuman.m_pHead", object.m_pHead);
Field(path + ".AHuman.m_LookToAimRatio", object.m_LookToAimRatio);
Field(path + ".AHuman.m_pFGArm", object.m_pFGArm);
Field(path + ".AHuman.m_pBGArm", object.m_pBGArm);
Field(path + ".AHuman.m_PendingDeferredEquips", object.m_PendingDeferredEquips);
Field(path + ".AHuman.m_pFGLeg", object.m_pFGLeg);
Field(path + ".AHuman.m_pBGLeg", object.m_pBGLeg);
Field(path + ".AHuman.m_pFGHandGroup", object.m_pFGHandGroup);
Field(path + ".AHuman.m_pBGHandGroup", object.m_pBGHandGroup);
Field(path + ".AHuman.m_pFGFootGroup", object.m_pFGFootGroup);
Field(path + ".AHuman.m_BackupFGFootGroup", object.m_BackupFGFootGroup);
Field(path + ".AHuman.m_pBGFootGroup", object.m_pBGFootGroup);
Field(path + ".AHuman.m_BackupBGFootGroup", object.m_BackupBGFootGroup);
Field(path + ".AHuman.m_PersistedFGHandResidue", object.m_PersistedFGHandResidue);
Field(path + ".AHuman.m_PersistedBGHandResidue", object.m_PersistedBGHandResidue);
Field(path + ".AHuman.m_PersistedFGFootResidue", object.m_PersistedFGFootResidue);
Field(path + ".AHuman.m_PersistedBGFootResidue", object.m_PersistedBGFootResidue);
Field(path + ".AHuman.m_PersistedLimbPathStates", object.m_PersistedLimbPathStates);
Field(path + ".AHuman.m_PersistedLimbPathStatesFromFile", object.m_PersistedLimbPathStatesFromFile);
Field(path + ".AHuman.m_PersistedLimbGroupPositions", object.m_PersistedLimbGroupPositions);
Field(path + ".AHuman.m_PersistedLimbGroupInertia", object.m_PersistedLimbGroupInertia);
Field(path + ".AHuman.m_PersistedWalkState", object.m_PersistedWalkState);
Field(path + ".AHuman.m_StrideSound", object.m_StrideSound);
Field(path + ".AHuman.m_pJetpack", object.m_pJetpack);
Field(path + ".AHuman.m_CanActivateBGItem", object.m_CanActivateBGItem);
Field(path + ".AHuman.m_TriggerPulled", object.m_TriggerPulled);
Field(path + ".AHuman.m_WaitingToReloadOffhand", object.m_WaitingToReloadOffhand);
Field(path + ".AHuman.m_IconBlinkTimer", object.m_IconBlinkTimer);
Field(path + ".AHuman.m_ArmsState", object.m_ArmsState);
Field(path + ".AHuman.m_ProneState", object.m_ProneState);
Field(path + ".AHuman.m_ProneTimer", object.m_ProneTimer);
Field(path + ".AHuman.m_MaxWalkPathCrouchShift", object.m_MaxWalkPathCrouchShift);
Field(path + ".AHuman.m_CrouchAmount", object.m_CrouchAmount);
Field(path + ".AHuman.m_CrouchAmountOverride", object.m_CrouchAmountOverride);
Field(path + ".AHuman.m_Paths", object.m_Paths);
Field(path + ".AHuman.m_RotAngleTargets", object.m_RotAngleTargets);
Field(path + ".AHuman.m_Aiming", object.m_Aiming);
Field(path + ".AHuman.m_ArmClimbing", object.m_ArmClimbing);
Field(path + ".AHuman.m_StrideFrame", object.m_StrideFrame);
Field(path + ".AHuman.m_StrideStart", object.m_StrideStart);
Field(path + ".AHuman.m_StrideTimer", object.m_StrideTimer);
Field(path + ".AHuman.m_ThrowTmr", object.m_ThrowTmr);
Field(path + ".AHuman.m_ThrowPrepTime", object.m_ThrowPrepTime);
Field(path + ".AHuman.m_SharpAimRevertTimer", object.m_SharpAimRevertTimer);
Field(path + ".AHuman.m_PersistedSharpAimRevertTimerAnchor", object.m_PersistedSharpAimRevertTimerAnchor);
Field(path + ".AHuman.m_FGArmFlailScalar", object.m_FGArmFlailScalar);
Field(path + ".AHuman.m_BGArmFlailScalar", object.m_BGArmFlailScalar);
Field(path + ".AHuman.m_EquipHUDTimer", object.m_EquipHUDTimer);
Field(path + ".AHuman.m_WalkAngle", object.m_WalkAngle);
Field(path + ".AHuman.m_WalkPathOffset", object.m_WalkPathOffset);
Field(path + ".AHuman.m_ArmSwingRate", object.m_ArmSwingRate);
Field(path + ".AHuman.m_DeviceArmSwayRate", object.m_DeviceArmSwayRate);
}
void Visit(const AHuman::DeferredEquip& object, const std::string& path) {
Field(path + ".AHuman::DeferredEquip.op", object.op);
Field(path + ".AHuman::DeferredEquip.depositToFront", object.depositToFront);
Field(path + ".AHuman::DeferredEquip.group", object.group);
Field(path + ".AHuman::DeferredEquip.excludeGroup", object.excludeGroup);
Field(path + ".AHuman::DeferredEquip.moduleName", object.moduleName);
Field(path + ".AHuman::DeferredEquip.presetName", object.presetName);
}
void Visit(const Activity& object, const std::string& path) {
Visit(static_cast<const Entity&>(object), path);
Field(path + ".Activity.m_ActivityState", object.m_ActivityState);
Field(path + ".Activity.m_Paused", object.m_Paused);
Field(path + ".Activity.m_AllowsUserSaving", object.m_AllowsUserSaving);
Field(path + ".Activity.m_IsTestActivity", object.m_IsTestActivity);
Field(path + ".Activity.m_Description", object.m_Description);
Field(path + ".Activity.m_SceneName", object.m_SceneName);
Field(path + ".Activity.m_MaxPlayerSupport", object.m_MaxPlayerSupport);
Field(path + ".Activity.m_MinTeamsRequired", object.m_MinTeamsRequired);
Field(path + ".Activity.m_Difficulty", object.m_Difficulty);
Field(path + ".Activity.m_CraftOrbitAtTheEdge", object.m_CraftOrbitAtTheEdge);
Field(path + ".Activity.m_InCampaignStage", object.m_InCampaignStage);
Field(path + ".Activity.m_PlayerCount", object.m_PlayerCount);
Field(path + ".Activity.m_IsActive", object.m_IsActive);
Field(path + ".Activity.m_IsHuman", object.m_IsHuman);
Field(path + ".Activity.m_PlayerScreen", object.m_PlayerScreen);
Field(path + ".Activity.m_ViewState", object.m_ViewState);
Field(path + ".Activity.m_DeathTimer", object.m_DeathTimer);
Field(path + ".Activity.m_TeamNames", object.m_TeamNames);
Field(path + ".Activity.m_TeamIcons", object.m_TeamIcons);
Field(path + ".Activity.m_TeamCount", object.m_TeamCount);
Field(path + ".Activity.m_TeamActive", object.m_TeamActive);
Field(path + ".Activity.m_Team", object.m_Team);
Field(path + ".Activity.m_TeamDeaths", object.m_TeamDeaths);
Field(path + ".Activity.m_TeamAISkillLevels", object.m_TeamAISkillLevels);
Field(path + ".Activity.m_TeamFunds", object.m_TeamFunds);
Field(path + ".Activity.m_TeamFundsShare", object.m_TeamFundsShare);
Field(path + ".Activity.m_FundsChanged", object.m_FundsChanged);
Field(path + ".Activity.m_FundsContribution", object.m_FundsContribution);
Field(path + ".Activity.m_Brain", object.m_Brain);
Field(path + ".Activity.m_HadBrain", object.m_HadBrain);
Field(path + ".Activity.m_BrainEvacuated", object.m_BrainEvacuated);
Field(path + ".Activity.m_ControlledActor", object.m_ControlledActor);
Field(path + ".Activity.m_PlayerController", object.m_PlayerController);
Field(path + ".Activity.m_MessageTimer", object.m_MessageTimer);
Field(path + ".Activity.m_SavedValues", object.m_SavedValues);
}
void Visit(const Actor& object, const std::string& path) {
Visit(static_cast<const MOSRotating&>(object), path);
Field(path + ".Actor.m_Controller", object.m_Controller);
Field(path + ".Actor.m_PersistedControllerInputMode", object.m_PersistedControllerInputMode);
Field(path + ".Actor.m_PersistedControllerQuickDisabled", object.m_PersistedControllerQuickDisabled);
Field(path + ".Actor.m_PersistedPieMenuState", object.m_PersistedPieMenuState);
Field(path + ".Actor.m_PersistedViewPoint", object.m_PersistedViewPoint);
Field(path + ".Actor.m_HasPersistedViewPoint", object.m_HasPersistedViewPoint);
Field(path + ".Actor.m_HasPersistedMovePath", object.m_HasPersistedMovePath);
Field(path + ".Actor.m_PersistedControllerPlayer", object.m_PersistedControllerPlayer);
Field(path + ".Actor.m_PlayerControllable", object.m_PlayerControllable);
Field(path + ".Actor.m_BodyHitSound", object.m_BodyHitSound);
Field(path + ".Actor.m_AlarmSound", object.m_AlarmSound);
Field(path + ".Actor.m_PainSound", object.m_PainSound);
Field(path + ".Actor.m_DeathSound", object.m_DeathSound);
Field(path + ".Actor.m_DeviceSwitchSound", object.m_DeviceSwitchSound);
Field(path + ".Actor.m_Status", object.m_Status);
Field(path + ".Actor.m_Health", object.m_Health);
Field(path + ".Actor.m_MaxHealth", object.m_MaxHealth);
Field(path + ".Actor.m_PrevHealth", object.m_PrevHealth);
Field(path + ".Actor.m_pTeamIcon", object.m_pTeamIcon);
Field(path + ".Actor.m_pControllerIcon", object.m_pControllerIcon);
Field(path + ".Actor.m_LastSecondTimer", object.m_LastSecondTimer);
Field(path + ".Actor.m_LastSecondPos", object.m_LastSecondPos);
Field(path + ".Actor.m_RecentMovement", object.m_RecentMovement);
Field(path + ".Actor.m_TravelImpulseDamage", object.m_TravelImpulseDamage);
Field(path + ".Actor.m_StableRecoverTimer", object.m_StableRecoverTimer);
Field(path + ".Actor.m_StableVel", object.m_StableVel);
Field(path + ".Actor.m_StableRecoverDelay", object.m_StableRecoverDelay);
Field(path + ".Actor.m_HeartBeat", object.m_HeartBeat);
Field(path + ".Actor.m_NewControlTmr", object.m_NewControlTmr);
Field(path + ".Actor.m_DeathTmr", object.m_DeathTmr);
Field(path + ".Actor.m_GoldCarried", object.m_GoldCarried);
Field(path + ".Actor.m_GoldPicked", object.m_GoldPicked);
Field(path + ".Actor.m_CanRun", object.m_CanRun);
Field(path + ".Actor.m_CrouchWalkSpeedMultiplier", object.m_CrouchWalkSpeedMultiplier);
Field(path + ".Actor.m_AimState", object.m_AimState);
Field(path + ".Actor.m_AimRange", object.m_AimRange);
Field(path + ".Actor.m_AimAngle", object.m_AimAngle);
Field(path + ".Actor.m_AimDistance", object.m_AimDistance);
Field(path + ".Actor.m_AimTmr", object.m_AimTmr);
Field(path + ".Actor.m_SharpAimTimer", object.m_SharpAimTimer);
Field(path + ".Actor.m_PersistedSharpAimTimerAnchor", object.m_PersistedSharpAimTimerAnchor);
Field(path + ".Actor.m_PersistedAimTimerAnchor", object.m_PersistedAimTimerAnchor);
Field(path + ".Actor.m_SharpAimDelay", object.m_SharpAimDelay);
Field(path + ".Actor.m_SharpAimProgress", object.m_SharpAimProgress);
Field(path + ".Actor.m_SharpAimMaxedOut", object.m_SharpAimMaxedOut);
Field(path + ".Actor.m_PointingTarget", object.m_PointingTarget);
Field(path + ".Actor.m_SeenTargetPos", object.m_SeenTargetPos);
Field(path + ".Actor.m_AlarmTimer", object.m_AlarmTimer);
Field(path + ".Actor.m_LastAlarmPos", object.m_LastAlarmPos);
Field(path + ".Actor.m_SightDistance", object.m_SightDistance);
Field(path + ".Actor.m_Perceptiveness", object.m_Perceptiveness);
Field(path + ".Actor.m_PainThreshold", object.m_PainThreshold);
Field(path + ".Actor.m_CanRevealUnseen", object.m_CanRevealUnseen);
Field(path + ".Actor.m_CharHeight", object.m_CharHeight);
Field(path + ".Actor.m_HolsterOffset", object.m_HolsterOffset);
Field(path + ".Actor.m_ReloadOffset", object.m_ReloadOffset);
Field(path + ".Actor.m_ViewPoint", object.m_ViewPoint);
Field(path + ".Actor.m_Inventory", object.m_Inventory);
Field(path + ".Actor.m_MaxInventoryMass", object.m_MaxInventoryMass);
Field(path + ".Actor.m_pItemInReach", object.m_pItemInReach);
Field(path + ".Actor.m_FaithfulItemInReachUID", object.m_FaithfulItemInReachUID);
Field(path + ".Actor.m_OffWireAimTick", object.m_OffWireAimTick);
Field(path + ".Actor.m_OffWireAim", object.m_OffWireAim);
Field(path + ".Actor.m_OffWireFlipTick", object.m_OffWireFlipTick);
Field(path + ".Actor.m_OffWireFlip", object.m_OffWireFlip);
Field(path + ".Actor.m_FaithfulMOMoveTargetUID", object.m_FaithfulMOMoveTargetUID);
Field(path + ".Actor.m_FaithfulWaypointUIDs", object.m_FaithfulWaypointUIDs);
Field(path + ".Actor.m_HotkeyActivated", object.m_HotkeyActivated);
Field(path + ".Actor.m_HUDStack", object.m_HUDStack);
Field(path + ".Actor.m_DeploymentID", object.m_DeploymentID);
Field(path + ".Actor.m_PassengerSlots", object.m_PassengerSlots);
Field(path + ".Actor.m_AIBaseDigStrength", object.m_AIBaseDigStrength);
Field(path + ".Actor.m_BaseMass", object.m_BaseMass);
Field(path + ".Actor.m_AIMode", object.m_AIMode);
Field(path + ".Actor.m_Waypoints", object.m_Waypoints);
Field(path + ".Actor.m_WaypointCursor", object.m_WaypointCursor);
Field(path + ".Actor.m_DrawWaypoints", object.m_DrawWaypoints);
Field(path + ".Actor.m_MoveTarget", object.m_MoveTarget);
Field(path + ".Actor.m_pMOMoveTarget", object.m_pMOMoveTarget);
Field(path + ".Actor.m_PrevPathTarget", object.m_PrevPathTarget);
Field(path + ".Actor.m_MoveVector", object.m_MoveVector);
Field(path + ".Actor.m_MovePath", object.m_MovePath);
Field(path + ".Actor.m_PathRequest", object.m_PathRequest);
Field(path + ".Actor.m_UpdateMovePath", object.m_UpdateMovePath);
Field(path + ".Actor.m_MoveProximityLimit", object.m_MoveProximityLimit);
Field(path + ".Actor.m_MovementState", object.m_MovementState);
Field(path + ".Actor.m_Organic", object.m_Organic);
Field(path + ".Actor.m_Mechanical", object.m_Mechanical);
Field(path + ".Actor.m_LimbPushForcesAndCollisionsDisabled", object.m_LimbPushForcesAndCollisionsDisabled);
Field(path + ".Actor.m_PieMenu", object.m_PieMenu);
}
void Visit(const AlarmEvent& object, const std::string& path) {
Field(path + ".AlarmEvent.m_ScenePos", object.m_ScenePos);
Field(path + ".AlarmEvent.m_Team", object.m_Team);
Field(path + ".AlarmEvent.m_Range", object.m_Range);
}
void Visit(const Arm& object, const std::string& path) {
Visit(static_cast<const Attachable&>(object), path);
Field(path + ".Arm.m_MaxLength", object.m_MaxLength);
Field(path + ".Arm.m_MoveSpeed", object.m_MoveSpeed);
Field(path + ".Arm.m_HandIdleOffset", object.m_HandIdleOffset);
Field(path + ".Arm.m_HandIdleRotation", object.m_HandIdleRotation);
Field(path + ".Arm.m_HandCurrentOffset", object.m_HandCurrentOffset);
Field(path + ".Arm.m_HandPrevPos", object.m_HandPrevPos);
Field(path + ".Arm.m_HandPos", object.m_HandPos);
Field(path + ".Arm.m_HandTargets", object.m_HandTargets);
Field(path + ".Arm.m_HandMovementDelayTimer", object.m_HandMovementDelayTimer);
Field(path + ".Arm.m_HandHasReachedCurrentTarget", object.m_HandHasReachedCurrentTarget);
Field(path + ".Arm.m_PersistedHandMovementDelayTimerAnchor", object.m_PersistedHandMovementDelayTimerAnchor);
Field(path + ".Arm.m_PersistedHandCurrentOffset", object.m_PersistedHandCurrentOffset);
Field(path + ".Arm.m_HasPersistedHandCurrentOffset", object.m_HasPersistedHandCurrentOffset);
Field(path + ".Arm.m_PersistedHandPos", object.m_PersistedHandPos);
Field(path + ".Arm.m_PersistedHandPrevPos", object.m_PersistedHandPrevPos);
Field(path + ".Arm.m_HasPersistedHandPos", object.m_HasPersistedHandPos);
Field(path + ".Arm.m_HandSpriteFile", object.m_HandSpriteFile);
Field(path + ".Arm.m_HandSpriteBitmap", object.m_HandSpriteBitmap);
Field(path + ".Arm.m_GripStrength", object.m_GripStrength);
Field(path + ".Arm.m_ThrowStrength", object.m_ThrowStrength);
Field(path + ".Arm.m_HeldDevice", object.m_HeldDevice);
Field(path + ".Arm.m_HeldDeviceThisArmIsTryingToSupport", object.m_HeldDeviceThisArmIsTryingToSupport);
Field(path + ".Arm.m_FaithfulSupportedDeviceUID", object.m_FaithfulSupportedDeviceUID);
}
void Visit(const Arm::HandTarget& object, const std::string& path) {
Field(path + ".Arm::HandTarget.Description", object.Description);
Field(path + ".Arm::HandTarget.TargetOffset", object.TargetOffset);
Field(path + ".Arm::HandTarget.DelayAtTarget", object.DelayAtTarget);
Field(path + ".Arm::HandTarget.HFlippedWhenTargetWasCreated", object.HFlippedWhenTargetWasCreated);
}
void Visit(const Atom& object, const std::string& path) {
Field(path + ".Atom.m_Offset", object.m_Offset);
Field(path + ".Atom.m_OriginalOffset", object.m_OriginalOffset);
Field(path + ".Atom.m_Normal", object.m_Normal);
Field(path + ".Atom.m_Material", object.m_Material);
Field(path + ".Atom.m_SubgroupID", object.m_SubgroupID);
Field(path + ".Atom.m_StepWasTaken", object.m_StepWasTaken);
Field(path + ".Atom.m_StepRatio", object.m_StepRatio);
Field(path + ".Atom.m_SegTraj", object.m_SegTraj);
Field(path + ".Atom.m_SegProgress", object.m_SegProgress);
Field(path + ".Atom.m_ChangedDir", object.m_ChangedDir);
Field(path + ".Atom.m_PrevError", object.m_PrevError);
Field(path + ".Atom.m_ResultWrapped", object.m_ResultWrapped);
Field(path + ".Atom.m_MOHitsDisabled", object.m_MOHitsDisabled);
Field(path + ".Atom.m_TerrainHitsDisabled", object.m_TerrainHitsDisabled);
Field(path + ".Atom.m_OwnerMO", object.m_OwnerMO);
Field(path + ".Atom.m_IgnoreMOID", object.m_IgnoreMOID);
Field(path + ".Atom.m_IgnoreMOIDs", object.m_IgnoreMOIDs);
Field(path + ".Atom.m_IgnoreMOIDsByGroup", object.m_IgnoreMOIDsByGroup);
Field(path + ".Atom.m_LastTrailPoints", object.m_LastTrailPoints);
Field(path + ".Atom.m_TrailPoints", object.m_TrailPoints);
Field(path + ".Atom.m_LastHit", object.m_LastHit);
Field(path + ".Atom.m_MOIDHit", object.m_MOIDHit);
Field(path + ".Atom.m_TerrainMatHit", object.m_TerrainMatHit);
Field(path + ".Atom.m_NumPenetrations", object.m_NumPenetrations);
Field(path + ".Atom.m_TrailColor", object.m_TrailColor);
Field(path + ".Atom.m_TrailLength", object.m_TrailLength);
Field(path + ".Atom.m_TrailLengthVariation", object.m_TrailLengthVariation);
Field(path + ".Atom.m_IntPos", object.m_IntPos);
Field(path + ".Atom.m_PrevIntPos", object.m_PrevIntPos);
Field(path + ".Atom.m_TrailPos", object.m_TrailPos);
Field(path + ".Atom.m_HitPos", object.m_HitPos);
Field(path + ".Atom.m_Delta", object.m_Delta);
Field(path + ".Atom.m_Delta2", object.m_Delta2);
Field(path + ".Atom.m_Increment", object.m_Increment);
Field(path + ".Atom.m_Error", object.m_Error);
Field(path + ".Atom.m_Dom", object.m_Dom);
Field(path + ".Atom.m_Sub", object.m_Sub);
Field(path + ".Atom.m_DomSteps", object.m_DomSteps);
Field(path + ".Atom.m_SubSteps", object.m_SubSteps);
Field(path + ".Atom.m_SubStepped", object.m_SubStepped);
}
void Visit(const AtomGroup& object, const std::string& path) {
Visit(static_cast<const Entity&>(object), path);
Field(path + ".AtomGroup.m_Atoms", object.m_Atoms);
Field(path + ".AtomGroup.m_SubGroups", object.m_SubGroups);
Field(path + ".AtomGroup.m_OwnerMOSR", object.m_OwnerMOSR);
Field(path + ".AtomGroup.m_StoredOwnerMass", object.m_StoredOwnerMass);
Field(path + ".AtomGroup.m_Material", object.m_Material);
Field(path + ".AtomGroup.m_AutoGenerate", object.m_AutoGenerate);
Field(path + ".AtomGroup.m_Resolution", object.m_Resolution);
Field(path + ".AtomGroup.m_Depth", object.m_Depth);
Field(path + ".AtomGroup.m_JointOffset", object.m_JointOffset);
Field(path + ".AtomGroup.m_LimbPos", object.m_LimbPos);
Field(path + ".AtomGroup.m_MomentOfInertia", object.m_MomentOfInertia);
Field(path + ".AtomGroup.m_IgnoreMOIDs", object.m_IgnoreMOIDs);
Field(path + ".AtomGroup.m_AreaDistributionType", object.m_AreaDistributionType);
Field(path + ".AtomGroup.m_AreaDistributionSurfaceAreaMultiplier", object.m_AreaDistributionSurfaceAreaMultiplier);
}
void Visit(const Attachable& object, const std::string& path) {
Visit(static_cast<const MOSRotating&>(object), path);
Field(path + ".Attachable.m_Parent", object.m_Parent);
Field(path + ".Attachable.m_ParentOffset", object.m_ParentOffset);
Field(path + ".Attachable.m_PersistedParentOffset", object.m_PersistedParentOffset);
Field(path + ".Attachable.m_HasPersistedParentOffset", object.m_HasPersistedParentOffset);
Field(path + ".Attachable.m_DrawAfterParent", object.m_DrawAfterParent);
Field(path + ".Attachable.m_DrawnNormallyByParent", object.m_DrawnNormallyByParent);
Field(path + ".Attachable.m_DeleteWhenRemovedFromParent", object.m_DeleteWhenRemovedFromParent);
Field(path + ".Attachable.m_GibWhenRemovedFromParent", object.m_GibWhenRemovedFromParent);
Field(path + ".Attachable.m_ApplyTransferredForcesAtOffset", object.m_ApplyTransferredForcesAtOffset);
Field(path + ".Attachable.m_GibWithParentChance", object.m_GibWithParentChance);
Field(path + ".Attachable.m_ParentGibBlastStrengthMultiplier", object.m_ParentGibBlastStrengthMultiplier);
Field(path + ".Attachable.m_IsWound", object.m_IsWound);
Field(path + ".Attachable.m_JointStrength", object.m_JointStrength);
Field(path + ".Attachable.m_JointStiffness", object.m_JointStiffness);
Field(path + ".Attachable.m_JointOffset", object.m_JointOffset);
Field(path + ".Attachable.m_JointPos", object.m_JointPos);
Field(path + ".Attachable.m_DamageCount", object.m_DamageCount);
Field(path + ".Attachable.m_BreakWound", object.m_BreakWound);
Field(path + ".Attachable.m_ParentBreakWound", object.m_ParentBreakWound);
Field(path + ".Attachable.m_InheritsHFlipped", object.m_InheritsHFlipped);
Field(path + ".Attachable.m_InheritsRotAngle", object.m_InheritsRotAngle);
Field(path + ".Attachable.m_InheritedRotAngleOffset", object.m_InheritedRotAngleOffset);
Field(path + ".Attachable.m_MountedRotAngleOffset", object.m_MountedRotAngleOffset);
Field(path + ".Attachable.m_InheritsFrame", object.m_InheritsFrame);
Field(path + ".Attachable.m_InheritsVelWhenDetached", object.m_InheritsVelWhenDetached);
Field(path + ".Attachable.m_InheritsAngularVelWhenDetached", object.m_InheritsAngularVelWhenDetached);
Field(path + ".Attachable.m_AtomSubgroupID", object.m_AtomSubgroupID);
Field(path + ".Attachable.m_CollidesWithTerrainWhileAttached", object.m_CollidesWithTerrainWhileAttached);
Field(path + ".Attachable.m_IgnoresParticlesWhileAttached", object.m_IgnoresParticlesWhileAttached);
Field(path + ".Attachable.m_PieSlices", object.m_PieSlices);
Field(path + ".Attachable.m_PrevParentOffset", object.m_PrevParentOffset);
Field(path + ".Attachable.m_PrevJointOffset", object.m_PrevJointOffset);
Field(path + ".Attachable.m_PrevRotAngleOffset", object.m_PrevRotAngleOffset);
Field(path + ".Attachable.m_PreUpdateHasRunThisFrame", object.m_PreUpdateHasRunThisFrame);
}
void Visit(const Box& object, const std::string& path) {
Field(path + ".Box.m_Corner", object.m_Corner);
Field(path + ".Box.m_Width", object.m_Width);
Field(path + ".Box.m_Height", object.m_Height);
}
void Visit(const ContentFile& object, const std::string& path) {
Field(path + ".ContentFile.m_DataPath", object.m_DataPath);
Field(path + ".ContentFile.m_DataPathExtension", object.m_DataPathExtension);
Field(path + ".ContentFile.m_DataPathWithoutExtension", object.m_DataPathWithoutExtension);
Field(path + ".ContentFile.m_DataPathIsImageFile", object.m_DataPathIsImageFile);
Field(path + ".ContentFile.m_ImageFileInfo", object.m_ImageFileInfo);
Field(path + ".ContentFile.m_FormattedReaderPosition", object.m_FormattedReaderPosition);
Field(path + ".ContentFile.m_DataPathAndReaderPosition", object.m_DataPathAndReaderPosition);
Field(path + ".ContentFile.m_DataModuleID", object.m_DataModuleID);
Field(path + ".ContentFile.m_IsMemoryPNG", object.m_IsMemoryPNG);
}
void Visit(const Controller& object, const std::string& path) {
Field(path + ".Controller.m_AnalogMove", object.m_AnalogMove);
Field(path + ".Controller.m_AnalogAim", object.m_AnalogAim);
Field(path + ".Controller.m_AnalogCursor", object.m_AnalogCursor);
Field(path + ".Controller.m_ControlStates", object.m_ControlStates);
Field(path + ".Controller.m_Disabled", object.m_Disabled);
Field(path + ".Controller.m_WireApplyTick", object.m_WireApplyTick);
Field(path + ".Controller.m_WireSchemeValid", object.m_WireSchemeValid);
Field(path + ".Controller.m_WireDeviceClass", object.m_WireDeviceClass);
Field(path + ".Controller.m_WireDigitalAimSpeed", object.m_WireDigitalAimSpeed);
Field(path + ".Controller.m_InputMode", object.m_InputMode);
Field(path + ".Controller.m_SeatMode", object.m_SeatMode);
Field(path + ".Controller.m_ControlledActor", object.m_ControlledActor);
Field(path + ".Controller.m_Player", object.m_Player);
Field(path + ".Controller.m_SeatPlayer", object.m_SeatPlayer);
Field(path + ".Controller.m_Team", object.m_Team);
Field(path + ".Controller.m_NextIgnore", object.m_NextIgnore);
Field(path + ".Controller.m_PrevIgnore", object.m_PrevIgnore);
Field(path + ".Controller.m_WeaponChangeNextIgnore", object.m_WeaponChangeNextIgnore);
Field(path + ".Controller.m_WeaponChangePrevIgnore", object.m_WeaponChangePrevIgnore);
Field(path + ".Controller.m_WeaponPickupIgnore", object.m_WeaponPickupIgnore);
Field(path + ".Controller.m_WeaponDropIgnore", object.m_WeaponDropIgnore);
Field(path + ".Controller.m_WeaponReloadIgnore", object.m_WeaponReloadIgnore);
Field(path + ".Controller.m_WeaponPrimaryHotkeyIgnore", object.m_WeaponPrimaryHotkeyIgnore);
Field(path + ".Controller.m_ReleaseTimer", object.m_ReleaseTimer);
Field(path + ".Controller.m_JoyAccelTimer", object.m_JoyAccelTimer);
Field(path + ".Controller.m_KeyAccelTimer", object.m_KeyAccelTimer);
Field(path + ".Controller.m_MouseMovement", object.m_MouseMovement);
Field(path + ".Controller.m_AnalogCursorAngleLimits", object.m_AnalogCursorAngleLimits);
}
void Visit(const Emission& object, const std::string& path) {
Visit(static_cast<const Entity&>(object), path);
Field(path + ".Emission.m_pEmission", object.m_pEmission);
Field(path + ".Emission.m_PPM", object.m_PPM);
Field(path + ".Emission.m_BurstSize", object.m_BurstSize);
Field(path + ".Emission.m_Accumulator", object.m_Accumulator);
Field(path + ".Emission.m_Spread", object.m_Spread);
Field(path + ".Emission.m_MinVelocity", object.m_MinVelocity);
Field(path + ".Emission.m_MaxVelocity", object.m_MaxVelocity);
Field(path + ".Emission.m_LifeVariation", object.m_LifeVariation);
Field(path + ".Emission.m_PushesEmitter", object.m_PushesEmitter);
Field(path + ".Emission.m_InheritsVel", object.m_InheritsVel);
Field(path + ".Emission.m_InheritsAngularVel", object.m_InheritsAngularVel);
Field(path + ".Emission.m_StartTimer", object.m_StartTimer);
Field(path + ".Emission.m_StopTimer", object.m_StopTimer);
Field(path + ".Emission.m_Offset", object.m_Offset);
Field(path + ".Emission.m_ParticleCount", object.m_ParticleCount);
}
void Visit(const Entity& object, const std::string& path) {
Field(path + ".Entity.m_PresetName", object.m_PresetName);
Field(path + ".Entity.m_CopiedFromPresetName", object.m_CopiedFromPresetName);
Field(path + ".Entity.m_PresetDescription", object.m_PresetDescription);
Field(path + ".Entity.m_FormattedReaderPosition", object.m_FormattedReaderPosition);
Field(path + ".Entity.m_IsOriginalPreset", object.m_IsOriginalPreset);
Field(path + ".Entity.m_DefinedInModule", object.m_DefinedInModule);
Field(path + ".Entity.m_Groups", object.m_Groups);
Field(path + ".Entity.m_RandomWeight", object.m_RandomWeight);
}
void Visit(const GAScripted& object, const std::string& path) {
Visit(static_cast<const GameActivity&>(object), path);
Field(path + ".GAScripted.m_ScriptPath", object.m_ScriptPath);
Field(path + ".GAScripted.m_LuaClassName", object.m_LuaClassName);
Field(path + ".GAScripted.m_RequiredAreas", object.m_RequiredAreas);
Field(path + ".GAScripted.m_PieSlicesToAdd", object.m_PieSlicesToAdd);
Field(path + ".GAScripted.m_GlobalScriptsList", object.m_GlobalScriptsList);
Field(path + ".GAScripted.m_HasSavedGlobalScripts", object.m_HasSavedGlobalScripts);
Field(path + ".GAScripted.m_ScriptFunctions", object.m_ScriptFunctions);
}
void Visit(const GameActivity& object, const std::string& path) {
Visit(static_cast<const Activity&>(object), path);
Field(path + ".GameActivity.m_CPUTeam", object.m_CPUTeam);
Field(path + ".GameActivity.m_TeamIsCPU", object.m_TeamIsCPU);
Field(path + ".GameActivity.m_ObservationTarget", object.m_ObservationTarget);
Field(path + ".GameActivity.m_DeathViewTarget", object.m_DeathViewTarget);
Field(path + ".GameActivity.m_ActorSelectTimer", object.m_ActorSelectTimer);
Field(path + ".GameActivity.m_ActorCursor", object.m_ActorCursor);
Field(path + ".GameActivity.m_pLastMarkedActor", object.m_pLastMarkedActor);
Field(path + ".GameActivity.m_LandingZone", object.m_LandingZone);
Field(path + ".GameActivity.m_AIReturnCraft", object.m_AIReturnCraft);
Field(path + ".GameActivity.m_NextMultiOrderYOffset", object.m_NextMultiOrderYOffset);
Field(path + ".GameActivity.m_StrategicModePieMenu", object.m_StrategicModePieMenu);
Field(path + ".GameActivity.m_InventoryMenuGUI", object.m_InventoryMenuGUI);
Field(path + ".GameActivity.m_pBuyGUI", object.m_pBuyGUI);
Field(path + ".GameActivity.m_pEditorGUI", object.m_pEditorGUI);
Field(path + ".GameActivity.m_LuaLockActor", object.m_LuaLockActor);
Field(path + ".GameActivity.m_LuaLockActorMode", object.m_LuaLockActorMode);
Field(path + ".GameActivity.m_pBannerRed", object.m_pBannerRed);
Field(path + ".GameActivity.m_pBannerYellow", object.m_pBannerYellow);
Field(path + ".GameActivity.m_BannerRepeats", object.m_BannerRepeats);
Field(path + ".GameActivity.m_ReadyToStart", object.m_ReadyToStart);
Field(path + ".GameActivity.m_PurchaseOverride", object.m_PurchaseOverride);
Field(path + ".GameActivity.m_Deliveries", object.m_Deliveries);
Field(path + ".GameActivity.m_LandingZoneArea", object.m_LandingZoneArea);
Field(path + ".GameActivity.m_BrainLZWidth", object.m_BrainLZWidth);
Field(path + ".GameActivity.m_Objectives", object.m_Objectives);
Field(path + ".GameActivity.m_TeamTech", object.m_TeamTech);
Field(path + ".GameActivity.m_TeamTechSwitchEnabled", object.m_TeamTechSwitchEnabled);
Field(path + ".GameActivity.m_StartingGold", object.m_StartingGold);
Field(path + ".GameActivity.m_FogOfWarEnabled", object.m_FogOfWarEnabled);
Field(path + ".GameActivity.m_RequireClearPathToOrbit", object.m_RequireClearPathToOrbit);
Field(path + ".GameActivity.m_DefaultFogOfWar", object.m_DefaultFogOfWar);
Field(path + ".GameActivity.m_DefaultRequireClearPathToOrbit", object.m_DefaultRequireClearPathToOrbit);
Field(path + ".GameActivity.m_DefaultDeployUnits", object.m_DefaultDeployUnits);
Field(path + ".GameActivity.m_DefaultGoldCakeDifficulty", object.m_DefaultGoldCakeDifficulty);
Field(path + ".GameActivity.m_DefaultGoldEasyDifficulty", object.m_DefaultGoldEasyDifficulty);
Field(path + ".GameActivity.m_DefaultGoldMediumDifficulty", object.m_DefaultGoldMediumDifficulty);
Field(path + ".GameActivity.m_DefaultGoldHardDifficulty", object.m_DefaultGoldHardDifficulty);
Field(path + ".GameActivity.m_DefaultGoldNutsDifficulty", object.m_DefaultGoldNutsDifficulty);
Field(path + ".GameActivity.m_DefaultGoldMaxDifficulty", object.m_DefaultGoldMaxDifficulty);
Field(path + ".GameActivity.m_FogOfWarSwitchEnabled", object.m_FogOfWarSwitchEnabled);
Field(path + ".GameActivity.m_DeployUnitsSwitchEnabled", object.m_DeployUnitsSwitchEnabled);
Field(path + ".GameActivity.m_GoldSwitchEnabled", object.m_GoldSwitchEnabled);
Field(path + ".GameActivity.m_RequireClearPathToOrbitSwitchEnabled", object.m_RequireClearPathToOrbitSwitchEnabled);
Field(path + ".GameActivity.m_BuyMenuEnabled", object.m_BuyMenuEnabled);
Field(path + ".GameActivity.m_aLZCursor", object.m_aLZCursor);
Field(path + ".GameActivity.m_LZCursorWidth", object.m_LZCursorWidth);
Field(path + ".GameActivity.m_aObjCursor", object.m_aObjCursor);
Field(path + ".GameActivity.m_DeliveryDelay", object.m_DeliveryDelay);
Field(path + ".GameActivity.m_CursorTimer", object.m_CursorTimer);
Field(path + ".GameActivity.m_GameTimer", object.m_GameTimer);
Field(path + ".GameActivity.m_GameOverTimer", object.m_GameOverTimer);
Field(path + ".GameActivity.m_GameOverPeriod", object.m_GameOverPeriod);
Field(path + ".GameActivity.m_WinnerTeam", object.m_WinnerTeam);
Field(path + ".GameActivity.m_NetworkPlayerNames", object.m_NetworkPlayerNames);
}
void Visit(const GameActivity::Delivery& object, const std::string& path) {
Field(path + ".GameActivity::Delivery.pCraft", object.pCraft);
Field(path + ".GameActivity::Delivery.orderedByPlayer", object.orderedByPlayer);
Field(path + ".GameActivity::Delivery.landingZone", object.landingZone);
Field(path + ".GameActivity::Delivery.multiOrderYOffset", object.multiOrderYOffset);
Field(path + ".GameActivity::Delivery.delay", object.delay);
Field(path + ".GameActivity::Delivery.timer", object.timer);
}
void Visit(const GameActivity::ObjectivePoint& object, const std::string& path) {
Field(path + ".GameActivity::ObjectivePoint.m_Description", object.m_Description);
Field(path + ".GameActivity::ObjectivePoint.m_ScenePos", object.m_ScenePos);
Field(path + ".GameActivity::ObjectivePoint.m_Team", object.m_Team);
Field(path + ".GameActivity::ObjectivePoint.m_ArrowDir", object.m_ArrowDir);
}
void Visit(const GameActivity::PurchaseOrder& object, const std::string& path) {
Field(path + ".GameActivity::PurchaseOrder.purchases", object.purchases);
Field(path + ".GameActivity::PurchaseOrder.team", object.team);
Field(path + ".GameActivity::PurchaseOrder.passengerAIMode", object.passengerAIMode);
Field(path + ".GameActivity::PurchaseOrder.waypoint", object.waypoint);
Field(path + ".GameActivity::PurchaseOrder.pTargetMO", object.pTargetMO);
Field(path + ".GameActivity::PurchaseOrder.totalCost", object.totalCost);
Field(path + ".GameActivity::PurchaseOrder.orderedByPlayer", object.orderedByPlayer);
Field(path + ".GameActivity::PurchaseOrder.aiReturnCraft", object.aiReturnCraft);
Field(path + ".GameActivity::PurchaseOrder.landingZone", object.landingZone);
Field(path + ".GameActivity::PurchaseOrder.multiOrderYOffset", object.multiOrderYOffset);
}
void Visit(const Gib& object, const std::string& path) {
Field(path + ".Gib.m_GibParticle", object.m_GibParticle);
Field(path + ".Gib.m_PersistedParticleUniqueID", object.m_PersistedParticleUniqueID);
Field(path + ".Gib.m_Offset", object.m_Offset);
Field(path + ".Gib.m_Count", object.m_Count);
Field(path + ".Gib.m_Spread", object.m_Spread);
Field(path + ".Gib.m_MinVelocity", object.m_MinVelocity);
Field(path + ".Gib.m_MaxVelocity", object.m_MaxVelocity);
Field(path + ".Gib.m_LifeVariation", object.m_LifeVariation);
Field(path + ".Gib.m_InheritsVel", object.m_InheritsVel);
Field(path + ".Gib.m_InheritsAngularVel", object.m_InheritsAngularVel);
Field(path + ".Gib.m_IgnoresTeamHits", object.m_IgnoresTeamHits);
Field(path + ".Gib.m_SpreadMode", object.m_SpreadMode);
}
void Visit(const GlobalScript& object, const std::string& path) {
Visit(static_cast<const Entity&>(object), path);
Field(path + ".GlobalScript.m_ScriptPath", object.m_ScriptPath);
Field(path + ".GlobalScript.m_LuaClassName", object.m_LuaClassName);
Field(path + ".GlobalScript.m_IsActive", object.m_IsActive);
Field(path + ".GlobalScript.m_HasStarted", object.m_HasStarted);
Field(path + ".GlobalScript.m_LateUpdate", object.m_LateUpdate);
Field(path + ".GlobalScript.m_PieSlicesToAdd", object.m_PieSlicesToAdd);
}
void Visit(const HDFirearm& object, const std::string& path) {
Visit(static_cast<const HeldDevice&>(object), path);
Field(path + ".HDFirearm.m_pMagazineReference", object.m_pMagazineReference);
Field(path + ".HDFirearm.m_pMagazine", object.m_pMagazine);
Field(path + ".HDFirearm.m_pFlash", object.m_pFlash);
Field(path + ".HDFirearm.m_PreFireSound", object.m_PreFireSound);
Field(path + ".HDFirearm.m_FireSound", object.m_FireSound);
Field(path + ".HDFirearm.m_FireEchoSound", object.m_FireEchoSound);
Field(path + ".HDFirearm.m_ActiveSound", object.m_ActiveSound);
Field(path + ".HDFirearm.m_DeactivationSound", object.m_DeactivationSound);
Field(path + ".HDFirearm.m_EmptySound", object.m_EmptySound);
Field(path + ".HDFirearm.m_ReloadStartSound", object.m_ReloadStartSound);
Field(path + ".HDFirearm.m_ReloadEndSound", object.m_ReloadEndSound);
Field(path + ".HDFirearm.m_ReloadEndOffset", object.m_ReloadEndOffset);
Field(path + ".HDFirearm.m_HasPlayedEndReloadSound", object.m_HasPlayedEndReloadSound);
Field(path + ".HDFirearm.m_RateOfFire", object.m_RateOfFire);
Field(path + ".HDFirearm.m_ActivationDelay", object.m_ActivationDelay);
Field(path + ".HDFirearm.m_DeactivationDelay", object.m_DeactivationDelay);
Field(path + ".HDFirearm.m_Reloading", object.m_Reloading);
Field(path + ".HDFirearm.m_DoneReloading", object.m_DoneReloading);
Field(path + ".HDFirearm.m_BaseReloadTime", object.m_BaseReloadTime);
Field(path + ".HDFirearm.m_FullAuto", object.m_FullAuto);
Field(path + ".HDFirearm.m_FireIgnoresThis", object.m_FireIgnoresThis);
Field(path + ".HDFirearm.m_Reloadable", object.m_Reloadable);
Field(path + ".HDFirearm.m_OneHandedReloadTimeMultiplier", object.m_OneHandedReloadTimeMultiplier);
Field(path + ".HDFirearm.m_DualReloadable", object.m_DualReloadable);
Field(path + ".HDFirearm.m_ReloadAngle", object.m_ReloadAngle);
Field(path + ".HDFirearm.m_OneHandedReloadAngle", object.m_OneHandedReloadAngle);
Field(path + ".HDFirearm.m_LastFireTmr", object.m_LastFireTmr);
Field(path + ".HDFirearm.m_ReloadTmr", object.m_ReloadTmr);
Field(path + ".HDFirearm.m_PersistedLastFireTimerAnchor", object.m_PersistedLastFireTimerAnchor);
Field(path + ".HDFirearm.m_PersistedReloadTimerAnchor", object.m_PersistedReloadTimerAnchor);
Field(path + ".HDFirearm.m_MuzzleOff", object.m_MuzzleOff);
Field(path + ".HDFirearm.m_EjectOff", object.m_EjectOff);
Field(path + ".HDFirearm.m_MagOff", object.m_MagOff);
Field(path + ".HDFirearm.m_ShakeRange", object.m_ShakeRange);
Field(path + ".HDFirearm.m_SharpShakeRange", object.m_SharpShakeRange);
Field(path + ".HDFirearm.m_NoSupportFactor", object.m_NoSupportFactor);
Field(path + ".HDFirearm.m_ParticleSpreadRange", object.m_ParticleSpreadRange);
Field(path + ".HDFirearm.m_ShellEjectAngle", object.m_ShellEjectAngle);
Field(path + ".HDFirearm.m_ShellSpreadRange", object.m_ShellSpreadRange);
Field(path + ".HDFirearm.m_ShellAngVelRange", object.m_ShellAngVelRange);
Field(path + ".HDFirearm.m_ShellVelVariation", object.m_ShellVelVariation);
Field(path + ".HDFirearm.m_RecoilScreenShakeAmount", object.m_RecoilScreenShakeAmount);
Field(path + ".HDFirearm.m_AIFireVel", object.m_AIFireVel);
Field(path + ".HDFirearm.m_AIBulletLifeTime", object.m_AIBulletLifeTime);
Field(path + ".HDFirearm.m_AIBulletAccScalar", object.m_AIBulletAccScalar);
Field(path + ".HDFirearm.m_FiredOnce", object.m_FiredOnce);
Field(path + ".HDFirearm.m_FireFrame", object.m_FireFrame);
Field(path + ".HDFirearm.m_FiredLastFrame", object.m_FiredLastFrame);
Field(path + ".HDFirearm.m_AlreadyClicked", object.m_AlreadyClicked);
Field(path + ".HDFirearm.m_RoundsFired", object.m_RoundsFired);
Field(path + ".HDFirearm.m_IsAnimatedManually", object.m_IsAnimatedManually);
Field(path + ".HDFirearm.m_LegacyCompatibilityRoundsAlwaysFireUnflipped", object.m_LegacyCompatibilityRoundsAlwaysFireUnflipped);
}
void Visit(const HeldDevice& object, const std::string& path) {
Visit(static_cast<const Attachable&>(object), path);
Field(path + ".HeldDevice.m_HeldDeviceType", object.m_HeldDeviceType);
Field(path + ".HeldDevice.m_Activated", object.m_Activated);
Field(path + ".HeldDevice.m_HotkeyActivated", object.m_HotkeyActivated);
Field(path + ".HeldDevice.m_ActivationTimer", object.m_ActivationTimer);
Field(path + ".HeldDevice.m_PersistedActivationTimerAnchor", object.m_PersistedActivationTimerAnchor);
Field(path + ".HeldDevice.m_HotkeyActivationTimer", object.m_HotkeyActivationTimer);
Field(path + ".HeldDevice.m_OneHanded", object.m_OneHanded);
Field(path + ".HeldDevice.m_DualWieldable", object.m_DualWieldable);
Field(path + ".HeldDevice.m_StanceOffset", object.m_StanceOffset);
Field(path + ".HeldDevice.m_SharpStanceOffset", object.m_SharpStanceOffset);
Field(path + ".HeldDevice.m_SupportOffset", object.m_SupportOffset);
Field(path + ".HeldDevice.m_UseSupportOffsetWhileReloading", object.m_UseSupportOffsetWhileReloading);
Field(path + ".HeldDevice.m_SharpAim", object.m_SharpAim);
Field(path + ".HeldDevice.m_MaxSharpLength", object.m_MaxSharpLength);
Field(path + ".HeldDevice.m_Supportable", object.m_Supportable);
Field(path + ".HeldDevice.m_Supported", object.m_Supported);
Field(path + ".HeldDevice.m_SupportAvailable", object.m_SupportAvailable);
Field(path + ".HeldDevice.m_IsUnPickupable", object.m_IsUnPickupable);
Field(path + ".HeldDevice.m_SeenByPlayer", object.m_SeenByPlayer);
Field(path + ".HeldDevice.m_PickupableByPresetNames", object.m_PickupableByPresetNames);
Field(path + ".HeldDevice.m_GripStrengthMultiplier", object.m_GripStrengthMultiplier);
Field(path + ".HeldDevice.m_BlinkTimer", object.m_BlinkTimer);
Field(path + ".HeldDevice.m_Loudness", object.m_Loudness);
Field(path + ".HeldDevice.m_IsExplosiveWeapon", object.m_IsExplosiveWeapon);
Field(path + ".HeldDevice.m_GetsHitByMOsWhenHeld", object.m_GetsHitByMOsWhenHeld);
Field(path + ".HeldDevice.m_VisualRecoilMultiplier", object.m_VisualRecoilMultiplier);
}
void Visit(const Icon& object, const std::string& path) {
Visit(static_cast<const Entity&>(object), path);
Field(path + ".Icon.m_BitmapFile", object.m_BitmapFile);
Field(path + ".Icon.m_FrameCount", object.m_FrameCount);
Field(path + ".Icon.m_BitmapsIndexed", object.m_BitmapsIndexed);
Field(path + ".Icon.m_BitmapsTrueColor", object.m_BitmapsTrueColor);
}
void Visit(const Leg& object, const std::string& path) {
Visit(static_cast<const Attachable&>(object), path);
Field(path + ".Leg.m_Foot", object.m_Foot);
Field(path + ".Leg.m_ContractedOffset", object.m_ContractedOffset);
Field(path + ".Leg.m_ExtendedOffset", object.m_ExtendedOffset);
Field(path + ".Leg.m_MinExtension", object.m_MinExtension);
Field(path + ".Leg.m_MaxExtension", object.m_MaxExtension);
Field(path + ".Leg.m_NormalizedExtension", object.m_NormalizedExtension);
Field(path + ".Leg.m_TargetPosition", object.m_TargetPosition);
Field(path + ".Leg.m_IdleOffset", object.m_IdleOffset);
Field(path + ".Leg.m_AnkleOffset", object.m_AnkleOffset);
Field(path + ".Leg.m_WillIdle", object.m_WillIdle);
Field(path + ".Leg.m_MoveSpeed", object.m_MoveSpeed);
}
void Visit(const LimbPath& object, const std::string& path) {
Visit(static_cast<const Entity&>(object), path);
Field(path + ".LimbPath.m_Start", object.m_Start);
Field(path + ".LimbPath.m_StartSegCount", object.m_StartSegCount);
Field(path + ".LimbPath.m_Segments", object.m_Segments);
Field(path + ".LimbPath.m_CurrentSegment", object.m_CurrentSegment);
Field(path + ".LimbPath.m_FootCollisionsDisabledSegment", object.m_FootCollisionsDisabledSegment);
Field(path + ".LimbPath.m_SegProgress", object.m_SegProgress);
Field(path + ".LimbPath.m_TravelSpeed", object.m_TravelSpeed);
Field(path + ".LimbPath.m_SegmentEndedThreshold", object.m_SegmentEndedThreshold);
Field(path + ".LimbPath.m_BaseTravelSpeedMultiplier", object.m_BaseTravelSpeedMultiplier);
Field(path + ".LimbPath.m_CurrentTravelSpeedMultiplier", object.m_CurrentTravelSpeedMultiplier);
Field(path + ".LimbPath.m_BaseScaleMultiplier", object.m_BaseScaleMultiplier);
Field(path + ".LimbPath.m_CurrentScaleMultiplier", object.m_CurrentScaleMultiplier);
Field(path + ".LimbPath.m_PushForce", object.m_PushForce);
Field(path + ".LimbPath.m_JointPos", object.m_JointPos);
Field(path + ".LimbPath.m_JointVel", object.m_JointVel);
Field(path + ".LimbPath.m_Rotation", object.m_Rotation);
Field(path + ".LimbPath.m_RotationOffset", object.m_RotationOffset);
Field(path + ".LimbPath.m_PositionOffset", object.m_PositionOffset);
Field(path + ".LimbPath.m_TimeLeft", object.m_TimeLeft);
Field(path + ".LimbPath.m_PathTimer", object.m_PathTimer);
Field(path + ".LimbPath.m_SegTimer", object.m_SegTimer);
Field(path + ".LimbPath.m_TotalLength", object.m_TotalLength);
Field(path + ".LimbPath.m_RegularLength", object.m_RegularLength);
Field(path + ".LimbPath.m_SegmentDone", object.m_SegmentDone);
Field(path + ".LimbPath.m_Ended", object.m_Ended);
Field(path + ".LimbPath.m_HFlipped", object.m_HFlipped);
}
void Visit(const MOPixel& object, const std::string& path) {
Visit(static_cast<const MovableObject&>(object), path);
Field(path + ".MOPixel.m_Atom", object.m_Atom);
Field(path + ".MOPixel.m_Color", object.m_Color);
Field(path + ".MOPixel.m_LethalRange", object.m_LethalRange);
Field(path + ".MOPixel.m_PersistedAtomResidue", object.m_PersistedAtomResidue);
Field(path + ".MOPixel.m_HasPersistedAtomResidue", object.m_HasPersistedAtomResidue);
Field(path + ".MOPixel.m_PersistedLethalRange", object.m_PersistedLethalRange);
Field(path + ".MOPixel.m_HasPersistedLethalRange", object.m_HasPersistedLethalRange);
Field(path + ".MOPixel.m_PersistedLethalSharpness", object.m_PersistedLethalSharpness);
Field(path + ".MOPixel.m_HasPersistedLethalSharpness", object.m_HasPersistedLethalSharpness);
Field(path + ".MOPixel.m_MinLethalRange", object.m_MinLethalRange);
Field(path + ".MOPixel.m_MaxLethalRange", object.m_MaxLethalRange);
Field(path + ".MOPixel.m_LethalSharpness", object.m_LethalSharpness);
Field(path + ".MOPixel.m_Staininess", object.m_Staininess);
}
void Visit(const MOSParticle& object, const std::string& path) {
Visit(static_cast<const MOSprite&>(object), path);
Field(path + ".MOSParticle.m_Atom", object.m_Atom);
Field(path + ".MOSParticle.m_PersistedAtomResidue", object.m_PersistedAtomResidue);
Field(path + ".MOSParticle.m_HasPersistedAtomResidue", object.m_HasPersistedAtomResidue);
}
void Visit(const MOSRotating& object, const std::string& path) {
Visit(static_cast<const MOSprite&>(object), path);
Field(path + ".MOSRotating.m_pAtomGroup", object.m_pAtomGroup);
Field(path + ".MOSRotating.m_PersistedAtomGroupResidue", object.m_PersistedAtomGroupResidue);
Field(path + ".MOSRotating.m_PersistedAtomGroupOffsets", object.m_PersistedAtomGroupOffsets);
Field(path + ".MOSRotating.m_PersistedAtomGroupSubIDs", object.m_PersistedAtomGroupSubIDs);
Field(path + ".MOSRotating.m_PersistedAtomGroupMaterials", object.m_PersistedAtomGroupMaterials);
Field(path + ".MOSRotating.m_PersistedGroupMomentOfInertia", object.m_PersistedGroupMomentOfInertia);
Field(path + ".MOSRotating.m_PersistedGroupStoredMass", object.m_PersistedGroupStoredMass);
Field(path + ".MOSRotating.m_HasPersistedGroupInertia", object.m_HasPersistedGroupInertia);
Field(path + ".MOSRotating.m_FaithfulAttachableOrder", object.m_FaithfulAttachableOrder);
Field(path + ".MOSRotating.m_FaithfulRadiusAffectingAttachableUID", object.m_FaithfulRadiusAffectingAttachableUID);
Field(path + ".MOSRotating.m_FaithfulFarthestAttachableDistanceAndRadius", object.m_FaithfulFarthestAttachableDistanceAndRadius);
Field(path + ".MOSRotating.m_PersistedAttachableAndWoundMass", object.m_PersistedAttachableAndWoundMass);
Field(path + ".MOSRotating.m_HasPersistedAttachableAndWoundMass", object.m_HasPersistedAttachableAndWoundMass);
Field(path + ".MOSRotating.m_pDeepGroup", object.m_pDeepGroup);
Field(path + ".MOSRotating.m_DeepCheck", object.m_DeepCheck);
Field(path + ".MOSRotating.m_ForceDeepCheck", object.m_ForceDeepCheck);
Field(path + ".MOSRotating.m_DeepHardness", object.m_DeepHardness);
Field(path + ".MOSRotating.m_TravelImpulse", object.m_TravelImpulse);
Field(path + ".MOSRotating.m_SpriteCenter", object.m_SpriteCenter);
Field(path + ".MOSRotating.m_OrientToVel", object.m_OrientToVel);
Field(path + ".MOSRotating.m_Recoiled", object.m_Recoiled);
Field(path + ".MOSRotating.m_RecoilForce", object.m_RecoilForce);
Field(path + ".MOSRotating.m_RecoilOffset", object.m_RecoilOffset);
Field(path + ".MOSRotating.m_Wounds", object.m_Wounds);
Field(path + ".MOSRotating.m_EntryWoundBurstSoundPlayedThisFrame", object.m_EntryWoundBurstSoundPlayedThisFrame);
Field(path + ".MOSRotating.m_ExitWoundBurstSoundPlayedThisFrame", object.m_ExitWoundBurstSoundPlayedThisFrame);
Field(path + ".MOSRotating.m_Attachables", object.m_Attachables);
Field(path + ".MOSRotating.m_ReferenceHardcodedAttachableUniqueIDs", object.m_ReferenceHardcodedAttachableUniqueIDs);
Field(path + ".MOSRotating.m_HardcodedAttachableUniqueIDsAndSetters", object.m_HardcodedAttachableUniqueIDsAndSetters);
Field(path + ".MOSRotating.m_HardcodedAttachableUniqueIDsAndRemovers", object.m_HardcodedAttachableUniqueIDsAndRemovers);
Field(path + ".MOSRotating.m_RadiusAffectingAttachable", object.m_RadiusAffectingAttachable);
Field(path + ".MOSRotating.m_FarthestAttachableDistanceAndRadius", object.m_FarthestAttachableDistanceAndRadius);
Field(path + ".MOSRotating.m_AttachableAndWoundMass", object.m_AttachableAndWoundMass);
Field(path + ".MOSRotating.m_Gibs", object.m_Gibs);
Field(path + ".MOSRotating.m_GibImpulseLimit", object.m_GibImpulseLimit);
Field(path + ".MOSRotating.m_GibWoundLimit", object.m_GibWoundLimit);
Field(path + ".MOSRotating.m_GibBlastStrength", object.m_GibBlastStrength);
Field(path + ".MOSRotating.m_GibScreenShakeAmount", object.m_GibScreenShakeAmount);
Field(path + ".MOSRotating.m_WoundCountAffectsImpulseLimitRatio", object.m_WoundCountAffectsImpulseLimitRatio);
Field(path + ".MOSRotating.m_DetachAttachablesBeforeGibbingFromWounds", object.m_DetachAttachablesBeforeGibbingFromWounds);
Field(path + ".MOSRotating.m_GibAtEndOfLifetime", object.m_GibAtEndOfLifetime);
Field(path + ".MOSRotating.m_GibSound", object.m_GibSound);
Field(path + ".MOSRotating.m_EffectOnGib", object.m_EffectOnGib);
Field(path + ".MOSRotating.m_LoudnessOnGib", object.m_LoudnessOnGib);
Field(path + ".MOSRotating.m_DamageMultiplier", object.m_DamageMultiplier);
Field(path + ".MOSRotating.m_NoSetDamageMultiplier", object.m_NoSetDamageMultiplier);
Field(path + ".MOSRotating.m_FlashWhiteTimer", object.m_FlashWhiteTimer);
Field(path + ".MOSRotating.m_pFlipBitmap", object.m_pFlipBitmap);
Field(path + ".MOSRotating.m_pFlipBitmapS", object.m_pFlipBitmapS);
Field(path + ".MOSRotating.m_pTempBitmap", object.m_pTempBitmap);
Field(path + ".MOSRotating.m_pTempBitmapS", object.m_pTempBitmapS);
}
void Visit(const MOSprite& object, const std::string& path) {
Visit(static_cast<const MovableObject&>(object), path);
Field(path + ".MOSprite.m_Rotation", object.m_Rotation);
Field(path + ".MOSprite.m_PrevRotation", object.m_PrevRotation);
Field(path + ".MOSprite.m_AngularVel", object.m_AngularVel);
Field(path + ".MOSprite.m_PrevAngVel", object.m_PrevAngVel);
Field(path + ".MOSprite.m_SpriteFile", object.m_SpriteFile);
Field(path + ".MOSprite.m_aSprite", object.m_aSprite);
Field(path + ".MOSprite.m_IconFile", object.m_IconFile);
Field(path + ".MOSprite.m_GraphicalIcon", object.m_GraphicalIcon);
Field(path + ".MOSprite.m_FrameCount", object.m_FrameCount);
Field(path + ".MOSprite.m_SpriteOffset", object.m_SpriteOffset);
Field(path + ".MOSprite.m_Frame", object.m_Frame);
Field(path + ".MOSprite.m_SpriteAnimMode", object.m_SpriteAnimMode);
Field(path + ".MOSprite.m_SpriteAnimDuration", object.m_SpriteAnimDuration);
Field(path + ".MOSprite.m_SpriteAnimTimer", object.m_SpriteAnimTimer);
Field(path + ".MOSprite.m_SpriteAnimIsReversingFrames", object.m_SpriteAnimIsReversingFrames);
Field(path + ".MOSprite.m_HFlipped", object.m_HFlipped);
Field(path + ".MOSprite.m_ForcedHFlip", object.m_ForcedHFlip);
Field(path + ".MOSprite.m_SpriteRadius", object.m_SpriteRadius);
Field(path + ".MOSprite.m_SpriteDiameter", object.m_SpriteDiameter);
Field(path + ".MOSprite.m_AngOscillations", object.m_AngOscillations);
Field(path + ".MOSprite.m_PersistedAngOscillations", object.m_PersistedAngOscillations);
Field(path + ".MOSprite.m_PersistedSpriteAnimTimerAnchor", object.m_PersistedSpriteAnimTimerAnchor);
Field(path + ".MOSprite.m_PersistedPrevAngVel", object.m_PersistedPrevAngVel);
Field(path + ".MOSprite.m_PersistedSpriteAnimIsReversingFrames", object.m_PersistedSpriteAnimIsReversingFrames);
Field(path + ".MOSprite.m_HasPersistedSpriteAnimState", object.m_HasPersistedSpriteAnimState);
Field(path + ".MOSprite.m_HasPersistedAngOscillations", object.m_HasPersistedAngOscillations);
Field(path + ".MOSprite.m_SettleMaterialDisabled", object.m_SettleMaterialDisabled);
Field(path + ".MOSprite.m_pEntryWound", object.m_pEntryWound);
Field(path + ".MOSprite.m_pExitWound", object.m_pExitWound);
Field(path + ".MOSprite.m_SpriteModified", object.m_SpriteModified);
}
void Visit(const Magazine& object, const std::string& path) {
Visit(static_cast<const Attachable&>(object), path);
Field(path + ".Magazine.m_RoundCount", object.m_RoundCount);
Field(path + ".Magazine.m_FullCapacity", object.m_FullCapacity);
Field(path + ".Magazine.m_RTTRatio", object.m_RTTRatio);
Field(path + ".Magazine.m_pRegularRound", object.m_pRegularRound);
Field(path + ".Magazine.m_pTracerRound", object.m_pTracerRound);
Field(path + ".Magazine.m_Discardable", object.m_Discardable);
Field(path + ".Magazine.m_AIAimVel", object.m_AIAimVel);
Field(path + ".Magazine.m_AIAimMaxDistance", object.m_AIAimMaxDistance);
Field(path + ".Magazine.m_AIAimPenetration", object.m_AIAimPenetration);
Field(path + ".Magazine.m_AIBlastRadius", object.m_AIBlastRadius);
}
void Visit(const Material& object, const std::string& path) {
Visit(static_cast<const Entity&>(object), path);
Field(path + ".Material.m_Index", object.m_Index);
Field(path + ".Material.m_Priority", object.m_Priority);
Field(path + ".Material.m_Piling", object.m_Piling);
Field(path + ".Material.m_Integrity", object.m_Integrity);
Field(path + ".Material.m_Restitution", object.m_Restitution);
Field(path + ".Material.m_Friction", object.m_Friction);
Field(path + ".Material.m_Stickiness", object.m_Stickiness);
Field(path + ".Material.m_VolumeDensity", object.m_VolumeDensity);
Field(path + ".Material.m_PixelDensity", object.m_PixelDensity);
Field(path + ".Material.m_GibImpulseLimitPerLiter", object.m_GibImpulseLimitPerLiter);
Field(path + ".Material.m_GibWoundLimitPerLiter", object.m_GibWoundLimitPerLiter);
Field(path + ".Material.m_SettleMaterialIndex", object.m_SettleMaterialIndex);
Field(path + ".Material.m_SpawnMaterialIndex", object.m_SpawnMaterialIndex);
Field(path + ".Material.m_IsScrap", object.m_IsScrap);
Field(path + ".Material.m_Color", object.m_Color);
Field(path + ".Material.m_UseOwnColor", object.m_UseOwnColor);
Field(path + ".Material.m_FGTextureFile", object.m_FGTextureFile);
Field(path + ".Material.m_BGTextureFile", object.m_BGTextureFile);
Field(path + ".Material.m_TerrainFGTexture", object.m_TerrainFGTexture);
Field(path + ".Material.m_TerrainBGTexture", object.m_TerrainBGTexture);
}
void Visit(const Matrix& object, const std::string& path) {
Field(path + ".Matrix.m_Rotation", object.m_Rotation);
Field(path + ".Matrix.m_Flipped", object.m_Flipped);
Field(path + ".Matrix.m_Elements", object.m_Elements);
Field(path + ".Matrix.m_ElementsUpdated", object.m_ElementsUpdated);
}
void Visit(const MovableObject& object, const std::string& path) {
Visit(static_cast<const SceneObject&>(object), path);
Field(path + ".MovableObject.m_MOType", object.m_MOType);
Field(path + ".MovableObject.m_Mass", object.m_Mass);
Field(path + ".MovableObject.m_Vel", object.m_Vel);
Field(path + ".MovableObject.m_PrevPos", object.m_PrevPos);
Field(path + ".MovableObject.m_PrevVel", object.m_PrevVel);
Field(path + ".MovableObject.m_DistanceTravelled", object.m_DistanceTravelled);
Field(path + ".MovableObject.m_Scale", object.m_Scale);
Field(path + ".MovableObject.m_GlobalAccScalar", object.m_GlobalAccScalar);
Field(path + ".MovableObject.m_AirResistance", object.m_AirResistance);
Field(path + ".MovableObject.m_AirThreshold", object.m_AirThreshold);
Field(path + ".MovableObject.m_PinStrength", object.m_PinStrength);
Field(path + ".MovableObject.m_RestThreshold", object.m_RestThreshold);
Field(path + ".MovableObject.m_Forces", object.m_Forces);
Field(path + ".MovableObject.m_ImpulseForces", object.m_ImpulseForces);
Field(path + ".MovableObject.m_AgeTimer", object.m_AgeTimer);
Field(path + ".MovableObject.m_RestTimer", object.m_RestTimer);
Field(path + ".MovableObject.m_Lifetime", object.m_Lifetime);
Field(path + ".MovableObject.m_Sharpness", object.m_Sharpness);
Field(path + ".MovableObject.m_CheckTerrIntersection", object.m_CheckTerrIntersection);
Field(path + ".MovableObject.m_HitsMOs", object.m_HitsMOs);
Field(path + ".MovableObject.m_pMOToNotHit", object.m_pMOToNotHit);
Field(path + ".MovableObject.m_MOToNotHitUID", object.m_MOToNotHitUID);
Field(path + ".MovableObject.m_MOIgnoreTimer", object.m_MOIgnoreTimer);
Field(path + ".MovableObject.m_GetsHitByMOs", object.m_GetsHitByMOs);
Field(path + ".MovableObject.m_IgnoresTeamHits", object.m_IgnoresTeamHits);
Field(path + ".MovableObject.m_IgnoresAtomGroupHits", object.m_IgnoresAtomGroupHits);
Field(path + ".MovableObject.m_IgnoresAGHitsWhenSlowerThan", object.m_IgnoresAGHitsWhenSlowerThan);
Field(path + ".MovableObject.m_IgnoresActorHits", object.m_IgnoresActorHits);
Field(path + ".MovableObject.m_MissionCritical", object.m_MissionCritical);
Field(path + ".MovableObject.m_CanBeSquished", object.m_CanBeSquished);
Field(path + ".MovableObject.m_IsUpdated", object.m_IsUpdated);
Field(path + ".MovableObject.m_WrapDoubleDraw", object.m_WrapDoubleDraw);
Field(path + ".MovableObject.m_DidWrap", object.m_DidWrap);
Field(path + ".MovableObject.m_MOID", object.m_MOID);
Field(path + ".MovableObject.m_RootMOID", object.m_RootMOID);
Field(path + ".MovableObject.m_MOIDFootprint", object.m_MOIDFootprint);
Field(path + ".MovableObject.m_HasEverBeenAddedToMovableMan", object.m_HasEverBeenAddedToMovableMan);
Field(path + ".MovableObject.m_AlreadyHitBy", object.m_AlreadyHitBy);
Field(path + ".MovableObject.m_VelOscillations", object.m_VelOscillations);
Field(path + ".MovableObject.m_ToSettle", object.m_ToSettle);
Field(path + ".MovableObject.m_ToDelete", object.m_ToDelete);
Field(path + ".MovableObject.m_HUDVisible", object.m_HUDVisible);
Field(path + ".MovableObject.m_IsTraveling", object.m_IsTraveling);
Field(path + ".MovableObject.m_ThreadedLuaState", object.m_ThreadedLuaState);
Field(path + ".MovableObject.m_ForceIntoMasterLuaState", object.m_ForceIntoMasterLuaState);
Field(path + ".MovableObject.m_ScriptObjectName", object.m_ScriptObjectName);
Field(path + ".MovableObject.m_AllLoadedScripts", object.m_AllLoadedScripts);
Field(path + ".MovableObject.m_EnabledScripts", object.m_EnabledScripts);
Field(path + ".MovableObject.m_FunctionsAndScripts", object.m_FunctionsAndScripts);
Field(path + ".MovableObject.m_RequestedSyncedUpdate", object.m_RequestedSyncedUpdate);
Field(path + ".MovableObject.m_StringValueMap", object.m_StringValueMap);
Field(path + ".MovableObject.m_NumberValueMap", object.m_NumberValueMap);
Field(path + ".MovableObject.m_ObjectValueMap", object.m_ObjectValueMap);
Field(path + ".MovableObject.m_ScreenEffectFile", object.m_ScreenEffectFile);
Field(path + ".MovableObject.m_pScreenEffect", object.m_pScreenEffect);
Field(path + ".MovableObject.m_ScreenEffectHash", object.m_ScreenEffectHash);
Field(path + ".MovableObject.m_EffectStartTime", object.m_EffectStartTime);
Field(path + ".MovableObject.m_EffectStopTime", object.m_EffectStopTime);
Field(path + ".MovableObject.m_EffectStartStrength", object.m_EffectStartStrength);
Field(path + ".MovableObject.m_EffectStopStrength", object.m_EffectStopStrength);
Field(path + ".MovableObject.m_EffectAlwaysShows", object.m_EffectAlwaysShows);
Field(path + ".MovableObject.m_EffectRotAngle", object.m_EffectRotAngle);
Field(path + ".MovableObject.m_InheritEffectRotAngle", object.m_InheritEffectRotAngle);
Field(path + ".MovableObject.m_RandomizeEffectRotAngle", object.m_RandomizeEffectRotAngle);
Field(path + ".MovableObject.m_RandomizeEffectRotAngleEveryFrame", object.m_RandomizeEffectRotAngleEveryFrame);
Field(path + ".MovableObject.m_PostEffectEnabled", object.m_PostEffectEnabled);
Field(path + ".MovableObject.m_UniqueID", object.m_UniqueID);
Field(path + ".MovableObject.m_PersistedUniqueID", object.m_PersistedUniqueID);
Field(path + ".MovableObject.m_ScriptStateRestored", object.m_ScriptStateRestored);
Field(path + ".MovableObject.m_PersistedScriptState", object.m_PersistedScriptState);
Field(path + ".MovableObject.m_PersistedLuaStateIndex", object.m_PersistedLuaStateIndex);
Field(path + ".MovableObject.m_FaithfulMOToNotHitUID", object.m_FaithfulMOToNotHitUID);
Field(path + ".MovableObject.m_PersistedRestTimerStart", object.m_PersistedRestTimerStart);
Field(path + ".MovableObject.m_HasPersistedRestTimerStart", object.m_HasPersistedRestTimerStart);
Field(path + ".MovableObject.m_PersistedVelOscillations", object.m_PersistedVelOscillations);
Field(path + ".MovableObject.m_HasPersistedVelOscillations", object.m_HasPersistedVelOscillations);
Field(path + ".MovableObject.m_PersistedAgeTimerAnchor", object.m_PersistedAgeTimerAnchor);
Field(path + ".MovableObject.m_PersistedMOIgnoreTimerAnchor", object.m_PersistedMOIgnoreTimerAnchor);
Field(path + ".MovableObject.m_RemoveOrphanTerrainRadius", object.m_RemoveOrphanTerrainRadius);
Field(path + ".MovableObject.m_RemoveOrphanTerrainMaxArea", object.m_RemoveOrphanTerrainMaxArea);
Field(path + ".MovableObject.m_RemoveOrphanTerrainRate", object.m_RemoveOrphanTerrainRate);
Field(path + ".MovableObject.m_DamageOnCollision", object.m_DamageOnCollision);
Field(path + ".MovableObject.m_DamageOnPenetration", object.m_DamageOnPenetration);
Field(path + ".MovableObject.m_WoundDamageMultiplier", object.m_WoundDamageMultiplier);
Field(path + ".MovableObject.m_ApplyWoundDamageOnCollision", object.m_ApplyWoundDamageOnCollision);
Field(path + ".MovableObject.m_ApplyWoundBurstDamageOnCollision", object.m_ApplyWoundBurstDamageOnCollision);
Field(path + ".MovableObject.m_IgnoreTerrain", object.m_IgnoreTerrain);
Field(path + ".MovableObject.m_MOIDHit", object.m_MOIDHit);
Field(path + ".MovableObject.m_TerrainMatHit", object.m_TerrainMatHit);
Field(path + ".MovableObject.m_ParticleUniqueIDHit", object.m_ParticleUniqueIDHit);
Field(path + ".MovableObject.m_LastCollisionSimFrameNumber", object.m_LastCollisionSimFrameNumber);
Field(path + ".MovableObject.m_SimUpdatesBetweenScriptedUpdates", object.m_SimUpdatesBetweenScriptedUpdates);
Field(path + ".MovableObject.m_SimUpdatesSinceLastScriptedUpdate", object.m_SimUpdatesSinceLastScriptedUpdate);
}
void Visit(const MovableObject::LuaFunction& object, const std::string& path) {
Field(path + ".MovableObject::LuaFunction.m_ScriptIsEnabled", object.m_ScriptIsEnabled);
Field(path + ".MovableObject::LuaFunction.m_LuaFunction", object.m_LuaFunction);
}
void Visit(const PEmitter& object, const std::string& path) {
Visit(static_cast<const MOSParticle&>(object), path);
Field(path + ".PEmitter.m_EmissionList", object.m_EmissionList);
Field(path + ".PEmitter.m_EmissionSound", object.m_EmissionSound);
Field(path + ".PEmitter.m_BurstSound", object.m_BurstSound);
Field(path + ".PEmitter.m_EndSound", object.m_EndSound);
Field(path + ".PEmitter.m_EmitEnabled", object.m_EmitEnabled);
Field(path + ".PEmitter.m_WasEmitting", object.m_WasEmitting);
Field(path + ".PEmitter.m_PersistedBurstTimerAnchor", object.m_PersistedBurstTimerAnchor);
Field(path + ".PEmitter.m_PersistedLastEmitTimerAnchor", object.m_PersistedLastEmitTimerAnchor);
Field(path + ".PEmitter.m_PersistedEmissionAccumulators", object.m_PersistedEmissionAccumulators);
Field(path + ".PEmitter.m_PersistedEmissionTimers", object.m_PersistedEmissionTimers);
Field(path + ".PEmitter.m_EmitCount", object.m_EmitCount);
Field(path + ".PEmitter.m_EmitCountLimit", object.m_EmitCountLimit);
Field(path + ".PEmitter.m_NegativeThrottleMultiplier", object.m_NegativeThrottleMultiplier);
Field(path + ".PEmitter.m_PositiveThrottleMultiplier", object.m_PositiveThrottleMultiplier);
Field(path + ".PEmitter.m_Throttle", object.m_Throttle);
Field(path + ".PEmitter.m_EmissionsIgnoreThis", object.m_EmissionsIgnoreThis);
Field(path + ".PEmitter.m_BurstScale", object.m_BurstScale);
Field(path + ".PEmitter.m_BurstTriggered", object.m_BurstTriggered);
Field(path + ".PEmitter.m_BurstSpacing", object.m_BurstSpacing);
Field(path + ".PEmitter.m_BurstTimer", object.m_BurstTimer);
Field(path + ".PEmitter.m_PlayBurstSound", object.m_PlayBurstSound);
Field(path + ".PEmitter.m_EmitAngle", object.m_EmitAngle);
Field(path + ".PEmitter.m_EmissionOffset", object.m_EmissionOffset);
Field(path + ".PEmitter.m_LastEmitTmr", object.m_LastEmitTmr);
Field(path + ".PEmitter.m_FlashScale", object.m_FlashScale);
Field(path + ".PEmitter.m_AvgBurstImpulse", object.m_AvgBurstImpulse);
Field(path + ".PEmitter.m_AvgImpulse", object.m_AvgImpulse);
Field(path + ".PEmitter.m_LoudnessOnEmit", object.m_LoudnessOnEmit);
Field(path + ".PEmitter.m_FlashOnlyOnBurst", object.m_FlashOnlyOnBurst);
Field(path + ".PEmitter.m_SustainBurstSound", object.m_SustainBurstSound);
Field(path + ".PEmitter.m_BurstSoundFollowsEmitter", object.m_BurstSoundFollowsEmitter);
}
void Visit(const PersistedTimerAnchor& object, const std::string& path) {
Field(path + ".PersistedTimerAnchor.startTicks", object.startTicks);
Field(path + ".PersistedTimerAnchor.pending", object.pending);
}
void Visit(const PieMenu& object, const std::string& path) {
Visit(static_cast<const Entity&>(object), path);
Field(path + ".PieMenu.m_LargeFont", object.m_LargeFont);
Field(path + ".PieMenu.m_Owner", object.m_Owner);
Field(path + ".PieMenu.m_MenuController", object.m_MenuController);
Field(path + ".PieMenu.m_AffectedObject", object.m_AffectedObject);
Field(path + ".PieMenu.m_DirectionIfSubPieMenu", object.m_DirectionIfSubPieMenu);
Field(path + ".PieMenu.m_MenuMode", object.m_MenuMode);
Field(path + ".PieMenu.m_CenterPos", object.m_CenterPos);
Field(path + ".PieMenu.m_Rotation", object.m_Rotation);
Field(path + ".PieMenu.m_EnabledState", object.m_EnabledState);
Field(path + ".PieMenu.m_EnableDisableAnimationTimer", object.m_EnableDisableAnimationTimer);
Field(path + ".PieMenu.m_HoverTimer", object.m_HoverTimer);
Field(path + ".PieMenu.m_SubPieMenuHoverOpenTimer", object.m_SubPieMenuHoverOpenTimer);
Field(path + ".PieMenu.m_IconSeparatorMode", object.m_IconSeparatorMode);
Field(path + ".PieMenu.m_FullInnerRadius", object.m_FullInnerRadius);
Field(path + ".PieMenu.m_BackgroundThickness", object.m_BackgroundThickness);
Field(path + ".PieMenu.m_BackgroundSeparatorSize", object.m_BackgroundSeparatorSize);
Field(path + ".PieMenu.m_DrawBackgroundTransparent", object.m_DrawBackgroundTransparent);
Field(path + ".PieMenu.m_BackgroundColor", object.m_BackgroundColor);
Field(path + ".PieMenu.m_BackgroundBorderColor", object.m_BackgroundBorderColor);
Field(path + ".PieMenu.m_SelectedItemBackgroundColor", object.m_SelectedItemBackgroundColor);
Field(path + ".PieMenu.m_PieQuadrants", object.m_PieQuadrants);
Field(path + ".PieMenu.m_HoveredPieSlice", object.m_HoveredPieSlice);
Field(path + ".PieMenu.m_ActivatedPieSlice", object.m_ActivatedPieSlice);
Field(path + ".PieMenu.m_AlreadyActivatedPieSlice", object.m_AlreadyActivatedPieSlice);
Field(path + ".PieMenu.m_CurrentPieSlices", object.m_CurrentPieSlices);
Field(path + ".PieMenu.m_ActiveSubPieMenu", object.m_ActiveSubPieMenu);
Field(path + ".PieMenu.m_WhilePieMenuOpenListeners", object.m_WhilePieMenuOpenListeners);
Field(path + ".PieMenu.m_CurrentInnerRadius", object.m_CurrentInnerRadius);
Field(path + ".PieMenu.m_CursorInVisiblePosition", object.m_CursorInVisiblePosition);
Field(path + ".PieMenu.m_CursorAngle", object.m_CursorAngle);
Field(path + ".PieMenu.m_CursorVisualAngle", object.m_CursorVisualAngle);
Field(path + ".PieMenu.m_BGBitmap", object.m_BGBitmap);
Field(path + ".PieMenu.m_BGRotationBitmap", object.m_BGRotationBitmap);
Field(path + ".PieMenu.m_BGPieSlicesWithSubPieMenuBitmap", object.m_BGPieSlicesWithSubPieMenuBitmap);
Field(path + ".PieMenu.m_BGBitmapNeedsRedrawing", object.m_BGBitmapNeedsRedrawing);
Field(path + ".PieMenu.m_BGPieSlicesWithSubPieMenuBitmapNeedsRedrawing", object.m_BGPieSlicesWithSubPieMenuBitmapNeedsRedrawing);
}
void Visit(const PieSlice& object, const std::string& path) {
Visit(static_cast<const Entity&>(object), path);
Field(path + ".PieSlice.m_Type", object.m_Type);
Field(path + ".PieSlice.m_Direction", object.m_Direction);
Field(path + ".PieSlice.m_CanBeMiddleSlice", object.m_CanBeMiddleSlice);
Field(path + ".PieSlice.m_OriginalSource", object.m_OriginalSource);
Field(path + ".PieSlice.m_Enabled", object.m_Enabled);
Field(path + ".PieSlice.m_Icon", object.m_Icon);
Field(path + ".PieSlice.m_LuabindFunctionObject", object.m_LuabindFunctionObject);
Field(path + ".PieSlice.m_FunctionName", object.m_FunctionName);
Field(path + ".PieSlice.m_SubPieMenu", object.m_SubPieMenu);
Field(path + ".PieSlice.m_StartAngle", object.m_StartAngle);
Field(path + ".PieSlice.m_SlotCount", object.m_SlotCount);
Field(path + ".PieSlice.m_MidAngle", object.m_MidAngle);
Field(path + ".PieSlice.m_DrawFlippedToMatchAbsoluteAngle", object.m_DrawFlippedToMatchAbsoluteAngle);
}
void Visit(const RandomGenerator& object, const std::string& path) {
Field(path + ".RandomGenerator.m_RNG", object.m_RNG);
Field(path + ".RandomGenerator.m_Seed", object.m_Seed);
Field(path + ".RandomGenerator.m_DrawCount", object.m_DrawCount);
Field(path + ".RandomGenerator.m_TraceDraws", object.m_TraceDraws);
}
void Visit(const Round& object, const std::string& path) {
Visit(static_cast<const Entity&>(object), path);
Field(path + ".Round.m_Particle", object.m_Particle);
Field(path + ".Round.m_ParticleCount", object.m_ParticleCount);
Field(path + ".Round.m_FireVel", object.m_FireVel);
Field(path + ".Round.m_InheritsFirerVelocity", object.m_InheritsFirerVelocity);
Field(path + ".Round.m_Separation", object.m_Separation);
Field(path + ".Round.m_LifeVariation", object.m_LifeVariation);
Field(path + ".Round.m_Shell", object.m_Shell);
Field(path + ".Round.m_ShellVel", object.m_ShellVel);
Field(path + ".Round.m_FireSound", object.m_FireSound);
Field(path + ".Round.m_AILifeTime", object.m_AILifeTime);
Field(path + ".Round.m_AIFireVel", object.m_AIFireVel);
Field(path + ".Round.m_AIPenetration", object.m_AIPenetration);
}
void Visit(const PathFinder& object, const std::string& path) {
Field(path + ".node_dimension", object.m_NodeDimension);
Field(path + ".offset", object.m_Offset);
Field(path + ".width", object.m_GridWidth);
Field(path + ".height", object.m_GridHeight);
Field(path + ".wrap_x", object.m_WrapsX);
Field(path + ".wrap_y", object.m_WrapsY);
Field(path + ".requests", object.m_CurrentPathingRequests.load());
Field(path + ".node_count", object.m_NodeGrid.size());
for (size_t index = 0; index < object.m_NodeGrid.size(); ++index) {
    const auto& node = object.m_NodeGrid[index];
    const std::string base = path + ".node[" + std::to_string(index) + "]";
    Field(base + ".position", node.Pos);
    Field(base + ".navigable", node.m_Navigable);
    for (size_t direction = 0; direction < node.AdjacentNodes.size(); ++direction) {
        Field(base + ".neighbor[" + std::to_string(direction) + "]", node.AdjacentNodes[direction] ? static_cast<int64_t>(node.AdjacentNodes[direction] - object.m_NodeGrid.data()) : int64_t{-1});
        Field(base + ".material[" + std::to_string(direction) + "]", node.AdjacentNodeBlockingMaterials[direction] ? static_cast<int>(node.AdjacentNodeBlockingMaterials[direction]->GetIndex()) : -1);
    }
}
}
void Visit(const Scene& object, const std::string& path) {
Visit(static_cast<const Entity&>(object), path);
Field(path + ".Scene.m_Location", object.m_Location);
Field(path + ".Scene.m_LocationOffset", object.m_LocationOffset);
Field(path + ".Scene.m_MetagamePlayable", object.m_MetagamePlayable);
Field(path + ".Scene.m_Revealed", object.m_Revealed);
Field(path + ".Scene.m_OwnedByTeam", object.m_OwnedByTeam);
Field(path + ".Scene.m_RoundIncome", object.m_RoundIncome);
Field(path + ".Scene.m_ResidentBrains", object.m_ResidentBrains);
Field(path + ".Scene.m_BuildBudget", object.m_BuildBudget);
Field(path + ".Scene.m_BuildBudgetRatio", object.m_BuildBudgetRatio);
Field(path + ".Scene.m_AutoDesigned", object.m_AutoDesigned);
Field(path + ".Scene.m_TotalInvestment", object.m_TotalInvestment);
Field(path + ".Scene.m_pTerrain", object.m_pTerrain);
Field(path + ".Scene.m_pPathFinders", object.m_pPathFinders);
Field(path + ".Scene.m_PathfindingUpdated", object.m_PathfindingUpdated);
Field(path + ".Scene.m_PartialPathUpdateTimer", object.m_PartialPathUpdateTimer);
Field(path + ".Scene.m_PlacedObjects", object.m_PlacedObjects);
Field(path + ".Scene.m_BackLayerList", object.m_BackLayerList);
Field(path + ".Scene.m_UnseenPixelSize", object.m_UnseenPixelSize);
Field(path + ".Scene.m_apUnseenLayer", object.m_apUnseenLayer);
Field(path + ".Scene.m_SeenPixels", object.m_SeenPixels);
Field(path + ".Scene.m_CleanedPixels", object.m_CleanedPixels);
Field(path + ".Scene.m_ScanScheduled", object.m_ScanScheduled);
Field(path + ".Scene.m_AreaList", object.m_AreaList);
Field(path + ".Scene.m_NavigableAreas", object.m_NavigableAreas);
Field(path + ".Scene.m_NavigableAreasUpToDate", object.m_NavigableAreasUpToDate);
Field(path + ".Scene.m_GlobalAcc", object.m_GlobalAcc);
Field(path + ".Scene.m_SelectedAssemblies", object.m_SelectedAssemblies);
Field(path + ".Scene.m_AssembliesCounts", object.m_AssembliesCounts);
Field(path + ".Scene.m_pPreviewBitmap", object.m_pPreviewBitmap);
Field(path + ".Scene.m_PreviewBitmapFile", object.m_PreviewBitmapFile);
Field(path + ".Scene.m_MetasceneParent", object.m_MetasceneParent);
Field(path + ".Scene.m_IsMetagameInternal", object.m_IsMetagameInternal);
Field(path + ".Scene.m_IsSavedGameInternal", object.m_IsSavedGameInternal);
Field(path + ".Scene.m_Deployments", object.m_Deployments);
}
void Visit(const Scene::Area& object, const std::string& path) {
Field(path + ".Scene::Area.m_BoxList", object.m_BoxList);
Field(path + ".Scene::Area.m_Name", object.m_Name);
}
void Visit(const SceneObject& object, const std::string& path) {
Visit(static_cast<const Entity&>(object), path);
Field(path + ".SceneObject.m_Pos", object.m_Pos);
Field(path + ".SceneObject.m_OzValue", object.m_OzValue);
Field(path + ".SceneObject.m_Buyable", object.m_Buyable);
Field(path + ".SceneObject.m_BuyableMode", object.m_BuyableMode);
Field(path + ".SceneObject.m_Team", object.m_Team);
Field(path + ".SceneObject.m_PlacedByPlayer", object.m_PlacedByPlayer);
}
void Visit(const SoundContainer& object, const std::string& path) {
Visit(static_cast<const Entity&>(object), path);
Field(path + ".SoundContainer.m_TopLevelSoundSet", object.m_TopLevelSoundSet);
Field(path + ".SoundContainer.m_PlayingChannels", object.m_PlayingChannels);
Field(path + ".SoundContainer.m_SoundOverlapMode", object.m_SoundOverlapMode);
Field(path + ".SoundContainer.m_BusRouting", object.m_BusRouting);
Field(path + ".SoundContainer.m_Immobile", object.m_Immobile);
Field(path + ".SoundContainer.m_AttenuationStartDistance", object.m_AttenuationStartDistance);
Field(path + ".SoundContainer.m_CustomPanValue", object.m_CustomPanValue);
Field(path + ".SoundContainer.m_PanningStrengthMultiplier", object.m_PanningStrengthMultiplier);
Field(path + ".SoundContainer.m_Loops", object.m_Loops);
Field(path + ".SoundContainer.m_SoundPropertiesUpToDate", object.m_SoundPropertiesUpToDate);
Field(path + ".SoundContainer.m_Priority", object.m_Priority);
Field(path + ".SoundContainer.m_AffectedByGlobalPitch", object.m_AffectedByGlobalPitch);
Field(path + ".SoundContainer.m_Pos", object.m_Pos);
Field(path + ".SoundContainer.m_Pitch", object.m_Pitch);
Field(path + ".SoundContainer.m_PitchVariation", object.m_PitchVariation);
Field(path + ".SoundContainer.m_Volume", object.m_Volume);
Field(path + ".SoundContainer.m_WasFadedOut", object.m_WasFadedOut);
Field(path + ".SoundContainer.m_Paused", object.m_Paused);
Field(path + ".SoundContainer.m_MusicPreEntryTime", object.m_MusicPreEntryTime);
Field(path + ".SoundContainer.m_MusicExitTime", object.m_MusicExitTime);
}
void Visit(const SoundData& object, const std::string& path) {
Field(path + ".SoundData.SoundFile", object.SoundFile);
Field(path + ".SoundData.SoundObject", object.SoundObject);
Field(path + ".SoundData.Offset", object.Offset);
Field(path + ".SoundData.MinimumAudibleDistance", object.MinimumAudibleDistance);
Field(path + ".SoundData.AttenuationStartDistance", object.AttenuationStartDistance);
}
void Visit(const SoundSet& object, const std::string& path) {
Field(path + ".SoundSet.m_SoundSelectionCycleMode", object.m_SoundSelectionCycleMode);
Field(path + ".SoundSet.m_CurrentSelection", object.m_CurrentSelection);
Field(path + ".SoundSet.m_SoundData", object.m_SoundData);
Field(path + ".SoundSet.m_SubSoundSets", object.m_SubSoundSets);
}
void Visit(const TDExplosive& object, const std::string& path) {
Visit(static_cast<const ThrownDevice&>(object), path);
Field(path + ".TDExplosive.m_IsAnimatedManually", object.m_IsAnimatedManually);
}
void Visit(const ThrownDevice& object, const std::string& path) {
Visit(static_cast<const HeldDevice&>(object), path);
Field(path + ".ThrownDevice.m_ActivationSound", object.m_ActivationSound);
Field(path + ".ThrownDevice.m_StartThrowOffset", object.m_StartThrowOffset);
Field(path + ".ThrownDevice.m_EndThrowOffset", object.m_EndThrowOffset);
Field(path + ".ThrownDevice.m_MinThrowVel", object.m_MinThrowVel);
Field(path + ".ThrownDevice.m_MaxThrowVel", object.m_MaxThrowVel);
Field(path + ".ThrownDevice.m_TriggerDelay", object.m_TriggerDelay);
Field(path + ".ThrownDevice.m_ActivatesWhenReleased", object.m_ActivatesWhenReleased);
Field(path + ".ThrownDevice.m_StrikerLever", object.m_StrikerLever);
}
void Visit(const Timer& object, const std::string& path) {
Field(path + ".Timer.m_TicksPerMS", object.m_TicksPerMS);
Field(path + ".Timer.m_StartRealTime", object.m_StartRealTime);
Field(path + ".Timer.m_RealTimeLimit", object.m_RealTimeLimit);
Field(path + ".Timer.m_StartSimTime", object.m_StartSimTime);
Field(path + ".Timer.m_SimTimeLimit", object.m_SimTimeLimit);
}
void Visit(const Turret& object, const std::string& path) {
Visit(static_cast<const Attachable&>(object), path);
Field(path + ".Turret.m_MountedDevices", object.m_MountedDevices);
Field(path + ".Turret.m_MountedDeviceRotationOffset", object.m_MountedDeviceRotationOffset);
}
void Visit(const Vector& object, const std::string& path) {
Field(path + ".Vector.m_X", object.m_X);
Field(path + ".Vector.m_Y", object.m_Y);
}

void Visit(const InputMapping& object, const std::string& path) {
Field(path + ".description", object.m_PresetDescription);
Field(path + ".key", object.m_KeyMap);
Field(path + ".mouse_button", object.m_MouseButtonMap);
Field(path + ".direction_mapped", object.m_DirectionMapped);
Field(path + ".joystick_button", object.m_JoyButtonMap);
Field(path + ".axis", object.m_AxisMap);
Field(path + ".direction", object.m_DirectionMap);
}
void Visit(const InputScheme& object, const std::string& path) {
Field(path + ".device", object.m_ActiveDevice);
Field(path + ".device_id", std::bit_cast<std::array<uint32_t, 2>>(object.m_DeviceID));
Field(path + ".preset", object.m_SchemePreset);
Field(path + ".deadzone_type", object.m_JoystickDeadzoneType);
Field(path + ".deadzone", object.m_JoystickDeadzone);
Field(path + ".digital_aim_speed", object.m_DigitalAimSpeed);
Field(path + ".mappings", object.m_InputMappings);
}
void Visit(const UInputMan::Keyboard& object, const std::string& path) {
Field(path + ".id", object.id);
Field(path + ".held", object.keyStates);
Field(path + ".changed", object.changedKeyStates);
Field(path + ".pressed_since_sim", object.pressedSinceSim);
Field(path + ".released_since_sim", object.releasedSinceSim);
}
void Visit(const UInputMan::Mouse& object, const std::string& path) {
Field(path + ".id", object.id);
Field(path + ".held", object.state);
Field(path + ".changed", object.change);
Field(path + ".pressed_since_sim", object.pressedSinceSim);
Field(path + ".released_since_sim", object.releasedSinceSim);
Field(path + ".position", object.position);
Field(path + ".relative_motion", object.relativeMotion);
Field(path + ".analog_aim", object.analogAim);
Field(path + ".wheel_change", object.wheelChange);
Field(path + ".relative_mode", object.relativeMode);
}
void Visit(const Gamepad& object, const std::string& path) {
Field(path + ".device_index", object.m_DeviceIndex);
Field(path + ".joystick_id", object.m_JoystickID);
Field(path + ".axis", object.m_Axis);
Field(path + ".digital_axis", object.m_DigitalAxis);
Field(path + ".buttons", object.m_Buttons);
Field(path + ".pressed_since_sim", object.m_ButtonsPressedSinceSim);
Field(path + ".released_since_sim", object.m_ButtonsReleasedSinceSim);
}
void Visit(const UInputMan& object, const std::string& path) {
Field(path + ".keyboards", object.m_KeyboardStates);
Field(path + ".mice", object.m_MouseStates);
Field(path + ".joysticks", object.s_PrevJoystickStates);
Field(path + ".changed_joysticks", object.s_ChangedJoystickStates);
Field(path + ".skip_special", object.m_SkipHandlingSpecialInput);
Field(path + ".joystick_count", object.m_NumJoysticks);
Field(path + ".text", object.m_TextInput);
Field(path + ".override", object.m_OverrideInput);
Field(path + ".schemes", object.m_ControlScheme);
Field(path + ".device_icons", object.m_DeviceIcons);
Field(path + ".mouse_sensitivity", object.m_MouseSensitivity);
Field(path + ".trap_mouse", object.m_TrapMousePos);
Field(path + ".trap_radius", object.m_MouseTrapRadius);
Field(path + ".bounds_x", object.m_PlayerScreenMouseBounds.x);
Field(path + ".bounds_y", object.m_PlayerScreenMouseBounds.y);
Field(path + ".bounds_width", object.m_PlayerScreenMouseBounds.w);
Field(path + ".bounds_height", object.m_PlayerScreenMouseBounds.h);
Field(path + ".last_cursor_device", object.m_LastDeviceWhichControlledGUICursor);
Field(path + ".force_disable_multi", object.m_ForceDisableMultiMouseKeyboard);
Field(path + ".enable_multi", object.m_EnableMultiMouseKeyboard);
Field(path + ".player_devices_known", object.m_PlayerMouseKeyboardKnown);
Field(path + ".disable_keyboard", object.m_DisableKeyboard);
Field(path + ".disable_mouse_motion", object.m_DisableMouseMoving);
Field(path + ".prepare_mouse_motion", object.m_PrepareToEnableMouseMoving);
Field(path + ".unused_event_queue_size", object.m_EventQueue.size());
}

static uint64_t HashBytes(const void* storage, size_t size) {
    uint64_t hash = 1469598103934665603ULL;
    const auto* bytes = static_cast<const unsigned char*>(storage);
    for (size_t index = 0; index < size; ++index) hash = (hash ^ bytes[index]) * 1099511628211ULL;
    return hash;
}
void Visit(const BITMAP& bitmap, const std::string& path) {
    Field(path + ".width", bitmap.w); Field(path + ".height", bitmap.h);
    const int depth = bitmap_color_depth(const_cast<BITMAP*>(&bitmap));
    Field(path + ".depth", depth);
    Field(path + ".clip", bitmap.clip); Field(path + ".clip_left", bitmap.cl); Field(path + ".clip_top", bitmap.ct);
    Field(path + ".clip_right", bitmap.cr); Field(path + ".clip_bottom", bitmap.cb);
    uint64_t hash = 1469598103934665603ULL;
    const size_t bytesPerRow = static_cast<size_t>(bitmap.w) * ((depth + 7) / 8);
    for (int y = 0; y < bitmap.h; ++y) {
        for (size_t x = 0; x < bytesPerRow; ++x) hash = (hash ^ bitmap.line[y][x]) * 1099511628211ULL;
    }
    Field(path + ".pixels", hash);
}
void Visit(const PostEffect& effect, const std::string& path) {
    Field(path + ".bitmap", effect.m_Bitmap); Field(path + ".bitmap_hash", effect.m_BitmapHash);
    Field(path + ".angle", effect.m_Angle); Field(path + ".strength", effect.m_Strength);
    Field(path + ".position", effect.m_Pos); Field(path + ".attached_moid", effect.m_AttachedToMOID);
}
void Visit(const IntRect& rect, const std::string& path) {
    Field(path + ".left", rect.m_Left); Field(path + ".right", rect.m_Right);
    Field(path + ".top", rect.m_Top); Field(path + ".bottom", rect.m_Bottom);
}
void Visit(const PostProcessMan& object, const std::string& path) {
    Field(path + ".screen_effects", object.m_PostScreenEffects); Field(path + ".scene_effects", object.m_PostSceneEffects);
    Field(path + ".screen_glow_boxes", object.m_PostScreenGlowBoxes); Field(path + ".glow_areas", object.m_GlowAreas);
    Field(path + ".player_effects", object.m_ScreenRelativeEffects);
    Field(path + ".yellow", object.m_YellowGlow); Field(path + ".red", object.m_RedGlow); Field(path + ".blue", object.m_BlueGlow);
    Field(path + ".yellow_hash", object.m_YellowGlowHash); Field(path + ".red_hash", object.m_RedGlowHash); Field(path + ".blue_hash", object.m_BlueGlowHash);
    Field(path + ".temporary_bitmaps", object.m_TempEffectBitmaps);
    Field(path + ".registration_suppressed", object.s_RegistrationSuppressed);
}
void Visit(const FrameMan& object, const std::string& path) {
    Field(path + ".horizontal_split", object.m_HSplit); Field(path + ".vertical_split", object.m_VSplit);
    Field(path + ".two_player_vertical", object.m_TwoPlayerVSplit); Field(path + ".palette_file", object.m_PaletteFile);
    Field(path + ".palette", HashBytes(object.m_Palette, sizeof(object.m_Palette)));
    Field(path + ".default_palette", HashBytes(object.m_DefaultPalette, sizeof(object.m_DefaultPalette)));
    PALETTE current; get_palette(current); Field(path + ".current_palette", HashBytes(current, sizeof(current)));
    Field(path + ".rgb_table", HashBytes(&object.m_RGBTable, sizeof(object.m_RGBTable)));
    Field(path + ".black", object.m_BlackColor); Field(path + ".almost_black", object.m_AlmostBlackColor);
    std::string active = color_map ? "external" : "null";
    for (size_t mode = 0; mode < object.m_ColorTables.size(); ++mode) {
        const auto& entries = object.m_ColorTables[mode];
        Field(path + ".table_count[" + std::to_string(mode) + "]", entries.size());
        for (const auto& [key, value]: entries) {
            const std::string name = path + ".table[" + std::to_string(mode) + ":" + Value(key, path + ".table_key") + "]";
            Field(name + ".pixels", HashBytes(&value.first, sizeof(value.first))); Field(name + ".last_use", value.second);
            if (color_map == &value.first) active = name;
        }
    }
    Field(path + ".active_table", active);
    if (color_map) Field(path + ".active_table_pixels", HashBytes(color_map, sizeof(*color_map)));
    Field(path + ".table_prune_timer", object.m_ColorTablePruneTimer); Field(path + ".alpha", object.m_CurrentAlpha);
    Field(path + ".screen_text", object.m_ScreenText); Field(path + ".text_centered", object.m_TextCentered);
    Field(path + ".text_duration", object.m_TextDuration); Field(path + ".text_timer", object.m_TextDurationTimer);
    Field(path + ".text_blink", object.m_TextBlinking); Field(path + ".text_blink_timer", object.m_TextBlinkTimer);
    Field(path + ".hud_disabled", object.m_HUDDisabled); Field(path + ".flash_color", object.m_FlashScreenColor);
    Field(path + ".flashed_last_frame", object.m_FlashedLastFrame); Field(path + ".flash_timer", object.m_FlashTimer);
    Field(path + ".player_width", object.m_PlayerScreenWidth); Field(path + ".player_height", object.m_PlayerScreenHeight);
    Field(path + ".small_fonts", object.m_SmallFonts); Field(path + ".large_fonts", object.m_LargeFonts);
    Field(path + ".backbuffer8", object.m_BackBuffer8); Field(path + ".backbuffer32", object.m_BackBuffer32);
    Field(path + ".overlay32", object.m_OverlayBitmap32); Field(path + ".player_screen8", object.m_PlayerScreen8);
}

static void Write(const State& state, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    for (const auto& [key, value]: state) out << key << " = " << value << "\n";
}
static size_t Compare(const State& before, const State& after, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    size_t count = 0;
    auto a = before.begin(), b = after.begin();
    while (a != before.end() || b != after.end()) {
        const bool hasA = a != before.end() && (b == after.end() || a->first <= b->first);
        const bool hasB = b != after.end() && (a == before.end() || b->first <= a->first);
        if (hasA && hasB && a->second == b->second) { ++a; ++b; continue; }
        ++count;
        out << (hasA ? a->first : b->first) << "\n  before " << (hasA ? a->second : "<absent>")
            << "\n  after  " << (hasB ? b->second : "<absent>") << "\n";
        if (hasA) ++a;
        if (hasB) ++b;
    }
    return count;
}
void Visit(const GraphicalPrimitive& object, const std::string& path) {
    Field(path + ".type", object.GetPrimitiveType());
    Field(path + ".start", object.m_StartPos); Field(path + ".end", object.m_EndPos);
    Field(path + ".radius_squared", object.m_DrawRadiusSquared); Field(path + ".color", object.m_Color);
    Field(path + ".player", object.m_Player); Field(path + ".blend_mode", object.m_BlendMode);
    Field(path + ".blend_channels", object.m_ColorChannelBlendAmounts); Field(path + ".depth", object.m_Depth);
    Field(path + ".image_owners", object.m_ImageOwners); Field(path + ".vertex_owners", object.m_VertexOwners);
    if (auto value = dynamic_cast<const LinePrimitive*>(&object)) Field(path + ".thickness", value->m_Thickness);
    if (auto value = dynamic_cast<const ArcPrimitive*>(&object)) {
        Field(path + ".start_angle", value->m_StartAngle); Field(path + ".end_angle", value->m_EndAngle);
        Field(path + ".radius", value->m_Radius); Field(path + ".thickness", value->m_Thickness);
    }
    if (auto value = dynamic_cast<const SplinePrimitive*>(&object)) {
        Field(path + ".guide_a", value->m_GuidePointAPos); Field(path + ".guide_b", value->m_GuidePointBPos);
    }
    if (auto value = dynamic_cast<const RoundedBoxPrimitive*>(&object)) Field(path + ".corner_radius", value->m_CornerRadius);
    if (auto value = dynamic_cast<const RoundedBoxFillPrimitive*>(&object)) Field(path + ".corner_radius", value->m_CornerRadius);
    if (auto value = dynamic_cast<const CirclePrimitive*>(&object)) Field(path + ".radius", value->m_Radius);
    if (auto value = dynamic_cast<const CircleFillPrimitive*>(&object)) Field(path + ".radius", value->m_Radius);
    if (auto value = dynamic_cast<const EllipsePrimitive*>(&object)) {
        Field(path + ".horizontal_radius", value->m_HorizRadius); Field(path + ".vertical_radius", value->m_VertRadius);
    }
    if (auto value = dynamic_cast<const EllipseFillPrimitive*>(&object)) {
        Field(path + ".horizontal_radius", value->m_HorizRadius); Field(path + ".vertical_radius", value->m_VertRadius);
    }
    if (auto value = dynamic_cast<const TrianglePrimitive*>(&object)) {
        Field(path + ".point_a", value->m_PointAPos); Field(path + ".point_b", value->m_PointBPos); Field(path + ".point_c", value->m_PointCPos);
    }
    if (auto value = dynamic_cast<const TriangleFillPrimitive*>(&object)) {
        Field(path + ".point_a", value->m_PointAPos); Field(path + ".point_b", value->m_PointBPos); Field(path + ".point_c", value->m_PointCPos);
    }
    if (auto value = dynamic_cast<const PolygonPrimitive*>(&object)) Field(path + ".vertices", value->m_Vertices);
    if (auto value = dynamic_cast<const PolygonFillPrimitive*>(&object)) Field(path + ".vertices", value->m_Vertices);
    if (auto value = dynamic_cast<const TextPrimitive*>(&object)) {
        Field(path + ".text", value->m_Text); Field(path + ".small", value->m_IsSmall); Field(path + ".alignment", value->m_Alignment);
        Field(path + ".rotation", value->m_RotAngle); Field(path + ".bitmap", value->m_TextBitmap); Field(path + ".target_alignment", value->m_TargetPosAlignment);
    }
    if (auto value = dynamic_cast<const BitmapPrimitive*>(&object)) {
        Field(path + ".bitmap", value->m_Bitmap); Field(path + ".rotation", value->m_RotAngle); Field(path + ".scale", value->m_Scale);
        std::vector<std::string> cacheReferences;
        if (value->m_Bitmap) for (size_t slot = 0; slot < ContentFile::s_LoadedBitmaps.size(); ++slot) {
            for (const auto& [name, bitmap]: ContentFile::s_LoadedBitmaps[slot]) {
                if (bitmap == value->m_Bitmap) cacheReferences.push_back(std::to_string(slot) + ":" + name);
            }
        }
        std::sort(cacheReferences.begin(), cacheReferences.end());
        Field(path + ".bitmap_cache_references", cacheReferences);
        Field(path + ".flip_h", value->m_HFlipped); Field(path + ".flip_v", value->m_VFlipped);
        Field(path + ".sprite_owner", value->m_SpriteOwner); Field(path + ".sprite_frame", value->m_SpriteFrame); Field(path + ".icon_bitmap", value->m_IconBitmap);
    }
}
static State Observe(const std::string& gapPath) {
    ContractAudit audit;
    std::map<long int, MovableObject*> objects;
    { std::lock_guard<std::mutex> guard(g_MovableMan.m_ObjectRegisteredMutex); objects = g_MovableMan.m_KnownObjects; }
    size_t invalid = 0;
    for (const auto& [uid, object]: objects) {
        if (!object || uid <= 0 || object->GetUniqueID() != uid) {
            ++invalid;
            audit.Field("registry.invalid[" + std::to_string(uid) + "]", object ? object->GetUniqueID() : -1);
            continue;
        }
    }
    std::cout << "[contract-audit] registry=" << objects.size() << " invalid=" << invalid << std::endl;
    Write(audit.values, gapPath + ".registry.txt");
    for (const auto& [uid, object]: objects) {
        if (object && uid > 0 && object->GetUniqueID() == uid) audit.VisitEntity(*object, "mo[" + std::to_string(uid) + "]");
    }
    if (const Activity* activity = g_ActivityMan.GetActivity()) audit.VisitEntity(*activity, "activity");
    if (const Activity* activity = g_ActivityMan.GetCheckpointStartActivity()) audit.VisitEntity(*activity, "start_activity");
    if (const Scene* scene = g_SceneMan.GetScene()) audit.Visit(*scene, "scene");
    // RemoveOrphans clears its fixed-size scratch pixels before every search; observe the
    // manager-owned allocation separately so a successful scene transaction cannot lose it.
    BITMAP* const orphanSearch = g_SceneMan.m_pOrphanSearchBitmap;
    audit.Field("scene_manager.orphan_search.present", orphanSearch != nullptr);
    if (orphanSearch) {
        audit.Field("scene_manager.orphan_search.depth", bitmap_color_depth(orphanSearch));
        audit.Field("scene_manager.orphan_search.width", orphanSearch->w);
        audit.Field("scene_manager.orphan_search.height", orphanSearch->h);
    }
    audit.Visit(g_UInputMan, "input");
    audit.Visit(g_FrameMan, "frame");
    audit.Visit(g_PostProcessMan, "postprocess");
    size_t primitiveCount = 0;
    while (const auto* primitive = g_PrimitiveMan.GetCheckpointPrimitive(primitiveCount)) {
        audit.Field("primitives.queue[" + std::to_string(primitiveCount++) + "]", primitive);
    }
    audit.Field("primitives.count", primitiveCount);
    g_GUISound.VisitCheckpointSounds([&audit](size_t index, const SoundContainer& sound) {
        audit.Visit(sound, "gui_sounds[" + std::to_string(index) + "]");
    });
    audit.Field("rng.sim", g_SimRNG.SerializeCheckpoint());
    audit.Field("rng.render", g_RenderRNG.SerializeCheckpoint());
    audit.Field("clock.sim_count", g_TimerMan.GetSimUpdateCount());
    audit.Field("clock.sim_time", g_TimerMan.GetSimTimeTicks());
    audit.Field("clock.real_time", g_TimerMan.GetRealTickCount());
    audit.Field("clock.ticks_per_second", g_TimerMan.GetTicksPerSecond());
    audit.Field("clock.sim_accumulator", g_TimerMan.GetSimAccumulator());
    audit.Field("clock.dt", g_TimerMan.GetDeltaTimeSecs());
    audit.Field("allocator.uid", MovableObject::GetUniqueIDCounter());
    audit.Field("allocator.lua_cursor", g_LuaMan.GetScriptStateCursor());
    audit.Field("checkpoint.restoring", g_MovableMan.IsRestoringSnapshot());
    audit.Field("checkpoint.pending_graph", g_ActivityMan.HasFullScriptGraphToRestore());
    audit.Field("world.actors", g_MovableMan.m_Actors);
    audit.Field("world.items", g_MovableMan.m_Items);
    audit.Field("world.particles", g_MovableMan.m_Particles);
    audit.Field("world.added_actors", g_MovableMan.m_AddedActors);
    audit.Field("world.added_items", g_MovableMan.m_AddedItems);
    audit.Field("world.added_particles", g_MovableMan.m_AddedParticles);
    audit.Field("world.alarms", g_MovableMan.m_AlarmEvents);
    audit.Field("world.added_alarms", g_MovableMan.m_AddedAlarmEvents);
    audit.Field("world.rosters", g_MovableMan.m_ActorRoster);
    audit.Field("world.sort_rosters", g_MovableMan.m_SortTeamRoster);
    audit.Field("world.quarantine", g_MovableMan.m_LockstepJoinQuarantine);
    audit.Field("world.moid_index", g_MovableMan.m_MOIDIndex);
    audit.Field("world.update_number", g_MovableMan.m_SimUpdateFrameNumber);
    audit.Field("world.roster", g_MovableMan.DescribeTeamRosters());
    audit.Field("scene.global_acc", g_SceneMan.GetScene()->GetGlobalAcc());
    TerrainLayerSnapshot terrain;
    if (terrain.Capture()) {
        const auto hash = [](const std::vector<uint8_t>& bytes) {
            uint64_t result = 1469598103934665603ULL;
            for (uint8_t value: bytes) result = (result ^ value) * 1099511628211ULL;
            return result;
        };
        audit.Field("terrain.material", hash(terrain.mat));
        audit.Field("terrain.foreground", hash(terrain.fg));
        audit.Field("terrain.background", hash(terrain.bg));
    } else audit.Field("terrain.capture_failed", true);
    std::ofstream gapsOut(gapPath, std::ios::binary);
    for (const auto& gap: audit.gaps) gapsOut << gap << "\n";
    return std::move(audit.values);
}
// Read-only staged-candidate transaction observer. These values are compared within
// one process before/after a rejected replacement; native identities remain exact.
static State ObservePendingCheckpoint(const std::string& gapPath) {
    ContractAudit audit;
    const auto& pending = g_ActivityMan.m_PendingCheckpoint;
    audit.Field("pending.scene", pending.scene.get());
    audit.Field("pending.activity", pending.activity.get());
    audit.Field("pending.start_activity", pending.startActivity.get());
    audit.Field("pending.has_start_activity", pending.hasStartActivity);
    audit.Field("pending.restart_preset", pending.restartPreset);
    audit.Field("pending.restart_objects", pending.restartObjects);
    audit.Field("pending.restart_units", pending.restartUnits);
    audit.Field("pending.sim_count", pending.simUpdateCount);
    audit.Field("pending.sim_time", pending.simTimeTicks);
    audit.Field("pending.uid_counter", pending.uniqueIDCounter);
    audit.Field("pending.lua_cursor", pending.luaStateCursor);
    audit.Field("pending.join_quarantine", pending.joinQuarantine);
    audit.Field("pending.runtime_globals", pending.runtimeGlobals);
    audit.Field("pending.world_structure", pending.worldStructure);
    audit.Field("pending.scene_runtime", pending.sceneRuntime);
    audit.Field("pending.script_graphs", pending.scriptGraphs);
    audit.Field("pending.sound_registrations", pending.soundRegistrations);
    audit.Field("manager.restores_snapshot", g_ActivityMan.m_RestartRestoresSnapshot);
    audit.Field("manager.needs_restart", g_ActivityMan.m_ActivityNeedsRestart);
    audit.Field("manager.needs_resume", g_ActivityMan.m_ActivityNeedsResume);
    audit.Field("manager.start_resumed", g_ActivityMan.m_StartActivityResumed);
    const auto identity = [&](const std::string& path, const void* pointer) {
        audit.Field("identity." + path, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer)));
    };
    identity("scene", pending.scene.get()); identity("activity", pending.activity.get());
    identity("start_activity", pending.startActivity.get()); identity("image_scope", pending.images.get());
    identity("live_activity", g_ActivityMan.m_Activity.get()); identity("configured_start", g_ActivityMan.m_StartActivity.get());
    std::set<const Entity*> walked;
    std::function<void(const Entity*, const std::string&)> native = [&](const Entity* entity, const std::string& path) {
        if (!entity) return;
        identity(path, entity);
        if (!walked.insert(entity).second) return;
        audit.VisitEntity(*entity, path);
        if (const auto* rotating = dynamic_cast<const MOSRotating*>(entity)) {
            size_t index = 0;
            for (const auto* child: rotating->GetAttachables()) native(child, path + ".attachable[" + std::to_string(index++) + "]");
            index = 0;
            for (const auto* child: rotating->GetWoundList()) native(child, path + ".wound[" + std::to_string(index++) + "]");
        }
        if (const auto* actor = dynamic_cast<const Actor*>(entity)) {
            size_t index = 0;
            for (const auto* child: *actor->GetInventory()) native(child, path + ".inventory[" + std::to_string(index++) + "]");
        }
        if (const auto* craft = dynamic_cast<const ACraft*>(entity)) {
            size_t index = 0;
            for (const auto* child: craft->GetCollectedInventory()) native(child, path + ".collected[" + std::to_string(index++) + "]");
        }
    };
    if (pending.scene) {
        native(pending.scene.get(), "candidate.scene");
        for (int set = 0; set < Scene::PLACEDSETSCOUNT; ++set) {
            size_t index = 0;
            for (const auto* entity: *pending.scene->GetPlacedObjects(set)) {
                native(entity, "candidate.placed[" + std::to_string(set) + "][" + std::to_string(index++) + "]");
            }
        }
        for (size_t index = 0; index < Players::MaxPlayerCount; ++index) {
            native(pending.scene->m_ResidentBrains[index], "candidate.brain[" + std::to_string(index) + "]");
        }
    }
    native(pending.activity.get(), "candidate.activity");
    native(pending.startActivity.get(), "candidate.start_activity");
    for (const auto& [owner, sounds]: pending.soundRegistrations) {
        size_t index = 0;
        for (const auto* sound: sounds) native(sound, "candidate.sound[" + std::to_string(owner) + "][" + std::to_string(index++) + "]");
    }
    const auto surface = [&](const std::string& path, const SDL_Surface* image) {
        identity(path, image);
        audit.Field(path + ".present", image != nullptr);
        if (!image) return;
        audit.Field(path + ".format", image->format); audit.Field(path + ".width", image->w);
        audit.Field(path + ".height", image->h); audit.Field(path + ".pitch", image->pitch);
        const auto* format = SDL_GetPixelFormatDetails(image->format);
        uint64_t hash = 1469598103934665603ULL;
        if (format && image->pixels) {
            const size_t rowBytes = static_cast<size_t>(image->w) * format->bytes_per_pixel;
            for (int y = 0; y < image->h; ++y) {
                const auto* row = static_cast<const uint8_t*>(image->pixels) + static_cast<size_t>(y) * image->pitch;
                for (size_t x = 0; x < rowBytes; ++x) hash = (hash ^ row[x]) * 1099511628211ULL;
            }
        } else audit.gaps.insert(path + " missing surface pixels/format");
        audit.Field(path + ".pixels", hash);
    };
    if (pending.images) {
        audit.Field("pending.images.active", pending.images->m_Active);
        audit.Field("pending.images.committed", pending.images->m_Committed);
        audit.Field("pending.images.count", pending.images->m_Entries.size());
        size_t index = 0;
        for (const auto& entry: pending.images->m_Entries) {
            const std::string path = "pending.images[" + std::to_string(index++) + "]";
            audit.Field(path + ".path", entry.path);
            audit.Field(path + ".previous_bitmap", entry.previousBitmap);
            identity(path + ".previous_bitmap", entry.previousBitmap);
            surface(path + ".previous_image", entry.previousImage);
        }
    }
    audit.Field("cache.bitmaps", ContentFile::s_LoadedBitmaps);
    for (size_t slot = 0; slot < ContentFile::s_LoadedBitmaps.size(); ++slot) {
        for (const auto& [path, bitmap]: ContentFile::s_LoadedBitmaps[slot]) identity("cache.bitmap[" + std::to_string(slot) + "][" + path + "]", bitmap);
    }
    for (const auto& [path, image]: ContentFile::s_MemoryPNGs) surface("cache.memory_png[" + path + "]", image);
    std::ofstream gapsOut(gapPath, std::ios::binary);
    for (const auto& gap: audit.gaps) gapsOut << gap << "\n";
    return std::move(audit.values);
}
// End staged-candidate transaction observer.

static State Identity() {
    State state;
    state["activity"] = std::to_string(reinterpret_cast<uintptr_t>(g_ActivityMan.GetActivity()));
    state["scene"] = std::to_string(reinterpret_cast<uintptr_t>(g_SceneMan.GetScene()));
    state["scene_manager.orphan_search"] = std::to_string(reinterpret_cast<uintptr_t>(g_SceneMan.m_pOrphanSearchBitmap));
    state["lua"] = g_MovableMan.DescribeLuaIdentity();
    std::lock_guard<std::mutex> guard(g_MovableMan.m_ObjectRegisteredMutex);
    std::map<const MovableObject*, long> addresses;
    for (const auto& [uid, object]: g_MovableMan.m_KnownObjects) {
        state["uid:" + std::to_string(uid)] = std::to_string(reinterpret_cast<uintptr_t>(object));
        if (object) addresses.emplace(object, uid);
    }
    const auto borrowed = [&](const std::string& name, const MovableObject* target, long capturedUID) {
        const auto found = addresses.find(target);
        state[name] = "address:" + std::to_string(reinterpret_cast<uintptr_t>(target)) +
            " registered_uid:" + std::to_string(found != addresses.end() ? found->second : 0) +
            " captured_uid:" + std::to_string(capturedUID);
    };
    for (const auto& [uid, object]: g_MovableMan.m_KnownObjects) {
        if (!object) continue;
        const std::string prefix = "borrowed[" + std::to_string(uid) + "]";
        borrowed(prefix + ".ignore", object->m_pMOToNotHit, object->m_MOToNotHitUID);
        if (const auto* actor = dynamic_cast<const Actor*>(object)) {
            borrowed(prefix + ".move_target", actor->m_pMOMoveTarget, actor->m_FaithfulMOMoveTargetUID);
            size_t index = 0;
            for (const auto& [position, target]: actor->m_Waypoints) {
                borrowed(prefix + ".waypoint[" + std::to_string(index) + "]", target,
                    index < actor->m_FaithfulWaypointUIDs.size() ? actor->m_FaithfulWaypointUIDs[index] : 0);
                ++index;
            }
        }
    }
    return state;
}
static void SetInputFixture(bool perturbed) {
    // Audit-only synthetic device state; this never sends input to SDL or the desktop.
    auto& input = g_UInputMan;
    UInputMan::Keyboard keyboard;
    keyboard.keyStates[SDL_SCANCODE_SPACE] = !perturbed;
    keyboard.changedKeyStates[SDL_SCANCODE_SPACE] = !perturbed;
    keyboard.pressedSinceSim[SDL_SCANCODE_SPACE] = !perturbed;
    keyboard.releasedSinceSim[SDL_SCANCODE_A] = !perturbed;
    input.m_KeyboardStates[0] = keyboard;
    UInputMan::Mouse mouse;
    mouse.position = perturbed ? Vector(-812.5F, 913.75F) : Vector(147.25F, 285.5F);
    mouse.relativeMotion = Vector(perturbed ? 31.25F : -13.5F, 7.25F);
    mouse.analogAim = Vector(perturbed ? -0.875F : 0.375F, -0.625F);
    mouse.wheelChange = perturbed ? -7.25F : 3.5F;
    mouse.state[0] = mouse.change[0] = mouse.pressedSinceSim[0] = !perturbed;
    mouse.releasedSinceSim[1] = !perturbed;
    input.m_MouseStates[0] = mouse;
    input.m_TextInput = perturbed ? "input fixture advanced" : "input fixture captured";
    input.m_MouseSensitivity = perturbed ? 0.25F : 1.375F;
    input.m_ControlScheme[0].SetKeyMapping(0, perturbed ? SDL_SCANCODE_W : SDL_SCANCODE_Q);
    input.m_ControlScheme[0].SetDigitalAimSpeed(perturbed ? 1.5F : 0.375F);
    Gamepad gamepad(0, 400000001U, 3, 4);
    gamepad.m_Axis[1] = perturbed ? -23456 : 12345;
    gamepad.m_DigitalAxis[1] = perturbed ? -1 : 1;
    gamepad.m_Buttons[2] = gamepad.m_ButtonsPressedSinceSim[2] = !perturbed;
    gamepad.m_ButtonsReleasedSinceSim[3] = !perturbed;
    input.s_PrevJoystickStates[0] = gamepad;
    input.s_ChangedJoystickStates[0] = gamepad;
}
};
inline std::string ContractAudit::EntityValue(const Entity* entity, const std::string& path) {
    auto [it, added] = visited.emplace(entity, path);
    if (added) VisitEntity(*entity, path);
    return "ref:" + it->second;
}
inline void ContractAudit::VisitEntity(const Entity& entity, const std::string& path) {
    values[path + ".class"] = entity.GetClassName();

if (const auto* typed = dynamic_cast<const TDExplosive*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const ACDropShip*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const ACRocket*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const AEJetpack*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const HDFirearm*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const ThrownDevice*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const ACrab*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const ACraft*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const ADoor*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const AEmitter*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const AHuman*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const Arm*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const HeldDevice*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const Leg*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const Magazine*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const Turret*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const Actor*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const Attachable*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const PEmitter*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const MOSParticle*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const MOSRotating*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const GAScripted*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const MOPixel*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const MOSprite*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const GameActivity*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const MovableObject*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const Activity*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const AtomGroup*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const Emission*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const GlobalScript*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const Icon*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const LimbPath*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const Material*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const PieMenu*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const PieSlice*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const Round*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const Scene*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const SceneObject*>(&entity)) { Visit(*typed, path); return; }
if (const auto* typed = dynamic_cast<const SoundContainer*>(&entity)) { Visit(*typed, path); return; }
Visit(entity, path);
}
} // namespace RTE
