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

	void NetSeatPresence::Observe(const NetLockstepSeatNotice& notice) {
		Seat& seat = m_Seats[notice.peerId];
		switch (notice.kind) {
			case NetSeatNoticeKind::Left:
				seat = {NetSeatPresenceState::Left, 0, false, std::string()};
				break;
			case NetSeatNoticeKind::Dropped:
				seat = {NetSeatPresenceState::Disconnected, notice.holdUntilFrame, false, std::string()};
				break;
			case NetSeatNoticeKind::Reclaiming:
				// Only a seat still being held can be on its way back.
				if (seat.state == NetSeatPresenceState::Disconnected) {
					seat.state = NetSeatPresenceState::Reconnecting;
				}
				break;
			case NetSeatNoticeKind::Reclaimed:
				seat = {NetSeatPresenceState::Present, 0, false, std::string()};
				break;
			case NetSeatNoticeKind::Substituted:
				seat = {NetSeatPresenceState::Substituted, 0, false, notice.name};
				break;
		}
	}

	void NetSeatPresence::NoteFrame(uint64_t appliedFrame) {
		if (appliedFrame < m_Frame) {
			// A resync starts a new round from its own first frame. The hold belongs to the seat and not
			// to the round, so it keeps the frames it had left rather than restarting or expiring.
			for (auto& [peerId, seat]: m_Seats) {
				seat.holdUntilFrame = seat.holdUntilFrame > m_Frame ? appliedFrame + (seat.holdUntilFrame - m_Frame) : 0;
			}
		}
		m_Frame = appliedFrame;
		for (auto& [peerId, seat]: m_Seats) {
			const bool holding = seat.state == NetSeatPresenceState::Disconnected || seat.state == NetSeatPresenceState::Reconnecting;
			if (holding && seat.holdUntilFrame != 0 && appliedFrame >= seat.holdUntilFrame) {
				seat.state = NetSeatPresenceState::Left;
				seat.holdRanOut = true;
			}
		}
	}

	void NetSeatPresence::Clear() {
		m_Seats.clear();
		m_Frame = 0;
	}

	NetSeatPresenceState NetSeatPresence::StateOf(uint8_t peerId) const {
		const auto it = m_Seats.find(peerId);
		return it == m_Seats.end() ? NetSeatPresenceState::Present : it->second.state;
	}

	uint64_t NetSeatPresence::HoldFramesRemaining(uint8_t peerId) const {
		const auto it = m_Seats.find(peerId);
		if (it == m_Seats.end() || it->second.holdUntilFrame <= m_Frame) {
			return 0;
		}
		return it->second.holdUntilFrame - m_Frame;
	}

	std::string NetSeatPresence::Line(uint8_t peerId, const std::string& playerName) const {
		const auto it = m_Seats.find(peerId);
		if (it == m_Seats.end() || it->second.state == NetSeatPresenceState::Present) {
			return std::string();
		}
		const Seat& seat = it->second;
		const std::string who = playerName.empty() ? "Player " + std::to_string(peerId) : playerName;
		switch (seat.state) {
			case NetSeatPresenceState::Disconnected:
			case NetSeatPresenceState::Reconnecting:
				return who + ": " + (seat.state == NetSeatPresenceState::Reconnecting ? "reconnecting" : "disconnected") +
				       " - seat held " + std::to_string(HoldSeconds(HoldFramesRemaining(peerId))) + "s";
			case NetSeatPresenceState::Substituted:
				return who + ": substituted by " + (seat.holderName.empty() ? "another player" : seat.holderName);
			case NetSeatPresenceState::Left:
				return who + (seat.holdRanOut ? ": left - the seat's hold ran out" : ": left");
			default:
				return std::string();
		}
	}

	uint64_t NetSeatPresence::HoldSeconds(uint64_t frames) {
		return (frames * c_FrameMicroseconds + 500000) / 1000000;
	}

	const char* NetSeatPresence::StateName(NetSeatPresenceState state) {
		switch (state) {
			case NetSeatPresenceState::Present: return "Present";
			case NetSeatPresenceState::Disconnected: return "Disconnected";
			case NetSeatPresenceState::Reconnecting: return "Reconnecting";
			case NetSeatPresenceState::Substituted: return "Substituted";
			case NetSeatPresenceState::Left: return "Left";
		}
		return "Unknown";
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
