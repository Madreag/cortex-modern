#pragma once

#include "NetReconnectTranscript.h"

#include <array>
#include <cstdint>
#include <unordered_map>

namespace RTE {

	using NetAuthEpoch = std::array<uint8_t, 16>;
	using NetSeatCredential = std::array<uint8_t, 32>;

	/// Host-side registry of the match-scoped reconnect-auth material: one off-sim CSPRNG epoch
	/// per hosted session and one credential per seat holder generation. Fails closed: without
	/// real crypto nothing is issued and automatic reclaim stays disabled.
	class NetSeatAuthRegistry {
	public:
		/// Arms the registry for a newly hosted session by drawing a fresh epoch.
		/// @return Whether the epoch was drawn; false leaves the registry inactive.
		bool BeginHostedSession();

		/// Clears the epoch and every credential at true session end.
		void EndSession();

		/// Whether an epoch is armed; the epoch survives resync, rejoin, and rematch.
		bool IsActive() const { return m_Active; }

		/// The armed epoch; all zero while inactive.
		const NetAuthEpoch& GetEpoch() const { return m_Epoch; }

		/// Issues a fresh credential for the seat under a new holder generation, invalidating any
		/// prior generation's credential. One credential per generation; reconnects reuse it.
		bool IssueCredential(uint16_t seat, uint32_t& holderGeneration, NetSeatCredential& credential);

		/// Whether the credential matches the seat's active generation, compared in constant time.
		bool MatchesActiveCredential(uint16_t seat, uint32_t holderGeneration, const NetSeatCredential& credential) const;

		/// Verifies a reclaim proof against the seat's active credential. The credential never leaves
		/// the registry, so no caller can copy it out to log or serialize it.
		bool VerifySeatProof(uint16_t seat, uint32_t holderGeneration, const NetH4Transcript& transcript, const NetAuthBytes32& mac) const;

		/// The seat's active holder generation; 0 when the seat holds none.
		uint32_t GetActiveGeneration(uint16_t seat) const;

		/// Revokes the seat's active credential (clean leave, substitution); generations never rewind.
		void RevokeSeat(uint16_t seat);

	private:
		struct SeatEntry {
			uint32_t lastGeneration = 0;
			bool active = false;
			NetSeatCredential credential{};
		};

		bool m_Active = false;
		NetAuthEpoch m_Epoch{};
		std::unordered_map<uint16_t, SeatEntry> m_Seats;
	};

} // namespace RTE
