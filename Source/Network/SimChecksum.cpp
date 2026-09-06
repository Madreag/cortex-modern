#include "SimChecksum.h"

#include "tracy/Tracy.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>

namespace RTE {

	// A small deterministic, non-cryptographic hash for the determinism checksum. Integer-only,
	// so it is bit-identical on every platform — a crypto hash isn't needed to detect divergence.
	// FNV-1a accumulation, expanded to the digest with a splitmix64 finalizer.
	struct ChecksumHasher {
		uint64_t state = 0xcbf29ce484222325ull; // FNV-1a 64-bit offset basis

		void update(const void* data, size_t bytes) {
			const auto* p = static_cast<const uint8_t*>(data);
			for (size_t i = 0; i < bytes; ++i) {
				state ^= p[i];
				state *= 0x100000001b3ull; // FNV-1a 64-bit prime
			}
		}

		void finalize(uint8_t* out, size_t outBytes) const {
			uint64_t x = state;
			for (size_t i = 0; i < outBytes; i += 8) {
				x += 0x9e3779b97f4a7c15ull;
				uint64_t z = x;
				z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
				z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
				z ^= z >> 31;
				for (size_t b = 0; b < 8 && i + b < outBytes; ++b) {
					out[i + b] = static_cast<uint8_t>(z >> (b * 8));
				}
			}
		}
	};

	struct SimChecksum::Impl {
		// std::map so iteration is name-sorted — the total combines subsystems in name order.
		std::map<std::string, ChecksumHasher> hashers;
		std::mutex                           mutex;
		uint64_t                             tick = 0;
		// Atomic so the hot feed paths can gate on it without taking the mutex.
		std::atomic<bool>                    active{false};
	};

	SimChecksum::SimChecksum() : m_Impl(std::make_unique<Impl>()) {}

	SimChecksum::~SimChecksum() = default;

	void SimChecksum::Destroy() {
		std::lock_guard<std::mutex> lock(m_Impl->mutex);
		m_Impl->hashers.clear();
		m_Impl->active = false;
		m_Impl->tick = 0;
	}

	void SimChecksum::SetSuppressed(bool suppressed) {
		m_Suppressed.store(suppressed, std::memory_order_relaxed);
	}

	bool SimChecksum::IsSuppressed() const {
		return m_Suppressed.load(std::memory_order_relaxed);
	}

	void SimChecksum::BeginTick(uint64_t simFrame) {
		ZoneScopedN("SimChecksum::BeginTick");
		std::lock_guard<std::mutex> lock(m_Impl->mutex);
		m_Impl->hashers.clear();
		m_Impl->tick = simFrame;
		m_Impl->active = true;

		// Always feed the tick subsystem so a no-op tick still has a stable hash.
		ChecksumHasher h;
		h.update(&simFrame, sizeof(simFrame));
		m_Impl->hashers.emplace("tick", h);
	}

	void SimChecksum::Update(std::string_view subsystem, const void* data, size_t bytes) {
		if (!m_Impl->active || m_Suppressed.load(std::memory_order_relaxed)) {
			return;
		}
		std::lock_guard<std::mutex> lock(m_Impl->mutex);
		std::string key(subsystem);
		auto it = m_Impl->hashers.find(key);
		if (it == m_Impl->hashers.end()) {
			it = m_Impl->hashers.emplace(std::move(key), ChecksumHasher{}).first;
		}
		it->second.update(data, bytes);
	}

	SimChecksum::Result SimChecksum::EndTick() {
		ZoneScopedN("SimChecksum::EndTick");
		std::lock_guard<std::mutex> lock(m_Impl->mutex);

		Result r;
		r.tick = m_Impl->tick;

		ChecksumHasher total;

		for (auto& [name, hasher]: m_Impl->hashers) {
			Hash out{};
			hasher.finalize(out.data(), out.size());
			r.per_subsystem[name] = out;

			// Combine as name || hash; map order is sorted, so total is registration-order-independent.
			total.update(name.data(), name.size());
			total.update(out.data(), out.size());
		}

		total.finalize(r.total.data(), r.total.size());

		m_Impl->active = false;
		m_Impl->hashers.clear();

		{
			std::lock_guard<std::mutex> rlock(m_Mutex);
			m_LastResult = r;
		}
		return r;
	}

	SimChecksum::Result SimChecksum::GetLastResult() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_LastResult;
	}

	SimChecksum::Hash SimChecksum::SimGatedHash(const Result& result) {
		// Combine name || hash in sorted name order, skipping the off-wire controller subsystem.
		std::map<std::string, const Hash*> sorted;
		for (const auto& [name, hash]: result.per_subsystem) {
			if (name != "controller") {
				sorted.emplace(name, &hash);
			}
		}
		ChecksumHasher combined;
		for (const auto& [name, hash]: sorted) {
			combined.update(name.data(), name.size());
			combined.update(hash->data(), hash->size());
		}
		Hash out{};
		combined.finalize(out.data(), out.size());
		return out;
	}

	bool SimChecksum::IsActive() const {
		return m_Impl->active;
	}

	std::string SimChecksum::HashHex(const Hash& h) {
		static const char hex[] = "0123456789abcdef";
		std::string out;
		out.resize(h.size() * 2);
		for (size_t i = 0; i < h.size(); ++i) {
			out[i * 2 + 0] = hex[(h[i] >> 4) & 0x0F];
			out[i * 2 + 1] = hex[h[i] & 0x0F];
		}
		return out;
	}

	SimChecksum::Hash SimChecksum::HashFromHex(std::string_view hex) {
		Hash out{};
		if (hex.size() != out.size() * 2) {
			return out;
		}
		auto hexVal = [](char c) -> int {
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return 10 + c - 'a';
			if (c >= 'A' && c <= 'F') return 10 + c - 'A';
			return -1;
		};
		for (size_t i = 0; i < out.size(); ++i) {
			const int hi = hexVal(hex[i * 2 + 0]);
			const int lo = hexVal(hex[i * 2 + 1]);
			if (hi < 0 || lo < 0) {
				out.fill(0);
				return out;
			}
			out[i] = static_cast<uint8_t>((hi << 4) | lo);
		}
		return out;
	}

} // namespace RTE
