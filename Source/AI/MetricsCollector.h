#pragma once

#include "SimChecksum.h"
#include "Singleton.h"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

#define g_MetricsCollector MetricsCollector::Instance()

namespace RTE {
	class MovableObject;
	bool CrossCaptureBarrier(const nlohmann::json& spec, const std::string& directory, const std::string& phase, uint64_t tick, uint64_t round);
	void CrossCaptureBarrierFromEnvironment(const char* phase, uint64_t tick, uint64_t round);

	/// Metrics collector for the determinism scenario runner.
	///
	/// Tracks per-scenario state, the per-tick hash trace, and arbitrary key=value metric
	/// records from Lua scenario scripts via `metrics.record(name, value)` / `metrics.set_result(passed)`.
	///
	/// Emits a single JSON file at end-of-scenario (or one aggregated JSON over K runs).
	class MetricsCollector : public Singleton<MetricsCollector> {
		friend class Singleton<MetricsCollector>;

	public:
		MetricsCollector();
		~MetricsCollector();

		void Initialize() {}
		void Destroy();

		/// Begin a new run. Resets per-run state. `scenario` and `seed` are recorded.
		void BeginRun(const std::string& scenario, uint64_t seed);

		/// Begin a run owned by the CLI trace path that armed it. A scenario script's own BeginRun joins
		/// this run instead of replacing it, so the armed tick-hash trace survives the activity's start.
		void BeginHostRun(const std::string& scenario, uint64_t seed);

		/// End the current run. Records the final pass/fail + summary.
		void EndRun();

		/// Mark the current run as passed or failed.
		void SetResult(bool passed) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Result.passed = passed;
			m_Result.resultSet = true;
		}

		/// Records how the engine's own run ended: whether it completed, why, and the last tick it simulated. A verdict the scenario set
		/// stands; the completion is kept beside it, and a scenario's own final_tick wins.
		void SetNativeOutcome(bool completed, const std::string& reason, uint64_t finalTick);
		/// Whether anything judged the current run yet.
		bool HasResult() const {
			std::lock_guard<std::mutex> lock(m_Mutex);
			return m_Result.resultSet;
		}

		/// Whether a numeric metric of that name was recorded this run.
		bool HasNumeric(const std::string& name) const {
			std::lock_guard<std::mutex> lock(m_Mutex);
			return m_Numeric.contains(name);
		}

		/// Record an arbitrary numeric metric. Called from scenario Lua via `metrics.record(name, value)`.
		void Record(const std::string& name, double value);

		/// Record a string-valued metric (e.g. a status code).
		void RecordString(const std::string& name, const std::string& value);

		/// Streams opt-in observations at committed tick boundaries.
		bool OpenEvents(const std::string& path, size_t byteLimit = 268435456);
		void BeginEventTick(const nlohmann::json& context, bool prediction = false);
		void UpdateEventContext(const nlohmann::json& context);
		void RecordEvent(const std::string& event, const MovableObject* object = nullptr, const std::string& result = "success", double amount = 1, long other = 0, int seat = -1);
		void AppendEvent(const nlohmann::json& event);
		void FlushEventTick();
		void WriteObservation(const nlohmann::json& observation);
		void RecordProducedController(uint64_t round, uint64_t producedTick, uint64_t targetTick, long actor, int seat, double producedWallMs = -1);
		void ConfirmProducedControllers(uint64_t round, uint64_t producedTick, uint64_t targetTick, const std::vector<long>& queuedActors, uint64_t priorInputThrough);
		nlohmann::json ProducedControllerFor(uint64_t round, uint64_t targetTick, long actor) const;
		static bool IsFreshControllerRecovery(const nlohmann::json& sample, uint64_t round, uint64_t tick, long actor,
		    int64_t wireTick, bool controllable, bool held, bool catchup, double afterWallMs);
		void CloseEvents();
		bool EventsEnabled() const { return m_EventsEnabled.load(std::memory_order_relaxed); }
		size_t EventBytes() const;
		size_t InstrumentationBytes() const;
		static nlohmann::json TickTiming(long long totalUs, long long waitUs, long long captureUs, long long captureWaitUs);
		/// Writes past a small byte budget into a scratch directory and checks that the oldest parts retire while recording goes on.
		static bool RunEventRotationSelfTest(const std::string& directory, std::string* error);

		/// Per-tick hash trace recording for the determinism CI check.
		///
		/// When enabled, every call to `RecordTickHash` appends the tick number, the
		/// `total` hash, and each subsystem hash to the run's trace file. The trace is then
		/// emitted in the JSON report under `tick_hashes` (hex, sorted by subsystem name).
		/// `cccp-determinism-check` parses those traces from N runs and diffs them tick-by-tick
		/// to surface non-determinism. Disabled by default — only the ScenarioRunner `-tick-hashes`
		/// flag (or an explicit call to `SetRecordTickHashes(true)`) turns it on so normal runs
		/// keep the JSON report small.
		void SetRecordTickHashes(bool enabled) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_RecordTickHashes = enabled;
		}

		/// Whether the per-tick hash trace is being recorded (a determinism run). Set once at run start.
		bool IsRecordingTickHashes() const { return m_RecordTickHashes; }

		/// Append a per-tick hash record. Silently no-op if recording is disabled or no run is
		/// active. The intended call site is `Main.cpp`'s sim-loop right after
		/// `g_SimChecksum.EndTick()` — that's when the per-subsystem accumulators are finalized
		/// for the tick.
		void RecordTickHash(const SimChecksum::Result& result, bool paused = false);

		/// Number of tick-hash records captured this run. For tests and CLI diagnostics.
		/// Drops the records of the ticks from this one on: a held seat ran them off the round before it learned of its hold.
		void RetractTickHashesFrom(uint64_t tick) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (TraceOnFileLocked()) RetractTraceLocked(tick);
			else std::erase_if(m_TickHashes, [tick](const TickHashRecord& record) { return record.tick >= tick; });
		}

		size_t GetTickHashCount() const {
			std::lock_guard<std::mutex> lock(m_Mutex);
			return TraceOnFileLocked() ? static_cast<size_t>(m_Trace.count) : m_TickHashes.size();
		}

		/// True if a run is currently active (between BeginRun and EndRun).
		bool IsRunActive() const {
			std::lock_guard<std::mutex> lock(m_Mutex);
			return !m_Scenario.empty();
		}

		/// True while a CLI trace run owns the collector; a scenario script joins that run rather than
		/// beginning or ending one of its own.
		bool IsHostRunActive() const {
			std::lock_guard<std::mutex> lock(m_Mutex);
			return m_HostRun;
		}

		/// Get the number of ticks recorded.
		uint64_t GetTickCount() const {
			std::lock_guard<std::mutex> lock(m_Mutex);
			return m_TickCount;
		}

		/// Write the run report as a JSON file at the given path. Returns true on success.
		bool WriteReport(const std::string& path) const;

		/// One per-tick hash record, kept as the raw hashes: each subsystem is named by its index in the run's name table and
		/// written out by name, sorted, at the report.
		struct TickHashRecord {
			uint64_t                                            tick = 0;
			bool                                                paused = false;
			SimChecksum::Hash                                   total{};
			std::vector<std::pair<uint16_t, SimChecksum::Hash>> subsystems;
		};

		/// Convenience: write a multi-run aggregated report.
		struct AggregatedRun {
			std::string                                  scenario;
			uint64_t                                     seed = 0;
			bool                                         passed = false;
			uint64_t                                     ticks = 0;
			std::unordered_map<std::string, double>      numeric;
			std::unordered_map<std::string, std::string> stringValues;
			std::string                                  finalTotalHashHex;
			std::vector<TickHashRecord>                  tickHashes; //!< Only when asked for: the report writes the collector's own.
			size_t                                       tickHashCount = 0;
			std::vector<std::string>                     subsystemNames; //!< The names tickHashes' indices read.
			std::map<std::string, std::string>           simConfig;
		};
		static bool WriteAggregatedReport(const std::string& path,
		                                  const std::vector<AggregatedRun>& runs,
		                                  const std::string& suiteVersion);

		/// Snapshot of the current run's results; the tick-hash trace is copied only when asked for.
		AggregatedRun GetCurrentRun(bool withTickHashes = false) const;

	private:
		struct EventStream;
		std::unique_ptr<EventStream> m_EventStream;
		std::atomic<bool> m_EventsEnabled{false};
		mutable std::mutex                            m_Mutex;
		std::string                                   m_Scenario;
		uint64_t                                      m_Seed = 0;
		uint64_t                                      m_TickCount = 0;
		std::chrono::steady_clock::time_point         m_StartWall;

		struct Result {
			bool passed = false;
			bool resultSet = false;
		};
		Result                                        m_Result;
		std::unordered_map<std::string, double>       m_Numeric;
		std::unordered_map<std::string, std::string>  m_Strings;
		std::string                                   m_FinalTotalHashHex;

		// Whether the CLI trace path owns this run. See BeginHostRun above.
		bool                                          m_HostRun = false;

		// Per-tick hash trace. See SetRecordTickHashes/RecordTickHash above.
		bool                                          m_RecordTickHashes = false;
		std::vector<TickHashRecord>                   m_TickHashes; //!< Only when the trace's file could not be opened.
		std::vector<std::string>                      m_SubsystemNames;
		std::unordered_map<std::string, uint16_t>     m_SubsystemIndex;

		/// The run's trace, written to a file as it is recorded: memory keeps the newest records' places for a retraction and
		/// one place in every c_TraceSparseEvery for a deeper one, so a long run's trace costs memory it does not grow.
		struct TraceFile {
			std::string path;
			std::unique_ptr<std::fstream> stream;
			uint64_t bytes = 0;
			uint64_t count = 0;
			std::deque<std::pair<uint64_t, uint64_t>> tail; //!< The newest records' ticks and offsets.
			std::vector<std::pair<uint64_t, uint64_t>> sparse; //!< The first record of every c_TraceSparseEvery: its tick and offset.
			bool reposition = false; //!< A retraction moved the end back; the next record is written there.
			bool failed = false;
		};
		static constexpr size_t c_TraceTail = 4096;
		static constexpr uint64_t c_TraceSparseEvery = 4096;
		TraceFile m_Trace;

		bool TraceOnFileLocked() const { return m_Trace.stream != nullptr; }
		void OpenTraceLocked();
		void CloseTraceLocked();
		void AppendTraceLocked(const TickHashRecord& record);
		/// Reads every record the file holds in order, one at a time.
		void ForEachTraceRecordLocked(const std::function<void(const TickHashRecord&)>& visit) const;
		void RetractTraceLocked(uint64_t tick);

		AggregatedRun CurrentRunLocked(bool withTickHashes) const;
		/// Writes the report, streaming each run's trace; the first run's may be the collector's own, read in place.
		static bool WriteRuns(const std::string& path, const std::vector<AggregatedRun>& runs, const std::string& suiteVersion,
		                      const std::function<void(const std::function<void(const TickHashRecord&)>&)>* firstRunTrace, uint64_t firstRunCount,
		                      const std::vector<std::string>* firstRunNames);
		std::map<std::string, std::string>            m_SimConfig;
	};

} // namespace RTE
