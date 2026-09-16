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
		class Sha256 {
		public:
			void Add(const char* bytes, size_t size) {
				m_Bytes += size;
				while (size) {
					const size_t count = std::min(size, m_Block.size() - m_Used);
					std::copy_n(reinterpret_cast<const uint8_t*>(bytes), count, m_Block.data() + m_Used);
					m_Used += count;
					bytes += count;
					size -= count;
					if (m_Used == m_Block.size()) { Compress(); m_Used = 0; }
				}
			}
			std::string Finish() {
				const uint64_t bits = m_Bytes * 8;
				m_Block[m_Used++] = 0x80;
				if (m_Used > 56) {
					std::fill(m_Block.begin() + m_Used, m_Block.end(), 0);
					Compress();
					m_Used = 0;
				}
				std::fill(m_Block.begin() + m_Used, m_Block.begin() + 56, 0);
				for (size_t index = 0; index < 8; ++index) m_Block[63 - index] = static_cast<uint8_t>(bits >> (index * 8));
				Compress();
				std::string result;
				for (uint32_t word: m_State) {
					for (int shift = 28; shift >= 0; shift -= 4) result += "0123456789abcdef"[(word >> shift) & 15];
				}
				return result;
			}
		private:
			std::array<uint32_t, 8> m_State{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
			std::array<uint8_t, 64> m_Block{};
			size_t m_Used = 0;
			uint64_t m_Bytes = 0;
			void Compress() {
				static constexpr std::array<uint32_t, 64> constants{
					0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
					0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
					0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
					0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
					0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
					0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
					0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
					0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
				std::array<uint32_t, 64> words{};
				for (size_t index = 0; index < 16; ++index) {
					for (size_t byte = 0; byte < 4; ++byte) words[index] = (words[index] << 8) | m_Block[index * 4 + byte];
				}
				for (size_t index = 16; index < words.size(); ++index) {
					const uint32_t a = words[index - 15], b = words[index - 2];
					words[index] = words[index - 16] + (std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3)) +
					               words[index - 7] + (std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10));
				}
				auto [a, b, c, d, e, f, g, h] = m_State;
				for (size_t index = 0; index < words.size(); ++index) {
					const uint32_t first = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + ((e & f) ^ (~e & g)) + constants[index] + words[index];
					const uint32_t second = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
					h = g; g = f; f = e; e = d + first; d = c; c = b; b = a; a = first + second;
				}
				const std::array<uint32_t, 8> work{a, b, c, d, e, f, g, h};
				for (size_t index = 0; index < work.size(); ++index) m_State[index] += work[index];
			}
		};

		std::string Digest(const std::string& bytes) {
			Sha256 hash;
			hash.Add(bytes.data(), bytes.size());
			return hash.Finish();
		}

		std::string FileDigest(const std::filesystem::path& path) {
			std::ifstream file(path, std::ios::binary);
			if (!file) throw std::runtime_error("could not read executable");
			Sha256 hash;
			std::array<char, 65536> buffer{};
			while (file) {
				file.read(buffer.data(), buffer.size());
				hash.Add(buffer.data(), static_cast<size_t>(file.gcount()));
			}
			if (!file.eof()) throw std::runtime_error("could not hash executable");
			return hash.Finish();
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

		std::filesystem::path ExecutablePath() {
#ifdef _WIN32
			std::array<wchar_t, 32768> path{};
			const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
			if (size && size < path.size()) return std::wstring(path.data(), size);
#elif defined(__APPLE__)
			uint32_t size = 0;
			_NSGetExecutablePath(nullptr, &size);
			std::vector<char> path(size);
			if (_NSGetExecutablePath(path.data(), &size) == 0) return path.data();
#else
			std::error_code error;
			return std::filesystem::read_symlink("/proc/self/exe", error);
#endif
			return {};
		}

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
			std::filesystem::path runtime, executable;
			std::string gpu;
			~State() { TelemetryBundle::Finish(); }
		} s_State;

		void WriteBundle(Job job) {
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
			add("Executable.json", json{{"sha256", FileDigest(s_State.executable)}, {"version", c_VersionString}}.dump(2));
			json manifest{{"schema", 1}, {"members", json::array()}, {"omitted", omissions}, {"replay", replayStatus},
			              {"identity_build_ms", job.snapshot.identityBuildMs}, {"redacted", redacted}};
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
			std::cout << "[telemetry] saved " + path.generic_string() + "\n" << std::flush;
			g_ConsoleMan.PrintString("SYSTEM: Diagnostics saved to " + path.generic_string());
		}
	}

	void TelemetryBundle::Initialize(const std::string& gpu) {
		if (s_State.worker.joinable()) return;
		s_State.runtime = System::GetWorkingDirectory();
		s_State.executable = ExecutablePath();
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
				std::string error;
				if (g_NetMatchService.RefreshDiagnosticIdentity(&error, &snapshot.identityBuildMs)) {
					std::cout << std::format("[telemetry] identity built in {:.3f} ms\n", snapshot.identityBuildMs) << std::flush;
					snapshot.joinIdentity = g_NetMatchService.ExportDiagnosticIdentity();
				} else {
					snapshot.joinIdentity = json{{"error", error}}.dump(2);
				}
			}
			if (snapshot.joinIdentity.empty()) snapshot.joinIdentity = json{{"error", "identity unavailable before module loading completes"}}.dump(2);
			snapshot.desyncHeal = g_NetMatchService.ExportDiagnosticDesyncHeal();
			if (ScenarioRunner::CopyLockstepReplayForDiagnostics(snapshot.replay, snapshot.replayTruncated)) {
				snapshot.replayReason = snapshot.replayTruncated ? "complete-record prefix at member limit" : "complete recorded ticks";
			} else if (snapshot.replayTruncated) {
				snapshot.replayReason = "no complete replay frame fits the member limit";
			}
			return Request(std::move(snapshot));
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
