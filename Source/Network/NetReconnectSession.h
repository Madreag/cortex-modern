#pragma once

#include "NetGameCommand.h"
#include "NetMatchConfig.h"
#include "NetProtocol.h"
#include "NetReconnectAdmission.h"
#include "NetReconnectLedger.h"
#include "NetReconnectTicketStore.h"
#include "NetReconnectTxCache.h"
#include "NetSeatRoster.h"
#include "NetTransport.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace RTE {

	class NetHostBanStore;
	class NetSeatAuthRegistry;

	/// The refusal key of a return the host will take later with the same ticket (a backoff, a host change): its client retries, never joins anew.
	constexpr const char* c_ReturnRetryKey = "return_retry";

	/// One admission reply the session owes a connection.
	struct NetH4Outbound {
		NetPeerId connection = c_InvalidNetPeerId;
		NetPayload payload;
	};

	/// A seat as the admission plane sees it: the stable slot index a ticket names (P9) and the peer id
	/// a committed reclaim reuses instead of allocating a fresh one (P4).
	struct NetH4Seat {
		uint16_t stableSeat = 0;
		uint8_t peerId = 0; //!< Session-assigned id: what a commit hands back, and what the session keys peers on.
		int32_t team = 0;
		bool cpu = false;
		uint8_t lockstepPeerId = 0; //!< The sim-side id: what the ledger and the reseat command name.
		bool local = false;         //!< The host's own seat. It never joins, and host loss is out of scope.

		bool operator==(const NetH4Seat&) const = default;
	};

	/// An application that names no seat: the host picks a substitutable one. It discloses nothing an
	/// application for a named seat does not already answer, and saves a joiner guessing seat numbers.
	constexpr uint16_t c_NetH4AnySubstitutableSeat = UINT16_MAX;

	/// The sim identity a seat's holder actually plays under. In an ordinary match that is the seat's
	/// own lockstep id and team. In a persistent world the world plane owns it: a promoted watcher
	/// keeps its seat but plays the slot that seat is bound to, whose id and team are the slot's.
	struct NetH4SeatSimIdentity {
		uint8_t peerId = 0;
		int32_t team = 0;
		bool fromWorldSlot = false; //!< The world plane answered; a false here is the seat's own pair.

		bool operator==(const NetH4SeatSimIdentity&) const = default;
	};

	enum class NetHoldResolution : uint16_t {
		Reclaimed = 1,
		Substituted = 2,
	};

	struct NetHoldResolutionNotice {
		uint8_t lockstepPeerId = 0;
		NetHoldResolution resolution = NetHoldResolution::Reclaimed;
	};

	/// The seat table in the pinned form, straight off the live match config.
	std::vector<NetH4Seat> NetH4BuildSeatTable(const NetMatchConfig& config);

	/// The admission plane's clock: milliseconds elapsed since the session began. Its deadlines are real
	/// time, so a tick that pumps twice, a stall that pumps hundreds of times and a pause that pumps none
	/// all have to read the same elapsed value. Counting pumps instead halves the P2 window.
	class NetAdmissionClock {
	public:
		void Start(uint64_t steadyNowMs) {
			m_OriginMs = steadyNowMs;
			m_Started = true;
		}
		bool IsStarted() const { return m_Started; }
		/// Time this peer spent with its pump parked is not time an admission was given to answer: the origin
		/// moves forward by exactly the parked span, so every deadline measured on this clock sees un-parked time.
		void NotePark(uint64_t parkedMs, uint64_t steadyNowMs) {
			const uint64_t moved = std::min(parkedMs, steadyNowMs > m_OriginMs ? steadyNowMs - m_OriginMs : 0);
			m_OriginMs += moved;
			m_ParkedMs += moved;
		}
		uint64_t ParkedMs() const { return m_ParkedMs; }
		/// @return Milliseconds since Start; zero before it, and never backwards if the clock hiccups.
		uint64_t NowMs(uint64_t steadyNowMs) const { return m_Started && steadyNowMs > m_OriginMs ? steadyNowMs - m_OriginMs : 0; }

	private:
		uint64_t m_OriginMs = 0;
		uint64_t m_ParkedMs = 0;
		bool m_Started = false;
	};

	/// What a disconnecting transport meant to the seats.
	enum class NetH4DisconnectOutcome : uint8_t {
		Unknown = 0,    //!< No seat ever bound this transport.
		Fenced = 1,     //!< A superseded incarnation timing out; the seat keeps its current holder.
		SeatDropped = 2, //!< The seat's active incarnation is gone; the seat is now reclaimable.
		Removed = 3,    //!< Host-authored removal; terminal, never reclaimable.
	};

	/// The binding a removal notice must name. Survivors accept only a matching current holder.
	struct NetParticipantRemovalBinding {
		uint64_t sessionId = 0;
		uint32_t round = 0;
		NetAuthBytes16 epoch{};
		uint16_t stableSeat = 0;
		uint32_t holderGeneration = 0;
		uint32_t incarnation = 0;
	};

	enum class NetParticipantRemovalVerdict : uint8_t {
		Accept = 0, //!< Terminal: this holder is gone and cannot reclaim.
		RejectForgedClient = 1,
		RejectRemappedSeat = 2,
		RejectWrongEpoch = 3,
		RejectDuplicate = 4,
		RejectStaleBinding = 5,
		RejectOldVersion = 6,
	};

	const char* NetParticipantRemovalVerdictName(NetParticipantRemovalVerdict verdict);
	/// Whether the verdict is a terminal removal. Accept only; every reject leaves the holder.
	inline bool NetParticipantRemovalIsTerminal(NetParticipantRemovalVerdict verdict) {
		return verdict == NetParticipantRemovalVerdict::Accept;
	}
	NetParticipantRemovalVerdict NetAcceptParticipantRemoval(const NetParticipantRemoval& notice, bool fromHost, const NetParticipantRemovalBinding& current, bool alreadyAppliedTx);

	enum class NetKickBanResult : uint8_t {
		Ok = 0,
		NotHosting = 1,
		UnknownSeat = 2,
		ForbiddenTarget = 3,
		StaleSelection = 4,
		ActionUnavailable = 5,
		PersistenceFailed = 6,
		UnknownIdentity = 7,
		/// Marshaled onto the setup worker; the applied result replaces this one at the next drain.
		Queued = 8,
	};

	const char* NetKickBanResultName(NetKickBanResult result);

	/// The notice and targeted transport the host confirmation dialog reads after RemoveParticipant.
	struct NetParticipantRemovalIssue {
		NetParticipantRemoval notice;
		NetPeerId connection = c_InvalidNetPeerId;
		uint8_t lockstepPeerId = 0;
		NetH4Identity identity;
		NetAuthBytes32 participantId{};
		bool hasParticipantId = false;
		std::string refusal;
	};

	/// A seat that just became this connection's. The session turns it into a ready peer so the
	/// returner sits on the seat's own peer id and the superseded link stops being one.
	struct NetH4Commit {
		NetPeerId connection = c_InvalidNetPeerId;
		uint16_t stableSeat = 0;
		uint8_t assignedPeerId = 0;
		uint32_t incarnation = 0;
		NetPeerId supersededConnection = c_InvalidNetPeerId;
		bool reclaim = false;
		bool substitute = false; //!< The seat changed hands by an explicit host action, not a reclaim.
	};

	/// A fault the §9b socket gates inject, so the failure windows can be driven over a real socket
	/// instead of only on an in-process wire. Off unless a command line asks for one; nothing reads it
	/// on a default build's happy path.
	enum class NetH4Fault : uint8_t {
		None = 0,
		AckDrop = 1,      //!< The substitute never sends its ack: the offer ladder runs out and P2 closes the window.
		AckDuplicate = 2, //!< It keeps re-sending the ack after the commit, so every duplicate meets the txId cache.
		CommitDrop = 3,   //!< The host throws its first commit result away, so the substitute has to ask again.
	};

	void NetH4SetFault(NetH4Fault fault);
	NetH4Fault NetH4GetFault();
	NetH4Fault NetH4FaultFromName(const std::string& name);

	/// One player asking the host for a seat whose holder is gone (§4, P15). An applicant holds no
	/// peer id, no team, no snapshot and no authority: it is a name on the host's list until an
	/// approved substitution commits.
	struct NetH4ApplicantView {
		NetPeerId connection = c_InvalidNetPeerId;
		uint16_t stableSeat = 0;
		std::string displayName;
		uint64_t appliedAtMs = 0;
		uint64_t expiresAtMs = 0;
		bool approved = false; //!< An approval for this applicant is in flight.
		NetAuthBytes16 transactionId{}; //!< The displayed application, even if a connection id is reused.

		bool operator==(const NetH4ApplicantView&) const = default;
	};

	/// One seat as the host's moderation view sees it (§9b): whether it is waiting for someone, and
	/// who is asking for it.
	struct NetH4ModerationSeat {
		uint16_t stableSeat = 0;
		uint8_t lockstepPeerId = 0;
		bool cpu = false;
		int32_t team = 0;
		std::string displayName; //!< The roster's name for the seat, so the host moderates a player and not a number.
		bool actionsAvailable = true; //!< Service disables actions while its worker owns the plane.
		bool committed = false;
		bool dropped = false;
		bool closed = false;
		bool heldForReclaim = false;  //!< The original holder can still return.
		bool held = false;            //!< Its player away and the AI playing it, by the roster.
		NetSeatHoldCause holdCause = NetSeatHoldCause::None; //!< Why the roster holds it.
		bool substitutable = false;   //!< A host action may reassign it right now.
		bool substituting = false;    //!< An approval is in flight for it.
		NetAuthBytes16 substitutionTransaction{};
		bool reclaiming = false;
		uint32_t incarnation = 0;
		NetAuthBytes16 epoch{};
		uint64_t holdUntilMs = 0;
		std::string substituteName; //!< Retained current-holder state, not an unread event history.
		uint32_t holderGeneration = 0;
		uint32_t seatGeneration = 0;  //!< The value a pending approval compares against at commit.
		uint64_t droppedAtMs = 0;     //!< When the holder's link went, on the admission plane's clock.
		uint64_t droppedForMs = 0;    //!< How long ago that was, so the panel needs no clock of its own.
		bool leftByChoice = false;    //!< The holder left on purpose; the seat is held for it as for a drop.
		uint64_t leftForMs = 0;       //!< How long ago it left.
		bool slowMachine = false;     //!< Held because the holder's machine cannot keep up with the round.
		std::string joinProgress;     //!< How far the player coming into the seat is: the world's image, then its replay; "" otherwise.
		std::vector<NetH4ApplicantView> applicants;

		bool operator==(const NetH4ModerationSeat&) const = default;
	};

	/// Why a host moderation action was refused. Nothing here reaches the wire.
	enum class NetH4ModerationResult : uint8_t {
		Ok = 0,
		NotHosting = 1,
		UnknownSeat = 2,
		SeatNotSubstitutable = 3,
		UnknownApplicant = 4,
		SubstitutionInFlight = 5,
		NoSubstitutionPending = 6,
		ProviderUnavailable = 7,
		StaleSelection = 8,
		ActionUnavailable = 9,
	};

	const char* NetH4ModerationResultName(NetH4ModerationResult result);

	enum class NetModerationAction : uint8_t { Wait, Substitute, Cancel };
	const char* NetModerationActionName(NetModerationAction action);

	/// Identity of the seat and application actually displayed when the host pressed a button.
	struct NetModerationSelection {
		NetAuthBytes16 epoch{};
		uint16_t stableSeat = 0;
		uint32_t holderGeneration = 0;
		uint32_t seatGeneration = 0;
		uint32_t incarnation = 0;
		NetPeerId applicant = c_InvalidNetPeerId;
		NetAuthBytes16 applicantTransaction{};
		NetAuthBytes16 substitutionTransaction{};
		bool operator==(const NetModerationSelection&) const = default;
	};

	NetModerationSelection NetSelectModerationSeat(const NetH4ModerationSeat& seat, NetPeerId applicant = c_InvalidNetPeerId);
	NetH4ModerationResult NetModerationAvailability(const NetH4ModerationSeat& seat, const NetModerationSelection& selection, NetModerationAction action);

	/// A seat's live admission status, for §11's persistent roster indication and the reports.
	struct NetH4SeatStatus {
		uint16_t stableSeat = 0;
		uint8_t lockstepPeerId = 0;
		bool committed = false;
		bool closed = false;
		bool dropped = false;      //!< Committed, but its holder's link is lost, by the roster.
		bool held = false;         //!< Committed, its player away and the AI playing it, by the roster: lost, left or held by the round.
		NetSeatHoldCause holdCause = NetSeatHoldCause::None; //!< Why the roster holds it.
		bool reclaiming = false;   //!< A reclaim transaction for it is in flight.
		bool substituting = false; //!< An approved substitute is persisting its ticket.
		uint16_t applicants = 0;   //!< Players asking the host for this seat.

		bool operator==(const NetH4SeatStatus&) const = default;
	};

	/// Whether a late joiner could still take this seat. The one rule the directory row, a resumed
	/// row and every count read: the host's own seat and a CPU slot are not seats, and a committed or
	/// closed one is taken.
	inline bool NetH4SeatIsOpen(uint8_t lockstepPeerId, uint8_t localPeerId, bool committed, bool closed) {
		return lockstepPeerId != 0 && lockstepPeerId != localPeerId && !committed && !closed;
	}

	/// Whether a running match holds this seat for a player who is gone (dropped or left), so a newcomer may apply to
	/// the host for it. The directory row's held count reads this; NetReconnectHost::IsSeatSubstitutable is the host's
	/// own form of the same rule.
	inline bool NetH4SeatIsHeld(uint8_t lockstepPeerId, uint8_t localPeerId, bool dropped, bool closed) {
		return lockstepPeerId != 0 && lockstepPeerId != localPeerId && (dropped || closed);
	}

	struct NetReconnectHostStats {
		uint32_t newJoins = 0;
		uint32_t ticketOffersSent = 0;
		uint32_t ticketOfferRetransmits = 0;
		uint32_t provisionalSeatsOpened = 0;
		uint32_t provisionalSeatsCommitted = 0;
		uint32_t provisionalSeatsExpired = 0;
		uint32_t provisionalSeatsRefused = 0;
		uint32_t provisionalSeatsResumed = 0;
		uint32_t persistenceFailures = 0;
		uint32_t reclaimsAccepted = 0;
		uint32_t identityRejections = 0;
		uint32_t denialsScheduled = 0;
		uint32_t denialsReleased = 0;
		uint32_t replayedResults = 0;
		uint32_t staleEpochDrops = 0;
		uint32_t unknownTransactionDrops = 0;
		uint32_t fencedPackets = 0;
		uint32_t fencedDisconnects = 0;
		uint32_t incarnationsBound = 0;
		uint32_t seatsDropped = 0;
		uint32_t seatsClosedByLeave = 0;
		uint32_t seatsRemoved = 0; //!< Host-authored removals; never a reclaimable drop.
		uint32_t seatsReleased = 0; //!< Seats handed back to the pool, whichever way their holder went.
		uint32_t ledgerDropsRecorded = 0;
		uint32_t reseatsIssued = 0;
		uint32_t reseatsWithoutALedger = 0;   //!< Reclaims whose seat ledgered nothing at the drop: a returner reseated onto nothing.
		uint32_t reseatsWithoutSurvivors = 0; //!< Reclaims whose ledgered units are all gone from the world; nothing to hand back.
		uint32_t reseatLiveOnTeamNotNamed = 0; //!< The most a reclaim found alive on the returner's team that its drop record does not name. Recorded, never judged.
		uint32_t reclaimRetransmitsDropped = 0;
		uint32_t answeredTransactionsDropped = 0; //!< A proof or Reclaim of a transaction its denial already answered.
		uint32_t seatHoldsExpired = 0;
		uint32_t seatsReleasedInLobby = 0;
		uint32_t applicantsRegistered = 0;
		uint32_t applicantsRefused = 0;
		uint32_t applicantsExpired = 0;
		uint32_t applicantsDisplaced = 0; //!< Told the seat went to somebody else.
		uint32_t substitutionOffersSent = 0;
		uint32_t substitutionOfferRetransmits = 0;
		uint32_t substitutionsCommitted = 0;
		uint32_t substitutionsCancelled = 0;
		uint32_t substitutionsSuperseded = 0; //!< Lost the seat-generation CAS to a returner.
		uint32_t substitutionAckFailures = 0;
		uint32_t reassignedReclaimsRefused = 0; //!< Proved the retired credential and was told why.
		uint32_t rosterRefusedReturns = 0;      //!< A proven return the seat roster refused: its backoff, its ticket or its host.
	};

	/// The host's §4/§6/§7 state machine: it runs the admission transaction, fences a superseded
	/// incarnation of a seat, closes a seat on a clean leave, and hands the match runner the
	/// system-authored reseat a returning holder has earned. It owns no transport and no clock -
	/// NetSession feeds it messages and time and sends whatever it produces.
	class NetReconnectHost {
	public:
		// P3: each handshake step retransmits at the codebase's existing cadence and gives up inside
		// the session heartbeat timeout, so the handshake fails before - not because of - the transport.
		static constexpr uint64_t c_RetransmitIntervalMs = 250;
		static constexpr uint32_t c_MaxRetransmits = 8;
		// P2: outlives one offer/persist/ack round trip plus a full retry ladder, and matches the
		// in-match "this peer is genuinely gone" horizon.
		static constexpr uint64_t c_ProvisionalExpiryMs = 20000;
		static constexpr size_t c_MaxProvisionalSeats = 4;
		// P15: the plan gates "two pending applicants for one seat", 4 global is the peer cap, and one
		// per connection stops a single socket filling the queue. Records expire at P2, so the
		// lifecycle has one lifetime constant rather than two.
		static constexpr size_t c_MaxApplicants = 4;
		static constexpr size_t c_MaxApplicantsPerSeat = 2;
		static constexpr size_t c_MaxApplicantsPerConnection = 1;

		void Configure(NetSeatAuthRegistry* registry, uint64_t hostSessionId, NetH4Identity localIdentity);
		NetAuthBytes16 GetEpoch() const;
		void SetSeatTable(std::vector<NetH4Seat> seats, NetMatchMode mode);
		/// Live match: a ticketless join is denied outright in Phase A; in a lobby it may fill a
		/// never-held seat.
		void SetLiveMatch(bool live);
		/// A persistent world admits a fresh, ticketless joiner into a LIVE round: its seats are the
		/// world's gameplay slots, freed by a clean leave under the next generation, never "used up".
		void SetPersistentWorld(bool persistent) { m_PersistentWorld = persistent; }
		bool IsPersistentWorld() const { return m_PersistentWorld; }
		/// Retains credentials between rounds without carrying world ownership into the lobby.
		void SetMatchEnded();
		bool IsLiveMatch() const { return m_LiveMatch; }
		/// Whether a seat's own player can come back to it now: a match running, or one between its rounds.
		bool HoldsSeatsForReturn() const { return m_LiveMatch || m_MatchEnded; }
		void SetHostAddress(std::string address) { m_HostAddress = std::move(address); }
		void SetMatchConfigHash(const NetHash32& hash) { m_MatchConfigHash = hash; }
		/// The actors the ledger records when a seat drops. Supplied by the match runner at the drop
		/// frame; without one, a drop records an empty ownership list and no reseat is issued.
		void SetDropOwnershipSource(std::vector<NetH4LedgerActor> (*source)(void*), void* context);
		/// The world plane's answer to "which slot is this seat bound to". Supplied by the owner of
		/// both planes; without one every seat plays its own lockstep id, which is the match case.
		void SetSeatSimIdentitySource(NetH4SeatSimIdentity (*source)(void*, uint16_t), void* context);
		/// The id and team this seat's holder plays on: the bound world slot's when there is one.
		NetH4SeatSimIdentity SimIdentityOfSeat(const NetH4Seat& seat) const;

		/// Routes one decoded H4 message. Non-H4 payloads are ignored.
		/// @return Whether the payload was an H4 message this plane handled.
		bool HandleMessage(NetPeerId connection, const NetPayload& payload, uint64_t nowMs);

		/// Expires provisional seats, retransmits unacknowledged offers and releases due denials.
		void Tick(uint64_t nowMs);

		/// Whether the connection is a superseded incarnation: its packets are dropped for the seat and
		/// its later timeout must not evict the seat.
		bool IsFenced(NetPeerId connection) const;
		void CountFencedPacket() { ++m_Stats.fencedPackets; }

		/// Tells the plane a transport went away.
		NetH4DisconnectOutcome NotifyDisconnect(NetPeerId connection, uint64_t frame);

		/// Whether the seat that lockstep peer plays is still worth waiting for: a committed seat whose
		/// holder dropped stays reclaimable for the P2 window, so the round it left must not end on it.
		bool IsSeatHeldForReclaim(uint8_t lockstepPeerId) const;

		/// Ends the hosted session: every credential dies, so every client may delete its record.
		void EndHostedSession();

		std::vector<NetH4Outbound> TakeOutbound();
		/// The seats committed since the last call.
		std::vector<NetH4Commit> TakeCommits();
		/// The seats that changed hands since the last read, for §11's roster line.
		/// The reseats a committed reclaim earned, for the match runner to enqueue as lockstep commands.
		std::vector<NetGameReseat> TakePendingReseats();
		std::vector<NetHoldResolutionNotice> TakePendingHoldResolutions();

		const NetReconnectHostStats& GetStats() const { return m_Stats; }
		const NetReconnectAdmission& GetAdmission() const { return m_Admission; }
		const NetReconnectLedger& GetLedger() const { return m_Ledger; }
		const NetReconnectTxCache& GetTxCache() const { return m_TxCache; }
		size_t GetProvisionalSeatCount() const { return m_Provisionals.size(); }

		/// §9b's moderation API: every seat, whether it may be reassigned, and who is asking for it.
		/// The host UI renders this and calls one of the three verbs below; nothing here is a secret.
		std::vector<NetH4ModerationSeat> GetModerationView() const;
		/// The round's frame, which a leave's drop is recorded at.
		void NoteLockstepFrame(uint64_t frame) { m_LockstepFrame = frame; }
		void PruneSeatRemovals(uint64_t retainedBoundary);
		/// Folds every field GetModerationView shows into one stamp, without allocating, so a caller
		/// polling at the lobby's cadence rebuilds the view only when a row actually changed.
		uint64_t GetModerationSignature() const;
		NetH4ModerationResult ApplyModeration(const NetModerationSelection& selection, NetModerationAction action, uint64_t nowMs);
		/// Keep waiting for the original holder. Explicit, so "wait" is a recorded decision rather
		/// than the absence of one.
		NetH4ModerationResult WaitForSeat(uint16_t stableSeat);
		/// Approves one applicant for one seat, atomically: the seat is never released first, and the
		/// approval hands out a provisional ticket without touching the seat's current holder.
		NetH4ModerationResult SubstituteApplicant(uint16_t stableSeat, NetPeerId applicantConnection, uint64_t nowMs);
		/// Withdraws an approval that has not committed. The provisional record is invalidated and
		/// removed; the seat was never given away, so there is nothing to take back.
		NetH4ModerationResult CancelSubstitution(uint16_t stableSeat, uint64_t nowMs);
		void SetBanStore(NetHostBanStore* store) { m_BanStore = store; ++m_StateRevision; }
		void SetMigrationCapacityCheck(std::function<bool(const std::vector<uint8_t>&, std::string&)> check) { m_MigrationCapacityCheck = std::move(check); }
		/// Rises with every change ExportMigrationState would render, so a caller can tell a plane that
		/// moved from one that did not without paying for the export itself.
		uint64_t GetStateRevision() const { return m_StateRevision; }
		void SetParticipantProofRequired(bool required) { m_ProofRequired = required; }
		void BindParticipantId(NetPeerId connection, const NetAuthBytes32& id);
		/// Host: close this holder without a reclaim hold. Reuses the clean-leave seat close.
		NetKickBanResult RemoveParticipant(const NetModerationSelection& selection, NetParticipantRemovalAction action, uint64_t nowMs, uint64_t unixNowMs, uint64_t sessionId, uint32_t round, uint64_t boundaryFrame, NetParticipantRemovalIssue& issued);
		bool HasSubstitution(uint16_t stableSeat) const;
		size_t GetApplicantCount() const { return m_Applicants.size(); }
		/// Applications a lost host left pending that no applicant has asked this host again for yet.
		size_t GetCarriedApplicantCount() const { return m_CarriedApplicants.size(); }

		/// Which peer id, if any, currently holds the seat on which transport.
		bool GetSeatHolder(uint16_t stableSeat, NetPeerId& connection, uint32_t& holderGeneration, uint32_t& incarnation) const;
		/// The committed H4 seat on this connection; none until admission has one. Seat 0 is a seat: an ordinary match's original host's.
		std::optional<uint16_t> StableSeatOfConnection(NetPeerId connection) const;
		bool IsSeatClosed(uint16_t stableSeat) const;
		/// Every seat's admission status, in stable-seat order.
		std::vector<NetH4SeatStatus> GetSeatStatuses() const;
		/// The host's seat roster: the one record of whether each seat's holder is away, why and since when.
		const NetSeatRoster& GetRoster() const { return m_Roster; }
		/// A world seat plays this slot (0: its own): the roster carries it to every peer.
		void NoteSeatSlot(uint16_t stableSeat, uint8_t slot);
		/// The roster seat a lockstep peer plays, read off the same binding the coordinator asks by; null when none.
		const NetRosterSeat* RosterSeatOfPeer(uint8_t lockstepPeerId) const;
		/// A rematch forms in its lobby: the round before it is over, its present seats go to the start and the seats whose players
		/// are away or that the host opened start held.
		void FormRematch();
		/// A returning seat's world is the round's: its image is in, or its player kept the world.
		void NoteReturnWorldReady(uint8_t lockstepPeerId);
		/// A returning seat plays the round again at a committed frame.
		void NoteReturnCaughtUp(uint8_t lockstepPeerId);
		/// The round holds a playing seat whose link stays open; Capacity is a machine too slow for the round.
		void NoteSeatHeldInPlace(uint8_t lockstepPeerId, NetSeatHoldCause cause);
		/// A seat the round held with its link open plays again: its player kept the world.
		void NoteSeatPlaysAgain(uint8_t lockstepPeerId);
		/// The lockstep peers whose seats the roster has playing, and those the round holds with their links open.
		std::vector<uint8_t> PlayingPeers() const;
		std::vector<uint8_t> HeldInPlacePeers() const;
		/// A returning seat's transfer was abandoned: the AI keeps the seat and the return is offered again after the roster's backoff.
		void NoteReturnAborted(uint8_t lockstepPeerId);
		/// The lockstep peers whose seats are on their way back, through the image or the catch-up.
		std::vector<uint8_t> ReturningPeers() const;
		/// A seat whose return failed while its player stayed connected: true when the return is offered again now (its backoff over,
		/// under the roster's bound). Past the bound the refusal names why and nothing more is offered on this link.
		bool ReofferReturn(uint8_t lockstepPeerId, std::string* refusal);
		/// The lockstep peers a forming round's start waits on - the roster's seats at the start on a live link - and the host's,
		/// sorted; empty when no round is forming.
		std::vector<uint8_t> StartMembers() const;
		/// The seat table as the plane holds it now, in table order.
		std::vector<NetH4Seat> GetSeatTable() const;
		std::vector<uint8_t> ExportMigrationState() const;
		void NoteSeatRelease(uint8_t peerId, uint64_t frame);
		static std::vector<uint8_t> MigrationStateAtFrame(const std::vector<uint8_t>& bytes, uint64_t frame);
		/// The wall clock the plane writes the roster's host-side times by when it hands them to another machine; the system clock unset.
		void SetUnixClock(uint64_t (*clock)(void*), void* context);
		/// How many seats an exported plane still offers a joiner, read without importing it, so a
		/// restarted host's directory row advertises what the match really has before the plane is live.
		/// @return The open seats, or -1 when the bytes are not an export.
		static int64_t CountExportedOpenSeats(const std::vector<uint8_t>& bytes, uint8_t localPeerId);
		/// Every mutable seat, ledger and ban path goes through NoteStateChanged, so the revision can
		/// only ever run ahead of the truth, never behind it.
		void NoteStateChanged() { ++m_StateRevision; }
		bool ImportMigrationState(const std::vector<uint8_t>& bytes, NetSeatAuthRegistry& registry, const NetMatchConfig& config, uint8_t localPeerId, const std::map<uint8_t, NetPeerId>& transports, uint64_t nowMs);
		void SetMigrationHold(bool held, uint64_t nowMs);
		void RecordMigrationDepartures(uint64_t frame);
		bool EnsureLocalTicket(NetH4TicketRecord& record);
		size_t HeldMigrationMessages() const { return m_MigrationHeldMessages.size(); }

	private:
		struct SeatState {
			NetH4Seat seat;
			// The identity the holder was admitted under, so a re-presented ack can rebuild the exact
			// transaction key the commit was cached against.
			NetH4Identity identity;
			uint32_t holderGeneration = 0;
			uint32_t incarnation = 0;
			NetPeerId activeConnection = c_InvalidNetPeerId;
			bool saturated = false;
			// The compare-and-swap value a pending substitution captures at approval. Anything that
			// changes who may hold the seat moves it, so an approval that was overtaken cannot commit.
			uint32_t seatGeneration = 1;
			uint32_t retiredGeneration = 0; //!< A generation a substitute superseded, kept only to answer it.
			uint64_t retiredUntilMs = 0;
			std::string substituteName;
			std::string holderName;
			NetAuthBytes32 participantId{};
			bool hasParticipantId = false;
		};

		/// A pending applicant. It carries an identity because §4 re-validates one on every admission
		/// message, and a display name because the host has to be able to tell two applicants apart.
		struct Applicant {
			NetPeerId connection = c_InvalidNetPeerId;
			uint16_t stableSeat = 0;
			NetAuthBytes16 txId{};
			NetH4Identity identity;
			std::string displayName;
			uint64_t appliedAtMs = 0;
			NetH4TxKey key;
			bool approved = false;
		};

		/// An approved substitution between the host action and the commit. The credential lives here
		/// and NOT in the registry until the commit, which is what makes the first COMMIT win instead
		/// of the first approval.
		struct Substitution {
			uint16_t stableSeat = 0;
			NetPeerId connection = c_InvalidNetPeerId;
			NetAuthBytes16 txId{};
			uint32_t holderGeneration = 0;
			uint32_t seatGeneration = 0;
			uint32_t supersededGeneration = 0;
			NetAuthBytes32 credential{};
			NetAuthBytes32 challenge{};
			NetH4Identity identity;
			std::string displayName;
			uint64_t openedAtMs = 0;
			uint64_t lastSentMs = 0;
			uint32_t retransmits = 0;
			NetH4TxKey key;
			NetH4SubstitutionOffer offer;
		};

		struct Provisional {
			std::string holderName;
			uint16_t stableSeat = 0;
			NetAuthBytes16 txId{};
			NetPeerId connection = c_InvalidNetPeerId;
			uint32_t holderGeneration = 0;
			uint64_t openedAtMs = 0;
			uint64_t lastSentMs = 0;
			uint32_t retransmits = 0;
			NetH4TxKey key;
			NetH4TicketOffer offer;
		};

		struct PendingReclaim {
			std::string holderName;
			NetPeerId connection = c_InvalidNetPeerId;
			NetAuthBytes16 txId{};
			uint16_t stableSeat = 0;
			uint32_t holderGeneration = 0;
			NetH4TxKey key;
			uint64_t openedAtMs = 0;
			// The generation this names was superseded by a substitute. The challenge is real and the
			// answer is a refusal either way; proving it only decides whether the refusal says why.
			bool superseded = false;
			bool proofFinished = false; //!< Keep replay suppression after a failed proof without showing an active reconnect.
		};

		struct Fence {
			NetPeerId connection = c_InvalidNetPeerId;
			uint16_t stableSeat = 0;
			uint32_t incarnation = 0;
		};

		void HandleNewJoin(NetPeerId connection, const NetH4NewJoin& message, uint64_t nowMs);
		void HandleTicketStoredAck(NetPeerId connection, const NetH4TicketStoredAck& message, uint64_t nowMs);
		void HandleReclaim(NetPeerId connection, const NetH4Reclaim& message, uint64_t nowMs);
		void HandleProof(NetPeerId connection, const NetH4Proof& message, uint64_t nowMs);
		void HandleLeaveRequest(NetPeerId connection, const NetH4LeaveRequest& message, uint64_t nowMs);
		void HandleApplicant(NetPeerId connection, const NetH4Applicant& message, uint64_t nowMs);
		void HandleSubstitutionAck(NetPeerId connection, const NetH4SubstitutionAck& message, uint64_t nowMs);

		/// P5: the identity re-validation, run before the seat lookup so a mismatch never says whether
		/// the seat exists. @return Whether the identity matches; sets a specific rejection when not.
		bool ValidateIdentity(NetPeerId connection, const NetH4Identity& identity);
		bool MatchesEpoch(const NetAuthBytes16& epoch) const;
		void DenyUniformly(NetPeerId connection, const NetAuthBytes16& txId, NetH4DenialReason reason, uint64_t nowMs);
		/// The synthetic-challenge half of the no-enumeration flow: an unknown seat and a stale epoch
		/// get the same challenge and the same delayed refusal a known seat's bad proof gets.
		void ChallengeSyntheticallyAndDeny(NetPeerId connection, const NetAuthBytes16& txId, NetH4DenialReason reason, uint64_t nowMs);
		void Send(NetPeerId connection, NetPayload payload);
		SeatState* FindSeat(uint16_t stableSeat);
		const SeatState* FindSeat(uint16_t stableSeat) const;
		/// The seat's entry in the roster, which holds whether its holder is away, why and since when.
		const NetRosterSeat* RosterSeatOf(const SeatState& seat) const;
		/// A player holds the seat: its roster seat has an owner.
		bool IsSeated(const SeatState& seat) const;
		/// The host opened the seat from the first start on (kicked, banned or released): closed to its former player, open to an applicant.
		bool IsHostOpened(const SeatState& seat) const;
		/// The holder is away and the seat is held for it.
		bool IsHolderAway(const SeatState& seat) const;
		/// The holder left on purpose and the seat is held for it.
		bool HolderLeftByChoice(const SeatState& seat) const;
		/// When the holder went away, on this plane's clock; 0 while it is here.
		uint64_t HolderAwaySinceMs(const SeatState& seat) const;
		/// Every change to a seat's hold goes through the roster's one transition function.
		void ApplySeatEvent(const SeatState& seat, NetRosterEventKind kind, bool byChoice = false, bool keptWorld = false, NetSeatHoldCause cause = NetSeatHoldCause::None);
		/// The roster holds the seat for its player: it has an owner who is away while the AI plays it.
		bool RosterHoldsSeat(const SeatState& seat) const;
		void ApplyStageEvent(NetRosterEventKind kind);
		/// Sends the roster's current revision to every connected holder, or to one connection.
		void SendRoster(NetPeerId only = c_InvalidNetPeerId);
		std::vector<std::pair<uint32_t, std::vector<uint8_t>>> m_RosterHistory; //!< The revisions this host published, oldest first, bounded.
		/// A seat whose holder never takes the round's image - the host's own, a watcher's - is back the moment it is seated.
		void SettleReturn(const SeatState& seat);
		/// The roster's owner for the seat's holder: the player's proven identity, or with none proven its ticket.
		uint64_t RosterOwnerOf(const SeatState& seat) const;
		/// The seat roster's answer to a return with the ticket of this holder generation; empty when the return may begin.
		/// retryLater says the same return is taken later.
		std::string RosterRefusesReturn(const SeatState& seat, uint32_t holderGeneration, bool* retryLater = nullptr) const;
		/// The plane seat a lockstep peer plays: a world slot's member before a seat whose own id it is.
		const SeatState* SeatOfPeer(uint8_t lockstepPeerId) const;
		SeatState* SeatOfPeer(uint8_t lockstepPeerId) { return const_cast<SeatState*>(std::as_const(*this).SeatOfPeer(lockstepPeerId)); }
		/// Seats a new holder in the roster: an open seat is admitted, a held one given to the applicant.
		void SeatHolder(const SeatState& seat);
		/// Keeps one roster seat per plane seat, the existing ones as they are.
		void RebuildRoster();
		SeatState* FindFreeNeverHeldSeat();
		/// A world's free gameplay slot: not the host's, not committed, not closed and not already being
		/// offered. Unlike a match seat it may have been held before - a clean leave gives it back.
		SeatState* FindFreeWorldSeat();
		/// The held seat a ticketless joiner owns by its proven participant identity, or by name when nothing proves it either
		/// way; only the refusal's wording rides on it, never the seat.
		const SeatState* HeldSeatOwnedBy(NetPeerId connection, const std::string& displayName) const;
		/// The refusal of a ticketless joiner the host cannot seat, naming why: its own seat held, a world's slots held, or full.
		NetJoinRejected RefuseUnseatable(NetPeerId connection, const std::string& displayName, NetRejectReason reason, const std::string& summary, const std::string& key) const;
		Provisional* FindProvisionalByTxId(const NetAuthBytes16& txId);
		bool BindIncarnation(SeatState& seat, NetPeerId connection);
		void ReleaseProvisional(uint16_t stableSeat);
		void RecordDrop(SeatState& seat, uint64_t frame);
		/// Hands a seat back to the pool. Only in a lobby: nothing has been played, so the player who
		/// left has nothing to reclaim and the seat must be joinable again.
		/// @param releasedBy Kicked for the host's removal, LinkDropped for a holder that left the first lobby.
		void ReleaseSeat(SeatState& seat, NetRosterEventKind releasedBy = NetRosterEventKind::Kicked);
		/// @param removedBy Kicked or Banned: the roster's cause for the open seat.
		void CloseSeatWithoutHold(SeatState& seat, NetRosterEventKind removedBy = NetRosterEventKind::Kicked);
		void CancelHolderTransactions(uint16_t stableSeat, uint64_t nowMs);
		/// Ends every transaction a removed link still had open, on every seat.
		void DropRemovedTransactions(NetPeerId connection);
		bool RefuseIfBanned(NetPeerId connection);
		bool LookupParticipantId(NetPeerId connection, NetAuthBytes32& out) const;
		void UnbindParticipantId(NetPeerId connection);
		void CaptureParticipant(SeatState& seat, NetPeerId connection);
		void IssueReseat(const SeatState& seat);
		void QueueHoldResolution(uint8_t lockstepPeerId, NetHoldResolution resolution);
		friend bool TestHoldResolutionPumpDoesNotRelock(std::string* error);
		friend bool TestFinishMatchDrainsFencedDisconnect(std::string* error);
		const NetPayload* FindCached(const NetAuthBytes16& txId, const NetH4TxKey& key, uint64_t nowMs);

		/// Whether an explicit host action may hand this seat to somebody else: a live match, a real
		/// human seat, and a holder who is either gone or has cleanly left.
		bool IsSeatSubstitutable(const SeatState& seat) const;
		/// Moves the seat's compare-and-swap value. Called by everything that changes who may hold it.
		void BumpSeatGeneration(SeatState& seat);
		/// The transaction key of a substitution. The host draws the transaction id, so the id itself
		/// is the unforgeable handle and the key binds only the seat and the generation it names.
		static NetH4TxKey SubstitutionKey(uint16_t stableSeat, uint32_t holderGeneration);
		Substitution* FindSubstitutionBySeat(uint16_t stableSeat);
		Substitution* FindSubstitutionByConnection(NetPeerId connection);
		Applicant* FindApplicant(NetPeerId connection, uint16_t stableSeat);
		Applicant* FindApplicantByTransaction(NetPeerId connection, const NetAuthBytes16& txId);
		/// The seat an application that names none should go to: substitutable and not already full.
		SeatState* FindSubstitutableSeatWithRoom();
		/// Ends a pending substitution: caches the terminal refusal under its transaction id so a
		/// retransmitted ack replays it, tells the substitute, and forgets the credential.
		void AbandonSubstitution(size_t index, NetH4DenialReason reason, const std::string& summary, uint64_t nowMs);
		/// Every applicant for the seat except the one that just took it hears that it is gone.
		void DisplaceApplicants(uint16_t stableSeat, NetPeerId keepConnection, uint64_t nowMs);
		void DropApplicantsFor(NetPeerId connection);

		NetSeatAuthRegistry* m_Registry = nullptr;
		uint64_t m_HostSessionId = 0;
		NetAuthBytes16 m_ConfiguredEpoch{};
		NetH4Identity m_LocalIdentity;
		NetHash32 m_MatchConfigHash{};
		std::string m_HostAddress;
		NetMatchMode m_Mode = NetMatchMode::PvPSkirmish;
		uint64_t m_NowMs = 0; //!< The plane's own clock, so a drop can be stamped without one being passed in.
		bool m_LiveMatch = false;
		uint64_t m_StateRevision = 0; //!< Bumped by every mutation the migration export would render.
		bool m_PersistentWorld = false;
		bool m_MatchEnded = false;
		std::vector<NetH4LedgerActor> (*m_DropOwnershipSource)(void*) = nullptr;
		void* m_DropOwnershipContext = nullptr;
		NetH4SeatSimIdentity (*m_SeatSimIdentitySource)(void*, uint16_t) = nullptr;
		void* m_SeatSimIdentityContext = nullptr;

		NetReconnectAdmission m_Admission;
		NetReconnectTxCache m_TxCache;
		NetReconnectLedger m_Ledger;
		std::vector<SeatState> m_Seats;
		std::vector<std::vector<uint8_t>> m_SeatRemovalUndo;
		std::function<bool(const std::vector<uint8_t>&, std::string&)> m_MigrationCapacityCheck;
		bool RememberSeatRemoval(uint16_t stableSeat, uint64_t frame);
		NetSeatRoster m_Roster; //!< Whether each seat's holder is away, why and since when; changed only through ApplyRosterEvent.
		uint64_t (*m_UnixClock)(void*) = nullptr;
		void* m_UnixClockContext = nullptr;
		uint64_t UnixNowMs() const;
		std::vector<Applicant> m_Applicants;
		/// An application a lost host left pending: the same application when its applicant asks this host again with its transaction.
		struct CarriedApplicant {
			uint16_t stableSeat = 0;
			NetAuthBytes16 txId{};
			std::string displayName;
			uint64_t appliedAtMs = 0;
		};
		std::vector<CarriedApplicant> m_CarriedApplicants;
		uint64_t m_CarriedApplicantsUntilMs = 0; //!< When the ones nobody asked again for are dropped, as an unanswered applicant expires.
		std::vector<Substitution> m_Substitutions;
		std::vector<Provisional> m_Provisionals;
		std::vector<PendingReclaim> m_PendingReclaims;
		std::vector<Fence> m_Fences;
		std::vector<NetH4Outbound> m_Outbound;
		std::vector<NetGameReseat> m_PendingReseats;
		std::vector<NetHoldResolutionNotice> m_PendingHoldResolutions;
		std::vector<NetH4Commit> m_Commits;
		NetReconnectHostStats m_Stats;
		bool m_MigrationHold = false;
		std::vector<std::pair<NetPeerId, NetPayload>> m_MigrationHeldMessages;
		NetAuthBytes16 m_LastRemovalTx{};
		bool m_HasRemovalTx = false;
		NetPeerId m_LastRemovedConnection = c_InvalidNetPeerId;
		NetHostBanStore* m_BanStore = nullptr;
		std::set<NetAuthBytes32> m_RemovedParticipants;
		uint64_t m_LockstepFrame = 0;
		bool m_ProofRequired = false;
		std::vector<std::pair<NetPeerId, NetAuthBytes32>> m_ConnectionIds;
	};

	enum class NetH4ClientState : uint8_t {
		Idle = 0,
		Joining = 1,     //!< NewJoin sent, waiting for the ticket offer.
		Storing = 2,     //!< Offer persisted, waiting for the commit.
		Reclaiming = 3,  //!< Reclaim sent, waiting for the challenge.
		Proving = 4,     //!< Proof sent, waiting for the commit.
		Joined = 5,
		Leaving = 6,
		Left = 7,        //!< Acknowledged leave; the record is gone.
		Denied = 8,      //!< The host refused; the record stays for the next attempt.
		Failed = 9,      //!< Local failure (no provider, no durable store); nothing was committed.
		Applying = 10,   //!< Applicant sent, waiting for the host to acknowledge the request.
		Applied = 11,    //!< On the host's list, waiting for a human decision.
		Substituting = 12, //!< Approved: the ticket is persisted and the ack proves it.
	};

	const char* NetReconnectClientStateName(NetH4ClientState state);
	/// The seat roster's match: the hosted session's admission epoch, which every admitted peer holds in its ticket; 0 for none.
	uint64_t NetRosterMatchIdOf(const NetAuthBytes16& epoch);
	/// The seat roster's owner for a player who proved its participant identity; 0 for none.
	uint64_t NetRosterOwnerIdOf(const NetAuthBytes32& participantId);
	/// The seat roster's ticket id: the ticket a holder returns with, named by its session's epoch, its seat and its holder generation.
	uint64_t NetRosterTicketIdOf(const NetAuthBytes16& epoch, uint16_t stableSeat, uint32_t holderGeneration);

	struct NetReconnectClientStats {
		uint32_t requestsSent = 0;
		uint32_t retransmits = 0;
		uint32_t ticketsStored = 0;
		uint32_t ticketStoreFailures = 0;
		uint32_t ticketsCleared = 0;
		uint32_t proofsSent = 0;
		uint32_t commitsReceived = 0;
		uint32_t leaveAcksReceived = 0;
		uint32_t applicationsSent = 0;
		uint32_t applicationsAcknowledged = 0;
		uint32_t substitutionOffersReceived = 0;
		uint32_t substitutionAcksSent = 0;
		uint32_t unacknowledgedLeaves = 0;
		uint32_t ambiguousLosses = 0;
		uint32_t confirmedSessionEnds = 0;
		uint32_t returnRetries = 0; //!< Returns the host said to ask again later, asked again with the same ticket.
		uint32_t returnEndpointsStored = 0;       //!< Proven returns whose new host address reached the stored ticket.
		uint32_t returnEndpointStoreFailures = 0; //!< Writes of such an address the store refused; each is retried.
	};

	/// The client half: it persists the ticket the host offers before acknowledging it, answers a
	/// challenge from the stored credential, and clears the record on exactly two events - an
	/// acknowledged leave and a confirmed hosted-session end.
	class NetReconnectClient {
	public:
		// P21: an unacknowledged leave is an ambiguous loss, so the ladder gives up inside the same
		// budget every other handshake step uses and the record survives.
		static constexpr uint64_t c_LeaveAckBudgetMs = 2000;
		/// A proven host address the store refused is written again after this, doubling to the longest.
		static constexpr uint64_t c_EndpointRetryMs = 1000;
		static constexpr uint64_t c_EndpointRetryLongestMs = 32000;

		void Configure(NetReconnectTicketStore* store, NetH4Identity identity, std::string displayName);
		void SetUnixClock(uint64_t (*clock)(void*), void* context);
		void SetRound(uint32_t round) { m_Round = round; }
		uint32_t GetRound() const { return m_Round; }
		/// Names the host this client is joining, so a stored record can be told from another host's and
		/// the record it writes says where it came from.
		void SetHostContext(std::string hostAddress, const NetHash32& matchConfigHash);
		bool MigrateHostContext(const std::string& address, const std::string& directorySessionId, const NetHash32& matchConfigHash);
		bool OpenSuccessorCapsule(const std::vector<uint8_t>& context, const std::vector<uint8_t>& sealed, std::vector<uint8_t>& plaintext) const;
		/// The host is a persistent world, so the record says so and a relaunch's rejoin hellos on the
		/// world plane instead of the ordinary one.
		void SetWorldTarget(bool world) { m_WorldTarget = world; }
		void SetDirectorySessionId(std::string directorySessionId);
		/// Points the record at the directory row it really belongs to, rewriting one already on disk.
		/// A persistent world registers under its own UUID, which the client only learns from the
		/// configuration it adopts, and that id is what its return watch browses for.
		void AdoptDirectorySessionId(const std::string& directorySessionId);

		/// The hosted session the host's join answer named; a stored record of that session is its own whatever address reached it.
		void NoteAcceptedHostSession(uint64_t hostSessionId) { m_AcceptedHostSessionId = hostSessionId; }
		void SetRequireStoredTicket(bool required) { m_RequireStoredTicket = required; }
		/// Starts the §4 transaction the session was accepted into: a stored record for THIS host is
		/// reclaimed, anything else is a fresh join.
		/// @return Whether a transaction is now running; false leaves the session's ordinary Ready path.
		bool BeginAdmission(uint64_t nowMs, std::string* error = nullptr);
		/// The host refused the transaction. A refused RECLAIM falls back to one fresh join (the ticket
		/// was for a session that is gone), which is exactly what a ticketless client would have sent.
		/// A seat the host says was REASSIGNED is gone for good, so that one is never retried. A return
		/// the host takes later (the key c_ReturnRetryKey) is asked again with the same ticket after a backoff.
		/// @return Whether the refusal was absorbed; false means the session should fail on it.
		bool AbsorbRejection(uint64_t nowMs, NetRejectReason reason = NetRejectReason::HostNotAccepting, const std::string& key = {});

		bool BeginNewJoin(uint64_t nowMs, std::string* error = nullptr);
		bool BeginReclaim(const NetH4TicketRecord& record, uint64_t nowMs, std::string* error = nullptr);
		bool BeginLeave(uint64_t nowMs, std::string* error = nullptr);
		/// Phase B: ask the host for a seat instead of joining one. A live match refuses an ordinary
		/// join, so this is the only way in for a player the host has to approve by hand.
		bool BeginApplication(uint16_t stableSeat, uint64_t nowMs, std::string* error = nullptr);
		/// Whether this player waits on an application its host acknowledged and nobody has answered.
		bool HasUnansweredApplication() const { return m_State == NetH4ClientState::Applied; }
		/// Keeps that application for the next host: the next application for its seat asks again with its transaction.
		void CarryApplicationToNextHost() {
			if (HasUnansweredApplication()) m_CarriedApplication = std::make_pair(m_ApplySeat, m_TxId);
		}
		/// Makes BeginAdmission apply for a seat rather than join or reclaim. The UI (B2) and the gate
		/// drivers set this; nothing on the wire does.
		void SetApplyForSeat(bool enabled, uint16_t stableSeat);

		/// @return Whether the payload was an H4 message this plane handled.
		bool HandleMessage(const NetPayload& payload, uint64_t nowMs);
		void Tick(uint64_t nowMs);

		/// The host said the hosted session ended (P22) - the only event other than a LeaveAck that may
		/// delete the record.
		void NotifyConfirmedSessionEnd();
		/// Host-authored removal of this client: the ticket dies and retry stops.
		void NotifyParticipantRemoved(NetRejectReason reason);
		bool WasRemoved() const { return m_Removed; }
		NetAuthBytes16 LastRemovalTx() const { return m_LastRemovalTx; }
		/// The frame the host's removal of this seat took effect at.
		uint64_t GetRemovalBoundary() const { return m_RemovalBoundary; }
		/// The link died without an answer. The record is exactly what this case exists for: it stays.
		void NotifyAmbiguousLoss();

		NetH4ClientState GetState() const { return m_State; }
		bool IsAdmitted() const { return m_State == NetH4ClientState::Joined; }
		/// A transaction is in flight: the session owes it a commit before it may declare itself Ready.
		bool IsAdmissionPending() const;
		/// Why the last store read produced nothing, so §11 can tell missing from corrupt from stale.
		NetH4TicketLoadResult GetLastLoadResult() const { return m_LastLoad; }
		bool UsedStoredTicket() const { return m_UsedStoredTicket && !m_FellBackToNewJoin; }
		const char* ReclaimOutcome() const {
			if (m_FellBackToNewJoin) {
				return "new_join_after_refusal";
			}
			if (m_UsedStoredTicket) {
				return "reclaim_accepted";
			}
			return "";
		}
		/// Set when an unacknowledged leave gave up: keep the ticket and close the link.
		bool WantsLinkClosed() const { return m_WantsLinkClosed; }
		/// The reason the host last refused this client, so §11 can say "the seat was reassigned"
		/// rather than "denied" when the host actually said so.
		NetRejectReason GetLastRejectReason() const { return m_LastRejectReason; }
		bool HasLastRejectReason() const { return m_HasLastRejectReason; }
		const std::string& GetError() const { return m_Error; }
		const NetH4TicketRecord& GetRecord() const { return m_Record; }
		bool HasRecord() const { return m_HasRecord; }
		uint32_t GetIncarnation() const { return m_Incarnation; }
		uint8_t GetAssignedPeerId() const { return m_AssignedPeerId; }

		std::vector<NetH4Outbound> TakeOutbound();
		const NetReconnectClientStats& GetStats() const { return m_Stats; }
		/// The host's seat roster as this peer last heard it, revision by revision.
		const NetRosterReplica& GetRosterReplica() const { return m_RosterReplica; }
		/// Asks the host for the seat roster revision a config names that this peer never heard.
		void RequestRosterRevision(uint32_t revision);

	private:
		void SendRequest(NetPayload payload, uint64_t nowMs);
		void Resend(uint64_t nowMs);
		void Fail(std::string error);
		uint64_t UnixNowMs() const;
		/// Writes where the host was reached into the stored ticket of the seat it just committed, and nothing else of it.
		void StoreReturnEndpoint(uint64_t nowMs);

		NetReconnectTicketStore* m_Store = nullptr;
		NetRosterReplica m_RosterReplica;
		NetH4Identity m_Identity;
		std::string m_DisplayName = "Player";
		std::string m_HostAddress;
		std::string m_DirectorySessionId;
		uint64_t m_AcceptedHostSessionId = 0; //!< The hosted session the host's join answer named.
		NetHash32 m_MatchConfigHash{};
		bool m_WorldTarget = false;
		uint64_t (*m_UnixClock)(void*) = nullptr;
		void* m_UnixClockContext = nullptr;

		NetH4ClientState m_State = NetH4ClientState::Idle;
		NetH4TicketLoadResult m_LastLoad = NetH4TicketLoadResult::Missing;
		bool m_UsedStoredTicket = false;
		bool m_RequireStoredTicket = false;
		uint64_t m_A7PreviousLeaveElapsedMs = 0;
		bool m_FellBackToNewJoin = false;
		bool m_ApplyForSeat = false;
		uint16_t m_ApplySeat = 0;
		uint64_t m_AppliedAtMs = 0;
		NetRejectReason m_LastRejectReason = NetRejectReason::HostNotAccepting;
		bool m_HasLastRejectReason = false;
		NetPayload m_PendingRequest;
		bool m_HasPendingRequest = false;
		uint64_t m_RequestSentMs = 0;
		uint64_t m_RequestOpenedMs = 0;
		uint32_t m_Retransmits = 0;
		uint64_t m_ReturnRetryAtMs = 0;   //!< When a return the host put off is asked again; 0 when none waits.
		uint64_t m_ReturnRetryDelayMs = 0; //!< The last wait, doubled each time up to the roster's longest backoff.
		uint64_t m_EndpointRetryAtMs = 0;    //!< When a proven address the store refused is written again; 0 when none waits.
		uint64_t m_EndpointRetryDelayMs = 0; //!< The last wait before such a write, doubled each time.
		NetAuthBytes16 m_TxId{};
		std::optional<std::pair<uint16_t, NetAuthBytes16>> m_CarriedApplication; //!< An application a lost host left unanswered, for the next host.
		NetH4TicketRecord m_Record;
		bool m_HasRecord = false;
		uint32_t m_Incarnation = 0;
		uint32_t m_Round = 0;
		uint8_t m_AssignedPeerId = 0;
		bool m_WantsLinkClosed = false;
		bool m_Removed = false;
		NetAuthBytes16 m_LastRemovalTx{};
		uint64_t m_RemovalBoundary = 0;
		std::string m_Error;
		std::vector<NetH4Outbound> m_Outbound;
		NetReconnectClientStats m_Stats;
	};

} // namespace RTE
