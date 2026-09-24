#include "SimChecksum.h"

#include "ThreadMan.h"

#include "tracy/Tracy.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <vector>

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

	namespace {
		constexpr uint64_t c_FnvPrime = 0x100000001b3ull;
		// A block is wide enough that folding it is rare work and narrow enough that a small change rebuilds little.
		constexpr int c_RowBlockBytes = 4096;
		// Table builds outstanding at once per key; a block past the budget folds byte by byte until its turn. Each build is
		// 256 passes over its block, so the budget keeps the background pool from crowding the sim thread's own cores.
		constexpr size_t c_RowBlockBuildsInFlight = 16;
		// Calls a block must hold still through before its table is worth building.
		constexpr uint8_t c_RowBlockStillCalls = 2;

		using RowBlockTable = std::array<uint64_t, 256>;

		uint64_t PrimePower(uint64_t exponent) {
			uint64_t result = 1;
			uint64_t base = c_FnvPrime;
			for (; exponent > 0; exponent >>= 1) {
				if (exponent & 1) {
					result *= base;
				}
				base *= base;
			}
			return result;
		}

		// FNV-1a over a block from any state s is s * P^n + table[s & 0xFF]: xor with a byte moves only the low eight bits,
		// so what the product adds depends on those bits alone.
		std::unique_ptr<RowBlockTable> BuildRowBlockTable(const std::vector<uint8_t>& bytes) {
			auto table = std::make_unique<RowBlockTable>();
			RowBlockTable states;
			for (uint64_t low = 0; low < 256; ++low) {
				states[low] = low;
			}
			for (const uint8_t byte: bytes) {
				for (uint64_t& state: states) {
					state = (state ^ byte) * c_FnvPrime;
				}
			}
			const uint64_t power = PrimePower(bytes.size());
			for (uint64_t low = 0; low < 256; ++low) {
				(*table)[low] = states[low] - low * power;
			}
			return table;
		}

		// Where the builds beside the game leave their tables; a build outlives nothing it reads.
		struct RowBlockSink {
			struct Built {
				size_t block = 0;
				uint64_t version = 0;
				std::unique_ptr<RowBlockTable> table;
			};
			std::mutex mutex;
			std::vector<Built> built;
		};

		// What one key's rows held at its last call, block by block, and the tables built from those bytes.
		struct RowBlockCache {
			std::string subsystem;
			const void* key = nullptr;
			int width = 0;
			int height = 0;
			int blocksPerRow = 0;
			uint64_t fullPower = 0;
			uint64_t tailPower = 0;
			uint64_t nextVersion = 0;
			uint64_t lastUse = 0;
			size_t inFlight = 0;
			uint64_t tableFolds = 0;
			std::vector<uint8_t> shadow;
			std::vector<uint64_t> version;
			std::vector<uint64_t> tableVersion;
			std::vector<uint8_t> building;
			std::vector<uint8_t> same;
			std::vector<uint8_t> stillCalls;
			std::vector<std::unique_ptr<RowBlockTable>> table;
			std::shared_ptr<RowBlockSink> sink = std::make_shared<RowBlockSink>();
		};
	} // namespace

	struct SimChecksum::Impl {
		// std::map so iteration is name-sorted — the total combines subsystems in name order.
		std::map<std::string, ChecksumHasher> hashers;
		std::mutex                           mutex;
		uint64_t                             tick = 0;
		// Atomic so the hot feed paths can gate on it without taking the mutex.
		std::atomic<bool>                    active{false};
		std::vector<std::unique_ptr<RowBlockCache>> rowCaches;
		uint64_t                             rowCacheClock = 0;
		// The self-test builds its tables in place, before the engine's pools exist.
		bool                                 buildRowTablesInPlace = false;
	};

	SimChecksum::SimChecksum() : m_Impl(std::make_unique<Impl>()) {}

	SimChecksum::~SimChecksum() = default;

	void SimChecksum::Destroy() {
		std::lock_guard<std::mutex> lock(m_Impl->mutex);
		m_Impl->hashers.clear();
		m_Impl->rowCaches.clear();
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

	void SimChecksum::UpdateRows(std::string_view subsystem, const void* key, const uint8_t* const* rows, int width, int height) {
		if (!m_Impl->active || m_Suppressed.load(std::memory_order_relaxed) || !rows || width <= 0 || height <= 0) {
			return;
		}
		ZoneScopedN("SimChecksum::UpdateRows");
		std::lock_guard<std::mutex> lock(m_Impl->mutex);
		std::string name(subsystem);
		auto hasher = m_Impl->hashers.find(name);
		if (hasher == m_Impl->hashers.end()) {
			hasher = m_Impl->hashers.emplace(name, ChecksumHasher{}).first;
		}

		auto found = std::find_if(m_Impl->rowCaches.begin(), m_Impl->rowCaches.end(), [&](const std::unique_ptr<RowBlockCache>& cache) {
			return cache->key == key && cache->subsystem == name;
		});
		if (found == m_Impl->rowCaches.end() || (*found)->width != width || (*found)->height != height) {
			if (found == m_Impl->rowCaches.end()) {
				// A handful of keys live at once (one per terrain bitmap); the one used longest ago makes room.
				if (m_Impl->rowCaches.size() >= 8) {
					m_Impl->rowCaches.erase(std::min_element(m_Impl->rowCaches.begin(), m_Impl->rowCaches.end(), [](const auto& a, const auto& b) { return a->lastUse < b->lastUse; }));
				}
				m_Impl->rowCaches.push_back(std::make_unique<RowBlockCache>());
				found = std::prev(m_Impl->rowCaches.end());
			} else {
				*found = std::make_unique<RowBlockCache>();
			}
			RowBlockCache& fresh = **found;
			fresh.subsystem = name;
			fresh.key = key;
			fresh.width = width;
			fresh.height = height;
			fresh.blocksPerRow = (width + c_RowBlockBytes - 1) / c_RowBlockBytes;
			fresh.fullPower = PrimePower(static_cast<uint64_t>(std::min(width, c_RowBlockBytes)));
			fresh.tailPower = PrimePower(static_cast<uint64_t>(width - (fresh.blocksPerRow - 1) * c_RowBlockBytes));
			const size_t blocks = static_cast<size_t>(fresh.blocksPerRow) * static_cast<size_t>(height);
			fresh.shadow.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
			fresh.version.assign(blocks, 0);
			fresh.tableVersion.assign(blocks, 0);
			fresh.building.assign(blocks, 0);
			fresh.stillCalls.assign(blocks, 0);
			fresh.table.resize(blocks);
		}
		RowBlockCache& cache = **found;
		cache.lastUse = ++m_Impl->rowCacheClock;

		{
			std::lock_guard<std::mutex> sinkLock(cache.sink->mutex);
			for (RowBlockSink::Built& built: cache.sink->built) {
				cache.building[built.block] = 0;
				--cache.inFlight;
				if (built.version == cache.version[built.block]) {
					cache.table[built.block] = std::move(built.table);
					cache.tableVersion[built.block] = built.version;
				}
			}
			cache.sink->built.clear();
		}

		// Which blocks still hold what they held last call; a changed block's bytes replace the remembered ones.
		cache.same.resize(cache.version.size());
		const auto compareRows = [&cache, rows, width](int firstRow, int endRow) {
			for (int y = firstRow; y < endRow; ++y) {
				for (int column = 0; column < cache.blocksPerRow; ++column) {
					const int offset = column * c_RowBlockBytes;
					const size_t length = static_cast<size_t>(std::min(c_RowBlockBytes, width - offset));
					const uint8_t* live = rows[y] + offset;
					uint8_t* saved = cache.shadow.data() + static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(offset);
					const size_t block = static_cast<size_t>(y) * static_cast<size_t>(cache.blocksPerRow) + static_cast<size_t>(column);
					const bool same = cache.version[block] != 0 && std::memcmp(live, saved, length) == 0;
					cache.same[block] = same ? 1 : 0;
					if (!same) {
						std::memcpy(saved, live, length);
					}
				}
			}
		};
		constexpr int c_CompareBands = 8;
		if (m_Impl->buildRowTablesInPlace || height < c_CompareBands * 8) {
			compareRows(0, height);
		} else {
			// Reading the whole terrain twice is most of what an unchanged tick costs, so the bands share it out.
			BS::multi_future<void> bands;
			for (int band = 0; band < c_CompareBands; ++band) {
				bands.push_back(g_ThreadMan.GetPriorityThreadPool().submit([&compareRows, height, band]() {
					compareRows(height * band / c_CompareBands, height * (band + 1) / c_CompareBands);
				}));
			}
			bands.wait();
		}

		uint64_t state = hasher->second.state;
		std::vector<size_t> worthATable;
		for (int y = 0; y < height; ++y) {
			const uint8_t* row = rows[y];
			for (int column = 0; column < cache.blocksPerRow; ++column) {
				const int offset = column * c_RowBlockBytes;
				const size_t length = static_cast<size_t>(std::min(c_RowBlockBytes, width - offset));
				const uint8_t* live = row + offset;
				const size_t block = static_cast<size_t>(y) * static_cast<size_t>(cache.blocksPerRow) + static_cast<size_t>(column);
				const bool same = cache.same[block] != 0;
				if (!same) {
					cache.version[block] = ++cache.nextVersion;
					cache.stillCalls[block] = 0;
				} else if (cache.stillCalls[block] < c_RowBlockStillCalls) {
					++cache.stillCalls[block];
				}
				if (same && cache.table[block] && cache.tableVersion[block] == cache.version[block]) {
					const uint64_t power = column + 1 == cache.blocksPerRow ? cache.tailPower : cache.fullPower;
					state = state * power + (*cache.table[block])[state & 0xFF];
					++cache.tableFolds;
					continue;
				}
				for (size_t index = 0; index < length; ++index) {
					state = (state ^ live[index]) * c_FnvPrime;
				}
				// A block that has held still is likely to hold still again: build its table.
				if (same && cache.stillCalls[block] >= c_RowBlockStillCalls && !cache.building[block] && cache.inFlight + worthATable.size() < c_RowBlockBuildsInFlight) {
					worthATable.push_back(block);
				}
			}
		}
		hasher->second.state = state;

		for (const size_t block: worthATable) {
			const size_t y = block / static_cast<size_t>(cache.blocksPerRow);
			const size_t offset = (block % static_cast<size_t>(cache.blocksPerRow)) * static_cast<size_t>(c_RowBlockBytes);
			const size_t length = std::min(static_cast<size_t>(c_RowBlockBytes), static_cast<size_t>(width) - offset);
			const uint8_t* saved = cache.shadow.data() + y * static_cast<size_t>(width) + offset;
			// The build reads its own copy, so the next call may rewrite the block while it runs.
			auto build = [sink = cache.sink, block, version = cache.version[block], bytes = std::vector<uint8_t>(saved, saved + length)]() {
				std::unique_ptr<RowBlockTable> table = BuildRowBlockTable(bytes);
				std::lock_guard<std::mutex> sinkLock(sink->mutex);
				sink->built.push_back({block, version, std::move(table)});
			};
			cache.building[block] = 1;
			++cache.inFlight;
			if (m_Impl->buildRowTablesInPlace) {
				build();
			} else {
				g_ThreadMan.GetBackgroundThreadPool().push_task(std::move(build));
			}
		}
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
		// Combine name || hash in sorted name order, skipping the off-wire controller subsystems: the input a
		// seat applies and which seat drives an actor on THIS machine are both outside the gated set.
		std::map<std::string, const Hash*> sorted;
		for (const auto& [name, hash]: result.per_subsystem) {
			if (name != "controller" && name != "controller_route") {
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

	bool SimChecksum::RunRowBlockSelfTest() {
		constexpr const char* Tag = "[sim-checksum-selftest]";
		bool passed = true;
		std::mt19937 random(20260924);
		// One block with a short row, two whole blocks, and three blocks with a tail.
		const std::array<std::pair<int, int>, 3> shapes = {{{300, 50}, {8192, 20}, {10000, 40}}};
		for (const auto& [width, height]: shapes) {
			std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height));
			for (uint8_t& pixel: pixels) {
				pixel = static_cast<uint8_t>(random() & 0xFF);
			}
			std::vector<const uint8_t*> rows(static_cast<size_t>(height));
			for (int y = 0; y < height; ++y) {
				rows[static_cast<size_t>(y)] = pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
			}
			SimChecksum byRow;
			SimChecksum byBlock;
			byBlock.m_Impl->buildRowTablesInPlace = true;
			constexpr int c_Ticks = 16;
			int matched = 0;
			for (int tick = 1; tick <= c_Ticks; ++tick) {
				// Still ticks let tables land; the rest change one byte, a whole row, a row's last byte or scattered bytes.
				const int change = tick <= 3 ? -1 : tick % 5;
				if (change == 0) {
					pixels[random() % pixels.size()] ^= 0x5A;
				} else if (change == 1) {
					const size_t y = random() % static_cast<size_t>(height);
					for (int x = 0; x < width; ++x) pixels[y * static_cast<size_t>(width) + static_cast<size_t>(x)] = static_cast<uint8_t>(random() & 0xFF);
				} else if (change == 2) {
					pixels[(random() % static_cast<size_t>(height)) * static_cast<size_t>(width) + static_cast<size_t>(width - 1)] += 1;
				} else if (change == 3) {
					for (int count = 0; count < 20; ++count) pixels[random() % pixels.size()] = static_cast<uint8_t>(random() & 0xFF);
				}
				const int dims[2] = {width, height};
				byRow.BeginTick(static_cast<uint64_t>(tick));
				byRow.Update("terrain", dims, sizeof(dims));
				for (int y = 0; y < height; ++y) {
					byRow.Update("terrain", rows[static_cast<size_t>(y)], static_cast<size_t>(width));
				}
				const Result expected = byRow.EndTick();
				byBlock.BeginTick(static_cast<uint64_t>(tick));
				byBlock.Update("terrain", dims, sizeof(dims));
				byBlock.UpdateRows("terrain", pixels.data(), rows.data(), width, height);
				const Result actual = byBlock.EndTick();
				if (actual.total == expected.total && actual.per_subsystem == expected.per_subsystem) {
					++matched;
				} else {
					std::cout << Tag << " FAIL row_blocks_hash_as_rows width=" << width << " height=" << height << " tick=" << tick
					          << " rows=" << HashHex(expected.total) << " blocks=" << HashHex(actual.total) << std::endl;
				}
			}
			const uint64_t folds = byBlock.m_Impl->rowCaches.empty() ? 0 : byBlock.m_Impl->rowCaches.front()->tableFolds;
			const bool shapePassed = matched == c_Ticks && folds > 0;
			passed = passed && shapePassed;
			std::cout << Tag << (shapePassed ? " PASS" : " FAIL") << " row_blocks_hash_as_rows width=" << width << " height=" << height
			          << " ticks_matched=" << matched << "/" << c_Ticks << " table_folds=" << folds << (folds > 0 ? "" : " (the table path never ran)") << std::endl;
		}
		std::cout << Tag << (passed ? " PASS" : " FAIL") << std::endl;
		return passed;
	}

} // namespace RTE
