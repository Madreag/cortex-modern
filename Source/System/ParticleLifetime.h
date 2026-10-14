#pragma once

#include <cstdint>

namespace RTE {
	/// Decays lifetime in 32-bit milliseconds, including particles already past expiry.
	inline uint32_t DecayParticleLifetime(uint32_t lifetime, uint32_t age, float fraction) {
		const uint32_t remaining = lifetime - age;
		return lifetime - static_cast<uint32_t>(static_cast<float>(remaining) * fraction);
	}
}
