#pragma once

#include <cstdint>

namespace RTE {

	/// What a seat's connection is doing to its presses right now; read by the HUD indicator and the Seats panel.
	struct NetLinkQuality {
		enum class State : uint8_t {
			Good, ///< Presses arrive inside the seat's input delay; nothing is lost.
			Marginal, ///< The jitter margin is in use or the delay was re-sized in the last 10 s; nothing lost yet.
			Substituting, ///< A substitute was committed for this seat within the last second; presses may not register.
			Lost, ///< No authenticated traffic for the hold threshold; the AI plays the seat until it returns.
		};
		State state = State::Good;
		uint32_t rttMs = 0; ///< Round trip to this seat as the lockstep measures it.
		uint32_t jitterMs = 0; ///< Its current jitter margin.
		uint16_t delayFrames = 0; ///< Its current input delay in ticks.
	};

	/// The link quality of the seat played by lockstep peer `peerId`, from the match's measurements.
	/// The test lever CC_TEST_LINK_QUALITY="<peerId>:<Good|Marginal|Substituting|Lost>:<rttMs>;..." overrides it.
	NetLinkQuality NetLinkQualityForSeat(uint8_t peerId);

} // namespace RTE
