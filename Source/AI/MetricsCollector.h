#pragma once

#include "SimChecksum.h"
#include "Singleton.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define g_MetricsCollector MetricsCollector::Instance()

namespace RTE {

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

		/// Per-tick hash trace recording for the determinism CI check.
		///
		/// When enabled, every call to `RecordTickHash` appends the tick number, the
		/// `total` hash (hex), and each subsystem hash (hex, sorted by subsystem name) into
		/// `m_TickHashes`. The trace is then emitted in the JSON report under `tick_hashes`.
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
			std::erase_if(m_TickHashes, [tick](const TickHashRecord& record) { return record.tick >= tick; });
		}

		size_t GetTickHashCount() const {
			std::lock_guard<std::mutex> lock(m_Mutex);
			return m_TickHashes.size();
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

		/// One per-tick hash record, kept as the raw hashes: a whole match's trace stays in memory until the report, so
		/// each subsystem is named by its index in the run's name table and written out by name, sorted, at the report.
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
		std::vector<TickHashRecord>                   m_TickHashes;
		std::vector<std::string>                      m_SubsystemNames;
		std::unordered_map<std::string, uint16_t>     m_SubsystemIndex;

		AggregatedRun CurrentRunLocked(bool withTickHashes) const;
		/// Writes the report, streaming each run's trace; the first run's may be the collector's own, read in place.
		static bool WriteRuns(const std::string& path, const std::vector<AggregatedRun>& runs, const std::string& suiteVersion,
		                      const std::vector<TickHashRecord>* firstRunHashes, const std::vector<std::string>* firstRunNames);
		std::map<std::string, std::string>            m_SimConfig;
	};

} // namespace RTE
