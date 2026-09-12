#include "PreviewScriptSelfTest.h"

#include "AHuman.h"
#include "Actor.h"
#include "AudioMan.h"
#include "HDFirearm.h"
#include "LuaMan.h"
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
		if (!object || !object->GetLuaState()) {
			return false;
		}
		return object->GetLuaState()->AttachPreviewInvStride(object);
	}
} // namespace RTE
