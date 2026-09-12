#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RTE {

	/// The five compatibility fields the directory row lists; a v2 beacon carries them so a LAN
	/// row can be judged with the same predicate as a NET row.
	struct NetLanCompatIdentity {
		int64_t networkProtocolVersion = 0;
		int64_t lockstepCodecVersion = 0;
		int64_t controllerFrameVersion = 0;
		std::string sessionIdentityHash;
		std::string moduleManifestHash;

		bool operator==(const NetLanCompatIdentity&) const = default;
	};

	/// A discovered LAN host, as advertised by its beacon.
	struct NetLanHostInfo {
		std::string address; // The sender's IP, ready to join.
		uint16_t port = 0;
		std::string hostName;
		std::string activity;
		std::string mode;
		uint8_t playerCount = 0;
		uint8_t maxPlayers = 0;
		uint64_t lastSeenMs = 0;
		bool hasCompatibility = false;      // False on a v1 beacon: there is nothing to judge.
		NetLanCompatIdentity compatibility; // Meaningful only when hasCompatibility is set.
	};

	/// LAN game discovery over UDP broadcast: a hosting lobby beacons once a second; the join
	/// screen's browser collects beacons and lists fresh ones. Direct IP stays the internet path —
	/// this is purely the same-network convenience.
	class NetLanDiscovery {
	public:
		static constexpr uint16_t c_DiscoveryPort = 42115;
		static constexpr uint32_t c_Magic = 0x434C4143U; // "CALC" -> CC LAn disCovery
		static constexpr uint16_t c_Version = 2;
		static constexpr uint64_t c_BeaconIntervalMs = 1000;
		static constexpr uint64_t c_EntryTtlMs = 3500;

		~NetLanDiscovery() { Stop(); }

		/// Starts broadcasting this host's lobby. Safe to call repeatedly to update the payload.
		bool StartBeacon(uint16_t gamePort, const std::string& hostName, const std::string& activity, const std::string& mode, uint8_t playerCount, uint8_t maxPlayers, std::string* error = nullptr);
		/// Same beacon plus the compatibility fields (v2). A nullptr compat emits the v1 layout, which an old reader still decodes.
		bool StartBeacon(uint16_t gamePort, const std::string& hostName, const std::string& activity, const std::string& mode, uint8_t playerCount, uint8_t maxPlayers, const NetLanCompatIdentity* compat, std::string* error = nullptr);
		/// Starts listening for other hosts' beacons.
		bool StartBrowser(std::string* error = nullptr);
		void Stop();

		/// Pumps the socket: sends the periodic beacon and/or drains received ones. nowMs is any
		/// monotonic milliseconds source.
		void Tick(uint64_t nowMs);

		/// Fresh hosts, most recently seen first. Ages out silently.
		std::vector<NetLanHostInfo> GetHosts(uint64_t nowMs);

		bool IsBeaconing() const { return m_Beaconing; }
		bool IsBrowsing() const { return m_Browsing; }

		/// The machine's primary outbound IPv4 (the address LAN peers can reach), "" when unknown.
		static std::string GetPrimaryLocalAddress();

	private:
		bool EnsureSocket(bool bindListenPort, std::string* error);

		intptr_t m_Socket = -1;
		bool m_Beaconing = false;
		bool m_Browsing = false;
		uint64_t m_NextBeaconMs = 0;
		std::vector<uint8_t> m_BeaconPayload;
		std::vector<NetLanHostInfo> m_Hosts;
	};

} // namespace RTE
