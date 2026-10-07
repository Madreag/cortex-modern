#pragma once

#include "NetLockstep.h"
#include "NetReconnectSession.h"
#include "NetReconnectTicketStore.h"

#include <cstdint>
#include <map>
#include <string>

namespace RTE {

	/// Where a dropped player is in the §11 recovery flow. Every transition is driven by the caller's
	/// clock, so the whole flow is drivable without a socket, a window or a real match.
	enum class NetReconnectUxState : uint8_t {
		Idle = 0,        //!< Nothing to recover.
		Connected = 1,   //!< In a match with a live link.
		Waiting = 2,     //!< Dropped, counting down to the next automatic attempt.
		Retrying = 3,    //!< An attempt is in flight.
		Reconnected = 4, //!< Back in the match.
		GaveUp = 5,      //!< The resume window closed; only a manual retry starts another attempt.
		Cancelled = 6,   //!< The player stopped the automatic retries.
		Refused = 7,     //!< The host gave the seat away or released it: nothing is left to rejoin.
	};

	/// What the startup scan of the recovery record found. The protocol does not care; the player does.
	enum class NetReconnectOffer : uint8_t {
		None = 0,      //!< Nothing to say: no scan has run, or the player dismissed what it found.
		Available = 1, //!< A usable record: offer to rejoin the match it names.
		Corrupt = 2,
		Stale = 3,
		Missing = 4, //!< The scan ran and found no record; §11 says so rather than saying nothing.
		Dismissed = 5, //!< The usable ticket remains for manual rejoin, with no automatic prompt or watch.
	};

	/// The reconnect UX (§11): the automatic-retry schedule with its cancel and manual-retry controls,
	/// the startup offer of a stored recovery record, and the persistent status text the lobby shows.
	/// It owns no clock and no I/O - the service feeds it time and outcomes and renders what it says.
	class NetReconnectUx {
	public:
		// One attempt per full P3 ladder (250 ms x 8), so a retry never races the previous attempt's own
		// retransmits, for the whole P2 resume window - the same horizon the host holds a seat open for.
		static constexpr uint64_t c_AttemptIntervalMs = NetReconnectHost::c_RetransmitIntervalMs * NetReconnectHost::c_MaxRetransmits;
		static constexpr uint64_t c_ResumeWindowMs = NetReconnectHost::c_ProvisionalExpiryMs;
		static constexpr uint32_t c_MaxAttempts = static_cast<uint32_t>(c_ResumeWindowMs / c_AttemptIntervalMs);

		void NoteConnected(uint64_t nowMs);
		/// The link is gone and a recovery record exists: the automatic schedule starts, first attempt
		/// immediately. Ignored while a schedule is already running, cancelled or spent.
		void NoteDropped(uint64_t nowMs, std::string reason);
		void NoteReconnected(uint64_t nowMs);
		/// @return Whether an attempt is due now. The caller starts it and reports back.
		bool Tick(uint64_t nowMs);
		void NoteAttemptStarted(uint64_t nowMs);
		void NoteAttemptFailed(uint64_t nowMs, std::string reason);
		/// The host's final answer to a rejoin: the seat is no longer this player's, so nothing retries and nothing is offered.
		void NoteRefused(std::string reason);
		bool IsRefused() const { return m_State == NetReconnectUxState::Refused; }

		/// Stops the automatic attempts. The record is untouched - a cancel is not a leave.
		void Cancel(uint64_t nowMs);
		/// Reopens the window from now, whatever state we were in.
		void RequestManualRetry(uint64_t nowMs);
		void SetRetryWindowMs(uint64_t windowMs) { m_ResumeWindowMs = windowMs; }
		bool CanCancel() const;
		bool CanRetryManually() const;

		/// Records what the startup scan of the store found, so the landing screen can offer the rejoin
		/// or say precisely why it cannot.
		void OfferStoredTicket(NetH4TicketLoadResult load, std::string hostAddress, std::string matchName = {});
		void DismissOffer();
		/// Keeps the stored address available manually while suppressing the startup prompt.
		void DismissStoredOffer();

		/// 7e: the match died with its host and no successor took it, so there is nobody to rejoin yet.
		/// The prompt stays on the screen and watches the directory for that session's row to return.
		/// @param matchName What to call the match in the prompt.
		/// @param directorySessionId The row to watch; empty leaves only the manual address.
		void WatchForHostReturn(std::string matchName, std::string directorySessionId);
		/// The directory poll's answer for the watched row; a returned host enables the rejoin.
		void NoteHostReturn(bool present, const std::string& matchName = {});
		/// There is no directory to watch, or it cannot be reached: the prompt stops waiting on a row it
		/// will never see and leaves the player the address route it always had.
		void NoteHostUnwatchable(std::string reason);
		void StopWatchingForHostReturn();
		bool IsAwaitingHostReturn() const { return m_AwaitingHostReturn; }
		bool HasHostReturned() const { return m_AwaitingHostReturn && m_HostReturned; }
		/// Whether the watch can say anything at all about that host. False leaves the rejoin pressable.
		bool CanWatchHostReturn() const { return m_AwaitingHostReturn && !m_AwaitSessionId.empty() && m_WatchReason.empty(); }
		const std::string& GetWatchedSessionId() const { return m_AwaitSessionId; }
		/// The prompt's own line: waiting for the host, or the host is back.
		std::string GetHostReturnText() const;
		NetReconnectOffer GetOffer() const { return m_Offer; }
		const std::string& GetOfferAddress() const { return m_OfferAddress; }
		std::string GetOfferText() const;

		NetReconnectUxState GetState() const { return m_State; }
		uint32_t GetAttempts() const { return m_Attempts; }
		uint64_t GetNextAttemptMs() const { return m_NextAttemptMs; }
		const std::string& GetReason() const { return m_Reason; }
		/// The persistent one-line status - never a toast; the lobby shows it until it changes.
		std::string GetStatusText() const;
		/// Whether the recovery banner should be on screen at all.
		bool IsActive() const;

		/// Whether §11's recovery applies to a lost session at all. It is a MATCH feature: a session
		/// that never left the lobby has no seat to reclaim, and retrying one drags the player back
		/// into a lobby that is gone instead of returning them to the menu.
		/// @param failed Whether the service settled into Failed.
		/// @param isHost Whether this peer hosts; host loss is out of scope.
		/// @param hasRecord Whether a recovery record survives.
		/// @param matchWasRunning Whether this session ever reached a running match.
		static constexpr bool RecoveryApplies(bool failed, bool isHost, bool hasRecord, bool matchWasRunning) {
			return failed && !isHost && hasRecord && matchWasRunning;
		}

		/// The §11 roster mark for another player's seat, or "" while the seat is fine.
		static const char* RosterMark(bool dropped, bool reclaiming);
		/// In-match leave and catch-up copy: a held seat can be reassigned by the host.
		static const char* HeldSeatReturnNotice();

		static const char* StateName(NetReconnectUxState state);

	private:
		NetReconnectUxState m_State = NetReconnectUxState::Idle;
		uint64_t m_DroppedAtMs = 0;
		uint64_t m_ResumeWindowMs = c_ResumeWindowMs;
		uint64_t m_NextAttemptMs = 0;
		uint32_t m_Attempts = 0;
		std::string m_Reason;
		NetReconnectOffer m_Offer = NetReconnectOffer::None;
		std::string m_OfferAddress;
		std::string m_OfferName;
		bool m_AwaitingHostReturn = false;
		bool m_HostReturned = false;
		std::string m_AwaitMatchName;
		std::string m_AwaitSessionId;
		std::string m_WatchReason; //!< Why the row cannot be watched; empty while it can be.
	};

	/// The seats panel's title line. The hold it reports is the ROUND's pause state, so the panel and
	/// the stall overlay cannot say different things about the same moment; a pause for everyone reads as the pause menu reads it.
	std::string NetModerationPanelTitle(bool running, bool holdPause, const std::string& holdName, uint32_t holdSeconds, bool sharedPause);

	/// §9b's moderation panel as a model: the rows the host sees and the three actions it can take.
	/// The panel renders this and the headless driver drives this, so a gate exercises the path a
	/// player's click takes - the same rows, the same choice of applicant, the same service calls.
	class NetModerationUx {
	public:
		struct Row {
			NetH4ModerationSeat view;
			NetModerationSelection selection;
			uint16_t stableSeat = 0;
			uint8_t lockstepPeerId = 0;
			std::string text;          //!< The seat's line: who, how long, how much hold is left, who is waiting.
			std::string applicantText; //!< The chosen applicant, or why there is none.
			NetPeerId applicant = c_InvalidNetPeerId;
			size_t applicants = 0;
			bool substitutable = false;
			bool substituting = false;
		};

		/// Rebuilds the rows from the host's view, keeping each seat's chosen applicant across refreshes.
		void Refresh(const std::vector<NetH4ModerationSeat>& seats);
		size_t RowCount() const { return m_Rows.size(); }
		const Row& GetRow(size_t index) const { return m_Rows[index]; }
		/// The row for a seat, or RowCount() when the seat is not one the host may decide about.
		size_t FindSeat(uint16_t stableSeat) const;
		/// Moves to the next applicant for the row's seat, wrapping.
		void CycleApplicant(size_t index);
		void CycleApplicant(const Row& displayed);
		/// Picks the row's request at index, as a pick from the list of the people asking.
		void ChooseApplicant(const Row& displayed, size_t index);
		static bool Available(const Row& row, NetModerationAction action);
		/// Runs the row's action through the service's §9b API and records what it answered.
		NetH4ModerationResult Act(size_t index, NetModerationAction action);
		NetH4ModerationResult Act(const Row& displayed, NetModerationAction action);

		const std::string& GetStatusText() const { return m_StatusText; }
		/// The panel's own line, so an empty panel says why it is empty.
		std::string GetSummaryText() const;

		/// The seat's line: who holds it, why it is held and who is waiting.
		static std::string DescribeSeat(const NetH4ModerationSeat& seat);
		/// Why a held seat is held, for the host: "Left 2 min ago", "Connection lost 10 s ago" or "Machine too slow"; empty when it is not held.
		static std::string HoldCause(const NetH4ModerationSeat& seat);
		/// A refused action's reason in the host's words.
		static std::string ResultWords(NetH4ModerationResult result);
		/// The player a row's give-away names: the chosen request, else the approved one.
		static std::string ApplicantName(const Row& row);

	private:
		std::vector<Row> m_Rows;
		std::map<uint16_t, NetModerationSelection> m_Chosen;
		std::string m_StatusText;
	};

	/// What a joiner the host refused can answer from the landing (§9b): apply to substitute in a running match, apply for its own
	/// held seat, or, in a world whose slots are all held for their players, apply for one or wait for one to open.
	enum class NetJoinRefusalOffer : uint8_t { None, Substitute, OwnSeat, SlotsHeld };
	/// The offer the host's refusal key names.
	NetJoinRefusalOffer NetJoinRefusalOfferOf(const std::string& mismatchKey);
	/// The landing's line for the offer, and its Apply button's caption.
	const char* NetJoinRefusalOfferLine(NetJoinRefusalOffer offer);
	const char* NetJoinRefusalApplyCaption(NetJoinRefusalOffer offer);

} // namespace RTE
