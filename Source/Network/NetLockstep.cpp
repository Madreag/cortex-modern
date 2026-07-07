#include "NetLockstep.h"

#include "NetActorOwnership.h"
#include "NetLobbyProtocol.h"
#include "NetProtocol.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <iterator>
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
				case NetLockstepStopReason::PeerLeft:
				case NetLockstepStopReason::ResyncRequested:
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
						AppendU32LE(out, static_cast<uint32_t>(spawn.aiMode));
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
						AppendU8(out, delivery.queuedPurchase ? 1 : 0);
						AppendU32LE(out, FloatToBitsLE(delivery.cost));
						AppendU8(out, delivery.returnCraft ? 1 : 0);
						AppendU32LE(out, static_cast<uint32_t>(delivery.passengerAIMode));
						AppendU32LE(out, FloatToBitsLE(delivery.waypointX));
						AppendU32LE(out, FloatToBitsLE(delivery.waypointY));
						AppendU64LE(out, static_cast<uint64_t>(delivery.targetUID));
						AppendU8(out, static_cast<uint8_t>(delivery.orderedByPlayer));
						AppendU32LE(out, FloatToBitsLE(delivery.multiOrderYOffset));
						break;
					}
					case NetGameCommandType::ScuttleCraft: {
						const NetGameScuttleCraft& scuttle = std::get<NetGameScuttleCraft>(command.payload);
						AppendU64LE(out, static_cast<uint64_t>(scuttle.actorUID));
						AppendU32LE(out, static_cast<uint32_t>(scuttle.team));
						break;
					}
					case NetGameCommandType::SetActorAIMode: {
						const NetGameSetActorAIMode& setMode = std::get<NetGameSetActorAIMode>(command.payload);
						AppendU64LE(out, static_cast<uint64_t>(setMode.actorUID));
						AppendU32LE(out, static_cast<uint32_t>(setMode.team));
						AppendU8(out, setMode.aiMode);
						break;
					}
					case NetGameCommandType::SwitchControl: {
						const NetGameSwitchControl& switchControl = std::get<NetGameSwitchControl>(command.payload);
						AppendU64LE(out, static_cast<uint64_t>(switchControl.actorUID));
						AppendU32LE(out, static_cast<uint32_t>(switchControl.team));
						AppendU8(out, switchControl.newOwnerPeerId);
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
					case NetGameCommandType::PauseMatch: {
						const NetGamePauseMatch& pauseMatch = std::get<NetGamePauseMatch>(command.payload);
						AppendU32LE(out, static_cast<uint32_t>(pauseMatch.team));
						AppendU8(out, pauseMatch.pause ? 1 : 0);
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
						uint32_t aiMode = 0;
						if (!reader.ReadString(spawn.className, NetLockstepCodec::c_MaxScenarioBytes, "spawn_class_name", error) ||
						    !reader.ReadString(spawn.preset, NetLockstepCodec::c_MaxScenarioBytes, "spawn_preset", error) ||
						    !reader.ReadString(spawn.module, NetLockstepCodec::c_MaxScenarioBytes, "spawn_module", error) ||
						    !ReadOrTruncated(reader.ReadU32LE(posXBits), reader, error, "spawn_pos_x") ||
						    !ReadOrTruncated(reader.ReadU32LE(posYBits), reader, error, "spawn_pos_y") ||
						    !ReadOrTruncated(reader.ReadU32LE(team), reader, error, "spawn_team") ||
						    !ReadOrTruncated(reader.ReadU32LE(aiMode), reader, error, "spawn_ai_mode")) {
							return false;
						}
						spawn.posX = FloatFromBitsLE(posXBits);
						spawn.posY = FloatFromBitsLE(posYBits);
						spawn.team = static_cast<int32_t>(team);
						spawn.aiMode = static_cast<int32_t>(aiMode);
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
						uint8_t queuedPurchase = 0;
						uint32_t costBits = 0;
						uint8_t returnCraft = 0;
						uint32_t passengerAIMode = 0;
						uint32_t waypointXBits = 0;
						uint32_t waypointYBits = 0;
						uint64_t targetUID = 0;
						uint8_t orderedByPlayer = 0;
						uint32_t multiOrderYOffsetBits = 0;
						if (!ReadOrTruncated(reader.ReadU8(queuedPurchase), reader, error, "delivery_queued_purchase") ||
						    !ReadOrTruncated(reader.ReadU32LE(costBits), reader, error, "delivery_cost") ||
						    !ReadOrTruncated(reader.ReadU8(returnCraft), reader, error, "delivery_return_craft") ||
						    !ReadOrTruncated(reader.ReadU32LE(passengerAIMode), reader, error, "delivery_passenger_ai_mode") ||
						    !ReadOrTruncated(reader.ReadU32LE(waypointXBits), reader, error, "delivery_waypoint_x") ||
						    !ReadOrTruncated(reader.ReadU32LE(waypointYBits), reader, error, "delivery_waypoint_y") ||
						    !ReadOrTruncated(reader.ReadU64LE(targetUID), reader, error, "delivery_target_uid") ||
						    !ReadOrTruncated(reader.ReadU8(orderedByPlayer), reader, error, "delivery_ordered_by_player") ||
						    !ReadOrTruncated(reader.ReadU32LE(multiOrderYOffsetBits), reader, error, "delivery_multi_order_y_offset")) {
							return false;
						}
						delivery.posX = FloatFromBitsLE(posXBits);
						delivery.posY = FloatFromBitsLE(posYBits);
						delivery.team = static_cast<int32_t>(team);
						delivery.queuedPurchase = queuedPurchase != 0;
						delivery.cost = FloatFromBitsLE(costBits);
						delivery.returnCraft = returnCraft != 0;
						delivery.passengerAIMode = static_cast<int32_t>(passengerAIMode);
						delivery.waypointX = FloatFromBitsLE(waypointXBits);
						delivery.waypointY = FloatFromBitsLE(waypointYBits);
						delivery.targetUID = static_cast<int64_t>(targetUID);
						delivery.orderedByPlayer = static_cast<int8_t>(orderedByPlayer);
						delivery.multiOrderYOffset = FloatFromBitsLE(multiOrderYOffsetBits);
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
					case NetGameCommandType::SetActorAIMode: {
						NetGameSetActorAIMode setMode;
						uint64_t actorUID = 0;
						uint32_t team = 0;
						if (!ReadOrTruncated(reader.ReadU64LE(actorUID), reader, error, "set_ai_mode_actor_uid") ||
						    !ReadOrTruncated(reader.ReadU32LE(team), reader, error, "set_ai_mode_team") ||
						    !ReadOrTruncated(reader.ReadU8(setMode.aiMode), reader, error, "set_ai_mode_mode")) {
							return false;
						}
						setMode.actorUID = static_cast<int64_t>(actorUID);
						setMode.team = static_cast<int32_t>(team);
						command.payload = setMode;
						break;
					}
					case NetGameCommandType::SwitchControl: {
						NetGameSwitchControl switchControl;
						uint64_t actorUID = 0;
						uint32_t team = 0;
						if (!ReadOrTruncated(reader.ReadU64LE(actorUID), reader, error, "switch_control_actor_uid") ||
						    !ReadOrTruncated(reader.ReadU32LE(team), reader, error, "switch_control_team") ||
						    !ReadOrTruncated(reader.ReadU8(switchControl.newOwnerPeerId), reader, error, "switch_control_new_owner")) {
							return false;
						}
						switchControl.actorUID = static_cast<int64_t>(actorUID);
						switchControl.team = static_cast<int32_t>(team);
						command.payload = switchControl;
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
					case NetGameCommandType::PauseMatch: {
						NetGamePauseMatch pauseMatch;
						uint32_t team = 0;
						uint8_t pause = 0;
						if (!ReadOrTruncated(reader.ReadU32LE(team), reader, error, "pause_team") ||
						    !ReadOrTruncated(reader.ReadU8(pause), reader, error, "pause_flag")) {
							return false;
						}
						pauseMatch.team = static_cast<int32_t>(team);
						pauseMatch.pause = pause != 0;
						command.payload = pauseMatch;
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
			case NetLockstepStopReason::PeerLeft: return "PeerLeft";
			case NetLockstepStopReason::ResyncRequested: return "ResyncRequested";
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
		if (config.peerCount < 2 || config.peerCount > NetLockstepCodec::c_MaxPeerCount ||
		    config.localPeerId == 0 || config.localPeerId > config.peerCount) {
			if (error) *error = "lockstep peer identity is invalid";
			return false;
		}

		// The RECEIVE set is every peer except local — peerIds are 1..peerCount per the match config.
		// The SEND routing is separate (host-star): the host sends directly to every client, but a
		// client sends only to the host, which relays. So derive the receive set from peerCount, and
		// take the send targets from the explicit transport map (or the 2-peer single-remote fields).
		std::vector<uint8_t> remotePeerIds;
		for (uint8_t peerId = 1; peerId <= config.peerCount; ++peerId) {
			if (peerId != config.localPeerId) {
				remotePeerIds.push_back(peerId);
			}
		}
		std::map<uint8_t, NetPeerId> remoteTransports;
		if (!config.remoteTransportPeerIds.empty()) {
			for (const auto& [peerId, transportId]: config.remoteTransportPeerIds) {
				if (peerId == 0 || peerId == config.localPeerId || peerId > config.peerCount || transportId == c_InvalidNetPeerId) {
					if (error) *error = "lockstep remote transport map is invalid";
					return false;
				}
				remoteTransports[peerId] = transportId;
			}
		} else {
			if (config.remotePeerId == 0 || config.remotePeerId == config.localPeerId ||
			    config.remotePeerId > config.peerCount || config.remoteTransportPeerId == c_InvalidNetPeerId) {
				if (error) *error = "lockstep peer identity is invalid";
				return false;
			}
			remoteTransports[config.remotePeerId] = config.remoteTransportPeerId;
		}
		if (remoteTransports.empty()) {
			if (error) *error = "lockstep has no remote transport targets";
			return false;
		}
		// A relay host forwards between clients, so it must reach every remote directly.
		if (config.relayToOtherPeers) {
			for (uint8_t peerId : remotePeerIds) {
				if (remoteTransports.find(peerId) == remoteTransports.end()) {
					if (error) *error = "lockstep relay host is missing a transport for peer " + std::to_string(peerId);
					return false;
				}
			}
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
		m_RemotePeerIds = std::move(remotePeerIds);
		m_RemoteTransports = std::move(remoteTransports);
		m_RelayHost = config.relayToOtherPeers;
		m_State = NetLockstepState::WaitingForStart;
		m_RemoteStartsReceived.clear();
		m_PeerLeaveFrames.clear();
		m_LastQueuedTargetFrame = std::numeric_limits<uint64_t>::max();
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
			// Carry the stop reason so the caller can route it (a resync request must not read as a
			// generic failure).
			if (error) *error = m_Stats.timeoutReason.empty() ? "lockstep coordinator is not running" : m_Stats.timeoutReason;
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
		if (m_LastQueuedTargetFrame == std::numeric_limits<uint64_t>::max() || targetFrame > m_LastQueuedTargetFrame) {
			m_LastQueuedTargetFrame = targetFrame;
		}
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
		if (!IsKnownRemotePeer(checksum.senderPeerId)) {
			return;
		}
		m_RemoteChecksums[checksum.frame][checksum.senderPeerId] = checksum.hash;
		RelayToOtherRemotes({checksum}, checksum.senderPeerId);
		CompareChecksums(checksum.frame);
	}

	void NetLockstepCoordinator::CompareChecksums(uint64_t frame) {
		const auto localIt = m_LocalChecksums.find(frame);
		const auto remoteIt = m_RemoteChecksums.find(frame);
		if (localIt == m_LocalChecksums.end() || remoteIt == m_RemoteChecksums.end()) {
			return;
		}
		// A desync on ANY peer aborts, naming it; only verify (and prune) once every REQUIRED remote
		// agrees. A cleanly-left peer's last hashes still compare, but nobody waits on it.
		for (const auto& [peerId, hash]: remoteIt->second) {
			if (localIt->second != hash) {
				Fail(NetLockstepStopReason::Desync, frame, "sim state diverged at tick " + std::to_string(frame) + " (" + DescribePeer(peerId) + ")");
				return;
			}
		}
		for (uint8_t peerId: m_RemotePeerIds) {
			if (IsRemoteRequiredForFrame(peerId, frame) && remoteIt->second.find(peerId) == remoteIt->second.end()) {
				return;
			}
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

	void NetLockstepCoordinator::Leave(const std::string& message) {
		if (m_State == NetLockstepState::Failed || m_State == NetLockstepState::Stopped || m_State == NetLockstepState::Idle) {
			return;
		}
		// The relay host is the star's hub — without it the survivors cannot exchange frames.
		if (m_RelayHost && m_RemotePeerIds.size() > 1) {
			Complete(message);
			return;
		}
		if (m_Transport) {
			NetLockstepStop stop;
			stop.senderPeerId = m_Config.localPeerId;
			stop.reason = NetLockstepStopReason::PeerLeft;
			// The first frame WITHOUT our data: peers advance freely from here; zero when we never produced.
			stop.frame = m_LastQueuedTargetFrame == std::numeric_limits<uint64_t>::max() ? 0 : m_LastQueuedTargetFrame + 1;
			stop.message = message;
			std::string ignored;
			(void)SendPacket({stop}, NetTransportLane::ControlReliable, &ignored);
		}
		m_State = NetLockstepState::Stopped;
		m_Stats.timeoutReason = std::string(NetLockstepCodec::StopReasonName(NetLockstepStopReason::PeerLeft)) + ":" + message;
	}

	void NetLockstepCoordinator::RequestResync(const std::string& message) {
		if (m_State == NetLockstepState::Failed || m_State == NetLockstepState::Stopped || m_State == NetLockstepState::Idle) {
			return;
		}
		if (m_Transport) {
			NetLockstepStop stop;
			stop.senderPeerId = m_Config.localPeerId;
			stop.reason = NetLockstepStopReason::ResyncRequested;
			stop.frame = m_Stats.nextFrame;
			stop.message = message;
			std::string ignored;
			(void)SendPacket({stop}, NetTransportLane::ControlReliable, &ignored);
		}
		m_State = NetLockstepState::Failed;
		m_Stats.timeoutReason = std::string(NetLockstepCodec::StopReasonName(NetLockstepStopReason::ResyncRequested)) + ":" + message;
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
			uint8_t ownerPeerId = NetActorOwnership::ResolveOwnerPeer(m_Config.matchConfig, {
				actorUniqueID,
				team,
				cpuControlled,
			});
			// A leaver's team falls to its next surviving human peer, so the units play on. The
			// lockstep gate synchronizes leave knowledge, so every peer re-resolves identically.
			if (m_PeerLeaveFrames.find(ownerPeerId) != m_PeerLeaveFrames.end()) {
				ownerPeerId = FirstAliveHumanPeerForTeam(team, std::numeric_limits<uint64_t>::max());
			}
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

	bool NetLockstepCoordinator::IsActorOwnerGone(int64_t actorUniqueID, int actorTeam, bool cpuControlled, uint64_t frame) const {
		if (m_PeerLeaveFrames.empty() || m_Config.matchConfig.players.empty()) {
			return false;
		}
		const uint8_t team = actorTeam < 0 ? 0 : static_cast<uint8_t>(actorTeam);
		if (!IsPeerGoneAtFrame(NetActorOwnership::ResolveOwnerPeer(m_Config.matchConfig, {actorUniqueID, team, cpuControlled}), frame)) {
			return false;
		}
		// The team's units fall to the next surviving human peer; only an ownerless team stands down.
		return FirstAliveHumanPeerForTeam(team, frame) == 0;
	}

	bool NetLockstepCoordinator::IsPeerGoneAtFrame(uint8_t peerId, uint64_t frame) const {
		if (peerId == m_Config.localPeerId) {
			return false;
		}
		const auto leaveIt = m_PeerLeaveFrames.find(peerId);
		return leaveIt != m_PeerLeaveFrames.end() && frame >= leaveIt->second;
	}

	uint8_t NetLockstepCoordinator::FirstAliveHumanPeerForTeam(uint8_t team, uint64_t frame) const {
		for (const NetMatchPlayerSlot& slot: m_Config.matchConfig.players) {
			if (slot.cpu || slot.team != team || slot.peerId == 0 || slot.peerId > m_Config.peerCount) {
				continue;
			}
			if (!IsPeerGoneAtFrame(slot.peerId, frame)) {
				return slot.peerId;
			}
		}
		return 0;
	}

	std::string NetLockstepCoordinator::DescribePeer(uint8_t peerId) const {
		for (const NetMatchPlayerSlot& slot: m_Config.matchConfig.players) {
			if (slot.peerId == peerId && !slot.displayName.empty()) {
				return slot.displayName;
			}
		}
		return "peer " + std::to_string(peerId);
	}

	std::string NetLockstepCoordinator::DescribeMissingPeers() const {
		if (m_State != NetLockstepState::Running) {
			return "";
		}
		const auto remoteIt = m_RemoteFrames.find(m_Stats.nextFrame);
		std::string missing;
		for (uint8_t peerId: m_RemotePeerIds) {
			if (!IsRemoteRequiredForFrame(peerId, m_Stats.nextFrame)) {
				continue;
			}
			if (remoteIt != m_RemoteFrames.end() && remoteIt->second.find(peerId) != remoteIt->second.end()) {
				continue;
			}
			if (!missing.empty()) {
				missing += ", ";
			}
			missing += DescribePeer(peerId);
		}
		return missing;
	}

	bool NetLockstepCoordinator::IsRemoteRequiredForFrame(uint8_t peerId, uint64_t frame) const {
		const auto leaveIt = m_PeerLeaveFrames.find(peerId);
		return leaveIt == m_PeerLeaveFrames.end() || frame < leaveIt->second;
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
		out << "\"peers_left\":" << m_PeerLeaveFrames.size() << ",";
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
		// Send to every remote peer's transport (a set of one in the 2-peer case).
		for (const auto& [peerId, transportId]: m_RemoteTransports) {
			if (!m_Transport->Send(transportId, lane, bytes, error)) {
				return false;
			}
		}
		return true;
	}

	bool NetLockstepCoordinator::IsKnownRemotePeer(uint8_t peerId) const {
		return std::find(m_RemotePeerIds.begin(), m_RemotePeerIds.end(), peerId) != m_RemotePeerIds.end();
	}

	bool NetLockstepCoordinator::UsesTransportPeer(NetPeerId transportPeerId) const {
		for (const auto& [peerId, transportId]: m_RemoteTransports) {
			if (transportId == transportPeerId) {
				return true;
			}
		}
		return false;
	}

	// Host-star: forward a packet received from one remote to the OTHER remotes, preserving its
	// sender id, so every peer sees every peer's frames without a client<->client mesh.
	void NetLockstepCoordinator::RelayToOtherRemotes(const NetLockstepPacket& packet, uint8_t fromPeerId) {
		if (!m_RelayHost) {
			return;
		}
		std::vector<uint8_t> bytes;
		if (!NetLockstepCodec::Encode(packet, bytes)) {
			return;
		}
		for (const auto& [peerId, transportId]: m_RemoteTransports) {
			if (peerId != fromPeerId) {
				std::string ignored;
				(void)m_Transport->Send(transportId, m_Config.frameLane, bytes, &ignored);
			}
		}
	}

	void NetLockstepCoordinator::HandleEvent(const NetTransportEvent& event, uint64_t nowMs) {
		if (m_State == NetLockstepState::Failed || m_State == NetLockstepState::Stopped) {
			return;
		}
		switch (event.type) {
			case NetTransportEventType::PeerConnected:
				// A NEW transport connection mid-match is a reconnecting peer knocking; the session
				// (via the sink) runs its handshake while the round keeps playing.
				if (m_SessionEventSink) {
					m_SessionEventSink(event);
				}
				break;
			case NetTransportEventType::PeerDisconnected: {
				uint8_t lockstepPeer = 0;
				for (const auto& [peerId, transportId]: m_RemoteTransports) {
					if (transportId == event.peerId) {
						lockstepPeer = peerId;
						break;
					}
				}
				// The session tracks the same lifecycles for the eventual rematch/rejoin bookkeeping.
				if (m_SessionEventSink) {
					m_SessionEventSink(event);
				}
				// A cleanly-left peer's socket closing behind its notice is expected.
				if (lockstepPeer != 0 && m_PeerLeaveFrames.find(lockstepPeer) != m_PeerLeaveFrames.end()) {
					m_RemoteTransports.erase(lockstepPeer);
					break;
				}
				// The relay host adjudicates a client drop as a leave at the first frame it has no data
				// for, so the survivors keep playing; a host drop still ends the match. The relayed
				// frames precede this notice on the reliable lane, so no survivor learns of the leave
				// before it holds everything the leave references.
				if (m_RelayHost && lockstepPeer != 0 && m_State == NetLockstepState::Running && m_Stats.nextFrame > 0) {
					uint64_t firstMissingFrame = m_Stats.nextFrame;
					while (true) {
						const auto it = m_RemoteFrames.find(firstMissingFrame);
						if (it == m_RemoteFrames.end() || it->second.find(lockstepPeer) == it->second.end()) {
							break;
						}
						++firstMissingFrame;
					}
					ApplyPeerLeave(lockstepPeer, firstMissingFrame, "connection lost", nowMs);
					break;
				}
				Fail(NetLockstepStopReason::PeerDisconnected,
				     m_Stats.nextFrame,
				     (lockstepPeer != 0 ? DescribePeer(lockstepPeer) : std::string("peer")) + " disconnected" + (event.reason.empty() ? "" : ": " + event.reason));
				break;
			}
			case NetTransportEventType::ConnectionFailed:
			case NetTransportEventType::TransportError:
				Fail(NetLockstepStopReason::InternalError, m_Stats.nextFrame, event.reason.empty() ? "transport error" : event.reason);
				break;
			case NetTransportEventType::PacketReceived: {
				const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(event.bytes);
				if (!decoded.ok) {
					if (decoded.error.code == NetLockstepErrorCode::BadMagic && NetProtocol::Decode(event.bytes).ok) {
						// Session-protocol traffic mid-match is a reconnect handshake; hand it over.
						if (m_SessionEventSink) {
							m_SessionEventSink(event);
						} else {
							++m_Stats.ignoredSessionPackets;
						}
						return;
					}
					if (decoded.error.code == NetLockstepErrorCode::BadMagic && NetLobbyProtocol::Decode(event.bytes).ok) {
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
			[&](const NetLockstepStop& stop) { HandleStop(stop, nowMs); },
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
		    !IsKnownRemotePeer(start.localPeerId) ||
		    start.peerCount != m_Config.peerCount ||
		    start.scenario != m_Config.scenario ||
		    start.ownershipPolicy != m_Config.ownershipPolicy) {
			Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "lockstep start mismatch");
			return;
		}
		const bool firstFromThisPeer = m_RemoteStartsReceived.insert(start.localPeerId).second;
		if (firstFromThisPeer) {
			RelayToOtherRemotes({start}, start.localPeerId);
		}
		if (m_State == NetLockstepState::WaitingForStart && AllRemoteStartsReceived()) {
			m_State = NetLockstepState::Running;
			m_WaitingFrame = std::numeric_limits<uint64_t>::max();
		}
	}

	void NetLockstepCoordinator::HandleFrame(const NetLockstepFrame& frame, uint64_t nowMs) {
		++m_Stats.framePacketsReceived;
		if (m_RemoteStartsReceived.find(frame.senderPeerId) == m_RemoteStartsReceived.end() || !IsKnownRemotePeer(frame.senderPeerId)) {
			Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "lockstep frame sender mismatch");
			return;
		}
		// Check staleness before touching the map, or a stale packet leaks an empty bucket forever.
		if (frame.targetFrame < m_Stats.nextFrame) {
			++m_Stats.duplicateFrames;
			return;
		}
		auto& peerFrames = m_RemoteFrames[frame.targetFrame];
		if (peerFrames.find(frame.senderPeerId) != peerFrames.end()) {
			++m_Stats.duplicateFrames;
			return;
		}
		if (frame.targetFrame > m_Stats.nextFrame) {
			++m_Stats.outOfOrderFrames;
		}
		m_Stats.remoteControllerFramesReceived += frame.frames.size();
		peerFrames[frame.senderPeerId] = frame.frames;
		if (!frame.commands.empty()) {
			m_RemoteCommands[frame.targetFrame][frame.senderPeerId] = frame.commands;
		}
		RelayToOtherRemotes({frame}, frame.senderPeerId);
		AdvanceReadyFrames(nowMs);
	}

	void NetLockstepCoordinator::HandleStop(const NetLockstepStop& stop, uint64_t nowMs) {
		if (stop.reason == NetLockstepStopReason::PeerLeft) {
			if (IsKnownRemotePeer(stop.senderPeerId)) {
				ApplyPeerLeave(stop.senderPeerId, stop.frame, stop.message, nowMs);
			}
			return;
		}
		m_Stats.timeoutReason = std::string(NetLockstepCodec::StopReasonName(stop.reason)) + ":" + stop.message;
		m_State = stop.reason == NetLockstepStopReason::Complete ? NetLockstepState::Stopped : NetLockstepState::Failed;
	}

	// A leave is deterministic by construction: no survivor can advance to the leaver's first missing
	// frame without processing this, so every peer drops the requirement at the same tick.
	void NetLockstepCoordinator::ApplyPeerLeave(uint8_t peerId, uint64_t firstFrameWithout, const std::string& message, uint64_t nowMs) {
		if (!m_PeerLeaveFrames.emplace(peerId, firstFrameWithout).second) {
			return;
		}
		std::cout << "[net-match] " << DescribePeer(peerId) << " left the match at frame " << firstFrameWithout << " (" << message << ")" << std::endl;
		NetLockstepStop notice;
		notice.senderPeerId = peerId;
		notice.reason = NetLockstepStopReason::PeerLeft;
		notice.frame = firstFrameWithout;
		notice.message = message;
		RelayToOtherRemotes({notice}, peerId);
		m_RemoteTransports.erase(peerId);
		if (m_PeerLeaveFrames.size() >= m_RemotePeerIds.size()) {
			// Nobody left to play with.
			m_Stats.timeoutReason = std::string(NetLockstepCodec::StopReasonName(NetLockstepStopReason::PeerLeft)) + ":" + message;
			m_State = NetLockstepState::Stopped;
			return;
		}
		AdvanceReadyFrames(nowMs);
	}

	void NetLockstepCoordinator::AdvanceReadyFrames(uint64_t nowMs) {
		if (m_State != NetLockstepState::Running) {
			return;
		}
		while (true) {
			const auto localIt = m_LocalFrames.find(m_Stats.nextFrame);
			if (localIt == m_LocalFrames.end()) {
				break;
			}
			// Advance only when every REQUIRED remote's frame is in — a cleanly-left peer stops being
			// required past its announced last frame.
			auto remoteIt = m_RemoteFrames.find(m_Stats.nextFrame);
			bool allRequiredIn = true;
			for (uint8_t peerId: m_RemotePeerIds) {
				if (!IsRemoteRequiredForFrame(peerId, m_Stats.nextFrame)) {
					continue;
				}
				if (remoteIt == m_RemoteFrames.end() || remoteIt->second.find(peerId) == remoteIt->second.end()) {
					allRequiredIn = false;
					break;
				}
			}
			if (!allRequiredIn) {
				break;
			}
			NetLockstepReadyFrame ready;
			ready.frame = m_Stats.nextFrame;
			ready.localFrames = std::move(localIt->second);
			// Merge every remote peer's frames in ascending peerId order (std::map iteration) so every
			// peer builds the byte-identical apply set. This is the one N-peer determinism-sensitive spot.
			if (remoteIt != m_RemoteFrames.end()) {
				for (auto& [peerId, frames]: remoteIt->second) {
					ready.remoteFrames.insert(ready.remoteFrames.end(), std::make_move_iterator(frames.begin()), std::make_move_iterator(frames.end()));
				}
			}
			if (auto localCmdIt = m_LocalCommands.find(ready.frame); localCmdIt != m_LocalCommands.end()) {
				ready.localCommands = std::move(localCmdIt->second);
				m_LocalCommands.erase(localCmdIt);
			}
			if (auto remoteCmdIt = m_RemoteCommands.find(ready.frame); remoteCmdIt != m_RemoteCommands.end()) {
				for (auto& [peerId, cmds]: remoteCmdIt->second) {
					ready.remoteCommands.insert(ready.remoteCommands.end(), std::make_move_iterator(cmds.begin()), std::make_move_iterator(cmds.end()));
				}
				m_RemoteCommands.erase(remoteCmdIt);
			}
			m_Stats.remoteControllerFramesAccepted += ready.remoteFrames.size();
			m_LocalFrames.erase(localIt);
			if (remoteIt != m_RemoteFrames.end()) {
				m_RemoteFrames.erase(remoteIt);
			}
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
			const std::string missing = DescribeMissingPeers();
			Fail(NetLockstepStopReason::MissingFrameTimeout, m_Stats.nextFrame, missing.empty() ? "missing lockstep frame" : "missing lockstep frame from " + missing);
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
