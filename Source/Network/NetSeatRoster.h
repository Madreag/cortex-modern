#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace RTE {

	/// The one phase a seat is in. Nothing else may say what a seat is doing.
	enum class NetSeatPhase : uint8_t {
		Lobby,         ///< Its owner is in the lobby before the first round.
		Starting,      ///< Its owner is at the round's start gate.
		Running,       ///< Its owner plays the round.
		Held,          ///< The AI plays it (or a lobby keeps it) while its owner is away.
		RejoinImage,   ///< Its owner returns to the running round and waits for the image.
		RejoinCatchUp, ///< Its owner returns to the running round and replays the tail.
		RoundEnd,      ///< The round ended while its owner was away; its end record is owed.
		RematchLobby,  ///< Its owner is in the lobby that follows a round.
		Relaunching,   ///< Its owner's process restarted and has not been admitted again.
		Migrating,     ///< Its owner plays on while the round changes host.
		Count
	};

	/// Why a seat is held, so the label a player reads names the cause.
	enum class NetSeatHoldCause : uint8_t { None, Capacity, LateStream, TimingAck, Quiet, OwnSeat, Leave, LinkDrop, Crash, RejoinFailed, Released };

	/// The transport's own word on the owner's link; nothing inferred.
	enum class NetSeatLink : uint8_t { Connected, Dropped };

	/// Where the match is; a present owner's phase follows it.
	enum class NetRosterStage : uint8_t { Lobby, Starting, Running, Ended, Migrating };

	/// One seat of the match.
	struct NetRosterSeat {
		uint8_t seatId = 0;                                    ///< 1..N for the life of the match: never reused or renumbered.
		uint64_t owner = 0;                                    ///< The owner's stable identity; 0 when the seat is open.
		uint64_t ticket = 0;                                   ///< The ticket its owner returns with; 0 when none.
		uint16_t incarnation = 0;                              ///< One more on every return or relaunch of its owner.
		NetSeatPhase phase = NetSeatPhase::Lobby;
		NetSeatHoldCause holdCause = NetSeatHoldCause::None;
		NetSeatLink link = NetSeatLink::Connected;
		uint8_t failedReturns = 0;                             ///< Returns that failed since its owner was last back.
		uint64_t returnAfterMs = 0;                            ///< A return is not offered again before this (the backoff).
		uint64_t bindingRef = 0;                               ///< The committed world's brain/actor for the seat.
		uint64_t givenAwayTicket = 0;                          ///< The ticket of the player the host gave this seat away from.
		uint64_t heldSinceMs = 0;                              ///< When its owner went away, on the host's clock; the host's own, never sent.
	};

	/// Who holds which seat, in what phase: owned by the host's session plane and replicated by revision.
	struct NetSeatRoster {
		uint32_t revision = 0;
		uint64_t matchId = 0;
		uint32_t roundNo = 0;
		NetRosterStage stage = NetRosterStage::Lobby;
		uint8_t hostSeat = 1;
		uint16_t migrationGen = 0;
		std::vector<NetRosterSeat> seats;
		std::vector<uint64_t> banned;

		const NetRosterSeat* Find(uint8_t seatId) const;
		NetRosterSeat* Find(uint8_t seatId);
		/// The start gate waits on a seat only while its owner is at the gate on a live link.
		bool StartWaitsOn(uint8_t seatId) const;
	};

	/// Every seat event; each goes through ApplyRosterEvent and nowhere else.
	enum class NetRosterEventKind : uint8_t {
		LinkDropped, ProcessRelaunched, Returned, Kicked, Banned, RoundEnded, RematchFormed, HostLinkLost, MemberSetProposed, TransferAborted,
		LivenessPassed, SlowMachine, HostStalled, Admitted, ApplicantAccepted, RoundStarted, ImageLoaded, CaughtUp, HostResumed, HostChanged, Count
	};

	struct NetRosterEvent {
		NetRosterEventKind kind = NetRosterEventKind::Count;
		uint8_t seat = 0;              ///< The seat the event names.
		uint64_t owner = 0;            ///< Admitted / ApplicantAccepted: the player taking the seat.
		uint64_t ticket = 0;           ///< Returned: the ticket shown; Admitted / ApplicantAccepted: the ticket issued.
		uint64_t nowMs = 0;            ///< The plane's clock, for the return's backoff.
		bool withTraffic = false;      ///< LivenessPassed: the link carried authenticated traffic.
		bool byChoice = false;         ///< LinkDropped: the owner left on purpose.
		bool afterGrace = false;       ///< SlowMachine: the round is past its warm-up grace.
		bool keptWorld = false;        ///< Returned: the owner's process kept the round's world.
		bool quorum = false;           ///< HostLinkLost: every surviving member agrees the host's link is gone.
		std::vector<uint8_t> members;  ///< MemberSetProposed: the members the host proposes to start.
	};

	struct NetRosterResult {
		NetSeatRoster roster;
		bool changed = false;          ///< A new revision.
		bool refused = false;          ///< The event was refused; the roster is unchanged.
		std::string reason;            ///< What happened, in the words a player can act on.
	};

	/// How many failed returns a seat takes before its return is offered only at the longest backoff.
	constexpr uint8_t c_RosterReturnAttempts = 3;
	/// The first backoff after a failed return; it doubles with each failure up to the bound.
	constexpr uint64_t c_RosterReturnBackoffMs = 2000;

	/// The one place a seat changes: pure, deterministic, no I/O.
	NetRosterResult ApplyRosterEvent(const NetSeatRoster& roster, const NetRosterEvent& event);
	/// Every invariant from one revision to the next under an event; the reason names the first broken.
	bool CheckRosterInvariants(const NetSeatRoster& before, const NetSeatRoster& after, NetRosterEventKind kind, std::string* reason);
	/// The label the Seats panel shows for a seat.
	std::string RosterSeatLabel(const NetRosterSeat& seat);
	const char* NetSeatPhaseName(NetSeatPhase phase);
	const char* NetRosterEventName(NetRosterEventKind kind);

	/// A roster revision as it goes to the peers: tickets and the ban list stay on the host.
	std::vector<uint8_t> EncodeRoster(const NetSeatRoster& roster);
	bool DecodeRoster(const std::vector<uint8_t>& bytes, NetSeatRoster& roster, std::string* error);
	/// The hash the agreed config carries: what every peer must hold of the roster to start the round.
	std::array<uint8_t, 32> HashRoster(const NetSeatRoster& roster);

	/// A peer's copy of the host's roster: revisions are applied in order and never derived locally.
	class NetRosterReplica {
	public:
		/// Takes a newer revision of this match's roster; says why when it does not.
		bool Apply(const NetSeatRoster& revision, std::string* why);
		/// Whether this copy is the one the host hashed; the reason names the difference for the refusal.
		bool Agrees(const std::array<uint8_t, 32>& hostHash, std::string* why) const;
		const NetSeatRoster& Roster() const { return m_Roster; }
		bool HasRoster() const { return m_HasRoster; }

	private:
		NetSeatRoster m_Roster;
		bool m_HasRoster = false;
	};

	/// The net-roster self-test: drives every reachable cell of REJOIN-GRID.md through ApplyRosterEvent.
	class NetSeatRosterSelfTest {
	public:
		static int Run();
	};

} // namespace RTE
