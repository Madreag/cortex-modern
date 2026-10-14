#pragma once

#include "CheckpointActivity.h"

#include <atomic>

namespace RTE {

	/// Marks the threads that write saves; kept below SaverThreads so the frozen-storage reads can ask it inline.
	struct SaverThreadMark {
		static inline thread_local bool current = false;
		/// Threads ever marked: while none has been, no thread needs the saver's checks.
		static inline std::atomic<int> marked{0};

		/// Marks the calling thread for the rest of its life.
		static void Set() noexcept {
			if (current) return;
			CheckpointActivity::Begin();
			current = true;
			marked.fetch_add(1, std::memory_order_relaxed);
		}
	};
} // namespace RTE
