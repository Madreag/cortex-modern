#pragma once

#include "Singleton.h"

#include <string>

#define g_AIDebugOverlay AIDebugOverlay::Instance()

namespace RTE {

	class Actor;

	/// ImGui-based per-actor debug overlay showing live AI cognition.
	///
	/// Reads the most recent K events from the AIDecisionChannel and renders them in a small
	/// floating panel. Toggle via the in-game console (`overlay on`/`overlay off`) or a hotkey.
	class AIDebugOverlay : public Singleton<AIDebugOverlay> {
		friend class Singleton<AIDebugOverlay>;

	public:
		AIDebugOverlay();
		~AIDebugOverlay();

		void Initialize() {}
		void Destroy() {}

		bool IsEnabled() const { return m_Enabled; }
		void SetEnabled(bool enabled) { m_Enabled = enabled; }
		void Toggle() { m_Enabled = !m_Enabled; }

		/// Watch a specific actor. If -1, the overlay shows the player's current actor (if any).
		void SetWatchedActorId(int id) { m_WatchedActorId = id; }
		int  GetWatchedActorId() const { return m_WatchedActorId; }

		/// Number of recent events to display.
		void SetHistoryLength(int n);
		int  GetHistoryLength() const { return m_HistoryLength; }

		/// Draw the overlay. Safe to call every frame; no-op when disabled or no ImGui context.
		/// `currentActor` is the player's currently controlled actor; may be null.
		void Draw(const Actor* currentActor);

	private:
		bool m_Enabled = false;
		int  m_WatchedActorId = -1;
		int  m_HistoryLength = 16;
	};

} // namespace RTE
