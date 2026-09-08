#include "MusicMan.h"
#include "CheckpointArchive.h"
#include "SoundSet.h"
#include "ContentFile.h"
#include <climits>
#include <iostream>
#include <algorithm>

#include "AudioMan.h"
#include "ConsoleMan.h"
#include "PresetMan.h"

using namespace RTE;

MusicMan::MusicMan() {
	Clear();
}

MusicMan::~MusicMan() {
	Destroy();
}

void MusicMan::Clear() {
	m_IsPlayingDynamicMusic = false;

	m_InterruptingMusicSoundContainer = nullptr;

	m_CurrentSong = nullptr;
	m_NextSongSectionType = "Default";
	m_NextSongSection = nullptr;

	m_PreviousSoundContainer = nullptr;
	m_CurrentSoundContainer = nullptr;
	m_NextSoundContainer = nullptr;

	m_MusicFadeTimer.Reset();
	m_MusicFadeTimer.SetRealTimeLimitMS(0);
	m_PreviousSoundContainerSetToFade = false;
	m_MusicTimer.Reset();
	m_MusicTimer.SetRealTimeLimitMS(0);
	m_MusicPausedTime = 0.0;
	m_ReturnToDynamicMusic = false;
}

bool MusicMan::Initialize() {
	return true;
}

void MusicMan::Destroy() {
	// AudioMan will stop any music playing on its Destroy since they're just SoundContainers, so we don't need to worry
	Clear();
}

void MusicMan::Update() {
	if (m_IsPlayingDynamicMusic) {
		if (m_MusicTimer.IsPastRealTimeLimit()) {
			CyclePlayingSoundContainers(false);
		}
		if (m_PreviousSoundContainerSetToFade && m_MusicFadeTimer.IsPastRealTimeLimit()) {
			m_PreviousSoundContainerSetToFade = false;
			if (m_PreviousSoundContainer) {
				const int musicFadeOutTimeMs = 250;
				m_PreviousSoundContainer->FadeOut(musicFadeOutTimeMs);
			}
		}
	} else if (!m_ReturnToDynamicMusic) {
		if (m_CurrentSoundContainer && m_CurrentSoundContainer->GetAudibleVolume() == 0.0F) {
			m_CurrentSoundContainer = nullptr;
		}
	}
	if (m_PreviousSoundContainer && m_PreviousSoundContainer->GetAudibleVolume() == 0.0F) {
		m_PreviousSoundContainer = nullptr;
	}
}

bool MusicMan::IsMusicPlaying() const {
	bool interruptingMusicSoundContainerPlaying = m_InterruptingMusicSoundContainer != nullptr && m_InterruptingMusicSoundContainer->GetAudibleVolume() > 0.0F;
	bool previousSoundContainerPlaying = m_PreviousSoundContainer != nullptr && m_PreviousSoundContainer->GetAudibleVolume() > 0.0F;
	bool currentSoundContainerPlaying = m_CurrentSoundContainer != nullptr && m_CurrentSoundContainer->GetAudibleVolume() > 0.0F;
	if (interruptingMusicSoundContainerPlaying || previousSoundContainerPlaying || currentSoundContainerPlaying) {
		return true;
	}
	return false;
}

void MusicMan::ResetMusicState() {
	if (m_InterruptingMusicSoundContainer != nullptr) {
		m_InterruptingMusicSoundContainer->Stop();
	}

	if (m_PreviousSoundContainer != nullptr) {
		m_PreviousSoundContainer->Stop();
	}

	if (m_CurrentSoundContainer != nullptr) {
		m_CurrentSoundContainer->Stop();
	}

	if (m_NextSoundContainer != nullptr) {
		m_NextSoundContainer->Stop();
	}

	Clear();
}

bool MusicMan::PlayDynamicSong(const std::string& songName, const std::string& songSectionType, bool playImmediately, bool playTransition, bool smoothFade) {
	if (const DynamicSong* dynamicSongToPlay = dynamic_cast<const DynamicSong*>(g_PresetMan.GetEntityPreset("DynamicSong", songName))) {
		m_NextSongSection = nullptr;
		m_CurrentSong = std::unique_ptr<DynamicSong>(dynamic_cast<DynamicSong*>(dynamicSongToPlay->Clone()));
		SetNextSongSectionType(songSectionType);
		SelectNextSongSection();
		SelectNextSoundContainer(playTransition);
		// If this isn't the case, then the MusicTimer's existing setup should make it play properly anyway, even if it's just instant
		if (playImmediately) {
			if (m_IsPlayingDynamicMusic) {
				if (m_PreviousSoundContainer) {
					m_PreviousSoundContainer->Stop();
					m_PreviousSoundContainer = nullptr;
				}
			}
			CyclePlayingSoundContainers(smoothFade);
		}
		m_IsPlayingDynamicMusic = true;

		return true;
	}

	return false;
}

bool MusicMan::SetNextDynamicSongSection(const std::string& newSongSectionType, bool playImmediately, bool playTransition, bool smoothFade) {
	std::string currentDynamicSongSection = "None";
	if (m_NextSongSection) {
		currentDynamicSongSection = m_NextSongSection->GetPresetName();
	}

	if (!m_IsPlayingDynamicMusic) {
		return false;
	}
	SetNextSongSectionType(newSongSectionType);
	SelectNextSongSection();
	SelectNextSoundContainer(playTransition);
	if (playImmediately) {
		if (m_PreviousSoundContainerSetToFade) {
			m_PreviousSoundContainerSetToFade = false;
			if (m_PreviousSoundContainer) {
				m_PreviousSoundContainer->Stop();
				m_PreviousSoundContainer = nullptr;
			}
		}
		CyclePlayingSoundContainers(smoothFade);
	}
	return true;
}

bool MusicMan::CyclePlayingSoundContainers(bool smoothFade) {
	std::string currentSoundContainer = "None";
	if (m_CurrentSoundContainer) {
		currentSoundContainer = m_CurrentSoundContainer->GetPresetName();
	}

	std::string previousSoundContainer = "None";
	if (m_PreviousSoundContainer) {
		previousSoundContainer = m_PreviousSoundContainer->GetPresetName();
	}

	if (m_CurrentSoundContainer && m_CurrentSoundContainer->IsBeingPlayed()) {
		if (smoothFade) {
			m_CurrentSoundContainer->FadeOut(static_cast<int>(m_NextSoundContainer->GetMusicPreEntryTime()));
		} else if (!m_MusicTimer.IsPastRealTimeLimit()) {
			m_PreviousSoundContainerSetToFade = true;
			m_MusicFadeTimer.Reset();
			m_MusicFadeTimer.SetRealTimeLimitMS(static_cast<int>(m_NextSoundContainer->GetMusicPreEntryTime()));
		}
		if (m_PreviousSoundContainer) {
			m_PreviousSoundContainerSetToFade = false;
			m_PreviousSoundContainer->Stop();
			m_PreviousSoundContainer = nullptr;
		}
		m_PreviousSoundContainer = std::unique_ptr<SoundContainer>(m_CurrentSoundContainer.release());
	}

	// Clone instead of just point to because we might wanna keep this around even if the DynamicSong is gone
	m_CurrentSoundContainer = std::unique_ptr<SoundContainer>(dynamic_cast<SoundContainer*>(m_NextSoundContainer->Clone()));
	SelectNextSoundContainer();
	m_MusicTimer.Reset();
	float exitTime = m_CurrentSoundContainer->GetMusicExitTime();
	if (exitTime == 0.0F) {
		exitTime = m_CurrentSoundContainer->GetLength(SoundContainer::LengthOfSoundType::NextPlayed);
	}
	double timeUntilNextShouldBePlayed = std::max(0.0F, exitTime - m_NextSoundContainer->GetMusicPreEntryTime());
	m_MusicTimer.SetRealTimeLimitMS(timeUntilNextShouldBePlayed);
	m_CurrentSoundContainer->Play();
	m_CurrentSongSectionType = m_NextSongSectionType;

	return true;
}

bool MusicMan::EndDynamicMusic(bool fadeOutCurrent) {
	if (!m_IsPlayingDynamicMusic) {
		return false;
	}

	m_CurrentSong = nullptr;
	m_NextSongSection = nullptr;

	if (m_PreviousSoundContainer && m_PreviousSoundContainer->IsBeingPlayed()) {
		m_PreviousSoundContainer->FadeOut(500);
	}

	if (fadeOutCurrent && m_CurrentSoundContainer && m_CurrentSoundContainer->IsBeingPlayed()) {
		m_CurrentSoundContainer->FadeOut(2000);
	}

	m_NextSoundContainer = nullptr;

	m_MusicTimer.Reset();
	m_MusicTimer.SetRealTimeLimitMS(0);
	m_MusicPausedTime = 0.0;
	m_ReturnToDynamicMusic = false;
	m_IsPlayingDynamicMusic = false;
	return true;
}

void MusicMan::PlayInterruptingMusic(const SoundContainer* soundContainer) {
	if (m_InterruptingMusicSoundContainer != nullptr) {
		m_InterruptingMusicSoundContainer->Stop();
	}

	if (m_PreviousSoundContainer != nullptr) {
		m_PreviousSoundContainer->SetPaused(true);
	}

	if (m_CurrentSoundContainer != nullptr) {
		m_CurrentSoundContainer->SetPaused(true);
	}

	if (m_NextSoundContainer != nullptr) {
		m_NextSoundContainer->SetPaused(true);
	}

	m_InterruptingMusicSoundContainer = std::unique_ptr<SoundContainer>(dynamic_cast<SoundContainer*>(soundContainer->Clone()));
	m_InterruptingMusicSoundContainer->Play();
	if (m_IsPlayingDynamicMusic) {
		m_ReturnToDynamicMusic = true;
		m_IsPlayingDynamicMusic = false;
		m_MusicPausedTime = m_MusicTimer.GetElapsedRealTimeMS();
	}
}

void MusicMan::EndInterruptingMusic() {
	if (m_InterruptingMusicSoundContainer && m_InterruptingMusicSoundContainer->IsBeingPlayed()) {
		m_InterruptingMusicSoundContainer->Stop();

		if (m_PreviousSoundContainer != nullptr) {
			m_PreviousSoundContainer->SetPaused(false);
		}

		if (m_CurrentSoundContainer != nullptr) {
			m_CurrentSoundContainer->SetPaused(false);
		}

		if (m_NextSoundContainer != nullptr) {
			m_NextSoundContainer->SetPaused(false);
		}

		if (m_ReturnToDynamicMusic) {
			m_ReturnToDynamicMusic = false;
			m_IsPlayingDynamicMusic = true;
			double elapsedPausedTime = m_MusicTimer.GetElapsedRealTimeMS() - m_MusicPausedTime;
			m_MusicTimer.SetRealTimeLimitMS(m_MusicTimer.GetRealTimeLimitMS() + elapsedPausedTime);
		}
	}
}

void MusicMan::SelectNextSongSection() {
	if (m_NextSongSection && m_NextSongSection->GetSectionType() == m_NextSongSectionType) {
		// Our current song section is already suitable
		return;
	}

	for (DynamicSongSection& dynamicSongSection: m_CurrentSong->GetSongSections()) {
		if (dynamicSongSection.GetSectionType() == m_NextSongSectionType) {
			m_NextSongSection = &dynamicSongSection;
			return;
		}
	}

	m_NextSongSection = &m_CurrentSong->GetDefaultSongSection();
}

void MusicMan::SelectNextSoundContainer(bool playTransition) {
	if (playTransition) {
		m_NextSoundContainer = &m_NextSongSection->SelectTransitionSoundContainer();
	} else {
		m_NextSoundContainer = &m_NextSongSection->SelectSoundContainer();
	}
}

namespace RTE {
struct MusicCheckpoint {
    template<class T> static std::string SaveRecord(const T& record, const char* version) {
        CheckpointWriter archive(version); const_cast<T&>(record).Fields(archive); return archive.Text();
    }
    template<class T> static bool LoadRecord(T& record, std::string_view text, const char* version, bool validateOnly) {
        try { T value; CheckpointReader archive(text, version); value.Fields(archive); archive.Finish(); if (!validateOnly) record = std::move(value); return true; }
        catch (const std::exception&) { return false; }
    }
    struct Sample {
        std::string content;
        std::string backend;
        Vector offset;
        float minimum = 0, attenuation = -1;
        template<class A> void Fields(A& a) { a(content, backend, offset, minimum, attenuation); }
        std::string SaveCheckpoint() const { return SaveRecord(*this, "MusicSample1"); }
        bool LoadCheckpoint(std::string_view text, bool validateOnly = false) { return LoadRecord(*this, text, "MusicSample1", validateOnly); }
    };
    struct Set {
        int cycle = 0;
        std::pair<bool, int> selection{false, -1};
        std::vector<Sample> samples;
        std::vector<Set> subsets;
        template<class A> void Fields(A& a) { a(cycle, selection, samples, subsets); }
        std::string SaveCheckpoint() const { return SaveRecord(*this, "MusicSet1"); }
        bool LoadCheckpoint(std::string_view text, bool validateOnly = false) {
            static thread_local unsigned depth = 0;
            if (depth >= 128) return false;
            struct Depth { unsigned& value; explicit Depth(unsigned& value) : value(value) { ++value; } ~Depth() { --value; } } scope(depth);
            return LoadRecord(*this, text, "MusicSet1", validateOnly);
        }
    };
    struct Sound {
        std::string native;
        Set set;
        template<class A> void Fields(A& a) { a(native, set); }
        std::string SaveCheckpoint() const { return SaveRecord(*this, "MusicSound1"); }
        bool LoadCheckpoint(std::string_view text, bool validateOnly = false) { return LoadRecord(*this, text, "MusicSound1", validateOnly); }
    };
    struct Section {
        std::string entity;
        std::vector<Sound> transitions, sounds;
        unsigned int lastTransition = UINT_MAX, lastSound = UINT_MAX;
        std::vector<unsigned int> transitionQueue, queue;
        int cycle = 0;
        std::string type;
        template<class A> void Fields(A& a) { a(entity, transitions, lastTransition, transitionQueue, sounds, lastSound, queue, cycle, type); }
        std::string SaveCheckpoint() const { return SaveRecord(*this, "MusicSection1"); }
        bool LoadCheckpoint(std::string_view text, bool validateOnly = false) { return LoadRecord(*this, text, "MusicSection1", validateOnly); }
    };
    struct Song {
        std::string entity;
        Section fallback;
        std::vector<Section> sections;
        template<class A> void Fields(A& a) { a(entity, fallback, sections); }
        std::string SaveCheckpoint() const { return SaveRecord(*this, "MusicSong1"); }
        bool LoadCheckpoint(std::string_view text, bool validateOnly = false) { return LoadRecord(*this, text, "MusicSong1", validateOnly); }
    };
    struct Record {
        bool playing = false;
        std::string interrupting, song, nextType, currentType;
        int nextSection = -2;
        std::string previous, current;
        std::array<int, 3> nextSound{-2, 0, -1};
        Timer fadeTimer;
        bool fadePrevious = false;
        Timer timer;
        double pausedTime = 0;
        bool returnToDynamic = false;
        template<class A> void Fields(A& a) { a(playing, interrupting, song, nextType, currentType, nextSection, previous, current, nextSound, fadeTimer, fadePrevious, timer, pausedTime, returnToDynamic); }
        std::string SaveCheckpoint() const { return SaveRecord(*this, "MusicMan1"); }
        bool LoadCheckpoint(std::string_view text, bool validateOnly = false) { return LoadRecord(*this, text, "MusicMan1", validateOnly); }
    };
    struct State {
        bool playing = false;
        std::unique_ptr<SoundContainer> interrupting;
        std::unique_ptr<DynamicSong> song;
        std::string nextType, currentType;
        DynamicSongSection* nextSection = nullptr;
        std::unique_ptr<SoundContainer> previous, current;
        SoundContainer* nextSound = nullptr;
        Timer fadeTimer;
        bool fadePrevious = false;
        Timer timer;
        double pausedTime = 0;
        bool returnToDynamic = false;
        std::vector<std::pair<SoundData*, std::string>> sampleBindings;
        void Swap(MusicMan& manager) noexcept {
            std::swap(playing, manager.m_IsPlayingDynamicMusic); interrupting.swap(manager.m_InterruptingMusicSoundContainer);
            song.swap(manager.m_CurrentSong); nextType.swap(manager.m_NextSongSectionType); currentType.swap(manager.m_CurrentSongSectionType);
            std::swap(nextSection, manager.m_NextSongSection); previous.swap(manager.m_PreviousSoundContainer); current.swap(manager.m_CurrentSoundContainer);
            std::swap(nextSound, manager.m_NextSoundContainer); std::swap(fadeTimer, manager.m_MusicFadeTimer);
            std::swap(fadePrevious, manager.m_PreviousSoundContainerSetToFade); std::swap(timer, manager.m_MusicTimer);
            std::swap(pausedTime, manager.m_MusicPausedTime); std::swap(returnToDynamic, manager.m_ReturnToDynamicMusic);
        }
    };

    static Set CaptureSet(const SoundSet& source) {
        Set value; value.cycle = source.m_SoundSelectionCycleMode; value.selection = source.m_CurrentSelection;
        for (const auto& sample: source.m_SoundData) {
            std::string path;
            if (sample.SoundObject) {
                for (const auto& [candidate, sound]: ContentFile::s_LoadedSamples) if (sound == sample.SoundObject && (path.empty() || candidate < path)) path = candidate;
                if (path.empty()) throw std::runtime_error("music sample has no cached asset identity");
            }
            value.samples.push_back({sample.SoundFile.SaveCheckpoint(), path, sample.Offset, sample.MinimumAudibleDistance, sample.AttenuationStartDistance});
        }
        for (const auto* subset: source.m_SubSoundSets) { if (!subset) throw std::runtime_error("null music sound subset"); value.subsets.push_back(CaptureSet(*subset)); }
        return value;
    }
    static Sound CaptureSound(const SoundContainer& source) { return {source.SaveCheckpoint(), CaptureSet(*source.m_TopLevelSoundSet)}; }
    static std::string CaptureSound(const std::unique_ptr<SoundContainer>& source) { return source ? CaptureSound(*source).SaveCheckpoint() : std::string{}; }
    static Section CaptureSection(const DynamicSongSection& source) {
        Section value; value.entity = source.Entity::SaveCheckpoint();
        for (const auto& sound: source.m_TransitionSoundContainers) value.transitions.push_back(CaptureSound(sound));
        for (const auto& sound: source.m_SoundContainers) value.sounds.push_back(CaptureSound(sound));
        value.lastTransition = source.m_LastTransitionSoundContainerIndex; value.lastSound = source.m_LastSoundContainerIndex;
        value.transitionQueue = source.m_TransitionShuffleUnplayedIndices; value.queue = source.m_ShuffleUnplayedIndices;
        value.cycle = source.m_SoundContainerSelectionCycleMode; value.type = source.m_SectionType; return value;
    }
    static Record Capture(const MusicMan& source) {
        Record value; value.playing = source.m_IsPlayingDynamicMusic; value.interrupting = CaptureSound(source.m_InterruptingMusicSoundContainer);
        value.nextType = source.m_NextSongSectionType; value.currentType = source.m_CurrentSongSectionType;
        value.previous = CaptureSound(source.m_PreviousSoundContainer); value.current = CaptureSound(source.m_CurrentSoundContainer);
        value.fadeTimer = source.m_MusicFadeTimer; value.fadePrevious = source.m_PreviousSoundContainerSetToFade;
        value.timer = source.m_MusicTimer; value.pausedTime = source.m_MusicPausedTime; value.returnToDynamic = source.m_ReturnToDynamicMusic;
        bool sectionFound = source.m_NextSongSection == nullptr, soundFound = source.m_NextSoundContainer == nullptr;
        if (source.m_CurrentSong) {
            const auto& song = *source.m_CurrentSong;
            Song captured; captured.entity = song.Entity::SaveCheckpoint(); captured.fallback = CaptureSection(song.m_DefaultSongSection);
            for (const auto& section: song.m_SongSections) captured.sections.push_back(CaptureSection(section));
            value.song = captured.SaveCheckpoint();
            const auto inspect = [&](const DynamicSongSection& section, int index) {
                if (&section == source.m_NextSongSection) { value.nextSection = index; sectionFound = true; }
                for (size_t i = 0; i < section.m_TransitionSoundContainers.size(); ++i) if (&section.m_TransitionSoundContainers[i] == source.m_NextSoundContainer) { value.nextSound = {index, 1, static_cast<int>(i)}; soundFound = true; }
                for (size_t i = 0; i < section.m_SoundContainers.size(); ++i) if (&section.m_SoundContainers[i] == source.m_NextSoundContainer) { value.nextSound = {index, 0, static_cast<int>(i)}; soundFound = true; }
            };
            inspect(song.m_DefaultSongSection, -1);
            for (size_t i = 0; i < song.m_SongSections.size(); ++i) inspect(song.m_SongSections[i], static_cast<int>(i));
        }
        if (!sectionFound || !soundFound) throw std::runtime_error("music points outside its owned song");
        return value;
    }

    static bool ValidateSet(const Set& value, int depth = 0) {
        if (depth > 128 || value.cycle < 0 || value.cycle > SoundSet::ALL || value.selection.second < -1) return false;
        if (value.selection.second >= 0 && static_cast<size_t>(value.selection.second) >= (value.selection.first ? value.subsets.size() : value.samples.size())) return false;
        ContentFile probe;
        for (const auto& sample: value.samples) if (!probe.LoadCheckpoint(sample.content, true)) return false;
        for (const auto& subset: value.subsets) if (!ValidateSet(subset, depth + 1)) return false;
        return true;
    }
    static bool ValidateSound(const Sound& value, SoundContainer& probe, std::set<uint64_t>& identities) {
        if (!probe.LoadCheckpoint(value.native, true) || !ValidateSet(value.set)) return false;
        CheckpointReader reader(value.native, "SoundContainer1"); std::string entity; uint64_t identity; reader.Value(entity); reader.Value(identity);
        return identities.insert(identity).second;
    }
    static bool ValidateSection(const Section& value, SoundContainer& probe, std::set<uint64_t>& identities) {
        if (!probe.Entity::LoadCheckpoint(value.entity, true) || value.cycle < 0 || value.cycle > DynamicSongSection::SHUFFLE) return false;
        const auto validQueue = [](const auto& queue, unsigned int last, size_t count) {
            if (last != UINT_MAX && last >= count) return false;
            std::set<unsigned int> seen; for (unsigned int index: queue) if (index >= count || !seen.insert(index).second) return false; return true;
        };
        if (!validQueue(value.transitionQueue, value.lastTransition, value.transitions.size()) || !validQueue(value.queue, value.lastSound, value.sounds.size())) return false;
        for (const auto& sound: value.transitions) if (!ValidateSound(sound, probe, identities)) return false;
        for (const auto& sound: value.sounds) if (!ValidateSound(sound, probe, identities)) return false;
        return true;
    }
    static bool Validate(const Record& value) {
        AudioMan::CheckpointRegistryScope registryScope;
        SoundContainer probe; std::set<uint64_t> identities;
        for (const auto* text: {&value.interrupting, &value.previous, &value.current}) if (!text->empty()) { Sound sound; if (!sound.LoadCheckpoint(*text) || !ValidateSound(sound, probe, identities)) return false; }
        Song song;
        if (!value.song.empty()) {
            if (!song.LoadCheckpoint(value.song) || !probe.Entity::LoadCheckpoint(song.entity, true) || !ValidateSection(song.fallback, probe, identities)) return false;
            for (const auto& section: song.sections) if (!ValidateSection(section, probe, identities)) return false;
        } else if (value.playing || value.returnToDynamic || value.nextSection != -2 || value.nextSound[2] != -1) return false;
        const auto section = [&](int index) -> const Section* { if (value.song.empty() || index < -1 || index >= static_cast<int>(song.sections.size())) return nullptr; return index == -1 ? &song.fallback : &song.sections[index]; };
        if (value.nextSection != -2 && !section(value.nextSection)) return false;
        if (value.nextSound[2] != -1) {
            const auto* owner = section(value.nextSound[0]);
            if (!owner || value.nextSound[1] < 0 || value.nextSound[1] > 1 || value.nextSound[2] < 0 || value.nextSound[2] >= static_cast<int>(value.nextSound[1] ? owner->transitions.size() : owner->sounds.size())) return false;
        } else if (value.nextSound != std::array<int, 3>{-2, 0, -1}) return false;
        return true;
    }
    static void BuildSet(const Set& record, SoundSet& value, std::vector<std::pair<SoundData*, std::string>>& bindings) {
        value.m_SoundSelectionCycleMode = static_cast<SoundSet::SoundSelectionCycleMode>(record.cycle); value.m_CurrentSelection = record.selection;
        value.m_SoundData.resize(record.samples.size());
        for (size_t i = 0; i < record.samples.size(); ++i) {
            const auto& sample = record.samples[i]; auto& data = value.m_SoundData[i];
            if (!data.SoundFile.LoadCheckpoint(sample.content)) throw std::runtime_error("invalid music sample metadata");
            data.SoundObject = nullptr; data.Offset = sample.offset; data.MinimumAudibleDistance = sample.minimum; data.AttenuationStartDistance = sample.attenuation;
            if (!sample.backend.empty()) bindings.emplace_back(&data, sample.backend);
        }
        value.m_SubSoundSets.reserve(record.subsets.size());
        for (const auto& subset: record.subsets) { auto child = std::make_unique<SoundSet>(); BuildSet(subset, *child, bindings); value.m_SubSoundSets.push_back(child.release()); }
    }
    static void BuildSound(const Sound& record, SoundContainer& value, std::vector<std::pair<SoundData*, std::string>>& bindings) {
        auto set = std::make_shared<SoundSet>(); BuildSet(record.set, *set, bindings);
        if (!value.LoadCheckpoint(record.native)) throw std::runtime_error("invalid music sound metadata");
        value.m_TopLevelSoundSet = std::move(set);
    }
    static std::unique_ptr<SoundContainer> BuildSound(const std::string& text, std::vector<std::pair<SoundData*, std::string>>& bindings) {
        if (text.empty()) return nullptr;
        Sound record; if (!record.LoadCheckpoint(text)) throw std::runtime_error("invalid music sound");
        auto sound = std::make_unique<SoundContainer>(); BuildSound(record, *sound, bindings); return sound;
    }
    static void BuildSection(const Section& record, DynamicSongSection& value, std::vector<std::pair<SoundData*, std::string>>& bindings) {
        if (!value.Entity::LoadCheckpoint(record.entity)) throw std::runtime_error("invalid music section metadata");
        value.m_TransitionSoundContainers.resize(record.transitions.size()); value.m_SoundContainers.resize(record.sounds.size());
        for (size_t i = 0; i < record.transitions.size(); ++i) BuildSound(record.transitions[i], value.m_TransitionSoundContainers[i], bindings);
        for (size_t i = 0; i < record.sounds.size(); ++i) BuildSound(record.sounds[i], value.m_SoundContainers[i], bindings);
        value.m_LastTransitionSoundContainerIndex = record.lastTransition; value.m_LastSoundContainerIndex = record.lastSound;
        value.m_TransitionShuffleUnplayedIndices = record.transitionQueue; value.m_ShuffleUnplayedIndices = record.queue;
        value.m_SoundContainerSelectionCycleMode = static_cast<DynamicSongSection::SoundContainerSelectionCycleMode>(record.cycle); value.m_SectionType = record.type;
    }
    static std::unique_ptr<State> Build(const Record& record) {
        auto value = std::make_unique<State>(); value->playing = record.playing;
        value->interrupting = BuildSound(record.interrupting, value->sampleBindings); value->previous = BuildSound(record.previous, value->sampleBindings); value->current = BuildSound(record.current, value->sampleBindings);
        value->nextType = record.nextType; value->currentType = record.currentType; value->fadeTimer = record.fadeTimer; value->fadePrevious = record.fadePrevious;
        value->timer = record.timer; value->pausedTime = record.pausedTime; value->returnToDynamic = record.returnToDynamic;
        if (!record.song.empty()) {
            Song song; if (!song.LoadCheckpoint(record.song)) throw std::runtime_error("invalid current music song");
            value->song = std::make_unique<DynamicSong>(); auto& native = *value->song;
            if (!native.Entity::LoadCheckpoint(song.entity)) throw std::runtime_error("invalid music song metadata");
            BuildSection(song.fallback, native.m_DefaultSongSection, value->sampleBindings); native.m_SongSections.resize(song.sections.size());
            for (size_t i = 0; i < song.sections.size(); ++i) BuildSection(song.sections[i], native.m_SongSections[i], value->sampleBindings);
            const auto section = [&](int index) { return index == -1 ? &native.m_DefaultSongSection : &native.m_SongSections.at(index); };
            if (record.nextSection != -2) value->nextSection = section(record.nextSection);
            if (record.nextSound[2] >= 0) { auto* owner = section(record.nextSound[0]); value->nextSound = &(record.nextSound[1] ? owner->m_TransitionSoundContainers : owner->m_SoundContainers).at(record.nextSound[2]); }
        }
        return value;
    }
    static std::set<const SoundContainer*> OwnedSounds(const MusicMan& manager) {
        std::set<const SoundContainer*> result;
        for (const auto* value: {manager.m_InterruptingMusicSoundContainer.get(), manager.m_PreviousSoundContainer.get(), manager.m_CurrentSoundContainer.get()}) if (value) result.insert(value);
        const auto section = [&result](const DynamicSongSection& owner) { for (const auto& sound: owner.m_SoundContainers) result.insert(&sound); for (const auto& sound: owner.m_TransitionSoundContainers) result.insert(&sound); };
        if (manager.m_CurrentSong) { section(manager.m_CurrentSong->m_DefaultSongSection); for (const auto& value: manager.m_CurrentSong->m_SongSections) section(value); }
        return result;
    }
    static CheckpointSoundRegistry WithoutOwners(CheckpointSoundRegistry registry, const std::set<const SoundContainer*>& owners) {
        for (auto entry = registry.begin(); entry != registry.end();) {
            std::erase_if(entry->second, [&owners](const SoundContainer* owner) { return owners.contains(owner); });
            if (entry->second.empty()) entry = registry.erase(entry); else ++entry;
        }
        return registry;
    }
    static bool RestoreControls(const Record& record, MusicMan& manager) {
        const auto sound = [](const std::string& text, const std::unique_ptr<SoundContainer>& value) {
            if (text.empty()) return value == nullptr;
            Sound saved; return value && saved.LoadCheckpoint(text) && value->LoadCheckpoint(saved.native);
        };
        if (!sound(record.interrupting, manager.m_InterruptingMusicSoundContainer) || !sound(record.previous, manager.m_PreviousSoundContainer) || !sound(record.current, manager.m_CurrentSoundContainer)) return false;
        if (record.song.empty()) return !manager.m_CurrentSong;
        Song song; if (!manager.m_CurrentSong || !song.LoadCheckpoint(record.song)) return false;
        const auto section = [](const Section& saved, DynamicSongSection& native) {
            if (saved.transitions.size() != native.m_TransitionSoundContainers.size() || saved.sounds.size() != native.m_SoundContainers.size()) return false;
            for (size_t i = 0; i < saved.transitions.size(); ++i) if (!native.m_TransitionSoundContainers[i].LoadCheckpoint(saved.transitions[i].native)) return false;
            for (size_t i = 0; i < saved.sounds.size(); ++i) if (!native.m_SoundContainers[i].LoadCheckpoint(saved.sounds[i].native)) return false;
            return true;
        };
        if (!section(song.fallback, manager.m_CurrentSong->m_DefaultSongSection) || song.sections.size() != manager.m_CurrentSong->m_SongSections.size()) return false;
        for (size_t i = 0; i < song.sections.size(); ++i) if (!section(song.sections[i], manager.m_CurrentSong->m_SongSections[i])) return false;
        return true;
    }
    static bool SelfTest(MusicMan& manager) {
        const std::string originalAudio = g_AudioMan.SaveCheckpoint();
        const auto originalRegistry = g_AudioMan.CaptureCheckpointSoundRegistry();
        const RandomGenerator originalRNG = g_RenderRNG;
        State original; original.Swap(manager);
        bool ok = true;
        try {
            const auto* preset = dynamic_cast<const SoundContainer*>(g_PresetMan.GetEntityPreset("SoundContainer", "Funds Changed", "Base.rte"));
            if (!preset) throw std::runtime_error("missing music checkpoint fixture sample");
            manager.m_CurrentSong = std::make_unique<DynamicSong>();
            auto& section = manager.m_CurrentSong->m_DefaultSongSection;
            for (int i = 0; i < 3; ++i) { section.m_SoundContainers.emplace_back(*preset); section.m_SoundContainers.back().SetBusRouting(SoundContainer::MUSIC); }
            for (int i = 0; i < 2; ++i) section.m_TransitionSoundContainers.emplace_back(*preset);
            section.m_SoundContainerSelectionCycleMode = DynamicSongSection::SHUFFLE;
            section.m_LastSoundContainerIndex = 1; section.m_ShuffleUnplayedIndices = {2, 0};
            section.m_LastTransitionSoundContainerIndex = 0; section.m_TransitionShuffleUnplayedIndices = {1};
            manager.m_CurrentSoundContainer = std::make_unique<SoundContainer>(section.m_SoundContainers[0]);
            manager.m_PreviousSoundContainer = std::make_unique<SoundContainer>(section.m_SoundContainers[1]);
            manager.m_InterruptingMusicSoundContainer = std::make_unique<SoundContainer>(section.m_SoundContainers[2]);
            for (auto* sound: {manager.m_CurrentSoundContainer.get(), manager.m_PreviousSoundContainer.get(), manager.m_InterruptingMusicSoundContainer.get()}) {
                sound->SetPaused(true); sound->SetImmobile(true); sound->SetLoopSetting(-1); if (!sound->Play()) throw std::runtime_error("music fixture did not play");
            }
            manager.m_NextSongSection = &section; manager.m_NextSoundContainer = &section.m_SoundContainers[2];
            manager.m_NextSongSectionType = "Checkpoint"; manager.m_CurrentSongSectionType = "Before";
            manager.m_IsPlayingDynamicMusic = false; manager.m_ReturnToDynamicMusic = true; manager.m_PreviousSoundContainerSetToFade = true;
            manager.m_MusicPausedTime = 137.125; manager.m_MusicTimer.SetStartSimTimeTicks(73); manager.m_MusicFadeTimer.SetRealTimeLimitTicks(271);
            const std::string music = manager.SaveCheckpoint(), audio = g_AudioMan.SaveCheckpoint();
            SoundContainer* originalCurrent = manager.m_CurrentSoundContainer.get();
            const std::string playback = g_AudioMan.GetSoundContainerPlaybackCheckpoint(originalCurrent);
            Record missingOwner; if (!missingOwner.LoadCheckpoint(music)) throw std::runtime_error("could not parse music fixture");
            missingOwner.current.clear();
            if (manager.LoadCheckpointWithAudio(missingOwner.SaveCheckpoint(), audio) || manager.m_CurrentSoundContainer.get() != originalCurrent || manager.SaveCheckpoint() != music || g_AudioMan.GetSoundContainerPlaybackCheckpoint(originalCurrent) != playback) throw std::runtime_error("failed music apply changed original owners or playback");
            if (manager.LoadCheckpointWithAudio(music + "trailing", audio) || manager.LoadCheckpointWithAudio(music, audio + "trailing")) throw std::runtime_error("malformed music checkpoint accepted");
            manager.m_CurrentSong->m_DefaultSongSection.m_ShuffleUnplayedIndices = {0}; manager.m_NextSoundContainer = &section.m_SoundContainers[0]; manager.m_MusicPausedTime = -5;
            if (!manager.LoadCheckpointWithAudio(music, audio) || manager.SaveCheckpoint() != music || manager.m_CurrentSoundContainer.get() == originalCurrent || g_AudioMan.GetSoundContainerPlaybackCheckpoint(manager.m_CurrentSoundContainer.get()) != playback) throw std::runtime_error("music state, queue, alias or playback changed after restore");
            auto& restoredSection = manager.m_CurrentSong->m_DefaultSongSection;
            if (restoredSection.m_ShuffleUnplayedIndices != std::vector<unsigned int>{2, 0} || restoredSection.m_TransitionShuffleUnplayedIndices != std::vector<unsigned int>{1} || manager.m_NextSoundContainer != &restoredSection.m_SoundContainers[2] || manager.m_NextSongSection != &restoredSection || manager.m_MusicPausedTime != 137.125 || manager.m_MusicTimer.GetStartSimTimeMS() != 73 || manager.m_MusicFadeTimer.GetRealTimeLimitTicks() != 271) throw std::runtime_error("native music continuation values were not restored");
            SoundContainer* heldCurrent = manager.m_CurrentSoundContainer.get();
            auto held = manager.TakeCheckpointOwners();
            if (manager.m_CurrentSoundContainer || !manager.RestoreCheckpointOwners(held) || held || manager.m_CurrentSoundContainer.get() != heldCurrent || manager.SaveCheckpoint() != music || g_AudioMan.GetSoundContainerPlaybackCheckpoint(heldCurrent) != playback) throw std::runtime_error("music hold/reinstate lost original owner identity");
            manager.EndDynamicMusic();
            // A stopped song must not leave a pointer into its destroyed sections.
            manager.m_IsPlayingDynamicMusic = true; manager.EndDynamicMusic();
            if (manager.m_NextSongSection || manager.m_NextSoundContainer) throw std::runtime_error("ended music retained a dead song alias");
            std::cout << "[music-checkpoint-selftest] full song/selection, owner rollback, alias and paused playback PASS" << std::endl;
        } catch (const std::exception& error) { std::cout << "[music-checkpoint-selftest] " << error.what() << std::endl; ok = false; }
        State discarded; discarded.Swap(manager); original.Swap(manager);
        g_AudioMan.RestoreCheckpointSoundRegistry(originalRegistry);
        ok = g_AudioMan.LoadCheckpoint(originalAudio) && ok;
        g_RenderRNG = originalRNG;
        return ok;
    }
};

struct MusicMan::CheckpointOwners {
    std::unique_ptr<MusicCheckpoint::State> state = std::make_unique<MusicCheckpoint::State>();
    MusicCheckpoint::Record record;
};
}

std::string MusicMan::SaveCheckpoint() const { return MusicCheckpoint::Capture(*this).SaveCheckpoint(); }

bool MusicMan::LoadCheckpoint(std::string_view text, bool validateOnly) {
    // An owning music checkpoint must commit together with its AudioMan voices.
    if (!validateOnly) return LoadCheckpointWithAudio(text, g_AudioMan.SaveCheckpoint());
    try { MusicCheckpoint::Record record; return record.LoadCheckpoint(text) && MusicCheckpoint::Validate(record); }
    catch (const std::exception&) { return false; }
}

bool MusicMan::LoadCheckpointWithAudio(std::string_view text, std::string_view audio, const std::vector<std::pair<SoundData*, std::string>>* inheritedBindings) {
    const auto originalRegistry = g_AudioMan.CaptureCheckpointSoundRegistry();
    const uint64_t originalCursor = g_AudioMan.GetCheckpointSoundContainerCursor();
    std::unique_ptr<MusicCheckpoint::State> candidate;
    bool swapped = false;
    try {
        MusicCheckpoint::Record record;
        if (!record.LoadCheckpoint(text) || !MusicCheckpoint::Validate(record) || !g_AudioMan.LoadCheckpoint(audio, true)) return false;
        if (SaveCheckpoint() == text) return g_AudioMan.LoadCheckpoint(audio, false, inheritedBindings);
        CheckpointSoundRegistry registrations;
        {
            AudioMan::CheckpointRegistryScope registryScope;
            candidate = MusicCheckpoint::Build(record);
            registrations = g_AudioMan.AddedCheckpointSoundRegistrations(originalRegistry);
        }
        if (inheritedBindings) candidate->sampleBindings.insert(candidate->sampleBindings.end(), inheritedBindings->begin(), inheritedBindings->end());
        g_AudioMan.RestoreCheckpointSoundRegistry(MusicCheckpoint::WithoutOwners(originalRegistry, MusicCheckpoint::OwnedSounds(*this)));
        g_AudioMan.ActivateCheckpointSoundRegistrations(registrations);
        candidate->Swap(*this); swapped = true;
        if (!g_AudioMan.LoadCheckpoint(audio, false, &candidate->sampleBindings)) throw std::runtime_error("music audio restoration failed");
        return true;
    } catch (const std::exception& error) {
        if (swapped) candidate->Swap(*this);
        g_AudioMan.RestoreCheckpointSoundRegistry(originalRegistry);
        g_AudioMan.SetCheckpointSoundContainerCursor(originalCursor);
        std::cout << "[music-checkpoint] " << error.what() << std::endl;
        return false;
    }
}

bool MusicMan::RunCheckpointSelfTest() { return MusicCheckpoint::SelfTest(*this); }

std::shared_ptr<MusicMan::CheckpointOwners> MusicMan::TakeCheckpointOwners() {
    auto owners = std::make_shared<CheckpointOwners>();
    owners->record = MusicCheckpoint::Capture(*this);
    g_AudioMan.RestoreCheckpointSoundRegistry(MusicCheckpoint::WithoutOwners(g_AudioMan.CaptureCheckpointSoundRegistry(), MusicCheckpoint::OwnedSounds(*this)));
    owners->state->Swap(*this);
    return owners;
}

bool MusicMan::RestoreCheckpointOwners(std::shared_ptr<CheckpointOwners>& owners) {
    if (!owners) return true;
    owners->state->Swap(*this);
    const bool restored = MusicCheckpoint::RestoreControls(owners->record, *this);
    owners.reset();
    return restored;
}
