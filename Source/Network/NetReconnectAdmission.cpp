#include "NetReconnectAdmission.h"

#include "NetReconnectTranscript.h"

#include <algorithm>

namespace RTE {

	NetReconnectAdmission::ConnectionState& NetReconnectAdmission::StateFor(NetPeerId connection) {
		const auto existing = std::find_if(m_Connections.begin(), m_Connections.end(), [connection](const ConnectionState& state) {
			return state.connection == connection;
		});
		if (existing != m_Connections.end()) {
			return *existing;
		}
		ConnectionState state;
		state.connection = connection;
		m_Connections.push_back(state);
		return m_Connections.back();
	}

	void NetReconnectAdmission::ExpireChallenges(uint64_t nowMs) {
		const auto expired = std::remove_if(m_Challenges.begin(), m_Challenges.end(), [nowMs](const NetH4ChallengeRecord& record) {
			return nowMs >= record.issuedAtMs && nowMs - record.issuedAtMs > c_ChallengeLifetimeMs;
		});
		m_ExpiredChallenges += static_cast<uint32_t>(std::distance(expired, m_Challenges.end()));
		m_Challenges.erase(expired, m_Challenges.end());
	}

	size_t NetReconnectAdmission::ChallengeCountFor(NetPeerId connection) const {
		return static_cast<size_t>(std::count_if(m_Challenges.begin(), m_Challenges.end(), [connection](const NetH4ChallengeRecord& record) {
			return record.connection == connection;
		}));
	}

	bool NetReconnectAdmission::BeginAttempt(NetPeerId connection, uint64_t nowMs) {
		if (!m_IntervalOpen || nowMs < m_IntervalStartMs || nowMs - m_IntervalStartMs >= c_AttemptIntervalMs) {
			m_IntervalStartMs = nowMs;
			m_IntervalAttempts = 0;
			m_IntervalOpen = true;
		}
		ConnectionState& state = StateFor(connection);
		if (state.hasAttempted && nowMs >= state.lastAttemptMs && nowMs - state.lastAttemptMs < c_AttemptIntervalMs) {
			++m_RateLimitedAttempts;
			return false;
		}
		if (m_IntervalAttempts >= c_MaxAttemptsPerInterval) {
			++m_RateLimitedAttempts;
			return false;
		}
		state.lastAttemptMs = nowMs;
		state.hasAttempted = true;
		++m_IntervalAttempts;
		return true;
	}

	bool NetReconnectAdmission::IssueChallenge(NetPeerId connection, const NetAuthBytes16& txId, uint16_t stableSeat, uint32_t holderGeneration, uint64_t nowMs, NetAuthBytes32& challenge) {
		ExpireChallenges(nowMs);
		if (m_Challenges.size() >= c_MaxChallengesGlobal || ChallengeCountFor(connection) >= c_MaxChallengesPerConnection) {
			++m_RefusedChallenges;
			return false;
		}
		NetAuthBytes32 drawn{};
		if (!NetH4DrawChallenge(drawn)) {
			++m_RefusedChallenges;
			return false;
		}
		NetH4ChallengeRecord record;
		record.connection = connection;
		record.txId = txId;
		record.challenge = drawn;
		record.stableSeat = stableSeat;
		record.holderGeneration = holderGeneration;
		record.issuedAtMs = nowMs;
		m_Challenges.push_back(record);
		challenge = drawn;
		++m_IssuedChallenges;
		return true;
	}

	bool NetReconnectAdmission::IssueSyntheticChallenge(NetPeerId connection, uint64_t nowMs, NetAuthBytes32& challenge) {
		ExpireChallenges(nowMs);
		(void)connection;
		NetAuthBytes32 drawn{};
		if (!NetH4DrawChallenge(drawn)) {
			++m_RefusedChallenges;
			return false;
		}
		challenge = drawn;
		++m_SyntheticChallenges;
		return true;
	}

	bool NetReconnectAdmission::ConsumeChallenge(NetPeerId connection, const NetAuthBytes16& txId, uint64_t nowMs, NetH4ChallengeRecord& out) {
		ExpireChallenges(nowMs);
		const auto found = std::find_if(m_Challenges.begin(), m_Challenges.end(), [connection, &txId](const NetH4ChallengeRecord& record) {
			return record.connection == connection && record.txId == txId;
		});
		if (found == m_Challenges.end()) {
			return false;
		}
		out = *found;
		m_Challenges.erase(found);
		return true;
	}

	void NetReconnectAdmission::ScheduleDenial(NetPeerId connection, const NetAuthBytes16& txId, NetH4DenialReason reason, uint64_t nowMs) {
		const auto pending = std::find_if(m_Denials.begin(), m_Denials.end(), [connection](const NetH4Denial& denial) {
			return denial.connection == connection;
		});
		if (pending != m_Denials.end()) {
			++m_CoalescedDenials;
			return;
		}
		NetH4Denial denial;
		denial.connection = connection;
		denial.txId = txId;
		denial.reason = reason;
		denial.issuedAtMs = nowMs;
		denial.releaseAtMs = nowMs + c_DenialReleaseMs;
		m_Denials.push_back(denial);
	}

	std::vector<NetH4Denial> NetReconnectAdmission::ReleaseDueDenials(uint64_t nowMs) {
		std::vector<NetH4Denial> due;
		const auto released = std::stable_partition(m_Denials.begin(), m_Denials.end(), [nowMs](const NetH4Denial& denial) {
			return nowMs < denial.releaseAtMs;
		});
		due.assign(released, m_Denials.end());
		m_Denials.erase(released, m_Denials.end());
		return due;
	}

	void NetReconnectAdmission::DropConnection(NetPeerId connection) {
		m_Challenges.erase(std::remove_if(m_Challenges.begin(), m_Challenges.end(), [connection](const NetH4ChallengeRecord& record) {
			return record.connection == connection;
		}), m_Challenges.end());
		m_Denials.erase(std::remove_if(m_Denials.begin(), m_Denials.end(), [connection](const NetH4Denial& denial) {
			return denial.connection == connection;
		}), m_Denials.end());
		m_Connections.erase(std::remove_if(m_Connections.begin(), m_Connections.end(), [connection](const ConnectionState& state) {
			return state.connection == connection;
		}), m_Connections.end());
	}

	void NetReconnectAdmission::Reset() {
		m_Challenges.clear();
		m_Denials.clear();
		m_Connections.clear();
		m_IntervalStartMs = 0;
		m_IntervalAttempts = 0;
		m_IntervalOpen = false;
	}

} // namespace RTE
