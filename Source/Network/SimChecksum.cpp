#include "SimChecksum.h"

#include "blake3.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

namespace RTE {

	struct SimChecksum::Impl {
		// std::map so iteration is name-sorted — the total combines subsystems in name order.
		std::map<std::string, blake3_hasher> hashers;
		std::mutex                           mutex;
		uint64_t                             tick = 0;
		bool                                 active = false;
	};

	SimChecksum::SimChecksum() : m_Impl(std::make_unique<Impl>()) {}

	SimChecksum::~SimChecksum() = default;

	void SimChecksum::Destroy() {
		std::lock_guard<std::mutex> lock(m_Impl->mutex);
		m_Impl->hashers.clear();
		m_Impl->active = false;
		m_Impl->tick = 0;
	}

	void SimChecksum::BeginTick(uint64_t simFrame) {
		ZoneScopedN("SimChecksum::BeginTick");
		std::lock_guard<std::mutex> lock(m_Impl->mutex);
		m_Impl->hashers.clear();
		m_Impl->tick = simFrame;
		m_Impl->active = true;

		// Always feed the tick subsystem so a no-op tick still has a stable hash.
		blake3_hasher h;
		blake3_hasher_init(&h);
		blake3_hasher_update(&h, &simFrame, sizeof(simFrame));
		m_Impl->hashers.emplace("tick", h);
	}

	void SimChecksum::Update(std::string_view subsystem, const void* data, size_t bytes) {
		if (!m_Impl->active) {
			return;
		}
		std::lock_guard<std::mutex> lock(m_Impl->mutex);
		std::string key(subsystem);
		auto it = m_Impl->hashers.find(key);
		if (it == m_Impl->hashers.end()) {
			blake3_hasher h;
			blake3_hasher_init(&h);
			it = m_Impl->hashers.emplace(std::move(key), h).first;
		}
		blake3_hasher_update(&it->second, data, bytes);
	}

	SimChecksum::Result SimChecksum::EndTick() {
		ZoneScopedN("SimChecksum::EndTick");
		std::lock_guard<std::mutex> lock(m_Impl->mutex);

		Result r;
		r.tick = m_Impl->tick;

		blake3_hasher total;
		blake3_hasher_init(&total);

		for (auto& [name, hasher]: m_Impl->hashers) {
			Hash out{};
			blake3_hasher_finalize(&hasher, out.data(), out.size());
			r.per_subsystem[name] = out;

			// Combine as name || hash; map order is sorted, so total is registration-order-independent.
			blake3_hasher_update(&total, name.data(), name.size());
			blake3_hasher_update(&total, out.data(), out.size());
		}

		blake3_hasher_finalize(&total, r.total.data(), r.total.size());

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

	bool SimChecksum::IsActive() const {
		std::lock_guard<std::mutex> lock(m_Impl->mutex);
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
