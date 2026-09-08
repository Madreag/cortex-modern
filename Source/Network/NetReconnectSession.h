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
	};

	/// A seat's live admission status, for §11's persistent roster indication and the reports.
	struct NetH4SeatStatus {
		uint16_t stableSeat = 0;
		uint8_t lockstepPeerId = 0;
		bool committed = false;
		bool closed = false;
		bool dropped = false;    //!< Committed, but its holder's transport is gone.
		bool reclaiming = false; //!< A reclaim transaction for it is in flight.

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
		uint32_t ledgerDropsRecorded = 0;
		uint32_t reseatsIssued = 0;
		uint32_t reclaimRetransmitsDropped = 0;
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

		void Configure(NetSeatAuthRegistry* registry, uint64_t hostSessionId, NetH4Identity localIdentity);
		void SetSeatTable(std::vector<NetH4Seat> seats, NetMatchMode mode);
		/// Live match: a ticketless join is denied outright in Phase A; in a lobby it may fill a
		/// never-held seat.
		void SetLiveMatch(bool live) { m_LiveMatch = live; }
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
		void IssueReseat(const SeatState& seat);
		const NetPayload* FindCached(const NetAuthBytes16& txId, const NetH4TxKey& key, uint64_t nowMs);

		NetSeatAuthRegistry* m_Registry = nullptr;
		uint64_t m_HostSessionId = 0;
		NetH4Identity m_LocalIdentity;
		NetHash32 m_MatchConfigHash{};
		std::string m_HostAddress;
		NetMatchMode m_Mode = NetMatchMode::PvPSkirmish;
		bool m_LiveMatch = false;
		std::vector<NetH4LedgerActor> (*m_DropOwnershipSource)(void*) = nullptr;
		void* m_DropOwnershipContext = nullptr;

		NetReconnectAdmission m_Admission;
		NetReconnectTxCache m_TxCache;
		NetReconnectLedger m_Ledger;
		std::vector<SeatState> m_Seats;
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
		/// @return Whether the refusal was absorbed; false means the session should fail on it.
		bool AbsorbRejection(uint64_t nowMs);

		bool BeginNewJoin(uint64_t nowMs, std::string* error = nullptr);
		bool BeginReclaim(const NetH4TicketRecord& record, uint64_t nowMs, std::string* error = nullptr);
		bool BeginLeave(uint64_t nowMs, std::string* error = nullptr);

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
