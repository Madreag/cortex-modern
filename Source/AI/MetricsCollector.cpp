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
#include <cstdlib>
#include <set>
#include <thread>
#include <deque>
#include <cmath>
#include <algorithm>

namespace RTE {

	using json = nlohmann::json;
	bool CrossCaptureBarrier(const json& spec, const std::string& directory, const std::string& phase, uint64_t tick, uint64_t round) {
		if (spec.at("phase") != phase || spec.at("tick").get<uint64_t>() != tick ||
		    (spec.value("round", uint64_t{0}) != 0 && spec.at("round").get<uint64_t>() != round)) return false;
		const std::string id = spec.at("id");
		const auto timeout = spec.at("timeout_ms").get<uint64_t>();
		if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos ||
		    (phase != "capture_announced" && phase != "writer_pending") || timeout == 0 || timeout > 120000)
			throw std::runtime_error("invalid capture barrier identity, phase or timeout");
		const auto root = std::filesystem::path(directory);
		std::filesystem::create_directories(root);
		const auto began = std::chrono::steady_clock::now();
		json receipt{{"type", "capture_barrier"}, {"id", id}, {"capture_phase", phase}, {"capture_tick", tick},
		    {"capture_round", round}, {"timeout_ms", timeout}, {"outcome", "entered"},
		    {"barrier_wall_ms", std::chrono::duration<double, std::milli>(began.time_since_epoch()).count()}};
		const auto prefix = root / (id + "." + std::to_string(round) + "." + phase + "." + std::to_string(tick));
		receipt["release_file"] = prefix.string() + ".release";
		const auto publish = [&](const std::string& suffix) {
			const auto target = prefix.string() + suffix;
			std::ofstream output(target + ".pending", std::ios::out | std::ios::trunc);
			output << receipt.dump() << '\n'; output.flush();
			if (!output) throw std::runtime_error("capture barrier receipt write failed");
			output.close(); std::filesystem::rename(target + ".pending", target);
			if (MetricsCollector::IsConstructed()) g_MetricsCollector.WriteObservation(receipt);
		};
		publish(".enter.json");
		const auto released = [&] { return std::filesystem::is_regular_file(prefix.string() + ".release"); };
		while (!released() && std::chrono::steady_clock::now() - began < std::chrono::milliseconds(timeout))
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		const bool passed = released();
		receipt["outcome"] = passed ? "released" : "timeout";
		receipt["wait_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
		publish(".exit.json");
		return passed;
	}

	void CrossCaptureBarrierFromEnvironment(const char* phase, uint64_t tick, uint64_t round) {
		const char* path = std::getenv("CC_TEST_CROSS_CAPTURE_BARRIER");
		if (!path || !*path) return;
		try {
			const char* headless = std::getenv("CCCP_HEADLESS");
			if (!headless || std::string(headless) != "1") throw std::runtime_error("capture barrier requires headless");
			std::ifstream input(path); const auto specs = json::parse(input);
			if (!specs.is_array() || specs.size() > 64) throw std::runtime_error("invalid capture barrier schedule");
			static std::mutex mutex;
			static std::set<std::string> fired;
			for (const auto& spec: specs) {
				if (spec.at("phase") != phase || spec.at("tick").get<uint64_t>() != tick ||
				    (spec.value("round", uint64_t{0}) != 0 && spec.at("round").get<uint64_t>() != round)) continue;
				const std::string key = spec.at("id").get<std::string>() + "/" + phase + "/" + std::to_string(round) + "/" + std::to_string(tick);
				{ std::lock_guard<std::mutex> lock(mutex); if (!fired.insert(key).second) continue; }
				CrossCaptureBarrier(spec, std::filesystem::path(path).parent_path().string(), phase, tick, round);
			}
		} catch (const std::exception& error) {
			System::PrintDiagnosticLine("[cross-record] FAIL capture barrier: " + std::string(error.what()));
		}
	}
	struct MetricsCollector::EventStream {
		std::ofstream output;
		std::string path;
		json context;
		std::vector<json> pending;
		std::deque<json> producedInputs;
		std::map<uint64_t, uint64_t> lastObservedInputTarget;
		uint64_t inputSerial = 0;
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

	void MetricsCollector::UpdateEventContext(const json& context) {
		if (!EventsEnabled()) return;
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_EventStream->context.update(context);
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

	void MetricsCollector::RecordProducedController(uint64_t round, uint64_t producedTick, uint64_t targetTick, long actor, int seat, double producedWallMs) {
		if (!EventsEnabled() || (MovableMan::IsConstructed() && g_MovableMan.IsSpeculative())) return;
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_EventStream->active) return;
		if (producedWallMs < 0) producedWallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
		json sample{{"type", "controller_input_produced"}, {"input_serial", ++m_EventStream->inputSerial},
		    {"input_round", round}, {"produced_tick", producedTick}, {"target_tick", targetTick},
		    {"actor", actor}, {"seat", seat}, {"produced_wall_ms", producedWallMs}};
		try {
			m_EventStream->producedInputs.push_back(sample);
			if (m_EventStream->producedInputs.size() > 512) m_EventStream->producedInputs.pop_front();
			m_EventStream->Write(sample);
		} catch (const std::exception& error) {
			System::PrintDiagnosticLine("[cross-record] FAIL produced input: " + std::string(error.what()));
			m_EventsEnabled.store(false);
		}
	}
	json MetricsCollector::ProducedControllerFor(uint64_t round, uint64_t targetTick, long actor) const {
		if (!EventsEnabled()) return json::object();
		std::lock_guard<std::mutex> lock(m_Mutex);
		for (auto found = m_EventStream->producedInputs.rbegin(); found != m_EventStream->producedInputs.rend(); ++found)
			if (found->at("input_round") == round && found->at("target_tick") == targetTick && found->at("actor") == actor) return *found;
		return json::object();
	}
	bool MetricsCollector::IsFreshControllerRecovery(const json& sample, uint64_t round, uint64_t tick, long actor,
	    int64_t wireTick, bool controllable, bool held, bool catchup, double afterWallMs) {
		try {
			return controllable && !held && !catchup && sample.value("queue_confirmed", false) && wireTick >= 0 && static_cast<uint64_t>(wireTick) == tick &&
			    sample.value("input_serial", uint64_t{0}) > 0 && sample.at("input_round") == round && sample.at("target_tick") == tick &&
			    sample.at("actor") == actor && std::isfinite(sample.at("produced_wall_ms").get<double>()) &&
			    std::isfinite(afterWallMs) && sample.at("produced_wall_ms").get<double>() >= afterWallMs;
		} catch (const json::exception&) { return false; }
	}
	void MetricsCollector::ConfirmProducedControllers(uint64_t round, uint64_t producedTick, uint64_t targetTick, const std::vector<long>& queuedActors, uint64_t priorInputThrough) {
		if (!EventsEnabled() || (MovableMan::IsConstructed() && g_MovableMan.IsSpeculative())) return;
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto& last = m_EventStream->lastObservedInputTarget[round];
		const bool freshTarget = targetTick > last && targetTick > priorInputThrough;
		last = std::max(last, targetTick);
		if (!freshTarget) return;
		for (auto& sample: m_EventStream->producedInputs) {
			if (sample.at("input_round") != round || sample.at("produced_tick") != producedTick || sample.at("target_tick") != targetTick ||
			    std::find(queuedActors.begin(), queuedActors.end(), sample.at("actor").get<long>()) == queuedActors.end()) continue;
			sample["queue_confirmed"] = true;
			sample["queue_readback"] = "ScenarioRunner::PeekLockstepLocalControllerFrames";
			sample["restored_input_through"] = priorInputThrough;
			try {
				auto receipt = sample; receipt["type"] = "controller_input_queued";
				m_EventStream->Write(receipt);
			} catch (const std::exception& error) {
				System::PrintDiagnosticLine("[cross-record] FAIL queued input: " + std::string(error.what()));
				m_EventsEnabled.store(false);
			}
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

	json MetricsCollector::TickTiming(long long totalUs, long long waitUs, long long captureUs, long long captureWaitUs) {
		const long long captureOnly = captureUs - captureWaitUs;
		const long long compute = totalUs - waitUs - captureOnly;
		return {{"type", "tick_timing"}, {"total_us", totalUs}, {"wait_us", waitUs}, {"capture_us", captureOnly}, {"compute_us", compute},
		    {"partition_valid", totalUs >= 0 && waitUs >= 0 && captureWaitUs >= 0 && captureWaitUs <= waitUs && captureOnly >= 0 && compute >= 0}};
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
