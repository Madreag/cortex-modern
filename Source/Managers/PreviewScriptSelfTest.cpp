#include "PreviewScriptSelfTest.h"

#include "AEmitter.h"
#include "AHuman.h"
#include "Actor.h"
#include "Arm.h"
#include "Attachable.h"
#include "AudioMan.h"
#include "HDFirearm.h"
#include "HeldDevice.h"
#include "LuaMan.h"
#include "Magazine.h"
#include "MovableMan.h"
#include "MovableObject.h"
#include "OwnedMovableObjects.h"
#include "PreviewEventLedger.h"
#include "RTETools.h"
#include "SoundContainer.h"
#include "SoundSimulation.h"
#include "TimerMan.h"

#include <iostream>
#include <unordered_set>

namespace RTE {

	bool PreviewScriptSelfTest::s_SubtreeProbe = false;
	bool PreviewScriptSelfTest::s_SharedSlot = false;
	bool PreviewScriptSelfTest::s_StrideCounter = false;
	bool PreviewScriptSelfTest::s_PreviewStrideSeen = false;
	bool PreviewScriptSelfTest::s_Probed = false;
	bool PreviewScriptSelfTest::s_Played = false;
	bool PreviewScriptSelfTest::s_ChildWasPreviewed = false;
	uint64_t PreviewScriptSelfTest::s_RootUID = 0;
	uint64_t PreviewScriptSelfTest::s_ChildUID = 0;
	uint64_t PreviewScriptSelfTest::s_CommittedTick = 0;
	PreviewScriptSelfTest::OverlayLinkProbe PreviewScriptSelfTest::s_OverlayLinkProbe;

	void PreviewScriptSelfTest::SetSubtreeProbe(bool enabled) {
		s_SubtreeProbe = enabled;
	}

	bool PreviewScriptSelfTest::SubtreeProbeEnabled() {
		return s_SubtreeProbe;
	}

	void PreviewScriptSelfTest::SetSharedSlot(bool shared) {
		s_SharedSlot = shared;
	}

	bool PreviewScriptSelfTest::SharedSlot() {
		return s_SharedSlot;
	}

	void PreviewScriptSelfTest::ProbeArmedEmitters(const Actor* original) {
		if (!s_SubtreeProbe || s_Played || !original || !g_AudioMan.IsAudioEnabled()) {
			return;
		}
		s_RootUID = static_cast<uint64_t>(original->GetUniqueID());
		s_ChildUID = 0;
		SoundContainer* sound = nullptr;
		if (const AHuman* human = dynamic_cast<const AHuman*>(original)) {
			if (const HDFirearm* gun = dynamic_cast<const HDFirearm*>(human->GetEquippedItem())) {
				s_ChildUID = static_cast<uint64_t>(gun->GetUniqueID());
				sound = gun->GetFireSound();
			}
			if (!sound) {
				sound = human->GetStrideSound();
			}
		}
		if (!s_ChildUID) {
			std::unordered_set<const Entity*> visited;
			std::unordered_set<const MovableObject*> objects;
			CollectOwnedMovableObjects(original, visited, objects);
			for (const MovableObject* mo: objects) {
				if (mo && mo->GetUniqueID() > 0 && static_cast<uint64_t>(mo->GetUniqueID()) != s_RootUID) {
					s_ChildUID = static_cast<uint64_t>(mo->GetUniqueID());
					break;
				}
			}
		}
		s_ChildWasPreviewed = s_ChildUID && PreviewEventLedger::IsPreviewedEmitter(s_ChildUID);
		s_CommittedTick = PreviewEventLedger::CommittedTick();
		s_Probed = true;
		if (!sound || !s_ChildUID || !sound->HasAnySounds()) {
			return;
		}
		SoundSimulationScope scope(s_ChildUID, Hash("subtree-emitter-probe"));
		s_Played = sound->Play();
	}

	bool PreviewScriptSelfTest::CheckSubtreeEmitter(long long pressTick) {
		bool passed = true;
		const auto check = [&passed](const char* name, bool ok, const std::string& detail) {
			std::cout << "[preview-subtree-selftest] " << (ok ? "PASS " : "FAIL ") << name << ": " << detail << std::endl;
			passed = passed && ok;
		};
		if (!s_SubtreeProbe) {
			return true;
		}
		check("a_non_root_part_exists", s_ChildUID != 0 && s_ChildUID != s_RootUID,
		      "root=" + std::to_string(s_RootUID) + " child=" + std::to_string(s_ChildUID) + " probed=" + std::to_string(s_Probed ? 1 : 0));
		check("the_non_root_part_is_a_previewed_emitter", s_ChildWasPreviewed,
		      "child uid=" + std::to_string(s_ChildUID) + " previewed=" + std::to_string(s_ChildWasPreviewed ? 1 : 0));
		const PreviewEventLedger::EventStart* hit = nullptr;
		for (const PreviewEventLedger::EventStart& start: PreviewEventLedger::GetEventStarts()) {
			if (start.kind == PreviewEventLedger::Sound && start.emitterUID == s_ChildUID && start.predicted) {
				hit = &start;
				break;
			}
		}
		if (!hit) {
			for (const PreviewEventLedger::EventStart& start: PreviewEventLedger::GetEventStarts()) {
				if (start.kind == PreviewEventLedger::Sound && start.emitterUID == s_ChildUID) {
					hit = &start;
					break;
				}
			}
		}
		const uint64_t press = pressTick > 0 ? static_cast<uint64_t>(pressTick) : s_CommittedTick;
		check("a_native_sound_starts_on_the_preview_tick", hit && hit->predicted && hit->committedTick <= press + 1,
		      hit ? "child uid=" + std::to_string(s_ChildUID) + " committed=" + std::to_string(hit->committedTick) + " event=" + std::to_string(hit->eventTick) +
		                " predicted=" + std::to_string(hit->predicted ? 1 : 0) + " played=" + std::to_string(s_Played ? 1 : 0)
		          : "no physical voice keyed by child uid " + std::to_string(s_ChildUID) + " played=" + std::to_string(s_Played ? 1 : 0));
		std::cout << "[preview-subtree-selftest] " << (passed ? "PASS" : "FAIL") << std::endl;
		return passed;
	}

	bool PreviewScriptSelfTest::CheckNestedHookScope() {
		LuaMan::SetRunningPreviewHook(false);
		bool nestedKept = false;
		{
			LuaMan::PreviewHookScope outer(true);
			{
				LuaMan::PreviewHookScope inner(true);
			}
			nestedKept = LuaMan::IsRunningPreviewHook();
		}
		const bool outerCleared = !LuaMan::IsRunningPreviewHook();
		const bool ok = nestedKept && outerCleared;
		std::cout << "[preview-hook-scope] " << (ok ? "PASS" : "FAIL") << " nested_inner_does_not_clear_outer kept=" << (nestedKept ? 1 : 0) << " cleared=" << (outerCleared ? 1 : 0) << std::endl;
		return ok;
	}

	void PreviewScriptSelfTest::SetStrideCounter(bool enabled) {
		s_StrideCounter = enabled;
	}

	bool PreviewScriptSelfTest::StrideCounterRequested() {
		return s_StrideCounter;
	}

	void PreviewScriptSelfTest::NotePreviewStride(bool initialized) {
		if (initialized) {
			s_PreviewStrideSeen = true;
		}
	}

	bool PreviewScriptSelfTest::InstallStrideCounter(MovableObject* object) {
		RunOverlayLinkProbe(object);
		if (!object || !object->GetLuaState()) {
			return false;
		}
		return object->GetLuaState()->AttachPreviewInvStride(object);
	}

	void PreviewScriptSelfTest::ArmOverlayLinkProbe(char mode, const Actor* residentActor, const MovableObject* residentItem) {
		s_OverlayLinkProbe = {};
		s_OverlayLinkProbe.mode = mode;
		s_OverlayLinkProbe.residentActor = residentActor;
		s_OverlayLinkProbe.residentItem = residentItem;
	}

	void PreviewScriptSelfTest::DisarmOverlayLinkProbe() {
		s_OverlayLinkProbe = {};
	}

	// Runs at the stride-counter seam: speculation is on, the clones are bound, their faithful links not yet resolved.
	void PreviewScriptSelfTest::RunOverlayLinkProbe(MovableObject* object) {
		OverlayLinkProbe& probe = s_OverlayLinkProbe;
		if (probe.mode == 0 || probe.ran || !object || !g_MovableMan.IsSpeculative() || !LuaMan::IsPreviewClone(object)) {
			return;
		}
		probe.ran = true;
		AHuman* clone = dynamic_cast<AHuman*>(object);
		if (!clone) {
			probe.failure = "the preview clone is not an AHuman";
			return;
		}
		probe.clone = clone;
		probe.cloneUID = clone->GetUniqueID();
		// A link set here has to outlive the faithful link resolution RunPreview does after this seam.
		const auto link = [](MovableObject* holder, MovableObject* target) {
			holder->m_FaithfulMOToNotHitUID = 0;
			holder->SetWhichMOToNotHit(target, -1.0F);
		};
		const auto shadowOf = [](const MovableObject* resident) -> MovableObject* {
			MovableObject* view = resident ? g_MovableMan.FindObjectByUniqueID(resident->GetUniqueID()) : nullptr;
			return view != resident ? view : nullptr;
		};
		// The root's direct parts are siblings, so a link set on one never propagates onto another.
		const std::vector<MovableObject*> parts(clone->GetAttachableList().begin(), clone->GetAttachableList().end());

		if (probe.mode == 'r') {
			if (clone->GetItemInReachUniqueID() != 0) {
				probe.failure = "the clone carries a faithful item in reach that link resolution would put back";
				return;
			}
			HeldDevice* shadow = dynamic_cast<HeldDevice*>(shadowOf(probe.residentItem));
			if (!shadow || !g_MovableMan.IsDevice(shadow)) {
				probe.failure = "no resident item with an in-world shadow";
				return;
			}
			// The shadow is the overlay's own copy; moving it into reach leaves the resident where it lies.
			const Arm* arm = clone->GetFGArm() ? clone->GetFGArm() : clone->GetBGArm();
			shadow->SetPos(arm ? arm->GetJointPos() : clone->GetPos());
			clone->SetItemInReach(shadow);
			return;
		}

		Arm* arm = nullptr;
		for (Arm* candidate: {clone->GetFGArm(), clone->GetBGArm()}) {
			if (candidate && candidate->GetHeldDevice()) {
				arm = candidate;
				break;
			}
		}
		if (!arm) {
			probe.failure = "the preview clone holds no device";
			return;
		}
		if (probe.mode == 'b' && clone->GetItemInReachUniqueID() != 0) {
			probe.failure = "the clone carries a faithful item in reach that link resolution would put back";
			return;
		}
		HeldDevice* held = arm->GetHeldDevice();
		probe.spawn = held;
		probe.spawnUID = held->GetUniqueID();
		probe.spawnPreset = held->GetPresetName();
		{
			// A named drop under a previewed emitter retires as a ghost that outlives EndSpeculation.
			const uint64_t emitterUID = probe.mode == 'c' ? static_cast<uint64_t>(held->GetUniqueID()) : static_cast<uint64_t>(clone->GetUniqueID());
			SoundSimulationScope emitter(emitterUID, Hash("overlay-link-probe"));
			arm->RemoveAttachable(held, true, false);
		}
		link(clone, held);

		if (probe.mode == 'b') {
			MovableObject* shadowActor = shadowOf(probe.residentActor);
			if (!shadowActor || parts.empty()) {
				probe.failure = shadowActor ? "the clone has no parts" : "no other resident actor with an in-world shadow";
				return;
			}
			clone->SetItemInReach(held);
			link(parts[0], shadowActor);
			probe.shadowLinkPart = parts[0];
			return;
		}

		if (probe.mode == 'c') {
			MovableObject* spawnPart = nullptr;
			if (const HDFirearm* firearm = dynamic_cast<const HDFirearm*>(held); firearm && firearm->GetMagazine()) {
				spawnPart = firearm->GetMagazine();
			} else if (!held->GetAttachableList().empty()) {
				spawnPart = held->GetAttachableList().front();
			}
			const MOSRotating* shadowActor = dynamic_cast<const MOSRotating*>(shadowOf(probe.residentActor));
			MovableObject* shadowPart = shadowActor && !shadowActor->GetAttachableList().empty() ? shadowActor->GetAttachableList().front() : nullptr;
			probe.residentPart = shadowPart ? const_cast<Actor*>(probe.residentActor)->FindPartByUniqueID(shadowPart->GetUniqueID()) : nullptr;
			// Wounds as a hit makes them: copies of a part's break wound, which carry no script and so take no Lua state.
			const AEmitter* woundPreset = nullptr;
			for (const MovableObject* part: parts) {
				const Attachable* attachable = dynamic_cast<const Attachable*>(part);
				if (attachable && attachable->GetBreakWound() && static_cast<const MovableObject*>(attachable->GetBreakWound())->m_AllLoadedScripts.empty()) {
					woundPreset = attachable->GetBreakWound();
					break;
				}
			}
			if (parts.size() < 3 || !spawnPart || !probe.residentPart || !woundPreset) {
				probe.failure = parts.size() < 3 ? "the clone has fewer than three parts" : !spawnPart ? "the dropped device has no part" : !probe.residentPart ? "no resident part behind a shadow part" : "no unscripted wound preset on the clone";
				return;
			}
			AEmitter* spawnWound = dynamic_cast<AEmitter*>(woundPreset->Clone());
			AEmitter* cloneWound = dynamic_cast<AEmitter*>(woundPreset->Clone());
			if (!spawnWound || !cloneWound) {
				delete spawnWound;
				delete cloneWound;
				probe.failure = "the wound preset did not clone";
				return;
			}
			held->AddWound(spawnWound, Vector(), false);
			clone->AddWound(cloneWound, Vector(), false);
			link(parts[0], spawnPart);
			link(parts[1], spawnWound);
			link(parts[2], shadowPart);
			link(cloneWound, held);
			probe.spawnPart = spawnPart;
			probe.spawnWound = spawnWound;
			probe.cloneWound = cloneWound;
			probe.spawnPartLinkPart = parts[0];
			probe.spawnWoundLinkPart = parts[1];
			probe.shadowPartLinkPart = parts[2];
			return;
		}
		probe.failure = std::string("unknown probe mode '") + probe.mode + "'";
	}
} // namespace RTE
