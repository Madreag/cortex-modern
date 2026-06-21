#pragma once

#include <cstdint>

namespace RTE {

	/// Connection quality tier derived from round-trip ping, shown in the lobby and in-match overlay.
	enum class NetConnectionQuality {
		Unknown,
		Lan,
		Excellent,
		Good,
		Playable,
		Warning,
		Poor,
	};

	/// Classifies a round-trip ping (ms) into a quality tier. 0ms is treated as LAN/loopback.
	inline NetConnectionQuality ClassifyConnectionQuality(uint32_t pingMs) {
		if (pingMs == 0) {
			return NetConnectionQuality::Lan;
		}
		if (pingMs < 50) {
			return NetConnectionQuality::Excellent;
		}
		if (pingMs < 100) {
			return NetConnectionQuality::Good;
		}
		if (pingMs < 150) {
			return NetConnectionQuality::Playable;
		}
		if (pingMs < 250) {
			return NetConnectionQuality::Warning;
		}
		return NetConnectionQuality::Poor;
	}

	inline const char* NetConnectionQualityName(NetConnectionQuality quality) {
		switch (quality) {
			case NetConnectionQuality::Lan: return "LAN";
			case NetConnectionQuality::Excellent: return "Excellent";
			case NetConnectionQuality::Good: return "Good";
			case NetConnectionQuality::Playable: return "Playable";
			case NetConnectionQuality::Warning: return "Warning";
			case NetConnectionQuality::Poor: return "Poor";
			default: return "";
		}
	}

} // namespace RTE
