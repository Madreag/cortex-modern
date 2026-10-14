#pragma once

namespace RTE {

	/// Marks the threads that write saves; kept below SaverThreads so the frozen-storage reads can ask it inline.
	struct SaverThreadMark {
		static inline thread_local bool current = false;
	};
} // namespace RTE
