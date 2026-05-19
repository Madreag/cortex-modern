#pragma once

#include "Singleton.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#define g_SimChecksum SimChecksum::Instance()

namespace RTE {

	/// Per-tick state hasher with per-subsystem breakdown.
	///
	/// Wraps BLAKE3. Each subsystem (`"terrain"`, `"decisions"`, `"tick"`, future `"carve_math"`, `"actors"`,
	/// `"rng"`) is hashed into its own accumulator; the total tick hash is `BLAKE3(concat over sorted subsystems
	/// of (name || subsystem_hash))`. Sorted by name so subsystem registration order does not affect the result.
	///
	/// Per the M0 plan (D:\Projects\M0_PLAN.md §A2), wired subsystems at M0:
	///   - `"terrain"` — raw FG/BG terrain bitmap bytes at end-of-tick (seed of the determinism island)
	///   - `"decisions"` — drained AIDecisionChannel events in sorted order
	///   - `"tick"` — the sim frame number
	/// M2 adds `"carve_math"` (deterministic carve/penetrate/dislodge result).
	/// M5 grows to whole-tick (`"actors"`, `"rng"`, `"lua_state"`).
	class SimChecksum : public Singleton<SimChecksum> {
		friend class Singleton<SimChecksum>;

	public:
		using Hash = std::array<uint8_t, 32>;

		struct Result {
			uint64_t                              tick = 0;
			Hash                                  total{};
			std::unordered_map<std::string, Hash> per_subsystem;
		};

		SimChecksum();
		~SimChecksum();

		void Initialize() {}
		void Destroy();

		/// Begin a new tick. Resets all subsystem accumulators.
		void BeginTick(uint64_t simFrame);

		/// Feed bytes into a named subsystem accumulator. Thread-safe.
		void Update(std::string_view subsystem, const void* data, size_t bytes);

		/// Finalize the current tick. Computes per-subsystem hashes + the total.
		Result EndTick();

		/// Get the most recent result. Thread-safe (returns a copy).
		Result GetLastResult() const;

		/// Convert a hash to a 64-character lowercase hex string.
		static std::string HashHex(const Hash& h);

		/// Parse a 64-character hex string into a hash. Returns all-zero on parse error.
		static Hash HashFromHex(std::string_view hex);

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;

		mutable std::mutex m_Mutex;
		Result             m_LastResult;
	};

} // namespace RTE
