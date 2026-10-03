#include "NetSeatRoster.h"

#include "NetIdentity.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <sstream>

namespace RTE {

	namespace {
		bool IsInRound(NetRosterStage stage) { return stage == NetRosterStage::Starting || stage == NetRosterStage::Running || stage == NetRosterStage::Migrating; }

		/// Where a present owner stands at each stage.
		NetSeatPhase PresentPhase(NetRosterStage stage) {
			switch (stage) {
				case NetRosterStage::Lobby: return NetSeatPhase::Lobby;
				case NetRosterStage::Starting: return NetSeatPhase::Starting;
				case NetRosterStage::Running: return NetSeatPhase::Running;
				case NetRosterStage::Ended: return NetSeatPhase::RematchLobby;
				default: return NetSeatPhase::Migrating;
			}
		}

		/// The phases in which a seat's owner is away and nobody waits on it.
		bool IsAway(NetSeatPhase phase) { return phase == NetSeatPhase::Held || phase == NetSeatPhase::RoundEnd || phase == NetSeatPhase::Relaunching; }

		uint64_t BackoffMs(uint8_t failed) {
			const uint8_t steps = std::min<uint8_t>(failed, c_RosterReturnAttempts);
			return steps == 0 ? 0 : c_RosterReturnBackoffMs << (steps - 1);
		}

		bool IsBanned(const NetSeatRoster& roster, uint64_t owner) { return owner != 0 && std::find(roster.banned.begin(), roster.banned.end(), owner) != roster.banned.end(); }

		/// A return that failed: the AI keeps the seat and the return is offered again after the backoff.
		void FailReturn(NetRosterSeat& seat, uint64_t nowMs) {
			seat.failedReturns = static_cast<uint8_t>(std::min<int>(seat.failedReturns + 1, 255));
			seat.returnAfterMs = nowMs + BackoffMs(seat.failedReturns);
			seat.phase = NetSeatPhase::Held;
			seat.holdCause = NetSeatHoldCause::RejoinFailed;
		}

		/// The phase a returning or newly seated owner enters at the match's stage.
		NetSeatPhase ReturnPhase(NetRosterStage stage, bool keptWorld) {
			if (stage == NetRosterStage::Running) return keptWorld ? NetSeatPhase::RejoinCatchUp : NetSeatPhase::RejoinImage;
			return PresentPhase(stage);
		}
	} // namespace

	const NetRosterSeat* NetSeatRoster::Find(uint8_t seatId) const {
		const auto found = std::find_if(seats.begin(), seats.end(), [seatId](const NetRosterSeat& seat) { return seat.seatId == seatId; });
		return found == seats.end() ? nullptr : &*found;
	}

	NetRosterSeat* NetSeatRoster::Find(uint8_t seatId) {
		const auto found = std::find_if(seats.begin(), seats.end(), [seatId](const NetRosterSeat& seat) { return seat.seatId == seatId; });
		return found == seats.end() ? nullptr : &*found;
	}

	bool NetSeatRoster::StartWaitsOn(uint8_t seatId) const {
		const NetRosterSeat* seat = Find(seatId);
		return seat && seat->owner != 0 && seat->phase == NetSeatPhase::Starting && seat->link == NetSeatLink::Connected;
	}

	NetRosterResult ApplyRosterEvent(const NetSeatRoster& roster, const NetRosterEvent& event) {
		NetRosterResult result;
		result.roster = roster;
		NetSeatRoster& next = result.roster;
		const auto refuse = [&](const std::string& reason) {
			result.roster = roster;
			result.refused = true;
			result.reason = reason;
			return result;
		};
		const auto keep = [&](const std::string& reason) {
			result.roster = roster;
			result.reason = reason;
			return result;
		};
		const auto commit = [&](const std::string& reason) {
			++next.revision;
			result.changed = true;
			result.reason = reason;
			return result;
		};
		NetRosterSeat* seat = event.seat != 0 ? next.Find(event.seat) : nullptr;
		NetRosterSeat* host = next.Find(next.hostSeat);
		switch (event.kind) {
			case NetRosterEventKind::LinkDropped: {
				if (!seat) return refuse("no such seat");
				// A seat's own link says nothing about the host's: host loss is HostLinkLost with every survivor's agreement.
				if (event.seat == next.hostSeat) return refuse("a link drop never replaces the host");
				if (next.stage == NetRosterStage::Lobby) {
					// Before the first start nothing is played and the lobby is still forming: a drop or a leave frees the seat.
					if (seat->owner == 0) return keep("the seat is already open");
					seat->owner = 0;
					seat->ticket = 0;
					seat->link = NetSeatLink::Dropped;
					seat->holdCause = NetSeatHoldCause::None;
					seat->failedReturns = 0;
					seat->returnAfterMs = 0;
					seat->heldSinceMs = 0;
					return commit("the player left the lobby - the seat is open");
				}
				if (seat->link == NetSeatLink::Dropped && (seat->owner == 0 || IsAway(seat->phase))) return keep("the seat is already away");
				const bool heldInPlace = seat->phase == NetSeatPhase::Held && seat->link == NetSeatLink::Connected && seat->owner != 0;
				seat->link = NetSeatLink::Dropped;
				if (seat->owner == 0) return commit("the open seat's link closed");
				if (heldInPlace) {
					seat->holdCause = event.byChoice ? NetSeatHoldCause::Leave : NetSeatHoldCause::LinkDrop;
					return commit(event.byChoice ? "Left - the AI plays the seat until the player returns" : "Connection lost - the AI plays the seat until the player returns");
				}
				seat->heldSinceMs = event.nowMs;
				if (seat->phase == NetSeatPhase::RejoinImage || seat->phase == NetSeatPhase::RejoinCatchUp) {
					FailReturn(*seat, event.nowMs);
					return commit("Connection lost - the AI plays the seat until the player returns");
				}
				if (!IsAway(seat->phase)) {
					seat->phase = NetSeatPhase::Held;
					seat->holdCause = event.byChoice ? NetSeatHoldCause::Leave : NetSeatHoldCause::LinkDrop;
				}
				return commit(event.byChoice ? "Left - the AI plays the seat until the player returns" : "Connection lost - the AI plays the seat until the player returns");
			}
			case NetRosterEventKind::ProcessRelaunched: {
				if (!seat || seat->owner == 0) return refuse("the seat has no player");
				if (seat->phase == NetSeatPhase::Relaunching) return keep("the player's game is already restarting");
				seat->phase = NetSeatPhase::Relaunching;
				seat->link = NetSeatLink::Dropped;
				seat->holdCause = NetSeatHoldCause::Crash;
				seat->failedReturns = 0;
				seat->returnAfterMs = 0;
				return commit("The player's game restarted - the AI plays the seat until it returns");
			}
			case NetRosterEventKind::Returned: {
				if (!seat || seat->owner == 0) return refuse("the seat has no player to return");
				if (IsBanned(next, seat->owner)) return refuse("the player is banned");
				if (event.ticket != 0 && event.ticket == seat->givenAwayTicket) return refuse("The host gave your seat to another player");
				if (event.ticket == 0 || event.ticket != seat->ticket) return refuse("the ticket is not this seat's");
				if (next.stage == NetRosterStage::Migrating) return refuse("the return waits for the new host");
				if (event.nowMs < seat->returnAfterMs) return refuse("Could not rejoin - retrying");
				++seat->incarnation;
				seat->link = NetSeatLink::Connected;
				seat->holdCause = NetSeatHoldCause::None;
				seat->heldSinceMs = 0;
				// A newer connection of a seat that plays the round takes over the play; any other return goes through its stage's path.
				if (seat->phase != NetSeatPhase::Running || next.stage != NetRosterStage::Running) seat->phase = ReturnPhase(next.stage, event.keptWorld);
				return commit(next.stage == NetRosterStage::Running ? "Rejoining - the AI plays the seat until the player is back" : "the player is back");
			}
			case NetRosterEventKind::Kicked:
			case NetRosterEventKind::Banned:
			case NetRosterEventKind::SeatReleased: {
				if (!seat || seat->owner == 0) return refuse("the seat is already open");
				if (event.seat == next.hostSeat) return refuse("the host cannot remove its own seat");
				if (next.stage == NetRosterStage::Migrating) return refuse("no host can remove a player until the new host hosts");
				// A release opens a seat its player is away from; a player who plays the seat is kicked.
				if (event.kind == NetRosterEventKind::SeatReleased && next.stage != NetRosterStage::Lobby && !IsAway(seat->phase))
					return refuse("the host releases only a held seat");
				if (event.kind == NetRosterEventKind::Banned && !IsBanned(next, seat->owner)) next.banned.push_back(seat->owner);
				seat->owner = 0;
				seat->ticket = 0;
				seat->givenAwayTicket = 0;
				seat->link = NetSeatLink::Dropped;
				seat->failedReturns = 0;
				seat->returnAfterMs = 0;
				seat->heldSinceMs = 0;
				if (next.stage == NetRosterStage::Lobby) {
					// Before the first start nothing is played: the seat is free for a newcomer.
					seat->phase = NetSeatPhase::Lobby;
					seat->holdCause = NetSeatHoldCause::None;
					return commit("The host removed the player - the seat is free");
				}
				// From the first start on the seat keeps its number and the AI plays it, open to an applicant or a newcomer.
				seat->phase = NetSeatPhase::Held;
				seat->holdCause = event.kind == NetRosterEventKind::Banned ? NetSeatHoldCause::Banned :
				                  event.kind == NetRosterEventKind::Kicked ? NetSeatHoldCause::Kicked : NetSeatHoldCause::Released;
				return commit(event.kind == NetRosterEventKind::Banned ? "The host banned the player - the seat is open" :
				              event.kind == NetRosterEventKind::Kicked ? "The host removed the player - the seat is open" : "The host released the seat - it is open");
			}
			case NetRosterEventKind::RoundEnded: {
				if (next.stage != NetRosterStage::Running) return refuse("no round is running");
				next.stage = NetRosterStage::Ended;
				for (NetRosterSeat& each: next.seats) {
					if (each.owner == 0) continue;
					// A present owner, a returning one included, takes the end record and the next lobby; an away one is owed it.
					if (each.phase == NetSeatPhase::Relaunching) continue;
					if (!IsAway(each.phase) && each.link == NetSeatLink::Connected) {
						each.phase = NetSeatPhase::RematchLobby;
						each.holdCause = NetSeatHoldCause::None;
					} else if (each.phase != NetSeatPhase::RoundEnd) {
						each.phase = NetSeatPhase::RoundEnd;
					}
					each.failedReturns = 0;
					each.returnAfterMs = 0;
				}
				return commit("the round ended");
			}
			case NetRosterEventKind::RematchFormed: {
				if (next.stage != NetRosterStage::Lobby && next.stage != NetRosterStage::Ended) return refuse("a round forms only from a lobby");
				if (next.stage == NetRosterStage::Ended) ++next.roundNo;
				next.stage = NetRosterStage::Starting;
				for (NetRosterSeat& each: next.seats) {
					// Every seat keeps its number; one whose owner is away starts held and its return is offered at the start.
					if (each.owner != 0 && !IsAway(each.phase) && each.link == NetSeatLink::Connected) {
						each.phase = NetSeatPhase::Starting;
					} else if (each.phase != NetSeatPhase::Relaunching) {
						each.phase = NetSeatPhase::Held;
						if (each.holdCause == NetSeatHoldCause::None && each.owner != 0) each.holdCause = NetSeatHoldCause::LinkDrop;
					}
					each.failedReturns = 0;
					each.returnAfterMs = 0;
				}
				return commit("the round forms");
			}
			case NetRosterEventKind::RoundStarted: {
				if (next.stage != NetRosterStage::Starting) return refuse("no round is at its start");
				next.stage = NetRosterStage::Running;
				for (NetRosterSeat& each: next.seats)
					if (each.phase == NetSeatPhase::Starting) each.phase = NetSeatPhase::Running;
				return commit("the round started");
			}
			case NetRosterEventKind::MemberSetProposed: {
				if (next.stage != NetRosterStage::Lobby && next.stage != NetRosterStage::Ended) return refuse("the members are agreed only in a lobby");
				bool changed = false;
				for (NetRosterSeat& each: next.seats) {
					if (each.owner == 0 || IsAway(each.phase) || each.seatId == next.hostSeat) continue;
					const bool member = std::find(event.members.begin(), event.members.end(), each.seatId) != event.members.end();
					// A seat left out of the agreement stays its player's and starts held; nobody waits on it.
					if (!member || each.link == NetSeatLink::Dropped) {
						each.phase = NetSeatPhase::Held;
						each.holdCause = each.link == NetSeatLink::Dropped ? NetSeatHoldCause::LinkDrop : NetSeatHoldCause::Leave;
						changed = true;
					}
				}
				return changed ? commit("the seats left out start held") : keep("every seat is a member");
			}
			case NetRosterEventKind::TransferAborted: {
				if (!seat || (seat->phase != NetSeatPhase::RejoinImage && seat->phase != NetSeatPhase::RejoinCatchUp)) return refuse("no return is in transfer to the seat");
				FailReturn(*seat, event.nowMs);
				return commit("Could not rejoin - retrying");
			}
			case NetRosterEventKind::LivenessPassed: {
				if (!seat) return refuse("no such seat");
				// Any authenticated traffic is liveness: a link that carries it is alive.
				if (event.withTraffic) return keep("the link carries traffic");
				NetRosterEvent dropped = event;
				dropped.kind = NetRosterEventKind::LinkDropped;
				return ApplyRosterEvent(roster, dropped);
			}
			case NetRosterEventKind::HeldInPlace: {
				// The round holds a playing seat whose link stays open - its input late, its player quiet - and the AI plays it.
				if (!seat || seat->phase != NetSeatPhase::Running) return keep("only a playing seat is held by the round");
				if (event.seat == next.hostSeat) return refuse("the host's own seat is held for its stall");
				seat->phase = NetSeatPhase::Held;
				seat->holdCause = event.cause == NetSeatHoldCause::None ? NetSeatHoldCause::LateStream : event.cause;
				seat->heldSinceMs = event.nowMs;
				return commit("The connection cannot keep up - the AI plays the seat");
			}
			case NetRosterEventKind::SlowMachine: {
				if (!seat || seat->phase != NetSeatPhase::Running) return keep("only a playing seat is judged for its machine");
				// A round's first seconds are warm-up on every machine; only a machine slow after them is held.
				if (!event.afterGrace) return keep("the round is in its warm-up");
				seat->phase = NetSeatPhase::Held;
				seat->holdCause = NetSeatHoldCause::Capacity;
				return commit("Your machine cannot keep up with this match - the AI plays the seat");
			}
			case NetRosterEventKind::HostStalled: {
				// The host's plane holds its own seat while its simulation stalls; nothing else changes and nobody elects.
				if (!host || host->phase != NetSeatPhase::Running) return keep("the host's seat is not playing");
				host->phase = NetSeatPhase::Held;
				host->holdCause = NetSeatHoldCause::OwnSeat;
				return commit("the host's seat is held while its game catches up");
			}
			case NetRosterEventKind::HostResumed: {
				if (!host || host->phase != NetSeatPhase::Held || host->holdCause != NetSeatHoldCause::OwnSeat) return keep("the host's seat is not held for a stall");
				host->phase = NetSeatPhase::Running;
				host->holdCause = NetSeatHoldCause::None;
				return commit("the host's seat plays again");
			}
			case NetRosterEventKind::HostLinkLost: {
				if (!event.quorum) return refuse("the host is still the host: its loss needs a majority of the connected seats");
				if (next.stage == NetRosterStage::Migrating) return keep("the round is already changing host");
				if (next.stage != NetRosterStage::Running) return refuse("no committed round to carry: the match ends with 'The host left the match'");
				next.stage = NetRosterStage::Migrating;
				for (NetRosterSeat& each: next.seats) {
					if (each.seatId == next.hostSeat) {
						each.phase = NetSeatPhase::Held;
						each.link = NetSeatLink::Dropped;
						each.holdCause = NetSeatHoldCause::LinkDrop;
					} else if (each.phase == NetSeatPhase::Running) {
						each.phase = NetSeatPhase::Migrating;
					} else if (each.phase == NetSeatPhase::RejoinImage || each.phase == NetSeatPhase::RejoinCatchUp) {
						// Its return was the lost host's to serve; it returns to the new host.
						FailReturn(each, event.nowMs);
					}
				}
				return commit("Host lost - arranging handover");
			}
			case NetRosterEventKind::HostChanged: {
				if (next.stage != NetRosterStage::Migrating) return refuse("no handover is in progress");
				if (!seat || seat->owner == 0 || seat->link != NetSeatLink::Connected || seat->phase != NetSeatPhase::Migrating) return refuse("the new host must be a playing member");
				next.hostSeat = event.seat;
				++next.migrationGen;
				next.stage = NetRosterStage::Running;
				for (NetRosterSeat& each: next.seats)
					if (each.phase == NetSeatPhase::Migrating) each.phase = NetSeatPhase::Running;
				return commit("the round goes on under its new host");
			}
			case NetRosterEventKind::Admitted:
			case NetRosterEventKind::ApplicantAccepted: {
				if (!seat) return refuse("no such seat");
				if (IsBanned(next, event.owner) || event.owner == 0) return refuse("the player is banned");
				if (next.stage == NetRosterStage::Migrating) return refuse("no host can seat a player until the new host hosts");
				const bool open = seat->owner == 0;
				if (event.kind == NetRosterEventKind::Admitted && !open) return refuse("the seat is held for its player: a newcomer applies for it");
				if (event.kind == NetRosterEventKind::ApplicantAccepted && (open || !IsAway(seat->phase))) return refuse(open ? "the seat is open: the newcomer joins it" : "the seat's player is playing it");
				if (!open) seat->givenAwayTicket = seat->ticket;
				seat->owner = event.owner;
				seat->ticket = event.ticket;
				++seat->incarnation;
				seat->link = NetSeatLink::Connected;
				seat->holdCause = NetSeatHoldCause::None;
				seat->failedReturns = 0;
				seat->returnAfterMs = 0;
				seat->phase = next.stage == NetRosterStage::Starting ? NetSeatPhase::Held : ReturnPhase(next.stage, false);
				if (seat->phase == NetSeatPhase::Held) seat->holdCause = NetSeatHoldCause::LinkDrop;
				return commit(open ? "the player takes the open seat" : "the host gave the seat to the player who applied");
			}
			case NetRosterEventKind::ImageLoaded: {
				if (!seat || seat->phase != NetSeatPhase::RejoinImage) return refuse("the seat waits for no image");
				seat->phase = NetSeatPhase::RejoinCatchUp;
				return commit("Rejoining - replaying the match");
			}
			case NetRosterEventKind::CaughtUp: {
				if (!seat || seat->phase != NetSeatPhase::RejoinCatchUp || next.stage != NetRosterStage::Running) return refuse("the seat is not catching up");
				seat->phase = NetSeatPhase::Running;
				seat->failedReturns = 0;
				seat->returnAfterMs = 0;
				return commit("the player is back");
			}
			default: break;
		}
		return refuse("no such event");
	}

	bool CheckRosterInvariants(const NetSeatRoster& before, const NetSeatRoster& after, NetRosterEventKind kind, std::string* reason) {
		const auto fail = [reason](const std::string& why) {
			if (reason) *reason = why;
			return false;
		};
		if (before.seats.size() != after.seats.size()) return fail("the seat count changed");
		for (size_t i = 0; i < before.seats.size(); ++i) {
			const NetRosterSeat& was = before.seats[i];
			const NetRosterSeat& is = after.seats[i];
			if (was.seatId != is.seatId) return fail("seat " + std::to_string(was.seatId) + " was renumbered");
			const bool ownerMoves = kind == NetRosterEventKind::Kicked || kind == NetRosterEventKind::Banned || kind == NetRosterEventKind::SeatReleased ||
			                        kind == NetRosterEventKind::Admitted || kind == NetRosterEventKind::ApplicantAccepted ||
			                        ((kind == NetRosterEventKind::LinkDropped || kind == NetRosterEventKind::LivenessPassed) && before.stage == NetRosterStage::Lobby);
			if (was.owner != is.owner && !ownerMoves) return fail("seat " + std::to_string(was.seatId) + " changed owner on a " + NetRosterEventName(kind));
			if (was.owner == is.owner && is.incarnation < was.incarnation) return fail("seat " + std::to_string(was.seatId) + " went back an incarnation");
			if (is.owner != 0 && is.link == NetSeatLink::Dropped && !IsAway(is.phase))
				return fail("seat " + std::to_string(is.seatId) + " has a dropped link in phase " + NetSeatPhaseName(is.phase));
			if (IsBanned(after, is.owner)) return fail("a banned player holds seat " + std::to_string(is.seatId));
		}
		if (!after.Find(after.hostSeat)) return fail("the host seat does not exist");
		if (after.migrationGen < before.migrationGen || (after.migrationGen != before.migrationGen && kind != NetRosterEventKind::HostChanged)) return fail("the migration generation moved outside a handover");
		if (after.revision < before.revision) return fail("the revision went back");
		return true;
	}

	const char* NetSeatPhaseName(NetSeatPhase phase) {
		static constexpr std::array<const char*, static_cast<size_t>(NetSeatPhase::Count)> names{
			"LOBBY", "STARTING", "RUNNING", "HELD", "REJOIN_IMAGE", "REJOIN_CATCHUP", "ROUND_END", "REMATCH_LOBBY", "RELAUNCHING", "MIGRATING"};
		return phase < NetSeatPhase::Count ? names[static_cast<size_t>(phase)] : "?";
	}

	const char* NetRosterEventName(NetRosterEventKind kind) {
		static constexpr std::array<const char*, static_cast<size_t>(NetRosterEventKind::Count)> names{
			"LinkDropped", "ProcessRelaunched", "Returned", "Kicked", "Banned", "RoundEnded", "RematchFormed", "HostLinkLost", "MemberSetProposed", "TransferAborted",
			"LivenessPassed", "SlowMachine", "HostStalled", "Admitted", "ApplicantAccepted", "RoundStarted", "ImageLoaded", "CaughtUp", "HostResumed", "HostChanged",
			"SeatReleased", "HeldInPlace"};
		return kind < NetRosterEventKind::Count ? names[static_cast<size_t>(kind)] : "?";
	}

	std::string RosterSeatLabel(const NetRosterSeat& seat) {
		if (seat.owner == 0) {
			switch (seat.holdCause) {
				case NetSeatHoldCause::Kicked: return "Open - AI in control (kicked)";
				case NetSeatHoldCause::Banned: return "Open - AI in control (banned)";
				case NetSeatHoldCause::Released: return "Open - AI in control (released)";
				default: return "Open";
			}
		}
		switch (seat.phase) {
			case NetSeatPhase::Held:
				switch (seat.holdCause) {
					case NetSeatHoldCause::Capacity: return "Held - AI in control (machine too slow)";
					case NetSeatHoldCause::OwnSeat: return "Held - AI in control (host catching up)";
					case NetSeatHoldCause::RejoinFailed: return "Held - AI in control (rejoin retrying)";
					case NetSeatHoldCause::Crash: return "Held - AI in control (game restarting)";
					case NetSeatHoldCause::Leave: return "Held - AI in control (left)";
					case NetSeatHoldCause::LateStream: return "Held - AI in control (connection too slow)";
					case NetSeatHoldCause::Quiet: return "Held - AI in control (no input)";
					case NetSeatHoldCause::TimingAck: return "Held - AI in control (catching up)";
					default: return "Held - AI in control (connection lost)";
				}
			case NetSeatPhase::RejoinImage:
			case NetSeatPhase::RejoinCatchUp: return "Rejoining";
			case NetSeatPhase::RoundEnd: return "Away - the seat is kept";
			case NetSeatPhase::Relaunching: return "Held - AI in control (game restarting)";
			case NetSeatPhase::Migrating: return "Host lost - arranging handover";
			default: return "Present";
		}
	}

	namespace {
		constexpr uint8_t c_RosterWireVersion = 1;
		constexpr size_t c_RosterMaxSeats = 32;

		void PutBytes(std::vector<uint8_t>& out, uint64_t value, int bytes) {
			for (int i = 0; i < bytes; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
		}

		bool TakeBytes(const std::vector<uint8_t>& in, size_t& at, int bytes, uint64_t& value) {
			if (at + static_cast<size_t>(bytes) > in.size()) return false;
			value = 0;
			for (int i = 0; i < bytes; ++i) value |= static_cast<uint64_t>(in[at + i]) << (8 * i);
			at += static_cast<size_t>(bytes);
			return true;
		}
	} // namespace

	std::vector<uint8_t> EncodeRoster(const NetSeatRoster& roster) {
		std::vector<uint8_t> out;
		PutBytes(out, c_RosterWireVersion, 1);
		PutBytes(out, roster.matchId, 8);
		PutBytes(out, roster.roundNo, 4);
		PutBytes(out, roster.revision, 4);
		PutBytes(out, static_cast<uint8_t>(roster.stage), 1);
		PutBytes(out, roster.hostSeat, 1);
		PutBytes(out, roster.migrationGen, 2);
		PutBytes(out, std::min(roster.seats.size(), c_RosterMaxSeats), 1);
		for (size_t i = 0; i < roster.seats.size() && i < c_RosterMaxSeats; ++i) {
			const NetRosterSeat& seat = roster.seats[i];
			PutBytes(out, seat.seatId, 1);
			PutBytes(out, seat.owner, 8);
			PutBytes(out, seat.incarnation, 2);
			PutBytes(out, static_cast<uint8_t>(seat.phase), 1);
			PutBytes(out, static_cast<uint8_t>(seat.holdCause), 1);
			PutBytes(out, static_cast<uint8_t>(seat.link), 1);
			PutBytes(out, seat.bindingRef, 8);
		}
		return out;
	}

	bool DecodeRoster(const std::vector<uint8_t>& bytes, NetSeatRoster& roster, std::string* error) {
		const auto fail = [error](const std::string& why) {
			if (error) *error = why;
			return false;
		};
		size_t at = 0;
		uint64_t version = 0, matchId = 0, roundNo = 0, revision = 0, stage = 0, hostSeat = 0, migrationGen = 0, count = 0;
		if (!TakeBytes(bytes, at, 1, version) || version != c_RosterWireVersion) return fail("a seat roster of another version");
		if (!TakeBytes(bytes, at, 8, matchId) || !TakeBytes(bytes, at, 4, roundNo) || !TakeBytes(bytes, at, 4, revision) || !TakeBytes(bytes, at, 1, stage) ||
		    !TakeBytes(bytes, at, 1, hostSeat) || !TakeBytes(bytes, at, 2, migrationGen) || !TakeBytes(bytes, at, 1, count)) return fail("a short seat roster");
		if (stage > static_cast<uint64_t>(NetRosterStage::Migrating) || count > c_RosterMaxSeats) return fail("a seat roster out of range");
		NetSeatRoster decoded;
		decoded.matchId = matchId;
		decoded.roundNo = static_cast<uint32_t>(roundNo);
		decoded.revision = static_cast<uint32_t>(revision);
		decoded.stage = static_cast<NetRosterStage>(stage);
		decoded.hostSeat = static_cast<uint8_t>(hostSeat);
		decoded.migrationGen = static_cast<uint16_t>(migrationGen);
		for (uint64_t i = 0; i < count; ++i) {
			uint64_t seatId = 0, owner = 0, incarnation = 0, phase = 0, cause = 0, link = 0, bindingRef = 0;
			if (!TakeBytes(bytes, at, 1, seatId) || !TakeBytes(bytes, at, 8, owner) || !TakeBytes(bytes, at, 2, incarnation) || !TakeBytes(bytes, at, 1, phase) ||
			    !TakeBytes(bytes, at, 1, cause) || !TakeBytes(bytes, at, 1, link) || !TakeBytes(bytes, at, 8, bindingRef)) return fail("a short seat roster");
			if (seatId == 0 || phase >= static_cast<uint64_t>(NetSeatPhase::Count) || cause > static_cast<uint64_t>(NetSeatHoldCause::Banned) ||
			    link > static_cast<uint64_t>(NetSeatLink::Dropped) || decoded.Find(static_cast<uint8_t>(seatId))) return fail("a seat roster with a bad seat");
			NetRosterSeat seat;
			seat.seatId = static_cast<uint8_t>(seatId);
			seat.owner = owner;
			seat.incarnation = static_cast<uint16_t>(incarnation);
			seat.phase = static_cast<NetSeatPhase>(phase);
			seat.holdCause = static_cast<NetSeatHoldCause>(cause);
			seat.link = static_cast<NetSeatLink>(link);
			seat.bindingRef = bindingRef;
			decoded.seats.push_back(seat);
		}
		if (at != bytes.size()) return fail("a seat roster with trailing bytes");
		// A host that plays no seat of its own (a dedicated host) names seat 0.
		if (decoded.hostSeat != 0 && !decoded.Find(decoded.hostSeat)) return fail("a seat roster without its host's seat");
		roster = std::move(decoded);
		return true;
	}

	std::array<uint8_t, 32> HashRoster(const NetSeatRoster& roster) {
		std::vector<std::pair<std::string, std::string>> fields = {
		    {"match", std::to_string(roster.matchId)}, {"round", std::to_string(roster.roundNo)}, {"revision", std::to_string(roster.revision)},
		    {"stage", std::to_string(static_cast<int>(roster.stage))}, {"host", std::to_string(roster.hostSeat)}, {"migration", std::to_string(roster.migrationGen)}};
		for (const NetRosterSeat& seat: roster.seats) {
			fields.emplace_back("seat." + std::to_string(seat.seatId), std::to_string(seat.owner) + ":" + std::to_string(seat.incarnation) + ":" + NetSeatPhaseName(seat.phase) + ":" +
			                                                                   std::to_string(static_cast<int>(seat.holdCause)) + ":" + std::to_string(static_cast<int>(seat.link)) + ":" +
			                                                                   std::to_string(seat.bindingRef));
		}
		return NetIdentity::HashCanonicalText("cortex.seat-roster.v1", fields);
	}

	bool NetRosterReplica::Apply(const NetSeatRoster& revision, std::string* why) {
		if (m_MatchId != 0 && revision.matchId != m_MatchId) {
			if (why) *why = "a seat roster of another match";
			return false;
		}
		m_MatchId = revision.matchId;
		if (m_HasRoster && revision.migrationGen < m_Roster.migrationGen) {
			if (why) *why = "a seat roster of an earlier host";
			return false;
		}
		// A new host numbers on from what it imported, so the old host's later revisions are not its to agree with.
		if (m_HasRoster && revision.migrationGen > m_Roster.migrationGen) {
			m_Recent.clear();
			m_HasRoster = false;
		}
		// Every revision is whole, so a later one heals any it missed and an earlier one is history.
		if (m_HasRoster && revision.revision <= m_Roster.revision) {
			if (why) *why = "an older seat roster revision";
			return false;
		}
		m_Roster = revision;
		m_HasRoster = true;
		m_Recent.emplace_back(revision.revision, HashRoster(revision));
		if (m_Recent.size() > 64) m_Recent.erase(m_Recent.begin());
		return true;
	}

	bool NetRosterReplica::ApplyPast(const NetSeatRoster& revision) {
		if (!m_HasRoster || revision.matchId != m_MatchId || revision.migrationGen != m_Roster.migrationGen || revision.revision >= m_Roster.revision) return false;
		if (std::any_of(m_Recent.begin(), m_Recent.end(), [&](const auto& entry) { return entry.first == revision.revision; })) return true;
		m_Recent.emplace_back(revision.revision, HashRoster(revision));
		if (m_Recent.size() > 64) m_Recent.erase(m_Recent.begin());
		return true;
	}

	void NetRosterReplica::Attach(uint64_t matchId) {
		if (matchId != m_MatchId) Reset(matchId);
	}

	void NetRosterReplica::Reset(uint64_t matchId) {
		m_Roster = {};
		m_HasRoster = false;
		m_Recent.clear();
		m_MatchId = matchId;
	}

	bool NetRosterReplica::AgreesAt(uint32_t revision, const std::array<uint8_t, 32>& hostHash, std::string* why) const {
		const auto heard = std::find_if(m_Recent.begin(), m_Recent.end(), [revision](const auto& entry) { return entry.first == revision; });
		if (heard == m_Recent.end()) {
			if (why) *why = "this peer never heard seat roster revision " + std::to_string(revision) + " (it holds " + (m_HasRoster ? std::to_string(m_Roster.revision) : std::string("none")) + ")";
			return false;
		}
		if (heard->second != hostHash) {
			if (why) *why = "seat roster revision " + std::to_string(revision) + " differs from the host's";
			return false;
		}
		return true;
	}

	bool NetRosterReplica::Agrees(const std::array<uint8_t, 32>& hostHash, std::string* why) const {
		if (m_HasRoster && HashRoster(m_Roster) == hostHash) return true;
		if (why) *why = m_HasRoster ? "the seat roster differs from the host's (revision " + std::to_string(m_Roster.revision) + ")" : "no seat roster from the host yet";
		return false;
	}

	namespace {
		/// The subject seat's phase and its round's stage for each grid row.
		struct RowSetup { NetSeatPhase phase; NetRosterStage stage; NetSeatLink link; };

		RowSetup RowOf(int row) {
			switch (row) {
				case 1: return {NetSeatPhase::Lobby, NetRosterStage::Lobby, NetSeatLink::Connected};
				case 2: return {NetSeatPhase::Starting, NetRosterStage::Starting, NetSeatLink::Connected};
				case 3: return {NetSeatPhase::Running, NetRosterStage::Running, NetSeatLink::Connected};
				case 4: return {NetSeatPhase::Held, NetRosterStage::Running, NetSeatLink::Dropped};
				case 5: return {NetSeatPhase::RejoinImage, NetRosterStage::Running, NetSeatLink::Connected};
				case 6: return {NetSeatPhase::RejoinCatchUp, NetRosterStage::Running, NetSeatLink::Connected};
				case 7: return {NetSeatPhase::RoundEnd, NetRosterStage::Ended, NetSeatLink::Dropped};
				case 8: return {NetSeatPhase::RematchLobby, NetRosterStage::Ended, NetSeatLink::Connected};
				case 9: return {NetSeatPhase::Relaunching, NetRosterStage::Running, NetSeatLink::Dropped};
				default: return {NetSeatPhase::Migrating, NetRosterStage::Migrating, NetSeatLink::Connected};
			}
		}

		/// Three seats: the host (1), the subject (2) in the row's phase, and one other present seat (3).
		NetSeatRoster RosterForRow(int row) {
			const RowSetup setup = RowOf(row);
			NetSeatRoster roster;
			roster.matchId = 0x52535452;
			roster.roundNo = 1;
			roster.stage = setup.stage;
			roster.hostSeat = 1;
			for (uint8_t id = 1; id <= 3; ++id) {
				NetRosterSeat seat;
				seat.seatId = id;
				seat.owner = 0x1000 + id;
				seat.ticket = 0x7000 + id;
				seat.incarnation = 1;
				seat.phase = PresentPhase(setup.stage);
				roster.seats.push_back(seat);
			}
			NetRosterSeat& subject = roster.seats[1];
			subject.phase = setup.phase;
			subject.link = setup.link;
			if (IsAway(setup.phase)) subject.holdCause = setup.phase == NetSeatPhase::Relaunching ? NetSeatHoldCause::Crash : NetSeatHoldCause::LinkDrop;
			// While the round changes host the old host's seat is held; the subject is a survivor.
			if (setup.stage == NetRosterStage::Migrating) {
				roster.seats[0].phase = NetSeatPhase::Held;
				roster.seats[0].link = NetSeatLink::Dropped;
				roster.seats[0].holdCause = NetSeatHoldCause::LinkDrop;
			}
			return roster;
		}

		/// The grid's columns as net-roster drives them.
		struct Column { const char* id; NetRosterEventKind kind; const char* variant; };
		constexpr std::array<Column, 20> c_Columns{{
			{"a", NetRosterEventKind::LinkDropped, ""}, {"b", NetRosterEventKind::ProcessRelaunched, ""}, {"c", NetRosterEventKind::Returned, ""},
			{"d", NetRosterEventKind::Kicked, ""}, {"d2", NetRosterEventKind::Banned, ""}, {"f", NetRosterEventKind::RoundEnded, ""},
			{"g", NetRosterEventKind::RematchFormed, ""}, {"h", NetRosterEventKind::HostStalled, ""}, {"i", NetRosterEventKind::HostLinkLost, "quorum"},
			{"i2", NetRosterEventKind::HostLinkLost, "one member"}, {"j", NetRosterEventKind::MemberSetProposed, "left out"}, {"k", NetRosterEventKind::TransferAborted, ""},
			{"l", NetRosterEventKind::LivenessPassed, "traffic"}, {"l2", NetRosterEventKind::LivenessPassed, "silent"}, {"m", NetRosterEventKind::LinkDropped, "two at once"},
			{"n", NetRosterEventKind::SlowMachine, "after the grace"}, {"o", NetRosterEventKind::Admitted, ""}, {"p", NetRosterEventKind::ApplicantAccepted, ""},
			{"r", NetRosterEventKind::SeatReleased, ""}, {"q", NetRosterEventKind::HeldInPlace, ""}}};

		/// The expected outcome of each cell for the subject seat: its next phase (L S R H I C E M Z G), '-' unchanged, 'X' refused, '.' unreachable.
		/// Columns in c_Columns' order; rows 1-10 are the grid's rows.
		constexpr std::array<const char*, 10> c_Expected{{
			/*  1 LOBBY          */ "LZLLLXS.XXHX-LL-XXL-",
			/*  2 STARTING       */ "HZSHHXX-XXXX-HH-XXX-",
			/*  3 RUNNING        */ "HZRHHMX-GXXX-HHHXXXH",
			/*  4 HELD           */ "-ZIHHEX--XXX----XIH-",
			/*  5 REJOIN_IMAGE   */ "HZIHHMX-HXXH-HH-XXX-",
			/*  6 REJOIN_CATCHUP */ "HZIHHMX-HXXH-HH-XXX-",
			/*  7 ROUND_END      */ "-ZMHHXH.XX-X----XMH-",
			/*  8 REMATCH_LOBBY  */ "HZMHHXS.XXHX-HH-XXX-",
			/*  9 RELAUNCHING    */ "--IHH-X--XXX----XIH-",
			/* 10 MIGRATING      */ "HZX...X.-X.X-HH.XX.."}};

		char PhaseCode(NetSeatPhase phase) {
			static constexpr const char* codes = "LSRHICEMZG";
			return phase < NetSeatPhase::Count ? codes[static_cast<size_t>(phase)] : '?';
		}

		NetRosterEvent EventFor(const Column& column, const NetSeatRoster& roster) {
			NetRosterEvent event;
			event.kind = column.kind;
			event.seat = 2;
			event.nowMs = 10000;
			event.ticket = 0x7002;
			event.quorum = std::string(column.variant) == "quorum";
			event.withTraffic = std::string(column.variant) == "traffic";
			event.afterGrace = true;
			if (column.kind == NetRosterEventKind::MemberSetProposed) event.members = {1, 3};
			if (column.kind == NetRosterEventKind::Admitted || column.kind == NetRosterEventKind::ApplicantAccepted) {
				event.owner = 0x2002;
				event.ticket = 0x8002;
			}
			(void)roster;
			return event;
		}
	} // namespace

	int NetSeatRosterSelfTest::Run() {
		constexpr const char* tag = "[net-roster-selftest]";
		int pass = 0, fail = 0, unreachable = 0;
		const auto check = [&](const std::string& cell, bool ok, const std::string& detail) {
			(ok ? pass : fail) += 1;
			std::cout << tag << " cell " << cell << " result=" << (ok ? "PASS" : "FAIL") << " " << detail << std::endl;
		};
		for (int row = 1; row <= 10; ++row) {
			for (size_t col = 0; col < c_Columns.size(); ++col) {
				const Column& column = c_Columns[col];
				const char expected = c_Expected[row - 1][col];
				const std::string cell = std::to_string(row) + column.id + " " + NetSeatPhaseName(RowOf(row).phase) + " x " + NetRosterEventName(column.kind) +
				                         (column.variant[0] ? std::string(" (") + column.variant + ")" : std::string());
				if (expected == '.') {
					++unreachable;
					std::cout << tag << " cell " << cell << " result=N/A" << std::endl;
					continue;
				}
				const NetSeatRoster before = RosterForRow(row);
				NetRosterResult result = ApplyRosterEvent(before, EventFor(column, before));
				// Two drops at once: the other present seat drops too.
				if (std::string(column.variant) == "two at once" && !result.refused) {
					NetRosterEvent other = EventFor(column, result.roster);
					other.seat = 3;
					const NetRosterResult second = ApplyRosterEvent(result.roster, other);
					if (!second.refused) result.roster = second.roster;
				}
				if (!result.roster.Find(2)) {
					check(cell, false, std::string("expected=") + expected + " got=gone (the seat left the roster)");
					continue;
				}
				const NetRosterSeat& subject = *result.roster.Find(2);
				const NetRosterSeat& was = *before.Find(2);
				std::string why;
				bool ok = CheckRosterInvariants(before, result.roster, column.kind, &why);
				const bool same = subject.phase == was.phase && subject.owner == was.owner && subject.incarnation == was.incarnation && subject.link == was.link &&
				                  subject.holdCause == was.holdCause;
				const char got = result.refused ? 'X' : same ? '-' : PhaseCode(subject.phase);
				ok = ok && got == expected;
				// A return that is accepted moves the owner's incarnation on; a refusal changes nothing.
				if (ok && !result.refused && column.kind == NetRosterEventKind::Returned) ok = subject.incarnation == was.incarnation + 1;
				if (ok && result.refused) ok = result.roster.revision == before.revision;
				// The start gate never waits on a seat its owner is away from.
				if (ok && IsAway(subject.phase)) ok = !result.roster.StartWaitsOn(2);
				check(cell, ok, std::string("expected=") + expected + " got=" + got + (why.empty() ? "" : " invariant='" + why + "'") + " reason='" + result.reason + "'");
			}
		}
		// The sequences the findings walked, each a path through the cells above.
		const auto phaseOf = [](const NetSeatRoster& roster, uint8_t id) { const NetRosterSeat* seat = roster.Find(id); return seat ? seat->phase : NetSeatPhase::Count; };
		{
			// RT4: a return whose round ends during its image carries the seat into the next round, never out of it.
			NetSeatRoster roster = RosterForRow(5);
			NetRosterEvent ended; ended.kind = NetRosterEventKind::RoundEnded;
			roster = ApplyRosterEvent(roster, ended).roster;
			NetRosterEvent formed; formed.kind = NetRosterEventKind::RematchFormed;
			roster = ApplyRosterEvent(roster, formed).roster;
			check("seq RT4 a return carried across the round's end", phaseOf(roster, 2) == NetSeatPhase::Starting && roster.StartWaitsOn(2),
			      std::string("phase=") + NetSeatPhaseName(phaseOf(roster, 2)));
		}
		{
			// RT4 form A: a relaunched owner whose round ends is admitted into the next lobby with its ticket.
			NetSeatRoster roster = RosterForRow(9);
			NetRosterEvent ended; ended.kind = NetRosterEventKind::RoundEnded;
			roster = ApplyRosterEvent(roster, ended).roster;
			NetRosterEvent back; back.kind = NetRosterEventKind::Returned; back.seat = 2; back.ticket = 0x7002;
			const NetRosterResult returned = ApplyRosterEvent(roster, back);
			check("seq RT4 a relaunched owner returns after the round ended", !returned.refused && phaseOf(returned.roster, 2) == NetSeatPhase::RematchLobby,
			      std::string("phase=") + NetSeatPhaseName(phaseOf(returned.roster, 2)) + " reason='" + returned.reason + "'");
		}
		{
			// RT3: two seats drop in the rematch lobby; the round forms with both held, every number kept, nobody waited on.
			NetSeatRoster roster = RosterForRow(8);
			roster.seats.push_back(roster.seats[2]);
			roster.seats.back().seatId = 4;
			roster.seats.back().owner = 0x1004;
			NetRosterEvent drop; drop.kind = NetRosterEventKind::LinkDropped; drop.seat = 2;
			roster = ApplyRosterEvent(roster, drop).roster;
			drop.seat = 4;
			roster = ApplyRosterEvent(roster, drop).roster;
			NetRosterEvent formed; formed.kind = NetRosterEventKind::RematchFormed;
			const NetSeatRoster before = roster;
			roster = ApplyRosterEvent(roster, formed).roster;
			std::string why;
			const bool ok = CheckRosterInvariants(before, roster, NetRosterEventKind::RematchFormed, &why) && roster.seats.size() == 4 && phaseOf(roster, 2) == NetSeatPhase::Held &&
			                phaseOf(roster, 4) == NetSeatPhase::Held && !roster.StartWaitsOn(2) && !roster.StartWaitsOn(4) && roster.StartWaitsOn(3);
			check("seq RT3 two drops in a rematch lobby", ok, "seats=" + std::to_string(roster.seats.size()) + (why.empty() ? "" : " invariant='" + why + "'"));
		}
		{
			// S1: one member's lost link or silence never replaces a live host; only every survivor's agreement does.
			const NetSeatRoster roster = RosterForRow(3);
			NetRosterEvent drop; drop.kind = NetRosterEventKind::LinkDropped; drop.seat = 1;
			NetRosterEvent lost; lost.kind = NetRosterEventKind::HostLinkLost; lost.quorum = false;
			const bool ok = ApplyRosterEvent(roster, drop).refused && ApplyRosterEvent(roster, lost).refused;
			check("seq S1 no election from one member's link", ok, "");
		}
		{
			// S2 / yy: failed returns back off and never remove the seat; past the bound the return is still offered at the longest backoff.
			NetSeatRoster roster = RosterForRow(5);
			uint64_t now = 10000;
			bool ok = true;
			for (int attempt = 1; attempt <= 6 && ok; ++attempt) {
				NetRosterEvent aborted; aborted.kind = NetRosterEventKind::TransferAborted; aborted.seat = 2; aborted.nowMs = now;
				const NetRosterResult failed = ApplyRosterEvent(roster, aborted);
				ok = !failed.refused && failed.roster.Find(2)->owner == 0x1002 && phaseOf(failed.roster, 2) == NetSeatPhase::Held;
				roster = failed.roster;
				NetRosterEvent early; early.kind = NetRosterEventKind::Returned; early.seat = 2; early.ticket = 0x7002; early.nowMs = now + 1;
				ok = ok && ApplyRosterEvent(roster, early).refused;
				NetRosterEvent later = early;
				later.nowMs = roster.Find(2)->returnAfterMs;
				const NetRosterResult again = ApplyRosterEvent(roster, later);
				ok = ok && !again.refused && phaseOf(again.roster, 2) == NetSeatPhase::RejoinImage && roster.Find(2)->returnAfterMs - now <= (c_RosterReturnBackoffMs << (c_RosterReturnAttempts - 1));
				roster = again.roster;
				now = later.nowMs;
			}
			check("seq S2 a failing return backs off and keeps its seat", ok, "failed=" + std::to_string(roster.Find(2)->failedReturns));
		}
		{
			// A12: an applicant the host accepts takes a held seat; its former player is told why when it returns.
			NetSeatRoster roster = RosterForRow(4);
			NetRosterEvent accepted; accepted.kind = NetRosterEventKind::ApplicantAccepted; accepted.seat = 2; accepted.owner = 0x2002; accepted.ticket = 0x8002;
			roster = ApplyRosterEvent(roster, accepted).roster;
			NetRosterEvent back; back.kind = NetRosterEventKind::Returned; back.seat = 2; back.ticket = 0x7002;
			const NetRosterResult returned = ApplyRosterEvent(roster, back);
			check("seq A12 a former player is told the seat was given away", returned.refused && returned.reason == "The host gave your seat to another player", "reason='" + returned.reason + "'");
		}
		{
			// A true host loss: the survivors agree, the round changes host, and the new host's generation is one more.
			NetSeatRoster roster = RosterForRow(3);
			NetRosterEvent lost; lost.kind = NetRosterEventKind::HostLinkLost; lost.quorum = true;
			roster = ApplyRosterEvent(roster, lost).roster;
			NetRosterEvent changed; changed.kind = NetRosterEventKind::HostChanged; changed.seat = 3;
			const NetRosterResult handed = ApplyRosterEvent(roster, changed);
			check("seq HL4 a true host loss hands the round over", !handed.refused && handed.roster.hostSeat == 3 && handed.roster.migrationGen == 1 &&
			      phaseOf(handed.roster, 2) == NetSeatPhase::Running && phaseOf(handed.roster, 1) == NetSeatPhase::Held, "reason='" + handed.reason + "'");
		}
		{
			// Replication: a revision goes whole and without tickets, a peer takes only newer ones, and a copy that differs is refused by name.
			NetSeatRoster host = RosterForRow(4);
			host.revision = 7;
			host.seats[1].givenAwayTicket = 0x6002;
			const std::vector<uint8_t> wire = EncodeRoster(host);
			NetSeatRoster copy;
			std::string error, why;
			bool ok = DecodeRoster(wire, copy, &error);
			// A reconnect ticket is its seat's secret: no peer but the host ever holds it.
			for (const NetRosterSeat& seat: host.seats) {
				for (const uint64_t secret: {seat.ticket, seat.givenAwayTicket}) {
					if (secret == 0) continue;
					std::vector<uint8_t> needle;
					PutBytes(needle, secret, 8);
					ok = ok && std::search(wire.begin(), wire.end(), needle.begin(), needle.end()) == wire.end();
				}
			}
			ok = ok && copy.revision == 7 && copy.seats.size() == host.seats.size() && HashRoster(copy) == HashRoster(host);
			NetRosterReplica replica;
			ok = ok && replica.Apply(copy, &why);
			NetSeatRoster older = copy;
			older.revision = 6;
			ok = ok && !replica.Apply(older, &why) && why == "an older seat roster revision";
			NetSeatRoster other = copy;
			other.matchId = 99;
			other.revision = 9;
			ok = ok && !replica.Apply(other, &why) && why == "a seat roster of another match";
			ok = ok && replica.Agrees(HashRoster(host), &why);
			NetSeatRoster moved = host;
			moved.seats[1].phase = NetSeatPhase::RejoinImage;
			ok = ok && !replica.Agrees(HashRoster(moved), &why) && why.find("differs from the host's") != std::string::npos;
			// The first seat's phase byte, past the header (22 bytes) and the seat's id, owner and incarnation.
			std::vector<uint8_t> bad = wire;
			bad[33] = 0xFF;
			NetSeatRoster rejected;
			ok = ok && !DecodeRoster(bad, rejected, &error);
			check("seq R5 the roster replicates whole, in order, refused by name when it differs", ok, "wire_bytes=" + std::to_string(wire.size()) + " error='" + error + "' why='" + why + "'");
		}
		{
			// Ruling ppp: before the first start a removal frees the seat; from the first start a kick, a ban or a release opens it with its
			// number kept and its cause named, and only a held seat is released.
			NetSeatRoster lobby = RosterForRow(1);
			NetRosterEvent kick; kick.kind = NetRosterEventKind::Kicked; kick.seat = 2;
			const NetRosterResult freed = ApplyRosterEvent(lobby, kick);
			const bool lobbyFrees = !freed.refused && freed.roster.Find(2)->owner == 0 && freed.roster.Find(2)->phase == NetSeatPhase::Lobby && RosterSeatLabel(*freed.roster.Find(2)) == "Open";
			NetSeatRoster running = RosterForRow(3);
			const NetRosterResult kickedOut = ApplyRosterEvent(running, kick);
			NetRosterEvent ban = kick; ban.kind = NetRosterEventKind::Banned;
			const NetRosterResult bannedOut = ApplyRosterEvent(running, ban);
			NetRosterEvent release = kick; release.kind = NetRosterEventKind::SeatReleased;
			const NetRosterResult playing = ApplyRosterEvent(running, release);
			const NetRosterResult released = ApplyRosterEvent(RosterForRow(4), release);
			const bool opens = RosterSeatLabel(*kickedOut.roster.Find(2)) == "Open - AI in control (kicked)" && kickedOut.roster.Find(2)->phase == NetSeatPhase::Held &&
			                   RosterSeatLabel(*bannedOut.roster.Find(2)) == "Open - AI in control (banned)" && bannedOut.roster.banned.size() == 1 &&
			                   playing.refused && playing.reason == "the host releases only a held seat" &&
			                   !released.refused && RosterSeatLabel(*released.roster.Find(2)) == "Open - AI in control (released)";
			check("seq OPEN the host frees a first-lobby seat and opens a later one with its number kept", lobbyFrees && opens,
			      "lobby='" + RosterSeatLabel(*freed.roster.Find(2)) + "' kicked='" + RosterSeatLabel(*kickedOut.roster.Find(2)) + "' release_while_playing='" + playing.reason + "'");
		}
		{
			// D54.6: the round holds a playing seat whose link stays open, with its cause; the seat returns in place, or its link closes
			// and it is held for the lost connection.
			NetSeatRoster running = RosterForRow(3);
			NetRosterEvent late; late.kind = NetRosterEventKind::HeldInPlace; late.seat = 2; late.nowMs = 10000; late.cause = NetSeatHoldCause::LateStream;
			const NetRosterResult held = ApplyRosterEvent(running, late);
			NetRosterEvent back; back.kind = NetRosterEventKind::Returned; back.seat = 2; back.nowMs = 11000; back.keptWorld = true; back.ticket = held.roster.Find(2)->ticket;
			const NetRosterResult returning = ApplyRosterEvent(held.roster, back);
			NetRosterEvent played; played.kind = NetRosterEventKind::CaughtUp; played.seat = 2;
			const NetRosterResult playing = ApplyRosterEvent(returning.roster, played);
			NetRosterEvent lost; lost.kind = NetRosterEventKind::LinkDropped; lost.seat = 2; lost.nowMs = 12000;
			const NetRosterResult closed = ApplyRosterEvent(held.roster, lost);
			const NetRosterSeat& inPlace = *held.roster.Find(2);
			const bool ok = inPlace.phase == NetSeatPhase::Held && inPlace.holdCause == NetSeatHoldCause::LateStream && inPlace.link == NetSeatLink::Connected &&
			                RosterSeatLabel(inPlace) == "Held - AI in control (connection too slow)" && returning.roster.Find(2)->phase == NetSeatPhase::RejoinCatchUp &&
			                playing.roster.Find(2)->phase == NetSeatPhase::Running && closed.roster.Find(2)->phase == NetSeatPhase::Held &&
			                closed.roster.Find(2)->link == NetSeatLink::Dropped && closed.roster.Find(2)->holdCause == NetSeatHoldCause::LinkDrop;
			check("seq IN PLACE the round holds a seat whose link stays open, which returns in place or is held for its lost link", ok,
			      "held='" + RosterSeatLabel(inPlace) + "' back=" + NetSeatPhaseName(playing.roster.Find(2)->phase) + " lost='" + RosterSeatLabel(*closed.roster.Find(2)) + "'");
		}
		{
			// The start gate: the round is agreed on one revision; a peer that heard it hashed alike starts, any other is refused by name.
			NetRosterReplica replica;
			std::string why;
			NetSeatRoster roster = RosterForRow(3);
			std::array<uint8_t, 32> agreed{};
			bool ok = true;
			for (uint32_t revision = 4; revision <= 6; ++revision) {
				roster.revision = revision;
				ok = ok && replica.Apply(roster, &why);
				if (revision == 5) agreed = HashRoster(roster);
			}
			const bool heard = replica.AgreesAt(5, agreed, &why);
			NetSeatRoster moved = roster;
			moved.revision = 5;
			moved.seats[1].phase = NetSeatPhase::Held;
			const bool differs = !replica.AgreesAt(5, HashRoster(moved), &why) && why == "seat roster revision 5 differs from the host's";
			std::string unheardWhy;
			const bool unheard = !replica.AgreesAt(9, agreed, &unheardWhy) && unheardWhy == "this peer never heard seat roster revision 9 (it holds 6)";
			check("seq START the round starts only on the revision agreed, refused by name otherwise", ok && heard && differs && unheard,
			      "heard=" + std::to_string(heard) + " why='" + why + "' unheard='" + unheardWhy + "'");
		}
		std::cout << tag << " totals pass=" << pass << " fail=" << fail << " na=" << unreachable << std::endl;
		std::cout << tag << (fail == 0 ? " PASS" : " FAIL") << std::endl;
		return fail == 0 ? 0 : 1;
	}

} // namespace RTE
