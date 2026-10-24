#pragma once

#include <atomic>
#include "SteadyClock.h"
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace RTE {

	/// Diagnostics: names where the simulation thread is while one tick runs longer than a threshold. Armed by the
	/// CCCP_STALL_STACK_MS environment variable; a watcher thread samples the thread's stack at the threshold and every
	/// threshold after, and prints each sample as one "[stall-stack]" line. After such a tick it names the process's threads
	/// that ran most meanwhile, with a stack of each, as "[stall-threads]": a tick kept off its cores shows who had them.
	/// Off, it costs two atomic stores per tick.
	class StallStackSampler {

	public:
		/// Reads the threshold and starts the watcher for the calling thread when it is set.
		static void ArmForCurrentThread();

		static void TickBegin(uint64_t tick) {
			if (s_ThresholdMs == 0) {
				return;
			}
			s_Tick.store(tick, std::memory_order_relaxed);
			s_BeganMs.store(NowMs(), std::memory_order_release);
		}

		static void TickEnd() {
			if (s_ThresholdMs != 0) {
				const int64_t began = s_BeganMs.exchange(0, std::memory_order_acq_rel);
				if (began != 0 && NowMs() - began > s_ThresholdMs) {
					s_LongTickBeganMs.store(began, std::memory_order_relaxed);
					s_LongTickMs.store(NowMs() - began, std::memory_order_relaxed);
					s_LongTickTick.store(s_Tick.load(std::memory_order_relaxed), std::memory_order_relaxed);
					s_LongTicks.fetch_add(1, std::memory_order_release);
				}
			}
		}

	private:
		static int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(RTE::SteadyClock::now().time_since_epoch()).count(); }
		static void Watch(std::stop_token stop);
		static std::string Sample();
		static std::string Sample(void* thread, bool simulation);

		static inline int64_t s_ThresholdMs = 0;
		static inline std::atomic<int64_t> s_BeganMs{0};
		static inline std::atomic<uint64_t> s_Tick{0};
		static inline std::atomic<int64_t> s_LongTickBeganMs{0}, s_LongTickMs{0}; //!< The last tick that ran past the threshold.
		static inline std::atomic<uint64_t> s_LongTickTick{0}, s_LongTicks{0};
		static inline void* s_Thread = nullptr;
		static inline std::jthread s_Watcher;
		static inline int64_t s_LastSuspendNs = 0; //!< How long the last sample held the thread suspended; the watcher's own.
		static inline bool s_Symbols = false; //!< Whether the symbols loaded when the sampler was armed.
	};
} // namespace RTE
