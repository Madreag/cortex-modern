#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace RTE {

	/// Diagnostics: names where the simulation thread is while one tick runs longer than a threshold. Armed by the
	/// CCCP_STALL_STACK_MS environment variable; a watcher thread samples the thread's stack at the threshold and every
	/// threshold after, and prints each sample as one "[stall-stack]" line. Off, it costs two atomic stores per tick.
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
				s_BeganMs.store(0, std::memory_order_release);
			}
		}

	private:
		static int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
		static void Watch(std::stop_token stop);
		static std::string Sample();

		static inline int64_t s_ThresholdMs = 0;
		static inline std::atomic<int64_t> s_BeganMs{0};
		static inline std::atomic<uint64_t> s_Tick{0};
		static inline void* s_Thread = nullptr;
		static inline std::jthread s_Watcher;
		static inline int64_t s_LastSuspendNs = 0; //!< How long the last sample held the thread suspended; the watcher's own.
		static inline bool s_Symbols = false; //!< Whether the symbols loaded when the sampler was armed.
	};
} // namespace RTE
