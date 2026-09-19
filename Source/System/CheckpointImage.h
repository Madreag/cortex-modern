#pragma once

#include "Writer.h"
#include "SceneLayer.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace RTE {

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
		bool LuaUnchanged(uint64_t writeGeneration) const;
		std::vector<CheckpointText> LastLua() const {
			std::lock_guard lock(m_Mutex);
			return m_LuaGraphs;
		}

		void FinishImage(std::shared_ptr<CheckpointImage> image);
		void RecordWorker(int64_t workerUs);
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
		int64_t m_LastFreezeUs = 0;
		int64_t m_LastWorkerUs = 0;
		size_t m_LastImageBytes = 0;
		double m_LastDirtyRatio = 0;
	};

	CheckpointText AssembleCheckpointSave(const CheckpointImage& image);
	CheckpointText AssembleCheckpointIndex(const CheckpointImage& image);

	void ArmLuaCheckpointBarrier();
	uint64_t LuaCheckpointWriteGeneration();

	bool RunCheckpointImageSelfTest();
} // namespace RTE
