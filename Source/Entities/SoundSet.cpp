#include "SoundSet.h"
#include "SoundContainer.h"
#include "CheckpointArchive.h"
#include "Base64/base64.h"
#include "AudioMan.h"
#include "RTETools.h"
#include "RTEError.h"

using namespace RTE;

const std::string SoundSet::m_sClassName = "SoundSet";

const std::unordered_map<std::string, SoundSet::SoundSelectionCycleMode> SoundSet::c_SoundSelectionCycleModeMap = {
    {"random", SoundSelectionCycleMode::RANDOM},
    {"forwards", SoundSelectionCycleMode::FORWARDS},
    {"all", SoundSelectionCycleMode::ALL}};

SoundSet::SoundSet() {
	Clear();
}

SoundSet::~SoundSet() {
	Destroy();
}

SoundSet& SoundSet::operator=(const SoundSet& reference) {
	if (this != &reference) {
		SoundSet copy(reference);
		std::swap(m_SoundSelectionCycleMode, copy.m_SoundSelectionCycleMode);
		std::swap(m_CurrentSelection, copy.m_CurrentSelection);
		std::swap(m_SimulationSelection, copy.m_SimulationSelection);
		m_SoundData.swap(copy.m_SoundData);
		m_SubSoundSets.swap(copy.m_SubSoundSets);
	}
	return *this;
}

void SoundSet::Clear() {
	m_SoundSelectionCycleMode = SoundSelectionCycleMode::RANDOM;
	m_CurrentSelection = {false, -1};
	m_SimulationSelection = {false, -1};
	m_OwnerContainer = nullptr;

	m_SoundData.clear();
	m_SubSoundSets.clear();
}

int SoundSet::Create(const SoundSet& reference) {
	m_SoundSelectionCycleMode = reference.m_SoundSelectionCycleMode;
	m_CurrentSelection = reference.m_CurrentSelection;
	m_SimulationSelection = reference.m_SimulationSelection;
	for (SoundData referenceSoundData: reference.m_SoundData) {
		m_SoundData.push_back(std::move(referenceSoundData));
	}
	for (const SoundSet* referenceSoundSet: reference.m_SubSoundSets) {
		SoundSet* soundSet = new SoundSet(*referenceSoundSet);
		m_SubSoundSets.push_back(soundSet);
	}

	return 0;
}

int SoundSet::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return Serializable::ReadProperty(propName, reader));

	MatchProperty("SoundSelectionCycleMode", { SetSoundSelectionCycleMode(ReadSoundSelectionCycleMode(reader)); });
	MatchProperty("AddSound", { AddSoundData(ReadAndGetSoundData(reader)); });
	MatchProperty("AddSoundSet", {
		SoundSet soundSetToAdd;
		reader >> soundSetToAdd;
		AddSoundSet(soundSetToAdd);
	});
	MatchProperty("SpecialBehaviour_CurrentSelectionIsSet", { reader >> m_CurrentSelection.first; });
	MatchProperty("SpecialBehaviour_CurrentSelectionIndex", { reader >> m_CurrentSelection.second; });

	MatchProperty("SpecialBehaviour_SimulationSelection", { if (!LoadSimulationCheckpoint(base64_decode(reader.ReadPropValue()))) reader.ReportError("invalid simulation sound selection"); });
	EndPropertyList;
}

int SoundSet::Save(Writer& writer) const {
	Serializable::Save(writer);

	writer.NewProperty("SoundSelectionCycleMode");
	SaveSoundSelectionCycleMode(writer, m_SoundSelectionCycleMode);

	for (const SoundData& soundData: m_SoundData) {
		writer.NewProperty("AddSound");
		writer.ObjectStart("ContentFile");

		writer.NewProperty("FilePath");
		writer << soundData.SoundFile.GetDataPath();
		writer.NewProperty("Offset");
		writer << soundData.Offset;
		writer.NewProperty("MinimumAudibleDistance");
		writer << soundData.MinimumAudibleDistance;
		writer.NewProperty("AttenuationStartDistance");
		writer << soundData.AttenuationStartDistance;
		if (writer.IsSnapshot()) writer.NewPropertyWithValue("SpecialBehaviour_ContentCheckpoint", base64_encode(soundData.SoundFile.SaveCheckpoint(), true));

		writer.ObjectEnd();
	}

	for (const SoundSet* subSoundSet: m_SubSoundSets) {
		writer.NewPropertyWithValue("AddSoundSet", *subSoundSet);
	}
	writer.NewPropertyWithValue("SpecialBehaviour_CurrentSelectionIsSet", m_CurrentSelection.first);
	writer.NewPropertyWithValue("SpecialBehaviour_CurrentSelectionIndex", m_CurrentSelection.second);
	if (writer.IsSnapshot()) writer.NewPropertyWithValue("SpecialBehaviour_SimulationSelection", base64_encode(SaveSimulationCheckpoint(), true));

	return 0;
}

void SoundSet::Destroy() {
	for (const SoundSet* subSoundSet: m_SubSoundSets) {
		delete subSoundSet;
	}

	Clear();
}

SoundData SoundSet::ReadAndGetSoundData(Reader& reader) {
	SoundData soundData;

	/// <summary>
	/// Internal lambda function to load an audio file by path in as a ContentFile, which in turn loads it into FMOD, then returns SoundData for it in the outParam outSoundData.
	/// </summary>
	/// <param name="soundPath">The path to the sound file.</param>
	auto readSoundFromPath = [&soundData, &reader](const std::string& soundPath) {
		ContentFile soundFile(soundPath.c_str());

		/// As Serializable::Create(&reader) isn't being used here, we need to set our formatted reader position manually.
		soundFile.SetFormattedReaderPosition("in file " + reader.GetCurrentFilePath() + " on line " + reader.GetCurrentFileLine());

		FMOD::Sound* soundObject = soundFile.GetAsSound();
		if (g_AudioMan.IsAudioEnabled() && !soundObject) {
			reader.ReportError(std::string("Failed to load the sound from the file"));
		}

		soundData.SoundFile = soundFile;
		soundData.SoundObject = soundObject;
	};

	std::string propValue = reader.ReadPropValue();
	if (propValue != "Sound" && propValue != "ContentFile") {
		readSoundFromPath(propValue);
		return soundData;
	}

	while (reader.NextProperty()) {
		std::string soundSubPropertyName = reader.ReadPropName();
		if (soundSubPropertyName == "FilePath" || soundSubPropertyName == "Path") {
			readSoundFromPath(reader.ReadPropValue());
		} else if (soundSubPropertyName == "Offset") {
			reader >> soundData.Offset;
		} else if (soundSubPropertyName == "MinimumAudibleDistance") {
			reader >> soundData.MinimumAudibleDistance;
		} else if (soundSubPropertyName == "AttenuationStartDistance") {
			reader >> soundData.AttenuationStartDistance;
		} else if (soundSubPropertyName == "SpecialBehaviour_ContentCheckpoint") {
			if (!soundData.SoundFile.LoadCheckpoint(base64_decode(reader.ReadPropValue()))) reader.ReportError("invalid sound file checkpoint");
		}
	}

	return soundData;
}

SoundSet::SoundSelectionCycleMode SoundSet::ReadSoundSelectionCycleMode(Reader& reader) {
	SoundSelectionCycleMode soundSelectionCycleModeToReturn;
	std::string soundSelectionCycleModeString = reader.ReadPropValue();
	std::locale locale;
	for (char& character: soundSelectionCycleModeString) {
		character = std::tolower(character, locale);
	}

	std::unordered_map<std::string, SoundSelectionCycleMode>::const_iterator soundSelectionCycleMode = c_SoundSelectionCycleModeMap.find(soundSelectionCycleModeString);
	if (soundSelectionCycleMode != c_SoundSelectionCycleModeMap.end()) {
		soundSelectionCycleModeToReturn = soundSelectionCycleMode->second;
	} else {
		try {
			soundSelectionCycleModeToReturn = static_cast<SoundSelectionCycleMode>(std::stoi(soundSelectionCycleModeString));
		} catch (const std::exception&) {
			reader.ReportError("Sound selection cycle mode " + soundSelectionCycleModeString + " is invalid.");
		}
	}

	return soundSelectionCycleModeToReturn;
}

void SoundSet::SaveSoundSelectionCycleMode(Writer& writer, SoundSelectionCycleMode soundSelectionCycleMode) {
	auto cycleModeMapEntry = std::find_if(c_SoundSelectionCycleModeMap.begin(), c_SoundSelectionCycleModeMap.end(), [&soundSelectionCycleMode = soundSelectionCycleMode](auto element) { return element.second == soundSelectionCycleMode; });
	if (cycleModeMapEntry != c_SoundSelectionCycleModeMap.end()) {
		writer << cycleModeMapEntry->first;
	} else {
		RTEAbort("Tried to write invalid SoundSelectionCycleMode when saving SoundContainer/SoundSet.");
	}
}

SoundContainer* SoundSet::DeferringOwner() const {
	return SoundSimulationScope::Domain() == SoundExecutionDomain::LocalSimulation ? m_OwnerContainer : nullptr;
}

void SoundSet::AddSound(const std::string& soundFilePath, const Vector& offset, float minimumAudibleDistance, float attenuationStartDistance, bool abortGameForInvalidSound) {
	// From an AI hook a structural change is a decision like any other: it lands at the committed tick.
	if (SoundContainer* owner = DeferringOwner()) {
		std::vector<uint16_t> path;
		if (owner->FindSoundSetPath(*this, path)) {
			SoundData data;
			data.SoundFile = ContentFile(soundFilePath.c_str());
			data.Offset = offset;
			data.MinimumAudibleDistance = minimumAudibleDistance;
			data.AttenuationStartDistance = attenuationStartDistance;
			owner->QueuePendingStructure(SoundContainer::PendingOp::AddSound, std::move(path), SaveSoundData(data), 0, true);
			return;
		}
	}
	AddSoundNow(soundFilePath, offset, minimumAudibleDistance, attenuationStartDistance, abortGameForInvalidSound);
}

void SoundSet::AddSoundSet(const SoundSet& soundSetToAdd) {
	if (SoundContainer* owner = DeferringOwner()) {
		std::vector<uint16_t> path;
		if (owner->FindSoundSetPath(*this, path)) {
			owner->QueuePendingStructure(SoundContainer::PendingOp::AddSoundSet, std::move(path), soundSetToAdd.SaveStructure(), 0, HasAnySounds() || soundSetToAdd.HasAnySounds());
			return;
		}
	}
	AddSoundSetNow(soundSetToAdd);
}

void SoundSet::SetSoundSelectionCycleMode(SoundSelectionCycleMode newSoundSelectionCycleMode) {
	if (SoundContainer* owner = DeferringOwner()) {
		std::vector<uint16_t> path;
		if (owner->FindSoundSetPath(*this, path)) {
			m_PendingCycleMode = newSoundSelectionCycleMode;
			m_PendingCycleModeWritten = true;
			owner->QueuePendingStructure(SoundContainer::PendingOp::SetCycleMode, std::move(path), std::string(), newSoundSelectionCycleMode, HasAnySounds());
			return;
		}
	}
	SetSoundSelectionCycleModeNow(newSoundSelectionCycleMode);
}

std::string SoundSet::SaveSoundData(const SoundData& soundData) {
	CheckpointWriter writer("SoundData1");
	writer(soundData.SoundFile.GetDataPath(), soundData.Offset, soundData.MinimumAudibleDistance, soundData.AttenuationStartDistance);
	return writer.Text();
}

bool SoundSet::LoadSoundData(std::string_view text, SoundData& soundData) {
	try {
		std::string path;
		Vector offset;
		float minimumAudibleDistance = 0;
		float attenuationStartDistance = -1;
		CheckpointReader reader(text, "SoundData1");
		reader.Value(path);
		reader.Value(offset);
		reader.Value(minimumAudibleDistance);
		reader.Value(attenuationStartDistance);
		reader.Finish();
		soundData.SoundFile = ContentFile(path.c_str());
		soundData.SoundObject = nullptr;
		soundData.Offset = offset;
		soundData.MinimumAudibleDistance = minimumAudibleDistance;
		soundData.AttenuationStartDistance = attenuationStartDistance;
		return true;
	} catch (const std::exception&) { return false; }
}

std::string SoundSet::SaveStructure() const {
	CheckpointWriter writer("SoundSetStructure1");
	std::vector<std::string> sounds;
	sounds.reserve(m_SoundData.size());
	for (const SoundData& soundData: m_SoundData) sounds.push_back(SaveSoundData(soundData));
	std::vector<std::string> subSets;
	subSets.reserve(m_SubSoundSets.size());
	for (const SoundSet* subSoundSet: m_SubSoundSets) subSets.push_back(subSoundSet->SaveStructure());
	writer(static_cast<int>(m_SoundSelectionCycleMode), sounds, subSets);
	return writer.Text();
}

bool SoundSet::LoadStructure(std::string_view text) {
	try {
		int cycleMode = RANDOM;
		std::vector<std::string> sounds;
		std::vector<std::string> subSets;
		CheckpointReader reader(text, "SoundSetStructure1");
		reader.Value(cycleMode);
		reader.Value(sounds);
		reader.Value(subSets);
		reader.Finish();
		if (cycleMode < RANDOM || cycleMode > ALL) return false;
		Destroy();
		m_SoundSelectionCycleMode = static_cast<SoundSelectionCycleMode>(cycleMode);
		for (const std::string& sound: sounds) {
			SoundData data;
			if (!LoadSoundData(sound, data)) return false;
			AddSoundNow(data.SoundFile.GetDataPath(), data.Offset, data.MinimumAudibleDistance, data.AttenuationStartDistance, false);
		}
		for (const std::string& subSet: subSets) {
			SoundSet added;
			if (!added.LoadStructure(subSet)) return false;
			AddSoundSetNow(added);
		}
		return true;
	} catch (const std::exception&) { return false; }
}

void SoundSet::AddSoundNow(const std::string& soundFilePath, const Vector& offset, float minimumAudibleDistance, float attenuationStartDistance, bool abortGameForInvalidSound) {
	ContentFile soundFile(soundFilePath.c_str());
	FMOD::Sound* soundObject = soundFile.GetAsSound(abortGameForInvalidSound, false);
	if (!soundObject) {
		return;
	}

	m_SoundData.push_back({soundFile, soundObject, offset, minimumAudibleDistance, attenuationStartDistance});
}

bool SoundSet::RemoveSound(const std::string& soundFilePath, bool removeFromSubSoundSets) {
	if (SoundContainer* owner = DeferringOwner()) {
		std::vector<uint16_t> path;
		if (owner->FindSoundSetPath(*this, path)) {
			const bool found = HasSound(soundFilePath, removeFromSubSoundSets);
			owner->QueuePendingStructure(SoundContainer::PendingOp::RemoveSound, std::move(path), soundFilePath, removeFromSubSoundSets ? 1 : 0, HasAnySounds() && !(m_SoundData.size() == 1 && found));
			return found;
		}
	}
	return RemoveSoundNow(soundFilePath, removeFromSubSoundSets);
}

bool SoundSet::HasSound(const std::string& soundFilePath, bool includeSubSoundSets) const {
	for (const SoundData& soundData: m_SoundData) {
		if (soundData.SoundFile.GetDataPath() == soundFilePath) return true;
	}
	if (includeSubSoundSets) {
		for (const SoundSet* subSoundSet: m_SubSoundSets) {
			if (subSoundSet->HasSound(soundFilePath, includeSubSoundSets)) return true;
		}
	}
	return false;
}

bool SoundSet::RemoveSoundNow(const std::string& soundFilePath, bool removeFromSubSoundSets) {
	auto soundsToRemove = std::remove_if(m_SoundData.begin(), m_SoundData.end(), [&soundFilePath](const SoundData& soundData) { return soundData.SoundFile.GetDataPath() == soundFilePath; });
	bool anySoundsToRemove = soundsToRemove != m_SoundData.end();
	if (anySoundsToRemove) {
		m_SoundData.erase(soundsToRemove, m_SoundData.end());
	}
	if (removeFromSubSoundSets) {
		for (SoundSet* subSoundSet: m_SubSoundSets) {
			anySoundsToRemove |= RemoveSound(soundFilePath, removeFromSubSoundSets);
		}
	}
	return anySoundsToRemove;
}

bool SoundSet::HasAnySounds(bool includeSubSoundSets) const {
	bool hasAnySounds = !m_SoundData.empty();
	if (!hasAnySounds && includeSubSoundSets) {
		for (const SoundSet* subSoundSet: m_SubSoundSets) {
			hasAnySounds = subSoundSet->HasAnySounds();
			if (hasAnySounds) {
				break;
			}
		}
	}
	return hasAnySounds;
}

void SoundSet::GetFlattenedSoundData(std::vector<SoundData*>& flattenedSoundData, bool onlyGetSelectedSoundData) {
	if (!onlyGetSelectedSoundData || m_SoundSelectionCycleMode == SoundSelectionCycleMode::ALL) {
		for (SoundData& soundData: m_SoundData) {
			flattenedSoundData.push_back(&soundData);
		}
		for (SoundSet* subSoundSet: m_SubSoundSets) {
			subSoundSet->GetFlattenedSoundData(flattenedSoundData, onlyGetSelectedSoundData);
		}
	} else if (HasSelectedSounds()) {
		if (CurrentSelection().first == false) {
			flattenedSoundData.push_back(&m_SoundData[CurrentSelection().second]);
		} else {
			m_SubSoundSets[CurrentSelection().second]->GetFlattenedSoundData(flattenedSoundData, onlyGetSelectedSoundData);
		}
	}
}

void SoundSet::GetFlattenedSoundData(std::vector<const SoundData*>& flattenedSoundData, bool onlyGetSelectedSoundData) const {
	if (!onlyGetSelectedSoundData || m_SoundSelectionCycleMode == SoundSelectionCycleMode::ALL) {
		for (const SoundData& soundData: m_SoundData) {
			flattenedSoundData.push_back(&soundData);
		}
		for (const SoundSet* subSoundSet: m_SubSoundSets) {
			subSoundSet->GetFlattenedSoundData(flattenedSoundData, onlyGetSelectedSoundData);
		}
	} else if (HasSelectedSounds()) {
		if (CurrentSelection().first == false) {
			flattenedSoundData.push_back(&m_SoundData[CurrentSelection().second]);
		} else {
			m_SubSoundSets[CurrentSelection().second]->GetFlattenedSoundData(flattenedSoundData, onlyGetSelectedSoundData);
		}
	}
}

void SoundSet::SetOwnerContainer(SoundContainer* owner) {
	m_OwnerContainer = owner;
	for (SoundSet* subSoundSet: m_SubSoundSets) {
		subSoundSet->SetOwnerContainer(owner);
	}
}

bool SoundSet::SelectNextSounds() {
	// From an AI hook this is a decision, not an action: it lands on the one simulation selection at
	// the committed tick, like every other sound call the hook makes.
	if (m_OwnerContainer && SoundSimulationScope::Domain() == SoundExecutionDomain::LocalSimulation) {
		std::vector<uint16_t> path;
		if (m_OwnerContainer->FindSoundSetPath(*this, path)) {
			return m_OwnerContainer->QueuePendingSelectSounds(std::move(path));
		}
	}
	return SelectNextSoundsNow();
}

bool SoundSet::SelectNextSoundsNow() {
	if (m_SoundSelectionCycleMode == SoundSelectionCycleMode::ALL) {
		for (SoundSet* subSoundSet: m_SubSoundSets) {
			if (!subSoundSet->SelectNextSoundsNow()) {
				return false;
			}
		}
		return true;
	}
	int selectedVectorSize = CurrentSelection().first == false ? m_SoundData.size() : m_SubSoundSets.size();
	int unselectedVectorSize = CurrentSelection().first == true ? m_SoundData.size() : m_SubSoundSets.size();
	if (selectedVectorSize == 0 && unselectedVectorSize > 0) {
		CurrentSelection().first = !CurrentSelection().first;
		std::swap(selectedVectorSize, unselectedVectorSize);
	}

	/// <summary>
	/// Internal lambda function to pick a random sound that's not the previously played sound. Done to avoid scoping issues inside the switch below.
	/// </summary>
	auto selectSoundRandom = [&selectedVectorSize, &unselectedVectorSize, this]() {
		if (unselectedVectorSize > 0 && (selectedVectorSize == 1 || SoundSimulationScope::RandomNum(0, 1) == 1)) {
			std::swap(selectedVectorSize, unselectedVectorSize);
			CurrentSelection() = {!CurrentSelection().first, SoundSimulationScope::RandomNum(0, selectedVectorSize - 1)};
		} else {
			size_t soundToSelect = SoundSimulationScope::RandomNum(0, selectedVectorSize - 1);
			while (soundToSelect == CurrentSelection().second) {
				soundToSelect = SoundSimulationScope::RandomNum(0, selectedVectorSize - 1);
			}
			CurrentSelection().second = soundToSelect;
		}
	};

	/// <summary>
	/// Internal lambda function to pick the next sound in the forwards direction.
	/// </summary>
	auto selectSoundForwards = [&selectedVectorSize, &unselectedVectorSize, this]() {
		CurrentSelection().second++;
		if (CurrentSelection().second > selectedVectorSize - 1) {
			CurrentSelection().second = 0;
			if (unselectedVectorSize > 0) {
				CurrentSelection().first = !CurrentSelection().first;
				std::swap(selectedVectorSize, unselectedVectorSize);
			}
		}
	};

	switch (selectedVectorSize + unselectedVectorSize) {
		case 0:
			return false;
		case 1:
			CurrentSelection().second = 0;
			break;
		default:
			switch (m_SoundSelectionCycleMode) {
				case SoundSelectionCycleMode::RANDOM:
					selectSoundRandom();
					break;
				case SoundSelectionCycleMode::FORWARDS:
					selectSoundForwards();
					break;
				default:
					RTEAbort("Invalid sound selection sound cycle mode. " + m_SoundSelectionCycleMode);
					break;
			}
			RTEAssert(CurrentSelection().second >= 0 && CurrentSelection().second < selectedVectorSize, "Failed to select next sound, either none was selected or the selected sound was invalid.");
	}

	if (CurrentSelection().first == true) {
		return m_SubSoundSets[CurrentSelection().second]->SelectNextSoundsNow();
	}

	return true;
}

bool SoundSet::HasSelectedSounds() const {
    if (m_SoundSelectionCycleMode == ALL) return HasAnySounds();
    const auto& selection = CurrentSelection();
    if (selection.second < 0) return false;
    if (selection.first) return static_cast<size_t>(selection.second) < m_SubSoundSets.size() && m_SubSoundSets[selection.second]->HasSelectedSounds();
    return static_cast<size_t>(selection.second) < m_SoundData.size();
}
std::string SoundSet::SaveSimulationCheckpoint() const { CheckpointWriter writer("SoundSetSimulation2"); writer(m_SimulationSelection); return writer.Text(); }
bool SoundSet::LoadSimulationCheckpoint(std::string_view text, bool validateOnly) {
    const bool twoCohorts = text.starts_with("20 SoundSetSimulation1 ");
    try {
        std::pair<bool, int> selection;
        CheckpointReader reader(text, twoCohorts ? "SoundSetSimulation1" : "SoundSetSimulation2");
        reader.Value(selection);
        if (twoCohorts) { std::pair<bool, int> discarded; reader.Value(discarded); }
        reader.Finish();
        if (selection.second < -1 || (selection.second >= 0 && static_cast<size_t>(selection.second) >= (selection.first ? m_SubSoundSets.size() : m_SoundData.size()))) return false;
        if (!validateOnly) m_SimulationSelection = selection; return true;
    } catch (const std::exception&) { return false; }
}
