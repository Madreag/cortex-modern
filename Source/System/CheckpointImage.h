#pragma once

#include "Writer.h"
#include "SceneLayer.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace RTE {

	/// What a script graph walk saw and what has been written since it.
	struct GraphDirt {
		size_t roots = 0;        //!< Roots the last walk recorded.
		size_t tables = 0;       //!< Tables the last walk recorded.
		size_t dirtyRoots = 0;   //!< Roots holding a table written since that walk.
		size_t dirtyTables = 0;  //!< Tables written since that walk.
		bool unknownTable = false;  //!< A table no walk has seen was written.
		int64_t noteUs = 0;      //!< Sim-thread microseconds the walk spent recording tables.
		size_t rootsReused = 0;     //!< Roots whose chunk the last capture reused byte for byte.
		size_t rootsRewritten = 0;  //!< Roots the last capture serialized again.
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
		std::vector<CheckpointText> graphs;
		std::string activityName;
		std::string originalScenePresetName;
		int64_t simUpdateCount = 0;
		int64_t simTimeTicks = 0;
		long uniqueIDCounter = 0;
		int luaStateCursor = 0;
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
		int64_t structureUs = 0;
		int64_t sceneRuntimeUs = 0;
		int64_t globalsUs = 0;
		int64_t layersUs = 0;
		GraphDirt graph;
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

		void FinishImage(std::shared_ptr<CheckpointImage> image);
		void RecordWorker(int64_t workerUs);
		void RecordGraphText(int64_t graphTextUs);
		void PublishLog(uint64_t tick) const;
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


	/// The table-to-root index a script graph walk fills and the write barrier marks.
	class CheckpointGraphIndex {
	public:
		static CheckpointGraphIndex& Get();

		void BeginWalk(bool full = true);
		void BeginRoot(uint64_t root);
		void NoteTable(const void* table);
		void EndWalk();
		void OnTableWritten(const void* table);
		void NoteRootReuse(size_t reused, size_t rewritten);
		/// The roots holding a table written since the walk that recorded them.
		std::unordered_set<uint64_t> DirtyRoots() const;
		bool UnknownTableWritten() const;
		bool HasWalked() const;
		GraphDirt Sample() const;

	private:
		mutable std::mutex m_Mutex;
		std::unordered_map<const void*, uint64_t> m_TableRoots;
		std::unordered_set<uint64_t> m_Roots;
		std::unordered_set<uint64_t> m_DirtyRoots;
		std::unordered_map<const void*, uint64_t> m_Walking;
		std::unordered_set<uint64_t> m_WalkingRoots;
		size_t m_DirtyTables = 0;
		bool m_UnknownTable = false;
		bool m_Walk = false;
		bool m_FullWalk = true;
		size_t m_RootsReused = 0;
		size_t m_RootsRewritten = 0;
		uint64_t m_Root = 0;
		int64_t m_NoteUs = 0;
		int64_t m_WalkNoteUs = 0;
	};

	void ArmLuaCheckpointBarrier();
	uint64_t LuaCheckpointWriteGeneration();

	bool RunCheckpointImageSelfTest();

	/// Runs the checkpoint rows that need a live scene; call from a running match.
	bool RunCheckpointSceneRows();
} // namespace RTE
