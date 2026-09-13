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
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
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
				json peerOver64 = json{{"token_or_join_nonce", "n"}, {"from", "client:" + std::string(NetDirectoryLimits::c_MaxPeerChars - 6, 'k')}, {"to", "host"}, {"payload_b64", "eA=="}};
				json embeddedPad = json{{"token_or_join_nonce", "n"}, {"from", "host"}, {"to", "client:n"}, {"payload_b64", "YQ=A"}};
				json tooManySignals = json{{"signals", json::array()}};
				for (size_t i = 0; i <= NetDirectoryLimits::c_MaxSignalRows; ++i) {
					tooManySignals["signals"].push_back(json{{"seq", 1}, {"from", "host"}, {"to", "client:n"}, {"payload_b64", "eA=="}});
				}
				const std::vector<Case> cases = {
					{"string over 64 chars", longName.dump(), DecodeInto(&NetDirectoryCodec::DecodeRegisterRequest)},
					{"more than 8 listen_addrs", manyAddrs.dump(), DecodeInto(&NetDirectoryCodec::DecodeRegisterRequest)},
					{"body over 128 KiB", padded.dump(), DecodeInto(&NetDirectoryCodec::DecodeRegisterRequest)},
					{"payload_b64 over the 64 KiB decoded cap", bigPayload.dump(), DecodeInto(&NetDirectoryCodec::DecodeSignalPost)},
					{"peer over 71 chars", longPeer.dump(), DecodeInto(&NetDirectoryCodec::DecodeSignalPost)},
					{"peer over 64 chars", peerOver64.dump(), DecodeInto(&NetDirectoryCodec::DecodeSignalPost)},
					{"payload_b64 with embedded padding", embeddedPad.dump(), DecodeInto(&NetDirectoryCodec::DecodeSignalPost)},
					{"signals over MAX_QUEUE", tooManySignals.dump(), DecodeInto(&NetDirectoryCodec::DecodeSignalList)},
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

			bool TestSessionsCountCap(std::string* error) {
				json tooMany = json{{"sessions", json::array()}};
				for (size_t i = 0; i <= NetDirectoryLimits::c_MaxListRows; ++i) {
					tooMany["sessions"].push_back(json::object());
				}
				NetDirectoryListResponse listed;
				std::string reason;
				const bool accepted = NetDirectoryCodec::DecodeListResponse(tooMany.dump(), listed, reason);
				if (accepted || reason != "invalid_field:sessions") {
					*error = std::string("sessions[] over MAX_ROWS was not refused as invalid_field:sessions; accepted=")
					         + (accepted ? "1" : "0") + " reason=" + reason;
					return false;
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

			// Optional visibility fields retain their value through a codec round trip.
			template <typename T>
			void CheckOptionalVisibilityField(const std::string& name, const char* field, const std::string& base,
			                                  bool (*decode)(const std::string&, T&, std::string&),
			                                  std::string (*encode)(const T&), bool keepsFalse,
			                                  std::vector<std::string>& misses) {
				std::string reason;
				for (const bool value : {true, false}) {
					json body = json::parse(base);
					body[field] = value;
					T decoded;
					reason.clear();
					if (!decode(body.dump(), decoded, reason)) {
						misses.push_back(name + " refused " + field + "=" + (value ? "true" : "false") + ": " + reason);
						continue;
					}
					const json re = json::parse(encode(decoded));
					const auto it = re.find(field);
					if (value || keepsFalse) {
						if (it == re.end() || !it->is_boolean() || it->get<bool>() != value) {
							misses.push_back(name + " lost " + field + "=" + (value ? "true" : "false") + " on decode->encode");
						}
					} else if (it != re.end()) {
						misses.push_back(name + " re-encoded " + field + "=false instead of omitting it");
					}
				}
				for (const bool value : {true, false}) {
					json previous = json::parse(base);
					previous[field] = value;
					T reused;
					reason.clear();
					if (!decode(previous.dump(), reused, reason) || !decode(base, reused, reason)) {
						misses.push_back(name + " reuse decode refused: " + reason);
					} else {
						const json re = json::parse(encode(reused));
						if (re.find(field) != re.end()) {
							misses.push_back(name + " kept " + field + " after " + (value ? "true" : "false") + " then absent decode");
						}
					}
				}
				T legacy;
				reason.clear();
				if (!decode(base, legacy, reason)) {
					misses.push_back(name + " legacy body refused: " + reason);
				} else if (json::parse(encode(legacy)) != json::parse(base)) {
					misses.push_back(name + " changed the legacy body shape on re-encode");
				}
				const json badValues[] = {json(nullptr), json("yes"), json(0), json(1), json::array(), json::object()};
				const char* badNames[] = {"null", "string", "0", "1", "array", "object"};
				for (size_t i = 0; i < 6; ++i) {
					json body = json::parse(base);
					body[field] = badValues[i];
					T decoded;
					reason.clear();
					if (decode(body.dump(), decoded, reason)) {
						misses.push_back(name + " accepted malformed " + field + " (" + badNames[i] + ")");
					} else if (reason != std::string("invalid_field:") + field) {
						misses.push_back(name + " refused malformed " + field + " (" + badNames[i] + ") with " + reason);
					}
				}
			}

			bool TestUnlistedVisibility(std::string* error) {
				std::vector<std::string> misses;
				CheckOptionalVisibilityField<NetDirectoryRegisterResponse>("register response", "supports_unlisted",
					R"({"session_id":"7b8c9d2e-1111-4222-8333-444455556666","token":"abcTOK123","expires_in_s":15,"heartbeat_s":5,"observed_ip":"127.0.0.1"})",
					&NetDirectoryCodec::DecodeRegisterResponse, &NetDirectoryCodec::EncodeRegisterResponse, false, misses);
				CheckOptionalVisibilityField<NetDirectoryHeartbeatRequest>("heartbeat request", "listed",
					R"({"token":"abcTOK123","peer_count":2,"seats_free":1})",
					&NetDirectoryCodec::DecodeHeartbeatRequest, &NetDirectoryCodec::EncodeHeartbeatRequest, true, misses);
				CheckOptionalVisibilityField<NetDirectoryHeartbeatResponse>("heartbeat response", "listed",
					R"({"expires_in_s":15,"heartbeat_s":5})",
					&NetDirectoryCodec::DecodeHeartbeatResponse, &NetDirectoryCodec::EncodeHeartbeatResponse, true, misses);
				if (misses.empty()) {
					return true;
				}
				*error = "unlisted visibility codec misses (" + std::to_string(misses.size()) + "):";
				for (const std::string& miss : misses) {
					*error += " [" + miss + "]";
				}
				return false;
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

			// A listener that never accepts: the TCP handshake completes in the kernel and the
			// TLS ClientHello sits unread, so the request is guaranteed to be stalled.
			bool OpenSilentListener(SocketHandle* listener, std::string* url, std::string* error) {
#ifdef _WIN32
				WSADATA wsaData;
				(void)WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
				SocketHandle opened = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
				sockaddr_in addr{};
				addr.sin_family = AF_INET;
				addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				addr.sin_port = 0;
				if (opened == c_InvalidSocket || bind(opened, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(opened, 1) != 0) {
					*error = "could not open the stall listener";
					return false;
				}
				sockaddr_in bound{};
				SocketLength boundSize = sizeof(bound);
				(void)getsockname(opened, reinterpret_cast<sockaddr*>(&bound), &boundSize);
				*listener = opened;
				*url = "https://127.0.0.1:" + std::to_string(ntohs(bound.sin_port)) + "/";
				return true;
			}

			bool TestHttpClientCancel(std::string* error) {
				SocketHandle listener = c_InvalidSocket;
				std::string url;
				if (!OpenSilentListener(&listener, &url, error)) {
					return false;
				}
				const bool stalled = MeasureCancel(url, error);
				CloseSocket(listener);
				if (error->empty() && !stalled) {
					*error = "the silent-listener request was not in flight when Cancel was measured";
					return false;
				}
				if (!error->empty()) {
					return false;
				}
				// The black-hole dial measures a cancel during the SYN phase, which no loopback listener
				// can stage; it sends a SYN off the machine, so an unattended run skips it.
				if (std::getenv("CC_SELFTEST_EXTERNAL_DIAL") != nullptr) {
					(void)MeasureCancel("https://10.255.255.1:8443/", error);
				} else {
					std::cout << "[net-directory-selftest] MEASURE http client cancel external dial skipped: set CC_SELFTEST_EXTERNAL_DIAL=1 to dial 10.255.255.1" << std::endl;
				}
				return error->empty();
			}

			// 200 quick cancels on a stalled request then 200 refusals: as close as a selftest
			// gets to a late HANDLE_CLOSING racing the state free; it cannot force the race.
#ifdef _WIN32
			bool TestHttpClientStress(std::string* error) {
				SocketHandle listener = c_InvalidSocket;
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
						CloseSocket(listener);
						*error = "cancelled request " + std::to_string(i) + " never reported done";
						return false;
					}
					if (response.error.empty()) {
						CloseSocket(listener);
						*error = "cancelled request " + std::to_string(i) + " finished without an error";
						return false;
					}
				}
				CloseSocket(listener);
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

			bool TestHttpClientCancelDuringCallback(std::string* error) {
				SocketHandle listener = c_InvalidSocket;
				std::string stalledUrl;
				if (!OpenSilentListener(&listener, &stalledUrl, error)) {
					return false;
				}
				for (int i = 0; i < 50; ++i) {
					NetHttpClient client;
					client.Start("GET", stalledUrl, {}, "", "");
					std::this_thread::sleep_for(std::chrono::milliseconds(2 + (i % 8)));
					std::atomic<bool> finished{false};
					std::thread cancelThread([&] {
						client.Cancel();
						finished.store(true);
					});
					const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
					while (!finished.load() && std::chrono::steady_clock::now() < deadline) {
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
					}
					if (!finished.load()) {
						cancelThread.detach();
						CloseSocket(listener);
						*error = "cancel-during-callback hung on request " + std::to_string(i);
						return false;
					}
					cancelThread.join();
					if (client.Poll() != NetHttpClient::PollResult::Done) {
						CloseSocket(listener);
						*error = "cancel-during-callback request " + std::to_string(i) + " never reported done";
						return false;
					}
				}
				CloseSocket(listener);
				return true;
			}
#endif
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
				std::cout << "[net-directory-selftest] PASS install key lazy: load left " << staged.size() << " bytes unchanged, first use wrote key " << key << std::endl;
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

			/// Compatibility shim: forwards the visibility intent when the client accepts it.
			template <typename Client>
			auto CallAdvertiseIntent(Client& client, const NetDirectoryRegisterRequest& row, bool running, bool listed, int) -> decltype(client.Advertise(row, running, listed), void()) {
				client.Advertise(row, running, listed);
			}
			template <typename Client>
			void CallAdvertiseIntent(Client& client, const NetDirectoryRegisterRequest& row, bool running, bool listed, long) {
				(void)listed;
				client.Advertise(row, running);
			}
			void CallAdvertise(NetDirectoryClient& client, const NetDirectoryRegisterRequest& row, bool running, bool listed) {
				CallAdvertiseIntent(client, row, running, listed, 0);
			}

			const char* kRegisterCapable = R"({"session_id":"7b8c9d2e-1111-4222-8333-444455556666","token":"tok","expires_in_s":15,"heartbeat_s":5,"observed_ip":"127.0.0.1","supports_unlisted":true})";
			const char* kRegisterLegacy = R"({"session_id":"7b8c9d2e-1111-4222-8333-444455556666","token":"tok","expires_in_s":15,"heartbeat_s":5,"observed_ip":"127.0.0.1"})";
			const std::string kSessionPath = "/v1/sessions/7b8c9d2e-1111-4222-8333-444455556666";
			const std::string kHeartbeatPath = kSessionPath + "/heartbeat";

			const NetDirectoryClient::Request* SentAt(const ScriptedClient& s, size_t index) {
				return index < s.sent->size() ? &s.sent->at(index) : nullptr;
			}
			uint64_t CountRequests(const ScriptedClient& s, const char* method, const std::string& path) {
				uint64_t count = 0;
				for (const NetDirectoryClient::Request& request : *s.sent) {
					if (request.method == method && request.path == path) {
						++count;
					}
				}
				return count;
			}
			bool RequestListed(const NetDirectoryClient::Request& request, bool* listed) {
				const json body = json::parse(request.body);
				if (!body.contains("listed") || !body["listed"].is_boolean()) {
					return false;
				}
				*listed = body["listed"].get<bool>();
				return true;
			}
			std::string RequestToken(const NetDirectoryClient::Request& request) {
				const json body = json::parse(request.body);
				return body.value("token", "");
			}

			bool TestClientUnlistedCapable(std::string* error) {
				std::vector<std::string> misses;
				auto note = [&misses](const std::string& miss) { misses.push_back(miss); };

				{   // hide -> keepalive -> relist: same id/token, no delete and no second register
					ScriptedClient s;
					s.replies->push_back({200, kRegisterCapable, ""});
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5,"listed":false})", ""});
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5,"listed":false})", ""});
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5,"listed":true})", ""});
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(0);
					s.client.Update(0); // the register lands; a pending hide schedules its heartbeat now
					const NetDirectoryClient::Request* hide = SentAt(s, 1);
					bool listed = true;
					if (!hide) {
						note("hide: no heartbeat carried the hidden intent right after register");
					} else if (hide->method != "POST" || hide->path != kHeartbeatPath) {
						note("hide: request 1 was " + hide->method + " " + hide->path + ", not the session heartbeat");
					} else if (!RequestListed(*hide, &listed) || listed) {
						note("hide: the visibility heartbeat did not send listed=false");
					} else if (RequestToken(*hide) != "tok") {
						note("hide: the visibility heartbeat lost the session token");
					}
					s.client.Update(0);
					s.client.Update(4999);
					s.client.Update(5000); // the keepalive heartbeat on a capable row repeats the visibility
					const NetDirectoryClient::Request* keepalive = SentAt(s, 2);
					if (!keepalive || keepalive->path != kHeartbeatPath) {
						note("keepalive: no interval heartbeat after the hidden row confirmed");
					} else if (!RequestListed(*keepalive, &listed) || listed) {
						note("keepalive: the interval heartbeat did not carry listed=false on a capable service");
					}
					s.client.Update(5000);
					CallAdvertise(s.client, SampleRegisterRequest(), false, true);
					s.client.Update(5000); // relist intent: a heartbeat with listed=true leaves immediately
					const NetDirectoryClient::Request* relist = SentAt(s, 3);
					if (!relist || relist->path != kHeartbeatPath) {
						note("relist: no heartbeat carried the restored intent");
					} else if (!RequestListed(*relist, &listed) || !listed) {
						note("relist: the heartbeat did not send listed=true");
					}
					s.client.Update(5000);
					if (s.client.GetSessionId() != "7b8c9d2e-1111-4222-8333-444455556666" || s.client.GetToken() != "tok") {
						note("identity: the session id or token changed across a visibility flip");
					}
					if (CountRequests(s, "DELETE", kSessionPath) != 0 || CountRequests(s, "POST", "/v1/sessions") != 1) {
						note("identity: a visibility flip deleted the row or registered a second session");
					}
					const json report = json::parse(s.client.BuildReportJson());
					if (report.value("desired_listed", json()) != json(true) || report.value("confirmed_listed", json()) != json(true) || report.value("supports_unlisted", json()) != json(true)) {
						note("report: desired/confirmed visibility or capability missing from BuildReportJson");
					}
				}

				{   // intent flips while register or heartbeat is in flight
					ScriptedClient s;
					s.replies->push_back({200, kRegisterCapable, ""});
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5,"listed":false})", ""});
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5,"listed":true})", ""});
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(0);
					CallAdvertise(s.client, SampleRegisterRequest(), false, true); // changed while the register is in flight
					s.client.Update(0);
					s.client.Update(0);
					if (s.sent->size() != 1) {
						note("pending-register: a visible flip during register still sent a hide");
					}
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(0); // hide intent: heartbeat listed=false now in flight
					CallAdvertise(s.client, SampleRegisterRequest(), false, true); // flip while that heartbeat is pending
					s.client.Update(0); // the in-flight ack (listed=false) lands; the new intent goes next
					const NetDirectoryClient::Request* relist = SentAt(s, 2);
					bool listed = false;
					if (!relist || relist->path != kHeartbeatPath) {
						note("pending-heartbeat: the newer intent was not sent after the in-flight ack");
					} else if (!RequestListed(*relist, &listed) || !listed) {
						note("pending-heartbeat: the follow-up heartbeat did not send listed=true");
					}
				}

				{   // echo absent on a capable service: never confirmed hidden, bounded retry resends
					ScriptedClient s;
					s.replies->push_back({200, kRegisterCapable, ""});
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5})", ""});
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5,"listed":false})", ""});
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(0);
					s.client.Update(0);
					s.client.Update(0);
					const json report = json::parse(s.client.BuildReportJson());
					if (report.value("confirmed_listed", json(true)) == json(false)) {
						note("echo-absent: an ack without listed claimed confirmed hidden");
					}
					if (s.client.GetState() == NetDirectoryClient::State::Failed) {
						note("echo-absent: a missing echo was terminal instead of a bounded retry");
					}
					s.client.Update(4999);
					if (s.sent->size() > 2) {
						note("echo-absent: the unconfirmed visibility retried before the backoff elapsed");
					}
					s.client.Update(5000);
					const NetDirectoryClient::Request* retry = SentAt(s, 2);
					bool listed = true;
					if (!retry || !RequestListed(*retry, &listed) || listed) {
						note("echo-absent: the bounded retry did not resend listed=false");
					}
				}

				{   // echo contradicts the in-flight intent: protocol failure, never confirmed
					ScriptedClient s;
					s.replies->push_back({200, kRegisterCapable, ""});
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5,"listed":true})", ""});
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5,"listed":false})", ""});
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(0);
					s.client.Update(0);
					s.client.Update(0);
					const json report = json::parse(s.client.BuildReportJson());
					if (report.value("confirmed_listed", json()) == json(false)) {
						note("echo-contradicted: a listed=true ack against a listed=false request claimed hidden");
					}
					s.client.Update(5000);
					const NetDirectoryClient::Request* retry = SentAt(s, 2);
					bool listed = true;
					if (!retry || !RequestListed(*retry, &listed) || listed) {
						note("echo-contradicted: the bounded retry did not resend listed=false");
					}
				}

				{   // malformed echo: the body refuses decode; the retry is bounded, never confirmed
					ScriptedClient s;
					s.replies->push_back({200, kRegisterCapable, ""});
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5,"listed":"yes"})", ""});
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5,"listed":false})", ""});
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(0);
					s.client.Update(0);
					s.client.Update(0);
					const json report = json::parse(s.client.BuildReportJson());
					if (report.value("confirmed_listed", json()) == json(false)) {
						note("echo-malformed: an undecodable ack claimed confirmed hidden");
					}
					s.client.Update(5000);
					const NetDirectoryClient::Request* retry = SentAt(s, 2);
					bool listed = true;
					if (!retry || !RequestListed(*retry, &listed) || listed) {
						note("echo-malformed: the bounded retry did not resend listed=false");
					}
				}

				{   // a 404 on a hidden row fails closed: no re-register, retract still deletes the id
					ScriptedClient s;
					s.replies->push_back({200, kRegisterCapable, ""});
					s.replies->push_back({404, R"({"error":"not_found"})", ""});
					s.replies->push_back({200, R"({"session_id":"8c9d2e1f-2222-4333-8444-555566667777","token":"tok2","expires_in_s":15,"heartbeat_s":5,"observed_ip":"127.0.0.1","supports_unlisted":true})", ""});
					s.replies->push_back({200, R"({"ok":true})", ""});
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(0);
					s.client.Update(0);
					s.client.Update(0);
					if (CountRequests(s, "POST", "/v1/sessions") != 1) {
						note("hidden-404: the lost hidden row re-registered a new visible identity");
					}
					if (s.client.GetState() != NetDirectoryClient::State::Failed) {
						note("hidden-404: the client did not fail closed on the lost hidden row");
					}
					s.client.Retract();
					s.client.Update(0);
					const NetDirectoryClient::Request* deleted = SentAt(s, 2);
					if (!deleted || deleted->method != "DELETE" || deleted->path != kSessionPath) {
						note("hidden-404: retract did not delete the known row after the failure");
					} else if (deleted->path.find("7b8c9d2e") == std::string::npos) {
						note("hidden-404: retract deleted a rebound identity instead of the lost row");
					}
				}

				if (misses.empty()) {
					return true;
				}
				*error = "client unlisted visibility misses (" + std::to_string(misses.size()) + "):";
				for (const std::string& miss : misses) {
					*error += " [" + miss + "]";
				}
				return false;
			}

			bool TestClientUnlistedLegacy(std::string* error) {
				std::vector<std::string> misses;
				auto note = [&misses](const std::string& miss) { misses.push_back(miss); };

				{   // a legacy service keeps the omission-shaped visible lifecycle unchanged
					ScriptedClient s;
					s.replies->push_back({200, kRegisterLegacy, ""});
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5})", ""});
					CallAdvertise(s.client, SampleRegisterRequest(), false, true);
					s.client.Update(0);
					s.client.Update(0);
					s.client.Update(5000);
					const NetDirectoryClient::Request* heartbeat = SentAt(s, 1);
					bool listed = false;
					if (!heartbeat || heartbeat->path != kHeartbeatPath) {
						note("legacy-visible: no interval heartbeat on a legacy service");
					} else if (RequestListed(*heartbeat, &listed)) {
						note("legacy-visible: a legacy heartbeat carried the listed field");
					}
				}

				{   // hidden intent on a legacy service deletes once and stays Failed, no loop
					ScriptedClient s;
					s.replies->push_back({200, kRegisterLegacy, ""});
					s.replies->push_back({200, R"({"ok":true})", ""});
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(0);
					s.client.Update(0);
					s.client.Update(0);
					const NetDirectoryClient::Request* deleted = SentAt(s, 1);
					if (!deleted || deleted->method != "DELETE" || deleted->path != kSessionPath) {
						note("legacy-hidden: the unsupported hidden row was never deleted");
					}
					s.client.Update(0);
					if (s.client.GetState() != NetDirectoryClient::State::Failed) {
						note("legacy-hidden: the client did not stay Failed for the unsupported intent");
					}
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(60000);
					s.client.Update(120000);
					if (CountRequests(s, "POST", "/v1/sessions") != 1 || CountRequests(s, "DELETE", kSessionPath) > 1) {
						note("legacy-hidden: a repeated hidden intent looped register/delete");
					}
					const json report = json::parse(s.client.BuildReportJson());
					if (report.value("supports_unlisted", json(true)) != json(false)) {
						note("legacy-hidden: the report did not expose capability=false");
					}
					s.replies->push_back({200, R"({"session_id":"8c9d2e1f-2222-4333-8444-555566667777","token":"tok2","expires_in_s":15,"heartbeat_s":5,"observed_ip":"127.0.0.1"})", ""});
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5})", ""});
					CallAdvertise(s.client, SampleRegisterRequest(), false, true);
					s.client.Update(120000);
					const NetDirectoryClient::Request* reregistered = SentAt(s, 2);
					if (!reregistered || reregistered->method != "POST" || reregistered->path != "/v1/sessions") {
						note("legacy-recover: a visible intent did not resume ordinary registration");
					}
					s.client.Update(120000);
					s.client.Update(125000); // ordinary interval: the recovered row heartbeats omission-shaped
					const NetDirectoryClient::Request* recovered = SentAt(s, 3);
					bool relisted = true;
					if (!recovered || recovered->path != "/v1/sessions/8c9d2e1f-2222-4333-8444-555566667777/heartbeat") {
						note("legacy-recover: the resumed registration did not heartbeat");
					} else if (RequestListed(*recovered, &relisted)) {
						note("legacy-recover: the recovered heartbeat carried the listed field");
					}
					if (CountRequests(s, "POST", "/v1/sessions") > 2 || CountRequests(s, "DELETE", kSessionPath) != 1) {
						note("legacy-recover: request counts were registers=" + std::to_string(CountRequests(s, "POST", "/v1/sessions")) + " deletes=" + std::to_string(CountRequests(s, "DELETE", kSessionPath)) + "");
					}
				}

				{   // an explicit supports_unlisted:false reply is the same legacy path as absent
					ScriptedClient s;
					s.replies->push_back({200, R"({"session_id":"7b8c9d2e-1111-4222-8333-444455556666","token":"tok","expires_in_s":15,"heartbeat_s":5,"observed_ip":"127.0.0.1","supports_unlisted":false})", ""});
					s.replies->push_back({200, R"({"ok":true})", ""});
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(0);
					s.client.Update(0);
					s.client.Update(0);
					const NetDirectoryClient::Request* deleted = SentAt(s, 1);
					if (!deleted || deleted->method != "DELETE" || deleted->path != kSessionPath) {
						note("legacy-hidden-explicit: capability=false did not delete the hidden row");
					}
					s.client.Update(0);
					if (s.client.GetState() != NetDirectoryClient::State::Failed) {
						note("legacy-hidden-explicit: capability=false did not stay Failed");
					}
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(60000);
					if (CountRequests(s, "POST", "/v1/sessions") != 1 || CountRequests(s, "DELETE", kSessionPath) > 1) {
						note("legacy-hidden-explicit: a repeated hidden intent looped after capability=false");
					}
				}

				{   // A visible row's actual 429 also gates a later retract.
					ScriptedClient s;
					s.replies->push_back({200, kRegisterLegacy, ""});
					s.replies->push_back({429, R"({"error":"rate_limited","retry_after_s":30})", ""});
					s.replies->push_back({200, R"({"ok":true})", ""});
					s.client.Advertise(SampleRegisterRequest(), false);
					s.client.Update(0);
					s.client.Update(0);
					s.client.Update(5000);
					const NetDirectoryClient::Request* heartbeat = SentAt(s, 1);
					if (!heartbeat || heartbeat->method != "POST" || heartbeat->path != kHeartbeatPath) {
						note("visible-retract-429 fixture: the interval heartbeat never left");
					}
					s.client.Update(5000);
					s.client.Retract();
					s.client.Update(5000);
					s.client.Update(34999);
					if (CountRequests(s, "DELETE", kSessionPath) != 0) {
						note("visible-retract-429: DELETE left before the t=35000 retry deadline");
					}
					s.client.Update(35000);
					if (CountRequests(s, "DELETE", kSessionPath) != 1) {
						note("visible-retract-429: the row did not receive exactly one DELETE by t=35000");
					}
				}

				{   // a 429 while the visibility intent is dirty still honors retry_after
					ScriptedClient s;
					s.replies->push_back({200, kRegisterCapable, ""});
					s.replies->push_back({429, R"({"error":"rate_limited","retry_after_s":30})", ""});
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5,"listed":false})", ""});
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(0);
					s.client.Update(0);
					s.client.Update(0);
					const NetDirectoryClient::Request* hide = SentAt(s, 1);
					bool listed = true;
					if (!hide || !RequestListed(*hide, &listed) || listed) {
						note("dirty-429: the hidden intent never left in a heartbeat");
					}
					s.client.Update(29999);
					if (s.sent->size() > 2) {
						note("dirty-429: a request left while retry_after was pending");
					}
					s.client.Update(30000);
					const NetDirectoryClient::Request* retry = SentAt(s, 2);
					if (!retry || !RequestListed(*retry, &listed) || listed) {
						note("dirty-429: the retry after retry_after did not resend listed=false");
					}
				}

				{   // retract after a visibility failure still deletes the known row, past the deadline
					ScriptedClient s;
					s.replies->push_back({200, kRegisterCapable, ""});
					s.replies->push_back({429, R"({"error":"rate_limited","retry_after_s":30})", ""});
					s.replies->push_back({200, R"({"ok":true})", ""});
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(0);
					s.client.Update(0);
					s.client.Update(0); // the hidden heartbeat is throttled; the intent stays dirty
					s.client.Retract();
					s.client.Update(0);
					s.client.Update(29999);
					if (CountRequests(s, "DELETE", kSessionPath) != 0) {
						note("retract-dirty: the delete raced ahead of the 429 retry deadline");
					}
					s.client.Update(30000);
					const NetDirectoryClient::Request* deleted = SentAt(s, 2);
					if (!deleted || deleted->method != "DELETE" || deleted->path != kSessionPath) {
						note("retract-dirty: the known row was not deleted after the deadline");
					}
				}

				if (misses.empty()) {
					return true;
				}
				*error = "client unlisted legacy misses (" + std::to_string(misses.size()) + "):";
				for (const std::string& miss : misses) {
					*error += " [" + miss + "]";
				}
				return false;
			}

			uint64_t SteadyMs() {
				return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
			}

			const char* kSecretToken = "tok-report-secret-5e1d";
			const char* kRegisterCapableSecret = R"({"session_id":"7b8c9d2e-1111-4222-8333-444455556666","token":"tok-report-secret-5e1d","expires_in_s":15,"heartbeat_s":5,"observed_ip":"127.0.0.1","supports_unlisted":true})";

			std::string VisibilityFields(const json& report) {
				return "desired=" + report.value("desired_listed", json("absent")).dump() + " confirmed=" + report.value("confirmed_listed", json("absent")).dump() + " capable=" + report.value("supports_unlisted", json("absent")).dump();
			}
			/// Without a held row the report claims no confirmed visibility and no capability.
			bool ReportRowless(const json& report) {
				return report.contains("confirmed_listed") && report["confirmed_listed"].is_null() &&
				       report.contains("supports_unlisted") && report["supports_unlisted"].is_boolean() && !report["supports_unlisted"].get<bool>();
			}
			bool ReportLeaksToken(const std::string& report) {
				return report.find(kSecretToken) != std::string::npos || json::parse(report).contains("token");
			}

			bool TestClientUnlistedDeadlines(std::string* error) {
				std::vector<std::string> misses;
				auto note = [&misses](const std::string& miss) { misses.push_back(miss); };

				// Retract then re-advertise inside a heartbeat 429 (retry at t=30000) or an echo-less ack's
				// backoff (retry at t=5000): the new intent keeps the deadline and the same row.
				const struct {
					const char* name;
					NetDirectoryClient::Reply answer;
					uint64_t deadline;
				} readvertise[] = {
					{"readvertise-429", {429, R"({"error":"rate_limited","retry_after_s":30})", ""}, 30000},
					{"readvertise-backoff", {200, R"({"expires_in_s":15,"heartbeat_s":5})", ""}, 5000},
				};
				for (const auto& arm : readvertise) {
					const std::string name = arm.name;
					ScriptedClient s;
					s.replies->push_back({200, kRegisterCapable, ""});
					s.replies->push_back(arm.answer);
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5,"listed":false})", ""});
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(0);
					s.client.Update(0);
					s.client.Update(0); // the hide heartbeat's answer sets the retry deadline
					s.client.Retract();
					s.client.Update(1000);
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(2000);
					const size_t afterReadvertise = s.sent->size();
					s.client.Update(arm.deadline - 1);
					if (afterReadvertise != 2 || s.sent->size() != 2) {
						note(name + ": a request left at t=" + std::to_string(afterReadvertise != 2 ? uint64_t{2000} : arm.deadline - 1) + ", inside the t=" + std::to_string(arm.deadline) + " retry deadline");
					}
					s.client.Update(arm.deadline);
					const NetDirectoryClient::Request* hide = SentAt(s, 2);
					bool listed = true;
					if (!hide || hide->method != "POST" || hide->path != kHeartbeatPath || !RequestListed(*hide, &listed) || listed || RequestToken(*hide) != "tok") {
						note(name + ": the hidden heartbeat did not resume on the same row at the deadline");
					}
					s.client.Update(arm.deadline);
					if (CountRequests(s, "DELETE", kSessionPath) != 0 || CountRequests(s, "POST", "/v1/sessions") != 1 || s.client.GetToken() != "tok") {
						note(name + ": the re-advertise deleted the row, registered again or changed the token");
					}
					if (json::parse(s.client.BuildReportJson()).value("confirmed_listed", json()) != json(false)) {
						note(name + ": the acknowledged hidden intent was not confirmed");
					}
				}

				{   // the default visible API: retract then re-advertise inside a heartbeat 503 backoff keeps it
					ScriptedClient s;
					s.replies->push_back({200, kRegisterLegacy, ""});
					s.replies->push_back({503, R"({"error":"unavailable"})", ""});
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5})", ""});
					s.client.Advertise(SampleRegisterRequest(), false);
					s.client.Update(0);
					s.client.Update(0);
					s.client.Update(5000);
					s.client.Update(5000); // the interval heartbeat's 503 backs off until t=10000
					s.client.Retract();
					s.client.Update(6000);
					s.client.Advertise(SampleRegisterRequest(), false);
					s.client.Update(7000);
					const size_t afterReadvertise = s.sent->size();
					s.client.Update(9999);
					if (afterReadvertise != 2 || s.sent->size() != 2) {
						note("readvertise-visible-backoff: a request left at t=" + std::string(afterReadvertise != 2 ? "7000" : "9999") + ", inside the t=10000 backoff deadline");
					}
					s.client.Update(10000);
					const NetDirectoryClient::Request* heartbeat = SentAt(s, 2);
					if (!heartbeat || heartbeat->method != "POST" || heartbeat->path != kHeartbeatPath) {
						note("readvertise-visible-backoff: the heartbeat did not resume on the same row at t=10000");
					}
					s.client.Update(10000);
					if (CountRequests(s, "DELETE", kSessionPath) != 0 || CountRequests(s, "POST", "/v1/sessions") != 1 || s.client.GetState() != NetDirectoryClient::State::Registered) {
						note("readvertise-visible-backoff: the re-advertise deleted the row or registered again");
					}
				}

				{   // the default visible API: retract then re-advertise inside a register 429 keeps retry_after
					ScriptedClient s;
					s.replies->push_back({429, R"({"error":"rate_limited","retry_after_s":30})", ""});
					s.replies->push_back({200, kRegisterLegacy, ""});
					s.client.Advertise(SampleRegisterRequest(), false);
					s.client.Update(0);
					s.client.Update(0); // the register's 429 holds every request until t=30000
					s.client.Retract();
					s.client.Update(1000);
					s.client.Advertise(SampleRegisterRequest(), false);
					s.client.Update(2000);
					const size_t afterReadvertise = s.sent->size();
					s.client.Update(29999);
					if (afterReadvertise != 1 || s.sent->size() != 1) {
						note("readvertise-register-429: a register left at t=" + std::string(afterReadvertise != 1 ? "2000" : "29999") + ", inside the t=30000 retry deadline");
					}
					s.client.Update(30000);
					s.client.Update(30000);
					if (CountRequests(s, "POST", "/v1/sessions") != 2 || s.client.GetState() != NetDirectoryClient::State::Registered) {
						note("readvertise-register-429: the register did not resume at t=30000");
					}
				}

				// A contradicting or undecodable echo retries at 5000 ms, then 10000 ms, and is never terminal.
				const struct {
					const char* name;
					const char* body;
				} mismatches[] = {
					{"echo-contradicted-timing", R"({"expires_in_s":15,"heartbeat_s":5,"listed":true})"},
					{"echo-malformed-timing", R"({"expires_in_s":15,"heartbeat_s":5,"listed":"yes"})"},
				};
				for (const auto& arm : mismatches) {
					const std::string name = arm.name;
					ScriptedClient s;
					s.replies->push_back({200, kRegisterCapable, ""});
					s.replies->push_back({200, arm.body, ""});
					s.replies->push_back({200, arm.body, ""});
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5,"listed":false})", ""});
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(0);
					s.client.Update(0);
					s.client.Update(0); // first mismatch: retry at t=5000
					if (s.client.GetState() == NetDirectoryClient::State::Failed) {
						note(name + ": the mismatched echo was terminal instead of a bounded retry");
					}
					if (json::parse(s.client.BuildReportJson()).value("confirmed_listed", json()) != json(true)) {
						note(name + ": the register's visible confirmation did not survive the mismatch");
					}
					s.client.Update(4999);
					const size_t beforeFirst = s.sent->size();
					s.client.Update(5000);
					const NetDirectoryClient::Request* first = SentAt(s, 2);
					bool listed = true;
					if (beforeFirst != 2 || !first || !RequestListed(*first, &listed) || listed) {
						note(name + ": the first retry did not resend listed=false at exactly t=5000");
					}
					s.client.Update(5000); // second mismatch: the backoff doubles, retry at t=15000
					s.client.Update(14999);
					const size_t beforeSecond = s.sent->size();
					s.client.Update(15000);
					const NetDirectoryClient::Request* second = SentAt(s, 3);
					listed = true;
					if (beforeSecond != 3 || !second || !RequestListed(*second, &listed) || listed) {
						note(name + ": the second retry did not resend listed=false at exactly t=15000");
					}
					s.client.Update(15000);
					if (json::parse(s.client.BuildReportJson()).value("confirmed_listed", json()) != json(false)) {
						note(name + ": the matching ack after the retries was not confirmed hidden");
					}
				}

				if (misses.empty()) {
					return true;
				}
				*error = "client unlisted deadline misses (" + std::to_string(misses.size()) + "):";
				for (const std::string& miss : misses) {
					*error += " [" + miss + "]";
				}
				return false;
			}

			bool TestClientUnlistedReportTruth(std::string* error) {
				std::vector<std::string> misses;
				auto note = [&misses](const std::string& miss) { misses.push_back(miss); };

				{   // nothing registered yet: no confirmation and no capability are claimed
					ScriptedClient s;
					const json report = json::parse(s.client.BuildReportJson());
					if (!ReportRowless(report)) {
						note("fresh: the report claimed " + VisibilityFields(report) + " before any register");
					}
				}

				{   // a held capable row reports typed fields; once deleted it claims nothing; never the token
					ScriptedClient s;
					s.replies->push_back({200, kRegisterCapableSecret, ""});
					s.replies->push_back({200, R"({"ok":true})", ""});
					s.client.Advertise(SampleRegisterRequest(), false);
					s.client.Update(0);
					s.client.Update(0);
					const std::string held = s.client.BuildReportJson();
					const json heldReport = json::parse(held);
					if (heldReport.value("desired_listed", json()) != json(true) || heldReport.value("confirmed_listed", json()) != json(true) || heldReport.value("supports_unlisted", json()) != json(true)) {
						note("held-visible: the report showed " + VisibilityFields(heldReport) + " for a registered capable visible row");
					}
					s.client.Retract();
					s.client.Update(0);
					s.client.Update(0);
					const std::string deleted = s.client.BuildReportJson();
					const json deletedReport = json::parse(deleted);
					if (s.client.GetState() != NetDirectoryClient::State::Idle || CountRequests(s, "DELETE", kSessionPath) != 1) {
						note("deleted fixture: the retract did not delete the row and settle Idle");
					} else if (!ReportRowless(deletedReport)) {
						note("deleted: the report still claimed " + VisibilityFields(deletedReport) + " after the row was deleted");
					}
					if (ReportLeaksToken(held) || ReportLeaksToken(deleted)) {
						note("token: the report exposed the session token");
					}
				}

				{   // a hidden row lost to a 404 keeps its id for Retract but claims no confirmation
					ScriptedClient s;
					s.replies->push_back({200, kRegisterCapableSecret, ""});
					s.replies->push_back({200, R"({"expires_in_s":15,"heartbeat_s":5,"listed":false})", ""});
					s.replies->push_back({404, R"({"error":"not_found"})", ""});
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(0);
					s.client.Update(0);
					s.client.Update(0); // the hide is acknowledged
					const std::string hidden = s.client.BuildReportJson();
					const json hiddenReport = json::parse(hidden);
					if (hiddenReport.value("desired_listed", json()) != json(false) || hiddenReport.value("confirmed_listed", json()) != json(false) || hiddenReport.value("supports_unlisted", json()) != json(true)) {
						note("held-hidden: the report showed " + VisibilityFields(hiddenReport) + " for an acknowledged hidden row");
					}
					s.client.Update(5000);
					s.client.Update(5000); // the keepalive answers 404
					const std::string lost = s.client.BuildReportJson();
					const json lostReport = json::parse(lost);
					if (s.client.GetState() != NetDirectoryClient::State::Failed || s.client.GetSessionId().empty()) {
						note("lost-hidden fixture: the 404 did not fail closed with the row id kept");
					} else if (!ReportRowless(lostReport)) {
						note("lost-hidden: the report still claimed " + VisibilityFields(lostReport) + " after the 404");
					}
					if (ReportLeaksToken(hidden) || ReportLeaksToken(lost)) {
						note("token: the report exposed the session token");
					}
				}

				{   // the legacy hidden delete leaves no confirmation behind
					ScriptedClient s;
					s.replies->push_back({200, kRegisterLegacy, ""});
					s.replies->push_back({200, R"({"ok":true})", ""});
					CallAdvertise(s.client, SampleRegisterRequest(), false, false);
					s.client.Update(0);
					s.client.Update(0);
					s.client.Update(0);
					const json report = json::parse(s.client.BuildReportJson());
					if (s.client.GetState() != NetDirectoryClient::State::Failed || CountRequests(s, "DELETE", kSessionPath) != 1) {
						note("legacy-deleted fixture: the unsupported hidden row was not deleted into Failed");
					} else if (!ReportRowless(report)) {
						note("legacy-deleted: the report still claimed " + VisibilityFields(report) + " after the unsupported row was deleted");
					}
				}

				{   // Shutdown with no deadline deletes the held row, then claims nothing about it
					ScriptedClient s;
					s.replies->push_back({200, kRegisterCapableSecret, ""});
					s.replies->push_back({200, R"({"ok":true})", ""});
					const uint64_t base = SteadyMs();
					s.client.Advertise(SampleRegisterRequest(), false);
					s.client.Update(base);
					s.client.Update(base);
					s.client.Shutdown();
					const json report = json::parse(s.client.BuildReportJson());
					if (CountRequests(s, "DELETE", kSessionPath) != 1 || report.value("deletes", json()) != json(1)) {
						note("shutdown fixture: Shutdown did not delete the held row once");
					} else if (!ReportRowless(report)) {
						note("shutdown: the report still claimed " + VisibilityFields(report) + " after Shutdown deleted the row");
					}
				}

				{   // Shutdown inside a heartbeat 429 keeps retry_after: no DELETE within its budget, none counted
					ScriptedClient s;
					s.replies->push_back({200, kRegisterCapableSecret, ""});
					s.replies->push_back({429, R"({"error":"rate_limited","retry_after_s":30})", ""});
					s.replies->push_back({200, R"({"ok":true})", ""});
					const uint64_t base = SteadyMs();
					s.client.Advertise(SampleRegisterRequest(), false);
					s.client.Update(base);
					s.client.Update(base);
					s.client.Update(base + 5000);
					s.client.Update(base + 5000); // the interval heartbeat is throttled until base+35000
					const auto begin = std::chrono::steady_clock::now();
					s.client.Shutdown();
					const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
					const json report = json::parse(s.client.BuildReportJson());
					if (CountRequests(s, "DELETE", kSessionPath) != 0 || report.value("deletes", json()) != json(0)) {
						note("shutdown-429: Shutdown sent or counted a DELETE inside retry_after");
					}
					if (elapsedMs > static_cast<long long>(NetDirectoryClient::c_ShutdownBudgetMs) + 500) {
						note("shutdown-429: Shutdown blocked " + std::to_string(elapsedMs) + " ms, past its budget");
					}
					if (s.client.GetState() != NetDirectoryClient::State::Idle || !s.client.GetSessionId().empty()) {
						note("shutdown-429: Shutdown did not release the row id after its budget");
					} else if (!ReportRowless(report)) {
						note("shutdown-429: the report still claimed " + VisibilityFields(report) + " for the row left to expire");
					}
					std::cout << "[net-directory-selftest] shutdown inside retry_after: deletes=" << report.value("deletes", json()).dump() << " elapsed_ms=" << elapsedMs << ", the row is left to the service's expiry" << std::endl;
				}

				{   // Reject absent or malformed row visibility.
					auto mutate = [](bool hasConfirmed, const json& confirmed, bool hasCapable, const json& capable) {
						json r = {{"state", "idle"}, {"desired_listed", false}};
						if (hasConfirmed) { r["confirmed_listed"] = confirmed; }
						if (hasCapable) { r["supports_unlisted"] = capable; }
						return r;
					};
					const struct { const char* name; json report; bool rowless; } mutations[] = {
						{"confirmed-missing", mutate(false, json(), true, false), false},
						{"confirmed-string", mutate(true, "yes", true, false), false},
						{"confirmed-number", mutate(true, 3, true, false), false},
						{"confirmed-boolean", mutate(true, false, true, false), false},
						{"confirmed-null", mutate(true, nullptr, true, false), true},
						{"capable-missing", mutate(true, nullptr, false, json()), false},
						{"capable-string", mutate(true, nullptr, true, "yes"), false},
						{"capable-true", mutate(true, nullptr, true, true), false},
					};
					for (const auto& m : mutations) {
						if (ReportRowless(m.report) != m.rowless) {
							note(std::string("rowless-shape: ReportRowless ") + (m.rowless ? "rejected " : "accepted ") + m.name);
						}
					}
				}

				if (misses.empty()) {
					return true;
				}
				*error = "client unlisted report misses (" + std::to_string(misses.size()) + "):";
				for (const std::string& miss : misses) {
					*error += " [" + miss + "]";
				}
				return false;
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
				if (lanRow.source != "LAN" || lanRow.joinable || lanRow.reason != "beacon" || lanRow.address != "10.0.0.5" || lanRow.port != 42000 || lanRow.players != "1/2") {
					*error = "a LAN row without beacon fields did not merge listed-but-not-joinable";
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

			bool TestJoinListLabels(std::string* error) {
				const NetDirectoryLocalIdentity local = SampleLocal();
				std::string failures;
				auto note = [&failures](const std::string& text) {
					failures += (failures.empty() ? "" : "; ") + text;
				};

				// The session identity hash contains the module manifest hash, so a mod-only
				// difference differs in both fields; the row must name the module field.
				NetDirectorySessionRow modded = SampleRow();
				modded.moduleManifestHash = kHex64A;
				modded.sessionIdentityHash = kHex64A;
				std::string reason;
				if (NetDirectoryCodec::IsJoinable(modded, local, &reason)) {
					note("a modded-host row was joinable");
				} else if (reason != "incompatible: module_manifest_hash") {
					note("modded-host reason \"" + reason + "\", expected incompatible: module_manifest_hash");
				}
				NetDirectorySessionRow identityOnly = SampleRow();
				identityOnly.sessionIdentityHash = kHex64A;
				reason.clear();
				if (NetDirectoryCodec::IsJoinable(identityOnly, local, &reason)) {
					note("an identity-mismatch row was joinable");
				} else if (reason != "incompatible: session_identity_hash") {
					note("identity-mismatch reason \"" + reason + "\", expected incompatible: session_identity_hash");
				}
				const std::vector<NetDirectoryClient::GameRow> netMerged =
					NetDirectoryClient::MergeGameLists({}, {modded, identityOnly}, local);
				if (netMerged.size() != 2) {
					note("NET merge size " + std::to_string(netMerged.size()));
				} else {
					if (netMerged[0].joinable || netMerged[0].reason != "modules") {
						note("modded-host NET row joinable=" + std::to_string(netMerged[0].joinable) + " reason=\"" + netMerged[0].reason + "\", expected joinable=no reason=modules");
					}
					if (netMerged[1].joinable || netMerged[1].reason != "identity") {
						note("identity-mismatch NET row joinable=" + std::to_string(netMerged[1].joinable) + " reason=\"" + netMerged[1].reason + "\", expected joinable=no reason=identity");
					}
				}

				// A v1 beacon carries no compatibility fields: the row must list but never be joinable.
				NetLanDiscovery oldBeacon;
				NetLanDiscovery oldBrowser;
				std::string setupError;
				NetLanHostInfo oldHost;
				bool listed = false;
				if (!oldBrowser.StartBrowser(&setupError) ||
					!oldBeacon.StartBeacon(47572, "W94OldHost", "P4 Alpha Duel", "pvp-skirmish", 1, 2, &setupError)) {
					*error = "v1 beacon pair setup failed: " + setupError;
					return false;
				}
				for (uint64_t nowMs = 0; nowMs <= 3000 && !listed; nowMs += 50) {
					oldBeacon.Tick(nowMs);
					oldBrowser.Tick(nowMs);
					for (const NetLanHostInfo& host : oldBrowser.GetHosts(nowMs)) {
						if (host.hostName == "W94OldHost" && host.port == 47572) {
							oldHost = host;
							listed = true;
						}
					}
					if (!listed) {
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
					}
				}
				oldBeacon.Stop();
				oldBrowser.Stop();
				if (!listed) {
					note("a v1 beacon never listed");
				} else {
					const std::vector<NetDirectoryClient::GameRow> lanMerged = NetDirectoryClient::MergeGameLists({oldHost}, {}, local);
					if (lanMerged.size() != 1 || lanMerged[0].joinable || lanMerged[0].reason != "beacon") {
						note("v1-beacon LAN row merged " + (lanMerged.empty() ? std::string("<missing>") :
							"joinable=" + std::to_string(lanMerged[0].joinable) + " reason=\"" + lanMerged[0].reason + "\"") +
							", expected joinable=no reason=beacon");
					}
				}

				// A v2 beacon carries the five fields; the row is judged with the same predicate.
				NetLanCompatIdentity compat;
				compat.networkProtocolVersion = local.networkProtocolVersion;
				compat.lockstepCodecVersion = local.lockstepCodecVersion;
				compat.controllerFrameVersion = local.controllerFrameVersion;
				compat.sessionIdentityHash = local.sessionIdentityHash;
				compat.moduleManifestHash = local.moduleManifestHash;
				NetLanDiscovery beaconV2;
				NetLanDiscovery browserV2;
				NetLanHostInfo hostV2;
				bool sawV2 = false;
				if (!browserV2.StartBrowser(&setupError) ||
					!beaconV2.StartBeacon(47571, "W94Host", "P4 Alpha Duel", "pvp-skirmish", 1, 2, &compat, &setupError)) {
					*error = "v2 beacon pair setup failed: " + setupError;
					return false;
				}
				for (uint64_t nowMs = 0; nowMs <= 3000 && !sawV2; nowMs += 50) {
					beaconV2.Tick(nowMs);
					browserV2.Tick(nowMs);
					for (const NetLanHostInfo& host : browserV2.GetHosts(nowMs)) {
						if (host.hostName == "W94Host" && host.port == 47571) {
							hostV2 = host;
							sawV2 = true;
						}
					}
					if (!sawV2) {
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
					}
				}
				beaconV2.Stop();
				browserV2.Stop();
				if (!sawV2) {
					note("a v2 beacon never listed");
				} else {
					if (!hostV2.hasCompatibility || !(hostV2.compatibility == compat)) {
						note("the v2 beacon did not round-trip its compatibility fields");
					}
					NetLanHostInfo lanModded = hostV2;
					lanModded.port = 47570;
					lanModded.compatibility.moduleManifestHash = kHex64A;
					lanModded.compatibility.sessionIdentityHash = kHex64A;
					const std::vector<NetDirectoryClient::GameRow> lanMerged2 = NetDirectoryClient::MergeGameLists({hostV2, lanModded}, {}, local);
					if (lanMerged2.size() != 2) {
						note("v2 LAN merge size " + std::to_string(lanMerged2.size()));
					} else {
						if (!lanMerged2[0].joinable) {
							note("matching v2-beacon LAN row was not joinable, reason=\"" + lanMerged2[0].reason + "\"");
						}
						if (lanMerged2[1].joinable || lanMerged2[1].reason != "modules") {
							note("modded v2-beacon LAN row joinable=" + std::to_string(lanMerged2[1].joinable) + " reason=\"" + lanMerged2[1].reason + "\", expected joinable=no reason=modules");
						}
					}
				}
				if (!failures.empty()) {
					*error = failures;
					return false;
				}
				return true;
			}

			std::string ListRowId(int n) {
				const std::string tail = std::to_string(n);
				return "7b8c9d2e-1111-4222-8333-" + std::string(12 - tail.size(), '0') + tail;
			}

			std::vector<NetDirectorySessionRow> MakeListRows(int begin, int count) {
				std::vector<NetDirectorySessionRow> rows;
				rows.reserve(static_cast<size_t>(count));
				for (int i = 0; i < count; ++i) {
					NetDirectorySessionRow row = SampleRow();
					row.sessionId = ListRowId(begin + i);
					rows.push_back(std::move(row));
				}
				return rows;
			}

			std::string ListPageBody(const std::vector<NetDirectorySessionRow>& rows, const std::string& nextCursor, int64_t total) {
				NetDirectoryListResponse list;
				list.sessions = rows;
				list.nextCursor = nextCursor;
				list.total = total;
				return NetDirectoryCodec::EncodeListResponse(list);
			}

			bool TestListPagination(std::string* error) {
				const std::string firstPath = std::string("/v1/sessions?limit=") + std::to_string(NetDirectoryClient::c_ListPageLimit);
				const std::string secondPath = firstPath + "&cursor=n1";
				{
					ScriptedClient s;
					s.replies->push_back({200, ListPageBody(MakeListRows(0, 100), "n1", 140), ""});
					s.replies->push_back({200, ListPageBody(MakeListRows(100, 40), "", 140), ""});
					s.client.PollList(0);
					s.client.Update(0);
					s.client.Update(0);
					if (s.sent->size() != 2 || !RequestIs(s.sent->at(0), "GET", firstPath.c_str(), error) || !RequestIs(s.sent->at(1), "GET", secondPath.c_str(), error)) {
						*error = error->empty() ? "list pagination sent the wrong requests" : *error;
						return false;
					}
					if (s.client.Rows().size() != 140) {
						*error = "merged list size " + std::to_string(s.client.Rows().size());
						return false;
					}
					const std::vector<NetDirectoryClient::GameRow> merged = NetDirectoryClient::MergeGameLists({}, s.client.Rows(), SampleLocal());
					if (merged.size() != 140) {
						*error = "MergeGameLists size " + std::to_string(merged.size());
						return false;
					}
					const json report = json::parse(s.client.BuildReportJson());
					if (report["list_pages"] != 2 || report["list_total"] != 140) {
						*error = "list report " + report.dump();
						return false;
					}
				}
				{
					ScriptedClient s;
					s.replies->push_back({200, ListPageBody(MakeListRows(0, 100), "n1", 200), ""});
					s.replies->push_back({500, R"({"error":"internal"})", ""});
					s.client.PollList(0);
					s.client.Update(0);
					s.client.Update(0);
					if (s.sent->size() != 2 || !RequestIs(s.sent->at(0), "GET", firstPath.c_str(), error) || !RequestIs(s.sent->at(1), "GET", secondPath.c_str(), error)) {
						*error = error->empty() ? "page-2 failure sent the wrong requests" : *error;
						return false;
					}
					if (s.client.Rows().size() != 100) {
						*error = "page-2 failure dropped page 1, size " + std::to_string(s.client.Rows().size());
						return false;
					}
					const json report = json::parse(s.client.BuildReportJson());
					if (!report.contains("last_error") || report["last_error"].get<std::string>().find("list") == std::string::npos) {
						*error = "page-2 failure left no list note: " + report.dump();
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

			// An ICE row is reached through its session id, so it carries no address to be refused for.
			bool TestMergeAcceptsIceRows(std::string* error) {
				const NetDirectoryLocalIdentity local = SampleLocal();
				NetDirectorySessionRow ice = SampleRow();
				ice.joinMode = "ice";
				ice.listenAddrs.clear();
				ice.listenPort = 0;
				NetDirectorySessionRow either = SampleRow();
				either.joinMode = "either";
				NetDirectorySessionRow eitherNoAddr = SampleRow();
				eitherNoAddr.joinMode = "either";
				eitherNoAddr.listenAddrs.clear();
				eitherNoAddr.listenPort = 0;
				NetDirectorySessionRow ip = SampleRow();
				ip.joinMode = "ip";
				ip.listenAddrs.clear();
				ip.listenPort = 0;
				NetDirectorySessionRow iceFull = SampleRow();
				iceFull.joinMode = "ice";
				iceFull.listenAddrs.clear();
				iceFull.listenPort = 0;
				iceFull.seatsFree = 0;

				const std::vector<NetDirectoryClient::GameRow> merged = NetDirectoryClient::MergeGameLists({}, {ice, either, eitherNoAddr, ip, iceFull}, local);
				if (merged.size() != 5) {
					*error = "ice merge: list size " + std::to_string(merged.size());
					return false;
				}
				const char* names[] = {"ice", "either", "either without an address", "ip without an address", "full ice"};
				const bool wantJoinable[] = {true, true, true, false, false};
				const char* wantReason[] = {"", "", "", "address", "full"};
				for (size_t i = 0; i < merged.size(); ++i) {
					if (merged[i].joinable != wantJoinable[i] || merged[i].reason != wantReason[i]) {
						*error = std::string("ice merge: the ") + names[i] + " row came back joinable=" + (merged[i].joinable ? "1" : "0") +
						         " reason=\"" + merged[i].reason + "\", expected joinable=" + (wantJoinable[i] ? "1" : "0") + " reason=\"" + wantReason[i] + "\"";
						return false;
					}
					if (merged[i].sessionId != SampleRow().sessionId) {
						*error = std::string("ice merge: the ") + names[i] + " row lost its session id";
						return false;
					}
				}
				std::cout << "[net-directory-selftest] ice rows: join_mode ice/either is joinable without an address, ip without one still refuses \"address\", and a full ice row still refuses \"full\"" << std::endl;
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

			bool TestSignalLongPoll(std::string* error) {
				ScriptedChannel s(false);
				const std::string me = s.channel.GetLocalPeer();
				s.channel.SetPollWait(7);
				s.replies->push_back({200, SignalListBody({{1, "host", me, B64("one")}}), ""});
				s.replies->push_back({200, SignalListBody({}), ""});

				s.channel.SetPolling(true);
				s.channel.Update(0);
				s.channel.Update(1); // poll 1 answers; a long-poll re-issues at once, not after c_PollIntervalMs
				if (s.sent->size() != 2) {
					*error = "signal long-poll: " + std::to_string(s.sent->size()) + " requests, expected the next GET right after the previous returned";
					return false;
				}
				for (size_t i = 0; i < s.sent->size(); ++i) {
					const std::string expected = s.PollPath(static_cast<int64_t>(i)) + "&wait=7";
					if (!RequestIs(s.sent->at(i), "GET", expected.c_str(), error)) {
						*error = "signal long-poll: poll " + std::to_string(i + 1) + ": " + *error;
						return false;
					}
				}
				if (Taken(s) != "1:one") {
					*error = "signal long-poll: the sink took " + Taken(s) + ", expected 1:one";
					return false;
				}
				// The wait is clamped under the HTTP client's total request timeout.
				ScriptedChannel clamped(false);
				clamped.channel.SetPollWait(60);
				clamped.channel.SetPolling(true);
				clamped.channel.Update(0);
				const std::string capped = clamped.PollPath(0) + "&wait=" + std::to_string(NetDirectorySignalChannel::c_MaxPollWaitS);
				if (clamped.sent->size() != 1 || !RequestIs(clamped.sent->at(0), "GET", capped.c_str(), error)) {
					*error = "signal long-poll clamp: " + (clamped.sent->empty() ? std::string("no poll issued") : *error);
					return false;
				}
				// No wait configured: the poll URL and the 500 ms interval are unchanged.
				ScriptedChannel plain(false);
				plain.replies->push_back({200, SignalListBody({}), ""});
				plain.channel.SetPolling(true);
				plain.channel.Update(0);
				plain.channel.Update(0);
				plain.channel.Update(499);
				if (plain.sent->size() != 1 || !RequestIs(plain.sent->at(0), "GET", plain.PollPath(0).c_str(), error)) {
					*error = "signal long-poll default: the unconfigured poll changed shape or left before the 500 ms interval";
					return false;
				}
				std::cout << "[net-directory-selftest] signal long-poll: wait=7 on the wire, the next GET issues right after each return, the wait clamps to " << NetDirectorySignalChannel::c_MaxPollWaitS << " s, unset keeps the 500 ms interval" << std::endl;
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
			if (!TestSessionsCountCap(&error)) return fail(error);
			if (!TestCompatibilityPredicate(&error)) return fail(error);
			if (!TestCannedSequence(&error)) return fail(error);
			if (!TestUnlistedVisibility(&error)) return fail(error);
			if (!TestHttpClientReuse(&error)) return fail(error);
			if (!TestClientLifecycle(&error)) return fail(error);
			if (!TestHeartbeat404Reregisters(&error)) return fail(error);
			if (!TestHeartbeat429HonorsRetryAfter(&error)) return fail(error);
			if (!TestTransportErrorBackoff(&error)) return fail(error);
			if (!TestJoinListLabels(&error)) return fail(error);
			if (!TestMergeGameLists(&error)) return fail(error);
			if (!TestMergeAcceptsIceRows(&error)) return fail(error);
			if (!TestListPagination(&error)) return fail(error);
			if (!TestSignalOrderingAndCursor(&error)) return fail(error);
			if (!TestSignalCursorWaitsForSink(&error)) return fail(error);
			if (!TestSignalLongPoll(&error)) return fail(error);
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
#ifdef _WIN32
			if (!TestHttpClientStress(&error)) return fail(error);
			if (!TestHttpClientCancelDuringCallback(&error)) return fail(error);
#endif
#endif
			if (!TestInstallKeyIsLazy(&error)) return fail(error);
			// The unlisted groups aggregate their misses and run last, so a miss never skips the tests above.
			std::string unlistedMisses;
			for (bool (*test)(std::string*) : {TestClientUnlistedCapable, TestClientUnlistedLegacy, TestClientUnlistedDeadlines, TestClientUnlistedReportTruth}) {
				if (!test(&error)) {
					unlistedMisses += (unlistedMisses.empty() ? "" : " ") + error;
					error.clear();
				}
			}
			if (!unlistedMisses.empty()) {
				return fail(unlistedMisses);
			}

			std::cout << "[net-directory-selftest] PASS" << std::endl;
			return 0;
		}

	} // namespace NetDirectorySelfTest

} // namespace RTE
