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
		} // namespace

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
			std::cout << "[net-port-map-selftest] PASS" << std::endl;
			return 0;
		}

	} // namespace NetPortMapSelfTest

} // namespace RTE
