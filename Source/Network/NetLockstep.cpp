#include "NetLockstep.h"

#include "NetActorOwnership.h"
#include "NetLobbyProtocol.h"
#include "NetProtocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace RTE {

	uint64_t NetLockstepNowMs() {
		static const std::chrono::steady_clock::time_point base = std::chrono::steady_clock::now();
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - base).count());
	}

	namespace {
		constexpr uint64_t c_StartRetransmitMs = 250;

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

		// LEB128, so a small key field costs one byte and a hash costs ten.
		void AppendVarU64(std::vector<uint8_t>& out, uint64_t value) {
			while (value >= 0x80U) {
				out.push_back(static_cast<uint8_t>(value) | 0x80U);
				value >>= 7;
			}
			out.push_back(static_cast<uint8_t>(value));
		}

		size_t VarU64Size(uint64_t value) {
			size_t bytes = 1;
			while (value >= 0x80U) {
				value >>= 7;
				++bytes;
			}
			return bytes;
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

			enum class VarStatus { Ok, Truncated, Malformed };

			VarStatus ReadVarU64(uint64_t& out) {
				uint64_t value = 0;
				for (int shift = 0; shift <= 63; shift += 7) {
					if (!CanRead(1)) {
						return VarStatus::Truncated;
					}
					const uint8_t byte = m_Data[m_Offset++];
					if (shift == 63 && (byte & 0xFEU) != 0) {
						return VarStatus::Malformed;
					}
					value |= static_cast<uint64_t>(byte & 0x7FU) << shift;
					if ((byte & 0x80U) == 0) {
						// One value, one encoding, so two peers cannot spell the same packet differently.
						if (shift > 0 && byte == 0) {
							return VarStatus::Malformed;
						}
						out = value;
						return VarStatus::Ok;
					}
				}
				return VarStatus::Malformed;
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

		bool ReadVarOrFail(ByteReader& reader, uint64_t& out, NetLockstepError* error, const char* fieldName) {
			switch (reader.ReadVarU64(out)) {
				case ByteReader::VarStatus::Ok:
					return true;
				case ByteReader::VarStatus::Truncated:
					SetError(error, NetLockstepErrorCode::TruncatedPayload, reader.Offset(), std::string(fieldName) + " is truncated");
					return false;
				case ByteReader::VarStatus::Malformed:
					break;
			}
			SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), std::string(fieldName) + " is not a canonical varint");
			return false;
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
			AppendU64LE(out, payload.roundId);
			return AppendString(out, payload.scenario, NetLockstepCodec::c_MaxScenarioBytes, "scenario", error) &&
			       AppendString(out, payload.ownershipPolicy, NetLockstepCodec::c_MaxOwnershipPolicyBytes, "ownership_policy", error);
		}

		// One observation is a slot number and a reading; the first time a sender sends a key it spells it
		// out as well, and a field the previous spelled-out key already had costs nothing. Whatever does
		// not fit the byte budget is left for the caller to carry, and the dictionary is only touched for
		// observations that actually go out, so a refused packet never leaves the two tables disagreeing.
		bool AppendObservations(const std::vector<NetSoundObservation>& observations, std::vector<uint8_t>& out, NetSoundObservationDictionary* dictionary, size_t* outEncoded, NetLockstepError* error) {
			if (observations.size() > NetLockstepCodec::c_MaxObservationsPerPacket) {
				SetError(error, NetLockstepErrorCode::PayloadTooLarge, out.size(), "frame packet has too many sound observations");
				return false;
			}
			for (const NetSoundObservation& observation : observations) {
				if (!std::isfinite(observation.value)) {
					SetError(error, NetLockstepErrorCode::InvalidValue, out.size(), "sound observation value is not finite");
					return false;
				}
			}
			// What the sender had spelled out before this packet. A receiver that missed a packet carrying
			// a binding sees the gap here, on the very next packet, whether or not that one binds anything.
			AppendVarU64(out, dictionary ? dictionary->BindingCount() : 0);
			const size_t countOffset = out.size();
			AppendU16LE(out, 0);
			const size_t blockStart = out.size();
			const size_t budget = std::min(NetLockstepCodec::c_MaxObservationBytesPerPacket,
			                               out.size() < NetLockstepCodec::c_MaxPayloadBytes ? NetLockstepCodec::c_MaxPayloadBytes - out.size() : size_t{0});
			NetSoundObservationKey previous;
			size_t encoded = 0;
			for (const NetSoundObservation& observation : observations) {
				const NetSoundObservationKey key = KeyOfObservation(observation);
				const uint64_t fields[5] = {key.objectUID, key.tick, key.phase, key.occurrence, key.ordinal};
				const uint64_t last[5] = {previous.objectUID, previous.tick, previous.phase, previous.occurrence, previous.ordinal};
				uint8_t mask = 0;
				size_t fullKeyBytes = 1;
				for (int field = 0; field < 5; ++field) {
					if (fields[field] != last[field]) {
						mask |= static_cast<uint8_t>(1U << field);
						fullKeyBytes += VarU64Size(fields[field]);
					}
				}
				uint16_t knownSlot = 0;
				const bool known = dictionary && dictionary->Lookup(key, knownSlot);
				// A slot the dictionary has not handed out yet is priced at its widest, so measuring never
				// costs a slot.
				const size_t cost = 4 + (known ? VarU64Size(static_cast<uint64_t>(knownSlot) << 1)
				                              : VarU64Size((static_cast<uint64_t>(NetSoundObservationDictionary::c_MaxSlots) << 1) | 1U) + fullKeyBytes);
				if (out.size() - blockStart + cost > budget) {
					break;
				}
				uint16_t slot = 0;
				bool fullKey = true;
				if (dictionary) {
					dictionary->Assign(key, slot, fullKey);
				}
				AppendVarU64(out, (static_cast<uint64_t>(slot) << 1) | (fullKey ? 1U : 0U));
				if (fullKey) {
					AppendU8(out, mask);
					for (int field = 0; field < 5; ++field) {
						if (mask & (1U << field)) {
							AppendVarU64(out, fields[field]);
						}
					}
					previous = key;
				}
				AppendU32LE(out, FloatToBitsLE(observation.value));
				++encoded;
			}
			out[countOffset] = static_cast<uint8_t>(encoded & 0xFFU);
			out[countOffset + 1] = static_cast<uint8_t>((encoded >> 8) & 0xFFU);
			if (outEncoded) {
				*outEncoded = encoded;
			}
			return true;
		}

		bool EncodePayload(const NetLockstepFrame& payload, std::vector<uint8_t>& out, NetLockstepError* error, NetSoundObservationDictionary* dictionary, size_t* outObservationsEncoded) {
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
					case NetGameCommandType::AIEquip: {
						const NetGameAIEquip& equip = std::get<NetGameAIEquip>(command.payload);
						AppendU64LE(out, static_cast<uint64_t>(equip.actorUID));
						AppendU32LE(out, static_cast<uint32_t>(equip.team));
						AppendU8(out, equip.op);
						AppendU8(out, equip.depositToFront ? 1 : 0);
						if (!AppendString(out, equip.group, NetLockstepCodec::c_MaxScenarioBytes, "ai_equip_group", error) ||
						    !AppendString(out, equip.excludeGroup, NetLockstepCodec::c_MaxScenarioBytes, "ai_equip_exclude_group", error) ||
						    !AppendString(out, equip.moduleName, NetLockstepCodec::c_MaxScenarioBytes, "ai_equip_module", error) ||
						    !AppendString(out, equip.presetName, NetLockstepCodec::c_MaxScenarioBytes, "ai_equip_preset", error)) {
							return false;
						}
						break;
					}
					case NetGameCommandType::Reseat: {
						const NetGameReseat& reseat = std::get<NetGameReseat>(command.payload);
						if (reseat.actorUIDs.size() > NetLockstepCodec::c_MaxReseatActors) {
							SetError(error, NetLockstepErrorCode::PayloadTooLarge, out.size(), "reseat names too many actors");
							return false;
						}
						AppendU32LE(out, static_cast<uint32_t>(reseat.team));
						AppendU8(out, reseat.newOwnerPeerId);
						AppendU16LE(out, static_cast<uint16_t>(reseat.actorUIDs.size()));
						for (const int64_t actorUID : reseat.actorUIDs) {
							AppendU64LE(out, static_cast<uint64_t>(actorUID));
						}
						break;
					}
					case NetGameCommandType::SoundOp: {
						const NetGameSoundOp& sound = std::get<NetGameSoundOp>(command.payload);
						if (sound.soundSetPath.size() > NetLockstepCodec::c_MaxSoundSetPath) {
							SetError(error, NetLockstepErrorCode::PayloadTooLarge, out.size(), "sound op names too deep a sound set");
							return false;
						}
						AppendU64LE(out, static_cast<uint64_t>(sound.actorUID));
						AppendU32LE(out, static_cast<uint32_t>(sound.team));
						AppendU64LE(out, sound.soundIdentity);
						AppendU8(out, sound.op);
						AppendU8(out, sound.property);
						AppendU32LE(out, static_cast<uint32_t>(sound.player));
						AppendU32LE(out, static_cast<uint32_t>(sound.value));
						AppendU32LE(out, FloatToBitsLE(sound.x));
						AppendU32LE(out, FloatToBitsLE(sound.y));
						AppendU16LE(out, static_cast<uint16_t>(sound.soundSetPath.size()));
						for (const uint16_t index: sound.soundSetPath) {
							AppendU16LE(out, index);
						}
						if (!AppendString(out, sound.payload, NetLockstepCodec::c_MaxSoundStructureBytes, "sound_op_payload", error)) {
							return false;
						}
						break;
					}
					case NetGameCommandType::AIOrder: {
						const NetGameAIOrder& order = std::get<NetGameAIOrder>(command.payload);
						AppendU64LE(out, static_cast<uint64_t>(order.actorUID));
						AppendU32LE(out, static_cast<uint32_t>(order.team));
						AppendU8(out, order.op);
						AppendU32LE(out, FloatToBitsLE(order.x));
						AppendU32LE(out, FloatToBitsLE(order.y));
						AppendU64LE(out, static_cast<uint64_t>(order.targetUID));
						break;
					}
				}
			}
			AppendU64LE(out, payload.roundId);
			return AppendObservations(payload.observations, out, dictionary, outObservationsEncoded, error);
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
			AppendU64LE(out, payload.roundId);
			return true;
		}

		bool DecodeStart(ByteReader& reader, NetLockstepPayload& out, NetLockstepError* error, uint16_t version) {
			NetLockstepStart payload;
			if (!ReadOrTruncated(reader.ReadU64LE(payload.sessionId), reader, error, "session_id") ||
			    !ReadOrTruncated(reader.ReadU64LE(payload.startFrame), reader, error, "start_frame") ||
			    !ReadOrTruncated(reader.ReadU16LE(payload.inputDelayFrames), reader, error, "input_delay_frames") ||
			    !ReadOrTruncated(reader.ReadU16LE(payload.controllerFrameVersion), reader, error, "controller_frame_version") ||
			    !ReadOrTruncated(reader.ReadU16LE(payload.controllerFrameEncodedSize), reader, error, "controller_frame_encoded_size") ||
			    !ReadOrTruncated(reader.ReadU8(payload.localPeerId), reader, error, "local_peer_id") ||
			    !ReadOrTruncated(reader.ReadU8(payload.peerCount), reader, error, "peer_count") ||
			    (version >= NetLockstepCodec::c_RoundVersion && !ReadOrTruncated(reader.ReadU64LE(payload.roundId), reader, error, "round_id")) ||
			    !reader.ReadString(payload.scenario, NetLockstepCodec::c_MaxScenarioBytes, "scenario", error) ||
			    !reader.ReadString(payload.ownershipPolicy, NetLockstepCodec::c_MaxOwnershipPolicyBytes, "ownership_policy", error) ||
			    !ValidateStart(payload, error)) {
				return false;
			}
			out = payload;
			return true;
		}

		// A block belonging to another round is read with `discard` and no dictionary: its shape is checked,
		// nothing is resolved and no observation comes out, so a frame this round is going to drop cannot
		// disturb its sender's live slots. Bindings are staged and applied once the whole block has read, so a
		// refusal part way through leaves the table exactly as it was.
		bool ReadObservations(ByteReader& reader, NetLockstepFrame& payload, NetSoundObservationDictionary* dictionary, NetLockstepError* error, uint16_t version, bool discard) {
			bool restart = false;
			if (version >= NetLockstepCodec::c_ObservationBindingSequenceVersion) {
				uint64_t bindingsBefore = 0;
				if (!ReadVarOrFail(reader, bindingsBefore, error, "observation_bindings_before")) {
					return false;
				}
				if (dictionary && bindingsBefore != dictionary->BindingCount()) {
					// A sender that starts over has spelled nothing out yet, which is a new round and not a
					// hole; anything else means this peer is missing a binding it can never be told again.
					if (bindingsBefore != 0) {
						SetError(error, NetLockstepErrorCode::ObservationBindingGap, reader.Offset(),
						         "sound observation bindings jump from " + std::to_string(dictionary->BindingCount()) + " to " + std::to_string(bindingsBefore));
						return false;
					}
					restart = true;
				}
			}
			// What this block binds, in the order it binds it, and what each of its slots means as it reads.
			std::vector<std::pair<uint16_t, NetSoundObservationKey>> staged;
			std::map<uint16_t, NetSoundObservationKey> stagedNow;
			uint16_t observationCount = 0;
			if (!ReadOrTruncated(reader.ReadU16LE(observationCount), reader, error, "observation_count")) {
				return false;
			}
			if (observationCount > NetLockstepCodec::c_MaxObservationsPerPacket) {
				SetError(error, NetLockstepErrorCode::PayloadTooLarge, reader.Offset(), "observation_count exceeds maximum");
				return false;
			}
			if (!discard) {
				payload.observations.reserve(observationCount);
			}
			NetSoundObservationKey previous;
			for (uint16_t i = 0; i < observationCount; ++i) {
				NetSoundObservationKey key;
				if (version < NetLockstepCodec::c_ObservationSlotVersion) {
					// Before the slot form every observation spelled out its own key, in full width.
					if (!ReadOrTruncated(reader.ReadU64LE(key.objectUID), reader, error, "observation_object") ||
					    !ReadOrTruncated(reader.ReadU64LE(key.tick), reader, error, "observation_tick") ||
					    !ReadOrTruncated(reader.ReadU64LE(key.phase), reader, error, "observation_phase") ||
					    !ReadOrTruncated(reader.ReadU64LE(key.occurrence), reader, error, "observation_occurrence") ||
					    !ReadOrTruncated(reader.ReadU64LE(key.ordinal), reader, error, "observation_ordinal")) {
						return false;
					}
				} else {
					uint64_t head = 0;
					if (!ReadVarOrFail(reader, head, error, "observation_slot")) {
						return false;
					}
					const uint64_t slot = head >> 1;
					if (slot >= NetSoundObservationDictionary::c_MaxSlots) {
						SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "observation names a slot past the table");
						return false;
					}
					if ((head & 1U) != 0) {
						uint8_t mask = 0;
						if (!ReadOrTruncated(reader.ReadU8(mask), reader, error, "observation_key_mask")) {
							return false;
						}
						if ((mask & 0xE0U) != 0) {
							SetError(error, NetLockstepErrorCode::ReservedFieldNonZero, reader.Offset() - 1, "observation key mask has reserved bits set");
							return false;
						}
						uint64_t* const fields[5] = {&key.objectUID, &key.tick, &key.phase, &key.occurrence, &key.ordinal};
						const uint64_t last[5] = {previous.objectUID, previous.tick, previous.phase, previous.occurrence, previous.ordinal};
						static const char* const names[5] = {"observation_object", "observation_tick", "observation_phase", "observation_occurrence", "observation_ordinal"};
						for (int field = 0; field < 5; ++field) {
							if (mask & (1U << field)) {
								if (!ReadVarOrFail(reader, *fields[field], error, names[field])) {
									return false;
								}
							} else {
								*fields[field] = last[field];
							}
						}
						previous = key;
						if (dictionary) {
							staged.emplace_back(static_cast<uint16_t>(slot), key);
							stagedNow[static_cast<uint16_t>(slot)] = key;
						}
					} else if (!discard) {
						// A slot this block already spelled out reads from the staged bindings; past a restart the
						// table those slots came from is already gone.
						const auto stagedKey = stagedNow.find(static_cast<uint16_t>(slot));
						if (stagedKey != stagedNow.end()) {
							key = stagedKey->second;
						} else if (!dictionary || restart || !dictionary->Resolve(static_cast<uint16_t>(slot), key)) {
							SetError(error, NetLockstepErrorCode::UnboundObservationSlot, reader.Offset(), "observation names a slot this sender never spelled out");
							return false;
						}
					}
				}
				uint32_t valueBits = 0;
				if (!ReadOrTruncated(reader.ReadU32LE(valueBits), reader, error, "observation_value")) {
					return false;
				}
				NetSoundObservation observation;
				observation.value = FloatFromBitsLE(valueBits);
				if (!std::isfinite(observation.value)) {
					SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset() - 4, "sound observation value is not finite");
					return false;
				}
				if (discard) {
					continue;
				}
				observation.objectUID = key.objectUID;
				observation.tick = key.tick;
				observation.phase = key.phase;
				observation.occurrence = key.occurrence;
				observation.ordinal = key.ordinal;
				// The observation's sender is the authenticated frame sender, like a command's.
				observation.senderPeerId = payload.senderPeerId;
				payload.observations.push_back(observation);
			}
			if (dictionary) {
				if (restart) {
					dictionary->Reset();
				}
				for (const auto& [slot, key]: staged) {
					dictionary->Bind(slot, key);
				}
			}
			return true;
		}

		bool DecodeFrame(ByteReader& reader, NetLockstepPayload& out, NetLockstepError* error, uint16_t controllerFrameVersion, uint16_t version, NetSoundObservationTables* tables) {
			NetLockstepFrame payload;
			uint8_t reserved = 0;
			uint16_t frameCount = 0;
			const size_t frameBytesSize = ControllerFrameCodec::EncodedSizeFor(controllerFrameVersion);
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
				if (!ReadOrTruncated(reader.ReadBytes(frameBytes, frameBytesSize), reader, error, "ControllerFrame")) {
					return false;
				}
				ControllerFrame frame;
				std::string frameError;
				if (!ControllerFrameCodec::Decode(frameBytes, frameBytesSize, frame, &frameError, controllerFrameVersion)) {
					SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset() - frameBytesSize, "ControllerFrame decode failed: " + frameError);
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
					case NetGameCommandType::AIEquip: {
						NetGameAIEquip equip;
						uint64_t actorUID = 0;
						uint32_t team = 0;
						uint8_t depositToFront = 0;
						if (!ReadOrTruncated(reader.ReadU64LE(actorUID), reader, error, "ai_equip_actor_uid") ||
						    !ReadOrTruncated(reader.ReadU32LE(team), reader, error, "ai_equip_team") ||
						    !ReadOrTruncated(reader.ReadU8(equip.op), reader, error, "ai_equip_op") ||
						    !ReadOrTruncated(reader.ReadU8(depositToFront), reader, error, "ai_equip_deposit_to_front") ||
						    !reader.ReadString(equip.group, NetLockstepCodec::c_MaxScenarioBytes, "ai_equip_group", error) ||
						    !reader.ReadString(equip.excludeGroup, NetLockstepCodec::c_MaxScenarioBytes, "ai_equip_exclude_group", error) ||
						    !reader.ReadString(equip.moduleName, NetLockstepCodec::c_MaxScenarioBytes, "ai_equip_module", error) ||
						    !reader.ReadString(equip.presetName, NetLockstepCodec::c_MaxScenarioBytes, "ai_equip_preset", error)) {
							return false;
						}
						if (equip.op > NetGameAIEquip::UnequipBGArm) {
							SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "ai equip op is invalid");
							return false;
						}
						equip.actorUID = static_cast<int64_t>(actorUID);
						equip.team = static_cast<int32_t>(team);
						equip.depositToFront = depositToFront != 0;
						command.payload = std::move(equip);
						break;
					}
					case NetGameCommandType::SoundOp: {
						NetGameSoundOp sound;
						uint64_t actorUID = 0;
						uint32_t team = 0;
						uint32_t player = 0;
						uint32_t value = 0;
						uint32_t xBits = 0;
						uint32_t yBits = 0;
						uint16_t pathSize = 0;
						if (!ReadOrTruncated(reader.ReadU64LE(actorUID), reader, error, "sound_op_actor_uid") ||
						    !ReadOrTruncated(reader.ReadU32LE(team), reader, error, "sound_op_team") ||
						    !ReadOrTruncated(reader.ReadU64LE(sound.soundIdentity), reader, error, "sound_op_identity") ||
						    !ReadOrTruncated(reader.ReadU8(sound.op), reader, error, "sound_op_op") ||
						    !ReadOrTruncated(reader.ReadU8(sound.property), reader, error, "sound_op_property") ||
						    !ReadOrTruncated(reader.ReadU32LE(player), reader, error, "sound_op_player") ||
						    !ReadOrTruncated(reader.ReadU32LE(value), reader, error, "sound_op_value") ||
						    !ReadOrTruncated(reader.ReadU32LE(xBits), reader, error, "sound_op_x") ||
						    !ReadOrTruncated(reader.ReadU32LE(yBits), reader, error, "sound_op_y") ||
						    !ReadOrTruncated(reader.ReadU16LE(pathSize), reader, error, "sound_op_path_size")) {
							return false;
						}
						if (sound.op >= NetGameSoundOp::OpCount || sound.property >= NetGameSoundOp::c_PropertyCount) {
							SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "sound op is invalid");
							return false;
						}
						if (pathSize > NetLockstepCodec::c_MaxSoundSetPath) {
							SetError(error, NetLockstepErrorCode::PayloadTooLarge, reader.Offset(), "sound op names too deep a sound set");
							return false;
						}
						sound.soundSetPath.resize(pathSize);
						for (uint16_t index = 0; index < pathSize; ++index) {
							if (!ReadOrTruncated(reader.ReadU16LE(sound.soundSetPath[index]), reader, error, "sound_op_path")) {
								return false;
							}
						}
						if (!reader.ReadString(sound.payload, NetLockstepCodec::c_MaxSoundStructureBytes, "sound_op_payload", error)) {
							return false;
						}
						sound.actorUID = static_cast<int64_t>(actorUID);
						sound.team = static_cast<int32_t>(team);
						sound.player = static_cast<int32_t>(player);
						sound.value = static_cast<int32_t>(value);
						sound.x = FloatFromBitsLE(xBits);
						sound.y = FloatFromBitsLE(yBits);
						command.payload = std::move(sound);
						break;
					}
					case NetGameCommandType::AIOrder: {
						NetGameAIOrder order;
						uint64_t actorUID = 0;
						uint32_t team = 0;
						uint32_t xBits = 0;
						uint32_t yBits = 0;
						uint64_t targetUID = 0;
						if (!ReadOrTruncated(reader.ReadU64LE(actorUID), reader, error, "ai_order_actor_uid") ||
						    !ReadOrTruncated(reader.ReadU32LE(team), reader, error, "ai_order_team") ||
						    !ReadOrTruncated(reader.ReadU8(order.op), reader, error, "ai_order_op") ||
						    !ReadOrTruncated(reader.ReadU32LE(xBits), reader, error, "ai_order_x") ||
						    !ReadOrTruncated(reader.ReadU32LE(yBits), reader, error, "ai_order_y") ||
						    !ReadOrTruncated(reader.ReadU64LE(targetUID), reader, error, "ai_order_target_uid")) {
							return false;
						}
						if (order.op > NetGameAIOrder::PopWaypoint) {
							SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "ai order op is invalid");
							return false;
						}
						order.actorUID = static_cast<int64_t>(actorUID);
						order.team = static_cast<int32_t>(team);
						order.x = FloatFromBitsLE(xBits);
						order.y = FloatFromBitsLE(yBits);
						order.targetUID = static_cast<int64_t>(targetUID);
						command.payload = order;
						break;
					}
					case NetGameCommandType::Reseat: {
						NetGameReseat reseat;
						uint32_t team = 0;
						uint16_t actorCount = 0;
						if (!ReadOrTruncated(reader.ReadU32LE(team), reader, error, "reseat_team") ||
						    !ReadOrTruncated(reader.ReadU8(reseat.newOwnerPeerId), reader, error, "reseat_new_owner") ||
						    !ReadOrTruncated(reader.ReadU16LE(actorCount), reader, error, "reseat_actor_count")) {
							return false;
						}
						if (actorCount > NetLockstepCodec::c_MaxReseatActors) {
							SetError(error, NetLockstepErrorCode::PayloadTooLarge, reader.Offset(), "reseat_actor_count exceeds maximum");
							return false;
						}
						reseat.actorUIDs.reserve(actorCount);
						for (uint16_t a = 0; a < actorCount; ++a) {
							uint64_t actorUID = 0;
							if (!ReadOrTruncated(reader.ReadU64LE(actorUID), reader, error, "reseat_actor_uid")) {
								return false;
							}
							reseat.actorUIDs.push_back(static_cast<int64_t>(actorUID));
						}
						reseat.team = static_cast<int32_t>(team);
						command.payload = std::move(reseat);
						break;
					}
					default:
						SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset() - 2, "game command has invalid type");
						return false;
				}
				payload.commands.push_back(std::move(command));
			}
			if (version >= NetLockstepCodec::c_RoundVersion) {
				if (!ReadOrTruncated(reader.ReadU64LE(payload.roundId), reader, error, "round_id")) {
					return false;
				}
				// A frame from another round decodes like any other and is dropped by the round's own rule, so
				// its sender still counts as heard from; its observations are read past rather than resolved,
				// so one round's slots never stand for another's keys and the counters stay for real holes.
				const bool otherRound = tables && payload.roundId != 0 && tables->roundId != 0 && payload.roundId != tables->roundId;
				NetSoundObservationDictionary* dictionary = tables && !otherRound ? &tables->For(payload.senderPeerId) : nullptr;
				if (!ReadObservations(reader, payload, dictionary, error, version, otherRound)) {
					return false;
				}
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

		bool DecodeChecksum(ByteReader& reader, NetLockstepPayload& out, NetLockstepError* error, uint16_t version) {
			NetLockstepChecksum payload;
			uint8_t reserved0 = 0;
			uint16_t reserved16 = 0;
			const uint8_t* hashBytes = nullptr;
			if (!ReadOrTruncated(reader.ReadU8(payload.senderPeerId), reader, error, "sender_peer_id") ||
			    !ReadOrTruncated(reader.ReadU8(reserved0), reader, error, "reserved") ||
			    !ReadOrTruncated(reader.ReadU16LE(reserved16), reader, error, "reserved") ||
			    !ReadOrTruncated(reader.ReadU64LE(payload.frame), reader, error, "frame") ||
			    !ReadOrTruncated(reader.ReadBytes(hashBytes, payload.hash.size()), reader, error, "checksum_hash") ||
			    (version >= NetLockstepCodec::c_RoundVersion && !ReadOrTruncated(reader.ReadU64LE(payload.roundId), reader, error, "round_id")) ||
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
		if (senderPeerId != rhs.senderPeerId || targetFrame != rhs.targetFrame || frames.size() != rhs.frames.size() || commands != rhs.commands ||
		    roundId != rhs.roundId || observations != rhs.observations) {
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
			case NetLockstepErrorCode::UnboundObservationSlot: return "UnboundObservationSlot";
			case NetLockstepErrorCode::ObservationBindingGap: return "ObservationBindingGap";
		}
		return "Unknown";
	}

	NetSoundObservationKey KeyOfObservation(const NetSoundObservation& observation) {
		return {observation.objectUID, observation.tick, observation.phase, observation.occurrence, observation.ordinal};
	}

	bool NetSoundObservationDictionary::Lookup(const NetSoundObservationKey& key, uint16_t& outSlot) const {
		const auto found = m_SlotOf.find(key);
		if (found == m_SlotOf.end()) {
			return false;
		}
		outSlot = found->second;
		return true;
	}

	void NetSoundObservationDictionary::Assign(const NetSoundObservationKey& key, uint16_t& outSlot, bool& outFullKey) {
		if (Lookup(key, outSlot)) {
			m_Recent.splice(m_Recent.end(), m_Recent, m_Slots[outSlot].recency);
			outFullKey = false;
			return;
		}
		// Grow while there is room, then reuse the slot whose key went longest without a mention. Which
		// slot that is stays a local choice: every binding is spelled out on the wire.
		if (m_Slots.size() < c_MaxSlots) {
			outSlot = static_cast<uint16_t>(m_Slots.size());
		} else {
			outSlot = m_Recent.front();
		}
		Bind(outSlot, key);
		outFullKey = true;
	}

	void NetSoundObservationDictionary::Bind(uint16_t slot, const NetSoundObservationKey& key) {
		if (slot >= c_MaxSlots) {
			return;
		}
		if (slot >= m_Slots.size()) {
			m_Slots.resize(static_cast<size_t>(slot) + 1);
		}
		Slot& entry = m_Slots[slot];
		if (entry.bound) {
			m_SlotOf.erase(entry.key);
			m_Recent.erase(entry.recency);
		}
		entry.key = key;
		entry.bound = true;
		entry.recency = m_Recent.insert(m_Recent.end(), slot);
		m_SlotOf[key] = slot;
		++m_Bindings;
	}

	bool NetSoundObservationDictionary::Resolve(uint16_t slot, NetSoundObservationKey& outKey) const {
		if (slot >= m_Slots.size() || !m_Slots[slot].bound) {
			return false;
		}
		outKey = m_Slots[slot].key;
		return true;
	}

	void NetSoundObservationDictionary::Reset() {
		m_Slots.clear();
		m_SlotOf.clear();
		m_Recent.clear();
		m_Bindings = 0;
	}

	bool NetLockstepCodec::Encode(const NetLockstepPacket& packet, std::vector<uint8_t>& outBytes, NetLockstepError* error, NetSoundObservationDictionary* dictionary, size_t* outObservationsEncoded) {
		std::vector<uint8_t> payloadBytes;
		const bool payloadOk = std::visit(Overloaded{
			[&](const NetLockstepStart& payload) { return EncodePayload(payload, payloadBytes, error); },
			[&](const NetLockstepFrame& payload) { return EncodePayload(payload, payloadBytes, error, dictionary, outObservationsEncoded); },
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

	NetLockstepDecodeResult NetLockstepCodec::Decode(const uint8_t* data, size_t size, uint16_t controllerFrameVersion, NetSoundObservationTables* tables) {
		if (!data && size > 0) {
			return Fail(NetLockstepErrorCode::NullBuffer, 0, "input buffer is null");
		}
		if (!ControllerFrameCodec::IsSupportedVersion(controllerFrameVersion)) {
			return Fail(NetLockstepErrorCode::UnsupportedVersion, 0, "unsupported ControllerFrame version");
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
		if (version < c_MinVersion || version > c_Version) {
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
				payloadOk = DecodeStart(payloadReader, payload, &payloadError, version);
				break;
			case NetLockstepPacketType::Frame:
				payloadOk = DecodeFrame(payloadReader, payload, &payloadError, controllerFrameVersion, version, tables);
				break;
			case NetLockstepPacketType::Ack:
				payloadOk = DecodeAck(payloadReader, payload, &payloadError);
				break;
			case NetLockstepPacketType::Stop:
				payloadOk = DecodeStop(payloadReader, payload, &payloadError);
				break;
			case NetLockstepPacketType::Checksum:
				payloadOk = DecodeChecksum(payloadReader, payload, &payloadError, version);
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

	NetLockstepDecodeResult NetLockstepCodec::Decode(const std::vector<uint8_t>& bytes, uint16_t controllerFrameVersion, NetSoundObservationTables* tables) {
		return Decode(bytes.data(), bytes.size(), controllerFrameVersion, tables);
	}

	// The header alone identifies the wire; a frame's observations need their sender's slot table, which
	// the session and lobby planes do not keep, so they must not have to decode a payload to place it.
	bool NetLockstepCodec::LooksLikePacket(const std::vector<uint8_t>& bytes) {
		if (bytes.size() < c_HeaderBytes) {
			return false;
		}
		ByteReader reader(bytes.data(), c_HeaderBytes);
		uint32_t magic = 0;
		uint16_t version = 0;
		uint16_t headerBytes = 0;
		uint16_t rawPacketType = 0;
		uint16_t flags = 0;
		uint32_t payloadLength = 0;
		reader.ReadU32LE(magic);
		reader.ReadU16LE(version);
		reader.ReadU16LE(headerBytes);
		reader.ReadU16LE(rawPacketType);
		reader.ReadU16LE(flags);
		reader.ReadU32LE(payloadLength);
		switch (static_cast<NetLockstepPacketType>(rawPacketType)) {
			case NetLockstepPacketType::Start:
			case NetLockstepPacketType::Frame:
			case NetLockstepPacketType::Ack:
			case NetLockstepPacketType::Stop:
			case NetLockstepPacketType::Checksum:
				break;
			default:
				return false;
		}
		return magic == c_Magic && version >= c_MinVersion && version <= c_Version && headerBytes == c_HeaderBytes &&
		       flags == 0 && bytes.size() == static_cast<size_t>(c_HeaderBytes) + payloadLength;
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
		// Per-sender delay: the set must cover every peer and agree with the local production delay.
		if (!config.peerInputDelayFrames.empty()) {
			if (config.peerInputDelayFrames.size() != config.peerCount) {
				if (error) *error = "lockstep per-peer input delays must cover every peer";
				return false;
			}
			for (uint8_t peerId = 1; peerId <= config.peerCount; ++peerId) {
				const auto delayIt = config.peerInputDelayFrames.find(peerId);
				if (delayIt == config.peerInputDelayFrames.end()) {
					if (error) *error = "lockstep per-peer input delays must cover every peer";
					return false;
				}
				if (delayIt->second > NetLockstepCodec::c_MaxInputDelayFrames) {
					if (error) *error = "lockstep per-peer input delay is out of range";
					return false;
				}
			}
			const auto localDelayIt = config.peerInputDelayFrames.find(config.localPeerId);
			if (localDelayIt == config.peerInputDelayFrames.end() || localDelayIt->second != config.inputDelayFrames) {
				if (error) *error = "lockstep local input delay disagrees with the per-peer set";
				return false;
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
		start.roundId = config.roundId;

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
		m_DeferStops = false;
		m_State = NetLockstepState::WaitingForStart;
		ResetRoundState();
		// A round of our own produces its own input; a round we FOLLOW keeps what we already queued.
		m_LocalFrames.clear();
		m_LocalCommands.clear();
		m_LocalObservations.clear();
		m_LastQueuedTargetFrame = std::numeric_limits<uint64_t>::max();
		m_RoundId = config.roundId;
		m_ObservationDecodeTables.roundId = m_RoundId;
		m_Stats = {};
		m_Stats.sessionId = config.sessionId;
		m_Stats.configuredStartFrame = config.startFrame;
		// The commit stream starts at the EARLIEST sender's first delayed frame; later senders ramp in.
		m_PeerEffectiveStart.clear();
		uint64_t firstCommitFrame = config.startFrame + config.inputDelayFrames;
		for (uint8_t peerId = 1; peerId <= config.peerCount; ++peerId) {
			const auto delayIt = config.peerInputDelayFrames.find(peerId);
			const uint16_t delay = delayIt != config.peerInputDelayFrames.end() ? delayIt->second : config.inputDelayFrames;
			m_PeerEffectiveStart[peerId] = config.startFrame + delay;
			if (m_PeerEffectiveStart[peerId] < firstCommitFrame) {
				firstCommitFrame = m_PeerEffectiveStart[peerId];
			}
		}
		m_Stats.effectiveStartFrame = firstCommitFrame;
		m_Stats.inputDelayFrames = config.inputDelayFrames;
		m_Stats.localPeerId = config.localPeerId;
		m_Stats.remotePeerId = config.remotePeerId;
		m_Stats.nextFrame = m_Stats.effectiveStartFrame;

		return SendStart(error);
	}

	bool NetLockstepCoordinator::SendStart(std::string* error, uint8_t onlyPeerId) {
		NetLockstepStart start;
		start.sessionId = m_Config.sessionId;
		start.startFrame = m_Config.startFrame;
		start.inputDelayFrames = m_Config.inputDelayFrames;
		start.controllerFrameVersion = ControllerFrame::c_Version;
		start.controllerFrameEncodedSize = static_cast<uint16_t>(ControllerFrame::c_EncodedSize);
		start.localPeerId = m_Config.localPeerId;
		start.peerCount = m_Config.peerCount;
		start.scenario = m_Config.scenario;
		start.ownershipPolicy = m_Config.ownershipPolicy;
		start.roundId = m_RoundId;
		if (!SendPacket({start}, NetTransportLane::ControlReliable, error, nullptr, nullptr, onlyPeerId)) {
			return false;
		}
		++m_Stats.startPacketsSent;
		return true;
	}

	// A peer that repeats its start is still waiting for one it missed, and ours may be it. Every
	// start a formed peer receives reads as a repeat though, and the answer is itself a start, so an
	// unconditional answer answers the answer: pace it by the ladder the repeats come from.
	bool NetLockstepCoordinator::StartMatchesConfig(const NetLockstepStart& start) const {
		return start.sessionId == m_Config.sessionId &&
		       start.startFrame == m_Config.startFrame &&
		       start.inputDelayFrames == PeerInputDelay(start.localPeerId) &&
		       start.controllerFrameVersion == ControllerFrame::c_Version &&
		       start.controllerFrameEncodedSize == ControllerFrame::c_EncodedSize &&
		       IsKnownRemotePeer(start.localPeerId) &&
		       start.peerCount == m_Config.peerCount &&
		       start.scenario == m_Config.scenario &&
		       start.ownershipPolicy == m_Config.ownershipPolicy;
	}

	bool NetLockstepCoordinator::IsRoundAuthority(uint8_t peerId, NetPeerId fromTransport) const {
		// A relay host issues the round, it never takes one.
		return !m_RelayHost && peerId != 0 && LockstepPeerOfTransport(fromTransport) == peerId;
	}

	// The round is the host's to name, so everything the REMOTES said belongs to the one we are leaving.
	// Our own production stands and goes out again under the new tag: a start we follow carries this
	// round's start frame and delays, so every frame we queued still targets the same frames.
	// Everything one round owns. Start, StartReplay and a round we follow all pass through here, so a
	// field a round carries cannot be reset in two of the three and forgotten in the last. What stays
	// out: the local production a follower keeps, and the deferred-stop mode the launch path sets.
	void NetLockstepCoordinator::ResetRoundState() {
		m_PendingRecoveryStop.reset();
		m_PendingCompleteStop.reset();
		m_LastCompletedSimulationTick.reset();
		m_RemoteStartsReceived.clear();
		m_PeersPlayedThisRound.clear();
		m_RemoteStarts.clear();
		m_LastStartAnswerMs.clear();
		m_ResendFrames.clear();
		m_PeerLeaveFrames.clear();
		m_LeftSeatsHeld.clear();
		m_PeerLastHeardMs.clear();
		m_UnreachablePeers.clear();
		m_RelayBacklog.clear();
		m_RelayBacklogSinceMs.clear();
		m_LastLeaveMessage.clear();
		m_WaitingFrame = std::numeric_limits<uint64_t>::max();
		m_WaitStartMs = 0;
		m_LastStallFrame = UINT64_MAX;
		m_RemoteFrames.clear();
		m_RemoteCommands.clear();
		m_RemoteObservations.clear();
		m_RemoteChecksums.clear();
		m_LocalChecksums.clear();
		m_PendingObservations.clear();
		m_DroppedObservations.clear();
		m_ObservationDecodeTables.Reset();
		m_ObservationEncodeTables.Reset();
		m_ReadyFrames.clear();
		m_PreStartFrames.clear();
		m_PreStartChecksums.clear();
		m_LastStartSentMs = UINT64_MAX;
	}

	void NetLockstepCoordinator::ReadoptRound(uint64_t roundId, uint64_t nowMs) {
		m_RoundId = roundId;
		++m_Stats.roundReadoptions;
		ResetRoundState();
		m_ObservationDecodeTables.roundId = m_RoundId;
		m_State = NetLockstepState::WaitingForStart;
		std::string ignored;
		(void)SendStart(&ignored);
		++m_Stats.startRetransmits;
		m_LastStartSentMs = nowMs;
		// Our frames went out under the round we left, which the host refuses, so they go out again.
		// Their readings ride the next frame we queue instead: re-sending them here would bind slots in
		// our own table that a refused send never showed the receiver.
		for (const auto& [targetFrame, frames]: m_LocalFrames) {
			m_ResendFrames.insert(targetFrame);
			const auto observationsIt = m_LocalObservations.find(targetFrame);
			if (observationsIt == m_LocalObservations.end()) {
				continue;
			}
			m_Stats.observationsCarried += observationsIt->second.size();
			m_PendingObservations.insert(m_PendingObservations.end(), observationsIt->second.begin(), observationsIt->second.end());
			m_LocalObservations.erase(observationsIt);
		}
		FlushResendFrames();
	}

	// A refused send on a reliable lane is backpressure, and the round waits on exactly these frames, so
	// they are retried in order and nothing after a refused one goes out before it does.
	void NetLockstepCoordinator::FlushResendFrames() {
		while (!m_ResendFrames.empty()) {
			const uint64_t targetFrame = *m_ResendFrames.begin();
			const auto framesIt = m_LocalFrames.find(targetFrame);
			if (framesIt == m_LocalFrames.end()) {
				m_ResendFrames.erase(m_ResendFrames.begin());
				continue;
			}
			NetLockstepFrame packet;
			packet.senderPeerId = m_Config.localPeerId;
			packet.targetFrame = targetFrame;
			packet.frames = framesIt->second;
			packet.roundId = m_RoundId;
			if (const auto commandsIt = m_LocalCommands.find(targetFrame); commandsIt != m_LocalCommands.end()) {
				packet.commands = commandsIt->second;
			}
			std::string ignored;
			if (!SendPacket({packet}, m_Config.frameLane, &ignored)) {
				return;
			}
			++m_Stats.framePacketsSent;
			m_ResendFrames.erase(m_ResendFrames.begin());
		}
	}

	void NetLockstepCoordinator::AnswerRepeatedStart(uint8_t peerId, uint64_t nowMs) {
		// A peer we have no link to is the host's to answer, not ours; ours reaches us relayed.
		if ((m_State != NetLockstepState::Running && m_State != NetLockstepState::WaitingForStart) ||
		    m_RemoteTransports.find(peerId) == m_RemoteTransports.end()) {
			return;
		}
		// A peer this round has taken frames from already had every start, or it could not have made
		// one. A new start from it is a peer that has LEFT the round, and ours would hand it a round it
		// is not in - which is how a peer that restarts first loses the next one.
		if (m_PeersPlayedThisRound.find(peerId) != m_PeersPlayedThisRound.end()) {
			++m_Stats.startAnswersSuppressed;
			return;
		}
		const auto answeredIt = m_LastStartAnswerMs.find(peerId);
		if (answeredIt != m_LastStartAnswerMs.end() && nowMs >= answeredIt->second && nowMs - answeredIt->second < c_StartRetransmitMs) {
			return;
		}
		m_LastStartAnswerMs[peerId] = nowMs;
		std::string ignored;
		(void)SendStart(&ignored, peerId);
		++m_Stats.startRetransmits;
		++m_Stats.startAnswers;
		// It cannot say WHICH start it is missing, and in a star the ones only we can give it are the
		// other remotes'. Re-send what we accepted from them, to the peer that asked and nobody else.
		if (!m_RelayHost) {
			return;
		}
		for (const auto& [otherPeerId, otherStart]: m_RemoteStarts) {
			if (otherPeerId == peerId) {
				continue;
			}
			(void)SendPacket({otherStart}, NetTransportLane::ControlReliable, &ignored, nullptr, nullptr, peerId);
			++m_Stats.startsRelayedOnRepeat;
		}
	}

	void NetLockstepCoordinator::FlushPreStart(uint8_t peerId, uint64_t nowMs) {
		const auto transportIt = m_RemoteTransports.find(peerId);
		const NetPeerId fromTransport = transportIt != m_RemoteTransports.end() ? transportIt->second : c_InvalidNetPeerId;
		std::deque<NetLockstepFrame> frames;
		std::deque<NetLockstepChecksum> checksums;
		if (auto held = m_PreStartFrames.find(peerId); held != m_PreStartFrames.end()) {
			frames = std::move(held->second);
			m_PreStartFrames.erase(held);
		}
		if (auto held = m_PreStartChecksums.find(peerId); held != m_PreStartChecksums.end()) {
			checksums = std::move(held->second);
			m_PreStartChecksums.erase(held);
		}
		for (const NetLockstepFrame& frame : frames) {
			HandleFrame(frame, nowMs, fromTransport, false);
		}
		for (const NetLockstepChecksum& checksum : checksums) {
			HandleChecksum(checksum, fromTransport);
		}
	}

	bool NetLockstepCoordinator::StartReplay(INetTransport& transport, const NetLockstepConfig& config, std::string* error) {
		if (config.peerCount < 2 || config.peerCount > NetLockstepCodec::c_MaxPeerCount ||
		    config.localPeerId == 0 || config.localPeerId > config.peerCount) {
			if (error) *error = "replay peer identity is invalid";
			return false;
		}
		m_Transport = &transport;
		m_Config = config;
		m_Config.inputDelayFrames = 0;
		m_Config.peerInputDelayFrames.clear();
		m_RemotePeerIds.clear();
		m_RemoteTransports.clear();
		m_RelayHost = false;
		m_DeferStops = false;
		m_State = NetLockstepState::Running;
		ResetRoundState();
		m_LocalFrames.clear();
		m_LocalCommands.clear();
		m_LocalObservations.clear();
		m_LastQueuedTargetFrame = std::numeric_limits<uint64_t>::max();
		m_PeerEffectiveStart.clear();
		m_RoundId = 0;
		m_Stats = {};
		m_Stats.sessionId = config.sessionId;
		m_Stats.configuredStartFrame = config.startFrame;
		m_Stats.effectiveStartFrame = config.startFrame;
		m_Stats.inputDelayFrames = 0;
		m_Stats.localPeerId = config.localPeerId;
		m_Stats.nextFrame = config.startFrame;
		return true;
	}

	bool NetLockstepCoordinator::QueueReplayFrame(uint64_t frame, std::vector<ControllerFrame> frames, std::vector<NetGameCommand> commands, std::string* error, std::vector<NetSoundObservation> observations) {
		if (m_State != NetLockstepState::Running) {
			if (error) *error = m_Stats.timeoutReason.empty() ? "replay coordinator is not running" : m_Stats.timeoutReason;
			return false;
		}
		if (frame < m_Stats.nextFrame || m_LocalFrames.find(frame) != m_LocalFrames.end()) {
			if (error) *error = "replay frame is duplicate or already accepted";
			return false;
		}
		NetLockstepError frameError;
		if (!ValidateSortedFrames(frames, &frameError)) {
			if (error) *error = frameError.message;
			return false;
		}
		// Playback owns no actor: every recorded frame rides the REMOTE side so the apply drives
		// every actor from the file; the empty local entry satisfies the advance.
		m_LocalFrames[frame] = {};
		const uint8_t bucketPeer = m_Config.localPeerId == 1 ? 2 : 1;
		m_RemoteFrames[frame][bucketPeer] = std::move(frames);
		if (!commands.empty()) {
			m_RemoteCommands[frame][bucketPeer] = std::move(commands);
		}
		if (!observations.empty()) {
			m_RemoteObservations[frame][bucketPeer] = std::move(observations);
		}
		return true;
	}

	bool NetLockstepCoordinator::RewindReplay(uint64_t firstFrame, std::string* error) {
		if (!m_RemotePeerIds.empty() || !m_RemoteTransports.empty()) {
			if (error) *error = "rewind is replay-only";
			return false;
		}
		if (m_State != NetLockstepState::Running) {
			if (error) *error = "rewind requires a running replay coordinator";
			return false;
		}
		m_LocalFrames.clear();
		m_RemoteFrames.clear();
		m_LocalCommands.clear();
		m_RemoteCommands.clear();
		m_LocalChecksums.clear();
		m_RemoteChecksums.clear();
		m_ReadyFrames.clear();
		m_Stats.nextFrame = firstFrame;
		m_Stats.effectiveStartFrame = firstFrame;
		m_Stats.timeoutReason.clear();
		return true;
	}

	bool NetLockstepCoordinator::QueueLocalInput(uint64_t producedFrame, const std::vector<ControllerFrame>& frames, const std::vector<NetGameCommand>& commands, std::string* error, const std::vector<NetSoundObservation>& observations) {
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
		packet.roundId = m_RoundId;
		// What the last frame could not hold goes first, minus anything this frame reads afresh: a newer
		// reading for the same sound would only overwrite it in the same commit.
		packet.observations.reserve(m_PendingObservations.size() + observations.size());
		if (!m_PendingObservations.empty()) {
			std::set<NetSoundObservationKey> resampled;
			for (const NetSoundObservation& observation : observations) {
				resampled.insert(KeyOfObservation(observation));
			}
			for (const NetSoundObservation& carried : m_PendingObservations) {
				if (!resampled.contains(KeyOfObservation(carried))) {
					packet.observations.push_back(carried);
				}
			}
			m_PendingObservations.clear();
		}
		packet.observations.insert(packet.observations.end(), observations.begin(), observations.end());
		for (NetSoundObservation& observation : packet.observations) {
			observation.senderPeerId = m_Config.localPeerId;
		}
		size_t heldBack = 0;
		if (packet.observations.size() > NetLockstepCodec::c_MaxObservationsPerPacket) {
			heldBack = packet.observations.size() - NetLockstepCodec::c_MaxObservationsPerPacket;
			m_PendingObservations.assign(packet.observations.begin() + NetLockstepCodec::c_MaxObservationsPerPacket, packet.observations.end());
			packet.observations.resize(NetLockstepCodec::c_MaxObservationsPerPacket);
		}
		size_t observationsEncoded = packet.observations.size();
		if (!SendPacket({packet}, m_Config.frameLane, error, &m_ObservationEncodeTables.Exactly(m_Config.localPeerId), &observationsEncoded)) {
			return false;
		}
		// Every peer commits what the packet carried, so the leftovers ride the next frame with their own
		// keys and land one frame later on all of them alike.
		if (observationsEncoded < packet.observations.size()) {
			heldBack += packet.observations.size() - observationsEncoded;
			m_PendingObservations.insert(m_PendingObservations.begin(), packet.observations.begin() + static_cast<std::ptrdiff_t>(observationsEncoded), packet.observations.end());
			packet.observations.resize(observationsEncoded);
		}
		m_Stats.observationsCarried += heldBack;
		if (m_PendingObservations.size() > NetLockstepCodec::c_MaxCarriedObservations) {
			// New sounds have outrun the wire for frames on end. The stalest readings go, on this peer
			// alone, before the packet that would have carried them, so every peer still commits the same.
			const size_t dropped = m_PendingObservations.size() - NetLockstepCodec::c_MaxCarriedObservations;
			m_DroppedObservations.insert(m_DroppedObservations.end(), m_PendingObservations.begin(), m_PendingObservations.begin() + static_cast<std::ptrdiff_t>(dropped));
			m_PendingObservations.erase(m_PendingObservations.begin(), m_PendingObservations.begin() + static_cast<std::ptrdiff_t>(dropped));
			if (m_Stats.observationsDropped == 0) {
				std::cout << "[lockstep] more new sounds than the frame can carry; dropping the oldest held readings" << std::endl;
			}
			m_Stats.observationsDropped += dropped;
		}
		m_LocalFrames[targetFrame] = frames;
		if (!packet.commands.empty()) {
			m_LocalCommands[targetFrame] = packet.commands;
		}
		if (!packet.observations.empty()) {
			m_LocalObservations[targetFrame] = packet.observations;
		}
		++m_Stats.framePacketsSent;
		m_Stats.localControllerFramesSent += frames.size();
		if (m_LastQueuedTargetFrame == std::numeric_limits<uint64_t>::max() || targetFrame > m_LastQueuedTargetFrame) {
			m_LastQueuedTargetFrame = targetFrame;
		}
		return true;
	}

	std::vector<NetSoundObservation> NetLockstepCoordinator::TakeDroppedObservations() {
		return std::exchange(m_DroppedObservations, {});
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
		packet.roundId = m_RoundId;
		if (!SendPacket({packet}, m_Config.frameLane, error)) {
			return false;
		}
		CompareChecksums(frame);
		return true;
	}

	void NetLockstepCoordinator::HandleChecksum(const NetLockstepChecksum& checksum, NetPeerId fromTransport) {
		if (!SenderOwnsTransport(checksum.senderPeerId, fromTransport)) {
			std::cout << "[lockstep] dropped a checksum claiming peer " << static_cast<int>(checksum.senderPeerId) << " from the wrong transport" << std::endl;
			return;
		}
		if (!IsKnownRemotePeer(checksum.senderPeerId)) {
			return;
		}
		NetLockstepPeerStats& peerStats = m_Stats.peers[checksum.senderPeerId];
		if (checksum.roundId != 0 && m_RoundId != 0 && checksum.roundId != m_RoundId) {
			++m_Stats.staleRoundPackets;
			++peerStats.staleRoundPackets;
			return;
		}
		if (m_RemoteStartsReceived.find(checksum.senderPeerId) == m_RemoteStartsReceived.end()) {
			std::deque<NetLockstepChecksum>& held = m_PreStartChecksums[checksum.senderPeerId];
			if (held.size() >= NetLockstepCodec::c_MaxFutureFrameSkew) {
				held.pop_front();
			}
			held.push_back(checksum);
			++m_Stats.preStartFramesBuffered;
			++peerStats.preStartBuffered;
			return;
		}
		// Drop absurd future checksums; CompareChecksums only prunes matched frames, so an unmatched
		// far-future frame would otherwise linger in the map.
		if (checksum.frame > m_Stats.nextFrame + NetLockstepCodec::c_MaxFutureFrameSkew) {
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
				ScheduleRecoveryStop(NetLockstepStopReason::Desync, frame, "sim state diverged at tick " + std::to_string(frame) + " (" + DescribePeer(peerId) + ")");
				return;
			}
		}
		for (uint8_t peerId: m_RemotePeerIds) {
			// Every live peer hashes every simulated tick — input ramp-in does not exempt it here.
			if (!IsPeerGoneAtFrame(peerId, frame) && remoteIt->second.find(peerId) == remoteIt->second.end()) {
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
		RefreshLeftSeatHolds();
		for (const NetTransportEvent& event : m_Transport->PollEvents()) {
			HandleEvent(event, nowMs);
		}
		// A start sent while a peer was between rounds is gone; repeat it until every start is in.
		if (m_State == NetLockstepState::WaitingForStart) {
			if (m_LastStartSentMs == UINT64_MAX) {
				m_LastStartSentMs = nowMs;
			} else if (nowMs >= m_LastStartSentMs + c_StartRetransmitMs) {
				std::string ignored;
				(void)SendStart(&ignored);
				++m_Stats.startRetransmits;
				m_LastStartSentMs = nowMs;
			}
		}
		FlushRelayBacklog(nowMs);
		FlushResendFrames();
		DropUnreachablePeers(nowMs);
		AdjudicateSilentPeers(nowMs);
		AdvanceReadyFrames(nowMs);
		EndRoundIfNobodyIsComingBack();
	}

	void NetLockstepCoordinator::Complete(const std::string& message) {
		if (m_State == NetLockstepState::Failed || m_State == NetLockstepState::Stopped || m_State == NetLockstepState::Idle) {
			return;
		}
		if (m_Transport) {
			NetLockstepStop stop;
			stop.senderPeerId = m_Config.localPeerId;
			stop.reason = NetLockstepStopReason::Complete;
			stop.frame = m_DeferStops && m_LastCompletedSimulationTick ? *m_LastCompletedSimulationTick + 1 : m_Stats.nextFrame;
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
		if (m_DeferStops) {
			ScheduleRecoveryStop(NetLockstepStopReason::ResyncRequested, m_Stats.nextFrame, message);
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

	void NetLockstepCoordinator::ScheduleRecoveryStop(NetLockstepStopReason reason, uint64_t frame, const std::string& message) {
		if (!m_DeferStops) {
			Fail(reason, frame, message);
			return;
		}
		if (m_PendingRecoveryStop || !IsRunning()) return;
		m_PendingRecoveryStop = NetLockstepStop{m_Config.localPeerId, reason, frame, message};
		if (m_Config.localPeerId != m_Config.matchConfig.hostPeerId) {
			// A client requests the stop while continuing to supply the host's current tick.
			std::string ignored;
			(void)SendPacket({*m_PendingRecoveryStop}, NetTransportLane::ControlReliable, &ignored);
		}
	}

	bool NetLockstepCoordinator::FinishSimulationTick(uint64_t completedTick) {
		if (!m_DeferStops || !IsRunning()) return false;
		m_LastCompletedSimulationTick = completedTick;
		if (m_PendingRecoveryStop && m_Config.localPeerId == m_Config.matchConfig.hostPeerId) {
			const NetLockstepStop stop = *m_PendingRecoveryStop;
			m_PendingRecoveryStop.reset();
			Fail(stop.reason, completedTick + 1, stop.message);
			return true;
		}
		if (m_PendingCompleteStop && completedTick + 1 >= m_PendingCompleteStop->frame) {
			m_Stats.timeoutReason = "Complete:" + m_PendingCompleteStop->message;
			m_PendingCompleteStop.reset();
			m_State = NetLockstepState::Stopped;
			return true;
		}
		return false;
	}

	bool NetLockstepCoordinator::PopReadyFrame(NetLockstepReadyFrame& outFrame) {
		if (m_ReadyFrames.empty()) {
			return false;
		}
		outFrame = std::move(m_ReadyFrames.front());
		m_ReadyFrames.pop_front();
		return true;
	}

	bool NetLockstepCoordinator::PeekLocalFrames(uint64_t frame, std::vector<ControllerFrame>& outFrames) const {
		const auto found = m_LocalFrames.find(frame);
		if (found == m_LocalFrames.end()) {
			return false;
		}
		outFrames = found->second;
		return true;
	}

	uint8_t NetLockstepCoordinator::ResolveActorOwnerBeforeLeaves(int64_t actorUniqueID, int actorTeam, bool cpuControlled) const {
		if (m_Config.peerCount == 0 || m_Config.localPeerId == 0) {
			return m_Config.localPeerId;
		}
		if (!m_Config.matchConfig.players.empty()) {
			const uint8_t team = actorTeam < 0 ? 0 : static_cast<uint8_t>(actorTeam);
			return NetActorOwnership::ResolveOwnerPeer(m_Config.matchConfig, {actorUniqueID, team, cpuControlled});
		}
		const uint64_t normalized = actorUniqueID < 0 ? static_cast<uint64_t>(-(actorUniqueID + 1)) + 1U : static_cast<uint64_t>(actorUniqueID);
		return static_cast<uint8_t>((normalized % m_Config.peerCount) + 1U);
	}

	uint8_t NetLockstepCoordinator::ResolveActorOwner(int64_t actorUniqueID, int actorTeam, bool cpuControlled) const {
		uint8_t ownerPeerId = ResolveActorOwnerBeforeLeaves(actorUniqueID, actorTeam, cpuControlled);
		if (m_Config.peerCount == 0 || m_Config.localPeerId == 0 || m_Config.matchConfig.players.empty()) {
			return ownerPeerId;
		}
		const uint8_t team = actorTeam < 0 ? 0 : static_cast<uint8_t>(actorTeam);
		// A leaver's team falls to its next surviving human peer, so the units play on. The
		// lockstep gate synchronizes leave knowledge, so every peer re-resolves identically.
		if (m_PeerLeaveFrames.find(ownerPeerId) != m_PeerLeaveFrames.end()) {
			const uint8_t survivor = FirstAliveHumanPeerForTeam(team, std::numeric_limits<uint64_t>::max());
			// H4 §4: a seat inside its reclaim window has not lost its player. With no surviving
			// teammate the relay host plays its units until the holder returns, instead of standing
			// them down to be shot where they stand - the round is held open only while it is alone.
			ownerPeerId = survivor != 0 ? survivor : (IsHoldingSeatForReclaim() ? m_Config.matchConfig.hostPeerId : survivor);
		}
		return ownerPeerId;
	}

	bool NetLockstepCoordinator::IsLocalActor(int64_t actorUniqueID, int actorTeam, bool cpuControlled) const {
		if (m_Config.peerCount == 0 || m_Config.localPeerId == 0) {
			return true;
		}
		return ResolveActorOwner(actorUniqueID, actorTeam, cpuControlled) == m_Config.localPeerId;
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
		// The team's units fall to the next surviving human peer; only an ownerless team stands down,
		// and a seat still inside its reclaim window is not ownerless (the relay host plays it).
		return FirstAliveHumanPeerForTeam(team, frame) == 0 && !IsHoldingSeatForReclaim();
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
		// Ramp-in: a sender contributes nothing before its own first delayed frame.
		if (frame < EffectiveStartOf(peerId)) {
			return false;
		}
		const auto leaveIt = m_PeerLeaveFrames.find(peerId);
		return leaveIt == m_PeerLeaveFrames.end() || frame < leaveIt->second;
	}

	uint16_t NetLockstepCoordinator::PeerInputDelay(uint8_t peerId) const {
		const auto delayIt = m_Config.peerInputDelayFrames.find(peerId);
		return delayIt != m_Config.peerInputDelayFrames.end() ? delayIt->second : m_Config.inputDelayFrames;
	}

	uint64_t NetLockstepCoordinator::EffectiveStartOf(uint8_t peerId) const {
		const auto startIt = m_PeerEffectiveStart.find(peerId);
		return startIt != m_PeerEffectiveStart.end() ? startIt->second : m_Config.startFrame + m_Config.inputDelayFrames;
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
		out << "\"peer_input_delays\":{";
		for (uint8_t peerId = 1; peerId <= m_Config.peerCount; ++peerId) {
			out << (peerId == 1 ? "" : ",") << "\"" << static_cast<int>(peerId) << "\":" << PeerInputDelay(peerId);
		}
		out << "},";
		out << "\"next_frame\":" << m_Stats.nextFrame << ",";
		out << "\"start_packets_sent\":" << m_Stats.startPacketsSent << ",";
		out << "\"start_packets_received\":" << m_Stats.startPacketsReceived << ",";
		out << "\"frame_packets_sent\":" << m_Stats.framePacketsSent << ",";
		out << "\"frame_packets_received\":" << m_Stats.framePacketsReceived << ",";
		out << "\"ignored_session_packets\":" << m_Stats.ignoredSessionPackets << ",";
		out << "\"stale_round_packets\":" << m_Stats.staleRoundPackets << ",";
		out << "\"start_retransmits\":" << m_Stats.startRetransmits << ",";
		out << "\"start_answers\":" << m_Stats.startAnswers << ",";
		out << "\"round_readoptions\":" << m_Stats.roundReadoptions << ",";
		out << "\"start_answers_suppressed\":" << m_Stats.startAnswersSuppressed << ",";
		out << "\"starts_relayed_on_repeat\":" << m_Stats.startsRelayedOnRepeat << ",";
		out << "\"pre_start_frames_buffered\":" << m_Stats.preStartFramesBuffered << ",";
		out << "\"round_id\":" << m_RoundId << ",";
		out << "\"local_controller_frames_sent\":" << m_Stats.localControllerFramesSent << ",";
		out << "\"remote_controller_frames_received\":" << m_Stats.remoteControllerFramesReceived << ",";
		out << "\"remote_controller_frames_accepted\":" << m_Stats.remoteControllerFramesAccepted << ",";
		out << "\"frames_accepted\":" << m_Stats.framesAccepted << ",";
		out << "\"duplicate_frames\":" << m_Stats.duplicateFrames << ",";
		out << "\"out_of_order_frames\":" << m_Stats.outOfOrderFrames << ",";
		out << "\"future_frame_drops\":" << m_Stats.futureFrameDrops << ",";
		out << "\"missing_frame_stalls\":" << m_Stats.missingFrameStalls << ",";
		out << "\"longest_stall_ms\":" << m_Stats.longestStallMs << ",";
		out << "\"last_missing_peers\":\"" << EscapeJson(m_Stats.lastMissingPeers) << "\",";
		out << "\"relay_packets_sent\":" << m_Stats.relayPacketsSent << ",";
		out << "\"relay_send_failures\":" << m_Stats.relaySendFailures << ",";
		out << "\"relay_resends\":" << m_Stats.relayResends << ",";
		out << "\"relay_backlog_peers\":" << m_RelayBacklog.size() << ",";
		out << "\"relay_bytes_sent\":" << m_Stats.relayBytesSent << ",";
		out << "\"largest_relay_packet_bytes\":" << m_Stats.largestRelayPacketBytes << ",";
		out << "\"relay_backlog_bytes\":" << m_Stats.relayBacklogBytes << ",";
		out << "\"observations_carried\":" << m_Stats.observationsCarried << ",";
		out << "\"observations_dropped\":" << m_Stats.observationsDropped << ",";
		out << "\"unresolved_observation_packets\":" << m_Stats.unresolvedObservationPackets << ",";
		out << "\"relay_observation_overflows\":" << m_Stats.relayObservationOverflows << ",";
		out << "\"last_relay_error\":\"" << EscapeJson(m_Stats.lastRelayError) << "\",";
		out << "\"peer_silence_leave_ms\":" << PeerSilenceLeaveMs() << ",";
		out << "\"peers_dropped_silent\":" << m_Stats.peersDroppedSilent << ",";
		out << "\"peers_left\":" << m_PeerLeaveFrames.size() << ",";
		out << "\"peer_leave_frames\":{";
		for (auto it = m_PeerLeaveFrames.begin(); it != m_PeerLeaveFrames.end(); ++it) {
			out << (it == m_PeerLeaveFrames.begin() ? "" : ",") << "\"" << static_cast<int>(it->first) << "\":" << it->second;
		}
		out << "},";
		// Per remote, so a stall says whether this peer stopped being sent frames, stopped receiving
		// them, or received them and refused them.
		out << "\"peers\":{";
		for (auto it = m_Stats.peers.begin(); it != m_Stats.peers.end(); ++it) {
			const NetLockstepPeerStats& peer = it->second;
			out << (it == m_Stats.peers.begin() ? "" : ",") << "\"" << static_cast<int>(it->first) << "\":{"
			    << "\"frame_packets_received\":" << peer.framePacketsReceived
			    << ",\"controller_frames_received\":" << peer.controllerFramesReceived
			    << ",\"frames_contributed\":" << peer.framesContributed
			    << ",\"duplicate_frames\":" << peer.duplicateFrames
			    << ",\"out_of_order_frames\":" << peer.outOfOrderFrames
			    << ",\"future_frame_drops\":" << peer.futureFrameDrops
			    << ",\"stale_round_packets\":" << peer.staleRoundPackets
			    << ",\"pre_start_buffered\":" << peer.preStartBuffered
			    << ",\"relay_packets_sent\":" << peer.relayPacketsSent
			    << ",\"relay_send_failures\":" << peer.relaySendFailures
			    << ",\"relay_resends\":" << peer.relayResends
			    << ",\"relay_bytes_sent\":" << peer.relayBytesSent
			    << ",\"largest_relay_packet_bytes\":" << peer.largestRelayPacketBytes
			    << ",\"relay_backlog_packets\":" << RelayBacklogPackets(it->first)
			    << ",\"highest_target_frame\":" << peer.highestTargetFrame
			    << ",\"last_heard_ms\":" << peer.lastHeardMs << "}";
		}
		out << "},";
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

	bool NetLockstepCoordinator::SendPacket(const NetLockstepPacket& packet, NetTransportLane lane, std::string* error, NetSoundObservationDictionary* dictionary, size_t* outObservationsEncoded, uint8_t onlyPeerId) {
		std::vector<uint8_t> bytes;
		NetLockstepError encodeError;
		if (!NetLockstepCodec::Encode(packet, bytes, &encodeError, dictionary, outObservationsEncoded)) {
			if (error) *error = encodeError.message;
			return false;
		}
		// Send to every remote peer's transport (a set of one in the 2-peer case), or to just the one asked for.
		for (const auto& [peerId, transportId]: m_RemoteTransports) {
			if (onlyPeerId != 0 && peerId != onlyPeerId) {
				continue;
			}
			// Behind an undrained backlog, or this peer's stream would arrive out of order.
			const bool backlogged = lane == m_Config.frameLane && [&] {
				const auto backlogIt = m_RelayBacklog.find(peerId);
				return backlogIt != m_RelayBacklog.end() && !backlogIt->second.empty();
			}();
			std::string sendError;
			if (!backlogged && m_Transport->Send(transportId, lane, bytes, &sendError)) {
				continue;
			}
			// One client's refused send is that peer's problem, not the round's: a seat reclaimed by a
			// newer connection leaves a handle the transport has forgotten, and the host must not end
			// everyone's match on it. Frames take the same retry backlog a refused forward takes, so a
			// brief refusal still arrives in order; a client has one link, so its refusal stays fatal.
			if (!m_RelayHost) {
				if (error) *error = sendError;
				return false;
			}
			if (!backlogged) {
				++m_Stats.relaySendFailures;
				++m_Stats.peers[peerId].relaySendFailures;
				m_Stats.lastRelayError = sendError;
			}
			if (lane == m_Config.frameLane) {
				QueueRelayBacklog(peerId, bytes);
			}
		}
		return true;
	}

	bool NetLockstepCoordinator::IsKnownRemotePeer(uint8_t peerId) const {
		return std::find(m_RemotePeerIds.begin(), m_RemotePeerIds.end(), peerId) != m_RemotePeerIds.end();
	}

	uint8_t NetLockstepCoordinator::LockstepPeerOfTransport(NetPeerId transportPeerId) const {
		for (const auto& [peerId, transportId]: m_RemoteTransports) {
			if (transportId == transportPeerId) {
				return peerId;
			}
		}
		return 0;
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
		size_t observationsEncoded = 0;
		if (!NetLockstepCodec::Encode(packet, bytes, nullptr, &m_ObservationEncodeTables.Exactly(fromPeerId), &observationsEncoded)) {
			return;
		}
		// The table this re-encodes from is the one that just decoded the packet, so every key is already
		// a slot and the forward is never longer than what arrived. If it ever were, the peers behind the
		// relay would commit a smaller table than the host and the desync check would find it.
		if (const NetLockstepFrame* frame = std::get_if<NetLockstepFrame>(&packet.payload); frame && observationsEncoded < frame->observations.size()) {
			++m_Stats.relayObservationOverflows;
			std::cout << "[lockstep] relay of peer " << static_cast<int>(fromPeerId) << "'s frame " << frame->targetFrame
			          << " carried " << observationsEncoded << " of " << frame->observations.size() << " sound observations" << std::endl;
		}
		for (const auto& [peerId, transportId]: m_RemoteTransports) {
			if (peerId == fromPeerId) {
				continue;
			}
			// Behind an undrained backlog, or this peer's stream would arrive out of order.
			const auto backlogIt = m_RelayBacklog.find(peerId);
			if (backlogIt != m_RelayBacklog.end() && !backlogIt->second.empty()) {
				QueueRelayBacklog(peerId, bytes);
				continue;
			}
			NetLockstepPeerStats& peerStats = m_Stats.peers[peerId];
			std::string sendError;
			if (m_Transport->Send(transportId, m_Config.frameLane, bytes, &sendError)) {
				CountRelaySent(peerStats, bytes.size());
				continue;
			}
			// A refused forward was never queued, and on a reliable lane the receiver cannot ask for
			// it again - it would wait on that frame until its own grace ran out. Hold it for retry.
			++m_Stats.relaySendFailures;
			++peerStats.relaySendFailures;
			m_Stats.lastRelayError = sendError;
			std::cout << "[net-match] relay to " << DescribePeer(peerId) << " refused at frame " << m_Stats.nextFrame << ": " << sendError << std::endl;
			QueueRelayBacklog(peerId, bytes);
		}
	}

	// Every forward's cost to the destination's send buffer, so a refusal can be read against the
	// bytes that filled it rather than the packet count.
	void NetLockstepCoordinator::CountRelaySent(NetLockstepPeerStats& peerStats, size_t bytes) {
		++m_Stats.relayPacketsSent;
		++peerStats.relayPacketsSent;
		m_Stats.relayBytesSent += bytes;
		peerStats.relayBytesSent += bytes;
		const uint32_t size = static_cast<uint32_t>(bytes);
		m_Stats.largestRelayPacketBytes = std::max(m_Stats.largestRelayPacketBytes, size);
		peerStats.largestRelayPacketBytes = std::max(peerStats.largestRelayPacketBytes, size);
	}

	uint64_t NetLockstepCoordinator::RelayBacklogBytes() const {
		uint64_t bytes = 0;
		for (const auto& [peerId, backlog]: m_RelayBacklog) {
			for (const std::vector<uint8_t>& packet : backlog) {
				bytes += packet.size();
			}
		}
		return bytes;
	}

	uint32_t NetLockstepCoordinator::RelayBacklogPackets(uint8_t peerId) const {
		const auto backlog = m_RelayBacklog.find(peerId);
		return backlog == m_RelayBacklog.end() ? 0 : static_cast<uint32_t>(backlog->second.size());
	}

	void NetLockstepCoordinator::QueueRelayBacklog(uint8_t peerId, const std::vector<uint8_t>& bytes) {
		std::deque<std::vector<uint8_t>>& backlog = m_RelayBacklog[peerId];
		// Past the skew window this peer could never catch up even if the transport freed up.
		if (backlog.size() >= NetLockstepCodec::c_MaxFutureFrameSkew) {
			m_UnreachablePeers.insert(peerId);
			return;
		}
		backlog.push_back(bytes);
		UpdateRelayBacklogBytes();
	}

	void NetLockstepCoordinator::FlushRelayBacklog(uint64_t nowMs) {
		for (auto it = m_RelayBacklog.begin(); it != m_RelayBacklog.end();) {
			const uint8_t peerId = it->first;
			const auto transportIt = m_RemoteTransports.find(peerId);
			if (transportIt == m_RemoteTransports.end()) {
				m_RelayBacklogSinceMs.erase(peerId);
				it = m_RelayBacklog.erase(it);
				continue;
			}
			while (!it->second.empty()) {
				std::string sendError;
				if (!m_Transport->Send(transportIt->second, m_Config.frameLane, it->second.front(), &sendError)) {
					m_Stats.lastRelayError = sendError;
					break;
				}
				++m_Stats.relayResends;
				++m_Stats.peers[peerId].relayResends;
				CountRelaySent(m_Stats.peers[peerId], it->second.front().size());
				it->second.pop_front();
			}
			if (it->second.empty()) {
				m_RelayBacklogSinceMs.erase(peerId);
				it = m_RelayBacklog.erase(it);
				continue;
			}
			const uint64_t since = m_RelayBacklogSinceMs.emplace(peerId, nowMs).first->second;
			if (m_Config.timeoutMs > 0 && nowMs >= since && nowMs - since >= PeerSilenceLeaveMs()) {
				m_UnreachablePeers.insert(peerId);
			}
			++it;
		}
		UpdateRelayBacklogBytes();
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
				// A superseded incarnation's socket finally closing says nothing about the seat: its live
				// holder is another transport, which the resync brings into the round. The dead handle
				// leaves the round with it, or the next send names a peer the transport has forgotten.
				if (m_RelayHost && lockstepPeer != 0 && SeatStateOf(lockstepPeer, event.peerId).fencedTransport) {
					m_RemoteTransports.erase(lockstepPeer);
					++m_Stats.ignoredAdmissionFaults;
					break;
				}
				// The relay host adjudicates a client drop as a leave at the first frame it has no data
				// for, so the survivors keep playing; a host drop still ends the match. The relayed
				// frames precede this notice on the reliable lane, so no survivor learns of the leave
				// before it holds everything the leave references.
				if (m_RelayHost && lockstepPeer != 0 && m_State == NetLockstepState::Running && m_Stats.nextFrame > 0) {
					ApplyPeerLeave(lockstepPeer, FirstFrameWithout(lockstepPeer), "connection lost", nowMs, false);
					break;
				}
				// A transport peer outside the round — a leaver's stale socket finally timing out, a
				// rejected joiner's half-open connection — cannot invalidate the match.
				if (lockstepPeer == 0) {
					break;
				}
				Fail(NetLockstepStopReason::PeerDisconnected,
				     m_Stats.nextFrame,
				     DescribePeer(lockstepPeer) + " disconnected" + (event.reason.empty() ? "" : ": " + event.reason));
				break;
			}
			case NetTransportEventType::LocalTransportFault:
				// Our own transport pump broke - genuinely fatal, both host and client.
				Fail(NetLockstepStopReason::InternalError, m_Stats.nextFrame, event.reason.empty() ? "local transport fault" : event.reason);
				break;
			case NetTransportEventType::ConnectionFailed:
			case NetTransportEventType::TransportError:
				// A per-connection fault. On the relay host it is admission traffic (an unauthenticated
				// joiner's half-open connection failing) and must never stop the running match; a
				// committed peer dropping arrives as PeerDisconnected. A client's lone link is still fatal.
				if (m_RelayHost) {
					++m_Stats.ignoredAdmissionFaults;
				} else {
					Fail(NetLockstepStopReason::InternalError, m_Stats.nextFrame, event.reason.empty() ? "transport error" : event.reason);
				}
				break;
			case NetTransportEventType::PacketReceived: {
				// A frame's observation slots only mean anything against its sender's table. The relay host
				// picks that table by the transport the bytes actually came in on, so a peer claiming to be
				// another can only ever disturb its own; an unbound transport gets no table at all.
				NetSoundObservationTables* tables = &m_ObservationDecodeTables;
				m_ObservationDecodeTables.transportSender = 0;
				if (m_RelayHost) {
					const uint8_t transportSender = LockstepPeerOfTransport(event.peerId);
					m_ObservationDecodeTables.transportSender = transportSender;
					tables = transportSender != 0 ? &m_ObservationDecodeTables : nullptr;
				}
				const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(event.bytes.data(), event.bytes.size(), ControllerFrame::c_Version, tables);
				if (!decoded.ok) {
					if (decoded.error.code == NetLockstepErrorCode::UnboundObservationSlot ||
					    decoded.error.code == NetLockstepErrorCode::ObservationBindingGap) {
						// A transport with no table of its own is a superseded incarnation or an unauthenticated
						// joiner still talking; its slots were never ours to resolve and it is admission traffic,
						// not a fault. From a peer of this round it means a binding this peer can never be told
						// again, so the packet is dropped rather than read as the wrong sound.
						if (tables == nullptr) {
							++m_Stats.ignoredAdmissionFaults;
							return;
						}
						if (m_Stats.unresolvedObservationPackets == 0) {
							std::cout << "[lockstep] dropped a frame over its sound observations: " << decoded.error.message << std::endl;
						}
						++m_Stats.unresolvedObservationPackets;
						return;
					}
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
					// A committed peer's stream that will not decode is a genuine protocol error worth
					// failing on. An UNBOUND transport (an unauthenticated joiner) must never stop the
					// match with garbage; only the relay host tells bound from unbound (clients receive
					// every remote through the host, so they trust their single source - as SenderOwnsTransport).
					if (!m_RelayHost || UsesTransportPeer(event.peerId)) {
						Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, decoded.error.message);
					} else {
						++m_Stats.ignoredAdmissionFaults;
					}
					return;
				}
				HandlePacket(decoded.packet, nowMs, event.peerId);
				break;
			}
		}
	}

	void NetLockstepCoordinator::HandlePacket(const NetLockstepPacket& packet, uint64_t nowMs, NetPeerId fromTransport) {
		// Any traffic proves the sender is alive, whatever the packet turns out to say; the host's
		// drop adjudication runs off this and nothing else.
		const uint8_t sender = std::visit(Overloaded{
			[](const NetLockstepStart& start) { return start.localPeerId; },
			[](const NetLockstepFrame& frame) { return frame.senderPeerId; },
			[](const NetLockstepAck& ack) { return ack.senderPeerId; },
			[](const NetLockstepStop& stop) { return stop.senderPeerId; },
			[](const NetLockstepChecksum& checksum) { return checksum.senderPeerId; },
		}, packet.payload);
		if (IsKnownRemotePeer(sender) && SenderOwnsTransport(sender, fromTransport)) {
			m_PeerLastHeardMs[sender] = nowMs;
			m_Stats.peers[sender].lastHeardMs = nowMs;
		}
		std::visit(Overloaded{
			[&](const NetLockstepStart& start) { HandleStart(start, nowMs, fromTransport); },
			[&](const NetLockstepFrame& frame) { HandleFrame(frame, nowMs, fromTransport); },
			[&](const NetLockstepAck&) {},
			[&](const NetLockstepStop& stop) { HandleStop(stop, nowMs, fromTransport); },
			[&](const NetLockstepChecksum& checksum) { HandleChecksum(checksum, fromTransport); },
		}, packet.payload);
	}

	bool NetLockstepCoordinator::SenderOwnsTransport(uint8_t claimedPeerId, NetPeerId fromTransport) const {
		// Clients receive every remote's traffic through the host relay, so the transport is always the
		// host's; only the relay host, which receives each remote directly, can bind claim to connection.
		if (!m_RelayHost) {
			return true;
		}
		const auto it = m_RemoteTransports.find(claimedPeerId);
		// Reject only a KNOWN mapping that is violated; an absent mapping stays gated by IsKnownRemotePeer
		// as before, so this never drops on a path that doesn't track transports.
		return it == m_RemoteTransports.end() || it->second == fromTransport;
	}

	void NetLockstepCoordinator::HandleStart(const NetLockstepStart& start, uint64_t nowMs, NetPeerId fromTransport) {
		++m_Stats.startPacketsReceived;
		if (!SenderOwnsTransport(start.localPeerId, fromTransport)) {
			std::cout << "[lockstep] dropped a start claiming peer " << static_cast<int>(start.localPeerId) << " from the wrong transport" << std::endl;
			return;
		}
		// The peer we take our round from has started another one and ours has committed nothing, so that
		// is the round we are in. Bounded to our own start frame, so a straggler from before a resync still
		// takes the rule below, and to the peer that owns the transport it came in on: starts ride the
		// reliable ordered lane, so a later start from that peer is a newer one, never an older one.
		const bool followTheAuthority = start.roundId != 0 && m_RoundId != 0 && start.roundId != m_RoundId &&
		                                start.startFrame == m_Config.startFrame && !HasCommittedAFrame() &&
		                                (m_State == NetLockstepState::WaitingForStart || m_State == NetLockstepState::Running) &&
		                                IsRoundAuthority(start.localPeerId, fromTransport);
		// Another round's start (a late one from before a resync) is not this round's handshake; before this
		// peer knows its round, a start for a different frame is that straggler too.
		if (!followTheAuthority && start.roundId != 0 && ((m_RoundId != 0 && start.roundId != m_RoundId) || (m_RoundId == 0 && start.startFrame != m_Config.startFrame))) {
			++m_Stats.staleRoundPackets;
			return;
		}
		if (!StartMatchesConfig(start)) {
			// A start we would only have taken by following its round belongs to another round after all,
			// and another round's start is ignored here - it was never this round's handshake to fail on.
			if (followTheAuthority) {
				++m_Stats.staleRoundPackets;
				std::cout << "[lockstep] ignored a start of round " << start.roundId << " that disagrees with this round's setup" << std::endl;
				return;
			}
			Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "lockstep start mismatch");
			return;
		}
		if (m_RoundId == 0 && start.roundId != 0) {
			m_RoundId = start.roundId;
			m_ObservationDecodeTables.roundId = m_RoundId;
		} else if (followTheAuthority) {
			ReadoptRound(start.roundId, nowMs);
		}
		const bool firstFromThisPeer = m_RemoteStartsReceived.insert(start.localPeerId).second;
		if (firstFromThisPeer) {
			m_RemoteStarts[start.localPeerId] = start;
			RelayToOtherRemotes({start}, start.localPeerId);
		} else {
			AnswerRepeatedStart(start.localPeerId, nowMs);
		}
		if (m_State == NetLockstepState::WaitingForStart && AllRemoteStartsReceived()) {
			m_State = NetLockstepState::Running;
			m_WaitingFrame = std::numeric_limits<uint64_t>::max();
		}
		if (firstFromThisPeer) {
			FlushPreStart(start.localPeerId, nowMs);
		}
	}

	void NetLockstepCoordinator::HandleFrame(const NetLockstepFrame& frame, uint64_t nowMs, NetPeerId fromTransport, bool relay) {
		++m_Stats.framePacketsReceived;
		NetLockstepPeerStats& peerStats = m_Stats.peers[frame.senderPeerId];
		++peerStats.framePacketsReceived;
		if (!SenderOwnsTransport(frame.senderPeerId, fromTransport)) {
			std::cout << "[lockstep] dropped a frame claiming peer " << static_cast<int>(frame.senderPeerId) << " from the wrong transport" << std::endl;
			return;
		}
		if (frame.roundId != 0 && m_RoundId != 0 && frame.roundId != m_RoundId) {
			++m_Stats.staleRoundPackets;
			++peerStats.staleRoundPackets;
			return;
		}
		if (!IsKnownRemotePeer(frame.senderPeerId)) {
			Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "lockstep frame sender mismatch: peer " + std::to_string(frame.senderPeerId) + " is not a remote");
			return;
		}
		peerStats.highestTargetFrame = std::max(peerStats.highestTargetFrame, frame.targetFrame);
		// Forward every frame this peer's slot table has taken in, before any rule of ours drops it: what
		// the peers behind the relay decode has to be the same sequence, or their tables fall behind and a
		// later slot reference means nothing to them. They apply the same rules to it that we do.
		if (relay) {
			RelayToOtherRemotes({frame}, frame.senderPeerId);
		}
		// After a round restart a peer's first frames can outrun its start; hold them until it lands.
		if (m_RemoteStartsReceived.find(frame.senderPeerId) == m_RemoteStartsReceived.end()) {
			std::deque<NetLockstepFrame>& held = m_PreStartFrames[frame.senderPeerId];
			if (held.size() >= NetLockstepCodec::c_MaxFutureFrameSkew) {
				held.pop_front();
			}
			held.push_back(frame);
			++m_Stats.preStartFramesBuffered;
			++peerStats.preStartBuffered;
			return;
		}
		// A sender's frames never target its own delay window; one that does is a broken build.
		if (frame.targetFrame < EffectiveStartOf(frame.senderPeerId)) {
			Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "lockstep frame targets the sender's delay window");
			return;
		}
		// Check staleness before touching the map, or a stale packet leaks an empty bucket forever.
		if (frame.targetFrame < m_Stats.nextFrame) {
			++m_Stats.duplicateFrames;
			++peerStats.duplicateFrames;
			return;
		}
		// Drop absurd future frames so a misbehaving peer cannot grow the per-frame maps without bound.
		if (frame.targetFrame > m_Stats.nextFrame + NetLockstepCodec::c_MaxFutureFrameSkew) {
			++m_Stats.futureFrameDrops;
			++peerStats.futureFrameDrops;
			std::cout << "[lockstep] dropped a frame targeting " << frame.targetFrame << " far past the committed frame " << m_Stats.nextFrame << std::endl;
			return;
		}
		auto& peerFrames = m_RemoteFrames[frame.targetFrame];
		if (peerFrames.find(frame.senderPeerId) != peerFrames.end()) {
			++m_Stats.duplicateFrames;
			++peerStats.duplicateFrames;
			return;
		}
		if (frame.targetFrame > m_Stats.nextFrame) {
			++m_Stats.outOfOrderFrames;
			++peerStats.outOfOrderFrames;
		}
		m_Stats.remoteControllerFramesReceived += frame.frames.size();
		peerStats.controllerFramesReceived += frame.frames.size();
		peerFrames[frame.senderPeerId] = frame.frames;
		m_PeersPlayedThisRound.insert(frame.senderPeerId);
		if (!frame.commands.empty()) {
			m_RemoteCommands[frame.targetFrame][frame.senderPeerId] = frame.commands;
		}
		if (!frame.observations.empty()) {
			m_RemoteObservations[frame.targetFrame][frame.senderPeerId] = frame.observations;
		}
		AdvanceReadyFrames(nowMs);
	}

	void NetLockstepCoordinator::HandleStop(const NetLockstepStop& stop, uint64_t nowMs, NetPeerId fromTransport) {
		if (!SenderOwnsTransport(stop.senderPeerId, fromTransport)) {
			std::cout << "[lockstep] dropped a stop claiming peer " << static_cast<int>(stop.senderPeerId) << " from the wrong transport" << std::endl;
			return;
		}
		if (!IsKnownRemotePeer(stop.senderPeerId)) return;
		if (m_DeferStops && stop.reason == NetLockstepStopReason::Complete &&
		    (!m_LastCompletedSimulationTick || *m_LastCompletedSimulationTick + 1 < stop.frame)) {
			if (!m_PendingCompleteStop || stop.frame < m_PendingCompleteStop->frame) m_PendingCompleteStop = stop;
			return;
		}
		if (m_DeferStops && stop.senderPeerId != m_Config.matchConfig.hostPeerId &&
		    (stop.reason == NetLockstepStopReason::Desync || stop.reason == NetLockstepStopReason::ResyncRequested)) {
			ScheduleRecoveryStop(stop.reason, stop.frame, stop.message);
			return;
		}
		if (stop.reason == NetLockstepStopReason::PeerLeft) {
			if (IsKnownRemotePeer(stop.senderPeerId)) {
				ApplyPeerLeave(stop.senderPeerId, stop.frame, stop.message, nowMs, true);
			}
			return;
		}
		m_Stats.timeoutReason = std::string(NetLockstepCodec::StopReasonName(stop.reason)) + ":" + stop.message;
		m_State = stop.reason == NetLockstepStopReason::Complete ? NetLockstepState::Stopped : NetLockstepState::Failed;
	}

	void NetLockstepCoordinator::SetSeatStateSource(NetLockstepSeatState (*source)(void*, uint8_t, NetPeerId), void* context) {
		m_SeatStateSource = source;
		m_SeatStateContext = context;
	}

	NetLockstepSeatState NetLockstepCoordinator::SeatStateOf(uint8_t peerId, NetPeerId transportPeerId) const {
		return m_SeatStateSource ? m_SeatStateSource(m_SeatStateContext, peerId, transportPeerId) : NetLockstepSeatState{};
	}

	// The match service pumps us with its own lock held, and the ownership census that pump takes asks
	// who owns a left seat's units - so asking the service back from there re-locks it on its own
	// thread. The answer is resolved here instead, from the tick, which also fixes it for the whole
	// tick: ownership cannot change under a subsystem halfway through one.
	void NetLockstepCoordinator::RefreshLeftSeatHolds() {
		m_LeftSeatsHeld.clear();
		for (const auto& left: m_PeerLeaveFrames) {
			if (SeatStateOf(left.first, c_InvalidNetPeerId).heldForReclaim) {
				m_LeftSeatsHeld.insert(left.first);
			}
		}
	}

	bool NetLockstepCoordinator::AnyLeftSeatHeld() const {
		return !m_LeftSeatsHeld.empty();
	}

	bool NetLockstepCoordinator::IsHoldingSeatForReclaim() const {
		return m_State == NetLockstepState::Running && !m_RemotePeerIds.empty() &&
		       m_PeerLeaveFrames.size() >= m_RemotePeerIds.size() && AnyLeftSeatHeld();
	}

	void NetLockstepCoordinator::EndRoundIfNobodyIsComingBack() {
		if (m_State != NetLockstepState::Running || m_RemotePeerIds.empty() ||
		    m_PeerLeaveFrames.size() < m_RemotePeerIds.size() || AnyLeftSeatHeld()) {
			return;
		}
		// Nobody left to play with.
		m_Stats.timeoutReason = std::string(NetLockstepCodec::StopReasonName(NetLockstepStopReason::PeerLeft)) + ":" + m_LastLeaveMessage;
		m_State = NetLockstepState::Stopped;
	}

	// A leave is deterministic by construction: no survivor can advance to the leaver's first missing
	// frame without processing this, so every peer drops the requirement at the same tick.
	void NetLockstepCoordinator::ApplyPeerLeave(uint8_t peerId, uint64_t firstFrameWithout, const std::string& message, uint64_t nowMs, bool announced) {
		if (!m_PeerLeaveFrames.emplace(peerId, firstFrameWithout).second) {
			return;
		}
		RefreshLeftSeatHolds();
		std::cout << "[net-match] " << DescribePeer(peerId) << " left the match at frame " << firstFrameWithout << " (" << message << ")" << std::endl;
		NetLockstepStop notice;
		notice.senderPeerId = peerId;
		notice.reason = NetLockstepStopReason::PeerLeft;
		notice.frame = firstFrameWithout;
		notice.message = message;
		RelayToOtherRemotes({notice}, peerId);
		m_RemoteTransports.erase(peerId);
		m_LastLeaveMessage = message;
		// A holder that DROPPED with a live ticket is not gone yet: the round plays on exactly as it does
		// with survivors present, and ends only once the last held seat's reclaim window closes. A peer
		// that announced its leave said it is not coming back, so that still ends the match at once.
		if (m_PeerLeaveFrames.size() >= m_RemotePeerIds.size() && (announced || !AnyLeftSeatHeld())) {
			// Nobody left to play with.
			m_Stats.timeoutReason = std::string(NetLockstepCodec::StopReasonName(NetLockstepStopReason::PeerLeft)) + ":" + message;
			m_State = NetLockstepState::Stopped;
			return;
		}
		AdvanceReadyFrames(nowMs);
	}

	uint64_t NetLockstepCoordinator::FirstFrameWithout(uint8_t peerId) const {
		uint64_t frame = m_Stats.nextFrame;
		while (true) {
			const auto it = m_RemoteFrames.find(frame);
			if (it == m_RemoteFrames.end() || it->second.find(peerId) == it->second.end()) {
				return frame;
			}
			++frame;
		}
	}

	// Only the relay host may call a peer gone: every survivor has to drop the requirement at the same
	// frame, and it can only do that from one relayed notice. The transport's own disconnect is no use
	// here - it waits on the dead peer's process, which outlasts every survivor's missing-frame grace,
	// so the star's other clients kill themselves waiting for someone the host knows nothing about yet.
	// A 2-peer host has no survivor to protect and keeps failing with MissingFrameTimeout.
	void NetLockstepCoordinator::AdjudicateSilentPeers(uint64_t nowMs) {
		if (!m_RelayHost || m_State != NetLockstepState::Running || m_RemotePeerIds.size() < 2 || m_Config.timeoutMs == 0) {
			return;
		}
		// Only judge a frame we have produced for ourselves: when OUR pipeline is the stalled one, the
		// clients are not the ones at fault.
		if (m_LastQueuedTargetFrame == std::numeric_limits<uint64_t>::max() || m_LastQueuedTargetFrame < m_Stats.nextFrame) {
			return;
		}
		const uint64_t budget = PeerSilenceLeaveMs();
		const auto remoteIt = m_RemoteFrames.find(m_Stats.nextFrame);
		std::vector<uint8_t> silent;
		for (uint8_t peerId: m_RemotePeerIds) {
			// Only a peer that is actually blocking the round; a quiet peer nobody waits on is fine.
			if (!IsRemoteRequiredForFrame(peerId, m_Stats.nextFrame)) {
				continue;
			}
			if (remoteIt != m_RemoteFrames.end() && remoteIt->second.find(peerId) != remoteIt->second.end()) {
				continue;
			}
			const auto heardIt = m_PeerLastHeardMs.find(peerId);
			if (heardIt == m_PeerLastHeardMs.end() || nowMs < heardIt->second || nowMs - heardIt->second < budget) {
				continue;
			}
			silent.push_back(peerId);
		}
		// If EVERY remaining peer looks silent at once, the fault is far more likely ours - our own
		// transport, or a stall the whole match shares - than all of theirs. Leave it to the ordinary
		// missing-frame timeout rather than emptying the round.
		size_t live = 0;
		for (uint8_t peerId: m_RemotePeerIds) {
			live += m_PeerLeaveFrames.find(peerId) == m_PeerLeaveFrames.end() ? 1 : 0;
		}
		if (silent.size() >= live) {
			return;
		}
		for (uint8_t peerId: silent) {
			if (m_State != NetLockstepState::Running) {
				break;
			}
			++m_Stats.peersDroppedSilent;
			ApplyPeerLeave(peerId, FirstFrameWithout(peerId), "no frames for " + std::to_string(budget) + "ms", nowMs, false);
		}
	}

	void NetLockstepCoordinator::DropUnreachablePeers(uint64_t nowMs) {
		if (m_UnreachablePeers.empty() || m_State != NetLockstepState::Running) {
			return;
		}
		std::set<uint8_t> unreachable;
		unreachable.swap(m_UnreachablePeers);
		for (uint8_t peerId: unreachable) {
			if (m_State != NetLockstepState::Running || m_PeerLeaveFrames.find(peerId) != m_PeerLeaveFrames.end()) {
				continue;
			}
			ApplyPeerLeave(peerId, FirstFrameWithout(peerId), "unreachable: " + m_Stats.lastRelayError, nowMs, false);
		}
	}

	void NetLockstepCoordinator::AdvanceReadyFrames(uint64_t nowMs) {
		if (m_State != NetLockstepState::Running) {
			return;
		}
		while (true) {
			// The LOCAL peer ramps in like any sender: its first queued input targets its own delay,
			// so earlier committed frames legitimately carry no local entry.
			const auto localIt = m_LocalFrames.find(m_Stats.nextFrame);
			if (localIt == m_LocalFrames.end() && m_Stats.nextFrame >= EffectiveStartOf(m_Config.localPeerId)) {
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
			if (localIt != m_LocalFrames.end()) {
				ready.localFrames = std::move(localIt->second);
			}
			// Merge every remote peer's frames in ascending peerId order (std::map iteration) so every
			// peer builds the byte-identical apply set. This is the one N-peer determinism-sensitive spot.
			if (remoteIt != m_RemoteFrames.end()) {
				for (auto& [peerId, frames]: remoteIt->second) {
					++m_Stats.peers[peerId].framesContributed;
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
			if (auto localObsIt = m_LocalObservations.find(ready.frame); localObsIt != m_LocalObservations.end()) {
				ready.localObservations = std::move(localObsIt->second);
				m_LocalObservations.erase(localObsIt);
			}
			if (auto remoteObsIt = m_RemoteObservations.find(ready.frame); remoteObsIt != m_RemoteObservations.end()) {
				for (auto& [peerId, observations]: remoteObsIt->second) {
					ready.remoteObservations.insert(ready.remoteObservations.end(), std::make_move_iterator(observations.begin()), std::make_move_iterator(observations.end()));
				}
				m_RemoteObservations.erase(remoteObsIt);
			}
			m_Stats.remoteControllerFramesAccepted += ready.remoteFrames.size();
			if (localIt != m_LocalFrames.end()) {
				m_LocalFrames.erase(localIt);
			}
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
		if (nowMs >= m_WaitStartMs && nowMs - m_WaitStartMs > m_Stats.longestStallMs) {
			m_Stats.longestStallMs = nowMs - m_WaitStartMs;
			m_Stats.lastMissingPeers = DescribeMissingPeers();
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
