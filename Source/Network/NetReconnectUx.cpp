#include "NetReconnectUx.h"

#include <utility>

namespace RTE {

	void NetReconnectUx::NoteConnected(uint64_t nowMs) {
		(void)nowMs;
		m_State = NetReconnectUxState::Connected;
		m_Attempts = 0;
		m_Reason.clear();
	}

	void NetReconnectUx::NoteDropped(uint64_t nowMs, std::string reason) {
		if (m_State == NetReconnectUxState::Waiting || m_State == NetReconnectUxState::Retrying) {
			return;
		}
		m_State = NetReconnectUxState::Waiting;
		m_DroppedAtMs = nowMs;
		// The first attempt is due at once: the seat is already open and the window is already running.
		m_NextAttemptMs = nowMs;
		m_Attempts = 0;
		m_Reason = std::move(reason);
	}

	void NetReconnectUx::NoteReconnected(uint64_t nowMs) {
		(void)nowMs;
		m_State = NetReconnectUxState::Reconnected;
		m_Reason.clear();
	}

	bool NetReconnectUx::Tick(uint64_t nowMs) {
		if (m_State != NetReconnectUxState::Waiting) {
			return false;
		}
		if (m_Attempts >= c_MaxAttempts || (nowMs >= m_DroppedAtMs && nowMs - m_DroppedAtMs > c_ResumeWindowMs)) {
			// The host's own resume window has closed, so nothing this side does can still land.
			m_State = NetReconnectUxState::GaveUp;
			return false;
		}
		return nowMs >= m_NextAttemptMs;
	}

	void NetReconnectUx::NoteAttemptStarted(uint64_t nowMs) {
		m_State = NetReconnectUxState::Retrying;
		++m_Attempts;
		m_NextAttemptMs = nowMs + c_AttemptIntervalMs;
	}

	void NetReconnectUx::NoteAttemptFailed(uint64_t nowMs, std::string reason) {
		if (!reason.empty()) {
			m_Reason = std::move(reason);
		}
		if (m_State != NetReconnectUxState::Retrying) {
			return;
		}
		m_State = m_Attempts >= c_MaxAttempts || (nowMs >= m_DroppedAtMs && nowMs - m_DroppedAtMs > c_ResumeWindowMs)
		              ? NetReconnectUxState::GaveUp
		              : NetReconnectUxState::Waiting;
	}

	void NetReconnectUx::Cancel(uint64_t nowMs) {
		(void)nowMs;
		if (!CanCancel()) {
			return;
		}
		// A cancel stops the attempts; it is not a leave, so the record stays for a manual retry.
		m_State = NetReconnectUxState::Cancelled;
	}

	void NetReconnectUx::RequestManualRetry(uint64_t nowMs) {
		if (!CanRetryManually()) {
			return;
		}
		m_State = NetReconnectUxState::Waiting;
		m_DroppedAtMs = nowMs;
		m_NextAttemptMs = nowMs;
		m_Attempts = 0;
	}

	bool NetReconnectUx::CanCancel() const {
		return m_State == NetReconnectUxState::Waiting || m_State == NetReconnectUxState::Retrying;
	}

	bool NetReconnectUx::CanRetryManually() const {
		return m_State == NetReconnectUxState::GaveUp || m_State == NetReconnectUxState::Cancelled;
	}

	void NetReconnectUx::OfferStoredTicket(NetH4TicketLoadResult load, std::string hostAddress) {
		switch (load) {
			case NetH4TicketLoadResult::Loaded:
				m_Offer = NetReconnectOffer::Available;
				m_OfferAddress = std::move(hostAddress);
				return;
			case NetH4TicketLoadResult::Corrupt:
				m_Offer = NetReconnectOffer::Corrupt;
				break;
			case NetH4TicketLoadResult::Stale:
				m_Offer = NetReconnectOffer::Stale;
				break;
			case NetH4TicketLoadResult::Missing:
				m_Offer = NetReconnectOffer::None;
				break;
		}
		m_OfferAddress.clear();
	}

	void NetReconnectUx::DismissOffer() {
		m_Offer = NetReconnectOffer::None;
		m_OfferAddress.clear();
	}

	std::string NetReconnectUx::GetOfferText() const {
		switch (m_Offer) {
			case NetReconnectOffer::Available: return "Rejoin your match at " + m_OfferAddress + "?";
			case NetReconnectOffer::Corrupt: return "The saved reconnect ticket is damaged and cannot be used.";
			case NetReconnectOffer::Stale: return "The saved reconnect ticket is too old to use.";
			case NetReconnectOffer::None: break;
		}
		return "";
	}

	std::string NetReconnectUx::GetStatusText() const {
		const std::string tail = m_Reason.empty() ? "" : " (" + m_Reason + ")";
		switch (m_State) {
			case NetReconnectUxState::Waiting:
			case NetReconnectUxState::Retrying:
				return "Reconnecting... attempt " + std::to_string(m_Attempts == 0 ? 1U : m_Attempts) + " of " +
				       std::to_string(c_MaxAttempts) + tail;
			case NetReconnectUxState::Reconnected: return "Reconnected.";
			case NetReconnectUxState::GaveUp: return "Could not reconnect" + tail + ". Retry to try again.";
			case NetReconnectUxState::Cancelled: return "Reconnecting cancelled. Retry to try again.";
			case NetReconnectUxState::Connected:
			case NetReconnectUxState::Idle: break;
		}
		return "";
	}

	bool NetReconnectUx::IsActive() const {
		return m_State == NetReconnectUxState::Waiting || m_State == NetReconnectUxState::Retrying ||
		       m_State == NetReconnectUxState::GaveUp || m_State == NetReconnectUxState::Cancelled;
	}

	const char* NetReconnectUx::RosterMark(bool dropped, bool reclaiming) {
		if (reclaiming) {
			return " - Reconnecting";
		}
		return dropped ? " - Disconnected" : "";
	}

	const char* NetReconnectUx::StateName(NetReconnectUxState state) {
		switch (state) {
			case NetReconnectUxState::Idle: return "Idle";
			case NetReconnectUxState::Connected: return "Connected";
			case NetReconnectUxState::Waiting: return "Waiting";
			case NetReconnectUxState::Retrying: return "Retrying";
			case NetReconnectUxState::Reconnected: return "Reconnected";
			case NetReconnectUxState::GaveUp: return "GaveUp";
			case NetReconnectUxState::Cancelled: return "Cancelled";
		}
		return "Unknown";
	}

} // namespace RTE
