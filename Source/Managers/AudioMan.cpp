#include "AudioMan.h"
#include <cstdlib>
#include <cstring>
#include "SoundSimulation.h"
#include "CheckpointArchive.h"
#include "AudioCheckpoint.h"

#include "CameraMan.h"
#include "ConsoleMan.h"
#include "FrameMan.h"
#include "SceneMan.h"
#include "ActivityMan.h"
#include "SoundContainer.h"
#include "WindowMan.h"
#include "SoundSet.h"
#include "ContentFile.h"
#include "PresetMan.h"
#include "MovableObject.h"
#include "MovableMan.h"
#include "Actor.h"
#include "AEmitter.h"
#include "HDFirearm.h"
#include "GUISound.h"
#include "LuaMan.h"
#include "MusicMan.h"
#include "NetLockstep.h"
#include "ScenarioRunner.h"
#include "Scene.h"
#include "SceneObject.h"
#include "Writer.h"

#include <iostream>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <functional>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>

using namespace RTE;

thread_local SoundSimulationScope* SoundSimulationScope::s_Current = nullptr;

uint64_t SoundSimulationScope::Mix(uint64_t value) {
	value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}

SoundSimulationScope::SoundSimulationScope(uint64_t objectUID, uint64_t phase, SoundExecutionDomain domain, uint64_t occurrence) : m_Previous(s_Current) {
	// A nested native callback inside local AI remains local unless it explicitly
	// enters presentation. Its shared cohort must not inherit local AI decisions.
	if (m_Previous && m_Previous->m_Key.domain == SoundExecutionDomain::LocalSimulation && domain == SoundExecutionDomain::SharedSimulation) domain = SoundExecutionDomain::LocalSimulation;
	// Only shared children number their parent's shared sequence: a per-machine AI scope must not shift the keys every peer derives.
	if (m_Previous && !occurrence && domain == SoundExecutionDomain::SharedSimulation && m_Previous->m_Key.domain == domain) occurrence = Mix(m_Previous->m_Seed ^ ++m_Previous->m_ChildOrdinal);
	else if (m_Previous && !occurrence && domain == SoundExecutionDomain::LocalSimulation) occurrence = Mix(m_Previous->m_Seed ^ ~(++m_Previous->m_LocalChildOrdinal));
	m_Key = {domain, objectUID, static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()), phase, occurrence, 0};
	m_Seed = Mix(g_SimRNG.GetSeed() ^ Mix(objectUID) ^ Mix(m_Key.tick) ^ Mix(phase) ^ Mix(occurrence));
	s_Current = this;
	// CC_TRACE_SOUND_SCOPE_OBJECT=<uid> prints every scope opened for that object or directly under it, so two peers' key derivations can be compared.
	static const uint64_t tracedObject = [] { const char* value = std::getenv("CC_TRACE_SOUND_SCOPE_OBJECT"); return value ? std::strtoull(value, nullptr, 10) : 0ULL; }();
	if (tracedObject && (objectUID == tracedObject || (m_Previous && m_Previous->m_Key.objectUID == tracedObject))) {
		std::cout << "[sound-scope] tick=" << m_Key.tick << " domain=" << static_cast<int>(domain) << " uid=" << objectUID << " phase=" << phase << " occurrence=" << occurrence;
		if (m_Previous) std::cout << " parent(uid=" << m_Previous->m_Key.objectUID << " phase=" << m_Previous->m_Key.phase << " occurrence=" << m_Previous->m_Key.occurrence << " shared=" << m_Previous->m_ChildOrdinal << " local=" << m_Previous->m_LocalChildOrdinal << ")";
		std::cout << std::endl;
	}
}

SoundSimulationScope::~SoundSimulationScope() { s_Current = m_Previous; }
SoundExecutionDomain SoundSimulationScope::Domain() { return s_Current ? s_Current->m_Key.domain : SoundExecutionDomain::Presentation; }
SoundExecutionKey SoundSimulationScope::CurrentKey() { return s_Current ? s_Current->m_Key : SoundExecutionKey{}; }
SoundExecutionKey SoundSimulationScope::NextQueryKey() { auto key = CurrentKey(); if (s_Current) key.ordinal = ++s_Current->m_QueryOrdinal; return key; }
SoundExecutionKey SoundSimulationScope::NextPlayKey() { auto key = CurrentKey(); if (s_Current) key.ordinal = ++s_Current->m_PlayOrdinal; return key; }
uint32_t SoundSimulationScope::Draw() { return static_cast<uint32_t>(Mix(m_Seed + (++m_DrawOrdinal * 0x9e3779b97f4a7c15ULL)) >> 32); }
int SoundSimulationScope::RandomNum(int minimum, int maximum) {
	if (!IsSimulation()) return g_RenderRNG.RandomNum(minimum, maximum);
	if (maximum <= minimum) return minimum;
	const uint32_t range = static_cast<uint32_t>(static_cast<int64_t>(maximum) - minimum + 1);
	const uint32_t threshold = (0u - range) % range;
	uint32_t draw; do { draw = s_Current->Draw(); } while (draw < threshold);
	return minimum + static_cast<int>(draw % range);
}
float SoundSimulationScope::RandomNum(float minimum, float maximum) {
	if (!IsSimulation()) return g_RenderRNG.RandomNum(minimum, maximum);
	return minimum + (maximum - minimum) * (static_cast<float>(s_Current->Draw() >> 8) * 0x1.0p-24F);
}

AudioMan::AudioMan() {
	Clear();
}

AudioMan::~AudioMan() {
	Destroy();
}

void AudioMan::Clear() {
	m_AudioSystem = nullptr;
	m_MasterChannelGroup = nullptr;
	m_SFXChannelGroup = nullptr;
	m_UIChannelGroup = nullptr;
	m_MusicChannelGroup = nullptr;
	m_InaudibleTestOutputVerified = false;
	m_ActiveLogicalSounds.clear();
	{
		std::lock_guard lock(m_PendingSoundOpsMutex);
		m_PendingSoundOpContainers.clear();
	}
	m_DeferredSoundOpTick = 0;
	m_DeferredSoundOpOrdinal = 0;
	m_CommittedAudibility.clear();
	m_LastSentAudibility.clear();
	m_AudioEnabled = false;
	m_PlayingVoices.clear();
	m_BackendVoiceIdentities.clear();
	m_NextVoiceIdentity = 0;
	m_CurrentActivityHumanPlayerPositions.clear();
	m_SoundChannelMinimumAudibleDistances.clear();

	m_MuteMaster = false;
	m_MuteMusic = false;
	m_MuteSounds = false;
	m_MuteAudioOnFocusLoss = false;
	m_MasterVolume = 0.5F;
	m_MusicVolume = 1.0F;
	m_SoundsVolume = 1.0F;
	m_GlobalPitch = 1.0F;

	m_SoundPanningEffectStrength = 0.5F;

	//////////////////////////////////////////////////
	// TODO These need to be removed when our soundscape is sorted out. They're only here temporarily to allow for easier tweaking by pawnis.
	m_ListenerZOffset = 400;
	m_MinimumDistanceForPanning = 30.0F;
	//////////////////////////////////////////////////

	m_MusicMuffled = false;

	m_IsInMultiplayerMode = false;
	for (int i = 0; i < c_MaxClients; i++) {
		m_SoundEvents[i].clear();
	}
}

FMOD_RESULT AudioMan::InitializeAudioSystem(bool silentOutput) {
	if (m_AudioSystem) {
		m_AudioSystem->release();
		m_AudioSystem = nullptr;
	}
	FMOD_RESULT audioSystemSetupResult = FMOD::System_Create(&m_AudioSystem);
	if (silentOutput) {
		audioSystemSetupResult = audioSystemSetupResult == FMOD_OK ? m_AudioSystem->setOutput(FMOD_OUTPUTTYPE_NOSOUND) : audioSystemSetupResult;
	}

	FMOD_ADVANCEDSETTINGS audioSystemAdvancedSettings;
	memset(&audioSystemAdvancedSettings, 0, sizeof(audioSystemAdvancedSettings));
	audioSystemAdvancedSettings.cbSize = sizeof(FMOD_ADVANCEDSETTINGS);
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_AudioSystem->getAdvancedSettings(&audioSystemAdvancedSettings) : audioSystemSetupResult;
	audioSystemAdvancedSettings.vol0virtualvol = 0.001F;
	audioSystemAdvancedSettings.randomSeed = g_RenderRNG.RandomNum(0, INT_MAX);

	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_AudioSystem->setAdvancedSettings(&audioSystemAdvancedSettings) : audioSystemSetupResult;
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_AudioSystem->set3DSettings(1, c_PPM, 1) : audioSystemSetupResult;
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_AudioSystem->setSoftwareChannels(c_MaxSoftwareChannels) : audioSystemSetupResult;
	FMOD_INITFLAGS flags = FMOD_INIT_VOL0_BECOMES_VIRTUAL;

#if !RELEASE_BUILD
	flags |= FMOD_INIT_PROFILE_ENABLE;
#endif

	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_AudioSystem->init(c_MaxVirtualChannels * 2, flags, 0) : audioSystemSetupResult;

	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_AudioSystem->getMasterChannelGroup(&m_MasterChannelGroup) : audioSystemSetupResult;
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_AudioSystem->createChannelGroup("SFX", &m_SFXChannelGroup) : audioSystemSetupResult;
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_AudioSystem->createChannelGroup("UI", &m_UIChannelGroup) : audioSystemSetupResult;
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_AudioSystem->createChannelGroup("Music", &m_MusicChannelGroup) : audioSystemSetupResult;

	// Add a lowpass filter to the music channel group for pause menu usage
	FMOD::DSP* dsp_multibandeq;
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_AudioSystem->createDSPByType(FMOD_DSP_TYPE_MULTIBAND_EQ, &dsp_multibandeq) : audioSystemSetupResult;
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? dsp_multibandeq->setParameterFloat(1, 22000.0f) : audioSystemSetupResult; // Functionally inactive lowpass filter
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_MusicChannelGroup->addDSP(0, dsp_multibandeq) : audioSystemSetupResult;

	// Add a safety limiter to the master channel group, after fader
	FMOD::DSP* dsp_limiter;
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_AudioSystem->createDSPByType(FMOD_DSP_TYPE_LIMITER, &dsp_limiter) : audioSystemSetupResult;
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_MasterChannelGroup->addDSP(0, dsp_limiter) : audioSystemSetupResult;

	// Add a compressor to the SFX channel group, pre fader
	// This is pretty heavy-handed, but it sounds great. Might need to be changed once we have sidechaining and fancier things going on.
	FMOD::DSP* dsp_compressor;
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_AudioSystem->createDSPByType(FMOD_DSP_TYPE_COMPRESSOR, &dsp_compressor) : audioSystemSetupResult;
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? dsp_compressor->setParameterFloat(0, -10.0f) : audioSystemSetupResult; // Threshold
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? dsp_compressor->setParameterFloat(1, 3.0f) : audioSystemSetupResult; // Ratio
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? dsp_compressor->setParameterFloat(2, 180.0f) : audioSystemSetupResult; // Attack time
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? dsp_compressor->setParameterFloat(3, 250.0f) : audioSystemSetupResult; // Release time
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? dsp_compressor->setParameterFloat(4, 5.0f) : audioSystemSetupResult; // Make-up gain
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_SFXChannelGroup->addDSP(1, dsp_compressor) : audioSystemSetupResult;

	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_MasterChannelGroup->addGroup(m_SFXChannelGroup) : audioSystemSetupResult;
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_MasterChannelGroup->addGroup(m_UIChannelGroup) : audioSystemSetupResult;
	audioSystemSetupResult = (audioSystemSetupResult == FMOD_OK) ? m_MasterChannelGroup->addGroup(m_MusicChannelGroup) : audioSystemSetupResult;

	return audioSystemSetupResult;
}

bool AudioMan::Initialize() {
	const char* testOutput = std::getenv("CC_TEST_AUDIO_NOSOUND");
	const bool requireSilentTestOutput = testOutput && std::strcmp(testOutput, "1") == 0;
	m_InaudibleTestOutputVerified = false;
	if (requireSilentTestOutput) m_OutputSilenced = true;
	FMOD_RESULT audioSystemSetupResult = InitializeAudioSystem(requireSilentTestOutput);
	if (requireSilentTestOutput) {
		FMOD_OUTPUTTYPE actualOutput = FMOD_OUTPUTTYPE_AUTODETECT;
		const FMOD_RESULT outputResult = audioSystemSetupResult == FMOD_OK ? m_AudioSystem->getOutput(&actualOutput) : audioSystemSetupResult;
		m_InaudibleTestOutputVerified = outputResult == FMOD_OK && actualOutput == FMOD_OUTPUTTYPE_NOSOUND;
		std::cout << "[audio-test-output] requested=NOSOUND verified=" << m_InaudibleTestOutputVerified << " output=" << static_cast<int>(actualOutput) << " result=" << static_cast<int>(outputResult) << std::endl;
		if (!m_InaudibleTestOutputVerified) audioSystemSetupResult = outputResult == FMOD_OK ? FMOD_ERR_OUTPUT_INIT : outputResult;
	} else if (audioSystemSetupResult != FMOD_OK) {
		// Simulation sounds need sample metadata on every peer, so a missing device still gets a silent backend.
		std::cout << "[audio-output] device unavailable, using silent output: " << FMOD_ErrorString(audioSystemSetupResult) << std::endl;
		m_OutputSilenced = true;
		audioSystemSetupResult = InitializeAudioSystem(true);
	}
	m_AudioEnabled = audioSystemSetupResult == FMOD_OK;

	if (!m_AudioEnabled) {
		return false;
	}

	if (m_MuteSounds) {
		SetSoundsMuted();
	}
	if (m_MuteMusic) {
		SetMusicMuted();
	}
	if (m_MuteMaster) {
		SetMasterMuted();
	}

	SetGlobalPitch(m_GlobalPitch, false, false);
	SetSoundsVolume(m_SoundsVolume);
	SetMusicVolume(m_MusicVolume);
	SetMasterVolume(m_MasterVolume);

	return true;
}

void AudioMan::Destroy() {
	if (m_AudioEnabled) {
		StopAll();
		ContentFile::FreeAllLoadedSounds();
		m_AudioSystem->release();
		Clear();
	}
}

void AudioMan::Update() {
	// A completed backend voice can still have a queued engine completion at capture.
	std::vector<int> completed;
	for (const auto& [identity, voice]: m_PlayingVoices) if (!voice.channel) completed.push_back(identity);
	for (int identity: completed) RetireVoice(identity);
	if (m_AudioEnabled) {
		FMOD_RESULT status = FMOD_OK;

		if (m_MuteAudioOnFocusLoss && !g_WindowMan.AnyWindowHasFocus()) {
			m_MasterChannelGroup->setMute(true);
		} else {
			m_MasterChannelGroup->setMute(m_MuteMaster || m_OutputSilenced);
		}

		float globalPitch = 1.0F;

		float timeScale = g_TimerMan.GetTimeScale();
		// Soften the ratio of the pitch adjustment so it's not such an extreme effect on the audio.
		// TODO: This coefficient should probably move to SettingsMan and be loaded from ini. That way this effect can be lessened or even turned off entirely by users. 0.35 is a good default value though.
		globalPitch = timeScale + (1.0F - timeScale) * 0.35F;

		SetGlobalPitch(globalPitch);

		if (!g_ActivityMan.ActivityPaused()) {
			const Activity* currentActivity = g_ActivityMan.GetActivity();
			uint8_t currentActivityHumanCount = m_IsInMultiplayerMode ? 1 : currentActivity->GetHumanCount();

			if (m_CurrentActivityHumanPlayerPositions.size() != currentActivityHumanCount) {
				status = status == FMOD_OK ? m_AudioSystem->set3DNumListeners(currentActivityHumanCount) : status;
			}

			m_CurrentActivityHumanPlayerPositions.clear();
			for (int player = Players::PlayerOne; player < Players::MaxPlayerCount && m_CurrentActivityHumanPlayerPositions.size() < currentActivityHumanCount; player++) {
				if (currentActivity->PlayerActive(player) && currentActivity->PlayerHuman(player)) {
					int screen = currentActivity->ScreenOfPlayer(player);
					Vector humanPlayerPosition = g_CameraMan.GetScrollTarget(screen);
					if (IsInMultiplayerMode()) {
						humanPlayerPosition += (Vector(static_cast<float>(g_FrameMan.GetPlayerFrameBufferWidth(screen)), static_cast<float>(g_FrameMan.GetPlayerFrameBufferHeight(screen))) / 2);
					}
					m_CurrentActivityHumanPlayerPositions.push_back(std::make_unique<const Vector>(humanPlayerPosition));
				}
			}

			int listenerNumber = 0;
			for (const std::unique_ptr<const Vector>& humanPlayerPosition: m_CurrentActivityHumanPlayerPositions) {
				if (status == FMOD_OK) {
					FMOD_VECTOR playerPosition = GetAsFMODVector(*(humanPlayerPosition.get()), m_ListenerZOffset);
					status = m_AudioSystem->set3DListenerAttributes(listenerNumber, &playerPosition, nullptr, &c_FMODForward, &c_FMODUp);
				}
				listenerNumber++;
			}

			Update3DEffectsForSFXChannels();
		} else {
			if (!m_CurrentActivityHumanPlayerPositions.empty()) {
				m_CurrentActivityHumanPlayerPositions.clear();
				status = status == FMOD_OK ? m_AudioSystem->set3DNumListeners(1) : status;
			}
			if (status == FMOD_OK) {
				FMOD_VECTOR scrollTarget = GetAsFMODVector(g_CameraMan.GetScrollTarget(), m_ListenerZOffset);
				status = m_AudioSystem->set3DListenerAttributes(0, &scrollTarget, nullptr, &c_FMODForward, &c_FMODUp);
			}
		}

		status = status == FMOD_OK ? m_AudioSystem->update() : status;
		if (status != FMOD_OK) {
			g_ConsoleMan.PrintString("ERROR: Could not update AudioMan due to FMOD error: " + std::string(FMOD_ErrorString(status)));
		}
	}
}

void AudioMan::SetGlobalPitch(float pitch, bool includeImmobileSounds, bool includeMusic) {
	if (!m_AudioEnabled) {
		return;
	}
	if (m_IsInMultiplayerMode) {
		RegisterSoundEvent(-1, SOUND_SET_GLOBAL_PITCH, nullptr);
	}

	m_GlobalPitch = std::clamp(pitch, 0.125F, 8.0F);

	m_SFXChannelGroup->setPitch(m_GlobalPitch);

	if (includeImmobileSounds) {
		m_UIChannelGroup->setPitch(m_GlobalPitch);
	}

	if (includeMusic) {
		m_MusicChannelGroup->setPitch(m_GlobalPitch);
	}
}

bool AudioMan::SetMusicPitch(float pitch) {
	if (!m_AudioEnabled) {
		return false;
	}

	pitch = Limit(pitch, 8, 0.125); // Limit pitch change to 8 octaves up or down
	FMOD_RESULT result = m_MusicChannelGroup->setPitch(pitch);

	if (result != FMOD_OK) {
		g_ConsoleMan.PrintString("ERROR: Could not set music pitch: " + std::string(FMOD_ErrorString(result)));
	}

	return true;
}

void AudioMan::FinishIngameLoopingSounds() {
	{
		std::lock_guard lock(m_LogicalSoundsMutex);
		const long long now = g_TimerMan.GetSimTimeTicks();
		const long long ticksPerSecond = g_TimerMan.GetTicksPerSecond();
		for (SoundContainer* container: m_ActiveLogicalSounds) {
			for (LogicalSoundVoice& voice: container->m_LogicalPlayback.voices) {
				if (voice.bus != SoundContainer::SFX || voice.loops == 0) continue;
				voice.Fold(now, ticksPerSecond);
				voice.loops = 0;
			}
		}
	}
	if (m_AudioEnabled) {
		int numberOfPlayingChannels;
		FMOD::Channel* soundChannel;

		FMOD_RESULT result = m_SFXChannelGroup->getNumChannels(&numberOfPlayingChannels);
		if (result != FMOD_OK) {
			g_ConsoleMan.PrintString("ERROR: Failed to get the number of playing SFX sound channels when finishing all looping sounds: " + std::string(FMOD_ErrorString(result)));
			return;
		}

		for (int i = 0; i < numberOfPlayingChannels; i++) {
			result = m_SFXChannelGroup->getChannel(i, &soundChannel);
			if (result != FMOD_OK) {
				g_ConsoleMan.PrintString("ERROR: Failed to get SFX sound channel when finishing all looping sounds: " + std::string(FMOD_ErrorString(result)));
				return;
			}
			soundChannel->setLoopCount(0);
		}
	}
}

SoundContainer* AudioMan::PlaySound(const std::string& filePath, const Vector& position, int player) {
	if (m_IsInMultiplayerMode) {
		return nullptr;
	}

	SoundContainer* newSoundContainer = new SoundContainer();
	newSoundContainer->SetPosition(position);
	newSoundContainer->GetTopLevelSoundSet().AddSound(filePath);
	if (newSoundContainer->HasAnySounds()) {
		PlaySoundContainer(newSoundContainer, player);
	}
	return newSoundContainer;
}

void AudioMan::GetSoundEvents(int player, std::list<NetworkSoundData>& list) {
	if (player < 0 || player >= c_MaxClients) {
		return;
	}
	list.clear();

	g_SoundEventsListMutex[player].lock();
	const NetworkSoundData* lastSetGlobalPitchEvent = nullptr;
	for (const NetworkSoundData& soundEvent: m_SoundEvents[player]) {
		if (soundEvent.State == SOUND_SET_GLOBAL_PITCH) {
			lastSetGlobalPitchEvent = &soundEvent;
		} else {
			list.push_back(soundEvent);
		}
	}
	if (lastSetGlobalPitchEvent) {
		list.push_back(*lastSetGlobalPitchEvent);
	}
	m_SoundEvents[player].clear();
	g_SoundEventsListMutex[player].unlock();
}

void AudioMan::RegisterSoundEvent(int player, NetworkSoundState state, const SoundContainer* soundContainer, int fadeoutTime) {
	if (player == -1) {
		for (int i = 0; i < c_MaxClients; i++) {
			RegisterSoundEvent(i, state, soundContainer, fadeoutTime);
		}
	} else {
		FMOD_RESULT result = FMOD_OK;
		std::vector<NetworkSoundData> soundDataVector;

		if (state == SOUND_SET_GLOBAL_PITCH) {
			NetworkSoundData soundData{};
			soundData.State = state;
			soundData.Pitch = m_GlobalPitch;
			soundDataVector.push_back(soundData);
		} else {
			for (int playingChannel: *soundContainer->GetPlayingChannels()) {
				if (!OwnsVoice(playingChannel, soundContainer)) continue;
				FMOD::Channel* soundChannel;
				result = GetVoiceChannel(playingChannel, &soundChannel);
				FMOD::Sound* sound;
				result = (result == FMOD_OK) ? soundChannel->getCurrentSound(&sound) : result;

				if (result != FMOD_OK) {
					continue;
				}
				NetworkSoundData soundData{};
				soundData.State = state;
				soundData.SoundFileHash = soundContainer->GetSoundDataForSound(sound)->SoundFile.GetHash();
				soundData.Channel = playingChannel;
				soundData.Immobile = soundContainer->IsImmobile();
				soundData.AttenuationStartDistance = soundContainer->GetAttenuationStartDistance();
				soundData.Loops = soundContainer->GetLoopSetting();
				soundData.Priority = soundContainer->GetPriority();
				soundData.AffectedByGlobalPitch = soundContainer->IsAffectedByGlobalPitch();
				soundData.Position[0] = soundContainer->GetPosition().m_X;
				soundData.Position[1] = soundContainer->GetPosition().m_Y;
				soundData.Volume = soundContainer->GetVolume();
				soundData.Pitch = soundContainer->GetPitch();
				soundData.FadeOutTime = fadeoutTime;
				soundDataVector.push_back(soundData);
			}
		}

		g_SoundEventsListMutex[player].lock();
		m_SoundEvents[player].insert(m_SoundEvents[player].end(), soundDataVector.begin(), soundDataVector.end());
		g_SoundEventsListMutex[player].unlock();
	}
}

void AudioMan::ClearSoundEvents(int player) {
	if (player == -1 || player >= c_MaxClients) {
		for (int i = 0; i < c_MaxClients; i++) {
			ClearSoundEvents(i);
		}
	} else {
		g_SoundEventsListMutex[player].lock();
		m_SoundEvents[player].clear();
		g_SoundEventsListMutex[player].unlock();
	}
}

bool AudioMan::PlaySoundContainer(SoundContainer* soundContainer, int player) {
	if (!soundContainer) return false;
	const bool logical = soundContainer->UsesLogicalPlayback();
	const bool physical = !s_PlaybackSuppressed && m_AudioEnabled;
	if (!logical && !physical) return false;
	if (logical) soundContainer->RetireFinishedLogicalVoices();
	std::erase_if(soundContainer->m_PlayingChannels, [this, soundContainer](int identity) { return !OwnsVoice(identity, soundContainer); });
	if (logical ? soundContainer->CurrentLogicalPlayback().voices.size() >= c_MaxPlayingSoundsPerContainer : soundContainer->m_PlayingChannels.size() >= c_MaxPlayingSoundsPerContainer) return false;
	FMOD_RESULT result = FMOD_OK;

	// A preview never touches the shared samples; the canonical play sets their properties.
	if (physical && !soundContainer->SoundPropertiesUpToDate()) {
		result = soundContainer->UpdateSoundProperties();
		if (result != FMOD_OK) {
			g_ConsoleMan.PrintString("ERROR: Could not update sound properties for SoundContainer " + soundContainer->GetPresetName() + ": " + std::string(FMOD_ErrorString(result)));
			if (!logical) return false;
			result = FMOD_OK;
		}
	}
	if (!soundContainer->GetTopLevelSoundSet().HasSelectedSounds() && !soundContainer->GetTopLevelSoundSet().SelectNextSounds()) return false;

	FMOD::ChannelGroup* channelGroupToPlayIn = m_SFXChannelGroup;

	switch (soundContainer->GetBusRouting()) {
		case SoundContainer::UI:
			channelGroupToPlayIn = m_UIChannelGroup;
			break;
		case SoundContainer::SFX:
			channelGroupToPlayIn = m_SFXChannelGroup;
			break;
		case SoundContainer::MUSIC:
			channelGroupToPlayIn = m_MusicChannelGroup;
			break;
	}

	FMOD::Channel* channel = nullptr;
	int channelIndex = 0;
	std::vector<const SoundData*> selectedSoundData;
	soundContainer->GetTopLevelSoundSet().GetFlattenedSoundData(selectedSoundData, true);
	float pitchVariationFactor = 1.0F + std::abs(soundContainer->GetPitchVariation());
	std::vector<float> pitches;
	pitches.reserve(selectedSoundData.size());
	std::vector<LogicalSoundVoice> logicalVoices;
	const auto playKey = logical ? SoundSimulationScope::NextPlayKey() : SoundExecutionKey{};
	const long long now = g_TimerMan.GetSimTimeTicks();
	for (size_t sampleIndex = 0; sampleIndex < selectedSoundData.size(); ++sampleIndex) {
		const float variation = pitchVariationFactor == 1.0F ? 1.0F : SoundSimulationScope::RandomNum(1.0F / pitchVariationFactor, pitchVariationFactor);
		pitches.push_back(soundContainer->GetPitch() * variation);
		if (!logical) continue;
		LogicalSoundVoice voice;
		voice.origin.value = playKey;
		voice.sampleOrdinal = static_cast<uint32_t>(sampleIndex);
		const SoundData* data = selectedSoundData[sampleIndex];
		if (!data->SoundObject || data->SoundObject->getLength(&voice.sampleFrames, FMOD_TIMEUNIT_PCM) != FMOD_OK ||
		    data->SoundObject->getDefaults(&voice.sampleRate, nullptr) != FMOD_OK || !voice.sampleFrames || !(voice.sampleRate > 0)) {
			g_ConsoleMan.PrintString("ERROR: No sample metadata for simulation sound " + soundContainer->GetPresetName() + " (" + data->SoundFile.GetDataPath() + ")");
			return false;
		}
		voice.loopEnd = voice.sampleFrames - 1;
		data->SoundObject->getLoopPoints(&voice.loopStart, FMOD_TIMEUNIT_PCM, &voice.loopEnd, FMOD_TIMEUNIT_PCM);
		voice.pitch = pitches.back();
		voice.loops = soundContainer->GetLoopSetting();
		voice.bus = soundContainer->GetBusRouting();
		voice.anchorTicks = now;
		voice.paused = soundContainer->IsPaused();
		if (voice.loops < -1 || !std::isfinite(voice.pitch) || voice.pitch <= 0 || voice.loopStart > voice.loopEnd || voice.loopEnd >= voice.sampleFrames) return false;
		logicalVoices.push_back(std::move(voice));
	}
	if (selectedSoundData.empty()) return false;
	if (logical) {
		LogicalSoundPlayback& playback = soundContainer->CurrentLogicalPlayback();
		if (playback.voices.size() + logicalVoices.size() > c_MaxPlayingSoundsPerContainer) return false;
		if (!playback.identity.value.ordinal) playback.identity.value = playKey;
		playback.lastObjectUID = playKey.objectUID;
		playback.voices.insert(playback.voices.end(), std::make_move_iterator(logicalVoices.begin()), std::make_move_iterator(logicalVoices.end()));
		RefreshLogicalSound(soundContainer);
	}
	size_t sampleIndex = 0;
	for (const SoundData* soundData: selectedSoundData) {
		const float selectedPitch = pitches[sampleIndex++];
		if (!physical) continue;
		if (!MakeVoiceSlotAvailable()) { if (logical) continue; return false; }
		channel = nullptr; channelIndex = 0;
		result = (result == FMOD_OK) ? m_AudioSystem->playSound(soundData->SoundObject, channelGroupToPlayIn, true, &channel) : result;
		if (result == FMOD_OK) channelIndex = RegisterPlayingVoice(channel, soundContainer, soundData->SoundFile.GetDataPath(), soundData->MinimumAudibleDistance);
		if (result != FMOD_OK) { if (logical) { result = FMOD_OK; continue; } return false; }

		result = (result == FMOD_OK) ? channel->setUserData(soundContainer) : result;
		result = (result == FMOD_OK) ? channel->setCallback(SoundChannelEndedCallback) : result;
		result = (result == FMOD_OK) ? channel->setPriority(soundContainer->GetPriority()) : result;
		result = (result == FMOD_OK) ? channel->setPitch(selectedPitch) : result;

		if (soundContainer->GetCustomPanValue() != 0.0f) {
			result = (result == FMOD_OK) ? channel->setPan(soundContainer->GetCustomPanValue()) : result;
		}

		if (soundContainer->IsImmobile()) {
			result = (result == FMOD_OK) ? channel->setVolume(soundContainer->GetVolume()) : result;
		} else {

			FMOD::DSP* dsp_multibandeq;
			result = (result == FMOD_OK) ? m_AudioSystem->createDSPByType(FMOD_DSP_TYPE_MULTIBAND_EQ, &dsp_multibandeq) : result;
			result = (result == FMOD_OK) ? dsp_multibandeq->setParameterFloat(1, 22000.0f) : result; // Functionally inactive lowpass filter
			result = (result == FMOD_OK) ? channel->addDSP(0, dsp_multibandeq) : result;

			{
				std::scoped_lock<std::mutex> lock(m_SoundChannelMinimumAudibleDistancesMutex);
				m_SoundChannelMinimumAudibleDistances.insert({channelIndex, soundData->MinimumAudibleDistance});
			}

			result = (result == FMOD_OK) ? channel->set3DLevel(m_SoundPanningEffectStrength * soundContainer->GetPanningStrengthMultiplier()) : result;

			FMOD_VECTOR soundContainerPosition = GetAsFMODVector(soundContainer->GetPosition() + soundData->Offset);
			UpdatePositionalEffectsForSoundChannel(channel, &soundContainerPosition);
		}

		if (result != FMOD_OK) {
			channel->setCallback(nullptr); channel->setUserData(nullptr); channel->stop(); RetireVoice(channelIndex);
			g_ConsoleMan.PrintString("ERROR: Could not play sounds from SoundContainer " + soundContainer->GetPresetName() + ": " + std::string(FMOD_ErrorString(result)));
			if (logical) { result = FMOD_OK; continue; }
			return false;
		}

		// At this point the sound is ready to go, but if the SoundContainer is explicitly paused, it and this new channel will hopefully be unpaused
		// at some later point in time by whatever paused it
		soundContainer->AddPlayingChannel(channelIndex);
		if (!soundContainer->IsPaused()) {
			result = channel->setPaused(false);
			if (result != FMOD_OK) {
				channel->setCallback(nullptr); channel->setUserData(nullptr); channel->stop(); RetireVoice(channelIndex);
				g_ConsoleMan.PrintString("ERROR: Failed to start playing sounds from SoundContainer " + soundContainer->GetPresetName() + " after setting it up: " + std::string(FMOD_ErrorString(result)));
				if (logical) { result = FMOD_OK; continue; }
				return false;
			}
		}

	}

	if (m_IsInMultiplayerMode) {
		RegisterSoundEvent(player, SOUND_PLAY, soundContainer);
	}

	// Choose the sounds for next time
	bool choseNext = soundContainer->GetTopLevelSoundSet().SelectNextSounds();
	RTEAssert(choseNext, "Unable to select new sounds to play for SoundContainer " + soundContainer->GetPresetName());

	return true;
}

bool AudioMan::ChangeSoundContainerPlayingChannelsPosition(const SoundContainer* soundContainer) {
	if (!m_AudioEnabled || !soundContainer) {
		return false;
	}
	if (m_IsInMultiplayerMode) {
		RegisterSoundEvent(-1, SOUND_SET_POSITION, soundContainer);
	}

	FMOD_RESULT result = FMOD_OK;
	FMOD::Channel* soundChannel;
	FMOD::Sound* sound;

	const std::unordered_set<int>* playingChannels = soundContainer->GetPlayingChannels();
	for (int channelIndex: *playingChannels) {
		if (!VoiceMatchesContext(channelIndex, soundContainer)) continue;
		result = GetVoiceChannel(channelIndex, &soundChannel);
		result = (result == FMOD_OK) ? soundChannel->getCurrentSound(&sound) : result;
		const SoundData* soundData = soundContainer->GetSoundDataForSound(sound);

		FMOD_VECTOR soundPosition = GetAsFMODVector(soundContainer->GetPosition() + ((soundData == nullptr) ? Vector() : soundData->Offset));
		result = (result == FMOD_OK) ? UpdatePositionalEffectsForSoundChannel(soundChannel, &soundPosition) : result;
		if (result != FMOD_OK) {
			g_ConsoleMan.PrintString("ERROR: Could not set sound position for the sound being played on channel " + std::to_string(channelIndex) + " for SoundContainer " + soundContainer->GetPresetName() + ": " + std::string(FMOD_ErrorString(result)));
		}
	}
	return result == FMOD_OK;
}

float AudioMan::GetSoundContainerAudibleVolume(const SoundContainer* soundContainer) {
	if (!m_AudioEnabled || !soundContainer || !soundContainer->IsBeingPlayed()) {
		return 0.0F;
	}

	FMOD_RESULT result;
	FMOD::Channel* soundChannel;
	float audibleVolume;

	const std::unordered_set<int> channels = *soundContainer->GetPlayingChannels();
	for (int channel: channels) {
		if (!VoiceMatchesContext(channel, soundContainer)) continue;
		result = GetVoiceChannel(channel, &soundChannel);
		result = (result == FMOD_OK) ? soundChannel->getAudibility(&audibleVolume) : result;

		if (result != FMOD_OK) {
			g_ConsoleMan.PrintString("ERROR: Could not get sound audible volume in SoundContainer " + soundContainer->GetPresetName() + ": " + std::string(FMOD_ErrorString(result)));
		} else {
			// Simply return the first one, they are all the same
			return audibleVolume;
		}
	}
	return 0.0F;
}

bool AudioMan::ChangeSoundContainerPlayingChannelsVolume(const SoundContainer* soundContainer, float newVolume) {
	if (!m_AudioEnabled || !soundContainer || !soundContainer->IsBeingPlayed()) {
		return false;
	}
	if (m_IsInMultiplayerMode) {
		RegisterSoundEvent(-1, SOUND_SET_VOLUME, soundContainer);
	}

	FMOD_RESULT result = FMOD_OK;
	FMOD::Channel* soundChannel;
	float soundContainerOldVolume = soundContainer->GetVolume() == 0 ? 1.0F : soundContainer->GetVolume();
	float soundChannelCurrentVolume;

	const std::unordered_set<int>* playingChannels = soundContainer->GetPlayingChannels();
	for (int channelIndex: *playingChannels) {
		if (!VoiceMatchesContext(channelIndex, soundContainer)) continue;
		result = GetVoiceChannel(channelIndex, &soundChannel);
		result = result == FMOD_OK ? soundChannel->getVolume(&soundChannelCurrentVolume) : result;

		if (newVolume == 0.0F) {
			result = result == FMOD_OK ? soundChannel->setMute(true) : result;
			result = result == FMOD_OK ? soundChannel->setVolume(soundChannelCurrentVolume / soundContainerOldVolume) : result;
		} else {
			result = result == FMOD_OK ? soundChannel->setMute(false) : result;
			result = result == FMOD_OK ? soundChannel->setVolume(newVolume / soundContainerOldVolume * soundChannelCurrentVolume) : result;
		}
		if (result != FMOD_OK) {
			g_ConsoleMan.PrintString("ERROR: Could not update sound volume for the sound being played on channel " + std::to_string(channelIndex) + " for SoundContainer " + soundContainer->GetPresetName() + ": " + std::string(FMOD_ErrorString(result)));
		}
	}
	return result == FMOD_OK;
}

bool AudioMan::ChangeSoundContainerPlayingChannelsPitch(const SoundContainer* soundContainer) {
	if (!m_AudioEnabled || !soundContainer || !soundContainer->IsBeingPlayed()) {
		return false;
	}
	if (m_IsInMultiplayerMode) {
		RegisterSoundEvent(-1, SOUND_SET_PITCH, soundContainer);
	}

	FMOD_RESULT result = FMOD_OK;
	FMOD::Channel* soundChannel;

	const std::unordered_set<int>* playingChannels = soundContainer->GetPlayingChannels();
	for (int channelIndex: *playingChannels) {
		if (!VoiceMatchesContext(channelIndex, soundContainer)) continue;
		result = GetVoiceChannel(channelIndex, &soundChannel);
		result = result == FMOD_OK ? soundChannel->setPitch(soundContainer->GetPitch()) : result;
		if (result != FMOD_OK) {
			g_ConsoleMan.PrintString("ERROR: Could not update sound pitch for the sound being played on channel " + std::to_string(channelIndex) + " for SoundContainer " + soundContainer->GetPresetName() + ": " + std::string(FMOD_ErrorString(result)));
		}
	}
	return result == FMOD_OK;
}

bool AudioMan::ChangeSoundContainerPlayingChannelsCustomPanValue(const SoundContainer* soundContainer) {
	if (!m_AudioEnabled || !soundContainer || !soundContainer->IsBeingPlayed()) {
		return false;
	}
	if (m_IsInMultiplayerMode) {
		RegisterSoundEvent(-1, SOUND_SET_PITCH, soundContainer);
	}

	FMOD_RESULT result = FMOD_OK;
	FMOD::Channel* soundChannel;

	const std::unordered_set<int>* playingChannels = soundContainer->GetPlayingChannels();
	for (int channelIndex: *playingChannels) {
		if (!VoiceMatchesContext(channelIndex, soundContainer)) continue;
		result = GetVoiceChannel(channelIndex, &soundChannel);
		result = result == FMOD_OK ? soundChannel->setPan(soundContainer->GetCustomPanValue()) : result;
		if (result != FMOD_OK) {
			g_ConsoleMan.PrintString("ERROR: Could not update sound custom pan value for the sound being played on channel " + std::to_string(channelIndex) + " for SoundContainer " + soundContainer->GetPresetName() + ": " + std::string(FMOD_ErrorString(result)));
		}
	}
	return result == FMOD_OK;
}

bool AudioMan::StopSoundContainerPlayingChannels(SoundContainer* soundContainer, int player) {
	if (!soundContainer) return false;
	const bool logical = soundContainer->UsesLogicalPlayback();
	if (logical) {
		if (!soundContainer->HasLiveLogicalVoices()) return false;
		soundContainer->CurrentLogicalPlayback().voices.clear();
		RefreshLogicalSound(soundContainer);
		if (!m_AudioEnabled || s_PlaybackSuppressed) return true;
	} else if (!m_AudioEnabled || !soundContainer->IsBeingPlayed()) return false;
	if (m_IsInMultiplayerMode) {
		RegisterSoundEvent(player, SOUND_STOP, soundContainer);
	}

	FMOD_RESULT result = FMOD_OK;
	FMOD::Channel* soundChannel;

	const std::unordered_set<int>* channels = soundContainer->GetPlayingChannels();
	for (std::unordered_set<int>::const_iterator channelIterator = channels->begin(); channelIterator != channels->end();) {
		const int identity = *channelIterator;
		++channelIterator; // NOTE - stopping the sound will remove the channel, screwing things up if we don't move to the next iterator preemptively
		if (!VoiceMatchesContext(identity, soundContainer)) continue;
		result = GetVoiceChannel(identity, &soundChannel);
		result = (result == FMOD_OK) ? soundChannel->stop() : result;
		if (result != FMOD_OK) {
			g_ConsoleMan.PrintString("Error: Failed to stop playing channel in SoundContainer " + soundContainer->GetPresetName() + ": " + std::string(FMOD_ErrorString(result)));
		}
	}
	return logical || result == FMOD_OK;
}

void AudioMan::DisownSoundContainerPlayingChannels(const SoundContainer* soundContainer) {
	if (!soundContainer) return;
	for (auto& [identity, voice]: m_PlayingVoices) {
		if (voice.owner != soundContainer) continue;
		voice.owner = nullptr;
		if (voice.channel) voice.channel->setUserData(nullptr);
	}
}

void AudioMan::FadeOutSoundContainerPlayingChannels(SoundContainer* soundContainer, int fadeOutTime) {
	if (soundContainer && soundContainer->UsesLogicalPlayback()) {
		const long long now = g_TimerMan.GetSimTimeTicks();
		for (LogicalSoundVoice& voice: soundContainer->CurrentLogicalPlayback().voices) {
			voice.fadeStartTicks = now;
			voice.fadeSeconds = std::max(0, fadeOutTime) / 1000.0;
		}
	}
	if (!m_AudioEnabled || s_PlaybackSuppressed || !soundContainer || !soundContainer->IsBeingPlayed()) {
		return;
	}
	if (m_IsInMultiplayerMode) {
		RegisterSoundEvent(-1, SOUND_FADE_OUT, soundContainer, fadeOutTime);
	}

	int sampleRate;
	m_AudioSystem->getSoftwareFormat(&sampleRate, nullptr, nullptr);
	int fadeOutTimeAsSamples = fadeOutTime * sampleRate / 1000;

	FMOD_RESULT result;
	FMOD::Channel* soundChannel;
	unsigned long long parentClock;
	float currentVolume;

	const std::unordered_set<int> channels = *soundContainer->GetPlayingChannels();
	for (int channel: channels) {
		if (!VoiceMatchesContext(channel, soundContainer)) continue;
		result = GetVoiceChannel(channel, &soundChannel);
		result = (result == FMOD_OK) ? soundChannel->getDSPClock(nullptr, &parentClock) : result;
		result = (result == FMOD_OK) ? soundChannel->getVolume(&currentVolume) : result;
		result = (result == FMOD_OK) ? soundChannel->addFadePoint(parentClock, currentVolume) : result;
		result = (result == FMOD_OK) ? soundChannel->addFadePoint(parentClock + fadeOutTimeAsSamples, 0) : result;

		if (result != FMOD_OK) {
			g_ConsoleMan.PrintString("ERROR: Could not fade out sounds in SoundContainer " + soundContainer->GetPresetName() + ": " + std::string(FMOD_ErrorString(result)));
		}
	}
}

void AudioMan::SetPausedSoundContainerPlayingChannels(SoundContainer* soundContainer, bool paused) const {
	FMOD_RESULT result = FMOD_OK;
	FMOD::Channel* soundChannel;

	const std::unordered_set<int>* playingChannels = soundContainer->GetPlayingChannels();
	for (int channelIndex: *playingChannels) {
		if (!VoiceMatchesContext(channelIndex, soundContainer)) continue;
		result = GetVoiceChannel(channelIndex, &soundChannel);
		result = (result == FMOD_OK) ? soundChannel->setPaused(paused) : result;
		if (result != FMOD_OK) {
			g_ConsoleMan.PrintString("ERROR: Could not set pausedness for SoundContainer " + soundContainer->GetPresetName() + ": " + std::string(FMOD_ErrorString(result)));
		}
	}
}

void AudioMan::Update3DEffectsForSFXChannels() {
	int numberOfPlayingChannels;
	FMOD::Channel* soundChannel;

	FMOD_RESULT result = m_SFXChannelGroup->getNumChannels(&numberOfPlayingChannels);
	if (result != FMOD_OK) {
		g_ConsoleMan.PrintString("ERROR: Failed to get the number of playing channels when updating calculated sound effects for all playing channels: " + std::string(FMOD_ErrorString(result)));
		return;
	}

	for (int i = 0; i < numberOfPlayingChannels; i++) {
		result = m_SFXChannelGroup->getChannel(i, &soundChannel);
		FMOD_MODE mode;
		result = (result == FMOD_OK) ? soundChannel->getMode(&mode) : result;
		if (result == FMOD_OK && (mode & FMOD_2D) == 0) {
			FMOD_VECTOR channelPosition;
			result = result == FMOD_OK ? soundChannel->get3DAttributes(&channelPosition, nullptr) : result;
			result = result == FMOD_OK ? UpdatePositionalEffectsForSoundChannel(soundChannel, &channelPosition) : result;
			float channel3dLevel;
			result = (result == FMOD_OK) ? soundChannel->get3DLevel(&channel3dLevel) : result;
			if (result == FMOD_OK && m_CurrentActivityHumanPlayerPositions.size() == 1) {
				float sqrDistanceToPlayer = (*(m_CurrentActivityHumanPlayerPositions[0].get()) - GetAsVector(channelPosition)).GetSqrMagnitude();
				float doubleMinimumDistanceForPanning = m_MinimumDistanceForPanning * 2.0F;
				void* userData;
				result = result == FMOD_OK ? soundChannel->getUserData(&userData) : result;
				if (result == FMOD_OK && userData != nullptr) {
					const SoundContainer* soundContainer = static_cast<SoundContainer*>(userData);
					if (sqrDistanceToPlayer < (m_MinimumDistanceForPanning * m_MinimumDistanceForPanning) || soundContainer->GetCustomPanValue() != 0.0f) {
						result = soundChannel->set3DLevel(0);
					} else if (sqrDistanceToPlayer < (doubleMinimumDistanceForPanning * doubleMinimumDistanceForPanning)) {
						result = soundChannel->set3DLevel(Lerp(0.0f, m_SoundPanningEffectStrength * soundContainer->GetPanningStrengthMultiplier(), channel3dLevel));
					} else {
						result = soundChannel->set3DLevel(m_SoundPanningEffectStrength * soundContainer->GetPanningStrengthMultiplier());
					}
				}
			}
		}

		if (result != FMOD_OK) {
			g_ConsoleMan.PrintString("ERROR: An error occurred updating calculated sound effects for playing channel with index " + std::to_string(i) + ": " + std::string(FMOD_ErrorString(result)));
			continue;
		}
	}
}

FMOD_RESULT AudioMan::UpdatePositionalEffectsForSoundChannel(FMOD::Channel* soundChannel, const FMOD_VECTOR* positionOverride) const {
	void* userData;
	FMOD_RESULT result = soundChannel->getUserData(&userData);

	if (result != FMOD_OK) {
		return result;
	}

	const SoundContainer* channelSoundContainer = static_cast<SoundContainer*>(userData);
	if (channelSoundContainer == nullptr) {
		return FMOD_OK; // The owning SoundContainer was destroyed; leave the channel playing where it is.
	}

	bool sceneWraps = g_SceneMan.SceneWrapsX();

	FMOD_VECTOR channelPosition;
	if (positionOverride) {
		channelPosition = *positionOverride;
	} else if (sceneWraps) {
		// NOTE If the scene doesn't wrap and the position hasn't changed, this method doesn't set the channel position below, so there's no need to get it here.
		result = soundChannel->get3DAttributes(&channelPosition, nullptr);
		if (result != FMOD_OK) {
			return result;
		}
	}

	float halfSceneWidth = static_cast<float>(g_SceneMan.GetSceneWidth()) / 2.0F;
	std::array<FMOD_VECTOR, 2> wrappedChannelPositions;
	if (!sceneWraps) {
		wrappedChannelPositions = {channelPosition};
	} else {
		if (channelPosition.x <= halfSceneWidth) {
			wrappedChannelPositions = {channelPosition, {channelPosition.x + g_SceneMan.GetSceneWidth(), channelPosition.y, 0.0}};
		} else {
			wrappedChannelPositions = {FMOD_VECTOR({channelPosition.x - g_SceneMan.GetSceneWidth(), channelPosition.y, 0.0}), channelPosition};
		}
	}

	float sqrShortestDistance = c_SoundMaxAudibleDistance * c_SoundMaxAudibleDistance;
	float sqrLongestDistance = 0.0F;
	for (const std::unique_ptr<const Vector>& humanPlayerPosition: m_CurrentActivityHumanPlayerPositions) {
		for (const FMOD_VECTOR& wrappedChannelPosition: wrappedChannelPositions) {
			float sqrDistanceToChannelPosition = (*(humanPlayerPosition.get()) - GetAsVector(wrappedChannelPosition)).GetSqrMagnitude();
			if (sqrDistanceToChannelPosition < sqrShortestDistance) {
				sqrShortestDistance = sqrDistanceToChannelPosition;
				channelPosition = wrappedChannelPosition;
			}
			if (sqrDistanceToChannelPosition > sqrLongestDistance) {
				sqrLongestDistance = sqrDistanceToChannelPosition;
			}
			if (!sceneWraps) {
				break;
			}
		}
	}
	float shortestDistance = std::sqrt(sqrShortestDistance);

	int soundChannelIndex;
	soundChannelIndex = FindVoiceIdentity(soundChannel);
	if (soundChannelIndex <= 0) result = FMOD_ERR_INVALID_HANDLE;
	if (result != FMOD_OK) {
		return result;
	}

	float attenuationStartDistance = c_DefaultAttenuationStartDistance;
	float soundMaxDistance = 0.0F;
	result = result == FMOD_OK ? soundChannel->get3DMinMaxDistance(&attenuationStartDistance, &soundMaxDistance) : result;

	float attenuatedVolume = (shortestDistance <= attenuationStartDistance) ? 1.0F : attenuationStartDistance / shortestDistance;

	// Lowpass as distance increases
	FMOD::DSP* dsp_multibandeq;
	result = (result == FMOD_OK) ? soundChannel->getDSP(0, &dsp_multibandeq) : result;
	float factor = 1 - pow(1 - attenuatedVolume, 3);
	float lowpassFrequency = 22000.0f * factor;
	lowpassFrequency = std::clamp(lowpassFrequency, 350.0f, 22000.0f);
	result = (result == FMOD_OK) ? dsp_multibandeq->setParameterFloat(1, lowpassFrequency) : result;

	if (channelSoundContainer->GetCustomPanValue() != 0.0f) {
		result = (result == FMOD_OK) ? soundChannel->setPan(channelSoundContainer->GetCustomPanValue()) : result;
	}

	float minimumAudibleDistance = m_SoundChannelMinimumAudibleDistances.at(soundChannelIndex);
	if (shortestDistance >= soundMaxDistance) {
		attenuatedVolume = 0.0F;
	} else if (m_SoundChannelMinimumAudibleDistances.find(soundChannelIndex) == m_SoundChannelMinimumAudibleDistances.end()) {
		g_ConsoleMan.PrintString("ERROR: An error occurred when checking to see if the sound at channel " + std::to_string(soundChannelIndex) + " was less than its minimum audible distance away from the farthest listener.");
	} else if (sqrLongestDistance < (minimumAudibleDistance * minimumAudibleDistance)) {
		attenuatedVolume = 0.0F;
	}

	float panLevel;
	result = result == FMOD_OK ? soundChannel->get3DLevel(&panLevel) : result;
	if (result == FMOD_OK && (panLevel < 1.0F || attenuatedVolume == 0.0F)) {
		result = soundChannel->setVolume(attenuatedVolume * channelSoundContainer->GetVolume());
	}

	result = (result == FMOD_OK && (sceneWraps || positionOverride)) ? soundChannel->set3DAttributes(&channelPosition, nullptr) : result;

	return result;
}

FMOD_RESULT F_CALLBACK AudioMan::SoundChannelEndedCallback(FMOD_CHANNELCONTROL* channelControl, FMOD_CHANNELCONTROL_TYPE channelControlType, FMOD_CHANNELCONTROL_CALLBACK_TYPE callbackType, void*, void*) {
	if (channelControlType == FMOD_CHANNELCONTROL_CHANNEL && callbackType == FMOD_CHANNELCONTROL_CALLBACK_END) {
		FMOD::Channel* channel = reinterpret_cast<FMOD::Channel*>(channelControl);
		const int identity = g_AudioMan.FindVoiceIdentity(channel);
		if (identity > 0) g_AudioMan.RetireVoice(identity);
		channel->setUserData(nullptr);
	}
	return FMOD_OK;
}

FMOD_RESULT AudioMan::SetMusicMuffledState(bool musicMuffledState) {
	FMOD_RESULT status = FMOD_OK;
	if (musicMuffledState != m_MusicMuffled) {
		FMOD::DSP* dsp_multibandeq;
		status = (status == FMOD_OK) ? m_MusicChannelGroup->getDSP(0, &dsp_multibandeq) : status;
		float frequency = 22000.0F;

		if (musicMuffledState) {
			frequency = 1000.0F;
		}

		status = (status == FMOD_OK) ? dsp_multibandeq->setParameterFloat(1, frequency) : status;
		m_MusicMuffled = musicMuffledState;
	}
	return status;
}

FMOD_VECTOR AudioMan::GetAsFMODVector(const Vector& vector, float zValue) const {
	Vector sceneDimensions = g_SceneMan.GetScene() ? g_SceneMan.GetSceneDim() : Vector();
	return sceneDimensions.IsZero() ? FMOD_VECTOR{0, 0, zValue} : FMOD_VECTOR{vector.m_X, sceneDimensions.m_Y - vector.m_Y, zValue};
}

Vector AudioMan::GetAsVector(FMOD_VECTOR fmodVector) const {
	Vector sceneDimensions = g_SceneMan.GetScene() ? g_SceneMan.GetSceneDim() : Vector();
	return sceneDimensions.IsZero() ? Vector() : Vector(fmodVector.x, sceneDimensions.m_Y - fmodVector.y);
}

uint64_t AudioMan::AllocateCheckpointSoundContainerID() {
	if (m_NextSoundContainerIdentity == std::numeric_limits<uint64_t>::max()) throw std::runtime_error("sound identity space exhausted");
	return ++m_NextSoundContainerIdentity;
}

void AudioMan::RegisterCheckpointSoundContainer(SoundContainer* container, uint64_t identity) {
	if (!identity) return;
	m_LiveCheckpointSoundContainers[container] = identity;
	m_NextSoundContainerIdentity = std::max(m_NextSoundContainerIdentity, identity);
	auto& owners = m_CheckpointSoundContainers[identity];
	if (std::find(owners.begin(), owners.end(), container) == owners.end()) owners.push_back(container);
	if (m_RestoredSoundRegistryRecording) {
		auto& restored = m_RestoredSoundContainers[identity];
		if (std::find(restored.begin(), restored.end(), container) == restored.end()) restored.push_back(container);
	}
}

void AudioMan::UnregisterCheckpointSoundContainer(SoundContainer* container, uint64_t identity) {
	m_LiveCheckpointSoundContainers.erase(container);
	auto found = m_CheckpointSoundContainers.find(identity);
	if (found != m_CheckpointSoundContainers.end()) {
		std::erase(found->second, container);
		if (found->second.empty()) m_CheckpointSoundContainers.erase(found);
	}
	if (!m_RestoredSoundRegistryActive) return;
	auto restored = m_RestoredSoundContainers.find(identity);
	if (restored == m_RestoredSoundContainers.end()) return;
	std::erase(restored->second, container);
	if (restored->second.empty()) m_RestoredSoundContainers.erase(restored);
}

SoundContainer* AudioMan::FindCheckpointSoundContainer(uint64_t identity) const {
	const auto found = m_CheckpointSoundContainers.find(identity);
	return found == m_CheckpointSoundContainers.end() || found->second.empty() ? nullptr : found->second.back();
}

CheckpointSoundRegistry AudioMan::AddedCheckpointSoundRegistrations(const CheckpointSoundRegistry& original) const {
	CheckpointSoundRegistry added;
	for (const auto& [identity, owners]: m_CheckpointSoundContainers) {
		const auto previous = original.find(identity);
		for (SoundContainer* owner: owners) {
			if (previous == original.end() || std::find(previous->second.begin(), previous->second.end(), owner) == previous->second.end()) added[identity].push_back(owner);
		}
	}
	return added;
}

void AudioMan::RestoreCheckpointSoundRegistry(CheckpointSoundRegistry original) {
	for (auto entry = original.begin(); entry != original.end();) {
		std::erase_if(entry->second, [this, identity = entry->first](const SoundContainer* owner) {
			const auto live = m_LiveCheckpointSoundContainers.find(owner);
			return live == m_LiveCheckpointSoundContainers.end() || live->second != identity;
		});
		if (entry->second.empty()) entry = original.erase(entry); else ++entry;
	}
	m_CheckpointSoundContainers.swap(original);
}

void AudioMan::ActivateCheckpointSoundRegistrations(const CheckpointSoundRegistry& candidates) {
	for (const auto& [identity, owners]: candidates) for (SoundContainer* owner: owners) {
		const auto live = m_LiveCheckpointSoundContainers.find(owner);
		if (live != m_LiveCheckpointSoundContainers.end() && live->second == identity) RegisterCheckpointSoundContainer(owner, identity);
	}
}

AudioMan::CheckpointRegistryScope::CheckpointRegistryScope() : m_Original(g_AudioMan.CaptureCheckpointSoundRegistry()), m_Cursor(g_AudioMan.GetCheckpointSoundContainerCursor()) {}

AudioMan::CheckpointRegistryScope::~CheckpointRegistryScope() {
	g_AudioMan.RestoreCheckpointSoundRegistry(std::move(m_Original));
	g_AudioMan.SetCheckpointSoundContainerCursor(m_Cursor);
}

thread_local AudioMan::SoundCheckpointSaveScope* AudioMan::SoundCheckpointSaveScope::s_Current = nullptr;

AudioMan::SoundCheckpointSaveScope::SoundCheckpointSaveScope() : m_Previous(s_Current) {
	s_Current = this;
}

AudioMan::SoundCheckpointSaveScope::~SoundCheckpointSaveScope() {
	g_AudioMan.RememberCarriedSoundIdentities(m_Carried);
	s_Current = m_Previous;
}

void AudioMan::SoundCheckpointSaveScope::Note(uint64_t identity) {
	if (identity) m_Carried.insert(identity);
}

void AudioMan::NoteCarriedSoundIdentity(uint64_t identity) {
	if (auto* scope = SoundCheckpointSaveScope::Current()) scope->Note(identity);
}

void AudioMan::RememberCarriedSoundIdentities(std::unordered_set<uint64_t> carried) {
	m_LastCarriedSoundIdentities = std::move(carried);
}

namespace {
	void NoteSoundContainerIdentity(const std::string& native, std::unordered_set<uint64_t>& out) {
		if (native.empty()) return;
		try {
			CheckpointReader reader(native, SoundContainer::CheckpointVersion(native));
			std::string entity;
			uint64_t identity = 0;
			reader.Value(entity);
			reader.Value(identity);
			if (identity) out.insert(identity);
		} catch (const std::exception&) {}
	}

	void NoteMusicSound(const std::string& text, std::unordered_set<uint64_t>& out) {
		if (text.empty()) return;
		CheckpointReader reader(text, "MusicSound1");
		std::string native;
		reader.Value(native);
		NoteSoundContainerIdentity(native, out);
	}

	void NoteMusicSection(const std::string& text, std::unordered_set<uint64_t>& out) {
		if (text.empty()) return;
		CheckpointReader reader(text, "MusicSection1");
		std::string entity;
		std::vector<std::string> transitions, sounds;
		unsigned int lastTransition = 0, lastSound = 0;
		std::vector<unsigned int> transitionQueue, queue;
		int cycle = 0;
		std::string type;
		reader.Value(entity);
		reader.Value(transitions);
		reader.Value(lastTransition);
		reader.Value(transitionQueue);
		reader.Value(sounds);
		reader.Value(lastSound);
		reader.Value(queue);
		reader.Value(cycle);
		reader.Value(type);
		for (const std::string& sound: transitions) NoteMusicSound(sound, out);
		for (const std::string& sound: sounds) NoteMusicSound(sound, out);
	}

	void NoteMusicSong(const std::string& text, std::unordered_set<uint64_t>& out) {
		if (text.empty()) return;
		CheckpointReader reader(text, "MusicSong1");
		std::string entity, fallback;
		std::vector<std::string> sections;
		reader.Value(entity);
		reader.Value(fallback);
		reader.Value(sections);
		NoteMusicSection(fallback, out);
		for (const std::string& section: sections) NoteMusicSection(section, out);
	}
}

void AudioMan::CollectManagerSoundIdentities(std::unordered_set<uint64_t>& out) const {
	g_GUISound.VisitCheckpointSounds([&out](size_t, const SoundContainer& sound) {
		if (const uint64_t identity = sound.GetCheckpointIdentity()) out.insert(identity);
	});
	try {
		CheckpointReader reader(g_MusicMan.SaveCheckpoint(), "MusicMan1");
		bool playing = false;
		std::string interrupting, song, nextType, currentType, previous, current;
		int nextSection = 0;
		std::array<int, 3> nextSound{};
		Timer fadeTimer, timer;
		bool fadePrevious = false, returnToDynamic = false;
		double pausedTime = 0;
		reader.Value(playing);
		reader.Value(interrupting);
		reader.Value(song);
		reader.Value(nextType);
		reader.Value(currentType);
		reader.Value(nextSection);
		reader.Value(previous);
		reader.Value(current);
		reader.Value(nextSound);
		reader.Value(fadeTimer);
		reader.Value(fadePrevious);
		reader.Value(timer);
		reader.Value(pausedTime);
		reader.Value(returnToDynamic);
		NoteMusicSound(interrupting, out);
		NoteMusicSong(song, out);
		NoteMusicSound(previous, out);
		NoteMusicSound(current, out);
	} catch (const std::exception&) {}
}

AudioMan::RestoredSoundRegistryScope::RestoredSoundRegistryScope() {
	g_AudioMan.m_RestoredSoundContainers.clear();
	g_AudioMan.m_RestoredManagerIdentities.clear();
	g_AudioMan.m_RestoredSoundRegistryActive = true;
	g_AudioMan.m_RestoredSoundRegistryRecording = true;
}

AudioMan::RestoredSoundRegistryScope::~RestoredSoundRegistryScope() {
	g_AudioMan.m_RestoredSoundRegistryRecording = false;
	g_AudioMan.m_RestoredSoundRegistryActive = false;
	g_AudioMan.m_RestoredSoundContainers.clear();
	g_AudioMan.m_RestoredManagerIdentities.clear();
}

void AudioMan::StopRecordingRestoredSoundRegistry() {
	m_RestoredSoundRegistryRecording = false;
}

SoundContainer* AudioMan::FindRestoredCheckpointSoundContainer(uint64_t identity) const {
	const auto found = m_RestoredSoundContainers.find(identity);
	return found == m_RestoredSoundContainers.end() || found->second.empty() ? nullptr : found->second.back();
}

void AudioMan::RefreshRestoredManagerIdentities() {
	m_RestoredManagerIdentities.clear();
	CollectManagerSoundIdentities(m_RestoredManagerIdentities);
}

SoundContainer* AudioMan::ResolveCheckpointVoiceOwner(uint64_t identity) const {
	if (!identity) return nullptr;
	if (!m_RestoredSoundRegistryActive) return FindCheckpointSoundContainer(identity);
	if (SoundContainer* owner = FindRestoredCheckpointSoundContainer(identity)) return owner;
	if (m_RestoredManagerIdentities.contains(identity)) return FindCheckpointSoundContainer(identity);
	return nullptr;
}

int AudioMan::RegisterPlayingVoice(FMOD::Channel* channel, SoundContainer* owner, const std::string& path, float minimumAudibleDistance) {
	do {
		if (m_NextVoiceIdentity == std::numeric_limits<int>::max()) m_NextVoiceIdentity = 0;
		++m_NextVoiceIdentity;
	} while (m_PlayingVoices.contains(m_NextVoiceIdentity));
	int backend;
	if (channel->getIndex(&backend) != FMOD_OK) throw std::runtime_error("could not identify playing audio channel");
	m_BackendVoiceIdentities[backend] = m_NextVoiceIdentity;
	m_PlayingVoices.emplace(m_NextVoiceIdentity, PlayingVoice{channel, owner, path, minimumAudibleDistance, SoundSimulationScope::Domain()});
	return m_NextVoiceIdentity;
}

int AudioMan::FindVoiceIdentity(const FMOD::Channel* channel) const {
	int backend;
	if (!channel || const_cast<FMOD::Channel*>(channel)->getIndex(&backend) != FMOD_OK) return 0;
	const auto found = m_BackendVoiceIdentities.find(backend);
	if (found == m_BackendVoiceIdentities.end()) return 0;
	const auto voice = m_PlayingVoices.find(found->second);
	return voice != m_PlayingVoices.end() && voice->second.channel == channel ? found->second : 0;
}

FMOD_RESULT AudioMan::GetVoiceChannel(int voiceIdentity, FMOD::Channel** channel) const {
	const auto found = m_PlayingVoices.find(voiceIdentity);
	if (found == m_PlayingVoices.end() || !found->second.channel) { *channel = nullptr; return FMOD_ERR_INVALID_HANDLE; }
	*channel = found->second.channel;
	return FMOD_OK;
}

bool AudioMan::OwnsVoice(int voiceIdentity, const SoundContainer* owner) const {
	const auto found = m_PlayingVoices.find(voiceIdentity);
	return found != m_PlayingVoices.end() && found->second.owner == owner;
}

bool AudioMan::VoiceMatchesContext(int identity, const SoundContainer* owner) const {
	const auto found = m_PlayingVoices.find(identity);
	if (found == m_PlayingVoices.end() || found->second.owner != owner) return false;
	// Only a shared simulation call is confined to the shared cohort's voices. An AI hook mutates
	// nothing directly any more, so its reads see every voice this container owns, as they always did.
	return SoundSimulationScope::Domain() != SoundExecutionDomain::SharedSimulation || found->second.domain == SoundExecutionDomain::SharedSimulation;
}

void AudioMan::NotePendingSoundOps(SoundContainer* container) {
	if (!container->GetCheckpointIdentity()) return;
	std::lock_guard lock(m_PendingSoundOpsMutex);
	m_PendingSoundOpContainers[container->GetCheckpointIdentity()] = container;
}

void AudioMan::ClearPendingSoundOps(SoundContainer* container) {
	std::lock_guard lock(m_PendingSoundOpsMutex);
	std::erase_if(m_PendingSoundOpContainers, [container](const auto& entry) { return entry.second == container; });
}

std::vector<SoundContainer*> AudioMan::TakePendingSoundOpContainers() {
	std::lock_guard lock(m_PendingSoundOpsMutex);
	std::vector<SoundContainer*> containers;
	containers.reserve(m_PendingSoundOpContainers.size());
	for (const auto& [identity, container]: m_PendingSoundOpContainers) containers.push_back(container);
	m_PendingSoundOpContainers.clear();
	return containers;
}

void AudioMan::SettleSharedSoundWrites() {
	std::vector<SoundContainer*> containers;
	{
		std::lock_guard lock(m_PendingSoundOpsMutex);
		containers.reserve(m_PendingSoundOpContainers.size());
		for (const auto& [identity, container]: m_PendingSoundOpContainers) containers.push_back(container);
	}
	for (SoundContainer* container: containers) container->SettleSharedWritesBeforeAIPass();
}

uint64_t AudioMan::NextDeferredSoundOpOrdinal() {
	const uint64_t tick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
	if (tick != m_DeferredSoundOpTick) {
		m_DeferredSoundOpTick = tick;
		m_DeferredSoundOpOrdinal = 0;
	}
	return ++m_DeferredSoundOpOrdinal;
}

void AudioMan::RefreshLogicalSound(SoundContainer* container) {
	std::lock_guard lock(m_LogicalSoundsMutex);
	if (container->m_CheckpointRegistered && !container->m_LogicalPlayback.voices.empty()) m_ActiveLogicalSounds.insert(container);
	else m_ActiveLogicalSounds.erase(container);
}

void AudioMan::UnregisterLogicalSound(SoundContainer* container) {
	std::lock_guard lock(m_LogicalSoundsMutex);
	m_ActiveLogicalSounds.erase(container);
}

void AudioMan::RetireFinishedSimulationSounds() {
	std::lock_guard lock(m_LogicalSoundsMutex);
	const long long now = g_TimerMan.GetSimTimeTicks();
	const long long ticksPerSecond = g_TimerMan.GetTicksPerSecond();
	for (auto it = m_ActiveLogicalSounds.begin(); it != m_ActiveLogicalSounds.end();) {
		SoundContainer* container = *it;
		if (FindCheckpointSoundContainer(container->GetCheckpointIdentity()) != container) { ++it; continue; }
		std::erase_if(container->m_LogicalPlayback.voices, [&](const LogicalSoundVoice& voice) { return voice.At(now, ticksPerSecond).finished; });
		if (container->m_LogicalPlayback.voices.empty()) it = m_ActiveLogicalSounds.erase(it);
		else ++it;
	}
}

void AudioMan::VisitSharedSimulationSounds(const std::function<void(const SoundContainer&)>& visitor) const {
	std::lock_guard lock(m_LogicalSoundsMutex);
	const long long now = g_TimerMan.GetSimTimeTicks();
	const long long ticksPerSecond = g_TimerMan.GetTicksPerSecond();
	for (const SoundContainer* container: m_ActiveLogicalSounds) {
		if (FindCheckpointSoundContainer(container->GetCheckpointIdentity()) != container) continue;
		for (const LogicalSoundVoice& voice: container->m_LogicalPlayback.voices) {
			if (!voice.At(now, ticksPerSecond).finished) { visitor(*container); break; }
		}
	}
}

float AudioMan::GetLocalSoundAudibility(const SoundContainer* container) const {
	if (!m_AudioEnabled || !container) return 0;
	// Keep the existing first-channel aggregation. Cohort filtering keeps an AI
	// sound on the same native container out of shared input observations.
	for (int identity: container->m_PlayingChannels) {
		const auto found = m_PlayingVoices.find(identity);
		if (found == m_PlayingVoices.end() || found->second.owner != container || found->second.domain != SoundExecutionDomain::SharedSimulation || !found->second.channel) continue;
		float value;
		if (found->second.channel->getAudibility(&value) == FMOD_OK) return value;
	}
	return 0;
}

void AudioMan::StopAll() {
	if (SoundSimulationScope::Domain() == SoundExecutionDomain::LocalSimulation) {
		// From an AI hook this is a decision about shared playback, so it becomes one deferred Stop
		// per live container, in identity order, keeping the order a later call on any of them made.
		std::vector<SoundContainer*> live;
		{
			std::lock_guard lock(m_LogicalSoundsMutex);
			live.assign(m_ActiveLogicalSounds.begin(), m_ActiveLogicalSounds.end());
		}
		std::sort(live.begin(), live.end(), [](const SoundContainer* first, const SoundContainer* second) {
			return first->GetCheckpointIdentity() < second->GetCheckpointIdentity();
		});
		for (SoundContainer* container: live) container->Stop();
		// The physical stop is presentation on this machine, exactly as it always was.
		if (m_AudioEnabled && !s_PlaybackSuppressed) m_MasterChannelGroup->stop();
		return;
	}
	{
		std::lock_guard lock(m_LogicalSoundsMutex);
		for (SoundContainer* container: m_ActiveLogicalSounds) container->m_LogicalPlayback.voices.clear();
		m_ActiveLogicalSounds.clear();
	}
	if (m_AudioEnabled && !s_PlaybackSuppressed) m_MasterChannelGroup->stop();
}

void AudioMan::PauseIngameSounds(bool pause) {
	if (m_AudioEnabled && !s_PlaybackSuppressed) m_SFXChannelGroup->setPaused(pause);
}

void AudioMan::RetireVoice(int identity) {
	const auto found = m_PlayingVoices.find(identity);
	if (found == m_PlayingVoices.end()) return;
	if (found->second.owner) found->second.owner->RemovePlayingChannel(identity);
	int backend;
	if (found->second.channel && found->second.channel->getIndex(&backend) == FMOD_OK) {
		const auto reverse = m_BackendVoiceIdentities.find(backend);
		if (reverse != m_BackendVoiceIdentities.end() && reverse->second == identity) m_BackendVoiceIdentities.erase(reverse);
	}
	m_PlayingVoices.erase(found);
	m_SoundChannelMinimumAudibleDistances.erase(identity);
}

bool AudioMan::MakeVoiceSlotAvailable() {
	if (m_PlayingVoices.size() < c_MaxVirtualChannels) return true;
	// Keep the existing live limit while reserving backend capacity for a replacement world.
	// FMOD steals lower-priority voices first, then the least audible at equal priority.
	int victim = 0, worstPriority = -1;
	float quietest = std::numeric_limits<float>::infinity();
	for (const auto& [identity, voice]: m_PlayingVoices) {
		if (!voice.channel) { victim = identity; break; }
		int priority; float audibility;
		if (voice.channel->getPriority(&priority) != FMOD_OK || voice.channel->getAudibility(&audibility) != FMOD_OK) { victim = identity; break; }
		if (priority > worstPriority || (priority == worstPriority && audibility < quietest)) { victim = identity; worstPriority = priority; quietest = audibility; }
	}
	if (!victim) return false;
	FMOD::Channel* channel = m_PlayingVoices.at(victim).channel;
	if (channel) channel->stop();
	RetireVoice(victim);
	return true;
}
namespace {
	bool s_TraceAudioCheckpoints = false;

	class PreservedGroupEffects {
	public:
		explicit PreservedGroupEffects(const std::array<FMOD::ChannelGroup*, 4>& groups) : m_Groups(groups) {}
		void Detach() {
			for (auto* group: m_Groups) {
				int count; AudioCheckpoint::Require(group->getNumDSPs(&count));
				for (int index = 0; index < count; ++index) {
					FMOD::DSP* dsp; FMOD_DSP_TYPE type;
					AudioCheckpoint::Require(group->getDSP(index, &dsp)); AudioCheckpoint::Require(dsp->getType(&type));
					if (AudioCheckpoint::Effect::Managed(type)) {
						bool active, bypass;
						AudioCheckpoint::Require(dsp->getActive(&active)); AudioCheckpoint::Require(dsp->getBypass(&bypass));
						m_Original.push_back({group, index, dsp, active, bypass});
					}
				}
			}
			m_Started = true;
			for (auto item = m_Original.rbegin(); item != m_Original.rend(); ++item) AudioCheckpoint::Require(item->group->removeDSP(item->dsp));
		}
		void Commit() { m_Committed = true; }
		~PreservedGroupEffects() {
			if (!m_Started) return;
			if (m_Committed) { for (const auto& original: m_Original) original.dsp->release(); return; }
			// Preserve original DSP instances on rejection, including their private filter history.
			for (auto* group: m_Groups) {
				int count = 0; group->getNumDSPs(&count);
				for (int index = count - 1; index >= 0; --index) {
					FMOD::DSP* dsp = nullptr; FMOD_DSP_TYPE type = FMOD_DSP_TYPE_UNKNOWN;
					if (group->getDSP(index, &dsp) != FMOD_OK || dsp->getType(&type) != FMOD_OK || !AudioCheckpoint::Effect::Managed(type)) continue;
					group->removeDSP(dsp);
					if (std::none_of(m_Original.begin(), m_Original.end(), [dsp](const Original& item) { return item.dsp == dsp; })) dsp->release();
				}
			}
			for (const auto& original: m_Original) {
				original.group->addDSP(original.index, original.dsp);
				original.dsp->setActive(original.active); original.dsp->setBypass(original.bypass);
			}
		}
	private:
		struct Original { FMOD::ChannelGroup* group; int index; FMOD::DSP* dsp; bool active, bypass; };
		std::array<FMOD::ChannelGroup*, 4> m_Groups;
		std::vector<Original> m_Original;
		bool m_Started = false, m_Committed = false;
	};

	struct CheckpointSoundEvent {
		AudioMan::NetworkSoundData data{};
		template <class Archive> void Fields(Archive& archive) {
			archive(data.State, data.SoundFileHash, data.Channel, data.Immobile, data.AttenuationStartDistance, data.CustomPanValue, data.PanningStrengthMultiplier, data.Loops, data.Priority, data.AffectedByGlobalPitch, data.Position, data.Volume, data.Pitch, data.FadeOutTime);
		}
		std::string SaveCheckpoint() const { CheckpointWriter archive("AudioEvent1"); const_cast<CheckpointSoundEvent*>(this)->Fields(archive); return archive.Text(); }
		bool LoadCheckpoint(std::string_view text, bool validateOnly = false) { try { CheckpointSoundEvent value; CheckpointReader archive(text, "AudioEvent1"); value.Fields(archive); archive.Finish(); if (!validateOnly) *this = value; return true; } catch (const std::exception&) { return false; } }
	};

	struct CommittedAudibilityRecord {
		SoundObservationKey key;
		uint8_t peer = 0;
		uint64_t frame = 0;
		float value = 0.0F;
		template <class Archive> void Fields(Archive& archive) { archive(key.objectUID, key.tick, key.phase, key.occurrence, key.ordinal, peer, frame, value); }
		std::string SaveCheckpoint() const { CheckpointWriter writer("CommittedAudibility1"); const_cast<CommittedAudibilityRecord*>(this)->Fields(writer); return writer.Text(); }
		bool LoadCheckpoint(std::string_view text, bool validateOnly = false) {
			try {
				CommittedAudibilityRecord candidate;
				CheckpointReader reader(text, "CommittedAudibility1");
				candidate.Fields(reader);
				reader.Finish();
				if (candidate.peer == 0 || candidate.peer > NetLockstepCodec::c_MaxPeerCount || !std::isfinite(candidate.value)) return false;
				if (!validateOnly) *this = candidate;
				return true;
			} catch (const std::exception&) { return false; }
		}
	};

	struct AudioRuntime {
		bool enabled = false;
		int nextVoice = 0;
		uint64_t nextSoundContainer = 0;
		bool muteMaster = false, muteMusic = false, muteSounds = false, muteOnFocusLoss = false;
		float masterVolume = 0, musicVolume = 0, soundsVolume = 0, globalPitch = 0, panning = 0, listenerZ = 0, minimumPanning = 0;
		bool musicMuffled = false, multiplayer = false;
		std::vector<Vector> playerPositions;
		std::vector<std::array<AudioCheckpoint::Position, 4>> listeners;
		std::array<AudioCheckpoint::Control, 4> groups;
		std::vector<AudioCheckpoint::Sample> samples;
		std::vector<AudioCheckpoint::Voice> voices;
		std::map<int, float> minimumDistances;
		std::array<std::vector<CheckpointSoundEvent>, c_MaxClients> events;
		std::vector<CommittedAudibilityRecord> audibility;
		// The deferred call's number inside its tick derives the archived playback key, so a restored
		// or re-simulated tick has to continue from the same place.
		uint64_t deferredSoundOpTick = 0;
		uint64_t deferredSoundOpOrdinal = 0;
		template <class Archive> void Fields(Archive& archive) {
			archive(enabled, nextVoice, nextSoundContainer, muteMaster, muteMusic, muteSounds, muteOnFocusLoss, masterVolume, musicVolume, soundsVolume, globalPitch, panning, listenerZ, minimumPanning, musicMuffled, multiplayer, playerPositions, listeners, groups, samples, voices, minimumDistances, events);
		}
		std::string Save() { CheckpointWriter archive("AudioRuntime3"); Fields(archive); archive(audibility, deferredSoundOpTick, deferredSoundOpOrdinal); return archive.Text(); }
		// Every refusal names itself, so a rejected archive says what was wrong with it.
		bool Load(std::string_view text, std::string* refusal = nullptr) {
			const auto refuse = [refusal](std::string reason) { if (refusal) *refusal = std::move(reason); return false; };
			try {
				const std::string_view version = AudioMan::CheckpointVersion(text);
				CheckpointReader archive(text, version); Fields(archive);
				if (version == "AudioRuntime3") archive(audibility, deferredSoundOpTick, deferredSoundOpOrdinal);
				else if (version == "AudioRuntime2") archive(audibility);
				archive.Finish();
				std::set<std::pair<SoundObservationKey, uint8_t>> readings;
				for (const auto& record: audibility) if (!readings.insert({record.key, record.peer}).second) return refuse("duplicate committed audibility for object " + std::to_string(record.key.objectUID) + " tick " + std::to_string(record.key.tick) + " peer " + std::to_string(record.peer));
				if (nextVoice < 0) return refuse("negative next voice identity " + std::to_string(nextVoice));
				if (voices.size() > c_MaxVirtualChannels) return refuse(std::to_string(voices.size()) + " voices exceed the " + std::to_string(c_MaxVirtualChannels) + " virtual channels");
				if (listeners.size() > 8) return refuse(std::to_string(listeners.size()) + " listeners");
				if (enabled && listeners.empty()) return refuse("audio is enabled but the checkpoint has no listener");
				for (const auto& [name, value]: {std::pair<const char*, float>{"master volume", masterVolume}, {"music volume", musicVolume}, {"sounds volume", soundsVolume}, {"global pitch", globalPitch}, {"panning", panning}, {"listener Z", listenerZ}, {"minimum panning", minimumPanning}})
					if (!AudioCheckpoint::Finite(value)) return refuse(std::string(name) + " is not finite");
				std::set<int> voiceIDs;
				for (const auto& voice: voices) {
					if (!voiceIDs.insert(voice.identity).second) return refuse("duplicate voice identity " + std::to_string(voice.identity));
					if (voice.owner > nextSoundContainer) return refuse("voice " + std::to_string(voice.identity) + " has owner " + std::to_string(voice.owner) + " past the sound container cursor " + std::to_string(nextSoundContainer));
				}
				for (const auto& [identity, distance]: minimumDistances) {
					if (!voiceIDs.contains(identity)) return refuse("minimum audible distance names unknown voice " + std::to_string(identity));
					if (!AudioCheckpoint::Finite(distance)) return refuse("minimum audible distance of voice " + std::to_string(identity) + " is not finite");
				}
				std::set<std::string> paths;
				for (const auto& sample: samples) if (!paths.insert(sample.path).second) return refuse("duplicate sample " + sample.path);
				for (const auto& voice: voices) if (voice.playing && !paths.contains(voice.path)) return refuse("playing voice " + std::to_string(voice.identity) + " has no captured sample for " + voice.path);
				return true;
			} catch (const std::exception& error) { return refuse(error.what()); }
		}
	};

	uint64_t ArchivedVoiceOwner(const std::string& text, int voiceIdentity) {
		AudioRuntime state;
		std::string refusal;
		if (!state.Load(text, &refusal)) throw std::runtime_error("could not parse audio archive: " + refusal);
		for (const AudioCheckpoint::Voice& voice: state.voices) {
			if (voice.identity == voiceIdentity) return voice.owner;
		}
		throw std::runtime_error("voice " + std::to_string(voiceIdentity) + " is absent from the audio archive");
	}

	void CheckCarriedAudioOwners(const std::string& text, const std::unordered_set<uint64_t>& carried) {
		std::unordered_set<uint64_t> managers;
		g_AudioMan.CollectManagerSoundIdentities(managers);
		AudioRuntime state;
		std::string refusal;
		if (!state.Load(text, &refusal)) throw std::runtime_error("could not parse audio archive: " + refusal);
		for (const AudioCheckpoint::Voice& voice: state.voices) {
			if (voice.owner && !carried.contains(voice.owner) && !managers.contains(voice.owner)) {
				throw std::runtime_error("voice " + std::to_string(voice.identity) + " owner " + std::to_string(voice.owner) + " is not in the carried set");
			}
		}
	}

	std::string ContainedAudioSave(const AudioMan::SoundCheckpointSaveScope& scope) {
		std::unordered_set<uint64_t> managers;
		g_AudioMan.CollectManagerSoundIdentities(managers);
		return g_AudioMan.SaveCheckpoint([&scope, &managers](uint64_t identity, const SoundContainer*) {
			return !identity || scope.Contains(identity) || managers.contains(identity);
		});
	}

	void WriteSnapshotObject(const SceneObject* object) {
		auto stream = std::make_unique<std::stringstream>();
		Writer writer(std::move(stream));
		Writer::SnapshotScope snapshot(writer);
		writer.NewProperty("ScriptEntity");
		Scene::SaveSceneObject(writer, object, false, true);
	}
}

std::string AudioMan::SaveCheckpoint() const {
	return SaveCheckpoint(std::function<bool(uint64_t, const SoundContainer*)>{});
}

std::string AudioMan::SaveCheckpoint(const std::function<bool(uint64_t, const SoundContainer*)>& contained) const {
	AudioRuntime state;
	state.enabled = m_AudioEnabled; state.nextVoice = m_NextVoiceIdentity;
	state.nextSoundContainer = m_NextSoundContainerIdentity;
	state.deferredSoundOpTick = m_DeferredSoundOpTick; state.deferredSoundOpOrdinal = m_DeferredSoundOpOrdinal;
	state.muteMaster = m_MuteMaster; state.muteMusic = m_MuteMusic; state.muteSounds = m_MuteSounds; state.muteOnFocusLoss = m_MuteAudioOnFocusLoss;
	state.masterVolume = m_MasterVolume; state.musicVolume = m_MusicVolume; state.soundsVolume = m_SoundsVolume; state.globalPitch = m_GlobalPitch;
	state.panning = m_SoundPanningEffectStrength; state.listenerZ = m_ListenerZOffset; state.minimumPanning = m_MinimumDistanceForPanning;
	state.musicMuffled = m_MusicMuffled; state.multiplayer = m_IsInMultiplayerMode;
	for (const auto& [key, readings]: m_CommittedAudibility) {
		for (const auto& [peer, reading]: readings) state.audibility.push_back({key, peer, reading.frame, reading.value});
	}
	for (const auto& position: m_CurrentActivityHumanPlayerPositions) if (position) state.playerPositions.push_back(*position);
	for (int player = 0; player < c_MaxClients; ++player) {
		std::lock_guard lock(const_cast<AudioMan*>(this)->g_SoundEventsListMutex[player]);
		for (const NetworkSoundData& event: m_SoundEvents[player]) state.events[player].push_back({event});
	}
	if (m_AudioEnabled) {
		AudioCheckpoint::MixerLock mixer(m_AudioSystem);
		state.minimumDistances.insert(m_SoundChannelMinimumAudibleDistances.begin(), m_SoundChannelMinimumAudibleDistances.end());
		int listeners; AudioCheckpoint::Require(m_AudioSystem->get3DNumListeners(&listeners));
		for (int index = 0; index < listeners; ++index) {
			FMOD_VECTOR position, velocity, forward, up;
			AudioCheckpoint::Require(m_AudioSystem->get3DListenerAttributes(index, &position, &velocity, &forward, &up));
			state.listeners.push_back({AudioCheckpoint::Pack(position), AudioCheckpoint::Pack(velocity), AudioCheckpoint::Pack(forward), AudioCheckpoint::Pack(up)});
		}
		const std::array<FMOD::ChannelGroup*, 4> groups = {m_MasterChannelGroup, m_SFXChannelGroup, m_UIChannelGroup, m_MusicChannelGroup};
		for (size_t index = 0; index < groups.size(); ++index) state.groups[index] = AudioCheckpoint::Control::Capture(groups[index], false);
		std::map<std::string, FMOD::Sound*> samples(ContentFile::s_LoadedSamples.begin(), ContentFile::s_LoadedSamples.end());
		for (const auto& [path, sound]: samples) {
			if (!sound) continue;
			FMOD_OPENSTATE open;
			if (sound->getOpenState(&open, nullptr, nullptr, nullptr) != FMOD_OK || (open != FMOD_OPENSTATE_READY && open != FMOD_OPENSTATE_PLAYING)) continue;
			state.samples.push_back(AudioCheckpoint::Sample::Capture(path, sound));
		}
		std::set<std::string> disownedPresets;
		for (const auto& [identity, voice]: m_PlayingVoices) {
			int bus = 0;
			FMOD::ChannelGroup* group = nullptr;
			if (voice.channel && voice.channel->getChannelGroup(&group) == FMOD_OK) bus = group == m_UIChannelGroup ? 1 : group == m_MusicChannelGroup ? 2 : 0;
			uint64_t ownerIdentity = voice.owner ? voice.owner->GetCheckpointIdentity() : 0;
			if (contained && ownerIdentity && !contained(ownerIdentity, voice.owner)) {
				if (disownedPresets.insert(voice.owner->GetPresetName()).second) {
					std::cout << "[audio-checkpoint] disowned voice owner " << voice.owner->GetPresetName() << std::endl;
				}
				ownerIdentity = 0;
			}
			state.voices.push_back(AudioCheckpoint::Voice::Capture(identity, ownerIdentity, voice.soundPath, voice.minimumAudibleDistance, voice.channel, bus));
		}
		TraceCheckpointBoundary("save-captured");
	}
	return state.Save();
}

bool AudioMan::LoadCheckpoint(std::string_view text, bool validateOnly, const std::vector<std::pair<SoundData*, std::string>>* sampleBindings, std::string* refusal) {
	AudioRuntime state;
	if (!state.Load(text, refusal)) return false;
	if (validateOnly) return true;
	try {
		if (!m_AudioEnabled && !state.voices.empty()) throw std::runtime_error("checkpoint contains voices but the audio system is disabled");
		std::map<std::string, FMOD::Sound*> sounds;
		std::unordered_map<std::string, FMOD::Sound*> newSamples;
		struct SampleCleanup {
			std::unordered_map<std::string, FMOD::Sound*>& samples;
			~SampleCleanup() { for (const auto& [path, sound]: samples) if (sound) sound->release(); }
		} sampleCleanup{newSamples};
		if (m_AudioEnabled) {
			for (const auto& sample: state.samples) {
				const auto cached = ContentFile::s_LoadedSamples.find(sample.path);
				FMOD::Sound* sound = cached == ContentFile::s_LoadedSamples.end() ? nullptr : cached->second;
				if (!sound) {
					// New assets remain private until commit. A failed restore must neither
					// replace a cache entry nor retain a partially loaded candidate sample.
					AudioCheckpoint::Require(m_AudioSystem->createSound(sample.path.c_str(), FMOD_CREATESAMPLE | FMOD_3D, nullptr, &sound));
					try { newSamples.emplace(sample.path, sound); } catch (...) { sound->release(); throw; }
				}
				FMOD_OPENSTATE open;
				AudioCheckpoint::Require(sound->getOpenState(&open, nullptr, nullptr, nullptr));
				if (open != FMOD_OPENSTATE_READY && open != FMOD_OPENSTATE_PLAYING) throw std::runtime_error("sample is not ready: " + sample.path + ", state=" + std::to_string(open));
				sounds.emplace(sample.path, sound);
			}
			ContentFile::s_LoadedSamples.reserve(ContentFile::s_LoadedSamples.size() + newSamples.size());
		}
		std::map<SoundData*, FMOD::Sound*> stagedSamples;
		if (sampleBindings) for (const auto& [data, path]: *sampleBindings) {
			if (!data || !sounds.contains(path)) throw std::runtime_error("music sample is absent from audio checkpoint: " + path);
			stagedSamples.emplace(data, sounds.at(path));
		}
		std::map<int, PlayingVoice> candidates;
		std::map<SoundContainer*, std::unordered_set<int>> ownerChannels;
		for (const auto& [identity, voice]: m_PlayingVoices) if (voice.owner) ownerChannels.try_emplace(voice.owner);
		std::map<int, const AudioCheckpoint::Voice*> descriptions;
		std::map<int, FMOD::Channel*> backendCandidates;
		if (m_RestoredSoundRegistryActive) RefreshRestoredManagerIdentities();
		for (const auto& voice: state.voices) {
			SoundContainer* owner = voice.owner ? ResolveCheckpointVoiceOwner(voice.owner) : nullptr;
			if (voice.owner && !owner) {
				SoundContainer* named = FindCheckpointSoundContainer(voice.owner);
				const std::string preset = named ? named->GetPresetName() : std::string();
				throw std::runtime_error("voice " + std::to_string(voice.identity) + " has no registered owner " + std::to_string(voice.owner) + (preset.empty() ? "" : " (" + preset + ")"));
			}
			if (owner) ownerChannels[owner].insert(voice.identity);
			if (voice.playing && !sounds.contains(voice.path)) throw std::runtime_error("voice sample is absent: " + voice.path);
			if (voice.playing && owner && !owner->GetSoundDataForSound(sounds.at(voice.path))) {
				std::vector<SoundData*> data; owner->GetTopLevelSoundSet().GetFlattenedSoundData(data, false);
				if (std::none_of(data.begin(), data.end(), [&](SoundData* value) { return stagedSamples.contains(value) && stagedSamples.at(value) == sounds.at(voice.path); }))
					throw std::runtime_error("voice " + std::to_string(voice.identity) + " sample is absent from owner " + std::to_string(voice.owner) + ": " + voice.path);
			}
			candidates.emplace(voice.identity, PlayingVoice{nullptr, owner, voice.path, voice.minimumAudibleDistance});
			descriptions.emplace(voice.identity, &voice);
		}
		std::vector<std::unique_ptr<const Vector>> playerPositions;
		for (const Vector& position: state.playerPositions) playerPositions.emplace_back(std::make_unique<const Vector>(position));
		std::array<std::list<NetworkSoundData>, c_MaxClients> events;
		for (int player = 0; player < c_MaxClients; ++player) for (const auto& event: state.events[player]) events[player].push_back(event.data);
		std::unordered_map<int, int> backendIdentities;
		std::unordered_map<int, float> minimumDistances;
		backendIdentities.reserve(candidates.size()); minimumDistances.reserve(candidates.size());
		// Allocate every replacement voice paused in reserved virtual slots. Originals retain
		// their callbacks and owning pointers until all candidate channels are ready.
		AudioCheckpoint::MixerLock mixer(m_AudioEnabled ? m_AudioSystem : nullptr);
		struct CandidateCleanup {
			std::map<int, FMOD::Channel*>& channels;
			bool committed = false;
			~CandidateCleanup() { if (!committed) for (const auto& [identity, channel]: channels) { channel->setCallback(nullptr); channel->setUserData(nullptr); channel->stop(); } }
		} cleanup{backendCandidates};
		const std::array<FMOD::ChannelGroup*, 3> buses = {m_SFXChannelGroup, m_UIChannelGroup, m_MusicChannelGroup};
		for (const auto& voice: state.voices) {
			if (!voice.playing) continue;
			FMOD::Channel* channel = nullptr;
			AudioCheckpoint::Require(m_AudioSystem->playSound(sounds.at(voice.path), buses[voice.bus], true, &channel));
			backendCandidates.emplace(voice.identity, channel);
			voice.Apply(m_AudioSystem, channel);
			candidates.at(voice.identity).channel = channel;
			int backend; AudioCheckpoint::Require(channel->getIndex(&backend)); backendIdentities.emplace(backend, voice.identity);
		}
		minimumDistances.insert(state.minimumDistances.begin(), state.minimumDistances.end());
		// Shared sample and bus controls are applied with an exact undo copy while the mixer
		// is locked. A backend refusal cannot leave the original soundscape partly changed.
		std::vector<std::pair<FMOD::Sound*, AudioCheckpoint::Sample>> oldSamples;
		std::array<AudioCheckpoint::Control, 4> oldGroups;
		std::vector<std::array<FMOD_VECTOR, 4>> oldListeners;
		const std::array<FMOD::ChannelGroup*, 4> groups = {m_MasterChannelGroup, m_SFXChannelGroup, m_UIChannelGroup, m_MusicChannelGroup};
		PreservedGroupEffects originalEffects(groups);
		if (m_AudioEnabled) {
			for (const auto& sample: state.samples) oldSamples.emplace_back(sounds.at(sample.path), AudioCheckpoint::Sample::Capture(sample.path, sounds.at(sample.path)));
			for (size_t index = 0; index < groups.size(); ++index) oldGroups[index] = AudioCheckpoint::Control::Capture(groups[index], false);
			int listeners; AudioCheckpoint::Require(m_AudioSystem->get3DNumListeners(&listeners)); oldListeners.resize(listeners);
			for (int index = 0; index < listeners; ++index) { auto& old = oldListeners[index]; AudioCheckpoint::Require(m_AudioSystem->get3DListenerAttributes(index, &old[0], &old[1], &old[2], &old[3])); }
			try {
				if (state.enabled) originalEffects.Detach();
				for (const auto& sample: state.samples) sample.Apply(sounds.at(sample.path));
				if (state.enabled) for (size_t index = 0; index < groups.size(); ++index) state.groups[index].Apply(m_AudioSystem, groups[index], true);
				for (const auto& [identity, channel]: backendCandidates) {
					AudioCheckpoint::Require(channel->setUserData(candidates.at(identity).owner));
					AudioCheckpoint::Require(channel->setCallback(SoundChannelEndedCallback));
					AudioCheckpoint::Require(channel->setPaused(descriptions.at(identity)->control.paused));
					AudioCheckpoint::Effect::ApplyActivation(channel, descriptions.at(identity)->control.effects);
				}
				if (state.enabled) {
					AudioCheckpoint::Require(m_AudioSystem->set3DNumListeners(static_cast<int>(state.listeners.size())));
					for (size_t index = 0; index < state.listeners.size(); ++index) {
						const auto& listener = state.listeners[index];
						const auto position = AudioCheckpoint::Unpack(listener[0]), velocity = AudioCheckpoint::Unpack(listener[1]), forward = AudioCheckpoint::Unpack(listener[2]), up = AudioCheckpoint::Unpack(listener[3]);
						AudioCheckpoint::Require(m_AudioSystem->set3DListenerAttributes(static_cast<int>(index), &position, &velocity, &forward, &up));
					}
					AudioCheckpoint::Require(m_MasterChannelGroup->setMute(state.muteMaster || m_OutputSilenced));
				}
			} catch (...) {
				for (const auto& [sound, sample]: oldSamples) { try { sample.Apply(sound); } catch (...) {} }
				for (size_t index = 0; index < groups.size(); ++index) { try { oldGroups[index].Apply(m_AudioSystem, groups[index], true, false); } catch (...) {} }
				m_AudioSystem->set3DNumListeners(static_cast<int>(oldListeners.size()));
				for (size_t index = 0; index < oldListeners.size(); ++index) { const auto& old = oldListeners[index]; m_AudioSystem->set3DListenerAttributes(static_cast<int>(index), &old[0], &old[1], &old[2], &old[3]); }
				throw;
			}
		}
		// Nothing that can reject the checkpoint remains after this ownership transfer.
		for (const auto& [data, sound]: stagedSamples) data->SoundObject = sound;
		for (auto& [identity, voice]: m_PlayingVoices) {
			if (voice.channel) { voice.channel->setCallback(nullptr); voice.channel->setUserData(nullptr); voice.channel->stop(); }
		}
		// The saved voice graph owns both directions of this relation. Current
		// containers may have stopped or played additional sounds during staging.
		for (auto& [owner, channels]: ownerChannels) owner->m_PlayingChannels.swap(channels);
		m_PlayingVoices = std::move(candidates);
		for (const auto& [path, sample]: newSamples) {
			const auto old = ContentFile::s_LoadedSamples.find(path);
			if (old != ContentFile::s_LoadedSamples.end() && !old->second) ContentFile::s_LoadedSamples.erase(old);
		}
		ContentFile::s_LoadedSamples.merge(newSamples);
		m_BackendVoiceIdentities.swap(backendIdentities); m_SoundChannelMinimumAudibleDistances.swap(minimumDistances);
		m_NextVoiceIdentity = state.nextVoice;
		m_NextSoundContainerIdentity = state.nextSoundContainer;
		m_DeferredSoundOpTick = state.deferredSoundOpTick;
		m_DeferredSoundOpOrdinal = state.deferredSoundOpOrdinal;
		m_MuteMaster = state.muteMaster; m_MuteMusic = state.muteMusic; m_MuteSounds = state.muteSounds; m_MuteAudioOnFocusLoss = state.muteOnFocusLoss;
		m_MasterVolume = state.masterVolume; m_MusicVolume = state.musicVolume; m_SoundsVolume = state.soundsVolume; m_GlobalPitch = state.globalPitch;
		m_SoundPanningEffectStrength = state.panning; m_ListenerZOffset = state.listenerZ; m_MinimumDistanceForPanning = state.minimumPanning;
		m_MusicMuffled = state.musicMuffled; m_IsInMultiplayerMode = state.multiplayer;
		m_CommittedAudibility.clear();
		for (const auto& record: state.audibility) m_CommittedAudibility[record.key][record.peer] = {record.frame, record.value};
		m_LastSentAudibility.clear();
		m_CurrentActivityHumanPlayerPositions.swap(playerPositions);
		for (int player = 0; player < c_MaxClients; ++player) {
			std::lock_guard lock(g_SoundEventsListMutex[player]);
			m_SoundEvents[player].swap(events[player]);
		}
		cleanup.committed = true;
		originalEffects.Commit();
		TraceCheckpointBoundary("load-committed");
		return true;
	} catch (const std::exception& error) {
		if (refusal) *refusal = error.what();
		g_ConsoleMan.PrintString(std::string("ERROR: Could not restore audio checkpoint: ") + error.what());
		std::cout << "[audio-checkpoint] " << error.what() << std::endl;
		return false;
	}
}

void AudioMan::SetCheckpointTraceEnabled(bool enabled) {
	s_TraceAudioCheckpoints = enabled;
}

void AudioMan::TraceCheckpointBoundary(const char* stage) const {
	if (!s_TraceAudioCheckpoints || !m_AudioEnabled) return;
	try {
		std::ostringstream trace;
		{
			AudioCheckpoint::MixerLock mixer(m_AudioSystem);
			FMOD_OUTPUTTYPE output; int rate, buffers; unsigned int buffer;
			AudioCheckpoint::Require(m_AudioSystem->getOutput(&output));
			AudioCheckpoint::Require(m_AudioSystem->getSoftwareFormat(&rate, nullptr, nullptr));
			AudioCheckpoint::Require(m_AudioSystem->getDSPBufferSize(&buffer, &buffers));
			const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
			trace << "[audio-boundary] stage=" << stage << " monotonic_ns=" << now << " output=" << output
			      << " rate=" << rate << " buffer=" << buffer << " buffers=" << buffers << '\n';
			for (const auto& [identity, voice]: m_PlayingVoices) {
				bool playing = false;
				if (!voice.channel || voice.channel->isPlaying(&playing) != FMOD_OK || !playing) {
					trace << "[audio-boundary-voice] stage=" << stage << " id=" << identity << " playing=0\n";
					continue;
				}
				unsigned int position; unsigned long long clock, parent; float frequency, pitch; bool paused;
				AudioCheckpoint::Require(voice.channel->getPosition(&position, FMOD_TIMEUNIT_PCM));
				AudioCheckpoint::Require(voice.channel->getDSPClock(&clock, &parent));
				AudioCheckpoint::Require(voice.channel->getFrequency(&frequency)); AudioCheckpoint::Require(voice.channel->getPitch(&pitch));
				AudioCheckpoint::Require(voice.channel->getPaused(&paused));
				trace << "[audio-boundary-voice] stage=" << stage << " id=" << identity << " owner=" << (voice.owner ? voice.owner->GetCheckpointIdentity() : 0)
				      << " playing=1 pcm=" << position << " clock=" << clock << " parent_clock=" << parent
				      << " frequency=" << frequency << " pitch=" << pitch << " paused=" << paused << '\n';
			}
		}
		std::cout << trace.str() << std::flush;
	} catch (const std::exception& error) {
		std::cout << "[audio-boundary] stage=" << stage << " error=" << error.what() << std::endl;
	}
}

bool AudioMan::PerturbCheckpointCursorForSelfTest() {
	if (!m_AudioEnabled) return false;
	AudioCheckpoint::MixerLock mixer(m_AudioSystem);
	for (const auto& [identity, voice]: m_PlayingVoices) {
		bool playing = false; unsigned int before, length, after; FMOD::Sound* sound = nullptr;
		if (!voice.channel || voice.channel->isPlaying(&playing) != FMOD_OK || !playing ||
		    voice.channel->getCurrentSound(&sound) != FMOD_OK || !sound || sound->getLength(&length, FMOD_TIMEUNIT_PCM) != FMOD_OK ||
		    voice.channel->getPosition(&before, FMOD_TIMEUNIT_PCM) != FMOD_OK || length < 2 || before >= length - 1) continue;
		if (voice.channel->setPosition(before + 1, FMOD_TIMEUNIT_PCM) != FMOD_OK ||
		    voice.channel->getPosition(&after, FMOD_TIMEUNIT_PCM) != FMOD_OK || after != before + 1) return false;
		std::cout << "[audio-cursor-negative] id=" << identity << " before=" << before << " after=" << after << std::endl;
		return true;
	}
	return false;
}

bool AudioMan::RunCheckpointPlaybackContinuationSelfTest() const {
	if (!m_AudioEnabled) return false;
	std::map<int, unsigned int> positions;
	bool queryFailed = false;
	{
		AudioCheckpoint::MixerLock mixer(m_AudioSystem);
		for (const auto& [identity, voice]: m_PlayingVoices) {
			bool playing = false, paused = true; unsigned int position;
			if (!voice.channel) continue;
			if (voice.channel->isPlaying(&playing) != FMOD_OK || voice.channel->getPaused(&paused) != FMOD_OK) { queryFailed = true; continue; }
			if (!playing || paused) continue;
			if (voice.channel->getPosition(&position, FMOD_TIMEUNIT_PCM) != FMOD_OK) { queryFailed = true; continue; }
			positions.emplace(identity, position);
		}
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(40));
	std::map<int, unsigned int> positionsBeforeUpdate;
	{
		AudioCheckpoint::MixerLock mixer(m_AudioSystem);
		for (const auto& [identity, before]: positions) {
			const auto voice = m_PlayingVoices.find(identity);
			if (voice == m_PlayingVoices.end() || !voice->second.channel) continue;
			unsigned int position;
			if (voice->second.channel->getPosition(&position, FMOD_TIMEUNIT_PCM) == FMOD_OK) positionsBeforeUpdate.emplace(identity, position);
			else queryFailed = true;
		}
	}
	// FMOD updates the reported PCM cursor of a virtual (muted) voice when the
	// regular backend update runs, although its DSP clock advances while sleeping.
	const FMOD_RESULT updateResult = m_AudioSystem->update();
	queryFailed = queryFailed || updateResult != FMOD_OK;
	size_t advanced = 0, completed = 0;
	{
		AudioCheckpoint::MixerLock mixer(m_AudioSystem);
		for (const auto& [identity, before]: positions) {
			const auto voice = m_PlayingVoices.find(identity);
			if (voice == m_PlayingVoices.end() || !voice->second.channel) {
				++completed;
				std::cout << "[audio-playback-progress] id=" << identity << " before=" << before << " completed=1" << std::endl;
				continue;
			}
			unsigned int after;
			if (voice->second.channel->getPosition(&after, FMOD_TIMEUNIT_PCM) != FMOD_OK) { queryFailed = true; continue; }
			if (after != before) ++advanced;
			std::cout << "[audio-playback-progress] id=" << identity << " before=" << before;
			if (const auto observed = positionsBeforeUpdate.find(identity); observed != positionsBeforeUpdate.end()) std::cout << " before_update=" << observed->second;
			std::cout << " after=" << after << std::endl;
		}
	}
	const bool passed = !queryFailed && (advanced > 0 || completed > 0);
	std::cout << "[audio-playback-continuation] " << (passed ? "PASS" : "FAIL") << " unpaused=" << positions.size() << " advanced=" << advanced << " completed=" << completed << " query_failed=" << queryFailed << " update_result=" << static_cast<int>(updateResult) << std::endl;
	return passed;
}

std::string AudioMan::GetSoundContainerPlaybackCheckpoint(const SoundContainer* container) const {
	std::vector<std::string> voices;
	AudioCheckpoint::MixerLock mixer(m_AudioEnabled ? m_AudioSystem : nullptr);
	for (const auto& [identity, voice]: m_PlayingVoices) {
		if (voice.owner != container) continue;
		int bus = container ? container->GetBusRouting() : 0;
		voices.push_back(AudioCheckpoint::Voice::Capture(identity, container ? container->GetCheckpointIdentity() : 0, voice.soundPath, voice.minimumAudibleDistance, voice.channel, bus).SaveCheckpoint());
	}
	CheckpointWriter writer("SoundPlayback1"); writer(voices); return writer.Text();
}
bool AudioMan::RunCheckpointSelfTest() {
	if (!m_AudioEnabled) return false;
	const std::string original = SaveCheckpoint();
	std::map<SoundContainer*, std::string> originalOwners;
	for (const auto& [identity, voice]: m_PlayingVoices) if (voice.owner) originalOwners.try_emplace(voice.owner, voice.owner->SaveCheckpoint());
	const RandomGenerator originalRenderRNG = g_RenderRNG;
	bool ok = true;
	try {
		const auto* preset = dynamic_cast<const SoundContainer*>(g_PresetMan.GetEntityPreset("SoundContainer", "Funds Changed", "Base.rte"));
		if (!preset) throw std::runtime_error("missing checkpoint test sound");
		std::unique_ptr<SoundContainer> source(static_cast<SoundContainer*>(preset->Clone()));
		source->SetPaused(true); source->SetImmobile(true); source->SetLoopSetting(-1); source->SetPitch(1.125F); source->SetVolume(0.35F);
		if (!source->Play()) throw std::runtime_error("checkpoint test sound did not play");
		const int identity = *source->GetPlayingChannels()->begin();
		FMOD::Channel* originalChannel = nullptr; AudioCheckpoint::Require(GetVoiceChannel(identity, &originalChannel));
		{
			AudioCheckpoint::MixerLock mixer(m_AudioSystem);
			FMOD::DSP* effect;
			AudioCheckpoint::Require(m_AudioSystem->createDSPByType(FMOD_DSP_TYPE_MULTIBAND_EQ, &effect));
			AudioCheckpoint::Require(originalChannel->addDSP(0, effect));
			AudioCheckpoint::Require(originalChannel->setPaused(false));
			AudioCheckpoint::Require(effect->setActive(false));
			const auto control = AudioCheckpoint::Control::Capture(originalChannel, false);
			control.Apply(m_AudioSystem, originalChannel, true);
			bool active; AudioCheckpoint::Require(effect->getActive(&active));
			if (active) throw std::runtime_error("unpausing activated a checkpoint-disabled effect");
			AudioCheckpoint::Require(originalChannel->setPaused(true));
		}
		AudioCheckpoint::Require(originalChannel->setPosition(123, FMOD_TIMEUNIT_PCM));
		const std::string native = source->SaveCheckpoint();
		const std::string playback = GetSoundContainerPlaybackCheckpoint(source.get());
		const std::string checkpoint = SaveCheckpoint();
		const CheckpointSoundRegistry liveRegistry = CaptureCheckpointSoundRegistry();
		CheckpointSoundRegistry stagedRegistry;
		std::unique_ptr<SoundContainer> stagedOwner;
		const uint64_t liveCursor = GetCheckpointSoundContainerCursor();
		{
			CheckpointRegistryScope registryScope;
			MovableObject::FaithfulCloneScope cloneScope(true);
			stagedOwner.reset(static_cast<SoundContainer*>(source->Clone()));
			stagedRegistry = AddedCheckpointSoundRegistrations(liveRegistry);
		}
		if (FindCheckpointSoundContainer(source->GetCheckpointIdentity()) != source.get() || GetCheckpointSoundContainerCursor() != liveCursor) throw std::runtime_error("private staging changed the live sound registry");
		ActivateCheckpointSoundRegistrations(stagedRegistry);
		if (FindCheckpointSoundContainer(source->GetCheckpointIdentity()) != stagedOwner.get()) throw std::runtime_error("staged sound owner could not be activated");
		RestoreCheckpointSoundRegistry(liveRegistry);
		stagedOwner.reset();
		// A setting the manager never assigned reaches the writer as a raw byte, which is neither
		// true nor false, and the reader has to refuse the archive rather than round it to a bool.
		{
			CheckpointWriter bools("Bool1");
			bool ordinary = false;
			bools(ordinary);
			ordinary = true;
			bools(ordinary, std::vector<bool>{true, false, true});
			if (bools.Text() != "5 Bool1 0 1 3 1 0 1 ") throw std::runtime_error("an assigned bool did not write as 0 or 1: " + bools.Text());
			bool first = true, second = false;
			std::vector<bool> bits;
			CheckpointReader boolReader(bools.Text(), "Bool1");
			boolReader(first, second, bits);
			boolReader.Finish();
			if (first || !second || bits != std::vector<bool>{true, false, true}) throw std::runtime_error("an assigned bool did not survive the archive");
			std::cout << "[audio-checkpoint-selftest] PASS assigned_bools_round_trip" << std::endl;
			AudioRuntime unset;
			const unsigned char rawByte = 100;
			std::memcpy(&unset.muteOnFocusLoss, &rawByte, sizeof(rawByte));
			const std::string unsetText = unset.Save();
			if (!unsetText.starts_with("13 AudioRuntime3 0 0 0 0 0 0 100 ")) throw std::runtime_error("the writer did not carry the setting's own byte into the archive: " + unsetText.substr(0, 40));
			std::cout << "[audio-checkpoint-selftest] PASS unset_setting_byte_reaches_the_archive" << std::endl;
			std::string unsetRefusal;
			if (unset.Load(unsetText, &unsetRefusal)) throw std::runtime_error("an audio setting that is neither true nor false was accepted");
			if (unsetRefusal.find("'100'") == std::string::npos) throw std::runtime_error("the refusal did not name the offending value: " + unsetRefusal);
			std::cout << "[audio-checkpoint-selftest] PASS unset_setting_byte_is_refused_and_named " << unsetRefusal << std::endl;
			unsigned char liveByte = 0;
			std::memcpy(&liveByte, &m_MuteAudioOnFocusLoss, sizeof(liveByte));
			if (liveByte > 1) throw std::runtime_error("MuteAudioOnFocusLoss was never initialised: byte " + std::to_string(liveByte));
			std::cout << "[audio-checkpoint-selftest] PASS mute_on_focus_loss_initialised" << std::endl;
		}
		AudioRuntime invalid;
		std::string refusal;
		if (!invalid.Load(checkpoint, &refusal)) throw std::runtime_error("could not parse generated audio checkpoint: " + refusal);
		for (auto& voice: invalid.voices) if (voice.identity == identity) {
			AudioCheckpoint::Effect invalidEffect; invalidEffect.index = 63; invalidEffect.type = FMOD_DSP_TYPE_MULTIBAND_EQ;
			voice.control.effects.push_back(invalidEffect);
		}
		if (LoadCheckpoint(invalid.Save()) || LoadCheckpoint(checkpoint + "trailing")) throw std::runtime_error("malformed audio checkpoint was accepted");
		FMOD::Channel* afterFailure = nullptr; AudioCheckpoint::Require(GetVoiceChannel(identity, &afterFailure));
		if (afterFailure != originalChannel || source->SaveCheckpoint() != native || GetSoundContainerPlaybackCheckpoint(source.get()) != playback) throw std::runtime_error("failed audio restore changed original voice or owner");
		{
			CheckpointRegistryScope registryScope;
			Entity::CheckpointCloneScope readerScope(true);
			std::unique_ptr<SoundContainer> readCopy(static_cast<SoundContainer*>(source->Clone()));
			if (readCopy->SaveCheckpoint() != native) throw std::runtime_error("INI checkpoint clone lost sound ownership or controls");
		}
		{
			MovableObject::FaithfulCloneScope scope(false);
			std::unique_ptr<SoundContainer> captured(static_cast<SoundContainer*>(source->Clone()));
			if (captured->SaveCheckpoint() != native) throw std::runtime_error("sound capture clone lost runtime state");
		}
		if (GetSoundContainerPlaybackCheckpoint(source.get()) != playback) throw std::runtime_error("destroying a capture clone changed original playback");
		std::unique_ptr<SoundContainer> replacement;
		{ MovableObject::FaithfulCloneScope scope(true); replacement.reset(static_cast<SoundContainer*>(source->Clone())); }
		if (!LoadCheckpoint(checkpoint)) throw std::runtime_error("valid audio checkpoint was rejected");
		FMOD::Channel* restoredChannel = nullptr; AudioCheckpoint::Require(GetVoiceChannel(identity, &restoredChannel));
		if (restoredChannel == originalChannel || source->IsBeingPlayed() || !replacement->IsBeingPlayed() || replacement->SaveCheckpoint() != native || GetSoundContainerPlaybackCheckpoint(replacement.get()) != playback) throw std::runtime_error("audio restoration lost controls or failed to rebind owner");
		{
			std::unique_ptr<SoundContainer> stale;
			{
				CheckpointRegistryScope registryScope;
				MovableObject::FaithfulCloneScope cloneScope(true);
				stale.reset(static_cast<SoundContainer*>(replacement->Clone()));
			}
			if (stale->SaveCheckpoint() != native || stale->IsBeingPlayed()) throw std::runtime_error("a detached checkpoint clone claims a live voice");
			stale->SetPosition(Vector(89, 144)); stale->SetVolume(0.9F); stale->SetPitch(0.5F); stale->SetCustomPanValue(0.25F); stale->SetPaused(false); stale->FadeOut(250); stale->Stop();
			auto retiredWeapon = std::make_unique<HDFirearm>();
			retiredWeapon->SetFireSound(stale.release());
			retiredWeapon.reset();
			if (!replacement->IsBeingPlayed() || GetSoundContainerPlaybackCheckpoint(replacement.get()) != playback) throw std::runtime_error("retired native sound controls or firearm cleanup changed the new owner's voice");
		}
		if (!replacement->Play() || replacement->GetPlayingChannels()->size() != 2) throw std::runtime_error("could not create an additional current voice");
		const std::string additionalPlayback = GetSoundContainerPlaybackCheckpoint(replacement.get());
		const std::string additionalNative = replacement->SaveCheckpoint();
		if (LoadCheckpoint(checkpoint + "trailing") || GetSoundContainerPlaybackCheckpoint(replacement.get()) != additionalPlayback || replacement->SaveCheckpoint() != additionalNative) throw std::runtime_error("failed restore changed additional current playback");
		if (!LoadCheckpoint(checkpoint) || replacement->SaveCheckpoint() != native || GetSoundContainerPlaybackCheckpoint(replacement.get()) != playback) throw std::runtime_error("saved audio cohort did not replace additional current playback");
		if (!replacement->Stop()) throw std::runtime_error("could not stop playback before restoration");
		AudioCheckpoint::Require(m_AudioSystem->update());
		if (!LoadCheckpoint(checkpoint) || replacement->SaveCheckpoint() != native || GetSoundContainerPlaybackCheckpoint(replacement.get()) != playback) throw std::runtime_error("saved audio cohort did not resume stopped playback");
		std::cout << "[audio-checkpoint-selftest] stale-owner controls/firearm cleanup, additional and stopped playback restore PASS" << std::endl;
		try {
			const auto* actorPreset = dynamic_cast<const Actor*>(g_PresetMan.GetEntityPreset("AHuman", "Green Dummy", "Base.rte"));
			const auto* woundPreset = dynamic_cast<const AEmitter*>(g_PresetMan.GetEntityPreset("AEmitter", "Leaking Machinery", "Base.rte"));
			if (!actorPreset || !woundPreset) throw std::runtime_error("missing wound owner selftest presets");
			std::unique_ptr<Actor> actor(static_cast<Actor*>(actorPreset->Clone()));
			AEmitter* wound = static_cast<AEmitter*>(woundPreset->Clone());
			actor->AddWound(wound, Vector(), false);
			wound->Update();
			SoundContainer* burst = wound->GetBurstSound();
			if (!burst) throw std::runtime_error("wound has no burst sound");
			if (burst->GetPlayingChannels()->empty()) {
				burst->SetPaused(true);
				burst->SetImmobile(true);
				burst->SetLoopSetting(-1);
				if (!burst->Play()) throw std::runtime_error("wound burst voice did not play");
			}
			const int woundVoice = *burst->GetPlayingChannels()->begin();
			Attachable* kept = actor->RemoveAttachable(wound, false, false);
			if (!kept) throw std::runtime_error("RemoveAttachable did not keep the wound");
			auto& wounds = const_cast<std::vector<AEmitter*>&>(actor->GetWoundList());
			std::erase(wounds, wound);
			{
				SoundCheckpointSaveScope scope;
				WriteSnapshotObject(actor.get());
				const std::string audio = ContainedAudioSave(scope);
				if (ArchivedVoiceOwner(audio, woundVoice) != 0) throw std::runtime_error("orphaned wound voice still named its owner");
				CheckCarriedAudioOwners(audio, scope.Carried());
				if (!LoadCheckpoint(audio) || m_PlayingVoices.at(woundVoice).owner) throw std::runtime_error("disowned wound voice did not apply on a clean owner");
			}
			delete kept;
			std::cout << "[audio-checkpoint-selftest] PASS orphaned_wound_owner_is_disowned" << std::endl;
		} catch (const std::exception& error) {
			std::cout << "[audio-checkpoint-selftest] FAIL orphaned_wound_owner_is_disowned " << error.what() << std::endl;
			ok = false;
		}
		try {
			std::set<int> before;
			for (const auto& [identity, voice]: m_PlayingVoices) before.insert(identity);
			if (g_LuaMan.GetMasterScriptState().RunScriptString("_W19OwnerSound = CreateSoundContainer(\"Funds Changed\", \"Base.rte\"); _W19OwnerSound.Loops = -1; _W19OwnerSound.Immobile = true; _W19OwnerSound.Paused = true; _W19OwnerPlayed = _W19OwnerSound:Play()") < 0) {
				throw std::runtime_error("lua copy owner did not play");
			}
			int luaVoice = 0;
			for (const auto& [identity, voice]: m_PlayingVoices) {
				if (!before.contains(identity) && voice.owner) { luaVoice = identity; break; }
			}
			if (!luaVoice) throw std::runtime_error("lua copy owner produced no voice");
			{
				SoundCheckpointSaveScope scope;
				std::vector<std::string> graphs, problems;
				if (!g_MovableMan.SerializeScriptGraphs(graphs, problems)) throw std::runtime_error("lua copy graph serialize failed");
				const std::string audio = ContainedAudioSave(scope);
				if (ArchivedVoiceOwner(audio, luaVoice) != 0) throw std::runtime_error("lua copy owner was archived");
				CheckCarriedAudioOwners(audio, scope.Carried());
			}
			g_LuaMan.GetMasterScriptState().RunScriptString("_W19OwnerSound = nil; _W19OwnerPlayed = nil");
			g_LuaMan.CollectGarbageForCheckpoint();
			std::cout << "[audio-checkpoint-selftest] PASS lua_copy_owner_is_disowned" << std::endl;
		} catch (const std::exception& error) {
			std::cout << "[audio-checkpoint-selftest] FAIL lua_copy_owner_is_disowned " << error.what() << std::endl;
			ok = false;
		}
		try {
			std::unique_ptr<SoundContainer> leftover(static_cast<SoundContainer*>(preset->Clone()));
			leftover->SetPaused(true); leftover->SetImmobile(true); leftover->SetLoopSetting(-1);
			if (!leftover->Play()) throw std::runtime_error("leftover apply fixture did not play");
			const int leftoverVoice = *leftover->GetPlayingChannels()->begin();
			const std::string leftoverAudio = leftover->SaveCheckpoint();
			const std::string snapshot = SaveCheckpoint();
			const uint64_t leftoverOwner = leftover->GetCheckpointIdentity();
			const CheckpointSoundRegistry live = CaptureCheckpointSoundRegistry();
			std::unique_ptr<SoundContainer> clone;
			{
				RestoredSoundRegistryScope restored;
				CheckpointSoundRegistry seed = live;
				seed.erase(leftoverOwner);
				ActivateCheckpointSoundRegistrations(seed);
				{ MovableObject::FaithfulCloneScope faithful(true); clone.reset(static_cast<SoundContainer*>(leftover->Clone())); }
				StopRecordingRestoredSoundRegistry();
				CheckpointSoundRegistry leftoverBack = CaptureCheckpointSoundRegistry();
				leftoverBack[leftoverOwner] = {leftover.get()};
				RestoreCheckpointSoundRegistry(leftoverBack);
				if (!LoadCheckpoint(snapshot)) throw std::runtime_error("written owner did not apply against a leftover registry");
				if (m_PlayingVoices.at(leftoverVoice).owner != clone.get()) throw std::runtime_error("written owner resolved to the leftover object");
			}
			RestoreCheckpointSoundRegistry(live);
			std::cout << "[audio-checkpoint-selftest] PASS written_owner_resolves_to_clone" << std::endl;
			bool leftoverApply = false, cleanApply = false;
			{
				RestoredSoundRegistryScope restored;
				StopRecordingRestoredSoundRegistry();
				leftoverApply = LoadCheckpoint(snapshot);
			}
			{
				RestoredSoundRegistryScope restored;
				StopRecordingRestoredSoundRegistry();
				RestoreCheckpointSoundRegistry({});
				cleanApply = LoadCheckpoint(snapshot);
			}
			RestoreCheckpointSoundRegistry(live);
			if (leftoverApply != cleanApply) throw std::runtime_error("leftover and clean apply differed");
			if (!leftover->LoadCheckpoint(leftoverAudio)) throw std::runtime_error("could not restore leftover apply fixture");
			std::cout << "[audio-checkpoint-selftest] PASS leftover_and_clean_apply_match" << std::endl;
		} catch (const std::exception& error) {
			std::cout << "[audio-checkpoint-selftest] FAIL leftover_or_clone_apply " << error.what() << std::endl;
			ok = false;
		}
		if (!LoadCheckpoint(checkpoint)) throw std::runtime_error("owner selftests left the audio checkpoint unrestored");
		std::unique_ptr<SoundContainer> orphan(static_cast<SoundContainer*>(preset->Clone()));
		orphan->SetPaused(true); orphan->SetImmobile(true); orphan->SetLoopSetting(-1);
		if (!orphan->Play()) throw std::runtime_error("orphan test sound did not play");
		const int orphanID = *orphan->GetPlayingChannels()->begin();
		orphan.reset();
		if (!m_PlayingVoices.contains(orphanID) || m_PlayingVoices.at(orphanID).owner) throw std::runtime_error("destroyed sound owner was not disowned");
		const std::string withOrphan = SaveCheckpoint();
		if (!LoadCheckpoint(withOrphan) || !m_PlayingVoices.contains(orphanID) || m_PlayingVoices.at(orphanID).owner || !m_PlayingVoices.at(orphanID).channel) throw std::runtime_error("orphan playback did not survive restoration");
	} catch (const std::exception& error) {
		std::cout << "[audio-checkpoint-selftest] " << error.what() << std::endl;
		ok = false;
	}
	for (const auto& [owner, checkpoint]: originalOwners) ok = owner->LoadCheckpoint(checkpoint) && ok;
	ok = LoadCheckpoint(original) && ok;
	g_RenderRNG = originalRenderRNG;
	return ok;
}

static_assert(c_MaxLogicalVoicesPerContainer == c_MaxPlayingSoundsPerContainer);

namespace {
	SoundObservationKey KeyOf(const SoundExecutionKey& key) { return {key.objectUID, key.tick, key.phase, key.occurrence, key.ordinal}; }
	SoundObservationKey KeyOf(const NetSoundObservation& observation) { return {observation.objectUID, observation.tick, observation.phase, observation.occurrence, observation.ordinal}; }
	constexpr uint64_t c_AudibilityPruneInterval = 600;
	constexpr uint64_t c_AudibilityRetainFrames = 36000;
	// CC_TRACE_AUDIBILITY=<nonzero> prints every sound's first committed reading and every query that finds none, so two peers' committed tables can be compared.
	bool TraceAudibility() {
		static const bool traced = [] { const char* value = std::getenv("CC_TRACE_AUDIBILITY"); return value && std::strtoull(value, nullptr, 10) != 0ULL; }();
		return traced;
	}
} // namespace

uint8_t AudioMan::AudibilityAuthority(uint64_t objectUID) {
	const uint8_t host = ScenarioRunner::GetLockstepHostPeerId();
	if (objectUID == 0) return host;
	const MovableObject* object = g_MovableMan.FindObjectByUniqueID(static_cast<long>(objectUID));
	const Actor* actor = object ? dynamic_cast<const Actor*>(object->GetRootParent()) : nullptr;
	if (!actor) return host;
	return ScenarioRunner::GetLockstepActorOwner(static_cast<int64_t>(actor->GetUniqueID()), actor->GetTeam(), !actor->IsPlayerControlled());
}

std::vector<NetSoundObservation> AudioMan::SampleSoundObservations() {
	std::vector<NetSoundObservation> observations;
	if (!ScenarioRunner::IsLockstepControllerSyncActive()) return observations;
	const uint8_t localPeer = ScenarioRunner::GetLockstepLocalPeerId();
	std::vector<const SoundContainer*> live;
	VisitSharedSimulationSounds([&](const SoundContainer& container) { live.push_back(&container); });
	std::sort(live.begin(), live.end(), [](const SoundContainer* a, const SoundContainer* b) { return a->GetCheckpointIdentity() < b->GetCheckpointIdentity(); });
	std::set<SoundObservationKey> answered;
	for (const SoundContainer* container: live) {
		const SoundExecutionKey& identity = container->GetSharedPlaybackIdentity();
		// Every peer reports every live shared sound, so the reading of whoever controls it next is already committed when control moves.
		if (!identity.ordinal) continue;
		const SoundObservationKey key = KeyOf(identity);
		answered.insert(key);
		const float value = GetLocalSoundAudibility(container);
		const auto sent = m_LastSentAudibility.find(key);
		if (sent != m_LastSentAudibility.end() && sent->second == value) continue;
		m_LastSentAudibility[key] = value;
		NetSoundObservation observation;
		observation.senderPeerId = localPeer;
		observation.objectUID = key.objectUID; observation.tick = key.tick; observation.phase = key.phase; observation.occurrence = key.occurrence; observation.ordinal = key.ordinal;
		observation.value = value;
		observations.push_back(observation);
	}
	// A sound that leaves the live set reports afresh if it returns.
	std::erase_if(m_LastSentAudibility, [&](const auto& entry) { return !answered.contains(entry.first); });
	return observations;
}

void AudioMan::ForgetSentAudibility(const std::vector<NetSoundObservation>& observations) {
	for (const NetSoundObservation& observation: observations) m_LastSentAudibility.erase(KeyOf(observation));
}

void AudioMan::CommitSoundObservations(uint64_t frame, const std::vector<NetSoundObservation>& local, const std::vector<NetSoundObservation>& remote) {
	std::vector<const NetSoundObservation*> observations;
	observations.reserve(local.size() + remote.size());
	for (const NetSoundObservation& observation: local) observations.push_back(&observation);
	for (const NetSoundObservation& observation: remote) observations.push_back(&observation);
	std::stable_sort(observations.begin(), observations.end(), [](const NetSoundObservation* a, const NetSoundObservation* b) { return a->senderPeerId < b->senderPeerId; });
	for (const NetSoundObservation* observation: observations) {
		const SoundObservationKey key = KeyOf(*observation);
		if (TraceAudibility() && !m_CommittedAudibility.contains(key)) {
			std::cout << "[audibility] first reading object=" << key.objectUID << " tick=" << key.tick << " phase=" << key.phase << " occurrence=" << key.occurrence << " ordinal=" << key.ordinal
			          << " from peer " << static_cast<int>(observation->senderPeerId) << " value=" << observation->value << " frame=" << frame << std::endl;
		}
		m_CommittedAudibility[key][observation->senderPeerId] = {frame, observation->value};
	}
	if (frame % c_AudibilityPruneInterval != 0) return;
	// Readings nobody refreshed for ten minutes belong to finished sounds; every peer prunes on the same frame.
	for (auto entry = m_CommittedAudibility.begin(); entry != m_CommittedAudibility.end();) {
		std::erase_if(entry->second, [&](const auto& reading) { return reading.second.frame + c_AudibilityRetainFrames <= frame; });
		entry = entry->second.empty() ? m_CommittedAudibility.erase(entry) : std::next(entry);
	}
}

float AudioMan::GetCommittedAudibility(const SoundContainer& container) const {
	const SoundExecutionKey& identity = container.GetSharedPlaybackIdentity();
	if (!identity.ordinal) return 0.0F;
	const auto entry = m_CommittedAudibility.find(KeyOf(identity));
	if (entry == m_CommittedAudibility.end()) {
		if (TraceAudibility()) {
			std::lock_guard lock(m_AudibilityMissMutex);
			if (m_ReportedAudibilityMisses.insert(KeyOf(identity)).second) {
				std::cout << "[audibility] no committed reading for " << container.GetPresetName() << " object=" << identity.objectUID << " tick=" << identity.tick << " phase=" << identity.phase
				          << " occurrence=" << identity.occurrence << " ordinal=" << identity.ordinal << " at " << g_TimerMan.GetSimUpdateCount() << " (table " << m_CommittedAudibility.size() << ")" << std::endl;
			}
		}
		return 0.0F;
	}
	const uint8_t authority = AudibilityAuthority(container.GetSharedLogicalPlayback().lastObjectUID);
	if (const auto reading = entry->second.find(authority); reading != entry->second.end()) return reading->second.value;
	// Between a control change and the new controller's first reading, the latest committed reading stands.
	const CommittedAudibility* latest = nullptr;
	for (const auto& [peer, reading]: entry->second) {
		if (!latest || reading.frame > latest->frame) latest = &reading;
	}
	return latest ? latest->value : 0.0F;
}

void AudioMan::ClearCommittedAudibility() {
	m_CommittedAudibility.clear();
	m_LastSentAudibility.clear();
	std::lock_guard lock(m_AudibilityMissMutex);
	m_ReportedAudibilityMisses.clear();
}

bool AudioMan::RunLogicalPlaybackSelfTest() {
	bool passed = true;
	const auto check = [&passed](const char* name, bool ok, const std::string& detail = std::string()) {
		std::cout << "[audio-logical-selftest] " << (ok ? "PASS " : "FAIL ") << name << (detail.empty() ? "" : " " + detail) << std::endl;
		passed = passed && ok;
	};
	if (!m_AudioEnabled) {
		check("backend_available", false, "audio backend unavailable");
		return false;
	}
	const long long simCount = g_TimerMan.GetSimUpdateCount();
	const long long simTicks = g_TimerMan.GetSimTimeTicks();
	const long long ticksPerSecond = g_TimerMan.GetTicksPerSecond();
	const bool suppressed = s_PlaybackSuppressed;
	s_PlaybackSuppressed = true;
	const auto advance = [&](double seconds) {
		const long long target = g_TimerMan.GetSimTimeTicks() + static_cast<long long>(seconds * static_cast<double>(ticksPerSecond));
		while (g_TimerMan.GetSimTimeTicks() < target) {
			const long long before = g_TimerMan.GetSimTimeTicks();
			g_TimerMan.AdvanceSimTickForPreview();
			if (g_TimerMan.GetSimTimeTicks() == before) return false;
		}
		return true;
	};
	static const uint64_t phase = Hash("LogicalSelfTest");
	const char* samplePath = "Base.rte/Sounds/GUIs/ButtonPress.flac";
	{
		SoundContainer sound;
		sound.Create(samplePath, false, true, SoundContainer::SFX);
		double duration = 0;
		{
			SoundSimulationScope shared(4242, phase);
			check("plays_logically", sound.Play() && sound.IsBeingPlayed() && sound.GetSharedLogicalVoices().size() == 1);
			if (!sound.GetSharedLogicalVoices().empty()) {
				const LogicalSoundVoice& voice = sound.GetSharedLogicalVoices().front();
				duration = static_cast<double>(voice.sampleFrames) / static_cast<double>(voice.sampleRate);
			}
			check("sample_metadata", duration > 0, std::to_string(duration));
		}
		check("logical_independent_of_output", !sound.IsBeingPlayed());
		{
			SoundSimulationScope shared(4242, phase);
			check("sim_time_advances", advance(duration * 0.5));
			check("still_playing_midway", sound.IsBeingPlayed());
			size_t visited = 0;
			VisitSharedSimulationSounds([&](const SoundContainer& container) { visited += &container == &sound; });
			check("registered_while_live", visited == 1);
			advance(duration * 0.6);
			check("finishes_by_sim_time", !sound.IsBeingPlayed());
			RetireFinishedSimulationSounds();
			visited = 0;
			VisitSharedSimulationSounds([&](const SoundContainer& container) { visited += &container == &sound; });
			check("retired_when_finished", visited == 0 && sound.GetSharedLogicalVoices().empty());

			sound.SetLoopSetting(-1);
			sound.Play();
			advance(duration * 10);
			const bool loopedTenTimes = sound.IsBeingPlayed();
			check("loops_until_stopped", loopedTenTimes && sound.Stop() && !sound.IsBeingPlayed());

			sound.SetLoopSetting(2);
			sound.Play();
			advance(duration * 2.5);
			const bool midLoops = sound.IsBeingPlayed();
			advance(duration * 0.6);
			check("finite_loops", midLoops && !sound.IsBeingPlayed());
			sound.SetLoopSetting(0);

			sound.Play();
			sound.SetPitch(2.0F);
			advance(duration * 0.6);
			check("pitch_scales_progress", !sound.IsBeingPlayed());
			sound.SetPitch(1.0F);

			sound.Play();
			sound.SetPaused(true);
			advance(duration * 2);
			const bool heldWhilePaused = sound.IsBeingPlayed();
			sound.SetPaused(false);
			advance(duration * 0.5);
			const bool resumedAfterPause = sound.IsBeingPlayed();
			advance(duration * 0.6);
			check("pause_holds_progress", heldWhilePaused && resumedAfterPause && !sound.IsBeingPlayed());

			sound.SetSoundOverlapMode(SoundContainer::RESTART);
			sound.Play();
			advance(duration * 0.5);
			sound.Play();
			advance(duration * 0.7);
			check("restart_replaces_voice", sound.IsBeingPlayed() && sound.GetSharedLogicalVoices().size() == 1);
			sound.Stop();
			sound.SetSoundOverlapMode(SoundContainer::OVERLAP);
			sound.Play();
			sound.FadeOut(200);
			advance(duration * 0.5);
			check("fade_keeps_liveness", sound.IsBeingPlayed() && sound.GetSharedLogicalVoices().front().fadeStartTicks >= 0);
			sound.Stop();
		}
		{
			SoundSimulationScope shared(4242, phase);
			sound.Play();
			sound.SetPosition(Vector(10.0F, 20.0F));
		}
		std::vector<SoundContainer::PendingOp> deferred;
		{
			SoundSimulationScope local(4242, phase, SoundExecutionDomain::LocalSimulation);
			// An AI hook asks the one simulation cohort what is playing, and reads back its own calls.
			const bool sawShared = sound.IsBeingPlayed();
			sound.SetVolume(0.25F);
			const bool readBack = sound.GetVolume() == 0.25F;
			const bool stopTook = sound.Stop() && !sound.IsBeingPlayed();
			check("ai_reads_the_simulation_cohort", sawShared && readBack && stopTook);
			Vector& alias = const_cast<Vector&>(sound.GetScriptPosition());
			const bool aliasTracks = alias.m_X == 10.0F && alias.m_Y == 20.0F;
			alias.m_X = 33.0F;
			check("ai_alias_reads_back", aliasTracks && sound.GetScriptPosition().m_X == 33.0F);
		}
		{
			SoundSimulationScope shared(4242, phase);
			// The calls are decisions, not actions: nothing has landed on shared state yet.
			check("ai_writes_wait_for_the_drain", sound.IsBeingPlayed() && sound.GetSharedLogicalVoices().size() == 1 && sound.GetVolume() == 1.0F && sound.GetPosition().m_X == 10.0F);
		}
		{
			deferred = sound.TakePendingSoundOps();
			SoundSimulationScope drain(4242, phase, SoundExecutionDomain::SharedSimulation, NextDeferredSoundOpOrdinal());
			bool applied = deferred.size() == 3;
			for (const SoundContainer::PendingOp& op: deferred) applied = sound.ApplyPendingSoundOp(op) && applied;
			check("ai_writes_land_at_the_drain", applied && !sound.IsBeingPlayed() && sound.GetVolume() == 0.25F && sound.GetPosition().m_X == 33.0F && sound.GetPosition().m_Y == 20.0F, std::to_string(deferred.size()));
			sound.SetVolume(1.0F);
			sound.SetPosition(Vector());
		}
		{
			// Every deferred call survives the wire byte for byte, so both peers apply the same one.
			bool identical = !deferred.empty();
			for (const SoundContainer::PendingOp& op: deferred) {
				NetGameSoundOp command;
				command.actorUID = op.actorUID;
				command.team = op.team;
				command.soundIdentity = sound.GetCheckpointIdentity();
				command.op = static_cast<uint8_t>(op.op);
				command.property = static_cast<uint8_t>(op.property);
				command.player = op.player;
				command.value = op.value;
				command.x = op.x;
				command.y = op.y;
				command.soundSetPath = op.soundSetPath;
				NetLockstepFrame frame;
				frame.senderPeerId = 1;
				frame.targetFrame = 5;
				frame.commands = {NetGameCommand{1, command}};
				const NetLockstepPacket packet{frame};
				std::vector<uint8_t> bytes;
				if (!NetLockstepCodec::Encode(packet, bytes)) { identical = false; break; }
				const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(bytes);
				identical = identical && decoded.ok && decoded.packet == packet;
				if (!identical) break;
			}
			check("deferred_call_survives_the_wire", identical);
		}
		{
			SoundSimulationScope shared(4242, phase);
			sound.Play();
			advance(duration * 0.4);
			const std::string checkpoint = sound.SaveCheckpoint();
			MovableObject::FaithfulCloneScope privateObjects(false);
			SoundContainer copy;
			const bool loaded = copy.LoadCheckpoint(checkpoint);
			const bool liveAfterLoad = loaded && copy.IsBeingPlayed();
			advance(duration * 0.7);
			check("checkpoint_carries_playback", loaded && liveAfterLoad && !copy.IsBeingPlayed() && !sound.IsBeingPlayed() && copy.SaveCheckpoint() == sound.SaveCheckpoint());
			LogicalSoundVoice broken = copy.GetSharedLogicalVoices().empty() ? LogicalSoundVoice{} : copy.GetSharedLogicalVoices().front();
			broken.anchorPosition = static_cast<double>(broken.sampleFrames);
			LogicalSoundVoice probe;
			check("checkpoint_rejects_invalid_voice", !copy.GetSharedLogicalVoices().empty() && !probe.LoadCheckpoint(broken.SaveCheckpoint(), true));
			sound.Stop();
		}
		{
			SoundContainer ui;
			ui.Create(samplePath, true, false, SoundContainer::UI);
			SoundSimulationScope shared(4242, phase);
			const bool played = ui.Play();
			check("ui_bus_in_simulation_is_logical", played && ui.IsBeingPlayed() && !ui.GetSharedLogicalVoices().empty() && static_cast<int>(ui.GetSharedLogicalVoices().front().bus) == static_cast<int>(SoundContainer::UI));
			ui.Stop();
		}
		{
			SoundContainer other;
			other.Create(samplePath, false, true, SoundContainer::SFX);
			SoundSimulationScope shared(4243, phase);
			sound.Play();
			other.Play();
			advance(duration * 0.3);
			const long long now = g_TimerMan.GetSimTimeTicks();
			const bool bothLive = sound.GetSharedLogicalVoices().size() == 1 && other.GetSharedLogicalVoices().size() == 1;
			const LogicalSoundVoice::Progress first = bothLive ? sound.GetSharedLogicalVoices().front().At(now, ticksPerSecond) : LogicalSoundVoice::Progress{};
			const LogicalSoundVoice::Progress second = bothLive ? other.GetSharedLogicalVoices().front().At(now, ticksPerSecond) : LogicalSoundVoice::Progress{};
			check("identical_progress", bothLive && first.position == second.position && first.position > 0 && !first.finished);
			sound.Stop();
			other.Stop();
		}
	}
	{
		SoundExecutionKey withoutLocal, withLocal;
		{
			SoundSimulationScope parent(77, phase);
			SoundSimulationScope child(78, phase);
			withoutLocal = SoundSimulationScope::CurrentKey();
		}
		{
			SoundSimulationScope parent(77, phase);
			{
				SoundSimulationScope ai(79, phase, SoundExecutionDomain::LocalSimulation);
				SoundSimulationScope nested(80, phase);
				check("shared_inside_local_stays_local", SoundSimulationScope::Domain() == SoundExecutionDomain::LocalSimulation);
			}
			SoundSimulationScope child(78, phase);
			withLocal = SoundSimulationScope::CurrentKey();
		}
		check("local_scopes_keep_shared_keys", withoutLocal.occurrence != 0 && withoutLocal.occurrence == withLocal.occurrence && withoutLocal.objectUID == 78);
	}
	{
		ClearCommittedAudibility();
		SoundContainer keyed;
		keyed.Create(samplePath, false, true, SoundContainer::SFX);
		SoundExecutionKey identity;
		{
			SoundSimulationScope shared(4242, phase);
			keyed.Play();
			identity = keyed.GetSharedPlaybackIdentity();
		}
		NetSoundObservation reading;
		reading.objectUID = identity.objectUID; reading.tick = identity.tick; reading.phase = identity.phase; reading.occurrence = identity.occurrence; reading.ordinal = identity.ordinal;
		NetSoundObservation earlier = reading, later = reading, newest = reading;
		earlier.senderPeerId = 2; earlier.value = 0.75F;
		later.senderPeerId = 1; later.value = 0.25F;
		newest.senderPeerId = 2; newest.value = 0.5F;
		CommitSoundObservations(100, {earlier}, {});
		CommitSoundObservations(101, {}, {later});
		SoundSimulationScope shared(4242, phase);
		const bool latestWins = GetCommittedAudibility(keyed) == 0.25F;
		CommitSoundObservations(102, {}, {newest});
		check("committed_latest_reading", latestWins && GetCommittedAudibility(keyed) == 0.5F, std::to_string(GetCommittedAudibility(keyed)));
		const std::string checkpoint = SaveCheckpoint();
		ClearCommittedAudibility();
		const bool cleared = GetCommittedAudibility(keyed) == 0.0F && GetCommittedAudibilityCount() == 0;
		check("committed_table_checkpoint", cleared && LoadCheckpoint(checkpoint) && GetCommittedAudibility(keyed) == 0.5F && GetCommittedAudibilityCount() == 1);
		CommitSoundObservations(36600, {}, {});
		check("committed_table_prunes_stale", GetCommittedAudibilityCount() == 0 && GetCommittedAudibility(keyed) == 0.0F);
		keyed.Stop();
		ClearCommittedAudibility();
	}
		{
			// The call's number inside its tick derives the archived playback key, so a run that
			// restores mid-tick has to carry on from the same place rather than start again.
			SoundContainer keyed;
			keyed.Create(samplePath, false, true, SoundContainer::SFX);
			ClearCommittedAudibility();
			SetDeferredSoundOpOrdinal(0, 0);
			const uint64_t first = NextDeferredSoundOpOrdinal();
			const uint64_t second = NextDeferredSoundOpOrdinal();
			const std::string midTick = SaveCheckpoint();
			SoundExecutionKey beforeKey;
			{
				SoundSimulationScope drain(4242, phase, SoundExecutionDomain::SharedSimulation, NextDeferredSoundOpOrdinal());
				keyed.Play();
				beforeKey = keyed.GetSharedPlaybackIdentity();
				keyed.Stop();
			}
			// A process that had gone further, then restores the same tick.
			for (int extra = 0; extra < 9; ++extra) NextDeferredSoundOpOrdinal();
			const bool moved = GetDeferredSoundOpOrdinal().second != second;
			const bool loaded = LoadCheckpoint(midTick);
			const uint64_t restoredOrdinal = GetDeferredSoundOpOrdinal().second;
			SoundExecutionKey afterKey;
			{
				SoundSimulationScope drain(4242, phase, SoundExecutionDomain::SharedSimulation, NextDeferredSoundOpOrdinal());
				keyed.Play();
				afterKey = keyed.GetSharedPlaybackIdentity();
				keyed.Stop();
			}
			check("deferred_ordinal_survives_a_restore",
			      first == 1 && second == 2 && moved && loaded && restoredOrdinal == second &&
			          afterKey.occurrence == beforeKey.occurrence && afterKey.ordinal == beforeKey.ordinal && beforeKey.occurrence != 0,
			      "key " + std::to_string(beforeKey.occurrence) + "/" + std::to_string(afterKey.occurrence) +
			          " ordinal " + std::to_string(restoredOrdinal) + " loaded " + std::to_string(loaded ? 1 : 0));
			ClearCommittedAudibility();
			SetDeferredSoundOpOrdinal(0, 0);
		}
		{
			// Two AI actors on one container: whichever order the worker threads reach it in, the
			// drain hands the calls out in one order, by actor and by that actor's own call number.
			SoundContainer shared;
			shared.Create(samplePath, false, true, SoundContainer::SFX);
			const Entity* preset = g_PresetMan.GetEntityPreset("AHuman", "Green Dummy", "Base.rte");
			Actor* first = preset ? dynamic_cast<Actor*>(preset->Clone()) : nullptr;
			Actor* second = preset ? dynamic_cast<Actor*>(preset->Clone()) : nullptr;
			const auto drive = [&](Actor* a, Actor* b) {
				SoundSimulationScope local(4242, phase, SoundExecutionDomain::LocalSimulation);
				g_CurrentAIActor = a;
				shared.SetVolume(0.25F);
				g_CurrentAIActor = b;
				shared.SetVolume(0.5F);
				g_CurrentAIActor = a;
				shared.SetPitch(2.0F);
				g_CurrentAIActor = b;
				shared.SetPitch(4.0F);
				g_CurrentAIActor = nullptr;
			};
			const auto describe = [](const std::vector<SoundContainer::PendingOp>& ops) {
				std::string text;
				for (const SoundContainer::PendingOp& op: ops) {
					text += std::to_string(op.actorUID) + ":" + std::to_string(static_cast<int>(op.property)) + ":" + std::to_string(op.sequence) + " ";
				}
				return text;
			};
			std::string forward, reversed;
			if (first && second) {
				drive(first, second);
				forward = describe(shared.TakePendingSoundOps());
				drive(second, first);
				reversed = describe(shared.TakePendingSoundOps());
			}
			check("drained_order_is_deterministic", first && second && !forward.empty() && forward == reversed, forward);
			shared.Stop();
			delete first;
			delete second;
		}
		{
			// A record an older build wrote, taken from a save that build produced: the reader has to
			// take every field of the two-cohort shape and give back the same container.
			static const char* const legacy = "15 SoundContainer2 56 7 Entity1 11 Robot Death 11 Robot Death 0  0  0 0 100 0  9144 1 13 1 1 0 1123483648 1056964608 1061158912 -1 1 42 1 1096286208 3250716672 1056964608 1040187392 1048576000 0 0 0 0 282 21 LogicalSoundPlayback2 77 16 LogicalSoundKey1 1 1048577 2 10017187472382406708 15672542840968860955 12  1048577 1 161 18 LogicalSoundVoice1 77 16 LogicalSoundKey1 1 1048577 2 10017187472382406708 15672542840968860955 12  0 38976 0 38975 1194083328 1055794925 1 33332 0 -1 0 -1 0   65 21 LogicalSoundPlayback2 32 16 LogicalSoundKey1 0 0 0 0 0 0  0 0  63 19 LocalSoundControls1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0  ";
			SoundContainer restored;
			const bool loaded = restored.LoadCheckpoint(legacy);
			const bool version = SoundContainer::CheckpointVersion(legacy) == "SoundContainer2";
			const bool fields = loaded &&
			    restored.GetPresetName() == "Robot Death" &&
			    restored.GetSoundOverlapMode() == SoundContainer::RESTART &&
			    restored.GetBusRouting() == SoundContainer::UI &&
			    !restored.IsImmobile() &&
			    restored.GetAttenuationStartDistance() == 123.5F &&
			    restored.GetCustomPanValue() == 0.5F &&
			    restored.GetPanningStrengthMultiplier() == 0.75F &&
			    restored.GetLoopSetting() == -1 &&
			    restored.GetPriority() == 42 &&
			    restored.IsAffectedByGlobalPitch() &&
			    restored.GetPosition().m_X == 13.5F && restored.GetPosition().m_Y == -24.25F &&
			    restored.GetPitch() == 0.5F && restored.GetPitchVariation() == 0.125F &&
			    restored.GetVolume() == 0.25F && !restored.IsPaused() &&
			    restored.GetMusicPreEntryTime() == 0.0F && restored.GetMusicExitTime() == 0.0F &&
			    restored.GetCheckpointIdentity() == 9144 &&
			    restored.GetPlayingChannels()->size() == 1 && restored.GetPlayingChannels()->contains(13);
			const std::vector<LogicalSoundVoice>& voices = restored.GetSharedLogicalVoices();
			const bool playback = voices.size() == 1 && voices.front().sampleOrdinal == 0 && voices.front().sampleFrames == 38976 &&
			    voices.front().loopStart == 0 && voices.front().loopEnd == 38975 && voices.front().sampleRate == 44100.0F &&
			    voices.front().bus == 1 && voices.front().anchorTicks == 33332 && voices.front().anchorPosition == 0.0 &&
			    voices.front().loops == -1 && !voices.front().paused && voices.front().fadeStartTicks == -1 && voices.front().fadeSeconds == 0.0 &&
			    voices.front().origin.value.objectUID == 1048577 && voices.front().origin.value.ordinal == 12 &&
			    restored.GetSharedPlaybackIdentity().objectUID == 1048577 && restored.GetSharedPlaybackIdentity().ordinal == 12 &&
			    restored.GetSharedLogicalPlayback().lastObjectUID == 1048577;
			// What it writes now must read back the same, so nothing was dropped on the way through.
			SoundContainer again;
			const std::string current = restored.SaveCheckpoint();
			const bool round = again.LoadCheckpoint(current) && again.SaveCheckpoint() == current &&
			    SoundContainer::CheckpointVersion(current) == "SoundContainer3";
			check("legacy_sound_container_record_loads", version && loaded && fields && playback && round,
			      "version " + std::to_string(version ? 1 : 0) + " loaded " + std::to_string(loaded ? 1 : 0) +
			          " rate " + std::to_string(voices.empty() ? 0.0F : voices.front().sampleRate) +
			          " pitch " + std::to_string(voices.empty() ? 0.0F : voices.front().pitch) +
			          " fields " + std::to_string(fields ? 1 : 0) + " playback " + std::to_string(playback ? 1 : 0) +
			          " round " + std::to_string(round ? 1 : 0) + " preset " + restored.GetPresetName() +
			          " identity " + std::to_string(restored.GetCheckpointIdentity()) +
			          " voices " + std::to_string(restored.GetSharedLogicalVoices().size()) +
			          " channels " + std::to_string(restored.GetPlayingChannels()->size()));
		}
		{
			// The AI hook keeps a position from one pass and writes it in a later one, making no other
			// call on that container at all: shared state must not move until the drain, and the drain
			// must hand out a deferred call rather than settle the write itself.
			SoundContainer kept;
			kept.Create(samplePath, false, true, SoundContainer::SFX);
			Vector* alias = nullptr;
			{
				SoundSimulationScope shared(4242, phase);
				kept.SetPosition(Vector(10.0F, 20.0F));
			}
			{
				SoundSimulationScope local(4242, phase, SoundExecutionDomain::LocalSimulation);
				alias = const_cast<Vector*>(&kept.GetScriptPosition());
			}
			kept.TakePendingSoundOps();
			// The pass boundary: the shared hooks' writes land here, before any AI hook runs.
			SettleSharedSoundWrites();
			const bool sharedUntouchedBefore = kept.GetPosition().m_X == 10.0F;
			{
				SoundSimulationScope local(4242, phase, SoundExecutionDomain::LocalSimulation);
				alias->m_X = 55.0F;
			}
			const bool sharedUntouchedAfterPass = kept.GetPosition().m_X == 10.0F;
			std::vector<SoundContainer::PendingOp> drained = kept.TakePendingSoundOps();
			const bool deferred = drained.size() == 1 && drained.front().op == SoundContainer::PendingOp::SetProperty &&
			    drained.front().property == SoundContainer::PendingOp::PositionAlias && drained.front().x == 55.0F;
			const bool sharedUntouchedAtDrain = kept.GetPosition().m_X == 10.0F;
			bool applied = !drained.empty();
			{
				SoundSimulationScope drain(4242, phase, SoundExecutionDomain::SharedSimulation, NextDeferredSoundOpOrdinal());
				for (const SoundContainer::PendingOp& op: drained) applied = kept.ApplyPendingSoundOp(op) && applied;
			}
			check("ai_alias_only_write_defers",
			      sharedUntouchedBefore && sharedUntouchedAfterPass && deferred && sharedUntouchedAtDrain &&
			          applied && kept.GetPosition().m_X == 55.0F && kept.GetPosition().m_Y == 20.0F,
			      std::to_string(drained.size()) + " ops, x " + std::to_string(kept.GetPosition().m_X));
			// The same write made by a shared hook still lands where it always did, at once, and the
			// next pass boundary leaves it alone rather than taking it for the AI's.
			{
				SoundSimulationScope shared(4242, phase);
				alias = const_cast<Vector*>(&kept.GetScriptPosition());
				alias->m_X = 77.0F;
			}
			const bool settledAtOnce = kept.GetPosition().m_X == 77.0F;
			SettleSharedSoundWrites();
			check("shared_alias_only_write_settles",
			      settledAtOnce && kept.TakePendingSoundOps().empty() && kept.GetPosition().m_X == 77.0F,
			      std::to_string(kept.GetPosition().m_X));
			kept.Stop();
		}
	{
		// A reading the wire had to drop must be offered again, so the sound does not keep a stale
		// committed value until its audibility happens to move.
		const std::map<SoundObservationKey, float> sentBefore = m_LastSentAudibility;
		NetSoundObservation dropped;
		dropped.objectUID = 4242; dropped.tick = 7; dropped.phase = phase; dropped.occurrence = 0; dropped.ordinal = 3;
		dropped.value = 0.375F;
		NetSoundObservation kept2 = dropped;
		kept2.ordinal = 4;
		m_LastSentAudibility[KeyOf(dropped)] = dropped.value;
		m_LastSentAudibility[KeyOf(kept2)] = kept2.value;
		ForgetSentAudibility({dropped});
		check("dropped_reading_is_sampled_again",
		      !m_LastSentAudibility.contains(KeyOf(dropped)) && m_LastSentAudibility.contains(KeyOf(kept2)),
		      std::to_string(m_LastSentAudibility.size()));
		m_LastSentAudibility = sentBefore;
	}
	g_TimerMan.RestoreSimTickAfterPreview(simCount, simTicks);
	s_PlaybackSuppressed = suppressed;
	return passed;
}
