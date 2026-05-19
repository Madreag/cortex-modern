#include "MetricsCollector.h"

#include "AIDecisionChannel.h"
#include "ScenarioRunner.h"
#include "SimChecksum.h"

#include "nlohmann/json.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace RTE {

	using json = nlohmann::json;

	MetricsCollector::MetricsCollector() = default;
	MetricsCollector::~MetricsCollector() = default;

	void MetricsCollector::Destroy() {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Scenario.clear();
		m_Seed = 0;
		m_TickCount = 0;
		m_Result = {};
		m_Numeric.clear();
		m_Strings.clear();
		m_EventCounts.clear();
		m_FinalTotalHashHex.clear();
	}

	void MetricsCollector::BeginRun(const std::string& scenario, uint64_t seed) {
		// If Lua passes 0 and the CLI runner is active with a non-zero -seed, use that — so
		// the report records the actual seed the user asked for instead of always 0.
		if (seed == 0 && ScenarioRunner::IsActive() && ScenarioRunner::GetArgs().seed != 0) {
			seed = ScenarioRunner::GetArgs().seed;
		}
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Scenario = scenario;
		m_Seed = seed;
		m_TickCount = 0;
		m_Result = {};
		m_Numeric.clear();
		m_Strings.clear();
		m_EventCounts.clear();
		m_FinalTotalHashHex.clear();
		m_StartWall = std::chrono::steady_clock::now();
	}

	void MetricsCollector::EndRun() {
		const auto last = g_SimChecksum.GetLastResult();
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_FinalTotalHashHex = SimChecksum::HashHex(last.total);
		const auto elapsed = std::chrono::steady_clock::now() - m_StartWall;
		m_Numeric["__wall_seconds"] =
		    std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
	}

	void MetricsCollector::Record(const std::string& name, double value) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Numeric[name] = value;
	}

	void MetricsCollector::RecordString(const std::string& name, const std::string& value) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Strings[name] = value;
	}

	void MetricsCollector::ConsumeEvents(const std::vector<AIDecisionChannel::Event>& events) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_Scenario.empty()) {
			return;
		}
		++m_TickCount;
		for (const auto& e: events) {
			const auto type = g_AIDecisionChannel.GetString(e.type);
			std::string key = std::string(AIDecisionChannel::LayerName(e.layer)) + "/" +
			                  std::string(type.empty() ? std::string_view("(unset)") : type);
			++m_EventCounts[key];
		}
	}

	MetricsCollector::AggregatedRun MetricsCollector::GetCurrentRun() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		AggregatedRun r;
		r.scenario = m_Scenario;
		r.seed = m_Seed;
		r.passed = m_Result.passed;
		r.ticks = m_TickCount;
		r.numeric = m_Numeric;
		r.stringValues = m_Strings;
		r.eventCounts = m_EventCounts;
		r.finalTotalHashHex = m_FinalTotalHashHex;
		return r;
	}

	bool MetricsCollector::WriteReport(const std::string& path) const {
		AggregatedRun r = GetCurrentRun();
		std::vector<AggregatedRun> single{r};
		return WriteAggregatedReport(path, single, "M0");
	}

	bool MetricsCollector::WriteAggregatedReport(const std::string& path,
	                                             const std::vector<AggregatedRun>& runs,
	                                             const std::string& suiteVersion) {
		json root;
		root["suite_version"] = suiteVersion;
		root["run_count"] = runs.size();

		// Aggregated pass rates per scenario (across K runs).
		std::unordered_map<std::string, std::pair<int, int>> scenarioPasses; // scenario -> (passed, total)
		for (const auto& r: runs) {
			auto& pp = scenarioPasses[r.scenario];
			pp.first += r.passed ? 1 : 0;
			pp.second += 1;
		}

		json scenariosJson = json::object();
		for (const auto& [name, pp]: scenarioPasses) {
			json s;
			s["passed"] = pp.first;
			s["total"] = pp.second;
			s["pass_rate"] = pp.second > 0 ? static_cast<double>(pp.first) / pp.second : 0.0;
			scenariosJson[name] = s;
		}
		root["scenarios"] = scenariosJson;

		json runsJson = json::array();
		for (const auto& r: runs) {
			json rj;
			rj["scenario"] = r.scenario;
			rj["seed"] = r.seed;
			rj["passed"] = r.passed;
			rj["ticks"] = r.ticks;
			rj["final_total_hash"] = r.finalTotalHashHex;

			json numeric = json::object();
			for (const auto& [k, v]: r.numeric) numeric[k] = v;
			rj["numeric"] = numeric;

			json strings = json::object();
			for (const auto& [k, v]: r.stringValues) strings[k] = v;
			rj["strings"] = strings;

			json events = json::object();
			for (const auto& [k, v]: r.eventCounts) events[k] = v;
			rj["event_counts"] = events;

			runsJson.push_back(rj);
		}
		root["runs"] = runsJson;

		std::ofstream out(path);
		if (!out.is_open()) {
			return false;
		}
		out << std::setw(2) << root << std::endl;
		return true;
	}

} // namespace RTE
