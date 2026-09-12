#include "NetDirectoryCodec.h"

#include "nlohmann/json.hpp"

#include <limits>

namespace RTE {

	namespace {
		using json = nlohmann::json;

		bool Fail(std::string& reason, const char* kind, const char* field) {
			reason = std::string(kind) + ":" + field;
			return false;
		}

		bool ParseBody(const std::string& body, json& out, std::string& reason) {
			if (body.size() > NetDirectoryLimits::c_MaxBodyBytes) {
				reason = "body exceeds the 128 KiB cap";
				return false;
			}
			const json parsed = json::parse(body, nullptr, false);
			if (parsed.is_discarded() || !parsed.is_object()) {
				reason = "malformed_json";
				return false;
			}
			out = parsed;
			return true;
		}

		bool ReadStr(const json& obj, const char* key, std::string& out, std::string& reason, size_t maxChars = NetDirectoryLimits::c_MaxStringChars) {
			const auto it = obj.find(key);
			if (it == obj.end()) return Fail(reason, "missing_field", key);
			if (!it->is_string() || it->get_ref<const std::string&>().size() > maxChars) return Fail(reason, "invalid_field", key);
			out = it->get_ref<const std::string&>();
			return true;
		}

		bool ReadInt(const json& obj, const char* key, int64_t minValue, int64_t maxValue, int64_t& out, std::string& reason) {
			const auto it = obj.find(key);
			if (it == obj.end()) return Fail(reason, "missing_field", key);
			if (!it->is_number_integer() && !it->is_number_unsigned()) return Fail(reason, "invalid_field", key);
			int64_t value = 0;
			if (it->is_number_unsigned()) {
				const uint64_t raw = it->get<uint64_t>();
				if (raw > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return Fail(reason, "invalid_field", key);
				value = static_cast<int64_t>(raw);
			} else {
				value = it->get<int64_t>();
			}
			if (value < minValue || value > maxValue) return Fail(reason, "invalid_field", key);
			out = value;
			return true;
		}

		bool ReadBool(const json& obj, const char* key, bool& out, std::string& reason) {
			const auto it = obj.find(key);
			if (it == obj.end()) return Fail(reason, "missing_field", key);
			if (!it->is_boolean()) return Fail(reason, "invalid_field", key);
			out = it->get<bool>();
			return true;
		}

		bool ReadListenAddrs(const json& obj, std::vector<std::string>& out, std::string& reason) {
			const char* key = "listen_addrs";
			const auto it = obj.find(key);
			if (it == obj.end()) return Fail(reason, "missing_field", key);
			if (!it->is_array() || it->size() > NetDirectoryLimits::c_MaxListenAddrs) return Fail(reason, "invalid_field", key);
			std::vector<std::string> addrs;
			for (const json& item : *it) {
				if (!item.is_string() || item.get_ref<const std::string&>().size() > NetDirectoryLimits::c_MaxStringChars) return Fail(reason, "invalid_field", key);
				addrs.push_back(item.get_ref<const std::string&>());
			}
			out = std::move(addrs);
			return true;
		}

		bool ReadOptionalStr(const json& obj, const char* key, std::optional<std::string>& out, std::string& reason) {
			const auto it = obj.find(key);
			if (it == obj.end()) {
				out.reset();
				return true;
			}
			std::string value;
			if (!ReadStr(obj, key, value, reason)) return false;
			out = std::move(value);
			return true;
		}

		bool ReadOptionalListenAddrs(const json& obj, std::optional<std::vector<std::string>>& out, std::string& reason) {
			const auto it = obj.find("listen_addrs");
			if (it == obj.end()) {
				out.reset();
				return true;
			}
			std::vector<std::string> addrs;
			if (!ReadListenAddrs(obj, addrs, reason)) return false;
			out = std::move(addrs);
			return true;
		}

		bool IsJoinMode(const std::string& value) {
			return value == "ip" || value == "ice" || value == "either";
		}

		bool IsSessionState(const std::string& value) {
			return value == "lobby" || value == "running";
		}

		bool IsInstallKeyChar(char ch) {
			const unsigned char c = static_cast<unsigned char>(ch);
			return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || ch == '-' || ch == '_';
		}

		// Peers are "host" or "client:<1..64 install-key chars>"; total can reach 71 chars.
		constexpr size_t c_MaxPeerChars = 7 + NetDirectoryLimits::c_MaxStringChars;

		bool ReadPeer(const json& obj, const char* key, std::string& out, std::string& reason) {
			std::string value;
			if (!ReadStr(obj, key, value, reason, c_MaxPeerChars)) return false;
			const bool valid = value == "host" ||
				(value.size() > 7 && value.compare(0, 7, "client:") == 0 &&
				 std::all_of(value.begin() + 7, value.end(), IsInstallKeyChar));
			if (!valid) return Fail(reason, "invalid_field", key);
			out = std::move(value);
			return true;
		}

		bool IsBase64(const std::string& value) {
			auto isChar = [](char ch) {
				const unsigned char c = static_cast<unsigned char>(ch);
				return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || ch == '+' || ch == '/' || ch == '=';
			};
			return std::all_of(value.begin(), value.end(), isChar);
		}

		bool ReadPayloadB64(const json& obj, const char* key, std::string& out, std::string& reason) {
			std::string value;
			if (!ReadStr(obj, key, value, reason, NetDirectoryLimits::c_MaxPayloadB64Chars)) return false;
			if (!IsBase64(value)) return Fail(reason, "invalid_field", key);
			out = std::move(value);
			return true;
		}

		bool ReadVersionField(const json& obj, const char* key, int64_t& out, std::string& reason) {
			return ReadInt(obj, key, 0, NetDirectoryLimits::c_MaxIntField, out, reason);
		}

		// The register body and each list row carry the same named fields.
		bool ReadRegisterFields(const json& obj, NetDirectoryRegisterRequest& out, std::string& reason) {
			return ReadStr(obj, "name", out.name, reason) &&
			       ReadStr(obj, "activity", out.activity, reason) &&
			       ReadStr(obj, "scene", out.scene, reason) &&
			       ReadStr(obj, "mode", out.mode, reason) &&
			       ReadVersionField(obj, "peer_count", out.peerCount, reason) &&
			       ReadVersionField(obj, "seats_free", out.seatsFree, reason) &&
			       ReadStr(obj, "game_version", out.gameVersion, reason) &&
			       ReadStr(obj, "build_id", out.buildId, reason) &&
			       ReadVersionField(obj, "network_protocol_version", out.networkProtocolVersion, reason) &&
			       ReadVersionField(obj, "lockstep_codec_version", out.lockstepCodecVersion, reason) &&
			       ReadVersionField(obj, "controller_frame_version", out.controllerFrameVersion, reason) &&
			       ReadStr(obj, "match_config_hash", out.matchConfigHash, reason) &&
			       ReadStr(obj, "session_identity_hash", out.sessionIdentityHash, reason) &&
			       ReadStr(obj, "module_manifest_hash", out.moduleManifestHash, reason) &&
			       ReadInt(obj, "listen_port", NetDirectoryLimits::c_MinListenPort, NetDirectoryLimits::c_MaxListenPort, out.listenPort, reason) &&
			       ReadListenAddrs(obj, out.listenAddrs, reason) &&
			       ReadStr(obj, "join_mode", out.joinMode, reason) &&
			       (IsJoinMode(out.joinMode) || Fail(reason, "invalid_field", "join_mode"));
		}

		void WriteRegisterFields(json& obj, const NetDirectoryRegisterRequest& in) {
			obj["name"] = in.name;
			obj["activity"] = in.activity;
			obj["scene"] = in.scene;
			obj["mode"] = in.mode;
			obj["peer_count"] = in.peerCount;
			obj["seats_free"] = in.seatsFree;
			obj["game_version"] = in.gameVersion;
			obj["build_id"] = in.buildId;
			obj["network_protocol_version"] = in.networkProtocolVersion;
			obj["lockstep_codec_version"] = in.lockstepCodecVersion;
			obj["controller_frame_version"] = in.controllerFrameVersion;
			obj["match_config_hash"] = in.matchConfigHash;
			obj["session_identity_hash"] = in.sessionIdentityHash;
			obj["module_manifest_hash"] = in.moduleManifestHash;
			obj["listen_port"] = in.listenPort;
			obj["listen_addrs"] = in.listenAddrs;
			obj["join_mode"] = in.joinMode;
		}

		bool ReadRowFields(const json& obj, NetDirectorySessionRow& out, std::string& reason) {
			NetDirectoryRegisterRequest fields;
			if (!ReadRegisterFields(obj, fields, reason)) return false;
			out.name = std::move(fields.name);
			out.activity = std::move(fields.activity);
			out.scene = std::move(fields.scene);
			out.mode = std::move(fields.mode);
			out.peerCount = fields.peerCount;
			out.seatsFree = fields.seatsFree;
			out.gameVersion = std::move(fields.gameVersion);
			out.buildId = std::move(fields.buildId);
			out.networkProtocolVersion = fields.networkProtocolVersion;
			out.lockstepCodecVersion = fields.lockstepCodecVersion;
			out.controllerFrameVersion = fields.controllerFrameVersion;
			out.matchConfigHash = std::move(fields.matchConfigHash);
			out.sessionIdentityHash = std::move(fields.sessionIdentityHash);
			out.moduleManifestHash = std::move(fields.moduleManifestHash);
			out.listenPort = fields.listenPort;
			out.listenAddrs = std::move(fields.listenAddrs);
			out.joinMode = std::move(fields.joinMode);
			return ReadStr(obj, "session_id", out.sessionId, reason) &&
			       ReadInt(obj, "age_s", 0, std::numeric_limits<int64_t>::max(), out.ageS, reason) &&
			       ReadStr(obj, "observed_ip", out.observedIp, reason) &&
			       ReadStr(obj, "state", out.state, reason) &&
			       (IsSessionState(out.state) || Fail(reason, "invalid_field", "state"));
		}

		NetDirectoryRegisterRequest RowAsRegister(const NetDirectorySessionRow& row) {
			NetDirectoryRegisterRequest fields;
			fields.name = row.name;
			fields.activity = row.activity;
			fields.scene = row.scene;
			fields.mode = row.mode;
			fields.peerCount = row.peerCount;
			fields.seatsFree = row.seatsFree;
			fields.gameVersion = row.gameVersion;
			fields.buildId = row.buildId;
			fields.networkProtocolVersion = row.networkProtocolVersion;
			fields.lockstepCodecVersion = row.lockstepCodecVersion;
			fields.controllerFrameVersion = row.controllerFrameVersion;
			fields.matchConfigHash = row.matchConfigHash;
			fields.sessionIdentityHash = row.sessionIdentityHash;
			fields.moduleManifestHash = row.moduleManifestHash;
			fields.listenPort = row.listenPort;
			fields.listenAddrs = row.listenAddrs;
			fields.joinMode = row.joinMode;
			return fields;
		}

		json SessionRowToJson(const NetDirectorySessionRow& row) {
			json obj;
			WriteRegisterFields(obj, RowAsRegister(row));
			obj["session_id"] = row.sessionId;
			obj["age_s"] = row.ageS;
			obj["observed_ip"] = row.observedIp;
			obj["state"] = row.state;
			return obj;
		}
	}

	std::string NetDirectoryCodec::EncodeRegisterRequest(const NetDirectoryRegisterRequest& request) {
		json obj;
		WriteRegisterFields(obj, request);
		return obj.dump();
	}

	bool NetDirectoryCodec::DecodeRegisterRequest(const std::string& body, NetDirectoryRegisterRequest& out, std::string& reason) {
		json obj;
		if (!ParseBody(body, obj, reason)) return false;
		return ReadRegisterFields(obj, out, reason);
	}

	std::string NetDirectoryCodec::EncodeRegisterResponse(const NetDirectoryRegisterResponse& response) {
		return json{
			{"session_id", response.sessionId},
			{"token", response.token},
			{"expires_in_s", response.expiresInS},
			{"heartbeat_s", response.heartbeatS},
			{"observed_ip", response.observedIp},
		}.dump();
	}

	bool NetDirectoryCodec::DecodeRegisterResponse(const std::string& body, NetDirectoryRegisterResponse& out, std::string& reason) {
		json obj;
		if (!ParseBody(body, obj, reason)) return false;
		return ReadStr(obj, "session_id", out.sessionId, reason) &&
		       ReadStr(obj, "token", out.token, reason) &&
		       ReadInt(obj, "expires_in_s", 0, NetDirectoryLimits::c_MaxIntField, out.expiresInS, reason) &&
		       ReadInt(obj, "heartbeat_s", 0, NetDirectoryLimits::c_MaxIntField, out.heartbeatS, reason) &&
		       ReadStr(obj, "observed_ip", out.observedIp, reason);
	}

	std::string NetDirectoryCodec::EncodeHeartbeatRequest(const NetDirectoryHeartbeatRequest& request) {
		json obj;
		obj["token"] = request.token;
		obj["peer_count"] = request.peerCount;
		obj["seats_free"] = request.seatsFree;
		if (request.listenAddrs.has_value()) obj["listen_addrs"] = *request.listenAddrs;
		if (request.state.has_value()) obj["state"] = *request.state;
		return obj.dump();
	}

	bool NetDirectoryCodec::DecodeHeartbeatRequest(const std::string& body, NetDirectoryHeartbeatRequest& out, std::string& reason) {
		json obj;
		if (!ParseBody(body, obj, reason)) return false;
		if (!ReadStr(obj, "token", out.token, reason) ||
		    !ReadVersionField(obj, "peer_count", out.peerCount, reason) ||
		    !ReadVersionField(obj, "seats_free", out.seatsFree, reason) ||
		    !ReadOptionalListenAddrs(obj, out.listenAddrs, reason) ||
		    !ReadOptionalStr(obj, "state", out.state, reason)) {
			return false;
		}
		if (out.state.has_value() && !IsSessionState(*out.state)) return Fail(reason, "invalid_field", "state");
		return true;
	}

	std::string NetDirectoryCodec::EncodeHeartbeatResponse(const NetDirectoryHeartbeatResponse& response) {
		return json{{"expires_in_s", response.expiresInS}, {"heartbeat_s", response.heartbeatS}}.dump();
	}

	bool NetDirectoryCodec::DecodeHeartbeatResponse(const std::string& body, NetDirectoryHeartbeatResponse& out, std::string& reason) {
		json obj;
		if (!ParseBody(body, obj, reason)) return false;
		return ReadInt(obj, "expires_in_s", 0, NetDirectoryLimits::c_MaxIntField, out.expiresInS, reason) &&
		       ReadInt(obj, "heartbeat_s", 0, NetDirectoryLimits::c_MaxIntField, out.heartbeatS, reason);
	}

	std::string NetDirectoryCodec::EncodeDeleteRequest(const NetDirectoryDeleteRequest& request) {
		return json{{"token", request.token}}.dump();
	}

	bool NetDirectoryCodec::DecodeDeleteRequest(const std::string& body, NetDirectoryDeleteRequest& out, std::string& reason) {
		json obj;
		if (!ParseBody(body, obj, reason)) return false;
		return ReadStr(obj, "token", out.token, reason);
	}

	std::string NetDirectoryCodec::EncodeDeleteResponse(const NetDirectoryDeleteResponse& response) {
		return json{{"ok", response.ok}}.dump();
	}

	bool NetDirectoryCodec::DecodeDeleteResponse(const std::string& body, NetDirectoryDeleteResponse& out, std::string& reason) {
		json obj;
		if (!ParseBody(body, obj, reason)) return false;
		return ReadBool(obj, "ok", out.ok, reason);
	}

	std::string NetDirectoryCodec::EncodeSessionRow(const NetDirectorySessionRow& row) {
		return SessionRowToJson(row).dump();
	}

	bool NetDirectoryCodec::DecodeSessionRow(const std::string& body, NetDirectorySessionRow& out, std::string& reason) {
		json obj;
		if (!ParseBody(body, obj, reason)) return false;
		return ReadRowFields(obj, out, reason);
	}

	std::string NetDirectoryCodec::EncodeListResponse(const NetDirectoryListResponse& response) {
		json rows = json::array();
		for (const NetDirectorySessionRow& row : response.sessions) {
			rows.push_back(SessionRowToJson(row));
		}
		return json{{"sessions", rows}}.dump();
	}

	bool NetDirectoryCodec::DecodeListResponse(const std::string& body, NetDirectoryListResponse& out, std::string& reason) {
		json obj;
		if (!ParseBody(body, obj, reason)) return false;
		const auto it = obj.find("sessions");
		if (it == obj.end()) return Fail(reason, "missing_field", "sessions");
		if (!it->is_array()) return Fail(reason, "invalid_field", "sessions");
		std::vector<NetDirectorySessionRow> rows;
		for (const json& item : *it) {
			if (!item.is_object()) return Fail(reason, "invalid_field", "sessions");
			NetDirectorySessionRow row;
			if (!ReadRowFields(item, row, reason)) return false;
			rows.push_back(std::move(row));
		}
		out.sessions = std::move(rows);
		return true;
	}

	std::string NetDirectoryCodec::EncodeSignalPost(const NetDirectorySignalPost& post) {
		return json{
			{"token_or_join_nonce", post.tokenOrJoinNonce},
			{"from", post.from},
			{"to", post.to},
			{"payload_b64", post.payloadB64},
		}.dump();
	}

	bool NetDirectoryCodec::DecodeSignalPost(const std::string& body, NetDirectorySignalPost& out, std::string& reason) {
		json obj;
		if (!ParseBody(body, obj, reason)) return false;
		return ReadStr(obj, "token_or_join_nonce", out.tokenOrJoinNonce, reason) &&
		       ReadPeer(obj, "from", out.from, reason) &&
		       ReadPeer(obj, "to", out.to, reason) &&
		       ReadPayloadB64(obj, "payload_b64", out.payloadB64, reason);
	}

	std::string NetDirectoryCodec::EncodeSignalPostResponse(const NetDirectorySignalPostResponse& response) {
		return json{{"ok", response.ok}, {"seq", response.seq}}.dump();
	}

	bool NetDirectoryCodec::DecodeSignalPostResponse(const std::string& body, NetDirectorySignalPostResponse& out, std::string& reason) {
		json obj;
		if (!ParseBody(body, obj, reason)) return false;
		return ReadBool(obj, "ok", out.ok, reason) &&
		       ReadInt(obj, "seq", 0, std::numeric_limits<int64_t>::max(), out.seq, reason);
	}

	std::string NetDirectoryCodec::EncodeSignalList(const NetDirectorySignalList& list) {
		json items = json::array();
		for (const NetDirectorySignal& signal : list.signals) {
			items.push_back(json{
				{"seq", signal.seq},
				{"from", signal.from},
				{"to", signal.to},
				{"payload_b64", signal.payloadB64},
			});
		}
		return json{{"signals", items}}.dump();
	}

	bool NetDirectoryCodec::DecodeSignalList(const std::string& body, NetDirectorySignalList& out, std::string& reason) {
		json obj;
		if (!ParseBody(body, obj, reason)) return false;
		const auto it = obj.find("signals");
		if (it == obj.end()) return Fail(reason, "missing_field", "signals");
		if (!it->is_array()) return Fail(reason, "invalid_field", "signals");
		std::vector<NetDirectorySignal> signals;
		for (const json& item : *it) {
			if (!item.is_object()) return Fail(reason, "invalid_field", "signals");
			NetDirectorySignal signal;
			if (!ReadInt(item, "seq", 0, std::numeric_limits<int64_t>::max(), signal.seq, reason) ||
			    !ReadPeer(item, "from", signal.from, reason) ||
			    !ReadPeer(item, "to", signal.to, reason) ||
			    !ReadPayloadB64(item, "payload_b64", signal.payloadB64, reason)) {
				return false;
			}
			signals.push_back(std::move(signal));
		}
		out.signals = std::move(signals);
		return true;
	}

	bool NetDirectoryCodec::IsJoinable(const NetDirectorySessionRow& row, const NetDirectoryLocalIdentity& local, std::string* reason) {
		auto mismatch = [&](const char* field) {
			if (reason) *reason = std::string("incompatible: ") + field;
			return false;
		};
		if (row.networkProtocolVersion != local.networkProtocolVersion) return mismatch("network_protocol_version");
		if (row.lockstepCodecVersion != local.lockstepCodecVersion) return mismatch("lockstep_codec_version");
		if (row.controllerFrameVersion != local.controllerFrameVersion) return mismatch("controller_frame_version");
		// The session identity hash contains the module manifest hash, so it names only a difference the specific fields did not.
		if (row.moduleManifestHash != local.moduleManifestHash) return mismatch("module_manifest_hash");
		if (row.sessionIdentityHash != local.sessionIdentityHash) return mismatch("session_identity_hash");
		if (reason) reason->clear();
		return true;
	}

} // namespace RTE
