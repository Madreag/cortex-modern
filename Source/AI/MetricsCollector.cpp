#include "MetricsCollector.h"

#include "ScenarioRunner.h"
#include "SimChecksum.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <new>
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
		m_FinalTotalHashHex.clear();
		m_TickHashes.clear();
		m_SubsystemNames.clear();
		m_SubsystemIndex.clear();
		m_HostRun = false;
		m_RecordTickHashes = false;
		m_SimConfig.clear();
	}

	void MetricsCollector::BeginRun(const std::string& scenario, uint64_t seed) {
		// If Lua passes 0 and the CLI runner is active with a non-zero -seed, use that — so
		// the report records the actual seed the user asked for instead of always 0.
		if (seed == 0 && ScenarioRunner::IsActive() && ScenarioRunner::GetArgs().seed != 0) {
			seed = ScenarioRunner::GetArgs().seed;
		}
		// The CLI `-tick-hashes` flag arms per-tick hash recording. Setting it here
		// (in BeginRun) keeps the recording state aligned with the run lifetime; EndRun does
		// not clear it because the trace is read by WriteReport after EndRun returns.
		const bool armTickHashes = ScenarioRunner::IsActive() && ScenarioRunner::GetArgs().tickHashes;
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Scenario = scenario;
		m_Seed = seed;
		m_TickCount = 0;
		m_Result = {};
		m_Numeric.clear();
		m_Strings.clear();
		m_FinalTotalHashHex.clear();
		m_TickHashes.clear();
		m_SubsystemNames.clear();
		m_SubsystemIndex.clear();
		m_HostRun = false;
		m_RecordTickHashes = armTickHashes;
		m_StartWall = std::chrono::steady_clock::now();
		m_SimConfig = ScenarioRunner::GatherSimConfig();
	}

	void MetricsCollector::BeginHostRun(const std::string& scenario, uint64_t seed) {
		BeginRun(scenario, seed);
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_HostRun = true;
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

	void MetricsCollector::RecordTickHash(const SimChecksum::Result& result, bool paused) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		// Silently no-op when recording is disabled or no run is active. This makes the
		// call site in Main.cpp unconditional — same shape as g_SimChecksum.EndTick.
		if (!m_RecordTickHashes || m_Scenario.empty()) {
			return;
		}
		// Cap the trace at the -max-ticks budget — the sim loop checks the cap per frame
		// but its inner fixed-step loop overshoots by a load-dependent tick batch, which
		// would otherwise make per-run trace lengths non-uniform across the determinism check.
		if (uint64_t cap = ScenarioRunner::GetArgs().maxTicks; cap > 0 && m_TickHashes.size() >= cap) {
			return;
		}
		TickHashRecord rec;
		rec.tick = result.tick;
		rec.paused = paused;
		rec.total = result.total;
		rec.subsystems.reserve(result.per_subsystem.size());
		for (const auto& [name, hash]: result.per_subsystem) {
			const auto [slot, added] = m_SubsystemIndex.try_emplace(name, static_cast<uint16_t>(m_SubsystemNames.size()));
			if (added) {
				m_SubsystemNames.push_back(name);
			}
			rec.subsystems.emplace_back(slot->second, hash);
		}
		m_TickHashes.push_back(std::move(rec));
	}

	MetricsCollector::AggregatedRun MetricsCollector::GetCurrentRun(bool withTickHashes) const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return CurrentRunLocked(withTickHashes);
	}

	MetricsCollector::AggregatedRun MetricsCollector::CurrentRunLocked(bool withTickHashes) const {
		AggregatedRun r;
		r.scenario = m_Scenario;
		r.seed = m_Seed;
		r.passed = m_Result.passed;
		// The scenario Lua records the authoritative tick count via final_tick; fall back to
		// m_TickCount only if it didn't.
		if (auto it = m_Numeric.find("final_tick"); it != m_Numeric.end()) {
			r.ticks = static_cast<uint64_t>(it->second);
		} else {
			r.ticks = m_TickCount;
		}
		r.numeric = m_Numeric;
		r.stringValues = m_Strings;
		r.finalTotalHashHex = m_FinalTotalHashHex;
		r.tickHashCount = m_TickHashes.size();
		if (withTickHashes) {
			r.tickHashes = m_TickHashes;
			r.subsystemNames = m_SubsystemNames;
		}
		r.simConfig = m_SimConfig;
		return r;
	}

	bool MetricsCollector::WriteReport(const std::string& path) const {
		// The trace is written in place under the lock: a copy of a long match's trace is the allocation that fails.
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::vector<AggregatedRun> single{CurrentRunLocked(false)};
		return WriteRuns(path, single, "M0", &m_TickHashes, &m_SubsystemNames);
	}

	bool MetricsCollector::WriteAggregatedReport(const std::string& path,
	                                             const std::vector<AggregatedRun>& runs,
	                                             const std::string& suiteVersion) {
		return WriteRuns(path, runs, suiteVersion, nullptr, nullptr);
	}

	bool MetricsCollector::WriteRuns(const std::string& path, const std::vector<AggregatedRun>& runs, const std::string& suiteVersion,
	                                 const std::vector<TickHashRecord>* firstRunHashes, const std::vector<std::string>* firstRunNames) {
		// The line a failed allocation is reported at; a report that cannot be built fails the run, it never throws out of it.
		int stage = __LINE__;
		size_t written = 0;
		try {
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

				json simConfig = json::object();
				for (const auto& [k, v]: r.simConfig) simConfig[k] = v;
				rj["sim_config"] = simConfig;

				json numeric = json::object();
				for (const auto& [k, v]: r.numeric) numeric[k] = v;
				rj["numeric"] = numeric;

				json strings = json::object();
				for (const auto& [k, v]: r.stringValues) strings[k] = v;
				rj["strings"] = strings;

				// Emit the per-tick hash trace when present. cccp-determinism-check
				// reads this array to diff multiple runs of the same scenario+seed and surface
				// the first tick at which divergence appears, plus which subsystem diverged.
				// Each trace is streamed in at its placeholder rather than built as a document.
				const size_t index = runsJson.size();
				if (!(index == 0 && firstRunHashes ? *firstRunHashes : r.tickHashes).empty()) {
					rj["tick_hashes"] = "@@tick_hashes_" + std::to_string(index) + "@@";
				}

				runsJson.push_back(rj);
			}
			root["runs"] = runsJson;

			stage = __LINE__;
			std::ostringstream document;
			document << std::setw(2) << root;
			const std::string text = document.str();

			std::ofstream out(path);
			if (!out.is_open()) {
				return false;
			}
			size_t from = 0;
			for (size_t index = 0; index < runs.size(); ++index) {
				const std::string placeholder = "\"@@tick_hashes_" + std::to_string(index) + "@@\"";
				const size_t at = text.find(placeholder, from);
				if (at == std::string::npos) {
					continue;
				}
				out.write(text.data() + from, static_cast<std::streamsize>(at - from));
				const bool live = index == 0 && firstRunHashes;
				const std::vector<TickHashRecord>& hashes = live ? *firstRunHashes : runs[index].tickHashes;
				const std::vector<std::string>& names = live ? *firstRunNames : runs[index].subsystemNames;
				stage = __LINE__;
				std::vector<std::pair<const std::string*, const SimChecksum::Hash*>> sorted;
				out << '[';
				for (const TickHashRecord& t: hashes) {
					sorted.clear();
					for (const auto& [name, hash]: t.subsystems) {
						sorted.emplace_back(&names[name], &hash);
					}
					std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return *a.first < *b.first; });
					out << (written == 0 ? "\n" : ",\n") << "{\"paused\":" << (t.paused ? "true" : "false") << ",\"subsystems\":{";
					for (size_t sub = 0; sub < sorted.size(); ++sub) {
						out << (sub == 0 ? "\"" : ",\"") << *sorted[sub].first << "\":\"" << SimChecksum::HashHex(*sorted[sub].second) << '"';
					}
					out << "},\"tick\":" << t.tick << ",\"total\":\"" << SimChecksum::HashHex(t.total) << "\"}";
					++written;
				}
				out << "\n]";
				from = at + placeholder.size();
			}
			out.write(text.data() + from, static_cast<std::streamsize>(text.size() - from));
			out << std::endl;
			return static_cast<bool>(out);
		} catch (const std::bad_alloc&) {
			std::cerr << "[metrics] report " << path << " ran out of memory at MetricsCollector.cpp:" << stage << " after " << written << " tick records" << std::endl;
			return false;
		}
	}

} // namespace RTE
