#include "TelemetryBundle.h"

#include "GameVersion.h"
#include "System.h"
#include "ConsoleMan.h"
#include "NetMatchService.h"
#include "ScenarioRunner.h"

#ifdef SYSTEM_MINIZIP
#include <minizip/zip.h>
#else
#include "zip.h"
#endif

#include "SDL3/SDL_cpuinfo.h"
#include "SDL3/SDL_platform.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <mutex>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <intrin.h>
#else
#include <sys/utsname.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif

namespace RTE {
	using json = nlohmann::json;

	namespace {
		std::string ThreadIdText(std::thread::id id) {
			std::ostringstream text;
			text << id;
			return text.str();
		}

		std::string Digest(const std::string& bytes) {
			return System::Sha256Hex(bytes.data(), bytes.size());
		}

		struct LogTail {
			std::mutex mutex;
			std::array<char, TelemetryBundle::c_LogTailLimit> bytes{};
			size_t next = 0, used = 0;
			void Append(const char* data, size_t size) {
				std::lock_guard lock(mutex);
				if (size > bytes.size()) { data += size - bytes.size(); size = bytes.size(); }
				const size_t first = std::min(size, bytes.size() - next);
				std::copy_n(data, first, bytes.data() + next);
				std::copy_n(data + first, size - first, bytes.data());
				next = (next + size) % bytes.size();
				used = std::min(used + size, bytes.size());
			}
			std::string Copy() {
				std::lock_guard lock(mutex);
				const size_t start = (next + bytes.size() - used) % bytes.size();
				const size_t first = std::min(used, bytes.size() - start);
				return std::string(bytes.data() + start, first) + std::string(bytes.data(), used - first);
			}
		};

		class LogMirror : public std::streambuf {
		public:
			LogMirror(std::streambuf* original, LogTail& tail) : m_Original(original), m_Tail(tail) {}
			std::streambuf* Original() const { return m_Original; }
		protected:
			std::streamsize xsputn(const char* data, std::streamsize size) override {
				if (size > 0) m_Tail.Append(data, static_cast<size_t>(size));
				return m_Original->sputn(data, size);
			}
			int_type overflow(int_type ch) override {
				if (traits_type::eq_int_type(ch, traits_type::eof())) return traits_type::not_eof(ch);
				const char value = traits_type::to_char_type(ch);
				m_Tail.Append(&value, 1);
				return m_Original->sputc(value);
			}
			int sync() override { return m_Original->pubsync(); }
		private:
			std::streambuf* m_Original;
			LogTail& m_Tail;
		};

		json SystemInfo(const std::string& gpu) {
			std::string os = SDL_GetPlatform(), cpu = "unknown";
#ifdef _WIN32
			using VersionQuery = LONG(WINAPI*)(OSVERSIONINFOW*);
			OSVERSIONINFOW version{};
			version.dwOSVersionInfoSize = sizeof(version);
			const auto query = reinterpret_cast<VersionQuery>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
			if (query && query(&version) == 0) os += " " + std::to_string(version.dwMajorVersion) + "." + std::to_string(version.dwMinorVersion) + "." + std::to_string(version.dwBuildNumber);
#if defined(_M_X64) || defined(_M_IX86)
			std::array<int, 4> registers{};
			__cpuid(registers.data(), static_cast<int>(0x80000000U));
			if (static_cast<uint32_t>(registers[0]) >= 0x80000004U) {
				std::array<char, 49> brand{};
				for (unsigned int index = 0; index < 3; ++index) {
					__cpuid(registers.data(), static_cast<int>(0x80000002U + index));
					std::copy_n(reinterpret_cast<const char*>(registers.data()), 16, brand.data() + index * 16);
				}
				cpu = brand.data();
			}
#endif
#else
			utsname version{};
			if (uname(&version) == 0) { os += " " + std::string(version.release); cpu = version.machine; }
#ifdef __APPLE__
			std::array<char, 256> brand{};
			size_t size = brand.size();
			if (sysctlbyname("machdep.cpu.brand_string", brand.data(), &size, nullptr, 0) == 0 && size > 0) cpu = brand.data();
#else
			std::ifstream info("/proc/cpuinfo");
			std::string line;
			while (std::getline(info, line)) {
				if (line.starts_with("model name") && line.find(':') != std::string::npos) { cpu = line.substr(line.find(':') + 1); break; }
			}
#endif
#endif
			return {{"os", os}, {"cpu", cpu}, {"gpu", gpu.empty() ? "unavailable" : gpu}, {"memory_mb", SDL_GetSystemRAM()}};
		}

		bool SecretSettingsKey(std::string_view name) {
			if (name == "SessionDirectoryInstallKey" || name == "NetworkTurnPass" || name == "NetworkTurnUser" ||
			    name == "SessionDirectoryCertSha256") {
				return true;
			}
			static constexpr std::string_view needles[] = {"Pass", "Password", "Secret", "Token", "PrivateKey", "Credential", "Ticket"};
			for (std::string_view needle: needles) {
				if (name.find(needle) != std::string_view::npos) return true;
			}
			return false;
		}

		std::string RedactSettingsValue(std::string_view name, std::string_view value) {
			if (name != "SessionDirectoryCertSha256") return "<redacted>";
			std::string hex;
			for (unsigned char ch: value) {
				if (std::isxdigit(ch)) {
					hex.push_back(static_cast<char>(ch));
					if (hex.size() == 8) break;
				}
			}
			hex += "\xE2\x80\xA6";
			return hex;
		}

		// Archive member only; the user's Settings.ini is not written.
		std::string FilterSettingsMember(const std::string& bytes, std::vector<std::string>& redacted) {
			std::string out;
			out.reserve(bytes.size());
			size_t offset = 0;
			while (offset < bytes.size()) {
				const size_t nl = bytes.find('\n', offset);
				const size_t next = (nl == std::string::npos) ? bytes.size() : nl + 1;
				const size_t ending = (nl != std::string::npos && nl > offset && bytes[nl - 1] == '\r') ? 2 : (nl != std::string::npos ? 1 : 0);
				const size_t lineLen = next - offset - ending;
				const std::string_view line(bytes.data() + offset, lineLen);
				size_t indent = 0;
				while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t')) ++indent;
				const std::string_view body = line.substr(indent);
				const size_t eq = (body.empty() || body[0] == '/' || body[0] == ';' || body[0] == '#') ? std::string_view::npos : body.find('=');
				if (eq == std::string_view::npos) {
					out.append(bytes, offset, next - offset);
					offset = next;
					continue;
				}
				size_t keyEnd = eq;
				while (keyEnd > 0 && (body[keyEnd - 1] == ' ' || body[keyEnd - 1] == '\t')) --keyEnd;
				const std::string key(body.substr(0, keyEnd));
				if (!SecretSettingsKey(key)) {
					out.append(bytes, offset, next - offset);
					offset = next;
					continue;
				}
				size_t valueStart = eq + 1;
				while (valueStart < body.size() && (body[valueStart] == ' ' || body[valueStart] == '\t')) ++valueStart;
				out.append(bytes, offset, indent + valueStart);
				out += RedactSettingsValue(key, body.substr(valueStart));
				out.append(bytes, offset + lineLen, ending);
				if (std::find(redacted.begin(), redacted.end(), key) == redacted.end()) redacted.push_back(key);
				offset = next;
			}
			return out;
		}

		std::string Stamp() {
			const std::time_t now = std::time(nullptr) - 7 * 60 * 60;
			std::tm local{};
#ifdef _WIN32
			gmtime_s(&local, &now);
#else
			gmtime_r(&now, &local);
#endif
			std::array<char, 32> text{};
			std::strftime(text.data(), text.size(), "%Y%m%d-%H%M%S", &local);
			return text.data();
		}

		struct Job {
			TelemetryBundle::Snapshot snapshot;
			std::string network;
			std::string gpu;
		};
		struct State {
			LogTail log;
			std::unique_ptr<LogMirror> out, err;
			std::mutex mutex;
			std::condition_variable ready;
			std::optional<Job> job;
			bool busy = false, stop = false, captureRequested = false, lastSucceeded = true;
			std::thread worker;
			std::filesystem::path runtime;
			std::string gpu, mainThread;
			~State() { TelemetryBundle::Finish(); }
		} s_State;

		void WriteBundle(Job job) {
			std::string identityThread;
			bool identityBuilt = true;
			if (job.snapshot.identityPending) {
				// The module hashing costs about a second: it runs here, never on the frame path. The thread
				// is named before the attempt, so a failed build cannot read as a build on the game thread.
				std::string identityError;
				identityThread = ThreadIdText(std::this_thread::get_id());
				if (g_NetMatchService.BuildCapturedDiagnosticIdentity(&identityError, &job.snapshot.identityBuildMs)) {
					job.snapshot.joinIdentity = g_NetMatchService.ExportDiagnosticIdentity();
					System::PrintDiagnosticLine(std::format("[telemetry] identity built in {:.3f} ms", job.snapshot.identityBuildMs));
				} else {
					identityBuilt = false;
					job.snapshot.joinIdentity = json{{"error", identityError}}.dump(2);
				}
			}
			if (job.snapshot.joinIdentity.empty()) job.snapshot.joinIdentity = json{{"error", "identity unavailable before module loading completes"}}.dump(2);
			std::vector<std::pair<std::string, std::string>> members;
			json omissions = json::array();
			const auto add = [&](const std::string& name, std::string data) {
				if (data.size() > TelemetryBundle::c_MemberLimit) {
					omissions.push_back({{"name", name}, {"reason", "member exceeds 8 MiB"}, {"size", data.size()}});
				} else members.emplace_back(name, std::move(data));
			};
			if (job.snapshot.consoleTail.size() > TelemetryBundle::c_LogTailLimit) job.snapshot.consoleTail.erase(0, job.snapshot.consoleTail.size() - TelemetryBundle::c_LogTailLimit);
			add("LogConsole.txt", std::move(job.snapshot.consoleTail));
			std::istringstream network(job.network);
			std::string line, filtered;
			while (std::getline(network, line)) {
				if (line.find("[net-match]") != std::string::npos || line.find("[net-lockstep]") != std::string::npos || line.find("[net-session]") != std::string::npos) filtered += line + "\n";
			}
			if (filtered.size() > TelemetryBundle::c_LogTailLimit) filtered.erase(0, filtered.size() - TelemetryBundle::c_LogTailLimit);
			add("NetMatch.log", std::move(filtered));
			add("JoinIdentity.json", std::move(job.snapshot.joinIdentity));
			add("DesyncHeal.json", std::move(job.snapshot.desyncHeal));
			const bool replayIncluded = !job.snapshot.replay.empty() && job.snapshot.replay.size() <= TelemetryBundle::c_MemberLimit;
			const json replayStatus{{"included", replayIncluded}, {"truncated", job.snapshot.replayTruncated}, {"reason", job.snapshot.replayReason}};
			add("Replay.status.json", replayStatus.dump(2));
			if (!job.snapshot.replay.empty()) add("Replay.ccrp", std::move(job.snapshot.replay));
			std::vector<std::string> redacted;
			std::ifstream settings(s_State.runtime / "Userdata" / "Settings.ini", std::ios::binary);
			if (settings) {
				std::string bytes(TelemetryBundle::c_MemberLimit + 1, '\0');
				settings.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
				bytes.resize(static_cast<size_t>(settings.gcount()));
				add("Settings.ini", FilterSettingsMember(bytes, redacted));
			} else omissions.push_back({{"name", "Settings.ini"}, {"reason", "file unavailable"}});
			add("SystemInfo.json", SystemInfo(job.gpu).dump(2));
			add("Executable.json", json{{"sha256", System::GetThisExeSha256()}, {"version", c_VersionString}}.dump(2));
			json manifest{{"schema", 1}, {"members", json::array()}, {"omitted", omissions}, {"replay", replayStatus},
			              {"identity_build_ms", job.snapshot.identityBuildMs}, {"main_thread_id", s_State.mainThread},
			              {"identity_thread_id", identityThread}, {"identity_build_ok", identityBuilt}, {"redacted", redacted}};
			for (const auto& [name, data]: members) {
				json entry{{"name", name}, {"size", data.size()}, {"sha256", Digest(data)}};
				if (name == "Replay.ccrp") entry["truncated"] = job.snapshot.replayTruncated;
				manifest["members"].push_back(std::move(entry));
			}
			add("manifest.json", manifest.dump(2));
			const auto directory = s_State.runtime / "Telemetry";
			std::filesystem::create_directories(directory);
			std::filesystem::path path;
			do {
				path = directory / ("diag-" + Stamp() + ".zip");
				if (!std::filesystem::exists(path)) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			} while (true);
			const auto temporary = std::filesystem::path(path.string() + ".tmp." + std::to_string(System::GetProcessID()));
			struct Archive {
				std::filesystem::path path;
				zipFile file;
				~Archive() {
					if (file) zipClose(file, nullptr);
					std::error_code ignored;
					std::filesystem::remove(path, ignored);
				}
			} archive{temporary, zipOpen(temporary.string().c_str(), APPEND_STATUS_CREATE)};
			if (!archive.file) throw std::runtime_error("could not create diagnostics archive");
			for (const auto& [name, data]: members) {
				zip_fileinfo info{};
#ifdef SYSTEM_MINIZIP
				const auto openEntry = zipOpenNewFileInZip64;
#else
				const auto openEntry = zipOpenNewFileInZip_64;
#endif
				if (openEntry(archive.file, name.c_str(), &info, nullptr, 0, nullptr, 0, nullptr, 8, 2, 0) != ZIP_OK ||
				    zipWriteInFileInZip(archive.file, data.data(), static_cast<unsigned int>(data.size())) != ZIP_OK || zipCloseFileInZip(archive.file) != ZIP_OK) throw std::runtime_error("could not write " + name);
			}
			const int closed = zipClose(archive.file, nullptr);
			archive.file = nullptr;
			if (closed != ZIP_OK) throw std::runtime_error("could not finish diagnostics archive");
			std::filesystem::rename(temporary, path);
			System::PrintDiagnosticLine("[telemetry] saved " + path.generic_string());
			g_ConsoleMan.PrintString("SYSTEM: Diagnostics saved to " + path.generic_string());
		}
	}

	void TelemetryBundle::Initialize(const std::string& gpu) {
		if (s_State.worker.joinable()) return;
		s_State.runtime = System::GetWorkingDirectory();
		s_State.mainThread = ThreadIdText(std::this_thread::get_id());
		s_State.gpu = gpu;
		s_State.stop = false;
		s_State.out = std::make_unique<LogMirror>(std::cout.rdbuf(), s_State.log);
		s_State.err = std::make_unique<LogMirror>(std::cerr.rdbuf(), s_State.log);
		std::cout.rdbuf(s_State.out.get());
		std::cerr.rdbuf(s_State.err.get());
		s_State.worker = std::thread([] {
			while (true) {
				Job job;
				{
					std::unique_lock lock(s_State.mutex);
					s_State.ready.wait(lock, [] { return s_State.stop || s_State.job.has_value(); });
					if (!s_State.job) return;
					job = std::move(*s_State.job);
					s_State.job.reset();
				}
				bool saved = true;
				try { WriteBundle(std::move(job)); }
				catch (const std::exception& error) {
					saved = false;
					std::cerr << std::string("[telemetry] failed: ") + error.what() + "\n" << std::flush;
					g_ConsoleMan.PrintString("ERROR: Could not save diagnostics: " + std::string(error.what()));
				}
				std::lock_guard lock(s_State.mutex);
				s_State.busy = false;
				s_State.lastSucceeded = saved;
				s_State.ready.notify_all();
			}
		});
	}

	void TelemetryBundle::SetGpuDescription(const std::string& gpu) {
		std::lock_guard lock(s_State.mutex);
		s_State.gpu = gpu;
	}

	bool TelemetryBundle::Request(Snapshot snapshot) {
		std::lock_guard lock(s_State.mutex);
		if (!s_State.worker.joinable() || s_State.stop || s_State.busy) return false;
		s_State.job.emplace(Job{std::move(snapshot), s_State.log.Copy(), s_State.gpu});
		s_State.busy = true;
		s_State.ready.notify_one();
		return true;
	}

	bool TelemetryBundle::RequestCapture() {
		std::lock_guard lock(s_State.mutex);
		if (!s_State.worker.joinable() || s_State.stop || s_State.busy || s_State.captureRequested) return false;
		s_State.captureRequested = true;
		return true;
	}

	bool TelemetryBundle::CaptureAtTickBoundary() {
		{
			std::lock_guard lock(s_State.mutex);
			if (!s_State.captureRequested || s_State.busy || s_State.stop) return false;
			s_State.captureRequested = false;
		}
		try {
			Snapshot snapshot;
			snapshot.consoleTail = g_ConsoleMan.CopyLogTail(c_LogTailLimit);
			snapshot.joinIdentity = g_NetMatchService.ExportDiagnosticIdentity();
			if (snapshot.joinIdentity.empty()) {
				// Only the manager reads happen here; the worker hashes the modules and caches the identity.
				std::string error;
				snapshot.identityPending = g_NetMatchService.CaptureDiagnosticIdentityInputs(&error);
				if (!snapshot.identityPending) snapshot.joinIdentity = json{{"error", error}}.dump(2);
			}
			snapshot.desyncHeal = g_NetMatchService.ExportDiagnosticDesyncHeal();
			if (ScenarioRunner::CopyLockstepReplayForDiagnostics(snapshot.replay, snapshot.replayTruncated)) {
				snapshot.replayReason = snapshot.replayTruncated ? "complete-record prefix at member limit" : "complete recorded ticks";
			} else if (snapshot.replayTruncated) {
				snapshot.replayReason = "no complete replay frame fits the member limit";
			}
			const bool identityPending = snapshot.identityPending;
			const bool queued = Request(std::move(snapshot));
			// Nothing will build inputs no bundle carries, so they are not left on the service.
			if (!queued && identityPending) g_NetMatchService.DropCapturedDiagnosticIdentityInputs();
			return queued;
		} catch (const std::exception& error) {
			g_ConsoleMan.PrintString("ERROR: Could not capture diagnostics: " + std::string(error.what()));
			std::lock_guard lock(s_State.mutex);
			s_State.lastSucceeded = false;
			return false;
		}
	}

	bool TelemetryBundle::IsBusy() {
		std::lock_guard lock(s_State.mutex);
		return s_State.busy || s_State.captureRequested;
	}

	bool TelemetryBundle::Flush() {
		std::unique_lock lock(s_State.mutex);
		s_State.ready.wait(lock, [] { return !s_State.busy; });
		return s_State.lastSucceeded;
	}

	void TelemetryBundle::Finish() {
		{
			std::lock_guard lock(s_State.mutex);
			s_State.stop = true;
		}
		s_State.ready.notify_one();
		if (s_State.worker.joinable()) s_State.worker.join();
		if (s_State.out) { std::cout.rdbuf(s_State.out->Original()); s_State.out.reset(); }
		if (s_State.err) { std::cerr.rdbuf(s_State.err->Original()); s_State.err.reset(); }
	}
}
