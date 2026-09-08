#pragma once

#include "Entity.h"
#include "AudioMan.h"
#include "LogicalSound.h"

#include <unordered_set>

namespace RTE {
	class Vector;
	struct SoundData;
	class SoundSet;

	/// A container for sounds that represent a specific sound effect.
	class SoundContainer : public Entity {
		friend struct MusicCheckpoint;
		friend struct GUISoundCheckpoint;
		friend class AudioMan;
		friend struct ContractAudit;


	public:
		std::string SaveCheckpoint() const;
		bool LoadCheckpoint(std::string_view text, bool validateOnly = false);
		static std::string_view CheckpointVersion(std::string_view text) { return text.starts_with("15 SoundContainer2 ") ? "SoundContainer2" : "SoundContainer1"; }
		uint64_t GetCheckpointIdentity() const { return m_CheckpointIdentity; }
		const SoundExecutionKey& GetSharedPlaybackIdentity() const { return m_LogicalPlayback[0].identity.value; }
		const LogicalSoundPlayback& GetSharedLogicalPlayback() const { return m_LogicalPlayback[0]; }
		const std::vector<LogicalSoundVoice>& GetSharedLogicalVoices() const { return m_LogicalPlayback[0].voices; }
		/// Every sound played from simulation code is logical whatever bus carries it: a mod can route a gameplay sound to the UI bus and still query it from shared simulation.
		bool UsesLogicalPlayback() const { return SoundSimulationScope::IsSimulation(); }
		EntityAllocation(SoundContainer);
		SerializableOverrideMethods;
		ClassInfoGetters;

		/// The FMOD channelgroup/bus this sound routes through.
		enum BusRouting {
			SFX = 0, // Default diegetic bus for general game SFX.
			UI = 1, // Menu sounds and other things that shouldn't be affected by diegetic sound processing.
			MUSIC = 2 // Self-explanatory music bus.
		};

		/// How the SoundContainer should behave when it tries to play again while already playing.
		enum SoundOverlapMode {
			OVERLAP = 0,
			RESTART = 1,
			IGNORE_PLAY = 2
		};

#pragma region Creation
		/// Constructor method used to instantiate a SoundContainer object in system memory. Create() should be called before using the object.
		SoundContainer();

		/// Copy constructor method used to instantiate a SoundContainer object identical to an already existing one.
		/// @param reference A reference to the SoundContainer to deep copy.
		SoundContainer(const SoundContainer& reference);
		SoundContainer& operator=(const SoundContainer& reference);

		/// Creates a SoundContainer to be identical to another, by deep copy.
		/// @param reference A reference to the SoundContainer to deep copy.
		/// @return An error return value signaling success or any particular failure. Anything below 0 is an error signal.
		int Create(const SoundContainer& reference);

		/// Creates a SoundContainer and adds a sound, optionally setting immobility, being affected by global pitch, and bus routing.
		/// @param soundFilePath The path to a sound to add to the first SoundSet of this SoundContainer.
		/// @param immobile Whether this SoundContainer's sounds will be treated as immobile, i.e. they won't be affected by 3D sound manipulation.
		/// @param affectedByGlobalPitch Whether this SoundContainer's sounds' frequency will be affected by the global pitch.
		/// @param busRouting Bus to route this sound to.
		/// @return An error return value signaling success or any particular failure. Anything below 0 is an error signal.
		int Create(const std::string& soundFilePath, bool immobile = false, bool affectedByGlobalPitch = true, BusRouting busRouting = BusRouting::SFX);
#pragma endregion

#pragma region Destruction
		/// Destructor method used to clean up a SoundContainer object before deletion from system memory.
		~SoundContainer() override;

		/// Destroys and resets (through Clear()) the SoundContainer object. It doesn't delete the Sound files, since they're owned by ContentFile static maps.
		/// @param notInherited Whether to only destroy the members defined in this derived class, or to destroy all inherited members also.
		void Destroy(bool notInherited = false) override {
			if (!notInherited) {
				Entity::Destroy();
			}
			Clear();
		}

		/// Resets the entire SoundContainer, including its inherited members, to their default settings or values.
		void Reset() override {
			Clear();
			Entity::Reset();
		}
#pragma endregion

#pragma region Sound Management Getters and Setters
		/// Shows whether this SoundContainer's top level SoundSet has any SoundData or SoundSets.
		/// @return Whether this SoundContainer has any sounds.
		bool HasAnySounds() const;

		enum class LengthOfSoundType {
			Any,
			NextPlayed
		};

		/// Gets the length of the sound in this sound container.
		/// @param type Whether to get the length of the next played sound, or the maximum length of any sound in this container.
		/// @return The length of this sound, in ms.
		float GetLength(LengthOfSoundType type) const;

		/// Gets a reference to the top level SoundSet of this SoundContainer, to which all SoundData and sub SoundSets belong.
		/// @return A reference to the top level SoundSet of this SoundContainer.
		SoundSet& GetTopLevelSoundSet() { return *m_TopLevelSoundSet; }

		/// Copies the passed in SoundSet reference into the top level SoundSet of this SoundContainer, effectively making that the new top level SoundSet.
		/// @param newTopLevelSoundSet A reference to the new top level SoundSet for this SoundContainer.
		void SetTopLevelSoundSet(const SoundSet& newTopLevelSoundSet);

		/// Gets a vector of hashes of the sounds selected to be played next in this SoundContainer.
		/// @return The currently playing sounds hashes.
		std::vector<std::size_t> GetSelectedSoundHashes() const;

		/// Gets the SoundData object that corresponds to the given FMOD::Sound. If the sound can't be found, it returns a null pointer.
		/// @param sound The FMOD::Sound to search for.
		/// @return A pointer to the corresponding SoundData or a null pointer.
		const SoundData* GetSoundDataForSound(const FMOD::Sound* sound) const;

		/// Gets the channels playing sounds from this SoundContainer.
		/// @return The channels currently being used.
		std::unordered_set<int> const* GetPlayingChannels() const { return &m_PlayingChannels; }

		/// Indicates whether any sound in this SoundContainer is currently being played.
		/// @return Whether any sounds are playing.
		bool IsBeingPlayed() const;

		/// Adds a channel index to the SoundContainer's collection of playing channels.
		/// @param channel The channel index to add.
		void AddPlayingChannel(int channel) { m_PlayingChannels.insert(channel); }

		/// Removes a channel index from the SoundContainer's collection of playing channels.
		/// @param channel The channel index to remove.
		void RemovePlayingChannel(int channel) { m_PlayingChannels.erase(channel); }

		/// Gets the SoundOverlapMode of this SoundContainer, which is used to determine how it should behave when it's told to play while already playing.
		/// @return The SoundOverlapMode of this SoundContainer.
		SoundOverlapMode GetSoundOverlapMode() const { return CurrentSoundOverlapMode(); }

		/// Sets the SoundOverlapMode of this SoundContainer, which is used to determine how it should behave when it's told to play while already playing.
		/// @param newSoundOverlapMode The new SoundOverlapMode this SoundContainer should use.
		void SetSoundOverlapMode(SoundOverlapMode newSoundOverlapMode) { CurrentSoundOverlapMode() = newSoundOverlapMode; }
#pragma endregion

#pragma region Sound Property Getters and Setters

		/// Gets the bus this sound routes to.
		/// @return The bus this sound routes to.
		BusRouting GetBusRouting() const { return CurrentBusRouting(); }

		/// Sets the bus this sound routes to.
		/// @param newBusRoute The new bus for this sound to route to.
		void SetBusRouting(BusRouting newBusRoute) { CurrentBusRouting() = newBusRoute; }

		/// Gets whether the sounds in this SoundContainer should be considered immobile, i.e. always play at the listener's position.
		/// @return Whether or not the sounds in this SoundContainer are immobile.
		bool IsImmobile() const { return CurrentImmobile(); }

		/// Sets whether the sounds in this SoundContainer should be considered immobile, i.e. always play at the listener's position. Does not affect currently playing sounds.
		/// @param immobile The new immobile setting.
		void SetImmobile(bool immobile) {
			CurrentImmobile() = immobile;
			CurrentSoundPropertiesUpToDate() = false;
		}

		/// Gets the attenuation start distance of this SoundContainer.
		/// @return A float with the attenuation start distance.
		float GetAttenuationStartDistance() const { return CurrentAttenuationStartDistance(); }

		/// Sets the attenuation start distance of this SoundContainer. Values < 0 set it to default. Does not affect currently playing sounds.
		/// @param attenuationStartDistance The new attenuation start distance.
		void SetAttenuationStartDistance(float attenuationStartDistance) {
			CurrentAttenuationStartDistance() = (attenuationStartDistance < 0) ? c_DefaultAttenuationStartDistance : attenuationStartDistance;
			CurrentSoundPropertiesUpToDate() = false;
		}

		/// Gets the custom pan value of this SoundContainer.
		/// @return A float with the custom pan value.
		float GetCustomPanValue() const { return CurrentCustomPanValue(); }

		/// Sets the custom pan value of this SoundContainer. Clamped between -1 and 1.
		/// @param customPanValue The new custom pan value.
		void SetCustomPanValue(float customPanValue);

		/// Gets the panning strength multiplier of this SoundContainer.
		/// @return A float with the panning strength multiplier.
		float GetPanningStrengthMultiplier() const { return CurrentPanningStrengthMultiplier(); }

		/// Sets the panning strength multiplier of this SoundContainer.
		/// @param panningStrengthMultiplier The new panning strength multiplier.
		void SetPanningStrengthMultiplier(float panningStrengthMultiplier) {
			CurrentPanningStrengthMultiplier() = panningStrengthMultiplier;
			CurrentSoundPropertiesUpToDate() = false;
		}

		/// Gets the looping setting of this SoundContainer.
		/// @return An int with the loop count.
		int GetLoopSetting() const { return CurrentLoops(); }

		/// Sets the looping setting of this SoundContainer. Does not affect currently playing sounds.
		/// 0 means the sound is set to only play once. -1 means it loops indefinitely.
		/// @param loops The new loop count.
		void SetLoopSetting(int loops) {
			CurrentLoops() = loops;
			CurrentSoundPropertiesUpToDate() = false;
		}

		/// Gets whether the sounds in this SoundContainer have all had all their properties set appropriately. Used to account for issues with ordering in INI loading.
		/// @return Whether or not the sounds in this SoundContainer have their properties set appropriately.
		bool SoundPropertiesUpToDate() const { return CurrentSoundPropertiesUpToDate(); }

		/// Gets the current playback priority.
		/// @return The playback priority.
		int GetPriority() const { return CurrentPriority(); }

		/// Sets the current playback priority. Higher priority (lower value) will make this more likely to make it into mixing on playback. Does not affect currently playing sounds.
		/// @param priority The new priority. See AudioMan::PRIORITY_* enumeration.
		void SetPriority(int priority) { CurrentPriority() = std::clamp(priority, 0, 256); }

		/// Gets whether the sounds in this SoundContainer are affected by global pitch changes or not.
		/// @return Whether or not the sounds in this SoundContainer are affected by global pitch changes.
		bool IsAffectedByGlobalPitch() const { return CurrentAffectedByGlobalPitch(); }

		/// Sets whether the sounds in this SoundContainer are affected by global pitch changes or not. Does not affect currently playing sounds.
		/// @param affectedByGlobalPitch The new affected by global pitch setting.
		void SetAffectedByGlobalPitch(bool affectedByGlobalPitch) { CurrentAffectedByGlobalPitch() = affectedByGlobalPitch; }

		/// Gets the position at which this SoundContainer's sound will be played. Note that its individual sounds can be offset from this.
		/// @return The position of this SoundContainer.
		const Vector& GetPosition() const { return CurrentPos(); }
		/// Lua receives the position by value: with copy-on-write local controls a reference could name shared state on one peer and this machine's copy on another.
		Vector GetLuaPosition() const { return CurrentPos(); }

		/// Sets the position of the SoundContainer's sounds while they're playing.
		/// @param position The new position to play the SoundContainer's sounds.
		/// @return Whether this SoundContainer's attenuation setting was successful.
		void SetPosition(const Vector& newPosition);

		/// Gets the real audible volume sounds in this SoundContainer are currently playing at, if any. Accounts for literally everything, including game volume.
		/// @return The real audible volume the sounds in this SoundContainer are played at.
		float GetAudibleVolume() const;

		/// Gets the volume the sounds in this SoundContainer are played at. Note that this does not factor volume changes due to the SoundContainer's position.
		/// @return The volume the sounds in this SoundContainer are played at.
		float GetVolume() const { return CurrentVolume(); }

		/// Sets the volume sounds in this SoundContainer should be played at. Note that this does not factor volume changes due to the SoundContainer's position. Does not affect currently playing sounds.
		/// @param newVolume The new volume sounds in this SoundContainer should be played at. Limited between 0 and 10.
		void SetVolume(float newVolume);

		/// Gets the pitch the sounds in this SoundContainer are played at. Note that this does not factor in global pitch.
		/// @return The pitch the sounds in this SoundContainer are played at.
		float GetPitch() const { return CurrentPitch(); }

		/// Sets the pitch sounds in this SoundContainer should be played at and updates any playing instances accordingly.
		/// @param newPitch The new pitch sounds in this SoundContainer should be played at. Limited between 0.125 and 8 (8 octaves up or down).
		void SetPitch(float newPitch);

		/// Gets the pitch variation the sounds in this SoundContainer are played at.
		/// @return The pitch variation the sounds in this SoundContainer are played at.
		float GetPitchVariation() const { return CurrentPitchVariation(); }

		/// Sets the pitch variation the sounds in this SoundContainer are played at.
		/// @param newValue The pitch variation the sounds in this SoundContainer are played at.
		void SetPitchVariation(float newValue) { CurrentPitchVariation() = newValue; }

		/// Gets whether this SoundContainer's channels are paused or not.
		/// @return Whether this SoundContainer's channels are paused or not.
		bool IsPaused() const { return CurrentPaused(); }

		/// Sets whether this SoundContainer's channels are paused or not.
		/// @param paused The new paused setting.
		void SetPaused(bool paused);

		/// Gets the music pre-entry time for this SoundContainer.
		/// @return The time before the music starts in this SoundContainer in MS.
		float GetMusicPreEntryTime() const { return CurrentMusicPreEntryTime(); }

		/// Sets the music pre-entry time for this SoundContainer.
		/// @param newValue The new MusicPreEntryTime for this SoundContainer in MS.
		void SetMusicPreEntryTime(float newValue) { CurrentMusicPreEntryTime() = newValue; }

		/// Gets the music post-exit time for this SoundContainer.
		/// @return The time after the music ends in this SoundContainer in MS.
		float GetMusicExitTime() const { return CurrentMusicExitTime(); }

		/// Sets the music post-exit time for this SoundContainer.
		/// @param newValue The new MusicExitTime for this SoundContainer in MS.
		void SetMusicExitTime(float newValue) { CurrentMusicExitTime() = newValue; }
#pragma endregion

#pragma region Playback Controls
		/// Plays the next sound of this SoundContainer at its current position for all players.
		/// @return Whether this SoundContainer successfully started playing on any channels.
		bool Play() { return Play(-1); }

		/// Plays the next sound of this container at its current position.
		/// @param player The player to start playback of this SoundContainer's sounds for.
		/// @return Whether there were sounds to play and they were able to be played.
		bool Play(int player);

		/// Plays the next sound of this SoundContainer at the given position for all players.
		/// @param position The position at which to play the SoundContainer's sounds.
		/// @return Whether this SoundContainer successfully started playing on any channels.
		bool Play(const Vector& position) { return Play(position, -1); }

		/// Plays the next sound of this SoundContainer with the given attenuation for a specific player.
		/// @param position The position at which to play the SoundContainer's sounds.
		/// @param player The player to start playback of this SoundContainer's sounds for.
		/// @return Whether this SoundContainer successfully started playing on any channels.
		bool Play(const Vector& position, int player) {
			SetPosition(position);
			return Play(player);
		}

		/// Stops playback of this SoundContainer for all players.
		/// @return Whether this SoundContainer successfully stopped playing.
		bool Stop() { return Stop(-1); }

		/// Stops playback of this SoundContainer for a specific player.
		/// @param player Player to stop playback of this SoundContainer for.
		/// @return Whether this SoundContainer successfully stopped playing.
		bool Stop(int player);

		/// Restarts playback of this SoundContainer for all players.
		/// @return Whether this SoundContainer successfully restarted its playback.
		bool Restart() { return Restart(-1); }

		/// Restarts playback of this SoundContainer for a specific player.
		/// @param player Player to restart playback of this SoundContainer for.
		/// @return Whether this SoundContainer successfully restarted its playback.
		bool Restart(int player);

		/// Fades out playback of the SoundContainer to 0 volume.
		/// @param fadeOutTime How long the fadeout should take.
		void FadeOut(int fadeOutTime = 1000);
#pragma endregion

#pragma region Miscellaneous
		/// Updates all sound properties to match this SoundContainer's settings.
		/// Necessary because sounds loaded from ini seem to be used directly instead of loaded from PresetMan, so their correctness can't be guaranteed when they're played.
		/// @return The FMOD_RESULT for updating all of the SoundContainer's sounds' properties. If it's not FMOD_OK, something went wrong.
		FMOD_RESULT UpdateSoundProperties();
#pragma endregion

	private:
		static Entity::ClassInfo m_sClass; //!< ClassInfo for this class.
		static const std::unordered_map<std::string, SoundOverlapMode> c_SoundOverlapModeMap; //!< A map of strings to SoundOverlapModes to support string parsing for the SoundOverlapMode enum. Populated in the implementing cpp file.
		static const std::unordered_map<std::string, BusRouting> c_BusRoutingMap; //!< A map of strings to BusRoutings to support string parsing for the BusRouting enum. Populated in the implementing cpp file.


		/// Local AI writes land on a private copy of a control; everything else reads and writes the shared one.
		struct LocalControls {
			uint32_t written = 0;
			SoundOverlapMode m_SoundOverlapMode{};
			BusRouting m_BusRouting{};
			bool m_Immobile{};
			float m_AttenuationStartDistance{};
			float m_CustomPanValue{};
			float m_PanningStrengthMultiplier{};
			int m_Loops{};
			bool m_SoundPropertiesUpToDate{};
			int m_Priority{};
			bool m_AffectedByGlobalPitch{};
			Vector m_Pos{};
			float m_Pitch{};
			float m_PitchVariation{};
			float m_Volume{};
			bool m_WasFadedOut{};
			bool m_Paused{};
			float m_MusicPreEntryTime{};
			float m_MusicExitTime{};
			template<class Archive> void Fields(Archive& archive) { archive(written, m_SoundOverlapMode, m_BusRouting, m_Immobile, m_AttenuationStartDistance, m_CustomPanValue, m_PanningStrengthMultiplier, m_Loops, m_SoundPropertiesUpToDate, m_Priority, m_AffectedByGlobalPitch, m_Pos, m_Pitch, m_PitchVariation, m_Volume, m_WasFadedOut, m_Paused, m_MusicPreEntryTime, m_MusicExitTime); }
			std::string SaveCheckpoint() const;
			bool LoadCheckpoint(std::string_view text, bool validateOnly = false);
		};
		enum LocalControl : uint32_t {
			LocalOverlapMode = 1u << 0, LocalBusRouting = 1u << 1, LocalImmobile = 1u << 2, LocalAttenuationStartDistance = 1u << 3, LocalCustomPanValue = 1u << 4,
			LocalPanningStrengthMultiplier = 1u << 5, LocalLoops = 1u << 6, LocalSoundPropertiesUpToDate = 1u << 7, LocalPriority = 1u << 8, LocalAffectedByGlobalPitch = 1u << 9,
			LocalPos = 1u << 10, LocalPitch = 1u << 11, LocalPitchVariation = 1u << 12, LocalVolume = 1u << 13, LocalWasFadedOut = 1u << 14, LocalPaused = 1u << 15,
			LocalMusicPreEntryTime = 1u << 16, LocalMusicExitTime = 1u << 17, LocalControlCount = 18
		};
		LocalControls m_LocalControls;
		std::array<LogicalSoundPlayback, 2> m_LogicalPlayback;

		static bool InLocalSoundContext() { return SoundSimulationScope::Domain() == SoundExecutionDomain::LocalSimulation; }
		template<class T> const T& Control(const T& shared, T LocalControls::*member, uint32_t field) const { return InLocalSoundContext() && (m_LocalControls.written & field) ? m_LocalControls.*member : shared; }
		template<class T> T& Control(T& shared, T LocalControls::*member, uint32_t field) {
			if (!InLocalSoundContext()) return shared;
			if (!(m_LocalControls.written & field)) {
				m_LocalControls.*member = shared;
				m_LocalControls.written |= field;
			}
			return m_LocalControls.*member;
		}
		const SoundOverlapMode& CurrentSoundOverlapMode() const { return Control(m_SoundOverlapMode, &LocalControls::m_SoundOverlapMode, LocalOverlapMode); }
		SoundOverlapMode& CurrentSoundOverlapMode() { return Control(m_SoundOverlapMode, &LocalControls::m_SoundOverlapMode, LocalOverlapMode); }
		const BusRouting& CurrentBusRouting() const { return Control(m_BusRouting, &LocalControls::m_BusRouting, LocalBusRouting); }
		BusRouting& CurrentBusRouting() { return Control(m_BusRouting, &LocalControls::m_BusRouting, LocalBusRouting); }
		const bool& CurrentImmobile() const { return Control(m_Immobile, &LocalControls::m_Immobile, LocalImmobile); }
		bool& CurrentImmobile() { return Control(m_Immobile, &LocalControls::m_Immobile, LocalImmobile); }
		const float& CurrentAttenuationStartDistance() const { return Control(m_AttenuationStartDistance, &LocalControls::m_AttenuationStartDistance, LocalAttenuationStartDistance); }
		float& CurrentAttenuationStartDistance() { return Control(m_AttenuationStartDistance, &LocalControls::m_AttenuationStartDistance, LocalAttenuationStartDistance); }
		const float& CurrentCustomPanValue() const { return Control(m_CustomPanValue, &LocalControls::m_CustomPanValue, LocalCustomPanValue); }
		float& CurrentCustomPanValue() { return Control(m_CustomPanValue, &LocalControls::m_CustomPanValue, LocalCustomPanValue); }
		const float& CurrentPanningStrengthMultiplier() const { return Control(m_PanningStrengthMultiplier, &LocalControls::m_PanningStrengthMultiplier, LocalPanningStrengthMultiplier); }
		float& CurrentPanningStrengthMultiplier() { return Control(m_PanningStrengthMultiplier, &LocalControls::m_PanningStrengthMultiplier, LocalPanningStrengthMultiplier); }
		const int& CurrentLoops() const { return Control(m_Loops, &LocalControls::m_Loops, LocalLoops); }
		int& CurrentLoops() { return Control(m_Loops, &LocalControls::m_Loops, LocalLoops); }
		const bool& CurrentSoundPropertiesUpToDate() const { return Control(m_SoundPropertiesUpToDate, &LocalControls::m_SoundPropertiesUpToDate, LocalSoundPropertiesUpToDate); }
		bool& CurrentSoundPropertiesUpToDate() { return Control(m_SoundPropertiesUpToDate, &LocalControls::m_SoundPropertiesUpToDate, LocalSoundPropertiesUpToDate); }
		const int& CurrentPriority() const { return Control(m_Priority, &LocalControls::m_Priority, LocalPriority); }
		int& CurrentPriority() { return Control(m_Priority, &LocalControls::m_Priority, LocalPriority); }
		const bool& CurrentAffectedByGlobalPitch() const { return Control(m_AffectedByGlobalPitch, &LocalControls::m_AffectedByGlobalPitch, LocalAffectedByGlobalPitch); }
		bool& CurrentAffectedByGlobalPitch() { return Control(m_AffectedByGlobalPitch, &LocalControls::m_AffectedByGlobalPitch, LocalAffectedByGlobalPitch); }
		const Vector& CurrentPos() const { return Control(m_Pos, &LocalControls::m_Pos, LocalPos); }
		Vector& CurrentPos() { return Control(m_Pos, &LocalControls::m_Pos, LocalPos); }
		const float& CurrentPitch() const { return Control(m_Pitch, &LocalControls::m_Pitch, LocalPitch); }
		float& CurrentPitch() { return Control(m_Pitch, &LocalControls::m_Pitch, LocalPitch); }
		const float& CurrentPitchVariation() const { return Control(m_PitchVariation, &LocalControls::m_PitchVariation, LocalPitchVariation); }
		float& CurrentPitchVariation() { return Control(m_PitchVariation, &LocalControls::m_PitchVariation, LocalPitchVariation); }
		const float& CurrentVolume() const { return Control(m_Volume, &LocalControls::m_Volume, LocalVolume); }
		float& CurrentVolume() { return Control(m_Volume, &LocalControls::m_Volume, LocalVolume); }
		const bool& CurrentWasFadedOut() const { return Control(m_WasFadedOut, &LocalControls::m_WasFadedOut, LocalWasFadedOut); }
		bool& CurrentWasFadedOut() { return Control(m_WasFadedOut, &LocalControls::m_WasFadedOut, LocalWasFadedOut); }
		const bool& CurrentPaused() const { return Control(m_Paused, &LocalControls::m_Paused, LocalPaused); }
		bool& CurrentPaused() { return Control(m_Paused, &LocalControls::m_Paused, LocalPaused); }
		const float& CurrentMusicPreEntryTime() const { return Control(m_MusicPreEntryTime, &LocalControls::m_MusicPreEntryTime, LocalMusicPreEntryTime); }
		float& CurrentMusicPreEntryTime() { return Control(m_MusicPreEntryTime, &LocalControls::m_MusicPreEntryTime, LocalMusicPreEntryTime); }
		const float& CurrentMusicExitTime() const { return Control(m_MusicExitTime, &LocalControls::m_MusicExitTime, LocalMusicExitTime); }
		float& CurrentMusicExitTime() { return Control(m_MusicExitTime, &LocalControls::m_MusicExitTime, LocalMusicExitTime); }

		const LogicalSoundPlayback& CurrentLogicalPlayback() const { return m_LogicalPlayback[InLocalSoundContext() ? 1 : 0]; }
		LogicalSoundPlayback& CurrentLogicalPlayback() { return m_LogicalPlayback[InLocalSoundContext() ? 1 : 0]; }
		bool HasLiveLogicalVoices() const;
		void RetireFinishedLogicalVoices();
		void FoldLogicalVoices();

		uint64_t m_CheckpointIdentity = 0;
		bool m_CheckpointRegistered = false;
		bool m_IsDestroying = false;
		void ReidentifyCheckpoint(uint64_t identity);
		void SwapCheckpoint(SoundContainer& other) noexcept;

		std::shared_ptr<SoundSet> m_TopLevelSoundSet; // The top level SoundSet that handles all SoundData and sub SoundSets in this SoundContainer.

		std::unordered_set<int> m_PlayingChannels; //!< The channels this SoundContainer is currently using.
		SoundOverlapMode m_SoundOverlapMode; //!< The SoundOverlapMode for this SoundContainer, used to determine how it should handle overlapping play calls.

		BusRouting m_BusRouting; //!< What bus this sound routes to.

		bool m_Immobile; //!< Whether this SoundContainer's sounds should be treated as immobile, i.e. not affected by 3D sound effects.
		float m_AttenuationStartDistance; //!< The distance away from the AudioSystem listener to start attenuating this sound. Attenuation follows FMOD 3D Inverse roll-off model.
		float m_CustomPanValue; //!< Custom stereo pan value using a Pan DSP on top of the basic spatialization.
		float m_PanningStrengthMultiplier; //!< Multiplier for panning strength.
		int m_Loops; //!< Number of loops (repeats) the SoundContainer's sounds should play when played. 0 means it plays once, -1 means it plays until stopped.
		bool m_SoundPropertiesUpToDate = false; //!< Whether this SoundContainer's sounds' modes and properties are up to date. Used primarily to handle discrepancies that can occur when loading from ini if the line ordering isn't ideal.

		int m_Priority; //!< The mixing priority of this SoundContainer's sounds. Higher values are more likely to be heard.
		bool m_AffectedByGlobalPitch; //!< Whether this SoundContainer's sounds should be able to be altered by global pitch changes.

		Vector m_Pos; //!< The current position of this SoundContainer's sounds.
		float m_Pitch; //!< The current natural pitch of this SoundContainer's sounds.
		float m_PitchVariation; //!< The randomized pitch variation of this SoundContainer's sounds. 1 means the sound will vary a full octave both ways.
		float m_Volume; //!< The current natural volume of this SoundContainer's sounds.

		bool m_WasFadedOut; //!< Whether this SoundContainer has had its current playing sounds faded out or not.
		bool m_Paused; //!< Whether this SoundContainer is paused or not.
		float m_MusicPreEntryTime; //!< The time in MS before the music starts in this SoundContainer.
		float m_MusicExitTime; //!< The time position in MS at which the music ends in this SoundContainer.

		/// Clears all the member variables of this SoundContainer, effectively resetting the members of this abstraction level only.
		void Clear();
	};
} // namespace RTE
