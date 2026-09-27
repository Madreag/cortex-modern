#include "StallStackSampler.h"

#include "System.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#endif

namespace RTE {

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
		s_Watcher = std::jthread([](std::stop_token stop) { Watch(stop); });
		System::PrintDiagnosticLine("[stall-stack] armed: a tick past " + std::to_string(threshold) + " ms is sampled every " + std::to_string(threshold) + " ms");
#else
		(void)threshold;
#endif
	}

	void StallStackSampler::Watch(std::stop_token stop) {
		int64_t sampledBegin = 0;
		int64_t nextSampleMs = 0;
		while (!stop.stop_requested()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
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
			const std::string frames = Sample();
			// A tick that ended while the stack was read is not reported.
			if (s_BeganMs.load(std::memory_order_acquire) != began) {
				continue;
			}
			System::PrintDiagnosticLine("[stall-stack] tick=" + std::to_string(s_Tick.load(std::memory_order_relaxed)) + " after_ms=" + std::to_string(now - began) + " " + frames);
		}
	}

	std::string StallStackSampler::Sample() {
#ifdef _WIN32
		HANDLE thread = static_cast<HANDLE>(s_Thread);
		constexpr int c_MaxFrames = 24;
		DWORD64 addresses[c_MaxFrames] = {};
		int count = 0;
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
		static bool symbols = false;
		HANDLE process = GetCurrentProcess();
		if (!symbols) {
			SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
			symbols = SymInitialize(process, nullptr, TRUE) || GetLastError() == ERROR_INVALID_PARAMETER;
		}
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
		return "unsupported";
#endif
	}
} // namespace RTE
