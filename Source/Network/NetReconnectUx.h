#pragma once

#include "NetReconnectSession.h"
#include "NetReconnectTicketStore.h"

#include <cstdint>
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
	};

	/// What the startup scan of the recovery record found. The protocol does not care; the player does.
	enum class NetReconnectOffer : uint8_t {
		None = 0,
		Available = 1, //!< A usable record: offer to rejoin the match it names.
		Corrupt = 2,
		Stale = 3,
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
		/// immediately.
		void NoteDropped(uint64_t nowMs, std::string reason);
		void NoteReconnected(uint64_t nowMs);
		/// @return Whether an attempt is due now. The caller starts it and reports back.
		bool Tick(uint64_t nowMs);
		void NoteAttemptStarted(uint64_t nowMs);
		void NoteAttemptFailed(uint64_t nowMs, std::string reason);

		/// Stops the automatic attempts. The record is untouched - a cancel is not a leave.
		void Cancel(uint64_t nowMs);
		/// Reopens the window from now, whatever state we were in.
		void RequestManualRetry(uint64_t nowMs);
		bool CanCancel() const;
		bool CanRetryManually() const;

		/// Records what the startup scan of the store found, so the landing screen can offer the rejoin
		/// or say precisely why it cannot.
		void OfferStoredTicket(NetH4TicketLoadResult load, std::string hostAddress);
		void DismissOffer();
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

		static const char* StateName(NetReconnectUxState state);

	private:
		NetReconnectUxState m_State = NetReconnectUxState::Idle;
		uint64_t m_DroppedAtMs = 0;
		uint64_t m_NextAttemptMs = 0;
		uint32_t m_Attempts = 0;
		std::string m_Reason;
		NetReconnectOffer m_Offer = NetReconnectOffer::None;
		std::string m_OfferAddress;
	};

} // namespace RTE
