#include "NetReconnectLedger.h"

#include <algorithm>
#include <utility>

namespace RTE {

	std::vector<int64_t> NetReconnectLedger::CollectOwnedActorUIDs(const std::vector<NetH4LedgerActor>& actors, uint8_t peerId) {
		std::vector<int64_t> uids;
		for (const NetH4LedgerActor& actor : actors) {
			if (actor.alive && actor.ownerPeerId == peerId) {
				uids.push_back(actor.actorUID);
			}
		}
		std::sort(uids.begin(), uids.end());
		uids.erase(std::unique(uids.begin(), uids.end()), uids.end());
		return uids;
	}

	void NetReconnectLedger::RecordDrop(uint16_t stableSeat, uint8_t peerId, int32_t team, uint64_t droppedAtFrame, std::vector<int64_t> actorUIDs) {
		std::sort(actorUIDs.begin(), actorUIDs.end());
		actorUIDs.erase(std::unique(actorUIDs.begin(), actorUIDs.end()), actorUIDs.end());
		if (actorUIDs.size() > c_MaxActorsPerSeat) {
			m_ActorsTruncated += static_cast<uint32_t>(actorUIDs.size() - c_MaxActorsPerSeat);
			actorUIDs.resize(c_MaxActorsPerSeat);
		}
		NetH4SeatOwnership record;
		record.stableSeat = stableSeat;
		record.peerId = peerId;
		record.team = team;
		record.droppedAtFrame = droppedAtFrame;
		record.actorUIDs = std::move(actorUIDs);

		const auto existing = std::find_if(m_Seats.begin(), m_Seats.end(), [stableSeat](const NetH4SeatOwnership& seat) {
			return seat.stableSeat == stableSeat;
		});
		if (existing != m_Seats.end()) {
			*existing = std::move(record);
		} else {
			m_Seats.push_back(std::move(record));
		}
		++m_DropsRecorded;
	}

	const NetH4SeatOwnership* NetReconnectLedger::Find(uint16_t stableSeat) const {
		const auto existing = std::find_if(m_Seats.begin(), m_Seats.end(), [stableSeat](const NetH4SeatOwnership& seat) {
			return seat.stableSeat == stableSeat;
		});
		return existing == m_Seats.end() ? nullptr : &*existing;
	}

	std::vector<int64_t> NetReconnectLedger::BuildRestoration(uint16_t stableSeat, NetMatchMode mode, const std::vector<NetH4LedgerActor>& actors) const {
		(void)mode;
		std::vector<int64_t> restored;
		const NetH4SeatOwnership* record = Find(stableSeat);
		if (record == nullptr) {
			return restored;
		}
		for (const int64_t uid : record->actorUIDs) {
			const auto actor = std::find_if(actors.begin(), actors.end(), [uid](const NetH4LedgerActor& candidate) {
				return candidate.actorUID == uid;
			});
			if (actor != actors.end() && actor->alive && actor->team == record->team) {
				restored.push_back(uid);
			}
		}
		return restored;
	}

	void NetReconnectLedger::ClearSeat(uint16_t stableSeat) {
		m_Seats.erase(std::remove_if(m_Seats.begin(), m_Seats.end(), [stableSeat](const NetH4SeatOwnership& seat) {
			return seat.stableSeat == stableSeat;
		}), m_Seats.end());
	}

	void NetReconnectLedger::Clear() {
		m_Seats.clear();
	}

} // namespace RTE
