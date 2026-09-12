#include "NetDirectoryClient.h"
#include "NetDirectoryCodec.h"
#include "NetDirectorySignalChannel.h"
#include "NetHttpClient.h"

#include "allegro.h"

#include "ActivityMan.h"
#include "AudioMan.h"
#include "CameraMan.h"
#include "ConsoleMan.h"
#include "FrameMan.h"
#include "MovableMan.h"
#include "PerformanceMan.h"
#include "PresetMan.h"
#include "SceneMan.h"
#include "SettingsMan.h"
#include "TimerMan.h"
#include "UInputMan.h"
#include "WindowMan.h"

#include "Base64/base64.h"
#include "nlohmann/json.hpp"

#ifdef _WIN32
#include <winsock2.h>
#elif defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
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

#if defined(_WIN32) || defined(__APPLE__)
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

#ifdef _WIN32
			using SocketHandle = SOCKET;
			using SocketLength = int;
			constexpr SocketHandle c_InvalidSocket = INVALID_SOCKET;
			void CloseSocket(SocketHandle socketHandle) { closesocket(socketHandle); }
#else
			using SocketHandle = int;
			using SocketLength = socklen_t;
			constexpr SocketHandle c_InvalidSocket = -1;
			void CloseSocket(SocketHandle socketHandle) { close(socketHandle); }
#endif

			bool TestHttpClientCancel(std::string* error) {
#ifdef _WIN32
				WSADATA wsaData;
				(void)WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
				// A listener that never accepts: the TCP handshake completes in the kernel and the
				// TLS ClientHello sits unread, so the request is guaranteed to be stalled.
				SocketHandle listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
				sockaddr_in addr{};
				addr.sin_family = AF_INET;
				addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				addr.sin_port = 0;
				if (listener == c_InvalidSocket || bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 1) != 0) {
					*error = "could not open the stall listener";
					return false;
				}
				sockaddr_in bound{};
				SocketLength boundSize = sizeof(bound);
				(void)getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &boundSize);
				const std::string url = "https://127.0.0.1:" + std::to_string(ntohs(bound.sin_port)) + "/";
				const bool stalled = MeasureCancel(url, error);
				CloseSocket(listener);
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
#endif

			std::string ReadWholeFile(const std::filesystem::path& path) {
				std::ifstream stream(path, std::ios::binary);
				return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
			}

			void SetSettingsPathEnv(const std::string& path) {
#ifdef _WIN32
				_putenv_s("CCCP_SETTINGSPATH", path.c_str());
#else
				if (path.empty()) {
					unsetenv("CCCP_SETTINGSPATH");
				} else {
					setenv("CCCP_SETTINGSPATH", path.c_str(), 1);
				}
#endif
			}

			bool IsInstallKeyHex(const std::string& key) {
				return key.size() == 32 && std::all_of(key.begin(), key.end(), [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
			}

			bool TestInstallKeyIsLazy(std::string* error) {
				// A settings write reads every manager and this selftest runs before the boot builds
				// them, so the ones SettingsMan::Save reads are built here.
				install_allegro(SYSTEM_NONE, &errno, std::atexit);
				if (!TimerMan::IsConstructed()) TimerMan::Construct();
				if (!PresetMan::IsConstructed()) PresetMan::Construct();
				if (!SettingsMan::IsConstructed()) SettingsMan::Construct();
				if (!WindowMan::IsConstructed()) WindowMan::Construct();
				if (!FrameMan::IsConstructed()) FrameMan::Construct();
				if (!AudioMan::IsConstructed()) AudioMan::Construct();
				if (!UInputMan::IsConstructed()) UInputMan::Construct();
				if (!ConsoleMan::IsConstructed()) ConsoleMan::Construct();
				if (!SceneMan::IsConstructed()) SceneMan::Construct();
				if (!MovableMan::IsConstructed()) MovableMan::Construct();
				if (!CameraMan::IsConstructed()) CameraMan::Construct();
				if (!ActivityMan::IsConstructed()) ActivityMan::Construct();
				if (!PerformanceMan::IsConstructed()) PerformanceMan::Construct();

				const std::filesystem::path settingsPath = std::filesystem::temp_directory_path() / "net-directory-install-key.ini";
				std::error_code fileError;
				std::filesystem::remove(settingsPath, fileError);
				{
					std::ofstream file(settingsPath, std::ios::binary | std::ios::trunc);
					file << "SettingsMan\n\tSessionDirectoryUrl = http://127.0.0.1:8099\n";
				}
				const std::string staged = ReadWholeFile(settingsPath);
				SetSettingsPathEnv(settingsPath.generic_string());
				g_SettingsMan.Initialize();
				const std::string afterLoad = ReadWholeFile(settingsPath);

				std::string key;
				if (g_SettingsMan.SettingsNeedOverwrite()) {
					*error = "a settings file without an install key was marked for a rewrite at load";
				} else if (!g_SettingsMan.GetSessionDirectoryInstallKey().empty()) {
					*error = "an install key was generated at load: " + g_SettingsMan.GetSessionDirectoryInstallKey();
				} else if (afterLoad != staged) {
					*error = "loading a settings file without an install key rewrote it";
				} else {
					key = g_SettingsMan.GetOrCreateSessionDirectoryInstallKey();
					const std::string persisted = ReadWholeFile(settingsPath);
					if (!IsInstallKeyHex(key)) {
						*error = "the first directory use returned install key \"" + key + "\"";
					} else if (persisted.find("SessionDirectoryInstallKey = " + key) == std::string::npos) {
						*error = "the first directory use did not persist the install key";
					} else if (g_SettingsMan.GetSessionDirectoryInstallKey() != key || g_SettingsMan.GetOrCreateSessionDirectoryInstallKey() != key) {
						*error = "a second directory use changed the install key";
					}
				}

				SetSettingsPathEnv("");
				std::filesystem::remove(settingsPath, fileError);
				if (!error->empty()) {
					return false;
				}
				std::cout << "[net-directory-selftest] install key lazy: load left " << staged.size() << " bytes unchanged, first use wrote key " << key << std::endl;
				return true;
			}

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

			const std::string kSignalSession = "7b8c9d2e-1111-4222-8333-444455556666";
			const std::string kSignalBase = "/v1/sessions/" + kSignalSession;
			const NetDirectoryClient::Reply kPostOk{200, R"({"ok":true,"seq":1})", ""};
			const NetDirectoryClient::Reply kNoSignals{200, R"({"signals":[]})", ""};

			std::string B64(const std::string& bytes) { return base64_encode(bytes, false); }

			std::string SignalListBody(const std::vector<NetDirectorySignal>& signals) {
				NetDirectorySignalList list;
				list.signals = signals;
				return NetDirectoryCodec::EncodeSignalList(list);
			}

			/// A channel on the scripted transport whose sink records every offer and what it took.
			struct ScriptedChannel {
				std::shared_ptr<std::deque<NetDirectoryClient::Reply>> replies = std::make_shared<std::deque<NetDirectoryClient::Reply>>();
				std::shared_ptr<std::vector<NetDirectoryClient::Request>> sent = std::make_shared<std::vector<NetDirectoryClient::Request>>();
				std::vector<int64_t> offered;
				std::vector<NetDirectorySignalChannel::Signal> taken;
				std::function<bool(const NetDirectorySignalChannel::Signal&)> accept = [](const NetDirectorySignalChannel::Signal&) { return true; };
				NetDirectorySignalChannel channel;

				explicit ScriptedChannel(bool host) {
					auto replies = this->replies;
					auto sent = this->sent;
					channel.SetTransportFactory([replies, sent] { return std::make_unique<ScriptedTransport>(replies, sent); });
					if (host) {
						channel.ConfigureHost("https://dir.test", "key0123456789abcd", "", kSignalSession, "hostToken_0123456789");
					} else {
						channel.ConfigureClient("https://dir.test", "key0123456789abcd", "", kSignalSession);
					}
					channel.SetSink([this](const NetDirectorySignalChannel::Signal& signal) {
						offered.push_back(signal.seq);
						if (!accept(signal)) {
							return false;
						}
						taken.push_back(signal);
						return true;
					});
				}

				std::string PollPath(int64_t after) const { return kSignalBase + "/signals?peer=" + channel.GetLocalPeer() + "&after=" + std::to_string(after); }
			};

			/// "seq:bytes,..." of what the sink took.
			std::string Taken(const ScriptedChannel& s) {
				std::string text;
				for (const NetDirectorySignalChannel::Signal& signal : s.taken) {
					text += (text.empty() ? "" : ",") + std::to_string(signal.seq) + ":" + signal.bytes;
				}
				return text;
			}

			bool TakenTwice(const ScriptedChannel& s) {
				for (size_t i = 0; i < s.taken.size(); ++i) {
					for (size_t j = i + 1; j < s.taken.size(); ++j) {
						if (s.taken[i].seq == s.taken[j].seq) {
							return true;
						}
					}
				}
				return false;
			}

			bool PollsReadAfter(const ScriptedChannel& s, const std::vector<int64_t>& afters, const char* what, std::string* error) {
				if (s.sent->size() != afters.size()) {
					*error = std::string(what) + ": " + std::to_string(s.sent->size()) + " requests, expected " + std::to_string(afters.size());
					return false;
				}
				for (size_t i = 0; i < afters.size(); ++i) {
					if (!RequestIs(s.sent->at(i), "GET", s.PollPath(afters[i]).c_str(), error)) {
						*error = std::string(what) + ": poll " + std::to_string(i + 1) + ": " + *error;
						return false;
					}
				}
				return true;
			}

			bool TestSignalOrderingAndCursor(std::string* error) {
				ScriptedChannel s(false);
				const std::string me = s.channel.GetLocalPeer();
				s.replies->push_back({200, SignalListBody({{2, "host", me, B64("two")}, {1, "host", me, B64("one")}}), ""});
				s.replies->push_back({200, SignalListBody({{2, "host", me, B64("two")}, {3, "host", me, B64("three")}}), ""});
				s.replies->push_back({200, SignalListBody({{4, "host", me, B64("four")}}), ""});

				s.channel.SetPolling(true);
				s.channel.Update(0);
				s.channel.Update(0); // poll 1 answers seq 2 before seq 1
				if (Taken(s) != "1:one,2:two") {
					*error = "signal ordering: poll 1 delivered " + Taken(s) + ", expected 1:one,2:two";
					return false;
				}
				s.channel.Update(499);
				if (s.sent->size() != 1) {
					*error = "signal ordering: a poll left before the 500 ms interval";
					return false;
				}
				s.channel.Update(500);
				s.channel.Update(500); // poll 2 re-delivers seq 2
				if (TakenTwice(s)) {
					*error = "signal ordering: a re-delivered signal reached the sink twice: " + Taken(s);
					return false;
				}
				s.channel.Update(1000);
				s.channel.Update(1000);
				if (Taken(s) != "1:one,2:two,3:three,4:four") {
					*error = "signal ordering: the sink took " + Taken(s) + ", expected 1:one,2:two,3:three,4:four";
					return false;
				}
				if (!PollsReadAfter(s, {0, 2, 3}, "signal ordering", error)) {
					return false;
				}
				const json report = json::parse(s.channel.BuildReportJson());
				if (s.channel.GetCursor() != 4 || report["state"] != "open" || report["polls"] != 3 || report["signals_received"] != 4 || report["signals_posted"] != 0 || report["last_status"] != 200 || report["last_error"] != "") {
					*error = "signal ordering: cursor " + std::to_string(s.channel.GetCursor()) + ", report " + report.dump();
					return false;
				}
				std::cout << "[net-directory-selftest] signal ordering: polls after=0,2,3 took 1,2,3,4 once each; seq 2 re-delivered by poll 2 was skipped; report " << report.dump() << std::endl;
				return true;
			}

			bool TestSignalCursorWaitsForSink(std::string* error) {
				ScriptedChannel s(false);
				const std::string me = s.channel.GetLocalPeer();
				bool busy = true;
				s.accept = [&busy](const NetDirectorySignalChannel::Signal& signal) {
					if (signal.seq == 2 && busy) {
						busy = false;
						return false;
					}
					return true;
				};
				s.replies->push_back({200, SignalListBody({{1, "host", me, B64("one")}, {2, "host", me, B64("two")}, {3, "host", me, B64("three")}}), ""});
				s.replies->push_back({200, SignalListBody({{2, "host", me, B64("two")}, {3, "host", me, B64("three")}}), ""});
				s.replies->push_back({200, SignalListBody({{3, "host", me, B64("three")}, {4, "host", me, B64("four")}}), ""});

				s.channel.SetPolling(true);
				for (const uint64_t now : {0, 0, 500, 500, 1000, 1000}) {
					s.channel.Update(now);
				}
				if (TakenTwice(s) || Taken(s) != "1:one,2:two,3:three,4:four") {
					*error = "signal cursor: the sink took " + Taken(s) + ", expected 1:one,2:two,3:three,4:four once each";
					return false;
				}
				// seq 2 refused on poll 1, so seq 3 waited; poll 3 re-sent seq 3 and it was not offered again.
				if (s.offered != std::vector<int64_t>{1, 2, 2, 3, 4}) {
					std::string offers;
					for (const int64_t seq : s.offered) {
						offers += (offers.empty() ? "" : ",") + std::to_string(seq);
					}
					*error = "signal cursor: the sink was offered " + offers + ", expected 1,2,2,3,4";
					return false;
				}
				if (!PollsReadAfter(s, {0, 1, 3}, "signal cursor", error)) {
					return false;
				}
				std::cout << "[net-directory-selftest] signal cursor: a refused seq 2 held the cursor at 1 (next poll after=1); offers 1,2,2,3,4; seq 3 re-sent by poll 3 was not offered again" << std::endl;
				return true;
			}

			bool TestSignal404Fails(std::string* error) {
				ScriptedChannel s(false);
				s.replies->push_back({404, R"({"error":"not_found"})", ""});
				s.channel.SetPolling(true);
				s.channel.Update(0);
				s.channel.Update(0);
				const bool took = s.channel.Post("host", "after-failure");
				s.channel.Update(600000);
				const json report = json::parse(s.channel.BuildReportJson());
				if (s.channel.GetState() != NetDirectorySignalChannel::State::Failed || s.channel.GetLastError() != "session gone" || report["state"] != "failed" || report["last_error"] != "session gone" || report["last_status"] != 404) {
					*error = "signal 404: expected failed with \"session gone\", report " + report.dump();
					return false;
				}
				if (took || s.sent->size() != 1) {
					*error = "signal 404: the failed channel still took or sent a signal";
					return false;
				}
				std::cout << "[net-directory-selftest] signal 404: poll answered 404 -> " << report.dump() << ", nothing sent after" << std::endl;
				return true;
			}

			bool TestSignal403Fails(std::string* error) {
				ScriptedChannel s(true);
				s.replies->push_back({403, R"({"error":"forbidden"})", ""});
				if (!s.channel.Post("client:joinNonce1", "offer")) {
					*error = "signal 403: the host refused to queue a signal for a client";
					return false;
				}
				s.channel.SetPolling(true);
				s.channel.Update(0);
				s.channel.Update(0);
				s.channel.Update(600000);
				const json report = json::parse(s.channel.BuildReportJson());
				if (s.sent->size() != 1 || !RequestIs(s.sent->at(0), "POST", (kSignalBase + "/signal").c_str(), error)) {
					*error = "signal 403: " + (error->empty() ? std::to_string(s.sent->size()) + " requests, expected the one post" : *error);
					return false;
				}
				if (s.channel.GetState() != NetDirectorySignalChannel::State::Failed || s.channel.GetLastError() != "bad credential" || report["last_error"] != "bad credential" || report["last_status"] != 403) {
					*error = "signal 403: expected failed with \"bad credential\", report " + report.dump();
					return false;
				}
				std::cout << "[net-directory-selftest] signal 403: host post answered 403 -> " << report.dump() << ", nothing sent after" << std::endl;
				return true;
			}

			bool TestSignalQueueFullRetries(std::string* error) {
				ScriptedChannel s(false);
				s.replies->push_back({400, R"({"error":"queue_full"})", ""});
				s.replies->push_back(kNoSignals);
				s.replies->push_back(kNoSignals);
				s.replies->push_back(kPostOk);
				if (!s.channel.Post("host", "offer")) {
					*error = "signal queue_full: Post refused a signal";
					return false;
				}
				s.channel.SetPolling(true);
				s.channel.Update(0);
				s.channel.Update(0); // queue_full: the post waits, the due poll goes
				s.channel.Update(0);
				s.channel.Update(4999);
				s.channel.Update(4999);
				if (s.sent->size() != 3 || s.sent->at(0).method != "POST" || s.sent->at(1).method != "GET" || s.sent->at(2).method != "GET") {
					*error = "signal queue_full: expected POST then two polls before +5000 ms, got " + std::to_string(s.sent->size()) + " requests";
					return false;
				}
				s.channel.Update(5000);
				s.channel.Update(5000);
				if (s.sent->size() != 4 || s.sent->at(3).method != "POST" || s.sent->at(3).body != s.sent->at(0).body || s.channel.PendingPosts() != 0 || s.channel.GetState() != NetDirectorySignalChannel::State::Open) {
					*error = "signal queue_full: the same signal was not re-posted at +5000 ms";
					return false;
				}
				std::cout << "[net-directory-selftest] signal queue_full: the same post body went again at t=5000; polls went on at t=0 and t=4999 meanwhile" << std::endl;
				return true;
			}

			bool TestSignal429RetryAfter(std::string* error) {
				ScriptedChannel s(false);
				s.replies->push_back({429, R"({"error":"rate_limited","retry_after_s":7})", ""});
				s.replies->push_back(kPostOk);
				s.replies->push_back(kNoSignals);
				if (!s.channel.Post("host", "offer")) {
					*error = "signal 429: Post refused a signal";
					return false;
				}
				s.channel.SetPolling(true);
				s.channel.Update(1000);
				s.channel.Update(1000); // 429: every request waits for t=8000
				s.channel.Update(7999);
				if (s.sent->size() != 1) {
					*error = "signal 429: a request left at t=7999, before retry_after_s=7 from t=1000 elapsed";
					return false;
				}
				s.channel.Update(8000);
				if (s.sent->size() != 2 || s.sent->at(1).method != "POST" || s.sent->at(1).body != s.sent->at(0).body) {
					*error = "signal 429: the post was not retried at exactly t=8000";
					return false;
				}
				s.channel.Update(8000);
				if (s.sent->size() != 3 || s.sent->at(2).method != "GET") {
					*error = "signal 429: the held poll did not follow the retried post";
					return false;
				}
				std::cout << "[net-directory-selftest] signal 429: retry_after_s=7 at t=1000 held every request through t=7999; the post went again at t=8000, then the poll" << std::endl;
				return true;
			}

			bool TestSignalTransportBackoff(std::string* error) {
				ScriptedChannel s(false);
				const std::string me = s.channel.GetLocalPeer();
				s.replies->push_back({0, "", "send: cannot connect"});
				s.replies->push_back({0, "", "send: timed out"});
				s.replies->push_back({0, "", "receive: timed out"});
				s.replies->push_back({200, SignalListBody({{1, "host", me, B64("one")}}), ""});
				s.replies->push_back({503, R"({"error":"full"})", ""});

				s.channel.SetPolling(true);
				s.channel.Update(0);
				s.channel.Update(0);
				const uint64_t retries[] = {5000, 15000, 35000};
				for (size_t i = 0; i < 3; ++i) {
					s.channel.Update(retries[i] - 1);
					if (s.sent->size() != i + 1) {
						*error = "signal backoff: a retry left at t=" + std::to_string(retries[i] - 1);
						return false;
					}
					s.channel.Update(retries[i]);
					s.channel.Update(retries[i]);
					if (s.sent->size() != i + 2) {
						*error = "signal backoff: no retry at t=" + std::to_string(retries[i]);
						return false;
					}
				}
				if (Taken(s) != "1:one" || !PollsReadAfter(s, {0, 0, 0, 0}, "signal backoff", error)) {
					*error = error->empty() ? "signal backoff: the recovered poll delivered " + Taken(s) : *error;
					return false;
				}
				s.channel.Update(35500);
				s.channel.Update(35500); // a 503 after the success starts the ladder over
				s.channel.Update(40499);
				const size_t held = s.sent->size();
				s.channel.Update(40500);
				if (held != 5 || s.sent->size() != 6) {
					*error = "signal backoff: after a success the next failure did not wait exactly 5 s";
					return false;
				}
				std::cout << "[net-directory-selftest] signal backoff: transport errors at t=0,5000,15000 retried at t=5000,15000,35000 (5/10/20 s); a 503 after the success waited 5 s again" << std::endl;
				return true;
			}

			bool TestSignalPayloadCap(std::string* error) {
				ScriptedChannel s(false);
				const std::string me = s.channel.GetLocalPeer();
				std::string atCap(NetDirectorySignalChannel::c_MaxSignalBytes, '\0');
				for (size_t i = 0; i < atCap.size(); ++i) {
					atCap[i] = static_cast<char>((i * 7) & 0xFF);
				}
				if (s.channel.Post("host", atCap + "x")) {
					*error = "signal cap: Post took 64 KiB + 1 bytes";
					return false;
				}
				if (!s.channel.Post("host", atCap) || s.channel.PendingPosts() != 1) {
					*error = "signal cap: Post refused exactly 64 KiB";
					return false;
				}
				s.replies->push_back(kPostOk);
				s.channel.Update(0);
				NetDirectorySignalPost post;
				std::string reason;
				if (s.sent->size() != 1 || s.sent->at(0).body.size() > NetDirectoryLimits::c_MaxBodyBytes || !NetDirectoryCodec::DecodeSignalPost(s.sent->at(0).body, post, reason) || base64_decode(post.payloadB64) != atCap) {
					*error = "signal cap: the 64 KiB post did not carry the payload intact " + reason;
					return false;
				}
				s.channel.Update(0);
				s.replies->push_back({200, SignalListBody({{1, "host", me, post.payloadB64}}), ""});
				s.channel.SetPolling(true);
				s.channel.Update(0);
				s.channel.Update(0);
				if (s.taken.size() != 1 || s.taken[0].bytes != atCap) {
					*error = "signal cap: the 64 KiB signal did not reach the sink intact";
					return false;
				}
				// The codec's 87384-character cap, unpadded, decodes to 65538 bytes: over the signal cap.
				s.replies->push_back({200, SignalListBody({{2, "host", me, std::string(NetDirectoryLimits::c_MaxPayloadB64Chars, 'A')}}), ""});
				s.channel.Update(500);
				s.channel.Update(500);
				if (s.taken.size() != 1 || s.channel.GetCursor() != 1) {
					*error = "signal cap: an inbound payload over 64 KiB reached the sink";
					return false;
				}
				std::cout << "[net-directory-selftest] signal cap: 65536 bytes posted (" << post.payloadB64.size() << " base64 chars, body " << s.sent->at(0).body.size() << " bytes) and delivered intact; 65537 refused at Post; an inbound 65538-byte payload refused" << std::endl;
				return true;
			}

			bool TestSignalPostBeforePoll(std::string* error) {
				ScriptedChannel s(false);
				s.replies->push_back(kPostOk);
				s.replies->push_back(kPostOk);
				s.replies->push_back(kNoSignals);
				s.channel.SetPolling(true);
				if (!s.channel.Post("host", "first") || !s.channel.Post("host", "second")) {
					*error = "signal priority: Post refused a signal";
					return false;
				}
				s.channel.Update(0);
				s.channel.Update(0);
				s.channel.Update(0);
				NetDirectorySignalPost first;
				NetDirectorySignalPost second;
				std::string reason;
				if (s.sent->size() != 3 || s.sent->at(0).method != "POST" || s.sent->at(1).method != "POST" || !RequestIs(s.sent->at(2), "GET", s.PollPath(0).c_str(), error) ||
				    !NetDirectoryCodec::DecodeSignalPost(s.sent->at(0).body, first, reason) || !NetDirectoryCodec::DecodeSignalPost(s.sent->at(1).body, second, reason) ||
				    base64_decode(first.payloadB64) != "first" || base64_decode(second.payloadB64) != "second") {
					*error = "signal priority: expected POST first, POST second, then the poll that was due all along";
					return false;
				}
				std::cout << "[net-directory-selftest] signal priority: with the poll due at t=0, POST(first) and POST(second) went before the GET" << std::endl;
				return true;
			}

			std::string HeaderValue(const NetDirectorySignalChannel& channel, const char* name) {
				for (const auto& [key, value] : channel.RequestHeaders()) {
					if (key == name) {
						return value;
					}
				}
				return "<absent>";
			}

			bool TestSignalNonceAndCredentials(std::string* error) {
				const std::string a = NetDirectorySignalChannel::MintJoinNonce();
				const std::string b = NetDirectorySignalChannel::MintJoinNonce();
				auto isKeyChar = [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '_'; };
				if (a.size() != NetDirectorySignalChannel::c_JoinNonceChars || !std::all_of(a.begin(), a.end(), isKeyChar) || a == b) {
					*error = "signal nonce: minted \"" + a + "\" then \"" + b + "\"";
					return false;
				}
				ScriptedChannel client(false);
				ScriptedChannel sameKey(false);
				const std::string nonce = client.channel.GetJoinNonce();
				if (nonce.size() != 32 || !std::all_of(nonce.begin(), nonce.end(), isKeyChar) || client.channel.GetLocalPeer() != "client:" + nonce || nonce == sameKey.channel.GetJoinNonce()) {
					*error = "signal nonce: two joiners on one install key did not get their own 32-character nonces";
					return false;
				}
				if (HeaderValue(client.channel, "X-Signal-Peer") != "client:" + nonce || HeaderValue(client.channel, "X-Install-Key") != "key0123456789abcd" || HeaderValue(client.channel, "X-Session-Token") != "<absent>") {
					*error = "signal credentials: the joiner's headers were wrong";
					return false;
				}
				client.replies->push_back(kPostOk);
				client.replies->push_back(kNoSignals);
				if (client.channel.Post("client:someoneElse", "x") || !client.channel.Post("host", "hello")) {
					*error = "signal peers: a joiner must signal the host and nobody else";
					return false;
				}
				client.channel.SetPolling(true);
				for (int i = 0; i < 3; ++i) {
					client.channel.Update(0);
				}
				NetDirectorySignalPost post;
				std::string reason;
				if (client.sent->size() != 2 || !NetDirectoryCodec::DecodeSignalPost(client.sent->at(0).body, post, reason) || post.from != "client:" + nonce || post.to != "host" || post.tokenOrJoinNonce != nonce || client.sent->at(1).path != client.PollPath(0)) {
					*error = "signal credentials: the joiner did not post as client:<nonce> proving the nonce, then poll its own queue";
					return false;
				}

				ScriptedChannel host(true);
				host.replies->push_back(kPostOk);
				host.replies->push_back(kNoSignals);
				if (HeaderValue(host.channel, "X-Signal-Peer") != "host" || HeaderValue(host.channel, "X-Session-Token") != "hostToken_0123456789" || !host.channel.GetJoinNonce().empty()) {
					*error = "signal credentials: the host's headers were wrong";
					return false;
				}
				if (host.channel.Post("host", "x") || host.channel.Post("client:", "x") || host.channel.Post("client:bad nonce", "x") || !host.channel.Post("client:" + nonce, "answer")) {
					*error = "signal peers: the host must answer a client:<nonce> and nobody else";
					return false;
				}
				host.channel.SetPolling(true);
				for (int i = 0; i < 3; ++i) {
					host.channel.Update(0);
				}
				if (host.sent->size() != 2 || !NetDirectoryCodec::DecodeSignalPost(host.sent->at(0).body, post, reason) || post.from != "host" || post.to != "client:" + nonce || post.tokenOrJoinNonce != "hostToken_0123456789" ||
				    host.sent->at(1).path != kSignalBase + "/signals?peer=host&after=0") {
					*error = "signal credentials: the host did not post as host proving the token, then poll with the token out of the URL";
					return false;
				}

				// A session id that could reshape the request path, or a token the service never mints, fails at configure.
				NetDirectorySignalChannel badSession;
				badSession.ConfigureClient("https://dir.test", "key0123456789abcd", "", "../../v1/sessions");
				NetDirectorySignalChannel badToken;
				badToken.ConfigureHost("https://dir.test", "key0123456789abcd", "", kSignalSession, "tok\r\nX-Evil: 1");
				if (badSession.GetState() != NetDirectorySignalChannel::State::Failed || badSession.GetLastError() != "invalid session id" ||
				    badToken.GetState() != NetDirectorySignalChannel::State::Failed || badToken.GetLastError() != "invalid session token") {
					*error = "signal credentials: a malformed session id or token was configured";
					return false;
				}
				std::cout << "[net-directory-selftest] signal credentials: nonce of 32 install-key characters, fresh per joiner; X-Signal-Peer on every request; the host token only in X-Session-Token and the post body" << std::endl;
				return true;
			}

			bool TestSignalDrain(std::string* error) {
				ScriptedChannel s(false);
				const std::string me = s.channel.GetLocalPeer();
				s.replies->push_back({200, SignalListBody({{1, "host", me, B64("one")}, {2, "host", me, B64("two")}}), ""});
				s.replies->push_back({200, SignalListBody({{3, "host", me, B64("three")}}), ""});
				s.channel.SetPolling(true);
				s.channel.Update(0);
				s.channel.Update(0);
				if (!s.channel.Post("host", "unsent")) {
					*error = "signal drain: Post refused a signal";
					return false;
				}
				s.channel.Drain();
				if (!PollsReadAfter(s, {0, 2}, "signal drain", error)) {
					return false;
				}
				if (Taken(s) != "1:one,2:two,3:three" || s.channel.GetState() != NetDirectorySignalChannel::State::Closed || s.channel.PendingPosts() != 0) {
					*error = "signal drain: took " + Taken(s) + ", state " + NetDirectorySignalChannel::StateName(s.channel.GetState());
					return false;
				}
				s.channel.Update(600000);
				if (s.channel.Post("host", "late") || s.sent->size() != 2) {
					*error = "signal drain: the closed channel still took or sent a signal";
					return false;
				}
				std::cout << "[net-directory-selftest] signal drain: one more poll after=2 inside the 500 ms interval took seq 3, then closed; the queued post was dropped" << std::endl;
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
			if (!TestSignalOrderingAndCursor(&error)) return fail(error);
			if (!TestSignalCursorWaitsForSink(&error)) return fail(error);
			if (!TestSignal404Fails(&error)) return fail(error);
			if (!TestSignal403Fails(&error)) return fail(error);
			if (!TestSignalQueueFullRetries(&error)) return fail(error);
			if (!TestSignal429RetryAfter(&error)) return fail(error);
			if (!TestSignalTransportBackoff(&error)) return fail(error);
			if (!TestSignalPayloadCap(&error)) return fail(error);
			if (!TestSignalPostBeforePoll(&error)) return fail(error);
			if (!TestSignalNonceAndCredentials(&error)) return fail(error);
			if (!TestSignalDrain(&error)) return fail(error);
#if defined(_WIN32) || defined(__APPLE__)
			if (!TestHttpClientCancel(&error)) return fail(error);
#endif
			if (!TestInstallKeyIsLazy(&error)) return fail(error);

			std::cout << "[net-directory-selftest] PASS" << std::endl;
			return 0;
		}

	} // namespace NetDirectorySelfTest

} // namespace RTE
