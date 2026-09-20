#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RTE {

	struct NetIceServer {
		std::vector<std::string> urls;
		std::string username;
		std::string credential;
		bool operator==(const NetIceServer&) const = default;
	};

	/// A match's expiring relay offer; backend signing keys never enter this type.
	struct NetRelayConfig {
		std::string matchId;
		uint64_t expiresAt = 0;
		std::vector<NetIceServer> iceServers;
		bool operator==(const NetRelayConfig&) const = default;
		bool Empty() const { return iceServers.empty(); }
		bool Valid() const;
		bool Usable(uint64_t unixSeconds) const { return !Empty() && expiresAt > unixSeconds && Valid(); }
		std::string ToJson() const;
		static bool FromJson(const std::string& text, NetRelayConfig& out);
		static NetRelayConfig Fixed(const std::string& servers, const std::string& user, const std::string& password, const std::string& matchId, uint64_t expiry);
		/// GNS's native ICE client consumes UDP host:port entries with parallel credential lists.
		void UdpLists(std::string& servers, std::string& users, std::string& passwords) const;
	};

} // namespace RTE
