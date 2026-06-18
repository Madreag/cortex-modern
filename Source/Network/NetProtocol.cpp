#include "NetProtocol.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace RTE {

	namespace {
		template <class... T>
		struct Overloaded : T... {
			using T::operator()...;
		};

		template <class... T>
		Overloaded(T...) -> Overloaded<T...>;

		void SetError(NetProtocolError* error, NetProtocolErrorCode code, size_t offset, std::string message) {
			if (error) {
				error->code = code;
				error->offset = offset;
				error->message = std::move(message);
			}
		}

		NetDecodeResult Fail(NetProtocolErrorCode code, size_t offset, std::string message) {
			NetDecodeResult result;
			result.error.code = code;
			result.error.offset = offset;
			result.error.message = std::move(message);
			return result;
		}

		void AppendU8(std::vector<uint8_t>& out, uint8_t value) {
			out.push_back(value);
		}

		void AppendBool(std::vector<uint8_t>& out, bool value) {
			AppendU8(out, value ? 1U : 0U);
		}

		void AppendU16LE(std::vector<uint8_t>& out, uint16_t value) {
			out.push_back(static_cast<uint8_t>(value & 0xFFU));
			out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
		}

		void AppendU32LE(std::vector<uint8_t>& out, uint32_t value) {
			for (int i = 0; i < 4; ++i) {
				out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFFU));
			}
		}

		void AppendU64LE(std::vector<uint8_t>& out, uint64_t value) {
			for (int i = 0; i < 8; ++i) {
				out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFFU));
			}
		}

		bool HasControlChars(const std::string& value) {
			return std::any_of(value.begin(), value.end(), [](unsigned char c) {
				return c < 0x20U || c == 0x7FU;
			});
		}

		bool AppendString(std::vector<uint8_t>& out, const std::string& value, size_t maxBytes, const char* fieldName, NetProtocolError* error) {
			if (value.size() > maxBytes || value.size() > std::numeric_limits<uint16_t>::max()) {
				SetError(error, NetProtocolErrorCode::StringTooLong, out.size(), std::string(fieldName) + " exceeds max encoded length");
				return false;
			}
			if (HasControlChars(value)) {
				SetError(error, NetProtocolErrorCode::InvalidString, out.size(), std::string(fieldName) + " contains control characters");
				return false;
			}
			AppendU16LE(out, static_cast<uint16_t>(value.size()));
			out.insert(out.end(), value.begin(), value.end());
			return true;
		}

		void AppendHash(std::vector<uint8_t>& out, const NetHash32& hash) {
			out.insert(out.end(), hash.begin(), hash.end());
		}

		class ByteReader {
		public:
			ByteReader(const uint8_t* data, size_t size) : m_Data(data), m_Size(size) {}

			size_t Offset() const { return m_Offset; }
			bool AtEnd() const { return m_Offset == m_Size; }

			bool ReadU8(uint8_t& out) {
				if (!CanRead(1)) {
					return false;
				}
				out = m_Data[m_Offset++];
				return true;
			}

			bool ReadU16LE(uint16_t& out) {
				if (!CanRead(2)) {
					return false;
				}
				out = static_cast<uint16_t>(m_Data[m_Offset]) |
				      static_cast<uint16_t>(static_cast<uint16_t>(m_Data[m_Offset + 1]) << 8);
				m_Offset += 2;
				return true;
			}

			bool ReadU32LE(uint32_t& out) {
				if (!CanRead(4)) {
					return false;
				}
				out = 0;
				for (int i = 0; i < 4; ++i) {
					out |= static_cast<uint32_t>(m_Data[m_Offset + i]) << (i * 8);
				}
				m_Offset += 4;
				return true;
			}

			bool ReadU64LE(uint64_t& out) {
				if (!CanRead(8)) {
					return false;
				}
				out = 0;
				for (int i = 0; i < 8; ++i) {
					out |= static_cast<uint64_t>(m_Data[m_Offset + i]) << (i * 8);
				}
				m_Offset += 8;
				return true;
			}

			bool ReadString(std::string& out, size_t maxBytes, const char* fieldName, NetProtocolError* error) {
				uint16_t length = 0;
				const size_t lengthOffset = m_Offset;
				if (!ReadU16LE(length)) {
					SetError(error, NetProtocolErrorCode::TruncatedPayload, lengthOffset, std::string(fieldName) + " length is truncated");
					return false;
				}
				if (length > maxBytes) {
					SetError(error, NetProtocolErrorCode::StringTooLong, lengthOffset, std::string(fieldName) + " exceeds max encoded length");
					return false;
				}
				if (!CanRead(length)) {
					SetError(error, NetProtocolErrorCode::TruncatedPayload, m_Offset, std::string(fieldName) + " data is truncated");
					return false;
				}
				out.assign(reinterpret_cast<const char*>(m_Data + m_Offset), length);
				m_Offset += length;
				if (HasControlChars(out)) {
					SetError(error, NetProtocolErrorCode::InvalidString, lengthOffset, std::string(fieldName) + " contains control characters");
					return false;
				}
				return true;
			}

			bool ReadHash(NetHash32& out) {
				if (!CanRead(out.size())) {
					return false;
				}
				std::memcpy(out.data(), m_Data + m_Offset, out.size());
				m_Offset += out.size();
				return true;
			}

		private:
			bool CanRead(size_t bytes) const {
				return bytes <= m_Size && m_Offset <= m_Size - bytes;
			}

			const uint8_t* m_Data = nullptr;
			size_t m_Size = 0;
			size_t m_Offset = 0;
		};

		bool ReadOrTruncated(bool ok, const ByteReader& reader, NetProtocolError* error, const char* fieldName) {
			if (!ok) {
				SetError(error, NetProtocolErrorCode::TruncatedPayload, reader.Offset(), std::string(fieldName) + " is truncated or invalid");
			}
			return ok;
		}

		bool ReadRejectReason(ByteReader& reader, NetRejectReason& out, NetProtocolError* error) {
			uint16_t rawReason = 0;
			if (!ReadOrTruncated(reader.ReadU16LE(rawReason), reader, error, "reject reason")) {
				return false;
			}
			switch (static_cast<NetRejectReason>(rawReason)) {
				case NetRejectReason::ProtocolMismatch:
				case NetRejectReason::GameVersionMismatch:
				case NetRejectReason::BuildMismatch:
				case NetRejectReason::ControllerFrameVersionMismatch:
				case NetRejectReason::ControllerFrameSizeMismatch:
				case NetRejectReason::DeterministicConfigMismatch:
				case NetRejectReason::ModuleManifestMismatch:
				case NetRejectReason::SessionRulesMismatch:
				case NetRejectReason::UserdataModulesNotAllowed:
				case NetRejectReason::SessionFull:
				case NetRejectReason::HostNotAccepting:
				case NetRejectReason::DuplicateClientNonce:
				case NetRejectReason::MalformedMessage:
				case NetRejectReason::Timeout:
				case NetRejectReason::InternalError:
					out = static_cast<NetRejectReason>(rawReason);
					return true;
			}
			SetError(error, NetProtocolErrorCode::InvalidValue, reader.Offset() - 2, "reject reason has invalid enum value");
			return false;
		}

		bool EncodePayload(const NetClientHello& payload, std::vector<uint8_t>& out, NetProtocolError* error) {
			AppendU64LE(out, payload.clientNonce);
			AppendU16LE(out, payload.minProtocolVersion);
			AppendU16LE(out, payload.maxProtocolVersion);
			AppendU16LE(out, payload.controllerFrameVersion);
			AppendU16LE(out, payload.controllerFrameEncodedSize);
			AppendU8(out, payload.platformId);
			if (!AppendString(out, payload.displayName, NetProtocol::c_MaxDisplayNameBytes, "display_name", error) ||
			    !AppendString(out, payload.gameVersion, NetProtocol::c_MaxShortTextBytes, "game_version", error) ||
			    !AppendString(out, payload.buildId, NetProtocol::c_MaxShortTextBytes, "build_id", error)) {
				return false;
			}
			AppendHash(out, payload.deterministicConfigHash);
			AppendHash(out, payload.moduleManifestHash);
			AppendHash(out, payload.sessionRulesHash);
			AppendHash(out, payload.sessionIdentityHash);
			AppendBool(out, payload.hasUserdataModules);
			for (int i = 0; i < 7; ++i) {
				AppendU8(out, 0);
			}
			return true;
		}

		bool EncodePayload(const NetHostHello& payload, std::vector<uint8_t>& out, NetProtocolError* error) {
			AppendU64LE(out, payload.sessionId);
			AppendU64LE(out, payload.hostNonce);
			AppendU16LE(out, payload.selectedProtocolVersion);
			AppendU16LE(out, payload.controllerFrameVersion);
			AppendU16LE(out, payload.controllerFrameEncodedSize);
			AppendU8(out, payload.assignedPeerId);
			AppendU8(out, payload.maxPeers);
			AppendU8(out, payload.hostPlatformId);
			AppendU8(out, 0);
			if (!AppendString(out, payload.gameVersion, NetProtocol::c_MaxShortTextBytes, "game_version", error) ||
			    !AppendString(out, payload.hostName, NetProtocol::c_MaxDisplayNameBytes, "host_name", error) ||
			    !AppendString(out, payload.buildId, NetProtocol::c_MaxShortTextBytes, "build_id", error)) {
				return false;
			}
			AppendHash(out, payload.deterministicConfigHash);
			AppendHash(out, payload.moduleManifestHash);
			AppendHash(out, payload.sessionRulesHash);
			AppendHash(out, payload.sessionIdentityHash);
			AppendBool(out, payload.hasUserdataModules);
			for (int i = 0; i < 7; ++i) {
				AppendU8(out, 0);
			}
			return true;
		}

		bool EncodePayload(const NetJoinAccepted& payload, std::vector<uint8_t>& out, NetProtocolError*) {
			AppendU64LE(out, payload.sessionId);
			AppendU8(out, payload.assignedPeerId);
			AppendU8(out, payload.maxPeers);
			AppendU16LE(out, payload.selectedProtocolVersion);
			AppendU32LE(out, payload.heartbeatIntervalMs);
			AppendU32LE(out, payload.timeoutMs);
			return true;
		}

		bool EncodePayload(const NetJoinRejected& payload, std::vector<uint8_t>& out, NetProtocolError* error) {
			AppendU16LE(out, static_cast<uint16_t>(payload.rejectReason));
			return AppendString(out, payload.humanMessage, NetProtocol::c_MaxDiagnosticTextBytes, "human_message", error) &&
			       AppendString(out, payload.mismatchKey, NetProtocol::c_MaxShortTextBytes, "mismatch_key", error) &&
			       AppendString(out, payload.expected, NetProtocol::c_MaxDiagnosticTextBytes, "expected", error) &&
			       AppendString(out, payload.actual, NetProtocol::c_MaxDiagnosticTextBytes, "actual", error);
		}

		bool EncodePayload(const NetReadyState& payload, std::vector<uint8_t>& out, NetProtocolError*) {
			AppendU8(out, payload.peerId);
			AppendBool(out, payload.ready);
			AppendU16LE(out, 0);
			AppendHash(out, payload.deterministicConfigHash);
			AppendHash(out, payload.moduleManifestHash);
			return true;
		}

		bool EncodePayload(const NetHeartbeat& payload, std::vector<uint8_t>& out, NetProtocolError*) {
			AppendU64LE(out, payload.senderTimeMs);
			AppendU32LE(out, payload.lastReceivedSequence);
			AppendU32LE(out, payload.sessionState);
			return true;
		}

		bool EncodePayload(const NetPing& payload, std::vector<uint8_t>& out, NetProtocolError*) {
			AppendU64LE(out, payload.pingId);
			AppendU64LE(out, payload.senderTimeMs);
			return true;
		}

		bool EncodePayload(const NetPong& payload, std::vector<uint8_t>& out, NetProtocolError*) {
			AppendU64LE(out, payload.pingId);
			AppendU64LE(out, payload.senderTimeMs);
			return true;
		}

		bool EncodePayload(const NetDisconnect& payload, std::vector<uint8_t>& out, NetProtocolError* error) {
			AppendU16LE(out, payload.disconnectReason);
			return AppendString(out, payload.message, NetProtocol::c_MaxDiagnosticTextBytes, "disconnect_message", error);
		}

		bool EncodePayload(const NetSessionSummary& payload, std::vector<uint8_t>& out, NetProtocolError*) {
			AppendU64LE(out, payload.sessionId);
			AppendU8(out, payload.peerCount);
			AppendU8(out, payload.localPeerId);
			AppendU16LE(out, payload.sessionState);
			AppendHash(out, payload.deterministicConfigHash);
			AppendHash(out, payload.moduleManifestHash);
			return true;
		}

		bool DecodePayload(ByteReader& reader, NetClientHello& payload, NetProtocolError* error) {
			if (!ReadOrTruncated(reader.ReadU64LE(payload.clientNonce), reader, error, "client_nonce") ||
			    !ReadOrTruncated(reader.ReadU16LE(payload.minProtocolVersion), reader, error, "min_protocol_version") ||
			    !ReadOrTruncated(reader.ReadU16LE(payload.maxProtocolVersion), reader, error, "max_protocol_version") ||
			    !ReadOrTruncated(reader.ReadU16LE(payload.controllerFrameVersion), reader, error, "controller_frame_version") ||
			    !ReadOrTruncated(reader.ReadU16LE(payload.controllerFrameEncodedSize), reader, error, "controller_frame_encoded_size") ||
			    !ReadOrTruncated(reader.ReadU8(payload.platformId), reader, error, "platform_id")) {
				return false;
			}
			if (!reader.ReadString(payload.displayName, NetProtocol::c_MaxDisplayNameBytes, "display_name", error) ||
			    !reader.ReadString(payload.gameVersion, NetProtocol::c_MaxShortTextBytes, "game_version", error) ||
			    !reader.ReadString(payload.buildId, NetProtocol::c_MaxShortTextBytes, "build_id", error)) {
				return false;
			}
			if (!ReadOrTruncated(reader.ReadHash(payload.deterministicConfigHash), reader, error, "deterministic_config_hash") ||
			    !ReadOrTruncated(reader.ReadHash(payload.moduleManifestHash), reader, error, "module_manifest_hash") ||
			    !ReadOrTruncated(reader.ReadHash(payload.sessionRulesHash), reader, error, "session_rules_hash") ||
			    !ReadOrTruncated(reader.ReadHash(payload.sessionIdentityHash), reader, error, "session_identity_hash")) {
				return false;
			}
			uint8_t hasUserdataModules = 0;
			if (!ReadOrTruncated(reader.ReadU8(hasUserdataModules), reader, error, "has_userdata_modules")) {
				return false;
			}
			if (hasUserdataModules > 1U) {
				SetError(error, NetProtocolErrorCode::InvalidValue, reader.Offset() - 1, "has_userdata_modules must be 0 or 1");
				return false;
			}
			payload.hasUserdataModules = hasUserdataModules != 0U;
			for (int i = 0; i < 7; ++i) {
				uint8_t reserved = 0;
				if (!ReadOrTruncated(reader.ReadU8(reserved), reader, error, "client reserved") || reserved != 0U) {
					SetError(error, NetProtocolErrorCode::ReservedFieldNonZero, reader.Offset() - 1, "client reserved field is nonzero");
					return false;
				}
			}
			if (payload.minProtocolVersion > payload.maxProtocolVersion) {
				SetError(error, NetProtocolErrorCode::InvalidValue, reader.Offset(), "client protocol range is invalid");
				return false;
			}
			return true;
		}

		bool DecodePayload(ByteReader& reader, NetHostHello& payload, NetProtocolError* error) {
			if (!ReadOrTruncated(reader.ReadU64LE(payload.sessionId), reader, error, "session_id") ||
			    !ReadOrTruncated(reader.ReadU64LE(payload.hostNonce), reader, error, "host_nonce") ||
			    !ReadOrTruncated(reader.ReadU16LE(payload.selectedProtocolVersion), reader, error, "selected_protocol_version") ||
			    !ReadOrTruncated(reader.ReadU16LE(payload.controllerFrameVersion), reader, error, "controller_frame_version") ||
			    !ReadOrTruncated(reader.ReadU16LE(payload.controllerFrameEncodedSize), reader, error, "controller_frame_encoded_size") ||
			    !ReadOrTruncated(reader.ReadU8(payload.assignedPeerId), reader, error, "assigned_peer_id") ||
			    !ReadOrTruncated(reader.ReadU8(payload.maxPeers), reader, error, "max_peers") ||
			    !ReadOrTruncated(reader.ReadU8(payload.hostPlatformId), reader, error, "host_platform_id")) {
				return false;
			}
			uint8_t reserved = 0;
			if (!ReadOrTruncated(reader.ReadU8(reserved), reader, error, "host reserved") || reserved != 0U) {
				SetError(error, NetProtocolErrorCode::ReservedFieldNonZero, reader.Offset() - 1, "host reserved field is nonzero");
				return false;
			}
			if (!reader.ReadString(payload.gameVersion, NetProtocol::c_MaxShortTextBytes, "game_version", error) ||
			    !reader.ReadString(payload.hostName, NetProtocol::c_MaxDisplayNameBytes, "host_name", error) ||
			    !reader.ReadString(payload.buildId, NetProtocol::c_MaxShortTextBytes, "build_id", error)) {
				return false;
			}
			if (!ReadOrTruncated(reader.ReadHash(payload.deterministicConfigHash), reader, error, "deterministic_config_hash") ||
			    !ReadOrTruncated(reader.ReadHash(payload.moduleManifestHash), reader, error, "module_manifest_hash") ||
			    !ReadOrTruncated(reader.ReadHash(payload.sessionRulesHash), reader, error, "session_rules_hash") ||
			    !ReadOrTruncated(reader.ReadHash(payload.sessionIdentityHash), reader, error, "session_identity_hash")) {
				return false;
			}
			uint8_t hasUserdataModules = 0;
			if (!ReadOrTruncated(reader.ReadU8(hasUserdataModules), reader, error, "has_userdata_modules")) {
				return false;
			}
			if (hasUserdataModules > 1U) {
				SetError(error, NetProtocolErrorCode::InvalidValue, reader.Offset() - 1, "has_userdata_modules must be 0 or 1");
				return false;
			}
			payload.hasUserdataModules = hasUserdataModules != 0U;
			for (int i = 0; i < 7; ++i) {
				uint8_t reservedHostIdentity = 0;
				if (!ReadOrTruncated(reader.ReadU8(reservedHostIdentity), reader, error, "host identity reserved") || reservedHostIdentity != 0U) {
					SetError(error, NetProtocolErrorCode::ReservedFieldNonZero, reader.Offset() - 1, "host identity reserved field is nonzero");
					return false;
				}
			}
			return true;
		}

		bool DecodePayload(ByteReader& reader, NetJoinAccepted& payload, NetProtocolError* error) {
			return ReadOrTruncated(reader.ReadU64LE(payload.sessionId), reader, error, "session_id") &&
			       ReadOrTruncated(reader.ReadU8(payload.assignedPeerId), reader, error, "assigned_peer_id") &&
			       ReadOrTruncated(reader.ReadU8(payload.maxPeers), reader, error, "max_peers") &&
			       ReadOrTruncated(reader.ReadU16LE(payload.selectedProtocolVersion), reader, error, "selected_protocol_version") &&
			       ReadOrTruncated(reader.ReadU32LE(payload.heartbeatIntervalMs), reader, error, "heartbeat_interval_ms") &&
			       ReadOrTruncated(reader.ReadU32LE(payload.timeoutMs), reader, error, "timeout_ms");
		}

		bool DecodePayload(ByteReader& reader, NetJoinRejected& payload, NetProtocolError* error) {
			return ReadRejectReason(reader, payload.rejectReason, error) &&
			       reader.ReadString(payload.humanMessage, NetProtocol::c_MaxDiagnosticTextBytes, "human_message", error) &&
			       reader.ReadString(payload.mismatchKey, NetProtocol::c_MaxShortTextBytes, "mismatch_key", error) &&
			       reader.ReadString(payload.expected, NetProtocol::c_MaxDiagnosticTextBytes, "expected", error) &&
			       reader.ReadString(payload.actual, NetProtocol::c_MaxDiagnosticTextBytes, "actual", error);
		}

		bool DecodePayload(ByteReader& reader, NetReadyState& payload, NetProtocolError* error) {
			if (!ReadOrTruncated(reader.ReadU8(payload.peerId), reader, error, "peer_id")) {
				return false;
			}
			uint8_t ready = 0;
			if (!ReadOrTruncated(reader.ReadU8(ready), reader, error, "ready")) {
				return false;
			}
			if (ready > 1U) {
				SetError(error, NetProtocolErrorCode::InvalidValue, reader.Offset() - 1, "ready must be 0 or 1");
				return false;
			}
			payload.ready = ready != 0U;
			uint16_t reserved = 0;
			if (!ReadOrTruncated(reader.ReadU16LE(reserved), reader, error, "ready reserved") || reserved != 0U) {
				SetError(error, NetProtocolErrorCode::ReservedFieldNonZero, reader.Offset() - 2, "ready reserved field is nonzero");
				return false;
			}
			return ReadOrTruncated(reader.ReadHash(payload.deterministicConfigHash), reader, error, "deterministic_config_hash") &&
			       ReadOrTruncated(reader.ReadHash(payload.moduleManifestHash), reader, error, "module_manifest_hash");
		}

		bool DecodePayload(ByteReader& reader, NetHeartbeat& payload, NetProtocolError* error) {
			return ReadOrTruncated(reader.ReadU64LE(payload.senderTimeMs), reader, error, "sender_time_ms") &&
			       ReadOrTruncated(reader.ReadU32LE(payload.lastReceivedSequence), reader, error, "last_received_sequence") &&
			       ReadOrTruncated(reader.ReadU32LE(payload.sessionState), reader, error, "session_state");
		}

		bool DecodePayload(ByteReader& reader, NetPing& payload, NetProtocolError* error) {
			return ReadOrTruncated(reader.ReadU64LE(payload.pingId), reader, error, "ping_id") &&
			       ReadOrTruncated(reader.ReadU64LE(payload.senderTimeMs), reader, error, "sender_time_ms");
		}

		bool DecodePayload(ByteReader& reader, NetPong& payload, NetProtocolError* error) {
			return ReadOrTruncated(reader.ReadU64LE(payload.pingId), reader, error, "ping_id") &&
			       ReadOrTruncated(reader.ReadU64LE(payload.senderTimeMs), reader, error, "sender_time_ms");
		}

		bool DecodePayload(ByteReader& reader, NetDisconnect& payload, NetProtocolError* error) {
			return ReadOrTruncated(reader.ReadU16LE(payload.disconnectReason), reader, error, "disconnect_reason") &&
			       reader.ReadString(payload.message, NetProtocol::c_MaxDiagnosticTextBytes, "disconnect_message", error);
		}

		bool DecodePayload(ByteReader& reader, NetSessionSummary& payload, NetProtocolError* error) {
			return ReadOrTruncated(reader.ReadU64LE(payload.sessionId), reader, error, "session_id") &&
			       ReadOrTruncated(reader.ReadU8(payload.peerCount), reader, error, "peer_count") &&
			       ReadOrTruncated(reader.ReadU8(payload.localPeerId), reader, error, "local_peer_id") &&
			       ReadOrTruncated(reader.ReadU16LE(payload.sessionState), reader, error, "session_state") &&
			       ReadOrTruncated(reader.ReadHash(payload.deterministicConfigHash), reader, error, "deterministic_config_hash") &&
			       ReadOrTruncated(reader.ReadHash(payload.moduleManifestHash), reader, error, "module_manifest_hash");
		}
	}

	NetMessageType NetProtocol::MessageTypeOf(const NetPayload& payload) {
		return std::visit(Overloaded{
			[](const NetClientHello&) { return NetMessageType::ClientHello; },
			[](const NetHostHello&) { return NetMessageType::HostHello; },
			[](const NetJoinAccepted&) { return NetMessageType::JoinAccepted; },
			[](const NetJoinRejected&) { return NetMessageType::JoinRejected; },
			[](const NetReadyState&) { return NetMessageType::ReadyState; },
			[](const NetHeartbeat&) { return NetMessageType::Heartbeat; },
			[](const NetPing&) { return NetMessageType::Ping; },
			[](const NetPong&) { return NetMessageType::Pong; },
			[](const NetDisconnect&) { return NetMessageType::Disconnect; },
			[](const NetSessionSummary&) { return NetMessageType::SessionSummary; },
		}, payload);
	}

	const char* NetProtocol::MessageTypeName(NetMessageType type) {
		switch (type) {
			case NetMessageType::ClientHello: return "ClientHello";
			case NetMessageType::HostHello: return "HostHello";
			case NetMessageType::JoinAccepted: return "JoinAccepted";
			case NetMessageType::JoinRejected: return "JoinRejected";
			case NetMessageType::ReadyState: return "ReadyState";
			case NetMessageType::Heartbeat: return "Heartbeat";
			case NetMessageType::Ping: return "Ping";
			case NetMessageType::Pong: return "Pong";
			case NetMessageType::Disconnect: return "Disconnect";
			case NetMessageType::SessionSummary: return "SessionSummary";
		}
		return "Unknown";
	}

	const char* NetProtocol::RejectReasonName(NetRejectReason reason) {
		switch (reason) {
			case NetRejectReason::ProtocolMismatch: return "ProtocolMismatch";
			case NetRejectReason::GameVersionMismatch: return "GameVersionMismatch";
			case NetRejectReason::BuildMismatch: return "BuildMismatch";
			case NetRejectReason::ControllerFrameVersionMismatch: return "ControllerFrameVersionMismatch";
			case NetRejectReason::ControllerFrameSizeMismatch: return "ControllerFrameSizeMismatch";
			case NetRejectReason::DeterministicConfigMismatch: return "DeterministicConfigMismatch";
			case NetRejectReason::ModuleManifestMismatch: return "ModuleManifestMismatch";
			case NetRejectReason::SessionRulesMismatch: return "SessionRulesMismatch";
			case NetRejectReason::UserdataModulesNotAllowed: return "UserdataModulesNotAllowed";
			case NetRejectReason::SessionFull: return "SessionFull";
			case NetRejectReason::HostNotAccepting: return "HostNotAccepting";
			case NetRejectReason::DuplicateClientNonce: return "DuplicateClientNonce";
			case NetRejectReason::MalformedMessage: return "MalformedMessage";
			case NetRejectReason::Timeout: return "Timeout";
			case NetRejectReason::InternalError: return "InternalError";
		}
		return "Unknown";
	}

	const char* NetProtocol::ErrorCodeName(NetProtocolErrorCode code) {
		switch (code) {
			case NetProtocolErrorCode::None: return "None";
			case NetProtocolErrorCode::NullBuffer: return "NullBuffer";
			case NetProtocolErrorCode::ShortHeader: return "ShortHeader";
			case NetProtocolErrorCode::BadMagic: return "BadMagic";
			case NetProtocolErrorCode::UnsupportedVersion: return "UnsupportedVersion";
			case NetProtocolErrorCode::BadHeaderSize: return "BadHeaderSize";
			case NetProtocolErrorCode::UnknownFlags: return "UnknownFlags";
			case NetProtocolErrorCode::UnknownMessageType: return "UnknownMessageType";
			case NetProtocolErrorCode::ReservedFieldNonZero: return "ReservedFieldNonZero";
			case NetProtocolErrorCode::PayloadLengthMismatch: return "PayloadLengthMismatch";
			case NetProtocolErrorCode::PayloadTooLarge: return "PayloadTooLarge";
			case NetProtocolErrorCode::TruncatedPayload: return "TruncatedPayload";
			case NetProtocolErrorCode::TrailingBytes: return "TrailingBytes";
			case NetProtocolErrorCode::StringTooLong: return "StringTooLong";
			case NetProtocolErrorCode::InvalidString: return "InvalidString";
			case NetProtocolErrorCode::InvalidValue: return "InvalidValue";
			case NetProtocolErrorCode::EncodeFailed: return "EncodeFailed";
		}
		return "Unknown";
	}

	bool NetProtocol::Encode(const NetMessage& message, std::vector<uint8_t>& outBytes, NetProtocolError* error) {
		outBytes.clear();
		if (message.flags != 0U) {
			SetError(error, NetProtocolErrorCode::UnknownFlags, 0, "message flags must be zero in protocol v1");
			return false;
		}

		std::vector<uint8_t> payloadBytes;
		const bool payloadOk = std::visit([&](const auto& payload) {
			return EncodePayload(payload, payloadBytes, error);
		}, message.payload);
		if (!payloadOk) {
			return false;
		}
		if (payloadBytes.size() > c_MaxControlPayloadBytes) {
			SetError(error, NetProtocolErrorCode::PayloadTooLarge, c_HeaderBytes, "payload exceeds max control payload size");
			return false;
		}

		outBytes.reserve(c_HeaderBytes + payloadBytes.size());
		AppendU32LE(outBytes, c_Magic);
		AppendU16LE(outBytes, c_Version);
		AppendU16LE(outBytes, c_HeaderBytes);
		AppendU16LE(outBytes, static_cast<uint16_t>(MessageTypeOf(message.payload)));
		AppendU16LE(outBytes, message.flags);
		AppendU32LE(outBytes, message.sequence);
		AppendU32LE(outBytes, static_cast<uint32_t>(payloadBytes.size()));
		AppendU32LE(outBytes, 0);
		outBytes.insert(outBytes.end(), payloadBytes.begin(), payloadBytes.end());
		return true;
	}

	NetDecodeResult NetProtocol::Decode(const std::vector<uint8_t>& bytes) {
		return Decode(bytes.data(), bytes.size());
	}

	NetDecodeResult NetProtocol::Decode(const uint8_t* data, size_t size) {
		if (!data && size > 0) {
			return Fail(NetProtocolErrorCode::NullBuffer, 0, "input buffer is null");
		}
		if (size < c_HeaderBytes) {
			return Fail(NetProtocolErrorCode::ShortHeader, size, "input shorter than network header");
		}

		ByteReader headerReader(data, c_HeaderBytes);
		uint32_t magic = 0;
		uint16_t protocolVersion = 0;
		uint16_t headerBytes = 0;
		uint16_t rawMessageType = 0;
		uint16_t flags = 0;
		uint32_t sequence = 0;
		uint32_t payloadBytes = 0;
		uint32_t reserved = 0;
		headerReader.ReadU32LE(magic);
		headerReader.ReadU16LE(protocolVersion);
		headerReader.ReadU16LE(headerBytes);
		headerReader.ReadU16LE(rawMessageType);
		headerReader.ReadU16LE(flags);
		headerReader.ReadU32LE(sequence);
		headerReader.ReadU32LE(payloadBytes);
		headerReader.ReadU32LE(reserved);

		if (magic != c_Magic) {
			return Fail(NetProtocolErrorCode::BadMagic, 0, "network magic mismatch");
		}
		if (protocolVersion != c_Version) {
			return Fail(NetProtocolErrorCode::UnsupportedVersion, 4, "unsupported protocol version");
		}
		if (headerBytes != c_HeaderBytes) {
			return Fail(NetProtocolErrorCode::BadHeaderSize, 6, "network header size mismatch");
		}
		if (flags != 0U) {
			return Fail(NetProtocolErrorCode::UnknownFlags, 10, "unknown message flags");
		}
		if (reserved != 0U) {
			return Fail(NetProtocolErrorCode::ReservedFieldNonZero, 20, "header reserved field is nonzero");
		}
		if (payloadBytes > c_MaxControlPayloadBytes) {
			return Fail(NetProtocolErrorCode::PayloadTooLarge, 16, "payload exceeds max control payload size");
		}
		if (payloadBytes > size - c_HeaderBytes || c_HeaderBytes + static_cast<size_t>(payloadBytes) != size) {
			return Fail(NetProtocolErrorCode::PayloadLengthMismatch, 16, "payload length does not match buffer length");
		}

		const auto messageType = static_cast<NetMessageType>(rawMessageType);
		ByteReader payloadReader(data + c_HeaderBytes, payloadBytes);
		NetProtocolError payloadError;
		NetPayload payload;
		bool decoded = false;
		switch (messageType) {
			case NetMessageType::ClientHello: {
				NetClientHello value;
				decoded = DecodePayload(payloadReader, value, &payloadError);
				payload = value;
				break;
			}
			case NetMessageType::HostHello: {
				NetHostHello value;
				decoded = DecodePayload(payloadReader, value, &payloadError);
				payload = value;
				break;
			}
			case NetMessageType::JoinAccepted: {
				NetJoinAccepted value;
				decoded = DecodePayload(payloadReader, value, &payloadError);
				payload = value;
				break;
			}
			case NetMessageType::JoinRejected: {
				NetJoinRejected value;
				decoded = DecodePayload(payloadReader, value, &payloadError);
				payload = value;
				break;
			}
			case NetMessageType::ReadyState: {
				NetReadyState value;
				decoded = DecodePayload(payloadReader, value, &payloadError);
				payload = value;
				break;
			}
			case NetMessageType::Heartbeat: {
				NetHeartbeat value;
				decoded = DecodePayload(payloadReader, value, &payloadError);
				payload = value;
				break;
			}
			case NetMessageType::Ping: {
				NetPing value;
				decoded = DecodePayload(payloadReader, value, &payloadError);
				payload = value;
				break;
			}
			case NetMessageType::Pong: {
				NetPong value;
				decoded = DecodePayload(payloadReader, value, &payloadError);
				payload = value;
				break;
			}
			case NetMessageType::Disconnect: {
				NetDisconnect value;
				decoded = DecodePayload(payloadReader, value, &payloadError);
				payload = value;
				break;
			}
			case NetMessageType::SessionSummary: {
				NetSessionSummary value;
				decoded = DecodePayload(payloadReader, value, &payloadError);
				payload = value;
				break;
			}
			default:
				return Fail(NetProtocolErrorCode::UnknownMessageType, 8, "unknown message type");
		}

		if (!decoded) {
			payloadError.offset += c_HeaderBytes;
			NetDecodeResult result;
			result.error = std::move(payloadError);
			return result;
		}
		if (!payloadReader.AtEnd()) {
			return Fail(NetProtocolErrorCode::TrailingBytes, c_HeaderBytes + payloadReader.Offset(), "payload has trailing bytes");
		}

		NetDecodeResult result;
		result.ok = true;
		result.message.sequence = sequence;
		result.message.flags = flags;
		result.message.payload = std::move(payload);
		return result;
	}

} // namespace RTE
