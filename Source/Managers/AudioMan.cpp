#include "AudioMan.h"
#include <cstdlib>
#include <cstring>
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
#include "HDFirearm.h"

#include <iostream>

#include <array>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

using namespace RTE;

AudioMan::AudioMan() {
	Clear();
}

AudioMan::~AudioMan() {
	Destroy();
}

void AudioMan::Clear() {
	m_InaudibleTestOutputVerified = false;
	m_AudioEnabled = false;
	m_PlayingVoices.clear();
	m_BackendVoiceIdentities.clear();
	m_NextVoiceIdentity = 0;
	m_CurrentActivityHumanPlayerPositions.clear();
	m_SoundChannelMinimumAudibleDistances.clear();

	m_MuteMaster = false;
	m_MuteMusic = false;
	m_MuteSounds = false;
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

bool AudioMan::Initialize() {
	FMOD_RESULT audioSystemSetupResult = FMOD::System_Create(&m_AudioSystem);
	const char* testOutput = std::getenv("CC_TEST_AUDIO_NOSOUND");
	const bool requireSilentTestOutput = testOutput && std::strcmp(testOutput, "1") == 0;
	m_InaudibleTestOutputVerified = false;
	if (requireSilentTestOutput) {
		m_OutputSilenced = true;
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
	if (requireSilentTestOutput) {
		FMOD_OUTPUTTYPE actualOutput = FMOD_OUTPUTTYPE_AUTODETECT;
		const FMOD_RESULT outputResult = audioSystemSetupResult == FMOD_OK ? m_AudioSystem->getOutput(&actualOutput) : audioSystemSetupResult;
		m_InaudibleTestOutputVerified = outputResult == FMOD_OK && actualOutput == FMOD_OUTPUTTYPE_NOSOUND;
		std::cout << "[audio-test-output] requested=NOSOUND verified=" << m_InaudibleTestOutputVerified << " output=" << static_cast<int>(actualOutput) << " result=" << static_cast<int>(outputResult) << std::endl;
		if (!m_InaudibleTestOutputVerified) audioSystemSetupResult = outputResult == FMOD_OK ? FMOD_ERR_OUTPUT_INIT : outputResult;
	}

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
	if (s_PlaybackSuppressed || !m_AudioEnabled || !soundContainer) {
		return false;
	}
	std::erase_if(soundContainer->m_PlayingChannels, [this, soundContainer](int identity) { return !OwnsVoice(identity, soundContainer); });
	if (soundContainer->m_PlayingChannels.size() >= c_MaxPlayingSoundsPerContainer) return false;
	FMOD_RESULT result = FMOD_OK;

	if (!soundContainer->SoundPropertiesUpToDate()) {
		result = soundContainer->UpdateSoundProperties();
		soundContainer->GetTopLevelSoundSet().SelectNextSounds();
		if (result != FMOD_OK) {
			g_ConsoleMan.PrintString("ERROR: Could not update sound properties for SoundContainer " + soundContainer->GetPresetName() + ": " + std::string(FMOD_ErrorString(result)));
			return false;
		}
	}

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
	for (const SoundData* soundData: selectedSoundData) {
		if (!MakeVoiceSlotAvailable()) return false;
		channel = nullptr; channelIndex = 0;
		result = (result == FMOD_OK) ? m_AudioSystem->playSound(soundData->SoundObject, channelGroupToPlayIn, true, &channel) : result;
		if (result == FMOD_OK) channelIndex = RegisterPlayingVoice(channel, soundContainer, soundData->SoundFile.GetDataPath(), soundData->MinimumAudibleDistance);
		if (result != FMOD_OK) return false;

		result = (result == FMOD_OK) ? channel->setUserData(soundContainer) : result;
		result = (result == FMOD_OK) ? channel->setCallback(SoundChannelEndedCallback) : result;
		result = (result == FMOD_OK) ? channel->setPriority(soundContainer->GetPriority()) : result;
		float pitchVariationMultiplier = pitchVariationFactor == 1.0F ? 1.0F : g_RenderRNG.RandomNum(1.0F / pitchVariationFactor, 1.0F * pitchVariationFactor);
		result = (result == FMOD_OK) ? channel->setPitch(soundContainer->GetPitch() * pitchVariationMultiplier) : result;

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
		if (!OwnsVoice(channelIndex, soundContainer)) continue;
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
		if (!OwnsVoice(channel, soundContainer)) continue;
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
		if (!OwnsVoice(channelIndex, soundContainer)) continue;
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
		if (!OwnsVoice(channelIndex, soundContainer)) continue;
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
		if (!OwnsVoice(channelIndex, soundContainer)) continue;
		result = GetVoiceChannel(channelIndex, &soundChannel);
		result = result == FMOD_OK ? soundChannel->setPan(soundContainer->GetCustomPanValue()) : result;
		if (result != FMOD_OK) {
			g_ConsoleMan.PrintString("ERROR: Could not update sound custom pan value for the sound being played on channel " + std::to_string(channelIndex) + " for SoundContainer " + soundContainer->GetPresetName() + ": " + std::string(FMOD_ErrorString(result)));
		}
	}
	return result == FMOD_OK;
}

bool AudioMan::StopSoundContainerPlayingChannels(SoundContainer* soundContainer, int player) {
	if (!m_AudioEnabled || !soundContainer || !soundContainer->IsBeingPlayed()) {
		return false;
	}
	if (m_IsInMultiplayerMode) {
		RegisterSoundEvent(player, SOUND_STOP, soundContainer);
	}

	FMOD_RESULT result = FMOD_OK;
	FMOD::Channel* soundChannel;

	const std::unordered_set<int>* channels = soundContainer->GetPlayingChannels();
	for (std::unordered_set<int>::const_iterator channelIterator = channels->begin(); channelIterator != channels->end();) {
		const int identity = *channelIterator;
		++channelIterator; // NOTE - stopping the sound will remove the channel, screwing things up if we don't move to the next iterator preemptively
		if (!OwnsVoice(identity, soundContainer)) continue;
		result = GetVoiceChannel(identity, &soundChannel);
		result = (result == FMOD_OK) ? soundChannel->stop() : result;
		if (result != FMOD_OK) {
			g_ConsoleMan.PrintString("Error: Failed to stop playing channel in SoundContainer " + soundContainer->GetPresetName() + ": " + std::string(FMOD_ErrorString(result)));
		}
	}
	return result == FMOD_OK;
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
	if (!m_AudioEnabled || !soundContainer || !soundContainer->IsBeingPlayed()) {
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
		if (!OwnsVoice(channel, soundContainer)) continue;
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
		if (!OwnsVoice(channelIndex, soundContainer)) continue;
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
}

void AudioMan::UnregisterCheckpointSoundContainer(SoundContainer* container, uint64_t identity) {
	m_LiveCheckpointSoundContainers.erase(container);
	auto found = m_CheckpointSoundContainers.find(identity);
	if (found == m_CheckpointSoundContainers.end()) return;
	std::erase(found->second, container);
	if (found->second.empty()) m_CheckpointSoundContainers.erase(found);
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

int AudioMan::RegisterPlayingVoice(FMOD::Channel* channel, SoundContainer* owner, const std::string& path, float minimumAudibleDistance) {
	do {
		if (m_NextVoiceIdentity == std::numeric_limits<int>::max()) m_NextVoiceIdentity = 0;
		++m_NextVoiceIdentity;
	} while (m_PlayingVoices.contains(m_NextVoiceIdentity));
	int backend;
	if (channel->getIndex(&backend) != FMOD_OK) throw std::runtime_error("could not identify playing audio channel");
	m_BackendVoiceIdentities[backend] = m_NextVoiceIdentity;
	m_PlayingVoices.emplace(m_NextVoiceIdentity, PlayingVoice{channel, owner, path, minimumAudibleDistance});
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
		template <class Archive> void Fields(Archive& archive) {
			archive(enabled, nextVoice, nextSoundContainer, muteMaster, muteMusic, muteSounds, muteOnFocusLoss, masterVolume, musicVolume, soundsVolume, globalPitch, panning, listenerZ, minimumPanning, musicMuffled, multiplayer, playerPositions, listeners, groups, samples, voices, minimumDistances, events);
		}
		std::string Save() { CheckpointWriter archive("AudioRuntime1"); Fields(archive); return archive.Text(); }
		bool Load(std::string_view text) {
			try {
				CheckpointReader archive(text, "AudioRuntime1"); Fields(archive); archive.Finish();
				if (nextVoice < 0 || voices.size() > c_MaxVirtualChannels || listeners.size() > 8 || (enabled && listeners.empty())) return false;
				for (float value: {masterVolume, musicVolume, soundsVolume, globalPitch, panning, listenerZ, minimumPanning}) if (!AudioCheckpoint::Finite(value)) return false;
				std::set<int> voiceIDs; for (const auto& voice: voices) if (!voiceIDs.insert(voice.identity).second || voice.owner > nextSoundContainer) return false;
				for (const auto& [identity, distance]: minimumDistances) if (!voiceIDs.contains(identity) || !AudioCheckpoint::Finite(distance)) return false;
				std::set<std::string> paths; for (const auto& sample: samples) if (!paths.insert(sample.path).second) return false;
				for (const auto& voice: voices) if (voice.playing && !paths.contains(voice.path)) return false;
				return true;
			} catch (const std::exception&) { return false; }
		}
	};
}

std::string AudioMan::SaveCheckpoint() const {
	AudioRuntime state;
	state.enabled = m_AudioEnabled; state.nextVoice = m_NextVoiceIdentity;
	state.nextSoundContainer = m_NextSoundContainerIdentity;
	state.muteMaster = m_MuteMaster; state.muteMusic = m_MuteMusic; state.muteSounds = m_MuteSounds; state.muteOnFocusLoss = m_MuteAudioOnFocusLoss;
	state.masterVolume = m_MasterVolume; state.musicVolume = m_MusicVolume; state.soundsVolume = m_SoundsVolume; state.globalPitch = m_GlobalPitch;
	state.panning = m_SoundPanningEffectStrength; state.listenerZ = m_ListenerZOffset; state.minimumPanning = m_MinimumDistanceForPanning;
	state.musicMuffled = m_MusicMuffled; state.multiplayer = m_IsInMultiplayerMode;
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
		for (const auto& [identity, voice]: m_PlayingVoices) {
			int bus = 0;
			FMOD::ChannelGroup* group = nullptr;
			if (voice.channel && voice.channel->getChannelGroup(&group) == FMOD_OK) bus = group == m_UIChannelGroup ? 1 : group == m_MusicChannelGroup ? 2 : 0;
			state.voices.push_back(AudioCheckpoint::Voice::Capture(identity, voice.owner ? voice.owner->GetCheckpointIdentity() : 0, voice.soundPath, voice.minimumAudibleDistance, voice.channel, bus));
		}
		TraceCheckpointBoundary("save-captured");
	}
	return state.Save();
}

bool AudioMan::LoadCheckpoint(std::string_view text, bool validateOnly, const std::vector<std::pair<SoundData*, std::string>>* sampleBindings) {
	AudioRuntime state;
	if (!state.Load(text)) return false;
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
		for (const auto& voice: state.voices) {
			SoundContainer* owner = voice.owner ? FindCheckpointSoundContainer(voice.owner) : nullptr;
			if (voice.owner && !owner) throw std::runtime_error("voice " + std::to_string(voice.identity) + " has no registered owner " + std::to_string(voice.owner));
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
		m_MuteMaster = state.muteMaster; m_MuteMusic = state.muteMusic; m_MuteSounds = state.muteSounds; m_MuteAudioOnFocusLoss = state.muteOnFocusLoss;
		m_MasterVolume = state.masterVolume; m_MusicVolume = state.musicVolume; m_SoundsVolume = state.soundsVolume; m_GlobalPitch = state.globalPitch;
		m_SoundPanningEffectStrength = state.panning; m_ListenerZOffset = state.listenerZ; m_MinimumDistanceForPanning = state.minimumPanning;
		m_MusicMuffled = state.musicMuffled; m_IsInMultiplayerMode = state.multiplayer;
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
		AudioRuntime invalid;
		if (!invalid.Load(checkpoint)) throw std::runtime_error("could not parse generated audio checkpoint");
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
