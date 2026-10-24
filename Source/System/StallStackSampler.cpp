#include "StallStackSampler.h"
#include "SteadyClock.h"
#include "FloatingPointEnvironment.h"

#include "System.h"
#include "HarnessCost.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>
#endif

namespace RTE {

#ifdef _WIN32
	namespace {
		// The CPU cycles each of this process's threads has run so far.
		std::vector<std::pair<DWORD, ULONG64>> ThreadCycles() {
			std::vector<std::pair<DWORD, ULONG64>> cycles;
			const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snapshot == INVALID_HANDLE_VALUE) return cycles;
			THREADENTRY32 entry{};
			entry.dwSize = sizeof(entry);
			const DWORD process = GetCurrentProcessId();
			for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
				if (entry.th32OwnerProcessID != process) continue;
				if (const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID)) {
					ULONG64 count = 0;
					if (QueryThreadCycleTime(thread, &count)) cycles.emplace_back(entry.th32ThreadID, count);
					CloseHandle(thread);
				}
			}
			CloseHandle(snapshot);
			return cycles;
		}
	}
#endif

	void StallStackSampler::ArmForCurrentThread() {
		const char* text = std::getenv("CCCP_STALL_STACK_MS");
		const long long threshold = text ? std::atoll(text) : 0;
#ifdef _WIN32
		if (threshold <= 0 || s_Watcher.joinable()) {
			return;
		}
		HANDLE thread = nullptr;
		if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
			return;
		}
		s_Thread = thread;
		s_ThresholdMs = threshold;
		HarnessCost::SetEnabled(HarnessCost::StallSampler, true);
		// The symbols load here, before any frame runs, so no sample pays for them inside a tick; the load is the sampler's own cost.
		const auto loadStart = RTE::SteadyClock::now();
		SymSetOptions(SYMOPT_UNDNAME);
		s_Symbols = SymInitialize(GetCurrentProcess(), nullptr, TRUE) || GetLastError() == ERROR_INVALID_PARAMETER;
		// A module's symbols load on its first lookup: one lookup in each module now keeps that out of every sample.
		if (s_Symbols) {
			SymEnumerateModules64(GetCurrentProcess(), [](PCSTR, DWORD64 base, PVOID) -> BOOL {
				char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
				auto* symbol = reinterpret_cast<PSYMBOL_INFO>(buffer);
				symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
				symbol->MaxNameLen = MAX_SYM_NAME;
				DWORD64 displacement = 0;
				SymFromAddr(GetCurrentProcess(), base + 0x1000, &displacement, symbol);
				return TRUE;
			}, nullptr);
		}
		const int64_t loadNs = std::chrono::duration_cast<std::chrono::nanoseconds>(RTE::SteadyClock::now() - loadStart).count();
		HarnessCost::Charge(HarnessCost::StallSampler, loadNs);
		s_Watcher = std::jthread([](std::stop_token stop) { FloatingPointEnvironment::Initialize(); const FloatingPointEnvironment::Scope scope("stack sampler"); Watch(stop); });
		System::PrintDiagnosticLine("[stall-stack] armed: a tick past " + std::to_string(threshold) + " ms is sampled every " + std::to_string(threshold) +
		                            " ms; symbols " + (s_Symbols ? "loaded" : "unavailable") + " in " + std::to_string(loadNs / 1000000) + " ms before the first frame");
#else
		(void)threshold;
#endif
	}

	void StallStackSampler::Watch(std::stop_token stop) {
		int64_t sampledBegin = 0;
		int64_t nextSampleMs = 0;
#ifdef _WIN32
		// Every thread's cycles, every 50 ms for the last few seconds: a long tick is compared with the count from before it began.
		std::deque<std::pair<int64_t, std::vector<std::pair<DWORD, ULONG64>>>> counts;
		uint64_t reportedLongTicks = s_LongTicks.load(std::memory_order_acquire);
#endif
		while (!stop.stop_requested()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
#ifdef _WIN32
			if (const int64_t now = NowMs(); counts.empty() || now - counts.back().first >= 50) {
				const auto started = RTE::SteadyClock::now();
				counts.emplace_back(now, ThreadCycles());
				if (counts.size() > 100) counts.pop_front();
				HarnessCost::Charge(HarnessCost::StallSampler, std::chrono::duration_cast<std::chrono::nanoseconds>(RTE::SteadyClock::now() - started).count());
			}
			if (const uint64_t longTicks = s_LongTicks.load(std::memory_order_acquire); longTicks != reportedLongTicks) {
				reportedLongTicks = longTicks;
				const auto started = RTE::SteadyClock::now();
				const int64_t longBegan = s_LongTickBeganMs.load(std::memory_order_relaxed);
				auto before = counts.begin();
				for (auto count = counts.begin(); count != counts.end() && count->first <= longBegan; ++count) before = count;
				std::unordered_map<DWORD, ULONG64> earlier(before->second.begin(), before->second.end());
				std::vector<std::pair<ULONG64, DWORD>> ran;
				for (const auto& [thread, cycles]: ThreadCycles()) {
					const auto found = earlier.find(thread);
					ran.emplace_back(cycles - (found != earlier.end() ? found->second : 0), thread);
				}
				std::sort(ran.rbegin(), ran.rend());
				const DWORD own = GetCurrentThreadId(), simulation = GetThreadId(static_cast<HANDLE>(s_Thread));
				for (size_t index = 0; index < ran.size() && index < 5; ++index) {
					const auto [cycles, id] = ran[index];
					std::string stack = "not sampled";
					int priority = THREAD_PRIORITY_ERROR_RETURN;
					if (const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id)) {
						priority = GetThreadPriority(thread);
						if (id != own && id != simulation) stack = Sample(thread, false);
						CloseHandle(thread);
					}
					System::PrintDiagnosticLine(std::format("[stall-threads] tick={} tick_ms={} since_ms={} thread={} priority={} mcycles={} {}{} at {}", s_LongTickTick.load(std::memory_order_relaxed),
					    s_LongTickMs.load(std::memory_order_relaxed), NowMs() - before->first, id, priority, cycles / 1000000, id == simulation ? "simulation" : "", id == own ? "sampler" : "", stack));
				}
				HarnessCost::Charge(HarnessCost::StallSampler, std::chrono::duration_cast<std::chrono::nanoseconds>(RTE::SteadyClock::now() - started).count());
			}
#endif
			const int64_t began = s_BeganMs.load(std::memory_order_acquire);
			if (began == 0) {
				continue;
			}
			const int64_t now = NowMs();
			if (began != sampledBegin) {
				sampledBegin = began;
				nextSampleMs = began + s_ThresholdMs;
			}
			if (now < nextSampleMs) {
				continue;
			}
			nextSampleMs = now + s_ThresholdMs;
			const auto sampleStart = RTE::SteadyClock::now();
			const std::string frames = Sample();
			const int64_t sampleNs = std::chrono::duration_cast<std::chrono::nanoseconds>(RTE::SteadyClock::now() - sampleStart).count();
			// A tick that ended while the stack was read is not reported.
			// The suspension is charged as it ends; the rest of the sample is the watcher's own work.
			HarnessCost::Charge(HarnessCost::StallSampler, sampleNs - s_LastSuspendNs);
			if (s_BeganMs.load(std::memory_order_acquire) != began) {
				continue;
			}
			System::PrintDiagnosticLine("[stall-stack] tick=" + std::to_string(s_Tick.load(std::memory_order_relaxed)) + " after_ms=" + std::to_string(now - began) + " " + frames);
		}
	}

	std::string StallStackSampler::Sample() {
		return Sample(s_Thread, true);
	}

	std::string StallStackSampler::Sample(void* target, bool simulation) {
#ifdef _WIN32
		HANDLE thread = static_cast<HANDLE>(target);
		constexpr int c_MaxFrames = 24;
		DWORD64 addresses[c_MaxFrames] = {};
		int count = 0;
		if (simulation) s_LastSuspendNs = 0;
		const auto suspended = RTE::SteadyClock::now();
		if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
			return "suspend failed";
		}
		CONTEXT context = {};
		context.ContextFlags = CONTEXT_FULL;
		if (GetThreadContext(thread, &context)) {
			// Unwinding a suspended thread without dbghelp, which may not be called while the target holds the loader lock.
			while (count < c_MaxFrames && context.Rip != 0) {
				addresses[count++] = context.Rip;
				DWORD64 imageBase = 0;
				PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
				if (!function) {
					context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
					context.Rsp += 8;
					continue;
				}
				void* handlerData = nullptr;
				DWORD64 establisherFrame = 0;
				RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisherFrame, nullptr);
			}
		}
		ResumeThread(thread);
		if (simulation) {
			s_LastSuspendNs = std::chrono::duration_cast<std::chrono::nanoseconds>(RTE::SteadyClock::now() - suspended).count();
			HarnessCost::NoteSimulationSuspended(s_LastSuspendNs);
		}
		HANDLE process = GetCurrentProcess();
		const bool symbols = s_Symbols;
		std::ostringstream line;
		for (int index = 0; index < count; ++index) {
			char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
			auto* symbol = reinterpret_cast<PSYMBOL_INFO>(buffer);
			symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen = MAX_SYM_NAME;
			DWORD64 displacement = 0;
			HMODULE module = nullptr;
			char moduleName[MAX_PATH] = "?";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(addresses[index]), &module)) {
				char path[MAX_PATH] = {};
				GetModuleFileNameA(module, path, MAX_PATH);
				const char* slash = std::strrchr(path, '\\');
				std::snprintf(moduleName, sizeof(moduleName), "%s", slash ? slash + 1 : path);
			}
			line << (index == 0 ? "" : " < ") << moduleName << "!";
			if (symbols && SymFromAddr(process, addresses[index], &displacement, symbol)) {
				line << symbol->Name;
			} else {
				line << std::format("0x{:X}", addresses[index] - reinterpret_cast<DWORD64>(module));
			}
		}
		return line.str();
#else
		(void)target;
		(void)simulation;
		return "unsupported";
#endif
	}
} // namespace RTE
