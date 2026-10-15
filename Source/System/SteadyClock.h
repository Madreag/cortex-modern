#pragma once

#include <chrono>

#if defined(__APPLE__) && defined(__GLIBCXX__)
#include <time.h>
#endif

namespace RTE {

	/// The steady clock's reading. libstdc++ on macOS reads the wall clock for std::chrono::steady_clock, which a time sync
	/// steps back; there the uptime clock that libc++'s steady_clock reads stands in for it.
	inline std::chrono::steady_clock::time_point SteadyNow() noexcept {
#if defined(__APPLE__) && defined(__GLIBCXX__)
		return std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(clock_gettime_nsec_np(CLOCK_UPTIME_RAW))));
#else
		return std::chrono::steady_clock::now();
#endif
	}
} // namespace RTE
