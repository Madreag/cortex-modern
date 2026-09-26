#include "CaptureSentinel.h"

#include "RTEError.h"
#include "System.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include "Windows.h"
#include <dbghelp.h>
#else
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#endif

using namespace RTE;

namespace {
	constexpr int c_Frames = 32;

	// The first report of an object waits for the derived constructors after it, which name its type better.
	struct PendingCreation {
		const void* object = nullptr;
		const char* type = nullptr;
		const char* task = nullptr;
		void* frames[c_Frames] = {};
		int count = 0;
	};
	thread_local PendingCreation t_Pending;

	std::mutex& ReportMutex() {
		static std::mutex mutex;
		return mutex;
	}

	// Names every frame, under the report lock; symbols load once per report, and only for frames no report named yet.
	std::vector<std::string> FrameNames(void* const* frames, int count) {
		static std::unordered_map<void*, std::string> names;
		std::vector<void*> unnamed;
		for (int index = 0; index < count; ++index) if (!names.contains(frames[index])) unnamed.push_back(frames[index]);
		if (!unnamed.empty()) {
#ifdef _WIN32
			HANDLE process = GetCurrentProcess();
			SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
			// The crash handler initializes the same process's symbols itself, so this one never keeps them; a session the
			// sanitizer runtime already holds is used as it is.
			const bool initialized = SymInitialize(process, nullptr, TRUE);
			const bool usable = initialized || GetLastError() == ERROR_INVALID_PARAMETER;
			const auto image = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
			for (void* frame: unnamed) {
				char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
				auto* symbol = reinterpret_cast<PSYMBOL_INFO>(buffer);
				symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
				symbol->MaxNameLen = MAX_SYM_NAME;
				DWORD64 displacement = 0;
				if (usable && SymFromAddr(process, reinterpret_cast<DWORD64>(frame), &displacement, symbol)) names[frame] = symbol->Name;
				else names[frame] = std::format("?exe+0x{:X}", reinterpret_cast<uintptr_t>(frame) - image);
			}
			if (initialized) SymCleanup(process);
#else
			for (void* frame: unnamed) {
				Dl_info info{};
				std::string name = "?";
				if (dladdr(frame, &info) && info.dli_sname) {
					int status = 0;
					char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
					name = status == 0 && demangled ? demangled : info.dli_sname;
					std::free(demangled);
				}
				names[frame] = name;
			}
#endif
		}
		std::vector<std::string> result;
		result.reserve(count);
		for (int index = 0; index < count; ++index) result.push_back(names[frames[index]]);
		return result;
	}

	// A constructor's own frame, whatever its template arguments or parameters: A::B::B.
	bool IsConstructor(std::string_view name) {
		std::string plain;
		int depth = 0;
		for (char character: name) {
			if (character == '<') ++depth;
			else if (character == '>') --depth;
			else if (character == '(' && depth == 0) break;
			else if (depth == 0) plain.push_back(character);
		}
		const size_t last = plain.rfind("::");
		if (last == std::string::npos || last == 0) return false;
		const size_t previous = plain.rfind("::", last - 1);
		const std::string_view owner = std::string_view(plain).substr(previous == std::string::npos ? 0 : previous + 2, last - (previous == std::string::npos ? 0 : previous + 2));
		return owner == std::string_view(plain).substr(last + 2);
	}

	// The frames a report skips to reach the code that asked for the object.
	bool IsPlumbing(std::string_view name) {
		return name.starts_with("?") || name.find("CaptureSentinel") != std::string_view::npos || IsConstructor(name) || name.starts_with("std::") || name.starts_with("operator new") ||
		       name.find("make_unique") != std::string_view::npos || name.find("make_shared") != std::string_view::npos || name.starts_with("_") || name.starts_with("RtlUser") ||
		       name.find("SeatStub") != std::string_view::npos;
	}
} // namespace

CaptureSentinel::WorkerScope::~WorkerScope() {
	if (t_Pending.type) Flush();
	s_Task = m_Previous;
}

void CaptureSentinel::Enable() {
	s_Enabled.store(true, std::memory_order_relaxed);
	static std::once_flag announced;
	std::call_once(announced, [] { System::PrintDiagnosticLine("[checkpoint-sentinel] armed: engine objects made on a capture's worker threads are reported\n"); });
}

void CaptureSentinel::Report(const char* type, const void* object) {
	if (t_Pending.type && t_Pending.object == object) {
		t_Pending.type = type;
		return;
	}
	if (t_Pending.type) Flush();
	t_Pending.object = object;
	t_Pending.type = type;
	t_Pending.task = s_Task;
#ifdef _WIN32
	t_Pending.count = CaptureStackBackTrace(1, c_Frames, t_Pending.frames, nullptr);
#else
	t_Pending.count = backtrace(t_Pending.frames, c_Frames);
#endif
}

void CaptureSentinel::Flush() {
	const PendingCreation pending = t_Pending;
	t_Pending = PendingCreation();
	if (!pending.type) return;
	std::lock_guard lock(ReportMutex());
	const int hit = s_Hits.fetch_add(1, std::memory_order_acq_rel) + 1;
	const std::vector<std::string> names = FrameNames(pending.frames, pending.count);
	const auto found = std::find_if(names.begin(), names.end(), [](const std::string& name) { return !IsPlumbing(name); });
	const std::string caller = found == names.end() ? "?" : *found;
	// The frames above the caller, or every frame when none could be named.
	std::ostringstream stack;
	int shown = 0;
	for (auto name = found == names.end() ? names.begin() : std::next(found); name != names.end() && shown < 8; ++name) stack << (shown++ ? " <- " : "") << *name;
	std::ostringstream thread;
	thread << std::this_thread::get_id();
	const std::string message = std::format("engine object created on a capture thread: {} from {}", pending.type, caller);
	System::PrintDiagnosticLine(std::format("[checkpoint-sentinel] {} task={} thread={} hit={} stack={}\n", message, pending.task ? pending.task : "?", thread.str(), hit, stack.str()));
	RTEAssert(false, message);
}

namespace {
	struct TraceRecord {
		const char* label;
		std::string detail;
		size_t thread;
		int64_t queuedUs, startUs, endUs;
	};
	struct TraceState {
		std::mutex mutex;
		std::chrono::steady_clock::time_point origin;
		uint64_t tick = 0;
		std::vector<TraceRecord> records;
	};
	TraceState& Trace() {
		static TraceState state;
		return state;
	}
	// A short name for the thread a span ran on.
	size_t ThreadOrdinal() {
		static std::atomic<size_t> next{0};
		thread_local const size_t ordinal = ++next;
		return ordinal;
	}
} // namespace

bool CaptureTrace::Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("CCCP_CHECKPOINT_TRACE");
		return value && *value && std::strcmp(value, "0") != 0;
	}();
	return enabled;
}

bool CaptureTrace::Serial() {
	static const bool serial = [] {
		const char* value = std::getenv("CCCP_CHECKPOINT_SERIAL");
		return value && *value && std::strcmp(value, "0") != 0;
	}();
	return serial;
}

int CaptureTrace::ObjectDepth() {
	static const int depth = [] {
		const char* value = std::getenv("CCCP_CHECKPOINT_TRACE");
		const int parsed = value ? std::atoi(value) : 0;
		return parsed > 1 ? parsed : 1;
	}();
	return depth;
}

void CaptureTrace::Begin(uint64_t tick) {
	if (!Enabled()) return;
	TraceState& state = Trace();
	std::lock_guard lock(state.mutex);
	state.origin = std::chrono::steady_clock::now();
	state.tick = tick;
	state.records.clear();
	s_Active.store(true, std::memory_order_relaxed);
}

void CaptureTrace::End() {
	if (!Active()) return;
	s_Active.store(false, std::memory_order_relaxed);
	TraceState& state = Trace();
	std::vector<TraceRecord> records;
	uint64_t tick = 0;
	{
		std::lock_guard lock(state.mutex);
		records.swap(state.records);
		tick = state.tick;
	}
	std::string out;
	for (const TraceRecord& record: records) {
		out += std::format("[capture-span] tick={} label={} thread={} queued_us={} start_us={} end_us={} us={} detail={}\n", tick, record.label, record.thread,
		                   record.queuedUs, record.startUs, record.endUs, record.endUs - record.startUs, record.detail.empty() ? "-" : record.detail);
	}
	out += std::format("[capture-trace] tick={} spans={}\n", tick, records.size());
	System::PrintDiagnosticLine(out);
}

void CaptureTrace::Span::Start(const char* label, std::string detail, std::chrono::steady_clock::time_point queuedAt) {
	m_Label = label;
	m_Detail = std::move(detail);
	m_Queued = queuedAt;
	m_Start = std::chrono::steady_clock::now();
}

void CaptureTrace::Span::Stop() {
	const auto end = std::chrono::steady_clock::now();
	TraceState& state = Trace();
	const auto offset = [&state](std::chrono::steady_clock::time_point at) {
		return std::chrono::duration_cast<std::chrono::microseconds>(at - state.origin).count();
	};
	std::lock_guard lock(state.mutex);
	if (!Active()) return;
	state.records.push_back({m_Label, std::move(m_Detail), ThreadOrdinal(),
	                         m_Queued == std::chrono::steady_clock::time_point{} ? -1 : offset(m_Queued), offset(m_Start), offset(end)});
}
