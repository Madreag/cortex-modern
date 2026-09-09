#pragma once

#include "NetGameCommand.h"
#include "NetMatchConfig.h"
#include "NetProtocol.h"
#include "NetReconnectAdmission.h"
#include "NetReconnectLedger.h"
#include "NetReconnectTicketStore.h"
#include "NetReconnectTxCache.h"
#include "NetTransport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RTE {

	class NetSeatAuthRegistry;

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
		/// @return Milliseconds since Start; zero before it, and never backwards if the clock hiccups.
		uint64_t NowMs(uint64_t steadyNowMs) const { return m_Started && steadyNowMs > m_OriginMs ? steadyNowMs - m_OriginMs : 0; }

	private:
		uint64_t m_OriginMs = 0;
		bool m_Started = false;
	};

	/// What a disconnecting transport meant to the seats.
	enum class NetH4DisconnectOutcome : uint8_t {
		Unknown = 0,    //!< No seat ever bound this transport.
		Fenced = 1,     //!< A superseded incarnation timing out; the seat keeps its current holder.
		SeatDropped = 2 //!< The seat's active incarnation is gone; the seat is now reclaimable.
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

		bool operator==(const NetH4ApplicantView&) const = default;
	};

	/// One seat as the host's moderation view sees it (§9b): whether it is waiting for someone, and
	/// who is asking for it.
	struct NetH4ModerationSeat {
		uint16_t stableSeat = 0;
		uint8_t lockstepPeerId = 0;
		int32_t team = 0;
		bool committed = false;
		bool dropped = false;
		bool closed = false;
		bool heldForReclaim = false;  //!< The original holder can still return.
		bool substitutable = false;   //!< A host action may reassign it right now.
		bool substituting = false;    //!< An approval is in flight for it.
		uint32_t holderGeneration = 0;
		uint32_t seatGeneration = 0;  //!< The value a pending approval compares against at commit.
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
	};

	const char* NetH4ModerationResultName(NetH4ModerationResult result);

	/// A seat's live admission status, for §11's persistent roster indication and the reports.
	struct NetH4SeatStatus {
		uint16_t stableSeat = 0;
		uint8_t lockstepPeerId = 0;
		bool committed = false;
		bool closed = false;
		bool dropped = false;      //!< Committed, but its holder's transport is gone.
		bool reclaiming = false;   //!< A reclaim transaction for it is in flight.
		bool substituting = false; //!< An approved substitute is persisting its ticket.
		uint16_t applicants = 0;   //!< Players asking the host for this seat.

		bool operator==(const NetH4SeatStatus&) const = default;
	};

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
		uint32_t seatsReleased = 0; //!< Seats handed back to the pool, whichever way their holder went.
		uint32_t ledgerDropsRecorded = 0;
		uint32_t reseatsIssued = 0;
		uint32_t reseatsWithoutALedger = 0;   //!< Reclaims whose seat ledgered nothing at the drop: a returner reseated onto nothing.
		uint32_t reseatsWithoutSurvivors = 0; //!< Reclaims whose ledgered units are all gone from the world; nothing to hand back.
		uint32_t reseatLiveOnTeamNotNamed = 0; //!< The most a reclaim found alive on the returner's team that its drop record does not name. Recorded, never judged.
		uint32_t reclaimRetransmitsDropped = 0;
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
		void SetSeatTable(std::vector<NetH4Seat> seats, NetMatchMode mode);
		/// Live match: a ticketless join is denied outright in Phase A; in a lobby it may fill a
		/// never-held seat.
		void SetLiveMatch(bool live) { m_LiveMatch = live; }
		bool IsLiveMatch() const { return m_LiveMatch; }
		void SetHostAddress(std::string address) { m_HostAddress = std::move(address); }
		void SetMatchConfigHash(const NetHash32& hash) { m_MatchConfigHash = hash; }
		/// The actors the ledger records when a seat drops. Supplied by the match runner at the drop
		/// frame; without one, a drop records an empty ownership list and no reseat is issued.
		void SetDropOwnershipSource(std::vector<NetH4LedgerActor> (*source)(void*), void* context);

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
		/// The reseats a committed reclaim earned, for the match runner to enqueue as lockstep commands.
		std::vector<NetGameReseat> TakePendingReseats();

		const NetReconnectHostStats& GetStats() const { return m_Stats; }
		const NetReconnectAdmission& GetAdmission() const { return m_Admission; }
		const NetReconnectLedger& GetLedger() const { return m_Ledger; }
		const NetReconnectTxCache& GetTxCache() const { return m_TxCache; }
		size_t GetProvisionalSeatCount() const { return m_Provisionals.size(); }

		/// §9b's moderation API: every seat, whether it may be reassigned, and who is asking for it.
		/// The host UI renders this and calls one of the three verbs below; nothing here is a secret.
		std::vector<NetH4ModerationSeat> GetModerationView() const;
		/// Keep waiting for the original holder. Explicit, so "wait" is a recorded decision rather
		/// than the absence of one.
		NetH4ModerationResult WaitForSeat(uint16_t stableSeat);
		/// Approves one applicant for one seat, atomically: the seat is never released first, and the
		/// approval hands out a provisional ticket without touching the seat's current holder.
		NetH4ModerationResult SubstituteApplicant(uint16_t stableSeat, NetPeerId applicantConnection, uint64_t nowMs);
		/// Withdraws an approval that has not committed. The provisional record is invalidated and
		/// removed; the seat was never given away, so there is nothing to take back.
		NetH4ModerationResult CancelSubstitution(uint16_t stableSeat, uint64_t nowMs);
		bool HasSubstitution(uint16_t stableSeat) const;
		size_t GetApplicantCount() const { return m_Applicants.size(); }

		/// Which peer id, if any, currently holds the seat on which transport.
		bool GetSeatHolder(uint16_t stableSeat, NetPeerId& connection, uint32_t& holderGeneration, uint32_t& incarnation) const;
		bool IsSeatClosed(uint16_t stableSeat) const;
		/// Every seat's admission status, in stable-seat order.
		std::vector<NetH4SeatStatus> GetSeatStatuses() const;

	private:
		struct SeatState {
			NetH4Seat seat;
			// The identity the holder was admitted under, so a re-presented ack can rebuild the exact
			// transaction key the commit was cached against.
			NetH4Identity identity;
			uint32_t holderGeneration = 0;
			uint32_t incarnation = 0;
			NetPeerId activeConnection = c_InvalidNetPeerId;
			bool committed = false;
			bool closed = false;
			bool saturated = false;
			bool dropped = false;
			uint64_t droppedAtMs = 0;
			bool holdExpired = false;
			// The compare-and-swap value a pending substitution captures at approval. Anything that
			// changes who may hold the seat moves it, so an approval that was overtaken cannot commit.
			uint32_t seatGeneration = 1;
			uint32_t retiredGeneration = 0; //!< A generation a substitute superseded, kept only to answer it.
			uint64_t retiredUntilMs = 0;
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
			NetPeerId connection = c_InvalidNetPeerId;
			NetAuthBytes16 txId{};
			uint16_t stableSeat = 0;
			uint32_t holderGeneration = 0;
			NetH4TxKey key;
			uint64_t openedAtMs = 0;
			// The generation this names was superseded by a substitute. The challenge is real and the
			// answer is a refusal either way; proving it only decides whether the refusal says why.
			bool superseded = false;
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
		SeatState* FindFreeNeverHeldSeat();
		Provisional* FindProvisionalByTxId(const NetAuthBytes16& txId);
		bool BindIncarnation(SeatState& seat, NetPeerId connection);
		void ReleaseProvisional(uint16_t stableSeat);
		void RecordDrop(SeatState& seat, uint64_t frame);
		/// Hands a seat back to the pool. Only in a lobby: nothing has been played, so the player who
		/// left has nothing to reclaim and the seat must be joinable again.
		void ReleaseSeat(SeatState& seat);
		void IssueReseat(const SeatState& seat);
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
		/// Ends a pending substitution: caches the terminal refusal under its transaction id so a
		/// retransmitted ack replays it, tells the substitute, and forgets the credential.
		void AbandonSubstitution(size_t index, NetH4DenialReason reason, const std::string& summary, uint64_t nowMs);
		/// Every applicant for the seat except the one that just took it hears that it is gone.
		void DisplaceApplicants(uint16_t stableSeat, NetPeerId keepConnection, uint64_t nowMs);
		void DropApplicantsFor(NetPeerId connection);

		NetSeatAuthRegistry* m_Registry = nullptr;
		uint64_t m_HostSessionId = 0;
		NetH4Identity m_LocalIdentity;
		NetHash32 m_MatchConfigHash{};
		std::string m_HostAddress;
		NetMatchMode m_Mode = NetMatchMode::PvPSkirmish;
		uint64_t m_NowMs = 0; //!< The plane's own clock, so a drop can be stamped without one being passed in.
		bool m_LiveMatch = false;
		std::vector<NetH4LedgerActor> (*m_DropOwnershipSource)(void*) = nullptr;
		void* m_DropOwnershipContext = nullptr;

		NetReconnectAdmission m_Admission;
		NetReconnectTxCache m_TxCache;
		NetReconnectLedger m_Ledger;
		std::vector<SeatState> m_Seats;
		std::vector<Applicant> m_Applicants;
		std::vector<Substitution> m_Substitutions;
		std::vector<Provisional> m_Provisionals;
		std::vector<PendingReclaim> m_PendingReclaims;
		std::vector<Fence> m_Fences;
		std::vector<NetH4Outbound> m_Outbound;
		std::vector<NetGameReseat> m_PendingReseats;
		std::vector<NetH4Commit> m_Commits;
		NetReconnectHostStats m_Stats;
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
	};

	/// The client half: it persists the ticket the host offers before acknowledging it, answers a
	/// challenge from the stored credential, and clears the record on exactly two events - an
	/// acknowledged leave and a confirmed hosted-session end.
	class NetReconnectClient {
	public:
		// P21: an unacknowledged leave is an ambiguous loss, so the ladder gives up inside the same
		// budget every other handshake step uses and the record survives.
		static constexpr uint64_t c_LeaveAckBudgetMs = 2000;

		void Configure(NetReconnectTicketStore* store, NetH4Identity identity, std::string displayName);
		void SetUnixClock(uint64_t (*clock)(void*), void* context);
		/// Names the host this client is joining, so a stored record can be told from another host's and
		/// the record it writes says where it came from.
		void SetHostContext(std::string hostAddress, const NetHash32& matchConfigHash);

		/// Starts the §4 transaction the session was accepted into: a stored record for THIS host is
		/// reclaimed, anything else is a fresh join.
		/// @return Whether a transaction is now running; false leaves the session's ordinary Ready path.
		bool BeginAdmission(uint64_t nowMs, std::string* error = nullptr);
		/// The host refused the transaction. A refused RECLAIM falls back to one fresh join (the ticket
		/// was for a session that is gone), which is exactly what a ticketless client would have sent.
		/// A seat the host says was REASSIGNED is gone for good, so that one is never retried.
		/// @return Whether the refusal was absorbed; false means the session should fail on it.
		bool AbsorbRejection(uint64_t nowMs, NetRejectReason reason = NetRejectReason::HostNotAccepting);

		bool BeginNewJoin(uint64_t nowMs, std::string* error = nullptr);
		bool BeginReclaim(const NetH4TicketRecord& record, uint64_t nowMs, std::string* error = nullptr);
		bool BeginLeave(uint64_t nowMs, std::string* error = nullptr);
		/// Phase B: ask the host for a seat instead of joining one. A live match refuses an ordinary
		/// join, so this is the only way in for a player the host has to approve by hand.
		bool BeginApplication(uint16_t stableSeat, uint64_t nowMs, std::string* error = nullptr);
		/// Makes BeginAdmission apply for a seat rather than join or reclaim. The UI (B2) and the gate
		/// drivers set this; nothing on the wire does.
		void SetApplyForSeat(bool enabled, uint16_t stableSeat);

		/// @return Whether the payload was an H4 message this plane handled.
		bool HandleMessage(const NetPayload& payload, uint64_t nowMs);
		void Tick(uint64_t nowMs);

		/// The host said the hosted session ended (P22) - the only event other than a LeaveAck that may
		/// delete the record.
		void NotifyConfirmedSessionEnd();
		/// The link died without an answer. The record is exactly what this case exists for: it stays.
		void NotifyAmbiguousLoss();

		NetH4ClientState GetState() const { return m_State; }
		bool IsAdmitted() const { return m_State == NetH4ClientState::Joined; }
		/// A transaction is in flight: the session owes it a commit before it may declare itself Ready.
		bool IsAdmissionPending() const;
		/// Why the last store read produced nothing, so §11 can tell missing from corrupt from stale.
		NetH4TicketLoadResult GetLastLoadResult() const { return m_LastLoad; }
		bool UsedStoredTicket() const { return m_UsedStoredTicket; }
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

	private:
		void SendRequest(NetPayload payload, uint64_t nowMs);
		void Resend(uint64_t nowMs);
		void Fail(std::string error);
		uint64_t UnixNowMs() const;

		NetReconnectTicketStore* m_Store = nullptr;
		NetH4Identity m_Identity;
		std::string m_DisplayName = "Player";
		std::string m_HostAddress;
		NetHash32 m_MatchConfigHash{};
		uint64_t (*m_UnixClock)(void*) = nullptr;
		void* m_UnixClockContext = nullptr;

		NetH4ClientState m_State = NetH4ClientState::Idle;
		NetH4TicketLoadResult m_LastLoad = NetH4TicketLoadResult::Missing;
		bool m_UsedStoredTicket = false;
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
		NetAuthBytes16 m_TxId{};
		NetH4TicketRecord m_Record;
		bool m_HasRecord = false;
		uint32_t m_Incarnation = 0;
		uint8_t m_AssignedPeerId = 0;
		bool m_WantsLinkClosed = false;
		std::string m_Error;
		std::vector<NetH4Outbound> m_Outbound;
		NetReconnectClientStats m_Stats;
	};

} // namespace RTE
