#include "GUISound.h"

#include "SoundSet.h"
#include "CheckpointArchive.h"
#include "MusicMan.h"
#include "MovableObject.h"
#include "ContentFile.h"
#include <array>
#include <iostream>

using namespace RTE;

GUISound::GUISound() {
	Clear();
}

void GUISound::Clear() {
	m_SplashSound.Reset();
	m_EnterMenuSound.Reset();
	m_ExitMenuSound.Reset();
	m_FocusChangeSound.Reset();
	m_SelectionChangeSound.Reset();
	m_ItemChangeSound.Reset();
	m_ButtonPressSound.Reset();
	m_BackButtonPressSound.Reset();
	m_ConfirmSound.Reset();
	m_UserErrorSound.Reset();
	m_TestSound.Reset();
	m_PieMenuEnterSound.Reset();
	m_PieMenuExitSound.Reset();
	m_HoverChangeSound.Reset();
	m_HoverDisabledSound.Reset();
	m_SlicePickedSound.Reset();
	m_DisabledPickedSound.Reset();
	m_FundsChangedSound.Reset();
	m_ActorSwitchSound.Reset();
	m_BrainSwitchSound.Reset();
	m_CameraTravelSound.Reset();
	m_AreaPickedSound.Reset();
	m_ObjectPickedSound.Reset();
	m_PurchaseMadeSound.Reset();
	m_PlacementBlip.Reset();
	m_PlacementThud.Reset();
	m_PlacementGravel.Reset();
}

void GUISound::Initialize() {
	// Interface sounds should not be pitched to reinforce the appearance of time decoupling between simulation and UI.

	m_SplashSound.Create("Base.rte/Sounds/GUIs/MetaStart.flac", true, false, SoundContainer::BusRouting::UI);

	m_EnterMenuSound.Create("Base.rte/Sounds/GUIs/MenuEnter.flac", true, false, SoundContainer::BusRouting::UI);

	m_ExitMenuSound.Create("Base.rte/Sounds/GUIs/MenuExit1.flac", true, false, SoundContainer::BusRouting::UI);
	m_ExitMenuSound.GetTopLevelSoundSet().AddSound("Base.rte/Sounds/GUIs/MenuExit2.flac", true);

	m_FocusChangeSound.Create("Base.rte/Sounds/GUIs/FocusChange.flac", true, false, SoundContainer::BusRouting::UI);

	m_SelectionChangeSound.Create("Base.rte/Sounds/GUIs/SelectionChange.flac", true, false, SoundContainer::BusRouting::UI);

	m_ItemChangeSound.Create("Base.rte/Sounds/GUIs/ItemChange.flac", true, false, SoundContainer::BusRouting::UI);

	m_ButtonPressSound.Create("Base.rte/Sounds/GUIs/ButtonPress.flac", true, false, SoundContainer::BusRouting::UI);

	m_BackButtonPressSound.Create("Base.rte/Sounds/GUIs/BackButtonPress.flac", true, false, SoundContainer::BusRouting::UI);

	m_ConfirmSound.Create("Base.rte/Sounds/GUIs/MenuExit1.flac", true, false, SoundContainer::BusRouting::UI);

	m_UserErrorSound.Create("Base.rte/Sounds/GUIs/UserError.flac", true, false, SoundContainer::BusRouting::UI);

	m_TestSound.Create("Base.rte/Sounds/GUIs/Test.flac", true, false, SoundContainer::BusRouting::UI);

	m_PieMenuEnterSound.Create("Base.rte/Sounds/GUIs/PieMenuEnter.flac", true, false, SoundContainer::BusRouting::UI);

	m_PieMenuExitSound.Create("Base.rte/Sounds/GUIs/PieMenuExit.flac", true, false, SoundContainer::BusRouting::UI);

	//		m_HoverChangeSound.Create("Base.rte/Sounds/GUIs/SelectionChange.flac", true, false, SoundContainer::BusRouting::UI);
	m_HoverChangeSound = m_SelectionChangeSound;

	m_HoverDisabledSound.Create("Base.rte/Sounds/GUIs/PlacementBlip.flac", true, false, SoundContainer::BusRouting::UI);

	m_SlicePickedSound.Create("Base.rte/Sounds/GUIs/SlicePicked.flac", true, false, SoundContainer::BusRouting::UI);

	//		m_DisabledPickedSound.Create("Base.rte/Sounds/GUIs/PieMenuExit.flac", true, false, SoundContainer::BusRouting::UI);
	m_DisabledPickedSound = m_PieMenuExitSound;

	m_FundsChangedSound.Create("Base.rte/Sounds/GUIs/FundsChanged1.flac", true, false, SoundContainer::BusRouting::UI);
	m_FundsChangedSound.GetTopLevelSoundSet().AddSound("Base.rte/Sounds/GUIs/FundsChanged2.flac", true);
	m_FundsChangedSound.GetTopLevelSoundSet().AddSound("Base.rte/Sounds/GUIs/FundsChanged3.flac", true);
	m_FundsChangedSound.GetTopLevelSoundSet().AddSound("Base.rte/Sounds/GUIs/FundsChanged4.flac", true);
	m_FundsChangedSound.GetTopLevelSoundSet().AddSound("Base.rte/Sounds/GUIs/FundsChanged5.flac", true);
	m_FundsChangedSound.GetTopLevelSoundSet().AddSound("Base.rte/Sounds/GUIs/FundsChanged6.flac", true);
	m_FundsChangedSound.SetSoundOverlapMode(SoundContainer::SoundOverlapMode::RESTART);

	m_ActorSwitchSound.Create("Base.rte/Sounds/GUIs/ActorSwitch.flac", true, false, SoundContainer::BusRouting::UI);

	m_BrainSwitchSound.Create("Base.rte/Sounds/GUIs/BrainSwitch.flac", true, false, SoundContainer::BusRouting::UI);

	m_CameraTravelSound.Create("Base.rte/Sounds/GUIs/CameraTravel1.flac", true, false, SoundContainer::BusRouting::UI);
	m_CameraTravelSound.GetTopLevelSoundSet().AddSound("Base.rte/Sounds/GUIs/CameraTravel2.flac", true);
	m_CameraTravelSound.GetTopLevelSoundSet().AddSound("Base.rte/Sounds/GUIs/CameraTravel3.flac", true);

	//		m_AreaPickedSound.Create("Base.rte/Sounds/GUIs/MenuEnter.flac", true, false, SoundContainer::BusRouting::UI);
	m_AreaPickedSound = m_ConfirmSound;

	//		m_ObjectPickedSound.Create("Base.rte/Sounds/GUIs/MenuEnter.flac", true, false, SoundContainer::BusRouting::UI);
	m_ObjectPickedSound = m_ConfirmSound;

	//		m_PurchaseMadeSound.Create("Base.rte/Sounds/GUIs/MenuEnter.flac", true, false, SoundContainer::BusRouting::UI);
	m_PurchaseMadeSound = m_ConfirmSound;

	m_PlacementBlip.Create("Base.rte/Sounds/GUIs/PlacementBlip.flac", true, false, SoundContainer::BusRouting::UI);

	m_PlacementThud.Create("Base.rte/Sounds/GUIs/PlacementThud1.flac", true, false, SoundContainer::BusRouting::UI);
	m_PlacementThud.GetTopLevelSoundSet().AddSound("Base.rte/Sounds/GUIs/PlacementThud2.flac", true);

	m_PlacementGravel.Create("Base.rte/Sounds/GUIs/PlacementGravel1.flac", true, false, SoundContainer::BusRouting::UI);
	m_PlacementGravel.GetTopLevelSoundSet().AddSound("Base.rte/Sounds/GUIs/PlacementGravel2.flac", true);
	m_PlacementGravel.GetTopLevelSoundSet().AddSound("Base.rte/Sounds/GUIs/PlacementGravel3.flac", true);
	m_PlacementGravel.GetTopLevelSoundSet().AddSound("Base.rte/Sounds/GUIs/PlacementGravel4.flac", true);
}

namespace RTE {
struct GUISoundCheckpoint {
    template<class T> static std::string SaveRecord(const T& value, const char* version) {
        CheckpointWriter archive(version); const_cast<T&>(value).Fields(archive); return archive.Text();
    }
    template<class T> static bool LoadRecord(T& value, std::string_view text, const char* version, bool validateOnly) {
        try { T candidate; CheckpointReader archive(text, version); candidate.Fields(archive); archive.Finish(); if (!validateOnly) value = std::move(candidate); return true; }
        catch (const std::exception&) { return false; }
    }
    struct Sample {
        std::string content, backend;
        Vector offset;
        float minimum = 0, attenuation = -1;
        template<class A> void Fields(A& a) { a(content, backend, offset, minimum, attenuation); }
        std::string SaveCheckpoint() const { return SaveRecord(*this, "GUISoundSample1"); }
        bool LoadCheckpoint(std::string_view text, bool validateOnly = false) { return LoadRecord(*this, text, "GUISoundSample1", validateOnly); }
    };
    struct Set {
        int cycle = 0;
        std::pair<bool, int> selection{false, -1};
        std::vector<Sample> samples;
        std::vector<Set> subsets;
        template<class A> void Fields(A& a) { a(cycle, selection, samples, subsets); }
        std::string SaveCheckpoint() const { return SaveRecord(*this, "GUISoundSet1"); }
        bool LoadCheckpoint(std::string_view text, bool validateOnly = false) {
            static thread_local unsigned depth = 0;
            if (depth >= 128) return false;
            struct Depth { unsigned& value; explicit Depth(unsigned& value) : value(value) { ++value; } ~Depth() { --value; } } scope(depth);
            return LoadRecord(*this, text, "GUISoundSet1", validateOnly);
        }
    };
    struct Sound {
        std::string native;
        Set set;
        template<class A> void Fields(A& a) { a(native, set); }
        std::string SaveCheckpoint() const { return SaveRecord(*this, "GUISoundContainer1"); }
        bool LoadCheckpoint(std::string_view text, bool validateOnly = false) { return LoadRecord(*this, text, "GUISoundContainer1", validateOnly); }
    };
    struct Record {
        std::array<Sound, 27> sounds;
        template<class A> void Fields(A& a) { a(sounds); }
        std::string SaveCheckpoint() const { return SaveRecord(*this, "GUISound1"); }
        bool LoadCheckpoint(std::string_view text, bool validateOnly = false) { return LoadRecord(*this, text, "GUISound1", validateOnly); }
    };
    struct State {
        std::array<std::unique_ptr<SoundContainer>, 27> sounds;
        std::vector<std::pair<SoundData*, std::string>> bindings;
    };
    static std::array<SoundContainer*, 27> Members(GUISound& source) {
        return {&source.m_SplashSound, &source.m_EnterMenuSound, &source.m_ExitMenuSound,
            &source.m_FocusChangeSound, &source.m_SelectionChangeSound, &source.m_ItemChangeSound,
            &source.m_ButtonPressSound, &source.m_BackButtonPressSound, &source.m_ConfirmSound,
            &source.m_UserErrorSound, &source.m_TestSound, &source.m_PieMenuEnterSound,
            &source.m_PieMenuExitSound, &source.m_HoverChangeSound, &source.m_HoverDisabledSound,
            &source.m_SlicePickedSound, &source.m_DisabledPickedSound, &source.m_FundsChangedSound,
            &source.m_ActorSwitchSound, &source.m_BrainSwitchSound, &source.m_CameraTravelSound,
            &source.m_AreaPickedSound, &source.m_ObjectPickedSound, &source.m_PurchaseMadeSound,
            &source.m_PlacementBlip, &source.m_PlacementThud, &source.m_PlacementGravel};
    }
    static Set CaptureSet(const SoundSet& source) {
        Set value; value.cycle = source.m_SoundSelectionCycleMode; value.selection = source.m_CurrentSelection;
        for (const auto& sample: source.m_SoundData) {
            std::string path;
            if (sample.SoundObject) {
                for (const auto& [candidate, sound]: ContentFile::s_LoadedSamples) if (sound == sample.SoundObject && (path.empty() || candidate < path)) path = candidate;
                if (path.empty()) throw std::runtime_error("GUI sample has no cached asset identity");
            }
            value.samples.push_back({sample.SoundFile.SaveCheckpoint(), path, sample.Offset, sample.MinimumAudibleDistance, sample.AttenuationStartDistance});
        }
        for (const auto* child: source.m_SubSoundSets) { if (!child) throw std::runtime_error("null GUI sound subset"); value.subsets.push_back(CaptureSet(*child)); }
        return value;
    }
    static Record Capture(const GUISound& source) {
        Record result;
        const auto members = Members(const_cast<GUISound&>(source));
        for (size_t i = 0; i < members.size(); ++i) result.sounds[i] = {members[i]->SaveCheckpoint(), CaptureSet(*members[i]->m_TopLevelSoundSet)};
        return result;
    }
    static bool ValidateSet(const Set& value, int depth = 0) {
        if (depth > 128 || value.cycle < 0 || value.cycle > SoundSet::ALL || value.selection.second < -1) return false;
        if (value.selection.second >= 0 && static_cast<size_t>(value.selection.second) >= (value.selection.first ? value.subsets.size() : value.samples.size())) return false;
        ContentFile probe;
        for (const auto& sample: value.samples) if (!probe.LoadCheckpoint(sample.content, true)) return false;
        for (const auto& child: value.subsets) if (!ValidateSet(child, depth + 1)) return false;
        return true;
    }
    static bool Validate(const Record& value) {
        MovableObject::FaithfulCloneScope privateObjects(false);
        SoundContainer probe;
        std::set<uint64_t> identities;
        for (const auto& sound: value.sounds) {
            if (!probe.LoadCheckpoint(sound.native, true) || !ValidateSet(sound.set)) return false;
            CheckpointReader reader(sound.native, SoundContainer::CheckpointVersion(sound.native)); std::string entity; uint64_t identity; reader.Value(entity); reader.Value(identity);
            if (!identities.insert(identity).second) return false;
        }
        return true;
    }
    static void BuildSet(const Set& record, SoundSet& value, State& state) {
        value.m_SoundSelectionCycleMode = static_cast<SoundSet::SoundSelectionCycleMode>(record.cycle); value.m_CurrentSelection = record.selection;
        value.m_SoundData.resize(record.samples.size());
        for (size_t i = 0; i < record.samples.size(); ++i) {
            const auto& sample = record.samples[i]; auto& data = value.m_SoundData[i];
            if (!data.SoundFile.LoadCheckpoint(sample.content)) throw std::runtime_error("invalid GUI sample metadata");
            data.SoundObject = nullptr; data.Offset = sample.offset; data.MinimumAudibleDistance = sample.minimum; data.AttenuationStartDistance = sample.attenuation;
            if (!sample.backend.empty()) state.bindings.emplace_back(&data, sample.backend);
        }
        value.m_SubSoundSets.reserve(record.subsets.size());
        for (const auto& recordChild: record.subsets) { auto child = std::make_unique<SoundSet>(); BuildSet(recordChild, *child, state); value.m_SubSoundSets.push_back(child.release()); }
    }
    static std::unique_ptr<State> Build(const Record& record) {
        MovableObject::FaithfulCloneScope privateObjects(false);
        auto state = std::make_unique<State>();
        for (size_t i = 0; i < record.sounds.size(); ++i) {
            auto sound = std::make_unique<SoundContainer>();
            if (!sound->LoadCheckpoint(record.sounds[i].native)) throw std::runtime_error("invalid GUI sound metadata");
            BuildSet(record.sounds[i].set, *sound->m_TopLevelSoundSet, *state);
            state->sounds[i] = std::move(sound);
        }
        return state;
    }
    static void Swap(State& state, GUISound& manager) noexcept {
        const auto members = Members(manager);
        for (size_t i = 0; i < members.size(); ++i) members[i]->SwapCheckpoint(*state.sounds[i]);
    }
    static bool Apply(GUISound& manager, std::string_view text, std::string_view music, std::string_view audio) {
        auto& audioManager = g_AudioMan;
        std::unique_ptr<State> candidate;
        CheckpointSoundRegistry registry;
        std::unordered_map<const SoundContainer*, uint64_t> live;
        const uint64_t originalCursor = audioManager.GetCheckpointSoundContainerCursor();
        bool swapped = false;
        try {
            Record record;
            if (!record.LoadCheckpoint(text) || !Validate(record) || !g_MusicMan.LoadCheckpoint(music, true) || !audioManager.LoadCheckpoint(audio, true)) return false;
            CheckpointReader audioHeader(audio, "AudioRuntime1"); bool enabled; int nextVoice; uint64_t nextSound;
            audioHeader.Value(enabled); audioHeader.Value(nextVoice); audioHeader.Value(nextSound);
            for (const auto& sound: record.sounds) {
                CheckpointReader soundHeader(sound.native, SoundContainer::CheckpointVersion(sound.native)); std::string entity; uint64_t identity;
                soundHeader.Value(entity); soundHeader.Value(identity);
                if (identity > nextSound) return false;
            }
            if (manager.SaveCheckpoint() == text) return g_MusicMan.LoadCheckpointWithAudio(music, audio);
            candidate = Build(record);
            registry = audioManager.m_CheckpointSoundContainers;
            live = audioManager.m_LiveCheckpointSoundContainers;
            const auto members = Members(manager);
            const std::set<const SoundContainer*> owners(members.begin(), members.end());
            for (auto entry = registry.begin(); entry != registry.end();) {
                std::erase_if(entry->second, [&owners](const SoundContainer* owner) { return owners.contains(owner); });
                if (entry->second.empty()) entry = registry.erase(entry); else ++entry;
            }
            for (size_t i = 0; i < members.size(); ++i) {
                const uint64_t identity = candidate->sounds[i]->GetCheckpointIdentity();
                registry[identity].push_back(members[i]); live[members[i]] = identity;
            }
            Swap(*candidate, manager);
            audioManager.m_CheckpointSoundContainers.swap(registry);
            audioManager.m_LiveCheckpointSoundContainers.swap(live);
            swapped = true;
            if (!g_MusicMan.LoadCheckpointWithAudio(music, audio, &candidate->bindings)) throw std::runtime_error("GUI audio restoration failed");
            return true;
        } catch (const std::exception& error) {
            if (swapped) {
                Swap(*candidate, manager);
                audioManager.m_CheckpointSoundContainers.swap(registry);
                audioManager.m_LiveCheckpointSoundContainers.swap(live);
                audioManager.SetCheckpointSoundContainerCursor(originalCursor);
            }
            std::cout << "[gui-sound-checkpoint] " << error.what() << std::endl;
            return false;
        }
    }
    static bool SelfTest(GUISound& manager) {
        const std::string original = manager.SaveCheckpoint(), originalMusic = g_MusicMan.SaveCheckpoint(), originalAudio = g_AudioMan.SaveCheckpoint();
        const RandomGenerator originalRNG = g_RenderRNG;
        bool ok = true;
        try {
            auto* owner = manager.FundsChangedSound();
            auto& set = owner->GetTopLevelSoundSet();
            set.SetSoundSelectionCycleMode(SoundSet::FORWARDS);
            set.m_CurrentSelection = {false, 2};
            set.m_SoundData[0].Offset = Vector(12.25F, -34.5F);
            set.AddSoundSet(manager.BrainSwitchSound()->GetTopLevelSoundSet());
            owner->SetPaused(true); owner->SetImmobile(true); owner->SetLoopSetting(-1); owner->SetVolume(0.125F); owner->SetPitch(1.25F);
            if (!owner->Play()) throw std::runtime_error("GUI checkpoint fixture did not play");
            const std::string target = manager.SaveCheckpoint(), music = g_MusicMan.SaveCheckpoint(), audio = g_AudioMan.SaveCheckpoint();
            const std::string playback = g_AudioMan.GetSoundContainerPlaybackCheckpoint(owner);
            auto* originalSet = &owner->GetTopLevelSoundSet();
            Record invalid; if (!invalid.LoadCheckpoint(target)) throw std::runtime_error("could not parse GUI sound fixture");
            invalid.sounds[17].set.samples[0].backend = "__missing_gui_checkpoint_sample__";
            if (manager.LoadCheckpointWithAudio(invalid.SaveCheckpoint(), music, audio) || manager.SaveCheckpoint() != target || &owner->GetTopLevelSoundSet() != originalSet || g_AudioMan.GetSoundContainerPlaybackCheckpoint(owner) != playback) throw std::runtime_error("failed GUI restore changed original owners, values or playback");
            if (manager.LoadCheckpointWithAudio(target + "trailing", music, audio)) throw std::runtime_error("trailing GUI checkpoint accepted");
            const RandomGenerator sequenceRNG = g_RenderRNG;
            const auto sequence = [&manager]() {
                std::vector<std::vector<size_t>> result;
                for (int i = 0; i < 10; ++i) {
                    result.push_back(manager.FundsChangedSound()->GetSelectedSoundHashes());
                    if (!manager.FundsChangedSound()->GetTopLevelSoundSet().SelectNextSounds()) throw std::runtime_error("GUI sound selection did not continue");
                }
                return result;
            };
            const auto expected = sequence();
            owner->SetVolume(0.8F); owner->SetPitch(0.75F); owner->GetTopLevelSoundSet().m_SoundData[0].Offset = Vector(-9, 8);
            for (auto* child: owner->GetTopLevelSoundSet().m_SubSoundSets) delete child;
            owner->GetTopLevelSoundSet().m_SubSoundSets.clear();
            if (!manager.LoadCheckpointWithAudio(target, music, audio) || manager.SaveCheckpoint() != target || manager.FundsChangedSound() != owner || g_AudioMan.GetSoundContainerPlaybackCheckpoint(owner) != playback) throw std::runtime_error("GUI native state, selection topology, owner identity or playback was not restored");
            const auto& restored = owner->GetTopLevelSoundSet();
            if (owner->GetVolume() != 0.125F || owner->GetPitch() != 1.25F || restored.m_SoundData[0].Offset != Vector(12.25F, -34.5F) || restored.m_SubSoundSets.size() != 1) throw std::runtime_error("GUI native values were not restored");
            g_RenderRNG = sequenceRNG;
            if (sequence() != expected) throw std::runtime_error("GUI sound selection continuation changed");
            std::cout << "[gui-sound-checkpoint-selftest] 27 native owners, failed-apply identity/playback, nested values and selection continuation PASS" << std::endl;
        } catch (const std::exception& error) { std::cout << "[gui-sound-checkpoint-selftest] " << error.what() << std::endl; ok = false; }
        ok = manager.LoadCheckpointWithAudio(original, originalMusic, originalAudio) && ok;
        g_RenderRNG = originalRNG;
        return ok;
    }
};
}

std::string GUISound::SaveCheckpoint() const { return GUISoundCheckpoint::Capture(*this).SaveCheckpoint(); }

bool GUISound::LoadCheckpoint(std::string_view text, bool validateOnly) {
    if (!validateOnly) return LoadCheckpointWithAudio(text, g_MusicMan.SaveCheckpoint(), g_AudioMan.SaveCheckpoint());
    try { GUISoundCheckpoint::Record record; return record.LoadCheckpoint(text) && GUISoundCheckpoint::Validate(record); }
    catch (const std::exception&) { return false; }
}

bool GUISound::LoadCheckpointWithAudio(std::string_view text, std::string_view music, std::string_view audio) { return GUISoundCheckpoint::Apply(*this, text, music, audio); }

bool GUISound::RunCheckpointSelfTest() { return GUISoundCheckpoint::SelfTest(*this); }

void GUISound::VisitCheckpointSounds(const std::function<void(size_t, const SoundContainer&)>& visitor) const {
    const auto sounds = GUISoundCheckpoint::Members(const_cast<GUISound&>(*this));
    for (size_t i = 0; i < sounds.size(); ++i) visitor(i, *sounds[i]);
}
