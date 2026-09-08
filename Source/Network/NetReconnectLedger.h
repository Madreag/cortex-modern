#pragma once

#include "NetMatchConfig.h"

#include <cstdint>
#include <vector>

namespace RTE {

	/// What a seat controlled at the frame it dropped. Kept for the whole hosted session, replaced
	/// wholesale on each new drop of that seat, and cleared with the seat's registry entry - the
	/// control-override map cannot hold this, because it is purged for peers that are gone.
	struct NetH4SeatOwnership {
		uint16_t stableSeat = 0;
		uint8_t peerId = 0;
		int32_t team = 0;
		uint64_t droppedAtFrame = 0;
		std::vector<int64_t> actorUIDs;

		bool operator==(const NetH4SeatOwnership&) const = default;
	};

	/// One actor as the drop-time capture sees it: enough to decide ownership without the sim.
	struct NetH4LedgerActor {
		int64_t actorUID = 0;
		int32_t team = 0;
		uint8_t ownerPeerId = 0;
		bool alive = true;

		bool operator==(const NetH4LedgerActor&) const = default;
	};

	/// The drop-time ownership ledger. A returning holder is reseated onto exactly what this recorded,
	/// minus whatever died while they were away.
	class NetReconnectLedger {
	public:
		// Far above any plausible per-seat actor count in a 2-4 peer skirmish, and a bound against a
		// degenerate or hostile scene.
		static constexpr size_t c_MaxActorsPerSeat = 512;

		/// The UIDs the peer owned, in ascending UID order so every peer records the identical list.
		static std::vector<int64_t> CollectOwnedActorUIDs(const std::vector<NetH4LedgerActor>& actors, uint8_t peerId);

		/// Records the seat's ownership at its drop frame, replacing any earlier drop of that seat.
		void RecordDrop(uint16_t stableSeat, uint8_t peerId, int32_t team, uint64_t droppedAtFrame, std::vector<int64_t> actorUIDs);

		/// @return The seat's recorded ownership, or nullptr when the seat never dropped.
		const NetH4SeatOwnership* Find(uint16_t stableSeat) const;

		/// The actors a returning holder gets back: the ledgered ones that are still alive and still on
		/// the seat's team. Dead actors are not resurrected; per §8 that is the same rule in every mode,
		/// because PvP, PvPvE and co-op differ in who held the actors, not in what survives.
		std::vector<int64_t> BuildRestoration(uint16_t stableSeat, NetMatchMode mode, const std::vector<NetH4LedgerActor>& actors) const;

		void ClearSeat(uint16_t stableSeat);
		void Clear();

		size_t Size() const { return m_Seats.size(); }
		uint32_t GetDropsRecorded() const { return m_DropsRecorded; }
		uint32_t GetActorsTruncated() const { return m_ActorsTruncated; }

	private:
		std::vector<NetH4SeatOwnership> m_Seats;
		uint32_t m_DropsRecorded = 0;
		uint32_t m_ActorsTruncated = 0;
	};

} // namespace RTE
