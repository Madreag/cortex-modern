#pragma once

#include <algorithm>

namespace RTE {

	/// Holds a VSync window's presents to its display's refresh where the platform does not. A display shows at most one frame a
	/// refresh, so presents that keep coming faster than that were not held by the swap: the swap interval the window asked for did
	/// not take. Then each present waits out the refresh itself, until the swaps wait on their own again.
	class PresentPacer {
	public:
		/// Presents in a row that came faster than the display shows them, or swaps in a row that waited, before the pacing turns.
		static constexpr int c_PresentsToTurn = 30;

		/// How long the next swap waits first; 0 while the swap waits for the display itself.
		double WaitBeforeSwapMs(double nowMs) const { return m_Pacing ? std::max(0.0, m_LastPresentMs + m_PeriodMs - nowMs) : 0.0; }

		/// One present: whether its window wants VSync, the display's refresh (0 when unknown), the swap's own wait and when the present
		/// ended. True when the pacing turned on or off with it.
		bool Note(bool vsync, double refreshHz, double swapMs, double presentEndMs) {
			const bool wasPacing = m_Pacing;
			if (!vsync || refreshHz <= 0) {
				*this = PresentPacer();
				return wasPacing;
			}
			m_PeriodMs = 1000.0 / refreshHz;
			const double intervalMs = m_LastPresentMs > 0 ? presentEndMs - m_LastPresentMs : m_PeriodMs;
			m_LastPresentMs = presentEndMs;
			// Paced here, a swap that still waits half a refresh means the display paces again.
			const bool turn = m_Pacing ? swapMs >= 0.5 * m_PeriodMs : intervalMs < 0.75 * m_PeriodMs;
			m_Turning = turn ? m_Turning + 1 : 0;
			if (m_Turning < c_PresentsToTurn) return false;
			m_Turning = 0;
			m_Pacing = !m_Pacing;
			return true;
		}

		bool Pacing() const { return m_Pacing; }
		double PeriodMs() const { return m_PeriodMs; }

	private:
		bool m_Pacing = false;
		int m_Turning = 0;
		double m_PeriodMs = 0;
		double m_LastPresentMs = 0;
	};
} // namespace RTE
