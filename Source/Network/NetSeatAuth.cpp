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

	uint32_t NetSeatAuthRegistry::PeekNextGeneration(uint16_t seat) const {
		if (!m_Active) {
			return 0;
		}
		const auto entry = m_Seats.find(seat);
		const uint32_t last = entry == m_Seats.end() ? 0U : entry->second.lastGeneration;
		return last == UINT32_MAX ? 0U : last + 1U;
	}

	bool NetSeatAuthRegistry::AdoptCredential(uint16_t seat, uint32_t holderGeneration, const NetSeatCredential& credential) {
		if (!m_Active || holderGeneration == 0 || holderGeneration != PeekNextGeneration(seat)) {
			return false;
		}
		SeatEntry& entry = m_Seats[seat];
		entry.lastGeneration = holderGeneration;
		entry.active = true;
		entry.credential = credential;
		return true;
	}

	void NetSeatAuthRegistry::RetireCredentialForSubstitution(uint16_t seat) {
		const auto entry = m_Seats.find(seat);
		if (entry == m_Seats.end() || !entry->second.active) {
			return;
		}
		entry->second.retiredGeneration = entry->second.lastGeneration;
		entry->second.retiredCredential = entry->second.credential;
		entry->second.hasRetired = true;
	}

	bool NetSeatAuthRegistry::HasRetiredGeneration(uint16_t seat, uint32_t holderGeneration) const {
		if (!m_Active || holderGeneration == 0) {
			return false;
		}
		const auto entry = m_Seats.find(seat);
		return entry != m_Seats.end() && entry->second.hasRetired && entry->second.retiredGeneration == holderGeneration;
	}

	bool NetSeatAuthRegistry::VerifyRetiredProof(uint16_t seat, uint32_t holderGeneration, const NetH4Transcript& transcript, const NetAuthBytes32& mac) const {
		if (!HasRetiredGeneration(seat, holderGeneration)) {
			return false;
		}
		return NetH4VerifyProof(m_Seats.find(seat)->second.retiredCredential, transcript, mac);
	}

	void NetSeatAuthRegistry::ClearRetired(uint16_t seat) {
		const auto entry = m_Seats.find(seat);
		if (entry != m_Seats.end()) {
			entry->second.hasRetired = false;
			entry->second.retiredGeneration = 0;
			entry->second.retiredCredential.fill(0);
		}
	}

	void NetSeatAuthRegistry::RevokeSeat(uint16_t seat) {
		const auto entry = m_Seats.find(seat);
		if (entry != m_Seats.end()) {
			entry->second.active = false;
			entry->second.credential.fill(0);
			entry->second.hasRetired = false;
			entry->second.retiredGeneration = 0;
			entry->second.retiredCredential.fill(0);
		}
	}

} // namespace RTE
