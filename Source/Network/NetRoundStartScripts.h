#pragma once

#include <cstdint>
#include <vector>

namespace RTE {

	/// The first bytes of the blob a lockstep round's host streams so every peer starts on its script state.
	inline constexpr uint8_t c_RoundStartScriptsMagic[4] = {'R', 'S', 'S', '1'};

	/// Whether a streamed start blob is a round's start scripts rather than a match image.
	inline bool IsRoundStartScriptBlob(const std::vector<uint8_t>& bytes) {
		return bytes.size() > 4 && bytes[0] == c_RoundStartScriptsMagic[0] && bytes[1] == c_RoundStartScriptsMagic[1] &&
		       bytes[2] == c_RoundStartScriptsMagic[2] && bytes[3] == c_RoundStartScriptsMagic[3];
	}
} // namespace RTE
