#include "NetA7Journal.h"

#include "System/FaultInjection.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <thread>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef CCCP_WITH_GNS
#include <openssl/evp.h>
#endif
#ifdef _WIN32
#include <Windows.h>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace RTE {

	namespace {
		using json = nlohmann::json;
		using Clock = std::chrono::steady_clock;
		constexpr size_t c_QueueSize = 1024;
		constexpr size_t c_RecordSize = 16384;
		constexpr uint64_t c_GateBudgetMs = 240000;
		constexpr const char* c_HashAlgorithm = "SimChecksum::SimGatedHash/fnv1a64-splitmix256-v1";
		struct Slot {
			std::atomic<uint64_t> turn{0};
			size_t size = 0;
			std::array<char, c_RecordSize> bytes{};
		};
		static_assert(std::atomic<uint64_t>::is_always_lock_free);
		static_assert(std::atomic<uint32_t>::is_always_lock_free);
		static_assert(std::atomic<bool>::is_always_lock_free);
		struct State {
			std::unique_ptr<std::array<Slot, c_QueueSize>> slots;
			std::atomic<uint64_t> head{0}, tail{0}, gaps{0}, frame{0}, resync{0};
			std::atomic<uint32_t> producers{0};
			std::atomic<bool> enabled{false}, stop{false}, failed{false}, gateReleased{false};
			std::string run, peer, exeSha, fault, gate;
			std::function<bool()> cancelled;
			uint32_t heartbeatTag = 0;
			uint64_t pid = 0;
			uint64_t heartbeatMs = 0, receiveBudgetMs = 0;
			Clock::time_point started;
			FILE* file = nullptr;
			std::thread writer;
			~State() {
				enabled.store(false);
				stop.store(true);
				if (writer.joinable()) writer.join();
				if (file) std::fclose(file);
			}
		};
		State s_State;
		thread_local json t_Seats = json::array();
		thread_local std::string t_SeatSource = "unobserved";
		thread_local uint64_t t_SeatSessionMs = 0, t_Round = 0, t_Frame = 0;

		std::string Env(const char* name) {
			const char* value = std::getenv(name);
			return value ? value : "";
		}

		uint64_t Elapsed() {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s_State.started).count());
		}

		bool Token(const std::string& value) {
			if (value.empty() || value.size() > 128) return false;
			for (const unsigned char c: value) {
				if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
			}
			return true;
		}

		bool Positive(const std::string& value, uint64_t& result) {
			if (value.empty()) return false;
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
			return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && result > 0;
		}

		std::filesystem::path ExecutablePath() {
		#ifdef _WIN32
			std::array<wchar_t, 32768> path{};
			const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
			return size && size < path.size() ? std::filesystem::path(std::wstring(path.data(), size)) : std::filesystem::path();
		#elif defined(__APPLE__)
			uint32_t size = 0;
			_NSGetExecutablePath(nullptr, &size);
			std::vector<char> path(size);
			return _NSGetExecutablePath(path.data(), &size) == 0 ? std::filesystem::path(path.data()) : std::filesystem::path();
		#else
			std::error_code error;
			return std::filesystem::read_symlink("/proc/self/exe", error);
		#endif
		}

		std::string FileSha(const std::filesystem::path& path) {
		#ifdef CCCP_WITH_GNS
			std::ifstream input(path, std::ios::binary);
			if (!input) return {};
			EVP_MD_CTX* context = EVP_MD_CTX_new();
			if (!context) return {};
			bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
			std::array<char, 65536> buffer{};
			while (ok && input) {
				input.read(buffer.data(), buffer.size());
				if (input.gcount() > 0) ok = EVP_DigestUpdate(context, buffer.data(), static_cast<size_t>(input.gcount())) == 1;
			}
			std::array<uint8_t, 32> digest{};
			unsigned int size = 0;
			ok = ok && input.eof() && EVP_DigestFinal_ex(context, digest.data(), &size) == 1 && size == digest.size();
			EVP_MD_CTX_free(context);
			return ok ? NetA7Journal::Hex(digest.data(), digest.size()) : std::string();
		#else
			return {};
		#endif
		}

		FILE* OpenExclusive(const std::filesystem::path& path) {
		#ifdef _WIN32
			const int fd = _wopen(path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
			if (fd < 0) return nullptr;
			FILE* file = _fdopen(fd, "wb");
			if (!file) _close(fd);
		#else
			const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
			if (fd < 0) return nullptr;
			FILE* file = fdopen(fd, "wb");
			if (!file) close(fd);
		#endif
			return file;
		}

		bool Enqueue(const std::string& record) {
			if (record.size() > c_RecordSize) return false;
			uint64_t position = s_State.head.load(std::memory_order_relaxed);
			for (unsigned attempt = 0; attempt < 32; ++attempt) {
				Slot& slot = (*s_State.slots)[position % c_QueueSize];
				const uint64_t turn = slot.turn.load(std::memory_order_acquire);
				if (turn < position) return false;
				if (turn == position && s_State.head.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
					std::memcpy(slot.bytes.data(), record.data(), record.size());
					slot.size = record.size();
					slot.turn.store(position + 1, std::memory_order_release);
					return true;
				}
				position = s_State.head.load(std::memory_order_relaxed);
			}
			return false;
		}

		bool WriteRecord(json record, uint64_t& sequence) {
			record["run_id"] = s_State.run;
			record["peer"] = s_State.peer;
			record["pid"] = s_State.pid;
			record["seq"] = sequence++;
			record["at_ms"] = Elapsed();
			record["journal_dropped"] = s_State.gaps.load();
			const std::string line = record.dump() + "\n";
			return std::fwrite(line.data(), 1, line.size(), s_State.file) == line.size() && std::fflush(s_State.file) == 0;
		}

		void DrainJournal() {
			uint64_t sequence = 0, reportedGaps = 0;
			std::map<std::string, uint64_t> leaveOrdinals;
			try {
				for (;;) {
					const uint64_t position = s_State.tail.load(std::memory_order_relaxed);
					Slot& slot = (*s_State.slots)[position % c_QueueSize];
					if (slot.turn.load(std::memory_order_acquire) == position + 1) {
						json record = json::parse(slot.bytes.data(), slot.bytes.data() + slot.size);
						slot.turn.store(position + c_QueueSize, std::memory_order_release);
						s_State.tail.store(position + 1, std::memory_order_release);
						if (record.at("event") == "leave_send") record["ordinal"] = leaveOrdinals[record.at("transaction").get<std::string>()]++;
						if (record.at("event") != "terminal" || s_State.gaps.load() == 0) {
							if (!WriteRecord(std::move(record), sequence)) { s_State.failed.store(true); break; }
						}
						continue;
					}
					const uint64_t gaps = s_State.gaps.load();
					if (gaps != reportedGaps && sequence != 0) {
						reportedGaps = gaps;
						if (!WriteRecord({{"event", "evidence_gap"}, {"reason", "journal queue overflow, contention, or invalid observation"}}, sequence)) { s_State.failed.store(true); break; }
					}
					if (s_State.stop.load() && position == s_State.head.load()) break;
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			} catch (...) {
				s_State.failed.store(true);
			}
		}
	}

	bool NetA7Journal::StartE2E(std::string* error, std::function<bool()> cancelled) {
		const std::string log = Env("CC_A7_EVENT_LOG"), run = Env("CC_A7_RUN_ID"), peer = Env("CC_A7_PEER"), expected = Env("CC_A7_BINARY_SHA256");
		if (log.empty() && run.empty() && peer.empty() && expected.empty()) return true;
		auto fail = [error](const char* message) { if (error) *error = message; return false; };
		if (!Token(run) || !Token(peer) || log.empty() || expected.size() != 64 || s_State.file) return fail("A7 requires one fresh E2E journal and complete process identity");
		for (char c: expected) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return fail("A7 binary identity must be lowercase SHA-256");
		s_State.exeSha = FileSha(ExecutablePath());
		if (s_State.exeSha != expected) return fail("A7 executable hash differs from the guarded launch identity, or SHA-256 is unavailable");
		s_State.fault = Env("CC_FAULT_INJECT");
		if (!s_State.fault.empty()) {
			size_t start = 0;
			while (start <= s_State.fault.size()) {
				const size_t comma = s_State.fault.find(',', start);
				const std::string fault = s_State.fault.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
				if (fault.empty() || !FaultInjected(fault.c_str())) return fail("A7 environment differs from the effective parsed fault set");
				if (comma == std::string::npos) break;
				start = comma + 1;
			}
		}
		s_State.gate = Env("CC_A7_CONNECT_GATE");
		std::error_code fsError;
		if (!s_State.gate.empty() && (std::filesystem::exists(s_State.gate, fsError) || fsError)) return fail("A7 connect gate must be absent at journal activation");
		const std::string heartbeat = Env("CC_A7_SILENT_HEARTBEAT_MS"), tag = Env("CC_A7_SILENT_HEARTBEAT_TAG"), receive = Env("CC_A7_SILENT_RECEIVE_BUDGET_MS");
		if (!heartbeat.empty() || !tag.empty() || !receive.empty()) {
			uint64_t parsedTag = 0;
			if (!FaultInjected("client_never_says_hello") || heartbeat != "4000" || receive != "30000" || !Positive(tag, parsedTag) || parsedTag > UINT32_MAX) return fail("A7 silent control requires the existing fault, a positive uint32 tag, and the exact 4000/30000 ms controls");
			s_State.heartbeatTag = static_cast<uint32_t>(parsedTag);
			s_State.heartbeatMs = 4000;
			s_State.receiveBudgetMs = 30000;
		}
		s_State.file = OpenExclusive(log);
		if (!s_State.file) return fail("A7 journal could not be created exclusively");
		s_State.run = run;
		s_State.peer = peer;
		s_State.cancelled = std::move(cancelled);
	#ifdef _WIN32
		s_State.pid = static_cast<uint64_t>(_getpid());
	#else
		s_State.pid = static_cast<uint64_t>(getpid());
	#endif
		s_State.started = Clock::now();
		s_State.slots = std::make_unique<std::array<Slot, c_QueueSize>>();
		for (size_t i = 0; i < c_QueueSize; ++i) (*s_State.slots)[i].turn.store(i);
		s_State.enabled.store(true);
		Emit("ready", {{"schema", 1}, {"exe_sha256", s_State.exeSha}, {"fault", s_State.fault},
			{"capabilities", {"identity", "listening", "commit", "running", "progress", "terminal", "connect_gate", "handshake_age", "drop", "reclaim", "resumption", "leave_exchange", "save_gap", "owner_observation", "decision_clock_v1", "journal_integrity_v1", "loaded_ticket_sha256_v1", "leave_queue_clock_v1", "local_player_view_v1"}},
			{"shared_hash_algorithm", c_HashAlgorithm}, {"shared_hash_scope", "existing approved SimGatedHash feeds; lua_state is RNG only"},
			{"clock_domains", {{"at_ms", "journal_writer_delivery"}, {"captured_ms", "journal_producer_capture"}, {"session_ms", "authority_session_decision"}}}});
		s_State.writer = std::thread(DrainJournal);
		return true;
	}

	bool NetA7Journal::Enabled() { return s_State.enabled.load(std::memory_order_relaxed); }

	void NetA7Journal::Emit(const char* event, json fields) {
		if (!Enabled()) return;
		++s_State.producers;
		if (!Enabled()) { --s_State.producers; return; }
		try {
			fields["event"] = event;
			fields["captured_ms"] = Elapsed();
			if (!Enqueue(fields.dump())) ++s_State.gaps;
		} catch (...) { ++s_State.gaps; }
		--s_State.producers;
	}

	void NetA7Journal::Session(const char* event, uint64_t sessionMs, json fields, const char* source) {
		if (!Enabled()) return;
		fields["session_ms"] = sessionMs;
		fields["decision_clock"] = "authority_session";
		fields["decision_source"] = source;
		Emit(event, std::move(fields));
	}

	void NetA7Journal::Gap(const char* reason) {
		if (!Enabled()) return;
		++s_State.gaps;
		Emit("evidence_gap", {{"reason", reason}});
	}

	bool NetA7Journal::Seal(const std::string& reportPath, int exitCode) {
		if (!s_State.enabled.exchange(false)) return true;
		while (s_State.producers.load() != 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		const std::string sha = reportPath.empty() ? std::string() : FileSha(reportPath);
		if (sha.empty()) ++s_State.gaps;
		else if (!Enqueue(json{{"event", "terminal"}, {"captured_ms", Elapsed()}, {"exit_code", exitCode}, {"report_sha256", sha}}.dump())) ++s_State.gaps;
		s_State.stop.store(true);
		if (s_State.writer.joinable()) s_State.writer.join();
		const bool ok = !s_State.failed.load() && s_State.gaps.load() == 0;
		if (s_State.file) { if (std::fclose(s_State.file) != 0) s_State.failed.store(true); s_State.file = nullptr; }
		return ok && !s_State.failed.load();
	}

	std::string NetA7Journal::Hex(const uint8_t* bytes, size_t size) {
		static constexpr char digits[] = "0123456789abcdef";
		std::string text(size * 2, '0');
		for (size_t i = 0; i < size; ++i) { text[i * 2] = digits[bytes[i] >> 4]; text[i * 2 + 1] = digits[bytes[i] & 15]; }
		return text;
	}

	std::string NetA7Journal::Sha256(const uint8_t* bytes, size_t size) {
	#ifdef CCCP_WITH_GNS
		std::array<uint8_t, 32> digest{};
		unsigned int outputSize = 0;
		if (EVP_Digest(bytes, size, digest.data(), &outputSize, EVP_sha256(), nullptr) == 1 && outputSize == digest.size()) return Hex(digest.data(), digest.size());
	#endif
		return {};
	}

	bool NetA7Journal::HasConnectGate() { return Enabled() && !s_State.gate.empty() && !s_State.gateReleased.load(); }

	bool NetA7Journal::WaitForConnectGate(const std::string& loadedTicketSha, std::string* error) {
		if (!HasConnectGate()) return true;
		Emit("connect_waiting", {{"ticket_sha256", loadedTicketSha}, {"load_phase", "preconnect_validation"}});
		const auto opened = Clock::now();
		while (Clock::now() - opened < std::chrono::milliseconds(c_GateBudgetMs)) {
			if (s_State.cancelled && s_State.cancelled()) { if (error) *error = "A7 connect gate cancelled"; return false; }
			std::error_code fsError;
			if (std::filesystem::exists(s_State.gate, fsError) && !fsError) {
				if (std::filesystem::file_size(s_State.gate, fsError) > 16384 || fsError) break;
				std::ifstream input(s_State.gate, std::ios::binary);
				const json gate = json::parse(input, nullptr, false);
				if (!gate.is_object() || !gate.contains("schema") || !gate["schema"].is_number_integer() || gate["schema"] != 1 || !gate.contains("run_id") || !gate["run_id"].is_string() || gate["run_id"] != s_State.run || !gate.contains("peers") || !gate["peers"].is_array()) break;
				bool includesPeer = false;
				std::vector<std::string> seen;
				for (const auto& entry: gate["peers"]) {
					if (!entry.is_string() || !Token(entry.get<std::string>())) { seen.clear(); break; }
					const std::string name = entry.get<std::string>();
					if (std::find(seen.begin(), seen.end(), name) != seen.end()) { seen.clear(); break; }
					seen.push_back(name);
					includesPeer = includesPeer || name == s_State.peer;
				}
				if (!seen.empty() && includesPeer) { s_State.gateReleased.store(true); Emit("connect_released"); return true; }
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		Gap("connect gate invalid or its bounded wait expired");
		if (error) *error = "A7 connect gate invalid or timed out";
		return false;
	}

	bool NetA7Journal::ControlledSilentClient() { return Enabled() && s_State.heartbeatTag != 0 && FaultInjected("client_never_says_hello"); }
	uint32_t NetA7Journal::SilentHeartbeatTag() { return s_State.heartbeatTag; }
	uint64_t NetA7Journal::SilentHeartbeatMs() { return s_State.heartbeatMs; }
	uint64_t NetA7Journal::SilentReceiveBudgetMs() { return s_State.receiveBudgetMs; }

	void NetA7Journal::SetSeatView(json seats, uint64_t sessionMs, const char* source) {
		if (!Enabled()) return;
		t_Seats = std::move(seats);
		t_SeatSessionMs = sessionMs;
		t_SeatSource = source;
	}

	void NetA7Journal::AppliedTick(uint64_t round, uint64_t frame, uint8_t peerId, const std::string& sharedHash) {
		if (!Enabled()) return;
		if (round == 0 || frame == 0 || peerId == 0) { Gap("applied tick lacks a live round, frame or peer"); return; }
		if (t_Round != round) {
			t_Round = round;
			t_Frame = frame - 1;
			Emit("running", {{"round_id", round}, {"frame", frame}, {"peer_id", peerId}});
		}
		if (frame != t_Frame + 1) Gap("applied frame coverage is not consecutive");
		t_Frame = frame;
		s_State.frame.store(frame);
		Emit("progress", {{"round_id", round}, {"frame", frame}, {"shared_hash", sharedHash},
			{"seats", t_Seats}, {"seat_source", t_SeatSource}, {"seat_observed_session_ms", t_SeatSessionMs}});
	}

	uint64_t NetA7Journal::AppliedFrame() { return s_State.frame.load(); }
	uint64_t NetA7Journal::BeginResync() { return Enabled() ? s_State.resync.fetch_add(1) + 1 : 0; }
	uint64_t NetA7Journal::CurrentResync() { return s_State.resync.load(); }

} // namespace RTE
