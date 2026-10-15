#pragma once

#include <algorithm>
#include <cstdint>

namespace RTE {

	/// Whether a match's presents wait for the display. A display that takes no frame at its refresh (switched off, or not shown)
	/// would hold the game loop in every swap, so a match stops waiting for it and looks now and then whether the display takes
	/// frames again: from the compositor's refresh count where one can be read, else with one waited swap - for a window off the
	/// display the compositor counts, only while that count shows the desktop composed. Outside a match nothing changes.
	class SwapWaitGuard {
	public:
		enum class Action { Keep, Suspend, Retry, Resume, Release };

		/// Swaps in a row that each waited several refresh periods before the wait is suspended.
		static constexpr int c_LongSwapsToSuspend = 30;
		/// How often a suspended wait looks at the display again.
		static constexpr double c_RetryEveryMs = 2000.0;

		/// One present's swap wait; refreshHz is the window's display rate (0 when unknown) and refreshes the compositor's count of its
		/// display's refreshes (negative where it cannot be read), onCountedDisplay whether the window is on that display and countedHz
		/// that display's rate. Resume means the display takes frames again, Release that the match ended with the wait suspended.
		Action Note(double swapMs, double refreshHz, double nowMs, bool inMatch, int64_t refreshes, bool onCountedDisplay = true, double countedHz = 0.0) {
			const double displayHz = refreshHz > 0 ? refreshHz : 60.0;
			const double periodMs = 1000.0 / displayHz;
			if (!inMatch) {
				m_LongSwaps = 0;
				m_Retrying = false;
				m_CountedRefreshes = -1;
				if (!m_Suspended) return Action::Keep;
				m_Suspended = false;
				return Action::Release;
			}
			if (!m_Suspended) {
				m_LongSwaps = swapMs >= std::max(4.0 * periodMs, 40.0) ? m_LongSwaps + 1 : 0;
				if (m_LongSwaps < c_LongSwapsToSuspend) return Action::Keep;
				m_LongSwaps = 0;
				m_Suspended = true;
				m_CountedRefreshes = -1;
				m_RetryAtMs = nowMs + c_RetryEveryMs;
				return Action::Suspend;
			}
			if (m_Retrying) {
				m_Retrying = false;
				if (swapMs < 2.0 * periodMs) {
					m_Suspended = false;
					m_CountedRefreshes = -1;
					return Action::Resume;
				}
				m_RetryAtMs = nowMs + c_RetryEveryMs;
				return Action::Suspend;
			}
			// The compositor's count tells without a waited swap, which would hold the loop for the display's whole fallback wait. A
			// window on another display reads in it only whether the desktop is composed at all; then one waited swap tells the rest.
			if (refreshes >= 0) {
				if (m_CountedRefreshes < 0 || refreshes < m_CountedRefreshes) {
					m_CountedRefreshes = refreshes;
					m_CountedAtMs = nowMs;
				}
				if (nowMs < m_RetryAtMs || nowMs <= m_CountedAtMs) return Action::Keep;
				m_ShownHz = static_cast<double>(refreshes - m_CountedRefreshes) * 1000.0 / (nowMs - m_CountedAtMs);
				m_CountedRefreshes = refreshes;
				m_CountedAtMs = nowMs;
				m_RetryAtMs = nowMs + c_RetryEveryMs;
				if (m_ShownHz < 0.5 * (onCountedDisplay ? displayHz : countedHz > 0 ? countedHz : 60.0)) return Action::Keep;
				if (!onCountedDisplay) {
					m_Retrying = true;
					return Action::Retry;
				}
				m_Suspended = false;
				m_CountedRefreshes = -1;
				return Action::Resume;
			}
			if (nowMs < m_RetryAtMs) return Action::Keep;
			m_Retrying = true;
			return Action::Retry;
		}

		bool Suspended() const { return m_Suspended; }
		/// The refresh rate the compositor last counted while the wait was suspended.
		double ShownHz() const { return m_ShownHz; }

	private:
		int m_LongSwaps = 0;
		bool m_Suspended = false;
		bool m_Retrying = false;
		double m_RetryAtMs = 0;
		int64_t m_CountedRefreshes = -1;
		double m_CountedAtMs = 0;
		double m_ShownHz = 0;
	};
} // namespace RTE
