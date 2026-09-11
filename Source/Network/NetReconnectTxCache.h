#pragma once

#include "NetProtocol.h"

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace RTE {

	/// What a re-presented transaction must match byte for byte before the host replays its cached
	/// result. Transaction ids are drawn by the client, so the key is what stops an attacker naming
	/// someone else's successful transaction and being handed its outcome.
	struct NetH4TxKey {
		NetMessageType requestType = NetMessageType::Reclaim;
		uint16_t stableSeat = 0;
		uint32_t holderGeneration = 0;
		NetH4Identity identity;

		bool operator==(const NetH4TxKey&) const = default;
	};

	/// The terminal results of admission transactions, kept long enough that every retransmission a
	/// live transaction can still produce lands inside the window. A duplicate valid proof replays
	/// the success it already earned; a losing ack never turns into a denial.
	class NetReconnectTxCache {
	public:
		static constexpr size_t c_MaxEntries = 64;
		static constexpr uint64_t c_RetentionMs = 60000;

		NetReconnectTxCache() = default;
		NetReconnectTxCache(size_t maxEntries, uint64_t retentionMs);
		using EvictionObserver = std::function<void(const NetAuthBytes16&, const NetH4TxKey&, const NetPayload&, uint64_t, uint64_t, bool)>;
		void SetEvictionObserver(EvictionObserver observer) { m_EvictionObserver = std::move(observer); }

		/// Records a transaction's terminal result. Re-storing a known id keeps the original age, so
		/// retransmissions cannot hold an entry alive past the window.
		void Store(const NetAuthBytes16& txId, const NetH4TxKey& key, NetPayload terminalResult, uint64_t nowMs);

		/// Looks up a re-presented transaction.
		/// @return The cached terminal result, or nullptr when the id is unknown, expired, or names a
		/// different transaction - which is a fresh transaction, to be denied on its own merits.
		const NetPayload* Find(const NetAuthBytes16& txId, const NetH4TxKey& key, uint64_t nowMs);

		/// Drops every entry past the retention window.
		void Expire(uint64_t nowMs);

		void Clear();

		size_t Size() const { return m_Entries.size(); }
		size_t GetMaxEntries() const { return m_MaxEntries; }
		uint64_t GetRetentionMs() const { return m_RetentionMs; }
		uint32_t GetHits() const { return m_Hits; }
		uint32_t GetMisses() const { return m_Misses; }
		uint32_t GetKeyMismatches() const { return m_KeyMismatches; }
		uint32_t GetExpiredEvictions() const { return m_ExpiredEvictions; }
		uint32_t GetCapacityEvictions() const { return m_CapacityEvictions; }

	private:
		struct Entry {
			NetAuthBytes16 txId{};
			NetH4TxKey key;
			NetPayload terminalResult;
			uint64_t storedAtMs = 0;
		};

		std::vector<Entry>::iterator FindEntry(const NetAuthBytes16& txId);

		size_t m_MaxEntries = c_MaxEntries;
		uint64_t m_RetentionMs = c_RetentionMs;
		// Insertion order, so eviction picks the oldest entry without consulting a clock.
		std::vector<Entry> m_Entries;
		uint32_t m_Hits = 0;
		uint32_t m_Misses = 0;
		uint32_t m_KeyMismatches = 0;
		uint32_t m_ExpiredEvictions = 0;
		uint32_t m_CapacityEvictions = 0;
		EvictionObserver m_EvictionObserver;
	};

} // namespace RTE
