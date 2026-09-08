#pragma once

#include "Constants.h"
#include "Entity.h"
#include "Timer.h"
#include "Vector.h"
#include "Singleton.h"
#include "SoundContainerRegistry.h"
#include "SoundSimulation.h"
#include "LogicalSound.h"

#include "fmod/fmod.hpp"
#include <map>
#include <string_view>
#include <vector>
#include <utility>
#include "fmod/fmod_errors.h"

#define g_AudioMan AudioMan::Instance()

namespace RTE {

	class SoundContainer;
	struct SoundData;
	struct NetSoundObservation;

	/// The singleton manager of sound effect and music playback.
	class AudioMan : public Singleton<AudioMan> {
		friend class SettingsMan;
		friend class SoundContainer;
		friend struct GUISoundCheckpoint;

	public:
		std::string SaveCheckpoint() const;
		static std::string_view CheckpointVersion(std::string_view text) { return text.starts_with("13 AudioRuntime3 ") ? "AudioRuntime3" : (text.starts_with("13 AudioRuntime2 ") ? "AudioRuntime2" : "AudioRuntime1"); }
		bool LoadCheckpoint(std::string_view text, bool validateOnly = false, const std::vector<std::pair<SoundData*, std::string>>* sampleBindings = nullptr);
		std::string GetSoundContainerPlaybackCheckpoint(const SoundContainer* container) const;
		bool RunCheckpointSelfTest();
		void SetCheckpointTraceEnabled(bool enabled);
		void TraceCheckpointBoundary(const char* stage) const;
		bool PerturbCheckpointCursorForSelfTest();
		bool RunCheckpointPlaybackContinuationSelfTest() const;
		/// Drops finished logical voices at a tick boundary; liveness itself is a function of simulation time.
		void RetireFinishedSimulationSounds();
		/// Notes that a SoundContainer holds sound calls an AI hook made and the drain must visit it.
		void NotePendingSoundOps(SoundContainer* container);
		void ClearPendingSoundOps(SoundContainer* container);
		/// The containers with pending calls, in checkpoint-identity order so the drain is the same everywhere.
		std::vector<SoundContainer*> TakePendingSoundOpContainers();
		/// Numbers a deferred sound call inside its tick, so every peer derives the same playback key.
		uint64_t NextDeferredSoundOpOrdinal();
		/// The tick and call number a restored run has to continue from; both are checkpoint state.
		std::pair<uint64_t, uint64_t> GetDeferredSoundOpOrdinal() const { return {m_DeferredSoundOpTick, m_DeferredSoundOpOrdinal}; }
		void SetDeferredSoundOpOrdinal(uint64_t tick, uint64_t ordinal) { m_DeferredSoundOpTick = tick; m_DeferredSoundOpOrdinal = ordinal; }
		/// The live SoundContainer a checkpoint identity names, or none.
		SoundContainer* FindSimulationSoundContainer(uint64_t identity) const { return FindCheckpointSoundContainer(identity); }
		void VisitSharedSimulationSounds(const std::function<void(const SoundContainer&)>& visitor) const;
		float GetLocalSoundAudibility(const SoundContainer* container) const;
		bool RunLogicalPlaybackSelfTest();
		/// This peer's actual audibility of every live shared sound it answers for, sampled at the input boundary; only changed readings ride.
		std::vector<NetSoundObservation> SampleSoundObservations();
		/// Commits a tick's readings from every peer; the table is identical on all peers and pruned on the same frames.
		void CommitSoundObservations(uint64_t frame, const std::vector<NetSoundObservation>& local, const std::vector<NetSoundObservation>& remote);
		/// The reading a shared query returns: the controlling peer's, else the host's, else the latest committed, else 0.
		float GetCommittedAudibility(const SoundContainer& container) const;
		size_t GetCommittedAudibilityCount() const { return m_CommittedAudibility.size(); }
		void ClearCommittedAudibility();
		uint64_t GetCheckpointSoundContainerCursor() const { return m_NextSoundContainerIdentity; }
		void SetCheckpointSoundContainerCursor(uint64_t value) { m_NextSoundContainerIdentity = value; }
		CheckpointSoundRegistry CaptureCheckpointSoundRegistry() const { return m_CheckpointSoundContainers; }
		CheckpointSoundRegistry AddedCheckpointSoundRegistrations(const CheckpointSoundRegistry& original) const;
		void RestoreCheckpointSoundRegistry(CheckpointSoundRegistry original);
		void ActivateCheckpointSoundRegistrations(const CheckpointSoundRegistry& candidates);
		class CheckpointRegistryScope {
		public:
			CheckpointRegistryScope();
			~CheckpointRegistryScope();
			CheckpointRegistryScope(const CheckpointRegistryScope&) = delete;
			CheckpointRegistryScope& operator=(const CheckpointRegistryScope&) = delete;
		private:
			CheckpointSoundRegistry m_Original;
			uint64_t m_Cursor;
		};
		/// Hardcoded playback priorities for sounds. Note that sounds don't have to use these specifically; their priority can be anywhere between high and low.
		enum PlaybackPriority {
			PRIORITY_HIGH = 0,
			PRIORITY_NORMAL = 128,
			PRIORITY_LOW = 256
		};

		/// Music event states for sending music data from the server to clients during multiplayer games.
		enum NetworkMusicState {
			MUSIC_PLAY = 0,
			MUSIC_STOP,
			MUSIC_SILENCE,
			MUSIC_SET_PITCH
		};

		/// The data struct used to send music data from the server to clients during multiplayer games.
		struct NetworkMusicData {
			unsigned char State;
			char Path[256];
			int LoopsOrSilence;
			float Position;
			float Pitch;
		};

		/// Sound event states for sending sound data from the server to clients during multiplayer games.
		enum NetworkSoundState {
			SOUND_SET_GLOBAL_PITCH = 0,
			SOUND_PLAY,
			SOUND_STOP,
			SOUND_SET_POSITION,
			SOUND_SET_VOLUME,
			SOUND_SET_CUSTOMPANVALUE,
			SOUND_SET_PANNINGSTRENGTHMULTIPLIER,
			SOUND_SET_PITCH,
			SOUND_FADE_OUT
		};

		/// The data struct used to send sound data from the server to clients during multiplayer games.
		struct NetworkSoundData {
			unsigned char State;
			std::size_t SoundFileHash;
			int Channel;
			bool Immobile;
			float AttenuationStartDistance;
			float CustomPanValue;
			float PanningStrengthMultiplier;
			int Loops;
			int Priority;
			bool AffectedByGlobalPitch;
			float Position[2];
			float Volume;
			float Pitch;
			int FadeOutTime;
		};

#pragma region Creation
		/// Constructor method used to instantiate a AudioMan object in system memory.
		/// Create() should be called before using the object.
		AudioMan();

		/// Makes the AudioMan object ready for use.
		/// @return Whether the audio system was initialized successfully. If not, no audio will be available.
		bool Initialize();
#pragma endregion

#pragma region Destruction
		/// Destructor method used to clean up a AudioMan object before deletion from system memory.
		~AudioMan();

		/// Destroys and resets (through Clear()) the AudioMan object.
		void Destroy();
#pragma endregion

#pragma region Concrete Methods
		/// Updates the state of this AudioMan. Supposed to be done every frame before drawing.
		void Update();
#pragma endregion

#pragma region General Getters and Setters
		/// Gets the audio management system object used for playing all audio.
		/// @return The audio management system object used by AudioMan for playing audio.
		FMOD::System* GetAudioSystem() const { return m_AudioSystem; }

		/// Reports whether audio is enabled.
		/// @return Whether audio is enabled.
		bool IsAudioEnabled() const { return m_AudioEnabled; }
		/// True only after the explicit silent-output test flag is verified against FMOD.
		bool IsInaudibleTestOutputVerified() const { return m_InaudibleTestOutputVerified; }

		/// Gets the virtual and real playing channel counts, filling in the passed-in out-parameters.
		/// @param outVirtualChannelCount The out-parameter that will hold the virtual channel count.
		/// @param outRealChannelCount The out-parameter that will hold the real channel count.
		/// @return Whether or not the playing channel count was succesfully gotten.
		bool GetPlayingChannelCount(int* outVirtualChannelCount, int* outRealChannelCount) const { return m_AudioSystem->getChannelsPlaying(outVirtualChannelCount, outRealChannelCount) == FMOD_OK; }

		/// Returns the total number of virtual audio channels available.
		/// @return The number of virtual audio channels available.
		int GetTotalVirtualChannelCount() const { return c_MaxVirtualChannels; }

		/// Returns the total number of real audio channels available.
		/// @return The number of real audio channels available.
		int GetTotalRealChannelCount() const {
			int channelCount;
			return m_AudioSystem->getSoftwareChannels(&channelCount) == FMOD_OK ? channelCount : 0;
		}

		/// Gets whether all audio is muted or not.
		/// @return Whether all the audio is muted or not.
		/// Silences PlaySoundContainer while a presentation preview runs the sim ahead.
		static void SetPlaybackSuppressed(bool suppressed) { s_PlaybackSuppressed = suppressed; }
		static bool IsPlaybackSuppressed() { return s_PlaybackSuppressed; }
		static inline bool s_PlaybackSuppressed = false;

		bool GetMasterMuted() const { return m_MuteMaster; }

		/// Mutes or unmutes all audio.
		/// @param muteOrUnmute Whether to mute or unmute all the audio.
		void SetMasterMuted(bool muteOrUnmute = true) {
			m_MuteMaster = muteOrUnmute;
			if (m_AudioEnabled) {
				m_MasterChannelGroup->setMute(m_MuteMaster || m_OutputSilenced);
			}
		}

		/// Silences this process's output without touching the mute setting: sounds still play, end and callback as usual.
		/// A verified silent test backend is already inaudible, so it keeps its real gains and audibility.
		void SetOutputSilenced(bool silenced) {
			m_OutputSilenced = silenced && !m_InaudibleTestOutputVerified;
			if (m_AudioEnabled) {
				m_MasterChannelGroup->setMute(m_MuteMaster || m_OutputSilenced);
			}
		}

		/// Gets the volume of all audio. Does not get music or sounds individual volumes.
		/// @return Current volume scalar value. 0.0-1.0.
		float GetMasterVolume() const { return m_MasterVolume; }

		/// Sets all the audio to a specific volume. Does not affect music or sounds individual volumes.
		/// @param volume The desired volume scalar. 0.0-1.0.
		void SetMasterVolume(float volume = 1.0F) {
			m_MasterVolume = std::clamp(volume, 0.0F, 1.0F);
			if (m_AudioEnabled) {
				m_MasterChannelGroup->setVolume(m_MasterVolume);
			}
		}

		/// Gets whether to mute all audio when the game loses focus.
		/// @returns Whether to mute all audio when the game loses focus.
		bool GetMuteAudioOnFocusLoss() const { return m_MuteAudioOnFocusLoss; }

		/// Sets whether to mute all audio when the game loses focus.
		/// @param mute Whether to mute all audio when the game loses focus.
		void SetMuteAudioOnFocusLoss(bool mute) { m_MuteAudioOnFocusLoss = mute; }

		/// Gets the global pitch scalar value for all sounds and music.
		/// @return The current pitch scalar. Will be > 0.
		float GetGlobalPitch() const { return m_GlobalPitch; }

		/// Sets the global pitch multiplier for mobile sounds, optionally setting it for immobile sounds and music.
		/// @param pitch New global pitch, limited to 8 octaves up or down (i.e. 0.125 - 8). Defaults to 1.
		/// @param includeImmobileSounds Whether to include immobile sounds (normally used for GUI and so on) in global pitch modification. Defaults to false.
		/// @param includeMusic Whether to include the music in global pitch modification. Defaults to false.
		void SetGlobalPitch(float pitch = 1.0F, bool includeImmobileSounds = false, bool includeMusic = false);

		/// The strength of the sound panning effect.
		/// @return 0 - 1, where 0 is no panning and 1 is fully panned.
		float GetSoundPanningEffectStrength() const { return m_SoundPanningEffectStrength; }
#pragma endregion

#pragma region Music Getters and Setters
		/// Reports whether any music stream is currently playing.
		/// @return Whether any music stream is currently playing.
		bool IsMusicPlaying() const {
			bool isPlayingMusic;
			return m_AudioEnabled && m_MusicChannelGroup->isPlaying(&isPlayingMusic) == FMOD_OK ? isPlayingMusic : false;
		}

		/// Gets whether the music channel is muted or not.
		/// @return Whether the music channel is muted or not.
		bool GetMusicMuted() const { return m_MuteMusic; }

		/// Mutes or unmutes the music channel.
		/// @param muteOrUnmute Whether to mute or unmute the music channel.
		void SetMusicMuted(bool muteOrUnmute = true) {
			m_MuteMusic = muteOrUnmute;
			if (m_AudioEnabled) {
				m_MusicChannelGroup->setMute(m_MuteMusic);
			}
		}

		/// Gets the volume of music. Does not get volume of SFX or UI.
		/// @return Current volume scalar value. 0.0-1.0.
		float GetMusicVolume() const { return m_MusicVolume; }

		/// Sets the music to a specific volume. Does not affect SFX or UI.
		/// @param volume The desired volume scalar. 0.0-1.0.
		void SetMusicVolume(float volume = 1.0F) {
			m_MusicVolume = std::clamp(volume, 0.0F, 1.0F);
			if (m_AudioEnabled) {
				m_MusicChannelGroup->setVolume(m_MusicVolume);
			}
		}

		/// Sets/updates the frequency/pitch for the music channel.
		/// @param pitch New pitch, a multiplier of the original normal frequency. Keep it > 0.
		/// @return Whether the music channel's pitch was successfully updated.
		bool SetMusicPitch(float pitch);
#pragma endregion

#pragma region Overall Sound Getters and Setters
		/// Gets whether all the sound effects channels are muted or not.
		/// @return Whether all the sound effects channels are muted or not.
		bool GetSoundsMuted() const { return m_MuteSounds; }

		/// Mutes or unmutes all the sound effects channels.
		/// @param muteOrUnmute Whether to mute or unmute all the sound effects channels.
		void SetSoundsMuted(bool muteOrUnmute = true) {
			m_MuteSounds = muteOrUnmute;
			if (m_AudioEnabled) {
				// TODO: We may or may not wanna separate this out and add a UI sound slider
				m_SFXChannelGroup->setMute(m_MuteSounds);
				m_UIChannelGroup->setMute(m_MuteSounds);
			}
		}

		/// Gets the volume of all sounds. Does not get volume of music.
		/// @return Current volume scalar value. 0.0-1.0.
		float GetSoundsVolume() const { return m_SoundsVolume; }

		/// Sets the volume of all sounds to a specific volume. Does not affect music.
		/// @param volume The desired volume scalar. 0.0-1.0.
		void SetSoundsVolume(float volume = 1.0F) {
			m_SoundsVolume = volume;
			if (m_AudioEnabled) {
				m_SFXChannelGroup->setVolume(m_SoundsVolume);
				m_UIChannelGroup->setVolume(m_SoundsVolume);
			}
		}
#pragma endregion

#pragma region Global Playback and Handling
		/// Stops all playback.
		void StopAll();

		/// Makes all sounds that are looping stop looping, allowing them to play once more then be finished.
		void FinishIngameLoopingSounds();

		/// Pauses all ingame sounds.
		/// @param pause Whether to pause sounds or resume them.
		void PauseIngameSounds(bool pause = true);
#pragma endregion

#pragma region Music Handling
		/// Sets the music muffled state.
		/// @param musicMuffledState The new music muffled state to set.
		/// @return Whether the new music muffled state was successfully set.
		FMOD_RESULT SetMusicMuffledState(bool musicMuffledState);
#pragma endregion

#pragma region Lua Sound File Playing
		/// Starts playing a certain sound file.
		/// @param filePath The path to the sound file to play.
		/// @return The new SoundContainer being played. OWNERSHIP IS TRANSFERRED!
		SoundContainer* PlaySound(const std::string& filePath) { return PlaySound(filePath, Vector(), -1); }

		/// Starts playing a certain sound file at a certain position for all players.
		/// @param filePath The path to the sound file to play.
		/// @return The new SoundContainer being played. OWNERSHIP IS TRANSFERRED!
		SoundContainer* PlaySound(const std::string& filePath, const Vector& position) { return PlaySound(filePath, position, -1); }

		/// Starts playing a certain sound file at a certain position for a certain player.
		/// @param filePath The path to the sound file to play.
		/// @param position The position at which to play the SoundContainer's sounds.
		/// @param player Which player to play the SoundContainer's sounds for, -1 means all players.
		/// @return The new SoundContainer being played. OWNERSHIP IS TRANSFERRED!
		SoundContainer* PlaySound(const std::string& filePath, const Vector& position, int player);
#pragma endregion

#pragma region Network Audio Handling
		/// Returns true if manager is in multiplayer mode.
		/// @return True if in multiplayer mode.
		bool IsInMultiplayerMode() const { return m_IsInMultiplayerMode; }

		/// Sets the multiplayer mode flag.
		/// @param value Whether this manager should operate in multiplayer mode.
		void SetMultiplayerMode(bool value) { m_IsInMultiplayerMode = value; }

		/// Fills the list with sound events happened for the specified network player.
		/// @param player Player to get events for.
		/// @param list List with events for this player.
		void GetSoundEvents(int player, std::list<NetworkSoundData>& list);

		/// Adds the sound event to the internal list of sound events for the specified player.
		/// @param player Player(s) for which the event happened.
		/// @param state NetworkSoundState for the event.
		/// @param soundContainer A pointer to the SoundContainer this event is happening to, or a null pointer for global events.
		/// @param fadeOutTime THe amount of time, in MS, to fade out over. This data isn't contained in SoundContainer, so it needs to be passed in separately.
		void RegisterSoundEvent(int player, NetworkSoundState state, const SoundContainer* soundContainer, int fadeOutTime = 0);

		/// Clears the list of current Sound events for the target player.
		/// @param player Player to clear sound events for. -1 clears for all players.
		void ClearSoundEvents(int player);
#pragma endregion

	protected:
		const FMOD_VECTOR c_FMODForward = FMOD_VECTOR{0, 0, 1}; //!< An FMOD_VECTOR defining the Forwards direction. Necessary for 3D Sounds.
		const FMOD_VECTOR c_FMODUp = FMOD_VECTOR{0, 1, 0}; //!< An FMOD_VECTOR defining the Up direction. Necessary for 3D Sounds.

		FMOD::System* m_AudioSystem; //!< The FMOD Sound management object.
		FMOD::ChannelGroup* m_MasterChannelGroup; //!< The top-level FMOD ChannelGroup that holds everything.
		FMOD::ChannelGroup* m_SFXChannelGroup; //!< The FMOD ChannelGroup for diegetic gameplay sounds.
		FMOD::ChannelGroup* m_UIChannelGroup; //!< The FMOD ChannelGroup for UI sounds.
		FMOD::ChannelGroup* m_MusicChannelGroup; //!< The FMOD ChannelGroup for music.

		bool m_AudioEnabled; //!< Bool to tell whether audio is enabled or not.
		bool m_InaudibleTestOutputVerified = false;
		bool m_OutputSilenced = false; //!< Whether this process's output is silenced (a headless run); not a saved setting.
		std::vector<std::unique_ptr<const Vector>> m_CurrentActivityHumanPlayerPositions; //!< The stored positions of each human player in the current activity. Only filled when there's an activity running.
		std::unordered_map<int, float> m_SoundChannelMinimumAudibleDistances; //!<  An unordered map of sound channel indices to floats representing each Sound Channel's minimum audible distances. This is necessary to keep safe data in case the SoundContainer is destroyed while the sound is still playing, as happens often with TDExplosives.

		bool m_MuteMaster; //!< Whether all the audio is muted.
		bool m_MuteMusic; //!< Whether the music channel is muted.
		bool m_MuteSounds; //!< Whether all the sound effects channels are muted.
		bool m_MuteAudioOnFocusLoss; //!< Whether audio should be muted when the game loses focus.
		float m_MasterVolume; //!< Global volume of all audio.
		float m_MusicVolume; //!< Global music volume.
		float m_SoundsVolume; //!< Global sounds effects volume.
		float m_GlobalPitch; //!< Global pitch multiplier.

		float m_SoundPanningEffectStrength; //!< The strength of the sound panning effect, 0 (no panning) - 1 (full panning).

		//////////////////////////////////////////////////
		// TODO These need to be removed when our soundscape is sorted out. They're only here temporarily to allow for easier tweaking by pawnis.
		float m_ListenerZOffset;
		float m_MinimumDistanceForPanning;
		//////////////////////////////////////////////////

		bool m_MusicMuffled; //!< Whether the music bus is muffled.

		bool m_IsInMultiplayerMode; //!< If true then the server is in multiplayer mode and will register sound and music events into internal lists.
		std::list<NetworkSoundData> m_SoundEvents[c_MaxClients]; //!< Lists of per player sound events.

		std::mutex g_SoundEventsListMutex[c_MaxClients]; //!< A list for locking sound events for multiplayer to avoid race conditions and other such problems.
		std::mutex m_SoundChannelMinimumAudibleDistancesMutex; //!, As above but for m_SoundChannelMinimumAudibleDistances

	private:

		// Voice identities belong to the engine. Backend channel numbers never enter a checkpoint.
		struct PlayingVoice {
			FMOD::Channel* channel = nullptr;
			SoundContainer* owner = nullptr;
			std::string soundPath;
			float minimumAudibleDistance = 0;
			SoundExecutionDomain domain = SoundExecutionDomain::Presentation;
		};
		std::map<int, PlayingVoice> m_PlayingVoices;
		std::unordered_map<int, int> m_BackendVoiceIdentities;
		int m_NextVoiceIdentity = 0;
		CheckpointSoundRegistry m_CheckpointSoundContainers;
		std::unordered_map<const SoundContainer*, uint64_t> m_LiveCheckpointSoundContainers;
		uint64_t m_NextSoundContainerIdentity = 0;
		mutable std::mutex m_LogicalSoundsMutex;
		std::unordered_set<SoundContainer*> m_ActiveLogicalSounds;
		mutable std::mutex m_PendingSoundOpsMutex;
		std::map<uint64_t, SoundContainer*> m_PendingSoundOpContainers;
		uint64_t m_DeferredSoundOpTick = 0;
		uint64_t m_DeferredSoundOpOrdinal = 0;
		struct CommittedAudibility {
			uint64_t frame = 0;
			float value = 0.0F;
		};
		std::map<SoundObservationKey, std::map<uint8_t, CommittedAudibility>> m_CommittedAudibility;
		std::map<SoundObservationKey, float> m_LastSentAudibility;
		mutable std::mutex m_AudibilityMissMutex;
		mutable std::set<SoundObservationKey> m_ReportedAudibilityMisses; //!< Keys already reported by the CC_TRACE_AUDIBILITY diagnostic; empty unless it is on.
		static uint8_t AudibilityAuthority(uint64_t objectUID);
		FMOD_RESULT InitializeAudioSystem(bool silentOutput);
		void RefreshLogicalSound(SoundContainer* container);
		void UnregisterLogicalSound(SoundContainer* container);
		bool VoiceMatchesContext(int identity, const SoundContainer* owner) const;
		uint64_t AllocateCheckpointSoundContainerID();
		void RegisterCheckpointSoundContainer(SoundContainer* container, uint64_t identity);
		void UnregisterCheckpointSoundContainer(SoundContainer* container, uint64_t identity);
		SoundContainer* FindCheckpointSoundContainer(uint64_t identity) const;
		int RegisterPlayingVoice(FMOD::Channel* channel, SoundContainer* owner, const std::string& path, float minimumAudibleDistance);
		int FindVoiceIdentity(const FMOD::Channel* channel) const;
		FMOD_RESULT GetVoiceChannel(int voiceIdentity, FMOD::Channel** channel) const;
		bool OwnsVoice(int voiceIdentity, const SoundContainer* owner) const;
		void RetireVoice(int identity);
		bool MakeVoiceSlotAvailable();

#pragma region Sound Container Actions and Modifications
		/// Starts playing the next SoundSet of the given SoundContainer for the give player.
		/// @param soundContainer Pointer to the SoundContainer to start playing. Ownership is NOT transferred!
		/// @param player Which player to play the SoundContainer's sounds for, -1 means all players. Defaults to -1.
		/// @return Whether or not playback of the Sound was successful.
		bool PlaySoundContainer(SoundContainer* soundContainer, int player = -1);

		/// Sets/updates the position of a SoundContainer's playing sounds.
		/// @param soundContainer A pointer to a SoundContainer object. Ownership IS NOT transferred!
		/// @return Whether the position was successfully set.
		bool ChangeSoundContainerPlayingChannelsPosition(const SoundContainer* soundContainer);

		/// Sets/updates the position of a SoundContainer's playing sounds.
		/// @param soundContainer A pointer to a SoundContainer object. Ownership IS NOT transferred!
		/// @return Whether the position was successfully set.
		float GetSoundContainerAudibleVolume(const SoundContainer* soundContainer);

		/// Changes the volume of a SoundContainer's playing sounds.
		/// @param soundContainer A pointer to a SoundContainer object. Ownership IS NOT transferred!
		/// @param newVolume The new volume to play sounds at, between 0 and 1.
		/// @return Whether the volume was successfully updated.
		bool ChangeSoundContainerPlayingChannelsVolume(const SoundContainer* soundContainer, float newVolume);

		/// Changes the frequency/pitch of a SoundContainer's playing sounds.
		/// @param soundContainer A pointer to a SoundContainer object. Ownership IS NOT transferred!
		/// @return Whether the pitch was successfully updated.
		bool ChangeSoundContainerPlayingChannelsPitch(const SoundContainer* soundContainer);

		/// Updates the custom pan value of a SoundContainer's playing sounds.
		/// @param soundContainer A pointer to a SoundContainer object. Ownership IS NOT transferred!
		/// @return Whether the custom pan value was successfully updated.
		bool ChangeSoundContainerPlayingChannelsCustomPanValue(const SoundContainer* soundContainer);

		/// Stops playing a SoundContainer's playing sounds for a certain player.
		/// @param soundContainer A pointer to a SoundContainer object6. Ownership is NOT transferred!
		/// @param player Which player to stop playing the SoundContainer for.
		/// @return
		bool StopSoundContainerPlayingChannels(SoundContainer* soundContainer, int player);

		/// Clears the back-reference from a destroyed SoundContainer's still-playing channels so they don't dangle.
		/// @param soundContainer A pointer to the SoundContainer being destroyed. Ownership is NOT transferred!
		void DisownSoundContainerPlayingChannels(const SoundContainer* soundContainer);

		/// Fades out playback a SoundContainer.
		/// @param soundContainer A pointer to a SoundContainer object. Ownership is NOT transferred!
		/// @param fadeOutTime The amount of time, in ms, to fade out over.
		void FadeOutSoundContainerPlayingChannels(SoundContainer* soundContainer, int fadeOutTime);

		/// Pauses or unpauses a SoundContainer.
		/// @param soundContainer A pointer to a SoundContainer object. Ownership is NOT transferred!
		/// @param paused Whether to pause or unpause.
		void SetPausedSoundContainerPlayingChannels(SoundContainer* soundContainer, bool paused) const;
#pragma endregion

#pragma region 3D Effect Handling
		/// Updates 3D effects calculations for all sound channels whose SoundContainers isn't immobile.
		void Update3DEffectsForSFXChannels();

		/// Sets or updates the position of the given sound channel so it handles scene wrapping correctly. Also handles volume attenuation and minimum audible distance.
		/// @param soundChannel The channel whose position should be set or updated.
		/// @param positionToUse An optional position to set for this sound channel. Done this way to save setting and resetting data in FMOD.
		/// @return Whether the channel's position was succesfully set.
		FMOD_RESULT UpdatePositionalEffectsForSoundChannel(FMOD::Channel* soundChannel, const FMOD_VECTOR* positionToUse = nullptr) const;
#pragma endregion

#pragma region FMOD Callbacks
		/// A static callback function for FMOD to invoke when a sound channel finished playing. See fmod docs - FMOD_CHANNELCONTROL_CALLBACK for details
		static FMOD_RESULT F_CALLBACK SoundChannelEndedCallback(FMOD_CHANNELCONTROL* channelControl, FMOD_CHANNELCONTROL_TYPE channelControlType, FMOD_CHANNELCONTROL_CALLBACK_TYPE callbackType, void* commandData1, void* commandData2);
#pragma endregion

#pragma region Utility Methods
		/// Gets the corresponding FMOD_VECTOR for a given RTE Vector.
		/// @param vector The RTE Vector to get as an FMOD_VECTOR.
		/// @return The FMOD_VECTOR that corresponds to the given RTE Vector.
		FMOD_VECTOR GetAsFMODVector(const Vector& vector, float zValue = 0) const;

		/// Gets the corresponding RTE Vector for a given FMOD_VECTOR.
		/// @param fmodVector The FMOD_VECTOR to get as an RTE Vector.
		/// @return The RTE Vector that corresponds to the given FMOD_VECTOR.
		Vector GetAsVector(FMOD_VECTOR fmodVector) const;
#pragma endregion

		/// Clears all the member variables of this AudioMan, effectively resetting the members of this abstraction level only.
		void Clear();

		// Disallow the use of some implicit methods.
		AudioMan(const AudioMan& reference) = delete;
		AudioMan& operator=(const AudioMan& rhs) = delete;
	};
} // namespace RTE
