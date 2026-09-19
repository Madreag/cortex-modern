#include "CheckpointImage.h"
#include "CheckpointArchive.h"
#include "Writer.h"
#include "Scene.h"
#include "MovableMan.h"
#include "MovableObject.h"
#include "AudioMan.h"
#include "LuaMan.h"
#include "RTETools.h"

#include "lua.hpp"

#include <algorithm>
#include <array>
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
#include <unordered_set>
#include <vector>

using namespace RTE;

namespace {
	std::atomic<uint64_t> s_LuaWrites{0};

	void OnLuaTableWrite(void* table) {
		s_LuaWrites.fetch_add(1, std::memory_order_relaxed);
		CheckpointGraphIndex::Get().OnTableWritten(table);
	}

	int64_t Percentile99(std::vector<int64_t> samples) {
		if (samples.empty()) return 0;
		std::sort(samples.begin(), samples.end());
		const size_t index = samples.size() - 1 - samples.size() / 100;
		return samples[std::min(index, samples.size() - 1)];
	}
}

CheckpointGraphIndex& CheckpointGraphIndex::Get() {
	static CheckpointGraphIndex index;
	return index;
}

void CheckpointGraphIndex::BeginWalk(bool full) {
	std::lock_guard lock(m_Mutex);
	m_Walking.clear();
	m_WalkingRoots.clear();
	m_Walk = true;
	m_FullWalk = full;
	m_Root = 0;
	m_WalkNoteUs = 0;
}

void CheckpointGraphIndex::BeginRoot(uint64_t root) {
	std::lock_guard lock(m_Mutex);
	m_Root = root;
	if (m_Walk) m_WalkingRoots.insert(root);
}

void CheckpointGraphIndex::NoteTable(const void* table) {
	const auto start = std::chrono::steady_clock::now();
	std::lock_guard lock(m_Mutex);
	if (!m_Walk || !table) return;
	// The first root to reach a shared table owns it; that root is dirty when the table is written.
	m_Walking.emplace(table, m_Root);
	m_WalkNoteUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
}

void CheckpointGraphIndex::EndWalk() {
	std::lock_guard lock(m_Mutex);
	if (m_FullWalk) {
		m_TableRoots = std::move(m_Walking);
		m_Roots = std::move(m_WalkingRoots);
		m_DirtyRoots.clear();
		m_UnknownTable = false;
	} else {
		// A partial walk keeps what the roots it skipped recorded, so their tables stay known.
		for (auto entry = m_TableRoots.begin(); entry != m_TableRoots.end();) {
			entry = m_WalkingRoots.count(entry->second) ? m_TableRoots.erase(entry) : std::next(entry);
		}
		for (const auto& [table, root]: m_Walking) m_TableRoots[table] = root;
		for (uint64_t root: m_WalkingRoots) {
			m_Roots.insert(root);
			m_DirtyRoots.erase(root);
		}
	}
	m_Walking.clear();
	m_WalkingRoots.clear();
	m_DirtyTables = 0;
	m_NoteUs = m_WalkNoteUs;
	m_Walk = false;
	m_FullWalk = true;
	m_Root = 0;
}

void CheckpointGraphIndex::NoteRootReuse(size_t reused, size_t rewritten) {
	std::lock_guard lock(m_Mutex);
	m_RootsReused = reused;
	m_RootsRewritten = rewritten;
	// A capture that reused a chunk did not walk that root, so the map keeps what it recorded before.
	m_FullWalk = reused == 0;
}

std::unordered_set<uint64_t> CheckpointGraphIndex::DirtyRoots() const {
	std::lock_guard lock(m_Mutex);
	return m_DirtyRoots;
}

bool CheckpointGraphIndex::UnknownTableWritten() const {
	std::lock_guard lock(m_Mutex);
	return m_UnknownTable;
}

bool CheckpointGraphIndex::HasWalked() const {
	std::lock_guard lock(m_Mutex);
	return !m_Roots.empty();
}

void CheckpointGraphIndex::OnTableWritten(const void* table) {
	std::lock_guard lock(m_Mutex);
	if (m_Walk) return;  // The walk writes its own scratch tables.
	if (const auto found = m_TableRoots.find(table); found != m_TableRoots.end()) {
		m_DirtyRoots.insert(found->second);
	} else if (!m_TableRoots.empty()) {
		m_UnknownTable = true;
	}
	++m_DirtyTables;
}

GraphDirt CheckpointGraphIndex::Sample() const {
	std::lock_guard lock(m_Mutex);
	GraphDirt dirt;
	dirt.roots = m_Roots.size();
	dirt.tables = m_TableRoots.size();
	dirt.dirtyRoots = m_DirtyRoots.size();
	dirt.dirtyTables = m_DirtyTables;
	dirt.unknownTable = m_UnknownTable;
	dirt.noteUs = m_NoteUs;
	dirt.rootsReused = m_RootsReused;
	dirt.rootsRewritten = m_RootsRewritten;
	return dirt;
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

bool CheckpointCow::LuaUnchanged(uint64_t writeGeneration, size_t stateCount) const {
	std::lock_guard lock(m_Mutex);
	// A state added or dropped since the last capture changes the graph set whatever the write generation says.
	return writeGeneration == m_LuaWriteGeneration && !m_LuaGraphs.empty() && m_LuaGraphs.size() == stateCount;
}

bool CheckpointCow::HasLua(size_t stateCount) const {
	std::lock_guard lock(m_Mutex);
	return !m_LuaGraphs.empty() && m_LuaGraphs.size() == stateCount;
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
	m_LastRecords = {image->layersUs, image->activityUs, image->graphUs, image->sceneUs,
	                 image->structureUs, image->sceneRuntimeUs, image->globalsUs};
	m_LastGraph = image->graph;
	m_LastGraphSerial = image->graphSerial;
	m_LastRootsReused = image->graphRootsReused;
	m_LastRootsRewritten = image->graphRootsRewritten;
	m_LastReused = image->objectsReused;
	m_LastCaptured = image->objectsCaptured;
	m_LastLuaReused = image->luaReused;
	m_Last = std::move(image);
}

void CheckpointCow::RecordWorker(int64_t workerUs) {
	std::lock_guard lock(m_Mutex);
	m_LastWorkerUs = workerUs;
}

// The graph's formatting runs on the worker, so its cost is reported apart from the freeze's walk.
void CheckpointCow::RecordGraphText(int64_t graphTextUs) {
	std::lock_guard lock(m_Mutex);
	m_LastGraphTextUs = graphTextUs;
}

void CheckpointCow::PublishLog(uint64_t tick) const {
	int64_t freezeUs = 0;
	int64_t workerUs = 0;
	size_t imageBytes = 0;
	double dirtyRatio = 0;
	int64_t p99 = 0;
	std::vector<int64_t> samples;
	std::array<int64_t, 7> records{};
	GraphDirt graph;
	size_t reused = 0;
	size_t captured = 0;
	bool luaReused = false;
	int64_t graphTextUs = 0;
	uint64_t graphSerial = 0;
	size_t rootsReused = 0;
	size_t rootsRewritten = 0;
	{
		std::lock_guard lock(m_Mutex);
		freezeUs = m_LastFreezeUs;
		workerUs = m_LastWorkerUs;
		imageBytes = m_LastImageBytes;
		dirtyRatio = m_LastDirtyRatio;
		samples = m_FreezeSamples;
		records = m_LastRecords;
		graph = m_LastGraph;
		reused = m_LastReused;
		captured = m_LastCaptured;
		luaReused = m_LastLuaReused;
		graphTextUs = m_LastGraphTextUs;
		graphSerial = m_LastGraphSerial;
		rootsReused = m_LastRootsReused;
		rootsRewritten = m_LastRootsRewritten;
	}
	p99 = Percentile99(std::move(samples));
	std::cout << std::format("[autosave] tick={} freeze_us={} worker_us={} image_bytes={} dirty_ratio={:.6f} p99_freeze_us={}\n",
	                         tick, freezeUs, workerUs, imageBytes, dirtyRatio, p99);
	// Where the freeze went, and how much of it the shadows and the graph index saved.
	std::cout << std::format("[autosave] tick={} layers_us={} activity_us={} graph_us={} scene_us={} structure_us={} scene_runtime_us={} globals_us={}\n",
	                         tick, records[0], records[1], records[2], records[3], records[4], records[5], records[6]);
	std::cout << std::format("[autosave] tick={} shadows_reused={} shadows_captured={} graph_roots={} graph_tables={} graph_dirty_roots={} graph_dirty_tables={} graph_unknown_table={} graph_note_us={} graph_reused={}\n",
	                         tick, reused, captured, graph.roots, graph.tables, graph.dirtyRoots, graph.dirtyTables,
	                         graph.unknownTable ? 1 : 0, graph.noteUs, luaReused ? 1 : 0);
	// The walk is the freeze's share and the text the worker's; the counter is the archive's numbering.
	std::cout << std::format("[autosave] tick={} graph_walk_us={} graph_text_us={} roots_reused={} roots_rewritten={} graph_state_serial={}\n",
	                         tick, records[2], graphTextUs, rootsReused, rootsRewritten, graphSerial) << std::flush;
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
	std::array<int64_t, 7> records{};
	GraphDirt graph;
	size_t reused = 0;
	size_t captured = 0;
	bool luaReused = false;
	int64_t graphTextUs = 0;
	uint64_t graphSerial = 0;
	size_t rootsReused = 0;
	size_t rootsRewritten = 0;
	{
		std::lock_guard lock(m_Mutex);
		freezeUs = m_LastFreezeUs;
		workerUs = m_LastWorkerUs;
		imageBytes = m_LastImageBytes;
		dirtyRatio = m_LastDirtyRatio;
		samples = m_FreezeSamples;
		sampleCount = m_FreezeSamples.size();
		records = m_LastRecords;
		graph = m_LastGraph;
		reused = m_LastReused;
		captured = m_LastCaptured;
		luaReused = m_LastLuaReused;
		graphTextUs = m_LastGraphTextUs;
		graphSerial = m_LastGraphSerial;
		rootsReused = m_LastRootsReused;
		rootsRewritten = m_LastRootsRewritten;
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
	    << "  \"layers_us\": " << records[0] << ",\n"
	    << "  \"activity_us\": " << records[1] << ",\n"
	    << "  \"graph_us\": " << records[2] << ",\n"
	    << "  \"scene_us\": " << records[3] << ",\n"
	    << "  \"structure_us\": " << records[4] << ",\n"
	    << "  \"scene_runtime_us\": " << records[5] << ",\n"
	    << "  \"globals_us\": " << records[6] << ",\n"
	    << "  \"shadows_reused\": " << reused << ",\n"
	    << "  \"shadows_captured\": " << captured << ",\n"
	    << "  \"graph_roots\": " << graph.roots << ",\n"
	    << "  \"graph_tables\": " << graph.tables << ",\n"
	    << "  \"graph_dirty_roots\": " << graph.dirtyRoots << ",\n"
	    << "  \"graph_dirty_tables\": " << graph.dirtyTables << ",\n"
	    << "  \"graph_unknown_table\": " << (graph.unknownTable ? 1 : 0) << ",\n"
	    << "  \"graph_note_us\": " << graph.noteUs << ",\n"
	    << "  \"graph_reused\": " << (luaReused ? 1 : 0) << ",\n"
	    << "  \"graph_text_us\": " << graphTextUs << ",\n"
	    << "  \"roots_reused\": " << rootsReused << ",\n"
	    << "  \"roots_rewritten\": " << rootsRewritten << ",\n"
	    << "  \"graph_state_serial\": " << graphSerial << ",\n"
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
		const auto graphTextStart = std::chrono::steady_clock::now();
		for (size_t i = 0; i < image.graphs.size(); ++i) {
			writer.NewProperty("LuaStateGraph");
			writer << i << "|";
			writer << image.graphs[i].Base64();
		}
		CheckpointCow::Get().RecordGraphText(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - graphTextStart).count());
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

}

bool RTE::RunCheckpointSceneRows() {
	bool passed = true;
	const auto fail = [&passed](const char* name, const std::string& actual, const std::string& required) {
		std::cout << "[cow-checkpoint-selftest] FAIL " << name << " actual=" << actual << " required=" << required << std::endl;
		passed = false;
	};
	const auto pass = [](const char* name, const std::string& detail) {
		std::cout << "[cow-checkpoint-selftest] PASS " << name << " " << detail << std::endl;
	};
	std::list<SceneObject*> actors;
	g_MovableMan.GetAllActors(false, actors);
	if (actors.empty()) {
		fail("image_ignores_writes_during_worker_traversal", "no live actor", "a live actor to capture");
		return false;
	}
	auto* live = dynamic_cast<MovableObject*>(actors.front());
	if (!live) {
		fail("image_ignores_writes_during_worker_traversal", "front actor is not a MovableObject", "a live MovableObject");
		return false;
	}
	// A capture assigns sound identities and can draw counters; the rows hand the sim back what they took.
	struct BorrowedCounters {
		RandomGenerator sim = g_SimRNG, render = g_RenderRNG;
		long uid = MovableObject::GetUniqueIDCounter();
		int cursor = g_LuaMan.GetScriptStateCursor();
		CheckpointSoundRegistry sounds = g_AudioMan.CaptureCheckpointSoundRegistry();
		uint64_t soundCursor = g_AudioMan.GetCheckpointSoundContainerCursor();
		std::unordered_set<uint64_t> carried = g_AudioMan.LastCarriedSoundIdentities();
		~BorrowedCounters() {
			g_SimRNG = sim;
			g_RenderRNG = render;
			MovableObject::PinUniqueIDCounter(uid);
			g_LuaMan.SetScriptStateCursor(cursor);
			g_AudioMan.RestoreCheckpointSoundRegistry(std::move(sounds));
			g_AudioMan.SetCheckpointSoundContainerCursor(soundCursor);
			g_AudioMan.RememberCarriedSoundIdentities(std::move(carried));
		}
	} borrowed;
	const auto capture = [live](CheckpointCache* cache) {
		CheckpointWriter::CacheScope scope(cache);
		return Writer::Capture([live](Writer& writer) { Scene::SaveSceneObject(writer, live, false, true); });
	};
	const auto syncSave = [live] {
		auto stream = std::make_unique<std::ostringstream>();
		std::ostringstream* text = stream.get();
		Writer writer(std::move(stream));
		Scene::SaveSceneObject(writer, live, false, true);
		return text->str();
	};
	const auto firstDifference = [](const std::string& left, const std::string& right) {
		const size_t at = std::mismatch(left.begin(), left.end(), right.begin(), right.end()).first - left.begin();
		const size_t from = left.rfind('\n', at) == std::string::npos ? 0 : left.rfind('\n', at) + 1;
		return left.substr(from, left.find('\n', at) == std::string::npos ? std::string::npos : left.find('\n', at) - from)
		     + " | " + right.substr(from, right.find('\n', at) == std::string::npos ? std::string::npos : right.find('\n', at) - from);
	};

	CheckpointCache cache;
	cache.Begin();
	const CheckpointText shadow = capture(&cache);
	const float pinned = live->GetPinStrength();
	std::atomic<bool> go{false};
	auto worker = std::async(std::launch::async, [&shadow, &go] {
		while (!go.load(std::memory_order_acquire)) {}
		return shadow.Text();
	});
	go.store(true, std::memory_order_release);
	live->SetPinStrength(4242.0F);
	const std::string formatted = worker.get();
	live->SetPinStrength(pinned);
	if (formatted.find("4242") != std::string::npos) {
		fail("image_ignores_writes_during_worker_traversal", "worker text carries the write it raced",
		     "the value the freeze owned");
	} else if (formatted.find("PinStrength") == std::string::npos) {
		fail("image_ignores_writes_during_worker_traversal", "no PinStrength in the owned text",
		     "the live object captured through Scene::SaveSceneObject");
	} else {
		pass("image_ignores_writes_during_worker_traversal", "worker formatted the frozen values of a live actor");
	}

	const std::string sync = syncSave();
	cache.Begin();
	const std::string fresh = capture(&cache).Text();
	cache.Begin();
	const std::string reused = capture(&cache).Text();
	if (const char* dump = std::getenv("CCCP_CHECKPOINT_ROUNDTRIP")) {
		std::filesystem::create_directories(dump);
		WriteStoreZip(std::filesystem::path(dump) / "sync.ccsave", {{"Save.ini", sync}});
		WriteStoreZip(std::filesystem::path(dump) / "image.ccsave", {{"Save.ini", reused}});
	}
	if (sync != fresh) {
		fail("restore_round_trip_matches_synchronous_capture", firstDifference(sync, fresh),
		     "the image capture of a live actor matches its synchronous save at the same tick");
	} else if (fresh != reused) {
		fail("restore_round_trip_matches_synchronous_capture", firstDifference(fresh, reused),
		     "a reused shadow matches a fresh capture of the same tick");
	} else {
		pass("restore_round_trip_matches_synchronous_capture", "sync, fresh and reused captures are the same bytes");
	}
	return passed;
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
