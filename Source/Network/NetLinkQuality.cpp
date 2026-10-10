#include "NetLinkQuality.h"

#include "NetMatchService.h"
#include <SDL3/SDL_stdinc.h>

#include <charconv>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace RTE {

	namespace {
		std::optional<NetLinkQuality> ForcedQuality(uint8_t peerId) {
			const char* lever = SDL_getenv_unsafe("CC_TEST_LINK_QUALITY");
			if (!lever) return std::nullopt;
			std::string_view entries(lever);
			while (!entries.empty()) {
				const size_t end = entries.find(';');
				const std::string_view entry = entries.substr(0, end);
				entries = end == std::string_view::npos ? std::string_view{} : entries.substr(end + 1);
				const size_t first = entry.find(':'), second = first == std::string_view::npos ? first : entry.find(':', first + 1);
				if (first == std::string_view::npos || second == std::string_view::npos) continue;
				unsigned int seat = 0;
				uint32_t rtt = 0;
				const auto parsedSeat = std::from_chars(entry.data(), entry.data() + first, seat);
				const auto parsedRtt = std::from_chars(entry.data() + second + 1, entry.data() + entry.size(), rtt);
				if (parsedSeat.ec != std::errc{} || parsedSeat.ptr != entry.data() + first || seat != peerId ||
				    parsedRtt.ec != std::errc{} || parsedRtt.ptr != entry.data() + entry.size()) continue;
				NetLinkQuality quality;
				const std::string_view state = entry.substr(first + 1, second - first - 1);
				if (state == "Good") quality.state = NetLinkQuality::State::Good;
				else if (state == "Marginal") quality.state = NetLinkQuality::State::Marginal;
				else if (state == "Substituting") quality.state = NetLinkQuality::State::Substituting;
				else if (state == "Lost") quality.state = NetLinkQuality::State::Lost;
				else continue;
				quality.rttMs = rtt;
				return quality;
			}
			return std::nullopt;
		}
	}

	NetLinkQuality NetLinkQualityForSeat(uint8_t peerId) {
		if (const auto forced = ForcedQuality(peerId)) return *forced;
		return g_NetMatchService.MeasuredLinkQualityForSeat(peerId);
	}

} // namespace RTE
