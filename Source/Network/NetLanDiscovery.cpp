#include "NetLanDiscovery.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
static constexpr SocketHandle c_InvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
static constexpr SocketHandle c_InvalidSocket = -1;
#endif

#include <algorithm>
#include <cstring>

namespace RTE {

	namespace {
		void AppendU16(std::vector<uint8_t>& out, uint16_t value) {
			out.push_back(static_cast<uint8_t>(value & 0xFFU));
			out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
		}

		void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
			for (int shift = 0; shift < 32; shift += 8) {
				out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFU));
			}
		}

		void AppendString(std::vector<uint8_t>& out, const std::string& text) {
			const uint8_t length = static_cast<uint8_t>(std::min<size_t>(text.size(), 255));
			out.push_back(length);
			out.insert(out.end(), text.begin(), text.begin() + length);
		}

		bool ReadString(const uint8_t* data, size_t size, size_t& offset, std::string& out) {
			if (offset >= size) {
				return false;
			}
			const uint8_t length = data[offset++];
			if (offset + length > size) {
				return false;
			}
			out.assign(reinterpret_cast<const char*>(data + offset), length);
			offset += length;
			return true;
		}

		bool SetNonBlocking(SocketHandle socketHandle) {
#ifdef _WIN32
			u_long nonBlocking = 1;
			return ioctlsocket(socketHandle, FIONBIO, &nonBlocking) == 0;
#else
			const int flags = fcntl(socketHandle, F_GETFL, 0);
			return flags >= 0 && fcntl(socketHandle, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
		}

		void CloseSocket(SocketHandle socketHandle) {
#ifdef _WIN32
			closesocket(socketHandle);
#else
			close(socketHandle);
#endif
		}
	} // namespace

	bool NetLanDiscovery::EnsureSocket(bool bindListenPort, std::string* error) {
		if (m_Socket != static_cast<intptr_t>(c_InvalidSocket) && m_Socket != -1) {
			return true;
		}
#ifdef _WIN32
		// GNS initializes winsock when a transport starts, but discovery can run before that.
		WSADATA wsaData;
		(void)WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
		const SocketHandle socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (socketHandle == c_InvalidSocket) {
			if (error) *error = "could not create the discovery socket";
			return false;
		}
		int enable = 1;
		setsockopt(socketHandle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enable), sizeof(enable));
		setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enable), sizeof(enable));
		if (!SetNonBlocking(socketHandle)) {
			CloseSocket(socketHandle);
			if (error) *error = "could not make the discovery socket non-blocking";
			return false;
		}
		sockaddr_in bindAddress{};
		bindAddress.sin_family = AF_INET;
		bindAddress.sin_addr.s_addr = htonl(INADDR_ANY);
		bindAddress.sin_port = bindListenPort ? htons(c_DiscoveryPort) : 0;
		if (bind(socketHandle, reinterpret_cast<const sockaddr*>(&bindAddress), sizeof(bindAddress)) != 0) {
			CloseSocket(socketHandle);
			if (error) *error = "could not bind the discovery socket (is another instance browsing?)";
			return false;
		}
		m_Socket = static_cast<intptr_t>(socketHandle);
		return true;
	}

	bool NetLanDiscovery::StartBeacon(uint16_t gamePort, const std::string& hostName, const std::string& activity, const std::string& mode, uint8_t playerCount, uint8_t maxPlayers, std::string* error) {
		if (!EnsureSocket(false, error)) {
			return false;
		}
		std::vector<uint8_t> payload;
		AppendU32(payload, c_Magic);
		AppendU16(payload, c_Version);
		AppendU16(payload, gamePort);
		payload.push_back(playerCount);
		payload.push_back(maxPlayers);
		AppendString(payload, hostName);
		AppendString(payload, activity);
		AppendString(payload, mode);
		// A hosting lobby calls this every menu frame; keep the send schedule unless the payload
		// actually changed, or the beacon would broadcast every frame instead of once per interval.
		if (m_Beaconing && payload == m_BeaconPayload) {
			return true;
		}
		m_BeaconPayload = std::move(payload);
		m_Beaconing = true;
		m_NextBeaconMs = 0;
		return true;
	}

	bool NetLanDiscovery::StartBrowser(std::string* error) {
		if (m_Browsing) {
			return true;
		}
		if (m_Socket != -1 && m_Beaconing) {
			// The beacon socket has an ephemeral port; browsing needs the listen port. One role per
			// instance is the intended shape (host beacons, joiner browses).
			if (error) *error = "cannot browse while beaconing";
			return false;
		}
		if (!EnsureSocket(true, error)) {
			return false;
		}
		m_Browsing = true;
		return true;
	}

	void NetLanDiscovery::Stop() {
		if (m_Socket != -1) {
			CloseSocket(static_cast<SocketHandle>(m_Socket));
			m_Socket = -1;
		}
		m_Beaconing = false;
		m_Browsing = false;
		m_BeaconPayload.clear();
		m_Hosts.clear();
	}

	void NetLanDiscovery::Tick(uint64_t nowMs) {
		if (m_Socket == -1) {
			return;
		}
		if (m_Beaconing && nowMs >= m_NextBeaconMs) {
			m_NextBeaconMs = nowMs + c_BeaconIntervalMs;
			sockaddr_in broadcastAddress{};
			broadcastAddress.sin_family = AF_INET;
			broadcastAddress.sin_addr.s_addr = htonl(INADDR_BROADCAST);
			broadcastAddress.sin_port = htons(c_DiscoveryPort);
			(void)sendto(static_cast<SocketHandle>(m_Socket), reinterpret_cast<const char*>(m_BeaconPayload.data()), static_cast<int>(m_BeaconPayload.size()), 0,
			             reinterpret_cast<const sockaddr*>(&broadcastAddress), sizeof(broadcastAddress));
			// The loopback fallback keeps same-machine testing honest (some stacks drop self-broadcast).
			broadcastAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			(void)sendto(static_cast<SocketHandle>(m_Socket), reinterpret_cast<const char*>(m_BeaconPayload.data()), static_cast<int>(m_BeaconPayload.size()), 0,
			             reinterpret_cast<const sockaddr*>(&broadcastAddress), sizeof(broadcastAddress));
		}
		if (!m_Browsing) {
			return;
		}
		uint8_t buffer[512];
		while (true) {
			sockaddr_in fromAddress{};
#ifdef _WIN32
			int fromLength = sizeof(fromAddress);
#else
			socklen_t fromLength = sizeof(fromAddress);
#endif
			const int received = recvfrom(static_cast<SocketHandle>(m_Socket), reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
			                              reinterpret_cast<sockaddr*>(&fromAddress), &fromLength);
			if (received <= 0) {
				break;
			}
			if (received < 10) {
				continue;
			}
			size_t offset = 0;
			const uint32_t magic = static_cast<uint32_t>(buffer[0]) | (static_cast<uint32_t>(buffer[1]) << 8) |
			                       (static_cast<uint32_t>(buffer[2]) << 16) | (static_cast<uint32_t>(buffer[3]) << 24);
			const uint16_t version = static_cast<uint16_t>(buffer[4]) | (static_cast<uint16_t>(buffer[5]) << 8);
			if (magic != c_Magic || version != c_Version) {
				continue;
			}
			offset = 6;
			NetLanHostInfo info;
			info.port = static_cast<uint16_t>(buffer[offset]) | (static_cast<uint16_t>(buffer[offset + 1]) << 8);
			offset += 2;
			info.playerCount = buffer[offset++];
			info.maxPlayers = buffer[offset++];
			if (!ReadString(buffer, static_cast<size_t>(received), offset, info.hostName) ||
			    !ReadString(buffer, static_cast<size_t>(received), offset, info.activity) ||
			    !ReadString(buffer, static_cast<size_t>(received), offset, info.mode)) {
				continue;
			}
			char addressText[INET_ADDRSTRLEN] = {};
			inet_ntop(AF_INET, &fromAddress.sin_addr, addressText, sizeof(addressText));
			info.address = addressText;
			info.lastSeenMs = nowMs;
			const auto existing = std::find_if(m_Hosts.begin(), m_Hosts.end(), [&](const NetLanHostInfo& host) {
				return host.address == info.address && host.port == info.port;
			});
			if (existing != m_Hosts.end()) {
				*existing = info;
			} else {
				m_Hosts.push_back(info);
			}
		}
	}

	std::string NetLanDiscovery::GetPrimaryLocalAddress() {
#ifdef _WIN32
		WSADATA wsaData;
		(void)WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
		const SocketHandle socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (socketHandle == c_InvalidSocket) {
			return "";
		}
		// A UDP connect sends nothing; it just routes, so getsockname yields the outbound interface.
		sockaddr_in probeAddress{};
		probeAddress.sin_family = AF_INET;
		probeAddress.sin_port = htons(53);
		inet_pton(AF_INET, "8.8.8.8", &probeAddress.sin_addr);
		std::string result;
		if (connect(socketHandle, reinterpret_cast<const sockaddr*>(&probeAddress), sizeof(probeAddress)) == 0) {
			sockaddr_in localAddress{};
#ifdef _WIN32
			int addressLength = sizeof(localAddress);
#else
			socklen_t addressLength = sizeof(localAddress);
#endif
			if (getsockname(socketHandle, reinterpret_cast<sockaddr*>(&localAddress), &addressLength) == 0) {
				char addressText[INET_ADDRSTRLEN] = {};
				inet_ntop(AF_INET, &localAddress.sin_addr, addressText, sizeof(addressText));
				result = addressText;
			}
		}
		CloseSocket(socketHandle);
		return result;
	}

	std::vector<NetLanHostInfo> NetLanDiscovery::GetHosts(uint64_t nowMs) {
		m_Hosts.erase(std::remove_if(m_Hosts.begin(), m_Hosts.end(), [&](const NetLanHostInfo& host) {
			return nowMs > host.lastSeenMs + c_EntryTtlMs;
		}), m_Hosts.end());
		std::vector<NetLanHostInfo> hosts = m_Hosts;
		// A stable order (not recency) keeps the list from reshuffling under the user's cursor.
		std::sort(hosts.begin(), hosts.end(), [](const NetLanHostInfo& lhs, const NetLanHostInfo& rhs) {
			if (lhs.hostName != rhs.hostName) {
				return lhs.hostName < rhs.hostName;
			}
			if (lhs.address != rhs.address) {
				return lhs.address < rhs.address;
			}
			return lhs.port < rhs.port;
		});
		return hosts;
	}

} // namespace RTE
