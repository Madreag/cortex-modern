#include "NetReconnectUx.h"

#include "NetMatchService.h"

#include <algorithm>
#include <iostream>
#include <utility>

namespace RTE {

	void NetReconnectUx::NoteConnected(uint64_t nowMs) {
		(void)nowMs;
		m_State = NetReconnectUxState::Connected;
		m_Attempts = 0;
		m_Reason.clear();
	}

	void NetReconnectUx::NoteDropped(uint64_t nowMs, std::string reason) {
		// A schedule that is already running, has been stopped by the player, or has run out is not
		// re-armed by the same loss being reported again.
		if (IsActive()) {
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

	const char* NetModerationActionName(NetModerationAction action) {
		switch (action) {
			case NetModerationAction::Wait: return "wait";
			case NetModerationAction::Substitute: return "substitute";
			case NetModerationAction::Cancel: return "cancel";
		}
		return "unknown";
	}

	std::string NetModerationUx::DescribeSeat(const NetH4ModerationSeat& seat) {
		std::string text = "Seat " + std::to_string(seat.stableSeat) + " - " +
		                   (seat.displayName.empty() ? "peer " + std::to_string(seat.lockstepPeerId) : seat.displayName);
		if (seat.dropped) {
			text += " - dropped " + std::to_string(seat.droppedForMs / 1000) + "s ago";
		} else if (seat.closed) {
			text += " - left";
		}
		text += seat.holdFramesRemaining > 0
		            ? " - hold " + std::to_string(seat.holdFramesRemaining) + "f (" + std::to_string(NetSeatPresence::HoldSeconds(seat.holdFramesRemaining)) + "s)"
		            : " - hold over";
		text += " - " + std::to_string(seat.applicants.size()) + " waiting";
		return text;
	}

	void NetModerationUx::Refresh(const std::vector<NetH4ModerationSeat>& seats) {
		m_Rows.clear();
		for (const NetH4ModerationSeat& seat: seats) {
			if (seat.cpu || (!seat.substitutable && !seat.substituting && !seat.dropped)) {
				continue;
			}
			Row row;
			row.view = seat;
			row.stableSeat = seat.stableSeat;
			row.lockstepPeerId = seat.lockstepPeerId;
			row.text = DescribeSeat(seat);
			row.applicants = seat.applicants.size();
			row.substitutable = seat.substitutable;
			row.substituting = seat.substituting;
			auto chosen = m_Chosen.find(seat.stableSeat);
			const auto identity = NetSelectModerationSeat(seat);
			if (chosen == m_Chosen.end() || chosen->second.epoch != seat.epoch ||
			    chosen->second.holderGeneration != seat.holderGeneration || chosen->second.seatGeneration != seat.seatGeneration ||
			    chosen->second.incarnation != seat.incarnation) {
				m_Chosen[seat.stableSeat] = seat.applicants.empty() ? identity : NetSelectModerationSeat(seat, seat.applicants.front().connection);
				chosen = m_Chosen.find(seat.stableSeat);
			}
			size_t index = seat.applicants.size();
			for (size_t i = 0; i < seat.applicants.size(); ++i) {
				if (seat.applicants[i].connection == chosen->second.applicant && seat.applicants[i].transactionId == chosen->second.applicantTransaction) {
					index = i;
					break;
				}
			}
			if (index == seat.applicants.size()) {
				row.applicantText = seat.applicants.empty() ? "No applicants" : "Choose an applicant";
			} else {
				const NetH4ApplicantView& applicant = seat.applicants[index];
				row.applicant = applicant.connection;
				row.applicantText = applicant.displayName + " (" + std::to_string(index + 1) + "/" + std::to_string(seat.applicants.size()) + ")";
				if (applicant.approved) {
					row.applicantText += " *";
				}
			}
			row.selection = NetSelectModerationSeat(seat, row.applicant);
			m_Rows.push_back(std::move(row));
		}
		std::erase_if(m_Chosen, [&seats](const auto& entry) {
			return std::none_of(seats.begin(), seats.end(), [&entry](const auto& seat) { return seat.stableSeat == entry.first; });
		});
	}

	size_t NetModerationUx::FindSeat(uint16_t stableSeat) const {
		for (size_t i = 0; i < m_Rows.size(); ++i) {
			if (m_Rows[i].stableSeat == stableSeat) {
				return i;
			}
		}
		return m_Rows.size();
	}

	void NetModerationUx::CycleApplicant(size_t index) {
		if (index < m_Rows.size()) CycleApplicant(m_Rows[index]);
	}

	void NetModerationUx::CycleApplicant(const Row& displayed) {
		if (!displayed.view.actionsAvailable || displayed.view.applicants.empty()) return;
		const auto& candidates = displayed.view.applicants;
		size_t next = 0;
		for (size_t i = 0; i < candidates.size(); ++i) {
			if (candidates[i].connection == displayed.selection.applicant && candidates[i].transactionId == displayed.selection.applicantTransaction) {
				next = (i + 1) % candidates.size();
				break;
			}
		}
		m_Chosen[displayed.stableSeat] = NetSelectModerationSeat(displayed.view, candidates[next].connection);
	}

	bool NetModerationUx::Available(const Row& row, NetModerationAction action) {
		return NetModerationAvailability(row.view, row.selection, action) == NetH4ModerationResult::Ok;
	}

	NetH4ModerationResult NetModerationUx::Act(size_t index, NetModerationAction action) {
		if (index >= m_Rows.size()) {
			m_StatusText = "That seat is not one this host decides about.";
			return NetH4ModerationResult::UnknownSeat;
		}
		return Act(m_Rows[index], action);
	}

	NetH4ModerationResult NetModerationUx::Act(const Row& displayed, NetModerationAction action) {
		const Row row = displayed;
		const NetH4ModerationResult result = Available(row, action) ? g_NetMatchService.ApplyModeration(row.selection, action) : NetH4ModerationResult::ActionUnavailable;
		const char* name = NetModerationActionName(action);
		m_StatusText = result == NetH4ModerationResult::Ok
		                   ? "Seat " + std::to_string(row.stableSeat) + ": " + name + " accepted."
		                   : "Seat " + std::to_string(row.stableSeat) + ": " + name + " refused - " + NetH4ModerationResultName(result) + ".";
		std::cout << "[net-moderation] " << name << " seat=" << row.stableSeat
		          << " applicant=" << static_cast<int>(row.applicant) << " result=" << NetH4ModerationResultName(result) << std::endl;
		return result;
	}

	std::string NetModerationUx::GetSummaryText() const {
		if (m_Rows.empty()) {
			return "No seat needs a decision.";
		}
		size_t applicants = 0;
		for (const Row& row: m_Rows) {
			applicants += row.applicants;
		}
		return std::to_string(m_Rows.size()) + (m_Rows.size() == 1 ? " seat waiting, " : " seats waiting, ") +
		       std::to_string(applicants) + (applicants == 1 ? " applicant" : " applicants");
	}

	bool NetSeatPresence::ApplySnapshot(const NetLockstepSeatSnapshot& snapshot, uint64_t receivedAtMs) {
		if (m_Snapshot && snapshot.epoch == m_Snapshot->epoch && snapshot.sessionId == m_Snapshot->sessionId &&
		    snapshot.roundId == m_Snapshot->roundId && snapshot.revision <= m_Snapshot->revision) return false;
		m_Seats.clear();
		for (const auto& seat: snapshot.seats) m_Seats.emplace(seat.peerId, seat);
		m_Snapshot = snapshot;
		m_ReceivedAtMs = receivedAtMs;
		return true;
	}

	void NetSeatPresence::NoteFrame(uint64_t appliedFrame) {
		m_Frame = appliedFrame;
	}

	void NetSeatPresence::Clear() {
		m_Seats.clear();
		m_Snapshot.reset();
		m_ReceivedAtMs = 0;
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

	uint64_t NetSeatPresence::HoldWallSecondsRemaining(uint8_t peerId, uint64_t nowMs) const {
		const auto it = m_Seats.find(peerId);
		if (!m_Snapshot || it == m_Seats.end() || !it->second.holdActive ||
		    it->second.holdUntilMs <= m_Snapshot->observedAtMs) return 0;
		const uint64_t atReceipt = it->second.holdUntilMs - m_Snapshot->observedAtMs;
		const uint64_t elapsed = nowMs > m_ReceivedAtMs ? nowMs - m_ReceivedAtMs : 0;
		return elapsed >= atReceipt ? 0 : (atReceipt - elapsed + 999) / 1000;
	}

	std::string NetSeatPresence::Line(uint8_t peerId, const std::string& playerName) const {
		const auto it = m_Seats.find(peerId);
		if (it == m_Seats.end() || it->second.state == NetSeatPresenceState::Present) {
			return std::string();
		}
		const NetSeatPresenceEntry& seat = it->second;
		const std::string who = !seat.holderName.empty() ? seat.holderName :
		                        (playerName.empty() ? "Player " + std::to_string(peerId) : playerName);
		switch (seat.state) {
			case NetSeatPresenceState::Disconnected:
			case NetSeatPresenceState::Reconnecting:
				return who + ": " + (seat.state == NetSeatPresenceState::Reconnecting ? "reconnecting" : "disconnected") +
				       (seat.holdActive ? " - round hold " + std::to_string(HoldWallSecondsRemaining(peerId)) + "s" : "");
			case NetSeatPresenceState::Substituted:
				return who + ": joined as substitute";
			case NetSeatPresenceState::Left:
				return who + ": left";
			default:
				return std::string();
		}
	}

	uint64_t NetSeatPresence::HoldSeconds(uint64_t frames) {
		// The pinned 0.0166666 s timestep, rounded up without overflowing the product.
		return (frames / 10000000) * 166666 + ((frames % 10000000) * 166666 + 9999999) / 10000000;
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
