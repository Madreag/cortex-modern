#include "ContentFile.h"
#include "SteadyClock.h"
#include "CheckpointNativeSnapshot.h"
#include "CheckpointFrozenContainers.h"
#include "CaptureSentinel.h"
#include "CheckpointArchive.h"
#include "Base64/base64.h"

#include "AudioMan.h"
#include "AudioCheckpoint.h"
#include "PresetMan.h"
#include "ConsoleMan.h"
#include "RTETools.h"
#include "System.h"
#include "FrameMan.h"

#include "png.h"
#include "fmod/fmod.hpp"
#include "fmod/fmod_errors.h"

#include <mutex>
#include <iostream>
#include <SDL3_image/SDL_image.h>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <thread>

using namespace RTE;

std::string ContentFile::SaveCheckpoint() const {
	CheckpointWriter archive("ContentFile1");
	archive(m_DataPath, m_DataPathExtension, m_DataPathWithoutExtension, m_DataPathIsImageFile, m_ImageFileInfo, m_FormattedReaderPosition, m_DataPathAndReaderPosition, m_DataModuleID, m_IsMemoryPNG);
	return archive.Text();
}

bool ContentFile::LoadCheckpoint(std::string_view text, bool validateOnly) {
	return LoadCheckpoint(text, validateOnly, true);
}

bool ContentFile::LoadCheckpoint(std::string_view text, bool validateOnly, bool registerPath) {
	try {
		CheckpointReader archive(text, "ContentFile1", validateOnly);
		archive(m_DataPath, m_DataPathExtension, m_DataPathWithoutExtension, m_DataPathIsImageFile, m_ImageFileInfo, m_FormattedReaderPosition, m_DataPathAndReaderPosition, m_DataModuleID, m_IsMemoryPNG);
		if (registerPath) archive.OnCommit([this] { if (!m_DataPath.empty()) RegisterPath(); });
		archive.Finish();
		return true;
	} catch (const std::exception&) { return false; }
}

void ContentFile::SwapCheckpoint(ContentFile& other) noexcept {
	using std::swap;
	swap(m_DataPath, other.m_DataPath); swap(m_DataPathExtension, other.m_DataPathExtension); swap(m_DataPathWithoutExtension, other.m_DataPathWithoutExtension);
	swap(m_DataPathIsImageFile, other.m_DataPathIsImageFile); swap(m_ImageFileInfo, other.m_ImageFileInfo);
	swap(m_FormattedReaderPosition, other.m_FormattedReaderPosition); swap(m_DataPathAndReaderPosition, other.m_DataPathAndReaderPosition);
	swap(m_DataModuleID, other.m_DataModuleID); swap(m_IsMemoryPNG, other.m_IsMemoryPNG);
}

const std::string ContentFile::c_ClassName = "ContentFile";

std::array<ContentFile::LoadedBitmaps, ContentFile::BitDepths::BitDepthCount> ContentFile::s_LoadedBitmaps;

namespace {
	// Per bitmap, the least path each depth's cache holds it under; rebuilt when the cache's version moved.
	struct LoadedBitmapIndex {
		uint64_t version = ~uint64_t{0};
		std::unordered_map<const BITMAP*, std::array<const std::string*, 2>> paths;
		std::atomic<int> scopes{0};
		std::mutex rebuild;
	};
	LoadedBitmapIndex& Index() {
		static LoadedBitmapIndex index;
		return index;
	}
	struct SavedBitmapIndex {
		std::unordered_map<const BITMAP*, std::array<std::optional<std::string>, 2>> paths;
		std::unordered_map<std::string, std::array<BITMAP*, 2>> images;
		std::vector<std::unique_ptr<BITMAP, void(*)(BITMAP*)>> owned;
	};
	struct SavedPathHashes {
		std::unordered_map<size_t, std::string> paths;
	};
	struct SavedSamples {
		FMOD::System* system = nullptr;
		std::unordered_map<std::string, FMOD::Sound*> sounds;
		std::unordered_map<std::string, AudioCheckpoint::Sample> values;
		~SavedSamples() { if (system) system->release(); }
	};
	struct LoadedSampleValues : CheckpointNativeAllocated {
		FMOD::Sound* sound = nullptr;
		AudioCheckpoint::SampleValues values;
		std::atomic<bool> published{false};
		FMOD_RESULT failure = FMOD_OK;
		void Publish(const AudioCheckpoint::SampleValues& captured) {
			values = captured; failure = FMOD_OK;
			published.store(captured.captured, std::memory_order_release);
		}
	};
	using SampleRegistry = CheckpointUnorderedMap<CheckpointString, std::shared_ptr<LoadedSampleValues>>;
	SampleRegistry s_SampleValues = [] { CheckpointNativeStorage::AllocationScope resources(true); return SampleRegistry{}; }();
	std::atomic<bool> s_SampleValuesReady{true};
	struct SampleLoadCallbacks {
		std::mutex mutex;
		std::unordered_map<FMOD::Sound*, std::shared_ptr<LoadedSampleValues>> values;
	};
	SampleLoadCallbacks& SampleCallbacks() { static SampleLoadCallbacks callbacks; return callbacks; }
	FMOD_RESULT F_CALLBACK SampleLoaded(FMOD_SOUND* handle, FMOD_RESULT result) {
		if (result != FMOD_OK) return FMOD_OK;
		try {
			auto& callbacks = SampleCallbacks();
			std::shared_ptr<LoadedSampleValues> record;
			{ std::lock_guard lock(callbacks.mutex); const auto found = callbacks.values.find(reinterpret_cast<FMOD::Sound*>(handle)); if (found != callbacks.values.end()) record = found->second; }
			if (record) {
				const auto values = AudioCheckpoint::SampleValues::Capture(record->sound, true);
				std::lock_guard lock(callbacks.mutex);
				if (!record->published.load(std::memory_order_acquire)) record->Publish(values);
			}
		} catch (...) { s_SampleValuesReady.store(false, std::memory_order_release); }
		return FMOD_OK;
	}
	std::shared_ptr<SavedPathHashes> SavedPaths() {
		static const char kind{};
		const auto [source, bytes] = ContentFile::PathHashRegistry();
		return std::const_pointer_cast<SavedPathHashes>(std::static_pointer_cast<const SavedPathHashes>(CheckpointNativeStorage::ReadIndex(source, &kind, [&] {
			using Registry = CheckpointUnorderedMap<size_t, CheckpointString>;
			if (bytes != sizeof(Registry)) throw std::logic_error("saved path registry size changed");
			auto values = std::make_shared<SavedPathHashes>();
			const auto* registry = static_cast<const Registry*>(CheckpointNativeStorage::View(source, bytes));
			CheckpointForEachValue(*registry, [&](const auto& value) { values->paths.emplace(value.first, value.second.Value()); });
			return values;
		})));
	}
	std::shared_ptr<const SavedBitmapIndex> SavedIndex() {
		static const char kind{};
		const auto [source, bytes] = ContentFile::LoadedBitmapRegistry();
		return std::static_pointer_cast<const SavedBitmapIndex>(CheckpointNativeStorage::ReadIndex(source, &kind, [&] {
			auto index = std::make_shared<SavedBitmapIndex>();
			using Registry = CheckpointUnorderedMap<CheckpointString, BITMAP*>;
			static_assert(sizeof(Registry) * 2 == sizeof(std::array<Registry, 2>));
			if (bytes != sizeof(std::array<Registry, 2>)) throw std::logic_error("saved bitmap registry size changed");
			const auto* registry = static_cast<const Registry*>(CheckpointNativeStorage::View(source, bytes));
			for (size_t depth = 0; depth < 2; ++depth) CheckpointForEachValue(registry[depth], [&](const auto& entry) {
				const auto& path = entry.first.Value();
				index->images[path][depth] = entry.second;
				auto& least = index->paths[entry.second][depth];
				if (!least || path < *least) least = path;
			});
			return index;
		}));
	}
}

std::pair<const void*, size_t> ContentFile::LoadedBitmapRegistry() {
	static_assert(sizeof(LoadedBitmaps) == sizeof(LoadedBitmaps::Base));
	return {s_LoadedBitmaps.data(), sizeof(s_LoadedBitmaps)};
}

std::pair<const void*, size_t> ContentFile::PathHashRegistry() { return {&s_PathHashes, sizeof(s_PathHashes)}; }

std::pair<const void*, size_t> ContentFile::LoadedSampleRegistry() { return {&s_SampleValues, sizeof(s_SampleValues)}; }

bool ContentFile::CheckpointSamplesReady() noexcept { return s_SampleValuesReady.load(std::memory_order_acquire); }

void ContentFile::InvalidateCheckpointSamples() noexcept { s_SampleValuesReady.store(false, std::memory_order_release); }

void ContentFile::RefreshCheckpointSample(const std::string& path, FMOD::Sound* sound) noexcept {
	if (CheckpointNativeStorage::WorldEnabled() && !CheckpointNativeStorage::Reading()) NoteLoadedSample(path, sound, true);
}

void ContentFile::NoteLoadedSample(const std::string& path, FMOD::Sound* sound, bool changed) noexcept {
	if (!CheckpointNativeStorage::WorldEnabled() || CheckpointNativeStorage::Reading()) return;
	try {
		CheckpointNativeStorage::AllocationScope resources(true);
		auto& record = s_SampleValues[path];
		if (!record || record->sound != sound) { record = std::shared_ptr<LoadedSampleValues>(new LoadedSampleValues); record->sound = sound; changed = true; }
		auto& callbacks = SampleCallbacks();
		{ std::lock_guard lock(callbacks.mutex); if (sound) callbacks.values[sound] = record; }
		if (changed || !record->published.load(std::memory_order_acquire)) {
			const auto values = AudioCheckpoint::SampleValues::Capture(sound);
			std::lock_guard lock(callbacks.mutex);
			if (values.captured || !record->published.load(std::memory_order_acquire)) record->Publish(values);
		}
	} catch (...) { InvalidateCheckpointSamples(); }
}

void ContentFile::PrepareCheckpointSamples() {
	if (CheckpointSamplesReady()) return;
	s_SampleValuesReady.store(true, std::memory_order_release);
	for (const auto& [path, sound]: s_LoadedSamples) NoteLoadedSample(path, sound, true);
	std::erase_if(s_SampleValues, [](const auto& value) { return !s_LoadedSamples.contains(value.first.Value()); });
}

std::vector<AudioCheckpoint::Sample> ContentFile::ReadCheckpointSamples() {
	if (!CheckpointNativeStorage::Reading()) throw std::logic_error("saved samples need their frozen registry");
	const auto* registry = CheckpointNativeStorage::Source(&s_SampleValues);
	std::map<std::string, AudioCheckpoint::Sample> ordered;
	CheckpointForEachValue(*registry, [&](const auto& entry) {
		const auto* record = CheckpointNativeStorage::Source(entry.second.get());
		if (!record->sound) return;
		if (record->failure != FMOD_OK) throw UnsupportedCheckpointNative("saved sample metadata could not be read");
		AudioCheckpoint::Sample sample; sample.path = entry.first.Value();
		if (record->published.load(std::memory_order_acquire)) static_cast<AudioCheckpoint::SampleValues&>(sample) = record->values;
		ordered.emplace(sample.path, std::move(sample));
	});
	std::vector<AudioCheckpoint::Sample> values;
	values.reserve(ordered.size());
	for (auto& [path, sample]: ordered) values.push_back(std::move(sample));
	return values;
}

std::string ContentFile::SavedSampleValuesMismatch() {
	CheckpointNativeStorage::AllocationScope storage(true);
	const std::string path = "checkpoint-selftest/saved-sample.wav";
	auto record = std::shared_ptr<LoadedSampleValues>(new LoadedSampleValues);
	record->sound = reinterpret_cast<FMOD::Sound*>(record.get());
	record->values.frequency = 44100; record->values.priority = 17; record->values.loopEnd = 23; record->values.captured = true;
	record->published.store(true);
	auto previous = s_SampleValues.extract(path);
	struct Restore { std::string path; SampleRegistry::node_type previous; ~Restore() { s_SampleValues.erase(path); if (previous) s_SampleValues.insert(std::move(previous)); } } restore{path, std::move(previous)};
	s_SampleValues[path] = record;
	AudioCheckpoint::Sample before; before.path = path; static_cast<AudioCheckpoint::SampleValues&>(before) = record->values;
	CheckpointNativeStorage::Root<SampleRegistry> root(&s_SampleValues);
	auto inventory = CheckpointNativeStorage::PrepareInventory();
	if (!inventory->Arm()) return "saved sample inventory was refused";
	root.Freeze();
	record->values.frequency = 22050; record->values.loopEnd = 47; s_SampleValues.erase(path);
	const std::array headers{root.Range()};
	CheckpointNativeStorage::ReadScope read(inventory->Pages(), headers);
	const auto values = ReadCheckpointSamples();
	const auto saved = std::find_if(values.begin(), values.end(), [&](const auto& sample) { return sample.path == path; });
	return saved != values.end() && saved->SaveCheckpoint() == before.SaveCheckpoint() ? std::string{} : "saved sample follows later values or cache removal";
}

BITMAP* ContentFile::ReadLoadedBitmap(const std::string& path, int depth) {
	if (!CheckpointNativeStorage::Reading() || depth < 0 || depth >= BitDepths::BitDepthCount) throw std::logic_error("saved bitmap lookup needs its frozen registry");
	const auto index = SavedIndex();
	const auto found = index->images.find(path);
	return found == index->images.end() ? nullptr : found->second[depth];
}

std::string ContentFile::GetPathFromHash(size_t hash) {
	if (CheckpointNativeStorage::Reading()) {
		const auto values = SavedPaths();
		const auto found = values->paths.find(hash);
		return found == values->paths.end() ? std::string{} : found->second;
	}
	const auto found = s_PathHashes.find(hash);
	return found == s_PathHashes.end() ? std::string{} : found->second.Value();
}

void ContentFile::RegisterPath() const {
	if (CheckpointNativeStorage::Reading()) SavedPaths()->paths[GetHash()] = m_DataPath.Value();
	else {
		CheckpointNativeStorage::AllocationScope resources(true);
		s_PathHashes[GetHash()] = m_DataPath;
	}
}

void ContentFile::LoadedBitmapIndexScope::Refresh() {
	if (CheckpointNativeStorage::Reading()) { SavedIndex(); return; }
	LoadedBitmapIndex& index = Index();
	std::lock_guard lock(index.rebuild);
	if (index.scopes.load() == 0) RebuildLoadedBitmapIndex();
}

ContentFile::LoadedBitmapIndexScope::LoadedBitmapIndexScope() : m_Saved(CheckpointNativeStorage::Reading()) {
	if (m_Saved) { SavedIndex(); return; }
	LoadedBitmapIndex& index = Index();
	std::lock_guard lock(index.rebuild);
	if (index.scopes.fetch_add(1) == 0) RebuildLoadedBitmapIndex();
}

void ContentFile::RebuildLoadedBitmapIndex() {
	LoadedBitmapIndex& index = Index();
	const auto& registry = std::as_const(s_LoadedBitmaps);
	static_assert(std::tuple_size_v<std::decay_t<decltype(registry)>> == 2);
	if (index.version != LoadedBitmaps::Version()) {
		CaptureSentinel::NoteCreation("loaded-bitmap index", &index);
		index.paths.clear();
		size_t count = 0;
		for (const auto& loaded: registry) count += loaded.size();
		index.paths.reserve(count);
		for (size_t depth = 0; depth < registry.size(); ++depth) {
			for (const auto& [path, bitmap]: registry[depth]) {
				const std::string*& least = index.paths[bitmap][depth];
				if (!least || path < *least) least = &path.Value();
			}
		}
		index.version = LoadedBitmaps::Version();
	}
}

ContentFile::LoadedBitmapIndexScope::~LoadedBitmapIndexScope() {
	if (!m_Saved) --Index().scopes;
}

std::string ContentFile::LoadedBitmapChangeMissedByIndex() {
	CheckpointNativeStorage::AllocationScope storage(true);
	// Only the scratch bitmaps' addresses are compared; nothing reads through them.
	static const std::array<char, 2> scratch{};
	BITMAP* first = reinterpret_cast<BITMAP*>(const_cast<char*>(&scratch[0]));
	BITMAP* second = reinterpret_cast<BITMAP*>(const_cast<char*>(&scratch[1]));
	const std::string key = "checkpoint-selftest/loaded-bitmap-index-row.png";
	LoadedBitmaps& registry = s_LoadedBitmaps[BitDepths::Eight];
	// The live entries step aside for the ways that clear or walk the whole registry, and come back after.
	LoadedBitmaps::Base live;
	registry.swap(live);
	// A stale index answers with a path the registry no longer holds, so an absence is checked without reading it.
	const auto present = [&key](const BITMAP* bitmap) {
		LoadedBitmapIndexScope scope;
		int depth = BitDepths::Eight;
		const std::string* path = LoadedBitmapPath(bitmap, depth);
		return path && *path == key;
	};
	const auto absent = [](const BITMAP* bitmap) {
		LoadedBitmapIndexScope scope;
		int depth = BitDepths::Eight;
		return LoadedBitmapPath(bitmap, depth) == nullptr;
	};
	std::string missed;
	const auto way = [&missed](const char* name, bool seen) {
		if (!seen && missed.empty()) missed = name;
	};
	absent(first);
	registry[key] = first;
	way("operator[]", present(first));
	registry.erase(key);
	way("erase(key)", absent(first));
	registry.try_emplace(key, first);
	way("try_emplace", present(first));
	registry.erase(std::as_const(registry).find(key));
	way("erase(position)", absent(first));
	registry.emplace(key, first);
	way("emplace", present(first));
	registry.find(key)->second = second;
	way("find", present(second) && absent(first));
	registry.at(key) = first;
	way("at", present(first) && absent(second));
	registry.begin()->second = second;
	way("begin", present(second) && absent(first));
	registry.clear();
	way("clear", absent(second));
	registry.insert(LoadedBitmaps::value_type(key, first));
	way("insert", present(first));
	LoadedBitmaps::Base emptied;
	registry.swap(emptied);
	way("swap", absent(first));
	registry.merge(emptied);
	way("merge", present(first));
	registry.clear();
	registry[key] = first;
	{
		CheckpointNativeStorage::Root<decltype(s_LoadedBitmaps)> root(&s_LoadedBitmaps);
		auto inventory = CheckpointNativeStorage::PrepareInventory();
		if (!inventory->Arm()) way("frozen inventory", false);
		else {
			root.Freeze();
			registry.clear(); registry[key + ".later"] = second;
			const std::array headers{root.Range()};
			CheckpointNativeStorage::ReadScope read(inventory->Pages(), headers);
			way("frozen path after cache removal", present(first) && absent(second));
		}
	}
	registry.clear();
	registry.swap(live);
	{
		const std::string path = "checkpoint-selftest/saved-path.png";
		const std::string privatePath = path + ".private";
		const size_t hash = Hash(path);
		const size_t privateHash = Hash(privatePath);
		const std::string privateBefore = GetPathFromHash(privateHash);
		const auto existing = s_PathHashes.find(hash);
		const std::optional<CheckpointString> previous = existing == s_PathHashes.end() ? std::nullopt : std::optional(existing->second);
		s_PathHashes[hash] = path;
		CheckpointNativeStorage::Root<decltype(s_PathHashes)> root(&s_PathHashes);
		auto inventory = CheckpointNativeStorage::PrepareInventory();
		if (!inventory->Arm()) way("frozen path inventory", false);
		else {
			root.Freeze();
			s_PathHashes[hash] = "live path after capture";
			const std::array headers{root.Range()};
			CheckpointNativeStorage::ReadScope read(inventory->Pages(), headers);
			way("frozen path value", GetPathFromHash(hash) == path);
			ContentFile saved;
			saved.m_DataPath = privatePath;
			saved.RegisterPath();
			way("private path registration", GetPathFromHash(saved.GetHash()) == saved.m_DataPath.Value());
		}
		way("live path stays unchanged", GetPathFromHash(hash) == "live path after capture");
		way("private path stays private", GetPathFromHash(privateHash) == privateBefore);
		if (previous) s_PathHashes[hash] = *previous; else s_PathHashes.erase(hash);
	}
	{
		const std::string path = "checkpoint-selftest/saved-image.png";
		auto snapshot = std::make_unique<CheckpointNativeSnapshot>();
		auto image = std::unique_ptr<BITMAP, void(*)(BITMAP*)>(create_bitmap_ex(8, 7, 5), destroy_bitmap);
		if (!image) throw std::bad_alloc();
		clear_to_color(image.get(), 37);
		const auto existing = std::as_const(registry).find(path);
		BITMAP* previous = existing == std::as_const(registry).end() ? nullptr : existing->second;
		registry[path] = image.get();
		CheckpointNativeStorage::Root<decltype(s_LoadedBitmaps)> root(&s_LoadedBitmaps);
		CheckpointNativeStorage::Root<decltype(s_PathHashes)> pathRoot(&s_PathHashes);
		auto inventory = CheckpointNativeStorage::PrepareInventory();
		if (!inventory->Arm()) way("frozen image inventory", false);
		else {
			root.Freeze();
			pathRoot.Freeze();
			clear_to_color(image.get(), 41);
			registry.erase(path);
			const std::array headers{root.Range(), pathRoot.Range()};
			CheckpointNativeStorage::ReadScope read(inventory->Pages(), headers);
			snapshot->SealBoundary();
			CheckpointNativeSnapshot::ReadScope savedValues(snapshot.get());
			ContentFile saved;
			saved.m_DataPath = path;
			saved.m_IsMemoryPNG = true; saved.m_DataPathIsImageFile = true;
			way("frozen memory image info", saved.GetImageBitDepth() == 8 && saved.GetImageWidth() == 7 && saved.GetImageHeight() == 5);
			BITMAP* captured = saved.GetAsBitmap();
			way("frozen image getter", captured && captured->line[4][6] == 37 && captured == saved.GetAsBitmap());
			auto owned = std::unique_ptr<BITMAP, void(*)(BITMAP*)>(saved.GetAsBitmap(0, false), destroy_bitmap);
			way("private image copy", owned && owned.get() != captured && owned->line[4][6] == 37);
			bool refused = false;
			try {
				CheckpointFailure::Scope failure(CheckpointFailure::Point::NativeObjects);
				saved.GetAsBitmap(0, false);
			} catch (const std::bad_alloc&) { refused = true; }
			way("private image refusal", refused);
		}
		way("image cache stays unchanged", std::as_const(registry).find(path) == std::as_const(registry).end() && image->line[4][6] == 41);
		if (previous) registry[path] = previous; else registry.erase(path);
	}
	return missed;
}

const std::string* ContentFile::LoadedBitmapPath(const BITMAP* bitmap, int& depth) {
	if (const auto* snapshot = CheckpointNativeSnapshot::Current()) if (auto path = snapshot->BitmapPath(bitmap, depth)) return *path;
	if (CheckpointNativeStorage::Reading()) {
		const auto index = SavedIndex();
		const auto found = index->paths.find(static_cast<const BITMAP*>(CheckpointNativeStorage::Original(bitmap)));
		if (found == index->paths.end()) return nullptr;
		if (depth >= 0) {
			if (static_cast<size_t>(depth) >= found->second.size()) throw std::logic_error("saved bitmap depth is out of range");
			const auto& path = found->second[depth];
			return path ? &*path : nullptr;
		}
		for (size_t at = 0; at < found->second.size(); ++at) if (const auto& path = found->second[at]) { depth = static_cast<int>(at); return &*path; }
		return nullptr;
	}
	const auto& registry = std::as_const(s_LoadedBitmaps);
	const LoadedBitmapIndex& index = Index();
	const auto least = [&](size_t at) -> const std::string* {
		if (index.scopes.load() > 0) {
			const auto found = index.paths.find(bitmap);
			return found == index.paths.end() ? nullptr : found->second[at];
		}
		const std::string* result = nullptr;
		for (const auto& [path, image]: registry[at]) if (image == bitmap && (!result || path < *result)) result = &path.Value();
		return result;
	};
	if (depth >= 0) return least(static_cast<size_t>(depth));
	for (size_t at = 0; at < registry.size(); ++at) {
		if (const std::string* path = least(at)) {
			depth = static_cast<int>(at);
			return path;
		}
	}
	return nullptr;
}
std::unordered_map<std::string, SDL_Surface*> ContentFile::s_MemoryPNGs;
std::unordered_map<std::string, FMOD::Sound*> ContentFile::s_LoadedSamples;
CheckpointUnorderedMap<size_t, CheckpointString> ContentFile::s_PathHashes = [] {
	CheckpointNativeStorage::AllocationScope resources(true);
	return CheckpointUnorderedMap<size_t, CheckpointString>{};
}();

void ContentFile::Clear() {
	m_DataPath.clear();
	m_DataPathExtension.clear();
	m_DataPathWithoutExtension.clear();
	m_DataPathIsImageFile = false;
	m_FormattedReaderPosition.clear();
	m_DataPathAndReaderPosition.clear();
	m_DataModuleID = 0;
	m_IsMemoryPNG = false;

	m_ImageFileInfo.fill(-1);
}

int ContentFile::Create(const char* filePath) {
	SetDataPath(filePath);

	return 0;
}

int ContentFile::Create(const ContentFile& reference) {
	*this = reference;

	return 0;
}

void ContentFile::FreeAllLoaded() {
	for (int depth = BitDepths::Eight; depth < BitDepths::BitDepthCount; ++depth) {
		for (const auto& [bitmapPath, bitmapPtr]: s_LoadedBitmaps[depth]) {
			destroy_bitmap(bitmapPtr);
		}
		s_LoadedBitmaps[depth].clear();
	}
	for (const auto& [path, image]: s_MemoryPNGs) SDL_DestroySurface(image);
	s_MemoryPNGs.clear();
}

void ContentFile::FreeAllLoadedSounds() {
	for (const auto& [soundPath, sound]: s_LoadedSamples) {
		if (sound) {
			{ auto& callbacks = SampleCallbacks(); std::lock_guard lock(callbacks.mutex); callbacks.values.erase(sound); }
			const FMOD_RESULT result = sound->release();
			if (result != FMOD_OK) {
				g_ConsoleMan.PrintString("ERROR: Could not release sound " + soundPath + ": " + FMOD_ErrorString(result));
			}
		}
	}
	s_LoadedSamples.clear();
	s_SampleValues.clear(); s_SampleValuesReady.store(true, std::memory_order_release);
}

int ContentFile::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return Serializable::ReadProperty(propName, reader));

	MatchForwards("FilePath")
	MatchProperty("Path", { SetDataPath(reader.ReadPropValue()); });
	MatchProperty("IsMemoryPNG", { reader >> m_IsMemoryPNG; });
	MatchProperty("SpecialBehaviour_ContentCheckpoint", {
		if (!LoadCheckpoint(base64_decode(reader.ReadPropValue()))) reader.ReportError("invalid content file checkpoint");
	});

	EndPropertyList;
}

int ContentFile::Save(Writer& writer) const {
	if (const std::string* path = writer.ContentOverride(this)) {
		ContentFile saved = *this;
		saved.SetIsMemoryFile(true);
		saved.SetDataPath(*path);
		return saved.Save(writer);
	}
	Serializable::Save(writer);

	if (!m_DataPath.empty()) {
		writer.NewPropertyWithValue("FilePath", m_DataPath);
	}
	writer.NewPropertyWithValue("IsMemoryPNG", m_IsMemoryPNG);
	if (writer.IsSnapshot()) writer.NewPropertyWithValue("SpecialBehaviour_ContentCheckpoint", CheckpointWriter::Native([&] { return SaveCheckpoint(); }).Base64(true));

	return 0;
}

int ContentFile::GetDataModuleID() const {
	return (m_DataModuleID < 0) ? g_PresetMan.GetModuleIDFromPath(m_DataPath) : m_DataModuleID;
}

void ContentFile::SetDataPath(const std::string& newDataPath) {
	m_DataPath = g_PresetMan.GetFullModulePath(newDataPath);
	m_DataPathExtension = std::filesystem::path(m_DataPath.Value()).extension().string();

	RTEAssert(!m_DataPathExtension.empty(), "Failed to find file extension when trying to find file with path and name:\n" + m_DataPath + "\n" + GetFormattedReaderPosition());

	m_DataPathIsImageFile = m_DataPathExtension == ".png" || m_DataPathExtension == ".bmp";

	m_DataPathWithoutExtension = m_DataPath.substr(0, m_DataPath.length() - m_DataPathExtension.length());
	RegisterPath();
	m_DataModuleID = g_PresetMan.GetModuleIDFromPath(m_DataPath);
}

size_t ContentFile::GetHash() const {
	return Hash(m_DataPath);
}

void ContentFile::SetFormattedReaderPosition(const std::string& newPosition) {
	m_FormattedReaderPosition = newPosition;
	m_DataPathAndReaderPosition = m_DataPath + "\n" + newPosition;
}

int ContentFile::GetImageFileInfo(ImageFileInfoType infoTypeToGet) {
	bool fetchFileInfo = false;
	for (const int& fileInfoEntry: m_ImageFileInfo) {
		if (fileInfoEntry == -1) {
			fetchFileInfo = true;
			break;
		}
	}
	if (fetchFileInfo) {
		if (m_IsMemoryPNG) {
			if (CheckpointNativeStorage::Reading()) {
				const BITMAP* image = ReadLoadedBitmap(m_DataPath.Value(), BitDepths::Eight);
				if (!image) throw UnsupportedCheckpointNative("saved memory image has no frozen bitmap");
				image = CheckpointNativeStorage::Source(image);
				m_ImageFileInfo = {8, image->w, image->h};
				return m_ImageFileInfo[infoTypeToGet];
			}
			if (const SDL_Surface* png = s_MemoryPNGs[m_DataPath]) {
				m_ImageFileInfo[ImageFileInfoType::ImageBitDepth] = static_cast<int>(SDL_BITSPERPIXEL(png->format));
				m_ImageFileInfo[ImageFileInfoType::ImageWidth] = static_cast<int>(png->w);
				m_ImageFileInfo[ImageFileInfoType::ImageHeight] = static_cast<int>(png->h);
				return m_ImageFileInfo[infoTypeToGet];
			}
		}

		FILE* imageFile = fopen(m_DataPath.c_str(), "rb");
		RTEAssert(imageFile, "Failed to open file prior to reading info of image file with following path and name:\n\n" + m_DataPath + "\n\nThe file may not exist or be corrupt.");

		if (m_DataPathExtension == ".png") {
			ReadAndStorePNGFileInfo(imageFile);
		} else if (m_DataPathExtension == ".bmp") {
			ReadAndStoreBMPFileInfo(imageFile);
		} else {
			RTEAbort("Somehow ended up attempting to read image file info for an unsupported image file type.\nThe file path and name were:\n\n" + m_DataPath);
		}
		fclose(imageFile);
	}
	return m_ImageFileInfo[infoTypeToGet];
}

void ContentFile::ReadAndStorePNGFileInfo(FILE* imageFile) {
	std::array<uint8_t, 8> fileSignature = {};

	// Read the first 8 bytes to then verify they match the PNG file signature which is { 137, 80, 78, 71, 13, 10, 26, 10 } or { '\211', 'P', 'N', 'G', '\r', '\n', '\032', '\n' }.
	fread(fileSignature.data(), sizeof(uint8_t), fileSignature.size(), imageFile);
	if (png_sig_cmp(fileSignature.data(), 0, fileSignature.size()) == 0) {
		png_structp pngReadStruct = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
		png_infop pngInfo = png_create_info_struct(pngReadStruct);

		png_init_io(pngReadStruct, imageFile);
		// Set the PNG reader to skip the first 8 bytes since we already handled them.
		png_set_sig_bytes(pngReadStruct, fileSignature.size());
		png_read_info(pngReadStruct, pngInfo);

		m_ImageFileInfo[ImageFileInfoType::ImageBitDepth] = static_cast<int>(png_get_bit_depth(pngReadStruct, pngInfo));
		m_ImageFileInfo[ImageFileInfoType::ImageWidth] = static_cast<int>(png_get_image_width(pngReadStruct, pngInfo));
		m_ImageFileInfo[ImageFileInfoType::ImageHeight] = static_cast<int>(png_get_image_height(pngReadStruct, pngInfo));

		png_destroy_read_struct(&pngReadStruct, &pngInfo, nullptr);
	} else {
		RTEAbort("Encountered invalid PNG file signature while attempting to read info of image file with following path and name:\n\n" + m_DataPath + "\n\nThe file may be corrupt or is not a PNG file.");
	}
}

void ContentFile::ReadAndStoreBMPFileInfo(FILE* imageFile) {
	std::array<uint8_t, 2> bmpSignature = {0x42, 0x4D}; // { 'B', 'M' }.
	std::array<uint8_t, 2> fileSignature = {};

	// Read the first 2 bytes to then verify they match the BMP file signature.
	fread(fileSignature.data(), sizeof(uint8_t), fileSignature.size(), imageFile);
	if (fileSignature == bmpSignature) {
		std::array<uint8_t, 4> bmpData = {};

		const auto toInt32 = [](uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
			return (static_cast<int32_t>(b0)) | (static_cast<int32_t>(b1) << 8) | (static_cast<int32_t>(b2) << 16) | (static_cast<int32_t>(b3) << 24);
		};

		// Skip the next 16 bytes. Dimensions data starts at the 18th byte.
		fseek(imageFile, 16, SEEK_CUR);

		fread(bmpData.data(), sizeof(uint8_t), bmpData.size(), imageFile);
		m_ImageFileInfo[ImageFileInfoType::ImageWidth] = toInt32(bmpData[0], bmpData[1], bmpData[2], bmpData[3]);

		fread(bmpData.data(), sizeof(uint8_t), bmpData.size(), imageFile);
		m_ImageFileInfo[ImageFileInfoType::ImageHeight] = toInt32(bmpData[0], bmpData[1], bmpData[2], bmpData[3]);

		// Skip the next 2 bytes. Bit depth data starts at the 28th byte.
		fseek(imageFile, 2, SEEK_CUR);

		// Bit depth is stored as 2 bytes, so ignore the last 2 in the array when converting to int32.
		fread(bmpData.data(), sizeof(uint8_t), bmpData.size(), imageFile);
		m_ImageFileInfo[ImageFileInfoType::ImageBitDepth] = toInt32(bmpData[0], bmpData[1], 0, 0);
	} else {
		RTEAbort("Encountered invalid BMP file signature while attempting to read info of image file with following path and name:\n\n" + m_DataPath + "\n\nThe file may be corrupt or is not a BMP file.");
	}
}

bool ContentFile::MemoryPNGScope::Add(const std::string& filePath, SDL_Surface* image) {
	if (!image || image->format != SDL_PIXELFORMAT_INDEX8 || image->w <= 0 || image->h <= 0) return false;
	std::unique_ptr<BITMAP, decltype(&destroy_bitmap)> bitmap(create_bitmap_ex(8, image->w, image->h), destroy_bitmap);
	if (!bitmap) return false;
	for (int y = 0; y < image->h; ++y) {
		std::memcpy(bitmap->line[y], static_cast<const unsigned char*>(image->pixels) + image->pitch * y, image->w);
	}
	auto& cachedImage = s_MemoryPNGs[filePath];
	auto& cachedBitmap = s_LoadedBitmaps[BitDepths::Eight][filePath];
	m_Entries.push_back({filePath, cachedImage, cachedBitmap});
	cachedImage = image;
	cachedBitmap = bitmap.release();
	return true;
}

void ContentFile::MemoryPNGScope::SetActive(bool active) {
	if (active == m_Active) return;
	for (auto entry = m_Entries.rbegin(); entry != m_Entries.rend(); ++entry) {
		std::swap(s_MemoryPNGs[entry->path], entry->previousImage);
		std::swap(s_LoadedBitmaps[BitDepths::Eight][entry->path], entry->previousBitmap);
		if (!s_MemoryPNGs[entry->path]) s_MemoryPNGs.erase(entry->path);
		if (!s_LoadedBitmaps[BitDepths::Eight][entry->path]) s_LoadedBitmaps[BitDepths::Eight].erase(entry->path);
	}
	m_Active = active;
}

ContentFile::MemoryPNGScope::~MemoryPNGScope() {
	if (m_Active && !m_Committed) SetActive(false);
	for (auto entry = m_Entries.rbegin(); entry != m_Entries.rend(); ++entry) {
		SDL_DestroySurface(entry->previousImage);
		if (entry->previousBitmap) destroy_bitmap(entry->previousBitmap);
	}
}

void ContentFile::ReloadAllBitmaps() {
	for (const auto& bitmapCache: s_LoadedBitmaps) {
		for (const auto& [filePath, oldBitmap]: bitmapCache) {
			ReloadBitmap(filePath);
		}
	}
	g_ConsoleMan.PrintString("SYSTEM: Sprites reloaded");
}

BITMAP* ContentFile::GetAsBitmap(int conversionMode, bool storeBitmap, const std::string& dataPathToSpecificFrame) {
	CheckpointNativeStorage::AllocationScope resources(true);
	if (m_DataPath.empty()) {
		return nullptr;
	}
	BITMAP* returnBitmap = nullptr;
	const int bitDepth = conversionMode == COLORCONV_8_TO_32 ? BitDepths::ThirtyTwo : BitDepths::Eight;
	std::string dataPathToLoad = dataPathToSpecificFrame.empty() ? m_DataPath.Value() : dataPathToSpecificFrame;
	if (CheckpointNativeStorage::Reading()) {
		auto index = std::const_pointer_cast<SavedBitmapIndex>(SavedIndex());
		BITMAP* bitmap = ReadLoadedBitmap(dataPathToLoad, bitDepth);
		if (!bitmap) {
			if (!System::PathExistsCaseSensitive(dataPathToLoad)) {
				const std::string extension = m_DataPathExtension == ".png" ? ".bmp" : ".png";
				const std::string alternate = dataPathToLoad.substr(0, dataPathToLoad.length() - m_DataPathExtension.length()) + extension;
				if (System::PathExistsCaseSensitive(alternate)) {
					SetDataPath(m_DataPathWithoutExtension + extension);
					dataPathToLoad = alternate;
					bitmap = ReadLoadedBitmap(dataPathToLoad, bitDepth);
				}
			}
			if (!bitmap) {
				auto loaded = std::unique_ptr<BITMAP, void(*)(BITMAP*)>(LoadSavedBitmap(conversionMode, dataPathToLoad), destroy_bitmap);
				if (!loaded) return nullptr;
				if (!storeBitmap) return loaded.release();
				bitmap = loaded.get();
				index->owned.push_back(std::move(loaded));
				index->images[dataPathToLoad][bitDepth] = bitmap;
				index->paths[bitmap][bitDepth] = dataPathToLoad;
			}
		}
		if (const auto* snapshot = CheckpointNativeSnapshot::Current()) bitmap = const_cast<CheckpointNativeSnapshot*>(snapshot)->ReadBitmap(bitmap);
		else if (!CheckpointNativeStorage::Private(bitmap, sizeof(BITMAP))) throw std::logic_error("a saved image needs its snapshot pixel owner");
		if (storeBitmap) return bitmap;
		const auto* saved = CheckpointNativeStorage::Source(bitmap);
		BITMAP* copy = create_bitmap_ex(bitmap_color_depth(const_cast<BITMAP*>(saved)), saved->w, saved->h);
		if (!copy) throw std::bad_alloc();
		const size_t stride = static_cast<size_t>(saved->w) * ((bitmap_color_depth(const_cast<BITMAP*>(saved)) + 7) / 8);
		for (int row = 0; row < saved->h; ++row) std::memcpy(copy->line[row], saved->line[row], stride);
		return copy;
	}

	if (g_PresetMan.GetReloadEntityPresetCalledThisUpdate()) {
		ReloadBitmap(dataPathToLoad, conversionMode);
	}

	// Check if the file has already been read and loaded from the disk and, if so, use that data.
	const auto foundBitmap = std::as_const(s_LoadedBitmaps[bitDepth]).find(dataPathToLoad);
	if (foundBitmap != std::as_const(s_LoadedBitmaps[bitDepth]).end()) {
		if (storeBitmap) {
			returnBitmap = (*foundBitmap).second;
		} else if (SDL_Surface* surface = s_MemoryPNGs[dataPathToLoad]) {
			auto foundBitmap = s_LoadedBitmaps[BitDepths::Eight].find(dataPathToLoad);
			if (foundBitmap != s_LoadedBitmaps[BitDepths::Eight].end()) {
				returnBitmap = foundBitmap->second;
				s_LoadedBitmaps[BitDepths::Eight].erase(dataPathToLoad);
			}

			SDL_DestroySurface(surface);
			s_MemoryPNGs.erase(dataPathToLoad);
		}
	}

	if (returnBitmap == nullptr) {
		if (!System::PathExistsCaseSensitive(dataPathToLoad)) {
			const std::string dataPathWithoutExtension = dataPathToLoad.substr(0, dataPathToLoad.length() - m_DataPathExtension.length());
			const std::string altFileExtension = (m_DataPathExtension == ".png") ? ".bmp" : ".png";

			if (System::PathExistsCaseSensitive(dataPathWithoutExtension + altFileExtension)) {
				g_ConsoleMan.AddLoadWarningLogExtensionMismatchEntry(m_DataPath, m_FormattedReaderPosition, altFileExtension);
				SetDataPath(m_DataPathWithoutExtension + altFileExtension);
				dataPathToLoad = dataPathWithoutExtension + altFileExtension;
			} else {
				RTEAbort("Failed to find image file with following path and name:\n\n" + dataPathToLoad + " or " + altFileExtension + "\n" + m_FormattedReaderPosition);
			}
		}
		returnBitmap = LoadAndReleaseBitmap(conversionMode, dataPathToLoad); // NOTE: This takes ownership of the bitmap file

		// Insert the bitmap into the map, PASSING OVER OWNERSHIP OF THE LOADED DATAFILE
		if (storeBitmap) {
			s_LoadedBitmaps[bitDepth].try_emplace(dataPathToLoad, returnBitmap);
		}
	}

	return returnBitmap;
}

template<class Allocator>
void ContentFile::GetAsAnimation(std::vector<BITMAP*, Allocator>& vectorToFill, int frameCount, int conversionMode) {
	if (m_DataPath.empty() || frameCount < 1) {
		return;
	}
	vectorToFill.reserve(frameCount);

	if (frameCount == 1) {
		// Check for 000 in the file name in case it is part of an animation but the FrameCount was set to 1. Do not warn about this because it's normal operation, but warn about incorrect extension.
		if (!System::PathExistsCaseSensitive(m_DataPath)) {
			const std::string altFileExtension = (m_DataPathExtension == ".png") ? ".bmp" : ".png";

			if (System::PathExistsCaseSensitive(m_DataPathWithoutExtension + "000" + m_DataPathExtension)) {
				SetDataPath(m_DataPathWithoutExtension + "000" + m_DataPathExtension);
			} else if (System::PathExistsCaseSensitive(m_DataPathWithoutExtension + "000" + altFileExtension)) {
				g_ConsoleMan.AddLoadWarningLogExtensionMismatchEntry(m_DataPath, m_FormattedReaderPosition, altFileExtension);
				SetDataPath(m_DataPathWithoutExtension + "000" + altFileExtension);
			}
		}
		vectorToFill.emplace_back(GetAsBitmap(conversionMode));
	} else {
		char framePath[1024];
		for (int frameNum = 0; frameNum < frameCount; ++frameNum) {
			std::snprintf(framePath, sizeof(framePath), "%s%03i%s", m_DataPathWithoutExtension.c_str(), frameNum, m_DataPathExtension.c_str());
			vectorToFill.emplace_back(GetAsBitmap(conversionMode, true, framePath));
		}
	}
}
template void ContentFile::GetAsAnimation(std::vector<BITMAP*>&, int, int);
template void ContentFile::GetAsAnimation(CheckpointVector<BITMAP*>&, int, int);

SDL_Palette* ContentFile::DefaultPaletteToSDL(bool preMask) {
	SDL_Palette* palette = SDL_CreatePalette(256);
	std::array<SDL_Color, 256> paletteColor;
	const PALETTE& defaultPalette = g_FrameMan.GetDefaultPalette();
	if (preMask) {
		paletteColor[0] = {.r = 0, .g = 0, .b = 0, .a = 0};
	} else {
		paletteColor[0] = {.r = defaultPalette[0].r,
			               .g = defaultPalette[0].g,
			               .b = defaultPalette[0].b,
						   .a = 255
			               };
	}
	for (size_t i = 1; i < paletteColor.size(); ++i) {
		paletteColor[i].r = defaultPalette[i].r;
		paletteColor[i].g = defaultPalette[i].g;
		paletteColor[i].b = defaultPalette[i].b;
		paletteColor[i].a = 255;
	}
	SDL_SetPaletteColors(palette, paletteColor.data(), 0, 256);
	return palette;
}

bool ContentFile::EncodeIndexedPNG(BITMAP* bitmap, std::vector<unsigned char>& output) {
	std::array<unsigned char, 256 * 3> palette{};
	const PALETTE& colors = g_FrameMan.GetDefaultPalette();
	for (size_t i = 0; i < 256; ++i) {
		palette[i * 3] = colors[i].r;
		palette[i * 3 + 1] = colors[i].g;
		palette[i * 3 + 2] = colors[i].b;
	}
	return EncodeIndexedPNG(bitmap, output, palette);
}

bool ContentFile::EncodeIndexedPNG(BITMAP* bitmap, std::vector<unsigned char>& output, const std::array<unsigned char, 256 * 3>& palette) {
	output.clear();
	if (!bitmap || bitmap_color_depth(bitmap) != 8 || bitmap->w <= 0 || bitmap->h <= 0) return false;
	const auto pitch = bitmap->h > 1 ? bitmap->line[1] - bitmap->line[0] : bitmap->w;
	if (pitch < bitmap->w || pitch > std::numeric_limits<png_int_32>::max() ||
	    (static_cast<uint64_t>(bitmap->w) + 1) * bitmap->h > std::numeric_limits<png_uint_32>::max()) return false;

	png_image image{};
	image.version = PNG_IMAGE_VERSION;
	image.width = bitmap->w;
	image.height = bitmap->h;
	image.format = PNG_FORMAT_RGB_COLORMAP;
	image.colormap_entries = 256;
	image.flags = PNG_IMAGE_FLAG_FAST;
	png_alloc_size_t size = PNG_IMAGE_PNG_SIZE_MAX(image);
	output.resize(size);
	const bool saved = png_image_write_to_memory(&image, output.data(), &size, 0, bitmap->line[0], static_cast<png_int_32>(pitch), palette.data()) != 0;
	png_image_free(&image);
	output.resize(saved ? size : 0);
	return saved;
}

SDL_Surface* ContentFile::LoadImageAsSurface(int conversionMode, const std::string& dataPathToLoad) {
	SDL_Surface* image = IMG_Load(dataPathToLoad.c_str());
	if (!image) {
		RTEAbort("Failed to load image file with following path and name:\n\n" + dataPathToLoad + "\n\n" + SDL_GetError());
		return nullptr;
	}
	bool convert8To32 = conversionMode & COLORCONV_8_TO_32;
	bool convertTo8 = conversionMode & COLORCONV_REDUCE_TO_256;
	int bitDepth = SDL_GetPixelFormatDetails(image->format)->bits_per_pixel;
	if (convertTo8 && bitDepth != 8) {
		SDL_Palette* palette = DefaultPaletteToSDL();
		SDL_Surface* newImage = SDL_ConvertSurfaceAndColorspace(image, SDL_PIXELFORMAT_INDEX8, palette, SDL_COLORSPACE_UNKNOWN, 0);
		SDL_DestroyPalette(palette);
		SDL_DestroySurface(image);
		image = newImage;
		bitDepth = 8;
	} else if (bitDepth != 8 || convert8To32) {
		SDL_Palette* palette = DefaultPaletteToSDL(true);
		if (SDL_GetPixelFormatDetails(image->format)->bits_per_pixel == 8) {
			SDL_SetSurfacePalette(image, palette);
			SDL_SetSurfaceColorKey(image, true, 0);
		}
		SDL_DestroyPalette(palette);
		SDL_Surface* newImage = SDL_ConvertSurface(image, SDL_PIXELFORMAT_RGBA32);
		SDL_DestroySurface(image);
		image = newImage;
		bitDepth = 32;
	}

	return image;
}

BITMAP* ContentFile::LoadAndReleaseBitmap(int conversionMode, const std::string& dataPathToSpecificFrame) {
	if (m_DataPath.empty()) {
		return nullptr;
	}
	const std::string dataPathToLoad = dataPathToSpecificFrame.empty() ? m_DataPath.Value() : dataPathToSpecificFrame;

	SDL_Surface* image = LoadImageAsSurface(conversionMode, dataPathToLoad);
	if (!image) {
		return nullptr;
	}
	int bitDepth = SDL_GetPixelFormatDetails(image->format)->bits_per_pixel;

	BITMAP* returnBitmap = create_bitmap_ex(bitDepth, image->w, image->h);

	// allegro doesn't (always) align lines to 4byte, so copy line by line. SDL_Surface.pitch is the size in bytes per line + alignment padding.
	for (int y = 0; y < image->h; ++y) {
		memcpy(returnBitmap->line[y], static_cast<unsigned char*>(image->pixels) + image->pitch * y, image->w * SDL_GetPixelFormatDetails(image->format)->bytes_per_pixel);
	}
	SDL_DestroySurface(image);
	RTEAssert(returnBitmap, "Failed to load image file with following path and name:\n\n" + m_DataPathAndReaderPosition + "\nThe file may be corrupt, incorrectly converted or saved with unsupported parameters.");

	return returnBitmap;
}

BITMAP* ContentFile::LoadSavedBitmap(int conversionMode, const std::string& path) {
	std::unique_ptr<SDL_Surface, void(*)(SDL_Surface*)> image(LoadImageAsSurface(conversionMode, path), SDL_DestroySurface);
	if (!image) return nullptr;
	const int depth = SDL_GetPixelFormatDetails(image->format)->bits_per_pixel;
	BITMAP* bitmap = create_bitmap_ex(depth, image->w, image->h);
	if (!bitmap) throw std::bad_alloc();
	const size_t stride = static_cast<size_t>(image->w) * SDL_GetPixelFormatDetails(image->format)->bytes_per_pixel;
	for (int row = 0; row < image->h; ++row) std::memcpy(bitmap->line[row], static_cast<unsigned char*>(image->pixels) + image->pitch * row, stride);
	return bitmap;
}

bool ContentFile::RunImageLoadSelfTest() {
	bool passed = true;
	const auto check = [&passed](const char* name, bool ok, const std::string& detail = std::string()) {
		std::cout << "[content-file-selftest] " << (ok ? "PASS " : "FAIL ") << name << (detail.empty() ? "" : " " + detail) << std::endl;
		passed = passed && ok;
	};

	// A module name of its own re-roots into the mod directory, so the files are written where ContentFile will look and nowhere else.
	ContentFile readable("SelfTest.rte/ContentFileSelfTestImage.png");
	ContentFile unreadable("SelfTest.rte/ContentFileSelfTestNotAnImage.png");
	const std::string readablePath = readable.GetDataPath();
	const std::string unreadablePath = unreadable.GetDataPath();
	std::error_code error;
	std::filesystem::create_directories(std::filesystem::path(readablePath).parent_path(), error);

	BITMAP* source = create_bitmap_ex(8, 4, 3);
	clear_to_color(source, 0);
	std::vector<unsigned char> encoded;
	const bool encodedImage = EncodeIndexedPNG(source, encoded);
	destroy_bitmap(source);
	std::ofstream(readablePath, std::ios::binary).write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
	std::ofstream(unreadablePath, std::ios::binary) << "this is not an image";

	check("selftest_files_are_private", readablePath.starts_with(System::GetModDirectory()), readablePath);
	BITMAP* loaded = readable.GetAsBitmap(0, false);
	check("readable_image_loads", encodedImage && loaded && loaded->w == 4 && loaded->h == 3);
	if (loaded) {
		destroy_bitmap(loaded);
	}

	// RTEAbort skips while another abort is in flight, so suppressing it here observes the guard instead of ending the process.
	SDL_ClearError();
	const bool wasAborting = RTEError::s_CurrentlyAborting;
	RTEError::s_CurrentlyAborting = true;
	BITMAP* refused = unreadable.GetAsBitmap(0, false);
	RTEError::s_CurrentlyAborting = wasAborting;
	const std::string loadError = SDL_GetError();
	// The file is there, so the abort that fired is the load guard and not the missing-file one above it.
	check("non_image_file_is_present", System::PathExistsCaseSensitive(unreadablePath));
	check("non_image_returns_null", refused == nullptr);
	check("non_image_reports_sdl_error", !loadError.empty(), loadError);
	if (refused) {
		destroy_bitmap(refused);
	}

	std::filesystem::remove(readablePath, error);
	std::filesystem::remove(unreadablePath, error);
	std::filesystem::remove(std::filesystem::path(readablePath).parent_path(), error);
	return passed;
}

bool ContentFile::WaitForPendingSounds(const ProgressCallback& progressCallback) {
	std::vector<std::pair<std::string, FMOD::Sound*>> pending(s_LoadedSamples.begin(), s_LoadedSamples.end());
	const auto start = RTE::SteadyClock::now();
	auto nextProgress = start;
	size_t initiallyPending = 0;
	bool firstPass = true;
	bool success = true;
	while (!pending.empty()) {
		std::erase_if(pending, [&success](const auto& entry) {
			const auto& [path, sound] = entry;
			if (!sound) return true; // The synchronous loader already reported this failure.
			FMOD_OPENSTATE state = FMOD_OPENSTATE_ERROR;
			const FMOD_RESULT result = sound->getOpenState(&state, nullptr, nullptr, nullptr);
			if (result != FMOD_OK || state == FMOD_OPENSTATE_ERROR) {
				g_ConsoleMan.PrintString("ERROR: Could not finish loading sound " + path + ": " + FMOD_ErrorString(result));
				success = false;
				return true;
			}
			const bool ready = state == FMOD_OPENSTATE_READY || state == FMOD_OPENSTATE_PLAYING;
			if (ready) NoteLoadedSample(path, sound);
			return ready;
		});
		if (firstPass) {
			initiallyPending = pending.size();
			firstPass = false;
		}
		if (pending.empty()) break;
		const auto now = RTE::SteadyClock::now();
		if (progressCallback && now >= nextProgress) {
			progressCallback("Finishing sound loading: " + std::to_string(pending.size()) + " remaining", nextProgress == start);
			nextProgress = now + std::chrono::milliseconds(100);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (initiallyPending != 0) {
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(RTE::SteadyClock::now() - start).count();
		g_ConsoleMan.PrintString("Sound loading completed: " + std::to_string(initiallyPending) + " pending samples, " + std::to_string(elapsed) + "ms");
	}
	return success;
}

FMOD::Sound* ContentFile::GetAsSound(bool abortGameForInvalidSound, bool asyncLoading) {
	if (m_DataPath.empty() || !g_AudioMan.IsAudioEnabled()) {
		return nullptr;
	}
	if (CheckpointNativeStorage::Reading()) return LoadSavedSound(abortGameForInvalidSound);
	FMOD::Sound* returnSample = nullptr;

	std::unordered_map<std::string, FMOD::Sound*>::iterator foundSound = s_LoadedSamples.find(m_DataPath);
	if (foundSound != s_LoadedSamples.end()) {
		returnSample = (*foundSound).second;
	} else {
		returnSample = LoadAndReleaseSound(abortGameForInvalidSound, asyncLoading); // NOTE: This takes ownership of the sample file

		// Insert the Sound object into the map, PASSING OVER OWNERSHIP OF THE LOADED FILE
		s_LoadedSamples.try_emplace(m_DataPath, returnSample);
	}
	NoteLoadedSample(m_DataPath.Value(), returnSample);
	return returnSample;
}

FMOD::Sound* ContentFile::LoadSavedSound(bool abortGameForInvalidSound) {
	static const char kind{};
	const auto [source, bytes] = LoadedSampleRegistry();
	const auto saved = std::const_pointer_cast<SavedSamples>(std::static_pointer_cast<const SavedSamples>(CheckpointNativeStorage::ReadIndex(source, &kind, [] {
		auto samples = std::make_shared<SavedSamples>();
		for (auto& value: ReadCheckpointSamples()) samples->values.emplace(value.path, std::move(value));
		return samples;
	})));
	std::string path = m_DataPath.Value();
	if (const auto found = saved->sounds.find(path); found != saved->sounds.end()) return found->second;
	if (!System::PathExistsCaseSensitive(path)) {
		for (const char* extension: c_SupportedAudioFormats) {
			const std::string alternate = m_DataPathWithoutExtension + extension;
			if (System::PathExistsCaseSensitive(alternate)) { SetDataPath(alternate); path = m_DataPath.Value(); break; }
		}
	}
	if (!System::PathExistsCaseSensitive(path) || std::filesystem::file_size(path) == 0) {
		if (abortGameForInvalidSound) throw UnsupportedCheckpointNative("saved sound file is absent or empty: " + path);
		System::PrintDiagnosticLine("[autosave] sound file is absent or empty: " + path);
		return nullptr;
	}
	if (const auto found = saved->sounds.find(path); found != saved->sounds.end()) return found->second;
	const auto require = [](FMOD_RESULT result) { if (result == FMOD_ERR_MEMORY) throw std::bad_alloc(); AudioCheckpoint::Require(result); };
	if (!saved->system) {
		CheckpointFailure::Check(CheckpointFailure::Point::NativeObjects);
		FMOD::System* candidate = nullptr;
		try {
			require(FMOD::System_Create(&candidate));
			require(candidate->setOutput(FMOD_OUTPUTTYPE_NOSOUND_NRT));
			require(candidate->init(1, FMOD_INIT_NORMAL, nullptr));
		} catch (...) { if (candidate) candidate->release(); throw; }
		saved->system = candidate;
	}
	FMOD::Sound* sound = nullptr;
	const FMOD_RESULT loaded = saved->system->createSound(path.c_str(), FMOD_CREATESAMPLE | FMOD_3D, nullptr, &sound);
	if (loaded != FMOD_OK) {
		if (sound) sound->release();
		if (loaded == FMOD_ERR_MEMORY || abortGameForInvalidSound) require(loaded);
		System::PrintDiagnosticLine("[autosave] sound load failed: " + path + ": " + FMOD_ErrorString(loaded));
		return nullptr;
	}
	try {
		if (const auto found = saved->values.find(path); found != saved->values.end()) found->second.Apply(sound);
		saved->sounds.emplace(path, sound);
	} catch (...) { sound->release(); throw; }
	return sound;
}

FMOD::Sound* ContentFile::LoadAndReleaseSound(bool abortGameForInvalidSound, bool asyncLoading) {
	if (m_DataPath.empty() || !g_AudioMan.IsAudioEnabled()) {
		return nullptr;
	}
	if (!System::PathExistsCaseSensitive(m_DataPath)) {
		bool foundAltExtension = false;
		for (const char* altFileExtension: c_SupportedAudioFormats) {
			const std::string altDataPathToLoad = m_DataPathWithoutExtension + altFileExtension;
			if (System::PathExistsCaseSensitive(altDataPathToLoad)) {
				g_ConsoleMan.AddLoadWarningLogExtensionMismatchEntry(m_DataPath, m_FormattedReaderPosition, altFileExtension);
				SetDataPath(altDataPathToLoad);
				foundAltExtension = true;
				break;
			}
		}
		if (!foundAltExtension) {
			std::string errorMessage = "Failed to find audio file with following path and name:\n\n" + m_DataPath + " or any alternative supported file type";
			RTEAssert(!abortGameForInvalidSound, errorMessage + "\n" + m_FormattedReaderPosition);
			g_ConsoleMan.PrintString(errorMessage + ". The file was not loaded!");
			return nullptr;
		}
	}
	if (std::filesystem::file_size(m_DataPath.Value()) == 0) {
		const std::string errorMessage = "Failed to create sound because the file was empty. The path and name were: ";
		RTEAssert(!abortGameForInvalidSound, errorMessage + "\n\n" + m_DataPathAndReaderPosition);
		g_ConsoleMan.PrintString("ERROR: " + errorMessage + m_DataPath);
		return nullptr;
	}
	FMOD::Sound* returnSample = nullptr;

	FMOD_MODE fmodFlags = FMOD_CREATESAMPLE | FMOD_3D | (asyncLoading ? FMOD_NONBLOCKING : FMOD_DEFAULT);
	FMOD_CREATESOUNDEXINFO info{}; info.cbsize = sizeof(info); info.nonblockcallback = SampleLoaded;
	FMOD_RESULT result = g_AudioMan.GetAudioSystem()->createSound(m_DataPath.c_str(), fmodFlags, asyncLoading && CheckpointNativeStorage::WorldEnabled() ? &info : nullptr, &returnSample);

	if (result != FMOD_OK) {
		const std::string errorMessage = "Failed to create sound because of FMOD error:\n" + std::string(FMOD_ErrorString(result)) + "\nThe path and name were: ";
		RTEAssert(!abortGameForInvalidSound, errorMessage + "\n\n" + m_DataPathAndReaderPosition);
		g_ConsoleMan.PrintString("ERROR: " + errorMessage + m_DataPath);
		return returnSample;
	}
	return returnSample;
}

void ContentFile::ReloadBitmap(const std::string& filePath, int conversionMode) {
	const int bitDepth = (conversionMode == COLORCONV_8_TO_32) ? BitDepths::ThirtyTwo : BitDepths::Eight;

	auto bmpItr = s_LoadedBitmaps[bitDepth].find(filePath);
	if (bmpItr == s_LoadedBitmaps[bitDepth].end()) {
		return;
	}

	PALETTE currentPalette;
	get_palette(currentPalette);
	set_color_conversion((conversionMode == COLORCONV_NONE) ? COLORCONV_NONE : conversionMode);

	BITMAP* loadedBitmap = (*bmpItr).second;

	SDL_Surface* newImage = LoadImageAsSurface(conversionMode, filePath);

	BITMAP* newBitmap = create_bitmap_ex(SDL_GetPixelFormatDetails(newImage->format)->bits_per_pixel, newImage->w, newImage->h);

	// allegro doesn't (always) align lines to 4byte, so copy line by line. SDL_Surface.pitch is the size in bytes per line + alignment padding.
	for (int y = 0; y < newImage->h; y++) {
		memcpy(newBitmap->line[y], static_cast<unsigned char*>(newImage->pixels) + y * newImage->pitch, newImage->w * SDL_GetPixelFormatDetails(newImage->format)->bytes_per_pixel);
	}

	// AddAlphaChannel(newBitmap);
	BITMAP swap;

	std::memcpy(&swap, loadedBitmap, sizeof(BITMAP));
	std::memcpy(loadedBitmap, newBitmap, sizeof(BITMAP));
	std::memcpy(newBitmap, &swap, sizeof(BITMAP));

	destroy_bitmap(newBitmap);
}

void ContentFile::AddAlphaChannel(BITMAP* bitmap) {
	if (!bitmap) {
		return;
	}
	if (bitmap_color_depth(bitmap) != 32) {
		return;
	}
	for (int y = 0; y < bitmap->h; ++y) {
		for (int x = 0; x < bitmap->w; ++x) {
			unsigned long color = _getpixel32(bitmap, x, y);
			if (color != MASK_COLOR_32) {
				_putpixel32(bitmap, x, y, color | (0xFF << _rgb_a_shift_32));
			}
		}
	}
}
