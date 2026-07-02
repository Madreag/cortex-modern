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

	/// Per-tick state hasher with per-subsystem breakdown. Uses a small deterministic non-crypto hash; the total tick hash
	/// combines name-sorted subsystems, so registration order is irrelevant. Subsystems are
	/// created on first Update().
	///
	/// Subsystems: `tick`, `terrain`, `carve_math`, `actors`, `items`, `particles`, `rot_angle`,
	/// `rot_angvel`, `scene`, `funds`, `sim_rng`, `lua_state`, `controller`.
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

		/// Combines a result's subsystem hashes EXCEPT `controller` into one hash, name-sorted so it is
		/// registration-order-independent. This is the on-wire (sim-gated) hash both peers must agree on; the
		/// `controller` subsystem is off-wire (per-machine AI) and excluded.
		/// @param result A finalized tick result.
		/// @return The sim-gated hash of all on-wire subsystems.
		static Hash SimGatedHash(const Result& result);

		/// Whether a tick is currently being accumulated (between BeginTick and EndTick). Thread-safe.
		bool IsActive() const;

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
