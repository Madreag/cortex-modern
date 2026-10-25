#include "ThreadMan.h"
#include "SteadyClock.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <limits>
#include <string>

#ifdef _MSC_VER
#include <intrin.h>
#define SAVER_CALLER _ReturnAddress()
#else
#define SAVER_CALLER __builtin_return_address(0)
#endif

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace RTE;

namespace {
	// A saver thread takes the game's own priority only while its machine's own save is overdue and it works beside the game;
	// in the game's idle time it stays below the game's threads, in a match as in single player.
	thread_local int t_SaverRaised = -1;
	void SetSaverRaised(bool raised) {
		if (t_SaverRaised == static_cast<int>(raised)) return;
		t_SaverRaised = raised;
#ifdef _WIN32
		SetThreadPriority(GetCurrentThread(), raised ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_LOWEST);
#elif defined(__APPLE__)
		pthread_set_qos_class_self_np(raised ? QOS_CLASS_DEFAULT : QOS_CLASS_UTILITY, 0);
#endif
		// Linux keeps its lowered nice value: an unprivileged thread cannot raise it again, and the scheduler's fair share keeps it running.
	}

	// The calling thread's scheduling level as the platform reports it back: the Windows priority, the macOS QoS class, the Linux nice value.
	int ThreadPriorityLevel() {
#ifdef _WIN32
		return GetThreadPriority(GetCurrentThread());
#elif defined(__APPLE__)
		qos_class_t qos = QOS_CLASS_UNSPECIFIED;
		int relative = 0;
		if (pthread_get_qos_class_np(pthread_self(), &qos, &relative) != 0) return (std::numeric_limits<int>::min)();
		return static_cast<int>(qos);
#else
		errno = 0;
		const int nice = getpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)));
		return errno != 0 ? (std::numeric_limits<int>::min)() : nice;
#endif
	}

#ifdef _WIN32
	constexpr int c_SaverLowest = THREAD_PRIORITY_LOWEST, c_SaverRaised = THREAD_PRIORITY_NORMAL;
#elif defined(__APPLE__)
	constexpr int c_SaverLowest = static_cast<int>(QOS_CLASS_UTILITY), c_SaverRaised = static_cast<int>(QOS_CLASS_DEFAULT);
#else
	// A Linux saver stays at nice 10 throughout.
	constexpr int c_SaverLowest = 10, c_SaverRaised = 10;
#endif
}

void SaverThreads::Enter() {
	if (s_Lowered) return;
	SaverThreadMark::Set();
	s_Lowered = true;
	t_SaverRaised = 0;
	// Save work has a lower base priority than gameplay's workers.
#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
	// Test lever CCCP_TEST_SAVER_AFFINITY=<hex mask>: confines the saver's threads to those logical processors.
	static const DWORD_PTR affinity = [] {
		const char* value = std::getenv("CCCP_TEST_SAVER_AFFINITY");
		return value ? static_cast<DWORD_PTR>(std::strtoull(value, nullptr, 16)) : DWORD_PTR{0};
	}();
	if (affinity) SetThreadAffinityMask(GetCurrentThread(), affinity);
#elif defined(__APPLE__)
	pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#else
	setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
}

BS::thread_pool& SaverThreads::Pool() {
	return g_ThreadMan.GetSaverThreadPool();
}

namespace {
	std::mutex s_SliceMutex;
	std::condition_variable s_SliceOpened;
	std::atomic<int64_t> s_ReportedStuckAtUs{0};
	// A tick that keeps the gate closed this long without opening a slice may be waiting on the saver's own work.
	constexpr int64_t c_StuckTickUs = 250000;
	thread_local uint64_t t_LastCheckCycles = 0;
	thread_local const void* t_LastCheckSite = nullptr;

	// Test lever CCCP_TEST_SAVER_STRETCHES=<cycles>: names every stretch of saver work between two yields that ran over that
	// many CPU cycles of its own thread (a small value means 8 million, 2 ms at 4 GHz): how far past its slice the saver can run.
	uint64_t StretchCycles() {
		static const uint64_t cycles = [] {
			const char* value = std::getenv("CCCP_TEST_SAVER_STRETCHES");
			if (!value) return uint64_t{0};
			const uint64_t parsed = std::strtoull(value, nullptr, 10);
			return parsed < 100000 ? uint64_t{8000000} : parsed;
		}();
		return cycles;
	}

	bool ReportingStretches() { return StretchCycles() != 0; }

	uint64_t ThreadCycles() {
#ifdef _WIN32
		ULONG64 cycles = 0;
		QueryThreadCycleTime(GetCurrentThread(), &cycles);
		return cycles;
#else
		return 0;
#endif
	}

	// The stuck-tick guard names where each saver it lets go was parked: whatever the tick waited on is found from there.
	void ReportParked(int64_t episode, int64_t waitedUs) {
		thread_local int64_t t_ReportedEpisode = -1;
		if (t_ReportedEpisode == episode) return;
		t_ReportedEpisode = episode;
#ifdef _WIN32
		void* frames[40];
		const USHORT count = CaptureStackBackTrace(1, 40, frames, nullptr);
		const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
		char line[1024];
		int used = std::snprintf(line, sizeof(line), "[saver-parked] thread=%lu waited_ms=%lld at", GetCurrentThreadId(), static_cast<long long>(waitedUs / 1000));
		for (USHORT index = 0; index < count && used > 0 && used < static_cast<int>(sizeof(line)) - 20; ++index)
			used += std::snprintf(line + used, sizeof(line) - used, " %llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[index]) - base));
		std::fprintf(stdout, "%s\n", line);
		std::fflush(stdout);
#else
		(void)waitedUs;
#endif
	}

	void NoteStretch(const void* site) {
		const uint64_t cycles = ThreadCycles();
		if (t_LastCheckCycles && cycles - t_LastCheckCycles > StretchCycles()) {
#ifdef _WIN32
			const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
			std::fprintf(stdout, "[saver-stretch] cycles=%llu from=%llx to=%llx\n", static_cast<unsigned long long>(cycles - t_LastCheckCycles),
			    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(t_LastCheckSite) - base), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(site) - base));
			std::fflush(stdout);
#endif
		}
		t_LastCheckSite = site;
		t_LastCheckCycles = cycles;
	}
}

void SaverThreads::SetOverdue(bool overdue) {
	if (s_Overdue.exchange(overdue, std::memory_order_acq_rel) == overdue || !overdue) return;
	try { std::lock_guard lock(s_SliceMutex); } catch (...) {}
	s_SliceOpened.notify_all();
}

std::string SaverThreads::PrioritySelfTestMismatch() {
	const bool wasOverdue = s_Overdue.load(std::memory_order_acquire);
	int entered = 0, inSlice = 0, overdue = 0, after = 0;
	std::thread saver([&] {
		Enter();
		entered = ThreadPriorityLevel();
		LendTickStart(5000000);
		CheckSlice();
		inSlice = ThreadPriorityLevel();
		SetOverdue(true);
		CheckSlice();
		overdue = ThreadPriorityLevel();
		SetOverdue(false);
		CheckSlice();
		after = ThreadPriorityLevel();
		CloseSlice();
	});
	saver.join();
	SetOverdue(wasOverdue);
#if defined(_WIN32) || defined(__APPLE__)
	const int lowest = c_SaverLowest, raised = c_SaverRaised;
#else
	// A process already nicer than 10 keeps its own value; the saver is never raised.
	const int lowest = (std::max)(entered, c_SaverLowest), raised = lowest;
#endif
	if (entered != lowest) return std::format("a saver thread entered at level {} instead of its lowest {}", entered, lowest);
	if (inSlice != lowest) return std::format("a saver thread in a slice ran at level {} instead of its lowest {}", inSlice, lowest);
	if (overdue != raised) return std::format("a saver thread with an overdue save ran at level {} instead of {}", overdue, raised);
	if (after != lowest) return std::format("a saver thread stayed at level {} after its save stopped being overdue", after);
	return {};
}

void SaverThreads::CheckSlice() {
	if (ReportingStretches()) NoteStretch(SAVER_CALLER);
	if (s_Overdue.load(std::memory_order_acquire)) {
		if (s_Lowered) SetSaverRaised(true);
		return;
	}
	if (NowUs() < s_SliceEndUs.load(std::memory_order_acquire) && (s_SliceLent.load(std::memory_order_acquire) || s_GameWork.load(std::memory_order_acquire) == 0)) {
		if (s_Lowered) SetSaverRaised(false);
		return;
	}
	WaitForSlice();
}

void SaverThreads::GameWorkEnds() noexcept {
	if (s_GameWork.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
	try { std::lock_guard lock(s_SliceMutex); } catch (...) {}
	s_SliceOpened.notify_all();
}

int64_t SaverThreads::NowUs() noexcept {
	return std::chrono::duration_cast<std::chrono::microseconds>(RTE::SteadyClock::now().time_since_epoch()).count();
}

void SaverThreads::OpenSlice(int64_t untilUs) {
	{
		std::lock_guard lock(s_SliceMutex);
		s_SliceLent.store(false, std::memory_order_release);
		s_SliceEndUs.store(untilUs, std::memory_order_release);
	}
	s_SliceOpened.notify_all();
}

void SaverThreads::CloseSlice() noexcept {
	s_TickStarts.fetch_add(1, std::memory_order_relaxed);
	s_SliceLent.store(false, std::memory_order_release);
	s_SliceEndUs.store(0, std::memory_order_release);
	s_ClosedAtUs.store(NowUs(), std::memory_order_release);
}

void SaverThreads::LendTickStart(int64_t microseconds) {
	s_TickStarts.fetch_add(1, std::memory_order_relaxed);
	{
		std::lock_guard lock(s_SliceMutex);
		s_SliceLent.store(true, std::memory_order_release);
		s_SliceEndUs.store(NowUs() + microseconds, std::memory_order_release);
	}
	s_SliceOpened.notify_all();
}

void SaverThreads::WaitForSlice() {
	if (s_Lowered) SetSaverRaised(false);
	std::unique_lock lock(s_SliceMutex);
	const int64_t began = NowUs();
	s_Parked.fetch_add(1, std::memory_order_relaxed);
	struct Unpark { ~Unpark() { s_Parked.fetch_sub(1, std::memory_order_relaxed); } } unpark;
	for (;;) {
		if (s_Gating.load(std::memory_order_acquire) == 0 || s_Lent.load(std::memory_order_acquire) > 0 || s_Overdue.load(std::memory_order_acquire)) {
			if (s_Lowered) SetSaverRaised(s_Overdue.load(std::memory_order_acquire));
			return;
		}
		const int64_t now = NowUs(), sliceEnd = s_SliceEndUs.load(std::memory_order_acquire);
		if (now < sliceEnd) {
			if (s_SliceLent.load(std::memory_order_acquire) || s_GameWork.load(std::memory_order_acquire) == 0) return;
			// Game work that never ends may itself wait on something the saver holds.
			if (now - began > c_StuckTickUs) {
				if (s_ReportedStuckAtUs.exchange(began) != began) {
					std::fprintf(stdout, "[saver-gate] game work kept the saver out %lld ms; the saver goes on beside it\n", static_cast<long long>((now - began) / 1000));
					std::fflush(stdout);
				}
				ReportParked(began, now - began);
				return;
			}
		}
		// A lent slice that has run out leaves the gate closed from its end, as a tick's start closes it.
		const bool lent = s_SliceLent.load(std::memory_order_acquire);
		const int64_t closedAt = lent ? sliceEnd : s_ClosedAtUs.load(std::memory_order_acquire);
		if ((sliceEnd == 0 || lent) && now - closedAt > c_StuckTickUs) {
			if (s_ReportedStuckAtUs.exchange(closedAt) != closedAt) {
				std::fprintf(stdout, "[saver-gate] a tick kept the gate closed %lld ms; the saver goes on beside it\n", static_cast<long long>((now - closedAt) / 1000));
				std::fflush(stdout);
			}
			if (closedAt) ReportParked(closedAt, now - began);
			return;
		}
		// Whatever lets the saver on notifies; waking only for the stuck-tick report keeps parked savers off the game's cores.
		s_SliceOpened.wait_for(lock, std::chrono::milliseconds(50));
	}
}

SaverThreads::Lend::Lend() {
	{
		std::lock_guard lock(s_SliceMutex);
		s_Lent.fetch_add(1, std::memory_order_acq_rel);
	}
	s_SliceOpened.notify_all();
}

SaverThreads::Lend::~Lend() { s_Lent.fetch_sub(1, std::memory_order_acq_rel); }

SaverThreads::Gating::Gating() { s_Gating.fetch_add(1, std::memory_order_acq_rel); }

SaverThreads::Gating::~Gating() {
	{
		std::lock_guard lock(s_SliceMutex);
		s_Gating.fetch_sub(1, std::memory_order_acq_rel);
	}
	s_SliceOpened.notify_all();
}

#ifdef _WIN32
namespace {
	const char* PriorityClassName(DWORD priorityClass) {
		switch (priorityClass) {
			case IDLE_PRIORITY_CLASS: return "idle";
			case BELOW_NORMAL_PRIORITY_CLASS: return "below_normal";
			case NORMAL_PRIORITY_CLASS: return "normal";
			case ABOVE_NORMAL_PRIORITY_CLASS: return "above_normal";
			case HIGH_PRIORITY_CLASS: return "high";
			case REALTIME_PRIORITY_CLASS: return "realtime";
			default: return "unknown";
		}
	}
}
#endif

const char* RoundScheduling::SetRoundRunning(bool running) {
#ifdef _WIN32
	static std::mutex s_Lock;
	static DWORD s_Restore = 0; //!< The class the round raised the process from; 0 while it raised nothing.
	std::lock_guard lock(s_Lock);
	const HANDLE process = GetCurrentProcess();
	if (running && s_Restore == 0) {
		// Above normal, not high: the round goes ahead of ordinary programs and stays behind the system's own interactive work.
		const DWORD started = GetPriorityClass(process);
		if ((started == IDLE_PRIORITY_CLASS || started == BELOW_NORMAL_PRIORITY_CLASS || started == NORMAL_PRIORITY_CLASS) && SetPriorityClass(process, ABOVE_NORMAL_PRIORITY_CLASS)) {
			s_Restore = started;
			return PriorityClassName(started);
		}
	} else if (!running && s_Restore != 0) {
		SetPriorityClass(process, s_Restore);
		s_Restore = 0;
	}
#else
	// The POSIX schedulers share the processor among ready threads by weight, and an unprivileged process cannot raise itself.
	(void)running;
#endif
	return nullptr;
}

const char* RoundScheduling::PriorityName() {
#ifdef _WIN32
	return PriorityClassName(GetPriorityClass(GetCurrentProcess()));
#else
	return "shared";
#endif
}

ThreadMan::ThreadMan() :
    m_PriorityThreadPool(0, FloatingPointEnvironment::Initialize, [] { FloatingPointEnvironment::Assert("priority task"); }),
    m_BackgroundThreadPool(0, FloatingPointEnvironment::Initialize, [] { FloatingPointEnvironment::Assert("background task"); }) {
	Clear();
	Create();
}

ThreadMan::~ThreadMan() {
	Destroy();
}

void ThreadMan::Clear() {
	m_PriorityThreadPool.reset();
	m_BackgroundThreadPool.reset(std::thread::hardware_concurrency() / 2);
}

int ThreadMan::Create() {
	return 0;
}

void ThreadMan::Destroy() {
	Clear();
}
