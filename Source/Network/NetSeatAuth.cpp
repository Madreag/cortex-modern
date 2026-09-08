#include "NetSeatAuth.h"

#include "NetAuthCrypto.h"

namespace RTE {

	bool NetSeatAuthRegistry::BeginHostedSession() {
		EndSession();
		NetAuthEpoch epoch{};
		if (!GetNetAuthCrypto().RandomBytes(epoch.data(), epoch.size())) {
			return false;
		}
		m_Epoch = epoch;
		m_Active = true;
		return true;
	}

	void NetSeatAuthRegistry::EndSession() {
		m_Active = false;
		m_Epoch.fill(0);
		m_Seats.clear();
	}

	bool NetSeatAuthRegistry::IssueCredential(uint16_t seat, uint32_t& holderGeneration, NetSeatCredential& credential) {
		if (!m_Active) {
			return false;
		}
		// Draw before mutating so an RNG failure leaves the seat's prior state intact.
		NetSeatCredential fresh{};
		if (!GetNetAuthCrypto().RandomBytes(fresh.data(), fresh.size())) {
			return false;
		}
		SeatEntry& entry = m_Seats[seat];
		++entry.lastGeneration;
		entry.active = true;
		entry.credential = fresh;
		holderGeneration = entry.lastGeneration;
		credential = fresh;
		return true;
	}

	bool NetSeatAuthRegistry::MatchesActiveCredential(uint16_t seat, uint32_t holderGeneration, const NetSeatCredential& credential) const {
		if (!m_Active || holderGeneration == 0) {
			return false;
		}
		const auto entry = m_Seats.find(seat);
		if (entry == m_Seats.end() || !entry->second.active || entry->second.lastGeneration != holderGeneration) {
			return false;
		}
		return NetAuthConstantTimeEquals(entry->second.credential.data(), credential.data(), credential.size());
	}

	bool NetSeatAuthRegistry::VerifySeatProof(uint16_t seat, uint32_t holderGeneration, const NetH4Transcript& transcript, const NetAuthBytes32& mac) const {
		if (!m_Active || holderGeneration == 0) {
			return false;
		}
		const auto entry = m_Seats.find(seat);
		if (entry == m_Seats.end() || !entry->second.active || entry->second.lastGeneration != holderGeneration) {
			return false;
		}
		return NetH4VerifyProof(entry->second.credential, transcript, mac);
	}

	uint32_t NetSeatAuthRegistry::GetActiveGeneration(uint16_t seat) const {
		if (!m_Active) {
			return 0;
		}
		const auto entry = m_Seats.find(seat);
		return (entry != m_Seats.end() && entry->second.active) ? entry->second.lastGeneration : 0;
	}

	void NetSeatAuthRegistry::RevokeSeat(uint16_t seat) {
		const auto entry = m_Seats.find(seat);
		if (entry != m_Seats.end()) {
			entry->second.active = false;
			entry->second.credential.fill(0);
		}
	}

} // namespace RTE
