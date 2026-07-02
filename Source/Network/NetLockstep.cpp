#include "NetLockstep.h"

#include "NetActorOwnership.h"
#include "NetLobbyProtocol.h"
#include "NetProtocol.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>

namespace RTE {

	namespace {
		template <class... T>
		struct Overloaded : T... {
			using T::operator()...;
		};

		template <class... T>
		Overloaded(T...) -> Overloaded<T...>;

		void SetError(NetLockstepError* error, NetLockstepErrorCode code, size_t offset, std::string message) {
			if (error) {
				error->code = code;
				error->offset = offset;
				error->message = std::move(message);
			}
		}

		NetLockstepDecodeResult Fail(NetLockstepErrorCode code, size_t offset, std::string message) {
			NetLockstepDecodeResult result;
			result.error.code = code;
			result.error.offset = offset;
			result.error.message = std::move(message);
			return result;
		}

		void AppendU8(std::vector<uint8_t>& out, uint8_t value) {
			out.push_back(value);
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

		bool AppendString(std::vector<uint8_t>& out, const std::string& value, size_t maxBytes, const char* fieldName, NetLockstepError* error) {
			if (value.size() > maxBytes || value.size() > std::numeric_limits<uint16_t>::max()) {
				SetError(error, NetLockstepErrorCode::StringTooLong, out.size(), std::string(fieldName) + " exceeds max encoded length");
				return false;
			}
			if (HasControlChars(value)) {
				SetError(error, NetLockstepErrorCode::InvalidString, out.size(), std::string(fieldName) + " contains control characters");
				return false;
			}
			AppendU16LE(out, static_cast<uint16_t>(value.size()));
			out.insert(out.end(), value.begin(), value.end());
			return true;
		}

		uint32_t FloatToBitsLE(float value) {
			uint32_t bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			return bits;
		}

		float FloatFromBitsLE(uint32_t bits) {
			float value = 0.0F;
			std::memcpy(&value, &bits, sizeof(value));
			return value;
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

			bool ReadBytes(const uint8_t*& out, size_t bytes) {
				if (!CanRead(bytes)) {
					return false;
				}
				out = m_Data + m_Offset;
				m_Offset += bytes;
				return true;
			}

			bool ReadString(std::string& out, size_t maxBytes, const char* fieldName, NetLockstepError* error) {
				uint16_t length = 0;
				const size_t lengthOffset = m_Offset;
				if (!ReadU16LE(length)) {
					SetError(error, NetLockstepErrorCode::TruncatedPayload, lengthOffset, std::string(fieldName) + " length is truncated");
					return false;
				}
				if (length > maxBytes) {
					SetError(error, NetLockstepErrorCode::StringTooLong, lengthOffset, std::string(fieldName) + " exceeds max encoded length");
					return false;
				}
				if (!CanRead(length)) {
					SetError(error, NetLockstepErrorCode::TruncatedPayload, m_Offset, std::string(fieldName) + " data is truncated");
					return false;
				}
				out.assign(reinterpret_cast<const char*>(m_Data + m_Offset), length);
				m_Offset += length;
				if (HasControlChars(out)) {
					SetError(error, NetLockstepErrorCode::InvalidString, lengthOffset, std::string(fieldName) + " contains control characters");
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

		bool ReadOrTruncated(bool ok, const ByteReader& reader, NetLockstepError* error, const char* fieldName) {
			if (!ok) {
				SetError(error, NetLockstepErrorCode::TruncatedPayload, reader.Offset(), std::string(fieldName) + " is truncated or invalid");
			}
			return ok;
		}

		bool ReadStopReason(ByteReader& reader, NetLockstepStopReason& out, NetLockstepError* error) {
			uint16_t rawReason = 0;
			if (!ReadOrTruncated(reader.ReadU16LE(rawReason), reader, error, "stop reason")) {
				return false;
			}
			switch (static_cast<NetLockstepStopReason>(rawReason)) {
				case NetLockstepStopReason::Complete:
				case NetLockstepStopReason::MissingFrameTimeout:
				case NetLockstepStopReason::Desync:
				case NetLockstepStopReason::ProtocolError:
				case NetLockstepStopReason::PeerDisconnected:
				case NetLockstepStopReason::InternalError:
					out = static_cast<NetLockstepStopReason>(rawReason);
					return true;
			}
			SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset() - 2, "stop reason has invalid enum value");
			return false;
		}

		bool ValidatePeerId(uint8_t peerId, NetLockstepError* error, const char* fieldName) {
			if (peerId == 0 || peerId > NetLockstepCodec::c_MaxPeerCount) {
				SetError(error, NetLockstepErrorCode::InvalidValue, 0, std::string(fieldName) + " is out of range");
				return false;
			}
			return true;
		}

		bool ValidateStart(const NetLockstepStart& payload, NetLockstepError* error) {
			if (payload.sessionId == 0) {
				SetError(error, NetLockstepErrorCode::InvalidValue, 0, "session_id must be nonzero");
				return false;
			}
			if (payload.inputDelayFrames > NetLockstepCodec::c_MaxInputDelayFrames) {
				SetError(error, NetLockstepErrorCode::InvalidValue, 0, "input_delay_frames is out of range");
				return false;
			}
			if (payload.controllerFrameVersion != ControllerFrame::c_Version) {
				SetError(error, NetLockstepErrorCode::InvalidValue, 0, "controller_frame_version mismatch");
				return false;
			}
			if (payload.controllerFrameEncodedSize != ControllerFrame::c_EncodedSize) {
				SetError(error, NetLockstepErrorCode::InvalidValue, 0, "controller_frame_encoded_size mismatch");
				return false;
			}
			if (payload.peerCount == 0 || payload.peerCount > NetLockstepCodec::c_MaxPeerCount || payload.localPeerId == 0 || payload.localPeerId > payload.peerCount) {
				SetError(error, NetLockstepErrorCode::InvalidValue, 0, "peer identity fields are out of range");
				return false;
			}
			return true;
		}

		bool ValidateSortedFrames(const std::vector<ControllerFrame>& frames, NetLockstepError* error) {
			if (frames.size() > NetLockstepCodec::c_MaxFramesPerPacket) {
				SetError(error, NetLockstepErrorCode::PayloadTooLarge, 0, "frame packet has too many ControllerFrames");
				return false;
			}
			int64_t previousActorId = std::numeric_limits<int64_t>::min();
			bool havePrevious = false;
			for (const ControllerFrame& frame : frames) {
				if (havePrevious && frame.actorUniqueID <= previousActorId) {
					SetError(error, NetLockstepErrorCode::InvalidValue, 0, "ControllerFrames must be sorted by unique id without duplicates");
					return false;
				}
				const std::vector<uint8_t> encodedFrame = ControllerFrameCodec::Encode(frame);
				ControllerFrame decodedFrame;
				std::string frameError;
				if (!ControllerFrameCodec::Decode(encodedFrame.data(), encodedFrame.size(), decodedFrame, &frameError)) {
					SetError(error, NetLockstepErrorCode::InvalidValue, 0, "ControllerFrame is invalid: " + frameError);
					return false;
				}
				previousActorId = frame.actorUniqueID;
				havePrevious = true;
			}
			return true;
		}

		bool EncodePayload(const NetLockstepStart& payload, std::vector<uint8_t>& out, NetLockstepError* error) {
			if (!ValidateStart(payload, error)) {
				return false;
			}
			AppendU64LE(out, payload.sessionId);
			AppendU64LE(out, payload.startFrame);
			AppendU16LE(out, payload.inputDelayFrames);
			AppendU16LE(out, payload.controllerFrameVersion);
			AppendU16LE(out, payload.controllerFrameEncodedSize);
			AppendU8(out, payload.localPeerId);
			AppendU8(out, payload.peerCount);
			return AppendString(out, payload.scenario, NetLockstepCodec::c_MaxScenarioBytes, "scenario", error) &&
			       AppendString(out, payload.ownershipPolicy, NetLockstepCodec::c_MaxOwnershipPolicyBytes, "ownership_policy", error);
		}

		bool EncodePayload(const NetLockstepFrame& payload, std::vector<uint8_t>& out, NetLockstepError* error) {
			if (!ValidatePeerId(payload.senderPeerId, error, "sender_peer_id") || !ValidateSortedFrames(payload.frames, error)) {
				return false;
			}
			if (payload.commands.size() > NetLockstepCodec::c_MaxCommandsPerPacket) {
				SetError(error, NetLockstepErrorCode::PayloadTooLarge, out.size(), "frame packet has too many game commands");
				return false;
			}
			AppendU8(out, payload.senderPeerId);
			AppendU8(out, 0);
			AppendU16LE(out, static_cast<uint16_t>(payload.frames.size()));
			AppendU64LE(out, payload.targetFrame);
			for (const ControllerFrame& frame : payload.frames) {
				const std::vector<uint8_t> encodedFrame = ControllerFrameCodec::Encode(frame);
				out.insert(out.end(), encodedFrame.begin(), encodedFrame.end());
			}
			AppendU16LE(out, static_cast<uint16_t>(payload.commands.size()));
			for (const NetGameCommand& command : payload.commands) {
				// The command's sender is the authenticated frame sender; it is set at decode, not encoded here.
				const NetGameCommandType type = NetGameCommandTypeOf(command.payload);
				AppendU16LE(out, static_cast<uint16_t>(type));
				switch (type) {
					case NetGameCommandType::SetTeamFunds: {
						const NetGameSetTeamFunds& funds = std::get<NetGameSetTeamFunds>(command.payload);
						AppendU32LE(out, static_cast<uint32_t>(funds.team));
						AppendU32LE(out, static_cast<uint32_t>(funds.funds));
						break;
					}
					case NetGameCommandType::SpawnActor: {
						const NetGameSpawnActor& spawn = std::get<NetGameSpawnActor>(command.payload);
						if (!AppendString(out, spawn.className, NetLockstepCodec::c_MaxScenarioBytes, "spawn_class_name", error) ||
						    !AppendString(out, spawn.preset, NetLockstepCodec::c_MaxScenarioBytes, "spawn_preset", error) ||
						    !AppendString(out, spawn.module, NetLockstepCodec::c_MaxScenarioBytes, "spawn_module", error)) {
							return false;
						}
						AppendU32LE(out, FloatToBitsLE(spawn.posX));
						AppendU32LE(out, FloatToBitsLE(spawn.posY));
						AppendU32LE(out, static_cast<uint32_t>(spawn.team));
						break;
					}
					case NetGameCommandType::DeliverCargo: {
						const NetGameDeliverCargo& delivery = std::get<NetGameDeliverCargo>(command.payload);
						if (delivery.cargo.size() > NetLockstepCodec::c_MaxCargoPerDelivery) {
							SetError(error, NetLockstepErrorCode::PayloadTooLarge, out.size(), "delivery has too many cargo items");
							return false;
						}
						if (!AppendString(out, delivery.craftClassName, NetLockstepCodec::c_MaxScenarioBytes, "delivery_craft_class_name", error) ||
						    !AppendString(out, delivery.craftPreset, NetLockstepCodec::c_MaxScenarioBytes, "delivery_craft_preset", error) ||
						    !AppendString(out, delivery.craftModule, NetLockstepCodec::c_MaxScenarioBytes, "delivery_craft_module", error)) {
							return false;
						}
						AppendU32LE(out, FloatToBitsLE(delivery.posX));
						AppendU32LE(out, FloatToBitsLE(delivery.posY));
						AppendU32LE(out, static_cast<uint32_t>(delivery.team));
						AppendU16LE(out, static_cast<uint16_t>(delivery.cargo.size()));
						for (const NetGameCargoItem& item : delivery.cargo) {
							if (!AppendString(out, item.className, NetLockstepCodec::c_MaxScenarioBytes, "delivery_cargo_class_name", error) ||
							    !AppendString(out, item.preset, NetLockstepCodec::c_MaxScenarioBytes, "delivery_cargo_preset", error) ||
							    !AppendString(out, item.module, NetLockstepCodec::c_MaxScenarioBytes, "delivery_cargo_module", error)) {
								return false;
							}
						}
						break;
					}
					case NetGameCommandType::ScuttleCraft: {
						const NetGameScuttleCraft& scuttle = std::get<NetGameScuttleCraft>(command.payload);
						AppendU64LE(out, static_cast<uint64_t>(scuttle.actorUID));
						AppendU32LE(out, static_cast<uint32_t>(scuttle.team));
						break;
					}
					case NetGameCommandType::InventoryOp: {
						const NetGameInventoryOp& inventoryOp = std::get<NetGameInventoryOp>(command.payload);
						AppendU64LE(out, static_cast<uint64_t>(inventoryOp.actorUID));
						AppendU32LE(out, static_cast<uint32_t>(inventoryOp.team));
						AppendU8(out, inventoryOp.op);
						AppendU8(out, inventoryOp.hasDropDirection ? 1 : 0);
						AppendU16LE(out, static_cast<uint16_t>(inventoryOp.a));
						AppendU16LE(out, static_cast<uint16_t>(inventoryOp.b));
						AppendU32LE(out, FloatToBitsLE(inventoryOp.dirX));
						AppendU32LE(out, FloatToBitsLE(inventoryOp.dirY));
						break;
					}
				}
			}
			return true;
		}

		bool EncodePayload(const NetLockstepAck& payload, std::vector<uint8_t>& out, NetLockstepError* error) {
			if (!ValidatePeerId(payload.senderPeerId, error, "sender_peer_id")) {
				return false;
			}
			AppendU8(out, payload.senderPeerId);
			AppendU8(out, 0);
			AppendU16LE(out, 0);
			AppendU64LE(out, payload.highestContiguousFrame);
			AppendU32LE(out, payload.receivedMask);
			return true;
		}

		bool EncodePayload(const NetLockstepStop& payload, std::vector<uint8_t>& out, NetLockstepError* error) {
			if (!ValidatePeerId(payload.senderPeerId, error, "sender_peer_id")) {
				return false;
			}
			AppendU8(out, payload.senderPeerId);
			AppendU8(out, 0);
			AppendU16LE(out, static_cast<uint16_t>(payload.reason));
			AppendU64LE(out, payload.frame);
			return AppendString(out, payload.message, NetLockstepCodec::c_MaxDiagnosticBytes, "message", error);
		}

		bool EncodePayload(const NetLockstepChecksum& payload, std::vector<uint8_t>& out, NetLockstepError* error) {
			if (!ValidatePeerId(payload.senderPeerId, error, "sender_peer_id")) {
				return false;
			}
			AppendU8(out, payload.senderPeerId);
			AppendU8(out, 0);
			AppendU16LE(out, 0);
			AppendU64LE(out, payload.frame);
			out.insert(out.end(), payload.hash.begin(), payload.hash.end());
			return true;
		}

		bool DecodeStart(ByteReader& reader, NetLockstepPayload& out, NetLockstepError* error) {
			NetLockstepStart payload;
			if (!ReadOrTruncated(reader.ReadU64LE(payload.sessionId), reader, error, "session_id") ||
			    !ReadOrTruncated(reader.ReadU64LE(payload.startFrame), reader, error, "start_frame") ||
			    !ReadOrTruncated(reader.ReadU16LE(payload.inputDelayFrames), reader, error, "input_delay_frames") ||
			    !ReadOrTruncated(reader.ReadU16LE(payload.controllerFrameVersion), reader, error, "controller_frame_version") ||
			    !ReadOrTruncated(reader.ReadU16LE(payload.controllerFrameEncodedSize), reader, error, "controller_frame_encoded_size") ||
			    !ReadOrTruncated(reader.ReadU8(payload.localPeerId), reader, error, "local_peer_id") ||
			    !ReadOrTruncated(reader.ReadU8(payload.peerCount), reader, error, "peer_count") ||
			    !reader.ReadString(payload.scenario, NetLockstepCodec::c_MaxScenarioBytes, "scenario", error) ||
			    !reader.ReadString(payload.ownershipPolicy, NetLockstepCodec::c_MaxOwnershipPolicyBytes, "ownership_policy", error) ||
			    !ValidateStart(payload, error)) {
				return false;
			}
			out = payload;
			return true;
		}

		bool DecodeFrame(ByteReader& reader, NetLockstepPayload& out, NetLockstepError* error) {
			NetLockstepFrame payload;
			uint8_t reserved = 0;
			uint16_t frameCount = 0;
			if (!ReadOrTruncated(reader.ReadU8(payload.senderPeerId), reader, error, "sender_peer_id") ||
			    !ReadOrTruncated(reader.ReadU8(reserved), reader, error, "reserved") ||
			    !ReadOrTruncated(reader.ReadU16LE(frameCount), reader, error, "frame_count") ||
			    !ReadOrTruncated(reader.ReadU64LE(payload.targetFrame), reader, error, "target_frame")) {
				return false;
			}
			if (reserved != 0) {
				SetError(error, NetLockstepErrorCode::ReservedFieldNonZero, 1, "frame reserved byte must be zero");
				return false;
			}
			if (!ValidatePeerId(payload.senderPeerId, error, "sender_peer_id")) {
				return false;
			}
			if (frameCount > NetLockstepCodec::c_MaxFramesPerPacket) {
				SetError(error, NetLockstepErrorCode::PayloadTooLarge, reader.Offset(), "frame_count exceeds maximum");
				return false;
			}
			payload.frames.reserve(frameCount);
			for (uint16_t i = 0; i < frameCount; ++i) {
				const uint8_t* frameBytes = nullptr;
				if (!ReadOrTruncated(reader.ReadBytes(frameBytes, ControllerFrame::c_EncodedSize), reader, error, "ControllerFrame")) {
					return false;
				}
				ControllerFrame frame;
				std::string frameError;
				if (!ControllerFrameCodec::Decode(frameBytes, ControllerFrame::c_EncodedSize, frame, &frameError)) {
					SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset() - ControllerFrame::c_EncodedSize, "ControllerFrame decode failed: " + frameError);
					return false;
				}
				payload.frames.push_back(frame);
			}
			if (!ValidateSortedFrames(payload.frames, error)) {
				return false;
			}
			uint16_t commandCount = 0;
			if (!ReadOrTruncated(reader.ReadU16LE(commandCount), reader, error, "command_count")) {
				return false;
			}
			if (commandCount > NetLockstepCodec::c_MaxCommandsPerPacket) {
				SetError(error, NetLockstepErrorCode::PayloadTooLarge, reader.Offset(), "command_count exceeds maximum");
				return false;
			}
			payload.commands.reserve(commandCount);
			for (uint16_t i = 0; i < commandCount; ++i) {
				NetGameCommand command;
				command.senderPeerId = payload.senderPeerId;
				uint16_t rawType = 0;
				if (!ReadOrTruncated(reader.ReadU16LE(rawType), reader, error, "command_type")) {
					return false;
				}
				switch (static_cast<NetGameCommandType>(rawType)) {
					case NetGameCommandType::SetTeamFunds: {
						uint32_t team = 0;
						uint32_t amount = 0;
						if (!ReadOrTruncated(reader.ReadU32LE(team), reader, error, "set_team_funds_team") ||
						    !ReadOrTruncated(reader.ReadU32LE(amount), reader, error, "set_team_funds_amount")) {
							return false;
						}
						NetGameSetTeamFunds funds;
						funds.team = static_cast<int32_t>(team);
						funds.funds = static_cast<int32_t>(amount);
						command.payload = funds;
						break;
					}
					case NetGameCommandType::SpawnActor: {
						NetGameSpawnActor spawn;
						uint32_t posXBits = 0;
						uint32_t posYBits = 0;
						uint32_t team = 0;
						if (!reader.ReadString(spawn.className, NetLockstepCodec::c_MaxScenarioBytes, "spawn_class_name", error) ||
						    !reader.ReadString(spawn.preset, NetLockstepCodec::c_MaxScenarioBytes, "spawn_preset", error) ||
						    !reader.ReadString(spawn.module, NetLockstepCodec::c_MaxScenarioBytes, "spawn_module", error) ||
						    !ReadOrTruncated(reader.ReadU32LE(posXBits), reader, error, "spawn_pos_x") ||
						    !ReadOrTruncated(reader.ReadU32LE(posYBits), reader, error, "spawn_pos_y") ||
						    !ReadOrTruncated(reader.ReadU32LE(team), reader, error, "spawn_team")) {
							return false;
						}
						spawn.posX = FloatFromBitsLE(posXBits);
						spawn.posY = FloatFromBitsLE(posYBits);
						spawn.team = static_cast<int32_t>(team);
						command.payload = spawn;
						break;
					}
					case NetGameCommandType::DeliverCargo: {
						NetGameDeliverCargo delivery;
						uint32_t posXBits = 0;
						uint32_t posYBits = 0;
						uint32_t team = 0;
						uint16_t cargoCount = 0;
						if (!reader.ReadString(delivery.craftClassName, NetLockstepCodec::c_MaxScenarioBytes, "delivery_craft_class_name", error) ||
						    !reader.ReadString(delivery.craftPreset, NetLockstepCodec::c_MaxScenarioBytes, "delivery_craft_preset", error) ||
						    !reader.ReadString(delivery.craftModule, NetLockstepCodec::c_MaxScenarioBytes, "delivery_craft_module", error) ||
						    !ReadOrTruncated(reader.ReadU32LE(posXBits), reader, error, "delivery_pos_x") ||
						    !ReadOrTruncated(reader.ReadU32LE(posYBits), reader, error, "delivery_pos_y") ||
						    !ReadOrTruncated(reader.ReadU32LE(team), reader, error, "delivery_team") ||
						    !ReadOrTruncated(reader.ReadU16LE(cargoCount), reader, error, "delivery_cargo_count")) {
							return false;
						}
						if (cargoCount > NetLockstepCodec::c_MaxCargoPerDelivery) {
							SetError(error, NetLockstepErrorCode::PayloadTooLarge, reader.Offset(), "delivery_cargo_count exceeds maximum");
							return false;
						}
						delivery.cargo.reserve(cargoCount);
						for (uint16_t c = 0; c < cargoCount; ++c) {
							NetGameCargoItem item;
							if (!reader.ReadString(item.className, NetLockstepCodec::c_MaxScenarioBytes, "delivery_cargo_class_name", error) ||
							    !reader.ReadString(item.preset, NetLockstepCodec::c_MaxScenarioBytes, "delivery_cargo_preset", error) ||
							    !reader.ReadString(item.module, NetLockstepCodec::c_MaxScenarioBytes, "delivery_cargo_module", error)) {
								return false;
							}
							delivery.cargo.push_back(std::move(item));
						}
						delivery.posX = FloatFromBitsLE(posXBits);
						delivery.posY = FloatFromBitsLE(posYBits);
						delivery.team = static_cast<int32_t>(team);
						command.payload = std::move(delivery);
						break;
					}
					case NetGameCommandType::ScuttleCraft: {
						NetGameScuttleCraft scuttle;
						uint64_t actorUID = 0;
						uint32_t team = 0;
						if (!ReadOrTruncated(reader.ReadU64LE(actorUID), reader, error, "scuttle_actor_uid") ||
						    !ReadOrTruncated(reader.ReadU32LE(team), reader, error, "scuttle_team")) {
							return false;
						}
						scuttle.actorUID = static_cast<int64_t>(actorUID);
						scuttle.team = static_cast<int32_t>(team);
						command.payload = scuttle;
						break;
					}
					case NetGameCommandType::InventoryOp: {
						NetGameInventoryOp inventoryOp;
						uint64_t actorUID = 0;
						uint32_t team = 0;
						uint8_t hasDropDirection = 0;
						uint16_t a = 0;
						uint16_t b = 0;
						uint32_t dirXBits = 0;
						uint32_t dirYBits = 0;
						if (!ReadOrTruncated(reader.ReadU64LE(actorUID), reader, error, "inventory_actor_uid") ||
						    !ReadOrTruncated(reader.ReadU32LE(team), reader, error, "inventory_team") ||
						    !ReadOrTruncated(reader.ReadU8(inventoryOp.op), reader, error, "inventory_op") ||
						    !ReadOrTruncated(reader.ReadU8(hasDropDirection), reader, error, "inventory_has_drop_direction") ||
						    !ReadOrTruncated(reader.ReadU16LE(a), reader, error, "inventory_a") ||
						    !ReadOrTruncated(reader.ReadU16LE(b), reader, error, "inventory_b") ||
						    !ReadOrTruncated(reader.ReadU32LE(dirXBits), reader, error, "inventory_dir_x") ||
						    !ReadOrTruncated(reader.ReadU32LE(dirYBits), reader, error, "inventory_dir_y")) {
							return false;
						}
						if (inventoryOp.op > NetGameInventoryOp::Drop) {
							SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "inventory op is invalid");
							return false;
						}
						inventoryOp.actorUID = static_cast<int64_t>(actorUID);
						inventoryOp.team = static_cast<int32_t>(team);
						inventoryOp.hasDropDirection = hasDropDirection != 0;
						inventoryOp.a = static_cast<int16_t>(a);
						inventoryOp.b = static_cast<int16_t>(b);
						inventoryOp.dirX = FloatFromBitsLE(dirXBits);
						inventoryOp.dirY = FloatFromBitsLE(dirYBits);
						command.payload = inventoryOp;
						break;
					}
					default:
						SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset() - 2, "game command has invalid type");
						return false;
				}
				payload.commands.push_back(std::move(command));
			}
			out = std::move(payload);
			return true;
		}

		bool DecodeAck(ByteReader& reader, NetLockstepPayload& out, NetLockstepError* error) {
			NetLockstepAck payload;
			uint8_t reserved0 = 0;
			uint16_t reserved16 = 0;
			if (!ReadOrTruncated(reader.ReadU8(payload.senderPeerId), reader, error, "sender_peer_id") ||
			    !ReadOrTruncated(reader.ReadU8(reserved0), reader, error, "reserved") ||
			    !ReadOrTruncated(reader.ReadU16LE(reserved16), reader, error, "reserved") ||
			    !ReadOrTruncated(reader.ReadU64LE(payload.highestContiguousFrame), reader, error, "highest_contiguous_frame") ||
			    !ReadOrTruncated(reader.ReadU32LE(payload.receivedMask), reader, error, "received_mask") ||
			    !ValidatePeerId(payload.senderPeerId, error, "sender_peer_id")) {
				return false;
			}
			if (reserved0 != 0 || reserved16 != 0) {
				SetError(error, NetLockstepErrorCode::ReservedFieldNonZero, 1, "ack reserved fields must be zero");
				return false;
			}
			out = payload;
			return true;
		}

		bool DecodeStop(ByteReader& reader, NetLockstepPayload& out, NetLockstepError* error) {
			NetLockstepStop payload;
			uint8_t reserved = 0;
			if (!ReadOrTruncated(reader.ReadU8(payload.senderPeerId), reader, error, "sender_peer_id") ||
			    !ReadOrTruncated(reader.ReadU8(reserved), reader, error, "reserved") ||
			    !ReadStopReason(reader, payload.reason, error) ||
			    !ReadOrTruncated(reader.ReadU64LE(payload.frame), reader, error, "frame") ||
			    !reader.ReadString(payload.message, NetLockstepCodec::c_MaxDiagnosticBytes, "message", error) ||
			    !ValidatePeerId(payload.senderPeerId, error, "sender_peer_id")) {
				return false;
			}
			if (reserved != 0) {
				SetError(error, NetLockstepErrorCode::ReservedFieldNonZero, 1, "stop reserved byte must be zero");
				return false;
			}
			out = payload;
			return true;
		}

		bool DecodeChecksum(ByteReader& reader, NetLockstepPayload& out, NetLockstepError* error) {
			NetLockstepChecksum payload;
			uint8_t reserved0 = 0;
			uint16_t reserved16 = 0;
			const uint8_t* hashBytes = nullptr;
			if (!ReadOrTruncated(reader.ReadU8(payload.senderPeerId), reader, error, "sender_peer_id") ||
			    !ReadOrTruncated(reader.ReadU8(reserved0), reader, error, "reserved") ||
			    !ReadOrTruncated(reader.ReadU16LE(reserved16), reader, error, "reserved") ||
			    !ReadOrTruncated(reader.ReadU64LE(payload.frame), reader, error, "frame") ||
			    !ReadOrTruncated(reader.ReadBytes(hashBytes, payload.hash.size()), reader, error, "checksum_hash") ||
			    !ValidatePeerId(payload.senderPeerId, error, "sender_peer_id")) {
				return false;
			}
			if (reserved0 != 0 || reserved16 != 0) {
				SetError(error, NetLockstepErrorCode::ReservedFieldNonZero, 1, "checksum reserved fields must be zero");
				return false;
			}
			std::memcpy(payload.hash.data(), hashBytes, payload.hash.size());
			out = payload;
			return true;
		}

		std::string EscapeJson(const std::string& value) {
			std::string escaped;
			escaped.reserve(value.size());
			for (char c : value) {
				switch (c) {
					case '\\': escaped += "\\\\"; break;
					case '"': escaped += "\\\""; break;
					default: escaped += c; break;
				}
			}
			return escaped;
		}
	}

	bool NetLockstepFrame::operator==(const NetLockstepFrame& rhs) const {
		if (senderPeerId != rhs.senderPeerId || targetFrame != rhs.targetFrame || frames.size() != rhs.frames.size() || commands != rhs.commands) {
			return false;
		}
		for (size_t i = 0; i < frames.size(); ++i) {
			if (ControllerFrameCodec::Encode(frames[i]) != ControllerFrameCodec::Encode(rhs.frames[i])) {
				return false;
			}
		}
		return true;
	}

	NetLockstepPacketType NetLockstepCodec::PacketTypeOf(const NetLockstepPayload& payload) {
		return std::visit(Overloaded{
			[](const NetLockstepStart&) { return NetLockstepPacketType::Start; },
			[](const NetLockstepFrame&) { return NetLockstepPacketType::Frame; },
			[](const NetLockstepAck&) { return NetLockstepPacketType::Ack; },
			[](const NetLockstepStop&) { return NetLockstepPacketType::Stop; },
			[](const NetLockstepChecksum&) { return NetLockstepPacketType::Checksum; },
		}, payload);
	}

	const char* NetLockstepCodec::PacketTypeName(NetLockstepPacketType type) {
		switch (type) {
			case NetLockstepPacketType::Start: return "Start";
			case NetLockstepPacketType::Frame: return "Frame";
			case NetLockstepPacketType::Ack: return "Ack";
			case NetLockstepPacketType::Stop: return "Stop";
			case NetLockstepPacketType::Checksum: return "Checksum";
		}
		return "Unknown";
	}

	const char* NetLockstepCodec::StopReasonName(NetLockstepStopReason reason) {
		switch (reason) {
			case NetLockstepStopReason::Complete: return "Complete";
			case NetLockstepStopReason::MissingFrameTimeout: return "MissingFrameTimeout";
			case NetLockstepStopReason::Desync: return "Desync";
			case NetLockstepStopReason::ProtocolError: return "ProtocolError";
			case NetLockstepStopReason::PeerDisconnected: return "PeerDisconnected";
			case NetLockstepStopReason::InternalError: return "InternalError";
		}
		return "Unknown";
	}

	const char* NetLockstepCodec::ErrorCodeName(NetLockstepErrorCode code) {
		switch (code) {
			case NetLockstepErrorCode::None: return "None";
			case NetLockstepErrorCode::NullBuffer: return "NullBuffer";
			case NetLockstepErrorCode::ShortHeader: return "ShortHeader";
			case NetLockstepErrorCode::BadMagic: return "BadMagic";
			case NetLockstepErrorCode::UnsupportedVersion: return "UnsupportedVersion";
			case NetLockstepErrorCode::BadHeaderSize: return "BadHeaderSize";
			case NetLockstepErrorCode::UnknownFlags: return "UnknownFlags";
			case NetLockstepErrorCode::UnknownPacketType: return "UnknownPacketType";
			case NetLockstepErrorCode::ReservedFieldNonZero: return "ReservedFieldNonZero";
			case NetLockstepErrorCode::PayloadLengthMismatch: return "PayloadLengthMismatch";
			case NetLockstepErrorCode::PayloadTooLarge: return "PayloadTooLarge";
			case NetLockstepErrorCode::TruncatedPayload: return "TruncatedPayload";
			case NetLockstepErrorCode::TrailingBytes: return "TrailingBytes";
			case NetLockstepErrorCode::StringTooLong: return "StringTooLong";
			case NetLockstepErrorCode::InvalidString: return "InvalidString";
			case NetLockstepErrorCode::InvalidValue: return "InvalidValue";
			case NetLockstepErrorCode::EncodeFailed: return "EncodeFailed";
		}
		return "Unknown";
	}

	bool NetLockstepCodec::Encode(const NetLockstepPacket& packet, std::vector<uint8_t>& outBytes, NetLockstepError* error) {
		std::vector<uint8_t> payloadBytes;
		const bool payloadOk = std::visit(Overloaded{
			[&](const NetLockstepStart& payload) { return EncodePayload(payload, payloadBytes, error); },
			[&](const NetLockstepFrame& payload) { return EncodePayload(payload, payloadBytes, error); },
			[&](const NetLockstepAck& payload) { return EncodePayload(payload, payloadBytes, error); },
			[&](const NetLockstepStop& payload) { return EncodePayload(payload, payloadBytes, error); },
			[&](const NetLockstepChecksum& payload) { return EncodePayload(payload, payloadBytes, error); },
		}, packet.payload);
		if (!payloadOk) {
			if (error && error->code == NetLockstepErrorCode::None) {
				SetError(error, NetLockstepErrorCode::EncodeFailed, 0, "payload encode failed");
			}
			return false;
		}
		if (payloadBytes.size() > c_MaxPayloadBytes || payloadBytes.size() > std::numeric_limits<uint32_t>::max()) {
			SetError(error, NetLockstepErrorCode::PayloadTooLarge, 0, "payload exceeds max encoded length");
			return false;
		}

		outBytes.clear();
		outBytes.reserve(c_HeaderBytes + payloadBytes.size());
		AppendU32LE(outBytes, c_Magic);
		AppendU16LE(outBytes, c_Version);
		AppendU16LE(outBytes, c_HeaderBytes);
		AppendU16LE(outBytes, static_cast<uint16_t>(PacketTypeOf(packet.payload)));
		AppendU16LE(outBytes, 0);
		AppendU32LE(outBytes, static_cast<uint32_t>(payloadBytes.size()));
		outBytes.insert(outBytes.end(), payloadBytes.begin(), payloadBytes.end());
		return true;
	}

	NetLockstepDecodeResult NetLockstepCodec::Decode(const uint8_t* data, size_t size) {
		if (!data && size > 0) {
			return Fail(NetLockstepErrorCode::NullBuffer, 0, "input buffer is null");
		}
		if (size < c_HeaderBytes) {
			return Fail(NetLockstepErrorCode::ShortHeader, 0, "packet header is truncated");
		}

		ByteReader headerReader(data, c_HeaderBytes);
		uint32_t magic = 0;
		uint16_t version = 0;
		uint16_t headerBytes = 0;
		uint16_t rawPacketType = 0;
		uint16_t flags = 0;
		uint32_t payloadLength = 0;
		headerReader.ReadU32LE(magic);
		headerReader.ReadU16LE(version);
		headerReader.ReadU16LE(headerBytes);
		headerReader.ReadU16LE(rawPacketType);
		headerReader.ReadU16LE(flags);
		headerReader.ReadU32LE(payloadLength);

		if (magic != c_Magic) {
			return Fail(NetLockstepErrorCode::BadMagic, 0, "packet magic mismatch");
		}
		if (version != c_Version) {
			return Fail(NetLockstepErrorCode::UnsupportedVersion, 4, "unsupported lockstep packet version");
		}
		if (headerBytes != c_HeaderBytes) {
			return Fail(NetLockstepErrorCode::BadHeaderSize, 6, "packet header size mismatch");
		}
		if (flags != 0) {
			return Fail(NetLockstepErrorCode::UnknownFlags, 10, "lockstep packet flags must be zero");
		}
		if (payloadLength > c_MaxPayloadBytes) {
			return Fail(NetLockstepErrorCode::PayloadTooLarge, 12, "payload exceeds max encoded length");
		}
		if (size != static_cast<size_t>(c_HeaderBytes) + payloadLength) {
			return Fail(NetLockstepErrorCode::PayloadLengthMismatch, 12, "payload length does not match packet size");
		}

		NetLockstepPacketType packetType;
		switch (static_cast<NetLockstepPacketType>(rawPacketType)) {
			case NetLockstepPacketType::Start:
			case NetLockstepPacketType::Frame:
			case NetLockstepPacketType::Ack:
			case NetLockstepPacketType::Stop:
			case NetLockstepPacketType::Checksum:
				packetType = static_cast<NetLockstepPacketType>(rawPacketType);
				break;
			default:
				return Fail(NetLockstepErrorCode::UnknownPacketType, 8, "unknown lockstep packet type");
		}

		ByteReader payloadReader(data + c_HeaderBytes, payloadLength);
		NetLockstepPayload payload;
		NetLockstepError payloadError;
		bool payloadOk = false;
		switch (packetType) {
			case NetLockstepPacketType::Start:
				payloadOk = DecodeStart(payloadReader, payload, &payloadError);
				break;
			case NetLockstepPacketType::Frame:
				payloadOk = DecodeFrame(payloadReader, payload, &payloadError);
				break;
			case NetLockstepPacketType::Ack:
				payloadOk = DecodeAck(payloadReader, payload, &payloadError);
				break;
			case NetLockstepPacketType::Stop:
				payloadOk = DecodeStop(payloadReader, payload, &payloadError);
				break;
			case NetLockstepPacketType::Checksum:
				payloadOk = DecodeChecksum(payloadReader, payload, &payloadError);
				break;
		}
		if (!payloadOk) {
			NetLockstepDecodeResult result;
			result.error = std::move(payloadError);
			result.error.offset += c_HeaderBytes;
			return result;
		}
		if (!payloadReader.AtEnd()) {
			return Fail(NetLockstepErrorCode::TrailingBytes, c_HeaderBytes + payloadReader.Offset(), "payload has trailing bytes");
		}

		NetLockstepDecodeResult result;
		result.ok = true;
		result.packet.payload = std::move(payload);
		return result;
	}

	NetLockstepDecodeResult NetLockstepCodec::Decode(const std::vector<uint8_t>& bytes) {
		return Decode(bytes.data(), bytes.size());
	}

	bool NetLockstepCoordinator::Start(INetTransport& transport, const NetLockstepConfig& config, std::string* error) {
		if (config.remoteTransportPeerId == c_InvalidNetPeerId) {
			if (error) *error = "remote transport peer id must be valid";
			return false;
		}
		if (config.localPeerId == 0 || config.remotePeerId == 0 || config.localPeerId == config.remotePeerId ||
		    config.peerCount == 0 || config.peerCount > NetLockstepCodec::c_MaxPeerCount ||
		    config.localPeerId > config.peerCount || config.remotePeerId > config.peerCount) {
			if (error) *error = "lockstep peer identity is invalid";
			return false;
		}
		NetLockstepStart start;
		start.sessionId = config.sessionId;
		start.startFrame = config.startFrame;
		start.inputDelayFrames = config.inputDelayFrames;
		start.controllerFrameVersion = ControllerFrame::c_Version;
		start.controllerFrameEncodedSize = static_cast<uint16_t>(ControllerFrame::c_EncodedSize);
		start.localPeerId = config.localPeerId;
		start.peerCount = config.peerCount;
		start.scenario = config.scenario;
		start.ownershipPolicy = config.ownershipPolicy;

		NetLockstepError validateError;
		std::vector<uint8_t> scratch;
		if (!NetLockstepCodec::Encode({start}, scratch, &validateError)) {
			if (error) *error = validateError.message;
			return false;
		}

		m_Transport = &transport;
		m_Config = config;
		m_State = NetLockstepState::WaitingForStart;
		m_RemoteStartReceived = false;
		m_WaitingFrame = std::numeric_limits<uint64_t>::max();
		m_WaitStartMs = 0;
		m_LastStallFrame = UINT64_MAX;
		m_LocalFrames.clear();
		m_RemoteFrames.clear();
		m_LocalCommands.clear();
		m_RemoteCommands.clear();
		m_LocalChecksums.clear();
		m_RemoteChecksums.clear();
		m_ReadyFrames.clear();
		m_Stats = {};
		m_Stats.sessionId = config.sessionId;
		m_Stats.configuredStartFrame = config.startFrame;
		m_Stats.effectiveStartFrame = config.startFrame + config.inputDelayFrames;
		m_Stats.inputDelayFrames = config.inputDelayFrames;
		m_Stats.localPeerId = config.localPeerId;
		m_Stats.remotePeerId = config.remotePeerId;
		m_Stats.nextFrame = m_Stats.effectiveStartFrame;

		if (!SendPacket({start}, NetTransportLane::ControlReliable, error)) {
			return false;
		}
		++m_Stats.startPacketsSent;
		return true;
	}

	bool NetLockstepCoordinator::QueueLocalInput(uint64_t producedFrame, const std::vector<ControllerFrame>& frames, const std::vector<NetGameCommand>& commands, std::string* error) {
		if (m_State != NetLockstepState::Running) {
			if (error) *error = "lockstep coordinator is not running";
			return false;
		}
		const uint64_t targetFrame = producedFrame + m_Config.inputDelayFrames;
		if (targetFrame < m_Stats.nextFrame || m_LocalFrames.find(targetFrame) != m_LocalFrames.end()) {
			if (error) *error = "local input frame is duplicate or already accepted";
			return false;
		}
		NetLockstepError frameError;
		if (!ValidateSortedFrames(frames, &frameError)) {
			if (error) *error = frameError.message;
			return false;
		}

		NetLockstepFrame packet;
		packet.senderPeerId = m_Config.localPeerId;
		packet.targetFrame = targetFrame;
		packet.frames = frames;
		packet.commands = commands;
		for (NetGameCommand& command : packet.commands) {
			command.senderPeerId = m_Config.localPeerId;
		}
		if (!SendPacket({packet}, m_Config.frameLane, error)) {
			return false;
		}
		m_LocalFrames[targetFrame] = frames;
		if (!packet.commands.empty()) {
			m_LocalCommands[targetFrame] = packet.commands;
		}
		++m_Stats.framePacketsSent;
		m_Stats.localControllerFramesSent += frames.size();
		return true;
	}

	bool NetLockstepCoordinator::SubmitLocalChecksum(uint64_t frame, const std::array<uint8_t, 32>& hash, std::string* error) {
		if (m_State != NetLockstepState::Running) {
			return true;
		}
		m_LocalChecksums[frame] = hash;
		NetLockstepChecksum packet;
		packet.senderPeerId = m_Config.localPeerId;
		packet.frame = frame;
		packet.hash = hash;
		if (!SendPacket({packet}, m_Config.frameLane, error)) {
			return false;
		}
		CompareChecksums(frame);
		return true;
	}

	void NetLockstepCoordinator::HandleChecksum(const NetLockstepChecksum& checksum) {
		if (checksum.senderPeerId != m_Config.remotePeerId) {
			return;
		}
		m_RemoteChecksums[checksum.frame] = checksum.hash;
		CompareChecksums(checksum.frame);
	}

	void NetLockstepCoordinator::CompareChecksums(uint64_t frame) {
		const auto localIt = m_LocalChecksums.find(frame);
		const auto remoteIt = m_RemoteChecksums.find(frame);
		if (localIt == m_LocalChecksums.end() || remoteIt == m_RemoteChecksums.end()) {
			return;
		}
		if (localIt->second != remoteIt->second) {
			Fail(NetLockstepStopReason::Desync, frame, "sim state diverged at tick " + std::to_string(frame));
			return;
		}
		// Matched — drop this tick and any older so the maps stay bounded.
		m_LocalChecksums.erase(m_LocalChecksums.begin(), m_LocalChecksums.upper_bound(frame));
		m_RemoteChecksums.erase(m_RemoteChecksums.begin(), m_RemoteChecksums.upper_bound(frame));
	}

	void NetLockstepCoordinator::Tick(uint64_t nowMs) {
		if (!m_Transport || m_State == NetLockstepState::Idle) {
			return;
		}
		for (const NetTransportEvent& event : m_Transport->PollEvents()) {
			HandleEvent(event, nowMs);
		}
		AdvanceReadyFrames(nowMs);
	}

	void NetLockstepCoordinator::Complete(const std::string& message) {
		if (m_State == NetLockstepState::Failed || m_State == NetLockstepState::Stopped || m_State == NetLockstepState::Idle) {
			return;
		}
		if (m_Transport) {
			NetLockstepStop stop;
			stop.senderPeerId = m_Config.localPeerId;
			stop.reason = NetLockstepStopReason::Complete;
			stop.frame = m_Stats.nextFrame;
			stop.message = message;
			std::string ignored;
			(void)SendPacket({stop}, NetTransportLane::ControlReliable, &ignored);
		}
		m_State = NetLockstepState::Stopped;
		m_Stats.timeoutReason = std::string(NetLockstepCodec::StopReasonName(NetLockstepStopReason::Complete)) + ":" + message;
	}

	bool NetLockstepCoordinator::PopReadyFrame(NetLockstepReadyFrame& outFrame) {
		if (m_ReadyFrames.empty()) {
			return false;
		}
		outFrame = std::move(m_ReadyFrames.front());
		m_ReadyFrames.pop_front();
		return true;
	}

	bool NetLockstepCoordinator::IsLocalActor(int64_t actorUniqueID, int actorTeam, bool cpuControlled) const {
		if (m_Config.peerCount == 0 || m_Config.localPeerId == 0) {
			return true;
		}
		if (!m_Config.matchConfig.players.empty()) {
			const uint8_t team = actorTeam < 0 ? 0 : static_cast<uint8_t>(actorTeam);
			const uint8_t ownerPeerId = NetActorOwnership::ResolveOwnerPeer(m_Config.matchConfig, {
				actorUniqueID,
				team,
				cpuControlled,
			});
			return ownerPeerId == m_Config.localPeerId;
		}
		const uint64_t normalized = actorUniqueID < 0 ? static_cast<uint64_t>(-(actorUniqueID + 1)) + 1U : static_cast<uint64_t>(actorUniqueID);
		const uint8_t ownerPeerId = static_cast<uint8_t>((normalized % m_Config.peerCount) + 1U);
		return ownerPeerId == m_Config.localPeerId;
	}

	uint8_t NetLockstepCoordinator::ResolveTeamCommandAuthority(int team) const {
		if (team < 0) {
			return 0;
		}
		return NetActorOwnership::ResolveTeamCommandAuthority(m_Config.matchConfig, static_cast<uint8_t>(team));
	}

	std::string NetLockstepCoordinator::BuildReportJson() const {
		std::ostringstream out;
		out << "{";
		out << "\"state\":\"" << StateName(m_State) << "\",";
		out << "\"session_id\":" << m_Stats.sessionId << ",";
		out << "\"local_peer_id\":" << static_cast<int>(m_Stats.localPeerId) << ",";
		out << "\"remote_peer_id\":" << static_cast<int>(m_Stats.remotePeerId) << ",";
		out << "\"configured_start_frame\":" << m_Stats.configuredStartFrame << ",";
		out << "\"effective_start_frame\":" << m_Stats.effectiveStartFrame << ",";
		out << "\"input_delay_frames\":" << m_Stats.inputDelayFrames << ",";
		out << "\"next_frame\":" << m_Stats.nextFrame << ",";
		out << "\"start_packets_sent\":" << m_Stats.startPacketsSent << ",";
		out << "\"start_packets_received\":" << m_Stats.startPacketsReceived << ",";
		out << "\"frame_packets_sent\":" << m_Stats.framePacketsSent << ",";
		out << "\"frame_packets_received\":" << m_Stats.framePacketsReceived << ",";
		out << "\"ignored_session_packets\":" << m_Stats.ignoredSessionPackets << ",";
		out << "\"local_controller_frames_sent\":" << m_Stats.localControllerFramesSent << ",";
		out << "\"remote_controller_frames_received\":" << m_Stats.remoteControllerFramesReceived << ",";
		out << "\"remote_controller_frames_accepted\":" << m_Stats.remoteControllerFramesAccepted << ",";
		out << "\"frames_accepted\":" << m_Stats.framesAccepted << ",";
		out << "\"duplicate_frames\":" << m_Stats.duplicateFrames << ",";
		out << "\"out_of_order_frames\":" << m_Stats.outOfOrderFrames << ",";
		out << "\"missing_frame_stalls\":" << m_Stats.missingFrameStalls << ",";
		out << "\"timeouts\":" << m_Stats.timeouts << ",";
		out << "\"timeout_reason\":\"" << EscapeJson(m_Stats.timeoutReason) << "\"";
		out << "}";
		return out.str();
	}

	const char* NetLockstepCoordinator::StateName(NetLockstepState state) {
		switch (state) {
			case NetLockstepState::Idle: return "Idle";
			case NetLockstepState::WaitingForStart: return "WaitingForStart";
			case NetLockstepState::Running: return "Running";
			case NetLockstepState::Stopped: return "Stopped";
			case NetLockstepState::Failed: return "Failed";
		}
		return "Unknown";
	}

	bool NetLockstepCoordinator::SendPacket(const NetLockstepPacket& packet, NetTransportLane lane, std::string* error) {
		std::vector<uint8_t> bytes;
		NetLockstepError encodeError;
		if (!NetLockstepCodec::Encode(packet, bytes, &encodeError)) {
			if (error) *error = encodeError.message;
			return false;
		}
		if (!m_Transport->Send(m_Config.remoteTransportPeerId, lane, bytes, error)) {
			return false;
		}
		return true;
	}

	void NetLockstepCoordinator::HandleEvent(const NetTransportEvent& event, uint64_t nowMs) {
		if (m_State == NetLockstepState::Failed || m_State == NetLockstepState::Stopped) {
			return;
		}
		switch (event.type) {
			case NetTransportEventType::PeerConnected:
				break;
			case NetTransportEventType::PeerDisconnected:
				Fail(NetLockstepStopReason::PeerDisconnected, m_Stats.nextFrame, event.reason.empty() ? "peer disconnected" : event.reason);
				break;
			case NetTransportEventType::ConnectionFailed:
			case NetTransportEventType::TransportError:
				Fail(NetLockstepStopReason::InternalError, m_Stats.nextFrame, event.reason.empty() ? "transport error" : event.reason);
				break;
			case NetTransportEventType::PacketReceived: {
				const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(event.bytes);
				if (!decoded.ok) {
					if (decoded.error.code == NetLockstepErrorCode::BadMagic && (NetProtocol::Decode(event.bytes).ok || NetLobbyProtocol::Decode(event.bytes).ok)) {
						++m_Stats.ignoredSessionPackets;
						return;
					}
					Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, decoded.error.message);
					return;
				}
				HandlePacket(decoded.packet, nowMs);
				break;
			}
		}
	}

	void NetLockstepCoordinator::HandlePacket(const NetLockstepPacket& packet, uint64_t nowMs) {
		std::visit(Overloaded{
			[&](const NetLockstepStart& start) { HandleStart(start); },
			[&](const NetLockstepFrame& frame) { HandleFrame(frame, nowMs); },
			[&](const NetLockstepAck&) {},
			[&](const NetLockstepStop& stop) { HandleStop(stop); },
			[&](const NetLockstepChecksum& checksum) { HandleChecksum(checksum); },
		}, packet.payload);
	}

	void NetLockstepCoordinator::HandleStart(const NetLockstepStart& start) {
		++m_Stats.startPacketsReceived;
		if (start.sessionId != m_Config.sessionId ||
		    start.startFrame != m_Config.startFrame ||
		    start.inputDelayFrames != m_Config.inputDelayFrames ||
		    start.controllerFrameVersion != ControllerFrame::c_Version ||
		    start.controllerFrameEncodedSize != ControllerFrame::c_EncodedSize ||
		    start.localPeerId != m_Config.remotePeerId ||
		    start.peerCount != m_Config.peerCount ||
		    start.scenario != m_Config.scenario ||
		    start.ownershipPolicy != m_Config.ownershipPolicy) {
			Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "lockstep start mismatch");
			return;
		}
		m_RemoteStartReceived = true;
		if (m_State == NetLockstepState::WaitingForStart) {
			m_State = NetLockstepState::Running;
			m_WaitingFrame = std::numeric_limits<uint64_t>::max();
		}
	}

	void NetLockstepCoordinator::HandleFrame(const NetLockstepFrame& frame, uint64_t nowMs) {
		++m_Stats.framePacketsReceived;
		if (!m_RemoteStartReceived || frame.senderPeerId != m_Config.remotePeerId) {
			Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "lockstep frame sender mismatch");
			return;
		}
		if (frame.targetFrame < m_Stats.nextFrame || m_RemoteFrames.find(frame.targetFrame) != m_RemoteFrames.end()) {
			++m_Stats.duplicateFrames;
			return;
		}
		if (frame.targetFrame > m_Stats.nextFrame) {
			++m_Stats.outOfOrderFrames;
		}
		m_Stats.remoteControllerFramesReceived += frame.frames.size();
		m_RemoteFrames[frame.targetFrame] = frame.frames;
		if (!frame.commands.empty()) {
			m_RemoteCommands[frame.targetFrame] = frame.commands;
		}
		AdvanceReadyFrames(nowMs);
	}

	void NetLockstepCoordinator::HandleStop(const NetLockstepStop& stop) {
		m_Stats.timeoutReason = std::string(NetLockstepCodec::StopReasonName(stop.reason)) + ":" + stop.message;
		m_State = stop.reason == NetLockstepStopReason::Complete ? NetLockstepState::Stopped : NetLockstepState::Failed;
	}

	void NetLockstepCoordinator::AdvanceReadyFrames(uint64_t nowMs) {
		if (m_State != NetLockstepState::Running) {
			return;
		}
		while (true) {
			const auto localIt = m_LocalFrames.find(m_Stats.nextFrame);
			const auto remoteIt = m_RemoteFrames.find(m_Stats.nextFrame);
			if (localIt == m_LocalFrames.end() || remoteIt == m_RemoteFrames.end()) {
				break;
			}
			NetLockstepReadyFrame ready;
			ready.frame = m_Stats.nextFrame;
			ready.localFrames = std::move(localIt->second);
			ready.remoteFrames = std::move(remoteIt->second);
			if (auto localCmdIt = m_LocalCommands.find(ready.frame); localCmdIt != m_LocalCommands.end()) {
				ready.localCommands = std::move(localCmdIt->second);
				m_LocalCommands.erase(localCmdIt);
			}
			if (auto remoteCmdIt = m_RemoteCommands.find(ready.frame); remoteCmdIt != m_RemoteCommands.end()) {
				ready.remoteCommands = std::move(remoteCmdIt->second);
				m_RemoteCommands.erase(remoteCmdIt);
			}
			m_Stats.remoteControllerFramesAccepted += ready.remoteFrames.size();
			m_LocalFrames.erase(localIt);
			m_RemoteFrames.erase(remoteIt);
			m_ReadyFrames.push_back(std::move(ready));
			++m_Stats.framesAccepted;
			++m_Stats.nextFrame;
			m_WaitingFrame = m_Stats.nextFrame;
			m_WaitStartMs = nowMs;
			m_LastStallFrame = UINT64_MAX;
		}

		const bool hasLocal = m_LocalFrames.find(m_Stats.nextFrame) != m_LocalFrames.end();
		const bool hasRemote = m_RemoteFrames.find(m_Stats.nextFrame) != m_RemoteFrames.end();
		const bool hasFutureLocal = m_LocalFrames.upper_bound(m_Stats.nextFrame) != m_LocalFrames.end();
		const bool hasFutureRemote = m_RemoteFrames.upper_bound(m_Stats.nextFrame) != m_RemoteFrames.end();
		const bool pending = hasLocal || hasRemote || hasFutureLocal || hasFutureRemote;
		if (!pending) {
			return;
		}
		if (m_WaitingFrame != m_Stats.nextFrame) {
			m_WaitingFrame = m_Stats.nextFrame;
			m_WaitStartMs = nowMs;
		}
		if (m_LastStallFrame != m_Stats.nextFrame) {
			++m_Stats.missingFrameStalls;
			m_LastStallFrame = m_Stats.nextFrame;
		}
		if (m_Config.timeoutMs > 0 && nowMs >= m_WaitStartMs && nowMs - m_WaitStartMs >= m_Config.timeoutMs) {
			Fail(NetLockstepStopReason::MissingFrameTimeout, m_Stats.nextFrame, "missing lockstep frame");
		}
	}

	void NetLockstepCoordinator::Fail(NetLockstepStopReason reason, uint64_t frame, const std::string& message) {
		if (m_State == NetLockstepState::Failed) {
			return;
		}
		m_State = NetLockstepState::Failed;
		++m_Stats.timeouts;
		m_Stats.timeoutReason = std::string(NetLockstepCodec::StopReasonName(reason)) + ":" + message;
		if (m_Transport) {
			NetLockstepStop stop;
			stop.senderPeerId = m_Config.localPeerId;
			stop.reason = reason;
			stop.frame = frame;
			stop.message = message;
			std::string ignored;
			(void)SendPacket({stop}, NetTransportLane::ControlReliable, &ignored);
		}
	}

} // namespace RTE
