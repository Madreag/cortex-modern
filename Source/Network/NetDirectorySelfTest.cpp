#include "NetDirectoryClient.h"
#include "NetDirectoryCodec.h"
#include "NetHttpClient.h"

#include "nlohmann/json.hpp"

#ifdef _WIN32
#include <winsock2.h>
#endif

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace RTE {

	namespace NetDirectorySelfTest {

		namespace {
			using json = nlohmann::json;

			const std::string kHex64A(64, 'a');
			const std::string kHex64B(64, 'b');
			const std::string kHex64C(64, 'c');

			NetDirectoryRegisterRequest SampleRegisterRequest() {
				NetDirectoryRegisterRequest request;
				request.name = "Erol";
				request.activity = "P4 Alpha Duel";
				request.scene = "Grasslands";
				request.mode = "pvp-skirmish";
				request.peerCount = 2;
				request.seatsFree = 1;
				request.gameVersion = "7.0.0";
				request.buildId = "stage2-p2d-local";
				request.networkProtocolVersion = 1;
				request.lockstepCodecVersion = 20;
				request.controllerFrameVersion = 6;
				request.matchConfigHash = kHex64A;
				request.sessionIdentityHash = kHex64B;
				request.moduleManifestHash = kHex64C;
				request.listenPort = 41010;
				request.listenAddrs = {"192.168.1.20"};
				request.joinMode = "ice";
				return request;
			}

			json SampleRegisterJson() {
				return json::parse(NetDirectoryCodec::EncodeRegisterRequest(SampleRegisterRequest()));
			}

			NetDirectorySessionRow SampleRow() {
				NetDirectorySessionRow row;
				row.name = "Erol";
				row.activity = "P4 Alpha Duel";
				row.scene = "Grasslands";
				row.mode = "pvp-skirmish";
				row.peerCount = 2;
				row.seatsFree = 1;
				row.gameVersion = "7.0.0";
				row.buildId = "stage2-p2d-local";
				row.networkProtocolVersion = 1;
				row.lockstepCodecVersion = 20;
				row.controllerFrameVersion = 6;
				row.matchConfigHash = kHex64A;
				row.sessionIdentityHash = kHex64B;
				row.moduleManifestHash = kHex64C;
				row.listenPort = 41010;
				row.listenAddrs = {"192.168.1.20"};
				row.joinMode = "ice";
				row.sessionId = "7b8c9d2e-1111-4222-8333-444455556666";
				row.ageS = 3;
				row.observedIp = "203.0.113.9";
				row.state = "lobby";
				return row;
			}

			json SampleRowJson() {
				return json::parse(NetDirectoryCodec::EncodeSessionRow(SampleRow()));
			}

			NetDirectoryLocalIdentity SampleLocal() {
				NetDirectoryLocalIdentity local;
				local.networkProtocolVersion = 1;
				local.lockstepCodecVersion = 20;
				local.controllerFrameVersion = 6;
				local.sessionIdentityHash = kHex64B;
				local.moduleManifestHash = kHex64C;
				return local;
			}

			NetDirectoryHeartbeatRequest SampleHeartbeatRequest() {
				NetDirectoryHeartbeatRequest request;
				request.token = "token-value";
				request.peerCount = 3;
				request.seatsFree = 0;
				request.listenAddrs = std::vector<std::string>{"10.0.0.8"};
				request.state = "running";
				return request;
			}

			NetDirectorySignalPost SampleSignalPost() {
				NetDirectorySignalPost post;
				post.tokenOrJoinNonce = "joinNonce1";
				post.from = "client:joinNonce1";
				post.to = "host";
				post.payloadB64 = "Y2xpZW50LWhlbGxv";
				return post;
			}

			NetDirectorySignalList SampleSignalList() {
				NetDirectorySignalList list;
				list.signals = {
					NetDirectorySignal{1, "client:joinNonce1", "host", "Y2xpZW50LWhlbGxv"},
					NetDirectorySignal{2, "host", "client:joinNonce1", "aG9zdC1yZXBseQ=="},
				};
				return list;
			}

			template <typename T>
			bool CheckRoundTrip(const char* name, const T& sample, std::string (*encode)(const T&), bool (*decode)(const std::string&, T&, std::string&), std::string* error) {
				const std::string body = encode(sample);
				T decoded;
				std::string reason;
				if (!decode(body, decoded, reason)) {
					*error = std::string(name) + " round-trip decode refused: " + reason;
					return false;
				}
				if (!(decoded == sample)) {
					*error = std::string(name) + " round-trip changed the value";
					return false;
				}
				return true;
			}

			bool TestRoundTrips(std::string* error) {
				if (!CheckRoundTrip("register request", SampleRegisterRequest(), &NetDirectoryCodec::EncodeRegisterRequest, &NetDirectoryCodec::DecodeRegisterRequest, error)) return false;
				if (!CheckRoundTrip("register response", NetDirectoryRegisterResponse{"7b8c9d2e-1111-4222-8333-444455556666", "tok123", 15, 5, "127.0.0.1"}, &NetDirectoryCodec::EncodeRegisterResponse, &NetDirectoryCodec::DecodeRegisterResponse, error)) return false;
				if (!CheckRoundTrip("heartbeat request", SampleHeartbeatRequest(), &NetDirectoryCodec::EncodeHeartbeatRequest, &NetDirectoryCodec::DecodeHeartbeatRequest, error)) return false;
				NetDirectoryHeartbeatRequest bareHeartbeat;
				bareHeartbeat.token = "token-value";
				bareHeartbeat.peerCount = 1;
				bareHeartbeat.seatsFree = 2;
				if (!CheckRoundTrip("bare heartbeat request", bareHeartbeat, &NetDirectoryCodec::EncodeHeartbeatRequest, &NetDirectoryCodec::DecodeHeartbeatRequest, error)) return false;
				if (!CheckRoundTrip("heartbeat response", NetDirectoryHeartbeatResponse{15, 5}, &NetDirectoryCodec::EncodeHeartbeatResponse, &NetDirectoryCodec::DecodeHeartbeatResponse, error)) return false;
				if (!CheckRoundTrip("delete request", NetDirectoryDeleteRequest{"tok123"}, &NetDirectoryCodec::EncodeDeleteRequest, &NetDirectoryCodec::DecodeDeleteRequest, error)) return false;
				if (!CheckRoundTrip("delete response", NetDirectoryDeleteResponse{true}, &NetDirectoryCodec::EncodeDeleteResponse, &NetDirectoryCodec::DecodeDeleteResponse, error)) return false;
				if (!CheckRoundTrip("session row", SampleRow(), &NetDirectoryCodec::EncodeSessionRow, &NetDirectoryCodec::DecodeSessionRow, error)) return false;
				NetDirectoryListResponse list;
				list.sessions = {SampleRow()};
				if (!CheckRoundTrip("list response", list, &NetDirectoryCodec::EncodeListResponse, &NetDirectoryCodec::DecodeListResponse, error)) return false;
				if (!CheckRoundTrip("signal post", SampleSignalPost(), &NetDirectoryCodec::EncodeSignalPost, &NetDirectoryCodec::DecodeSignalPost, error)) return false;
				if (!CheckRoundTrip("signal post response", NetDirectorySignalPostResponse{true, 7}, &NetDirectoryCodec::EncodeSignalPostResponse, &NetDirectoryCodec::DecodeSignalPostResponse, error)) return false;
				if (!CheckRoundTrip("signal list", SampleSignalList(), &NetDirectoryCodec::EncodeSignalList, &NetDirectoryCodec::DecodeSignalList, error)) return false;
				return true;
			}

			using DecodeFn = std::function<bool(const std::string&, std::string&)>;

			DecodeFn DecodeInto(std::function<bool(const std::string&, NetDirectoryRegisterRequest&, std::string&)> fn) {
				return [fn](const std::string& body, std::string& reason) { NetDirectoryRegisterRequest out; return fn(body, out, reason); };
			}
			DecodeFn DecodeInto(std::function<bool(const std::string&, NetDirectoryRegisterResponse&, std::string&)> fn) {
				return [fn](const std::string& body, std::string& reason) { NetDirectoryRegisterResponse out; return fn(body, out, reason); };
			}
			DecodeFn DecodeInto(std::function<bool(const std::string&, NetDirectoryHeartbeatRequest&, std::string&)> fn) {
				return [fn](const std::string& body, std::string& reason) { NetDirectoryHeartbeatRequest out; return fn(body, out, reason); };
			}
			DecodeFn DecodeInto(std::function<bool(const std::string&, NetDirectoryHeartbeatResponse&, std::string&)> fn) {
				return [fn](const std::string& body, std::string& reason) { NetDirectoryHeartbeatResponse out; return fn(body, out, reason); };
			}
			DecodeFn DecodeInto(std::function<bool(const std::string&, NetDirectoryDeleteRequest&, std::string&)> fn) {
				return [fn](const std::string& body, std::string& reason) { NetDirectoryDeleteRequest out; return fn(body, out, reason); };
			}
			DecodeFn DecodeInto(std::function<bool(const std::string&, NetDirectoryDeleteResponse&, std::string&)> fn) {
				return [fn](const std::string& body, std::string& reason) { NetDirectoryDeleteResponse out; return fn(body, out, reason); };
			}
			DecodeFn DecodeInto(std::function<bool(const std::string&, NetDirectorySessionRow&, std::string&)> fn) {
				return [fn](const std::string& body, std::string& reason) { NetDirectorySessionRow out; return fn(body, out, reason); };
			}
			DecodeFn DecodeInto(std::function<bool(const std::string&, NetDirectoryListResponse&, std::string&)> fn) {
				return [fn](const std::string& body, std::string& reason) { NetDirectoryListResponse out; return fn(body, out, reason); };
			}
			DecodeFn DecodeInto(std::function<bool(const std::string&, NetDirectorySignalPost&, std::string&)> fn) {
				return [fn](const std::string& body, std::string& reason) { NetDirectorySignalPost out; return fn(body, out, reason); };
			}
			DecodeFn DecodeInto(std::function<bool(const std::string&, NetDirectorySignalPostResponse&, std::string&)> fn) {
				return [fn](const std::string& body, std::string& reason) { NetDirectorySignalPostResponse out; return fn(body, out, reason); };
			}
			DecodeFn DecodeInto(std::function<bool(const std::string&, NetDirectorySignalList&, std::string&)> fn) {
				return [fn](const std::string& body, std::string& reason) { NetDirectorySignalList out; return fn(body, out, reason); };
			}

			struct BodyCase {
				const char* name;
				json canonical;
				std::vector<const char*> required;
				DecodeFn decode;
			};

			std::vector<BodyCase> BodyCases() {
				std::vector<BodyCase> cases;
				cases.push_back({"register request", SampleRegisterJson(),
					{"name", "activity", "scene", "mode", "peer_count", "seats_free", "game_version", "build_id",
					 "network_protocol_version", "lockstep_codec_version", "controller_frame_version", "match_config_hash",
					 "session_identity_hash", "module_manifest_hash", "listen_port", "listen_addrs", "join_mode"},
					DecodeInto(&NetDirectoryCodec::DecodeRegisterRequest)});
				cases.push_back({"register response", json{{"session_id", "7b8c9d2e-1111-4222-8333-444455556666"}, {"token", "tok123"}, {"expires_in_s", 15}, {"heartbeat_s", 5}, {"observed_ip", "127.0.0.1"}},
					{"session_id", "token", "expires_in_s", "heartbeat_s", "observed_ip"},
					DecodeInto(&NetDirectoryCodec::DecodeRegisterResponse)});
				cases.push_back({"heartbeat request", json{{"token", "tok123"}, {"peer_count", 2}, {"seats_free", 1}, {"listen_addrs", json::array({"10.0.0.8"})}, {"state", "lobby"}},
					{"token", "peer_count", "seats_free"},
					DecodeInto(&NetDirectoryCodec::DecodeHeartbeatRequest)});
				cases.push_back({"heartbeat response", json{{"expires_in_s", 15}, {"heartbeat_s", 5}},
					{"expires_in_s", "heartbeat_s"},
					DecodeInto(&NetDirectoryCodec::DecodeHeartbeatResponse)});
				cases.push_back({"delete request", json{{"token", "tok123"}},
					{"token"},
					DecodeInto(&NetDirectoryCodec::DecodeDeleteRequest)});
				cases.push_back({"delete response", json{{"ok", true}},
					{"ok"},
					DecodeInto(&NetDirectoryCodec::DecodeDeleteResponse)});
				cases.push_back({"session row", SampleRowJson(),
					{"name", "activity", "scene", "mode", "peer_count", "seats_free", "game_version", "build_id",
					 "network_protocol_version", "lockstep_codec_version", "controller_frame_version", "match_config_hash",
					 "session_identity_hash", "module_manifest_hash", "listen_port", "listen_addrs", "join_mode",
					 "session_id", "age_s", "observed_ip", "state"},
					DecodeInto(&NetDirectoryCodec::DecodeSessionRow)});
				cases.push_back({"list response", json{{"sessions", json::array({SampleRowJson()})}},
					{"sessions"},
					DecodeInto(&NetDirectoryCodec::DecodeListResponse)});
				cases.push_back({"signal post", json{{"token_or_join_nonce", "joinNonce1"}, {"from", "client:joinNonce1"}, {"to", "host"}, {"payload_b64", "Y2xpZW50LWhlbGxv"}},
					{"token_or_join_nonce", "from", "to", "payload_b64"},
					DecodeInto(&NetDirectoryCodec::DecodeSignalPost)});
				cases.push_back({"signal post response", json{{"ok", true}, {"seq", 1}},
					{"ok", "seq"},
					DecodeInto(&NetDirectoryCodec::DecodeSignalPostResponse)});
				cases.push_back({"signal list", json{{"signals", json::array({json{{"seq", 1}, {"from", "client:joinNonce1"}, {"to", "host"}, {"payload_b64", "Y2xpZW50LWhlbGxv"}}})}},
					{"signals"},
					DecodeInto(&NetDirectoryCodec::DecodeSignalList)});
				return cases;
			}

			bool TestMissingFieldsRefused(std::string* error) {
				for (const BodyCase& body : BodyCases()) {
					std::string reason;
					if (!body.decode(body.canonical.dump(), reason)) {
						*error = std::string(body.name) + " canonical body refused: " + reason;
						return false;
					}
					for (const char* field : body.required) {
						json broken = body.canonical;
						broken.erase(field);
						reason.clear();
						if (body.decode(broken.dump(), reason)) {
							*error = std::string(body.name) + " accepted a body missing " + field;
							return false;
						}
						if (reason.find(field) == std::string::npos && reason.find("malformed") == std::string::npos) {
							*error = std::string(body.name) + " refusal for missing " + field + " gave an unclear reason: " + reason;
							return false;
						}
					}
				}
				return true;
			}

			bool TestWrongTypesRefused(std::string* error) {
				struct Case {
					const char* name;
					json body;
					DecodeFn decode;
				};
				const std::vector<Case> cases = {
					{"register request name as int", [] { json j = SampleRegisterJson(); j["name"] = 42; return j; }(), DecodeInto(&NetDirectoryCodec::DecodeRegisterRequest)},
					{"register request peer_count as string", [] { json j = SampleRegisterJson(); j["peer_count"] = "2"; return j; }(), DecodeInto(&NetDirectoryCodec::DecodeRegisterRequest)},
					{"register request peer_count as bool", [] { json j = SampleRegisterJson(); j["peer_count"] = true; return j; }(), DecodeInto(&NetDirectoryCodec::DecodeRegisterRequest)},
					{"register request peer_count as float", [] { json j = SampleRegisterJson(); j["peer_count"] = 2.5; return j; }(), DecodeInto(&NetDirectoryCodec::DecodeRegisterRequest)},
					{"register request listen_addrs as string", [] { json j = SampleRegisterJson(); j["listen_addrs"] = "192.168.1.20"; return j; }(), DecodeInto(&NetDirectoryCodec::DecodeRegisterRequest)},
					{"register request join_mode bogus", [] { json j = SampleRegisterJson(); j["join_mode"] = "carrier-pigeon"; return j; }(), DecodeInto(&NetDirectoryCodec::DecodeRegisterRequest)},
					{"register request listen_port zero", [] { json j = SampleRegisterJson(); j["listen_port"] = 0; return j; }(), DecodeInto(&NetDirectoryCodec::DecodeRegisterRequest)},
					{"register request negative version", [] { json j = SampleRegisterJson(); j["lockstep_codec_version"] = -1; return j; }(), DecodeInto(&NetDirectoryCodec::DecodeRegisterRequest)},
					{"heartbeat request token as int", [] { json j = json{{"token", 5}, {"peer_count", 1}, {"seats_free", 1}}; return j; }(), DecodeInto(&NetDirectoryCodec::DecodeHeartbeatRequest)},
					{"heartbeat request state bogus", [] { json j = json{{"token", "t"}, {"peer_count", 1}, {"seats_free", 1}, {"state", "done"}}; return j; }(), DecodeInto(&NetDirectoryCodec::DecodeHeartbeatRequest)},
					{"delete response ok as string", json{{"ok", "true"}}, DecodeInto(&NetDirectoryCodec::DecodeDeleteResponse)},
					{"list response sessions as object", json{{"sessions", json::object()}}, DecodeInto(&NetDirectoryCodec::DecodeListResponse)},
					{"session row state bogus", [] { json j = SampleRowJson(); j["state"] = "exploded"; return j; }(), DecodeInto(&NetDirectoryCodec::DecodeSessionRow)},
					{"signal post bad peer", [] { json j = json{{"token_or_join_nonce", "n"}, {"from", "intruder"}, {"to", "host"}, {"payload_b64", "eA=="}}; return j; }(), DecodeInto(&NetDirectoryCodec::DecodeSignalPost)},
					{"signal post bad base64", [] { json j = json{{"token_or_join_nonce", "n"}, {"from", "host"}, {"to", "client:n"}, {"payload_b64", "not base64!!"}}; return j; }(), DecodeInto(&NetDirectoryCodec::DecodeSignalPost)},
					{"signal post response seq as string", json{{"ok", true}, {"seq", "1"}}, DecodeInto(&NetDirectoryCodec::DecodeSignalPostResponse)},
					{"top level array not object", json::array({1, 2, 3}), DecodeInto(&NetDirectoryCodec::DecodeRegisterRequest)},
					{"top level string not object", json("hello"), DecodeInto(&NetDirectoryCodec::DecodeRegisterRequest)},
				};
				for (const Case& item : cases) {
					std::string reason;
					if (item.decode(item.body.dump(), reason)) {
						*error = std::string("accepted a wrong-typed body: ") + item.name;
						return false;
					}
				}
				return true;
			}

			bool TestOversizeRefused(std::string* error) {
				struct Case {
					const char* name;
					std::string body;
					DecodeFn decode;
				};
				json longName = SampleRegisterJson();
				longName["name"] = std::string(NetDirectoryLimits::c_MaxStringChars + 1, 'x');
				json manyAddrs = SampleRegisterJson();
				manyAddrs["listen_addrs"] = json::array();
				for (size_t i = 0; i <= NetDirectoryLimits::c_MaxListenAddrs; ++i) {
					manyAddrs["listen_addrs"].push_back("10.0.0." + std::to_string(i));
				}
				json padded = SampleRegisterJson();
				padded["pad"] = std::string(NetDirectoryLimits::c_MaxBodyBytes, 'y');
				json bigPayload = json{{"token_or_join_nonce", "n"}, {"from", "host"}, {"to", "client:n"}, {"payload_b64", std::string(NetDirectoryLimits::c_MaxPayloadB64Chars + 4, 'A')}};
				json longPeer = json{{"token_or_join_nonce", "n"}, {"from", "client:" + std::string(70, 'k')}, {"to", "host"}, {"payload_b64", "eA=="}};
				const std::vector<Case> cases = {
					{"string over 64 chars", longName.dump(), DecodeInto(&NetDirectoryCodec::DecodeRegisterRequest)},
					{"more than 8 listen_addrs", manyAddrs.dump(), DecodeInto(&NetDirectoryCodec::DecodeRegisterRequest)},
					{"body over 128 KiB", padded.dump(), DecodeInto(&NetDirectoryCodec::DecodeRegisterRequest)},
					{"payload_b64 over the 64 KiB decoded cap", bigPayload.dump(), DecodeInto(&NetDirectoryCodec::DecodeSignalPost)},
					{"peer over 71 chars", longPeer.dump(), DecodeInto(&NetDirectoryCodec::DecodeSignalPost)},
				};
				for (const Case& item : cases) {
					std::string reason;
					if (item.decode(item.body, reason)) {
						*error = std::string("accepted an oversize body: ") + item.name;
						return false;
					}
				}
				return true;
			}

			bool TestCompatibilityPredicate(std::string* error) {
				const NetDirectorySessionRow row = SampleRow();
				const NetDirectoryLocalIdentity local = SampleLocal();
				std::string reason = "should-be-cleared";
				if (!NetDirectoryCodec::IsJoinable(row, local, &reason) || !reason.empty()) {
					*error = "equal identities were not joinable";
					return false;
				}
				struct FieldCase {
					const char* name;
					NetDirectorySessionRow changed;
				};
				FieldCase cases[] = {
					{"network_protocol_version", SampleRow()},
					{"lockstep_codec_version", SampleRow()},
					{"controller_frame_version", SampleRow()},
					{"session_identity_hash", SampleRow()},
					{"module_manifest_hash", SampleRow()},
				};
				cases[0].changed.networkProtocolVersion = 2;
				cases[1].changed.lockstepCodecVersion = 19;
				cases[2].changed.controllerFrameVersion = 7;
				cases[3].changed.sessionIdentityHash = kHex64A;
				cases[4].changed.moduleManifestHash = kHex64A;
				for (const FieldCase& item : cases) {
					reason.clear();
					if (NetDirectoryCodec::IsJoinable(item.changed, local, &reason)) {
						*error = std::string("joinable despite mismatching ") + item.name;
						return false;
					}
					if (reason.find(item.name) == std::string::npos) {
						*error = std::string("mismatch on ") + item.name + " did not name the field: " + reason;
						return false;
					}
				}
				// build_id and match_config_hash are informational and must not gate joining.
				NetDirectorySessionRow informational = SampleRow();
				informational.buildId = "other-build";
				informational.matchConfigHash = kHex64C;
				if (!NetDirectoryCodec::IsJoinable(informational, local, nullptr)) {
					*error = "informational fields blocked a join";
					return false;
				}
				return true;
			}

			bool TestCannedSequence(std::string* error) {
				std::string reason;
				NetDirectoryRegisterResponse created;
				if (!NetDirectoryCodec::DecodeRegisterResponse(
						R"({"session_id":"7b8c9d2e-1111-4222-8333-444455556666","token":"abcTOK123","expires_in_s":15,"heartbeat_s":5,"observed_ip":"127.0.0.1"})",
						created, reason)) {
					*error = "canned register response refused: " + reason;
					return false;
				}
				if (created.sessionId != "7b8c9d2e-1111-4222-8333-444455556666" || created.token != "abcTOK123" || created.expiresInS != 15 || created.heartbeatS != 5 || created.observedIp != "127.0.0.1") {
					*error = "canned register response decoded the wrong values";
					return false;
				}
				NetDirectoryHeartbeatResponse beat;
				if (!NetDirectoryCodec::DecodeHeartbeatResponse(R"({"expires_in_s":15,"heartbeat_s":5})", beat, reason)) {
					*error = "canned heartbeat response refused: " + reason;
					return false;
				}
				NetDirectoryListResponse listed;
				const std::string listBody = std::string("{\"sessions\":[") + NetDirectoryCodec::EncodeSessionRow(SampleRow()) + "]}";
				if (!NetDirectoryCodec::DecodeListResponse(listBody, listed, reason)) {
					*error = "canned list response refused: " + reason;
					return false;
				}
				if (listed.sessions.size() != 1 || listed.sessions[0].sessionId != created.sessionId) {
					*error = "canned list did not carry the registered session";
					return false;
				}
				NetDirectoryDeleteResponse deleted;
				if (!NetDirectoryCodec::DecodeDeleteResponse(R"({"ok":true})", deleted, reason) || !deleted.ok) {
					*error = "canned delete response refused: " + reason;
					return false;
				}
				return true;
			}

			bool TestHttpClientReuse(std::string* error) {
				NetHttpClient client;
				client.Start("GET", "https://127.0.0.1:1/", {}, "", "");
				while (client.Poll() == NetHttpClient::PollResult::Pending) {
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
				client.Start("GET", "https://127.0.0.1:1/", {}, "", "");
				if (client.Poll() != NetHttpClient::PollResult::Done) {
					*error = "second Start on a used client did not finish an error response";
					return false;
				}
				const NetHttpClient::Response second = client.GetResponse();
				if (second.error != "client already used") {
					*error = "second Start gave \"" + second.error + "\" instead of refusing the used client";
					return false;
				}
				return true;
			}

#ifdef _WIN32
			// A listener that never accepts: the TCP handshake completes in the kernel and the
			// TLS ClientHello sits unread, so the request is guaranteed to be stalled.
			bool OpenSilentListener(SOCKET* listener, std::string* url, std::string* error) {
				WSADATA wsaData;
				(void)WSAStartup(MAKEWORD(2, 2), &wsaData);
				SOCKET opened = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
				sockaddr_in addr{};
				addr.sin_family = AF_INET;
				addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				addr.sin_port = 0;
				if (opened == INVALID_SOCKET || bind(opened, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(opened, 1) != 0) {
					*error = "could not open the stall listener";
					return false;
				}
				sockaddr_in bound{};
				int boundSize = sizeof(bound);
				(void)getsockname(opened, reinterpret_cast<sockaddr*>(&bound), &boundSize);
				*listener = opened;
				*url = "https://127.0.0.1:" + std::to_string(ntohs(bound.sin_port)) + "/";
				return true;
			}

			bool MeasureCancel(const std::string& url, std::string* error) {
				NetHttpClient client;
				client.Start("GET", url, {}, "", "");
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				const bool inFlight = client.Poll() == NetHttpClient::PollResult::Pending;
				const auto begin = std::chrono::steady_clock::now();
				client.Cancel();
				const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
				std::cout << "[net-directory-selftest] http client cancel url=" << url << " in_flight=" << (inFlight ? "true" : "false") << " cancel_ms=" << elapsed << std::endl;
				if (elapsed >= 500) {
					*error = "Cancel() took " + std::to_string(elapsed) + " ms for " + url;
					return false;
				}
				return inFlight;
			}

			bool TestHttpClientCancel(std::string* error) {
				SOCKET listener = INVALID_SOCKET;
				std::string url;
				if (!OpenSilentListener(&listener, &url, error)) {
					return false;
				}
				const bool stalled = MeasureCancel(url, error);
				closesocket(listener);
				if (error->empty() && !stalled) {
					*error = "the silent-listener request was not in flight when Cancel was measured";
					return false;
				}
				if (!error->empty()) {
					return false;
				}
				// Black-hole address per the brief; on hosts where the route fails fast the
				// request is already done and only the timing bound is asserted.
				(void)MeasureCancel("https://10.255.255.1:8443/", error);
				return error->empty();
			}

			// 200 quick cancels on a stalled request then 200 refusals: as close as a selftest
			// gets to a late HANDLE_CLOSING racing the state free; it cannot force the race.
			bool TestHttpClientStress(std::string* error) {
				SOCKET listener = INVALID_SOCKET;
				std::string stalledUrl;
				if (!OpenSilentListener(&listener, &stalledUrl, error)) {
					return false;
				}
				for (int i = 0; i < 200; ++i) {
					NetHttpClient client;
					client.Start("GET", stalledUrl, {}, "", "");
					std::this_thread::sleep_for(std::chrono::milliseconds(1 + (i % 5)));
					client.Cancel();
					const NetHttpClient::Response response = client.GetResponse();
					if (client.Poll() != NetHttpClient::PollResult::Done) {
						closesocket(listener);
						*error = "cancelled request " + std::to_string(i) + " never reported done";
						return false;
					}
					if (response.error.empty()) {
						closesocket(listener);
						*error = "cancelled request " + std::to_string(i) + " finished without an error";
						return false;
					}
				}
				closesocket(listener);
				// A refused async connect takes ~2 s to report here, so the 200 are issued
				// together and joined in order; run strictly serial they would take minutes.
				std::vector<std::unique_ptr<NetHttpClient>> refused;
				refused.reserve(200);
				for (int i = 0; i < 200; ++i) {
					auto client = std::make_unique<NetHttpClient>();
					client->Start("GET", "https://127.0.0.1:1/", {}, "", "");
					refused.push_back(std::move(client));
				}
				const auto joinDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
				for (int i = 0; i < 200; ++i) {
					NetHttpClient& client = *refused[i];
					while (client.Poll() == NetHttpClient::PollResult::Pending && std::chrono::steady_clock::now() < joinDeadline) {
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
					}
					const NetHttpClient::Response response = client.GetResponse();
					if (client.Poll() != NetHttpClient::PollResult::Done) {
						*error = "refused request " + std::to_string(i) + " never reported done";
						return false;
					}
					if (response.error.empty()) {
						*error = "refused request " + std::to_string(i) + " finished without an error";
						return false;
					}
				}
				return true;
			}
#endif

			/// The injected transport: each request records itself and answers the next canned reply.
			class ScriptedTransport final : public NetDirectoryClient::Transport {
			public:
				ScriptedTransport(std::shared_ptr<std::deque<NetDirectoryClient::Reply>> replies, std::shared_ptr<std::vector<NetDirectoryClient::Request>> sent) :
					m_Replies(std::move(replies)), m_Sent(std::move(sent)) {}
				void Start(const NetDirectoryClient::Request& request) override { m_Sent->push_back(request); }
				bool Finished() override { return true; }
				NetDirectoryClient::Reply Take() override {
					NetDirectoryClient::Reply reply = m_Replies->empty() ? NetDirectoryClient::Reply{500, "", ""} : m_Replies->front();
					if (!m_Replies->empty()) {
						m_Replies->pop_front();
					}
					return reply;
				}
				void Abort() override {}

			private:
				std::shared_ptr<std::deque<NetDirectoryClient::Reply>> m_Replies;
				std::shared_ptr<std::vector<NetDirectoryClient::Request>> m_Sent;
			};

			struct ScriptedClient {
				std::shared_ptr<std::deque<NetDirectoryClient::Reply>> replies = std::make_shared<std::deque<NetDirectoryClient::Reply>>();
				std::shared_ptr<std::vector<NetDirectoryClient::Request>> sent = std::make_shared<std::vector<NetDirectoryClient::Request>>();
				NetDirectoryClient client;

				ScriptedClient() {
					auto replies = this->replies;
					auto sent = this->sent;
					client.SetTransportFactory([replies, sent] { return std::make_unique<ScriptedTransport>(replies, sent); });
					client.Configure("https://dir.test", "key0123456789abcd", "");
				}
			};

			bool RequestIs(const NetDirectoryClient::Request& request, const char* method, const char* path, std::string* error) {
				if (request.method != method || request.path != path) {
					*error = "request was " + request.method + " " + request.path + ", expected " + method + " " + path;
					return false;
				}
				return true;
			}

			bool TestClientLifecycle(std::string* error) {
				ScriptedClient s;
				s.replies->push_back({200, R"({"session_id":"7b8c9d2e-1111-4222-8333-444455556666","token":"tok","expires_in_s":15,"heartbeat_s":5,"observed_ip":"127.0.0.1"})", ""});
				s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5})", ""});
				s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5})", ""});
				s.replies->push_back({200, R"({"ok":true})", ""});

				s.client.Advertise(SampleRegisterRequest(), false);
				s.client.Update(0);
				if (s.sent->size() != 1 || !RequestIs(s.sent->at(0), "POST", "/v1/sessions", error)) {
					*error = error->empty() ? "no register request issued" : *error;
					return false;
				}
				s.client.Update(0);
				if (s.client.GetState() != NetDirectoryClient::State::Registered) {
					*error = "client did not register";
					return false;
				}
				s.client.Update(4999); // before the interval: nothing may be sent
				if (s.sent->size() != 1) {
					*error = "a heartbeat left before the interval elapsed";
					return false;
				}
				s.client.Update(5000);
				if (s.sent->size() != 2 || !RequestIs(s.sent->at(1), "POST", "/v1/sessions/7b8c9d2e-1111-4222-8333-444455556666/heartbeat", error)) {
					*error = error->empty() ? "no heartbeat at the interval" : *error;
					return false;
				}
				NetDirectoryHeartbeatRequest heartbeat;
				std::string reason;
				if (!NetDirectoryCodec::DecodeHeartbeatRequest(s.sent->at(1).body, heartbeat, reason) || heartbeat.token != "tok" || heartbeat.peerCount != 2 || heartbeat.seatsFree != 1 || !heartbeat.state || *heartbeat.state != "lobby") {
					*error = "heartbeat body wrong: " + reason;
					return false;
				}
				s.client.Update(5000);
				s.client.Update(9999);
				if (s.sent->size() != 2) {
					*error = "a second heartbeat left before its interval elapsed";
					return false;
				}
				s.client.Update(10000);
				s.client.Update(10000);
				if (s.sent->size() != 3) {
					*error = "the second heartbeat was not sent at the doubled interval";
					return false;
				}
				s.client.Retract();
				s.client.Update(10000);
				if (s.sent->size() != 4 || !RequestIs(s.sent->at(3), "DELETE", "/v1/sessions/7b8c9d2e-1111-4222-8333-444455556666", error)) {
					*error = error->empty() ? "no delete on retract" : *error;
					return false;
				}
				NetDirectoryDeleteRequest deleted;
				if (!NetDirectoryCodec::DecodeDeleteRequest(s.sent->at(3).body, deleted, reason) || deleted.token != "tok") {
					*error = "delete body wrong: " + reason;
					return false;
				}
				s.client.Update(10000);
				if (s.client.GetState() != NetDirectoryClient::State::Idle) {
					*error = "client did not settle after the delete";
					return false;
				}
				const json report = json::parse(s.client.BuildReportJson());
				if (report["registers"] != 1 || report["heartbeats"] != 2 || report["deletes"] != 1) {
					*error = "report counters wrong: " + report.dump();
					return false;
				}
				return true;
			}

			bool TestHeartbeat404Reregisters(std::string* error) {
				ScriptedClient s;
				s.replies->push_back({200, R"({"session_id":"7b8c9d2e-1111-4222-8333-444455556666","token":"tok","expires_in_s":15,"heartbeat_s":5,"observed_ip":"127.0.0.1"})", ""});
				s.replies->push_back({404, R"({"error":"not_found"})", ""});
				s.replies->push_back({200, R"({"session_id":"8c9d2e1f-2222-4333-8444-555566667777","token":"tok2","expires_in_s":15,"heartbeat_s":5,"observed_ip":"127.0.0.1"})", ""});
				s.replies->push_back({404, R"({"error":"not_found"})", ""});

				s.client.Advertise(SampleRegisterRequest(), false);
				s.client.Update(0);
				s.client.Update(0);
				s.client.Update(5000); // heartbeat -> 404
				s.client.Update(5000); // handled: one re-register is issued immediately
				if (s.sent->size() != 3 || !RequestIs(s.sent->at(2), "POST", "/v1/sessions", error)) {
					*error = error->empty() ? "no re-register after the 404" : *error;
					return false;
				}
				s.client.Update(5000); // the second registration lands
				if (s.client.GetState() != NetDirectoryClient::State::Registered || s.client.GetSessionId() != "8c9d2e1f-2222-4333-8444-555566667777") {
					*error = "the re-registered session was not adopted";
					return false;
				}
				s.client.Update(10000); // heartbeat against the new session -> 404 again
				s.client.Update(10000); // the once-only re-register is spent: the client fails closed
				if (s.client.GetState() != NetDirectoryClient::State::Failed || s.sent->size() != 4) {
					*error = "a second 404 did not fail closed";
					return false;
				}
				return true;
			}

			bool TestHeartbeat429HonorsRetryAfter(std::string* error) {
				ScriptedClient s;
				s.replies->push_back({200, R"({"session_id":"7b8c9d2e-1111-4222-8333-444455556666","token":"tok","expires_in_s":15,"heartbeat_s":5,"observed_ip":"127.0.0.1"})", ""});
				s.replies->push_back({429, R"({"error":"rate_limited","retry_after_s":30})", ""});
				s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5})", ""});

				s.client.Advertise(SampleRegisterRequest(), false);
				s.client.Update(0);
				s.client.Update(0);
				s.client.Update(5000);
				s.client.Update(5000); // 429: nothing may leave until retry_after elapses
				s.client.Update(34999);
				if (s.sent->size() != 2) {
					*error = "a request left while the 429 retry_after was pending";
					return false;
				}
				s.client.Update(35000);
				s.client.Update(35000);
				if (s.sent->size() != 3 || !RequestIs(s.sent->at(2), "POST", "/v1/sessions/7b8c9d2e-1111-4222-8333-444455556666/heartbeat", error)) {
					*error = error->empty() ? "the heartbeat did not resume after retry_after" : *error;
					return false;
				}
				return true;
			}

			bool TestTransportErrorBackoff(std::string* error) {
				ScriptedClient s;
				s.replies->push_back({0, "", "send: certificate verification failed"});
				s.replies->push_back({0, "", "connect timed out"});
				s.replies->push_back({0, "", "connect timed out"});
				s.replies->push_back({200, R"({"session_id":"7b8c9d2e-1111-4222-8333-444455556666","token":"tok","expires_in_s":15,"heartbeat_s":5,"observed_ip":"127.0.0.1"})", ""});

				s.client.Advertise(SampleRegisterRequest(), false);
				s.client.Update(0);
				s.client.Update(0); // first failure: retry in 5s
				s.client.Update(4999);
				if (s.sent->size() != 1) {
					*error = "a retry left before the first backoff elapsed";
					return false;
				}
				s.client.Update(5000);
				s.client.Update(5000); // second failure: retry in 10s
				s.client.Update(14999);
				if (s.sent->size() != 2) {
					*error = "a retry left before the second backoff elapsed";
					return false;
				}
				s.client.Update(15000);
				s.client.Update(15000); // third failure: retry in 20s
				s.client.Update(34999);
				if (s.sent->size() != 3) {
					*error = "a retry left before the third backoff elapsed";
					return false;
				}
				s.client.Update(35000);
				s.client.Update(35000);
				if (s.client.GetState() != NetDirectoryClient::State::Registered || s.sent->size() != 4) {
					*error = "the register did not recover after the backoff sequence";
					return false;
				}
				return true;
			}

			bool TestMergeGameLists(std::string* error) {
				NetLanHostInfo lan;
				lan.address = "10.0.0.5";
				lan.port = 42000;
				lan.hostName = "Erol-LAN";
				lan.activity = "P4 Alpha Duel";
				lan.mode = "pvp-skirmish";
				lan.playerCount = 1;
				lan.maxPlayers = 2;

				const NetDirectoryLocalIdentity local = SampleLocal();
				NetDirectorySessionRow joinable = SampleRow();
				NetDirectorySessionRow codecMismatch = SampleRow();
				codecMismatch.lockstepCodecVersion = 99;
				NetDirectorySessionRow protocolMismatch = SampleRow();
				protocolMismatch.networkProtocolVersion = 9;
				NetDirectorySessionRow framesMismatch = SampleRow();
				framesMismatch.controllerFrameVersion = 9;
				NetDirectorySessionRow identityMismatch = SampleRow();
				identityMismatch.sessionIdentityHash = kHex64A;
				NetDirectorySessionRow modulesMismatch = SampleRow();
				modulesMismatch.moduleManifestHash = kHex64A;
				NetDirectorySessionRow full = SampleRow();
				full.seatsFree = 0;

				const std::vector<NetDirectoryClient::GameRow> merged = NetDirectoryClient::MergeGameLists(
					{lan},
					{joinable, codecMismatch, protocolMismatch, framesMismatch, identityMismatch, modulesMismatch, full},
					local);
				if (merged.size() != 8) {
					*error = "merged list size " + std::to_string(merged.size());
					return false;
				}
				const NetDirectoryClient::GameRow& lanRow = merged[0];
				if (lanRow.source != "LAN" || !lanRow.joinable || lanRow.address != "10.0.0.5" || lanRow.port != 42000 || lanRow.players != "1/2") {
					*error = "the LAN row did not merge unchanged";
					return false;
				}
				const NetDirectoryClient::GameRow& netRow = merged[1];
				if (netRow.source != "NET" || !netRow.joinable || !netRow.reason.empty() || netRow.address != "192.168.1.20" || netRow.port != 41010 || netRow.players != "1/2") {
					*error = "the joinable NET row did not carry its address:port";
					return false;
				}
				const std::vector<std::string> expectedReasons = {"codec", "protocol", "controller frames", "identity", "modules", "full"};
				for (size_t i = 0; i < expectedReasons.size(); ++i) {
					const NetDirectoryClient::GameRow& row = merged[2 + i];
					if (row.joinable || row.reason != expectedReasons[i]) {
						*error = "row " + std::to_string(i) + " reason was \"" + row.reason + "\" joinable=" + std::to_string(row.joinable) + ", expected \"" + expectedReasons[i] + "\"";
						return false;
					}
				}
				return true;
			}
		}

		int Run() {
			auto fail = [](const std::string& message) {
				std::cerr << "[net-directory-selftest] FAIL: " << message << std::endl;
				return 1;
			};

			std::string error;
			if (!TestRoundTrips(&error)) return fail(error);
			if (!TestMissingFieldsRefused(&error)) return fail(error);
			if (!TestWrongTypesRefused(&error)) return fail(error);
			if (!TestOversizeRefused(&error)) return fail(error);
			if (!TestCompatibilityPredicate(&error)) return fail(error);
			if (!TestCannedSequence(&error)) return fail(error);
			if (!TestHttpClientReuse(&error)) return fail(error);
			if (!TestClientLifecycle(&error)) return fail(error);
			if (!TestHeartbeat404Reregisters(&error)) return fail(error);
			if (!TestHeartbeat429HonorsRetryAfter(&error)) return fail(error);
			if (!TestTransportErrorBackoff(&error)) return fail(error);
			if (!TestMergeGameLists(&error)) return fail(error);
#ifdef _WIN32
			if (!TestHttpClientCancel(&error)) return fail(error);
			if (!TestHttpClientStress(&error)) return fail(error);
#endif

			std::cout << "[net-directory-selftest] PASS" << std::endl;
			return 0;
		}

	} // namespace NetDirectorySelfTest

} // namespace RTE
