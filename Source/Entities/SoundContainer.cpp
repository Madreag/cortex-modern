#include "SoundContainer.h"
#include "CheckpointArchive.h"
#include "TimerMan.h"
#include "Base64/base64.h"
#include "MovableObject.h"

#include "SoundSet.h"
#include "Actor.h"
#include "Controller.h"
#include "SettingsMan.h"
#include "ConsoleMan.h"
#include "ScenarioRunner.h"
#include "FaultInjection.h"

#include <cmath>
#include <utility>

using namespace RTE;

ConcreteClassInfo(SoundContainer, Entity, 50);

const std::unordered_map<std::string, SoundContainer::SoundOverlapMode> SoundContainer::c_SoundOverlapModeMap = {
    {"Overlap", SoundContainer::SoundOverlapMode::OVERLAP},
    {"Restart", SoundContainer::SoundOverlapMode::RESTART},
    {"Ignore Play", SoundContainer::SoundOverlapMode::IGNORE_PLAY}};

const std::unordered_map<std::string, SoundContainer::BusRouting> SoundContainer::c_BusRoutingMap = {
    {"SFX", SoundContainer::BusRouting::SFX},
    {"UI", SoundContainer::BusRouting::UI},
    {"Music", SoundContainer::BusRouting::MUSIC}};

SoundContainer::SoundContainer() {
	Clear();
}

SoundContainer::SoundContainer(const SoundContainer& reference) {
	Clear();
	Create(reference);
}

SoundContainer& SoundContainer::operator=(const SoundContainer& reference) {
	if (this != &reference) { Destroy(); Create(reference); }
	return *this;
}

SoundContainer::~SoundContainer() {
	m_IsDestroying = true;
	g_AudioMan.DisownSoundContainerPlayingChannels(this);
	Destroy(true);
}

void SoundContainer::Clear() {
	g_AudioMan.UnregisterLogicalSound(this);
	g_AudioMan.ClearPendingSoundOps(this);
	m_LogicalPlayback = {};
	m_Pending = {};
	m_PendingOps.clear();
	m_PendingPlays = 0;
	m_PendingStopped = false;
	m_PendingPositionWritten = false;
	m_PendingTouchedByAI = false;
	m_SharedAliasHeld = false;
	m_PendingActorUID = 0;
	m_PendingTeam = -1;
	m_PendingAliasBaseline = Vector();
	m_SharedAliasBaseline = Vector();
	if (m_CheckpointRegistered) {
		g_AudioMan.DisownSoundContainerPlayingChannels(this);
		g_AudioMan.UnregisterCheckpointSoundContainer(this, m_CheckpointIdentity);
		m_CheckpointRegistered = false;
	}
	m_CheckpointIdentity = 0;
	if (!m_IsDestroying && (!MovableObject::IsFaithfulClone() || MovableObject::FaithfulCloneRegisters())) {
		ReidentifyCheckpoint(g_AudioMan.AllocateCheckpointSoundContainerID());
	}
	m_TopLevelSoundSet = std::make_shared<SoundSet>();
	m_TopLevelSoundSet->Destroy();
	m_TopLevelSoundSet->SetOwnerContainer(this);

	m_PlayingChannels.clear();
	m_SoundOverlapMode = SoundOverlapMode::OVERLAP;

	m_BusRouting = BusRouting::SFX;
	m_Immobile = false;
	m_AttenuationStartDistance = c_DefaultAttenuationStartDistance;
	m_CustomPanValue = 0.0f;
	m_PanningStrengthMultiplier = 1.0F;
	m_Loops = 0;
	m_SoundPropertiesUpToDate = false;

	m_Priority = AudioMan::PRIORITY_NORMAL;
	m_AffectedByGlobalPitch = true;

	m_Pos = Vector();
	m_Volume = 1.0F;
	m_Pitch = 1.0F;
	m_PitchVariation = 0;

	m_WasFadedOut = false;
	m_Paused = false;
	m_MusicPreEntryTime = 0.0F;
	m_MusicExitTime = 0.0F;
}

int SoundContainer::Create(const SoundContainer& reference) {
	Entity::Create(reference);

	m_TopLevelSoundSet->Create(*reference.m_TopLevelSoundSet);
	m_TopLevelSoundSet->SetOwnerContainer(this);

	m_PlayingChannels.clear();
	m_SoundOverlapMode = reference.m_SoundOverlapMode;

	m_BusRouting = reference.m_BusRouting;
	m_Immobile = reference.m_Immobile;
	m_AttenuationStartDistance = reference.m_AttenuationStartDistance;
	m_CustomPanValue = reference.m_CustomPanValue;
	m_PanningStrengthMultiplier = reference.m_PanningStrengthMultiplier;
	m_Loops = reference.m_Loops;

	m_Priority = reference.m_Priority;
	m_AffectedByGlobalPitch = reference.m_AffectedByGlobalPitch;

	m_Pos = reference.m_Pos;
	m_Volume = reference.m_Volume;
	m_Pitch = reference.m_Pitch;
	m_PitchVariation = reference.m_PitchVariation;

	m_WasFadedOut = reference.m_WasFadedOut;
	m_Paused = reference.m_Paused;
	m_MusicPreEntryTime = reference.m_MusicPreEntryTime;
	m_MusicExitTime = reference.m_MusicExitTime;
	if (Entity::IsCheckpointClone()) {
		if (!LoadCheckpoint(reference.SaveCheckpoint())) throw std::runtime_error("could not clone sound container checkpoint");
	}


	return 0;
}

int SoundContainer::Create(const std::string& soundFilePath, bool immobile, bool affectedByGlobalPitch, BusRouting busRouting) {
	m_TopLevelSoundSet->AddSound(soundFilePath, true);
	SetImmobile(immobile);
	SetAffectedByGlobalPitch(affectedByGlobalPitch);
	SetBusRouting(busRouting);
	return 0;
}

int SoundContainer::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return Entity::ReadProperty(propName, reader));
	MatchProperty("SpecialBehaviour_SoundCheckpoint", {
		if (!LoadCheckpoint(base64_decode(reader.ReadPropValue()))) reader.ReportError("invalid sound container checkpoint");
	});


	MatchProperty("SpecialBehaviour_TopLevelSoundSet", {
		SoundSet topLevelSoundSet;
		reader >> topLevelSoundSet;
		m_TopLevelSoundSet->Destroy();
		m_TopLevelSoundSet->Create(topLevelSoundSet);
		m_TopLevelSoundSet->SetOwnerContainer(this);
	});
	MatchProperty("AddSound", { m_TopLevelSoundSet->AddSoundData(SoundSet::ReadAndGetSoundData(reader)); });
	MatchProperty("AddSoundSet", {
		SoundSet soundSetToAdd;
		reader >> soundSetToAdd;
		m_TopLevelSoundSet->AddSoundSet(soundSetToAdd);
		m_TopLevelSoundSet->SetOwnerContainer(this);
	});
	MatchForwards("SoundSelectionCycleMode") MatchProperty("CycleMode", { m_TopLevelSoundSet->SetSoundSelectionCycleMode(SoundSet::ReadSoundSelectionCycleMode(reader)); });
	MatchProperty("SoundOverlapMode", {
		std::string soundOverlapModeString = reader.ReadPropValue();
		if (c_SoundOverlapModeMap.find(soundOverlapModeString) != c_SoundOverlapModeMap.end()) {
			m_SoundOverlapMode = c_SoundOverlapModeMap.find(soundOverlapModeString)->second;
		} else {
			try {
				m_SoundOverlapMode = static_cast<SoundOverlapMode>(std::stoi(soundOverlapModeString));
			} catch (const std::exception&) {
				reader.ReportError("Cycle mode " + soundOverlapModeString + " is invalid.");
			}
		}
	});
	MatchProperty("BusRouting", {
		std::string busRoutingString = reader.ReadPropValue();
		if (c_BusRoutingMap.find(busRoutingString) != c_BusRoutingMap.end()) {
			m_BusRouting = c_BusRoutingMap.find(busRoutingString)->second;
		} else {
			try {
				m_BusRouting = static_cast<BusRouting>(std::stoi(busRoutingString));
			} catch (const std::exception&) {
				reader.ReportError("Tried to route to non-existent sound bus " + busRoutingString);
			}
		}
	});
	MatchProperty("Immobile", { reader >> m_Immobile; });
	MatchProperty("AttenuationStartDistance", { reader >> m_AttenuationStartDistance; });
	MatchProperty("CustomPanValue", {
		reader >> m_CustomPanValue;
		if (m_CustomPanValue < -1.0f || m_CustomPanValue > 1.0f) {
			reader.ReportError("SoundContainer CustomPanValue must be between -1 and 1.");
		}
	});
	MatchProperty("PanningStrengthMultiplier", { reader >> m_PanningStrengthMultiplier; });
	MatchProperty("LoopSetting", { reader >> m_Loops; });
	MatchProperty("Priority", {
		reader >> m_Priority;
		if (m_Priority < 0 || m_Priority > 256) {
			reader.ReportError("SoundContainer priority must be between 256 (lowest priority) and 0 (highest priority).");
		}
	});
	MatchProperty("AffectedByGlobalPitch", { reader >> m_AffectedByGlobalPitch; });
	MatchProperty("Position", { reader >> m_Pos; });
	MatchProperty("Volume", { reader >> m_Volume; });
	MatchProperty("Pitch", { reader >> m_Pitch; });
	MatchProperty("PitchVariation", { reader >> m_PitchVariation; });
	MatchProperty("WasFadedOut", { reader >> m_WasFadedOut; });
	MatchProperty("Paused", { reader >> m_Paused; });

	MatchProperty("MusicPreEntryTime", { reader >> m_MusicPreEntryTime; });
	MatchProperty("MusicExitTime", { reader >> m_MusicExitTime; });

	EndPropertyList;
}

int SoundContainer::Save(Writer& writer) const {
	Entity::Save(writer);

	// Due to writer limitations, the top level SoundSet has to be explicitly written out, even though SoundContainer standard behaviour is to hide it in INI and just have properties be part of the SoundContainer.
	writer.NewPropertyWithValue("SpecialBehaviour_TopLevelSoundSet", *m_TopLevelSoundSet);

	writer.NewProperty("SoundOverlapMode");
	auto overlapModeMapEntry = std::find_if(c_SoundOverlapModeMap.begin(), c_SoundOverlapModeMap.end(), [&soundOverlapMode = m_SoundOverlapMode](auto element) { return element.second == soundOverlapMode; });
	if (overlapModeMapEntry != c_SoundOverlapModeMap.end()) {
		writer << overlapModeMapEntry->first;
	} else {
		RTEAbort("Tried to write invalid SoundOverlapMode when saving SoundContainer.");
	}
	writer.NewProperty("BusRouting");
	writer << m_BusRouting;
	writer.NewProperty("Immobile");
	writer << m_Immobile;
	writer.NewProperty("AttenuationStartDistance");
	writer << m_AttenuationStartDistance;
	writer.NewProperty("CustomPanValue");
	writer << m_CustomPanValue;
	writer.NewProperty("PanningStrengthMultiplier");
	writer << m_PanningStrengthMultiplier;
	writer.NewProperty("LoopSetting");
	writer << m_Loops;

	writer.NewProperty("Priority");
	writer << m_Priority;
	writer.NewProperty("AffectedByGlobalPitch");
	writer << m_AffectedByGlobalPitch;

	writer.NewProperty("Position");
	writer << m_Pos;
	writer.NewProperty("Volume");
	writer << m_Volume;
	writer.NewProperty("Pitch");
	writer << m_Pitch;
	writer.NewProperty("PitchVariation");
	writer << m_PitchVariation;

	writer.NewProperty("WasFadedOut");
	writer << m_WasFadedOut;
	writer.NewProperty("Paused");
	writer << m_Paused;
	writer.NewProperty("MusicPreEntryTime");
	writer << m_MusicPreEntryTime;
	writer.NewProperty("MusicExitTime");
	writer << m_MusicExitTime;
	if (writer.IsSnapshot()) writer.NewPropertyWithValue("SpecialBehaviour_SoundCheckpoint", base64_encode(SaveCheckpoint(), true));

	return 0;
}

bool SoundContainer::HasAnySounds() const {
	return m_TopLevelSoundSet->HasAnySounds();
}

float SoundContainer::GetLength(LengthOfSoundType type) const {
	if (!m_SoundPropertiesUpToDate) {
		// Todo - use a post-load fixup stage instead of lazily initializing shit everywhere... Eugh.
		const_cast<SoundContainer*>(this)->UpdateSoundProperties();
		const_cast<SoundContainer*>(this)->m_TopLevelSoundSet->SelectNextSounds();
	}

	std::vector<const SoundData*> flattenedSoundData;
	m_TopLevelSoundSet->GetFlattenedSoundData(flattenedSoundData, type == LengthOfSoundType::NextPlayed);

	float lengthMilliseconds = 0.0f;
	for (const SoundData* selectedSoundData: flattenedSoundData) {
		unsigned int length;
		selectedSoundData->SoundObject->getLength(&length, FMOD_TIMEUNIT_MS);
		lengthMilliseconds = std::max(lengthMilliseconds, static_cast<float>(length));
	}

	return lengthMilliseconds;
}

void SoundContainer::SetTopLevelSoundSet(const SoundSet& newTopLevelSoundSet) {
	*m_TopLevelSoundSet = newTopLevelSoundSet;
	m_TopLevelSoundSet->SetOwnerContainer(this);
	m_SoundPropertiesUpToDate = false;
}

std::vector<std::size_t> SoundContainer::GetSelectedSoundHashes() const {
	std::vector<size_t> soundHashes;
	std::vector<const SoundData*> flattenedSoundData;
	m_TopLevelSoundSet->GetFlattenedSoundData(flattenedSoundData, false);
	for (const SoundData* selectedSoundData: flattenedSoundData) {
		soundHashes.push_back(selectedSoundData->SoundFile.GetHash());
	}
	return soundHashes;
}

const SoundData* SoundContainer::GetSoundDataForSound(const FMOD::Sound* sound) const {
	std::vector<const SoundData*> flattenedSoundData;
	m_TopLevelSoundSet->GetFlattenedSoundData(flattenedSoundData, false);
	for (const SoundData* soundData: flattenedSoundData) {
		if (sound == soundData->SoundObject) {
			return soundData;
		}
	}
	return nullptr;
}

void SoundContainer::SetCustomPanValue(float customPanValue) {
	customPanValue = std::clamp(customPanValue, -1.0f, 1.0f);
	const bool deferred = DeferProperty(PendingOp::CustomPan, customPanValue);
	CurrentCustomPanValue() = customPanValue;
	if (!deferred && IsBeingPlayed()) {
		g_AudioMan.ChangeSoundContainerPlayingChannelsCustomPanValue(this);
	}
}

void SoundContainer::SetPosition(const Vector& newPosition) {
	if (std::as_const(*this).CurrentImmobile() || newPosition == std::as_const(*this).CurrentPos()) {
		return;
	}
	if (DeferProperty(PendingOp::Position, newPosition.m_X, newPosition.m_Y)) {
		CurrentPos() = newPosition;
		return;
	}
	m_Pos = newPosition;
	if (IsBeingPlayed()) {
		g_AudioMan.ChangeSoundContainerPlayingChannelsPosition(this);
	}
}

float SoundContainer::GetAudibleVolume() const {
	// A shared query reads the committed observation set so every peer sees one value; local AI and presentation keep this machine's.
	if (SoundSimulationScope::Domain() == SoundExecutionDomain::SharedSimulation && ScenarioRunner::IsLockstepControllerSyncActive() && !FaultInjected("local_audibility")) {
		return g_AudioMan.GetCommittedAudibility(*this);
	}
	return g_AudioMan.GetSoundContainerAudibleVolume(this);
}

void SoundContainer::SetVolume(float newVolume) {
	newVolume = std::clamp(newVolume, 0.0F, 10.0F);
	const bool deferred = DeferProperty(PendingOp::Volume, newVolume);
	if (!deferred && IsBeingPlayed()) {
		g_AudioMan.ChangeSoundContainerPlayingChannelsVolume(this, newVolume);
	}
	CurrentVolume() = newVolume;
}

void SoundContainer::SetPitch(float newPitch) {
	newPitch = std::clamp(newPitch, 0.125F, 8.0F);
	if (DeferProperty(PendingOp::Pitch, newPitch)) {
		CurrentPitch() = newPitch;
		return;
	}
	const bool logical = UsesLogicalPlayback();
	if (logical) FoldLogicalVoices();
	CurrentPitch() = newPitch;
	if (logical) {
		for (LogicalSoundVoice& voice: CurrentLogicalPlayback().voices) voice.pitch = std::as_const(*this).CurrentPitch();
	}
	if (IsBeingPlayed()) {
		g_AudioMan.ChangeSoundContainerPlayingChannelsPitch(this);
	}
}

void SoundContainer::SetPaused(bool paused) {
	if (paused == std::as_const(*this).CurrentPaused()) return;
	if (DeferProperty(PendingOp::Paused, 0.0F, 0.0F, paused ? 1 : 0)) {
		CurrentPaused() = paused;
		return;
	}
	if (UsesLogicalPlayback()) {
		FoldLogicalVoices();
		for (LogicalSoundVoice& voice: CurrentLogicalPlayback().voices) voice.paused = paused;
	}
	CurrentPaused() = paused;
	g_AudioMan.SetPausedSoundContainerPlayingChannels(this, paused);
}

bool SoundContainer::Play(int player) {
	if (HasAnySounds()) {
		std::unique_lock<std::recursive_mutex> pending(m_PendingMutex, std::defer_lock);
		if (Deferring()) pending.lock();
		CurrentWasFadedOut() = false;
		if (IsBeingPlayed()) {
			const SoundOverlapMode overlapMode = std::as_const(*this).CurrentSoundOverlapMode();
			if (overlapMode == SoundOverlapMode::RESTART) {
				return Restart(player);
			} else if (overlapMode == SoundOverlapMode::IGNORE_PLAY) {
				return false;
			}
		}
		if (pending.owns_lock()) {
			PendingOp op;
			op.op = PendingOp::Play;
			op.player = player;
			QueuePendingOp(std::move(op));
			++m_PendingPlays;
			return true;
		}
		return g_AudioMan.PlaySoundContainer(this, player);
	}
	return false;
}

bool SoundContainer::Stop(int player) {
	if (!HasAnySounds()) return false;
	if (Deferring()) {
		std::lock_guard<std::recursive_mutex> pending(m_PendingMutex);
		if (!IsBeingPlayed()) return false;
		PendingOp op;
		op.op = PendingOp::Stop;
		op.player = player;
		QueuePendingOp(std::move(op));
		m_PendingStopped = true;
		m_PendingPlays = 0;
		return true;
	}
	return IsBeingPlayed() ? g_AudioMan.StopSoundContainerPlayingChannels(this, player) : false;
}

bool SoundContainer::IsBeingPlayed() const {
	if (Deferring()) {
		std::lock_guard<std::recursive_mutex> pending(m_PendingMutex);
		if (m_PendingPlays > 0) return true;
		if (m_PendingStopped) return false;
	}
	if (UsesLogicalPlayback()) return HasLiveLogicalVoices();
	for (int identity: m_PlayingChannels) if (g_AudioMan.OwnsVoice(identity, this)) return true;
	return false;
}

bool SoundContainer::Restart(int player) {
	if (!HasAnySounds()) return false;
	if (Deferring()) {
		std::lock_guard<std::recursive_mutex> pending(m_PendingMutex);
		if (!IsBeingPlayed()) return false;
		PendingOp op;
		op.op = PendingOp::Restart;
		op.player = player;
		QueuePendingOp(std::move(op));
		m_PendingStopped = true;
		m_PendingPlays = 1;
		return true;
	}
	return IsBeingPlayed() ? g_AudioMan.StopSoundContainerPlayingChannels(this, player) && g_AudioMan.PlaySoundContainer(this, player) : false;
}

void SoundContainer::FadeOut(int fadeOutTime) {
	if (Deferring()) {
		std::lock_guard<std::recursive_mutex> pending(m_PendingMutex);
		if (std::as_const(*this).CurrentWasFadedOut() || !IsBeingPlayed()) return;
		CurrentWasFadedOut() = true;
		PendingOp op;
		op.op = PendingOp::FadeOut;
		op.value = fadeOutTime;
		QueuePendingOp(std::move(op));
		return;
	}
	if (!std::as_const(*this).CurrentWasFadedOut() && IsBeingPlayed()) {
		CurrentWasFadedOut() = true;
		return g_AudioMan.FadeOutSoundContainerPlayingChannels(this, fadeOutTime);
	}
}

const Vector& SoundContainer::GetScriptPosition() const {
	std::lock_guard<std::recursive_mutex> pending(m_PendingMutex);
	SoundContainer* self = const_cast<SoundContainer*>(this);
	if (!Deferring()) {
		// A write Lua made through the position this pass holds is a shared write when a shared hook
		// makes it: it lands now, exactly as it did when the alias was the position itself.
		self->SettleSharedAliasWrite();
		self->m_SharedAliasHeld = true;
		self->m_SharedAliasBaseline = m_Pos;
		self->NotePending();
		return m_Pos;
	}
	// The AI hook holds this pass's own position: a write through it is a decision, folded into a
	// deferred write at the next read and at the drain, never landing on shared state early.
	self->NoteAIActor();
	self->AdoptSharedAliasWrite();
	if (!(m_Pending.written & LocalPos)) {
		self->m_Pending.m_Pos = m_Pos;
		self->m_Pending.written |= LocalPos;
		self->m_PendingAliasBaseline = m_Pos;
		self->NotePending();
	}
	self->ReconcileAliasPosition();
	return m_Pending.m_Pos;
}

void SoundContainer::AdoptSharedAliasWrite() {
	// A position handed out in a shared scope is m_Pos itself, so an AI hook writing through it would
	// reach shared state directly. Take the value back off m_Pos and defer it like any other AI write.
	if (!m_SharedAliasHeld || m_Pos == m_SharedAliasBaseline) return;
	const Vector written = m_Pos;
	m_Pos = m_SharedAliasBaseline;
	m_Pending.m_Pos = written;
	m_Pending.written |= LocalPos;
	m_PendingAliasBaseline = m_SharedAliasBaseline;
	NotePending();
	ReconcileAliasPosition();
}

void SoundContainer::SettleSharedAliasWrite() {
	// Outside the AI pass a change to the position this container holds for Lua is a shared write, so
	// it lands on shared state now rather than waiting for a drain that may never visit again.
	if (m_Pos != m_SharedAliasBaseline) m_SharedAliasBaseline = m_Pos;
	if (!(m_Pending.written & LocalPos) || m_Pending.m_Pos == m_PendingAliasBaseline) return;
	m_Pos = m_Pending.m_Pos;
	m_PendingAliasBaseline = m_Pending.m_Pos;
	m_SharedAliasBaseline = m_Pos;
}

void SoundContainer::ReconcileAliasPosition() {
	if (!(m_Pending.written & LocalPos)) return;
	if (m_Pending.m_Pos != m_PendingAliasBaseline) {
		m_PendingAliasBaseline = m_Pending.m_Pos;
		m_PendingPositionWritten = true;
		PendingOp op;
		op.op = PendingOp::SetProperty;
		op.property = PendingOp::PositionAlias;
		op.x = m_Pending.m_Pos.m_X;
		op.y = m_Pending.m_Pos.m_Y;
		QueuePendingOp(std::move(op));
	} else if (!m_PendingPositionWritten) {
		// A mere read must not freeze this pass's view of a position the simulation moved.
		m_Pending.m_Pos = m_Pos;
		m_PendingAliasBaseline = m_Pos;
	}
}

void SoundContainer::ApplyPendingControl(PendingOp::Property property, float x, float y, int32_t value) {
	switch (property) {
		case PendingOp::Volume: m_Pending.m_Volume = x; m_Pending.written |= LocalVolume; break;
		case PendingOp::Pitch: m_Pending.m_Pitch = x; m_Pending.written |= LocalPitch; break;
		case PendingOp::PitchVariation: m_Pending.m_PitchVariation = x; m_Pending.written |= LocalPitchVariation; break;
		case PendingOp::Loops: m_Pending.m_Loops = value; m_Pending.written |= LocalLoops; break;
		case PendingOp::Priority: m_Pending.m_Priority = value; m_Pending.written |= LocalPriority; break;
		case PendingOp::Immobile: m_Pending.m_Immobile = value != 0; m_Pending.written |= LocalImmobile; break;
		case PendingOp::BusRoute: m_Pending.m_BusRouting = static_cast<BusRouting>(value); m_Pending.written |= LocalBusRouting; break;
		case PendingOp::OverlapMode: m_Pending.m_SoundOverlapMode = static_cast<SoundOverlapMode>(value); m_Pending.written |= LocalOverlapMode; break;
		case PendingOp::AttenuationStart: m_Pending.m_AttenuationStartDistance = x; m_Pending.written |= LocalAttenuationStartDistance; break;
		case PendingOp::CustomPan: m_Pending.m_CustomPanValue = x; m_Pending.written |= LocalCustomPanValue; break;
		case PendingOp::PanningStrength: m_Pending.m_PanningStrengthMultiplier = x; m_Pending.written |= LocalPanningStrengthMultiplier; break;
		case PendingOp::GlobalPitch: m_Pending.m_AffectedByGlobalPitch = value != 0; m_Pending.written |= LocalAffectedByGlobalPitch; break;
		case PendingOp::Paused: m_Pending.m_Paused = value != 0; m_Pending.written |= LocalPaused; break;
		case PendingOp::Position:
		case PendingOp::PositionAlias: m_Pending.m_Pos = Vector(x, y); m_Pending.written |= LocalPos; break;
		case PendingOp::MusicPreEntry: m_Pending.m_MusicPreEntryTime = x; m_Pending.written |= LocalMusicPreEntryTime; break;
		case PendingOp::MusicExit: m_Pending.m_MusicExitTime = x; m_Pending.written |= LocalMusicExitTime; break;
		default: break;
	}
	switch (property) {
		case PendingOp::Loops:
		case PendingOp::Immobile:
		case PendingOp::AttenuationStart:
		case PendingOp::PanningStrength:
			m_Pending.m_SoundPropertiesUpToDate = false;
			m_Pending.written |= LocalSoundPropertiesUpToDate;
			break;
		default: break;
	}
}

bool SoundContainer::DeferProperty(PendingOp::Property property, float x, float y, int32_t value) {
	if (!Deferring()) return false;
	std::lock_guard<std::recursive_mutex> pending(m_PendingMutex);
	NoteAIActor();
	AdoptSharedAliasWrite();
	if (property == PendingOp::Position) {
		ReconcileAliasPosition();
		m_PendingPositionWritten = true;
		m_PendingAliasBaseline = Vector(x, y);
	}
	PendingOp op;
	op.op = PendingOp::SetProperty;
	op.property = property;
	op.x = x;
	op.y = y;
	op.value = value;
	QueuePendingOp(std::move(op));
	return true;
}

void SoundContainer::NoteAIActor() {
	m_PendingTouchedByAI = true;
	if (!g_CurrentAIActor) return;
	m_PendingActorUID = static_cast<int64_t>(g_CurrentAIActor->GetUniqueID());
	m_PendingTeam = g_CurrentAIActor->GetTeam();
}

void SoundContainer::QueuePendingOp(PendingOp op) {
	// A call reconciled at the drain has no running AI actor, so it keeps the one that last touched
	// this container: an unattributed call would be refused on the wire.
	op.actorUID = g_CurrentAIActor ? static_cast<int64_t>(g_CurrentAIActor->GetUniqueID()) : m_PendingActorUID;
	op.team = g_CurrentAIActor ? g_CurrentAIActor->GetTeam() : m_PendingTeam;
	m_PendingOps.push_back(std::move(op));
	NotePending();
}

void SoundContainer::NotePending() {
	g_AudioMan.NotePendingSoundOps(this);
}

bool SoundContainer::QueuePendingSelectSounds(std::vector<uint16_t> soundSetPath) {
	std::lock_guard<std::recursive_mutex> pending(m_PendingMutex);
	PendingOp op;
	op.op = PendingOp::SelectSounds;
	op.soundSetPath = std::move(soundSetPath);
	QueuePendingOp(std::move(op));
	return true;
}

static bool FindSoundSetPathIn(const SoundSet& parent, const SoundSet& target, std::vector<uint16_t>& path) {
	if (&parent == &target) return true;
	const std::vector<SoundSet*>& subSoundSets = const_cast<SoundSet&>(parent).GetSubSoundSets();
	for (size_t index = 0; index < subSoundSets.size(); ++index) {
		path.push_back(static_cast<uint16_t>(index));
		if (FindSoundSetPathIn(*subSoundSets[index], target, path)) return true;
		path.pop_back();
	}
	return false;
}

bool SoundContainer::FindSoundSetPath(const SoundSet& soundSet, std::vector<uint16_t>& path) const {
	path.clear();
	return FindSoundSetPathIn(*m_TopLevelSoundSet, soundSet, path);
}

std::vector<SoundContainer::PendingOp> SoundContainer::TakePendingSoundOps() {
	std::lock_guard<std::recursive_mutex> pending(m_PendingMutex);
	if (m_PendingTouchedByAI) {
		AdoptSharedAliasWrite();
		ReconcileAliasPosition();
	} else {
		// Nothing ran in an AI pass since the last drain, so a change found now was a shared write.
		SettleSharedAliasWrite();
	}
	std::vector<PendingOp> taken;
	taken.swap(m_PendingOps);
	// The position Lua holds outlives the pass that took it, exactly as the shared one does, so the
	// container keeps being drained: that is where a write made through a retained alias is found.
	const bool aliasHeld = (m_Pending.written & LocalPos) != 0;
	const Vector aliasPosition = m_Pending.m_Pos;
	m_Pending = {};
	if (aliasHeld) {
		m_Pending.m_Pos = aliasPosition;
		m_Pending.written = LocalPos;
	}
	m_PendingPlays = 0;
	m_PendingStopped = false;
	m_PendingPositionWritten = false;
	m_PendingTouchedByAI = false;
	m_PendingAliasBaseline = aliasHeld ? aliasPosition : Vector();
	if (aliasHeld || m_SharedAliasHeld) NotePending();
	else g_AudioMan.ClearPendingSoundOps(this);
	return taken;
}

bool SoundContainer::ApplyPendingSoundOp(const PendingOp& op) {
	switch (op.op) {
		case PendingOp::Play:
			return Play(op.player);
		case PendingOp::Stop:
			return Stop(op.player);
		case PendingOp::Restart:
			return Restart(op.player);
		case PendingOp::FadeOut:
			FadeOut(op.value);
			return true;
		case PendingOp::SelectSounds: {
			SoundSet* soundSet = m_TopLevelSoundSet.get();
			for (uint16_t index: op.soundSetPath) {
				if (!soundSet || index >= soundSet->GetSubSoundSets().size()) return false;
				soundSet = soundSet->GetSubSoundSets()[index];
			}
			return soundSet && soundSet->SelectNextSounds();
		}
		case PendingOp::SetProperty:
			return ApplyPendingProperty(op);
		default:
			return false;
	}
}

bool SoundContainer::ApplyPendingProperty(const PendingOp& op) {
	switch (op.property) {
		case PendingOp::Volume:
			SetVolume(op.x);
			return true;
		case PendingOp::Pitch:
			SetPitch(op.x);
			return true;
		case PendingOp::PitchVariation:
			SetPitchVariation(op.x);
			return true;
		case PendingOp::Loops:
			SetLoopSetting(op.value);
			return true;
		case PendingOp::Priority:
			SetPriority(op.value);
			return true;
		case PendingOp::Immobile:
			SetImmobile(op.value != 0);
			return true;
		case PendingOp::BusRoute:
			SetBusRouting(static_cast<BusRouting>(op.value));
			return true;
		case PendingOp::OverlapMode:
			SetSoundOverlapMode(static_cast<SoundOverlapMode>(op.value));
			return true;
		case PendingOp::AttenuationStart:
			SetAttenuationStartDistance(op.x);
			return true;
		case PendingOp::CustomPan:
			SetCustomPanValue(op.x);
			return true;
		case PendingOp::PanningStrength:
			SetPanningStrengthMultiplier(op.x);
			return true;
		case PendingOp::GlobalPitch:
			SetAffectedByGlobalPitch(op.value != 0);
			return true;
		case PendingOp::Paused:
			SetPaused(op.value != 0);
			return true;
		case PendingOp::Position:
			SetPosition(Vector(op.x, op.y));
			return true;
		case PendingOp::PositionAlias:
			// A write through the alias never went through the setter, so it kept neither the
			// immobile guard nor the repositioning of playing channels.
			m_Pos = Vector(op.x, op.y);
			return true;
		case PendingOp::MusicPreEntry:
			SetMusicPreEntryTime(op.x);
			return true;
		case PendingOp::MusicExit:
			SetMusicExitTime(op.x);
			return true;
		default:
			return false;
	}
}

bool SoundContainer::HasLiveLogicalVoices() const {
	const long long now = g_TimerMan.GetSimTimeTicks();
	const long long ticksPerSecond = g_TimerMan.GetTicksPerSecond();
	for (const LogicalSoundVoice& voice: CurrentLogicalPlayback().voices) {
		if (!voice.At(now, ticksPerSecond).finished) return true;
	}
	return false;
}

void SoundContainer::RetireFinishedLogicalVoices() {
	const long long now = g_TimerMan.GetSimTimeTicks();
	const long long ticksPerSecond = g_TimerMan.GetTicksPerSecond();
	std::erase_if(CurrentLogicalPlayback().voices, [&](const LogicalSoundVoice& voice) { return voice.At(now, ticksPerSecond).finished; });
}

void SoundContainer::FoldLogicalVoices() {
	RetireFinishedLogicalVoices();
	const long long now = g_TimerMan.GetSimTimeTicks();
	const long long ticksPerSecond = g_TimerMan.GetTicksPerSecond();
	for (LogicalSoundVoice& voice: CurrentLogicalPlayback().voices) voice.Fold(now, ticksPerSecond);
}

FMOD_RESULT SoundContainer::UpdateSoundProperties() {
	FMOD_RESULT result = FMOD_OK;

	std::vector<SoundData*> flattenedSoundData;
	m_TopLevelSoundSet->GetFlattenedSoundData(flattenedSoundData, false);
	for (SoundData* soundData: flattenedSoundData) {
		FMOD_MODE soundMode = (std::as_const(*this).CurrentLoops() == 0) ? FMOD_LOOP_OFF : FMOD_LOOP_NORMAL;
		if (std::as_const(*this).CurrentImmobile()) {
			soundMode |= FMOD_2D;
			CurrentAttenuationStartDistance() = c_SoundMaxAudibleDistance;
		} else if (g_AudioMan.GetSoundPanningEffectStrength() == 1.0F) {
			soundMode |= FMOD_3D_INVERSEROLLOFF;
		} else {
			soundMode |= FMOD_3D_CUSTOMROLLOFF;
		}

		result = (result == FMOD_OK) ? soundData->SoundObject->setMode(soundMode) : result;
		result = (result == FMOD_OK) ? soundData->SoundObject->setLoopCount(CurrentLoops()) : result;
		CurrentAttenuationStartDistance() = std::clamp(CurrentAttenuationStartDistance(), 0.0F, static_cast<float>(c_SoundMaxAudibleDistance) - soundData->MinimumAudibleDistance);
		result = (result == FMOD_OK) ? soundData->SoundObject->set3DMinMaxDistance(soundData->MinimumAudibleDistance + CurrentAttenuationStartDistance(), c_SoundMaxAudibleDistance) : result;
		if (result != FMOD_OK) {
			FMOD_OPENSTATE openState = FMOD_OPENSTATE_ERROR;
			const FMOD_RESULT openResult = soundData->SoundObject->getOpenState(&openState, nullptr, nullptr, nullptr);
			g_ConsoleMan.PrintString("ERROR: Sound property update failed for " + soundData->SoundFile.GetDataPath() + " (open state=" + std::to_string(static_cast<int>(openState)) + ", result=" + std::to_string(static_cast<int>(openResult)) + ")");
			break;
		}
	}
	CurrentSoundPropertiesUpToDate() = result == FMOD_OK;

	return result;
}

void SoundContainer::ReidentifyCheckpoint(uint64_t identity) {
	if (m_CheckpointRegistered) g_AudioMan.UnregisterCheckpointSoundContainer(this, m_CheckpointIdentity);
	m_CheckpointIdentity = identity;
	m_CheckpointRegistered = !m_IsDestroying && (!MovableObject::IsFaithfulClone() || MovableObject::FaithfulCloneRegisters());
	if (m_CheckpointRegistered) g_AudioMan.RegisterCheckpointSoundContainer(this, identity);
}

std::string SoundContainer::SaveCheckpoint() const {
	CheckpointWriter archive("SoundContainer3");
	archive(Entity::SaveCheckpoint(), m_CheckpointIdentity, std::set<int>(m_PlayingChannels.begin(), m_PlayingChannels.end()));
	archive(m_SoundOverlapMode, m_BusRouting, m_Immobile, m_AttenuationStartDistance, m_CustomPanValue, m_PanningStrengthMultiplier, m_Loops, m_SoundPropertiesUpToDate, m_Priority, m_AffectedByGlobalPitch, m_Pos, m_Pitch, m_PitchVariation, m_Volume, m_WasFadedOut, m_Paused, m_MusicPreEntryTime, m_MusicExitTime);
	archive(m_LogicalPlayback);
	return archive.Text();
}

bool SoundContainer::LoadCheckpoint(std::string_view text, bool validateOnly) {
	try {
		const auto version = CheckpointVersion(text);
		CheckpointReader archive(text, version, validateOnly);
		std::string identity;
		uint64_t checkpointIdentity;
		std::set<int> playing;
		archive.Value(identity); archive.Value(checkpointIdentity); archive.Value(playing);
		if (!checkpointIdentity || !Entity::LoadCheckpoint(identity, true)) return false;
		for (int voice: playing) if (voice <= 0) return false;
		archive.OnCommit([this, identity, checkpointIdentity, playing = std::move(playing)] {
			Entity::LoadCheckpoint(identity);
			ReidentifyCheckpoint(checkpointIdentity);
			m_PlayingChannels.clear(); m_PlayingChannels.insert(playing.begin(), playing.end());
		});
		archive(m_SoundOverlapMode, m_BusRouting, m_Immobile, m_AttenuationStartDistance, m_CustomPanValue, m_PanningStrengthMultiplier, m_Loops, m_SoundPropertiesUpToDate, m_Priority, m_AffectedByGlobalPitch, m_Pos, m_Pitch, m_PitchVariation, m_Volume, m_WasFadedOut, m_Paused, m_MusicPreEntryTime, m_MusicExitTime);
		if (version == "SoundContainer3") {
			archive(m_LogicalPlayback);
		} else if (version == "SoundContainer2") {
			// The old shape carried a second cohort and the transient AI-pass controls; only the
			// shared cohort was ever simulation state.
			std::string shared, discardedCohort, discardedControls;
			archive.Value(shared);
			archive.Value(discardedCohort);
			archive.Value(discardedControls);
			if (!m_LogicalPlayback.LoadCheckpoint(shared, true)) return false;
			archive.OnCommit([this, shared] {
				if (!m_LogicalPlayback.LoadCheckpoint(shared)) throw std::runtime_error("could not apply sound playback checkpoint");
			});
		} else {
			archive.OnCommit([this] { m_LogicalPlayback = {}; });
		}
		archive.Finish();
		if (!validateOnly) g_AudioMan.RefreshLogicalSound(this);
		return true;
	} catch (const std::exception&) { return false; }
}

void SoundContainer::SwapCheckpoint(SoundContainer& other) noexcept {
	using std::swap;
	swap(m_PresetName, other.m_PresetName);
	swap(m_CopiedFromPresetName, other.m_CopiedFromPresetName);
	swap(m_PresetDescription, other.m_PresetDescription);
	swap(m_FormattedReaderPosition, other.m_FormattedReaderPosition);
	swap(m_IsOriginalPreset, other.m_IsOriginalPreset);
	swap(m_DefinedInModule, other.m_DefinedInModule);
	swap(m_RandomWeight, other.m_RandomWeight);
	swap(m_Groups, other.m_Groups);
	swap(m_CheckpointIdentity, other.m_CheckpointIdentity);
	swap(m_LogicalPlayback, other.m_LogicalPlayback);
	swap(m_TopLevelSoundSet, other.m_TopLevelSoundSet);
	m_TopLevelSoundSet->SetOwnerContainer(this);
	other.m_TopLevelSoundSet->SetOwnerContainer(&other);
	swap(m_PlayingChannels, other.m_PlayingChannels);
	swap(m_SoundOverlapMode, other.m_SoundOverlapMode);
	swap(m_BusRouting, other.m_BusRouting);
	swap(m_Immobile, other.m_Immobile);
	swap(m_AttenuationStartDistance, other.m_AttenuationStartDistance);
	swap(m_CustomPanValue, other.m_CustomPanValue);
	swap(m_PanningStrengthMultiplier, other.m_PanningStrengthMultiplier);
	swap(m_Loops, other.m_Loops);
	swap(m_SoundPropertiesUpToDate, other.m_SoundPropertiesUpToDate);
	swap(m_Priority, other.m_Priority);
	swap(m_AffectedByGlobalPitch, other.m_AffectedByGlobalPitch);
	swap(m_Pos, other.m_Pos);
	swap(m_Pitch, other.m_Pitch);
	swap(m_PitchVariation, other.m_PitchVariation);
	swap(m_Volume, other.m_Volume);
	swap(m_WasFadedOut, other.m_WasFadedOut);
	swap(m_Paused, other.m_Paused);
	swap(m_MusicPreEntryTime, other.m_MusicPreEntryTime);
	swap(m_MusicExitTime, other.m_MusicExitTime);
}
