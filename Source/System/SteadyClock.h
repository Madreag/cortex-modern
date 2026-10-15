#pragma once

#include <chrono>

#if defined(__APPLE__) && defined(__GLIBCXX__)
#include <time.h>
#endif

namespace RTE {

	/// The one steady clock the game's timers, deadlines and timestamps read, so any two of its readings compare. libstdc++ on macOS
	/// reads the wall clock for std::chrono::steady_clock, which a time sync steps back; there the uptime clock that libc++'s
	/// steady_clock reads stands in for it. Its own time_point type keeps a reading of another clock from meeting one of these.
	struct SteadyClock {
		using duration = std::chrono::nanoseconds;
		using rep = duration::rep;
		using period = duration::period;
		using time_point = std::chrono::time_point<SteadyClock, duration>;
		static constexpr bool is_steady = true;

		static time_point now() noexcept {
#if defined(__APPLE__) && defined(__GLIBCXX__)
			return time_point(duration(static_cast<rep>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW))));
#else
			return time_point(std::chrono::duration_cast<duration>(std::chrono::steady_clock::now().time_since_epoch()));
#endif
		}
	};

	/// The steady clock's reading.
	inline SteadyClock::time_point SteadyNow() noexcept { return SteadyClock::now(); }
} // namespace RTE
