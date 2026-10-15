#pragma once

#include <algorithm>

namespace RTE {

	/// Whether a match's presents wait for the display. A display that takes no frame at its refresh (switched off, or not shown)
	/// would hold the game loop in every swap, so a match stops waiting for it and tries one waited swap now and then until the
	/// display takes frames again. Outside a match nothing changes.
	class SwapWaitGuard {
	public:
		enum class Action { Keep, Suspend, Retry, Resume };

		/// Swaps in a row that each waited several refresh periods before the wait is suspended.
		static constexpr int c_LongSwapsToSuspend = 30;
		/// How often a suspended wait is tried on one frame.
		static constexpr double c_RetryEveryMs = 2000.0;

		/// One present's swap wait; refreshHz is the window's display rate (0 when unknown).
		Action Note(double swapMs, double refreshHz, double nowMs, bool inMatch) {
			const double periodMs = 1000.0 / (refreshHz > 0 ? refreshHz : 60.0);
			if (!inMatch) {
				m_LongSwaps = 0;
				m_Retrying = false;
				if (!m_Suspended) return Action::Keep;
				m_Suspended = false;
				return Action::Resume;
			}
			if (!m_Suspended) {
				m_LongSwaps = swapMs >= std::max(4.0 * periodMs, 40.0) ? m_LongSwaps + 1 : 0;
				if (m_LongSwaps < c_LongSwapsToSuspend) return Action::Keep;
				m_LongSwaps = 0;
				m_Suspended = true;
				m_RetryAtMs = nowMs + c_RetryEveryMs;
				return Action::Suspend;
			}
			if (m_Retrying) {
				m_Retrying = false;
				if (swapMs < 2.0 * periodMs) {
					m_Suspended = false;
					return Action::Resume;
				}
				m_RetryAtMs = nowMs + c_RetryEveryMs;
				return Action::Suspend;
			}
			if (nowMs < m_RetryAtMs) return Action::Keep;
			m_Retrying = true;
			return Action::Retry;
		}

		bool Suspended() const { return m_Suspended; }

	private:
		int m_LongSwaps = 0;
		bool m_Suspended = false;
		bool m_Retrying = false;
		double m_RetryAtMs = 0;
	};
} // namespace RTE
