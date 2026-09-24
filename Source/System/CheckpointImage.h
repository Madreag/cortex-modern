#pragma once

#include "Writer.h"
#include "SceneLayer.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace RTE {

	struct GraphWalkPart {
		size_t state = 0;
		std::string part;
		uint64_t root = 0;
		int64_t elapsedUs = 0;
		bool reused = false;
		std::string unwatched;
	};

	/// What a script graph walk saw and what has been written since it.
	struct GraphDirt {
		size_t roots = 0;        //!< Roots the last walk recorded.
		size_t tables = 0;       //!< Tables the last walk recorded.
		size_t dirtyRoots = 0;   //!< Roots holding a table written since that walk.
		size_t dirtyTables = 0;  //!< Tables written since that walk.
		size_t values = 0;       //!< Script-owned native values the last walk wrote into a chunk.
		size_t dirtyValues = 0;  //!< Recorded values mutated since that walk.
		size_t uncacheableRoots = 0;  //!< Roots the last walk barred from reuse, having reached state no barrier watches.
		bool unknownTable = false;  //!< A table no walk has seen was written.
		int64_t noteUs = 0;      //!< Sim-thread microseconds the walk spent recording tables.
		size_t rootsReused = 0;     //!< Roots whose chunk the last capture reused byte for byte.
		size_t rootsRewritten = 0;  //!< Roots the last capture serialized again.
		std::vector<GraphWalkPart> walkParts;
	};

	/// Whether a captured part is the world every peer of a match holds alike, or this machine's own view of it.
	enum class CheckpointScope : uint8_t { Shared, PerPeer };

	/// One named part of a capture's runtime globals, with the scope its writer gives it.
	struct CheckpointSection {
		std::string name;
		CheckpointScope scope = CheckpointScope::Shared;
		CheckpointText text;
	};

	/// Frozen checkpoint values at one sim tick. The worker formats this image.
	struct CheckpointImage {
		uint64_t tick = 0;
		uint64_t generation = 0;
		CheckpointText activity;
		CheckpointText scene;
		CheckpointText structure;
		CheckpointText sceneRuntime;
		CheckpointText globals;
		std::vector<CheckpointSection> globalSections; //!< The runtime globals' parts in the archive's order; filled only when asked for.
		std::vector<CheckpointText> graphs;
		std::string activityName;
		std::string originalScenePresetName;
		int64_t simUpdateCount = 0;
		int64_t simTimeTicks = 0;
		long uniqueIDCounter = 0;
		long scriptRegistrationSerial = 0;
		std::vector<std::pair<uint64_t, long int>> quarantine;
		bool placeObjects = false;
		bool placeUnits = false;
		std::vector<std::pair<std::string, std::shared_ptr<const BitmapSnapshot>>> layers;
		size_t imageBytes = 0;
		size_t dirtyBytes = 0;
		double dirtyRatio = 0;
		int64_t freezeUs = 0;
		int64_t activityUs = 0;
		int64_t graphUs = 0;
		int64_t graphWalkUs = 0;
		int64_t graphTextUs = 0;
		uint64_t graphSerial = 0;
		size_t graphRootsReused = 0;
		size_t graphRootsRewritten = 0;
		int64_t sceneUs = 0;
		int64_t movableUs = 0;
		int64_t structureUs = 0;
		int64_t sceneRuntimeUs = 0;
		int64_t globalsUs = 0;
		std::vector<std::pair<std::string, int64_t>> globalParts;
		int64_t layersUs = 0;
		GraphDirt graph;
		//! The dirt as it stood BEFORE the walk, which is what decided whether the root cache
		//! engaged; EndWalk clears those counters, so the sample above always reads them as zero.
		GraphDirt graphBeforeWalk;
		bool luaReused = false;
		size_t objectsReused = 0;
		size_t objectsCaptured = 0;
	};

	/// Generational copy-on-write store for autosave images.
	class CheckpointCow {
	public:
		static CheckpointCow& Get();

		CheckpointCache& Cache() { return m_Cache; }
		const CheckpointCache& Cache() const { return m_Cache; }
		std::shared_ptr<const CheckpointImage> Last() const {
			std::lock_guard lock(m_Mutex);
			return m_Last;
		}

		void BeginImage();
		void RememberLua(std::vector<CheckpointText> graphs, uint64_t writeGeneration);
		bool LuaUnchanged(uint64_t writeGeneration, size_t stateCount) const;
		bool HasLua(size_t stateCount) const;
		std::vector<CheckpointText> LastLua() const {
			std::lock_guard lock(m_Mutex);
			return m_LuaGraphs;
		}

		std::shared_ptr<const CheckpointImage> FinishImage(std::shared_ptr<CheckpointImage> image);
		void RecordWorker(int64_t workerUs);
		void RecordGraphText(int64_t graphTextUs);
		void PublishLog(const CheckpointImage& image, int64_t workerUs) const;
		void WriteMetricsJson(const std::string& path) const;

		int64_t LastFreezeUs() const { std::lock_guard lock(m_Mutex); return m_LastFreezeUs; }
		int64_t LastWorkerUs() const { std::lock_guard lock(m_Mutex); return m_LastWorkerUs; }
		int64_t P99FreezeUs() const;
		double LastDirtyRatio() const { std::lock_guard lock(m_Mutex); return m_LastDirtyRatio; }
		size_t LastImageBytes() const { std::lock_guard lock(m_Mutex); return m_LastImageBytes; }

	private:
		CheckpointCache m_Cache;
		mutable std::mutex m_Mutex;
		std::shared_ptr<const CheckpointImage> m_Last;
		std::vector<CheckpointText> m_LuaGraphs;
		uint64_t m_LuaWriteGeneration = 0;
		std::vector<int64_t> m_FreezeSamples;
		std::array<int64_t, 7> m_LastRecords{};
		GraphDirt m_LastGraph;
		GraphDirt m_LastGraphBeforeWalk;
		size_t m_LastReused = 0;
		size_t m_LastCaptured = 0;
		bool m_LastLuaReused = false;
		int64_t m_LastFreezeUs = 0;
		int64_t m_LastWorkerUs = 0;
		int64_t m_LastGraphTextUs = 0;
		uint64_t m_LastGraphSerial = 0;
		size_t m_LastRootsReused = 0;
		size_t m_LastRootsRewritten = 0;
		size_t m_LastImageBytes = 0;
		double m_LastDirtyRatio = 0;
	};

	CheckpointText AssembleCheckpointSave(const CheckpointImage& image);
	CheckpointText AssembleCheckpointIndex(const CheckpointImage& image);

	/// Visits every part of a capture that carries its globals' sections, in the archive's order, with the part's scope.
	/// Formats deferred text, so it belongs off the simulation thread.
	void VisitCheckpointSections(const CheckpointImage& image, const std::function<void(const std::string& name, CheckpointScope scope, std::string_view bytes)>& visit);

	/// The full-state oracle's line for a capture: a hash per shared section and one over them all. With a dump
	/// directory, every section's bytes are written there too.
	std::string FullStateHashLine(const CheckpointImage& image, const std::string& dumpDirectory = {});


	/// The table-to-root index a script graph walk fills and the write barrier marks.
	class CheckpointGraphIndex {
	public:
		static CheckpointGraphIndex& Get();

		/// A world walk covers every state and retires the roots of states it no longer sees; a walk of one state leaves the others' entries and dirt in place.
		void BeginWalk(bool full = true, bool world = true);
		void BeginRoot(uint64_t root, const void* state = nullptr, std::string part = {});
		void ReuseRoot(uint64_t root, const void* state, const std::string& part);
		void RestartStateWalk(const void* state);
		void NoteTable(const void* table);
		/// Records a script-owned native whose engine values this root's chunk carries as text.
		void NoteValue(const void* value);
		void NoteUncacheableRoots(size_t roots);
		void EndWalk();
		void OnTableWritten(const void* table);
		/// Marks the root whose chunk carries this native's values; a native no walk recorded is not ours.
		void OnValueWritten(const void* value);
		void NoteRootReuse(size_t reused, size_t rewritten);
		void NoteWalkPart(const void* state, std::string part, uint64_t root, int64_t elapsedUs, bool reused, std::string unwatched);
		/// The roots holding a table written since the walk that recorded them.
		std::unordered_set<uint64_t> DirtyRoots(const void* state = nullptr) const;
		std::unordered_set<std::string> DirtyParts(const void* state, uint64_t root) const;
		bool UnknownTableWritten() const;
		bool HasWalked() const;
		bool CanReuseWhole() const;
		GraphDirt Sample() const;

	private:
		struct Root {
			const void* state = nullptr;
			uint64_t id = 0;
			std::string part;
			bool operator==(const Root&) const = default;
		};
		struct RootHash {
			size_t operator()(const Root& root) const {
				return std::hash<const void*>{}(root.state) ^ std::hash<uint64_t>{}(root.id) ^ std::hash<std::string>{}(root.part);
			}
		};
		using Roots = std::unordered_set<Root, RootHash>;
		using Tables = std::unordered_map<const void*, std::vector<Root>>;
		mutable std::mutex m_Mutex;
		Tables m_TableRoots;
		std::unordered_map<const void*, Root> m_ValueRoots;
		Roots m_Roots;
		Roots m_DirtyRoots;
		Tables m_Walking;
		std::unordered_map<const void*, Root> m_WalkingValues;
		Roots m_WalkingRoots;
		Roots m_ReusedRoots;
		size_t m_DirtyTables = 0;
		size_t m_DirtyValues = 0;
		size_t m_UncacheableRoots = 0;
		bool m_UnknownTable = false;
		bool m_Walk = false;
		bool m_FullWalk = true;
		bool m_WorldWalk = true;
		int m_WalkDepth = 0;
		size_t m_RootsReused = 0;
		size_t m_RootsRewritten = 0;
		Root m_Root;
		int64_t m_NoteUs = 0;
		int64_t m_WalkNoteUs = 0;
		std::vector<const void*> m_WalkStates;
		std::vector<GraphWalkPart> m_WalkParts;
	};

	void ArmLuaCheckpointBarrier();
	/// Arms the native-value half of the barrier alone, for a harness that cannot afford the table half.
	void ArmLuaCheckpointValueBarrier();
	uint64_t LuaCheckpointWriteGeneration();

	/// Reports a write to a script-owned native whose values a cached chunk carries. The engine calls
	/// this where it writes such an object behind Lua's back; Lua's own writes come through luabind.
	void CheckpointValueWritten(const void* value);

	/// How many table writes arrived from a thread the capture's freeze did not hold. The freeze is
	/// meant to quiesce every Lua thread, so a non-zero count is a defect, not a tolerance.
	uint64_t LuaCheckpointPausedWrites();

	/// Holds the table write barrier off for a capture, so its scratch tables are never armed.
	class LuaCheckpointBarrierPause {
	public:
		LuaCheckpointBarrierPause();
		~LuaCheckpointBarrierPause();
		LuaCheckpointBarrierPause(const LuaCheckpointBarrierPause&) = delete;
		LuaCheckpointBarrierPause& operator=(const LuaCheckpointBarrierPause&) = delete;
	};

	/// Keeps a VM that holds no gameplay state, the checkpoint worker's, out of the barrier's counts.
	class LuaCheckpointBarrierIgnore {
	public:
		LuaCheckpointBarrierIgnore();
		~LuaCheckpointBarrierIgnore();
		LuaCheckpointBarrierIgnore(const LuaCheckpointBarrierIgnore&) = delete;
		LuaCheckpointBarrierIgnore& operator=(const LuaCheckpointBarrierIgnore&) = delete;
	};

	bool RunCheckpointImageSelfTest();

	/// Runs the checkpoint rows that need a live scene; call from a running match.
	bool RunCheckpointSceneRows();
} // namespace RTE
