#include "CheckpointImage.h"
#include "PageWriteFence.h"
#include "CheckpointArchive.h"
#include "ContentFile.h"
#include "Writer.h"
#include "Scene.h"
#include "MovableMan.h"
#include "MovableObject.h"
#include "Atom.h"
#include "AtomGroup.h"
#include "Gib.h"
#include "MOSRotating.h"
#include "Actor.h"
#include "ACDropShip.h"
#include "ADoor.h"
#include "SoundSet.h"
#include "SoundContainer.h"
#include "AudioMan.h"
#include "LuaMan.h"
#include "RTETools.h"
#include "TimerMan.h"

#include "lua.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace RTE;

namespace {
	std::atomic<uint64_t> s_LuaWrites{0};
	std::atomic<int> s_BarrierPaused{0};
	thread_local int s_BarrierPauseDepth = 0;
	std::atomic<uint64_t> s_PausedForeignWrites{0};
	thread_local int s_BarrierIgnored = 0;

	void OnLuaTableWrite(void* table) {
		if (s_BarrierIgnored > 0) return;
		if (s_BarrierPaused.load(std::memory_order_relaxed) > 0) {
			// A capturing thread's own scratch writes are its business; the callback stays installed so
			// every table born in the capture still gets its trap.
			if (s_BarrierPauseDepth > 0) {
				return;
			}
			// The freeze is meant to hold every Lua thread. One that wrote anyway would be lost
			// here, and the next capture would reuse a graph that changed under it, so the write
			// is counted and reported instead of assumed away.
			s_PausedForeignWrites.fetch_add(1, std::memory_order_relaxed);
		}
		s_LuaWrites.fetch_add(1, std::memory_order_relaxed);
		CheckpointGraphIndex::Get().OnTableWritten(table);
	}

	// A mutated native whose engine values a chunk carries stales that chunk exactly as a table write
	// does, so it is counted and attributed the same way. The write generation moves too: a capture
	// that reuses the whole Lua half keys on it.
	void OnLuaValueWrite(void* value) {
		if (s_BarrierIgnored > 0) return;
		if (s_BarrierPaused.load(std::memory_order_relaxed) > 0) {
			if (s_BarrierPauseDepth > 0) {
				return;
			}
			s_PausedForeignWrites.fetch_add(1, std::memory_order_relaxed);
		}
		s_LuaWrites.fetch_add(1, std::memory_order_relaxed);
		CheckpointGraphIndex::Get().OnValueWritten(value);
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

void CheckpointGraphIndex::BeginWalk(bool full, bool world) {
	std::lock_guard lock(m_Mutex);
	// A capture of every state opens the walk once; each state's own capture nests inside it.
	if (m_WalkDepth++ > 0) return;
	m_Walking.clear();
	m_WalkingValues.clear();
	m_WalkingRoots.clear();
	m_ReusedRoots.clear();
	m_Walk = true;
	m_FullWalk = full;
	m_WorldWalk = world;
	m_RootsReused = 0;
	m_RootsRewritten = 0;
	m_Root = {};
	m_UncacheableRoots = 0;
	m_WalkNoteUs = 0;
	m_WalkStates.clear();
	m_WalkParts.clear();
}

void CheckpointGraphIndex::BeginRoot(uint64_t root, const void* state, std::string part) {
	std::lock_guard lock(m_Mutex);
	m_Root = {state, root, std::move(part)};
	if (m_Walk) m_WalkingRoots.insert(m_Root);
}

void CheckpointGraphIndex::ReuseRoot(uint64_t root, const void* state, const std::string& part) {
	std::lock_guard lock(m_Mutex);
	m_FullWalk = false;
	m_ReusedRoots.insert({state, root, part});
}

void CheckpointGraphIndex::RestartStateWalk(const void* state) {
	std::lock_guard lock(m_Mutex);
	for (auto entry = m_Walking.begin(); entry != m_Walking.end();) {
		std::erase_if(entry->second, [state](const Root& root) { return root.state == state; });
		entry = entry->second.empty() ? m_Walking.erase(entry) : std::next(entry);
	}
	std::erase_if(m_WalkingValues, [state](const auto& entry) { return entry.second.state == state; });
	std::erase_if(m_WalkingRoots, [state](const Root& root) { return root.state == state; });
	std::erase_if(m_ReusedRoots, [state](const Root& root) { return root.state == state; });
}

void CheckpointGraphIndex::NoteTable(const void* table) {
	const auto start = std::chrono::steady_clock::now();
	std::lock_guard lock(m_Mutex);
	if (!m_Walk || !table) return;
	// Path lookup and node ownership can depend on the same table.
	auto& roots = m_Walking[table];
	if (std::find(roots.begin(), roots.end(), m_Root) == roots.end()) roots.push_back(m_Root);
	m_WalkNoteUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
}

void CheckpointGraphIndex::NoteValue(const void* value) {
	std::lock_guard lock(m_Mutex);
	if (!m_Walk || !value) return;
	// A native's engine values are written into the chunk of the first root that reached it, so that
	// root is the one a mutation of those values makes stale.
	m_WalkingValues.emplace(value, m_Root);
}

void CheckpointGraphIndex::NoteUncacheableRoots(size_t roots) {
	std::lock_guard lock(m_Mutex);
	m_UncacheableRoots += roots;
}

void CheckpointGraphIndex::EndWalk() {
	const auto start = std::chrono::steady_clock::now();
	std::lock_guard lock(m_Mutex);
	if (!m_Walk || --m_WalkDepth > 0) return;
	// A walk answers for the states it covered: their rewritten roots leave the index, their reused
	// ones stay. A world walk covered every state, so a root it never saw belongs to a state that is
	// gone; a walk of one state leaves the other states' entries and their dirt in place.
	std::unordered_set<const void*> walked;
	for (const Root& root: m_WalkingRoots) walked.insert(root.state);
	for (const Root& root: m_ReusedRoots) walked.insert(root.state);
	for (const void* state: m_WalkStates) walked.insert(state);
	const bool world = m_WorldWalk;
	const auto covered = [&](const Root& root) { return world || walked.contains(root.state); };
	const auto rewritten = [&](const Root& root) { return covered(root) && !m_ReusedRoots.contains(root); };
	for (auto entry = m_TableRoots.begin(); entry != m_TableRoots.end();) {
		std::erase_if(entry->second, rewritten);
		entry = entry->second.empty() ? m_TableRoots.erase(entry) : std::next(entry);
	}
	for (auto entry = m_ValueRoots.begin(); entry != m_ValueRoots.end();) {
		entry = rewritten(entry->second) ? m_ValueRoots.erase(entry) : std::next(entry);
	}
	std::erase_if(m_Roots, rewritten);
	std::erase_if(m_DirtyRoots, covered);
	for (const auto& [table, roots]: m_Walking) {
		auto& kept = m_TableRoots[table];
		for (const Root& root: roots) if (std::find(kept.begin(), kept.end(), root) == kept.end()) kept.push_back(root);
	}
	for (const auto& [value, root]: m_WalkingValues) m_ValueRoots[value] = root;
	m_Roots.insert(m_WalkingRoots.begin(), m_WalkingRoots.end());
	// The unknown table is answered once no state this walk skipped could still own it.
	const bool skipped = !world && std::any_of(m_Roots.begin(), m_Roots.end(), [&walked](const Root& root) { return !walked.contains(root.state); });
	if (!skipped) m_UnknownTable = false;
	m_Walking.clear();
	m_WalkingValues.clear();
	m_WalkingRoots.clear();
	m_ReusedRoots.clear();
	if (world) {
		m_DirtyTables = 0;
		m_DirtyValues = 0;
	}
	m_NoteUs = m_WalkNoteUs;
	m_Walk = false;
	m_FullWalk = true;
	m_WorldWalk = true;
	m_Root = {};
	m_WalkParts.push_back({0, "index_finish", 0, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count(), false, {}});
}

void CheckpointGraphIndex::NoteRootReuse(size_t reused, size_t rewritten) {
	std::lock_guard lock(m_Mutex);
	// One walk covers every state, so the counts are the whole capture's, not the last state's.
	if (m_Walk) {
		m_RootsReused += reused;
		m_RootsRewritten += rewritten;
	} else {
		m_RootsReused = reused;
		m_RootsRewritten = rewritten;
	}
	// A capture that reused a chunk did not walk that root, so the map keeps what it recorded before.
	m_FullWalk = m_FullWalk && m_RootsReused == 0;
}

void CheckpointGraphIndex::NoteWalkPart(const void* state, std::string part, uint64_t root, int64_t elapsedUs, bool reused, std::string unwatched) {
	std::lock_guard lock(m_Mutex);
	const auto found = !state && !m_WalkStates.empty() ? std::prev(m_WalkStates.end()) : std::find(m_WalkStates.begin(), m_WalkStates.end(), state);
	const size_t index = found - m_WalkStates.begin();
	if (found == m_WalkStates.end()) m_WalkStates.push_back(state);
	m_WalkParts.push_back({index, std::move(part), root, elapsedUs, reused, std::move(unwatched)});
}

std::unordered_set<uint64_t> CheckpointGraphIndex::DirtyRoots(const void* state) const {
	std::lock_guard lock(m_Mutex);
	std::unordered_set<uint64_t> roots;
	for (const Root& root: m_DirtyRoots) if (root.state == state) roots.insert(root.id);
	return roots;
}

std::unordered_set<std::string> CheckpointGraphIndex::DirtyParts(const void* state, uint64_t root) const {
	std::lock_guard lock(m_Mutex);
	std::unordered_set<std::string> parts;
	for (const Root& dirty: m_DirtyRoots) if (dirty.state == state && dirty.id == root) parts.insert(dirty.part);
	return parts;
}

bool CheckpointGraphIndex::UnknownTableWritten() const {
	std::lock_guard lock(m_Mutex);
	return m_UnknownTable;
}

bool CheckpointGraphIndex::HasWalked() const {
	std::lock_guard lock(m_Mutex);
	return !m_Roots.empty();
}

bool CheckpointGraphIndex::CanReuseWhole() const {
	std::lock_guard lock(m_Mutex);
	return !m_Roots.empty() && m_UncacheableRoots == 0 && m_DirtyRoots.empty();
}

void CheckpointGraphIndex::OnTableWritten(const void* table) {
	std::lock_guard lock(m_Mutex);
	if (m_Walk) return;  // The walk writes its own scratch tables.
	if (const auto found = m_TableRoots.find(table); found != m_TableRoots.end()) {
		for (const Root& root: found->second) m_DirtyRoots.insert(root);
	} else if (!m_TableRoots.empty()) {
		// A table the walk never recorded is in no chunk, so it stales none. The flag stays as the
		// count of writes that named no root; the root cache no longer refuses itself over it.
		m_UnknownTable = true;
	}
	++m_DirtyTables;
}

void CheckpointGraphIndex::OnValueWritten(const void* value) {
	std::lock_guard lock(m_Mutex);
	if (m_Walk || !value) return;  // The walk reads every value it records.
	if (const auto found = m_ValueRoots.find(value); found != m_ValueRoots.end()) {
		m_DirtyRoots.insert(found->second);
		++m_DirtyValues;
	}
}

GraphDirt CheckpointGraphIndex::Sample() const {
	std::lock_guard lock(m_Mutex);
	GraphDirt dirt;
	Roots roots, dirtyRoots;
	for (const Root& root: m_Roots) roots.insert({root.state, root.id, {}});
	for (const Root& root: m_DirtyRoots) dirtyRoots.insert({root.state, root.id, {}});
	dirt.roots = roots.size();
	dirt.tables = m_TableRoots.size();
	dirt.dirtyRoots = dirtyRoots.size();
	dirt.dirtyTables = m_DirtyTables;
	dirt.values = m_ValueRoots.size();
	dirt.dirtyValues = m_DirtyValues;
	dirt.uncacheableRoots = m_UncacheableRoots;
	dirt.unknownTable = m_UnknownTable;
	dirt.noteUs = m_NoteUs;
	dirt.rootsReused = m_RootsReused;
	dirt.rootsRewritten = m_RootsRewritten;
	dirt.walkParts = m_WalkParts;
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
	if (!CheckpointGraphIndex::Get().CanReuseWhole()) return false;
	std::lock_guard lock(m_Mutex);
	// A state added or dropped since the last capture changes the graph set whatever the write generation says.
	return writeGeneration == m_LuaWriteGeneration && !m_LuaGraphs.empty() && m_LuaGraphs.size() == stateCount;
}

bool CheckpointCow::HasLua(size_t stateCount) const {
	if (!CheckpointGraphIndex::Get().CanReuseWhole()) return false;
	std::lock_guard lock(m_Mutex);
	return !m_LuaGraphs.empty() && m_LuaGraphs.size() == stateCount;
}

std::shared_ptr<const CheckpointImage> CheckpointCow::FinishImage(std::shared_ptr<CheckpointImage> image) {
	if (!image) return {};
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
	m_LastGraphBeforeWalk = image->graphBeforeWalk;
	m_LastGraphSerial = image->graphSerial;
	m_LastRootsReused = image->graphRootsReused;
	m_LastRootsRewritten = image->graphRootsRewritten;
	m_LastReused = image->objectsReused;
	m_LastCaptured = image->objectsCaptured;
	m_LastLuaReused = image->luaReused;
	return std::exchange(m_Last, std::move(image));
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

void CheckpointCow::PublishLog(const CheckpointImage& image, int64_t workerUs) const {
	const uint64_t tick = image.tick;
	int64_t freezeUs = 0;
	size_t imageBytes = 0;
	double dirtyRatio = 0;
	int64_t p99 = 0;
	std::vector<int64_t> samples;
	std::array<int64_t, 7> records{};
	GraphDirt graph;
	GraphDirt before;
	size_t reused = 0;
	size_t captured = 0;
	bool luaReused = false;
	int64_t graphTextUs = 0;
	uint64_t graphSerial = 0;
	size_t rootsReused = 0;
	size_t rootsRewritten = 0;
	{
		std::lock_guard lock(m_Mutex);
		freezeUs = image.freezeUs;
		imageBytes = image.imageBytes;
		dirtyRatio = image.dirtyRatio;
		samples = m_FreezeSamples;
		records = {image.layersUs, image.activityUs, image.graphUs, image.sceneUs, image.structureUs, image.sceneRuntimeUs, image.globalsUs};
		graph = image.graph;
		before = image.graphBeforeWalk;
		reused = image.objectsReused;
		captured = image.objectsCaptured;
		luaReused = image.luaReused;
		graphTextUs = m_LastGraphTextUs;
		graphSerial = image.graphSerial;
		rootsReused = image.graphRootsReused;
		rootsRewritten = image.graphRootsRewritten;
	}
	p99 = Percentile99(std::move(samples));
	std::cout << std::format("[autosave] tick={} freeze_us={} worker_us={} image_bytes={} dirty_ratio={:.6f} p99_freeze_us={}\n",
	                         tick, freezeUs, workerUs, imageBytes, dirtyRatio, p99);
	// Where the freeze went, and how much of it the shadows and the graph index saved.
	std::cout << std::format("[autosave] tick={} layers_us={} activity_us={} graph_us={} scene_us={} structure_us={} scene_runtime_us={} globals_us={}\n",
	                         tick, records[0], records[1], records[2], records[3], records[4], records[5], records[6]);
	std::cout << std::format("[autosave] tick={} shadows_reused={} shadows_captured={} graph_roots={} graph_tables={} graph_dirty_roots={} graph_dirty_tables={} graph_unknown_table={} graph_note_us={} graph_reused={} paused_writes={}\n",
	                         tick, reused, captured, graph.roots, graph.tables, before.dirtyRoots, before.dirtyTables,
	                         before.unknownTable ? 1 : 0, graph.noteUs, luaReused ? 1 : 0, LuaCheckpointPausedWrites());
	// The walk is the freeze's share and the text the worker's; the counter is the archive's numbering.
	// A root barred from reuse reached an upvalue cell or a coroutine, which no barrier watches.
	std::cout << std::format("[autosave] tick={} graph_walk_us={} graph_text_us={} roots_reused={} roots_rewritten={} graph_state_serial={} graph_values={} graph_dirty_values={} graph_uncacheable_roots={}\n",
	                         tick, records[2], graphTextUs, rootsReused, rootsRewritten, graphSerial,
	                         graph.values, before.dirtyValues, graph.uncacheableRoots) << std::flush;
	if (!luaReused) {
		for (const auto& part: graph.walkParts) {
			std::cout << std::format("[autosave] tick={} graph_vm={} graph_part={} graph_root={} graph_part_us={} graph_chunk_reused={} graph_unwatched=",
			                         tick, part.state, part.part, part.root, part.elapsedUs, part.reused ? 1 : 0)
			          << std::quoted(part.unwatched) << '\n';
		}
		std::cout << std::flush;
	}
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
	GraphDirt before;
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
		before = m_LastGraphBeforeWalk;
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
	ArmLuaCheckpointValueBarrier();
}

void RTE::CheckpointValueWritten(const void* value) {
	OnLuaValueWrite(const_cast<void*>(value));
}

// A capture's own scratch tables are not gameplay writes, and the walk discards every write it sees.
RTE::LuaCheckpointBarrierPause::LuaCheckpointBarrierPause() {
	// Each pausing thread's own writes are the capture's scratch; any other thread's are a defect.
	++s_BarrierPauseDepth;
	s_BarrierPaused.fetch_add(1, std::memory_order_relaxed);
}

RTE::LuaCheckpointBarrierPause::~LuaCheckpointBarrierPause() {
	s_BarrierPaused.fetch_sub(1, std::memory_order_relaxed);
	--s_BarrierPauseDepth;
}

RTE::LuaCheckpointBarrierIgnore::LuaCheckpointBarrierIgnore() { ++s_BarrierIgnored; }

RTE::LuaCheckpointBarrierIgnore::~LuaCheckpointBarrierIgnore() { --s_BarrierIgnored; }

uint64_t RTE::LuaCheckpointPausedWrites() {
	return s_PausedForeignWrites.load(std::memory_order_relaxed);
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
		writer.NewPropertyWithValue("ScriptRegistrationSerial", image.scriptRegistrationSerial);
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
	}).BindSimTime(image.simTimeTicks);
}

CheckpointText RTE::AssembleCheckpointIndex(const CheckpointImage& image) {
	return Writer::Capture([&](Writer& writer) {
		writer.NewPropertyWithValue("ActivityName", image.activityName);
		writer.NewPropertyWithValue("OriginalScenePresetName", image.originalScenePresetName);
	});
}

void RTE::VisitCheckpointSections(const CheckpointImage& image, const std::function<void(const std::string& name, CheckpointScope scope, std::string_view bytes)>& visit) {
	// Timer fields are written relative to the capture's sim time, as the archive binds them. A shared section's values
	// only this machine holds leave it, and its whole text is named beside it as this machine's own.
	const auto part = [&](const std::string& name, CheckpointScope scope, const CheckpointText& value) {
		const CheckpointText bound = value.BindSimTime(image.simTimeTicks);
		if (scope != CheckpointScope::Shared) {
			visit(name, scope, bound.Text());
			return;
		}
		const std::string shared = value.SharedText(image.simTimeTicks);
		visit(name, scope, shared);
		if (shared != bound.Text()) visit(name + ".local", CheckpointScope::PerPeer, bound.Text());
	};
	std::ostringstream header;
	header << "ActivityName " << image.activityName << "\nOriginalScenePresetName " << image.originalScenePresetName
	       << "\nSimUpdateCount " << image.simUpdateCount << "\nSimTimeTicks " << image.simTimeTicks << "\nUniqueIDCounter " << image.uniqueIDCounter
	       << "\nScriptRegistrationSerial " << image.scriptRegistrationSerial << "\nPlaceObjects " << image.placeObjects << "\nPlaceUnits " << image.placeUnits;
	for (const auto& [savedTick, uid]: image.quarantine) header << "\nLockstepJoinQuarantine " << savedTick << "|" << uid;
	visit("header", CheckpointScope::Shared, header.str());
	part("activity", CheckpointScope::Shared, image.activity);
	for (const CheckpointSection& section: image.globalSections) part("globals." + section.name, section.scope, section.text);
	part("structure", CheckpointScope::Shared, image.structure);
	part("scene_runtime", CheckpointScope::Shared, image.sceneRuntime);
	for (size_t i = 0; i < image.graphs.size(); ++i) part("graph." + std::to_string(i), i < image.graphScopes.size() ? image.graphScopes[i] : CheckpointScope::Shared, image.graphs[i]);
	part("scene", CheckpointScope::Shared, image.scene);
	for (const auto& [name, layer]: image.layers) {
		if (layer) visit("layer." + name, CheckpointScope::Shared, layer->PixelBytes());
	}
}

namespace {
	// Word-wise multiply-xor, then a splitmix64 finish: every step is a bijection of the state, so a single changed word always shows.
	uint64_t FullStateHash(std::string_view bytes) {
		uint64_t state = 0xcbf29ce484222325ull ^ bytes.size();
		size_t at = 0;
		for (; at + 8 <= bytes.size(); at += 8) {
			uint64_t word;
			std::memcpy(&word, bytes.data() + at, 8);
			state = (state ^ word) * 0x9e3779b97f4a7c15ull;
			state ^= state >> 32;
		}
		for (; at < bytes.size(); ++at) state = (state ^ static_cast<unsigned char>(bytes[at])) * 0x100000001b3ull;
		uint64_t z = state + 0x9e3779b97f4a7c15ull;
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
		return z ^ (z >> 31);
	}
} // namespace

std::string RTE::FullStateHashLine(const CheckpointImage& image, const std::string& dumpDirectory) {
	std::string sections, combined;
	std::filesystem::path dump;
	if (!dumpDirectory.empty()) {
		dump = std::filesystem::path(dumpDirectory) / std::to_string(image.tick);
		std::filesystem::create_directories(dump);
	}
	VisitCheckpointSections(image, [&](const std::string& name, CheckpointScope scope, std::string_view bytes) {
		const std::string hash = std::format("{:016x}", FullStateHash(bytes));
		// Pixels are hashed only; the text sections are what a reader diffs line by line.
		if (!dump.empty() && !name.starts_with("layer.")) std::ofstream(dump / (name + (scope == CheckpointScope::PerPeer ? ".peer" : "") + ".txt"), std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		// A per-peer section is this machine's own by construction; it is named in the dump, never in the compared line.
		if (scope != CheckpointScope::Shared) return;
		sections += (sections.empty() ? "" : ",") + name + ":" + hash;
		combined += name + "=" + hash + ";";
	});
	return std::format("[fullstate] tick={} hash={:016x} sections={}", image.tick, FullStateHash(combined), sections);
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
	// Only the failing actual goes on the line; the expectation belongs in the row, not in the log.
	const auto fail = [&passed](const char* name, const std::string& actual) {
		std::cout << "[cow-checkpoint-selftest] FAIL " << name << " actual=" << actual << std::endl;
		passed = false;
	};
	const auto pass = [](const char* name, const std::string& detail) {
		std::cout << "[cow-checkpoint-selftest] PASS " << name << " " << detail << std::endl;
	};
	std::list<SceneObject*> actors;
	g_MovableMan.GetAllActors(false, actors);
	if (actors.empty()) {
		fail("image_ignores_writes_during_worker_traversal", "no live actor");
		return false;
	}
	auto* live = dynamic_cast<MovableObject*>(actors.front());
	if (!live) {
		fail("image_ignores_writes_during_worker_traversal", "front actor is not a MovableObject");
		return false;
	}
	{
		const int result = g_LuaMan.GetMasterScriptState().RunScriptString(R"lua(
local vector = Vector(1000000.25, 0)
local roots = { ["62"] = { value = vector } }
local before, problems = _ScriptGraph.serialize(roots)
assert(#problems == 0, table.concat(problems, " | "))
SceneMan:WrapPosition(vector)
assert(vector.X ~= 1000000.25, "the scene did not wrap the argument")
assert(_ScriptGraphDirtyRoots().roots["62"], "the wrapped argument left root 62 clean")
local after = _ScriptGraph.serialize(roots)
assert(before ~= after, "the wrapped value kept its old archive")
)lua");
		if (result == 0) pass("vector_out_argument_dirties_its_root", "WrapPosition marked the argument's root");
		else fail("vector_out_argument_dirties_its_root", "script result " + std::to_string(result));
	}
	// A write to a field the archive carries has to move the object's stamp, or its shadow is served
	// again with the old value. NotResting writes three saved fields and no travel stamps them.
	{
		const uint64_t before = live->CheckpointWriteGeneration();
		live->NotResting();
		const uint64_t after = live->CheckpointWriteGeneration();
		const char* row = "a_saved_field_write_moves_the_stamp";
		if (after == before) {
			fail(row, "generation " + std::to_string(after) + " after NotResting wrote three saved fields");
		} else {
			pass(row, "generation " + std::to_string(before) + " -> " + std::to_string(after));
		}
	}
	// An Atom is archived through its owner and is not an Entity, so a write to one has to reach the
	// owner's stamp or the owner's whole shadow is served again with the old trail.
	{
		Atom borrowed;
		borrowed.SetOwner(live);
		const uint64_t before = live->CheckpointWriteGeneration();
		borrowed.SetTrailLength(borrowed.GetTrailLength() + 1);
		const uint64_t after = live->CheckpointWriteGeneration();
		const char* row = "an_atom_write_moves_its_owner_stamp";
		if (after == before) {
			fail(row, "generation " + std::to_string(after) + " after the trail length moved");
		} else {
			pass(row, "generation " + std::to_string(before) + " -> " + std::to_string(after));
		}
	}
	{
		MOSRotating owner;
		struct RotationProbe : MOSRotating { void Prime() { m_Rotation.m_ElementsUpdated = true; } } rotation;
		rotation.Prime();
		const uint64_t rotationBefore = rotation.CheckpointWriteGeneration();
		rotation.SetRotAngle(rotation.GetRotAngle());
		const uint64_t rotationAfter = rotation.CheckpointWriteGeneration();
		rotation.SetRotAngle(rotation.GetRotAngle());
		if (rotationAfter > rotationBefore && rotation.CheckpointWriteGeneration() == rotationAfter) pass("an_archived_matrix_cache_write_moves_the_stamp", "only the archived cache flag changed");
		else fail("an_archived_matrix_cache_write_moves_the_stamp", "the matrix cache flag was not stamped once");
		SceneObject::SOPlacer placer;
		placer.SetCheckpointOwner(&owner);
		const uint64_t placementBefore = owner.CheckpointWriteGeneration();
		placer.SetOffset(Vector(3, 4));
		const uint64_t placementAfter = owner.CheckpointWriteGeneration();
		placer.SetOffset(Vector(3, 4));
		if (placementAfter > placementBefore && owner.CheckpointWriteGeneration() == placementAfter) pass("a_placement_write_moves_its_owner_stamp", "equal writes kept the stamp");
		else fail("a_placement_write_moves_its_owner_stamp", "the placement did not stamp its owner once");
		AtomGroup group;
		group.SetOwner(&owner);
		const uint64_t before = owner.CheckpointWriteGeneration();
		group.SetStoredMomentOfInertia(7.0F, 3.0F);
		const uint64_t after = owner.CheckpointWriteGeneration();
		group.SetStoredMomentOfInertia(7.0F, 3.0F);
		if (after > before && owner.CheckpointWriteGeneration() == after) pass("an_atomgroup_write_moves_its_owner_stamp", "changed once, equal setter kept the stamp");
		else fail("an_atomgroup_write_moves_its_owner_stamp", "before=" + std::to_string(before) + " after=" + std::to_string(after));
		Gib gib;
		gib.SetCheckpointOwner(&owner);
		const uint64_t gibBefore = owner.CheckpointWriteGeneration();
		gib.SetMinVelocity(3.0F);
		const uint64_t gibAfter = owner.CheckpointWriteGeneration();
		gib.SetMinVelocity(3.0F);
		if (gibAfter > gibBefore && owner.CheckpointWriteGeneration() == gibAfter) pass("a_gib_write_moves_its_owner_stamp", "changed once, equal setter kept the stamp");
		else fail("a_gib_write_moves_its_owner_stamp", "before=" + std::to_string(gibBefore) + " after=" + std::to_string(gibAfter));
		Gib* boundGib = new Gib();
		boundGib->SetCheckpointOwner(&owner);
		owner.GetGibList()->push_back(boundGib);
		auto& state = g_LuaMan.GetMasterScriptState();
		Entity* previous = state.GetTempEntity();
		state.SetTempEntity(&owner);
		const uint64_t boundBefore = owner.CheckpointWriteGeneration();
		const int changed = state.RunScriptString(R"lua(
for gib in ToMOSRotating(LuaMan.TempEntity).Gibs do
    gib.Count = 2; gib.Spread = 0.25; gib.LifeVariation = 0.2
    gib.InheritsVel = 0.3; gib.InheritsAngularVel = 0.4; gib.IgnoresTeamHits = true
    gib.Offset = Vector(3, 4)
    local offset = gib.Offset; offset.X = 9
end
)lua");
		const uint64_t boundAfter = owner.CheckpointWriteGeneration();
		const int same = state.RunScriptString(R"lua(
for gib in ToMOSRotating(LuaMan.TempEntity).Gibs do
    local offset = gib.Offset
    gib.Count = gib.Count; gib.Offset = Vector(9, 4); offset.X = offset.X
    assert(offset.X == 9 and offset.Y == 4, "the Gib offset stopped being a live alias")
end
)lua");
		state.SetTempEntity(previous);
		if (changed == 0 && same == 0 && boundAfter >= boundBefore + 8 && owner.CheckpointWriteGeneration() == boundAfter && boundGib->GetOffset() == Vector(9, 4)) {
			pass("gib_members_and_live_offsets_stamp_the_owner", "seven properties and the held offset changed, equal writes kept the stamp");
		} else {
			fail("gib_members_and_live_offsets_stamp_the_owner", "changed=" + std::to_string(changed) + " same=" + std::to_string(same) + " stamps=" + std::to_string(boundAfter - boundBefore));
		}
	}
	{
		ADoor owner;
		ADSensor sensor;
		sensor.SetCheckpointOwner(&owner);
		const uint64_t before = owner.CheckpointWriteGeneration();
		sensor.SetStartOffset(Vector(4, 5));
		const uint64_t after = owner.CheckpointWriteGeneration();
		sensor.SetStartOffset(Vector(4, 5));
		if (after > before && owner.CheckpointWriteGeneration() == after) pass("a_sensor_write_moves_its_owner_stamp", "equal writes kept the stamp");
		else fail("a_sensor_write_moves_its_owner_stamp", "the sensor did not stamp its door once");
		owner.GetController()->SetControlledActor(nullptr);
		const uint64_t controlBefore = owner.CheckpointWriteGeneration();
		owner.GetController()->SetAnalogMove(Vector(0.25F, 0.5F));
		const uint64_t controlAfter = owner.CheckpointWriteGeneration();
		owner.GetController()->SetAnalogMove(Vector(0.25F, 0.5F));
		if (controlAfter > controlBefore && owner.CheckpointWriteGeneration() == controlAfter) pass("a_controller_write_moves_its_owner_stamp", "the owning actor was stamped with no controlled actor");
		else fail("a_controller_write_moves_its_owner_stamp", "the controller did not stamp its actor");
	}
	{
		ACDropShip owner;
		struct ExitProbe : ACraft::Exit { void Prime() { m_Offset = Vector(1, 2); } } exit;
		exit.SetCheckpointOwner(&owner);
		exit.Prime();
		const uint64_t before = owner.CheckpointWriteGeneration();
		exit.Reset();
		const uint64_t after = owner.CheckpointWriteGeneration();
		exit.Reset();
		if (after > before && owner.CheckpointWriteGeneration() == after) pass("an_exit_reset_moves_its_owner_stamp", "equal resets kept the stamp");
		else fail("an_exit_reset_moves_its_owner_stamp", "the exit did not stamp its craft once");
	}
	{
		SoundContainer owner;
		SoundSet sound;
		sound.SetOwnerContainer(&owner);
		const uint64_t before = owner.CheckpointWriteGeneration();
		sound.SetSoundSelectionCycleModeNow(SoundSet::FORWARDS);
		const uint64_t after = owner.CheckpointWriteGeneration();
		sound.SetSoundSelectionCycleModeNow(SoundSet::FORWARDS);
		if (after > before && owner.CheckpointWriteGeneration() == after) pass("a_soundset_write_moves_its_owner_stamp", "equal writes kept the stamp");
		else fail("a_soundset_write_moves_its_owner_stamp", "the sound set did not stamp its container once");
	}
	{
		live->UpdateScripts();
		const uint64_t before = live->CheckpointWriteGeneration();
		live->UpdateScripts();
		live->UpdateScripts();
		const uint64_t after = live->CheckpointWriteGeneration();
		const char* row = "a_quiet_scripted_update_counter_leaves_the_stamp";
		if (after != before) {
			fail(row, "generation moved " + std::to_string(after - before) + " times over two script updates");
		} else {
			pass(row, "generation " + std::to_string(after) + " held");
		}
	}
	// A capture assigns sound identities and can draw counters; the rows hand the sim back what they took.
	struct BorrowedCounters {
		RandomGenerator sim = g_SimRNG, render = g_RenderRNG;
		long uid = MovableObject::GetUniqueIDCounter();
		CheckpointSoundRegistry sounds = g_AudioMan.CaptureCheckpointSoundRegistry();
		uint64_t soundCursor = g_AudioMan.GetCheckpointSoundContainerCursor();
		std::unordered_set<uint64_t> carried = g_AudioMan.LastCarriedSoundIdentities();
		~BorrowedCounters() {
			g_SimRNG = sim;
			g_RenderRNG = render;
			MovableObject::PinUniqueIDCounter(uid);
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
		fail("image_ignores_writes_during_worker_traversal", "worker text carries the write it raced");
	} else if (formatted.find("PinStrength") == std::string::npos) {
		fail("image_ignores_writes_during_worker_traversal", "no PinStrength in the owned text");
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
		fail("restore_round_trip_matches_synchronous_capture", firstDifference(sync, fresh));
	} else if (fresh != reused) {
		fail("restore_round_trip_matches_synchronous_capture", firstDifference(fresh, reused));
	} else {
		pass("restore_round_trip_matches_synchronous_capture", "sync, fresh and reused captures are the same bytes");
	}

	// The production peek hands back the shadow while the object's own stamp holds, so a field write
	// that forgot TouchCheckpoint would put a stale actor in the archive.
	if (auto* actor = dynamic_cast<Actor*>(live)) {
		cache.Begin();
		const std::string beforeWrite = capture(&cache).Text();
		const uint64_t stampBefore = actor->CheckpointWriteGeneration();
		const float health = actor->GetHealth();
		actor->SetHealth(health - 1.0F);
		const uint64_t stampAfter = actor->CheckpointWriteGeneration();
		cache.Begin();
		const std::string afterWrite = capture(&cache).Text();
		actor->SetHealth(health);
		if (stampAfter == stampBefore || afterWrite == beforeWrite) {
			fail("a_stamped_write_is_not_reused_from_the_shadow",
			     "stamp " + std::to_string(stampBefore) + "->" + std::to_string(stampAfter) +
			         (afterWrite == beforeWrite ? " text unchanged" : " text changed"));
		} else {
			pass("a_stamped_write_is_not_reused_from_the_shadow", "a health write moved the stamp and the captured text");
		}
	} else {
		fail("a_stamped_write_is_not_reused_from_the_shadow", "front object is not an Actor");
	}
	// A capture answers which objects exist from a copy; a change to them while it lives must reach the answers.
	{
		const std::string missed = g_MovableMan.KnownObjectsScopeMissedChange();
		if (missed.empty()) pass("a_known_objects_change_reaches_a_capture_scope", "ways=3");
		else fail("a_known_objects_change_reaches_a_capture_scope", "missed_way=" + missed);
	}
	return passed;
}

bool RTE::RunCheckpointImageSelfTest() {
	if (!TimerMan::IsConstructed()) TimerMan::Construct();
	bool passed = true;
	// Only the failing actual goes on the line; the expectation belongs in the row, not in the log.
	const auto fail = [&passed](const char* name, const std::string& actual) {
		std::cout << "[cow-checkpoint-selftest] FAIL " << name << " actual=" << actual << std::endl;
		passed = false;
	};
	const auto pass = [](const char* name, const std::string& detail) {
		std::cout << "[cow-checkpoint-selftest] PASS " << name << " " << detail << std::endl;
	};
	try {
		// The freeze is meant to hold every Lua thread. A write from one it did not hold is a defect
		// the capture has to report, not a write it may drop; its own thread's scratch still passes.
		{
			int table = 0;
			const uint64_t before = LuaCheckpointPausedWrites();
			uint64_t duringOwn = 0;
			{
				LuaCheckpointBarrierPause pause;
				OnLuaTableWrite(&table);
				duringOwn = LuaCheckpointPausedWrites();
				std::thread foreign([&table] { OnLuaTableWrite(&table); });
				foreign.join();
			}
			const uint64_t after = LuaCheckpointPausedWrites();
			const char* row = "barrier_pause_reports_a_foreign_write";
			if (duringOwn != before || after != before + 1) {
				fail(row, "own=" + std::to_string(duringOwn - before) + " foreign=" + std::to_string(after - duringOwn));
			} else {
				pass(row, "the capture's own write passed, the other thread's was counted");
			}
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
			fail("generational_shadow_keeps_the_freeze_value", later.Text());
		} else {
			pass("generational_shadow_keeps_the_freeze_value", "later shadow holds 9");
		}
		stamp = 9;
		const CheckpointText* peeked = cache.Peek(&stamp, 1);
		if (!peeked || cache.Stamp(&stamp, 1) != 12 || !peeked->SameValues(later)) {
			fail("peek_reuses_the_shadow_when_the_stamp_matches", "peek missed");
		} else {
			pass("peek_reuses_the_shadow_when_the_stamp_matches", "stamp 12");
		}

		// One walk spans every state: a state that rewrote all of its roots must not drop the tables
		// another state kept by reusing its chunk.
		int stateOneTable = 0, stateTwoTable = 0;
		CheckpointGraphIndex& index = CheckpointGraphIndex::Get();
		{
			Controller value;
			CheckpointCow cow;
			int state = 0;
			index.BeginWalk();
			index.BeginRoot(73, &state); index.NoteValue(&value);
			index.EndWalk();
			value.ArmCheckpointValueTrap();
			const auto capture = [&] { return CheckpointWriter::CaptureNative([&] { return value.SaveCheckpoint(); }); };
			const CheckpointText first = capture();
			cow.RememberLua({first}, LuaCheckpointWriteGeneration());
			value.SetAnalogMove(Vector(0.25F, 0.5F));
			const GraphDirt dirt = index.Sample();
			const bool cleanTables = dirt.roots > 0 && dirt.dirtyTables == 0 && !dirt.unknownTable;
			const bool reuse = cow.LuaUnchanged(LuaCheckpointWriteGeneration(), 1) || (cleanTables && cow.HasLua(1));
			const CheckpointText second = reuse ? cow.LastLua().front() : capture();
			if (cleanTables && dirt.dirtyValues == 1 && !reuse && first.Text() != second.Text()) pass("a_native_only_write_prevents_whole_graph_reuse", "the second capture carries the changed controller");
			else fail("a_native_only_write_prevents_whole_graph_reuse", "reused=" + std::to_string(reuse) + " dirty_values=" + std::to_string(dirt.dirtyValues));
		}
		{
			int firstState = 0, secondState = 0, firstGlobal = 0, secondGlobal = 0;
			index.BeginWalk();
			index.BeginRoot(0, &firstState, "global"); index.NoteTable(&firstGlobal);
			index.BeginRoot(0, &secondState, "global"); index.NoteTable(&secondGlobal);
			index.NoteUncacheableRoots(1); index.NoteUncacheableRoots(2);
			index.EndWalk();
			if (!index.CanReuseWhole() && index.Sample().uncacheableRoots == 3) pass("unwatched_roots_prevent_whole_graph_reuse", "three unwatched roots across both states");
			else fail("unwatched_roots_prevent_whole_graph_reuse", "an unwatched root was eligible for whole reuse");
			index.OnTableWritten(&firstGlobal);
			const bool isolated = index.DirtyRoots(&firstState).contains(0) && index.DirtyRoots(&secondState).empty();
			index.BeginWalk();
			index.BeginRoot(0, &firstState, "global"); index.NoteTable(&firstGlobal);
			index.ReuseRoot(0, &secondState, "global");
			index.EndWalk();
			index.OnTableWritten(&secondGlobal);
			const bool retained = index.DirtyRoots(&secondState).contains(0) && index.DirtyRoots(&firstState).empty();
			if (isolated && retained) pass("globals_roots_are_separate_in_each_state", "both writes reached their own state");
			else fail("globals_roots_are_separate_in_each_state", "isolated=" + std::to_string(isolated) + " retained=" + std::to_string(retained));
			index.BeginWalk();
			index.ReuseRoot(0, &firstState, "global");
			index.EndWalk();
			index.OnTableWritten(&secondGlobal);
			if (index.Sample().roots == 1 && index.DirtyRoots(&secondState).empty()) pass("removed_roots_release_their_recorded_tables", "only the retained state remains indexed");
			else fail("removed_roots_release_their_recorded_tables", "a removed root survived a partial walk");
		}
		index.BeginWalk();
		index.BeginRoot(11); index.NoteTable(&stateOneTable); index.NoteRootReuse(0, 1);
		index.BeginRoot(21); index.NoteTable(&stateTwoTable); index.NoteRootReuse(0, 1);
		index.EndWalk();
		index.BeginWalk();
		index.ReuseRoot(11, nullptr, "");
		index.NoteRootReuse(1, 0);
		index.BeginRoot(21); index.NoteTable(&stateTwoTable); index.NoteRootReuse(0, 1);
		index.EndWalk();
		const GraphDirt spanned = index.Sample();
		index.OnTableWritten(&stateOneTable);
		const bool reusedRootKept = !index.UnknownTableWritten() && index.DirtyRoots().count(11) == 1;
		if (!reusedRootKept || spanned.rootsReused != 1 || spanned.rootsRewritten != 1) {
			fail("one_walk_keeps_every_state_reused_root",
			     "unknown=" + std::to_string(index.UnknownTableWritten()) + " dirty11=" + std::to_string(index.DirtyRoots().count(11)) +
			         " reused=" + std::to_string(spanned.rootsReused) + " rewritten=" + std::to_string(spanned.rootsRewritten));
		} else {
			pass("one_walk_keeps_every_state_reused_root", "reused 1 rewritten 1");
		}

		// A write to a table no walk recorded raises the unknown flag, which turns the root cache off
		// wholesale. The walk that follows answers it, so the flag must not survive that walk -
		// whether it reused a chunk or not. It used to survive a partial one, disabling the cache for
		// the rest of the process the first time reuse ever succeeded.
		{
			int stranger = 0;
			index.OnTableWritten(&stranger);
			const bool raised = index.UnknownTableWritten();
			index.BeginWalk();
			index.ReuseRoot(11, nullptr, "");
			index.NoteRootReuse(1, 0);
			index.BeginRoot(21); index.NoteTable(&stateTwoTable); index.NoteRootReuse(0, 1);
			index.EndWalk();
			const char* row = "a_partial_walk_answers_the_unknown_table";
			if (!raised || index.UnknownTableWritten()) {
				fail(row, "raised=" + std::to_string(raised) + " still_unknown=" + std::to_string(index.UnknownTableWritten()));
			} else {
				pass(row, "the flag was raised and the walk that followed cleared it");
			}
		}

		// Pages that go quiet at different freezes each hold the copy buffer they were copied into. A heap
		// keeps a bounded set of those buffers mapped, and its last snapshot still reads as the heap it froze.
		{
			const CopyBufferProbe probe = ProbeCheckpointCopyBuffers();
			const char* row = "settled_pages_keep_a_bounded_set_of_copy_buffers";
			const std::string detail = "most_live=" + std::to_string(probe.mostLive) + " bound=" + std::to_string(probe.bound) +
			                           " freezes=" + std::to_string(probe.freezes) + " pages_match=" + std::to_string(probe.pagesMatch);
			if (!probe.error.empty()) {
				fail(row, detail + " error=" + probe.error);
			} else if (probe.mostLive > probe.bound || !probe.pagesMatch) {
				fail(row, detail);
			} else {
				pass(row, detail);
			}
		}

		// A checkpoint image can outlive the Lua state it froze (the last image at shutdown). Releasing it
		// afterwards must not reach into the destroyed heap, and its copy buffers must still be unmapped.
		{
			const OrphanSnapshotProbe probe = ProbeSnapshotAfterItsHeap();
			const char* row = "a_snapshot_released_after_its_heap_never_calls_into_it";
			const std::string detail = "dead_heap_calls=" + std::to_string(probe.deadHeapCalls) + " mapped_after=" + std::to_string(probe.mappedAfter) +
			                           " pages_read=" + std::to_string(probe.pagesRead);
			if (!probe.error.empty()) {
				fail(row, detail + " error=" + probe.error);
			} else if (probe.deadHeapCalls != 0 || probe.mappedAfter != 0 || !probe.pagesRead) {
				fail(row, detail);
			} else {
				pass(row, detail);
			}
		}

		// luaL_ref and luabind both take registry slots on a luabind state. A slot one of them holds is never
		// handed to the other, whichever took its ref first.
		{
			const RegistryRefProbe probe = ProbeRegistryRefs();
			const char* row = "a_registry_ref_is_never_handed_to_a_second_owner";
			const std::string detail = "held_ref=" + std::to_string(probe.heldRef) + " shared=" + std::to_string(probe.shared) +
			                           " held_intact=" + std::to_string(probe.heldIntact) + " luabind_intact=" + std::to_string(probe.luabindIntact);
			if (!probe.error.empty()) {
				fail(row, detail + " error=" + probe.error);
			} else if (probe.shared != 0 || !probe.heldIntact || !probe.luabindIntact) {
				fail(row, detail);
			} else {
				pass(row, detail);
			}
		}
		// A registry scope that saves the registry only when something changes it must leave what an eager copy would.
		{
			if (!AudioMan::IsConstructed()) AudioMan::Construct();
			const std::string missed = g_AudioMan.RegistryScopeMissedChange();
			if (missed.empty()) pass("a_registry_scope_puts_back_what_any_change_moved", "ways=5");
			else fail("a_registry_scope_puts_back_what_any_change_moved", "missed_way=" + missed);
		}
		// A capture's bitmap index is kept while the loaded bitmaps' version holds, so every way that changes them moves it.
		{
			const std::string missed = ContentFile::LoadedBitmapChangeMissedByIndex();
			if (missed.empty()) pass("a_loaded_bitmap_change_by_any_way_reaches_the_next_index", "ways=12");
			else fail("a_loaded_bitmap_change_by_any_way_reaches_the_next_index", "missed_way=" + missed);
		}
		// A preview's page fence puts back exactly the bytes written under it, edges included, and nothing after it lifts.
		{
			if (!PageWriteFence::IsSupported()) {
				pass("a_page_fence_puts_back_exactly_what_was_written", "platform=copies");
			} else if (const std::string mismatch = PageWriteFence::SelfTestMismatch(); mismatch.empty()) {
				pass("a_page_fence_puts_back_exactly_what_was_written", "rounds=2");
			} else {
				fail("a_page_fence_puts_back_exactly_what_was_written", mismatch);
			}
		}
	} catch (const std::exception& error) {
		fail("no_unexpected_exception", error.what());
	}
	std::cout << "[cow-checkpoint-selftest] " << (passed ? "PASS" : "FAIL") << std::endl;
	return passed;
}
