#include "NetReconnectTxCache.h"

#include <algorithm>
#include <utility>

namespace RTE {

	NetReconnectTxCache::NetReconnectTxCache(size_t maxEntries, uint64_t retentionMs) :
	    m_MaxEntries(maxEntries == 0 ? 1U : maxEntries), m_RetentionMs(retentionMs) {}

	std::vector<NetReconnectTxCache::Entry>::iterator NetReconnectTxCache::FindEntry(const NetAuthBytes16& txId) {
		return std::find_if(m_Entries.begin(), m_Entries.end(), [&txId](const Entry& entry) {
			return entry.txId == txId;
		});
	}

	void NetReconnectTxCache::Expire(uint64_t nowMs) {
		std::vector<Entry> observed;
		if (m_EvictionObserver) {
			for (const auto& entry: m_Entries) {
				if (nowMs >= entry.storedAtMs && nowMs - entry.storedAtMs > m_RetentionMs) observed.push_back(entry);
			}
		}
		const auto expired = std::remove_if(m_Entries.begin(), m_Entries.end(), [this, nowMs](const Entry& entry) {
			return nowMs >= entry.storedAtMs && nowMs - entry.storedAtMs > m_RetentionMs;
		});
		m_ExpiredEvictions += static_cast<uint32_t>(std::distance(expired, m_Entries.end()));
		m_Entries.erase(expired, m_Entries.end());
		for (const auto& entry: observed) m_EvictionObserver(entry.txId, entry.key, entry.terminalResult, entry.storedAtMs, nowMs, true);
	}

	void NetReconnectTxCache::Store(const NetAuthBytes16& txId, const NetH4TxKey& key, NetPayload terminalResult, uint64_t nowMs) {
		Expire(nowMs);
		if (const auto existing = FindEntry(txId); existing != m_Entries.end()) {
			existing->key = key;
			existing->terminalResult = std::move(terminalResult);
			return;
		}
		while (m_Entries.size() >= m_MaxEntries) {
			std::vector<Entry> observed;
			if (m_EvictionObserver) observed.push_back(m_Entries.front());
			m_Entries.erase(m_Entries.begin());
			++m_CapacityEvictions;
			for (const auto& entry: observed) m_EvictionObserver(entry.txId, entry.key, entry.terminalResult, entry.storedAtMs, nowMs, false);
		}
		m_Entries.push_back({txId, key, std::move(terminalResult), nowMs});
	}

	const NetPayload* NetReconnectTxCache::Find(const NetAuthBytes16& txId, const NetH4TxKey& key, uint64_t nowMs) {
		Expire(nowMs);
		const auto entry = FindEntry(txId);
		if (entry == m_Entries.end()) {
			++m_Misses;
			return nullptr;
		}
		if (!(entry->key == key)) {
			++m_KeyMismatches;
			return nullptr;
		}
		++m_Hits;
		return &entry->terminalResult;
	}

	void NetReconnectTxCache::Clear() {
		m_Entries.clear();
	}

} // namespace RTE
