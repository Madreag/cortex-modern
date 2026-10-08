#include "NetReconnectUx.h"
#include "DiagnosticLine.h"

#include "NetMatchService.h"

#include <algorithm>
#include <iostream>
#include <utility>

namespace RTE {

	void NetReconnectUx::NoteConnected(uint64_t nowMs) {
		(void)nowMs;
		m_State = NetReconnectUxState::Connected;
		m_ResumeWindowMs = c_ResumeWindowMs;
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
		m_ResumeWindowMs = c_ResumeWindowMs;
		m_Reason.clear();
	}

	bool NetReconnectUx::Tick(uint64_t nowMs) {
		if (m_State != NetReconnectUxState::Waiting) {
			return false;
		}
		if (m_Attempts >= c_MaxAttempts || (nowMs >= m_DroppedAtMs && nowMs - m_DroppedAtMs > m_ResumeWindowMs)) {
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
		m_State = m_Attempts >= c_MaxAttempts || (nowMs >= m_DroppedAtMs && nowMs - m_DroppedAtMs > m_ResumeWindowMs)
		              ? NetReconnectUxState::GaveUp
		              : NetReconnectUxState::Waiting;
	}

	void NetReconnectUx::NoteRefused(std::string reason) {
		m_Reason = std::move(reason);
		m_State = NetReconnectUxState::Refused;
		DismissOffer();
		StopWatchingForHostReturn();
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
		if (!CanRetryManually() && m_Offer != NetReconnectOffer::Available) {
			return;
		}
		m_State = NetReconnectUxState::Waiting;
		m_DroppedAtMs = nowMs;
		m_NextAttemptMs = nowMs;
		m_Attempts = 0;
		m_Reason.clear();
	}

	bool NetReconnectUx::CanCancel() const {
		return m_State == NetReconnectUxState::Waiting || m_State == NetReconnectUxState::Retrying;
	}

	bool NetReconnectUx::CanRetryManually() const {
		return m_State == NetReconnectUxState::GaveUp || m_State == NetReconnectUxState::Cancelled;
	}

	void NetReconnectUx::OfferStoredTicket(NetH4TicketLoadResult load, std::string hostAddress, std::string matchName) {
		m_OfferName = std::move(matchName);
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
				m_Offer = NetReconnectOffer::Missing;
				break;
		}
		m_OfferAddress.clear();
		m_OfferName.clear();
	}

	void NetReconnectUx::DismissOffer() {
		m_Offer = NetReconnectOffer::None;
		m_OfferAddress.clear();
		m_OfferName.clear();
	}

	void NetReconnectUx::DismissStoredOffer() {
		if (m_Offer == NetReconnectOffer::Available) m_Offer = NetReconnectOffer::Dismissed;
		StopWatchingForHostReturn();
	}

	void NetReconnectUx::WatchForHostReturn(std::string matchName, std::string directorySessionId) {
		m_AwaitingHostReturn = true;
		m_HostReturned = false;
		m_AwaitMatchName = matchName.empty() || matchName.starts_with("ice:") || matchName.starts_with("iceip:") || matchName.starts_with("iceid:") ? "your match" : std::move(matchName);
		m_AwaitSessionId = std::move(directorySessionId);
	}

	void NetReconnectUx::NoteHostReturn(bool present, const std::string& matchName) {
		if (m_AwaitingHostReturn) {
			if (present && !matchName.empty()) m_AwaitMatchName = m_OfferName = matchName;
			m_HostReturned = present;
			m_WatchReason.clear();
		}
	}

	void NetReconnectUx::NoteHostUnwatchable(std::string reason) {
		if (m_AwaitingHostReturn) {
			m_HostReturned = false;
			m_WatchReason = reason.empty() ? "there is no directory to watch" : std::move(reason);
		}
	}

	void NetReconnectUx::StopWatchingForHostReturn() {
		m_AwaitingHostReturn = false;
		m_HostReturned = false;
		m_AwaitMatchName.clear();
		m_AwaitSessionId.clear();
		m_WatchReason.clear();
	}

	std::string NetReconnectUx::GetHostReturnText() const {
		if (!m_AwaitingHostReturn) {
			return {};
		}
		if (m_HostReturned) {
			return m_AwaitMatchName + " is back - rejoin now.";
		}
		if (!CanWatchHostReturn()) {
			const std::string why = m_WatchReason.empty() ? "there is no directory to watch" : m_WatchReason;
			return "Rejoin " + m_AwaitMatchName + " when the host returns - " + why + ", so try it or type the host's address.";
		}
		return "Rejoin " + m_AwaitMatchName + " when the host returns. Watching for it to come back.";
	}

	std::string NetReconnectUx::GetOfferText() const {
		switch (m_Offer) {
			case NetReconnectOffer::Available:
				if (!m_OfferName.empty()) return "Rejoin " + m_OfferName + "?";
				return "Rejoin your match?";
			case NetReconnectOffer::Corrupt: return "The saved rejoin information is damaged and cannot be used.";
			case NetReconnectOffer::Stale: return "The saved rejoin information is too old to use.";
			case NetReconnectOffer::Missing: return "No reconnect record for that match.";
			case NetReconnectOffer::None: break;
			case NetReconnectOffer::Dismissed: break;
		}
		return "";
	}

	std::string NetReconnectUx::GetStatusText() const {
		const std::string tail = m_Reason.empty() ? "" : " (" + m_Reason + ")";
		switch (m_State) {
			case NetReconnectUxState::Waiting:
				return m_Attempts == 0 ? "Preparing to rejoin the match..." : "Rejoin attempt " + std::to_string(m_Attempts) + " of " +
				       std::to_string(c_MaxAttempts) + " failed; retrying shortly" + tail;
			case NetReconnectUxState::Retrying:
				return "Rejoining the match... attempt " + std::to_string(m_Attempts == 0 ? 1U : m_Attempts) + " of " +
				       std::to_string(c_MaxAttempts) + tail;
			case NetReconnectUxState::Reconnected: return "Back in the match.";
			case NetReconnectUxState::GaveUp: return "Could not rejoin" + tail + ". Retry to try again.";
			case NetReconnectUxState::Cancelled: return "Stopped rejoining. Retry to try again.";
			case NetReconnectUxState::Refused: return m_Reason;
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

	const char* NetReconnectUx::HeldSeatReturnNotice() {
		return "The AI plays your units; your seat is held until the host reassigns it.";
	}

	std::string NetModerationPanelTitle(bool running, bool holdPause, const std::string& holdName, uint32_t holdSeconds, bool sharedPause) {
		if (!running) {
			return "PLAYERS  /  Restoring the shared match state...";
		}
		if (!holdPause) {
			return sharedPause ? "PLAYERS  /  The match is paused for everyone" : "PLAYERS  /  The match continues while this panel is open";
		}
		return "PLAYERS  /  Match paused: waiting for " + (holdName.empty() ? std::string("a player") : holdName) +
		       " to return (" + std::to_string(holdSeconds) + "s left)";
	}

	const char* NetModerationActionName(NetModerationAction action) {
		switch (action) {
			case NetModerationAction::Wait: return "wait";
			case NetModerationAction::Substitute: return "substitute";
			case NetModerationAction::Cancel: return "cancel";
		}
		return "unknown";
	}

	std::string NetModerationUx::HoldCause(const NetH4ModerationSeat& seat) {
		const auto ago = [](uint64_t ms) {
			const uint64_t seconds = ms / 1000;
			return seconds < 60 ? std::to_string(seconds) + " s ago" : std::to_string(seconds / 60) + " min ago";
		};
		if (seat.closed) return {};
		switch (seat.holdCause) {
			case NetSeatHoldCause::Capacity: return "Machine too slow";
			case NetSeatHoldCause::LateStream: return "Inputs arrived too late";
			case NetSeatHoldCause::TimingAck: return "Waiting for the player to accept the input delay";
			case NetSeatHoldCause::Quiet: return "Stopped receiving this player's input";
			case NetSeatHoldCause::OwnSeat: return "Catching up before taking control";
			case NetSeatHoldCause::Crash: return "The player's game stopped";
			case NetSeatHoldCause::RejoinFailed: return "The return could not catch up";
			case NetSeatHoldCause::Leave: return "Left " + ago(seat.leftForMs);
			case NetSeatHoldCause::LinkDrop: return "Connection lost " + ago(seat.droppedForMs);
			case NetSeatHoldCause::Released: return "The host opened the seat";
			case NetSeatHoldCause::Kicked: return "Removed by the host";
			case NetSeatHoldCause::Banned: return "Banned by the host";
			case NetSeatHoldCause::None: break;
		}
		if (seat.leftByChoice) return "Left " + ago(seat.leftForMs);
		if (seat.slowMachine) return "Machine too slow";
		if (seat.dropped) return "Connection lost " + ago(seat.droppedForMs);
		return {};
	}

	std::string NetModerationUx::DescribeSeat(const NetH4ModerationSeat& seat) {
		std::string text = seat.displayName.empty() ? "Player " + std::to_string(seat.lockstepPeerId) : seat.displayName;
		if (const std::string cause = HoldCause(seat); !cause.empty()) {
			text += " - " + cause;
		} else if (seat.closed) {
			text += " - left";
		}
		const size_t requests = seat.applicants.size();
		text += " - " + (requests == 0 ? std::string("no requests") : requests == 1 ? std::string("1 request") : std::to_string(requests) + " requests");
		return text;
	}

	void NetModerationUx::Refresh(const std::vector<NetH4ModerationSeat>& seats) {
		m_Rows.clear();
		for (const NetH4ModerationSeat& seat: seats) {
			if (seat.cpu || (!seat.held && !seat.substitutable && !seat.substituting && !seat.dropped && !seat.slowMachine)) {
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
				row.applicantText = seat.applicants.empty() ? "No requests" : "Choose a player";
			} else {
				const NetH4ApplicantView& applicant = seat.applicants[index];
				row.applicant = applicant.connection;
				// An applicant that carries the held seat's own player's name is shown as such; the host still decides.
				row.applicantText = applicant.displayName + (seat.held && applicant.displayName == seat.displayName ? " - same name" : "") + " (" +
				                    std::to_string(index + 1) + "/" + std::to_string(seat.applicants.size()) + ")";
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

	void NetModerationUx::ChooseApplicant(const Row& displayed, size_t index) {
		if (!displayed.view.actionsAvailable || index >= displayed.view.applicants.size()) return;
		m_Chosen[displayed.stableSeat] = NetSelectModerationSeat(displayed.view, displayed.view.applicants[index].connection);
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

	std::string NetModerationUx::ResultWords(NetH4ModerationResult result) {
		switch (result) {
			case NetH4ModerationResult::Ok: return "done";
			case NetH4ModerationResult::NotHosting: return "only the host can do that";
			case NetH4ModerationResult::UnknownSeat:
			case NetH4ModerationResult::StaleSelection: return "that place changed - choose it again";
			case NetH4ModerationResult::SeatNotSubstitutable: return "that place is not free to give away";
			case NetH4ModerationResult::UnknownApplicant: return "that player is no longer asking";
			case NetH4ModerationResult::SubstitutionInFlight: return "another player is already joining in that place";
			case NetH4ModerationResult::NoSubstitutionPending: return "nobody is joining in that place";
			case NetH4ModerationResult::ProviderUnavailable:
			case NetH4ModerationResult::ActionUnavailable: return "that is not possible right now";
		}
		return "that is not possible right now";
	}

	std::string NetModerationUx::ApplicantName(const Row& row) {
		for (const NetH4ApplicantView& applicant: row.view.applicants) {
			if (applicant.connection == row.selection.applicant && !applicant.displayName.empty()) return applicant.displayName;
		}
		for (const NetH4ApplicantView& applicant: row.view.applicants) {
			if (applicant.approved && !applicant.displayName.empty()) return applicant.displayName;
		}
		return row.view.substituteName.empty() ? std::string("the approved player") : row.view.substituteName;
	}

	NetH4ModerationResult NetModerationUx::Act(const Row& displayed, NetModerationAction action) {
		const Row row = displayed;
		const NetH4ModerationResult result = Available(row, action) ? g_NetMatchService.ApplyModeration(row.selection, action) : NetH4ModerationResult::ActionUnavailable;
		const char* name = NetModerationActionName(action);
		const std::string player = row.view.displayName.empty() ? "Seat " + std::to_string(row.stableSeat) : row.view.displayName;
		const std::string applicant = ApplicantName(row);
		if (result == NetH4ModerationResult::Ok) {
			m_StatusText = action == NetModerationAction::Wait       ? player + "'s place stays theirs; the AI keeps playing it."
			               : action == NetModerationAction::Substitute ? applicant + " is joining in " + player + "'s place."
			                                                           : applicant + " will not join.";
		} else {
			const std::string tried = action == NetModerationAction::Wait       ? "keep " + player + "'s place"
			                          : action == NetModerationAction::Substitute ? "let " + applicant + " join"
			                                                                      : "cancel the approval";
			m_StatusText = "Could not " + tried + ": " + ResultWords(result) + ".";
		}
		DiagnosticLine() << "[net-moderation] " << name << " seat=" << row.stableSeat
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

	const char* NetReconnectUx::StateName(NetReconnectUxState state) {
		switch (state) {
			case NetReconnectUxState::Idle: return "Idle";
			case NetReconnectUxState::Connected: return "Connected";
			case NetReconnectUxState::Waiting: return "Waiting";
			case NetReconnectUxState::Retrying: return "Retrying";
			case NetReconnectUxState::Reconnected: return "Reconnected";
			case NetReconnectUxState::GaveUp: return "GaveUp";
			case NetReconnectUxState::Cancelled: return "Cancelled";
			case NetReconnectUxState::Refused: return "Refused";
		}
		return "Unknown";
	}

	NetJoinRefusalOffer NetJoinRefusalOfferOf(const std::string& mismatchKey) {
		if (mismatchKey == "live_match") return NetJoinRefusalOffer::Substitute;
		if (mismatchKey == "seat_held_for_you") return NetJoinRefusalOffer::OwnSeat;
		if (mismatchKey == "slots_held") return NetJoinRefusalOffer::SlotsHeld;
		return NetJoinRefusalOffer::None;
	}

	const char* NetJoinRefusalOfferLine(NetJoinRefusalOffer offer) {
		switch (offer) {
			case NetJoinRefusalOffer::Substitute: return "The match is already in progress. Apply to substitute for a dropped player?";
			case NetJoinRefusalOffer::OwnSeat: return "Your slot is held for you. Apply to rejoin, the host decides";
			case NetJoinRefusalOffer::SlotsHeld: return "All slots are held for returning players. Apply for a slot or wait";
			case NetJoinRefusalOffer::None: break;
		}
		return "";
	}

	const char* NetJoinRefusalApplyCaption(NetJoinRefusalOffer offer) {
		switch (offer) {
			case NetJoinRefusalOffer::Substitute: return "Apply to Substitute";
			case NetJoinRefusalOffer::OwnSeat: return "Apply to Rejoin";
			case NetJoinRefusalOffer::SlotsHeld: return "Apply for a Slot";
			case NetJoinRefusalOffer::None: break;
		}
		return "";
	}

} // namespace RTE
