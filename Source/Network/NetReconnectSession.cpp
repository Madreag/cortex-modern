#include "NetReconnectSession.h"

#include "NetAuthCrypto.h"
#include "NetReconnectTranscript.h"
#include "NetSeatAuth.h"

#include <algorithm>
#include <utility>

namespace RTE {

	namespace {
		// Every denial reads the same on the wire; the reason is diagnostics the host keeps to itself.
		constexpr const char* c_DenialText = "reconnect denied";

		NetH4TxKey MakeKey(NetMessageType requestType, uint16_t stableSeat, uint32_t holderGeneration, const NetH4Identity& identity) {
			NetH4TxKey key;
			key.requestType = requestType;
			key.stableSeat = stableSeat;
			key.holderGeneration = holderGeneration;
			key.identity = identity;
			return key;
		}
	} // namespace

	std::vector<NetH4Seat> NetH4BuildSeatTable(const NetMatchConfig& config) {
		std::vector<NetH4Seat> seats;
		seats.reserve(config.players.size());
		for (size_t index = 0; index < config.players.size(); ++index) {
			const NetMatchPlayerSlot& slot = config.players[index];
			// The match config carries the LOCKSTEP id; the session's is one lower. A commit hands back
			// the session id, the ledger and the reseat name the lockstep one - the two are not the same
			// number and swapping them would re-point every actor the returner had.
			NetH4Seat seat;
			seat.stableSeat = static_cast<uint16_t>(index);
			seat.peerId = slot.peerId > 0 ? static_cast<uint8_t>(slot.peerId - 1) : 0;
			seat.team = static_cast<int32_t>(slot.team);
			seat.cpu = slot.cpu;
			seat.lockstepPeerId = slot.peerId;
			seat.local = !slot.cpu && slot.peerId == config.hostPeerId;
			seats.push_back(seat);
		}
		return seats;
	}

	void NetReconnectHost::Configure(NetSeatAuthRegistry* registry, uint64_t hostSessionId, NetH4Identity localIdentity) {
		m_Registry = registry;
		m_HostSessionId = hostSessionId;
		m_LocalIdentity = std::move(localIdentity);
	}

	void NetReconnectHost::SetSeatTable(std::vector<NetH4Seat> seats, NetMatchMode mode) {
		m_Mode = mode;
		std::vector<SeatState> next;
		next.reserve(seats.size());
		for (const NetH4Seat& seat : seats) {
			SeatState state;
			state.seat = seat;
			// A rematch keeps the epoch and the tickets, so a seat that is already held keeps its holder.
			if (const SeatState* existing = FindSeat(seat.stableSeat)) {
				state.identity = existing->identity;
				state.holderGeneration = existing->holderGeneration;
				state.incarnation = existing->incarnation;
				state.activeConnection = existing->activeConnection;
				state.committed = existing->committed;
				state.closed = existing->closed;
				state.saturated = existing->saturated;
			}
			next.push_back(state);
		}
		m_Seats = std::move(next);
	}

	void NetReconnectHost::SetDropOwnershipSource(std::vector<NetH4LedgerActor> (*source)(void*), void* context) {
		m_DropOwnershipSource = source;
		m_DropOwnershipContext = context;
	}

	NetReconnectHost::SeatState* NetReconnectHost::FindSeat(uint16_t stableSeat) {
		const auto found = std::find_if(m_Seats.begin(), m_Seats.end(), [stableSeat](const SeatState& state) {
			return state.seat.stableSeat == stableSeat;
		});
		return found == m_Seats.end() ? nullptr : &*found;
	}

	const NetReconnectHost::SeatState* NetReconnectHost::FindSeat(uint16_t stableSeat) const {
		const auto found = std::find_if(m_Seats.begin(), m_Seats.end(), [stableSeat](const SeatState& state) {
			return state.seat.stableSeat == stableSeat;
		});
		return found == m_Seats.end() ? nullptr : &*found;
	}

	NetReconnectHost::SeatState* NetReconnectHost::FindFreeNeverHeldSeat() {
		for (SeatState& state : m_Seats) {
			// The host's own seat is never offered: nobody joins it, and host loss is out of scope.
			if (state.seat.cpu || state.seat.local || state.committed || state.closed || state.holderGeneration != 0) {
				continue;
			}
			const bool provisional = std::any_of(m_Provisionals.begin(), m_Provisionals.end(), [&state](const Provisional& pending) {
				return pending.stableSeat == state.seat.stableSeat;
			});
			if (!provisional) {
				return &state;
			}
		}
		return nullptr;
	}

	NetReconnectHost::Provisional* NetReconnectHost::FindProvisionalByTxId(const NetAuthBytes16& txId) {
		const auto found = std::find_if(m_Provisionals.begin(), m_Provisionals.end(), [&txId](const Provisional& pending) {
			return pending.txId == txId;
		});
		return found == m_Provisionals.end() ? nullptr : &*found;
	}

	void NetReconnectHost::Send(NetPeerId connection, NetPayload payload) {
		m_Outbound.push_back({connection, std::move(payload)});
	}

	std::vector<NetH4Outbound> NetReconnectHost::TakeOutbound() {
		std::vector<NetH4Outbound> taken = std::move(m_Outbound);
		m_Outbound.clear();
		return taken;
	}

	std::vector<NetH4Commit> NetReconnectHost::TakeCommits() {
		std::vector<NetH4Commit> taken = std::move(m_Commits);
		m_Commits.clear();
		return taken;
	}

	std::vector<NetGameReseat> NetReconnectHost::TakePendingReseats() {
		std::vector<NetGameReseat> taken = std::move(m_PendingReseats);
		m_PendingReseats.clear();
		return taken;
	}

	bool NetReconnectHost::MatchesEpoch(const NetAuthBytes16& epoch) const {
		if (m_Registry == nullptr || !m_Registry->IsActive()) {
			return false;
		}
		return NetAuthConstantTimeEquals(m_Registry->GetEpoch().data(), epoch.data(), epoch.size());
	}

	bool NetReconnectHost::ValidateIdentity(NetPeerId connection, const NetH4Identity& identity) {
		const auto reject = [&](NetRejectReason reason, const std::string& key, const std::string& summary) {
			++m_Stats.identityRejections;
			Send(connection, NetJoinRejected{reason, summary, key, "", ""});
			return false;
		};
		if (identity.controllerFrameVersion != m_LocalIdentity.controllerFrameVersion) {
			return reject(NetRejectReason::ControllerFrameVersionMismatch, "controller_frame_version", "ControllerFrame version does not match");
		}
		if (identity.controllerFrameEncodedSize != m_LocalIdentity.controllerFrameEncodedSize) {
			return reject(NetRejectReason::ControllerFrameSizeMismatch, "controller_frame_encoded_size", "ControllerFrame encoded size does not match");
		}
		if (identity.gameVersion != m_LocalIdentity.gameVersion) {
			return reject(NetRejectReason::GameVersionMismatch, "game_version", "game version does not match");
		}
		if (identity.buildId != m_LocalIdentity.buildId) {
			return reject(NetRejectReason::BuildMismatch, "build_id", "build id does not match");
		}
		if (identity.deterministicConfigHash != m_LocalIdentity.deterministicConfigHash) {
			return reject(NetRejectReason::DeterministicConfigMismatch, "deterministic_config_hash", "deterministic config hash does not match");
		}
		if (identity.moduleManifestHash != m_LocalIdentity.moduleManifestHash) {
			return reject(NetRejectReason::ModuleManifestMismatch, "module_manifest_hash", "module manifest hash does not match");
		}
		if (identity.sessionRulesHash != m_LocalIdentity.sessionRulesHash) {
			return reject(NetRejectReason::SessionRulesMismatch, "session_rules_hash", "session rules hash does not match");
		}
		if (identity.sessionIdentityHash != m_LocalIdentity.sessionIdentityHash) {
			return reject(NetRejectReason::BuildMismatch, "session_identity_hash", "session identity hash does not match");
		}
		return true;
	}

	void NetReconnectHost::DenyUniformly(NetPeerId connection, const NetAuthBytes16& txId, NetH4DenialReason reason, uint64_t nowMs) {
		m_Admission.ScheduleDenial(connection, txId, reason, nowMs);
		++m_Stats.denialsScheduled;
	}

	void NetReconnectHost::ChallengeSyntheticallyAndDeny(NetPeerId connection, const NetAuthBytes16& txId, NetH4DenialReason reason, uint64_t nowMs) {
		NetAuthBytes32 challenge{};
		if (m_Admission.IssueSyntheticChallenge(connection, nowMs, challenge)) {
			Send(connection, NetH4Challenge{c_NetH4Version, txId, challenge, static_cast<uint32_t>(NetReconnectAdmission::c_ChallengeLifetimeMs)});
		}
		DenyUniformly(connection, txId, reason, nowMs);
	}

	const NetPayload* NetReconnectHost::FindCached(const NetAuthBytes16& txId, const NetH4TxKey& key, uint64_t nowMs) {
		const NetPayload* cached = m_TxCache.Find(txId, key, nowMs);
		if (cached != nullptr) {
			++m_Stats.replayedResults;
		}
		return cached;
	}

	bool NetReconnectHost::HandleMessage(NetPeerId connection, const NetPayload& payload, uint64_t nowMs) {
		m_NowMs = std::max(m_NowMs, nowMs);
		if (const auto* newJoin = std::get_if<NetH4NewJoin>(&payload)) {
			HandleNewJoin(connection, *newJoin, nowMs);
			return true;
		}
		if (const auto* ack = std::get_if<NetH4TicketStoredAck>(&payload)) {
			HandleTicketStoredAck(connection, *ack, nowMs);
			return true;
		}
		if (const auto* reclaim = std::get_if<NetH4Reclaim>(&payload)) {
			HandleReclaim(connection, *reclaim, nowMs);
			return true;
		}
		if (const auto* proof = std::get_if<NetH4Proof>(&payload)) {
			HandleProof(connection, *proof, nowMs);
			return true;
		}
		if (const auto* leave = std::get_if<NetH4LeaveRequest>(&payload)) {
			HandleLeaveRequest(connection, *leave, nowMs);
			return true;
		}
		if (const auto* applicant = std::get_if<NetH4Applicant>(&payload)) {
			HandleApplicant(connection, *applicant, nowMs);
			return true;
		}
		if (const auto* ack = std::get_if<NetH4SubstitutionAck>(&payload)) {
			HandleSubstitutionAck(connection, *ack, nowMs);
			return true;
		}
		// TicketOffer, Challenge, JoinCommitted, LeaveAck, ApplicantAck and SubstitutionOffer are the
		// host's own replies; a client that sends one is talking out of turn.
		return std::holds_alternative<NetH4TicketOffer>(payload) || std::holds_alternative<NetH4Challenge>(payload) ||
		       std::holds_alternative<NetH4JoinCommitted>(payload) || std::holds_alternative<NetH4LeaveAck>(payload) ||
		       std::holds_alternative<NetH4ApplicantAck>(payload) || std::holds_alternative<NetH4SubstitutionOffer>(payload);
	}

	void NetReconnectHost::HandleNewJoin(NetPeerId connection, const NetH4NewJoin& message, uint64_t nowMs) {
		++m_Stats.newJoins;
		if (!ValidateIdentity(connection, message.identity)) {
			return;
		}
		const NetH4TxKey key = MakeKey(NetMessageType::NewJoin, 0, 0, message.identity);
		if (const NetPayload* cached = FindCached(message.txId, key, nowMs)) {
			Send(connection, *cached);
			return;
		}
		if (Provisional* pending = FindProvisionalByTxId(message.txId)) {
			// A retransmitted NewJoin re-offers the same ticket rather than opening a second seat.
			pending->connection = connection;
			pending->lastSentMs = nowMs;
			++pending->retransmits;
			++m_Stats.ticketOfferRetransmits;
			Send(connection, pending->offer);
			return;
		}
		if (m_Registry == nullptr || !m_Registry->IsActive()) {
			++m_Stats.provisionalSeatsRefused;
			Send(connection, NetJoinRejected{NetRejectReason::HostNotAccepting, "reconnect auth is unavailable", "reconnect_auth", "", ""});
			return;
		}
		if (m_LiveMatch) {
			// Phase A: a claimant without a ticket cannot prove anything, so a live match refuses it.
			++m_Stats.provisionalSeatsRefused;
			DenyUniformly(connection, message.txId, NetH4DenialReason::UnknownSeat, nowMs);
			return;
		}
		if (m_Provisionals.size() >= c_MaxProvisionalSeats) {
			++m_Stats.provisionalSeatsRefused;
			Send(connection, NetJoinRejected{NetRejectReason::SessionFull, "session is full", "provisional_seats", std::to_string(c_MaxProvisionalSeats), std::to_string(m_Provisionals.size())});
			return;
		}
		SeatState* seat = FindFreeNeverHeldSeat();
		if (seat == nullptr) {
			++m_Stats.provisionalSeatsRefused;
			Send(connection, NetJoinRejected{NetRejectReason::SessionFull, "session is full", "seats", "", ""});
			return;
		}
		uint32_t holderGeneration = 0;
		NetSeatCredential credential{};
		if (!m_Registry->IssueCredential(seat->seat.stableSeat, holderGeneration, credential)) {
			++m_Stats.provisionalSeatsRefused;
			Send(connection, NetJoinRejected{NetRejectReason::InternalError, "reconnect auth could not issue a ticket", "reconnect_auth", "", ""});
			return;
		}

		Provisional pending;
		pending.stableSeat = seat->seat.stableSeat;
		pending.txId = message.txId;
		pending.connection = connection;
		pending.holderGeneration = holderGeneration;
		pending.openedAtMs = nowMs;
		pending.lastSentMs = nowMs;
		pending.key = MakeKey(NetMessageType::NewJoin, seat->seat.stableSeat, holderGeneration, message.identity);
		pending.offer = NetH4TicketOffer{c_NetH4Version, message.txId, m_Registry->GetEpoch(), seat->seat.stableSeat, holderGeneration, credential, m_HostSessionId, static_cast<uint32_t>(c_ProvisionalExpiryMs)};
		m_Provisionals.push_back(pending);
		++m_Stats.provisionalSeatsOpened;
		++m_Stats.ticketOffersSent;
		Send(connection, pending.offer);
	}

	void NetReconnectHost::HandleTicketStoredAck(NetPeerId connection, const NetH4TicketStoredAck& message, uint64_t nowMs) {
		// The ack carries no identity block, so the seat's own recorded identity rebuilds the key the
		// commit was cached against - a lost JoinCommitted must never read as a failure.
		if (const SeatState* seat = FindSeat(message.stableSeat); seat != nullptr && seat->committed && seat->holderGeneration == message.holderGeneration) {
			const NetH4TxKey key = MakeKey(NetMessageType::NewJoin, message.stableSeat, message.holderGeneration, seat->identity);
			if (const NetPayload* cached = FindCached(message.txId, key, nowMs)) {
				Send(connection, *cached);
				return;
			}
		}
		Provisional* pending = FindProvisionalByTxId(message.txId);
		if (pending == nullptr) {
			// The provisional seat expired, or this names a transaction that never existed. Either way
			// the client's orphan ticket is stale: drop it without answering.
			++m_Stats.unknownTransactionDrops;
			return;
		}
		if (pending->stableSeat != message.stableSeat || pending->holderGeneration != message.holderGeneration) {
			++m_Stats.unknownTransactionDrops;
			return;
		}
		if (pending->connection != connection) {
			// The ack arrived on a fresh connection: the offer was persisted but the first link died
			// before the ack landed, which is exactly the transaction the resume window exists for.
			++m_Stats.provisionalSeatsResumed;
			pending->connection = connection;
		}
		if (!message.stored) {
			// Persistence failure blocks the join: without a durable ticket the client could never
			// prove this seat again, so the seat must not be committed to it.
			++m_Stats.persistenceFailures;
			if (m_Registry != nullptr) {
				m_Registry->RevokeSeat(pending->stableSeat);
			}
			const uint16_t stableSeat = pending->stableSeat;
			Send(connection, NetJoinRejected{NetRejectReason::InternalError, "the reconnect ticket could not be stored", "ticket_store", "stored", "failed"});
			ReleaseProvisional(stableSeat);
			return;
		}

		SeatState* seat = FindSeat(pending->stableSeat);
		if (seat == nullptr) {
			++m_Stats.unknownTransactionDrops;
			return;
		}
		seat->identity = pending->key.identity;
		seat->holderGeneration = pending->holderGeneration;
		seat->committed = true;
		seat->closed = false;
		seat->incarnation = 0;
		seat->saturated = false;
		if (!BindIncarnation(*seat, connection)) {
			Send(connection, NetJoinRejected{NetRejectReason::InternalError, c_DenialText, "incarnation", "", ""});
			ReleaseProvisional(pending->stableSeat);
			return;
		}
		const NetH4JoinCommitted committed{c_NetH4Version, message.txId, seat->seat.stableSeat, seat->holderGeneration, seat->incarnation, seat->seat.peerId};
		m_TxCache.Store(message.txId, pending->key, committed, nowMs);
		++m_Stats.provisionalSeatsCommitted;
		m_Commits.push_back({connection, seat->seat.stableSeat, seat->seat.peerId, seat->incarnation, c_InvalidNetPeerId, false});
		Send(connection, committed);
		ReleaseProvisional(pending->stableSeat);
	}

	void NetReconnectHost::HandleReclaim(NetPeerId connection, const NetH4Reclaim& message, uint64_t nowMs) {
		if (!ValidateIdentity(connection, message.identity)) {
			return;
		}
		const NetH4TxKey key = MakeKey(NetMessageType::Reclaim, message.stableSeat, message.holderGeneration, message.identity);
		if (const NetPayload* cached = FindCached(message.txId, key, nowMs)) {
			Send(connection, *cached);
			return;
		}
		// A retransmitted Reclaim is the SAME attempt, not a second one. P3's ladder retransmits every
		// 250 ms while P16 admits one attempt per second, so charging a retransmit to the rate limit
		// denies a client that is merely doing what the ladder says. The answer is already in flight on
		// ControlReliable - the ladder covers a peer restart, not packet loss - so this drops the
		// duplicate rather than re-answering it: re-sending the challenge would tell an attacker the
		// seat is known BEFORE the uniform denial fires, which is the oracle §5 exists to remove.
		if (std::any_of(m_PendingReclaims.begin(), m_PendingReclaims.end(), [connection, &message](const PendingReclaim& pending) {
			    return pending.connection == connection && pending.txId == message.txId;
		    })) {
			++m_Stats.reclaimRetransmitsDropped;
			return;
		}
		if (!m_Admission.BeginAttempt(connection, nowMs)) {
			DenyUniformly(connection, message.txId, NetH4DenialReason::RateLimited, nowMs);
			return;
		}
		if (!MatchesEpoch(message.epoch)) {
			// A stale epoch takes the unknown-seat path, so the two cannot be told apart by timing.
			++m_Stats.staleEpochDrops;
			ChallengeSyntheticallyAndDeny(connection, message.txId, NetH4DenialReason::StaleEpoch, nowMs);
			return;
		}
		SeatState* seat = FindSeat(message.stableSeat);
		const bool superseded = seat != nullptr && m_Registry != nullptr && seat->retiredGeneration == message.holderGeneration &&
		                        nowMs < seat->retiredUntilMs && m_Registry->HasRetiredGeneration(message.stableSeat, message.holderGeneration);
		if (!superseded && (seat == nullptr || !seat->committed || seat->closed || seat->saturated ||
		                    seat->holderGeneration == 0 || seat->holderGeneration != message.holderGeneration ||
		                    m_Registry == nullptr || m_Registry->GetActiveGeneration(message.stableSeat) != message.holderGeneration)) {
			ChallengeSyntheticallyAndDeny(connection, message.txId, NetH4DenialReason::UnknownSeat, nowMs);
			return;
		}
		NetAuthBytes32 challenge{};
		if (!m_Admission.IssueChallenge(connection, message.txId, message.stableSeat, message.holderGeneration, nowMs, challenge)) {
			DenyUniformly(connection, message.txId, NetH4DenialReason::ProviderUnavailable, nowMs);
			return;
		}
		PendingReclaim reclaim;
		reclaim.connection = connection;
		reclaim.txId = message.txId;
		reclaim.stableSeat = message.stableSeat;
		reclaim.holderGeneration = message.holderGeneration;
		reclaim.key = key;
		reclaim.openedAtMs = nowMs;
		reclaim.superseded = superseded;
		m_PendingReclaims.erase(std::remove_if(m_PendingReclaims.begin(), m_PendingReclaims.end(), [connection, &message](const PendingReclaim& pending) {
			return pending.connection == connection && pending.txId == message.txId;
		}), m_PendingReclaims.end());
		m_PendingReclaims.push_back(reclaim);
		Send(connection, NetH4Challenge{c_NetH4Version, message.txId, challenge, static_cast<uint32_t>(NetReconnectAdmission::c_ChallengeLifetimeMs)});
	}

	void NetReconnectHost::HandleProof(NetPeerId connection, const NetH4Proof& message, uint64_t nowMs) {
		const auto pending = std::find_if(m_PendingReclaims.begin(), m_PendingReclaims.end(), [connection, &message](const PendingReclaim& reclaim) {
			return reclaim.connection == connection && reclaim.txId == message.txId;
		});
		// P1: a Proof retransmitted because JoinCommitted was lost must replay the cached success, never
		// read as an auth failure. The pending record is gone the moment the reclaim commits, so past
		// that point the key is rebuilt from the seat's own recorded identity - the same thing a
		// re-presented TicketStoredAck does.
		if (pending != m_PendingReclaims.end()) {
			if (const NetPayload* cached = FindCached(message.txId, pending->key, nowMs)) {
				Send(connection, *cached);
				return;
			}
		} else if (const SeatState* committedSeat = FindSeat(message.stableSeat);
		           committedSeat != nullptr && committedSeat->committed && !committedSeat->closed &&
		           committedSeat->holderGeneration == message.holderGeneration) {
			const NetH4TxKey key = MakeKey(NetMessageType::Reclaim, message.stableSeat, message.holderGeneration, committedSeat->identity);
			if (const NetPayload* cached = FindCached(message.txId, key, nowMs)) {
				Send(connection, *cached);
				return;
			}
		}
		NetH4ChallengeRecord issued;
		if (!m_Admission.ConsumeChallenge(connection, message.txId, nowMs, issued)) {
			// Consumed on the first attempt whether or not it verified, so a replay reads as expired.
			DenyUniformly(connection, message.txId, NetH4DenialReason::ExpiredChallenge, nowMs);
			return;
		}
		if (!MatchesEpoch(message.epoch) || issued.stableSeat != message.stableSeat || issued.holderGeneration != message.holderGeneration) {
			DenyUniformly(connection, message.txId, NetH4DenialReason::BadProof, nowMs);
			return;
		}
		NetH4Transcript transcript;
		transcript.domain = NetH4ProofDomain::Reclaim;
		transcript.protocolVersion = NetProtocol::c_Version;
		transcript.epoch = message.epoch;
		transcript.stableSeat = message.stableSeat;
		transcript.holderGeneration = message.holderGeneration;
		transcript.challenge = issued.challenge;
		transcript.clientNonce = message.clientNonce;
		if (pending != m_PendingReclaims.end() && pending->superseded) {
			// Only a claimant that proved the retired credential hears this, so it tells nobody who does
			// not already hold that credential that the seat ever existed.
			const bool proved = m_Registry != nullptr && m_Registry->VerifyRetiredProof(message.stableSeat, message.holderGeneration, transcript, message.mac);
			if (proved) {
				++m_Stats.reassignedReclaimsRefused;
				const NetPayload refusal = NetJoinRejected{NetRejectReason::SeatReassigned, "this seat was given to another player", "seat_reassigned", "", ""};
				m_Admission.ScheduleDenial(connection, message.txId, NetH4DenialReason::SeatReassigned, nowMs, &refusal);
				++m_Stats.denialsScheduled;
			} else {
				DenyUniformly(connection, message.txId, NetH4DenialReason::BadProof, nowMs);
			}
			m_PendingReclaims.erase(pending);
			return;
		}
		SeatState* seat = FindSeat(message.stableSeat);
		if (seat == nullptr || !seat->committed || seat->closed || m_Registry == nullptr ||
		    !m_Registry->VerifySeatProof(message.stableSeat, message.holderGeneration, transcript, message.mac)) {
			DenyUniformly(connection, message.txId, NetH4DenialReason::BadProof, nowMs);
			return;
		}
		const NetPeerId supersededConnection = seat->activeConnection != connection ? seat->activeConnection : c_InvalidNetPeerId;
		if (!BindIncarnation(*seat, connection)) {
			// The incarnation counter saturated: refuse further reclaims on this generation.
			DenyUniformly(connection, message.txId, NetH4DenialReason::UnknownSeat, nowMs);
			return;
		}
		const NetH4JoinCommitted committed{c_NetH4Version, message.txId, seat->seat.stableSeat, seat->holderGeneration, seat->incarnation, seat->seat.peerId};
		const NetH4TxKey key = pending != m_PendingReclaims.end() ? pending->key : MakeKey(NetMessageType::Reclaim, message.stableSeat, message.holderGeneration, seat->identity);
		seat->identity = key.identity;
		m_TxCache.Store(message.txId, key, committed, nowMs);
		if (pending != m_PendingReclaims.end()) {
			m_PendingReclaims.erase(pending);
		}
		++m_Stats.reclaimsAccepted;
		m_Commits.push_back({connection, seat->seat.stableSeat, seat->seat.peerId, seat->incarnation, supersededConnection, true});
		Send(connection, committed);
		IssueReseat(*seat);
	}

	void NetReconnectHost::HandleLeaveRequest(NetPeerId connection, const NetH4LeaveRequest& message, uint64_t nowMs) {
		const NetH4TxKey key = MakeKey(NetMessageType::LeaveRequest, message.stableSeat, message.holderGeneration, m_LocalIdentity);
		if (const NetPayload* cached = FindCached(message.txId, key, nowMs)) {
			Send(connection, *cached);
			return;
		}
		if (!MatchesEpoch(message.epoch)) {
			// Every message other than a Reclaim is dropped and counted on a stale epoch, never answered.
			++m_Stats.staleEpochDrops;
			return;
		}
		SeatState* seat = FindSeat(message.stableSeat);
		if (seat == nullptr || !seat->committed || seat->holderGeneration != message.holderGeneration || seat->activeConnection != connection) {
			++m_Stats.unknownTransactionDrops;
			return;
		}
		m_Admission.DropConnection(connection);
		if (m_LiveMatch) {
			if (m_Registry != nullptr) {
				m_Registry->RevokeSeat(message.stableSeat);
			}
			seat->closed = true;
			seat->committed = false;
			seat->activeConnection = c_InvalidNetPeerId;
			seat->dropped = false;
			BumpSeatGeneration(*seat);
			seat->retiredGeneration = 0;
			seat->retiredUntilMs = 0;
			m_Ledger.ClearSeat(message.stableSeat);
			m_Fences.erase(std::remove_if(m_Fences.begin(), m_Fences.end(), [&message](const Fence& fence) {
				return fence.stableSeat == message.stableSeat;
			}), m_Fences.end());
		} else {
			// A lobby leave takes nothing with it: the seat goes back in the pool so the next player -
			// this one returning or somebody new - joins exactly as they did before H4 existed.
			ReleaseSeat(*seat);
		}
		const NetH4LeaveAck ack{c_NetH4Version, message.txId, message.stableSeat, message.holderGeneration, true};
		m_TxCache.Store(message.txId, key, ack, nowMs);
		++m_Stats.seatsClosedByLeave;
		Send(connection, ack);
	}

	bool NetReconnectHost::BindIncarnation(SeatState& seat, NetPeerId connection) {
		if (seat.incarnation == UINT32_MAX) {
			seat.saturated = true;
			return false;
		}
		if (seat.activeConnection != c_InvalidNetPeerId && seat.activeConnection != connection) {
			// The superseded transport keeps sending for a while and will time out later; both must be
			// attributable to the dead incarnation, or a stale timeout evicts the live holder.
			m_Fences.push_back({seat.activeConnection, seat.seat.stableSeat, seat.incarnation});
			m_Admission.DropConnection(seat.activeConnection);
		}
		++seat.incarnation;
		seat.activeConnection = connection;
		seat.dropped = false;
		seat.holdExpired = false;
		BumpSeatGeneration(seat);
		// Returner wins: whoever commits first takes the seat, and a substitution that was still
		// waiting for its ack is invalidated and removed here, not left to be discovered later.
		for (size_t index = 0; index < m_Substitutions.size();) {
			if (m_Substitutions[index].stableSeat == seat.seat.stableSeat && m_Substitutions[index].connection != connection) {
				AbandonSubstitution(index, NetH4DenialReason::SubstitutionSuperseded, "the seat's own player returned first", m_NowMs);
				continue;
			}
			++index;
		}
		// The seat has a holder again, so nobody is waiting for it any more.
		DisplaceApplicants(seat.seat.stableSeat, connection, m_NowMs);
		// A transport that reclaims its own seat is live again, so it is no longer a fence.
		m_Fences.erase(std::remove_if(m_Fences.begin(), m_Fences.end(), [connection](const Fence& fence) {
			return fence.connection == connection;
		}), m_Fences.end());
		++m_Stats.incarnationsBound;
		return true;
	}

	void NetReconnectHost::ReleaseProvisional(uint16_t stableSeat) {
		m_Provisionals.erase(std::remove_if(m_Provisionals.begin(), m_Provisionals.end(), [stableSeat](const Provisional& pending) {
			return pending.stableSeat == stableSeat;
		}), m_Provisionals.end());
	}

	void NetReconnectHost::RecordDrop(SeatState& seat, uint64_t frame) {
		std::vector<int64_t> owned;
		if (m_DropOwnershipSource != nullptr) {
			owned = NetReconnectLedger::CollectOwnedActorUIDs(m_DropOwnershipSource(m_DropOwnershipContext), seat.seat.lockstepPeerId);
		}
		m_Ledger.RecordDrop(seat.seat.stableSeat, seat.seat.lockstepPeerId, seat.seat.team, frame, std::move(owned));
		++m_Stats.ledgerDropsRecorded;
	}

	void NetReconnectHost::ReleaseSeat(SeatState& seat) {
		if (m_Registry != nullptr) {
			m_Registry->RevokeSeat(seat.seat.stableSeat);
		}
		seat.holderGeneration = 0;
		seat.incarnation = 0;
		seat.activeConnection = c_InvalidNetPeerId;
		seat.committed = false;
		seat.closed = false;
		seat.saturated = false;
		seat.dropped = false;
		seat.holdExpired = false;
		seat.identity = {};
		seat.retiredGeneration = 0;
		seat.retiredUntilMs = 0;
		BumpSeatGeneration(seat);
		if (m_Registry != nullptr) {
			m_Registry->ClearRetired(seat.seat.stableSeat);
		}
		m_Ledger.ClearSeat(seat.seat.stableSeat);
		ReleaseProvisional(seat.seat.stableSeat);
		const uint16_t stableSeat = seat.seat.stableSeat;
		for (size_t index = 0; index < m_Substitutions.size();) {
			if (m_Substitutions[index].stableSeat == stableSeat) {
				AbandonSubstitution(index, NetH4DenialReason::SeatNotSubstitutable, "the seat went back into the pool", m_NowMs);
				continue;
			}
			++index;
		}
		m_Applicants.erase(std::remove_if(m_Applicants.begin(), m_Applicants.end(), [stableSeat](const Applicant& applicant) {
			return applicant.stableSeat == stableSeat;
		}), m_Applicants.end());
		m_Fences.erase(std::remove_if(m_Fences.begin(), m_Fences.end(), [stableSeat](const Fence& fence) {
			return fence.stableSeat == stableSeat;
		}), m_Fences.end());
		++m_Stats.seatsReleasedInLobby;
	}

	void NetReconnectHost::IssueReseat(const SeatState& seat) {
		const NetH4SeatOwnership* record = m_Ledger.Find(seat.seat.stableSeat);
		if (record == nullptr || record->actorUIDs.empty()) {
			return;
		}
		std::vector<NetH4LedgerActor> actors;
		if (m_DropOwnershipSource != nullptr) {
			actors = m_DropOwnershipSource(m_DropOwnershipContext);
		}
		std::vector<int64_t> restored = m_Ledger.BuildRestoration(seat.seat.stableSeat, m_Mode, actors);
		if (restored.empty()) {
			return;
		}
		NetGameReseat reseat;
		reseat.team = seat.seat.team;
		reseat.newOwnerPeerId = seat.seat.lockstepPeerId;
		reseat.actorUIDs = std::move(restored);
		m_PendingReseats.push_back(std::move(reseat));
		++m_Stats.reseatsIssued;
	}


	const char* NetH4ModerationResultName(NetH4ModerationResult result) {
		switch (result) {
			case NetH4ModerationResult::Ok: return "Ok";
			case NetH4ModerationResult::NotHosting: return "NotHosting";
			case NetH4ModerationResult::UnknownSeat: return "UnknownSeat";
			case NetH4ModerationResult::SeatNotSubstitutable: return "SeatNotSubstitutable";
			case NetH4ModerationResult::UnknownApplicant: return "UnknownApplicant";
			case NetH4ModerationResult::SubstitutionInFlight: return "SubstitutionInFlight";
			case NetH4ModerationResult::NoSubstitutionPending: return "NoSubstitutionPending";
			case NetH4ModerationResult::ProviderUnavailable: return "ProviderUnavailable";
		}
		return "Unknown";
	}

	bool NetReconnectHost::IsSeatSubstitutable(const SeatState& seat) const {
		// Moderation is a MATCH feature. A lobby seat goes back in the pool when its holder leaves, so
		// anyone can simply take it and there is nothing for the host to decide.
		if (!m_LiveMatch || seat.seat.cpu || seat.seat.local) {
			return false;
		}
		// Either the holder dropped and has not come back, or it left cleanly and the seat sits empty.
		return (seat.committed && seat.activeConnection == c_InvalidNetPeerId) || seat.closed;
	}

	void NetReconnectHost::BumpSeatGeneration(SeatState& seat) {
		if (seat.seatGeneration != UINT32_MAX) {
			++seat.seatGeneration;
		}
	}

	NetH4TxKey NetReconnectHost::SubstitutionKey(uint16_t stableSeat, uint32_t holderGeneration) {
		// The HOST draws a substitution transaction id (P1), so unlike a client-drawn one it is not
		// attacker-chosen: holding it already proves the offer was received. The key therefore binds
		// the seat and the generation - so a cached result can never be replayed onto another seat -
		// and nothing else.
		return MakeKey(NetMessageType::SubstitutionAck, stableSeat, holderGeneration, NetH4Identity{});
	}

	NetReconnectHost::Substitution* NetReconnectHost::FindSubstitutionBySeat(uint16_t stableSeat) {
		const auto found = std::find_if(m_Substitutions.begin(), m_Substitutions.end(), [stableSeat](const Substitution& pending) {
			return pending.stableSeat == stableSeat;
		});
		return found == m_Substitutions.end() ? nullptr : &*found;
	}

	NetReconnectHost::Substitution* NetReconnectHost::FindSubstitutionByConnection(NetPeerId connection) {
		const auto found = std::find_if(m_Substitutions.begin(), m_Substitutions.end(), [connection](const Substitution& pending) {
			return pending.connection == connection;
		});
		return found == m_Substitutions.end() ? nullptr : &*found;
	}

	NetReconnectHost::Applicant* NetReconnectHost::FindApplicant(NetPeerId connection, uint16_t stableSeat) {
		const auto found = std::find_if(m_Applicants.begin(), m_Applicants.end(), [connection, stableSeat](const Applicant& applicant) {
			return applicant.connection == connection && applicant.stableSeat == stableSeat;
		});
		return found == m_Applicants.end() ? nullptr : &*found;
	}

	bool NetReconnectHost::HasSubstitution(uint16_t stableSeat) const {
		return std::any_of(m_Substitutions.begin(), m_Substitutions.end(), [stableSeat](const Substitution& pending) {
			return pending.stableSeat == stableSeat;
		});
	}

	void NetReconnectHost::AbandonSubstitution(size_t index, NetH4DenialReason reason, const std::string& summary, uint64_t nowMs) {
		Substitution& pending = m_Substitutions[index];
		// The terminal result is cached before the record goes, so an ack already in flight replays
		// this refusal instead of finding nothing and being told nothing.
		const NetJoinRejected refusal{NetRejectReason::HostNotAccepting, summary, "substitution", NetH4DenialReasonName(reason), ""};
		m_TxCache.Store(pending.txId, pending.key, refusal, nowMs);
		if (pending.connection != c_InvalidNetPeerId) {
			Send(pending.connection, refusal);
		}
		for (Applicant& applicant : m_Applicants) {
			if (applicant.connection == pending.connection && applicant.stableSeat == pending.stableSeat) {
				applicant.approved = false;
			}
		}
		pending.credential.fill(0);
		++m_Stats.substitutionsCancelled;
		if (reason == NetH4DenialReason::SubstitutionSuperseded) {
			++m_Stats.substitutionsSuperseded;
		}
		m_Substitutions.erase(m_Substitutions.begin() + static_cast<std::ptrdiff_t>(index));
	}

	void NetReconnectHost::DisplaceApplicants(uint16_t stableSeat, NetPeerId keepConnection, uint64_t nowMs) {
		for (auto applicant = m_Applicants.begin(); applicant != m_Applicants.end();) {
			if (applicant->stableSeat != stableSeat || applicant->connection == keepConnection) {
				++applicant;
				continue;
			}
			// The host acknowledged this applicant, so telling it the seat is taken says nothing it
			// was not already told when the seat was offered to it as available.
			Send(applicant->connection, NetJoinRejected{NetRejectReason::SessionFull, "the seat went to another player", "applicant", "", ""});
			++m_Stats.applicantsDisplaced;
			applicant = m_Applicants.erase(applicant);
		}
		(void)nowMs;
	}

	void NetReconnectHost::DropApplicantsFor(NetPeerId connection) {
		m_Applicants.erase(std::remove_if(m_Applicants.begin(), m_Applicants.end(), [connection](const Applicant& applicant) {
			return applicant.connection == connection;
		}), m_Applicants.end());
	}

	void NetReconnectHost::HandleApplicant(NetPeerId connection, const NetH4Applicant& message, uint64_t nowMs) {
		// P5: identity first, before the seat is looked at, so a mismatch never says whether the seat
		// exists. An applicant is re-validated exactly like a NewJoin and a Reclaim.
		if (!ValidateIdentity(connection, message.identity)) {
			return;
		}
		const NetH4TxKey key = MakeKey(NetMessageType::Applicant, message.stableSeat, 0, message.identity);
		if (const NetPayload* cached = FindCached(message.txId, key, nowMs)) {
			Send(connection, *cached);
			return;
		}
		if (Applicant* existing = FindApplicant(connection, message.stableSeat); existing != nullptr && existing->txId == message.txId) {
			// The ladder is retransmitting; the answer is the one already recorded, not a second slot.
			Send(connection, NetH4ApplicantAck{c_NetH4Version, message.txId, message.stableSeat, static_cast<uint32_t>(c_ProvisionalExpiryMs)});
			return;
		}
		if (!m_Admission.BeginAttempt(connection, nowMs)) {
			++m_Stats.applicantsRefused;
			DenyUniformly(connection, message.txId, NetH4DenialReason::RateLimited, nowMs);
			return;
		}
		SeatState* seat = FindSeat(message.stableSeat);
		if (seat == nullptr || !IsSeatSubstitutable(*seat)) {
			++m_Stats.applicantsRefused;
			DenyUniformly(connection, message.txId, NetH4DenialReason::SeatNotSubstitutable, nowMs);
			return;
		}
		const size_t forSeat = static_cast<size_t>(std::count_if(m_Applicants.begin(), m_Applicants.end(), [&message](const Applicant& applicant) {
			return applicant.stableSeat == message.stableSeat;
		}));
		const size_t forConnection = static_cast<size_t>(std::count_if(m_Applicants.begin(), m_Applicants.end(), [connection](const Applicant& applicant) {
			return applicant.connection == connection;
		}));
		if (m_Applicants.size() >= c_MaxApplicants || forSeat >= c_MaxApplicantsPerSeat || forConnection >= c_MaxApplicantsPerConnection) {
			++m_Stats.applicantsRefused;
			DenyUniformly(connection, message.txId, NetH4DenialReason::ApplicantBoundReached, nowMs);
			return;
		}

		Applicant applicant;
		applicant.connection = connection;
		applicant.stableSeat = message.stableSeat;
		applicant.txId = message.txId;
		applicant.identity = message.identity;
		applicant.displayName = message.displayName;
		applicant.appliedAtMs = nowMs;
		applicant.key = key;
		m_Applicants.push_back(applicant);
		++m_Stats.applicantsRegistered;
		const NetH4ApplicantAck ack{c_NetH4Version, message.txId, message.stableSeat, static_cast<uint32_t>(c_ProvisionalExpiryMs)};
		m_TxCache.Store(message.txId, key, ack, nowMs);
		Send(connection, ack);
	}

	std::vector<NetH4ModerationSeat> NetReconnectHost::GetModerationView() const {
		std::vector<NetH4ModerationSeat> view;
		view.reserve(m_Seats.size());
		for (const SeatState& seat : m_Seats) {
			NetH4ModerationSeat entry;
			entry.stableSeat = seat.seat.stableSeat;
			entry.lockstepPeerId = seat.seat.lockstepPeerId;
			entry.team = seat.seat.team;
			entry.committed = seat.committed;
			entry.dropped = seat.committed && seat.activeConnection == c_InvalidNetPeerId;
			entry.closed = seat.closed;
			entry.heldForReclaim = seat.committed && !seat.closed && !seat.holdExpired;
			entry.substitutable = IsSeatSubstitutable(seat);
			entry.substituting = std::any_of(m_Substitutions.begin(), m_Substitutions.end(), [&seat](const Substitution& pending) {
				return pending.stableSeat == seat.seat.stableSeat;
			});
			entry.holderGeneration = seat.holderGeneration;
			entry.seatGeneration = seat.seatGeneration;
			for (const Applicant& applicant : m_Applicants) {
				if (applicant.stableSeat != seat.seat.stableSeat) {
					continue;
				}
				entry.applicants.push_back({applicant.connection, applicant.stableSeat, applicant.displayName,
				                            applicant.appliedAtMs, applicant.appliedAtMs + c_ProvisionalExpiryMs, applicant.approved});
			}
			view.push_back(std::move(entry));
		}
		return view;
	}

	NetH4ModerationResult NetReconnectHost::WaitForSeat(uint16_t stableSeat) {
		const SeatState* seat = FindSeat(stableSeat);
		if (seat == nullptr) {
			return NetH4ModerationResult::UnknownSeat;
		}
		// Waiting is what the plane already does; the verb exists so the host's choice is a decision it
		// made rather than one it never got round to.
		return IsSeatSubstitutable(*seat) ? NetH4ModerationResult::Ok : NetH4ModerationResult::SeatNotSubstitutable;
	}

	NetH4ModerationResult NetReconnectHost::SubstituteApplicant(uint16_t stableSeat, NetPeerId applicantConnection, uint64_t nowMs) {
		m_NowMs = std::max(m_NowMs, nowMs);
		if (m_Registry == nullptr || !m_Registry->IsActive()) {
			return NetH4ModerationResult::NotHosting;
		}
		SeatState* seat = FindSeat(stableSeat);
		if (seat == nullptr) {
			return NetH4ModerationResult::UnknownSeat;
		}
		if (!IsSeatSubstitutable(*seat)) {
			return NetH4ModerationResult::SeatNotSubstitutable;
		}
		if (HasSubstitution(stableSeat)) {
			return NetH4ModerationResult::SubstitutionInFlight;
		}
		Applicant* applicant = FindApplicant(applicantConnection, stableSeat);
		if (applicant == nullptr) {
			return NetH4ModerationResult::UnknownApplicant;
		}
		const uint32_t holderGeneration = m_Registry->PeekNextGeneration(stableSeat);
		if (holderGeneration == 0) {
			return NetH4ModerationResult::ProviderUnavailable;
		}
		// The credential is drawn here but NOT installed: the seat keeps whatever holder it has until
		// this transaction commits, so a returner racing it still wins by committing first.
		NetAuthBytes32 credential{};
		NetAuthBytes32 challenge{};
		NetAuthBytes16 txId{};
		if (!GetNetAuthCrypto().RandomBytes(credential.data(), credential.size()) ||
		    !NetH4DrawChallenge(challenge) || !NetH4DrawTxId(txId)) {
			return NetH4ModerationResult::ProviderUnavailable;
		}

		Substitution pending;
		pending.stableSeat = stableSeat;
		pending.connection = applicantConnection;
		pending.txId = txId;
		pending.holderGeneration = holderGeneration;
		pending.seatGeneration = seat->seatGeneration;
		pending.supersededGeneration = seat->holderGeneration;
		pending.credential = credential;
		pending.challenge = challenge;
		pending.identity = applicant->identity;
		pending.displayName = applicant->displayName;
		pending.openedAtMs = nowMs;
		pending.lastSentMs = nowMs;
		pending.key = SubstitutionKey(stableSeat, holderGeneration);
		pending.offer = NetH4SubstitutionOffer{c_NetH4Version, txId, m_Registry->GetEpoch(), stableSeat, holderGeneration,
		                                       credential, challenge, m_HostSessionId, static_cast<uint32_t>(c_ProvisionalExpiryMs)};
		applicant->approved = true;
		m_Substitutions.push_back(pending);
		++m_Stats.substitutionOffersSent;
		Send(applicantConnection, pending.offer);
		return NetH4ModerationResult::Ok;
	}

	NetH4ModerationResult NetReconnectHost::CancelSubstitution(uint16_t stableSeat, uint64_t nowMs) {
		m_NowMs = std::max(m_NowMs, nowMs);
		for (size_t index = 0; index < m_Substitutions.size(); ++index) {
			if (m_Substitutions[index].stableSeat == stableSeat) {
				AbandonSubstitution(index, NetH4DenialReason::SeatNotSubstitutable, "the host withdrew this substitution", nowMs);
				return NetH4ModerationResult::Ok;
			}
		}
		return NetH4ModerationResult::NoSubstitutionPending;
	}

	void NetReconnectHost::HandleSubstitutionAck(NetPeerId connection, const NetH4SubstitutionAck& message, uint64_t nowMs) {
		const NetH4TxKey key = SubstitutionKey(message.stableSeat, message.holderGeneration);
		// A lost commit result, or an ack that arrives after the transaction ended, replays the
		// terminal result it already earned - success or refusal.
		if (const NetPayload* cached = FindCached(message.txId, key, nowMs)) {
			Send(connection, *cached);
			return;
		}
		const auto pending = std::find_if(m_Substitutions.begin(), m_Substitutions.end(), [&message](const Substitution& entry) {
			return entry.txId == message.txId;
		});
		if (pending == m_Substitutions.end() || pending->connection != connection ||
		    pending->stableSeat != message.stableSeat || pending->holderGeneration != message.holderGeneration) {
			++m_Stats.unknownTransactionDrops;
			return;
		}
		const size_t index = static_cast<size_t>(pending - m_Substitutions.begin());
		if (!message.stored) {
			// Same rule as a first join: without a durable ticket the substitute could never prove this
			// seat again, so the seat is not given to it.
			++m_Stats.persistenceFailures;
			AbandonSubstitution(index, NetH4DenialReason::ProviderUnavailable, "the reconnect ticket could not be stored", nowMs);
			return;
		}
		NetH4Transcript transcript;
		transcript.domain = NetH4ProofDomain::Substitution;
		transcript.protocolVersion = NetProtocol::c_Version;
		transcript.epoch = m_Registry != nullptr ? m_Registry->GetEpoch() : NetAuthBytes16{};
		transcript.stableSeat = message.stableSeat;
		transcript.holderGeneration = message.holderGeneration;
		transcript.challenge = pending->challenge;
		transcript.clientNonce = message.clientNonce;
		if (m_Registry == nullptr || !m_Registry->IsActive() || !NetH4VerifyProof(pending->credential, transcript, message.mac)) {
			// The record stays: a correct retransmit can still land inside its window, and a broken one
			// simply expires. The refusal is the ordinary uniform one.
			++m_Stats.substitutionAckFailures;
			DenyUniformly(connection, message.txId, NetH4DenialReason::BadProof, nowMs);
			return;
		}
		SeatState* seat = FindSeat(message.stableSeat);
		// The compare-and-swap: anything that moved the seat since the approval - a returner committing,
		// another drop, a leave - invalidates this transaction rather than overwriting the winner.
		if (seat == nullptr || seat->seatGeneration != pending->seatGeneration || !IsSeatSubstitutable(*seat)) {
			AbandonSubstitution(index, NetH4DenialReason::SubstitutionSuperseded, "the seat changed hands before this substitution committed", nowMs);
			return;
		}
		// The outgoing credential is set aside BEFORE the new one is installed, so the player who just
		// lost the seat can still be told why - and only if it proves it held that credential.
		m_Registry->RetireCredentialForSubstitution(message.stableSeat);
		if (!m_Registry->AdoptCredential(message.stableSeat, message.holderGeneration, pending->credential)) {
			m_Registry->ClearRetired(message.stableSeat);
			AbandonSubstitution(index, NetH4DenialReason::ProviderUnavailable, "the host could not install the substitution credential", nowMs);
			return;
		}
		seat->identity = pending->identity;
		seat->holderGeneration = message.holderGeneration;
		seat->committed = true;
		seat->closed = false;
		seat->incarnation = 0;
		seat->saturated = false;
		if (pending->supersededGeneration != 0) {
			seat->retiredGeneration = pending->supersededGeneration;
			seat->retiredUntilMs = nowMs + c_ProvisionalExpiryMs;
		}
		if (!BindIncarnation(*seat, connection)) {
			m_Registry->ClearRetired(message.stableSeat);
			AbandonSubstitution(index, NetH4DenialReason::ProviderUnavailable, "the seat cannot bind another transport", nowMs);
			return;
		}
		const NetH4JoinCommitted committed{c_NetH4Version, message.txId, seat->seat.stableSeat, seat->holderGeneration, seat->incarnation, seat->seat.peerId};
		m_TxCache.Store(message.txId, key, committed, nowMs);
		++m_Stats.substitutionsCommitted;
		m_Commits.push_back({connection, seat->seat.stableSeat, seat->seat.peerId, seat->incarnation, c_InvalidNetPeerId, false, true});
		Send(connection, committed);
		// §8: the substitute receives the ledgered ownership from resumed tick 1, through the same
		// system-authored reseat a returning holder gets.
		IssueReseat(*seat);
		m_Substitutions.erase(m_Substitutions.begin() + static_cast<std::ptrdiff_t>(index));
		DropApplicantsFor(connection);
	}

	bool NetReconnectHost::IsFenced(NetPeerId connection) const {
		return std::any_of(m_Fences.begin(), m_Fences.end(), [connection](const Fence& fence) {
			return fence.connection == connection;
		});
	}

	NetH4DisconnectOutcome NetReconnectHost::NotifyDisconnect(NetPeerId connection, uint64_t frame) {
		const auto fence = std::find_if(m_Fences.begin(), m_Fences.end(), [connection](const Fence& entry) {
			return entry.connection == connection;
		});
		if (fence != m_Fences.end()) {
			// A dead incarnation timing out is not a seat loss.
			m_Fences.erase(fence);
			++m_Stats.fencedDisconnects;
			return NetH4DisconnectOutcome::Fenced;
		}
		m_Admission.DropConnection(connection);
		DropApplicantsFor(connection);
		// A substitute that disappears before its ack has not been given anything, so the approval is
		// invalidated and removed. Unlike a first join there is no resume: the offer named this link.
		for (size_t index = 0; index < m_Substitutions.size();) {
			if (m_Substitutions[index].connection == connection) {
				m_Substitutions[index].connection = c_InvalidNetPeerId;
				AbandonSubstitution(index, NetH4DenialReason::SeatNotSubstitutable, "the substitute disconnected before its ticket was stored", m_NowMs);
				continue;
			}
			++index;
		}
		// A disconnect before the commit leaves no committed seat. The client may already hold the
		// persisted ticket, so the transaction stays resumable for its window - only the link is gone.
		for (Provisional& pending : m_Provisionals) {
			if (pending.connection == connection) {
				pending.connection = c_InvalidNetPeerId;
			}
		}
		m_PendingReclaims.erase(std::remove_if(m_PendingReclaims.begin(), m_PendingReclaims.end(), [connection](const PendingReclaim& pending) {
			return pending.connection == connection;
		}), m_PendingReclaims.end());
		for (SeatState& seat : m_Seats) {
			if (seat.committed && seat.activeConnection == connection) {
				if (!m_LiveMatch) {
					// Nothing has been played, so there is no ownership to hold and no world to come
					// back to; the seat is free for the next joiner.
					ReleaseSeat(seat);
					return NetH4DisconnectOutcome::SeatDropped;
				}
				seat.activeConnection = c_InvalidNetPeerId;
				seat.dropped = true;
				seat.droppedAtMs = m_NowMs;
				seat.holdExpired = false;
				BumpSeatGeneration(seat);
				RecordDrop(seat, frame);
				++m_Stats.seatsDropped;
				return NetH4DisconnectOutcome::SeatDropped;
			}
		}
		return NetH4DisconnectOutcome::Unknown;
	}

	void NetReconnectHost::EndHostedSession() {
		if (m_Registry != nullptr) {
			m_Registry->EndSession();
		}
		m_Admission.Reset();
		m_TxCache.Clear();
		m_Ledger.Clear();
		m_Provisionals.clear();
		m_PendingReclaims.clear();
		m_Applicants.clear();
		for (Substitution& pending : m_Substitutions) {
			pending.credential.fill(0);
		}
		m_Substitutions.clear();
		m_Fences.clear();
		m_PendingReseats.clear();
		m_Commits.clear();
		for (SeatState& seat : m_Seats) {
			seat.holderGeneration = 0;
			seat.incarnation = 0;
			seat.activeConnection = c_InvalidNetPeerId;
			seat.committed = false;
			seat.closed = false;
			seat.saturated = false;
			seat.dropped = false;
			seat.holdExpired = false;
			seat.retiredGeneration = 0;
			seat.retiredUntilMs = 0;
			BumpSeatGeneration(seat);
		}
	}

	void NetReconnectHost::Tick(uint64_t nowMs) {
		m_NowMs = std::max(m_NowMs, nowMs);
		for (auto pending = m_Provisionals.begin(); pending != m_Provisionals.end();) {
			if (nowMs >= pending->openedAtMs && nowMs - pending->openedAtMs > c_ProvisionalExpiryMs) {
				if (m_Registry != nullptr) {
					m_Registry->RevokeSeat(pending->stableSeat);
				}
				++m_Stats.provisionalSeatsExpired;
				pending = m_Provisionals.erase(pending);
				continue;
			}
			if (pending->connection != c_InvalidNetPeerId && pending->retransmits < c_MaxRetransmits &&
			    nowMs >= pending->lastSentMs + c_RetransmitIntervalMs) {
				pending->lastSentMs = nowMs;
				++pending->retransmits;
				++m_Stats.ticketOfferRetransmits;
				Send(pending->connection, pending->offer);
			}
			++pending;
		}
		m_PendingReclaims.erase(std::remove_if(m_PendingReclaims.begin(), m_PendingReclaims.end(), [nowMs](const PendingReclaim& pending) {
			return nowMs >= pending.openedAtMs && nowMs - pending.openedAtMs > NetReconnectAdmission::c_ChallengeLifetimeMs;
		}), m_PendingReclaims.end());
		for (auto applicant = m_Applicants.begin(); applicant != m_Applicants.end();) {
			if (nowMs >= applicant->appliedAtMs && nowMs - applicant->appliedAtMs > c_ProvisionalExpiryMs && !applicant->approved) {
				++m_Stats.applicantsExpired;
				applicant = m_Applicants.erase(applicant);
				continue;
			}
			++applicant;
		}
		for (size_t index = 0; index < m_Substitutions.size();) {
			Substitution& pending = m_Substitutions[index];
			if (nowMs >= pending.openedAtMs && nowMs - pending.openedAtMs > c_ProvisionalExpiryMs) {
				AbandonSubstitution(index, NetH4DenialReason::SeatNotSubstitutable, "the substitution was not acknowledged in time", nowMs);
				continue;
			}
			if (pending.connection != c_InvalidNetPeerId && pending.retransmits < c_MaxRetransmits &&
			    nowMs >= pending.lastSentMs + c_RetransmitIntervalMs) {
				pending.lastSentMs = nowMs;
				++pending.retransmits;
				++m_Stats.substitutionOfferRetransmits;
				Send(pending.connection, pending.offer);
			}
			++index;
		}
		for (SeatState& seat : m_Seats) {
			if (seat.retiredGeneration != 0 && nowMs >= seat.retiredUntilMs) {
				if (m_Registry != nullptr) {
					m_Registry->ClearRetired(seat.seat.stableSeat);
				}
				seat.retiredGeneration = 0;
				seat.retiredUntilMs = 0;
			}
		}
		for (const NetH4Denial& denial : m_Admission.ReleaseDueDenials(nowMs)) {
			++m_Stats.denialsReleased;
			Send(denial.connection, denial.precise ? denial.payload : NetPayload{NetJoinRejected{NetRejectReason::HostNotAccepting, c_DenialText, "", "", ""}});
		}
		// A dropped holder stops being worth waiting for at the same P2 horizon a provisional seat has;
		// the seat stays reclaimable, it just no longer keeps a round alive on its own.
		for (SeatState& seat : m_Seats) {
			if (seat.dropped && !seat.holdExpired && nowMs >= seat.droppedAtMs && nowMs - seat.droppedAtMs > c_ProvisionalExpiryMs) {
				seat.holdExpired = true;
				++m_Stats.seatHoldsExpired;
			}
		}
		m_TxCache.Expire(nowMs);
	}

	bool NetReconnectHost::IsSeatHeldForReclaim(uint8_t lockstepPeerId) const {
		return std::any_of(m_Seats.begin(), m_Seats.end(), [lockstepPeerId](const SeatState& seat) {
			return seat.seat.lockstepPeerId == lockstepPeerId && seat.committed && !seat.closed && !seat.holdExpired;
		});
	}

	bool NetReconnectHost::GetSeatHolder(uint16_t stableSeat, NetPeerId& connection, uint32_t& holderGeneration, uint32_t& incarnation) const {
		const SeatState* seat = FindSeat(stableSeat);
		if (seat == nullptr || !seat->committed) {
			return false;
		}
		connection = seat->activeConnection;
		holderGeneration = seat->holderGeneration;
		incarnation = seat->incarnation;
		return true;
	}

	bool NetReconnectHost::IsSeatClosed(uint16_t stableSeat) const {
		const SeatState* seat = FindSeat(stableSeat);
		return seat != nullptr && seat->closed;
	}

	std::vector<NetH4SeatStatus> NetReconnectHost::GetSeatStatuses() const {
		std::vector<NetH4SeatStatus> statuses;
		statuses.reserve(m_Seats.size());
		for (const SeatState& seat : m_Seats) {
			NetH4SeatStatus status;
			status.stableSeat = seat.seat.stableSeat;
			status.lockstepPeerId = seat.seat.lockstepPeerId;
			status.committed = seat.committed;
			status.closed = seat.closed;
			status.dropped = seat.committed && seat.activeConnection == c_InvalidNetPeerId;
			status.reclaiming = std::any_of(m_PendingReclaims.begin(), m_PendingReclaims.end(), [&seat](const PendingReclaim& pending) {
				return pending.stableSeat == seat.seat.stableSeat;
			});
			status.substituting = std::any_of(m_Substitutions.begin(), m_Substitutions.end(), [&seat](const Substitution& pending) {
				return pending.stableSeat == seat.seat.stableSeat;
			});
			status.applicants = static_cast<uint16_t>(std::count_if(m_Applicants.begin(), m_Applicants.end(), [&seat](const Applicant& applicant) {
				return applicant.stableSeat == seat.seat.stableSeat;
			}));
			statuses.push_back(status);
		}
		return statuses;
	}

	void NetReconnectClient::Configure(NetReconnectTicketStore* store, NetH4Identity identity, std::string displayName) {
		m_Store = store;
		m_Identity = std::move(identity);
		m_DisplayName = std::move(displayName);
	}

	void NetReconnectClient::SetUnixClock(uint64_t (*clock)(void*), void* context) {
		m_UnixClock = clock;
		m_UnixClockContext = context;
	}

	uint64_t NetReconnectClient::UnixNowMs() const {
		return m_UnixClock != nullptr ? m_UnixClock(m_UnixClockContext) : 0;
	}

	std::vector<NetH4Outbound> NetReconnectClient::TakeOutbound() {
		std::vector<NetH4Outbound> taken = std::move(m_Outbound);
		m_Outbound.clear();
		return taken;
	}

	void NetReconnectClient::Fail(std::string error) {
		m_State = NetH4ClientState::Failed;
		m_HasPendingRequest = false;
		m_Error = std::move(error);
	}

	void NetReconnectClient::SendRequest(NetPayload payload, uint64_t nowMs) {
		m_PendingRequest = std::move(payload);
		m_HasPendingRequest = true;
		m_RequestSentMs = nowMs;
		m_RequestOpenedMs = nowMs;
		m_Retransmits = 0;
		++m_Stats.requestsSent;
		m_Outbound.push_back({c_InvalidNetPeerId, m_PendingRequest});
	}

	void NetReconnectClient::Resend(uint64_t nowMs) {
		m_RequestSentMs = nowMs;
		++m_Retransmits;
		++m_Stats.retransmits;
		m_Outbound.push_back({c_InvalidNetPeerId, m_PendingRequest});
	}

	const char* NetReconnectClientStateName(NetH4ClientState state) {
		switch (state) {
			case NetH4ClientState::Idle: return "Idle";
			case NetH4ClientState::Joining: return "Joining";
			case NetH4ClientState::Storing: return "Storing";
			case NetH4ClientState::Reclaiming: return "Reclaiming";
			case NetH4ClientState::Proving: return "Proving";
			case NetH4ClientState::Joined: return "Joined";
			case NetH4ClientState::Leaving: return "Leaving";
			case NetH4ClientState::Left: return "Left";
			case NetH4ClientState::Denied: return "Denied";
			case NetH4ClientState::Failed: return "Failed";
			case NetH4ClientState::Applying: return "Applying";
			case NetH4ClientState::Applied: return "Applied";
			case NetH4ClientState::Substituting: return "Substituting";
		}
		return "Unknown";
	}

	void NetReconnectClient::SetHostContext(std::string hostAddress, const NetHash32& matchConfigHash) {
		m_HostAddress = std::move(hostAddress);
		m_MatchConfigHash = matchConfigHash;
		m_Record.hostAddress = m_HostAddress;
		m_Record.matchConfigHash = m_MatchConfigHash;
	}

	bool NetReconnectClient::IsAdmissionPending() const {
		// Applied waits for a human, which is why it is bounded by the same P2 window everything else
		// on this plane is, rather than by the handshake ladder.
		return m_State == NetH4ClientState::Joining || m_State == NetH4ClientState::Storing ||
		       m_State == NetH4ClientState::Reclaiming || m_State == NetH4ClientState::Proving ||
		       m_State == NetH4ClientState::Applying || m_State == NetH4ClientState::Applied ||
		       m_State == NetH4ClientState::Substituting;
	}

	void NetReconnectClient::SetApplyForSeat(bool enabled, uint16_t stableSeat) {
		m_ApplyForSeat = enabled;
		m_ApplySeat = stableSeat;
	}

	bool NetReconnectClient::BeginAdmission(uint64_t nowMs, std::string* error) {
		if (m_Store == nullptr) {
			if (error) *error = "no ticket store";
			return false;
		}
		m_UsedStoredTicket = false;
		m_FellBackToNewJoin = false;
		if (m_ApplyForSeat) {
			// A live match refuses an ordinary join, so a player the host has to approve asks instead.
			return BeginApplication(m_ApplySeat, nowMs, error);
		}
		NetH4TicketRecord record;
		m_LastLoad = m_Store->Load(UnixNowMs(), record, nullptr);
		// A record for a different host names a different session's seat; only this host's reclaims.
		if (m_LastLoad == NetH4TicketLoadResult::Loaded && record.hostAddress == m_HostAddress) {
			m_UsedStoredTicket = true;
			record.matchConfigHash = m_MatchConfigHash;
			return BeginReclaim(record, nowMs, error);
		}
		return BeginNewJoin(nowMs, error);
	}

	bool NetReconnectClient::AbsorbRejection(uint64_t nowMs, NetRejectReason reason) {
		m_LastRejectReason = reason;
		m_HasLastRejectReason = true;
		if (reason == NetRejectReason::SeatReassigned) {
			// The host gave this seat away. There is nothing left to retry, and a fresh join into a
			// live match would only be refused again.
			return false;
		}
		if (!m_UsedStoredTicket || m_FellBackToNewJoin || m_State == NetH4ClientState::Joined) {
			return false;
		}
		// The stored ticket named a hosted session that is gone (or a seat this host no longer knows).
		// A fresh join is what a ticketless client would have sent, so try it once and let the seat
		// protection decide; a second refusal is a real refusal.
		m_FellBackToNewJoin = true;
		m_HasRecord = false;
		m_Record = {};
		m_Record.hostAddress = m_HostAddress;
		m_Record.matchConfigHash = m_MatchConfigHash;
		if (!BeginNewJoin(nowMs, nullptr)) {
			return false;
		}
		m_Error = "the stored reconnect ticket was refused; joining as a new player";
		return true;
	}

	bool NetReconnectClient::BeginNewJoin(uint64_t nowMs, std::string* error) {
		if (m_Store == nullptr) {
			Fail("no ticket store");
			if (error) *error = m_Error;
			return false;
		}
		if (!NetH4DrawTxId(m_TxId)) {
			Fail("no crypto provider to draw a transaction id");
			if (error) *error = m_Error;
			return false;
		}
		m_State = NetH4ClientState::Joining;
		m_Error.clear();
		m_WantsLinkClosed = false;
		SendRequest(NetH4NewJoin{c_NetH4Version, m_TxId, m_Identity, m_DisplayName}, nowMs);
		return true;
	}

	bool NetReconnectClient::BeginReclaim(const NetH4TicketRecord& record, uint64_t nowMs, std::string* error) {
		if (!NetH4DrawTxId(m_TxId)) {
			Fail("no crypto provider to draw a transaction id");
			if (error) *error = m_Error;
			return false;
		}
		m_Record = record;
		m_HasRecord = true;
		m_State = NetH4ClientState::Reclaiming;
		m_Error.clear();
		m_WantsLinkClosed = false;
		SendRequest(NetH4Reclaim{c_NetH4Version, m_TxId, record.epoch, record.stableSeat, record.holderGeneration, m_Identity, m_DisplayName}, nowMs);
		return true;
	}

	bool NetReconnectClient::BeginApplication(uint16_t stableSeat, uint64_t nowMs, std::string* error) {
		if (!NetH4DrawTxId(m_TxId)) {
			Fail("no crypto provider to draw a transaction id");
			if (error) *error = m_Error;
			return false;
		}
		m_State = NetH4ClientState::Applying;
		m_Error.clear();
		m_WantsLinkClosed = false;
		m_AppliedAtMs = nowMs;
		m_ApplySeat = stableSeat;
		++m_Stats.applicationsSent;
		SendRequest(NetH4Applicant{c_NetH4Version, m_TxId, stableSeat, m_Identity, m_DisplayName}, nowMs);
		return true;
	}

	bool NetReconnectClient::BeginLeave(uint64_t nowMs, std::string* error) {
		if (!m_HasRecord) {
			if (error) *error = "no seat to leave";
			return false;
		}
		if (!NetH4DrawTxId(m_TxId)) {
			Fail("no crypto provider to draw a transaction id");
			if (error) *error = m_Error;
			return false;
		}
		m_State = NetH4ClientState::Leaving;
		m_Error.clear();
		m_WantsLinkClosed = false;
		SendRequest(NetH4LeaveRequest{c_NetH4Version, m_TxId, m_Record.epoch, m_Record.stableSeat, m_Record.holderGeneration}, nowMs);
		return true;
	}

	bool NetReconnectClient::HandleMessage(const NetPayload& payload, uint64_t nowMs) {
		if (const auto* offer = std::get_if<NetH4TicketOffer>(&payload)) {
			if (m_State != NetH4ClientState::Joining && m_State != NetH4ClientState::Storing) {
				return true;
			}
			NetH4TicketRecord record;
			record.recordVersion = NetReconnectTicketStore::c_RecordVersion;
			record.epoch = offer->epoch;
			record.stableSeat = offer->stableSeat;
			record.holderGeneration = offer->holderGeneration;
			record.credential = offer->credential;
			record.hostSessionId = offer->hostSessionId;
			record.hostAddress = m_Record.hostAddress;
			record.issuedAtUnixMs = UnixNowMs();
			record.matchConfigHash = m_Record.matchConfigHash;
			std::string storeError;
			// The ack must never be sent before the record is durable: the host commits the seat on it.
			const bool stored = m_Store != nullptr && m_Store->Store(record, &storeError);
			if (stored) {
				m_Record = record;
				m_HasRecord = true;
				m_State = NetH4ClientState::Storing;
				++m_Stats.ticketsStored;
			} else {
				++m_Stats.ticketStoreFailures;
				m_Error = storeError.empty() ? "no ticket store" : storeError;
			}
			m_HasPendingRequest = false;
			m_Outbound.push_back({c_InvalidNetPeerId, NetH4TicketStoredAck{c_NetH4Version, offer->txId, offer->stableSeat, offer->holderGeneration, stored}});
			if (!stored) {
				m_State = NetH4ClientState::Failed;
			} else {
				m_PendingRequest = NetH4TicketStoredAck{c_NetH4Version, offer->txId, offer->stableSeat, offer->holderGeneration, true};
				m_HasPendingRequest = true;
				m_RequestSentMs = nowMs;
				m_RequestOpenedMs = nowMs;
				m_Retransmits = 0;
			}
			return true;
		}
		if (const auto* challenge = std::get_if<NetH4Challenge>(&payload)) {
			if (m_State != NetH4ClientState::Reclaiming || !m_HasRecord || !(challenge->txId == m_TxId)) {
				return true;
			}
			NetAuthBytes16 nonce{};
			if (!NetH4DrawNonce(nonce)) {
				Fail("no crypto provider to answer the challenge");
				return true;
			}
			NetH4Transcript transcript;
			transcript.domain = NetH4ProofDomain::Reclaim;
			transcript.protocolVersion = NetProtocol::c_Version;
			transcript.epoch = m_Record.epoch;
			transcript.stableSeat = m_Record.stableSeat;
			transcript.holderGeneration = m_Record.holderGeneration;
			transcript.challenge = challenge->challenge;
			transcript.clientNonce = nonce;
			NetAuthBytes32 mac{};
			if (!NetH4ComputeProof(m_Record.credential, transcript, mac)) {
				Fail("no crypto provider to prove the ticket");
				return true;
			}
			m_State = NetH4ClientState::Proving;
			++m_Stats.proofsSent;
			SendRequest(NetH4Proof{c_NetH4Version, m_TxId, m_Record.epoch, m_Record.stableSeat, m_Record.holderGeneration, nonce, mac}, nowMs);
			return true;
		}
		if (const auto* ack = std::get_if<NetH4ApplicantAck>(&payload)) {
			if (m_State != NetH4ClientState::Applying && m_State != NetH4ClientState::Applied) {
				return true;
			}
			// On the list. Nothing else happens until a human decides, so the ladder stops here and the
			// wait is bounded by the record's own lifetime rather than by retransmits.
			m_HasPendingRequest = false;
			m_State = NetH4ClientState::Applied;
			m_AppliedAtMs = nowMs;
			++m_Stats.applicationsAcknowledged;
			return true;
		}
		if (const auto* offer = std::get_if<NetH4SubstitutionOffer>(&payload)) {
			if (m_State != NetH4ClientState::Applying && m_State != NetH4ClientState::Applied && m_State != NetH4ClientState::Substituting) {
				return true;
			}
			++m_Stats.substitutionOffersReceived;
			NetH4TicketRecord record;
			record.recordVersion = NetReconnectTicketStore::c_RecordVersion;
			record.epoch = offer->epoch;
			record.stableSeat = offer->stableSeat;
			record.holderGeneration = offer->holderGeneration;
			record.credential = offer->credential;
			record.hostSessionId = offer->hostSessionId;
			record.hostAddress = m_Record.hostAddress;
			record.issuedAtUnixMs = UnixNowMs();
			record.matchConfigHash = m_Record.matchConfigHash;
			std::string storeError;
			const bool stored = m_Store != nullptr && m_Store->Store(record, &storeError);
			NetAuthBytes16 nonce{};
			NetAuthBytes32 mac{};
			bool proved = false;
			if (stored) {
				m_Record = record;
				m_HasRecord = true;
				// The ack is not just "written": the mac proves this connection holds the credential it
				// just persisted, so no other connection can commit the seat in its place.
				NetH4Transcript transcript;
				transcript.domain = NetH4ProofDomain::Substitution;
				transcript.protocolVersion = NetProtocol::c_Version;
				transcript.epoch = offer->epoch;
				transcript.stableSeat = offer->stableSeat;
				transcript.holderGeneration = offer->holderGeneration;
				transcript.challenge = offer->challenge;
				if (NetH4DrawNonce(nonce)) {
					transcript.clientNonce = nonce;
					proved = NetH4ComputeProof(record.credential, transcript, mac);
				}
				++m_Stats.ticketsStored;
			} else {
				++m_Stats.ticketStoreFailures;
				m_Error = storeError.empty() ? "no ticket store" : storeError;
			}
			const NetH4SubstitutionAck ack{c_NetH4Version, offer->txId, offer->stableSeat, offer->holderGeneration, nonce, mac, stored && proved};
			m_HasPendingRequest = false;
			++m_Stats.substitutionAcksSent;
			m_Outbound.push_back({c_InvalidNetPeerId, ack});
			if (!stored || !proved) {
				m_State = NetH4ClientState::Failed;
				if (m_Error.empty()) {
					m_Error = "no crypto provider to prove the substitution ticket";
				}
			} else {
				m_State = NetH4ClientState::Substituting;
				m_PendingRequest = ack;
				m_HasPendingRequest = true;
				m_RequestSentMs = nowMs;
				m_RequestOpenedMs = nowMs;
				m_Retransmits = 0;
			}
			return true;
		}
		if (const auto* committed = std::get_if<NetH4JoinCommitted>(&payload)) {
			// A commit for some other seat is not ours, whatever transaction it names. The txId itself
			// cannot gate this: a relaunched client resumes its stored ticket under a fresh one.
			if (m_HasRecord && (committed->stableSeat != m_Record.stableSeat || committed->holderGeneration != m_Record.holderGeneration)) {
				return true;
			}
			m_HasPendingRequest = false;
			m_Incarnation = committed->incarnation;
			m_AssignedPeerId = committed->assignedPeerId;
			m_State = NetH4ClientState::Joined;
			++m_Stats.commitsReceived;
			return true;
		}
		if (const auto* ack = std::get_if<NetH4LeaveAck>(&payload)) {
			if (m_State != NetH4ClientState::Leaving && m_State != NetH4ClientState::Left) {
				return true;
			}
			m_HasPendingRequest = false;
			++m_Stats.leaveAcksReceived;
			if (ack->seatClosed && m_Store != nullptr) {
				m_Store->Clear();
				++m_Stats.ticketsCleared;
			}
			m_HasRecord = false;
			m_Record = {};
			m_State = NetH4ClientState::Left;
			return true;
		}
		if (std::holds_alternative<NetH4NewJoin>(payload) || std::holds_alternative<NetH4TicketStoredAck>(payload) ||
		    std::holds_alternative<NetH4Reclaim>(payload) || std::holds_alternative<NetH4Proof>(payload) ||
		    std::holds_alternative<NetH4LeaveRequest>(payload) || std::holds_alternative<NetH4Applicant>(payload) ||
		    std::holds_alternative<NetH4SubstitutionAck>(payload)) {
			// The client's own requests; a host that echoes one is talking out of turn.
			return true;
		}
		return false;
	}

	void NetReconnectClient::Tick(uint64_t nowMs) {
		if (m_State == NetH4ClientState::Applied) {
			// The host's own record of this application expires at P2; past that nobody is coming.
			if (nowMs >= m_AppliedAtMs && nowMs - m_AppliedAtMs > NetReconnectHost::c_ProvisionalExpiryMs) {
				m_State = NetH4ClientState::Denied;
				m_Error = "the host did not act on this application";
			}
			return;
		}
		if (!m_HasPendingRequest) {
			return;
		}
		if (m_State == NetH4ClientState::Leaving && nowMs >= m_RequestOpenedMs && nowMs - m_RequestOpenedMs >= c_LeaveAckBudgetMs) {
			// Unacknowledged: an ambiguous loss, so the record stays and the link closes.
			m_HasPendingRequest = false;
			m_WantsLinkClosed = true;
			++m_Stats.unacknowledgedLeaves;
			return;
		}
		if (m_Retransmits >= NetReconnectHost::c_MaxRetransmits) {
			m_HasPendingRequest = false;
			if (m_State != NetH4ClientState::Joined && m_State != NetH4ClientState::Left) {
				m_State = NetH4ClientState::Denied;
			}
			return;
		}
		if (nowMs >= m_RequestSentMs + NetReconnectHost::c_RetransmitIntervalMs) {
			Resend(nowMs);
		}
	}

	void NetReconnectClient::NotifyConfirmedSessionEnd() {
		++m_Stats.confirmedSessionEnds;
		m_HasPendingRequest = false;
		if (m_Store != nullptr && m_Store->HasRecord()) {
			m_Store->Clear();
			++m_Stats.ticketsCleared;
		}
		m_HasRecord = false;
		m_Record = {};
		m_State = NetH4ClientState::Left;
	}

	void NetReconnectClient::NotifyAmbiguousLoss() {
		// Exactly the case the record exists for: keep it, so the next launch can reclaim.
		++m_Stats.ambiguousLosses;
		m_HasPendingRequest = false;
		if (m_State == NetH4ClientState::Joined || m_State == NetH4ClientState::Storing) {
			m_State = NetH4ClientState::Idle;
		}
	}

} // namespace RTE
