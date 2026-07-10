#include "NetLobbyProtocol.h"

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

		void SetError(NetLobbyError* error, NetLobbyErrorCode code, size_t offset, std::string message) {
			if (error) {
				error->code = code;
				error->offset = offset;
				error->message = std::move(message);
			}
		}

		NetLobbyDecodeResult Fail(NetLobbyErrorCode code, size_t offset, std::string message) {
			NetLobbyDecodeResult result;
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

		bool AppendString(std::vector<uint8_t>& out, const std::string& value, size_t maxBytes, const char* fieldName, NetLobbyError* error) {
			if (value.size() > maxBytes || value.size() > std::numeric_limits<uint16_t>::max()) {
				SetError(error, NetLobbyErrorCode::StringTooLong, out.size(), std::string(fieldName) + " exceeds max encoded length");
				return false;
			}
			if (HasControlChars(value)) {
				SetError(error, NetLobbyErrorCode::InvalidString, out.size(), std::string(fieldName) + " contains control characters");
				return false;
			}
			AppendU16LE(out, static_cast<uint16_t>(value.size()));
			out.insert(out.end(), value.begin(), value.end());
			return true;
		}

		void AppendHash(std::vector<uint8_t>& out, const NetHash32& hash) {
			out.insert(out.end(), hash.begin(), hash.end());
		}

		bool AppendPlayer(std::vector<uint8_t>& out, const NetMatchPlayerSlot& player, NetLobbyError* error) {
			AppendU8(out, player.peerId);
			AppendU8(out, player.team);
			AppendBool(out, player.cpu);
			AppendU8(out, 0);
			return AppendString(out, player.displayName, NetLobbyProtocol::c_MaxDisplayNameBytes, "player.display_name", error);
		}

		class ByteReader {
		public:
			ByteReader(const uint8_t* data, size_t size) : m_Data(data), m_Size(size) {}

			size_t Offset() const { return m_Offset; }
			bool AtEnd() const { return m_Offset == m_Size; }

			bool ReadU8(uint8_t& out) {
				if (!CanRead(1)) return false;
				out = m_Data[m_Offset++];
				return true;
			}

			bool ReadBool(bool& out) {
				uint8_t value = 0;
				if (!ReadU8(value) || value > 1) return false;
				out = value != 0;
				return true;
			}

			bool ReadU16LE(uint16_t& out) {
				if (!CanRead(2)) return false;
				out = static_cast<uint16_t>(m_Data[m_Offset]) |
				      static_cast<uint16_t>(static_cast<uint16_t>(m_Data[m_Offset + 1]) << 8);
				m_Offset += 2;
				return true;
			}

			bool ReadU32LE(uint32_t& out) {
				if (!CanRead(4)) return false;
				out = 0;
				for (int i = 0; i < 4; ++i) {
					out |= static_cast<uint32_t>(m_Data[m_Offset + i]) << (i * 8);
				}
				m_Offset += 4;
				return true;
			}

			bool ReadU64LE(uint64_t& out) {
				if (!CanRead(8)) return false;
				out = 0;
				for (int i = 0; i < 8; ++i) {
					out |= static_cast<uint64_t>(m_Data[m_Offset + i]) << (i * 8);
				}
				m_Offset += 8;
				return true;
			}

			bool ReadHash(NetHash32& out) {
				if (!CanRead(out.size())) return false;
				std::memcpy(out.data(), m_Data + m_Offset, out.size());
				m_Offset += out.size();
				return true;
			}

			bool ReadBytes(std::vector<uint8_t>& out, size_t count) {
				if (!CanRead(count)) return false;
				out.assign(m_Data + m_Offset, m_Data + m_Offset + count);
				m_Offset += count;
				return true;
			}

			bool ReadString(std::string& out, size_t maxBytes, const char* fieldName, NetLobbyError* error) {
				uint16_t length = 0;
				const size_t lengthOffset = m_Offset;
				if (!ReadU16LE(length)) {
					SetError(error, NetLobbyErrorCode::TruncatedPayload, lengthOffset, std::string(fieldName) + " length is truncated");
					return false;
				}
				if (length > maxBytes) {
					SetError(error, NetLobbyErrorCode::StringTooLong, lengthOffset, std::string(fieldName) + " exceeds max encoded length");
					return false;
				}
				if (!CanRead(length)) {
					SetError(error, NetLobbyErrorCode::TruncatedPayload, m_Offset, std::string(fieldName) + " data is truncated");
					return false;
				}
				out.assign(reinterpret_cast<const char*>(m_Data + m_Offset), length);
				m_Offset += length;
				if (HasControlChars(out)) {
					SetError(error, NetLobbyErrorCode::InvalidString, lengthOffset, std::string(fieldName) + " contains control characters");
					return false;
				}
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

		bool ReadOrTruncated(bool ok, const ByteReader& reader, NetLobbyError* error, const char* fieldName) {
			if (!ok) {
				SetError(error, NetLobbyErrorCode::TruncatedPayload, reader.Offset(), std::string(fieldName) + " is truncated or invalid");
			}
			return ok;
		}

		bool ReadMode(ByteReader& reader, NetMatchMode& out, NetLobbyError* error) {
			uint8_t raw = 0;
			if (!ReadOrTruncated(reader.ReadU8(raw), reader, error, "match mode")) return false;
			switch (static_cast<NetMatchMode>(raw)) {
				case NetMatchMode::PvPSkirmish:
				case NetMatchMode::CoopPvE:
				case NetMatchMode::PvPvE:
					out = static_cast<NetMatchMode>(raw);
					return true;
			}
			SetError(error, NetLobbyErrorCode::InvalidValue, reader.Offset() - 1, "match mode has invalid enum value");
			return false;
		}

		bool ReadOwnershipPolicy(ByteReader& reader, NetActorOwnershipPolicy& out, NetLobbyError* error) {
			uint8_t raw = 0;
			if (!ReadOrTruncated(reader.ReadU8(raw), reader, error, "ownership policy")) return false;
			switch (static_cast<NetActorOwnershipPolicy>(raw)) {
				case NetActorOwnershipPolicy::UniqueIdModPeerCount:
				case NetActorOwnershipPolicy::TeamOwner:
				case NetActorOwnershipPolicy::HostCpuRemoteHuman:
					out = static_cast<NetActorOwnershipPolicy>(raw);
					return true;
			}
			SetError(error, NetLobbyErrorCode::InvalidValue, reader.Offset() - 1, "ownership policy has invalid enum value");
			return false;
		}

		bool ReadPlayer(ByteReader& reader, NetMatchPlayerSlot& out, NetLobbyError* error) {
			uint8_t reserved = 0;
			return ReadOrTruncated(reader.ReadU8(out.peerId), reader, error, "player.peer_id") &&
			       ReadOrTruncated(reader.ReadU8(out.team), reader, error, "player.team") &&
			       ReadOrTruncated(reader.ReadBool(out.cpu), reader, error, "player.cpu") &&
			       ReadOrTruncated(reader.ReadU8(reserved), reader, error, "player.reserved") &&
			       (reserved == 0 || (SetError(error, NetLobbyErrorCode::ReservedFieldNonZero, reader.Offset() - 1, "player reserved field must be zero"), false)) &&
			       reader.ReadString(out.displayName, NetLobbyProtocol::c_MaxDisplayNameBytes, "player.display_name", error);
		}

		bool EncodeConfig(const NetMatchConfig& config, std::vector<uint8_t>& out, NetLobbyError* error) {
			std::string validateError;
			if (!NetMatchConfigUtil::ValidateLocalAlpha(config, &validateError)) {
				SetError(error, NetLobbyErrorCode::InvalidValue, out.size(), validateError);
				return false;
			}
			AppendU16LE(out, config.version);
			AppendU64LE(out, config.sessionId);
			AppendU8(out, config.hostPeerId);
			AppendU8(out, config.peerCount);
			AppendU16LE(out, config.inputDelayFrames);
			AppendU8(out, static_cast<uint8_t>(config.mode));
			AppendU8(out, static_cast<uint8_t>(config.ownershipPolicy));
			AppendU16LE(out, 0);
			if (!AppendString(out, config.activityType, NetLobbyProtocol::c_MaxShortTextBytes, "activity_type", error) ||
			    !AppendString(out, config.activityPreset, NetLobbyProtocol::c_MaxShortTextBytes, "activity_preset", error) ||
			    !AppendString(out, config.sceneName, NetLobbyProtocol::c_MaxShortTextBytes, "scene_name", error) ||
			    !AppendString(out, config.modePreset, NetLobbyProtocol::c_MaxShortTextBytes, "mode_preset", error)) {
				return false;
			}
			if (config.players.size() > NetLobbyProtocol::c_MaxPlayers) {
				SetError(error, NetLobbyErrorCode::PayloadTooLarge, out.size(), "too many player slots");
				return false;
			}
			AppendU8(out, static_cast<uint8_t>(config.players.size()));
			for (const NetMatchPlayerSlot& player : config.players) {
				if (!AppendPlayer(out, player, error)) return false;
			}
			AppendU8(out, static_cast<uint8_t>(config.peerInputDelayFrames.size()));
			for (uint16_t delay : config.peerInputDelayFrames) {
				AppendU16LE(out, delay);
			}
			return true;
		}

		bool DecodeConfig(ByteReader& reader, NetMatchConfig& out, NetLobbyError* error) {
			uint16_t reserved = 0;
			uint8_t playerCount = 0;
			if (!ReadOrTruncated(reader.ReadU16LE(out.version), reader, error, "config.version") ||
			    !ReadOrTruncated(reader.ReadU64LE(out.sessionId), reader, error, "config.session_id") ||
			    !ReadOrTruncated(reader.ReadU8(out.hostPeerId), reader, error, "config.host_peer_id") ||
			    !ReadOrTruncated(reader.ReadU8(out.peerCount), reader, error, "config.peer_count") ||
			    !ReadOrTruncated(reader.ReadU16LE(out.inputDelayFrames), reader, error, "config.input_delay_frames") ||
			    !ReadMode(reader, out.mode, error) ||
			    !ReadOwnershipPolicy(reader, out.ownershipPolicy, error) ||
			    !ReadOrTruncated(reader.ReadU16LE(reserved), reader, error, "config.reserved")) {
				return false;
			}
			if (reserved != 0) {
				SetError(error, NetLobbyErrorCode::ReservedFieldNonZero, reader.Offset() - 2, "config reserved field must be zero");
				return false;
			}
			if (!reader.ReadString(out.activityType, NetLobbyProtocol::c_MaxShortTextBytes, "activity_type", error) ||
			    !reader.ReadString(out.activityPreset, NetLobbyProtocol::c_MaxShortTextBytes, "activity_preset", error) ||
			    !reader.ReadString(out.sceneName, NetLobbyProtocol::c_MaxShortTextBytes, "scene_name", error) ||
			    !reader.ReadString(out.modePreset, NetLobbyProtocol::c_MaxShortTextBytes, "mode_preset", error) ||
			    !ReadOrTruncated(reader.ReadU8(playerCount), reader, error, "player_count")) {
				return false;
			}
			if (playerCount == 0 || playerCount > NetLobbyProtocol::c_MaxPlayers) {
				SetError(error, NetLobbyErrorCode::InvalidValue, reader.Offset() - 1, "player_count is out of range");
				return false;
			}
			out.players.clear();
			out.players.reserve(playerCount);
			for (uint8_t i = 0; i < playerCount; ++i) {
				NetMatchPlayerSlot player;
				if (!ReadPlayer(reader, player, error)) {
					return false;
				}
				out.players.push_back(std::move(player));
			}
			out.peerInputDelayFrames.clear();
			// A v1 config (an old replay header) predates per-peer delays and means a uniform delay.
			if (out.version == 1) {
				out.version = 2;
			} else {
				uint8_t delayCount = 0;
				if (!ReadOrTruncated(reader.ReadU8(delayCount), reader, error, "peer_input_delay_count")) {
					return false;
				}
				out.peerInputDelayFrames.reserve(delayCount);
				for (uint8_t i = 0; i < delayCount; ++i) {
					uint16_t delay = 0;
					if (!ReadOrTruncated(reader.ReadU16LE(delay), reader, error, "peer_input_delay")) {
						return false;
					}
					out.peerInputDelayFrames.push_back(delay);
				}
			}
			std::string validateError;
			if (!NetMatchConfigUtil::ValidateLocalAlpha(out, &validateError)) {
				SetError(error, NetLobbyErrorCode::InvalidValue, reader.Offset(), validateError);
				return false;
			}
			return true;
		}

		bool EncodePayload(const NetLobbyHello& payload, std::vector<uint8_t>& out, NetLobbyError* error) {
			AppendU16LE(out, payload.minProtocolVersion);
			AppendU16LE(out, payload.maxProtocolVersion);
			AppendU8(out, payload.peerId);
			AppendU8(out, 0);
			if (!AppendString(out, payload.displayName, NetLobbyProtocol::c_MaxDisplayNameBytes, "display_name", error) ||
			    !AppendString(out, payload.desiredRole, NetLobbyProtocol::c_MaxShortTextBytes, "desired_role", error)) {
				return false;
			}
			return true;
		}

		bool EncodePayload(const NetLobbyPeerState& payload, std::vector<uint8_t>& out, NetLobbyError* error) {
			AppendU8(out, payload.peerId);
			AppendBool(out, payload.ready);
			AppendU16LE(out, 0);
			AppendU32LE(out, payload.pingMs);
			AppendU32LE(out, payload.jitterMs);
			return AppendString(out, payload.displayName, NetLobbyProtocol::c_MaxDisplayNameBytes, "display_name", error) &&
			       AppendString(out, payload.platform, NetLobbyProtocol::c_MaxShortTextBytes, "platform", error);
		}

		bool EncodePayload(const NetLobbyMatchConfig& payload, std::vector<uint8_t>& out, NetLobbyError* error) {
			return EncodeConfig(payload.config, out, error);
		}

		bool EncodePayload(const NetLobbyConfigAck& payload, std::vector<uint8_t>& out, NetLobbyError* error) {
			AppendU8(out, payload.peerId);
			AppendBool(out, payload.accepted);
			AppendU16LE(out, 0);
			AppendHash(out, payload.matchConfigHash);
			return AppendString(out, payload.reason, NetLobbyProtocol::c_MaxShortTextBytes, "reason", error);
		}

		bool EncodePayload(const NetLobbyReady& payload, std::vector<uint8_t>& out, NetLobbyError*) {
			AppendU8(out, payload.peerId);
			AppendBool(out, payload.ready);
			AppendU16LE(out, 0);
			return true;
		}

		bool EncodePayload(const NetLobbyStart& payload, std::vector<uint8_t>& out, NetLobbyError*) {
			AppendU64LE(out, payload.sessionId);
			AppendU64LE(out, payload.startFrame);
			AppendU16LE(out, payload.inputDelayFrames);
			AppendU16LE(out, 0);
			AppendHash(out, payload.matchConfigHash);
			return true;
		}

		bool EncodePayload(const NetLobbyAbort& payload, std::vector<uint8_t>& out, NetLobbyError* error) {
			AppendU8(out, payload.peerId);
			AppendU8(out, 0);
			AppendU16LE(out, 0);
			return AppendString(out, payload.reason, NetLobbyProtocol::c_MaxShortTextBytes, "reason", error);
		}

		bool EncodePayload(const NetLobbyStateChunk& payload, std::vector<uint8_t>& out, NetLobbyError* error) {
			if (payload.bytes.size() > NetLobbyProtocol::c_MaxStateChunkBytes) {
				SetError(error, NetLobbyErrorCode::PayloadTooLarge, out.size(), "state chunk exceeds max size");
				return false;
			}
			AppendU64LE(out, payload.transferId);
			AppendU32LE(out, payload.totalBytes);
			AppendU16LE(out, payload.chunkIndex);
			AppendU16LE(out, payload.chunkCount);
			AppendU32LE(out, static_cast<uint32_t>(payload.bytes.size()));
			out.insert(out.end(), payload.bytes.begin(), payload.bytes.end());
			return true;
		}

		bool DecodePayload(NetLobbyMessageType type, ByteReader& reader, NetLobbyPayload& out, NetLobbyError* error) {
			switch (type) {
				case NetLobbyMessageType::Hello: {
					NetLobbyHello payload;
					uint8_t reserved = 0;
					if (!ReadOrTruncated(reader.ReadU16LE(payload.minProtocolVersion), reader, error, "min_protocol_version") ||
					    !ReadOrTruncated(reader.ReadU16LE(payload.maxProtocolVersion), reader, error, "max_protocol_version") ||
					    !ReadOrTruncated(reader.ReadU8(payload.peerId), reader, error, "peer_id") ||
					    !ReadOrTruncated(reader.ReadU8(reserved), reader, error, "reserved")) return false;
					if (reserved != 0) {
						SetError(error, NetLobbyErrorCode::ReservedFieldNonZero, reader.Offset() - 1, "reserved field must be zero");
						return false;
					}
					if (!reader.ReadString(payload.displayName, NetLobbyProtocol::c_MaxDisplayNameBytes, "display_name", error) ||
					    !reader.ReadString(payload.desiredRole, NetLobbyProtocol::c_MaxShortTextBytes, "desired_role", error)) return false;
					out = std::move(payload);
					return true;
				}
				case NetLobbyMessageType::PeerState: {
					NetLobbyPeerState payload;
					uint16_t reserved = 0;
					if (!ReadOrTruncated(reader.ReadU8(payload.peerId), reader, error, "peer_id") ||
					    !ReadOrTruncated(reader.ReadBool(payload.ready), reader, error, "ready") ||
					    !ReadOrTruncated(reader.ReadU16LE(reserved), reader, error, "reserved") ||
					    !ReadOrTruncated(reader.ReadU32LE(payload.pingMs), reader, error, "ping_ms") ||
					    !ReadOrTruncated(reader.ReadU32LE(payload.jitterMs), reader, error, "jitter_ms")) return false;
					if (reserved != 0) {
						SetError(error, NetLobbyErrorCode::ReservedFieldNonZero, reader.Offset() - 10, "reserved field must be zero");
						return false;
					}
					if (!reader.ReadString(payload.displayName, NetLobbyProtocol::c_MaxDisplayNameBytes, "display_name", error) ||
					    !reader.ReadString(payload.platform, NetLobbyProtocol::c_MaxShortTextBytes, "platform", error)) return false;
					out = std::move(payload);
					return true;
				}
				case NetLobbyMessageType::MatchConfig: {
					NetLobbyMatchConfig payload;
					if (!DecodeConfig(reader, payload.config, error)) return false;
					out = std::move(payload);
					return true;
				}
				case NetLobbyMessageType::ConfigAck: {
					NetLobbyConfigAck payload;
					uint16_t reserved = 0;
					if (!ReadOrTruncated(reader.ReadU8(payload.peerId), reader, error, "peer_id") ||
					    !ReadOrTruncated(reader.ReadBool(payload.accepted), reader, error, "accepted") ||
					    !ReadOrTruncated(reader.ReadU16LE(reserved), reader, error, "reserved") ||
					    !ReadOrTruncated(reader.ReadHash(payload.matchConfigHash), reader, error, "match_config_hash")) return false;
					if (reserved != 0) {
						SetError(error, NetLobbyErrorCode::ReservedFieldNonZero, reader.Offset() - 34, "reserved field must be zero");
						return false;
					}
					if (!reader.ReadString(payload.reason, NetLobbyProtocol::c_MaxShortTextBytes, "reason", error)) return false;
					out = std::move(payload);
					return true;
				}
				case NetLobbyMessageType::Ready: {
					NetLobbyReady payload;
					uint16_t reserved = 0;
					if (!ReadOrTruncated(reader.ReadU8(payload.peerId), reader, error, "peer_id") ||
					    !ReadOrTruncated(reader.ReadBool(payload.ready), reader, error, "ready") ||
					    !ReadOrTruncated(reader.ReadU16LE(reserved), reader, error, "reserved")) return false;
					if (reserved != 0) {
						SetError(error, NetLobbyErrorCode::ReservedFieldNonZero, reader.Offset() - 2, "reserved field must be zero");
						return false;
					}
					out = payload;
					return true;
				}
				case NetLobbyMessageType::Start: {
					NetLobbyStart payload;
					uint16_t reserved = 0;
					if (!ReadOrTruncated(reader.ReadU64LE(payload.sessionId), reader, error, "session_id") ||
					    !ReadOrTruncated(reader.ReadU64LE(payload.startFrame), reader, error, "start_frame") ||
					    !ReadOrTruncated(reader.ReadU16LE(payload.inputDelayFrames), reader, error, "input_delay_frames") ||
					    !ReadOrTruncated(reader.ReadU16LE(reserved), reader, error, "reserved") ||
					    !ReadOrTruncated(reader.ReadHash(payload.matchConfigHash), reader, error, "match_config_hash")) return false;
					if (reserved != 0) {
						SetError(error, NetLobbyErrorCode::ReservedFieldNonZero, reader.Offset() - 34, "reserved field must be zero");
						return false;
					}
					out = payload;
					return true;
				}
				case NetLobbyMessageType::Abort: {
					NetLobbyAbort payload;
					uint8_t reserved8 = 0;
					uint16_t reserved16 = 0;
					if (!ReadOrTruncated(reader.ReadU8(payload.peerId), reader, error, "peer_id") ||
					    !ReadOrTruncated(reader.ReadU8(reserved8), reader, error, "reserved") ||
					    !ReadOrTruncated(reader.ReadU16LE(reserved16), reader, error, "reserved")) return false;
					if (reserved8 != 0 || reserved16 != 0) {
						SetError(error, NetLobbyErrorCode::ReservedFieldNonZero, reader.Offset() - 3, "reserved field must be zero");
						return false;
					}
					if (!reader.ReadString(payload.reason, NetLobbyProtocol::c_MaxShortTextBytes, "reason", error)) return false;
					out = std::move(payload);
					return true;
				}
				case NetLobbyMessageType::StateChunk: {
					NetLobbyStateChunk payload;
					uint32_t byteCount = 0;
					if (!ReadOrTruncated(reader.ReadU64LE(payload.transferId), reader, error, "transfer_id") ||
					    !ReadOrTruncated(reader.ReadU32LE(payload.totalBytes), reader, error, "total_bytes") ||
					    !ReadOrTruncated(reader.ReadU16LE(payload.chunkIndex), reader, error, "chunk_index") ||
					    !ReadOrTruncated(reader.ReadU16LE(payload.chunkCount), reader, error, "chunk_count") ||
					    !ReadOrTruncated(reader.ReadU32LE(byteCount), reader, error, "byte_count")) return false;
					if (byteCount > NetLobbyProtocol::c_MaxStateChunkBytes || payload.chunkIndex >= payload.chunkCount || payload.chunkCount == 0 ||
					    payload.totalBytes > NetLobbyProtocol::c_MaxTotalStateBytes || payload.chunkCount > NetLobbyProtocol::c_MaxStateChunkCount) {
						SetError(error, NetLobbyErrorCode::InvalidValue, reader.Offset(), "state chunk header is invalid");
						return false;
					}
					if (!reader.ReadBytes(payload.bytes, byteCount)) {
						SetError(error, NetLobbyErrorCode::TruncatedPayload, reader.Offset(), "state chunk bytes are truncated");
						return false;
					}
					out = std::move(payload);
					return true;
				}
			}
			SetError(error, NetLobbyErrorCode::UnknownMessageType, reader.Offset(), "unknown lobby message type");
			return false;
		}

		bool ReadMessageType(uint16_t raw, NetLobbyMessageType& out) {
			switch (static_cast<NetLobbyMessageType>(raw)) {
				case NetLobbyMessageType::Hello:
				case NetLobbyMessageType::PeerState:
				case NetLobbyMessageType::MatchConfig:
				case NetLobbyMessageType::ConfigAck:
				case NetLobbyMessageType::Ready:
				case NetLobbyMessageType::Start:
				case NetLobbyMessageType::Abort:
				case NetLobbyMessageType::StateChunk:
					out = static_cast<NetLobbyMessageType>(raw);
					return true;
			}
			return false;
		}
	}

	NetLobbyMessageType NetLobbyProtocol::MessageTypeOf(const NetLobbyPayload& payload) {
		return std::visit(Overloaded{
			[](const NetLobbyHello&) { return NetLobbyMessageType::Hello; },
			[](const NetLobbyPeerState&) { return NetLobbyMessageType::PeerState; },
			[](const NetLobbyMatchConfig&) { return NetLobbyMessageType::MatchConfig; },
			[](const NetLobbyConfigAck&) { return NetLobbyMessageType::ConfigAck; },
			[](const NetLobbyReady&) { return NetLobbyMessageType::Ready; },
			[](const NetLobbyStart&) { return NetLobbyMessageType::Start; },
			[](const NetLobbyAbort&) { return NetLobbyMessageType::Abort; },
			[](const NetLobbyStateChunk&) { return NetLobbyMessageType::StateChunk; },
		}, payload);
	}

	const char* NetLobbyProtocol::MessageTypeName(NetLobbyMessageType type) {
		switch (type) {
			case NetLobbyMessageType::Hello: return "Hello";
			case NetLobbyMessageType::PeerState: return "PeerState";
			case NetLobbyMessageType::MatchConfig: return "MatchConfig";
			case NetLobbyMessageType::ConfigAck: return "ConfigAck";
			case NetLobbyMessageType::Ready: return "Ready";
			case NetLobbyMessageType::Start: return "Start";
			case NetLobbyMessageType::Abort: return "Abort";
			case NetLobbyMessageType::StateChunk: return "StateChunk";
		}
		return "Unknown";
	}

	const char* NetLobbyProtocol::ErrorCodeName(NetLobbyErrorCode code) {
		switch (code) {
			case NetLobbyErrorCode::None: return "None";
			case NetLobbyErrorCode::NullBuffer: return "NullBuffer";
			case NetLobbyErrorCode::ShortHeader: return "ShortHeader";
			case NetLobbyErrorCode::BadMagic: return "BadMagic";
			case NetLobbyErrorCode::UnsupportedVersion: return "UnsupportedVersion";
			case NetLobbyErrorCode::BadHeaderSize: return "BadHeaderSize";
			case NetLobbyErrorCode::UnknownFlags: return "UnknownFlags";
			case NetLobbyErrorCode::UnknownMessageType: return "UnknownMessageType";
			case NetLobbyErrorCode::ReservedFieldNonZero: return "ReservedFieldNonZero";
			case NetLobbyErrorCode::PayloadLengthMismatch: return "PayloadLengthMismatch";
			case NetLobbyErrorCode::PayloadTooLarge: return "PayloadTooLarge";
			case NetLobbyErrorCode::TruncatedPayload: return "TruncatedPayload";
			case NetLobbyErrorCode::TrailingBytes: return "TrailingBytes";
			case NetLobbyErrorCode::StringTooLong: return "StringTooLong";
			case NetLobbyErrorCode::InvalidString: return "InvalidString";
			case NetLobbyErrorCode::InvalidValue: return "InvalidValue";
			case NetLobbyErrorCode::EncodeFailed: return "EncodeFailed";
		}
		return "Unknown";
	}

	bool NetLobbyProtocol::Encode(const NetLobbyMessage& message, std::vector<uint8_t>& outBytes, NetLobbyError* error) {
		if (error) *error = {};
		std::vector<uint8_t> payloadBytes;
		const bool ok = std::visit([&](const auto& payload) {
			return EncodePayload(payload, payloadBytes, error);
		}, message.payload);
		if (!ok) {
			if (error && error->code == NetLobbyErrorCode::None) {
				SetError(error, NetLobbyErrorCode::EncodeFailed, 0, "could not encode lobby payload");
			}
			return false;
		}
		if (payloadBytes.size() > c_MaxPayloadBytes) {
			SetError(error, NetLobbyErrorCode::PayloadTooLarge, 0, "payload exceeds max encoded length");
			return false;
		}
		outBytes.clear();
		AppendU32LE(outBytes, c_Magic);
		AppendU16LE(outBytes, c_Version);
		AppendU16LE(outBytes, c_HeaderBytes);
		AppendU16LE(outBytes, static_cast<uint16_t>(MessageTypeOf(message.payload)));
		AppendU16LE(outBytes, 0);
		AppendU32LE(outBytes, static_cast<uint32_t>(payloadBytes.size()));
		outBytes.insert(outBytes.end(), payloadBytes.begin(), payloadBytes.end());
		return true;
	}

	NetLobbyDecodeResult NetLobbyProtocol::Decode(const uint8_t* data, size_t size) {
		if (!data) {
			return Fail(NetLobbyErrorCode::NullBuffer, 0, "data is null");
		}
		if (size < c_HeaderBytes) {
			return Fail(NetLobbyErrorCode::ShortHeader, size, "header is truncated");
		}
		ByteReader reader(data, size);
		uint32_t magic = 0;
		uint16_t version = 0;
		uint16_t headerBytes = 0;
		uint16_t rawType = 0;
		uint16_t flags = 0;
		uint32_t payloadBytes = 0;
		NetLobbyError error;
		if (!ReadOrTruncated(reader.ReadU32LE(magic), reader, &error, "magic") ||
		    !ReadOrTruncated(reader.ReadU16LE(version), reader, &error, "version") ||
		    !ReadOrTruncated(reader.ReadU16LE(headerBytes), reader, &error, "header_bytes") ||
		    !ReadOrTruncated(reader.ReadU16LE(rawType), reader, &error, "message_type") ||
		    !ReadOrTruncated(reader.ReadU16LE(flags), reader, &error, "flags") ||
		    !ReadOrTruncated(reader.ReadU32LE(payloadBytes), reader, &error, "payload_bytes")) {
			return Fail(error.code, error.offset, error.message);
		}
		if (magic != c_Magic) {
			return Fail(NetLobbyErrorCode::BadMagic, 0, "bad lobby protocol magic");
		}
		if (version != c_Version) {
			return Fail(NetLobbyErrorCode::UnsupportedVersion, 4, "unsupported lobby protocol version");
		}
		if (headerBytes != c_HeaderBytes) {
			return Fail(NetLobbyErrorCode::BadHeaderSize, 6, "unsupported lobby header size");
		}
		if (flags != 0) {
			return Fail(NetLobbyErrorCode::UnknownFlags, 10, "lobby flags must be zero");
		}
		if (payloadBytes > c_MaxPayloadBytes) {
			return Fail(NetLobbyErrorCode::PayloadTooLarge, 12, "payload exceeds max encoded length");
		}
		if (payloadBytes != size - c_HeaderBytes) {
			return Fail(NetLobbyErrorCode::PayloadLengthMismatch, 12, "payload length does not match buffer size");
		}
		NetLobbyMessageType type{};
		if (!ReadMessageType(rawType, type)) {
			return Fail(NetLobbyErrorCode::UnknownMessageType, 8, "unknown lobby message type");
		}
		NetLobbyDecodeResult result;
		NetLobbyPayload payload;
		if (!DecodePayload(type, reader, payload, &result.error)) {
			return result;
		}
		if (!reader.AtEnd()) {
			return Fail(NetLobbyErrorCode::TrailingBytes, reader.Offset(), "payload has trailing bytes");
		}
		result.ok = true;
		result.message.payload = std::move(payload);
		return result;
	}

	NetLobbyDecodeResult NetLobbyProtocol::Decode(const std::vector<uint8_t>& bytes) {
		return Decode(bytes.data(), bytes.size());
	}

} // namespace RTE
