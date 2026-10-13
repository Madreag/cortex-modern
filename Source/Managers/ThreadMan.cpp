#include "ThreadMan.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

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

void SaverThreads::Enter() {
	if (s_Lowered) return;
	s_Current = s_Lowered = true;
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

void SaverThreads::CheckSlice() {
	if (ReportingStretches()) NoteStretch(SAVER_CALLER);
	if (NowUs() < s_SliceEndUs.load(std::memory_order_acquire)) return;
	WaitForSlice();
}

int64_t SaverThreads::NowUs() noexcept {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void SaverThreads::OpenSlice(int64_t untilUs) {
	{
		std::lock_guard lock(s_SliceMutex);
		s_SliceEndUs.store(untilUs, std::memory_order_release);
	}
	s_SliceOpened.notify_all();
}

void SaverThreads::CloseSlice() noexcept {
	s_SliceEndUs.store(0, std::memory_order_release);
	s_ClosedAtUs.store(NowUs(), std::memory_order_release);
}

void SaverThreads::WaitForSlice() {
	std::unique_lock lock(s_SliceMutex);
	for (;;) {
		if (s_Gating.load(std::memory_order_acquire) == 0 || s_Lent.load(std::memory_order_acquire) > 0) return;
		const int64_t now = NowUs(), sliceEnd = s_SliceEndUs.load(std::memory_order_acquire);
		if (now < sliceEnd) return;
		const int64_t closedAt = s_ClosedAtUs.load(std::memory_order_acquire);
		if (sliceEnd == 0 && now - closedAt > c_StuckTickUs) {
			if (s_ReportedStuckAtUs.exchange(closedAt) != closedAt) {
				std::fprintf(stdout, "[saver-gate] a tick kept the gate closed %lld ms; the saver goes on beside it\n", static_cast<long long>((now - closedAt) / 1000));
				std::fflush(stdout);
			}
			return;
		}
		s_SliceOpened.wait_for(lock, std::chrono::milliseconds(1));
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
