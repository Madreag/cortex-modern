#include "MetricsCollector.h"

#include "ScenarioRunner.h"
#include "SimChecksum.h"
#include "MovableObject.h"
#include "MovableMan.h"
#include "System.h"

#include "nlohmann/json.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <filesystem>

namespace RTE {

	using json = nlohmann::json;
	struct MetricsCollector::EventStream {
		std::ofstream output;
		std::string path;
		json context;
		std::vector<json> pending;
		size_t bytes = 0, partBytes = 0, limit = 0, part = 0, sequence = 0, overflow = 0;
		bool active = false;
		void Write(json record) {
			for (const auto& [key, value]: context.items()) record[key] = value;
			record["sequence"] = ++sequence;
			const std::string line = record.dump() + '\n';
			if (bytes + line.size() > limit) throw std::runtime_error("event byte budget exhausted");
			if (partBytes >= 8 * 1024 * 1024) {
				output.close();
				output.open(path + ".part" + std::to_string(++part), std::ios::out | std::ios::trunc);
				partBytes = 0;
			}
			output << line;
			if (!output) throw std::runtime_error("event record write failed");
			bytes += line.size(); partBytes += line.size();
		}
	};

	bool MetricsCollector::OpenEvents(const std::string& path, size_t byteLimit) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_EventStream = std::make_unique<EventStream>();
		m_EventStream->output.open(path, std::ios::out | std::ios::trunc);
		m_EventStream->path = path;
		m_EventStream->limit = byteLimit;
		m_EventsEnabled.store(m_EventStream->output.good());
		return m_EventsEnabled.load();
	}

	void MetricsCollector::BeginEventTick(const json& context, bool prediction) {
		if (!EventsEnabled()) return;
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_EventStream->pending.empty()) ++m_EventStream->overflow;
		m_EventStream->pending.clear();
		m_EventStream->context = context;
		m_EventStream->active = !prediction;
	}

	void MetricsCollector::RecordEvent(const std::string& event, const MovableObject* object, const std::string& result, double amount, long other, int seat) {
		if (!EventsEnabled()) return;
		AppendEvent({{"event", event}, {"result", result}, {"amount", amount}, {"seat", seat}, {"other", other},
		    {"actor", object ? object->GetRootParent()->GetUniqueID() : 0}, {"object", object ? object->GetUniqueID() : 0},
		    {"team", object ? object->GetTeam() : -1}, {"preset", object ? object->GetPresetName() : ""},
		    {"class", object ? object->GetClassName() : ""}});
	}

	void MetricsCollector::AppendEvent(const json& event) {
		if (!EventsEnabled() || (MovableMan::IsConstructed() && g_MovableMan.IsSpeculative())) return;
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_EventStream->active) return;
		if (m_EventStream->pending.size() >= 4096) { ++m_EventStream->overflow; return; }
		m_EventStream->pending.push_back(event);
	}

	void MetricsCollector::FlushEventTick() {
		if (!EventsEnabled()) return;
		std::lock_guard<std::mutex> lock(m_Mutex);
		try {
			for (auto& event: m_EventStream->pending) {
				event["type"] = "coverage";
				m_EventStream->Write(std::move(event));
			}
			if (m_EventStream->overflow) {
				m_EventStream->Write({{"type", "record_loss"}, {"count", m_EventStream->overflow}});
				m_EventStream->overflow = 0;
			}
			m_EventStream->output.flush();
		} catch (const std::exception& error) {
			System::PrintDiagnosticLine("[cross-record] FAIL " + std::string(error.what()));
			m_EventsEnabled.store(false);
		}
		m_EventStream->pending.clear();
		m_EventStream->active = false;
	}

	void MetricsCollector::WriteObservation(const json& observation) {
		if (!EventsEnabled()) return;
		std::lock_guard<std::mutex> lock(m_Mutex);
		try { m_EventStream->Write(observation); m_EventStream->output.flush(); }
		catch (const std::exception& error) {
			System::PrintDiagnosticLine("[cross-record] FAIL " + std::string(error.what()));
			m_EventsEnabled.store(false);
		}
	}

	void MetricsCollector::CloseEvents() {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_EventsEnabled.store(false);
		if (m_EventStream) m_EventStream->output.close();
	}

	size_t MetricsCollector::EventBytes() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_EventStream ? m_EventStream->bytes : 0;
	}

	size_t MetricsCollector::InstrumentationBytes() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		size_t bytes = m_TickHashes.capacity() * sizeof(TickHashRecord);
		for (const auto& record: m_TickHashes) {
			bytes += record.totalHex.capacity();
			for (const auto& [key, value]: record.subsystemHex) bytes += key.capacity() + value.capacity();
		}
		return bytes;
	}

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

	void MetricsCollector::SetNativeOutcome(bool completed, const std::string& reason, uint64_t finalTick) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_Result.resultSet) {
			m_Result.passed = completed;
			m_Result.resultSet = true;
		}
		m_Strings["completion"] = completed ? "completed" : "failed";
		m_Strings["completion_reason"] = reason;
		m_Numeric.try_emplace("final_tick", static_cast<double>(finalTick));
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
		rec.totalHex = SimChecksum::HashHex(result.total);
		for (const auto& [name, hash]: result.per_subsystem) {
			rec.subsystemHex.emplace(name, SimChecksum::HashHex(hash));
		}
		m_TickHashes.push_back(std::move(rec));
	}

	MetricsCollector::AggregatedRun MetricsCollector::GetCurrentRun() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
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
		r.tickHashes = m_TickHashes;
		r.simConfig = m_SimConfig;
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
			if (!r.tickHashes.empty()) {
				json tickHashes = json::array();
				for (const auto& t: r.tickHashes) {
					json th;
					th["tick"] = t.tick;
					th["paused"] = t.paused;
					th["total"] = t.totalHex;
					json subs = json::object();
					for (const auto& [name, hex]: t.subsystemHex) {
						subs[name] = hex;
					}
					th["subsystems"] = subs;
					tickHashes.push_back(th);
				}
				rj["tick_hashes"] = tickHashes;
			}

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
