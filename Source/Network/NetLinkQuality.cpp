#include "NetLinkQuality.h"

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <string_view>
#include <system_error>

namespace RTE {
	NetLinkQuality NetLinkQualityForSeat(uint8_t peerId) {
		const char* lever = std::getenv("CC_TEST_LINK_QUALITY");
		if (!lever || !*lever) return {};
		std::string_view entries(lever);
		while (!entries.empty()) {
			const size_t end = entries.find(';');
			const std::string_view entry = entries.substr(0, end);
			entries = end == std::string_view::npos ? std::string_view{} : entries.substr(end + 1);
			const size_t first = entry.find(':'), last = entry.rfind(':');
			if (first == std::string_view::npos || first == last) continue;
			unsigned peer = 0;
			uint32_t rtt = 0;
			const auto id = std::from_chars(entry.data(), entry.data() + first, peer);
			const auto time = std::from_chars(entry.data() + last + 1, entry.data() + entry.size(), rtt);
			if (id.ec != std::errc{} || id.ptr != entry.data() + first || peer != peerId ||
			    time.ec != std::errc{} || time.ptr != entry.data() + entry.size()) continue;
			const std::string_view state = entry.substr(first + 1, last - first - 1);
			NetLinkQuality quality;
			if (state == "Good") quality.state = NetLinkQuality::State::Good;
			else if (state == "Marginal") quality.state = NetLinkQuality::State::Marginal;
			else if (state == "Substituting") quality.state = NetLinkQuality::State::Substituting;
			else if (state == "Lost") quality.state = NetLinkQuality::State::Lost;
			else continue;
			quality.rttMs = rtt;
			return quality;
		}
		return {};
	}
}
