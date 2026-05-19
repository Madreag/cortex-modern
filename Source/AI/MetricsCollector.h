#pragma once

#include "AIDecisionChannel.h"
#include "Singleton.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define g_MetricsCollector MetricsCollector::Instance()

namespace RTE {

	/// Metrics collector for headless trust-suite scenario runs.
	///
	/// Tracks per-scenario state, aggregates decision events from the AIDecisionChannel into
	/// per-actor / per-type histograms, and accepts arbitrary key=value metric records from Lua
	/// scenario scripts via `metrics.record(name, value)` / `metrics.set_result(passed)`.
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

		/// End the current run. Records the final pass/fail + summary.
		void EndRun();

		/// Mark the current run as passed or failed.
		void SetResult(bool passed) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Result.passed = passed;
			m_Result.resultSet = true;
		}

		/// Record an arbitrary numeric metric. Called from scenario Lua via `metrics.record(name, value)`.
		void Record(const std::string& name, double value);

		/// Record a string-valued metric (e.g. a status code).
		void RecordString(const std::string& name, const std::string& value);

		/// Consume the events drained from AIDecisionChannel for the current tick. Increments
		/// the per-run tick counter and aggregates per-(layer/type) event counts. Silently no-op
		/// when no run is active (i.e. outside a BeginRun/EndRun bracket).
		void ConsumeEvents(const std::vector<AIDecisionChannel::Event>& events);

		/// True if a run is currently active (between BeginRun and EndRun).
		bool IsRunActive() const {
			std::lock_guard<std::mutex> lock(m_Mutex);
			return !m_Scenario.empty();
		}

		/// Get the number of ticks recorded.
		uint64_t GetTickCount() const {
			std::lock_guard<std::mutex> lock(m_Mutex);
			return m_TickCount;
		}

		/// Write the run report as a JSON file at the given path. Returns true on success.
		bool WriteReport(const std::string& path) const;

		/// Convenience: write a multi-run aggregated report.
		struct AggregatedRun {
			std::string                                  scenario;
			uint64_t                                     seed = 0;
			bool                                         passed = false;
			uint64_t                                     ticks = 0;
			std::unordered_map<std::string, double>      numeric;
			std::unordered_map<std::string, std::string> stringValues;
			std::unordered_map<std::string, uint64_t>    eventCounts;
			std::string                                  finalTotalHashHex;
		};
		static bool WriteAggregatedReport(const std::string& path,
		                                  const std::vector<AggregatedRun>& runs,
		                                  const std::string& suiteVersion);

		/// Snapshot of the current run's results.
		AggregatedRun GetCurrentRun() const;

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
		std::unordered_map<std::string, uint64_t>     m_EventCounts;
		std::string                                   m_FinalTotalHashHex;
	};

} // namespace RTE
