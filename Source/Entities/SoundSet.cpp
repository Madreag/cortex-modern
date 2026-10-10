#include "SoundSet.h"
#include "CheckpointNativeSnapshot.h"
#include "SoundContainer.h"
#include "CheckpointImage.h"
#include "CheckpointArchive.h"
#include "Base64/base64.h"
#include "AudioMan.h"
#include "RTETools.h"
#include "RTEError.h"

#include <bit>
#include <mutex>
#include <unordered_map>
#include <future>
#include <iostream>

using namespace RTE;

SoundData CheckpointNativeSnapshot::Freeze(const SoundData& source) {
	SoundData value = source;
	value.SoundObject = nullptr;
	return value;
}

size_t CheckpointNativeSnapshot::SoundSetKeyHash::operator()(const std::vector<uint64_t>& key) const noexcept {
	uint64_t hash = 1469598103934665603ULL;
	for (const uint64_t word: key) hash = (hash ^ word) * 1099511628211ULL;
	return static_cast<size_t>(hash);
}

std::shared_ptr<SoundSet> CheckpointNativeSnapshot::FreezeSoundSet(const std::shared_ptr<SoundSet>& source) {
	if (!source) return {};
	thread_local std::vector<uint64_t> key;
	key.clear();
	source->AppendFreezeKey(key);
	auto& shard = m_SoundSets[SoundSetKeyHash{}(key) % m_SoundSets.size()];
	{
		std::lock_guard lock(shard.mutex);
		if (const auto found = shard.sets.find(key); found != shard.sets.end()) return std::shared_ptr<SoundSet>(found->second, [](SoundSet*) {});
	}
	SoundSet* frozen = ValueObject(source.get());
	{
		std::lock_guard lock(shard.mutex);
		shard.sets.try_emplace(key, frozen);
	}
	return std::shared_ptr<SoundSet>(frozen, [](SoundSet*) {});
}

const std::string SoundSet::m_sClassName = "SoundSet";

const std::unordered_map<std::string, SoundSet::SoundSelectionCycleMode> SoundSet::c_SoundSelectionCycleModeMap = {
    {"random", SoundSelectionCycleMode::RANDOM},
    {"forwards", SoundSelectionCycleMode::FORWARDS},
    {"all", SoundSelectionCycleMode::ALL}};

SoundSet::SoundSet(const SoundSet& source, CheckpointNativeSnapshot& snapshot) :
	m_FrozenNative(true),
	m_SoundSelectionCycleMode(snapshot.Freeze(source.m_SoundSelectionCycleMode)),
	m_CurrentSelection(snapshot.Freeze(source.m_CurrentSelection)),
	m_SimulationSelection(snapshot.Freeze(source.m_SimulationSelection)),
	m_OwnerContainer(snapshot.Freeze(source.m_OwnerContainer)),
	m_PendingCycleMode{},
	m_PendingCycleModeWritten(false),
	m_SoundData(snapshot.Freeze(source.m_SoundData)),
	m_SoundDataSource(source.m_SoundDataSource),
	m_SubSoundSets(snapshot.Freeze(source.m_SubSoundSets, &m_SubSoundSets)),
	m_CheckpointInitialized(snapshot.Freeze(source.m_CheckpointInitialized)),
	m_CheckpointValueTrap(false),
	m_CheckpointOwner(nullptr) {
}

SoundSet::SoundSet() {
	Clear();
}

SoundSet::~SoundSet() {
	if (m_FrozenNative) return;
	Destroy();
}

SoundSet& SoundSet::operator=(const SoundSet& reference) {
	CheckpointChange changed(*this, [this] { return CheckpointStampValue(); });
	if (this != &reference) {
		SoundSet copy(reference);
		std::swap(m_SoundSelectionCycleMode, copy.m_SoundSelectionCycleMode);
		std::swap(m_CurrentSelection, copy.m_CurrentSelection);
		std::swap(m_SimulationSelection, copy.m_SimulationSelection);
		m_SoundData.swap(copy.m_SoundData);
		std::swap(m_SoundDataSource, copy.m_SoundDataSource);
		m_SubSoundSets.swap(copy.m_SubSoundSets);
		SetCheckpointOwner(m_CheckpointOwner);
		copy.SetCheckpointOwner(nullptr);
	}
	return *this;
}

void SoundSet::Clear() {
	if (m_CheckpointInitialized && (m_SoundSelectionCycleMode != RANDOM || m_CurrentSelection != std::pair(false, -1) || m_SimulationSelection != std::pair(false, -1) || !m_SoundData.empty() || !m_SubSoundSets.empty())) TouchCheckpoint();
	m_CheckpointInitialized = true;
	m_SoundSelectionCycleMode = SoundSelectionCycleMode::RANDOM;
	m_CurrentSelection = {false, -1};
	m_SimulationSelection = {false, -1};
	m_OwnerContainer = nullptr;
	m_CheckpointOwner = nullptr;

	m_SoundData.clear();
	m_SoundDataSource = 0;
	m_SubSoundSets.clear();
}

void SoundSet::TouchCheckpoint() {
	if (m_CheckpointOwner) m_CheckpointOwner->TouchCheckpoint();
	if (m_CheckpointValueTrap) {
		m_CheckpointValueTrap = false;
		CheckpointValueWritten(this);
	}
}

void SoundSet::SetCheckpointOwner(SoundContainer* owner) {
	m_CheckpointOwner = owner;
	for (SoundSet* child: m_SubSoundSets) child->SetCheckpointOwner(owner);
}

uint64_t SoundSet::NameSoundData(const std::vector<SoundData>& data) {
	if (data.empty()) return 0;
	// Equal sound data, byte for byte, always gets the same name; the backend sound is not part of it.
	struct Names { std::mutex mutex; std::unordered_map<std::string, uint64_t> values; };
	static Names* names = new Names;
	std::string key;
	for (const SoundData& sound: data) {
		const std::string file = sound.SoundFile.SaveCheckpoint();
		const size_t size = file.size();
		const float values[] = {sound.Offset.m_X, sound.Offset.m_Y, sound.MinimumAudibleDistance, sound.AttenuationStartDistance};
		key.append(reinterpret_cast<const char*>(&size), sizeof(size));
		key += file;
		key.append(reinterpret_cast<const char*>(values), sizeof(values));
	}
	std::lock_guard lock(names->mutex);
	return names->values.try_emplace(std::move(key), names->values.size() + 1).first->second;
}

void SoundSet::AppendFreezeKey(std::vector<uint64_t>& key) const {
	key.insert(key.end(), {m_SoundDataSource, static_cast<uint64_t>(m_SoundSelectionCycleMode), m_CurrentSelection.first, static_cast<uint32_t>(m_CurrentSelection.second),
	    m_SimulationSelection.first, static_cast<uint32_t>(m_SimulationSelection.second), m_CheckpointInitialized, m_SubSoundSets.size()});
	for (const SoundSet* child: m_SubSoundSets) child->AppendFreezeKey(key);
}

std::vector<std::pair<bool, int>> SoundSet::CheckpointSelections() const {
	std::vector<std::pair<bool, int>> selections{m_CurrentSelection, m_SimulationSelection};
	for (const SoundSet* child: m_SubSoundSets) {
		const auto nested = child->CheckpointSelections();
		selections.insert(selections.end(), nested.begin(), nested.end());
	}
	return selections;
}

int SoundSet::Create(const SoundSet& reference) {
	m_SoundSelectionCycleMode = reference.m_SoundSelectionCycleMode;
	m_CurrentSelection = reference.m_CurrentSelection;
	m_SimulationSelection = reference.m_SimulationSelection;
	const bool copied = m_SoundData.empty();
	for (SoundData referenceSoundData: reference.m_SoundData) {
		m_SoundData.push_back(std::move(referenceSoundData));
	}
	m_SoundDataSource = copied ? reference.m_SoundDataSource : NameSoundData(m_SoundData);
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
	if (writer.IsCapturing() && CheckpointWriter::BatchEnabled()) {
		struct File {
			std::string path;
			float x, y, minimumDistance, attenuationDistance;
			CheckpointText checkpoint;
		};
		struct Fields {
			SoundSelectionCycleMode cycle;
			unsigned selectionIsSet;
			int selectionIndex;
			std::vector<File> files;
			std::vector<CheckpointText> subsets;
			CheckpointText simulation;
		};
		Fields fields{m_SoundSelectionCycleMode, static_cast<unsigned>(m_CurrentSelection.first), m_CurrentSelection.second};
		fields.files.reserve(m_SoundData.size());
		size_t bytes = sizeof(Fields);
		for (const SoundData& data: m_SoundData) {
			CheckpointText checkpoint = CheckpointWriter::Native([&] { return data.SoundFile.SaveCheckpoint(); });
			bytes += sizeof(File) + data.SoundFile.GetDataPath().size() + checkpoint.OwnedBytes();
			fields.files.push_back({data.SoundFile.GetDataPath(), data.Offset.m_X, data.Offset.m_Y,
			    data.MinimumAudibleDistance, data.AttenuationStartDistance, std::move(checkpoint)});
		}
		const int indent = writer.GetIndent();
		fields.subsets.reserve(m_SubSoundSets.size());
		for (const SoundSet* subset: m_SubSoundSets) {
			fields.subsets.push_back(Writer::Capture([subset](Writer& owned) { owned.NewPropertyWithValue("AddSoundSet", *subset); }, indent));
			bytes += sizeof(CheckpointText) + fields.subsets.back().OwnedBytes();
		}
		fields.simulation = CheckpointWriter::Native([&] { return SaveSimulationCheckpoint(); });
		bytes += fields.simulation.OwnedBytes();
		// The saver formats owned sound fields in their archive order.
		writer.Append(CheckpointText::Deferred([fields = std::move(fields), indent] {
			return Writer::Capture([&](Writer& owned) {
				owned.NewProperty("SoundSelectionCycleMode");
				SaveSoundSelectionCycleMode(owned, fields.cycle);
				for (const File& file: fields.files) {
					owned.NewProperty("AddSound");
					owned.ObjectStart("ContentFile");
					owned.NewPropertyWithValue("FilePath", file.path);
					owned.NewPropertyWithValue("Offset", Vector(file.x, file.y));
					owned.NewPropertyWithValue("MinimumAudibleDistance", file.minimumDistance);
					owned.NewPropertyWithValue("AttenuationStartDistance", file.attenuationDistance);
					owned.NewPropertyWithValue("SpecialBehaviour_ContentCheckpoint", file.checkpoint.Base64(true));
					owned.ObjectEnd();
				}
				for (const CheckpointText& subset: fields.subsets) owned.Append(subset);
				owned.NewPropertyWithValue("SpecialBehaviour_CurrentSelectionIsSet", fields.selectionIsSet);
				owned.NewPropertyWithValue("SpecialBehaviour_CurrentSelectionIndex", fields.selectionIndex);
				owned.NewPropertyWithValue("SpecialBehaviour_SimulationSelection", fields.simulation.Base64(true));
			}, indent).Text();
		}, bytes));
		return 0;
	}

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
		if (writer.IsSnapshot()) writer.NewPropertyWithValue("SpecialBehaviour_ContentCheckpoint", CheckpointWriter::Native([&] { return soundData.SoundFile.SaveCheckpoint(); }).Base64(true));

		writer.ObjectEnd();
	}

	for (const SoundSet* subSoundSet: m_SubSoundSets) {
		writer.NewPropertyWithValue("AddSoundSet", *subSoundSet);
	}
	writer.NewPropertyWithValue("SpecialBehaviour_CurrentSelectionIsSet", m_CurrentSelection.first);
	writer.NewPropertyWithValue("SpecialBehaviour_CurrentSelectionIndex", m_CurrentSelection.second);
	if (writer.IsSnapshot()) writer.NewPropertyWithValue("SpecialBehaviour_SimulationSelection", CheckpointWriter::Native([&] { return SaveSimulationCheckpoint(); }).Base64(true));

	return 0;
}

bool RTE::RunOwnedSoundSetCaptureSelfTest() {
	bool passed = true;
	for (const auto cycle: {SoundSet::RANDOM, SoundSet::FORWARDS, SoundSet::ALL}) {
		std::string expected;
		CheckpointText captured;
		{
			auto source = std::make_unique<SoundSet>();
			source->SetSoundSelectionCycleModeNow(cycle);
			SoundData file;
			file.SoundObject = nullptr;
			file.SoundFile.Create("CheckpointCapture.rte/Sounds/A.wav");
			file.SoundFile.SetFormattedReaderPosition("owned sound definition source");
			file.Offset.m_X = -0.0F;
			file.Offset.m_Y = std::bit_cast<float>(uint32_t{0x7fc01234});
			file.MinimumAudibleDistance = 1.25F;
			file.AttenuationStartDistance = -1.0F;
			source->AddSoundData(file);
			file.SoundFile.Create("CheckpointCapture.rte/Sounds/B.wav");
			file.Offset.SetXY(3.5F, 9.75F);
			source->AddSoundData(file);
			auto child = std::make_unique<SoundSet>();
			child->SetSoundSelectionCycleModeNow(SoundSet::FORWARDS);
			child->AddSoundData(file);
			source->GetSubSoundSets().push_back(child.release());
			const auto save = [&] { return Writer::Capture([&](Writer& owned) { owned << *source; }, 1); };
			expected = save().Text();
			{
				CheckpointWriter::BatchScope batch(true);
				captured = save();
			}
			source->Destroy();
		}
		auto worker = std::async(std::launch::async, [captured] { return std::pair{captured.Text(), captured.SharedText()}; });
		const auto [full, shared] = worker.get();
		passed = full == expected && shared == expected && passed;
	}
	std::cout << "[script-graph-selftest] " << (passed ? "PASS" : "FAIL")
	          << " owned_sound_definitions_outlive_their_sources files, subsets, float bits and all selection modes stay exact" << std::endl;
	return passed;
}

bool RTE::RunOwnedSoundParametersCaptureSelfTest() {
	std::string expected, expectedShared;
	CheckpointText captured;
	{
		auto source = std::make_unique<SoundContainer>();
		source->SetPosition(Vector(-0.0F, 13.25F));
		source->SetVolume(0.625F);
		source->SetPitch(1.25F);
		source->SetPitchVariation(0.375F);
		source->SetLoopSetting(3);
		source->SetBusRouting(SoundContainer::BusRouting::MUSIC);
		source->SetMusicPreEntryTime(5.5F);
		source->SetMusicExitTime(17.75F);
		source->SetPaused(true);
		const auto save = [&] { return Writer::Capture([&](Writer& owned) { owned << *source; }, 1); };
		const CheckpointText ordinary = save();
		expected = ordinary.Text();
		expectedShared = ordinary.SharedText();
		{
			CheckpointWriter::BatchScope batch(true);
			captured = save();
		}
		source->SetPitch(0.75F);
		source->SetVolume(1.0F);
		source->SetPosition(Vector(99, 101));
	}
	auto worker = std::async(std::launch::async, [captured] { return std::pair{captured.Text(), captured.SharedText()}; });
	const auto [full, shared] = worker.get();
	const bool passed = full == expected && shared == expectedShared;
	std::cout << "[script-graph-selftest] " << (passed ? "PASS" : "FAIL")
	          << " owned_sound_parameters_outlive_their_sources full and shared bytes preserve parameters and runtime peer fields" << std::endl;
	return passed;
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
	std::vector<CheckpointText> sounds;
	sounds.reserve(m_SoundData.size());
	for (const SoundData& soundData: m_SoundData) sounds.push_back(CheckpointWriter::Native([&] { return SaveSoundData(soundData); }));
	std::vector<CheckpointText> subSets;
	subSets.reserve(m_SubSoundSets.size());
	for (const SoundSet* subSoundSet: m_SubSoundSets) subSets.push_back(CheckpointWriter::Native([subSoundSet] { return subSoundSet->SaveStructure(); }));
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
	m_SoundDataSource = NameSoundData(m_SoundData);
	TouchCheckpoint();
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
		TouchCheckpoint();
		m_SoundData.erase(soundsToRemove, m_SoundData.end());
		m_SoundDataSource = NameSoundData(m_SoundData);
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
	m_CheckpointOwner = owner;
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
	CheckpointChange changed(*this, [this] { return CheckpointFields(m_CurrentSelection, m_SimulationSelection); });
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
