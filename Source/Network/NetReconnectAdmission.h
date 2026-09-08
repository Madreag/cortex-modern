#pragma once

#include "NetProtocol.h"
#include "NetTransport.h"

#include <cstdint>
#include <vector>

namespace RTE {

	/// Why an admission attempt was refused. Recorded for diagnostics only - every reason takes the
	/// same path and the same delay, so the wire cannot tell them apart.
	enum class NetH4DenialReason : uint8_t {
		UnknownSeat = 0,
		StaleEpoch = 1,
		BadProof = 2,
		ExpiredChallenge = 3,
		RateLimited = 4,
		ProviderUnavailable = 5,
	};

	struct NetH4Denial {
		NetPeerId connection = c_InvalidNetPeerId;
		NetAuthBytes16 txId{};
		NetH4DenialReason reason = NetH4DenialReason::UnknownSeat;
		uint64_t issuedAtMs = 0;
		uint64_t releaseAtMs = 0;
	};

	struct NetH4ChallengeRecord {
		NetPeerId connection = c_InvalidNetPeerId;
		NetAuthBytes16 txId{};
		NetAuthBytes32 challenge{};
		uint16_t stableSeat = 0;
		uint32_t holderGeneration = 0;
		uint64_t issuedAtMs = 0;
	};

	/// The host's admission gate: it rate-limits reclaim attempts, issues the challenges they answer,
	/// and releases every denial at one fixed delay. An unknown seat gets a synthetic challenge that
	/// stores nothing, so it looks and times exactly like a known seat answering badly, and unknown-seat
	/// spam cannot starve a real reclaimer of a challenge slot.
	///
	/// Everything is clocked by the caller's nowMs; nothing here ever sleeps.
	class NetReconnectAdmission {
	public:
		static constexpr uint64_t c_ChallengeLifetimeMs = 10000;
		static constexpr size_t c_MaxChallengesPerConnection = 2;
		static constexpr size_t c_MaxChallengesGlobal = 8;
		static constexpr uint64_t c_AttemptIntervalMs = 1000;
		static constexpr uint32_t c_MaxAttemptsPerInterval = 4;
		static constexpr uint64_t c_DenialReleaseMs = 1000;

		/// Opens an admission attempt on the connection.
		/// @return Whether the attempt may proceed; a refusal is rate limiting, and the caller still
		/// schedules the same denial it would for a bad proof.
		bool BeginAttempt(NetPeerId connection, uint64_t nowMs);

		/// Issues a challenge bound to this connection and transaction for a seat that exists.
		/// @return Whether a challenge was issued and stored; false when the pool is full or the
		/// provider cannot draw, both of which fail closed.
		bool IssueChallenge(NetPeerId connection, const NetAuthBytes16& txId, uint16_t stableSeat, uint32_t holderGeneration, uint64_t nowMs, NetAuthBytes32& challenge);

		/// Issues a synthetic challenge for an unknown seat or a stale epoch. Nothing is retained, so
		/// this draws no slot from the bounded pool.
		bool IssueSyntheticChallenge(NetPeerId connection, uint64_t nowMs, NetAuthBytes32& challenge);

		/// Consumes the connection's challenge for the transaction. The challenge is spent whether or
		/// not the proof that follows verifies, so a replay reads as an expired challenge.
		/// @return Whether a live challenge was found and removed.
		bool ConsumeChallenge(NetPeerId connection, const NetAuthBytes16& txId, uint64_t nowMs, NetH4ChallengeRecord& out);

		/// Schedules the uniform denial. One pending denial per connection: a second refusal inside the
		/// window joins the first rather than adding a reply.
		void ScheduleDenial(NetPeerId connection, const NetAuthBytes16& txId, NetH4DenialReason reason, uint64_t nowMs);

		/// Collects the denials whose release time has arrived, oldest first.
		std::vector<NetH4Denial> ReleaseDueDenials(uint64_t nowMs);

		/// Drops the connection's challenges, its pending denial and its rate-limit state.
		void DropConnection(NetPeerId connection);

		void Reset();

		size_t GetOutstandingChallengeCount() const { return m_Challenges.size(); }
		size_t GetPendingDenialCount() const { return m_Denials.size(); }
		uint32_t GetIssuedChallenges() const { return m_IssuedChallenges; }
		uint32_t GetSyntheticChallenges() const { return m_SyntheticChallenges; }
		uint32_t GetRefusedChallenges() const { return m_RefusedChallenges; }
		uint32_t GetRateLimitedAttempts() const { return m_RateLimitedAttempts; }
		uint32_t GetCoalescedDenials() const { return m_CoalescedDenials; }
		uint32_t GetExpiredChallenges() const { return m_ExpiredChallenges; }

	private:
		struct ConnectionState {
			NetPeerId connection = c_InvalidNetPeerId;
			uint64_t lastAttemptMs = 0;
			bool hasAttempted = false;
		};

		void ExpireChallenges(uint64_t nowMs);
		size_t ChallengeCountFor(NetPeerId connection) const;
		ConnectionState& StateFor(NetPeerId connection);

		std::vector<NetH4ChallengeRecord> m_Challenges;
		std::vector<NetH4Denial> m_Denials;
		std::vector<ConnectionState> m_Connections;
		uint64_t m_IntervalStartMs = 0;
		uint32_t m_IntervalAttempts = 0;
		bool m_IntervalOpen = false;
		uint32_t m_IssuedChallenges = 0;
		uint32_t m_SyntheticChallenges = 0;
		uint32_t m_RefusedChallenges = 0;
		uint32_t m_RateLimitedAttempts = 0;
		uint32_t m_CoalescedDenials = 0;
		uint32_t m_ExpiredChallenges = 0;
	};

} // namespace RTE
