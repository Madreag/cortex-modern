#include "CheckpointImage.h"
#include "CheckpointArchive.h"
#include "Writer.h"

#include "lua.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <vector>

using namespace RTE;

namespace {
	std::atomic<uint64_t> s_LuaWrites{0};

	void OnLuaTableWrite(void*) {
		s_LuaWrites.fetch_add(1, std::memory_order_relaxed);
	}

	int64_t Percentile99(std::vector<int64_t> samples) {
		if (samples.empty()) return 0;
		std::sort(samples.begin(), samples.end());
		const size_t index = samples.size() - 1 - samples.size() / 100;
		return samples[std::min(index, samples.size() - 1)];
	}
}

CheckpointCow& CheckpointCow::Get() {
	static CheckpointCow store;
	return store;
}

void CheckpointCow::BeginImage() {
	static const bool armed = [] { ArmLuaCheckpointBarrier(); return true; }();
	(void)armed;
	m_Cache.Begin();
}

void CheckpointCow::RememberLua(std::vector<CheckpointText> graphs, uint64_t writeGeneration) {
	std::lock_guard lock(m_Mutex);
	m_LuaGraphs = std::move(graphs);
	m_LuaWriteGeneration = writeGeneration;
}

bool CheckpointCow::LuaUnchanged(uint64_t writeGeneration) const {
	std::lock_guard lock(m_Mutex);
	return writeGeneration == m_LuaWriteGeneration && !m_LuaGraphs.empty();
}

void CheckpointCow::FinishImage(std::shared_ptr<CheckpointImage> image) {
	if (!image) return;
	image->generation = m_Cache.Generation();
	image->objectsReused = m_Cache.Reused();
	image->objectsCaptured = m_Cache.Touched() > m_Cache.Reused() ? m_Cache.Touched() - m_Cache.Reused() : 0;
	std::lock_guard lock(m_Mutex);
	m_LastFreezeUs = image->freezeUs;
	m_LastImageBytes = image->imageBytes;
	m_LastDirtyRatio = image->dirtyRatio;
	m_FreezeSamples.push_back(image->freezeUs);
	m_Last = std::move(image);
}

void CheckpointCow::RecordWorker(int64_t workerUs) {
	std::lock_guard lock(m_Mutex);
	m_LastWorkerUs = workerUs;
}

void CheckpointCow::PublishLog(uint64_t tick) const {
	int64_t freezeUs = 0;
	int64_t workerUs = 0;
	size_t imageBytes = 0;
	double dirtyRatio = 0;
	int64_t p99 = 0;
	std::vector<int64_t> samples;
	{
		std::lock_guard lock(m_Mutex);
		freezeUs = m_LastFreezeUs;
		workerUs = m_LastWorkerUs;
		imageBytes = m_LastImageBytes;
		dirtyRatio = m_LastDirtyRatio;
		samples = m_FreezeSamples;
	}
	p99 = Percentile99(std::move(samples));
	std::cout << std::format("[autosave] tick={} capture_ms={:.3f} bytes={}\n",
	                         tick, freezeUs / 1000.0, imageBytes);
	std::cout << std::format("[autosave] tick={} freeze_us={} worker_us={} image_bytes={} dirty_ratio={:.6f} p99_freeze_us={}\n",
	                         tick, freezeUs, workerUs, imageBytes, dirtyRatio, p99) << std::flush;
}

void CheckpointCow::WriteMetricsJson(const std::string& path) const {
	if (path.empty()) return;
	int64_t freezeUs = 0;
	int64_t workerUs = 0;
	size_t imageBytes = 0;
	double dirtyRatio = 0;
	int64_t p99 = 0;
	size_t sampleCount = 0;
	std::vector<int64_t> samples;
	{
		std::lock_guard lock(m_Mutex);
		freezeUs = m_LastFreezeUs;
		workerUs = m_LastWorkerUs;
		imageBytes = m_LastImageBytes;
		dirtyRatio = m_LastDirtyRatio;
		samples = m_FreezeSamples;
		sampleCount = m_FreezeSamples.size();
	}
	p99 = Percentile99(std::move(samples));
	std::ofstream out(path, std::ios::trunc);
	if (!out) return;
	out << "{\n"
	    << "  \"freeze_us\": " << freezeUs << ",\n"
	    << "  \"worker_us\": " << workerUs << ",\n"
	    << "  \"image_bytes\": " << imageBytes << ",\n"
	    << "  \"dirty_ratio\": " << dirtyRatio << ",\n"
	    << "  \"p99_freeze_us\": " << p99 << ",\n"
	    << "  \"samples\": " << sampleCount << "\n"
	    << "}\n";
}

int64_t CheckpointCow::P99FreezeUs() const {
	std::vector<int64_t> samples;
	{
		std::lock_guard lock(m_Mutex);
		samples = m_FreezeSamples;
	}
	return Percentile99(std::move(samples));
}

void RTE::ArmLuaCheckpointBarrier() {
	luaJIT_set_tab_write_callback(&OnLuaTableWrite);
}

uint64_t RTE::LuaCheckpointWriteGeneration() {
	return s_LuaWrites.load(std::memory_order_relaxed);
}

CheckpointText RTE::AssembleCheckpointSave(const CheckpointImage& image) {
	return Writer::Capture([&](Writer& writer) {
		writer.Append(image.activity);
		writer.NewPropertyWithValue("RuntimeGlobals", image.globals.Base64());
		writer.NewPropertyWithValue("WorldStructure", image.structure.Base64());
		writer.NewPropertyWithValue("SceneRuntime", image.sceneRuntime.Base64());
		writer.NewPropertyWithValue("OriginalScenePresetName", image.originalScenePresetName);
		writer.NewPropertyWithValue("SimUpdateCount", image.simUpdateCount);
		writer.NewPropertyWithValue("SimTimeTicks", image.simTimeTicks);
		writer.NewPropertyWithValue("UniqueIDCounter", image.uniqueIDCounter);
		writer.NewPropertyWithValue("LuaStateCursor", image.luaStateCursor);
		for (const auto& [savedTick, uid]: image.quarantine) {
			writer.NewProperty("LockstepJoinQuarantine");
			writer << savedTick << "|" << uid;
		}
		for (size_t i = 0; i < image.graphs.size(); ++i) {
			writer.NewProperty("LuaStateGraph");
			writer << i << "|";
			writer << image.graphs[i].Base64();
		}
		writer.NewPropertyWithValue("PlaceObjectsIfSceneIsRestarted", image.placeObjects);
		writer.NewPropertyWithValue("PlaceUnitsIfSceneIsRestarted", image.placeUnits);
		writer.Append(image.scene);
	});
}

CheckpointText RTE::AssembleCheckpointIndex(const CheckpointImage& image) {
	return Writer::Capture([&](Writer& writer) {
		writer.NewPropertyWithValue("ActivityName", image.activityName);
		writer.NewPropertyWithValue("OriginalScenePresetName", image.originalScenePresetName);
	});
}

namespace {
	void WriteStoreZip(const std::filesystem::path& path, const std::map<std::string, std::string>& members) {
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out) throw std::runtime_error("could not write " + path.string());
		struct Entry { std::string name; uint32_t crc; uint32_t size; uint32_t offset; };
		std::vector<Entry> catalog;
		const auto crc32 = [](const std::string& data) {
			uint32_t crc = 0xFFFFFFFFu;
			for (unsigned char byte: data) {
				crc ^= byte;
				for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
			}
			return crc ^ 0xFFFFFFFFu;
		};
		const auto put32 = [&](uint32_t value) {
			const unsigned char bytes[] = {
				static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8),
				static_cast<unsigned char>(value >> 16), static_cast<unsigned char>(value >> 24)
			};
			out.write(reinterpret_cast<const char*>(bytes), 4);
		};
		const auto put16 = [&](uint16_t value) {
			const unsigned char bytes[] = {static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8)};
			out.write(reinterpret_cast<const char*>(bytes), 2);
		};
		for (const auto& [name, data]: members) {
			Entry entry{name, crc32(data), static_cast<uint32_t>(data.size()), static_cast<uint32_t>(out.tellp())};
			put32(0x04034b50);
			put16(20); put16(0); put16(0); put16(0); put16(0);
			put32(entry.crc); put32(entry.size); put32(entry.size);
			put16(static_cast<uint16_t>(name.size())); put16(0);
			out.write(name.data(), static_cast<std::streamsize>(name.size()));
			out.write(data.data(), static_cast<std::streamsize>(data.size()));
			catalog.push_back(std::move(entry));
		}
		const uint32_t central = static_cast<uint32_t>(out.tellp());
		for (const auto& entry: catalog) {
			put32(0x02014b50);
			put16(20); put16(20); put16(0); put16(0); put16(0); put16(0);
			put32(entry.crc); put32(entry.size); put32(entry.size);
			put16(static_cast<uint16_t>(entry.name.size())); put16(0); put16(0); put16(0); put16(0);
			put32(0); put32(entry.offset);
			out.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
		}
		const uint32_t end = static_cast<uint32_t>(out.tellp());
		put32(0x06054b50);
		put16(0); put16(0);
		put16(static_cast<uint16_t>(catalog.size())); put16(static_cast<uint16_t>(catalog.size()));
		put32(end - central); put32(central); put16(0);
	}

	std::map<std::string, std::string> ImageMembers(const CheckpointImage& image, const std::string& png) {
		return {
			{"Save.ini", AssembleCheckpointSave(image).Text()},
			{"Index.ini", AssembleCheckpointIndex(image).Text()},
			{"Save Mat.png", png},
			{"Save FG.png", png},
			{"Save BG.png", png},
		};
	}
}

bool RTE::RunCheckpointImageSelfTest() {
	bool passed = true;
	const auto fail = [&passed](const char* name, const std::string& actual, const std::string& required) {
		std::cout << "[cow-checkpoint-selftest] FAIL " << name << " actual=" << actual << " required=" << required << std::endl;
		passed = false;
	};
	const auto pass = [](const char* name, const std::string& detail) {
		std::cout << "[cow-checkpoint-selftest] PASS " << name << " " << detail << std::endl;
	};
	try {
		struct LiveObject {
			int health = 10;
			float rotation = 0.25f;
		} live;
		CheckpointImage image;
		image.activityName = "Autosave Capture 240";
		image.originalScenePresetName = "Grasslands";
		image.simUpdateCount = 60;
		image.activity = Writer::Capture([](Writer& writer) { writer.NewPropertyWithValue("Activity", "probe"); });
		image.scene = Writer::Capture([&live](Writer& writer) {
			writer.NewPropertyWithValue("Health", live.health);
			writer.NewPropertyWithValue("Rotation", live.rotation);
		});
		image.globals = Writer::Capture([](Writer& writer) { writer.NewPropertyWithValue("Globals", 1); });
		image.structure = Writer::Capture([](Writer& writer) { writer.NewPropertyWithValue("Structure", 1); });
		image.sceneRuntime = Writer::Capture([](Writer& writer) { writer.NewPropertyWithValue("Runtime", 1); });
		const std::string png(64, '\x89');
		std::atomic<bool> go{false};
		auto worker = std::async(std::launch::async, [&image, &go, &png] {
			while (!go.load(std::memory_order_acquire)) {}
			return ImageMembers(image, png);
		});
		go.store(true, std::memory_order_release);
		live.health = 77;
		live.rotation = 9.9f;
		const auto members = worker.get();
		const std::string& save = members.at("Save.ini");
		if (save.find("77") != std::string::npos || save.find("9.9") != std::string::npos) {
			fail("image_ignores_writes_during_worker_traversal", "worker text contains the live mutation", "frozen Health=10 Rotation=0.25");
		} else if (save.find("10") == std::string::npos) {
			fail("image_ignores_writes_during_worker_traversal", "missing frozen Health", "Health=10 in the image");
		} else {
			pass("image_ignores_writes_during_worker_traversal", "worker formatted the frozen image");
		}

		const auto syncMembers = ImageMembers(image, png);
		const auto workerMembers = [&] {
			return std::async(std::launch::async, [&image, &png] { return ImageMembers(image, png); }).get();
		}();
		const std::filesystem::path root = std::filesystem::temp_directory_path() / "cow-checkpoint-selftest";
		std::filesystem::create_directories(root);
		const auto syncZip = root / "sync.ccsave";
		const auto imageZip = root / "image.ccsave";
		WriteStoreZip(syncZip, syncMembers);
		WriteStoreZip(imageZip, workerMembers);
		if (const char* dump = std::getenv("CCCP_CHECKPOINT_ROUNDTRIP")) {
			std::filesystem::create_directories(dump);
			WriteStoreZip(std::filesystem::path(dump) / "sync.ccsave", syncMembers);
			WriteStoreZip(std::filesystem::path(dump) / "image.ccsave", workerMembers);
		}
		std::ifstream syncIn(syncZip, std::ios::binary);
		std::ifstream imageIn(imageZip, std::ios::binary);
		const std::string syncBytes((std::istreambuf_iterator<char>(syncIn)), {});
		const std::string imageBytes((std::istreambuf_iterator<char>(imageIn)), {});
		std::string memberDiff;
		if (syncMembers.size() != workerMembers.size()) memberDiff = "member count";
		for (const auto& [name, data]: syncMembers) {
			const auto found = workerMembers.find(name);
			if (found == workerMembers.end() || found->second != data) memberDiff = name;
		}
		if (!memberDiff.empty() || syncBytes != imageBytes) {
			fail("restore_round_trip_matches_synchronous_capture",
			     memberDiff.empty() ? "zip bytes differ" : memberDiff + " differs",
			     "every zip member including PNGs matches the synchronous capture of the same image");
		} else {
			pass("restore_round_trip_matches_synchronous_capture", "Save.ini Index.ini and PNG members match");
		}

		CheckpointCache cache;
		cache.Begin();
		int stamp = 3;
		CheckpointText first = Writer::Capture([&stamp](Writer& writer) { writer.NewPropertyWithValue("Value", stamp); });
		first = cache.Remember(&stamp, 1, first, 11);
		stamp = 9;
		CheckpointText later = Writer::Capture([&stamp](Writer& writer) { writer.NewPropertyWithValue("Value", stamp); });
		later = cache.Remember(&stamp, 1, later, 12);
		if (first.SameValues(later) || later.Text().find("9") == std::string::npos) {
			fail("generational_shadow_keeps_the_freeze_value", later.Text(), "owned 9 after the stamp moved");
		} else {
			pass("generational_shadow_keeps_the_freeze_value", "later shadow holds 9");
		}
		stamp = 9;
		const CheckpointText* peeked = cache.Peek(&stamp, 1);
		if (!peeked || cache.Stamp(&stamp, 1) != 12 || !peeked->SameValues(later)) {
			fail("peek_reuses_the_shadow_when_the_stamp_matches", "peek missed", "stamp 12 reuses the later shadow");
		} else {
			pass("peek_reuses_the_shadow_when_the_stamp_matches", "stamp 12");
		}
	} catch (const std::exception& error) {
		fail("no_unexpected_exception", error.what(), "no exception");
	}
	std::cout << "[cow-checkpoint-selftest] " << (passed ? "PASS" : "FAIL") << std::endl;
	return passed;
}
