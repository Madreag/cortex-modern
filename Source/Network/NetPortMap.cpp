#include "NetPortMap.h"
#include "NetLanDiscovery.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <cstdio>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>

namespace RTE {

	namespace {
		using SocketHandle =
#ifdef _WIN32
			SOCKET;
		constexpr SocketHandle c_InvalidSocket = INVALID_SOCKET;
#else
			int;
		constexpr SocketHandle c_InvalidSocket = -1;
#endif

		uint64_t NowMs() {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		void Log(const std::string& line) {
			static std::mutex s_LogMutex;
			std::lock_guard<std::mutex> lock(s_LogMutex);
			std::cout << "[net-port-map] " << line << std::endl;
		}

		void CloseSocket(SocketHandle socketHandle) {
#ifdef _WIN32
			closesocket(socketHandle);
#else
			close(socketHandle);
#endif
		}

		void AppendBe16(std::vector<uint8_t>& out, uint16_t value) {
			out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
			out.push_back(static_cast<uint8_t>(value & 0xFF));
		}

		void AppendBe32(std::vector<uint8_t>& out, uint32_t value) {
			for (int shift = 24; shift >= 0; shift -= 8) {
				out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
			}
		}

		uint16_t ReadBe16(const uint8_t* data) { return static_cast<uint16_t>((data[0] << 8) | data[1]); }

		uint32_t ReadBe32(const uint8_t* data) {
			return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
			       (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
		}

		// Address fields are wire-order byte strings: read little-endian so the result is an
		// s_addr value, directly comparable with ParseIpv4() and assignable to sin_addr.
		uint32_t ReadLe32(const uint8_t* data) {
			return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
			       (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
		}

		std::string Ipv4Text(uint32_t networkOrderAddr) {
			in_addr addr{};
			addr.s_addr = networkOrderAddr;
			char text[INET_ADDRSTRLEN] = {};
			return inet_ntop(AF_INET, &addr, text, sizeof(text)) ? text : "";
		}

		bool ParseIpv4(const std::string& text, uint32_t& networkOrderAddr) {
			in_addr addr{};
			if (inet_pton(AF_INET, text.c_str(), &addr) != 1) {
				return false;
			}
			networkOrderAddr = addr.s_addr;
			return true;
		}

		// Splits "a.b.c.d[:port]" into the host and port; a missing port keeps `defaultPort`.
		bool ParseHostPort(const std::string& text, uint16_t defaultPort, std::string& host, uint16_t& port) {
			const size_t colon = text.rfind(':');
			host = colon == std::string::npos ? text : text.substr(0, colon);
			port = defaultPort;
			uint32_t addr;
			if (!ParseIpv4(host, addr)) {
				return false;
			}
			if (colon != std::string::npos) {
				const long parsed = std::strtol(text.substr(colon + 1).c_str(), nullptr, 10);
				if (parsed <= 0 || parsed > 65535) {
					return false;
				}
				port = static_cast<uint16_t>(parsed);
			}
			return true;
		}

		bool Stopped(const std::atomic<bool>* stop) { return stop != nullptr && stop->load(); }

		bool WaitReadable(SocketHandle socketHandle, uint64_t deadlineMs, const std::atomic<bool>* stop) {
			while (!Stopped(stop)) {
				const uint64_t now = NowMs();
				if (now >= deadlineMs) {
					return false;
				}
				fd_set readSet;
				FD_ZERO(&readSet);
				FD_SET(socketHandle, &readSet);
				timeval tv{};
				const uint64_t remaining = std::min<uint64_t>(deadlineMs - now, 50);
				tv.tv_sec = static_cast<long>(remaining / 1000);
				tv.tv_usec = static_cast<long>((remaining % 1000) * 1000);
				const int ready = select(static_cast<int>(socketHandle) + 1, &readSet, nullptr, nullptr, &tv);
				if (ready > 0) {
					return true;
				}
				if (ready < 0) {
					return false;
				}
			}
			return false;
		}

		std::string DiscoverDefaultGateway() {
#ifdef _WIN32
			// iphlpapi is not on the link line; GetBestRoute is reached through the DLL instead.
			static const HMODULE lib = LoadLibraryA("iphlpapi.dll");
			if (lib == nullptr) {
				return "";
			}
			using GetBestRouteFn = DWORD(WINAPI*)(DWORD, DWORD, PMIB_IPFORWARDROW);
			const auto getBestRoute = reinterpret_cast<GetBestRouteFn>(GetProcAddress(lib, "GetBestRoute"));
			if (getBestRoute == nullptr) {
				return "";
			}
			in_addr destination{};
			inet_pton(AF_INET, "8.8.8.8", &destination);
			MIB_IPFORWARDROW route{};
			if (getBestRoute(destination.s_addr, 0, &route) != NO_ERROR || route.dwForwardNextHop == 0) {
				return "";
			}
			return Ipv4Text(route.dwForwardNextHop);
#elif defined(__linux__)
			std::FILE* table = std::fopen("/proc/net/route", "r");
			if (table == nullptr) {
				return "";
			}
			char line[512];
			(void)std::fgets(line, sizeof(line), table); // the header row
			std::string best;
			unsigned long bestMetric = ~0UL;
			while (std::fgets(line, sizeof(line), table) != nullptr) {
				char iface[32];
				unsigned long destination, gateway, flags, metric;
				if (std::sscanf(line, "%31s %lx %lx %lx %*u %*u %lu", iface, &destination, &gateway, &flags, &metric) != 5) {
					continue;
				}
				if (destination != 0 || (flags & 0x2) == 0 || gateway == 0 || metric >= bestMetric) {
					continue;
				}
				bestMetric = metric;
				// /proc fields print the address little-endian, which is already the s_addr shape.
				best = Ipv4Text(static_cast<uint32_t>(gateway));
			}
			std::fclose(table);
			return best;
#else
			std::string gateway;
			if (std::FILE* pipe = popen("netstat -rn -f inet", "r")) {
				char line[512];
				while (std::fgets(line, sizeof(line), pipe) != nullptr) {
					char first[64], second[64];
					if (std::sscanf(line, "%63s %63s", first, second) != 2) {
						continue;
					}
					if (std::strcmp(first, "default") != 0 && std::strcmp(first, "0.0.0.0") != 0) {
						continue;
					}
					uint32_t addr;
					if (ParseIpv4(second, addr) && addr != 0) {
						gateway = second;
						break;
					}
				}
				pclose(pipe);
			}
			return gateway;
#endif
		}

		/// The socket Wan: every call blocks the calling thread (the worker) and nothing else.
		class SocketWan : public NetPortMapWan {
		public:
			explicit SocketWan(const std::atomic<bool>* stop) : m_Stop(stop) {
#ifdef _WIN32
				WSADATA wsaData;
				(void)WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
			}

			std::string DefaultGateway() override { return DiscoverDefaultGateway(); }
			std::string LocalAddress() override { return NetLanDiscovery::GetPrimaryLocalAddress(); }

			bool UdpExchange(const std::string& host, uint16_t port, const std::vector<uint8_t>& request, std::vector<uint8_t>& reply, uint32_t timeoutMs) override {
				uint32_t addr;
				if (!ParseIpv4(host, addr)) {
					return false;
				}
				const SocketHandle socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
				if (socketHandle == c_InvalidSocket) {
					return false;
				}
				sockaddr_in target{};
				target.sin_family = AF_INET;
				target.sin_port = htons(port);
				target.sin_addr.s_addr = addr;
				const uint64_t deadline = NowMs() + timeoutMs;
				bool got = false;
				if (sendto(socketHandle, reinterpret_cast<const char*>(request.data()), static_cast<int>(request.size()), 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == static_cast<int>(request.size())) {
					while (WaitReadable(socketHandle, deadline, m_Stop)) {
						uint8_t buffer[2048];
						sockaddr_in from{};
#ifdef _WIN32
						int fromLength = sizeof(from);
#else
						socklen_t fromLength = sizeof(from);
#endif
						const int received = recvfrom(socketHandle, reinterpret_cast<char*>(buffer), sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
						if (received <= 0) {
							break;
						}
						// Stray datagrams (a late answer to an earlier attempt) are skipped.
						if (from.sin_addr.s_addr != addr || from.sin_port != htons(port)) {
							continue;
						}
						reply.assign(buffer, buffer + received);
						got = true;
						break;
					}
				}
				CloseSocket(socketHandle);
				return got;
			}

			std::vector<std::string> SsdpDiscover(const std::string& st, uint32_t windowMs) override {
				std::vector<std::string> replies;
				const SocketHandle socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
				if (socketHandle == c_InvalidSocket) {
					return replies;
				}
				const std::string local = LocalAddress();
				uint32_t localAddr;
				if (ParseIpv4(local, localAddr)) {
					in_addr iface{};
					iface.s_addr = localAddr;
					(void)setsockopt(socketHandle, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&iface), sizeof(iface));
				}
				sockaddr_in bindAddr{};
				bindAddr.sin_family = AF_INET;
				bindAddr.sin_port = htons(0);
				bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
				if (bind(socketHandle, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == 0) {
					const std::string query = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: " + st + "\r\n\r\n";
					sockaddr_in target{};
					target.sin_family = AF_INET;
					target.sin_port = htons(1900);
					(void)inet_pton(AF_INET, "239.255.255.250", &target.sin_addr);
					if (sendto(socketHandle, query.data(), static_cast<int>(query.size()), 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == static_cast<int>(query.size())) {
						const uint64_t deadline = NowMs() + windowMs;
						while (WaitReadable(socketHandle, deadline, m_Stop)) {
							char buffer[4096];
							const int received = recvfrom(socketHandle, buffer, sizeof(buffer) - 1, 0, nullptr, nullptr);
							if (received <= 0) {
								break;
							}
							buffer[received] = '\0';
							replies.emplace_back(buffer, static_cast<size_t>(received));
						}
					}
				}
				CloseSocket(socketHandle);
				return replies;
			}

			bool HttpGet(const std::string& url, std::string& body, uint32_t timeoutMs) override {
				std::string host, path;
				uint16_t port;
				if (!SplitHttpUrl(url, host, port, path)) {
					return false;
				}
				const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
				std::string response;
				long status = 0;
				if (!HttpRoundTrip(host, port, request, response, timeoutMs, status)) {
					return false;
				}
				return status == 200 && SplitHttpResponse(response, body);
			}

			bool HttpPostSoap(const std::string& url, const std::string& soapAction, const std::string& body, long& status, std::string& replyBody, uint32_t timeoutMs) override {
				std::string host, path;
				uint16_t port;
				if (!SplitHttpUrl(url, host, port, path)) {
					status = 0;
					return false;
				}
				const std::string request = "POST " + path + " HTTP/1.1\r\nHost: " + host +
				                            "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + soapAction +
				                            "\"\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
				std::string response;
				if (!HttpRoundTrip(host, port, request, response, timeoutMs, status)) {
					return false;
				}
				return SplitHttpResponse(response, replyBody);
			}

		private:
			static bool SplitHttpUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path) {
				if (url.rfind("http://", 0) != 0) {
					return false;
				}
				const std::string rest = url.substr(7);
				const size_t slash = rest.find('/');
				const std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
				path = slash == std::string::npos ? "/" : rest.substr(slash);
				return ParseHostPort(authority, 80, host, port);
			}

			/// A minimal plain-HTTP round trip: non-blocking connect under the deadline, then a
			/// Content-Length-aware read. Only what an IGD speaks is supported (no TLS, no chunked).
			bool HttpRoundTrip(const std::string& host, uint16_t port, const std::string& request, std::string& response, uint32_t timeoutMs, long& status) {
				uint32_t addr;
				if (!ParseIpv4(host, addr)) {
					return false;
				}
				const SocketHandle socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
				if (socketHandle == c_InvalidSocket) {
					return false;
				}
				const uint64_t deadline = NowMs() + timeoutMs;
				sockaddr_in target{};
				target.sin_family = AF_INET;
				target.sin_port = htons(port);
				target.sin_addr.s_addr = addr;
				bool ok = false;
#ifdef _WIN32
				u_long nonBlocking = 1;
				(void)ioctlsocket(socketHandle, FIONBIO, &nonBlocking);
#else
				const int flags = fcntl(socketHandle, F_GETFL, 0);
				if (flags >= 0) {
					(void)fcntl(socketHandle, F_SETFL, flags | O_NONBLOCK);
				}
#endif
				if (connect(socketHandle, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == 0) {
					ok = true;
				} else {
					fd_set writeSet;
					FD_ZERO(&writeSet);
					FD_SET(socketHandle, &writeSet);
					while (!ok && !Stopped(m_Stop) && NowMs() < deadline) {
						timeval tv{};
						const uint64_t remaining = std::min<uint64_t>(deadline - NowMs(), 50);
						tv.tv_sec = static_cast<long>(remaining / 1000);
						tv.tv_usec = static_cast<long>((remaining % 1000) * 1000);
						if (select(static_cast<int>(socketHandle) + 1, nullptr, &writeSet, nullptr, &tv) > 0) {
							int error = 0;
#ifdef _WIN32
							int errorLength = sizeof(error);
#else
							socklen_t errorLength = sizeof(error);
#endif
							ok = getsockopt(socketHandle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &errorLength) == 0 && error == 0;
						}
					}
				}
				if (ok) {
					size_t sent = 0;
					while (ok && sent < request.size() && !Stopped(m_Stop)) {
						const int wrote = send(socketHandle, request.data() + sent, static_cast<int>(request.size() - sent), 0);
						if (wrote > 0) {
							sent += static_cast<size_t>(wrote);
						} else {
							fd_set writeSet;
							FD_ZERO(&writeSet);
							FD_SET(socketHandle, &writeSet);
							if (!WaitWritable(socketHandle, deadline)) {
								ok = false;
							}
						}
					}
					while (ok && !Stopped(m_Stop)) {
						if (!WaitReadable(socketHandle, deadline, m_Stop)) {
							break;
						}
						char buffer[8192];
						const int received = recv(socketHandle, buffer, sizeof(buffer), 0);
						if (received <= 0) {
							break;
						}
						response.append(buffer, static_cast<size_t>(received));
						// Bound a missing Content-Length read; the peer close still ends the body.
						if (response.size() > 262144 || ResponseComplete(response)) {
							break;
						}
					}
				}
				CloseSocket(socketHandle);
				if (response.rfind("HTTP/", 0) == 0) {
					const size_t space = response.find(' ');
					status = space == std::string::npos ? 0 : std::strtol(response.c_str() + space + 1, nullptr, 10);
				}
				return ok && status > 0;
			}

			bool WaitWritable(SocketHandle socketHandle, uint64_t deadlineMs) {
				fd_set writeSet;
				FD_ZERO(&writeSet);
				FD_SET(socketHandle, &writeSet);
				timeval tv{};
				const uint64_t remaining = deadlineMs > NowMs() ? std::min<uint64_t>(deadlineMs - NowMs(), 200) : 0;
				tv.tv_sec = static_cast<long>(remaining / 1000);
				tv.tv_usec = static_cast<long>((remaining % 1000) * 1000);
				return select(static_cast<int>(socketHandle) + 1, nullptr, &writeSet, nullptr, &tv) > 0;
			}

			static bool ResponseComplete(const std::string& response) {
				const size_t headerEnd = response.find("\r\n\r\n");
				if (headerEnd == std::string::npos) {
					return false;
				}
				const std::string headers = response.substr(0, headerEnd);
				const std::string needle = "content-length:";
				std::string lower = headers;
				std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				const size_t at = lower.find(needle);
				if (at == std::string::npos) {
					return false;
				}
				const long length = std::strtol(headers.c_str() + at + needle.size(), nullptr, 10);
				return response.size() - (headerEnd + 4) >= static_cast<size_t>(std::max<long>(length, 0));
			}

			static bool SplitHttpResponse(const std::string& response, std::string& body) {
				const size_t headerEnd = response.find("\r\n\r\n");
				if (headerEnd == std::string::npos) {
					return false;
				}
				body = response.substr(headerEnd + 4);
				return true;
			}

			const std::atomic<bool>* m_Stop;
		};

		const char* c_IgdDeviceType = "urn:schemas-upnp-org:device:InternetGatewayDevice:1";
		const char* c_WanIpService = "urn:schemas-upnp-org:service:WANIPConnection:1";
		const char* c_WanPppService = "urn:schemas-upnp-org:service:WANPPPConnection:1";
		constexpr uint32_t c_UdpTimeoutsMs[] = {250, 500, 1000};
		constexpr uint32_t c_SsdpWindowMs = 2000;
		constexpr uint32_t c_HttpTimeoutMs = 1500;

		std::string ExtractTagValue(const std::string& xml, const char* tag, size_t from = 0, size_t* end = nullptr) {
			const std::string open = std::string("<") + tag + ">";
			const std::string close = std::string("</") + tag + ">";
			const size_t begin = xml.find(open, from);
			if (begin == std::string::npos) {
				return "";
			}
			const size_t valueBegin = begin + open.size();
			const size_t valueEnd = xml.find(close, valueBegin);
			if (valueEnd == std::string::npos) {
				return "";
			}
			if (end != nullptr) {
				*end = valueEnd + close.size();
			}
			return xml.substr(valueBegin, valueEnd - valueBegin);
		}
	} // namespace

	namespace NetPortMapCodec {

		std::vector<uint8_t> EncodeNatPmpAddressRequest() { return {0, 0}; }

		std::vector<uint8_t> EncodeNatPmpMapRequest(uint16_t internalPort, uint16_t suggestedExternalPort, uint32_t lifetimeS) {
			std::vector<uint8_t> out{0, 2, 0, 0};
			AppendBe16(out, internalPort);
			AppendBe16(out, suggestedExternalPort);
			AppendBe32(out, lifetimeS);
			return out;
		}

		bool DecodeNatPmpAddressResponse(const uint8_t* data, size_t size, uint32_t& externalAddr, uint16_t& resultCode) {
			if (size < 12 || data[0] != 0 || data[1] != 0x80) {
				return false;
			}
			resultCode = ReadBe16(data + 2);
			externalAddr = ReadLe32(data + 8);
			return true;
		}

		bool DecodeNatPmpMapResponse(const uint8_t* data, size_t size, uint8_t opcode, uint16_t& internalPort, uint16_t& externalPort, uint32_t& lifetimeS, uint16_t& resultCode) {
			if (size < 16 || data[0] != 0 || data[1] != static_cast<uint8_t>(0x80 | opcode)) {
				return false;
			}
			resultCode = ReadBe16(data + 2);
			internalPort = ReadBe16(data + 8);
			externalPort = ReadBe16(data + 10);
			lifetimeS = ReadBe32(data + 12);
			return true;
		}

		std::vector<uint8_t> EncodePcpMapRequest(uint32_t localAddr, uint16_t internalPort, uint16_t suggestedExternalPort, uint32_t lifetimeS, const uint8_t nonce[12]) {
			std::vector<uint8_t> out{2, 1, 0, 0};
			AppendBe32(out, lifetimeS);
			out.insert(out.end(), 10, 0);
			out.push_back(0xFF);
			out.push_back(0xFF);
			out.push_back(static_cast<uint8_t>((localAddr) & 0xFF));
			out.push_back(static_cast<uint8_t>((localAddr >> 8) & 0xFF));
			out.push_back(static_cast<uint8_t>((localAddr >> 16) & 0xFF));
			out.push_back(static_cast<uint8_t>((localAddr >> 24) & 0xFF));
			out.insert(out.end(), nonce, nonce + 12);
			out.push_back(17); // UDP
			out.insert(out.end(), 3, 0);
			AppendBe16(out, internalPort);
			AppendBe16(out, suggestedExternalPort);
			out.insert(out.end(), 16, 0);
			return out;
		}

		bool DecodePcpMapResponse(const uint8_t* data, size_t size, const uint8_t nonce[12], uint16_t& internalPort, uint16_t& externalPort, uint32_t& externalAddr, uint32_t& lifetimeS, uint8_t& resultCode) {
			if (size < 60 || data[0] != 2 || data[1] != 0x81) {
				return false;
			}
			resultCode = data[3];
			if (std::memcmp(data + 24, nonce, 12) != 0 || data[36] != 17) {
				return false;
			}
			lifetimeS = ReadBe32(data + 4);
			internalPort = ReadBe16(data + 40);
			externalPort = ReadBe16(data + 42);
			const uint8_t* addr = data + 44;
			static const uint8_t kMappedPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
			if (std::memcmp(addr, kMappedPrefix, 12) != 0) {
				return resultCode == 0 ? false : true; // only an IPv4-mapped answer is usable
			}
			externalAddr = ReadLe32(addr + 12);
			return true;
		}

		std::string BuildIgdSoapAddPortMapping(const std::string& serviceType, uint16_t externalPort, uint16_t internalPort, const std::string& internalClient, uint32_t leaseS) {
			return "<?xml version=\"1.0\"?>"
			       "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
			       "<s:Body><u:AddPortMapping xmlns:u=\"" + serviceType + "\">"
			       "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(externalPort) + "</NewExternalPort>"
			       "<NewProtocol>UDP</NewProtocol><NewInternalPort>" + std::to_string(internalPort) + "</NewInternalPort>"
			       "<NewInternalClient>" + internalClient + "</NewInternalClient><NewEnabled>1</NewEnabled>"
			       "<NewLeaseDuration>" + std::to_string(leaseS) + "</NewLeaseDuration></u:AddPortMapping></s:Body></s:Envelope>";
		}

		std::string BuildIgdSoapDeletePortMapping(const std::string& serviceType, uint16_t externalPort) {
			return "<?xml version=\"1.0\"?>"
			       "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
			       "<s:Body><u:DeletePortMapping xmlns:u=\"" + serviceType + "\">"
			       "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(externalPort) + "</NewExternalPort>"
			       "<NewProtocol>UDP</NewProtocol></u:DeletePortMapping></s:Body></s:Envelope>";
		}

		std::string BuildIgdSoapGetExternalIp(const std::string& serviceType) {
			return "<?xml version=\"1.0\"?>"
			       "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
			       "<s:Body><u:GetExternalIPAddress xmlns:u=\"" + serviceType + "\"></u:GetExternalIPAddress></s:Body></s:Envelope>";
		}

		bool ParseIgdSoapReply(const std::string& body, const char* acceptedElement, long& faultCode, std::string& detail) {
			faultCode = 0;
			detail.clear();
			if (body.find("Fault") != std::string::npos) {
				const std::string code = ExtractTagValue(body, "errorCode");
				const std::string description = ExtractTagValue(body, "errorDescription");
				faultCode = std::strtol(code.c_str(), nullptr, 10);
				detail = description.empty() ? "SOAP fault" : description;
				return false;
			}
			if (body.find(acceptedElement) != std::string::npos) {
				return true;
			}
			detail = "unrecognized SOAP reply";
			return false;
		}

		bool ParseIgdExternalIp(const std::string& body, std::string& externalIp) {
			const std::string value = ExtractTagValue(body, "NewExternalIPAddress");
			uint32_t addr;
			if (!ParseIpv4(value, addr)) {
				return false;
			}
			externalIp = value;
			return true;
		}

		std::string ParseSsdpLocation(const std::string& reply) {
			size_t at = 0;
			while (at < reply.size()) {
				const size_t lineEnd = reply.find("\r\n", at);
				const std::string line = reply.substr(at, lineEnd == std::string::npos ? std::string::npos : lineEnd - at);
				const size_t colon = line.find(':');
				if (colon != std::string::npos) {
					std::string name = line.substr(0, colon);
					std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (name == "location") {
						std::string value = line.substr(colon + 1);
						const size_t first = value.find_first_not_of(" \t");
						const size_t last = value.find_last_not_of(" \t");
						return first == std::string::npos ? "" : value.substr(first, last - first + 1);
					}
				}
				if (lineEnd == std::string::npos) {
					break;
				}
				at = lineEnd + 2;
			}
			return "";
		}

		bool ParseIgdService(const std::string& descriptionXml, std::string& serviceType, std::string& controlUrl) {
			std::string foundType, foundUrl;
			size_t at = 0;
			while (true) {
				const size_t serviceBegin = descriptionXml.find("<service>", at);
				if (serviceBegin == std::string::npos) {
					break;
				}
				const size_t serviceEnd = descriptionXml.find("</service>", serviceBegin);
				if (serviceEnd == std::string::npos) {
					break;
				}
				const std::string service = descriptionXml.substr(serviceBegin, serviceEnd - serviceBegin);
				const std::string type = ExtractTagValue(service, "serviceType");
				if (type.find("WANIPConnection") != std::string::npos || type.find("WANPPPConnection") != std::string::npos) {
					const std::string url = ExtractTagValue(service, "controlURL");
					if (!url.empty()) {
						if (type.find("WANIPConnection") != std::string::npos) {
							serviceType = type;
							controlUrl = url;
							return true;
						}
						if (foundType.empty()) {
							foundType = type;
							foundUrl = url;
						}
					}
				}
				at = serviceEnd;
			}
			if (!foundType.empty()) {
				serviceType = foundType;
				controlUrl = foundUrl;
				return true;
			}
			return false;
		}

		std::string ResolveIgdUrl(const std::string& baseUrl, const std::string& controlUrl) {
			if (controlUrl.rfind("http://", 0) == 0 || controlUrl.rfind("https://", 0) == 0) {
				return controlUrl;
			}
			if (baseUrl.rfind("http://", 0) != 0) {
				return "";
			}
			const std::string rest = baseUrl.substr(7);
			const size_t slash = rest.find('/');
			const std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
			if (controlUrl.empty()) {
				return "";
			}
			return "http://" + authority + (controlUrl.front() == '/' ? "" : "/") + controlUrl;
		}

		bool SameSlash24(const std::string& addrA, const std::string& addrB) {
			uint32_t a, b;
			if (!ParseIpv4(addrA, a) || !ParseIpv4(addrB, b)) {
				return false;
			}
			return (ntohl(a) >> 8) == (ntohl(b) >> 8);
		}

	} // namespace NetPortMapCodec

	namespace {
		NetPortMap::Options s_ProbeOverrides;
	}

	namespace NetPortMapSelfTest {
		bool HttpGetThroughSockets(const std::string& url, std::string& body, uint32_t timeoutMs) {
			SocketWan wan(nullptr);
			return wan.HttpGet(url, body, timeoutMs);
		}
	}

	void NetPortMap::SetProbeOverrides(const std::string& gateway, const std::string& igdLocation) {
		s_ProbeOverrides.gateway = gateway;
		s_ProbeOverrides.igdLocation = igdLocation;
	}

	NetPortMap::Options NetPortMap::ProbeOverrides() { return s_ProbeOverrides; }

	const char* NetPortMap::MethodName(Method method) {
		switch (method) {
			case Method::NatPmp: return "natpmp";
			case Method::Pcp: return "pcp";
			case Method::Upnp: return "upnp";
			default: return "none";
		}
	}

	NetPortMap::Result NetPortMap::RunMappingChain(NetPortMapWan& wan, uint16_t internalPort, uint32_t leaseS, const Options& options, const std::atomic<bool>* stop) {
		Result result;
		result.internalPort = internalPort;
		const std::string localAddr = options.localAddress.empty() ? wan.LocalAddress() : options.localAddress;
		Log("request: map udp " + std::to_string(internalPort) + " lease " + std::to_string(leaseS) + "s from " + (localAddr.empty() ? "unknown-local" : localAddr));

		std::string gateway;
		uint16_t gatewayPort = NetPortMapCodec::c_NatPmpPort;
		if (!options.gateway.empty()) {
			std::string host;
			if (ParseHostPort(options.gateway, NetPortMapCodec::c_NatPmpPort, host, gatewayPort)) {
				gateway = host;
			}
			Log("gateway " + (gateway.empty() ? "unparseable override " + options.gateway : gateway + ":" + std::to_string(gatewayPort) + " (override)"));
		} else {
			gateway = wan.DefaultGateway();
			Log(gateway.empty() ? "no default gateway in the routing table" : "gateway " + gateway + " (routing table)");
		}

		uint32_t externalAddr = 0;
		if (!gateway.empty() && !Stopped(stop)) {
			// NAT-PMP: learn the external address first, then ask for the mapping.
			for (size_t attempt = 0; attempt < 3 && externalAddr == 0 && !Stopped(stop); ++attempt) {
				std::vector<uint8_t> reply;
				if (!wan.UdpExchange(gateway, gatewayPort, NetPortMapCodec::EncodeNatPmpAddressRequest(), reply, c_UdpTimeoutsMs[attempt])) {
					Log("natpmp address request attempt " + std::to_string(attempt + 1) + ": no reply");
					continue;
				}
				uint16_t code = 0;
				if (!NetPortMapCodec::DecodeNatPmpAddressResponse(reply.data(), reply.size(), externalAddr, code)) {
					Log("natpmp address reply not understood (" + std::to_string(reply.size()) + " bytes)");
					continue;
				}
				if (code != 0) {
					Log("natpmp refused the address request: result=" + std::to_string(code));
					externalAddr = 0;
					break;
				}
				Log("natpmp external address " + Ipv4Text(externalAddr));
			}
			bool natpmpDead = externalAddr == 0;
			for (size_t attempt = 0; attempt < 3 && !natpmpDead && !Stopped(stop); ++attempt) {
				std::vector<uint8_t> reply;
				if (!wan.UdpExchange(gateway, gatewayPort, NetPortMapCodec::EncodeNatPmpMapRequest(internalPort, internalPort, leaseS), reply, c_UdpTimeoutsMs[attempt])) {
					Log("natpmp map attempt " + std::to_string(attempt + 1) + ": no reply");
					continue;
				}
				uint16_t gotInternal = 0, externalPort = 0, code = 0;
				uint32_t lifetime = 0;
				if (!NetPortMapCodec::DecodeNatPmpMapResponse(reply.data(), reply.size(), 2, gotInternal, externalPort, lifetime, code)) {
					Log("natpmp map reply not understood (" + std::to_string(reply.size()) + " bytes)");
					continue;
				}
				if (code != 0) {
					Log("natpmp refused the mapping: result=" + std::to_string(code));
					break;
				}
				if (gotInternal != internalPort || externalPort == 0 || lifetime == 0) {
					Log("natpmp answered a different mapping or a zero lease");
					break;
				}
				result.method = Method::NatPmp;
				result.externalIp = Ipv4Text(externalAddr);
				result.externalPort = externalPort;
				result.leaseS = lifetime;
				result.leaseExpiresMs = NowMs() + static_cast<uint64_t>(lifetime) * 1000;
				result.gateway = gateway;
				result.gatewayPort = gatewayPort;
				break;
			}

			// PCP: one MAP exchange carries the external address and the port together.
			if (result.method == Method::None && !Stopped(stop)) {
				uint8_t nonce[12];
				{
					std::random_device device;
					for (uint8_t& byte : nonce) {
						byte = static_cast<uint8_t>(device());
					}
				}
				uint32_t localAddrNum = 0;
				(void)ParseIpv4(localAddr, localAddrNum);
				bool pcpDead = false;
				for (size_t attempt = 0; attempt < 3 && !pcpDead && !Stopped(stop); ++attempt) {
					std::vector<uint8_t> reply;
					if (!wan.UdpExchange(gateway, gatewayPort, NetPortMapCodec::EncodePcpMapRequest(localAddrNum, internalPort, internalPort, leaseS, nonce), reply, c_UdpTimeoutsMs[attempt])) {
						Log("pcp map attempt " + std::to_string(attempt + 1) + ": no reply");
						continue;
					}
					uint16_t gotInternal = 0, externalPort = 0;
					uint32_t assignedAddr = 0, lifetime = 0;
					uint8_t code = 0;
					if (!NetPortMapCodec::DecodePcpMapResponse(reply.data(), reply.size(), nonce, gotInternal, externalPort, assignedAddr, lifetime, code)) {
						Log("pcp map reply not understood (" + std::to_string(reply.size()) + " bytes)");
						continue;
					}
					if (code != 0) {
						Log("pcp refused the mapping: result=" + std::to_string(code));
						pcpDead = true;
						break;
					}
					if (gotInternal != internalPort || externalPort == 0 || lifetime == 0) {
						Log("pcp answered a different mapping or a zero lease");
						pcpDead = true;
						break;
					}
					result.method = Method::Pcp;
					result.externalIp = Ipv4Text(assignedAddr);
					result.externalPort = externalPort;
					result.leaseS = lifetime;
					result.leaseExpiresMs = NowMs() + static_cast<uint64_t>(lifetime) * 1000;
					result.gateway = gateway;
					result.gatewayPort = gatewayPort;
					break;
				}
			}
		}

		// UPnP IGD: an override names the description URL; otherwise SSDP finds it, gated to the /24.
		if (result.method == Method::None && !Stopped(stop)) {
			std::string serviceType, controlUrl;
			if (!options.igdLocation.empty()) {
				Log("igd description from override " + options.igdLocation);
				std::string description;
				if (wan.HttpGet(options.igdLocation, description, c_HttpTimeoutMs) &&
				    NetPortMapCodec::ParseIgdService(description, serviceType, controlUrl)) {
					controlUrl = NetPortMapCodec::ResolveIgdUrl(options.igdLocation, controlUrl);
				} else {
					Log("igd override answered no WANIPConnection/WANPPPConnection service");
				}
			} else {
				Log("ssdp m-search for InternetGatewayDevice (2s window)");
				for (const std::string& answer : wan.SsdpDiscover(c_IgdDeviceType, c_SsdpWindowMs)) {
					const std::string location = NetPortMapCodec::ParseSsdpLocation(answer);
					if (location.empty()) {
						continue;
					}
					std::string host;
					uint16_t port;
					{
						const size_t schemeEnd = location.find("://");
						const std::string rest = schemeEnd == std::string::npos ? location : location.substr(schemeEnd + 3);
						const size_t slash = rest.find('/');
						if (!ParseHostPort(slash == std::string::npos ? rest : rest.substr(0, slash), 80, host, port)) {
							continue;
						}
					}
					if (!NetPortMapCodec::SameSlash24(host, localAddr)) {
						Log("igd at " + host + " refused: outside the /24 of " + localAddr);
						continue;
					}
					std::string description;
					if (!wan.HttpGet(location, description, c_HttpTimeoutMs)) {
						Log("igd at " + host + " served no description");
						continue;
					}
					if (!NetPortMapCodec::ParseIgdService(description, serviceType, controlUrl)) {
						Log("igd at " + host + " lists no WANIPConnection/WANPPPConnection service");
						continue;
					}
					controlUrl = NetPortMapCodec::ResolveIgdUrl(location, controlUrl);
					{
						const size_t schemeEnd = controlUrl.find("://");
						const std::string rest = schemeEnd == std::string::npos ? controlUrl : controlUrl.substr(schemeEnd + 3);
						const size_t slash = rest.find('/');
						std::string controlHost;
						uint16_t controlPort;
						if (!ParseHostPort(slash == std::string::npos ? rest : rest.substr(0, slash), 80, controlHost, controlPort) ||
						    !NetPortMapCodec::SameSlash24(controlHost, localAddr)) {
							Log("igd control URL at " + (controlHost.empty() ? controlUrl : controlHost) + " refused: outside the /24 of " + localAddr);
							controlUrl.clear();
							serviceType.clear();
							continue;
						}
					}
					break;
				}
			}
			for (int variant = 0; !controlUrl.empty() && variant < 3 && result.method == Method::None && !Stopped(stop); ++variant) {
				const uint16_t wantedExternal = static_cast<uint16_t>(internalPort + variant);
				const std::string action = serviceType + "#AddPortMapping";
				long status = 0;
				std::string body;
				if (!wan.HttpPostSoap(controlUrl, action, NetPortMapCodec::BuildIgdSoapAddPortMapping(serviceType, wantedExternal, internalPort, localAddr, leaseS), status, body, c_HttpTimeoutMs)) {
					Log("upnp AddPortMapping to " + controlUrl + ": no reply");
					break;
				}
				long fault = 0;
				std::string detail;
				if (!NetPortMapCodec::ParseIgdSoapReply(body, "AddPortMappingResponse", fault, detail)) {
					if (fault == 718 && variant + 1 < 3) {
						Log("upnp AddPortMapping " + std::to_string(wantedExternal) + " refused: 718 ConflictInMappingEntry, trying the next port");
						continue;
					}
					Log("upnp AddPortMapping refused: " + detail + (fault ? " (code " + std::to_string(fault) + ")" : ""));
					break;
				}
				std::string externalIp;
				long ipStatus = 0;
				std::string ipBody;
				if (!wan.HttpPostSoap(controlUrl, serviceType + "#GetExternalIPAddress", NetPortMapCodec::BuildIgdSoapGetExternalIp(serviceType), ipStatus, ipBody, c_HttpTimeoutMs) ||
				    !NetPortMapCodec::ParseIgdExternalIp(ipBody, externalIp)) {
					Log("upnp mapped but GetExternalIPAddress gave no address");
					break;
				}
				result.method = Method::Upnp;
				result.externalIp = externalIp;
				result.externalPort = wantedExternal;
				result.leaseS = leaseS;
				result.leaseExpiresMs = NowMs() + static_cast<uint64_t>(leaseS) * 1000;
				result.controlUrl = controlUrl;
				result.serviceType = serviceType;
			}
		}

		if (result.method == Method::None) {
			result.error = Stopped(stop) ? "cancelled" : "no method answered";
			Log("failed: " + result.error);
			return result;
		}
		Log("mapped udp " + std::to_string(internalPort) + " -> " + result.externalIp + ":" + std::to_string(result.externalPort) +
		    " via " + MethodName(result.method) + " lease " + std::to_string(result.leaseS));
		return result;
	}

	std::string NetPortMap::RunReleaseChain(NetPortMapWan& wan, const Result& mapped, const Options& options, const std::atomic<bool>* stop) {
		std::string detail;
		switch (mapped.method) {
			case Method::NatPmp: {
				std::string gateway = mapped.gateway;
				uint16_t gatewayPort = mapped.gatewayPort ? mapped.gatewayPort : NetPortMapCodec::c_NatPmpPort;
				if (!options.gateway.empty()) {
					ParseHostPort(options.gateway, gatewayPort, gateway, gatewayPort);
				} else if (gateway.empty()) {
					gateway = wan.DefaultGateway();
				}
				std::vector<uint8_t> reply;
				uint16_t code = 0, internal = 0, external = 0;
				uint32_t lifetime = 0;
				if (!gateway.empty() && wan.UdpExchange(gateway, gatewayPort, NetPortMapCodec::EncodeNatPmpMapRequest(mapped.internalPort, 0, 0), reply, 1000) &&
				    NetPortMapCodec::DecodeNatPmpMapResponse(reply.data(), reply.size(), 2, internal, external, lifetime, code)) {
					detail = "natpmp delete result=" + std::to_string(code);
				} else {
					detail = "natpmp delete: no reply";
				}
				break;
			}
			case Method::Pcp: {
				std::string gateway = mapped.gateway;
				uint16_t gatewayPort = mapped.gatewayPort ? mapped.gatewayPort : NetPortMapCodec::c_NatPmpPort;
				if (!options.gateway.empty()) {
					ParseHostPort(options.gateway, gatewayPort, gateway, gatewayPort);
				} else if (gateway.empty()) {
					gateway = wan.DefaultGateway();
				}
				uint8_t nonce[12];
				{
					std::random_device device;
					for (uint8_t& byte : nonce) {
						byte = static_cast<uint8_t>(device());
					}
				}
				uint32_t localAddrNum = 0;
				(void)ParseIpv4(options.localAddress.empty() ? wan.LocalAddress() : options.localAddress, localAddrNum);
				std::vector<uint8_t> reply;
				uint16_t internal = 0, external = 0;
				uint32_t addr = 0, lifetime = 0;
				uint8_t code = 0;
				if (!gateway.empty() && wan.UdpExchange(gateway, gatewayPort, NetPortMapCodec::EncodePcpMapRequest(localAddrNum, mapped.internalPort, 0, 0, nonce), reply, 1000) &&
				    NetPortMapCodec::DecodePcpMapResponse(reply.data(), reply.size(), nonce, internal, external, addr, lifetime, code)) {
					detail = "pcp delete result=" + std::to_string(code);
				} else {
					detail = "pcp delete: no reply";
				}
				break;
			}
			case Method::Upnp: {
				long status = 0;
				std::string body;
				if (mapped.controlUrl.empty() ||
				    !wan.HttpPostSoap(mapped.controlUrl, mapped.serviceType + "#DeletePortMapping",
				                      NetPortMapCodec::BuildIgdSoapDeletePortMapping(mapped.serviceType, mapped.externalPort), status, body, c_HttpTimeoutMs)) {
					detail = "upnp DeletePortMapping: no reply";
					break;
				}
				long fault = 0;
				detail = NetPortMapCodec::ParseIgdSoapReply(body, "DeletePortMappingResponse", fault, detail)
				             ? "upnp DeletePortMapping ok"
				             : "upnp DeletePortMapping refused: " + detail;
				break;
			}
			default:
				detail = "nothing mapped";
				break;
		}
		Log("release udp " + std::to_string(mapped.internalPort) + " via " + MethodName(mapped.method) + ": " + detail);
		return detail;
	}

	void NetPortMap::Request(uint16_t internalUdpPort, uint32_t leaseSeconds, const Options& options) {
		Release();
		m_Options = options;
		m_Port = internalUdpPort;
		m_LeaseS = leaseSeconds;
		m_Result = Result{};
		m_Result.internalPort = internalUdpPort;
		m_MappedResult = Result{};
		m_Done = false;
		m_Mapped = false;
		m_Released = false;
		m_RenewAtMs = UINT64_MAX;
		m_Cancel.store(false);
		StartWorker(false);
	}

	void NetPortMap::StartWorker(bool renewal) {
		if (m_Worker.joinable()) {
			Log("refusing a second worker while one is still running");
			return;
		}
		m_Worker = std::thread([this]() {
			std::unique_ptr<SocketWan> owned;
			NetPortMapWan* wan = m_Options.wan;
			if (wan == nullptr) {
				owned = std::make_unique<SocketWan>(&m_Cancel);
				wan = owned.get();
			}
			Result result = RunMappingChain(*wan, m_Port, m_LeaseS, m_Options, &m_Cancel);
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_PendingResult = std::move(result);
				m_ResultReady.store(true);
			}
		});
		(void)renewal;
	}

	void NetPortMap::JoinWorker() {
		if (m_Worker.joinable()) {
			m_Worker.join();
		}
	}

	void NetPortMap::Update(uint64_t nowMs) {
		if (m_ResultReady.load()) {
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_Result = m_PendingResult;
				m_ResultReady.store(false);
			}
			JoinWorker();
			m_Done = true;
			if (m_Result.method != Method::None) {
				m_MappedResult = m_Result;
				m_Mapped = true;
				m_RenewAtMs = m_Result.leaseS > 0
				                  ? m_Result.leaseExpiresMs - static_cast<uint64_t>(m_Result.leaseS) * 500
				                  : UINT64_MAX;
			} else if (m_MappedResult.method != Method::None) {
				m_Mapped = nowMs < m_MappedResult.leaseExpiresMs;
				m_RenewAtMs = UINT64_MAX;
			} else {
				m_Mapped = false;
				m_RenewAtMs = UINT64_MAX;
			}
		}
		if (m_MappedResult.method != Method::None && !m_Worker.joinable() && !m_ResultReady && nowMs >= m_MappedResult.leaseExpiresMs) {
			m_Mapped = false;
		}
		if (m_Mapped && nowMs >= m_RenewAtMs && !m_Worker.joinable() && !m_ResultReady) {
			Log("renewing the lease for udp " + std::to_string(m_Port));
			m_Done = false;
			m_Cancel.store(false);
			StartWorker(true);
		}
	}

	void NetPortMap::Release() {
		if (!m_Worker.joinable() && !m_ResultReady && m_MappedResult.method == Method::None && m_Result.method == Method::None && !m_Mapped && m_Released) {
			return;
		}
		m_Cancel.store(true);
		JoinWorker();
		// Release the last result that actually mapped, not a failed or cancelled attempt.
		Result mapped = m_MappedResult;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_ResultReady.load()) {
				m_Result = m_PendingResult;
				m_ResultReady.store(false);
				if (m_Result.method != Method::None) {
					m_MappedResult = m_Result;
					mapped = m_Result;
				}
			}
		}
		if (!m_Released && mapped.method != Method::None) {
			const Options options = m_Options;
			// The removal reply must be logged, so the release runs on its own thread and is joined:
			// bounded by the chain's own per-call timeouts, never by a frame.
			std::thread release([mapped, options]() {
				std::unique_ptr<SocketWan> owned;
				NetPortMapWan* wan = options.wan;
				if (wan == nullptr) {
					owned = std::make_unique<SocketWan>(nullptr);
					wan = owned.get();
				}
				RunReleaseChain(*wan, mapped, options, nullptr);
			});
			release.join();
			m_Mapped = false;
			m_MappedResult = Result{};
			m_RenewAtMs = UINT64_MAX;
		}
		m_Released = true;
		m_Cancel.store(false);
	}

} // namespace RTE
