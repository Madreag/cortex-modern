#include "NetPortMap.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace RTE {

	namespace NetPortMapSelfTest {

		namespace {
			void PutBe16(std::vector<uint8_t>& out, uint16_t value) {
				out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
				out.push_back(static_cast<uint8_t>(value & 0xFF));
			}
			void PutBe32(std::vector<uint8_t>& out, uint32_t value) {
				for (int shift = 24; shift >= 0; shift -= 8) {
					out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
				}
			}
			uint32_t AddrOf(const char* dotted) {
				in_addr addr{};
				(void)inet_pton(AF_INET, dotted, &addr);
				return addr.s_addr;
			}

			const char* kServiceType = "urn:schemas-upnp-org:service:WANIPConnection:1";

			std::vector<uint8_t> NatPmpAddressReply(uint16_t result, const char* external) {
				std::vector<uint8_t> out{0, 0x80};
				PutBe16(out, result);
				PutBe32(out, 1000);
				const uint32_t addr = AddrOf(external);
				out.push_back(static_cast<uint8_t>(addr & 0xFF));
				out.push_back(static_cast<uint8_t>((addr >> 8) & 0xFF));
				out.push_back(static_cast<uint8_t>((addr >> 16) & 0xFF));
				out.push_back(static_cast<uint8_t>((addr >> 24) & 0xFF));
				return out;
			}

			std::vector<uint8_t> NatPmpMapReply(uint16_t result, uint16_t internal, uint16_t external, uint32_t lifetime) {
				std::vector<uint8_t> out{0, 0x82};
				PutBe16(out, result);
				PutBe32(out, 1000);
				PutBe16(out, internal);
				PutBe16(out, external);
				PutBe32(out, lifetime);
				return out;
			}

			std::vector<uint8_t> PcpMapReply(const uint8_t nonce[12], uint8_t result, uint16_t internal, uint16_t external, const char* externalAddr, uint32_t lifetime) {
				std::vector<uint8_t> out{2, 0x81, 0, result};
				PutBe32(out, lifetime);
				PutBe32(out, 77); // epoch
				out.insert(out.end(), 12, 0);
				out.insert(out.end(), nonce, nonce + 12);
				out.push_back(17);
				out.insert(out.end(), 3, 0);
				PutBe16(out, internal);
				PutBe16(out, external);
				out.insert(out.end(), 10, 0);
				out.push_back(0xFF);
				out.push_back(0xFF);
				const uint32_t addr = AddrOf(externalAddr);
				out.push_back(static_cast<uint8_t>(addr & 0xFF));
				out.push_back(static_cast<uint8_t>((addr >> 8) & 0xFF));
				out.push_back(static_cast<uint8_t>((addr >> 16) & 0xFF));
				out.push_back(static_cast<uint8_t>((addr >> 24) & 0xFF));
				return out;
			}

			/// The no-socket Wan: each method consults the installed script and appends to `calls`.
			class ScriptedWan : public NetPortMapWan {
			public:
				std::string gateway = "192.168.1.1";
				std::string local = "192.168.1.20";
				std::vector<std::string> calls;
				std::vector<std::vector<uint8_t>> udpRequests;
				std::function<bool(const std::vector<uint8_t>&, std::vector<uint8_t>&)> udpScript;
				std::vector<std::string> ssdpReplies;
				std::function<bool(const std::string&, std::string&)> getScript;
				std::function<bool(const std::string&, const std::string&, const std::string&, long&, std::string&)> postScript;

				std::string DefaultGateway() override {
					calls.push_back("gateway");
					return gateway;
				}
				std::string LocalAddress() override { return local; }
				bool UdpExchange(const std::string& host, uint16_t port, const std::vector<uint8_t>& request, std::vector<uint8_t>& reply, uint32_t) override {
					calls.push_back("udp:" + host + ":" + std::to_string(port) + ":" + std::to_string(request.size()));
					udpRequests.push_back(request);
					return udpScript && udpScript(request, reply);
				}
				std::vector<std::string> SsdpDiscover(const std::string&, uint32_t) override {
					calls.push_back("ssdp");
					return ssdpReplies;
				}
				bool HttpGet(const std::string& url, std::string& body, uint32_t) override {
					calls.push_back("get:" + url);
					return getScript && getScript(url, body);
				}
				bool HttpPostSoap(const std::string& url, const std::string& action, const std::string& body, long& status, std::string& replyBody, uint32_t) override {
					calls.push_back("post:" + action);
					return postScript && postScript(url, action, body, status, replyBody);
				}
			};

			const char* kDescription =
				"<?xml version=\"1.0\"?><root><device><deviceList><device><serviceList>"
				"<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
				"<controlURL>/ctl/IPConn</controlURL></service></serviceList></device></deviceList></device></root>";

			const char* kAddMappingOk =
				"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
				"<u:AddPortMappingResponse xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\"></u:AddPortMappingResponse>"
				"</s:Body></s:Envelope>";

			const char* kExternalIpOk =
				"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
				"<u:GetExternalIPAddressResponse xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">"
				"<NewExternalIPAddress>203.0.113.7</NewExternalIPAddress></u:GetExternalIPAddressResponse></s:Body></s:Envelope>";

			const char* kConflict718 =
				"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><s:Fault>"
				"<faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>"
				"<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>718</errorCode>"
				"<errorDescription>ConflictInMappingEntry</errorDescription></UPnPError></detail>"
				"</s:Fault></s:Body></s:Envelope>";

			bool TestNatPmpCodec(std::string* error) {
				const std::vector<uint8_t> addrReq = NetPortMapCodec::EncodeNatPmpAddressRequest();
				if (addrReq.size() != 2 || addrReq[0] != 0 || addrReq[1] != 0) {
					*error = "natpmp address request is not {0,0}";
					return false;
				}
				const std::vector<uint8_t> mapReq = NetPortMapCodec::EncodeNatPmpMapRequest(47603, 47603, 600);
				const uint8_t expected[] = {0, 2, 0, 0, 0xB9, 0xF3, 0xB9, 0xF3, 0, 0, 0x02, 0x58};
				if (mapReq.size() != sizeof(expected) || std::memcmp(mapReq.data(), expected, sizeof(expected)) != 0) {
					*error = "natpmp map request bytes differ (port 47603 = 0xB9F3, lease 600)";
					return false;
				}
				uint32_t extAddr = 0;
				uint16_t code = 0xFFFF;
				const std::vector<uint8_t> addrReply = NatPmpAddressReply(0, "203.0.113.7");
				if (!NetPortMapCodec::DecodeNatPmpAddressResponse(addrReply.data(), addrReply.size(), extAddr, code) || code != 0 || extAddr != AddrOf("203.0.113.7")) {
					*error = "natpmp address reply did not round-trip 203.0.113.7";
					return false;
				}
				uint16_t internal = 0, external = 0;
				uint32_t lifetime = 0;
				const std::vector<uint8_t> mapReply = NatPmpMapReply(0, 47603, 47603, 600);
				if (!NetPortMapCodec::DecodeNatPmpMapResponse(mapReply.data(), mapReply.size(), 2, internal, external, lifetime, code) ||
				    code != 0 || internal != 47603 || external != 47603 || lifetime != 600) {
					*error = "natpmp map reply did not round-trip 47603/47603/600";
					return false;
				}
				const std::vector<uint8_t> refused = NatPmpMapReply(2, 0, 0, 0);
				if (!NetPortMapCodec::DecodeNatPmpMapResponse(refused.data(), refused.size(), 2, internal, external, lifetime, code) || code != 2) {
					*error = "natpmp refusal code 2 was not preserved";
					return false;
				}
				const std::vector<uint8_t> wrongOp = NatPmpAddressReply(0, "203.0.113.7");
				if (NetPortMapCodec::DecodeNatPmpMapResponse(wrongOp.data(), wrongOp.size(), 2, internal, external, lifetime, code)) {
					*error = "an address reply decoded as a map reply";
					return false;
				}
				const uint8_t shortBuffer[] = {0, 0x82, 0, 0};
				if (NetPortMapCodec::DecodeNatPmpMapResponse(shortBuffer, sizeof(shortBuffer), 2, internal, external, lifetime, code)) {
					*error = "a truncated map reply decoded";
					return false;
				}
				return true;
			}

			bool TestPcpCodec(std::string* error) {
				const uint8_t nonce[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
				const std::vector<uint8_t> request = NetPortMapCodec::EncodePcpMapRequest(AddrOf("192.168.1.20"), 47603, 47603, 600, nonce);
				if (request.size() != 60) {
					*error = "pcp map request is not 60 bytes";
					return false;
				}
				if (request[0] != 2 || request[1] != 1 || request[36] != 17) {
					*error = "pcp request header fields differ (version/opcode/protocol)";
					return false;
				}
				if (request[18] != 0xFF || request[19] != 0xFF || std::memcmp(&request[20], "\xC0\xA8\x01\x14", 4) != 0) {
					*error = "pcp request client address is not ::ffff:192.168.1.20";
					return false;
				}
				if (std::memcmp(&request[24], nonce, 12) != 0 || request[40] != 0xB9 || request[41] != 0xF3 || request[42] != 0xB9 || request[43] != 0xF3) {
					*error = "pcp request nonce or port fields differ";
					return false;
				}
				uint16_t internal = 0, external = 0;
				uint32_t extAddr = 0, lifetime = 0;
				uint8_t code = 0xFF;
				const std::vector<uint8_t> reply = PcpMapReply(nonce, 0, 47603, 47603, "203.0.113.7", 600);
				if (!NetPortMapCodec::DecodePcpMapResponse(reply.data(), reply.size(), nonce, internal, external, extAddr, lifetime, code) ||
				    code != 0 || internal != 47603 || external != 47603 || lifetime != 600 || extAddr != AddrOf("203.0.113.7")) {
					*error = "pcp map reply did not round-trip";
					return false;
				}
				const uint8_t otherNonce[12] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
				if (NetPortMapCodec::DecodePcpMapResponse(reply.data(), reply.size(), otherNonce, internal, external, extAddr, lifetime, code)) {
					*error = "a pcp reply with a foreign nonce decoded";
					return false;
				}
				const std::vector<uint8_t> refused = PcpMapReply(nonce, 2, 0, 0, "0.0.0.0", 0);
				if (!NetPortMapCodec::DecodePcpMapResponse(refused.data(), refused.size(), nonce, internal, external, extAddr, lifetime, code) || code != 2) {
					*error = "a pcp refusal did not decode its result code";
					return false;
				}
				return true;
			}

			bool TestIgdSoap(std::string* error) {
				const std::string add = NetPortMapCodec::BuildIgdSoapAddPortMapping(kServiceType, 47603, 47603, "192.168.1.20", 600);
				for (const char* needle : {"<u:AddPortMapping xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">",
				                           "<NewExternalPort>47603</NewExternalPort>", "<NewProtocol>UDP</NewProtocol>",
				                           "<NewInternalPort>47603</NewInternalPort>", "<NewInternalClient>192.168.1.20</NewInternalClient>",
				                           "<NewEnabled>1</NewEnabled>", "<NewLeaseDuration>600</NewLeaseDuration>"}) {
					if (add.find(needle) == std::string::npos) {
						*error = std::string("AddPortMapping body is missing ") + needle;
						return false;
					}
				}
				const std::string del = NetPortMapCodec::BuildIgdSoapDeletePortMapping(kServiceType, 47603);
				if (del.find("<u:DeletePortMapping") == std::string::npos || del.find("<NewExternalPort>47603</NewExternalPort>") == std::string::npos) {
					*error = "DeletePortMapping body is missing its action or port";
					return false;
				}
				long fault = -1;
				std::string detail;
				if (!NetPortMapCodec::ParseIgdSoapReply(kAddMappingOk, "AddPortMappingResponse", fault, detail)) {
					*error = "a canned AddPortMappingResponse did not parse as success";
					return false;
				}
				std::string externalIp;
				if (!NetPortMapCodec::ParseIgdExternalIp(kExternalIpOk, externalIp) || externalIp != "203.0.113.7") {
					*error = "GetExternalIPAddressResponse did not yield 203.0.113.7";
					return false;
				}
				if (NetPortMapCodec::ParseIgdSoapReply(kConflict718, "AddPortMappingResponse", fault, detail) || fault != 718 || detail.find("ConflictInMappingEntry") == std::string::npos) {
					*error = "the 718 ConflictInMappingEntry fault did not parse";
					return false;
				}
				return true;
			}

			bool TestDiscoveryParsers(std::string* error) {
				const std::string reply = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nLOCATION: http://192.168.1.1:80/rootDesc.xml\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
				if (NetPortMapCodec::ParseSsdpLocation(reply) != "http://192.168.1.1:80/rootDesc.xml") {
					*error = "the SSDP LOCATION header did not parse";
					return false;
				}
				if (!NetPortMapCodec::ParseSsdpLocation("HTTP/1.1 200 OK\r\nST: x\r\n\r\n").empty()) {
					*error = "a reply with no LOCATION yielded one";
					return false;
				}
				std::string serviceType, controlUrl;
				if (!NetPortMapCodec::ParseIgdService(kDescription, serviceType, controlUrl) ||
				    serviceType != kServiceType || controlUrl != "/ctl/IPConn") {
					*error = "the description document's WANIPConnection service did not parse";
					return false;
				}
				if (NetPortMapCodec::ParseIgdService("<root><device/></root>", serviceType, controlUrl)) {
					*error = "a document with no IGD service produced one";
					return false;
				}
				if (NetPortMapCodec::ResolveIgdUrl("http://192.168.1.1:80/rootDesc.xml", "/ctl/IPConn") != "http://192.168.1.1:80/ctl/IPConn" ||
				    NetPortMapCodec::ResolveIgdUrl("http://192.168.1.1/rootDesc.xml", "http://10.0.0.1/x") != "http://10.0.0.1/x") {
					*error = "control URL resolution is wrong";
					return false;
				}
				if (!NetPortMapCodec::SameSlash24("192.168.1.20", "192.168.1.1") || NetPortMapCodec::SameSlash24("192.168.1.20", "192.168.2.1") ||
				    NetPortMapCodec::SameSlash24("192.168.1.20", "not-an-ip")) {
					*error = "the /24 gate misjudged its cases";
					return false;
				}
				return true;
			}

			bool TestFallbackOrder(std::string* error) {
				// NAT-PMP answers: the chain must stop there and never touch PCP or HTTP.
				{
					ScriptedWan wan;
					wan.udpScript = [](const std::vector<uint8_t>& request, std::vector<uint8_t>& reply) {
						if (request.size() == 2) {
							reply = NatPmpAddressReply(0, "203.0.113.7");
						} else {
							reply = NatPmpMapReply(0, 47603, 47603, 600);
						}
						return true;
					};
					const NetPortMap::Result result = NetPortMap::RunMappingChain(wan, 47603, 600, NetPortMap::Options{}, nullptr);
					if (result.method != NetPortMap::Method::NatPmp || result.externalIp != "203.0.113.7" || result.externalPort != 47603 || result.leaseS != 600) {
						*error = "the natpmp success case produced a wrong result";
						return false;
					}
					for (const std::string& call : wan.calls) {
						if (call == "ssdp" || call.rfind("post:", 0) == 0 || call.rfind("get:", 0) == 0) {
							*error = "the chain ran " + call + " after natpmp had already mapped";
							return false;
						}
					}
				}
				// NAT-PMP refuses the mapping (result 2), PCP refuses too: the IGD path must run and map.
				{
					ScriptedWan wan;
					wan.udpScript = [](const std::vector<uint8_t>& request, std::vector<uint8_t>& reply) {
						if (request.size() == 2) {
							reply = NatPmpAddressReply(0, "203.0.113.7");
						} else if (request[0] == 0) {
							reply = NatPmpMapReply(2, 0, 0, 0);
						} else {
							std::vector<uint8_t> nonce(request.begin() + 24, request.begin() + 36);
							reply = PcpMapReply(nonce.data(), 2, 0, 0, "0.0.0.0", 0);
						}
						return true;
					};
					wan.ssdpReplies = {"HTTP/1.1 200 OK\r\nLOCATION: http://192.168.1.1:8467/rootDesc.xml\r\n\r\n"};
					wan.getScript = [](const std::string&, std::string& body) {
						body = kDescription;
						return true;
					};
					wan.postScript = [](const std::string&, const std::string& action, const std::string&, long& status, std::string& body) {
						status = 200;
						body = action.find("GetExternalIPAddress") != std::string::npos ? kExternalIpOk : kAddMappingOk;
						return true;
					};
					const NetPortMap::Result result = NetPortMap::RunMappingChain(wan, 47603, 600, NetPortMap::Options{}, nullptr);
					if (result.method != NetPortMap::Method::Upnp || result.externalIp != "203.0.113.7" || result.externalPort != 47603 ||
					    result.controlUrl != "http://192.168.1.1:8467/ctl/IPConn") {
						*error = "the upnp fallback produced a wrong result";
						return false;
					}
					std::vector<std::string> kinds;
					for (const std::string& call : wan.calls) {
						kinds.push_back(call.substr(0, call.find(':')));
					}
					const std::vector<std::string> expected = {"gateway", "udp", "udp", "udp", "ssdp", "get", "post", "post"};
					if (kinds != expected) {
						std::string got;
						for (const std::string& kind : kinds) {
							got += (got.empty() ? "" : ",") + kind;
						}
						*error = "the fallback order was " + got;
						return false;
					}
				}
				// An off-subnet SSDD location is refused and the whole chain fails closed.
				{
					ScriptedWan wan;
					wan.udpScript = [](const std::vector<uint8_t>& request, std::vector<uint8_t>& reply) {
						if (request.size() == 2) {
							reply = NatPmpAddressReply(0, "203.0.113.7");
						} else if (request[0] == 0) {
							reply = NatPmpMapReply(2, 0, 0, 0);
						} else {
							std::vector<uint8_t> nonce(request.begin() + 24, request.begin() + 36);
							reply = PcpMapReply(nonce.data(), 2, 0, 0, "0.0.0.0", 0);
						}
						return true;
					};
					wan.ssdpReplies = {"HTTP/1.1 200 OK\r\nLOCATION: http://10.9.9.9:80/rootDesc.xml\r\n\r\n"};
					const NetPortMap::Result result = NetPortMap::RunMappingChain(wan, 47603, 600, NetPortMap::Options{}, nullptr);
					if (result.method != NetPortMap::Method::None || result.error.empty()) {
						*error = "an off-subnet IGD was accepted";
						return false;
					}
					for (const std::string& call : wan.calls) {
						if (call.rfind("get:", 0) == 0 || call.rfind("post:", 0) == 0) {
							*error = "the chain contacted the off-subnet IGD anyway";
							return false;
						}
					}
				}
				return true;
			}

			uint64_t NowMsForTest() {
				return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
			}

			bool TestReleaseAndRenewal(std::string* error) {
				ScriptedWan wan;
				std::atomic<int> maps{0};
				wan.udpScript = [&maps](const std::vector<uint8_t>& request, std::vector<uint8_t>& reply) {
					if (request.size() == 2) {
						reply = NatPmpAddressReply(0, "203.0.113.7");
						return true;
					}
					// A map request carries the lifetime at bytes 8-11; 0 is the delete.
					const uint32_t lifetime = (static_cast<uint32_t>(request[8]) << 24) | (static_cast<uint32_t>(request[9]) << 16) |
					                          (static_cast<uint32_t>(request[10]) << 8) | request[11];
					reply = NatPmpMapReply(0, 47603, 47603, lifetime == 0 ? 0 : 1);
					if (lifetime != 0) {
						++maps;
					}
					return true;
				};
				NetPortMap::Options options;
				options.wan = &wan;
				options.gateway = "192.168.1.1:5351";
				NetPortMap mapper;
				mapper.Request(47603, 1, options); // a 1 s lease renews at its half-life
				for (int i = 0; i < 400 && !mapper.Done(); ++i) {
					mapper.Update(NowMsForTest());
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
				if (!mapper.Mapped() || mapper.GetResult().method != NetPortMap::Method::NatPmp) {
					*error = "the scripted mapping never landed";
					return false;
				}
				for (int i = 0; i < 400 && maps < 2; ++i) {
					mapper.Update(NowMsForTest());
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
				if (maps < 2) {
					*error = "the half-life renewal never re-requested the mapping";
					return false;
				}
				const size_t udpBefore = wan.udpRequests.size();
				mapper.Release();
				bool sawDelete = false;
				for (size_t i = udpBefore; i < wan.udpRequests.size(); ++i) {
					const std::vector<uint8_t>& request = wan.udpRequests[i];
					if (request.size() == 12 && request[8] == 0 && request[9] == 0 && request[10] == 0 && request[11] == 0) {
						sawDelete = true;
					}
				}
				if (!sawDelete || mapper.Mapped()) {
					*error = "Release() sent no lifetime-0 map request";
					return false;
				}
				return true;
			}

			uint32_t RequestLifetime(const std::vector<uint8_t>& request) {
				if (request.size() < 12) {
					return 0xFFFFFFFFu;
				}
				return (static_cast<uint32_t>(request[8]) << 24) | (static_cast<uint32_t>(request[9]) << 16) |
				       (static_cast<uint32_t>(request[10]) << 8) | request[11];
			}

			int CountLifetimeZero(const std::vector<std::vector<uint8_t>>& requests, size_t from) {
				int deletes = 0;
				for (size_t i = from; i < requests.size(); ++i) {
					const std::vector<uint8_t>& request = requests[i];
					if (request.size() == 12 && RequestLifetime(request) == 0) {
						++deletes;
					}
				}
				return deletes;
			}

			bool WaitUntil(NetPortMap& mapper, const std::function<bool()>& done, int spins, int sleepMs) {
				for (int i = 0; i < spins && !done(); ++i) {
					mapper.Update(NowMsForTest());
					std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
				}
				return done();
			}

			/// Map succeeds, a scripted renewal fails, Update consumes the failure, then Release.
			bool TestFailedRenewalThenRelease(std::string* error) {
				ScriptedWan wan;
				std::atomic<int> maps{0};
				wan.udpScript = [&maps](const std::vector<uint8_t>& request, std::vector<uint8_t>& reply) {
					if (request.size() == 2) {
						reply = NatPmpAddressReply(0, "203.0.113.7");
						return true;
					}
					const uint32_t lifetime = RequestLifetime(request);
					if (lifetime == 0) {
						reply = NatPmpMapReply(0, 47603, 47603, 0);
						return true;
					}
					if (++maps == 1) {
						reply = NatPmpMapReply(0, 47603, 47603, 2);
						return true;
					}
					reply = NatPmpMapReply(2, 0, 0, 0);
					return true;
				};
				NetPortMap::Options options;
				options.wan = &wan;
				options.gateway = "192.168.1.1:5351";
				NetPortMap mapper;
				mapper.Request(47603, 2, options);
				if (!WaitUntil(mapper, [&]() { return mapper.Mapped(); }, 400, 5)) {
					*error = "the scripted mapping never landed before the failed renewal";
					return false;
				}
				if (!WaitUntil(mapper, [&]() { return maps.load() >= 2 && mapper.Done(); }, 400, 10)) {
					*error = "the failed renewal was never consumed";
					return false;
				}
				const size_t udpBefore = wan.udpRequests.size();
				mapper.Release();
				const int deletes = CountLifetimeZero(wan.udpRequests, udpBefore);
				if (deletes != 1) {
					*error = "Release after a failed renewal sent " + std::to_string(deletes) + " lifetime-0 deletes";
					return false;
				}
				return true;
			}

			bool TestRenewalStaysMapped(std::string* error) {
				ScriptedWan wan;
				std::atomic<int> maps{0};
				std::atomic<bool> holdRenewal{true};
				wan.udpScript = [&maps, &holdRenewal](const std::vector<uint8_t>& request, std::vector<uint8_t>& reply) {
					if (request.size() == 2) {
						reply = NatPmpAddressReply(0, "203.0.113.7");
						return true;
					}
					const uint32_t lifetime = RequestLifetime(request);
					if (lifetime == 0) {
						reply = NatPmpMapReply(0, 47603, 47603, 0);
						return true;
					}
					if (++maps == 1) {
						reply = NatPmpMapReply(0, 47603, 47603, 2);
						return true;
					}
					while (holdRenewal.load()) {
						std::this_thread::sleep_for(std::chrono::milliseconds(5));
					}
					reply = NatPmpMapReply(2, 0, 0, 0);
					return true;
				};
				NetPortMap::Options options;
				options.wan = &wan;
				options.gateway = "192.168.1.1:5351";
				NetPortMap mapper;
				mapper.Request(47603, 2, options);
				struct ReleaseHold {
					std::atomic<bool>* flag;
					~ReleaseHold() { flag->store(false); }
				} releaseHold{&holdRenewal};
				if (!WaitUntil(mapper, [&]() { return mapper.Mapped(); }, 400, 5)) {
					*error = "the scripted mapping never landed before renewal";
					return false;
				}
				if (!WaitUntil(mapper, [&]() { return maps.load() >= 2; }, 400, 10)) {
					*error = "the in-flight renewal never started";
					return false;
				}
				mapper.Update(NowMsForTest());
				if (!mapper.Mapped()) {
					*error = "Mapped() was false during an in-flight renewal";
					return false;
				}
				if (mapper.GetResult().externalIp != "203.0.113.7" || mapper.GetResult().externalPort != 47603) {
					*error = "GetResult() lost the external endpoint during renewal";
					return false;
				}
				holdRenewal.store(false);
				if (!WaitUntil(mapper, [&]() { return mapper.Done(); }, 400, 5)) {
					*error = "the failed renewal never landed";
					return false;
				}
				if (!mapper.Mapped()) {
					*error = "Mapped() was false after a failed renewal before lease expiry";
					return false;
				}
				if (mapper.GetResult().externalIp != "203.0.113.7" || mapper.GetResult().externalPort != 47603) {
					*error = "GetResult() lost the mapping after a failed renewal";
					return false;
				}
				mapper.Update(mapper.GetResult().leaseExpiresMs + 1);
				if (mapper.Mapped()) {
					*error = "Mapped() stayed true after the recorded lease expiry";
					return false;
				}
				mapper.Release();
				return true;
			}

			bool TestOffSubnetControlUrl(std::string* error) {
				ScriptedWan wan;
				wan.udpScript = [](const std::vector<uint8_t>& request, std::vector<uint8_t>& reply) {
					if (request.size() == 2) {
						reply = NatPmpAddressReply(0, "203.0.113.7");
					} else if (request[0] == 0) {
						reply = NatPmpMapReply(2, 0, 0, 0);
					} else {
						std::vector<uint8_t> nonce(request.begin() + 24, request.begin() + 36);
						reply = PcpMapReply(nonce.data(), 2, 0, 0, "0.0.0.0", 0);
					}
					return true;
				};
				wan.ssdpReplies = {"HTTP/1.1 200 OK\r\nLOCATION: http://192.168.1.1:8467/rootDesc.xml\r\n\r\n"};
				wan.getScript = [](const std::string&, std::string& body) {
					body = "<?xml version=\"1.0\"?><root><device><serviceList>"
					       "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
					       "<controlURL>http://10.9.9.9/ctl/IPConn</controlURL></service></serviceList></device></root>";
					return true;
				};
				wan.postScript = [](const std::string&, const std::string&, const std::string&, long& status, std::string&) {
					status = 200;
					return true;
				};
				const NetPortMap::Result result = NetPortMap::RunMappingChain(wan, 47603, 600, NetPortMap::Options{}, nullptr);
				if (result.method != NetPortMap::Method::None) {
					*error = "an off-subnet absolute controlURL was accepted";
					return false;
				}
				bool fetched = false;
				for (const std::string& call : wan.calls) {
					if (call.rfind("get:", 0) == 0) {
						fetched = true;
					}
					if (call.rfind("post:", 0) == 0) {
						*error = "SOAP POST was sent to an off-subnet control URL";
						return false;
					}
				}
				if (!fetched) {
					*error = "the on-subnet description was never fetched";
					return false;
				}
				return true;
			}

			bool TestHttpReaderNoContentLength(std::string* error) {
#ifdef _WIN32
				WSADATA wsaData;
				(void)WSAStartup(MAKEWORD(2, 2), &wsaData);
				const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
				if (listener == INVALID_SOCKET) {
					*error = "HTTP reader test could not create a listen socket";
					return false;
				}
				sockaddr_in bindAddr{};
				bindAddr.sin_family = AF_INET;
				bindAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				bindAddr.sin_port = htons(0);
				if (bind(listener, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0 || listen(listener, 1) != 0) {
					closesocket(listener);
					*error = "HTTP reader test could not bind 127.0.0.1";
					return false;
				}
				sockaddr_in bound{};
				int boundLen = sizeof(bound);
				if (getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
					closesocket(listener);
					*error = "HTTP reader test could not read the listen port";
					return false;
				}
				const uint16_t port = ntohs(bound.sin_port);
				const char* kHeaders = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n";
				const char* kPart1 = "FIRST-SEGMENT";
				const char* kPart2 = "SECOND-SEGMENT";
				std::atomic<bool> served{false};
				std::thread server([&]() {
					const SOCKET client = accept(listener, nullptr, nullptr);
					if (client == INVALID_SOCKET) {
						return;
					}
					char discard[512];
					(void)recv(client, discard, sizeof(discard), 0);
					(void)send(client, kHeaders, static_cast<int>(std::strlen(kHeaders)), 0);
					std::this_thread::sleep_for(std::chrono::milliseconds(80));
					(void)send(client, kPart1, static_cast<int>(std::strlen(kPart1)), 0);
					std::this_thread::sleep_for(std::chrono::milliseconds(40));
					(void)send(client, kPart2, static_cast<int>(std::strlen(kPart2)), 0);
					closesocket(client);
					served.store(true);
				});
				std::string body;
				const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/desc.xml";
				const bool got = HttpGetThroughSockets(url, body, 1500);
				server.join();
				closesocket(listener);
				const std::string expected = std::string(kPart1) + kPart2;
				if (!got || body != expected) {
					*error = "HTTP body without Content-Length was not read whole (got " + std::to_string(body.size()) + " bytes)";
					return false;
				}
				(void)served;
				return true;
#else
				(void)error;
				return true;
#endif
			}
		} // namespace

		bool TestDoubleStartGuard(std::string* error) {
			ScriptedWan wan;
			std::atomic<bool> holdFirst{true};
			wan.udpScript = [&holdFirst](const std::vector<uint8_t>& request, std::vector<uint8_t>& reply) {
				if (request.size() == 2) {
					reply = NatPmpAddressReply(0, "203.0.113.7");
					return true;
				}
				while (holdFirst.load()) {
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
				reply = NatPmpMapReply(0, 47603, 47603, 600);
				return true;
			};
			NetPortMap::Options options;
			options.wan = &wan;
			options.gateway = "192.168.1.1:5351";
			NetPortMap mapper;
			mapper.m_Options = options;
			mapper.m_Port = 47603;
			mapper.m_LeaseS = 600;
			mapper.m_Cancel.store(false);
			mapper.StartWorker(false);
			mapper.StartWorker(false);
			holdFirst.store(false);
			for (int i = 0; i < 400 && !mapper.Done(); ++i) {
				mapper.Update(NowMsForTest());
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			if (!mapper.Mapped()) {
				*error = "the first worker did not land after a refused second start";
				mapper.Release();
				return false;
			}
			mapper.Release();
			return true;
		}

		int Run() {
			auto fail = [](const std::string& message) {
				std::cerr << "[net-port-map-selftest] FAIL: " << message << std::endl;
				return 1;
			};
			std::string error;
			if (!TestNatPmpCodec(&error)) return fail(error);
			if (!TestPcpCodec(&error)) return fail(error);
			if (!TestIgdSoap(&error)) return fail(error);
			if (!TestDiscoveryParsers(&error)) return fail(error);
			if (!TestFallbackOrder(&error)) return fail(error);
			if (!TestReleaseAndRenewal(&error)) return fail(error);
			if (!TestFailedRenewalThenRelease(&error)) return fail(error);
			if (!TestRenewalStaysMapped(&error)) return fail(error);
			if (!TestOffSubnetControlUrl(&error)) return fail(error);
			if (!TestHttpReaderNoContentLength(&error)) return fail(error);
			if (!TestDoubleStartGuard(&error)) return fail(error);
			std::cout << "[net-port-map-selftest] PASS" << std::endl;
			return 0;
		}

	} // namespace NetPortMapSelfTest

} // namespace RTE
