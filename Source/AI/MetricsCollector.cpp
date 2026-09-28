#include "MetricsCollector.h"

#include "ScenarioRunner.h"
#include "SimChecksum.h"
#include "MovableObject.h"
#include "MovableMan.h"
#include "System.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <new>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <set>
#include <thread>
#include <condition_variable>
#include <tuple>
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
	// The tick only queues its records: the context merge, the serialisation, the part rotation and the retirement of old
	// parts run on the stream's own thread, so a record never costs the measured tick more than its own fields.
	struct MetricsCollector::EventStream {
		struct Item {
			json record;
			std::shared_ptr<const json> context;
			size_t sequence = 0;
		};
		using TallyKey = std::tuple<std::string, std::string, long, int>;
		std::string path;
		json context;
		std::shared_ptr<const json> contextSnapshot; //!< The context the queued records carry; rebuilt after a change.
		std::vector<json> pending;
		std::map<TallyKey, std::pair<double, uint64_t>> tally; //!< This tick's terrain removals, summed per kind.
		std::deque<json> producedInputs;
		std::map<uint64_t, uint64_t> lastObservedInputTarget;
		uint64_t inputSerial = 0;
		size_t limit = 0, partLimit = 0, sequence = 0, overflow = 0;
		std::atomic<size_t> bytes{0};
		bool active = false;

		// The writer thread's own state.
		size_t partBytes = 0, part = 0, retainedBytes = 0, retiredParts = 0, retiredBytes = 0;
		std::deque<std::pair<size_t, size_t>> closedParts; //!< Retained closed parts: number and bytes.
		std::ofstream output;
		std::string failure;

		std::thread thread;
		std::mutex queueMutex;
		std::condition_variable wake, drained;
		std::deque<Item> queue;
		size_t queueBound = c_QueueBound; //!< Records waiting for the writer; one past it is dropped and counted.
		uint64_t queued = 0, written = 0;
		uint64_t dropped = 0; //!< Records dropped at the bound since the writer last took the queue.
		size_t lastDroppedSequence = 0;
		bool stopping = false, closedWriteReported = false;
		std::atomic<bool> failed{false};

		/// A stalled disk holds at most this many records in memory.
		static constexpr size_t c_QueueBound = 65536;

		~EventStream() { Close(); }

		std::string PartPath(size_t number) const { return number == 0 ? path : path + ".part" + std::to_string(number); }

		bool Open(const std::string& file, size_t byteLimit) { return OpenFile(file, byteLimit) && Start(); }

		bool OpenFile(const std::string& file, size_t byteLimit) {
			path = file;
			limit = byteLimit;
			partLimit = std::clamp<size_t>(byteLimit / 4, 1, 8 * 1024 * 1024);
			output.open(path, std::ios::out | std::ios::trunc);
			return static_cast<bool>(output);
		}

		bool Start() {
			thread = std::thread([this] { Run(); });
			return true;
		}

		void SetContext(json value) {
			context = std::move(value);
			contextSnapshot.reset();
		}

		/// False when the record was not queued: after Close, or past the queue's bound (counted as a record_loss).
		bool Write(json record) {
			if (failed.load()) throw std::runtime_error("event record write failed");
			if (!contextSnapshot) contextSnapshot = std::make_shared<const json>(context);
			Item item{std::move(record), contextSnapshot, ++sequence};
			std::string refusal;
			bool taken = false;
			{
				std::lock_guard<std::mutex> lock(queueMutex);
				if (stopping) {
					if (!std::exchange(closedWriteReported, true)) refusal = "[cross-record] an event record came after its stream closed: dropped";
				} else if (queue.size() >= queueBound) {
					// One line per stall; the writer records the count as a record_loss after the records it had queued.
					if (dropped++ == 0) refusal = "[cross-record] the event queue reached its bound of " + std::to_string(queueBound) + " records: the records past it are dropped and counted as a record_loss";
					lastDroppedSequence = item.sequence;
				} else {
					queue.push_back(std::move(item));
					taken = true;
				}
			}
			if (!refusal.empty()) System::PrintDiagnosticLine(refusal);
			if (taken) wake.notify_one();
			return taken;
		}

		void Close() {
			{
				std::lock_guard<std::mutex> lock(queueMutex);
				stopping = true;
			}
			wake.notify_one();
			if (thread.joinable()) thread.join();
			if (output.is_open()) output.close();
		}

	private:
		void Fail(const std::string& reason) {
			if (!failed.exchange(true)) failure = reason;
		}

		// One finished line into the current part: a full part closes, and at the budget the oldest parts retire so
		// recording goes on with the newest records kept.
		void Place(json& record, const json& itemContext, size_t itemSequence) {
			for (const auto& [key, value]: itemContext.items()) record[key] = value;
			record["sequence"] = itemSequence;
			std::string line = record.dump() + '\n';
			if (line.size() > partLimit) return Fail("event record larger than a part");
			if (partBytes > 0 && partBytes + line.size() > partLimit) {
				closedParts.emplace_back(part, partBytes);
				output.close();
				output.open(PartPath(++part), std::ios::out | std::ios::trunc);
				partBytes = 0;
			}
			if (retainedBytes + line.size() > limit && !closedParts.empty()) {
				json rotation = itemContext;
				rotation["type"] = "record_rotation";
				rotation["retired_parts"] = json::array();
				while (retainedBytes + line.size() + 1024 > limit && !closedParts.empty()) {
					rotation["retired_parts"].push_back(std::filesystem::path(PartPath(closedParts.front().first)).filename().string());
					std::error_code ignored;
					std::filesystem::remove(PartPath(closedParts.front().first), ignored);
					retainedBytes -= closedParts.front().second;
					retiredBytes += closedParts.front().second;
					++retiredParts;
					closedParts.pop_front();
				}
				rotation["retired_parts_total"] = retiredParts;
				rotation["retired_bytes_total"] = retiredBytes;
				rotation["byte_budget"] = limit;
				rotation["sequence"] = itemSequence;
				line = rotation.dump() + '\n' + line;
			}
			output << line;
			if (!output) return Fail("event record write failed");
			partBytes += line.size();
			retainedBytes += line.size();
			bytes.fetch_add(line.size());
		}

		void Run() {
			std::unique_lock<std::mutex> lock(queueMutex);
			while (true) {
				wake.wait(lock, [this] { return stopping || !queue.empty(); });
				std::deque<Item> batch;
				batch.swap(queue);
				queued += batch.size();
				const uint64_t lost = std::exchange(dropped, 0);
				const size_t lostSequence = lastDroppedSequence;
				const bool stop = stopping;
				lock.unlock();
				for (Item& item: batch) {
					if (!failed.load()) Place(item.record, *item.context, item.sequence);
				}
				if (lost && !failed.load()) {
					json loss{{"type", "record_loss"}, {"count", lost}, {"reason", "queue_bound"}, {"queue_bound", queueBound}};
					Place(loss, batch.empty() ? json::object() : *batch.back().context, lostSequence);
				}
				output.flush();
				if (!output) Fail("event record write failed");
				lock.lock();
				written += batch.size();
				drained.notify_all();
				if (stop && queue.empty()) return;
			}
		}
	};

	bool MetricsCollector::OpenEvents(const std::string& path, size_t byteLimit) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_EventStream = std::make_unique<EventStream>();
		m_EventsEnabled.store(m_EventStream->Open(path, byteLimit));
		return m_EventsEnabled.load();
	}

	void MetricsCollector::BeginEventTick(const json& context, bool prediction) {
		if (!EventsEnabled()) return;
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_EventStream->pending.empty() || !m_EventStream->tally.empty()) ++m_EventStream->overflow;
		m_EventStream->pending.clear();
		m_EventStream->tally.clear();
		m_EventStream->SetContext(context);
		m_EventStream->active = !prediction;
	}

	void MetricsCollector::RecordEvent(const std::string& event, const MovableObject* object, const std::string& result, double amount, long other, int seat) {
		if (!EventsEnabled()) return;
		if (!object && event == "terrain_removed") {
			// Terrain removal reports one pixel at a time: the tick's record carries their sum.
			if (MovableMan::IsConstructed() && g_MovableMan.IsSpeculative()) return;
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (!m_EventStream->active) return;
			auto& [total, calls] = m_EventStream->tally[{event, result, other, seat}];
			total += amount;
			++calls;
			return;
		}
		AppendEvent({{"event", event}, {"result", result}, {"amount", amount}, {"seat", seat}, {"other", other},
		    {"actor", object ? object->GetRootParent()->GetUniqueID() : 0}, {"object", object ? object->GetUniqueID() : 0},
		    {"team", object ? object->GetTeam() : -1}, {"preset", object ? object->GetPresetName() : ""},
		    {"class", object ? object->GetClassName() : ""}});
	}

	void MetricsCollector::UpdateEventContext(const json& context) {
		if (!EventsEnabled()) return;
		std::lock_guard<std::mutex> lock(m_Mutex);
		json merged = m_EventStream->context;
		merged.update(context);
		m_EventStream->SetContext(std::move(merged));
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
			for (const auto& [key, sum]: m_EventStream->tally) {
				const auto& [event, result, other, seat] = key;
				m_EventStream->Write({{"type", "coverage"}, {"event", event}, {"result", result}, {"amount", sum.first}, {"seat", seat},
				    {"other", other}, {"actor", 0}, {"object", 0}, {"team", -1}, {"preset", ""}, {"class", ""}, {"merged_calls", sum.second}});
			}
			if (m_EventStream->overflow) {
				m_EventStream->Write({{"type", "record_loss"}, {"count", m_EventStream->overflow}});
				m_EventStream->overflow = 0;
			}
		} catch (const std::exception& error) {
			System::PrintDiagnosticLine("[cross-record] FAIL " + std::string(error.what()));
			m_EventsEnabled.store(false);
		}
		m_EventStream->pending.clear();
		m_EventStream->tally.clear();
		m_EventStream->active = false;
	}

	void MetricsCollector::WriteObservation(const json& observation) {
		if (!EventsEnabled()) return;
		std::lock_guard<std::mutex> lock(m_Mutex);
		try { m_EventStream->Write(observation); }
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
		if (m_EventStream) m_EventStream->Close();
	}

	size_t MetricsCollector::EventBytes() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_EventStream ? m_EventStream->bytes.load() : 0;
	}

	size_t MetricsCollector::InstrumentationBytes() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		size_t bytes = m_TickHashes.capacity() * sizeof(TickHashRecord);
		for (const auto& record: m_TickHashes) bytes += record.subsystems.capacity() * sizeof(decltype(record.subsystems)::value_type);
		return bytes;
	}

	json MetricsCollector::TickTiming(long long totalUs, long long waitUs, long long captureUs, long long captureWaitUs) {
		const long long captureOnly = captureUs - captureWaitUs;
		const long long compute = totalUs - waitUs - captureOnly;
		return {{"type", "tick_timing"}, {"total_us", totalUs}, {"wait_us", waitUs}, {"capture_us", captureOnly}, {"compute_us", compute},
		    {"partition_valid", totalUs >= 0 && waitUs >= 0 && captureWaitUs >= 0 && captureWaitUs <= waitUs && captureOnly >= 0 && compute >= 0}};
	}

	bool MetricsCollector::RunEventRotationSelfTest(const std::string& directory, std::string* error) {
		std::filesystem::remove_all(directory);
		std::filesystem::create_directories(directory);
		const auto path = (std::filesystem::path(directory) / "events.jsonl").string();
		constexpr size_t budget = 16 * 1024;
		constexpr int records = 600;
		size_t written = 0;
		{
			EventStream stream;
			if (!stream.Open(path, budget)) { *error = "event stream open"; return false; }
			stream.SetContext({{"tick", 0}});
			try {
				for (int i = 0; i < records; ++i) {
					stream.SetContext({{"tick", i}});
					stream.Write({{"type", "coverage"}, {"index", i}, {"padding", std::string(96, 'x')}});
					++written;
				}
			} catch (const std::exception& failure) { *error = std::string("recording stopped at the budget: ") + failure.what(); return false; }
			stream.Close();
			if (stream.failed.load()) { *error = "event stream failed: " + stream.failure; return false; }
		}
		size_t retained = 0, rotations = 0;
		int last = -1;
		for (const auto& file: std::filesystem::directory_iterator(directory)) {
			retained += static_cast<size_t>(file.file_size());
			std::ifstream input(file.path());
			for (std::string line; std::getline(input, line);) {
				const auto row = json::parse(line, nullptr, false);
				if (row.is_discarded()) { *error = "unreadable retained record"; return false; }
				if (row.value("type", "") == "record_rotation") ++rotations;
				else last = std::max(last, row.value("index", -1));
			}
		}
		const bool oldestRetired = !std::filesystem::exists(path);
		const bool pass = written == records && last == records - 1 && retained <= budget && rotations > 0 && oldestRetired;
		System::PrintDiagnosticLine(std::string("[net-match-selftest] ") + (pass ? "PASS" : "FAIL") + " event_budget_rotates_parts written=" + std::to_string(written) +
		    " last_retained=" + std::to_string(last) + " retained_bytes=" + std::to_string(retained) + " budget=" + std::to_string(budget) +
		    " rotations=" + std::to_string(rotations) + " oldest_retired=" + std::to_string(oldestRetired));
		if (!pass) *error = "the event budget did not rotate its parts";
		// A stalled writer holds at most its bound: the records past it are dropped, counted and written as one loss; a write
		// after Close is refused.
		const auto boundPath = (std::filesystem::path(directory) / "bounded.jsonl").string();
		size_t accepted = 0;
		bool acceptedAfterClose = true;
		{
			EventStream stream;
			stream.queueBound = 8;
			if (!stream.OpenFile(boundPath, budget)) { *error = "bounded event stream open"; return false; }
			stream.SetContext({{"tick", 1}});
			for (int i = 0; i < 10; ++i) accepted += stream.Write({{"type", "coverage"}, {"index", i}}) ? 1 : 0;
			stream.Start();
			stream.Close();
			acceptedAfterClose = stream.Write({{"type", "coverage"}, {"index", 10}});
		}
		size_t coverage = 0, lossRecords = 0;
		uint64_t lost = 0;
		{
			std::ifstream input(boundPath);
			for (std::string line; std::getline(input, line);) {
				const auto row = json::parse(line, nullptr, false);
				if (row.is_discarded()) continue;
				if (row.value("type", "") == "record_loss") {
					++lossRecords;
					lost += row.value("count", uint64_t{0});
				} else if (row.value("type", "") == "coverage") ++coverage;
			}
		}
		const bool bounded = accepted == 8 && coverage == 8 && lossRecords == 1 && lost == 2 && !acceptedAfterClose;
		System::PrintDiagnosticLine(std::string("[net-match-selftest] ") + (bounded ? "PASS" : "FAIL") + " event_queue_bound_counts_its_losses accepted=" +
		    std::to_string(accepted) + " written=" + std::to_string(coverage) + " loss_records=" + std::to_string(lossRecords) + " lost=" +
		    std::to_string(lost) + " accepted_after_close=" + std::to_string(acceptedAfterClose));
		if (!bounded) *error = pass ? "the event queue dropped records past its bound without counting them, or took a write after Close" : *error;
		std::filesystem::remove_all(directory);
		return pass && bounded;
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
