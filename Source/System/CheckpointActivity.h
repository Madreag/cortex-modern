#pragma once

#include <atomic>

namespace RTE {

	/// Counts what can make a thread's view of native values differ from the live ones: open frozen-read, frozen-clock and
	/// snapshot-stream scopes on any thread. While it reads zero, a check that would ask the calling thread's own state answers
	/// with the live value; a saver thread there reads nothing frozen, so it yields at its other points instead.
	struct CheckpointActivity {
		static bool Quiet() noexcept { return s_Count.load(std::memory_order_relaxed) == 0; }
		static void Begin() noexcept { s_Count.fetch_add(1, std::memory_order_relaxed); }
		static void End() noexcept { s_Count.fetch_sub(1, std::memory_order_relaxed); }

	private:
		static inline std::atomic<int> s_Count{0};
	};
} // namespace RTE
