#include "NetDirectoryCodec.h"

#include "nlohmann/json.hpp"

#include <functional>
#include <iostream>
#include <string>
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

			std::cout << "[net-directory-selftest] PASS" << std::endl;
			return 0;
		}

	} // namespace NetDirectorySelfTest

} // namespace RTE
