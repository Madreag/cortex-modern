#include "NetLockstep.h"

#include "NetActorOwnership.h"
#include "NetAuthCrypto.h"
#include "NetLobbyProtocol.h"
#include "NetProtocol.h"
#include "NetResyncState.h"
#include "NetWorldJoin.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

namespace RTE {

	uint64_t NetLockstepNowMs() {
		static const std::chrono::steady_clock::time_point base = std::chrono::steady_clock::now();
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - base).count());
	}

	uint64_t NetLockstepSharedClockMs() {
		// The machine's monotonic clock, the same in every process on it, for diagnostics that compare peers.
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	namespace {
		constexpr uint64_t c_StartRetransmitMs = 250;
		// A round configured without an answer budget still bounds a capture park.
		constexpr uint64_t c_DefaultCaptureParkBudgetMs = 500;
		constexpr uint32_t c_RecoveryInputMagic = 0x314e4952;
		constexpr uint16_t c_RecoveryInputVersion = 5;

		std::optional<uint64_t> TestFrameFromEnvironment(const char* name) {
			const char* text = std::getenv(name);
			if (!text) return std::nullopt;
			uint64_t frame;
			const char* end = text + std::strlen(text);
			const auto parsed = std::from_chars(text, end, frame);
			return parsed.ec == std::errc{} && parsed.ptr == end ? std::optional<uint64_t>(frame) : std::nullopt;
		}

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

		uint64_t DoubleToBitsLE(double value) {
			uint64_t bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			return bits;
		}

		double DoubleFromBitsLE(uint64_t bits) {
			double value = 0;
			std::memcpy(&value, &bits, sizeof(value));
			return value;
		}

		bool AppendBinaryString(std::vector<uint8_t>& out, const std::string& value, size_t maxBytes, const char* fieldName, NetLockstepError* error) {
			if (value.size() > maxBytes || value.size() > std::numeric_limits<uint16_t>::max()) {
				SetError(error, NetLockstepErrorCode::StringTooLong, out.size(), std::string(fieldName) + " exceeds max encoded length");
				return false;
			}
			AppendU16LE(out, static_cast<uint16_t>(value.size()));
			out.insert(out.end(), value.begin(), value.end());
			return true;
		}

		class ByteReader {
		public:
			ByteReader(const uint8_t* data, size_t size) : m_Data(data), m_Size(size) {}

			size_t Offset() const { return m_Offset; }
			size_t Remaining() const { return m_Size - m_Offset; }
			bool AtEnd() const { return m_Offset == m_Size; }
			const uint8_t* Current() const { return m_Data + m_Offset; }

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

		bool ReadBinaryString(ByteReader& reader, std::string& out, size_t maxBytes, const char* fieldName, NetLockstepError* error) {
			uint16_t length = 0;
			const size_t lengthOffset = reader.Offset();
			if (!reader.ReadU16LE(length)) {
				SetError(error, NetLockstepErrorCode::TruncatedPayload, lengthOffset, std::string(fieldName) + " length is truncated");
				return false;
			}
			if (length > maxBytes) {
				SetError(error, NetLockstepErrorCode::StringTooLong, lengthOffset, std::string(fieldName) + " exceeds max encoded length");
				return false;
			}
			const uint8_t* data = nullptr;
			if (!reader.ReadBytes(data, length)) {
				SetError(error, NetLockstepErrorCode::TruncatedPayload, reader.Offset(), std::string(fieldName) + " data is truncated");
				return false;
			}
			out.assign(reinterpret_cast<const char*>(data), length);
			return true;
		}

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

		bool IsKnownStopReason(NetLockstepStopReason reason) {
			switch (reason) {
				case NetLockstepStopReason::Complete:
				case NetLockstepStopReason::MissingFrameTimeout:
				case NetLockstepStopReason::Desync:
				case NetLockstepStopReason::ProtocolError:
				case NetLockstepStopReason::PeerDisconnected:
				case NetLockstepStopReason::InternalError:
				case NetLockstepStopReason::PeerLeft:
				case NetLockstepStopReason::ResyncRequested:
				case NetLockstepStopReason::PeerDropped:
				case NetLockstepStopReason::Reclaimed:
				case NetLockstepStopReason::Substituted:
				case NetLockstepStopReason::PeerRemoved:
				case NetLockstepStopReason::Expired:
					return true;
			}
			return false;
		}

		bool ReadStopReason(ByteReader& reader, NetLockstepStopReason& out, NetLockstepError* error) {
			uint16_t rawReason = 0;
			if (!ReadOrTruncated(reader.ReadU16LE(rawReason), reader, error, "stop reason")) {
				return false;
			}
			if (IsKnownStopReason(static_cast<NetLockstepStopReason>(rawReason))) {
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
			const auto knownDevice = [](uint8_t deviceClass) { return deviceClass < static_cast<uint8_t>(Controller::WireDeviceClass::Count); };
			if (!knownDevice(payload.deviceClass) || !std::all_of(payload.peerDeviceClasses.begin(), payload.peerDeviceClasses.end(), knownDevice)) {
				SetError(error, NetLockstepErrorCode::InvalidValue, 0, "device class is out of range");
				return false;
			}
			if (payload.agreedStartRecord) {
				const uint32_t validPeers = payload.peerCount == 32 ? UINT32_MAX : ((uint32_t{1} << payload.peerCount) - 1U);
				if (payload.agreedFirstFrame < payload.startFrame || payload.agreedEffectiveStartFrame < payload.agreedFirstFrame ||
				    (payload.publishedPeerMask & ~validPeers) != 0 || (payload.heldPeerMask & ~validPeers) != 0 ||
				    (payload.publishedPeerMask & payload.heldPeerMask) != 0) {
					SetError(error, NetLockstepErrorCode::InvalidValue, 0, "invalid agreed start boundary");
					return false;
				}
				for (uint8_t peer = 1; peer <= payload.peerCount; ++peer) {
					if (payload.peerEffectiveStartFrames[peer - 1] < payload.agreedFirstFrame ||
					    payload.peerInputDelays[peer - 1] > NetLockstepCodec::c_MaxInputDelayFrames) {
						SetError(error, NetLockstepErrorCode::InvalidValue, 0, "agreed peer start precedes the boundary");
						return false;
					}
				}
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
			AppendU8(out, payload.resumeFromSnapshot ? 1 : 0);
			if (!AppendString(out, payload.scenario, NetLockstepCodec::c_MaxScenarioBytes, "scenario", error) ||
			    !AppendString(out, payload.ownershipPolicy, NetLockstepCodec::c_MaxOwnershipPolicyBytes, "ownership_policy", error)) {
				return false;
			}
			// The startup reading rides one past itself so that a publication is a fact on the wire and a
			// machine that restarts in 0 ms still publishes: zero means nothing was measured yet.
			AppendU32LE(out, payload.startupPublished && payload.activityRestartMs < UINT32_MAX ? payload.activityRestartMs + 1 : 0);
			AppendU8(out, payload.deviceClass);
			if (payload.agreedStartRecord) {
				AppendU8(out, 1);
				AppendU64LE(out, payload.agreedFirstFrame);
				AppendU64LE(out, payload.agreedEffectiveStartFrame);
				AppendU64LE(out, payload.agreedDeadlineMs);
				AppendU32LE(out, payload.publishedPeerMask);
				AppendU32LE(out, payload.heldPeerMask);
				for (uint64_t frame: payload.peerEffectiveStartFrames) AppendU64LE(out, frame);
				for (uint32_t park: payload.peerStartupParks) AppendU32LE(out, park);
				for (uint16_t delay: payload.peerInputDelays) AppendU16LE(out, delay);
				for (uint8_t deviceClass: payload.peerDeviceClasses) AppendU8(out, deviceClass);
			}
			return true;
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

		size_t ValueObservationCost(const NetValueObservation& observation) {
			size_t bytes = VarU64Size(observation.objectUID) + VarU64Size(observation.tick) + VarU64Size(observation.ordinal) + 2;
			bytes += 2 + observation.key.size();
			if (observation.op == 0) {
				bytes += observation.mapKind == 0 ? 8 : (2 + observation.stringValue.size());
			}
			return bytes;
		}

		bool ValidateValueObservation(const NetValueObservation& observation, size_t offset, NetLockstepError* error) {
			if (observation.mapKind > 1 || observation.op > 1) {
				SetError(error, NetLockstepErrorCode::InvalidValue, offset, "value observation map or op is invalid");
				return false;
			}
			if (observation.key.size() > NetLockstepCodec::c_MaxValueKeyBytes) {
				SetError(error, NetLockstepErrorCode::StringTooLong, offset, "value observation key exceeds max encoded length");
				return false;
			}
			if (observation.stringValue.size() > NetLockstepCodec::c_MaxValueStringBytes) {
				SetError(error, NetLockstepErrorCode::StringTooLong, offset, "value observation string exceeds max encoded length");
				return false;
			}
			return true;
		}

		bool WriteValueObservation(const NetValueObservation& observation, std::vector<uint8_t>& out, NetLockstepError* error) {
			if (!ValidateValueObservation(observation, out.size(), error)) {
				return false;
			}
			AppendVarU64(out, observation.objectUID);
			AppendVarU64(out, observation.tick);
			AppendVarU64(out, observation.ordinal);
			AppendU8(out, observation.mapKind);
			AppendU8(out, observation.op);
			if (!AppendBinaryString(out, observation.key, NetLockstepCodec::c_MaxValueKeyBytes, "value_observation_key", error)) {
				return false;
			}
			if (observation.op != 0) {
				return true;
			}
			if (observation.mapKind == 0) {
				AppendU64LE(out, DoubleToBitsLE(observation.numberValue));
				return true;
			}
			return AppendBinaryString(out, observation.stringValue, NetLockstepCodec::c_MaxValueStringBytes, "value_observation_string", error);
		}

		bool AppendValueObservations(const std::vector<NetValueObservation>& observations, std::vector<uint8_t>& out, size_t* outEncoded, NetLockstepError* error, bool recovery, bool writeEmpty = false) {
			if (observations.empty()) {
				if (writeEmpty) {
					AppendU16LE(out, 0);
				}
				if (outEncoded) {
					*outEncoded = 0;
				}
				return true;
			}
			if (observations.size() > NetLockstepCodec::c_MaxObservationsPerPacket) {
				SetError(error, NetLockstepErrorCode::PayloadTooLarge, out.size(), "frame packet has too many value observations");
				return false;
			}
			for (const NetValueObservation& observation: observations) {
				if (!ValidateValueObservation(observation, out.size(), error)) {
					return false;
				}
			}
			const size_t countOffset = out.size();
			AppendU16LE(out, 0);
			const size_t blockStart = out.size();
			const size_t budget = recovery ? std::numeric_limits<size_t>::max()
			                               : (out.size() < NetLockstepCodec::c_MaxPayloadBytes ? NetLockstepCodec::c_MaxPayloadBytes - out.size() : size_t{0});
			size_t encoded = 0;
			for (const NetValueObservation& observation: observations) {
				const size_t cost = ValueObservationCost(observation);
				if (out.size() - blockStart + cost > budget) {
					if (recovery) {
						SetError(error, NetLockstepErrorCode::PayloadTooLarge, out.size(), "recovery input exceeds maximum");
						return false;
					}
					break;
				}
				if (!WriteValueObservation(observation, out, error)) {
					return false;
				}
				++encoded;
			}
			out[countOffset] = static_cast<uint8_t>(encoded & 0xFFU);
			out[countOffset + 1] = static_cast<uint8_t>((encoded >> 8) & 0xFFU);
			if (outEncoded) {
				*outEncoded = encoded;
			}
			return true;
		}

		bool ValidatePlayerBindings(const NetGamePlayerBindings& bindings, NetLockstepError* error) {
			if (bindings.appliedCommands.size() > NetLockstepCodec::c_MaxPeerCount) return false;
			for (const auto& [peer, sequence]: bindings.appliedCommands) {
				if (!ValidatePeerId(peer, error, "command_peer") || sequence == UINT64_MAX) return false;
			}
			for (const NetPlayerBinding& player: bindings.players) {
				if (player.team < -1 || player.team >= 4 || player.viewState > 9 || player.controlledUID < 0 || player.brainUID < 0 || player.controlledUID > INT32_MAX || player.brainUID > INT32_MAX ||
				    !std::isfinite(player.cameraX) || !std::isfinite(player.cameraY) ||
				    std::any_of(player.viewTargets.begin(), player.viewTargets.end(), [](float value) { return !std::isfinite(value); })) {
					SetError(error, NetLockstepErrorCode::InvalidValue, 0, "invalid local player binding");
					return false;
				}
			}
			return true;
		}

		bool EmptyPlayerBinding(const NetPlayerBinding& player) {
			return player == NetPlayerBinding{} && FloatToBitsLE(player.cameraX) == 0 && FloatToBitsLE(player.cameraY) == 0 &&
			       std::all_of(player.viewTargets.begin(), player.viewTargets.end(), [](float value) { return FloatToBitsLE(value) == 0; });
		}

		bool ValidateCommandCounts(const std::vector<NetGameCommand>& commands, NetLockstepError* error) {
			const size_t bindings = std::count_if(commands.begin(), commands.end(), [](const NetGameCommand& command) { return std::holds_alternative<NetGamePlayerBindings>(command.payload); });
			if (bindings > 1 || commands.size() - bindings > NetLockstepCodec::c_MaxCommandsPerPacket) {
				SetError(error, NetLockstepErrorCode::PayloadTooLarge, 0, "frame packet has too many game commands or player bindings");
				return false;
			}
			return true;
		}

		bool EncodeWorldTransition(const NetGameWorldTransition& transition, std::vector<uint8_t>& out, NetLockstepError* error) {
			AppendU16LE(out, transition.schema);
			AppendU8(out, transition.kind);
			AppendU8(out, transition.peerId);
			AppendU32LE(out, transition.holderGeneration);
			AppendU64LE(out, transition.membershipRevision);
			AppendU64LE(out, transition.activationFrame);
			AppendU32LE(out, static_cast<uint32_t>(transition.team));
			AppendU32LE(out, static_cast<uint32_t>(transition.player));
			AppendU32LE(out, FloatToBitsLE(transition.posX));
			AppendU32LE(out, FloatToBitsLE(transition.posY));
			AppendU32LE(out, static_cast<uint32_t>(transition.aiMode));
			AppendU8(out, transition.bindBrain ? 1 : 0);
			if (!AppendString(out, transition.className, NetLockstepCodec::c_MaxScenarioBytes, "world_transition_class_name", error) ||
			    !AppendString(out, transition.preset, NetLockstepCodec::c_MaxScenarioBytes, "world_transition_preset", error) ||
			    !AppendString(out, transition.module, NetLockstepCodec::c_MaxScenarioBytes, "world_transition_module", error)) {
				return false;
			}
			return true;
		}

		bool DecodeWorldTransition(ByteReader& reader, NetGameWorldTransition& transition, NetLockstepError* error) {
			uint32_t team = 0;
			uint32_t player = 0;
			uint32_t xBits = 0;
			uint32_t yBits = 0;
			uint32_t aiMode = 0;
			uint8_t bindBrain = 0;
			if (!ReadOrTruncated(reader.ReadU16LE(transition.schema), reader, error, "world_transition_schema") ||
			    !ReadOrTruncated(reader.ReadU8(transition.kind), reader, error, "world_transition_kind") ||
			    !ReadOrTruncated(reader.ReadU8(transition.peerId), reader, error, "world_transition_peer_id") ||
			    !ReadOrTruncated(reader.ReadU32LE(transition.holderGeneration), reader, error, "world_transition_generation") ||
			    !ReadOrTruncated(reader.ReadU64LE(transition.membershipRevision), reader, error, "world_transition_revision") ||
			    !ReadOrTruncated(reader.ReadU64LE(transition.activationFrame), reader, error, "world_transition_activation_frame") ||
			    !ReadOrTruncated(reader.ReadU32LE(team), reader, error, "world_transition_team") ||
			    !ReadOrTruncated(reader.ReadU32LE(player), reader, error, "world_transition_player") ||
			    !ReadOrTruncated(reader.ReadU32LE(xBits), reader, error, "world_transition_pos_x") ||
			    !ReadOrTruncated(reader.ReadU32LE(yBits), reader, error, "world_transition_pos_y") ||
			    !ReadOrTruncated(reader.ReadU32LE(aiMode), reader, error, "world_transition_ai_mode") ||
			    !ReadOrTruncated(reader.ReadU8(bindBrain), reader, error, "world_transition_bind_brain") ||
			    !reader.ReadString(transition.className, NetLockstepCodec::c_MaxScenarioBytes, "world_transition_class_name", error) ||
			    !reader.ReadString(transition.preset, NetLockstepCodec::c_MaxScenarioBytes, "world_transition_preset", error) ||
			    !reader.ReadString(transition.module, NetLockstepCodec::c_MaxScenarioBytes, "world_transition_module", error)) {
				return false;
			}
			if (transition.schema != c_NetWorldJoinSchema) {
				SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "world_transition_schema is not a known schema");
				return false;
			}
			if (transition.kind > NetGameWorldTransition::SeatRespawn) {
				SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "world_transition_kind is not a known transition");
				return false;
			}
			transition.team = static_cast<int32_t>(team);
			transition.player = static_cast<int32_t>(player);
			transition.posX = FloatFromBitsLE(xBits);
			transition.posY = FloatFromBitsLE(yBits);
			transition.aiMode = static_cast<int32_t>(aiMode);
			transition.bindBrain = bindBrain != 0;
			return true;
		}

		bool EncodeCommandList(const std::vector<NetGameCommand>& commands, uint8_t senderPeerId, std::vector<uint8_t>& out, NetLockstepError* error, bool recovery, bool holdIdentity = true, bool admissionIdentity = true) {
			AppendU16LE(out, static_cast<uint16_t>(commands.size()));
			for (const NetGameCommand& command : commands) {
				if (recovery && command.senderPeerId != senderPeerId) {
					SetError(error, NetLockstepErrorCode::InvalidValue, out.size(), "recovery command sender mismatch");
					return false;
				}
				// The command's sender is the authenticated frame sender; it is set at decode, not encoded here.
				const NetGameCommandType type = NetGameCommandTypeOf(command.payload);
				AppendU16LE(out, static_cast<uint16_t>(type));
				if (command.sequence == UINT64_MAX || (type == NetGameCommandType::PlayerBindings && command.sequence != 0)) {
					SetError(error, NetLockstepErrorCode::InvalidValue, out.size(), "invalid command sequence");
					return false;
				}
				AppendVarU64(out, command.sequence);
				switch (type) {
					case NetGameCommandType::InputDelay: {
						const auto& delay = std::get<NetGameInputDelay>(command.payload);
						if (command.sequence != 0 || !ValidatePeerId(delay.peerId, error, "delay peer") || delay.frames > NetLockstepCodec::c_MaxInputDelayFrames) return false;
						AppendU8(out, delay.peerId);
						AppendU16LE(out, delay.frames);
						break;
					}
					case NetGameCommandType::SeatReclaim: {
						const auto& reclaim = std::get<NetGameSeatReclaim>(command.payload);
						if (!holdIdentity || command.sequence != 0 || !ValidatePeerId(reclaim.peerId, error, "reclaimed peer") ||
						    reclaim.seatIncarnation == 0 || reclaim.eventSequence == 0 || reclaim.delayFrames > NetLockstepCodec::c_MaxInputDelayFrames) return false;
						AppendU8(out, reclaim.peerId); AppendU64LE(out, reclaim.authorityGeneration); AppendU64LE(out, reclaim.eventSequence);
						AppendU32LE(out, reclaim.seatIncarnation); AppendU64LE(out, reclaim.activationFrame); AppendU16LE(out, reclaim.delayFrames); AppendU64LE(out, reclaim.neutralThroughFrame);
						if (admissionIdentity) {
							AppendU8(out, reclaim.worldTransition.has_value());
							if (reclaim.worldTransition && !EncodeWorldTransition(*reclaim.worldTransition, out, error)) return false;
						}
						break;
					}
					case NetGameCommandType::Checkpoint: {
						const auto& checkpoint = std::get<NetGameCheckpoint>(command.payload);
						if (checkpoint.kind != NetGameCheckpoint::Capture && checkpoint.kind != NetGameCheckpoint::Written) {
							SetError(error, NetLockstepErrorCode::InvalidValue, out.size(), "checkpoint kind is not a known kind");
							return false;
						}
						AppendU8(out, checkpoint.kind);
						AppendU64LE(out, checkpoint.tick);
						break;
					}
					case NetGameCommandType::SeatHold: {
						const auto& hold = std::get<NetGameSeatHold>(command.payload);
						if (command.sequence != 0 || !ValidatePeerId(hold.peerId, error, "held peer")) return false;
						AppendU8(out, hold.peerId);
						if (holdIdentity) {
							AppendU64LE(out, hold.authorityGeneration);
							AppendU64LE(out, hold.eventSequence);
							AppendU32LE(out, hold.seatIncarnation);
							AppendU64LE(out, hold.cutoffFrame);
						}
						break;
					}
					case NetGameCommandType::PlayerBindings: {
						const auto& bindings = std::get<NetGamePlayerBindings>(command.payload);
						if (!ValidatePlayerBindings(bindings, error)) return false;
						uint8_t present = 0;
						for (size_t player = 0; player < bindings.players.size(); ++player) {
							if (!EmptyPlayerBinding(bindings.players[player])) present |= uint8_t(1U << player);
						}
						AppendU8(out, present);
						for (size_t index = 0; index < bindings.players.size(); ++index) {
							if (!(present & (1U << index))) continue;
							const auto& player = bindings.players[index];
							AppendU8(out, uint8_t(player.active | (player.human << 1) | (player.hadBrain << 2) | (player.brainEvacuated << 3)));
							AppendU8(out, uint8_t(player.team + 1));
							AppendU8(out, player.viewState);
							AppendVarU64(out, uint64_t(player.controlledUID));
							AppendVarU64(out, uint64_t(player.brainUID));
							AppendU32LE(out, FloatToBitsLE(player.cameraX));
							AppendU32LE(out, FloatToBitsLE(player.cameraY));
							for (const float target: player.viewTargets) AppendU32LE(out, FloatToBitsLE(target));
						}
						AppendU8(out, static_cast<uint8_t>(bindings.appliedCommands.size()));
						for (const auto& [peer, sequence]: bindings.appliedCommands) { AppendU8(out, peer); AppendVarU64(out, sequence); }
						break;
					}
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
						AppendU64LE(out, static_cast<uint64_t>(order.writerUID));
						break;
					}
					case NetGameCommandType::AIScriptMessage: {
						const NetGameAIScriptMessage& scriptMessage = std::get<NetGameAIScriptMessage>(command.payload);
						AppendU64LE(out, static_cast<uint64_t>(scriptMessage.writerUID));
						AppendU64LE(out, static_cast<uint64_t>(scriptMessage.objectUID));
						AppendU32LE(out, static_cast<uint32_t>(scriptMessage.team));
						AppendU8(out, scriptMessage.context);
						AppendU64LE(out, DoubleToBitsLE(scriptMessage.number));
						AppendU64LE(out, static_cast<uint64_t>(scriptMessage.contextUID));
						if (!AppendString(out, scriptMessage.message, NetLockstepCodec::c_MaxValueKeyBytes, "ai_message_name", error) ||
						    !AppendString(out, scriptMessage.text, NetLockstepCodec::c_MaxValueStringBytes, "ai_message_text", error)) {
							return false;
						}
						break;
					}
					case NetGameCommandType::AIGib: {
						const NetGameAIGib& gib = std::get<NetGameAIGib>(command.payload);
						AppendU64LE(out, static_cast<uint64_t>(gib.writerUID));
						AppendU64LE(out, static_cast<uint64_t>(gib.objectUID));
						AppendU64LE(out, static_cast<uint64_t>(gib.ignoreUID));
						AppendU32LE(out, static_cast<uint32_t>(gib.team));
						AppendU32LE(out, FloatToBitsLE(gib.impulseX));
						AppendU32LE(out, FloatToBitsLE(gib.impulseY));
						break;
					}
					case NetGameCommandType::PlaceBrain: {
						const NetGamePlaceBrain& place = std::get<NetGamePlaceBrain>(command.payload);
						AppendU32LE(out, static_cast<uint32_t>(place.team));
						AppendU32LE(out, static_cast<uint32_t>(place.player));
						AppendU32LE(out, FloatToBitsLE(place.posX));
						AppendU32LE(out, FloatToBitsLE(place.posY));
						if (!AppendString(out, place.className, NetLockstepCodec::c_MaxScenarioBytes, "place_brain_class_name", error) ||
						    !AppendString(out, place.preset, NetLockstepCodec::c_MaxScenarioBytes, "place_brain_preset", error) ||
						    !AppendString(out, place.module, NetLockstepCodec::c_MaxScenarioBytes, "place_brain_module", error)) {
							return false;
						}
						break;
					}
					case NetGameCommandType::WorldTransition: {
						if (!EncodeWorldTransition(std::get<NetGameWorldTransition>(command.payload), out, error)) return false;
						break;
					}
				}
				if (recovery && out.size() > NetLockstepCodec::c_MaxRecoveryInputBytes - 10) {
					SetError(error, NetLockstepErrorCode::PayloadTooLarge, out.size(), "recovery input exceeds maximum");
					return false;
				}
			}
			return true;
		}

		bool EncodeTickFrames(uint64_t targetFrame, const std::vector<ControllerFrame>& frames, const std::vector<ControllerFrame>* previousTick, std::vector<uint8_t>& out, NetLockstepError* error) {
			if (!ValidateSortedFrames(frames, error)) {
				return false;
			}
			AppendU16LE(out, static_cast<uint16_t>(frames.size()));
			AppendU64LE(out, targetFrame);
			for (const ControllerFrame& frame : frames) {
				if (!previousTick) {
					const std::vector<uint8_t> encodedFrame = ControllerFrameCodec::Encode(frame);
					out.insert(out.end(), encodedFrame.begin(), encodedFrame.end());
					continue;
				}
				const ControllerFrame* previous = nullptr;
				for (const ControllerFrame& candidate : *previousTick) {
					if (candidate.actorUniqueID == frame.actorUniqueID) {
						previous = &candidate;
						break;
					}
				}
				ControllerFrameCodec::EncodeDelta(out, frame, previous);
			}
			return true;
		}

		// Every copy of a tick repeats the bytes its first packet spelled, so a repair binds what the peer that saw that packet bound.
		bool AppendTickObservations(const NetLockstepFrame& tick, std::vector<uint8_t>& out, NetSoundObservationDictionary* dictionary, NetLockstepObservationBlocks* blocks, bool writeEmptyValues, size_t* outObservationsEncoded, size_t* outValueObservationsEncoded, NetLockstepError* error) {
			if (blocks) {
				if (const auto kept = blocks->find(tick.targetFrame); kept != blocks->end()) {
					out.insert(out.end(), kept->second.soundBytes.begin(), kept->second.soundBytes.end());
					if (!kept->second.valueBytes.empty()) {
						out.insert(out.end(), kept->second.valueBytes.begin(), kept->second.valueBytes.end());
					} else if (writeEmptyValues) {
						AppendU16LE(out, 0);
					}
					if (outObservationsEncoded) *outObservationsEncoded = kept->second.observationsEncoded;
					if (outValueObservationsEncoded) *outValueObservationsEncoded = kept->second.valueObservationsEncoded;
					return true;
				}
			}
			const size_t soundStart = out.size();
			size_t encoded = 0;
			if (!AppendObservations(tick.observations, out, dictionary, &encoded, error)) {
				return false;
			}
			const size_t valueStart = out.size();
			size_t valueEncoded = 0;
			if (!AppendValueObservations(tick.valueObservations, out, &valueEncoded, error, false, writeEmptyValues)) {
				return false;
			}
			if (outObservationsEncoded) *outObservationsEncoded = encoded;
			if (outValueObservationsEncoded) *outValueObservationsEncoded = valueEncoded;
			if (blocks) {
				NetLockstepObservationBlock block;
				block.soundBytes.assign(out.begin() + static_cast<std::ptrdiff_t>(soundStart), out.begin() + static_cast<std::ptrdiff_t>(valueStart));
				if (valueEncoded > 0) {
					block.valueBytes.assign(out.begin() + static_cast<std::ptrdiff_t>(valueStart), out.end());
				}
				block.observationsEncoded = encoded;
				block.valueObservationsEncoded = valueEncoded;
				(*blocks)[tick.targetFrame] = std::move(block);
			}
			return true;
		}

		bool EncodePayload(const NetLockstepFrame& payload, std::vector<uint8_t>& out, NetLockstepError* error, NetSoundObservationDictionary* dictionary, size_t* outObservationsEncoded, bool recovery = false, size_t* outValueObservationsEncoded = nullptr, NetLockstepObservationBlocks* blocks = nullptr, bool holdIdentity = true, bool admissionIdentity = true) {
			if (!ValidatePeerId(payload.senderPeerId, error, "sender_peer_id") || !ValidateSortedFrames(payload.frames, error)) {
				return false;
			}
			if (!ValidateCommandCounts(payload.commands, error)) return false;
			// Only ticks whose own block is still kept can ride the window; an older one is left off rather
			// than re-encoded against a dictionary that has moved past it. Without a dictionary every key is
			// spelled out, so a copy made there stands alone and needs no kept block.
			size_t oldest = payload.priorWindow.size();
			if (!recovery && blocks) {
				for (size_t index = payload.priorWindow.size(); index-- > 0;) {
					if (!blocks->contains(payload.priorWindow[index].targetFrame)) {
						break;
					}
					oldest = index;
				}
			} else if (!recovery && !dictionary) {
				oldest = 0;
			}
			const bool window = !recovery && oldest < payload.priorWindow.size();
			if (window) {
				const size_t ticks = payload.priorWindow.size() - oldest + 1;
				if (ticks < 2 || ticks > NetLockstepCodec::c_MaxWindowTicks) {
					SetError(error, NetLockstepErrorCode::InvalidValue, 0, "frame window tick count is out of range");
					return false;
				}
				for (size_t index = oldest; index < payload.priorWindow.size(); ++index) {
					const NetLockstepFrame& older = payload.priorWindow[index];
					if (older.senderPeerId != payload.senderPeerId || !ValidateSortedFrames(older.frames, error) || !ValidateCommandCounts(older.commands, error)) {
						return false;
					}
				}
			}
			AppendU8(out, payload.senderPeerId);
			AppendU8(out, window ? static_cast<uint8_t>(payload.priorWindow.size() - oldest + 1) : 0);
			if (window) {
				AppendU64LE(out, payload.roundId);
				const std::vector<ControllerFrame>* previousTick = nullptr;
				std::vector<ControllerFrame> previousFrames;
				auto encodeTick = [&](const NetLockstepFrame& tick) {
					if (!EncodeTickFrames(tick.targetFrame, tick.frames, previousTick, out, error) ||
					    !EncodeCommandList(tick.commands, tick.senderPeerId, out, error, recovery, holdIdentity, admissionIdentity) ||
					    !AppendTickObservations(tick, out, dictionary, blocks, true, outObservationsEncoded, outValueObservationsEncoded, error)) {
						return false;
					}
					previousFrames = tick.frames;
					previousTick = &previousFrames;
					return true;
				};
				for (size_t index = oldest; index < payload.priorWindow.size(); ++index) {
					if (!encodeTick(payload.priorWindow[index])) {
						return false;
					}
				}
				return encodeTick(payload);
			}
			if (!EncodeTickFrames(payload.targetFrame, payload.frames, nullptr, out, error) ||
			    !EncodeCommandList(payload.commands, payload.senderPeerId, out, error, recovery, holdIdentity, admissionIdentity)) {
				return false;
			}
			AppendU64LE(out, payload.roundId);
			if (!recovery) {
				return AppendTickObservations(payload, out, dictionary, blocks, false, outObservationsEncoded, outValueObservationsEncoded, error);
			}
			if (payload.observations.size() > NetLockstepCodec::c_MaxObservationsPerPacket ||
			    out.size() + 2 + payload.observations.size() * 45 > NetLockstepCodec::c_MaxRecoveryInputBytes) {
				SetError(error, NetLockstepErrorCode::PayloadTooLarge, out.size(), "recovery input exceeds maximum");
				return false;
			}
			AppendU16LE(out, static_cast<uint16_t>(payload.observations.size()));
			for (const auto& observation: payload.observations) {
				if (observation.senderPeerId != payload.senderPeerId || !std::isfinite(observation.value)) {
					SetError(error, NetLockstepErrorCode::InvalidValue, out.size(), "invalid recovery observation");
					return false;
				}
				AppendU8(out, observation.senderPeerId);
				AppendU64LE(out, observation.objectUID);
				AppendU64LE(out, observation.tick);
				AppendU64LE(out, observation.phase);
				AppendU64LE(out, observation.occurrence);
				AppendU64LE(out, observation.ordinal);
				AppendU32LE(out, FloatToBitsLE(observation.value));
			}
			return AppendValueObservations(payload.valueObservations, out, outValueObservationsEncoded, error, true);
		}

		bool ValidateRecoveryChunk(const NetLockstepRecoveryChunk& chunk, NetLockstepError* error) {
			if (!ValidatePeerId(chunk.senderPeerId, error, "sender_peer_id")) return false;
			if (chunk.sessionId == 0 || chunk.roundId == 0 || chunk.totalBytes == 0 || chunk.totalBytes > NetLockstepCodec::c_MaxRecoveryInputBytes ||
			    chunk.offset >= chunk.totalBytes || chunk.offset % NetLockstepCodec::c_MaxRecoveryChunkBytes != 0 ||
			    chunk.bytes.size() != std::min(NetLockstepCodec::c_MaxRecoveryChunkBytes, size_t(chunk.totalBytes - chunk.offset))) {
				SetError(error, NetLockstepErrorCode::InvalidValue, 0, "invalid recovery chunk bounds or scope");
				return false;
			}
			return true;
		}

		bool EncodePayload(const NetLockstepRecoveryChunk& payload, std::vector<uint8_t>& out, NetLockstepError* error) {
			if (!ValidateRecoveryChunk(payload, error)) return false;
			AppendU8(out, payload.senderPeerId);
			AppendU8(out, 0);
			AppendU16LE(out, 0);
			AppendU64LE(out, payload.sessionId);
			AppendU64LE(out, payload.roundId);
			AppendU64LE(out, payload.targetFrame);
			AppendU32LE(out, payload.totalBytes);
			AppendU32LE(out, payload.offset);
			out.insert(out.end(), payload.bytes.begin(), payload.bytes.end());
			return true;
		}

		bool DecodeRecoveryChunk(ByteReader& reader, NetLockstepPayload& out, NetLockstepError* error, uint16_t version) {
			NetLockstepRecoveryChunk chunk;
			uint8_t reserved8 = 0;
			uint16_t reserved16 = 0;
			if (version < NetLockstepCodec::c_PlayerBindingsVersion) {
				SetError(error, NetLockstepErrorCode::UnsupportedVersion, 0, "recovery chunks require version 18");
				return false;
			}
			if (!reader.ReadU8(chunk.senderPeerId) || !reader.ReadU8(reserved8) || !reader.ReadU16LE(reserved16) ||
			    !reader.ReadU64LE(chunk.sessionId) || !reader.ReadU64LE(chunk.roundId) || !reader.ReadU64LE(chunk.targetFrame) ||
			    !reader.ReadU32LE(chunk.totalBytes) || !reader.ReadU32LE(chunk.offset)) {
				SetError(error, NetLockstepErrorCode::TruncatedPayload, reader.Offset(), "truncated recovery chunk");
				return false;
			}
			if (reserved8 != 0 || reserved16 != 0 || reader.Remaining() > NetLockstepCodec::c_MaxRecoveryChunkBytes) {
				SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "invalid recovery chunk header");
				return false;
			}
			const uint8_t* data = nullptr;
			const size_t count = reader.Remaining();
			if (!reader.ReadBytes(data, count)) return false;
			chunk.bytes.assign(data, data + count);
			if (!ValidateRecoveryChunk(chunk, error)) return false;
			out = std::move(chunk);
			return true;
		}

		bool ValidateTiming(const NetLockstepTiming& timing, NetLockstepError* error) {
			const auto action = static_cast<uint8_t>(timing.action);
			const auto phase = static_cast<uint8_t>(timing.phase);
			if (!ValidatePeerId(timing.senderPeerId, error, "timing sender") || !ValidatePeerId(timing.peerId, error, "timing subject")) return false;
			if (action < 1 || action > 5 || phase < 1 || phase > 7 || timing.sessionId == 0 || timing.roundId == 0 ||
			    (action == 1 && phase > 4) || (action == 2 && phase > 6) || (action == 3 && phase != 7) || (action == 4 && (phase > 3 || !timing.worldTransition)) ||
			    (action == 5 && phase != static_cast<uint8_t>(NetTimingPhase::Commit) && phase != static_cast<uint8_t>(NetTimingPhase::Status)) ||
			    timing.delayFrames > NetLockstepCodec::c_MaxInputDelayFrames || (timing.requiredPeers & 0xF0U) != 0 ||
			    (timing.heldPeers & 0xF0U) != 0 || (timing.heldPeers & timing.requiredPeers) != 0 ||
			    (timing.phase != NetTimingPhase::Status && timing.action == NetTimingAction::Hold && (timing.heldPeers & (1U << (timing.peerId - 1))) == 0) ||
			    (timing.action == NetTimingAction::Delay && timing.heldPeers != 0) ||
			    (timing.action == NetTimingAction::CapturePark && timing.heldPeers != 0) ||
			    (timing.phase == NetTimingPhase::Status ? timing.revision != 0 || timing.requiredPeers != 0 : timing.revision == 0 || timing.requiredPeers == 0) ||
			    (timing.supersededRevision != 0 && (timing.phase == NetTimingPhase::Status || timing.supersededRevision >= timing.revision))) {
				SetError(error, NetLockstepErrorCode::InvalidValue, 0, "invalid timing decision");
				return false;
			}
			if (timing.worldTransition && (timing.action != NetTimingAction::WorldAdmission || timing.worldTransition->kind != NetGameWorldTransition::Activate ||
			    timing.worldTransition->peerId != timing.peerId || timing.worldTransition->activationFrame != timing.applyFrame || timing.seatIncarnations[timing.peerId - 1] == 0)) return false;
			if (phase >= 5) {
				if (timing.peerId > 4 || (timing.heldPeers & (1U << (timing.peerId - 1))) == 0) return false;
				if (timing.cutoffFrame != timing.applyFrame) {
					SetError(error, NetLockstepErrorCode::InvalidValue, 0, "hold cutoff differs from its committed frame");
					return false;
				}
				for (size_t peer = 0; peer < timing.seatIncarnations.size(); ++peer) {
					if (((timing.heldPeers >> peer) & 1U) != (timing.seatIncarnations[peer] != 0)) {
						SetError(error, NetLockstepErrorCode::InvalidValue, 0, "hold incarnation mask differs from its held seats");
						return false;
					}
				}
			}
			return true;
		}

		bool EncodePayload(const NetLockstepTiming& timing, std::vector<uint8_t>& out, NetLockstepError* error) {
			if (!ValidateTiming(timing, error)) return false;
			AppendU8(out, timing.senderPeerId);
			AppendU8(out, timing.peerId);
			AppendU8(out, static_cast<uint8_t>(timing.action));
			AppendU8(out, static_cast<uint8_t>(timing.phase));
			AppendU64LE(out, timing.sessionId);
			AppendU64LE(out, timing.roundId);
			AppendU64LE(out, timing.revision);
			AppendU64LE(out, timing.applyFrame);
			AppendU64LE(out, timing.nextFrame);
			AppendU16LE(out, timing.delayFrames);
			AppendU8(out, timing.requiredPeers);
			AppendU8(out, timing.heldPeers);
			AppendU32LE(out, timing.pingMs);
			AppendU32LE(out, timing.jitterMs);
			AppendU64LE(out, timing.authorityGeneration);
			AppendU64LE(out, timing.cutoffFrame);
			for (uint32_t incarnation: timing.seatIncarnations) AppendU32LE(out, incarnation);
			AppendU64LE(out, timing.neutralThroughFrame);
			AppendU8(out, timing.worldTransition.has_value());
			if (timing.worldTransition && !EncodeWorldTransition(*timing.worldTransition, out, error)) return false;
			AppendU64LE(out, timing.supersededRevision);
			return true;
		}

		bool DecodeTiming(ByteReader& reader, NetLockstepPayload& out, NetLockstepError* error, uint16_t version) {
			if (version < NetLockstepCodec::c_TimingVersion) {
				SetError(error, NetLockstepErrorCode::UnsupportedVersion, 0, "timing decisions require the timing wire");
				return false;
			}
			NetLockstepTiming timing;
			uint8_t action = 0, phase = 0;
			if (!ReadOrTruncated(reader.ReadU8(timing.senderPeerId) && reader.ReadU8(timing.peerId) && reader.ReadU8(action) && reader.ReadU8(phase) &&
			    reader.ReadU64LE(timing.sessionId) && reader.ReadU64LE(timing.roundId) && reader.ReadU64LE(timing.revision) &&
			    reader.ReadU64LE(timing.applyFrame) && reader.ReadU64LE(timing.nextFrame) && reader.ReadU16LE(timing.delayFrames) &&
			    reader.ReadU8(timing.requiredPeers) && reader.ReadU8(timing.heldPeers) && reader.ReadU32LE(timing.pingMs) && reader.ReadU32LE(timing.jitterMs), reader, error, "timing decision")) return false;
			timing.action = static_cast<NetTimingAction>(action);
			timing.phase = static_cast<NetTimingPhase>(phase);
			if (version >= NetLockstepCodec::c_HoldTransactionVersion) {
				if (!ReadOrTruncated(reader.ReadU64LE(timing.authorityGeneration) && reader.ReadU64LE(timing.cutoffFrame), reader, error, "timing authority")) return false;
				for (auto& incarnation: timing.seatIncarnations)
					if (!ReadOrTruncated(reader.ReadU32LE(incarnation), reader, error, "timing incarnation")) return false;
				if (!ReadOrTruncated(reader.ReadU64LE(timing.neutralThroughFrame), reader, error, "reclaim neutral gap")) return false;
			} else if (phase > 4) {
				SetError(error, NetLockstepErrorCode::UnsupportedVersion, 0, "hold transactions require the current wire");
				return false;
			}
			if (version >= NetLockstepCodec::c_WorldAdmissionVersion) {
				uint8_t present = 0;
				if (!reader.ReadU8(present) || present > 1) return false;
				if (present) { timing.worldTransition.emplace(); if (!DecodeWorldTransition(reader, *timing.worldTransition, error)) return false; }
			}
			if (version >= NetLockstepCodec::c_TimingWithdrawVersion &&
			    !ReadOrTruncated(reader.ReadU64LE(timing.supersededRevision), reader, error, "timing withdrawal")) return false;
			if (!ValidateTiming(timing, error)) return false;
			out = timing;
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
			if (payload.receivedMask == NetLockstepCodec::c_InputAcceptedMask) {
				if (payload.roundId == 0 || payload.seatIncarnation == 0 || payload.sessionId == 0) {
					SetError(error, NetLockstepErrorCode::InvalidValue, 0, "input acceptance requires a round and seat incarnation");
					return false;
				}
				AppendU64LE(out, payload.roundId);
				AppendU32LE(out, payload.seatIncarnation);
				AppendU64LE(out, payload.sessionId);
				AppendU64LE(out, payload.authorityGeneration);
			}
			return true;
		}

		bool EncodePayload(const NetLockstepStop& payload, std::vector<uint8_t>& out, NetLockstepError* error) {
			if (!IsKnownStopReason(payload.reason)) {
				SetError(error, NetLockstepErrorCode::InvalidValue, 0, "stop reason has invalid enum value");
				return false;
			}
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
			if (payload.appliedCommands.size() > NetLockstepCodec::c_MaxPeerCount) return false;
			AppendU8(out, static_cast<uint8_t>(payload.appliedCommands.size()));
			for (const auto& [peer, sequence]: payload.appliedCommands) {
				if (!ValidatePeerId(peer, error, "command_peer") || sequence == UINT64_MAX) return false;
				AppendU8(out, peer); AppendVarU64(out, sequence);
			}
			return true;
		}

		bool ValidateSeatSnapshot(const NetLockstepSeatSnapshot& payload, NetLockstepError* error) {
			if (!ValidatePeerId(payload.senderPeerId, error, "sender_peer_id")) return false;
			if (payload.sessionId == 0 || payload.roundId == 0 || payload.revision == 0 ||
			    std::all_of(payload.epoch.begin(), payload.epoch.end(), [](uint8_t byte) { return byte == 0; }) ||
			    payload.seats.empty() || payload.seats.size() > NetLockstepCodec::c_MaxPeerCount) {
				SetError(error, NetLockstepErrorCode::InvalidValue, 0, "invalid seat snapshot identity or count");
				return false;
			}
			std::set<uint8_t> peers;
			std::optional<uint16_t> lastSeat;
			for (const NetSeatPresenceEntry& seat: payload.seats) {
				if (!ValidatePeerId(seat.peerId, error, "seat_peer_id")) return false;
				if ((lastSeat && seat.stableSeat <= *lastSeat) || !peers.insert(seat.peerId).second ||
				    static_cast<uint8_t>(seat.state) > static_cast<uint8_t>(NetSeatPresenceState::Left) ||
				    (!seat.holdActive && seat.holdUntilMs != 0) ||
				    (seat.holdActive && seat.state != NetSeatPresenceState::Disconnected && seat.state != NetSeatPresenceState::Reconnecting)) {
					SetError(error, NetLockstepErrorCode::InvalidValue, 0, "invalid seat snapshot entry");
					return false;
				}
				lastSeat = seat.stableSeat;
			}
			return true;
		}

		bool AdvancesSeatSnapshot(const std::optional<NetLockstepSeatSnapshot>& previous, const NetLockstepSeatSnapshot& next, uint8_t peerCount, const NetMatchConfig& config) {
			if (previous && (next.sessionId != previous->sessionId || next.epoch != previous->epoch || next.roundId != previous->roundId ||
			                 next.revision <= previous->revision || next.observedAtMs < previous->observedAtMs)) return false;
			for (const auto& seat: next.seats) {
				if (seat.peerId > peerCount) return false;
				if (!previous) continue;
				const auto old = std::find_if(previous->seats.begin(), previous->seats.end(), [&](const auto& entry) { return entry.stableSeat == seat.stableSeat; });
				if (old != previous->seats.end() &&
				    (seat.peerId != old->peerId || seat.seatGeneration < old->seatGeneration || seat.holderGeneration < old->holderGeneration ||
				     (seat.holderGeneration == old->holderGeneration && seat.incarnation < old->incarnation) ||
				     (seat.holderGeneration > old->holderGeneration && seat.seatGeneration <= old->seatGeneration))) return false;
			}
			if (previous) {
				for (const auto& old: previous->seats) {
					if (std::none_of(next.seats.begin(), next.seats.end(), [&](const auto& seat) { return seat.stableSeat == old.stableSeat; })) return false;
				}
			}
			for (const auto& player: config.players) {
				if (!player.cpu && player.peerId != 0 && std::none_of(next.seats.begin(), next.seats.end(),
				    [&](const auto& seat) { return seat.peerId == player.peerId; })) return false;
			}
			return true;
		}

		bool EncodePayload(const NetLockstepSeatSnapshot& payload, std::vector<uint8_t>& out, NetLockstepError* error) {
			if (!ValidateSeatSnapshot(payload, error)) return false;
			AppendU8(out, payload.senderPeerId);
			AppendU8(out, static_cast<uint8_t>(payload.seats.size()));
			AppendU16LE(out, 0);
			AppendU64LE(out, payload.sessionId);
			out.insert(out.end(), payload.epoch.begin(), payload.epoch.end());
			AppendU64LE(out, payload.roundId);
			AppendU64LE(out, payload.revision);
			AppendU64LE(out, payload.observedAtMs);
			for (const NetSeatPresenceEntry& seat: payload.seats) {
				AppendU16LE(out, seat.stableSeat);
				AppendU8(out, seat.peerId);
				AppendU8(out, static_cast<uint8_t>(seat.state));
				AppendU32LE(out, seat.holderGeneration);
				AppendU32LE(out, seat.seatGeneration);
				AppendU32LE(out, seat.incarnation);
				AppendU8(out, seat.holdActive ? 1 : 0);
				AppendU64LE(out, seat.holdUntilMs);
				AppendU64LE(out, seat.holdUntilFrame);
				if (!AppendString(out, seat.holderName, NetProtocol::c_MaxDisplayNameBytes, "seat_holder_name", error)) return false;
			}
			return true;
		}

		bool DecodeSeatSnapshot(ByteReader& reader, NetLockstepPayload& out, NetLockstepError* error, uint16_t version) {
			if (version < NetLockstepCodec::c_SeatSnapshotVersion) {
				SetError(error, NetLockstepErrorCode::UnsupportedVersion, 0, "seat snapshots require lockstep version 17");
				return false;
			}
			NetLockstepSeatSnapshot payload;
			uint8_t count = 0;
			uint16_t reserved = 0;
			const uint8_t* epoch = nullptr;
			if (!ReadOrTruncated(reader.ReadU8(payload.senderPeerId), reader, error, "sender_peer_id") ||
			    !ReadOrTruncated(reader.ReadU8(count), reader, error, "seat_count") ||
			    !ReadOrTruncated(reader.ReadU16LE(reserved), reader, error, "reserved") ||
			    !ReadOrTruncated(reader.ReadU64LE(payload.sessionId), reader, error, "session_id") ||
			    !ReadOrTruncated(reader.ReadBytes(epoch, payload.epoch.size()), reader, error, "epoch") ||
			    !ReadOrTruncated(reader.ReadU64LE(payload.roundId), reader, error, "round_id") ||
			    !ReadOrTruncated(reader.ReadU64LE(payload.revision), reader, error, "revision") ||
			    !ReadOrTruncated(reader.ReadU64LE(payload.observedAtMs), reader, error, "observed_at_ms")) return false;
			if (reserved != 0 || count > NetLockstepCodec::c_MaxPeerCount) {
				SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "invalid seat snapshot header");
				return false;
			}
			std::copy_n(epoch, payload.epoch.size(), payload.epoch.begin());
			for (uint8_t index = 0; index < count; ++index) {
				NetSeatPresenceEntry seat;
				uint8_t state = 0, reclaim = 0;
				if (!ReadOrTruncated(reader.ReadU16LE(seat.stableSeat), reader, error, "stable_seat") ||
				    !ReadOrTruncated(reader.ReadU8(seat.peerId), reader, error, "seat_peer_id") ||
				    !ReadOrTruncated(reader.ReadU8(state), reader, error, "seat_state") ||
				    !ReadOrTruncated(reader.ReadU32LE(seat.holderGeneration), reader, error, "holder_generation") ||
				    !ReadOrTruncated(reader.ReadU32LE(seat.seatGeneration), reader, error, "seat_generation") ||
				    !ReadOrTruncated(reader.ReadU32LE(seat.incarnation), reader, error, "incarnation") ||
				    !ReadOrTruncated(reader.ReadU8(reclaim), reader, error, "hold_active") ||
				    !ReadOrTruncated(reader.ReadU64LE(seat.holdUntilMs), reader, error, "hold_until_ms") ||
				    !ReadOrTruncated(reader.ReadU64LE(seat.holdUntilFrame), reader, error, "hold_until_frame") ||
				    !reader.ReadString(seat.holderName, NetProtocol::c_MaxDisplayNameBytes, "seat_holder_name", error)) return false;
				if (reclaim > 1) {
					SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "invalid hold flag");
					return false;
				}
				seat.state = static_cast<NetSeatPresenceState>(state);
				seat.holdActive = reclaim != 0;
				payload.seats.push_back(std::move(seat));
			}
			if (!ValidateSeatSnapshot(payload, error)) return false;
			out = std::move(payload);
			return true;
		}

		bool DecodeStart(ByteReader& reader, NetLockstepPayload& out, NetLockstepError* error, uint16_t version) {
			NetLockstepStart payload;
			uint8_t resume = 0;
			uint32_t startupReading = 0;
			if (!ReadOrTruncated(reader.ReadU64LE(payload.sessionId), reader, error, "session_id") ||
			    !ReadOrTruncated(reader.ReadU64LE(payload.startFrame), reader, error, "start_frame") ||
			    !ReadOrTruncated(reader.ReadU16LE(payload.inputDelayFrames), reader, error, "input_delay_frames") ||
			    !ReadOrTruncated(reader.ReadU16LE(payload.controllerFrameVersion), reader, error, "controller_frame_version") ||
			    !ReadOrTruncated(reader.ReadU16LE(payload.controllerFrameEncodedSize), reader, error, "controller_frame_encoded_size") ||
			    !ReadOrTruncated(reader.ReadU8(payload.localPeerId), reader, error, "local_peer_id") ||
			    !ReadOrTruncated(reader.ReadU8(payload.peerCount), reader, error, "peer_count") ||
			    (version >= NetLockstepCodec::c_RoundVersion && !ReadOrTruncated(reader.ReadU64LE(payload.roundId), reader, error, "round_id")) ||
			    (version >= NetLockstepCodec::c_PlayerBindingsVersion && !ReadOrTruncated(reader.ReadU8(resume), reader, error, "resume_from_snapshot")) || resume > 1 ||
			    !reader.ReadString(payload.scenario, NetLockstepCodec::c_MaxScenarioBytes, "scenario", error) ||
			    !reader.ReadString(payload.ownershipPolicy, NetLockstepCodec::c_MaxOwnershipPolicyBytes, "ownership_policy", error) ||
			    (version >= NetLockstepCodec::c_StartParkVersion && !ReadOrTruncated(reader.ReadU32LE(startupReading), reader, error, "activity_restart_ms")) ||
			    (version >= NetLockstepCodec::c_SeatDeviceVersion && !ReadOrTruncated(reader.ReadU8(payload.deviceClass), reader, error, "device_class")) ||
			    !ValidateStart(payload, error)) {
				return false;
			}
			payload.resumeFromSnapshot = resume != 0;
			payload.startupPublished = startupReading > 0;
			payload.activityRestartMs = startupReading > 0 ? startupReading - 1 : 0;
			if (version >= NetLockstepCodec::c_AgreedStartVersion && reader.Remaining() != 0) {
				uint8_t agreed = 0;
				if (!ReadOrTruncated(reader.ReadU8(agreed), reader, error, "agreed_start_record") || agreed > 1) return false;
				if (agreed) {
					payload.agreedStartRecord = true;
					if (!ReadOrTruncated(reader.ReadU64LE(payload.agreedFirstFrame) && reader.ReadU64LE(payload.agreedEffectiveStartFrame) &&
					    reader.ReadU64LE(payload.agreedDeadlineMs) && reader.ReadU32LE(payload.publishedPeerMask) && reader.ReadU32LE(payload.heldPeerMask),
					    reader, error, "agreed start boundary")) return false;
					for (auto& frame: payload.peerEffectiveStartFrames)
						if (!ReadOrTruncated(reader.ReadU64LE(frame), reader, error, "agreed peer start")) return false;
					for (auto& park: payload.peerStartupParks)
						if (!ReadOrTruncated(reader.ReadU32LE(park), reader, error, "agreed peer startup")) return false;
					for (auto& delay: payload.peerInputDelays)
						if (!ReadOrTruncated(reader.ReadU16LE(delay), reader, error, "agreed peer delay")) return false;
					if (version >= NetLockstepCodec::c_SeatDeviceVersion) {
						for (auto& deviceClass: payload.peerDeviceClasses)
							if (!ReadOrTruncated(reader.ReadU8(deviceClass), reader, error, "agreed peer device")) return false;
						if (!ValidateStart(payload, error)) return false;
					}
				}
			}
			out = payload;
			return true;
		}

		// A block belonging to another round is read with `discard` and no dictionary: its shape is checked,
		// nothing is resolved and no observation comes out, so a frame this round is going to drop cannot
		// disturb its sender's live slots. Bindings are staged and applied once the whole block has read, so a
		// refusal part way through leaves the table exactly as it was.
		bool ReadObservations(ByteReader& reader, NetLockstepFrame& payload, NetSoundObservationDictionary* dictionary, NetLockstepError* error, uint16_t version, bool discard, bool windowCopy = false, bool* outReadPast = nullptr) {
			bool restart = false;
			if (outReadPast) {
				*outReadPast = false;
			}
			if (version >= NetLockstepCodec::c_ObservationBindingSequenceVersion) {
				uint64_t bindingsBefore = 0;
				if (!ReadVarOrFail(reader, bindingsBefore, error, "observation_bindings_before")) {
					return false;
				}
				// On the unreliable lane a sender's first block can arrive after this peer read the ones after it: that is a tick
				// it holds already, not a sender that started over, which spells from nothing at a tick beyond them.
				const bool late = dictionary && version >= NetLockstepCodec::c_UnreliableFrameVersion && bindingsBefore == 0 && dictionary->BindingCount() != 0 &&
				                  dictionary->HasReadTickAtOrAfter(payload.targetFrame, payload.roundId);
				if (dictionary && (late || bindingsBefore != dictionary->BindingCount())) {
					// A window copy of a tick that stands behind the table's place in this sender's stream is
					// one this peer already read: it is read past, the table stays where it is, and the copy
					// is not offered again.
					if (late || (windowCopy && bindingsBefore < dictionary->BindingCount())) {
						dictionary = nullptr;
						discard = true;
						if (outReadPast) {
							*outReadPast = true;
						}
					} else if (bindingsBefore != 0) {
						// A sender that starts over has spelled nothing out yet, which is a new round and not a
						// hole; anything else means this peer is missing a binding it can never be told again.
						SetError(error, NetLockstepErrorCode::ObservationBindingGap, reader.Offset(),
						         "sound observation bindings jump from " + std::to_string(dictionary->BindingCount()) + " to " + std::to_string(bindingsBefore));
						return false;
					} else {
						restart = true;
					}
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
				dictionary->NoteTickRead(payload.targetFrame, payload.roundId);
			}
			return true;
		}

		bool ReadValueObservations(ByteReader& reader, NetLockstepFrame& payload, NetLockstepError* error, bool discard) {
			uint16_t observationCount = 0;
			if (!ReadOrTruncated(reader.ReadU16LE(observationCount), reader, error, "value_observation_count")) {
				return false;
			}
			if (observationCount > NetLockstepCodec::c_MaxObservationsPerPacket) {
				SetError(error, NetLockstepErrorCode::PayloadTooLarge, reader.Offset(), "value_observation_count exceeds maximum");
				return false;
			}
			if (!discard) {
				payload.valueObservations.reserve(observationCount);
			}
			for (uint16_t i = 0; i < observationCount; ++i) {
				NetValueObservation observation;
				uint64_t ordinal = 0;
				if (!ReadVarOrFail(reader, observation.objectUID, error, "value_observation_object") ||
				    !ReadVarOrFail(reader, observation.tick, error, "value_observation_tick") ||
				    !ReadVarOrFail(reader, ordinal, error, "value_observation_ordinal") ||
				    !ReadOrTruncated(reader.ReadU8(observation.mapKind), reader, error, "value_observation_map") ||
				    !ReadOrTruncated(reader.ReadU8(observation.op), reader, error, "value_observation_op") ||
				    !ReadBinaryString(reader, observation.key, NetLockstepCodec::c_MaxValueKeyBytes, "value_observation_key", error)) {
					return false;
				}
				if (ordinal > std::numeric_limits<uint32_t>::max()) {
					SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "value observation ordinal is out of range");
					return false;
				}
				observation.ordinal = static_cast<uint32_t>(ordinal);
				if (observation.op == 0) {
					if (observation.mapKind == 0) {
						uint64_t bits = 0;
						if (!ReadOrTruncated(reader.ReadU64LE(bits), reader, error, "value_observation_number")) {
							return false;
						}
						observation.numberValue = DoubleFromBitsLE(bits);
					} else if (observation.mapKind == 1) {
						if (!ReadBinaryString(reader, observation.stringValue, NetLockstepCodec::c_MaxValueStringBytes, "value_observation_string", error)) {
							return false;
						}
					}
				}
				if (!ValidateValueObservation(observation, reader.Offset(), error)) {
					return false;
				}
				if (discard) {
					continue;
				}
				observation.senderPeerId = payload.senderPeerId;
				payload.valueObservations.push_back(std::move(observation));
			}
			return true;
		}

		bool DecodeCommandList(ByteReader& reader, std::vector<NetGameCommand>& commands, uint8_t senderPeerId, uint16_t version, NetLockstepError* error) {
			uint16_t commandCount = 0;
			if (!ReadOrTruncated(reader.ReadU16LE(commandCount), reader, error, "command_count")) {
				return false;
			}
			if (commandCount > NetLockstepCodec::c_MaxCommandsPerPacket + (version >= NetLockstepCodec::c_PlayerBindingsVersion ? 1 : 0)) {
				SetError(error, NetLockstepErrorCode::PayloadTooLarge, reader.Offset(), "command_count exceeds maximum");
				return false;
			}
			commands.reserve(commandCount);
			for (uint16_t i = 0; i < commandCount; ++i) {
				NetGameCommand command;
				command.senderPeerId = senderPeerId;
				uint16_t rawType = 0;
				if (!ReadOrTruncated(reader.ReadU16LE(rawType), reader, error, "command_type")) {
					return false;
				}
				if (version >= NetLockstepCodec::c_PlayerBindingsVersion) {
					if (!ReadVarOrFail(reader, command.sequence, error, "command_sequence")) return false;
					if (command.sequence == UINT64_MAX || (rawType == uint16_t(NetGameCommandType::PlayerBindings) && command.sequence != 0)) {
						SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "invalid command sequence");
						return false;
					}
				}
				switch (static_cast<NetGameCommandType>(rawType)) {
					case NetGameCommandType::InputDelay: {
						NetGameInputDelay delay;
						if (version < NetLockstepCodec::c_TimingVersion || command.sequence != 0) {
							SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "input delay requires the timing wire and a system sequence");
							return false;
						}
						if (!ReadOrTruncated(reader.ReadU8(delay.peerId) && reader.ReadU16LE(delay.frames) && delay.frames <= NetLockstepCodec::c_MaxInputDelayFrames, reader, error, "input delay") || !ValidatePeerId(delay.peerId, error, "delay peer")) return false;
						command.payload = delay;
						break;
					}
					case NetGameCommandType::SeatReclaim: {
						NetGameSeatReclaim reclaim;
						if (version < NetLockstepCodec::c_HoldTransactionVersion || command.sequence != 0 ||
						    !ReadOrTruncated(reader.ReadU8(reclaim.peerId) && reader.ReadU64LE(reclaim.authorityGeneration) && reader.ReadU64LE(reclaim.eventSequence) &&
						    reader.ReadU32LE(reclaim.seatIncarnation) && reader.ReadU64LE(reclaim.activationFrame) && reader.ReadU16LE(reclaim.delayFrames) && reader.ReadU64LE(reclaim.neutralThroughFrame), reader, error, "seat reclaim") ||
						    !ValidatePeerId(reclaim.peerId, error, "reclaimed peer") || reclaim.seatIncarnation == 0 || reclaim.eventSequence == 0 ||
						    reclaim.delayFrames > NetLockstepCodec::c_MaxInputDelayFrames) return false;
						if (version >= NetLockstepCodec::c_WorldAdmissionVersion) {
							uint8_t present = 0;
							if (!reader.ReadU8(present) || present > 1) return false;
							if (present) { reclaim.worldTransition.emplace(); if (!DecodeWorldTransition(reader, *reclaim.worldTransition, error)) return false; }
						}
						command.payload = reclaim;
						break;
					}
					case NetGameCommandType::Checkpoint: {
						NetGameCheckpoint checkpoint;
						if (version < NetLockstepCodec::c_CheckpointVersion) {
							SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "checkpoint commands require the checkpoint wire");
							return false;
						}
						if (!ReadOrTruncated(reader.ReadU8(checkpoint.kind) && reader.ReadU64LE(checkpoint.tick), reader, error, "checkpoint")) return false;
						if (checkpoint.kind != NetGameCheckpoint::Capture && checkpoint.kind != NetGameCheckpoint::Written) {
							SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "checkpoint kind is not a known kind");
							return false;
						}
						command.payload = checkpoint;
						break;
					}
					case NetGameCommandType::SeatHold: {
						NetGameSeatHold hold;
						if (version < NetLockstepCodec::c_TimingVersion || command.sequence != 0) {
							SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "seat hold requires the timing wire and a system sequence");
							return false;
						}
						if (!ReadOrTruncated(reader.ReadU8(hold.peerId), reader, error, "held peer") || !ValidatePeerId(hold.peerId, error, "held peer")) return false;
						if (version >= NetLockstepCodec::c_HoldTransactionVersion && !ReadOrTruncated(reader.ReadU64LE(hold.authorityGeneration) && reader.ReadU64LE(hold.eventSequence) &&
						    reader.ReadU32LE(hold.seatIncarnation) && reader.ReadU64LE(hold.cutoffFrame), reader, error, "held incarnation")) return false;
						command.payload = hold;
						break;
					}
					case NetGameCommandType::PlayerBindings: {
						uint8_t present = 0;
						if (version < NetLockstepCodec::c_PlayerBindingsVersion) {
							SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "player bindings require the current frame version");
							return false;
						}
						if (!ReadOrTruncated(reader.ReadU8(present), reader, error, "player_binding_mask")) return false;
						if (present & 0xF0U) {
							SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "invalid player binding mask");
							return false;
						}
						NetGamePlayerBindings bindings;
						for (size_t index = 0; index < bindings.players.size(); ++index) {
							if (!(present & (1U << index))) continue;
							auto& player = bindings.players[index];
							uint8_t flags = 0, team = 0;
							uint64_t controlled = 0, brain = 0;
							if (!ReadOrTruncated(reader.ReadU8(flags), reader, error, "player_flags") ||
							    !ReadOrTruncated(reader.ReadU8(team), reader, error, "player_team") ||
							    !ReadOrTruncated(reader.ReadU8(player.viewState), reader, error, "player_view_state") ||
							    !ReadVarOrFail(reader, controlled, error, "player_controlled_uid") ||
							    !ReadVarOrFail(reader, brain, error, "player_brain_uid")) return false;
							if ((flags & 0xF0U) || team > 4 || controlled > INT64_MAX || brain > INT64_MAX) {
								SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "invalid player flags, team or UID");
								return false;
							}
							player.active = flags & 1; player.human = flags & 2; player.hadBrain = flags & 4; player.brainEvacuated = flags & 8;
							player.team = int8_t(team) - 1;
							player.controlledUID = int64_t(controlled); player.brainUID = int64_t(brain);
							const auto readFloat = [&](float& target) {
								uint32_t bits = 0;
								if (!ReadOrTruncated(reader.ReadU32LE(bits), reader, error, "player_view_target")) return false;
								target = FloatFromBitsLE(bits);
								return true;
							};
							if (!readFloat(player.cameraX) || !readFloat(player.cameraY)) return false;
							for (float& target: player.viewTargets) if (!readFloat(target)) return false;
							if (EmptyPlayerBinding(player)) {
								SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "redundant empty player binding");
								return false;
							}
						}
						uint8_t ackCount = 0, previousPeer = 0;
						if (!ReadOrTruncated(reader.ReadU8(ackCount), reader, error, "command_ack_count") || ackCount > NetLockstepCodec::c_MaxPeerCount) return false;
						for (uint8_t i = 0; i < ackCount; ++i) {
							uint8_t peer = 0; uint64_t sequence = 0;
							if (!ReadOrTruncated(reader.ReadU8(peer), reader, error, "command_peer") || !ReadVarOrFail(reader, sequence, error, "command_sequence") || peer <= previousPeer) return false;
							bindings.appliedCommands.emplace(peer, sequence); previousPeer = peer;
						}
						if (!ValidatePlayerBindings(bindings, error)) return false;
						command.payload = bindings;
						break;
					}
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
						// The move-target and alarm-point ops arrived with version 22; below it the byte can only be a stray.
						if (order.op > NetGameAIOrder::SetAlarmPoint ||
						    (order.op >= NetGameAIOrder::SetMOMoveTarget && version < NetLockstepCodec::c_AIPassEventVersion)) {
							SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "ai order op is invalid");
							return false;
						}
						order.actorUID = static_cast<int64_t>(actorUID);
						order.team = static_cast<int32_t>(team);
						order.x = FloatFromBitsLE(xBits);
						order.y = FloatFromBitsLE(yBits);
						order.targetUID = static_cast<int64_t>(targetUID);
						if (version >= NetLockstepCodec::c_AIOrderWriterVersion) {
							uint64_t writerUID = 0;
							if (!ReadOrTruncated(reader.ReadU64LE(writerUID), reader, error, "ai_order_writer_uid")) {
								return false;
							}
							order.writerUID = static_cast<int64_t>(writerUID);
						}
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
					case NetGameCommandType::AIScriptMessage: {
						if (version < NetLockstepCodec::c_AIPassEventVersion) {
							SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "ai script message needs a newer frame version");
							return false;
						}
						NetGameAIScriptMessage scriptMessage;
						uint64_t writerUID = 0;
						uint64_t objectUID = 0;
						uint64_t contextUID = 0;
						uint64_t numberBits = 0;
						uint32_t team = 0;
						if (!ReadOrTruncated(reader.ReadU64LE(writerUID), reader, error, "ai_message_writer_uid") ||
						    !ReadOrTruncated(reader.ReadU64LE(objectUID), reader, error, "ai_message_object_uid") ||
						    !ReadOrTruncated(reader.ReadU32LE(team), reader, error, "ai_message_team") ||
						    !ReadOrTruncated(reader.ReadU8(scriptMessage.context), reader, error, "ai_message_context") ||
						    !ReadOrTruncated(reader.ReadU64LE(numberBits), reader, error, "ai_message_number") ||
						    !ReadOrTruncated(reader.ReadU64LE(contextUID), reader, error, "ai_message_context_uid") ||
						    !reader.ReadString(scriptMessage.message, NetLockstepCodec::c_MaxValueKeyBytes, "ai_message_name", error) ||
						    !reader.ReadString(scriptMessage.text, NetLockstepCodec::c_MaxValueStringBytes, "ai_message_text", error)) {
							return false;
						}
						if (scriptMessage.context >= NetGameAIScriptMessage::ContextCount || scriptMessage.message.empty()) {
							SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "ai script message is invalid");
							return false;
						}
						scriptMessage.writerUID = static_cast<int64_t>(writerUID);
						scriptMessage.objectUID = static_cast<int64_t>(objectUID);
						scriptMessage.team = static_cast<int32_t>(team);
						scriptMessage.number = DoubleFromBitsLE(numberBits);
						scriptMessage.contextUID = static_cast<int64_t>(contextUID);
						command.payload = std::move(scriptMessage);
						break;
					}
					case NetGameCommandType::AIGib: {
						if (version < NetLockstepCodec::c_AIPassEventVersion) {
							SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "ai gib needs a newer frame version");
							return false;
						}
						NetGameAIGib gib;
						uint64_t writerUID = 0;
						uint64_t objectUID = 0;
						uint64_t ignoreUID = 0;
						uint32_t team = 0;
						uint32_t impulseXBits = 0;
						uint32_t impulseYBits = 0;
						if (!ReadOrTruncated(reader.ReadU64LE(writerUID), reader, error, "ai_gib_writer_uid") ||
						    !ReadOrTruncated(reader.ReadU64LE(objectUID), reader, error, "ai_gib_object_uid") ||
						    !ReadOrTruncated(reader.ReadU64LE(ignoreUID), reader, error, "ai_gib_ignore_uid") ||
						    !ReadOrTruncated(reader.ReadU32LE(team), reader, error, "ai_gib_team") ||
						    !ReadOrTruncated(reader.ReadU32LE(impulseXBits), reader, error, "ai_gib_impulse_x") ||
						    !ReadOrTruncated(reader.ReadU32LE(impulseYBits), reader, error, "ai_gib_impulse_y")) {
							return false;
						}
						gib.writerUID = static_cast<int64_t>(writerUID);
						gib.objectUID = static_cast<int64_t>(objectUID);
						gib.ignoreUID = static_cast<int64_t>(ignoreUID);
						gib.team = static_cast<int32_t>(team);
						gib.impulseX = FloatFromBitsLE(impulseXBits);
						gib.impulseY = FloatFromBitsLE(impulseYBits);
						command.payload = gib;
						break;
					}
					case NetGameCommandType::PlaceBrain: {
						if (version < NetLockstepCodec::c_PlaceBrainVersion) {
							SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset() - 2, "game command has invalid type");
							return false;
						}
						NetGamePlaceBrain place;
						uint32_t team = 0;
						uint32_t player = 0;
						uint32_t xBits = 0;
						uint32_t yBits = 0;
						if (!ReadOrTruncated(reader.ReadU32LE(team), reader, error, "place_brain_team") ||
						    !ReadOrTruncated(reader.ReadU32LE(player), reader, error, "place_brain_player") ||
						    !ReadOrTruncated(reader.ReadU32LE(xBits), reader, error, "place_brain_x") ||
						    !ReadOrTruncated(reader.ReadU32LE(yBits), reader, error, "place_brain_y")) {
							return false;
						}
						if (!reader.ReadString(place.className, NetLockstepCodec::c_MaxScenarioBytes, "place_brain_class_name", error) ||
						    !reader.ReadString(place.preset, NetLockstepCodec::c_MaxScenarioBytes, "place_brain_preset", error) ||
						    !reader.ReadString(place.module, NetLockstepCodec::c_MaxScenarioBytes, "place_brain_module", error)) {
							return false;
						}
						place.team = static_cast<int32_t>(team);
						place.player = static_cast<int32_t>(player);
						place.posX = FloatFromBitsLE(xBits);
						place.posY = FloatFromBitsLE(yBits);
						command.payload = std::move(place);
						break;
					}
					case NetGameCommandType::WorldTransition: {
						NetGameWorldTransition transition;
						if (version < NetLockstepCodec::c_WorldTransitionVersion || !DecodeWorldTransition(reader, transition, error)) return false;
						command.payload = std::move(transition);
						break;
					}
					default:
						SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset() - 2, "game command has invalid type");
						return false;
				}
				commands.push_back(std::move(command));
			}
			if (!ValidateCommandCounts(commands, error)) return false;
			return true;
		}

		bool DecodeTickFrames(ByteReader& reader, NetLockstepFrame& tick, uint16_t controllerFrameVersion, const std::vector<ControllerFrame>* previousTick, NetLockstepError* error) {
			uint16_t frameCount = 0;
			const size_t frameBytesSize = ControllerFrameCodec::EncodedSizeFor(controllerFrameVersion);
			if (!ReadOrTruncated(reader.ReadU16LE(frameCount), reader, error, "frame_count") ||
			    !ReadOrTruncated(reader.ReadU64LE(tick.targetFrame), reader, error, "target_frame")) {
				return false;
			}
			if (frameCount > NetLockstepCodec::c_MaxFramesPerPacket) {
				SetError(error, NetLockstepErrorCode::PayloadTooLarge, reader.Offset(), "frame_count exceeds maximum");
				return false;
			}
			tick.frames.reserve(frameCount);
			for (uint16_t i = 0; i < frameCount; ++i) {
				ControllerFrame frame;
				std::string frameError;
				if (!previousTick) {
					const uint8_t* frameBytes = nullptr;
					if (!ReadOrTruncated(reader.ReadBytes(frameBytes, frameBytesSize), reader, error, "ControllerFrame")) {
						return false;
					}
					if (!ControllerFrameCodec::Decode(frameBytes, frameBytesSize, frame, &frameError, controllerFrameVersion)) {
						SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset() - frameBytesSize, "ControllerFrame decode failed: " + frameError);
						return false;
					}
				} else {
					if (reader.Remaining() < 8) {
						SetError(error, NetLockstepErrorCode::TruncatedPayload, reader.Offset(), "ControllerFrame delta is truncated");
						return false;
					}
					uint64_t actorBits = 0;
					const uint8_t* idBytes = reader.Current();
					for (int b = 0; b < 8; ++b) {
						actorBits |= static_cast<uint64_t>(idBytes[b]) << (b * 8);
					}
					const int64_t actorId = static_cast<int64_t>(actorBits);
					const ControllerFrame* previous = nullptr;
					for (const ControllerFrame& candidate : *previousTick) {
						if (candidate.actorUniqueID == actorId) {
							previous = &candidate;
							break;
						}
					}
					size_t consumed = 0;
					if (!ControllerFrameCodec::DecodeDelta(reader.Current(), reader.Remaining(), frame, previous, &consumed, &frameError, controllerFrameVersion)) {
						SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "ControllerFrame delta decode failed: " + frameError);
						return false;
					}
					const uint8_t* skipped = nullptr;
					if (!ReadOrTruncated(reader.ReadBytes(skipped, consumed), reader, error, "ControllerFrame")) {
						return false;
					}
				}
				tick.frames.push_back(frame);
			}
			return ValidateSortedFrames(tick.frames, error);
		}

		bool DecodeFrame(ByteReader& reader, NetLockstepPayload& out, NetLockstepError* error, uint16_t controllerFrameVersion, uint16_t version, NetSoundObservationTables* tables, bool recovery = false) {
			NetLockstepFrame payload;
			uint8_t reserved = 0;
			if (!ReadOrTruncated(reader.ReadU8(payload.senderPeerId), reader, error, "sender_peer_id") ||
			    !ReadOrTruncated(reader.ReadU8(reserved), reader, error, "reserved")) {
				return false;
			}
			if (reserved != 0 && (recovery || reserved > NetLockstepCodec::c_MaxWindowTicks)) {
				SetError(error, NetLockstepErrorCode::ReservedFieldNonZero, 1, "frame reserved byte must be zero");
				return false;
			}
			if (!ValidatePeerId(payload.senderPeerId, error, "sender_peer_id")) {
				return false;
			}
			if (reserved == 0) {
				if (!DecodeTickFrames(reader, payload, controllerFrameVersion, nullptr, error) ||
				    !DecodeCommandList(reader, payload.commands, payload.senderPeerId, version, error)) {
					return false;
				}
			} else {
				if (version >= NetLockstepCodec::c_RoundVersion &&
				    !ReadOrTruncated(reader.ReadU64LE(payload.roundId), reader, error, "round_id")) {
					return false;
				}
				const bool otherRound = tables && payload.roundId != 0 && tables->roundId != 0 && payload.roundId != tables->roundId;
				NetSoundObservationDictionary* dictionary = tables && !otherRound ? &tables->For(payload.senderPeerId) : nullptr;
				std::vector<NetLockstepFrame> ticks;
				ticks.reserve(reserved);
				const std::vector<ControllerFrame>* previousTick = nullptr;
				std::vector<ControllerFrame> previousFrames;
				for (uint8_t i = 0; i < reserved; ++i) {
					NetLockstepFrame tick;
					tick.senderPeerId = payload.senderPeerId;
					tick.roundId = payload.roundId;
					const bool windowCopy = i + 1 < reserved;
					bool readPast = false;
					if (!DecodeTickFrames(reader, tick, controllerFrameVersion, previousTick, error) ||
					    !DecodeCommandList(reader, tick.commands, tick.senderPeerId, version, error)) {
						return false;
					}
					if (version >= NetLockstepCodec::c_RoundVersion) {
						if (!ReadObservations(reader, tick, dictionary, error, version, otherRound, windowCopy, &readPast)) {
							return false;
						}
						if (version >= NetLockstepCodec::c_ValueObservationVersion &&
						    !ReadValueObservations(reader, tick, error, otherRound || readPast)) {
							return false;
						}
					}
					// A copy whose observations stood behind the table was read past: its tick is already in
					// this peer's stream, so it is not committed again, and a relay still forwards it.
					tick.observationsReadPast = readPast;
					ticks.push_back(std::move(tick));
					previousFrames = ticks.back().frames;
					previousTick = &previousFrames;
				}
				payload = std::move(ticks.back());
				payload.priorWindow.assign(std::make_move_iterator(ticks.begin()), std::make_move_iterator(ticks.end() - 1));
				out = std::move(payload);
				return true;
			}
			if (version >= NetLockstepCodec::c_RoundVersion) {
				if (!ReadOrTruncated(reader.ReadU64LE(payload.roundId), reader, error, "round_id")) {
					return false;
				}
				if (recovery) {
					uint16_t count = 0;
					if (!reader.ReadU16LE(count) || count > NetLockstepCodec::c_MaxObservationsPerPacket || reader.Remaining() < size_t(count) * 45) {
						SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "invalid recovery observation count");
						return false;
					}
					payload.observations.reserve(count);
					for (size_t index = 0; index < count; ++index) {
						NetSoundObservation observation;
						uint32_t bits = 0;
						if (!reader.ReadU8(observation.senderPeerId) || !reader.ReadU64LE(observation.objectUID) || !reader.ReadU64LE(observation.tick) ||
						    !reader.ReadU64LE(observation.phase) || !reader.ReadU64LE(observation.occurrence) || !reader.ReadU64LE(observation.ordinal) || !reader.ReadU32LE(bits)) return false;
						observation.value = FloatFromBitsLE(bits);
						if (observation.senderPeerId != payload.senderPeerId || !std::isfinite(observation.value)) {
							SetError(error, NetLockstepErrorCode::InvalidValue, reader.Offset(), "invalid recovery observation");
							return false;
						}
						payload.observations.push_back(observation);
					}
					if (reader.Remaining() != 0 && !ReadValueObservations(reader, payload, error, false)) {
						return false;
					}
					out = std::move(payload);
					return true;
				}
				// A frame from another round decodes like any other and is dropped by the round's own rule, so
				// its sender still counts as heard from; its observations are read past rather than resolved,
				// so one round's slots never stand for another's keys and the counters stay for real holes.
				const bool otherRound = tables && payload.roundId != 0 && tables->roundId != 0 && payload.roundId != tables->roundId;
				NetSoundObservationDictionary* dictionary = tables && !otherRound ? &tables->For(payload.senderPeerId) : nullptr;
				bool readPast = false;
				if (!ReadObservations(reader, payload, dictionary, error, version, otherRound, false, &readPast)) {
					return false;
				}
				if (version >= NetLockstepCodec::c_ValueObservationVersion && reader.Remaining() != 0 && !ReadValueObservations(reader, payload, error, otherRound || readPast)) {
					return false;
				}
				payload.observationsReadPast = readPast;
			}
			out = std::move(payload);
			return true;
		}

		bool DecodeAck(ByteReader& reader, uint16_t version, NetLockstepPayload& out, NetLockstepError* error) {
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
			if (version >= NetLockstepCodec::c_InputAcceptanceVersion && payload.receivedMask == NetLockstepCodec::c_InputAcceptedMask) {
				if (!ReadOrTruncated(reader.ReadU64LE(payload.roundId), reader, error, "round_id") ||
				    !ReadOrTruncated(reader.ReadU32LE(payload.seatIncarnation), reader, error, "seat_incarnation") ||
				    !ReadOrTruncated(reader.ReadU64LE(payload.sessionId), reader, error, "session_id") ||
				    !ReadOrTruncated(reader.ReadU64LE(payload.authorityGeneration), reader, error, "authority_generation") ||
				    payload.roundId == 0 || payload.seatIncarnation == 0 || payload.sessionId == 0) return false;
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
			if (version >= NetLockstepCodec::c_PlayerBindingsVersion) {
				uint8_t count = 0, previous = 0;
				if (!ReadOrTruncated(reader.ReadU8(count), reader, error, "command_ack_count") || count > NetLockstepCodec::c_MaxPeerCount) return false;
				for (uint8_t i = 0; i < count; ++i) {
					uint8_t peer = 0; uint64_t sequence = 0;
					if (!ReadOrTruncated(reader.ReadU8(peer), reader, error, "command_peer") || !ReadVarOrFail(reader, sequence, error, "command_sequence") ||
					    !ValidatePeerId(peer, error, "command_peer") || peer <= previous || sequence == UINT64_MAX) return false;
					payload.appliedCommands.emplace(peer, sequence); previous = peer;
				}
			}
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
		    roundId != rhs.roundId || observations != rhs.observations || valueObservations != rhs.valueObservations || priorWindow != rhs.priorWindow) {
			return false;
		}
		for (size_t i = 0; i < frames.size(); ++i) {
			if (ControllerFrameCodec::Encode(frames[i]) != ControllerFrameCodec::Encode(rhs.frames[i])) {
				return false;
			}
		}
		return true;
	}

	bool NetLockstepCodec::IsWireString(const std::string& value, size_t maxBytes) {
		return value.size() <= maxBytes && value.size() <= std::numeric_limits<uint16_t>::max() && !HasControlChars(value);
	}

	NetLockstepPacketType NetLockstepCodec::PacketTypeOf(const NetLockstepPayload& payload) {
		return std::visit(Overloaded{
			[](const NetLockstepStart&) { return NetLockstepPacketType::Start; },
			[](const NetLockstepFrame&) { return NetLockstepPacketType::Frame; },
			[](const NetLockstepAck&) { return NetLockstepPacketType::Ack; },
			[](const NetLockstepStop&) { return NetLockstepPacketType::Stop; },
			[](const NetLockstepChecksum&) { return NetLockstepPacketType::Checksum; },
			[](const NetLockstepSeatSnapshot&) { return NetLockstepPacketType::SeatSnapshot; },
			[](const NetLockstepRecoveryChunk&) { return NetLockstepPacketType::RecoveryChunk; },
			[](const NetLockstepTiming&) { return NetLockstepPacketType::Timing; },
		}, payload);
	}

	const char* NetLockstepCodec::PacketTypeName(NetLockstepPacketType type) {
		switch (type) {
			case NetLockstepPacketType::Start: return "Start";
			case NetLockstepPacketType::Frame: return "Frame";
			case NetLockstepPacketType::Ack: return "Ack";
			case NetLockstepPacketType::Stop: return "Stop";
			case NetLockstepPacketType::Checksum: return "Checksum";
			case NetLockstepPacketType::SeatSnapshot: return "SeatSnapshot";
			case NetLockstepPacketType::RecoveryChunk: return "RecoveryChunk";
			case NetLockstepPacketType::Timing: return "Timing";
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
			case NetLockstepStopReason::PeerDropped: return "PeerDropped";
			case NetLockstepStopReason::Reclaimed: return "Reclaimed";
			case NetLockstepStopReason::Substituted: return "Substituted";
			case NetLockstepStopReason::PeerRemoved: return "PeerRemoved";
			case NetLockstepStopReason::Expired: return "Expired";
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
		m_LastTickRead = 0;
		m_LastTickRound = 0;
		m_HasReadTick = false;
	}

	bool NetLockstepCodec::Encode(const NetLockstepPacket& packet, std::vector<uint8_t>& outBytes, NetLockstepError* error, NetSoundObservationDictionary* dictionary, size_t* outObservationsEncoded, size_t* outValueObservationsEncoded, NetLockstepObservationBlocks* blocks) {
		std::vector<uint8_t> payloadBytes;
		const bool payloadOk = std::visit(Overloaded{
			[&](const NetLockstepStart& payload) { return EncodePayload(payload, payloadBytes, error); },
			[&](const NetLockstepFrame& payload) { return EncodePayload(payload, payloadBytes, error, dictionary, outObservationsEncoded, false, outValueObservationsEncoded, blocks); },
			[&](const NetLockstepAck& payload) { return EncodePayload(payload, payloadBytes, error); },
			[&](const NetLockstepStop& payload) { return EncodePayload(payload, payloadBytes, error); },
			[&](const NetLockstepChecksum& payload) { return EncodePayload(payload, payloadBytes, error); },
			[&](const NetLockstepSeatSnapshot& payload) { return EncodePayload(payload, payloadBytes, error); },
			[&](const NetLockstepRecoveryChunk& payload) { return EncodePayload(payload, payloadBytes, error); },
			[&](const NetLockstepTiming& payload) { return EncodePayload(payload, payloadBytes, error); },
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

		uint16_t encodeVersion = c_Version;
		if (const auto* start = std::get_if<NetLockstepStart>(&packet.payload); start && start->agreedStartRecord) {
			encodeVersion = c_SeatDeviceVersion;
		}
		if (const auto* frame = std::get_if<NetLockstepFrame>(&packet.payload)) {
			for (const NetGameCommand& command: frame->commands) {
				if (std::holds_alternative<NetGameWorldTransition>(command.payload)) encodeVersion = c_WorldVersion;
			}
			// A checkpoint entry in the tick or in any older tick the packet repeats needs the newest wire.
			const auto carriesCheckpoint = [](const NetLockstepFrame& tick) {
				return std::any_of(tick.commands.begin(), tick.commands.end(), [](const NetGameCommand& command) { return std::holds_alternative<NetGameCheckpoint>(command.payload); });
			};
			if (carriesCheckpoint(*frame) || std::any_of(frame->priorWindow.begin(), frame->priorWindow.end(), carriesCheckpoint)) encodeVersion = c_CheckpointVersion;
		}

		outBytes.clear();
		outBytes.reserve(c_HeaderBytes + payloadBytes.size());
		AppendU32LE(outBytes, c_Magic);
		AppendU16LE(outBytes, encodeVersion);
		AppendU16LE(outBytes, c_HeaderBytes);
		AppendU16LE(outBytes, static_cast<uint16_t>(PacketTypeOf(packet.payload)));
		AppendU16LE(outBytes, 0);
		AppendU32LE(outBytes, static_cast<uint32_t>(payloadBytes.size()));
		outBytes.insert(outBytes.end(), payloadBytes.begin(), payloadBytes.end());
		return true;
	}

	bool NetLockstepCodec::EncodeRecoveryInput(const NetLockstepFrame& frame, std::vector<uint8_t>& outBytes, NetLockstepError* error) {
		std::vector<uint8_t> bytes;
		AppendU32LE(bytes, c_RecoveryInputMagic);
		AppendU16LE(bytes, c_RecoveryInputVersion);
		AppendU16LE(bytes, ControllerFrame::c_Version);
		if (!EncodePayload(frame, bytes, error, nullptr, nullptr, true)) return false;
		if (bytes.size() > c_MaxRecoveryInputBytes) {
			SetError(error, NetLockstepErrorCode::PayloadTooLarge, bytes.size(), "recovery input exceeds maximum");
			return false;
		}
		outBytes = std::move(bytes);
		return true;
	}

	bool NetLockstepCodec::DecodeRecoveryInput(const std::vector<uint8_t>& bytes, NetLockstepFrame& outFrame, NetLockstepError* error) {
		if (bytes.size() < 8 || bytes.size() > c_MaxRecoveryInputBytes) {
			SetError(error, NetLockstepErrorCode::PayloadTooLarge, 0, "invalid recovery input size");
			return false;
		}
		ByteReader reader(bytes.data(), bytes.size());
		uint32_t magic = 0;
		uint16_t version = 0, controllerVersion = 0;
		reader.ReadU32LE(magic);
		reader.ReadU16LE(version);
		reader.ReadU16LE(controllerVersion);
		if (magic != c_RecoveryInputMagic || version == 0 || version > c_RecoveryInputVersion || controllerVersion != ControllerFrame::c_Version) {
			SetError(error, NetLockstepErrorCode::UnsupportedVersion, 0, "unsupported recovery input header");
			return false;
		}
		NetLockstepPayload payload;
		// Each recovery layout keeps the command vocabulary it recorded.
		if (!DecodeFrame(reader, payload, error, controllerVersion, version == 1 ? c_WorldTransitionVersion : version == 2 ? 25 : version == 3 ? 27 : version == 4 ? c_WorldVersion : c_CheckpointVersion, nullptr, true) || !reader.AtEnd()) return false;
		NetLockstepFrame frame = std::get<NetLockstepFrame>(std::move(payload));
		std::vector<uint8_t> canonical;
		AppendU32LE(canonical, c_RecoveryInputMagic);
		AppendU16LE(canonical, version);
		AppendU16LE(canonical, ControllerFrame::c_Version);
		if (!EncodePayload(frame, canonical, error, nullptr, nullptr, true, nullptr, nullptr, version >= 3, version >= 4)) return false;
		if (canonical != bytes) {
			SetError(error, NetLockstepErrorCode::InvalidValue, 0, "noncanonical recovery input");
			return false;
		}
		outFrame = std::move(frame);
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
		if (version < c_MinVersion || version > c_CheckpointVersion) {
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
			case NetLockstepPacketType::SeatSnapshot:
			case NetLockstepPacketType::RecoveryChunk:
			case NetLockstepPacketType::Timing:
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
				payloadOk = DecodeAck(payloadReader, version, payload, &payloadError);
				break;
			case NetLockstepPacketType::Stop:
				payloadOk = DecodeStop(payloadReader, payload, &payloadError);
				break;
			case NetLockstepPacketType::Checksum:
				payloadOk = DecodeChecksum(payloadReader, payload, &payloadError, version);
				break;
			case NetLockstepPacketType::SeatSnapshot:
				payloadOk = DecodeSeatSnapshot(payloadReader, payload, &payloadError, version);
				break;
			case NetLockstepPacketType::RecoveryChunk:
				payloadOk = DecodeRecoveryChunk(payloadReader, payload, &payloadError, version);
				break;
			case NetLockstepPacketType::Timing:
				payloadOk = DecodeTiming(payloadReader, payload, &payloadError, version);
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
			case NetLockstepPacketType::SeatSnapshot:
			case NetLockstepPacketType::RecoveryChunk:
			case NetLockstepPacketType::Timing:
				break;
			default:
				return false;
		}
		return magic == c_Magic && version >= c_MinVersion && version <= c_CheckpointVersion && headerBytes == c_HeaderBytes &&
		       flags == 0 && bytes.size() == static_cast<size_t>(c_HeaderBytes) + payloadLength;
	}

	bool NetHostMigrationCodec::LooksLikePacket(const std::vector<uint8_t>& bytes) {
		uint32_t magic = 0;
		ByteReader reader(bytes.data(), bytes.size());
		return reader.ReadU32LE(magic) && magic == c_Magic;
	}

	bool NetHostMigrationCodec::Encode(const NetHostMigrationMessage& message, const NetHash32& key, std::vector<uint8_t>& bytes) {
		const auto type = static_cast<uint16_t>(message.type);
		if (type < 1 || type > static_cast<uint16_t>(NetHostMigrationMessageType::Abort) || message.sessionId == 0 || message.roundId == 0 || message.generation == 0 ||
		    message.senderPeerId == 0 || message.senderPeerId > NetMatchConfigUtil::c_MaxPeerCount || message.successorPeerId == 0 || message.successorPeerId > NetMatchConfigUtil::c_MaxPeerCount ||
		    message.members.size() > NetMatchConfigUtil::c_MaxPeerCount || message.futureDelays.size() > 128 ||
		    (!message.futureDelays.empty() && message.bytes.size() > 4) || message.bytes.size() > c_ChunkBytes || message.totalBytes > c_MaxFrameBytes ||
		    std::all_of(key.begin(), key.end(), [](uint8_t value) { return value == 0; }))
			return false;
		bytes.clear();
		AppendU32LE(bytes, c_Magic);
		AppendU16LE(bytes, c_Version);
		AppendU16LE(bytes, type);
		AppendU64LE(bytes, message.sessionId);
		AppendU64LE(bytes, message.roundId);
		AppendU64LE(bytes, message.generation);
		AppendU8(bytes, message.senderPeerId);
		AppendU8(bytes, message.successorPeerId);
		bytes.insert(bytes.end(), message.configHash.begin(), message.configHash.end());
		AppendU64LE(bytes, message.appliedFrame);
		AppendU64LE(bytes, message.completeFrom);
		AppendU64LE(bytes, message.boundary);
		AppendU64LE(bytes, message.frame);
		AppendU32LE(bytes, message.totalBytes);
		AppendU32LE(bytes, message.offset);
		AppendU8(bytes, static_cast<uint8_t>(message.members.size()));
		bytes.insert(bytes.end(), message.members.begin(), message.members.end());
		AppendU32LE(bytes, static_cast<uint32_t>(message.bytes.size()));
		bytes.insert(bytes.end(), message.bytes.begin(), message.bytes.end());
		AppendU16LE(bytes, static_cast<uint16_t>(message.futureDelays.size()));
		for (const auto& decision: message.futureDelays) {
			if (decision.action != NetTimingAction::Delay || decision.phase != NetTimingPhase::Commit || decision.sessionId != message.sessionId) return false;
			std::vector<uint8_t> encoded;
			if (!NetLockstepCodec::Encode({decision}, encoded) || encoded.size() > 256) return false;
			AppendU16LE(bytes, static_cast<uint16_t>(encoded.size())); bytes.insert(bytes.end(), encoded.begin(), encoded.end());
		}
		uint8_t mac[32]{};
		if (!GetNetAuthCrypto().HmacSha256(key.data(), key.size(), bytes.data(), bytes.size(), mac)) {
			bytes.clear();
			return false;
		}
		bytes.insert(bytes.end(), std::begin(mac), std::end(mac));
		return true;
	}

	bool NetHostMigrationCodec::Decode(const std::vector<uint8_t>& bytes, const NetHash32& key, NetHostMigrationMessage& message) {
		if (bytes.size() < 32 || bytes.size() > c_ChunkBytes + 192 || std::all_of(key.begin(), key.end(), [](uint8_t value) { return value == 0; }))
			return false;
		uint8_t mac[32]{};
		const size_t bodySize = bytes.size() - sizeof(mac);
		if (!GetNetAuthCrypto().HmacSha256(key.data(), key.size(), bytes.data(), bodySize, mac) || !NetAuthConstantTimeEquals(mac, bytes.data() + bodySize, sizeof(mac)))
			return false;
		ByteReader reader(bytes.data(), bodySize);
		NetHostMigrationMessage decoded;
		uint32_t magic = 0, count = 0;
		uint16_t version = 0, type = 0;
		uint8_t members = 0;
		const uint8_t* data = nullptr;
		if (!reader.ReadU32LE(magic) || magic != c_Magic || !reader.ReadU16LE(version) || version != c_Version || !reader.ReadU16LE(type) ||
		    type < 1 || type > static_cast<uint16_t>(NetHostMigrationMessageType::Abort) || !reader.ReadU64LE(decoded.sessionId) || !reader.ReadU64LE(decoded.roundId) || !reader.ReadU64LE(decoded.generation) ||
		    !reader.ReadU8(decoded.senderPeerId) || !reader.ReadU8(decoded.successorPeerId) || !reader.ReadBytes(data, decoded.configHash.size()))
			return false;
		std::copy_n(data, decoded.configHash.size(), decoded.configHash.begin());
		if (!reader.ReadU64LE(decoded.appliedFrame) || !reader.ReadU64LE(decoded.completeFrom) || !reader.ReadU64LE(decoded.boundary) || !reader.ReadU64LE(decoded.frame) ||
		    !reader.ReadU32LE(decoded.totalBytes) || decoded.totalBytes > c_MaxFrameBytes || !reader.ReadU32LE(decoded.offset) || !reader.ReadU8(members) || members > NetMatchConfigUtil::c_MaxPeerCount || !reader.ReadBytes(data, members))
			return false;
		decoded.members.assign(data, data + members);
		std::set<uint8_t> unique;
		for (uint8_t peer: decoded.members)
			if (peer == 0 || peer > NetMatchConfigUtil::c_MaxPeerCount || !unique.insert(peer).second)
				return false;
		if (!reader.ReadU32LE(count) || count > c_ChunkBytes || !reader.ReadBytes(data, count) || decoded.sessionId == 0 || decoded.roundId == 0 || decoded.generation == 0 ||
		    decoded.senderPeerId == 0 || decoded.senderPeerId > NetMatchConfigUtil::c_MaxPeerCount || decoded.successorPeerId == 0 || decoded.successorPeerId > NetMatchConfigUtil::c_MaxPeerCount)
			return false;
		decoded.bytes.assign(data, data + count);
		uint16_t decisions = 0;
		if (!reader.ReadU16LE(decisions) || decisions > 128 || (decisions != 0 && decoded.bytes.size() > 4)) return false;
		for (uint16_t index = 0; index < decisions; ++index) {
			uint16_t length = 0;
			if (!reader.ReadU16LE(length) || length > 256 || !reader.ReadBytes(data, length)) return false;
			const auto packet = NetLockstepCodec::Decode(data, length);
			if (!packet.ok) return false;
			const auto* decision = std::get_if<NetLockstepTiming>(&packet.packet.payload);
			if (!decision || decision->action != NetTimingAction::Delay || decision->phase != NetTimingPhase::Commit || decision->sessionId != decoded.sessionId) return false;
			decoded.futureDelays.push_back(*decision);
		}
		if (!reader.AtEnd()) return false;
		decoded.type = static_cast<NetHostMigrationMessageType>(type);
		message = std::move(decoded);
		return true;
	}

	bool NetLockstepCoordinator::EncodeMigrationFrame(const NetLockstepReadyFrame& ready, std::vector<uint8_t>& bytes) const {
		bytes.clear();
		AppendU8(bytes, static_cast<uint8_t>(ready.departedPeerIds.size()));
		bytes.insert(bytes.end(), ready.departedPeerIds.begin(), ready.departedPeerIds.end());
		for (const auto* facts: {&ready.committedPeerLeaves, &ready.committedFrameWaivers}) {
			AppendU8(bytes, static_cast<uint8_t>(facts->size()));
			for (const auto& [peer, boundary]: *facts) {
				AppendU8(bytes, peer);
				AppendU64LE(bytes, boundary);
			}
		}
		AppendU8(bytes, m_Config.peerCount);
		size_t remoteOffset = 0;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			NetLockstepFrame input;
			input.senderPeerId = peer;
			input.targetFrame = ready.frame;
			input.roundId = m_Config.matchConfig.roundId;
			const bool local = peer == m_Config.localPeerId;
			const bool present = local ? ready.hasLocalInput : ready.remoteFrameCounts.contains(peer);
			if (local)
				input.frames = ready.localFrames;
			else if (present) {
				const size_t count = ready.remoteFrameCounts.at(peer);
				if (remoteOffset > ready.remoteFrames.size() || count > ready.remoteFrames.size() - remoteOffset)
					return false;
				input.frames.assign(ready.remoteFrames.begin() + remoteOffset, ready.remoteFrames.begin() + remoteOffset + count);
				remoteOffset += count;
			}
			for (const auto& command: (local ? ready.localCommands : ready.remoteCommands))
				if (command.senderPeerId == peer)
					input.commands.push_back(command);
			for (const auto& observation: (local ? ready.localObservations : ready.remoteObservations))
				if (observation.senderPeerId == peer)
					input.observations.push_back(observation);
			for (const auto& observation: (local ? ready.localValueObservations : ready.remoteValueObservations))
				if (observation.senderPeerId == peer)
					input.valueObservations.push_back(observation);
			std::vector<uint8_t> encoded;
			if (!NetLockstepCodec::EncodeRecoveryInput(input, encoded))
				return false;
			AppendU8(bytes, present ? 1 : 0);
			AppendU32LE(bytes, static_cast<uint32_t>(encoded.size()));
			bytes.insert(bytes.end(), encoded.begin(), encoded.end());
		}
		return bytes.size() <= NetHostMigrationCodec::c_MaxFrameBytes && remoteOffset == ready.remoteFrames.size();
	}

	bool NetLockstepCoordinator::DecodeMigrationFrame(const std::vector<uint8_t>& bytes, uint64_t frame, NetLockstepReadyFrame& ready) const {
		ByteReader reader(bytes.data(), bytes.size());
		uint8_t departures = 0, peers = 0;
		const uint8_t* data = nullptr;
		NetLockstepReadyFrame decoded;
		decoded.frame = frame;
		if (!reader.ReadU8(departures) || departures > m_Config.peerCount || !reader.ReadBytes(data, departures))
			return false;
		decoded.departedPeerIds.assign(data, data + departures);
		if (!std::is_sorted(decoded.departedPeerIds.begin(), decoded.departedPeerIds.end()) || std::adjacent_find(decoded.departedPeerIds.begin(), decoded.departedPeerIds.end()) != decoded.departedPeerIds.end())
			return false;
		for (uint8_t peer: decoded.departedPeerIds)
			if (peer == 0 || peer > m_Config.peerCount)
				return false;
		for (auto* facts: {&decoded.committedPeerLeaves, &decoded.committedFrameWaivers}) {
			uint8_t count = 0;
			if (!reader.ReadU8(count) || count > m_Config.peerCount)
				return false;
			for (uint8_t index = 0; index < count; ++index) {
				uint8_t peer = 0;
				uint64_t boundary = 0;
				if (!reader.ReadU8(peer) || peer == 0 || peer > m_Config.peerCount || !reader.ReadU64LE(boundary) || !facts->emplace(peer, boundary).second)
					return false;
			}
		}
		if (!reader.ReadU8(peers) || peers != m_Config.peerCount)
			return false;
		for (uint8_t peer = 1; peer <= peers; ++peer) {
			uint8_t present = 0;
			uint32_t size = 0;
			if (!reader.ReadU8(present) || present > 1 || !reader.ReadU32LE(size) || size > NetLockstepCodec::c_MaxRecoveryInputBytes || !reader.ReadBytes(data, size))
				return false;
			NetLockstepFrame input;
			if (!NetLockstepCodec::DecodeRecoveryInput(std::vector<uint8_t>(data, data + size), input) || input.senderPeerId != peer || input.targetFrame != frame || input.roundId != m_Config.matchConfig.roundId)
				return false;
			if (!present && (!input.frames.empty() || !input.commands.empty() || !input.observations.empty() || !input.valueObservations.empty()))
				return false;
			if (peer == m_Config.localPeerId) {
				decoded.hasLocalInput = present != 0;
				decoded.localFrames = std::move(input.frames);
				decoded.localCommands = std::move(input.commands);
				decoded.localObservations = std::move(input.observations);
				decoded.localValueObservations = std::move(input.valueObservations);
			} else {
				if (present)
					decoded.remoteFrameCounts[peer] = input.frames.size();
				decoded.remoteFrames.insert(decoded.remoteFrames.end(), input.frames.begin(), input.frames.end());
				decoded.remoteCommands.insert(decoded.remoteCommands.end(), input.commands.begin(), input.commands.end());
				decoded.remoteObservations.insert(decoded.remoteObservations.end(), input.observations.begin(), input.observations.end());
				decoded.remoteValueObservations.insert(decoded.remoteValueObservations.end(), input.valueObservations.begin(), input.valueObservations.end());
			}
		}
		if (!reader.AtEnd())
			return false;
		ready = std::move(decoded);
		return true;
	}

	void NetLockstepCoordinator::RetainMigrationFrame(const NetLockstepReadyFrame& ready) {
		if (m_Config.matchConfig.successorOrder.empty())
			return;
		std::vector<uint8_t> bytes;
		if (EncodeMigrationFrame(ready, bytes)) {
			StoreMigrationFrame(ready.frame, std::move(bytes));
		}
	}

	void NetLockstepCoordinator::StoreMigrationFrame(uint64_t frame, std::vector<uint8_t> bytes) {
		auto& stored = m_MigrationHistory[frame];
		m_MigrationHistoryBytes -= stored.size();
		stored = std::move(bytes);
		m_MigrationHistoryBytes += stored.size();
		while (m_MigrationHistory.size() > NetHostMigrationCodec::c_HistoryFrames || m_MigrationHistoryBytes > NetHostMigrationCodec::c_MaxHistoryBytes) {
			const auto oldest = m_MigrationHistory.begin();
			m_MigrationHistoryBytes -= oldest->second.size();
			m_MigrationHistory.erase(oldest);
		}
	}

	NetHostMigrationMessage NetLockstepCoordinator::MigrationMessage(NetHostMigrationMessageType type) const {
		NetHostMigrationMessage message;
		message.type = type;
		message.sessionId = m_Config.sessionId;
		message.roundId = m_Config.matchConfig.roundId;
		message.generation = m_MigrationGeneration;
		message.frame = type == NetHostMigrationMessageType::Plan || type == NetHostMigrationMessageType::Commit || type == NetHostMigrationMessageType::Rejoin ? m_MigrationWireRound : m_RoundId;
		message.senderPeerId = m_Config.localPeerId;
		message.successorPeerId = m_MigrationSuccessor;
		message.configHash = m_RoundConfigHash;
		message.appliedFrame = GetResumeFrame() > 0 ? GetResumeFrame() - 1 : 0;
		message.completeFrom = message.appliedFrame + 1;
		while (message.completeFrom > 0 && m_MigrationHistory.contains(message.completeFrom - 1))
			--message.completeFrom;
		if (m_MigrationNeedsResync)
			message.completeFrom = UINT64_MAX;
		message.boundary = m_MigrationBoundary;
		message.members = m_MigrationResult.members;
		if (type == NetHostMigrationMessageType::Plan || type == NetHostMigrationMessageType::Commit) message.futureDelays = m_MigrationFutureDelays;
		if (type == NetHostMigrationMessageType::Answer || type == NetHostMigrationMessageType::Ready) {
			for (const auto& decision: m_MigrationFutureDelays) if (decision.applyFrame > message.appliedFrame) message.futureDelays.push_back(decision);
			for (const auto& [revision, decision]: m_TimingDecisions) {
				if (!decision.committed || decision.proposal.action != NetTimingAction::Delay || decision.proposal.applyFrame <= message.appliedFrame) continue;
				auto committed = decision.proposal; committed.phase = NetTimingPhase::Commit;
				if (std::find(message.futureDelays.begin(), message.futureDelays.end(), committed) == message.futureDelays.end()) message.futureDelays.push_back(committed);
			}
		}
		return message;
	}

	bool NetLockstepCoordinator::SendMigration(NetPeerId peer, NetHostMigrationMessage message) {
		if (peer == c_InvalidNetPeerId)
			return false;
		std::vector<uint8_t> bytes;
		if (!NetHostMigrationCodec::Encode(message, m_Config.migrationKey, bytes))
			return false;
		auto& pending = m_MigrationOutbox[peer];
		INetTransport* wire = m_MigrationTransport ? m_MigrationTransport.get() : m_Transport;
		if (pending.empty() && wire->Send(peer, NetTransportLane::ControlReliable, bytes))
			return true;
		if (pending.size() >= 128)
			return false;
		pending.push_back(std::move(bytes));
		return true;
	}

	bool NetLockstepCoordinator::BeginHostMigration(uint64_t nowMs) {
		NET_PLANE_CHECK();
		if (IsMigrating())
			return true;
		// An election is the simulation thread's to start: the plane only keeps the round's frames moving.
		if (m_PlaneTicking)
			return false;
		if (!IsRunning() || m_Config.matchConfig.dedicated || m_Config.matchConfig.persistentWorld || m_Config.matchConfig.successorOrder.empty() || !m_Config.migrationTransportFactory ||
		    m_Config.localPeerId == GetHostPeerId() || m_RoundId == 0 || m_MigrationGeneration == UINT64_MAX || std::all_of(m_Config.migrationKey.begin(), m_Config.migrationKey.end(), [](uint8_t value) { return value == 0; }))
			return false;
		++m_MigrationGeneration;
		m_MigrationWireRound = m_RoundId;
		m_MigrationPhase = NetHostMigrationPhase::Contacting;
		m_MigrationSinceMs = nowMs;
		m_MigrationStartedMs = nowMs;
		m_MigrationLastSendMs = 0;
		m_MigrationBoundary = 0;
		m_MigrationCandidateIndex = 0;
		m_MigrationSuccessor = 0;
		m_MigrationNextAddress = 0;
		m_MigrationAddress.clear();
		m_MigrationDonor = 0;
		m_MigrationHostTransport = c_InvalidNetPeerId;
		m_MigrationAnswers.clear();
		m_MigrationExpected.clear();
		m_MigrationAuthoritySeen = false;
		m_MigrationPeers.clear();
		m_MigrationReady.clear();
		m_MigrationIncoming.clear();
		m_MigrationOutbox.clear();
		m_MigrationFrameQueue.clear();
		m_MigrationResult = {};
		m_MigrationNeedsResync = false;
		m_MigrationCommitQueued = false;
		m_MigrationEarlyInputs.clear();
		m_MigrationAdmissionEvents.clear();
		m_OwnEndDuringMigration.reset();
		m_ReadyFrames.clear();
		m_PendingRecoveryStop.reset();
		m_PendingCompleteStop.reset();
		m_Stats.nextFrame = GetResumeFrame();
		m_Stats.timeoutReason.clear();
		std::cout << "[net-match] host lost; collecting surviving peers at applied frame " << (GetResumeFrame() > 0 ? GetResumeFrame() - 1 : 0) << std::endl;
		return true;
	}

	bool NetLockstepCoordinator::BeginHostMigrationAfterHeal(uint64_t nowMs) {
		NET_PLANE_CHECK();
		const auto previous = m_State;
		m_State = NetLockstepState::Running;
		if (!BeginHostMigration(nowMs)) {
			m_State = previous;
			return false;
		}
		m_MigrationNeedsResync = true;
		return true;
	}

	bool NetLockstepCoordinator::ConnectMigrationEndpoint(INetTransport& transport, const NetMatchMigrationPeer& peer, size_t& nextAddress, std::string& connectedAddress, std::string* error) {
		while (nextAddress < peer.listenAddrs.size()) {
			const std::string& address = peer.listenAddrs[nextAddress++];
			if (transport.Connect(address, peer.listenPort, error)) {
				connectedAddress = address;
				return true;
			}
		}
		return false;
	}

	bool NetLockstepCoordinator::ContactMigrationSuccessor(uint64_t nowMs) {
		const auto& order = m_Config.matchConfig.successorOrder;
		while (m_MigrationCandidateIndex < order.size() && (order[m_MigrationCandidateIndex] == GetHostPeerId() || IsPeerGoneAtFrame(order[m_MigrationCandidateIndex], GetResumeFrame()) ||
		                                                   IsLostMigrationSuccessor(order[m_MigrationCandidateIndex])))
			++m_MigrationCandidateIndex;
		if (m_MigrationCandidateIndex == order.size()) {
			FailHostMigration("no surviving successor");
			return false;
		}
		if (m_MigrationSuccessor != order[m_MigrationCandidateIndex]) {
			m_MigrationAuthoritySeen = false;
			m_MigrationNextAddress = 0;
		}
		m_MigrationSuccessor = order[m_MigrationCandidateIndex];
		const auto endpoint = std::find_if(m_Config.matchConfig.migrationPeers.begin(), m_Config.matchConfig.migrationPeers.end(), [&](const auto& peer) { return peer.peerId == m_MigrationSuccessor; });
		if (endpoint == m_Config.matchConfig.migrationPeers.end()) {
			FailHostMigration("successor has no agreed listen endpoint");
			return false;
		}
		if (!m_MigrationListener) {
			const auto local = std::find_if(m_Config.matchConfig.migrationPeers.begin(), m_Config.matchConfig.migrationPeers.end(), [&](const auto& peer) { return peer.peerId == m_Config.localPeerId; });
			m_MigrationListener = m_Config.migrationTransportFactory();
			if (local == m_Config.matchConfig.migrationPeers.end() || !m_MigrationListener || !m_MigrationListener->StartHost(local->listenPort)) {
				FailHostMigration("the local handover listener could not open");
				return false;
			}
		}
		m_MigrationSinceMs = nowMs;
		m_MigrationLastSendMs = nowMs;
		m_MigrationPeers.clear();
		m_MigrationAnswers.clear();
		m_MigrationOutbox.clear();
		m_MigrationFrameQueue.clear();
		m_MigrationHostTransport = c_InvalidNetPeerId;
		std::string error;
		const bool hosting = m_MigrationSuccessor == m_Config.localPeerId;
		m_MigrationTransport = hosting ? std::move(m_MigrationListener) : m_Config.migrationTransportFactory();
		if (!m_MigrationTransport) {
			FailHostMigration("the handover transport could not be created");
			return false;
		}
		if (m_MigrationNextAddress == endpoint->listenAddrs.size()) {
			m_MigrationNextAddress = 0;
		}
		const bool opened = m_MigrationTransport && (hosting || ConnectMigrationEndpoint(*m_MigrationTransport, *endpoint, m_MigrationNextAddress, m_MigrationAddress, &error));
		if (!opened && hosting) {
			FailHostMigration("successor listen failed: " + error);
			return false;
		}
		m_MigrationProbes.clear();
		m_MigrationExpected.clear();
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if (peer != GetHostPeerId() && !IsPeerGoneAtFrame(peer, GetResumeFrame())) {
				m_MigrationExpected.insert(peer);
			}
		}
		if (hosting)
			for (const auto& peer: m_Config.matchConfig.migrationPeers) {
				if (peer.peerId == m_Config.localPeerId || peer.peerId == GetHostPeerId() || IsPeerGoneAtFrame(peer.peerId, GetResumeFrame()))
					continue;
				MigrationProbe probe;
				probe.transport = m_Config.migrationTransportFactory();
				probe.lastDialMs = nowMs;
				if (probe.transport && ConnectMigrationEndpoint(*probe.transport, peer, probe.nextAddress, probe.address)) {
					m_MigrationProbes[peer.peerId] = std::move(probe);
				}
			}
		if (hosting)
			m_MigrationAnswers[m_Config.localPeerId] = MigrationMessage(NetHostMigrationMessageType::Answer);
		return true;
	}

	bool NetLockstepCoordinator::IsLostMigrationSuccessor(uint8_t peerId) const {
		return std::find(m_MigrationLostSuccessors.begin(), m_MigrationLostSuccessors.end(), peerId) != m_MigrationLostSuccessors.end();
	}

	// An open handover link is the candidate answering for itself: a stalled exchange must not elect a
	// second host past a peer that is plainly alive, so the candidate is kept until the whole election expires.
	bool NetLockstepCoordinator::HoldsLiveMigrationCandidate(uint64_t nowMs, uint64_t budget) const {
		return m_MigrationHostTransport != c_InvalidNetPeerId && nowMs >= m_MigrationStartedMs && nowMs - m_MigrationStartedMs < 3 * budget;
	}

	// A peer the successor left out of its roster waits for the commit; when the successor dies first the
	// successor is this peer's dead host, so the next generation is elected exactly as an ordinary host loss.
	bool NetLockstepCoordinator::RestartHostMigrationAfterSuccessorLoss(uint64_t nowMs) {
		const uint8_t lost = m_MigrationSuccessor;
		if (lost == 0 || lost == m_Config.localPeerId)
			return false;
		if (!IsLostMigrationSuccessor(lost))
			m_MigrationLostSuccessors.push_back(lost);
		const auto& order = m_Config.matchConfig.successorOrder;
		if (std::none_of(order.begin(), order.end(), [&](uint8_t peer) { return peer != GetHostPeerId() && !IsPeerGoneAtFrame(peer, GetResumeFrame()) && !IsLostMigrationSuccessor(peer); }))
			return false;
		m_MigrationPhase = NetHostMigrationPhase::None;
		std::cout << "[net-match] handover successor " << static_cast<int>(lost) << " was lost before the commit; electing again" << std::endl;
		if (BeginHostMigration(nowMs))
			return true;
		m_MigrationPhase = NetHostMigrationPhase::WaitingForReady;
		return false;
	}

	void NetLockstepCoordinator::TickMigrationRollCallLinks(uint64_t nowMs) {
		const auto authenticated = [&](const NetTransportEvent& event, NetHostMigrationMessage& message) {
			return event.type == NetTransportEventType::PacketReceived && event.lane == NetTransportLane::ControlReliable && NetHostMigrationCodec::Decode(event.bytes, m_Config.migrationKey, message) &&
			       message.sessionId == m_Config.sessionId && message.roundId == m_Config.matchConfig.roundId && message.configHash == m_RoundConfigHash;
		};
		if (m_MigrationListener) {
			INetTransport* listener = m_MigrationListener.get();
			for (const auto& event: listener->PollEvents()) {
				NetHostMigrationMessage request;
				if (!authenticated(event, request) || request.type != NetHostMigrationMessageType::RollCall || request.senderPeerId != request.successorPeerId)
					continue;
				// A peer that elected again is a generation or more ahead; a peer left behind joins the one it is shown.
				if (!IsMigrating() && request.generation > m_MigrationGeneration) {
					const uint64_t previous = m_MigrationGeneration;
					m_MigrationGeneration = request.generation - 1;
					if (!BeginHostMigration(nowMs))
						m_MigrationGeneration = previous;
				}
				if (m_MigrationSuccessor == 0 && IsMigrating())
					(void)ContactMigrationSuccessor(nowMs);
				if (request.generation != m_MigrationGeneration || m_MigrationSuccessor == 0)
					continue;
				if (request.successorPeerId == m_MigrationSuccessor) {
					m_MigrationAuthoritySeen = true;
				}
				std::vector<uint8_t> bytes;
				if (NetHostMigrationCodec::Encode(MigrationMessage(NetHostMigrationMessageType::Answer), m_Config.migrationKey, bytes))
					(void)listener->Send(event.peerId, NetTransportLane::ControlReliable, bytes);
			}
		}
		uint8_t earlier = 0;
		NetTransportEvent redirect;
		uint8_t redirectPeer = 0;
		NetHostMigrationMessage redirectMessage;
		for (auto& [peer, dial]: m_MigrationProbes) {
			auto& probe = dial.transport;
			if (!probe)
				continue;
			const auto endpoint = std::find_if(m_Config.matchConfig.migrationPeers.begin(), m_Config.matchConfig.migrationPeers.end(), [&](const auto& entry) { return entry.peerId == peer; });
			if (!dial.answered && endpoint != m_Config.matchConfig.migrationPeers.end() && dial.nextAddress < endpoint->listenAddrs.size() && nowMs >= dial.lastDialMs + 250) {
				probe->Stop();
				(void)ConnectMigrationEndpoint(*probe, *endpoint, dial.nextAddress, dial.address);
				dial.lastDialMs = nowMs;
			}
			for (const auto& event: probe->PollEvents()) {
				if (event.type == NetTransportEventType::PeerConnected) {
					std::vector<uint8_t> bytes;
					if (NetHostMigrationCodec::Encode(MigrationMessage(NetHostMigrationMessageType::RollCall), m_Config.migrationKey, bytes))
						(void)probe->Send(event.peerId, NetTransportLane::ControlReliable, bytes);
				}
				NetHostMigrationMessage answer;
				if (!authenticated(event, answer) || answer.senderPeerId != peer)
					continue;
				dial.answered = true;
				if (answer.type == NetHostMigrationMessageType::Rejoin && answer.successorPeerId == peer && answer.generation >= m_MigrationGeneration) {
					redirectPeer = peer;
					redirect = event;
					redirectMessage = answer;
				}
				if (m_MigrationPhase == NetHostMigrationPhase::Contacting && answer.type == NetHostMigrationMessageType::Answer && answer.successorPeerId == peer && answer.generation == m_MigrationGeneration) {
					const auto& order = m_Config.matchConfig.successorOrder;
					if (std::find(order.begin(), order.end(), peer) < std::find(order.begin(), order.end(), m_MigrationSuccessor))
						earlier = peer;
				}
			}
		}
		if (redirectPeer != 0) {
			m_MigrationAddress = m_MigrationProbes.at(redirectPeer).address;
			m_MigrationTransport = std::move(m_MigrationProbes.at(redirectPeer).transport);
			m_MigrationProbes.clear();
			m_MigrationOutbox.clear();
			m_MigrationFrameQueue.clear();
			m_MigrationSuccessor = redirectPeer;
			m_MigrationGeneration = redirectMessage.generation;
			m_MigrationHostTransport = redirect.peerId;
			HandleMigrationEvent(redirect, nowMs);
		} else if (earlier != 0 && !m_MigrationAuthoritySeen) {
			const auto& order = m_Config.matchConfig.successorOrder;
			m_MigrationCandidateIndex = static_cast<size_t>(std::find(order.begin(), order.end(), earlier) - order.begin());
			m_MigrationTransport.reset();
			(void)ContactMigrationSuccessor(nowMs);
		}
	}

	void NetLockstepCoordinator::FailHostMigration(const std::string& reason) {
		if (m_MigrationSuccessor == m_Config.localPeerId) {
			for (const auto& [peer, transport]: m_MigrationPeers)
				(void)SendMigration(transport, MigrationMessage(NetHostMigrationMessageType::Abort));
		}
		m_MigrationPhase = NetHostMigrationPhase::Failed;
		m_State = NetLockstepState::Stopped;
		m_Stats.timeoutReason = "Complete:host handover ended: " + reason;
		std::cout << "[net-match] " << m_Stats.timeoutReason << std::endl;
	}

	void NetLockstepCoordinator::SendMigrationFrame(NetPeerId peer, uint64_t frame) {
		if (std::none_of(m_MigrationFrameQueue.begin(), m_MigrationFrameQueue.end(), [&](const auto& queued) { return std::get<0>(queued) == peer && std::get<1>(queued) == frame; }))
			m_MigrationFrameQueue.emplace_back(peer, frame, 0);
	}

	void NetLockstepCoordinator::PublishMigrationPlan(uint64_t nowMs) {
		if (!std::all_of(m_MigrationExpected.begin(), m_MigrationExpected.end(), [&](uint8_t peer) { return m_MigrationAnswers.contains(peer); })) {
			return;
		}
		m_MigrationBoundary = 0;
		m_MigrationDonor = 0;
		m_MigrationReady.clear();
		m_MigrationFrameQueue.clear();
		m_MigrationIncoming.clear();
		m_MigrationFirstNeeded = UINT64_MAX;
		m_MigrationResult = {};
		m_MigrationResult.generation = m_MigrationGeneration;
		m_MigrationResult.hostPeerId = m_MigrationSuccessor;
		for (const auto& [peer, answer]: m_MigrationAnswers) {
			m_MigrationResult.members.push_back(peer);
			m_MigrationBoundary = std::max(m_MigrationBoundary, answer.appliedFrame);
		}
		for (const auto& [peer, answer]: m_MigrationAnswers) {
			if (answer.completeFrom == UINT64_MAX)
				m_MigrationResult.resyncPeers.push_back(peer);
			else if (answer.appliedFrame == m_MigrationBoundary && (m_MigrationDonor == 0 || answer.completeFrom < m_MigrationAnswers.at(m_MigrationDonor).completeFrom)) {
				m_MigrationDonor = peer;
				m_MigrationWireRound = answer.frame;
			}
		}
		if (m_MigrationDonor == 0 || m_MigrationBoundary == UINT64_MAX || m_MigrationBoundary + 1 < m_Config.startFrame) {
			FailHostMigration("no recoverable committed boundary");
			return;
		}
		std::map<std::pair<uint64_t, uint64_t>, NetLockstepTiming> agreed;
		for (const auto& [peer, answer]: m_MigrationAnswers) for (const auto& decision: answer.futureDelays) {
			if (decision.applyFrame <= m_MigrationBoundary) continue;
			const auto identity = std::make_pair(decision.authorityGeneration, decision.revision);
			const auto [prior, inserted] = agreed.emplace(identity, decision);
			if (!inserted && prior->second != decision) { FailHostMigration("conflicting future delay identity"); return; }
		}
		m_MigrationFutureDelays.clear();
		std::map<std::pair<uint8_t, uint64_t>, uint16_t> boundaries;
		for (const auto& [identity, decision]: agreed) {
			const auto [at, inserted] = boundaries.emplace(std::make_pair(decision.peerId, decision.applyFrame), decision.delayFrames);
			if (!inserted && at->second != decision.delayFrames) { FailHostMigration("conflicting future input delay"); return; }
			m_MigrationFutureDelays.push_back(decision);
		}
		const uint64_t availableFrom = m_MigrationAnswers.at(m_MigrationDonor).completeFrom;
		for (const auto& [peer, answer]: m_MigrationAnswers) {
			if (answer.completeFrom == UINT64_MAX || (answer.appliedFrame < m_MigrationBoundary && answer.appliedFrame + 1 < availableFrom)) {
				if (std::find(m_MigrationResult.resyncPeers.begin(), m_MigrationResult.resyncPeers.end(), peer) == m_MigrationResult.resyncPeers.end())
					m_MigrationResult.resyncPeers.push_back(peer);
				if (peer == m_Config.localPeerId)
					m_MigrationNeedsResync = true;
			} else
				m_MigrationFirstNeeded = std::min(m_MigrationFirstNeeded, answer.appliedFrame + 1);
		}
		m_MigrationResult.boundary = m_MigrationBoundary;
		if (m_MigrationNeedsResync)
			m_MigrationResult.snapshotProviderPeerId = m_MigrationDonor;
		m_MigrationResult.transports = m_MigrationPeers;
		std::erase_if(m_MigrationResult.transports, [&](const auto& peer) { return !m_MigrationAnswers.contains(peer.first); });
		m_MigrationPhase = NetHostMigrationPhase::Recovering;
		m_MigrationSinceMs = nowMs;
		auto plan = MigrationMessage(NetHostMigrationMessageType::Plan);
		plan.bytes = m_MigrationResult.resyncPeers;
		for (const auto& [peer, transport]: m_MigrationPeers) {
			(void)SendMigration(transport, plan);
		}
		if (m_MigrationFirstNeeded <= m_MigrationBoundary) {
			if (m_MigrationDonor == m_Config.localPeerId) {
				for (const auto& [peer, answer]: m_MigrationAnswers) {
					if (peer == m_Config.localPeerId || std::find(m_MigrationResult.resyncPeers.begin(), m_MigrationResult.resyncPeers.end(), peer) != m_MigrationResult.resyncPeers.end())
						continue;
					for (uint64_t frame = answer.appliedFrame + 1; frame <= m_MigrationBoundary; ++frame)
						SendMigrationFrame(m_MigrationPeers.at(peer), frame);
				}
			} else {
				auto request = MigrationMessage(NetHostMigrationMessageType::RequestInput);
				request.frame = m_MigrationFirstNeeded;
				(void)SendMigration(m_MigrationPeers.at(m_MigrationDonor), std::move(request));
			}
		}
	}

	void NetLockstepCoordinator::HandleMigrationEvent(const NetTransportEvent& event, uint64_t nowMs) {
		if (m_MigrationPhase == NetHostMigrationPhase::Complete &&
		    !(event.type == NetTransportEventType::PacketReceived && NetHostMigrationCodec::LooksLikePacket(event.bytes))) {
			HandleEvent(event, nowMs);
			return;
		}
		const bool hosting = m_MigrationSuccessor == m_Config.localPeerId;
		if (event.type == NetTransportEventType::PeerConnected) {
			if (hosting && m_MigrationAdmissionEvents.size() < 128)
				m_MigrationAdmissionEvents.push_back(event);
			if (!hosting) {
				m_MigrationHostTransport = event.peerId;
				(void)SendMigration(event.peerId, MigrationMessage(NetHostMigrationMessageType::Hello));
			} else if (m_MigrationPhase == NetHostMigrationPhase::Contacting) {
				(void)SendMigration(event.peerId, MigrationMessage(NetHostMigrationMessageType::RollCall));
			} else if (m_SessionEventSink)
				m_SessionEventSink(event);
			return;
		}
		if (event.type != NetTransportEventType::PacketReceived) {
			// A closed handover link stops answering for the candidate, so the election may leave it again.
			if (!hosting && event.peerId == m_MigrationHostTransport && (event.type == NetTransportEventType::PeerDisconnected || event.type == NetTransportEventType::ConnectionFailed))
				m_MigrationHostTransport = c_InvalidNetPeerId;
			if (m_MigrationPhase == NetHostMigrationPhase::Complete && m_SessionEventSink)
				m_SessionEventSink(event);
			return;
		}
		NetHostMigrationMessage message;
		if (NetLockstepCodec::LooksLikePacket(event.bytes) && event.bytes.size() <= NetLockstepCodec::c_MaxRecoveryInputBytes) {
			const bool survivor = hosting ? std::any_of(m_MigrationPeers.begin(), m_MigrationPeers.end(), [&](const auto& peer) { return peer.second == event.peerId; }) : event.peerId == m_MigrationHostTransport;
			if (survivor && m_MigrationEarlyInputs.size() < 128)
				m_MigrationEarlyInputs.push_back(event);
			return;
		}
		if (hosting && !NetHostMigrationCodec::LooksLikePacket(event.bytes)) {
			if (event.bytes.size() <= NetProtocol::c_MaxControlPayloadBytes && m_MigrationAdmissionEvents.size() < 128)
				m_MigrationAdmissionEvents.push_back(event);
			return;
		}
		if (m_MigrationPhase == NetHostMigrationPhase::Complete && hosting && NetHostMigrationCodec::Decode(event.bytes, m_Config.migrationKey, message) &&
		    message.sessionId == m_Config.sessionId && message.roundId == m_Config.matchConfig.roundId && message.configHash == m_RoundConfigHash &&
		    (message.type == NetHostMigrationMessageType::RollCall || message.type == NetHostMigrationMessageType::Hello || message.type == NetHostMigrationMessageType::Answer) && message.generation <= m_MigrationGeneration) {
			(void)SendMigration(event.peerId, MigrationMessage(NetHostMigrationMessageType::Rejoin));
			return;
		}
		if (event.lane != NetTransportLane::ControlReliable || !NetHostMigrationCodec::Decode(event.bytes, m_Config.migrationKey, message) || message.sessionId != m_Config.sessionId || message.roundId != m_Config.matchConfig.roundId ||
		    message.generation != m_MigrationGeneration || message.successorPeerId != m_MigrationSuccessor || message.configHash != m_RoundConfigHash ||
		    message.senderPeerId > m_Config.peerCount || message.senderPeerId == m_Config.localPeerId || message.senderPeerId == GetHostPeerId())
			return;
		if (!hosting && (message.senderPeerId != m_MigrationSuccessor || event.peerId != m_MigrationHostTransport))
			return;
		if (hosting && m_MigrationPeers.contains(message.senderPeerId) && m_MigrationPeers.at(message.senderPeerId) != event.peerId)
			return;
		switch (message.type) {
			case NetHostMigrationMessageType::Hello:
				if (hosting && m_MigrationPhase == NetHostMigrationPhase::Contacting) {
					m_MigrationPeers[message.senderPeerId] = event.peerId;
					(void)SendMigration(event.peerId, MigrationMessage(NetHostMigrationMessageType::RollCall));
				} else if (hosting) {
					m_MigrationPeers[message.senderPeerId] = event.peerId;
					auto plan = MigrationMessage(NetHostMigrationMessageType::Plan);
					plan.bytes = m_MigrationResult.resyncPeers;
					(void)SendMigration(event.peerId, std::move(plan));
				}
				break;
			case NetHostMigrationMessageType::RollCall:
				if (!hosting && m_MigrationPhase == NetHostMigrationPhase::Contacting) {
					m_MigrationAuthoritySeen = true;
					(void)SendMigration(event.peerId, MigrationMessage(NetHostMigrationMessageType::Answer));
				}
				break;
			case NetHostMigrationMessageType::Answer:
				if (hosting && m_MigrationExpected.contains(message.senderPeerId) && m_MigrationPhase == NetHostMigrationPhase::Contacting && nowMs >= m_MigrationSinceMs && nowMs - m_MigrationSinceMs <= MigrationStepBudgetMs() && message.appliedFrame != UINT64_MAX) {
					m_MigrationAnswers[message.senderPeerId] = message;
					m_MigrationPeers[message.senderPeerId] = event.peerId;
				} else if (hosting && m_MigrationAnswers.contains(message.senderPeerId) && !m_MigrationCommitQueued && message.appliedFrame > m_MigrationBoundary && message.appliedFrame != UINT64_MAX) {
					m_MigrationAnswers[message.senderPeerId] = message;
					PublishMigrationPlan(nowMs);
				}
				break;
			case NetHostMigrationMessageType::Plan: {
				const uint64_t applied = GetResumeFrame() > 0 ? GetResumeFrame() - 1 : 0;
				if (!hosting && IsMigrating() && m_MigrationPhase != NetHostMigrationPhase::ResyncAdmission && message.boundary != UINT64_MAX && message.boundary >= m_MigrationBoundary &&
				    std::find(message.members.begin(), message.members.end(), m_MigrationSuccessor) != message.members.end()) {
					m_MigrationAuthoritySeen = true;
					if (m_MigrationResult.generation != 0 && message.boundary == m_MigrationBoundary && message.members == m_MigrationResult.members && message.futureDelays == m_MigrationFutureDelays) {
						break;
					}
					m_MigrationFutureDelays = message.futureDelays;
					m_MigrationBoundary = message.boundary;
					m_MigrationSinceMs = nowMs;
					m_MigrationWireRound = message.frame;
					if (std::find(message.bytes.begin(), message.bytes.end(), m_Config.localPeerId) != message.bytes.end())
						m_MigrationNeedsResync = true;
					m_MigrationResult = {m_MigrationGeneration, message.boundary, m_MigrationSuccessor, message.members, {}, {{m_MigrationSuccessor, event.peerId}}};
					m_MigrationPhase = NetHostMigrationPhase::Recovering;
					if (std::find(message.members.begin(), message.members.end(), m_Config.localPeerId) == message.members.end()) {
						m_MigrationNeedsResync = true;
						m_MigrationPhase = NetHostMigrationPhase::WaitingForReady;
					} else if (applied > message.boundary) {
						(void)SendMigration(event.peerId, MigrationMessage(NetHostMigrationMessageType::Answer));
					}
				}
				break;
			}
			case NetHostMigrationMessageType::RequestInput:
				if (!hosting && IsMigrating() && message.boundary == m_MigrationBoundary && message.frame <= message.boundary && message.boundary - message.frame < NetHostMigrationCodec::c_HistoryFrames) {
					for (uint64_t frame = message.frame; frame <= message.boundary; ++frame)
						SendMigrationFrame(event.peerId, frame);
				}
				break;
			case NetHostMigrationMessageType::Input: {
				if (!IsMigrating() || message.boundary != m_MigrationBoundary || message.frame > m_MigrationBoundary || message.totalBytes == 0 ||
				    message.offset > message.totalBytes || message.bytes.size() > message.totalBytes - message.offset || (hosting && message.senderPeerId != m_MigrationDonor))
					break;
				if (m_MigrationHistory.contains(message.frame))
					break;
				if (message.frame + NetHostMigrationCodec::c_HistoryFrames <= m_MigrationBoundary || m_MigrationIncoming.size() >= NetHostMigrationCodec::c_HistoryFrames)
					break;
				auto& incoming = m_MigrationIncoming[message.frame];
				if (incoming.size() != message.offset)
					break;
				incoming.insert(incoming.end(), message.bytes.begin(), message.bytes.end());
				if (incoming.size() != message.totalBytes)
					break;
				NetLockstepReadyFrame ready;
				if (!DecodeMigrationFrame(incoming, message.frame, ready)) {
					m_MigrationNeedsResync = true;
					m_MigrationIncoming.erase(message.frame);
					break;
				}
				StoreMigrationFrame(message.frame, std::move(incoming));
				m_MigrationIncoming.erase(message.frame);
				if (hosting) {
					for (const auto& [peer, answer]: m_MigrationAnswers)
						if (peer != m_Config.localPeerId && peer != m_MigrationDonor && answer.appliedFrame < message.frame)
							SendMigrationFrame(m_MigrationPeers.at(peer), message.frame);
				}
				break;
			}
			case NetHostMigrationMessageType::Ready:
				if (hosting && IsMigrating() && m_MigrationAnswers.contains(message.senderPeerId) && message.boundary == m_MigrationBoundary) {
					if (message.appliedFrame > m_MigrationBoundary && message.appliedFrame != UINT64_MAX && !m_MigrationCommitQueued) {
						m_MigrationAnswers[message.senderPeerId] = message;
						PublishMigrationPlan(nowMs);
						break;
					}
					if ((message.completeFrom == UINT64_MAX || message.appliedFrame != m_MigrationBoundary) && std::find(m_MigrationResult.resyncPeers.begin(), m_MigrationResult.resyncPeers.end(), message.senderPeerId) == m_MigrationResult.resyncPeers.end())
						m_MigrationResult.resyncPeers.push_back(message.senderPeerId);
					for (const auto& decision: message.futureDelays) if (decision.applyFrame > m_MigrationBoundary &&
					    std::find(m_MigrationFutureDelays.begin(), m_MigrationFutureDelays.end(), decision) == m_MigrationFutureDelays.end()) {
						m_MigrationAnswers[message.senderPeerId] = message; PublishMigrationPlan(nowMs); return;
					}
					m_MigrationReady.insert(message.senderPeerId);
				}
				break;
			case NetHostMigrationMessageType::Commit:
				if (!hosting && IsMigrating() && message.boundary == m_MigrationBoundary && std::is_sorted(message.members.begin(), message.members.end()) &&
				    std::includes(m_MigrationResult.members.begin(), m_MigrationResult.members.end(), message.members.begin(), message.members.end()) &&
				    std::find(message.members.begin(), message.members.end(), m_Config.localPeerId) != message.members.end() && GetResumeFrame() == m_MigrationBoundary + 1) {
					if (message.futureDelays != m_MigrationFutureDelays) { FailHostMigration("future delays changed after the recovery plan"); break; }
					m_MigrationResult.members = message.members;
					if (!message.bytes.empty()) {
						if (message.bytes.size() != 1 || std::find(message.members.begin(), message.members.end(), message.bytes.front()) == message.members.end() || message.bytes.front() == m_MigrationSuccessor)
							break;
						m_MigrationResult.snapshotProviderPeerId = message.bytes.front();
					}
					CompleteHostMigration(nowMs);
				}
				break;
			case NetHostMigrationMessageType::Rejoin:
				if (!hosting && IsMigrating()) {
					m_MigrationAuthoritySeen = true;
					m_MigrationNeedsResync = true;
					m_MigrationBoundary = message.boundary;
					m_MigrationWireRound = message.frame;
					m_RoundId = message.frame;
					m_Config.authorityPeerId = m_MigrationSuccessor;
					m_Config.migrationGeneration = m_MigrationGeneration;
					m_Config.relayToOtherPeers = false;
					m_RelayHost = false;
					m_RemoteTransports = {{m_MigrationSuccessor, event.peerId}};
					m_Config.remoteTransportPeerIds = m_RemoteTransports;
					m_Transport = m_MigrationTransport.get();
					m_MigrationResult = {m_MigrationGeneration, message.boundary, m_MigrationSuccessor, message.members, {m_Config.localPeerId}, {{m_MigrationSuccessor, event.peerId}}};
					ApplyMigrationMembership(nowMs);
					m_MigrationPhase = NetHostMigrationPhase::ResyncAdmission;
					m_MigrationNotice = true;
				}
				break;
			case NetHostMigrationMessageType::Abort:
				if (!hosting)
					FailHostMigration("successor found no recoverable boundary");
				break;
		}
	}

	void NetLockstepCoordinator::TickHostMigration(uint64_t nowMs) {
		if (m_MigrationPhase == NetHostMigrationPhase::ResyncAdmission)
			return;
		if (m_MigrationSuccessor == 0 && !ContactMigrationSuccessor(nowMs))
			return;
		for (const auto& event: m_MigrationTransport->PollEvents())
			HandleMigrationEvent(event, nowMs);
		if (m_MigrationPhase == NetHostMigrationPhase::ResyncAdmission)
			return;
		for (auto& [peer, pending]: m_MigrationOutbox) {
			while (!pending.empty() && m_MigrationTransport->Send(peer, NetTransportLane::ControlReliable, pending.front()))
				pending.pop_front();
		}
		for (size_t sent = 0; sent < 8 && !m_MigrationFrameQueue.empty(); ++sent) {
			auto& [peer, frame, offset] = m_MigrationFrameQueue.front();
			const auto input = m_MigrationHistory.find(frame);
			if (input == m_MigrationHistory.end()) {
				auto missing = MigrationMessage(NetHostMigrationMessageType::Ready);
				missing.appliedFrame = UINT64_MAX;
				(void)SendMigration(peer, std::move(missing));
				m_MigrationFrameQueue.pop_front();
				continue;
			}
			auto chunk = MigrationMessage(NetHostMigrationMessageType::Input);
			chunk.frame = frame;
			chunk.totalBytes = static_cast<uint32_t>(input->second.size());
			chunk.offset = static_cast<uint32_t>(offset);
			const size_t count = std::min(NetHostMigrationCodec::c_ChunkBytes, input->second.size() - offset);
			chunk.bytes.assign(input->second.begin() + offset, input->second.begin() + offset + count);
			if (m_MigrationOutbox[peer].size() >= 8 || !SendMigration(peer, std::move(chunk)))
				break;
			offset += count;
			if (offset == input->second.size())
				m_MigrationFrameQueue.pop_front();
		}
		if (!IsMigrating())
			return;
		const bool hosting = m_MigrationSuccessor == m_Config.localPeerId;
		const uint64_t budget = MigrationStepBudgetMs();
		const bool expired = nowMs >= m_MigrationSinceMs && nowMs - m_MigrationSinceMs >= budget;
		if (m_MigrationPhase == NetHostMigrationPhase::Contacting) {
			if (!hosting && m_MigrationAuthoritySeen && nowMs >= m_MigrationStartedMs && nowMs - m_MigrationStartedMs >= 3 * budget) {
				FailHostMigration("the successor did not publish a handover plan");
				return;
			}
			if (hosting) {
				// The closed roster excludes every unanswered seat at the published resume frame.
				if (expired) {
					std::erase_if(m_MigrationExpected, [&](uint8_t peer) { return !m_MigrationAnswers.contains(peer); });
				}
				if (m_MigrationAnswers.size() == m_MigrationExpected.size()) {
					PublishMigrationPlan(nowMs);
				}
			} else if (expired && !m_MigrationAuthoritySeen && !HoldsLiveMigrationCandidate(nowMs, budget)) {
				const auto endpoint = std::find_if(m_Config.matchConfig.migrationPeers.begin(), m_Config.matchConfig.migrationPeers.end(), [&](const auto& peer) { return peer.peerId == m_MigrationSuccessor; });
				if (endpoint == m_Config.matchConfig.migrationPeers.end() || m_MigrationNextAddress >= endpoint->listenAddrs.size()) {
					++m_MigrationCandidateIndex;
				}
				m_MigrationTransport.reset();
				(void)ContactMigrationSuccessor(nowMs);
			} else if (!hosting && nowMs >= m_MigrationLastSendMs + 250 && m_MigrationHostTransport != c_InvalidNetPeerId) {
				(void)SendMigration(m_MigrationHostTransport, MigrationMessage(NetHostMigrationMessageType::Hello));
				m_MigrationLastSendMs = nowMs;
			} else if (!hosting && m_MigrationHostTransport == c_InvalidNetPeerId && nowMs >= m_MigrationLastSendMs + 250) {
				const uint64_t started = m_MigrationSinceMs;
				(void)ContactMigrationSuccessor(nowMs);
				m_MigrationSinceMs = started;
			}
			return;
		}
		if (GetResumeFrame() <= m_MigrationBoundary && !m_MigrationNeedsResync && m_ReadyFrames.empty()) {
			const auto found = m_MigrationHistory.find(GetResumeFrame());
			if (found != m_MigrationHistory.end()) {
				NetLockstepReadyFrame ready;
				if (!DecodeMigrationFrame(found->second, GetResumeFrame(), ready))
					m_MigrationNeedsResync = true;
				else {
					m_Stats.nextFrame = ready.frame + 1;
					m_ReadyFrames.push_back(std::move(ready));
				}
			}
		}
		if (expired && GetResumeFrame() <= m_MigrationBoundary)
			m_MigrationNeedsResync = true;
		if (GetResumeFrame() == m_MigrationBoundary + 1 || m_MigrationNeedsResync) {
			if (hosting) {
				m_MigrationReady.insert(m_Config.localPeerId);
			} else if (m_MigrationPhase != NetHostMigrationPhase::WaitingForReady) {
				(void)SendMigration(m_MigrationHostTransport, MigrationMessage(NetHostMigrationMessageType::Ready));
				m_MigrationPhase = NetHostMigrationPhase::WaitingForReady;
			}
		}
		if (hosting && m_MigrationReady.size() == m_MigrationAnswers.size() && m_MigrationFrameQueue.empty()) {
			if (!m_MigrationCommitQueued) {
				for (uint8_t peer: m_MigrationResult.resyncPeers)
					if (peer != m_MigrationSuccessor)
						std::erase(m_MigrationResult.members, peer);
				auto commit = MigrationMessage(NetHostMigrationMessageType::Commit);
				if (m_MigrationResult.snapshotProviderPeerId != 0)
					commit.bytes = {m_MigrationResult.snapshotProviderPeerId};
				for (const auto& [peer, transport]: m_MigrationPeers) {
					const bool resync = std::find(m_MigrationResult.members.begin(), m_MigrationResult.members.end(), peer) == m_MigrationResult.members.end();
					if (!SendMigration(transport, resync ? MigrationMessage(NetHostMigrationMessageType::Rejoin) : commit)) {
						FailHostMigration("handover publication could not be queued");
						return;
					}
				}
				m_MigrationCommitQueued = true;
			}
			if (std::all_of(m_MigrationOutbox.begin(), m_MigrationOutbox.end(), [](const auto& peer) { return peer.second.empty(); }))
				CompleteHostMigration(nowMs);
		} else if (hosting && nowMs >= m_MigrationSinceMs && nowMs - m_MigrationSinceMs >= 2 * budget && m_MigrationReady.size() != m_MigrationAnswers.size()) {
			FailHostMigration("a surviving peer did not finish boundary recovery");
		}
		if (IsMigrating() && nowMs >= m_MigrationSinceMs && nowMs - m_MigrationSinceMs >= 3 * budget) {
			if (std::find(m_MigrationResult.members.begin(), m_MigrationResult.members.end(), m_Config.localPeerId) != m_MigrationResult.members.end())
				FailHostMigration("handover publication timed out");
			// An excluded peer is owed the commit's rejoin and nothing else answers for it.
			else if (m_MigrationPhase == NetHostMigrationPhase::WaitingForReady && !RestartHostMigrationAfterSuccessorLoss(nowMs))
				FailHostMigration("the successor was lost before the handover commit");
		}
	}

	void NetLockstepCoordinator::ApplyMigrationMembership(uint64_t nowMs) {
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if (std::find(m_MigrationResult.members.begin(), m_MigrationResult.members.end(), peer) == m_MigrationResult.members.end()) {
				ApplyPeerLeave(peer, m_MigrationBoundary + 1, "absent from host handover", nowMs, true, false, true);
			}
		}
	}

	void NetLockstepCoordinator::CompleteHostMigration(uint64_t nowMs) {
		auto pending = m_LocalInputHistory;
		for (const auto& outgoing: m_RecoveryOutgoing)
			if (outgoing.frame.senderPeerId == m_Config.localPeerId)
				pending[outgoing.frame.targetFrame] = outgoing.frame;
		auto carried = std::move(m_PendingObservations);
		auto carriedValues = std::move(m_PendingValueObservations);
		auto dropped = std::move(m_DroppedObservations);
		auto droppedValues = std::move(m_DroppedValueObservations);
		const auto commandAcks = m_AuthoritativeCommandAcks;
		const auto leaves = m_PeerLeaveFrames;
		const auto heldSeats = m_AiHeldSeats;
		const auto releasedSeats = m_ReleasedAiSeats;
		const auto holdTransactions = m_HoldTransactions;
		const auto reclaimTransactions = m_ReclaimTransactions;
		const auto heldResolutions = m_DroppedSeatResolutions;
		const auto droppedSeats = m_DroppedSeats;
		const auto droppedAt = m_DroppedAtMs;
		const auto completed = m_LastCompletedSimulationTick;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer)
			m_Config.peerInputDelayFrames[peer] = InputDelayAt(peer, m_MigrationBoundary);
		m_Config.inputDelayFrames = m_Config.peerInputDelayFrames.at(m_Config.localPeerId);
		m_OpeningMatchConfig = m_Config.matchConfig;
		m_RoundConfigHash = NetMatchConfigUtil::HashConfig(m_OpeningMatchConfig);
		m_Config.authorityPeerId = m_MigrationSuccessor;
		m_Config.migrationGeneration = m_MigrationGeneration;
		m_RoundId = m_MigrationWireRound;
		m_Config.activePeerIds = m_MigrationResult.members;
		for (uint8_t peer: m_MigrationResult.resyncPeers)
			if (peer != m_MigrationSuccessor)
				std::erase(m_Config.activePeerIds, peer);
		m_Config.startFrame = m_MigrationBoundary + 1;
		m_Config.resumeFromSnapshot = false;
		m_Config.remoteTransportPeerIds = m_MigrationResult.transports;
		for (uint8_t peer: m_MigrationResult.resyncPeers)
			m_Config.remoteTransportPeerIds.erase(peer);
		m_Config.relayToOtherPeers = m_Config.localPeerId == m_MigrationSuccessor;
		m_RemoteTransports = m_Config.remoteTransportPeerIds;
		m_RemotePeerIds.clear();
		for (uint8_t peer: m_Config.activePeerIds)
			if (peer != m_Config.localPeerId)
				m_RemotePeerIds.push_back(peer);
		m_RelayHost = m_Config.relayToOtherPeers;
		m_Transport = m_MigrationTransport.get();
		m_Config.initialDelayChanges.clear(); m_Config.initialPeerLeaves.clear(); m_Config.initialSeatHolds.clear(); m_Config.initialSeatReclaims.clear();
		ResetRoundState();
		for (const auto& decision: m_MigrationFutureDelays) m_DelayChanges[decision.peerId][decision.applyFrame] = decision.delayFrames;
		m_ReclaimTransactions = reclaimTransactions;
		m_LastCompletedSimulationTick = completed;
		m_LastDeliveredFrame = m_MigrationBoundary;
		for (const auto& [peer, frame]: heldSeats) {
			if (frame > m_MigrationBoundary || std::find(m_Config.activePeerIds.begin(), m_Config.activePeerIds.end(), peer) != m_Config.activePeerIds.end()) continue;
			m_AiHeldSeats[peer] = frame;
			if (releasedSeats.contains(peer)) m_ReleasedAiSeats.insert(peer);
			if (const auto transaction = holdTransactions.find(peer); transaction != holdTransactions.end()) m_HoldTransactions[peer] = transaction->second;
			if (const auto resolution = heldResolutions.find(peer); resolution != heldResolutions.end()) m_DroppedSeatResolutions[peer] = resolution->second;
			if (droppedSeats.contains(peer)) m_DroppedSeats.insert(peer);
			if (const auto dropped = droppedAt.find(peer); dropped != droppedAt.end()) m_DroppedAtMs[peer] = dropped->second;
		}
		m_PeerLeaveFrames = leaves;
		for (uint8_t peer: m_Config.activePeerIds)
			m_PeerLeaveFrames.erase(peer);
		m_AuthoritativeCommandAcks = commandAcks;
		ApplyMigrationMembership(nowMs);
		m_LocalFrames.clear();
		m_LocalCommands.clear();
		m_LocalObservations.clear();
		m_LocalValueObservations.clear();
		m_LocalInputHistory.clear();
		m_ObservationEncodeTables.Reset();
		m_ObservationDecodeTables.Reset();
		m_ObservationDecodeTables.roundId = m_RoundId;
		m_LastQueuedTargetFrame = UINT64_MAX;
		m_LastProducedFrame = UINT64_MAX;
		m_Stats.nextFrame = m_Config.startFrame;
		m_Stats.configuredStartFrame = m_Config.startFrame;
		m_Stats.effectiveStartFrame = m_Config.startFrame;
		m_PeerEffectiveStart.clear();
		for (uint8_t peer: m_Config.activePeerIds) {
			m_PeerEffectiveStart[peer] = m_Config.startFrame;
			m_PeerLastHeardMs[peer] = nowMs;
		}
		m_RemoteStartsReceived.insert(m_RemotePeerIds.begin(), m_RemotePeerIds.end());
		m_State = NetLockstepState::Running;
		m_ResyncPrimed = true;
		m_MigrationPhase = NetHostMigrationPhase::Complete;
		m_MigrationNotice = true;
		m_MigrationProbes.clear();
		m_MigrationLostSuccessors.clear();
		const auto earlyInputs = std::exchange(m_MigrationEarlyInputs, {});
		for (const auto& event: earlyInputs)
			HandleEvent(event, nowMs);
		m_WaitingFrame = m_Config.startFrame;
		m_WaitStartMs = nowMs;
		m_AuthorityLastHeardMs = nowMs;
		// The new host is in its start work until its first frame of the handed-over round, as a host is at a round's own start:
		// only its link's close or the round's timeout ends that wait. Its predecessor's talk gaps say nothing of it.
		if (m_MigrationSuccessor != m_Config.localPeerId) m_PeersPlayedThisRound.erase(m_MigrationSuccessor);
		m_AuthorityGaps.clear();
		m_AuthorityLongestGapMs = 0;
		if (NeedsMigrationSnapshot()) {
			std::string error;
			if (!QueueInputAtTarget(m_Config.startFrame, {}, {}, &error, {}))
				Fail(NetLockstepStopReason::InternalError, m_Config.startFrame, error);
			return;
		}
		uint64_t lastPending = m_Config.startFrame + m_Config.inputDelayFrames;
		if (!pending.empty())
			lastPending = std::max(lastPending, pending.rbegin()->first + 1);
		for (uint64_t frame = m_Config.startFrame; frame < lastPending; ++frame) {
			const auto previous = pending.find(frame);
			if (previous == pending.end() && frame >= m_Config.startFrame + m_Config.inputDelayFrames)
				continue;
			NetLockstepFrame input;
			if (previous != pending.end())
				input = previous->second;
			std::string error;
			if (!QueueInputAtTarget(frame, input.frames, input.commands, &error, input.observations, input.valueObservations)) {
				Fail(NetLockstepStopReason::InternalError, frame, "handover input priming failed: " + error);
				break;
			}
		}
		m_PendingObservations.insert(m_PendingObservations.end(), carried.begin(), carried.end());
		m_PendingValueObservations.insert(m_PendingValueObservations.end(), carriedValues.begin(), carriedValues.end());
		m_DroppedObservations.insert(m_DroppedObservations.end(), dropped.begin(), dropped.end());
		m_DroppedValueObservations.insert(m_DroppedValueObservations.end(), droppedValues.begin(), droppedValues.end());
		// A handover that found nobody but this peer is no election: the service refuses it, so nobody is hosting.
		if (std::any_of(m_MigrationResult.members.begin(), m_MigrationResult.members.end(), [&](uint8_t peer) { return peer != m_Config.localPeerId; }))
			std::cout << "[net-match] Host left - " << DescribePeer(m_MigrationSuccessor) << " is now hosting; boundary=" << m_MigrationBoundary << " round=" << m_RoundId << std::endl;
		else
			std::cout << "[net-match] Host left - no other survivor; boundary=" << m_MigrationBoundary << " round=" << m_RoundId << std::endl;
		// Our own end outlives the handover: the new host hears it as our leave, and this peer stays ended.
		if (const auto ownEnd = std::exchange(m_OwnEndDuringMigration, std::nullopt); ownEnd && IsRunning()) {
			if (ownEnd->reason == NetLockstepStopReason::Complete) Complete(ownEnd->message); else Leave(ownEnd->message);
		}
	}

	namespace {
		// One plane per process, like the one round it serves. The thread is the last member, so it is joined before the lock goes.
		struct PlaneState {
			std::recursive_mutex lock;
			std::atomic<NetLockstepCoordinator*> target{nullptr};
			std::atomic<int> windows{0};
			std::atomic<uint64_t> ticks{0};
			std::atomic<bool> checksArmed{false};
			std::atomic<uint64_t> checkTrips{0};
			std::mutex checkSitesLock;
			std::set<std::string> checkSites;
			std::once_flag started;
			std::jthread thread;
		};

		PlaneState& Plane() {
			static PlaneState state;
			return state;
		}

		void RunPlane(std::stop_token stop) {
			PlaneState& plane = Plane();
			while (!stop.stop_requested()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				if (plane.windows.load(std::memory_order_acquire) == 0) continue;
				std::unique_lock<std::recursive_mutex> lock(plane.lock, std::try_to_lock);
				NetLockstepCoordinator* target = plane.target.load(std::memory_order_acquire);
				if (!lock.owns_lock() || !target || plane.windows.load(std::memory_order_acquire) == 0) continue;
				++NetLockstepPlane::LockDepth();
				const uint64_t nowMs = NetLockstepNowMs();
				if (target->PlaneShouldTick(nowMs)) {
					target->PlaneTick(nowMs);
					plane.ticks.fetch_add(1, std::memory_order_relaxed);
				}
				--NetLockstepPlane::LockDepth();
			}
		}
	}

	std::recursive_mutex& NetLockstepPlane::Lock() { return Plane().lock; }

	void NetLockstepPlane::Target(NetLockstepCoordinator* coordinator) {
		PlaneState& plane = Plane();
		std::lock_guard<std::recursive_mutex> lock(plane.lock);
		plane.target.store(coordinator, std::memory_order_release);
		if (coordinator) std::call_once(plane.started, [&plane] { plane.thread = std::jthread(RunPlane); });
	}

	void NetLockstepPlane::Forget(const NetLockstepCoordinator* coordinator) {
		PlaneState& plane = Plane();
		std::lock_guard<std::recursive_mutex> lock(plane.lock);
		if (plane.target.load(std::memory_order_acquire) == coordinator) plane.target.store(nullptr, std::memory_order_release);
	}

	uint64_t NetLockstepPlane::Ticks() { return Plane().ticks.load(std::memory_order_relaxed); }

	void NetLockstepPlane::ArmChecks(bool armed) {
		Plane().checksArmed.store(armed, std::memory_order_release);
		if (armed) std::cout << "[net-plane] lock checks armed" << std::endl;
	}

	bool NetLockstepPlane::ChecksArmed() { return Plane().checksArmed.load(std::memory_order_acquire); }

	uint64_t NetLockstepPlane::CheckTrips() { return Plane().checkTrips.load(std::memory_order_acquire); }

	int& NetLockstepPlane::LockDepth() {
		thread_local int depth = 0;
		return depth;
	}

	void NetLockstepPlane::Check(const void* coordinator, const char* where) {
		PlaneState& plane = Plane();
		if (!plane.checksArmed.load(std::memory_order_relaxed) || plane.windows.load(std::memory_order_acquire) == 0 || LockDepth() > 0 ||
		    coordinator != plane.target.load(std::memory_order_acquire)) return;
		const uint64_t trips = plane.checkTrips.fetch_add(1, std::memory_order_acq_rel) + 1;
		std::lock_guard<std::mutex> sites(plane.checkSitesLock);
		if (plane.checkSites.insert(where).second) {
			std::ostringstream thread;
			thread << std::this_thread::get_id();
			std::cout << "[net-plane] unguarded coordinator access in " << where << " inside an open window: thread=" << thread.str() << " trips=" << trips << std::endl;
		}
	}

	NetLockstepPlane::Window::Window() { Plane().windows.fetch_add(1, std::memory_order_acq_rel); }
	NetLockstepPlane::Window::~Window() {
		// A tick in flight finishes before the simulation thread goes on to code that reads the coordinator unguarded.
		PlaneState& plane = Plane();
		std::lock_guard<std::recursive_mutex> lock(plane.lock);
		plane.windows.fetch_sub(1, std::memory_order_acq_rel);
		if (plane.checksArmed.load(std::memory_order_relaxed) && plane.checkTrips.load(std::memory_order_acquire) > 0) {
			std::lock_guard<std::mutex> sites(plane.checkSitesLock);
			std::cout << "[net-plane] ASSERT: " << plane.checkTrips.load() << " coordinator accesses without the plane's lock inside open windows at " << plane.checkSites.size() << " sites; stopping" << std::endl;
			std::fflush(stdout);
			std::abort();
		}
	}

	NetLockstepCoordinator::~NetLockstepCoordinator() { NetLockstepPlane::Forget(this); }

	bool NetLockstepCoordinator::PlaneShouldTick(uint64_t nowMs) const {
		NET_PLANE_CHECK();
		// The host is the round's hub: its relay and its commits are what every other peer waits on.
		if (m_State != NetLockstepState::Running || !m_Transport || m_Playback || IsMigrating() || m_Config.localPeerId != GetHostPeerId()) return false;
		const uint64_t simTickedMs = m_SimTickedMs.load(std::memory_order_acquire);
		return simTickedMs != 0 && nowMs >= simTickedMs && static_cast<double>(nowMs - simTickedMs) >= std::max(1.0, m_Config.simTickMs);
	}

	void NetLockstepCoordinator::PlaneTick(uint64_t nowMs) {
		NET_PLANE_CHECK();
		m_PlaneTicking = true;
		Tick(nowMs);
		m_PlaneTicking = false;
	}

	void NetLockstepCoordinator::HandleTransportEvents(uint64_t nowMs) {
		// What the plane left behind arrived first, so it is handled first.
		if (!m_PlaneTicking && !m_PlaneDeferredEvents.empty()) {
			m_PlaneHeldTransports.clear();
			std::vector<NetTransportEvent> deferred = std::exchange(m_PlaneDeferredEvents, {});
			for (size_t i = 0; i < deferred.size(); ++i) {
				HandleEvent(deferred[i], nowMs);
				if (IsMigrating()) {
					m_PlaneDeferredEvents.insert(m_PlaneDeferredEvents.begin(), std::make_move_iterator(deferred.begin() + i + 1), std::make_move_iterator(deferred.end()));
					return;
				}
			}
		}
		for (const NetTransportEvent& event : m_Transport->PollEvents()) {
			HandleEvent(event, nowMs);
			if (IsMigrating())
				break;
		}
	}

	bool NetLockstepCoordinator::Start(INetTransport& transport, const NetLockstepConfig& config, std::string* error) {
		NET_PLANE_CHECK();
		if ((config.adaptiveInputDelay || config.substituteSlowPeers) &&
		    (!std::isfinite(config.simTickMs) || config.simTickMs <= 0 || config.peerCount > NetMatchConfigUtil::c_MaxPeerCount || config.slowPlayerBoundTicks == 0 || config.slowPlayerBoundTicks > NetMatchConfigUtil::c_MaxSlowPlayerBoundTicks)) {
			if (error) *error = "invalid simulation tick or slow player bound";
			return false;
		}
		// One peer is a round whose only producer is here (an AI-only dedicated host); two or more
		// still need the remotes below.
		if (config.peerCount == 0 || config.peerCount > NetLockstepCodec::c_MaxPeerCount ||
		    config.localPeerId == 0 || config.localPeerId > config.peerCount) {
			if (error) *error = "lockstep peer identity is invalid";
			return false;
		}

		const uint8_t authority = config.authorityPeerId != 0 ? config.authorityPeerId : config.matchConfig.hostPeerId;
		if (!config.activePeerIds.empty()) {
			std::set<uint8_t> active;
			for (uint8_t peer: config.activePeerIds)
				if (peer == 0 || peer > config.peerCount || !active.insert(peer).second) {
					if (error)
						*error = "invalid active migration roster";
					return false;
				}
			if (!active.contains(config.localPeerId) || !active.contains(authority)) {
				if (error)
					*error = "active migration roster omits the local peer or host";
				return false;
			}
		}

		// The RECEIVE set is every peer except local — peerIds are 1..peerCount per the match config.
		// The SEND routing is separate (host-star): the host sends directly to every client, but a
		// client sends only to the host, which relays. So derive the receive set from peerCount, and
		// take the send targets from the explicit transport map (or the 2-peer single-remote fields).
		// A persistent world's capacity is configured; its MEMBERSHIP is not. The round waits only on
		// the members admission has already activated, which at boot is nobody, and grows one at a
		// time at an announced tick. peerCount stays the capacity so the config hash never moves.
		const bool worldMembership = config.matchConfig.persistentWorld;
		std::vector<uint8_t> remotePeerIds;
		for (uint8_t peerId = 1; peerId <= config.peerCount; ++peerId) {
			if (peerId == config.localPeerId) {
				continue;
			}
			if (!config.activePeerIds.empty() && std::find(config.activePeerIds.begin(), config.activePeerIds.end(), peerId) == config.activePeerIds.end()) {
				continue;
			}
			if (worldMembership && config.remoteTransportPeerIds.find(peerId) == config.remoteTransportPeerIds.end()) {
				continue;
			}
			remotePeerIds.push_back(peerId);
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
		} else if (!remotePeerIds.empty()) {
			if (config.remotePeerId == 0 || config.remotePeerId == config.localPeerId ||
			    config.remotePeerId > config.peerCount || config.remoteTransportPeerId == c_InvalidNetPeerId) {
				if (error) *error = "lockstep peer identity is invalid";
				return false;
			}
			remoteTransports[config.remotePeerId] = config.remoteTransportPeerId;
		}
		if (remoteTransports.empty() && !remotePeerIds.empty()) {
			if (error) *error = "lockstep has no remote transport targets";
			return false;
		}
		// A client's one link is its host's. A successor hosts the session from session seat 0, so a seat that joins it after a
		// migration is handed that link under the first seat's id; the round's authority is the peer it talks to.
		if (const uint8_t authority = config.authorityPeerId != 0 ? config.authorityPeerId : config.matchConfig.hostPeerId;
		    !config.relayToOtherPeers && authority != 0 && authority != config.localPeerId && authority <= config.peerCount &&
		    remoteTransports.size() == 1 && !remoteTransports.contains(authority)) {
			const NetPeerId link = remoteTransports.begin()->second;
			remoteTransports.clear();
			remoteTransports[authority] = link;
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
		m_MigrationFutureDelays.clear();
		m_Config = config;
		m_OpeningMatchConfig = config.matchConfig;
		m_RoundConfigHash = config.originalRoundConfigHash.value_or(NetMatchConfigUtil::HashConfig(config.matchConfig));
		m_MigrationPhase = NetHostMigrationPhase::None;
		m_MigrationHistory.clear();
		m_MigrationHistoryBytes = 0;
		m_MigrationTransport.reset();
		m_MigrationListener.reset();
		m_MigrationProbes.clear();
		m_MigrationGeneration = config.migrationGeneration;
		m_MigrationNotice = false;
		m_MigrationAuthoritySeen = false;
		m_MigrationExpected.clear();
		m_MigrationAnswers.clear();
		m_MigrationPeers.clear();
		m_MigrationReady.clear();
		m_MigrationIncoming.clear();
		m_MigrationOutbox.clear();
		m_MigrationFrameQueue.clear();
		m_MigrationResult = {};
		m_MigrationNeedsResync = false;
		m_RemotePeerIds = std::move(remotePeerIds);
		m_RemoteTransports = std::move(remoteTransports);
		m_RelayHost = config.relayToOtherPeers;
		m_ResyncPrimed = !config.resumeFromSnapshot;
		m_DeferStops = false;
		m_State = NetLockstepState::WaitingForStart;
		// The sim can measure its restart before the rematch worker has finished discovering its
		// transports. Preserve that publication across the round reset so the first start (and its
		// retransmit once remotes become known) carries the fact instead of silently losing it.
		const bool preStartPublished = m_LocalStartupPublished;
		const uint32_t preStartParkMs = m_LocalStartParkMs;
		ResetRoundState();
		if (preStartPublished) {
			m_LocalStartupPublished = true;
			m_LocalStartParkMs = preStartParkMs;
		}
		m_DelayEstimators = config.initialDelaySamples;
		for (auto& [peer, sample]: m_DelayEstimators) sample.Rebase(NetLockstepNowMs());
		// A round of our own produces its own input; a round we FOLLOW keeps what we already queued.
		if (!config.activePeerIds.empty()) {
			for (uint8_t peer = 1; peer <= config.peerCount; ++peer)
				if (std::find(config.activePeerIds.begin(), config.activePeerIds.end(), peer) == config.activePeerIds.end()) {
					m_PeerLeaveFrames.try_emplace(peer, config.startFrame);
					if (UsesBoundedWait()) {
						m_AiHeldSeats.try_emplace(peer, m_PeerLeaveFrames.at(peer));
						m_DroppedSeats.insert(peer);
						m_DroppedSeatResolutions[peer] = NetLockstepHoldResolution::Substituted;
					}
				}
		}
		m_LocalFrames.clear();
		m_LocalInputHistory.clear();
		m_LocalCommands.clear();
		m_LocalObservations.clear();
		m_LocalValueObservations.clear();
		m_LastQueuedTargetFrame = std::numeric_limits<uint64_t>::max();
		m_LastProducedFrame = UINT64_MAX;
		m_RoundId = config.roundId;
		m_ObservationDecodeTables.roundId = m_RoundId;
		m_Stats = {};
		m_Stats.sessionId = config.sessionId;
		m_Stats.configuredStartFrame = config.startFrame;
		// The commit stream starts at the EARLIEST sender's first delayed frame; later senders ramp in.
		m_PeerEffectiveStart.clear();
		uint64_t firstCommitFrame = config.startFrame + (config.resumeFromSnapshot ? 0 : config.inputDelayFrames);
		for (uint8_t peerId = 1; peerId <= config.peerCount; ++peerId) {
			const auto delayIt = config.peerInputDelayFrames.find(peerId);
			const uint16_t delay = delayIt != config.peerInputDelayFrames.end() ? delayIt->second : config.inputDelayFrames;
			// A round that joins one already running has no ramp-in: every remote has been producing for
			// a long time and owes this round's first frame, which is why the host replays what it sent
			// before the admission. Only the local peer, which starts here, ramps in behind its delay.
			const bool ramps = !config.joinsRunningRound || peerId == config.localPeerId;
			m_PeerEffectiveStart[peerId] = config.startFrame + (config.resumeFromSnapshot || !ramps ? 0 : delay);
			if (!ramps) {
				m_PeerEffectiveStart[peerId] = config.startFrame;
			}
			if (m_PeerEffectiveStart[peerId] < firstCommitFrame) {
				firstCommitFrame = m_PeerEffectiveStart[peerId];
			}
		}
		m_Stats.effectiveStartFrame = firstCommitFrame;
		m_Stats.inputDelayFrames = config.inputDelayFrames;
		m_Stats.localPeerId = config.localPeerId;
		m_Stats.remotePeerId = config.remotePeerId;
		m_Stats.nextFrame = m_Stats.effectiveStartFrame;

		// With no remote there is no start to hand out and none to wait for: the round runs at once.
		if (m_RemotePeerIds.empty()) {
			m_State = NetLockstepState::Running;
			return true;
		}
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
		start.resumeFromSnapshot = m_Config.resumeFromSnapshot;
		start.activityRestartMs = m_LocalStartParkMs;
		start.startupPublished = m_LocalStartupPublished;
		start.deviceClass = m_LocalDeviceClass;
		// A returning seat's reclaim delay is host-authored.  The sender must adopt its own
		// admission fact even when this initial start is broadcast to the host rather than sent
		// through the host's onlyPeerId filter.
		if (const auto admission = m_PeerAdmissions.find(m_Config.localPeerId); admission != m_PeerAdmissions.end()) {
			start.startFrame = admission->second.frame;
			start.inputDelayFrames = admission->second.delay;
			start.resumeFromSnapshot = false;
		}
		// A returning seat's start names the terms it goes out on, so a host that reads it as a straggler is explained.
		if (const auto own = m_ReclaimTransactions.find(m_Config.localPeerId); own != m_ReclaimTransactions.end() && m_Config.localPeerId != GetHostPeerId() && m_StartsSentNamed < 4) {
			++m_StartsSentNamed;
			std::cout << "[lockstep] returning seat sends its start: frame=" << start.startFrame << " delay=" << start.inputDelayFrames << " reclaim=" << own->second.activationFrame
			          << "/" << own->second.delayFrames << " admitted=" << m_PeerAdmissions.contains(m_Config.localPeerId) << " config_start=" << m_Config.startFrame
			          << " config_delay=" << m_Config.inputDelayFrames << " state=" << StateName(m_State) << " clock=" << NetLockstepSharedClockMs() << std::endl;
		}
		if (!SendPacket({start}, NetTransportLane::ControlReliable, error, nullptr, nullptr, onlyPeerId)) {
			return false;
		}
		++m_Stats.startPacketsSent;
		AdvertiseFrameWindow();
		return true;
	}

	// A peer that repeats its start is still waiting for one it missed, and ours may be it. Every
	// start a formed peer receives reads as a repeat though, and the answer is itself a start, so an
	// unconditional answer answers the answer: pace it by the ladder the repeats come from.
	std::string NetLockstepCoordinator::DescribeStartMismatch(const NetLockstepStart& start) const {
		const auto admission = m_PeerAdmissions.find(start.localPeerId);
		const bool admitted = admission != m_PeerAdmissions.end();
		std::string named;
		const auto note = [&named](const char* field, auto theirs, auto ours) {
			named += std::string(named.empty() ? "" : " ") + field + "=" + std::to_string(theirs) + "/" + std::to_string(ours);
		};
		if (start.sessionId != m_Config.sessionId) note("session", start.sessionId, m_Config.sessionId);
		if (start.startFrame != (admitted ? admission->second.frame : m_Config.startFrame)) note("start_frame", start.startFrame, admitted ? admission->second.frame : m_Config.startFrame);
		if (start.inputDelayFrames != (admitted ? admission->second.delay : PeerInputDelay(start.localPeerId))) note("delay", start.inputDelayFrames, admitted ? admission->second.delay : PeerInputDelay(start.localPeerId));
		if (start.controllerFrameVersion != ControllerFrame::c_Version) note("controller_version", start.controllerFrameVersion, ControllerFrame::c_Version);
		if (start.controllerFrameEncodedSize != ControllerFrame::c_EncodedSize) note("controller_size", start.controllerFrameEncodedSize, ControllerFrame::c_EncodedSize);
		if (!IsKnownRemotePeer(start.localPeerId)) note("unknown_peer", start.localPeerId, start.localPeerId);
		if (start.peerCount != m_Config.peerCount) note("peer_count", start.peerCount, m_Config.peerCount);
		if (start.scenario != m_Config.scenario) named += (named.empty() ? "" : " ") + std::string("scenario=") + start.scenario + "/" + m_Config.scenario;
		if (start.ownershipPolicy != m_Config.ownershipPolicy) named += (named.empty() ? "" : " ") + std::string("ownership=") + start.ownershipPolicy + "/" + m_Config.ownershipPolicy;
		if (start.resumeFromSnapshot != (admitted ? false : m_Config.resumeFromSnapshot)) note("resume", start.resumeFromSnapshot ? 1 : 0, (admitted ? false : m_Config.resumeFromSnapshot) ? 1 : 0);
		return named;
	}

	bool NetLockstepCoordinator::StartMatchesConfig(const NetLockstepStart& start) const {
		const auto admission = m_PeerAdmissions.find(start.localPeerId);
		const bool admitted = admission != m_PeerAdmissions.end();
		return start.sessionId == m_Config.sessionId &&
		       start.startFrame == (admitted ? admission->second.frame : m_Config.startFrame) &&
		       start.inputDelayFrames == (admitted ? admission->second.delay : PeerInputDelay(start.localPeerId)) &&
		       start.controllerFrameVersion == ControllerFrame::c_Version &&
		       start.controllerFrameEncodedSize == ControllerFrame::c_EncodedSize &&
		       IsKnownRemotePeer(start.localPeerId) &&
		       start.peerCount == m_Config.peerCount &&
		       start.scenario == m_Config.scenario &&
		       start.ownershipPolicy == m_Config.ownershipPolicy && start.resumeFromSnapshot == (admitted ? false : m_Config.resumeFromSnapshot);
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
		m_PeerAdmissions.clear();
		m_LeavesHeardAhead.clear();
		m_PeerDeviceClasses.fill(0);
		m_HostAcceptedLocalFrames.clear();
		m_ResumeAdmissionPending = m_Config.resumeFromSnapshot;
		m_SynchronizedCaptureStartFrame = UINT64_MAX;
		m_SynchronizedCaptureEndFrame = 0;
		m_SynchronizedCaptureBudgetMs = 250.0;
		m_CaptureParkRevision = 0;
		m_CaptureParkDeadlineMs = 0;
		m_CaptureParkPublishedEndFrame = UINT64_MAX;
		m_HighestParkEndFrame = 0;
		m_PendingCaptureReportMs = 0;
		m_PendingCaptureTick = UINT64_MAX;
		m_ReportedCaptureParkStart = UINT64_MAX;
		m_ReportedCaptureParkMs = 0;
		m_CaptureReportSentMs = 0;
		m_CaptureReportResent = false;
		m_CaptureParkReportsMs.clear();
		m_CommittedAtMs.clear();
		m_ParkFrameSimulated = UINT64_MAX;
		m_ParkFrameSimulatedMs = 0;
		m_GoodbyeDrain = false;
		m_FinalFrame = UINT64_MAX;
		m_DeferredParkTimings.clear();
		m_ParkCarriedCommands.clear();
		m_ApplyingDeferredParkTiming = false;
		m_CaptureParkAwaitingReports = false;
		m_CaptureParkFinalized = false;
		m_AiHeldSeats.clear();
		m_ReleasedAiSeats.clear();
		m_ReleaseWhenHeld.clear();
		m_EvictAfterReclaim.clear();
		m_HoldTransactions.clear();
		m_ReclaimTransactions.clear();
		m_RetiredReclaimGaps.clear();
		m_ConsumerWaitingFrame.reset();
		m_FirstMissingFrame.reset();
		m_LastDeliveredFrame.reset();
		m_ConsumerWaitStartMs = 0;
		m_ConsumerWaitCounted = false;
		m_LocalSeatHeld = false;
		m_LocalHoldFrame = 0;
		m_Playback = false;
		m_TimingNowMs = 0;
		m_ProductionBaseFrame.reset();
		m_ProductionBaseUs = m_ProductionWaitBaseUs = 0;
		m_TimingDecisions.clear();
		m_PreStartTiming.clear();
		m_DelayChanges = m_Config.initialDelayChanges;
		m_DelayEstimators.clear();
		m_ArrivalLeads.clear();
		m_ArrivalLateness.clear();
		m_TimingOutgoing.clear();
		m_DeferredControllerFrames.clear();
		m_NextTimingRevision = 1;
		m_LastTimingSampleMs = UINT64_MAX;
		m_LastTimingStatusMs = UINT64_MAX;
		m_AuthoritativeCommandAcks.clear();
		m_PendingRecoveryStop.reset();
		m_PendingCompleteStop.reset();
		m_LastCompletedSimulationTick.reset();
		m_RemoteStartsReceived.clear();
		m_PeersPlayedThisRound.clear();
		m_RemoteStarts.clear();
		m_LastStartAnswerMs.clear();
		m_ResendFrames.clear();
		m_RecoveryOutgoing.clear();
		m_RecoveryIncoming.clear();
		m_RecoveryBlockedSinceMs.clear();
		m_ResyncPrimeInputs.clear();
		m_InstalledResyncTargets.clear();
		m_PeerLeaveFrames.clear();
		m_PeerFrameWaivers.clear();
		m_SeatSnapshot.reset();
		m_SeatSnapshotUnread = false;
		m_PendingSeatSnapshotPeers.clear();
		m_LeftSeatsHeld.clear();
		m_DroppedSeats.clear();
		m_DroppedSeatResolutions.clear();
		m_DroppedAtMs.clear();
		m_LastHoldHeartbeatMs = 0;
		m_RequirePublishedStart = m_Config.requirePublishedStart;
		m_PeerStartupPublished.clear();
		m_StartupLinksLost.clear();
		m_LocalStartupPublished = false;
		m_AgreedStartApplied = false;
		m_AgreedStartRecord.reset();
		m_StartupHeldSeatStamps.clear();
		m_LateStartReclaims.clear();
		m_StartWaitAnnounced = false;
		m_StartWaitSinceMs = 0;
		m_LocalStartParkMs = 0;
		m_PeerLastHeardMs.clear();
		m_UnreachablePeers.clear();
		m_CongestedPeers.clear();
		m_HeldForCongestion.clear();
		m_RelayBacklog.clear();
		m_RelayBacklogSinceMs.clear();
		m_RelayedTicks.clear();
		m_RelayedTickFrames.clear();
		m_ReliableFramesThrough.clear();
		m_ResendRequests.clear();
		m_DecisionCommittedAtMs.clear();
		m_MissingSinceFrame = UINT64_MAX;
		m_MissingSinceMs = 0;
		m_LastLeaveMessage.clear();
		m_WaitingFrame = std::numeric_limits<uint64_t>::max();
		m_WaitStartMs = 0;
		m_AuthorityLastHeardMs = 0;
		m_AuthorityGaps.clear();
		m_AuthorityLongestGapMs = 0;
		m_LastStallFrame = UINT64_MAX;
		m_RemoteFrames.clear();
		m_RemoteCommands.clear();
		m_RemoteObservations.clear();
		m_RemoteChecksums.clear();
		m_LocalChecksums.clear();
		m_PendingObservations.clear();
		m_DroppedObservations.clear();
		m_PendingValueObservations.clear();
		m_DroppedValueObservations.clear();
		m_LocalValueObservations.clear();
		m_RemoteValueObservations.clear();
		m_ObservationDecodeTables.Reset();
		m_ObservationEncodeTables.Reset();
		m_ObservationBlocks.clear();
		m_ReadyFrames.clear();
		m_ReadyHistory.clear();
		m_PreStartFrames.clear();
		m_PreStartChecksums.clear();
		m_LastStartSentMs = UINT64_MAX;
		m_RemoteFrameWindow.clear();
		m_PeerLeaveFrames = m_Config.initialPeerLeaves;
		for (const auto& [peer, hold]: m_Config.initialSeatHolds) {
			// A held host is still the round's hub: its seat is under the AI, never gone.
			m_HoldTransactions[peer] = hold; m_AiHeldSeats[peer] = hold.cutoffFrame;
			if (peer != GetHostPeerId()) m_PeerLeaveFrames[peer] = hold.cutoffFrame;
			m_DroppedSeatResolutions[peer] = NetLockstepHoldResolution::Substituted;
		}
		m_ReclaimTransactions = m_Config.initialSeatReclaims;
		// A round joined from a replayed tail commits nothing until the seats that tail changed after its seat state was read are taken.
		m_AwaitingReplayedSeatState = m_Config.joinsRunningRound && m_Config.seatStateThroughFrame != 0;
		// A seat taken back before our first frame is a member from it: the start the host hands out for it names that frame.
		for (const auto& [peer, reclaim]: m_ReclaimTransactions)
			m_PeerAdmissions[peer] = reclaim.activationFrame < m_Config.startFrame ? PeerAdmission{m_Config.startFrame, PeerInputDelay(peer)} :
			    PeerAdmission{reclaim.activationFrame, reclaim.delayFrames};
		// Our own return produces on the delay the host admitted it on, whatever the config this round started from last heard.
		if (const auto own = m_ReclaimTransactions.find(m_Config.localPeerId); own != m_ReclaimTransactions.end() && own->second.activationFrame >= m_Config.startFrame &&
		    own->second.delayFrames != 0 && InputDelayAt(m_Config.localPeerId, own->second.activationFrame) != own->second.delayFrames) {
			m_DelayChanges[m_Config.localPeerId][own->second.activationFrame] = own->second.delayFrames;
			std::cout << "[net-lockstep] our return produces on its admitted delay " << own->second.delayFrames << " from frame " << own->second.activationFrame << std::endl;
		}
	}

	void NetLockstepCoordinator::ReadoptRound(uint64_t roundId, uint64_t nowMs) {
		auto installedTargets = std::move(m_InstalledResyncTargets);
		auto primeInputs = std::move(m_ResyncPrimeInputs);
		const bool hadResyncAdmission = m_ResyncPrimed;
		auto carried = std::move(m_PendingObservations);
		auto carriedValues = std::move(m_PendingValueObservations);
		std::map<uint64_t, NetLockstepFrame> ownInputs = m_LocalInputHistory;
		std::set<uint64_t> availableInputs;
		for (const auto& pending: m_RecoveryOutgoing) if (pending.frame.senderPeerId == m_Config.localPeerId) ownInputs[pending.frame.targetFrame] = pending.frame;
		for (const auto& [target, frames]: m_LocalFrames) {
			availableInputs.insert(target);
			auto& input = ownInputs[target];
			input.senderPeerId = m_Config.localPeerId; input.targetFrame = target; input.frames = frames;
			if (const auto commands = m_LocalCommands.find(target); commands != m_LocalCommands.end()) input.commands = commands->second;
			if (const auto observations = m_LocalObservations.find(target); observations != m_LocalObservations.end()) input.observations = observations->second;
			if (const auto values = m_LocalValueObservations.find(target); values != m_LocalValueObservations.end()) input.valueObservations = values->second;
		}
		for (const auto& ready: m_ReadyFrames) {
			if (!ready.hasLocalInput) continue;
			availableInputs.insert(ready.frame);
			auto& input = ownInputs[ready.frame];
			input.senderPeerId = m_Config.localPeerId; input.targetFrame = ready.frame;
			input.frames = ready.localFrames; input.commands = ready.localCommands; input.observations = ready.localObservations;
			input.valueObservations = ready.localValueObservations;
		}
		std::vector<NetLockstepFrame> installedRemotes;
		for (const auto& [target, peer]: installedTargets) {
			if (peer == m_Config.localPeerId) continue;
			const auto found = m_RemoteFrames.find(target);
			if (found == m_RemoteFrames.end() || !found->second.contains(peer)) continue;
			NetLockstepFrame input;
			input.senderPeerId = peer; input.targetFrame = target; input.roundId = roundId; input.frames = found->second.at(peer);
			if (const auto commands = m_RemoteCommands.find(target); commands != m_RemoteCommands.end() && commands->second.contains(peer)) input.commands = commands->second.at(peer);
			if (const auto observations = m_RemoteObservations.find(target); observations != m_RemoteObservations.end() && observations->second.contains(peer)) input.observations = observations->second.at(peer);
			if (const auto values = m_RemoteValueObservations.find(target); values != m_RemoteValueObservations.end() && values->second.contains(peer)) input.valueObservations = values->second.at(peer);
			installedRemotes.push_back(std::move(input));
		}
		m_RoundId = roundId;
		++m_Stats.roundReadoptions;
		ResetRoundState();
		m_LocalFrames.clear();
		m_LocalCommands.clear();
		m_LocalObservations.clear();
		m_LocalValueObservations.clear();
		m_PendingObservations = std::move(carried);
		m_PendingValueObservations = std::move(carriedValues);
		m_InstalledResyncTargets = std::move(installedTargets);
		for (auto& [target, input]: m_LocalInputHistory) input.roundId = roundId;
		for (auto& bytes: primeInputs) {
			NetLockstepFrame input;
			if (!NetLockstepCodec::DecodeRecoveryInput(bytes, input)) {
				Fail(NetLockstepStopReason::InternalError, m_Stats.nextFrame, "invalid retained resync priming input");
				return;
			}
			input.roundId = roundId;
			if (!NetLockstepCodec::EncodeRecoveryInput(input, bytes)) {
				Fail(NetLockstepStopReason::InternalError, m_Stats.nextFrame, "cannot rebind retained resync priming input");
				return;
			}
			m_ResyncPrimeInputs.push_back(std::move(bytes));
		}
		for (const auto& input: installedRemotes) {
			m_RemoteFrames[input.targetFrame][input.senderPeerId] = input.frames;
			if (!input.commands.empty()) m_RemoteCommands[input.targetFrame][input.senderPeerId] = input.commands;
			if (!input.observations.empty()) m_RemoteObservations[input.targetFrame][input.senderPeerId] = input.observations;
			if (!input.valueObservations.empty()) m_RemoteValueObservations[input.targetFrame][input.senderPeerId] = input.valueObservations;
		}
		m_ObservationDecodeTables.roundId = m_RoundId;
		m_State = NetLockstepState::WaitingForStart;
		std::string ignored;
		(void)SendStart(&ignored);
		++m_Stats.startRetransmits;
		m_LastStartSentMs = nowMs;
		for (auto& [target, input]: ownInputs) {
			input.roundId = roundId;
			if (availableInputs.contains(target) || m_InstalledResyncTargets.contains({target, m_Config.localPeerId})) {
				m_LocalFrames[target] = input.frames;
				if (!input.commands.empty()) m_LocalCommands[target] = input.commands;
				if (!input.observations.empty()) m_LocalObservations[target] = input.observations;
				if (!input.valueObservations.empty()) m_LocalValueObservations[target] = input.valueObservations;
			}
			if (!m_InstalledResyncTargets.contains({target, m_Config.localPeerId})) {
				m_ResendFrames.emplace(target, std::move(input));
			}
		}
		if (hadResyncAdmission || !m_ResyncPrimeInputs.empty()) m_ResumeAdmissionPending = false;
		FlushResendFrames();
	}

	// A refused send on a reliable lane is backpressure, and the round waits on exactly these frames, so
	// they are retried in order and nothing after a refused one goes out before it does.
	void NetLockstepCoordinator::FlushResendFrames() {
		// A round that has stopped or failed owes nobody anything.
		if (m_State != NetLockstepState::Running && m_State != NetLockstepState::WaitingForStart) {
			return;
		}
		if (m_ResendFrames.empty()) return;
		while (!m_ResendFrames.empty()) {
			std::vector<uint8_t> bytes;
			std::string error;
			const bool commitLocal = !m_LocalFrames.contains(m_ResendFrames.begin()->first) && m_ResendFrames.begin()->first >= m_Stats.nextFrame;
			if (!NetLockstepCodec::EncodeRecoveryInput(m_ResendFrames.begin()->second, bytes) ||
			    !RetainRecoveryInput(m_ResendFrames.begin()->second, std::move(bytes), commitLocal, &error)) {
				Fail(NetLockstepStopReason::InternalError, m_Stats.nextFrame, "cannot retain readopted input: " + error);
				return;
			}
			m_ResendFrames.erase(m_ResendFrames.begin());
		}
		FlushRecoveryInputs();
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
		if (m_AgreedStartRecord && m_Config.localPeerId == GetHostPeerId()) (void)SendAgreedStart(peerId);
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
			auto answer = otherStart;
			if (const auto admission = m_PeerAdmissions.find(peerId); admission != m_PeerAdmissions.end() && !IsPersistentWorldRound()) {
				answer.startFrame = admission->second.frame;
				answer.inputDelayFrames = InputDelayAt(otherPeerId, answer.startFrame);
				answer.roundId = m_RoundId;
				answer.resumeFromSnapshot = false;
			}
			(void)SendPacket({answer}, NetTransportLane::ControlReliable, &ignored, nullptr, nullptr, peerId);
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
		NET_PLANE_CHECK();
		// A one-peer recording is the AI-only round's; playback has no remotes either way.
		if (config.peerCount == 0 || config.peerCount > NetLockstepCodec::c_MaxPeerCount ||
		    config.localPeerId == 0 || config.localPeerId > config.peerCount) {
			if (error) *error = "replay peer identity is invalid";
			return false;
		}
		m_Transport = &transport;
		m_MigrationFutureDelays.clear();
		m_Config = config;
		m_OpeningMatchConfig = config.matchConfig;
		m_RoundConfigHash = config.originalRoundConfigHash.value_or(NetMatchConfigUtil::HashConfig(config.matchConfig));
		m_Config.inputDelayFrames = 0;
		m_Config.peerInputDelayFrames.clear();
		m_RemotePeerIds.clear();
		m_RemoteTransports.clear();
		m_RelayHost = false;
		m_DeferStops = false;
		m_State = NetLockstepState::Running;
		ResetRoundState();
		m_Playback = true;
		m_LocalFrames.clear();
		m_LocalInputHistory.clear();
		m_ResyncPrimed = true;
		m_LocalCommands.clear();
		m_LocalObservations.clear();
		m_LocalValueObservations.clear();
		m_LastQueuedTargetFrame = std::numeric_limits<uint64_t>::max();
		m_LastProducedFrame = UINT64_MAX;
		m_PeerEffectiveStart.clear();
		m_RoundId = config.roundId;
		m_Stats = {};
		m_Stats.sessionId = config.sessionId;
		m_Stats.configuredStartFrame = config.startFrame;
		m_Stats.effectiveStartFrame = config.startFrame;
		m_Stats.inputDelayFrames = 0;
		m_Stats.localPeerId = config.localPeerId;
		m_Stats.nextFrame = config.startFrame;
		return true;
	}

	bool NetLockstepCoordinator::ApplyReplayAgreedStart(const NetLockstepStart& start, std::string* error) {
		NET_PLANE_CHECK();
		if (!m_Playback || m_State != NetLockstepState::Running || m_Stats.framesAccepted != 0) {
			if (error) *error = "replay agreed start must be applied before playback commits";
			return false;
		}
		if (!start.agreedStartRecord) {
			if (error) *error = "replay record has no agreed-start boundary";
			return false;
		}
		ApplyAgreedStart(start, NetLockstepNowMs());
		if (m_State == NetLockstepState::Failed) {
			if (error) *error = m_Stats.timeoutReason;
			return false;
		}
		return true;
	}

	bool NetLockstepCoordinator::QueueReplayFrame(uint64_t frame, std::vector<ControllerFrame> frames, std::vector<NetGameCommand> commands, std::string* error, std::vector<NetSoundObservation> observations, std::vector<NetValueObservation> valueObservations) {
		NET_PLANE_CHECK();
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
		if (!valueObservations.empty()) {
			m_RemoteValueObservations[frame][bucketPeer] = std::move(valueObservations);
		}
		return true;
	}

	bool NetLockstepCoordinator::RewindReplay(uint64_t firstFrame, std::string* error) {
		NET_PLANE_CHECK();
		if (!m_RemotePeerIds.empty() || !m_RemoteTransports.empty()) {
			if (error) *error = "rewind is replay-only";
			return false;
		}
		if (m_State != NetLockstepState::Running) {
			if (error) *error = "rewind requires a running replay coordinator";
			return false;
		}
		for (auto seat = m_AiHeldSeats.begin(); seat != m_AiHeldSeats.end();) {
			if (seat->second >= firstFrame) {
				m_PeerLeaveFrames.erase(seat->first);
				m_HoldTransactions.erase(seat->first);
				m_DroppedSeatResolutions.erase(seat->first);
				seat = m_AiHeldSeats.erase(seat);
			} else ++seat;
		}
		m_LastDeliveredFrame = firstFrame > 0 ? std::optional<uint64_t>{firstFrame - 1} : std::nullopt;
		m_Config.matchConfig = m_OpeningMatchConfig;
		std::set<uint64_t> retainedChanges;
		for (auto& [peer, changes]: m_DelayChanges) {
			changes.erase(changes.lower_bound(firstFrame), changes.end());
			if (changes.empty()) continue;
			if (m_Config.matchConfig.peerInputDelayFrames.empty()) m_Config.matchConfig.peerInputDelayFrames.resize(m_Config.peerCount, m_OpeningMatchConfig.inputDelayFrames);
			m_Config.matchConfig.peerInputDelayFrames[peer - 1] = changes.rbegin()->second;
			for (const auto& [frame, delay]: changes) retainedChanges.insert(frame);
		}
		m_Config.matchConfig.configRevision += retainedChanges.size();
		m_LocalFrames.clear();
		m_RemoteFrames.clear();
		m_LocalCommands.clear();
		m_RemoteCommands.clear();
		m_LocalChecksums.clear();
		m_RemoteChecksums.clear();
		m_ReadyFrames.clear();
		m_ReadyHistory.clear();
		m_Stats.nextFrame = firstFrame;
		m_Stats.effectiveStartFrame = firstFrame;
		m_Stats.timeoutReason.clear();
		return true;
	}

	static uint64_t DelayedControllerEdges() {
		uint64_t mask = 0;
		for (ControlState state: {MOVE_FAST_TOGGLE, BODY_JUMPSTART, WEAPON_RELOAD, PIE_MENU_OPENED, WEAPON_CHANGE_NEXT, WEAPON_CHANGE_PREV, WEAPON_PICKUP, WEAPON_DROP,
		     WEAPON_PRIMARY_HOTKEYSTART, WEAPON_AUXILIARY_HOTKEYSTART, ACTOR_PRIMARY_HOTKEYSTART, ACTOR_AUXILIARY_HOTKEYSTART,
		     ACTOR_NEXT, ACTOR_PREV, ACTOR_BRAIN, PRESS_PRIMARY, PRESS_SECONDARY, PRESS_RIGHT, PRESS_LEFT, PRESS_UP, PRESS_DOWN,
		     RELEASE_PRIMARY, RELEASE_SECONDARY, PRESS_FACEBUTTON, RELEASE_FACEBUTTON, SCROLL_UP, SCROLL_DOWN}) mask |= uint64_t{1} << state;
		return mask;
	}

	bool NetLockstepCoordinator::QueueLocalInput(uint64_t producedFrame, const std::vector<ControllerFrame>& frames, const std::vector<NetGameCommand>& commands, std::string* error, const std::vector<NetSoundObservation>& observations, const std::vector<NetValueObservation>& valueObservations) {
		NET_PLANE_CHECK();
		if (IsMigrating()) {
			const uint16_t delay = InputDelayAt(m_Config.localPeerId, producedFrame);
			if (producedFrame > m_MigrationBoundary && producedFrame <= UINT64_MAX - delay) {
				NetLockstepFrame input{m_Config.localPeerId, producedFrame + delay, frames, commands, m_RoundId, observations, valueObservations};
				for (auto& command: input.commands)
					command.senderPeerId = m_Config.localPeerId;
				for (auto& observation: input.observations)
					observation.senderPeerId = m_Config.localPeerId;
				for (auto& observation: input.valueObservations)
					observation.senderPeerId = m_Config.localPeerId;
				m_LocalInputHistory.try_emplace(input.targetFrame, std::move(input));
			}
			return true;
		}
		if (m_Config.resumeFromSnapshot && !m_ResyncPrimed) { if (error) *error = "resync frames have not been primed"; return false; }
		const uint16_t delay = InputDelayAt(m_Config.localPeerId, producedFrame);
		if (producedFrame > UINT64_MAX - delay) { if (error) *error = "input target overflow"; return false; }
		const uint64_t target = producedFrame + delay;
		// Our return's first inputs past its gap, on the shared clock, so the host's reading of them can be timed.
		if (const auto own = m_ReclaimTransactions.find(m_Config.localPeerId); own != m_ReclaimTransactions.end() && m_Config.localPeerId != GetHostPeerId() &&
		    !IsSeatReclaimGap(m_Config.localPeerId, target)) {
			if (m_ReturnerInputsNamedFor != own->second.activationFrame) { m_ReturnerInputsNamedFor = own->second.activationFrame; m_ReturnerInputsNamed = 0; }
			if (m_ReturnerInputsNamed < 3) {
				++m_ReturnerInputsNamed;
				std::cout << "[lockstep] returning seat queues input produced=" << producedFrame << " target=" << target << " reclaim=" << own->second.activationFrame
				          << " next=" << m_Stats.nextFrame << " clock=" << NetLockstepSharedClockMs() << std::endl;
			}
		}
		if (IsSeatReclaimGap(m_Config.localPeerId, target)) { m_DeferredControllerFrames.clear(); return true; }
		// A host whose own seat the AI holds catches up in place: its input for the frames the AI played is discarded, as on every peer.
		if (m_Config.localPeerId == GetHostPeerId() && IsSeatUnderAI(m_Config.localPeerId, target)) { m_DeferredControllerFrames.clear(); return true; }
		// The agreed first frame can sit past our own start frame, so the ramp keeps producing targets the
		// round will never require. Only that shift is dropped instead of refused - every peer agrees the
		// same first frame - and a target below the round's own start is still an error.
		if (target >= m_Config.startFrame + delay && target < EffectiveStartOf(m_Config.localPeerId)) {
			m_DeferredControllerFrames.clear();
			return true;
		}
		if (TimingDecisionPendingAt(producedFrame)) { if (error) *error = "input is waiting for a timing decision"; return false; }
		if (!m_DelayChanges.empty() && m_LastQueuedTargetFrame != UINT64_MAX && target <= m_LastQueuedTargetFrame) {
			if (error) *error = "input sample must be deferred while the delay shrinks";
			return false;
		}
		if (producedFrame > 0 && delay > InputDelayAt(m_Config.localPeerId, producedFrame - 1) &&
		    m_LastQueuedTargetFrame != UINT64_MAX && target > m_LastQueuedTargetFrame + 1) {
			NetLockstepFrame previous;
			if (!FindLocalInput(m_LastQueuedTargetFrame, previous)) { if (error) *error = "delay padding has no preceding input"; return false; }
			for (auto& controller: previous.frames) {
				controller.stateMask &= ~DelayedControllerEdges();
				controller.mouseDeltaX = controller.mouseDeltaY = 0;
				controller.SetAimIntent(false);
				controller.SetFlipIntent(false);
				controller.hatchCommand = static_cast<uint8_t>(ControllerFrame::HatchCommand::None);
			}
			// The cursor is this loop's own: a frame the capture park already committed is accepted without
			// moving the queued watermark, and re-reading it there would pad the same frame for ever.
			for (uint64_t pad = m_LastQueuedTargetFrame + 1; pad < target; ++pad) {
				if (!QueueInputAtTarget(pad, previous.frames, {}, error, {})) return false;
				++m_Stats.delayPaddingFrames;
			}
		}
		std::vector<ControllerFrame> merged = frames;
		for (auto& frame: merged) {
			if (const auto pending = m_DeferredControllerFrames.find(frame.actorUniqueID); pending != m_DeferredControllerFrames.end()) {
				const uint64_t edges = pending->second.stateMask;
				frame.stateMask |= edges;
				frame.mouseDeltaX = static_cast<int16_t>(std::clamp<int>(frame.mouseDeltaX + pending->second.mouseDeltaX, INT16_MIN, INT16_MAX));
				frame.mouseDeltaY = static_cast<int16_t>(std::clamp<int>(frame.mouseDeltaY + pending->second.mouseDeltaY, INT16_MIN, INT16_MAX));
				if (!frame.HasAimIntent() && pending->second.HasAimIntent()) { frame.SetAimIntent(true); frame.aimAngle = pending->second.aimAngle; }
				if (!frame.HasFlipIntent() && pending->second.HasFlipIntent()) { frame.SetFlipIntent(true); frame.SetActorHFlipped(pending->second.IsActorHFlipped()); }
				if (!frame.HasHatchIntent()) frame.hatchCommand = pending->second.hatchCommand;
			}
		}
		if (!QueueInputAtTarget(target, merged, commands, error, observations, valueObservations)) {
			if (m_Config.localPeerId != GetHostPeerId() && m_Transport) {
				for (const auto& event: m_Transport->PollEvents()) HandleEvent(event, m_TimingNowMs);
				if (m_LocalSeatHeld) { m_DeferredControllerFrames.clear(); if (error) error->clear(); return true; }
			}
			return false;
		}
		m_DeferredControllerFrames.clear();
		m_LastProducedFrame = producedFrame;
		PadAheadOfDelayRise();
		return true;
	}

	void NetLockstepCoordinator::PadAheadOfDelayRise() {
		if (m_Playback || !IsRunning() || m_LastProducedFrame == UINT64_MAX || m_LastQueuedTargetFrame == UINT64_MAX) return;
		// The sim waits for a tick's committed frame before it runs that tick once its delay is above zero, so a delay
		// that rises from zero leaves the targets between the last one sent and the next production's owed now: the
		// same padding the next production would send, sent before the tick that would have to wait for it.
		const uint64_t next = m_LastProducedFrame + 1;
		const uint64_t nextTarget = next + InputDelayAt(m_Config.localPeerId, next);
		if (nextTarget <= m_LastQueuedTargetFrame + 1) return;
		NetLockstepFrame previous;
		if (!FindLocalInput(m_LastQueuedTargetFrame, previous)) return;
		for (auto& controller: previous.frames) {
			controller.stateMask &= ~DelayedControllerEdges();
			controller.mouseDeltaX = controller.mouseDeltaY = 0;
			controller.SetAimIntent(false);
			controller.SetFlipIntent(false);
			controller.hatchCommand = static_cast<uint8_t>(ControllerFrame::HatchCommand::None);
		}
		for (uint64_t pad = m_LastQueuedTargetFrame + 1; pad < nextTarget; ++pad) {
			std::string ignored;
			if (!QueueInputAtTarget(pad, previous.frames, {}, &ignored, {})) return;
			++m_Stats.delayPaddingFrames;
		}
	}

	void NetLockstepCoordinator::NoteReturnerCatchingUp(uint8_t peerId, uint64_t nowMs) {
		NET_PLANE_CHECK();
		if (!IsReturningSeatBeforeItsFirstInput(peerId)) return;
		auto& stats = m_Stats.peers[peerId];
		// A seat catching up in place started its round before its reclaim frame; one still replaying when its first input falls due
		// is late like any seat, and the survivors do not wait on it.
		if (stats.returnsInPlace) return;
		stats.reclaimAdmittedMs = std::max(stats.reclaimAdmittedMs, nowMs);
	}

	void NetLockstepCoordinator::NoteAuthorityHeard(uint64_t nowMs) {
		NET_PLANE_CHECK();
		// The gaps a live host leaves between its packets are its jitter as a talker: a loaded host that stalls for a frame now and then
		// is read against the longest of its recent ones, never as gone inside them. Its start work and announced captures are busy
		// spans the silence check excuses anyway, so they are not its jitter.
		if (m_AuthorityLastHeardMs != 0 && nowMs > m_AuthorityLastHeardMs && IsRunning() && m_PeersPlayedThisRound.contains(GetHostPeerId()) &&
		    !HostBusyWithAnnouncedCapture(m_Stats.nextFrame)) {
			const uint32_t gap = static_cast<uint32_t>(std::min<uint64_t>(nowMs - m_AuthorityLastHeardMs, UINT32_MAX));
			m_AuthorityGaps.push_back(gap);
			while (m_AuthorityGaps.size() > c_AuthorityGapSamples) m_AuthorityGaps.pop_front();
			m_AuthorityLongestGapMs = std::max(m_AuthorityLongestGapMs, gap);
		}
		m_AuthorityLastHeardMs = std::max(m_AuthorityLastHeardMs, nowMs);
	}

	void NetLockstepCoordinator::NoteAnnouncedCapture(uint64_t tick) {
		NET_PLANE_CHECK();
		m_AnnouncedCaptureTicks.insert(tick);
		while (m_AnnouncedCaptureTicks.size() > 64) m_AnnouncedCaptureTicks.erase(m_AnnouncedCaptureTicks.begin());
	}

	bool NetLockstepCoordinator::HostBusyWithAnnouncedCapture(uint64_t frame) const {
		NET_PLANE_CHECK();
		// The host's first input after a capture at the end of tick T is the one it produces simulating T + 1, which targets a delay later.
		const uint64_t delay = InputDelayAt(GetHostPeerId(), frame);
		const auto covers = [&](uint64_t tick) { return tick < frame && frame <= tick + delay + 2; };
		if (m_AnnouncedCaptureEvery > 0 && frame > 1 && covers((frame - 1) / m_AnnouncedCaptureEvery * m_AnnouncedCaptureEvery)) return true;
		const auto after = m_AnnouncedCaptureTicks.lower_bound(frame > delay + 2 ? frame - delay - 2 : 0);
		return after != m_AnnouncedCaptureTicks.end() && covers(*after);
	}

	void NetLockstepCoordinator::AdoptReplayedSeatTransitions(const NetLockstepCoordinator& replay, uint64_t throughFrame) {
		NET_PLANE_CHECK();
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if (peer == m_Config.localPeerId) continue;
			const auto back = replay.m_ReclaimTransactions.find(peer);
			const auto ours = m_ReclaimTransactions.find(peer);
			if (back != replay.m_ReclaimTransactions.end() && back->second.activationFrame > m_Config.seatStateThroughFrame && back->second.activationFrame <= throughFrame &&
			    (ours == m_ReclaimTransactions.end() || ours->second.eventSequence < back->second.eventSequence) &&
			    (m_AiHeldSeats.contains(peer) || m_PeerLeaveFrames.contains(peer))) {
				NetLockstepTiming reclaim;
				reclaim.peerId = peer; reclaim.authorityGeneration = back->second.authorityGeneration; reclaim.revision = back->second.eventSequence;
				reclaim.seatIncarnations[peer - 1] = back->second.seatIncarnation; reclaim.applyFrame = back->second.activationFrame;
				reclaim.delayFrames = back->second.delayFrames; reclaim.neutralThroughFrame = back->second.neutralThroughFrame; reclaim.worldTransition = back->second.worldTransition;
				TakeReturnBeforeFirstFrame(reclaim);
				std::cout << "[net-lockstep] took peer " << static_cast<int>(peer) << "'s return at " << back->second.activationFrame << " from the replayed tail" << std::endl;
				continue;
			}
			const auto held = replay.m_HoldTransactions.find(peer);
			if (held != replay.m_HoldTransactions.end() && replay.m_AiHeldSeats.contains(peer) && held->second.cutoffFrame > m_Config.seatStateThroughFrame &&
			    held->second.cutoffFrame <= throughFrame && !m_AiHeldSeats.contains(peer)) {
				// A return this round already knows of that lands after the hold ends the hold there; only an older one is gone.
				if (const auto later = m_ReclaimTransactions.find(peer); later != m_ReclaimTransactions.end() && later->second.activationFrame <= held->second.cutoffFrame)
					m_ReclaimTransactions.erase(later);
				m_HoldTransactions[peer] = held->second;
				m_AiHeldSeats[peer] = held->second.cutoffFrame;
				if (peer != GetHostPeerId()) m_PeerLeaveFrames[peer] = held->second.cutoffFrame;
				m_DroppedSeatResolutions[peer] = NetLockstepHoldResolution::Substituted;
				m_Config.peerIncarnations[peer] = std::max(m_Config.peerIncarnations[peer], held->second.seatIncarnation);
				std::cout << "[net-lockstep] took peer " << static_cast<int>(peer) << "'s hold at " << held->second.cutoffFrame << " from the replayed tail" << std::endl;
			}
		}
		m_AwaitingReplayedSeatState = false;
	}

	void NetLockstepCoordinator::NoteInPlaceReturn(uint8_t peerId) {
		NET_PLANE_CHECK();
		if (!m_AiHeldSeats.contains(peerId)) return;
		m_Stats.peers[peerId].startParkMs = 0;
		m_Stats.peers[peerId].returnsInPlace = true;
	}

	void NetLockstepCoordinator::NoteReturnerCaughtUp(uint8_t peerId, uint64_t nowMs) {
		NET_PLANE_CHECK();
		if (!IsReturningSeatBeforeItsFirstInput(peerId)) return;
		auto& stats = m_Stats.peers[peerId];
		if (stats.returnerCaughtUpMs == 0) stats.returnerCaughtUpMs = nowMs;
	}

	void NetLockstepCoordinator::NoteSeatReclaimed(uint8_t peerId) {
		NET_PLANE_CHECK();
		// The seat is back. What it waited while it was away is the round's record, not the reading the
		// surfaces owe the player from here on.
		auto& stats = m_Stats.peers[peerId];
		stats.waitsSinceReclaim = 0;
		stats.longestWaitMsSinceReclaim = 0;
		// A returning seat owes its first input from the frame it was admitted at, so its allowance is measured
		// from that moment; the horizon may have gone dry long before, while the seat was still away.
		stats.reclaimAdmittedMs = m_TimingNowMs;
		// Its restart cost belongs to the incarnation that is coming back, not to the one that left.
		stats.startParkMs = 0;
		if (peerId != m_Config.localPeerId) return;
		++m_LocalSeatReclaims;
		// The lateness that named this machine slow was measured before the seat came back, against a
		// production baseline that belongs to the round we left.
		m_Stats.consecutiveLateInputs = 0;
		m_Stats.localComputeDebtMs = 0;
		m_Stats.localProductionLateMs = 0;
		m_Stats.localMachineSlow = false;
		m_ProductionBaseFrame.reset();
	}

	uint32_t NetLockstepCoordinator::WaitsSinceReclaim(uint8_t peerId) const {
		NET_PLANE_CHECK();
		const auto peer = m_Stats.peers.find(peerId);
		return peer == m_Stats.peers.end() ? 0 : peer->second.waitsSinceReclaim;
	}

	uint64_t NetLockstepCoordinator::LongestWaitMsSinceReclaim(uint8_t peerId) const {
		NET_PLANE_CHECK();
		const auto peer = m_Stats.peers.find(peerId);
		return peer == m_Stats.peers.end() ? 0 : peer->second.longestWaitMsSinceReclaim;
	}

	uint16_t NetLockstepCoordinator::InputDelayAt(uint8_t peerId, uint64_t producedFrame) const {
		NET_PLANE_CHECK();
		const auto peer = m_DelayChanges.find(peerId);
		if (peer != m_DelayChanges.end()) {
			auto change = peer->second.upper_bound(producedFrame);
			if (change != peer->second.begin()) return std::prev(change)->second;
		}
		return PeerInputDelay(peerId);
	}

	bool NetLockstepCoordinator::DeferLocalInput(uint64_t producedFrame, const std::vector<ControllerFrame>& frames) {
		NET_PLANE_CHECK();
		if (!m_DelayChanges.contains(m_Config.localPeerId)) return false;
		const uint16_t delay = InputDelayAt(m_Config.localPeerId, producedFrame);
		if (m_LastQueuedTargetFrame == UINT64_MAX || producedFrame > UINT64_MAX - delay || producedFrame + delay > m_LastQueuedTargetFrame) return false;
		const uint64_t edgeMask = DelayedControllerEdges();
		for (const ControllerFrame& frame: frames) {
			auto& pending = m_DeferredControllerFrames[frame.actorUniqueID];
			pending.stateMask |= frame.stateMask & edgeMask;
			pending.mouseDeltaX = static_cast<int16_t>(std::clamp<int>(pending.mouseDeltaX + frame.mouseDeltaX, INT16_MIN, INT16_MAX));
			pending.mouseDeltaY = static_cast<int16_t>(std::clamp<int>(pending.mouseDeltaY + frame.mouseDeltaY, INT16_MIN, INT16_MAX));
			if (frame.HasAimIntent()) { pending.SetAimIntent(true); pending.aimAngle = frame.aimAngle; }
			if (frame.HasFlipIntent()) { pending.SetFlipIntent(true); pending.SetActorHFlipped(frame.IsActorHFlipped()); }
			if (frame.HasHatchIntent()) pending.hatchCommand = frame.hatchCommand;
		}
		++m_Stats.delayDeferredSamples;
		return true;
	}

	bool NetLockstepCoordinator::TimingDecisionPendingAt(uint64_t frame) const {
		NET_PLANE_CHECK();
		// A decision the capture park is holding cannot commit until the park closes, and the park only closes as
		// the round keeps producing: waiting on it here is a wait on this peer's own next frame.  The barrier
		// applies again to the fresh proposal the flush sends at the park's end.
		if (m_CaptureParkAwaitingReports) return false;
		for (const auto& [revision, decision]: m_TimingDecisions)
			if (!decision.committed && decision.proposal.applyFrame <= frame) return true;
		return false;
	}

	std::string NetLockstepCoordinator::DescribePendingTimingDecisions(uint64_t frame) const {
		NET_PLANE_CHECK();
		std::ostringstream out;
		out << "frame=" << frame << " next=" << m_Stats.nextFrame << " park_awaiting=" << m_CaptureParkAwaitingReports
		    << " park=" << (m_SynchronizedCaptureStartFrame == UINT64_MAX ? 0 : m_SynchronizedCaptureStartFrame)
		    << ".." << m_SynchronizedCaptureEndFrame << " final=" << m_CaptureParkFinalized << " deferred=" << m_DeferredParkTimings.size();
		for (const auto& [revision, decision]: m_TimingDecisions) {
			if (decision.committed || decision.proposal.applyFrame > frame) continue;
			out << " [rev=" << revision << " peer=" << static_cast<int>(decision.proposal.peerId)
			    << " action=" << static_cast<int>(decision.proposal.action) << " apply=" << decision.proposal.applyFrame
			    << " required=" << static_cast<int>(decision.proposal.requiredPeers) << " acked=" << static_cast<int>(decision.acknowledgedPeers)
			    << " proposed_ms=" << decision.proposedAtMs << "]";
		}
		return out.str();
	}

	uint64_t NetLockstepCoordinator::FutureTimingFrame() const {
		uint64_t horizon = std::max(m_Stats.nextFrame, m_LastQueuedTargetFrame == UINT64_MAX ? 0 : m_LastQueuedTargetFrame + 1);
		uint16_t delay = InputDelayAt(m_Config.localPeerId, m_Stats.nextFrame);
		for (uint8_t peer: m_RemotePeerIds) {
			if (IsPeerGoneAtFrame(peer, m_Stats.nextFrame)) continue;
			if (const auto stats = m_Stats.peers.find(peer); stats != m_Stats.peers.end()) horizon = std::max(horizon, stats->second.reportedNextFrame);
			delay = std::max(delay, InputDelayAt(peer, m_Stats.nextFrame));
			if (const auto estimate = m_DelayEstimators.find(peer); m_Config.simTickMs > 0 && estimate != m_DelayEstimators.end())
				delay = std::max(delay, static_cast<uint16_t>(std::min<uint32_t>(NetLockstepCodec::c_MaxInputDelayFrames, estimate->second.RequiredFrames(m_Config.simTickMs))));
		}
		return horizon + 2ULL * std::max<uint16_t>(delay, 1) + 2;
	}

	void NetLockstepCoordinator::QueueTiming(const NetLockstepTiming& timing, uint8_t onlyPeer) {
		// A park handshake holds only the traffic the park itself authored.  A decision another peer needs in
		// order to produce or consume a frame is never held behind it: the window the decision lands in is what
		// defers its application, and the re-stamp at the final end withdraws what it replaces.
		for (const auto& [peer, transport]: m_RemoteTransports) {
			if (onlyPeer != 0 && peer != onlyPeer) continue;
			auto& queue = m_TimingOutgoing[peer];
			if (timing.phase == NetTimingPhase::Status)
				std::erase_if(queue, [&](const auto& pending) { return pending.phase == NetTimingPhase::Status && pending.peerId == timing.peerId; });
			else std::erase_if(queue, [&](const auto& pending) { return pending.phase == timing.phase && pending.revision == timing.revision; });
			// The withdrawal travels ahead of anything still queued for the revision it replaces, so a peer sees
			// proposal then commit, or proposal then withdrawal, but never a commit after a withdrawal.
			if (timing.supersededRevision != 0)
				std::erase_if(queue, [&](const auto& pending) { return pending.revision == timing.supersededRevision; });
			if (std::find(queue.begin(), queue.end(), timing) == queue.end()) queue.push_back(timing);
		}
	}

	bool NetLockstepCoordinator::ProposeInputDelay(uint8_t peerId, uint16_t delayFrames, uint64_t applyFrame, std::string* error) {
		NET_PLANE_CHECK();
		if (std::any_of(m_MigrationFutureDelays.begin(), m_MigrationFutureDelays.end(), [&](const auto& decision) { return decision.peerId == peerId && decision.applyFrame >= GetResumeFrame(); })) {
			if (error) *error = "the peer still has a recovered future delay";
			return false;
		}
		if (!IsRunning() || m_Config.localPeerId != GetHostPeerId() || peerId == 0 || peerId > m_Config.peerCount ||
		    delayFrames > NetLockstepCodec::c_MaxInputDelayFrames || applyFrame < FutureTimingFrame() ||
		    applyFrame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew || m_NextTimingRevision == UINT64_MAX || m_Config.matchConfig.configRevision == UINT64_MAX) {
			if (error) *error = "invalid live delay boundary or authority";
			return false;
		}
		for (const auto& [revision, decision]: m_TimingDecisions) {
			if (decision.proposal.peerId == peerId && decision.proposal.applyFrame >= GetResumeFrame()) {
				if (error) *error = "the peer already has a pending timing change";
				return false;
			}
		}
		NetLockstepTiming timing;
		timing.senderPeerId = m_Config.localPeerId;
		timing.peerId = peerId;
		timing.sessionId = m_Config.sessionId;
		timing.roundId = m_RoundId;
		timing.revision = m_NextTimingRevision++;
		timing.authorityGeneration = m_Config.migrationGeneration;
		timing.applyFrame = applyFrame;
		timing.nextFrame = m_Stats.nextFrame;
		timing.delayFrames = delayFrames;
		timing.requiredPeers = static_cast<uint8_t>(1U << (m_Config.localPeerId - 1));
		for (uint8_t peer: m_RemotePeerIds)
			if (m_RemoteStartsReceived.contains(peer) && !IsPeerGoneAtFrame(peer, applyFrame)) timing.requiredPeers |= static_cast<uint8_t>(1U << (peer - 1));
		m_TimingDecisions[timing.revision] = {timing, static_cast<uint8_t>(1U << (m_Config.localPeerId - 1)), false, m_TimingNowMs};
		++m_Stats.delayChangesProposed;
		QueueTiming(timing);
		CommitTiming(timing.revision);
		return true;
	}

	void NetLockstepCoordinator::ApplyTiming(const NetLockstepTiming& timing) {
		// Only a decision that lands INSIDE the park window waits for the window's final end; one outside it is
		// applied at once, on every peer, exactly as it is sent.
		if (!m_ApplyingDeferredParkTiming && timing.action != NetTimingAction::CapturePark &&
		    m_SynchronizedCaptureStartFrame != UINT64_MAX && timing.applyFrame >= m_SynchronizedCaptureStartFrame &&
		    timing.applyFrame <= m_SynchronizedCaptureEndFrame) {
			if (std::none_of(m_DeferredParkTimings.begin(), m_DeferredParkTimings.end(), [&](const auto& pending) { return pending.revision == timing.revision; }))
				m_DeferredParkTimings.push_back(timing);
			return;
		}
		if (timing.action == NetTimingAction::CapturePark) {
			ApplyCapturePark(timing);
		} else if (timing.action == NetTimingAction::Delay) {
			m_DelayChanges[timing.peerId][timing.applyFrame] = timing.delayFrames;
			++m_Stats.delayChangesCommitted;
			if (timing.peerId == m_Config.localPeerId) PadAheadOfDelayRise();
			std::cout << "[net-match] delay change peer=" << static_cast<int>(timing.peerId) << " frame=" << timing.applyFrame
			          << " delay=" << timing.delayFrames << " revision=" << timing.revision << std::endl;
		} else if (timing.action == NetTimingAction::Reclaim || timing.action == NetTimingAction::WorldAdmission) {
			if (timing.peerId != m_Config.localPeerId && !IsKnownRemotePeer(timing.peerId)) { m_RemotePeerIds.push_back(timing.peerId); std::sort(m_RemotePeerIds.begin(), m_RemotePeerIds.end()); }
			m_ReclaimTransactions[timing.peerId] = {timing.peerId, timing.authorityGeneration, timing.revision,
			    timing.seatIncarnations[timing.peerId - 1], timing.applyFrame, timing.delayFrames, timing.neutralThroughFrame, timing.worldTransition};
			std::cout << "[net-lockstep] return of peer " << static_cast<int>(timing.peerId) << " at " << timing.applyFrame << " delay=" << timing.delayFrames
			          << " neutral_through=" << timing.neutralThroughFrame << " revision=" << timing.revision << " incarnation=" << timing.seatIncarnations[timing.peerId - 1]
			          << " next=" << m_Stats.nextFrame << " clock=" << NetLockstepSharedClockMs() << std::endl;
			m_ReturnerFirstFrameNamed.erase(timing.peerId);
			m_Config.peerIncarnations[timing.peerId] = timing.seatIncarnations[timing.peerId - 1];
			m_PeerEffectiveStart[timing.peerId] = timing.applyFrame + timing.delayFrames;
			m_PeerAdmissions[timing.peerId] = {timing.applyFrame, timing.delayFrames};
			// The host's own seat comes back without a restart: its start stands and every table that reads its stream, or that it
			// reads the others' with, is still the one in use.
			if (timing.peerId != GetHostPeerId()) {
				m_RemoteStartsReceived.erase(timing.peerId);
				m_RemoteStarts.erase(timing.peerId);
				SetObservationEpoch(timing.applyFrame);
			}
		} else if (timing.action == NetTimingAction::Hold) {
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if ((timing.heldPeers & (1U << (peer - 1))) != 0) {
				// A hold ends a return from its own frame on; the return's neutral gap still covers the frames before it that this peer
				// has yet to commit or to apply, exactly as it did on the host that applied them before it held the seat again.
				if (const auto back = m_ReclaimTransactions.find(peer); back != m_ReclaimTransactions.end()) {
					const uint64_t gapEnd = std::max(back->second.neutralThroughFrame, back->second.activationFrame + back->second.delayFrames);
					if (timing.applyFrame > back->second.activationFrame)
						m_RetiredReclaimGaps[peer] = {back->second.activationFrame, std::min(gapEnd, timing.applyFrame - 1)};
					m_ReclaimTransactions.erase(back);
				}
				m_HoldTransactions[peer] = {peer, timing.authorityGeneration, timing.revision, timing.seatIncarnations[peer - 1], timing.cutoffFrame};
				m_Config.peerIncarnations[peer] = timing.seatIncarnations[peer - 1];
			}
			if ((timing.heldPeers & (1U << (m_Config.localPeerId - 1))) != 0 && m_Config.localPeerId != GetHostPeerId()) {
				m_LocalSeatHeld = true;
				m_LocalHoldFrame = timing.applyFrame;
				m_PeerLeaveFrames[m_Config.localPeerId] = timing.applyFrame;
				m_Stats.timeoutReason = "PeerHeld:Your seat is held by the AI. Rejoin when your connection and machine can keep up.";
				m_State = NetLockstepState::Stopped;
				return;
			}
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
				if ((timing.heldPeers & (1U << (peer - 1))) == 0 || m_AiHeldSeats.contains(peer)) continue;
				m_AiHeldSeats[peer] = timing.applyFrame;
				// The host's seat is held for its simulation, never its membership: it stays the round's hub and authority, the AI of the
				// first playing peer of its succession drives what it drove, and its link stays up on every peer.
				if (peer == GetHostPeerId()) {
					m_ArrivalLeads.erase(peer);
					if (peer == m_Config.localPeerId) m_LocalHoldFrame = timing.applyFrame;
					++m_Stats.peers[peer].holds;
					std::cout << "[net-match] hold peer=" << static_cast<int>(peer) << " frame=" << timing.applyFrame << " AI in control (the host's own seat, AI of peer "
					          << static_cast<int>(AiAuthorityAt(timing.applyFrame)) << ")" << std::endl;
					continue;
				}
				// The seat's lead is measured afresh once it is back; what arrived before the hold says nothing of it.
				m_ArrivalLeads.erase(peer);
				m_Stats.peers[peer].returnerCaughtUpMs = 0;
				m_Stats.peers[peer].returnsInPlace = false;
				// The held seat keeps its connection: its player catches up in place on the committed tail and reclaims over it.
				// The hold goes to it here too, since a queue flushed after its link left the round would drop it.
				if (m_Config.localPeerId == GetHostPeerId() && m_Transport && !m_ReleaseWhenHeld.contains(peer))
					if (const auto link = m_RemoteTransports.find(peer); link != m_RemoteTransports.end()) {
						NetLockstepTiming hold = timing;
						hold.phase = NetTimingPhase::HoldAtFrame;
						std::vector<uint8_t> bytes;
						if (NetLockstepCodec::Encode({hold}, bytes)) (void)m_Transport->Send(link->second, NetTransportLane::ControlReliable, bytes);
					}
				ApplyPeerLeave(peer, timing.applyFrame, "slow player: AI takeover", m_TimingNowMs, false, false, true);
				m_DroppedSeatResolutions[peer] = NetLockstepHoldResolution::Substituted;
				++m_Stats.peers[peer].holds;
				std::cout << "[net-match] hold peer=" << static_cast<int>(peer) << " frame=" << timing.applyFrame << " AI in control" << std::endl;
				// A clean leaver's seat goes to the AI like any other and is released at once: a return is a new join.
				if (m_ReleaseWhenHeld.erase(peer) != 0) ReleaseHeldSeat(peer, m_TimingNowMs, true);
			}
			for (auto& [revision, pending]: m_TimingDecisions) pending.acknowledgedPeers |= timing.heldPeers;
		}
	}

	bool NetLockstepCoordinator::ProposePeerHold(uint8_t peerId, uint64_t nowMs, std::string* error) {
		NET_PLANE_CHECK();
		m_TimingNowMs = nowMs;
		// The host holds its own seat like any other: its session plane authors the hold while its simulation is away.
		const bool ownSeat = peerId == m_Config.localPeerId && peerId == GetHostPeerId();
		if (!IsRunning() || !UsesBoundedWait() || m_Config.localPeerId != GetHostPeerId() || (peerId == GetHostPeerId() && !ownSeat) ||
		    (!ownSeat && !IsKnownRemotePeer(peerId)) || m_PeerLeaveFrames.contains(peerId) || m_NextTimingRevision == UINT64_MAX) {
			if (error) *error = "invalid held peer or authority";
			return false;
		}
		// The round has run its last tick: nothing simulates past it, so no seat is judged.
		if (m_GoodbyeDrain) {
			if (error) *error = "the goodbye drain takes no hold";
			return false;
		}
		for (const auto& [revision, pending]: m_TimingDecisions)
			if (!pending.committed && pending.proposal.action == NetTimingAction::Hold && (pending.proposal.heldPeers & (1U << (peerId - 1))) != 0) return true;
		std::cout << "[net-lockstep] propose hold peer=" << static_cast<int>(peerId) << " next_frame=" << m_Stats.nextFrame
		          << " played=" << m_PeersPlayedThisRound.contains(peerId) << " first_missing_ms=" << m_FirstMissingMs
		          << " now=" << nowMs << " own_park=" << m_Stats.longestOwnParkMs
		          << " peer_park=" << m_Stats.peers[peerId].startParkMs << " ready=" << m_ReadyFrames.size() << std::endl;
		NetLockstepTiming timing;
		timing.senderPeerId = m_Config.localPeerId; timing.peerId = peerId;
		timing.action = NetTimingAction::Hold;
		timing.phase = NetTimingPhase::HoldAtFrame;
		timing.sessionId = m_Config.sessionId; timing.roundId = m_RoundId;
		timing.authorityGeneration = m_Config.migrationGeneration;
		timing.revision = m_NextTimingRevision++;
		// A hold is a host-authored boundary.  The host may have advanced less than a survivor
		// that already accepted the same input, so the first missing frame alone is not safe: every
		// peer must see the hold at or after its accepted horizon.
		// The host's own first frame without input is the one after the last it put on the wire.
		timing.applyFrame = std::max(ownSeat ? SentInputThrough() + 1 : FirstFrameWithout(peerId), m_Stats.nextFrame);
		for (uint8_t peer: m_RemotePeerIds) {
			if (peer == peerId || IsPeerGoneAtFrame(peer, timing.applyFrame)) continue;
			timing.applyFrame = std::max(timing.applyFrame, m_Stats.peers[peer].reportedNextFrame);
		}
		timing.heldPeers = static_cast<uint8_t>(1U << (peerId - 1));
		for (const auto& [revision, pending]: m_TimingDecisions) {
			if (pending.committed || pending.proposal.action != NetTimingAction::Hold) continue;
			timing.heldPeers |= pending.proposal.heldPeers;
			timing.applyFrame = std::min(timing.applyFrame, pending.proposal.applyFrame);
		}
		// Coalescing several holds must not move the combined boundary back behind a peer that
		// already accepted input while the first proposal was in flight.
		uint64_t acceptedHorizon = m_Stats.nextFrame;
		for (uint8_t peer: m_RemotePeerIds) {
			if (peer != peerId && !IsPeerGoneAtFrame(peer, timing.applyFrame))
				acceptedHorizon = std::max(acceptedHorizon, m_Stats.peers[peer].reportedNextFrame);
		}
		timing.applyFrame = std::max(timing.applyFrame, acceptedHorizon);
		timing.cutoffFrame = timing.applyFrame;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if ((timing.heldPeers & (1U << (peer - 1))) != 0) {
			const auto incarnation = m_Config.peerIncarnations.find(peer);
			timing.seatIncarnations[peer - 1] = incarnation == m_Config.peerIncarnations.end() ? 1 : incarnation->second;
		}
		timing.nextFrame = m_Stats.nextFrame;
		// A held seat is never also a required one: the host's own hold is acknowledged by the peers it keeps playing with.
		timing.requiredPeers = ownSeat ? 0 : static_cast<uint8_t>(1U << (m_Config.localPeerId - 1));
		for (uint8_t peer: m_RemotePeerIds)
			if ((timing.heldPeers & (1U << (peer - 1))) == 0 && !IsPeerGoneAtFrame(peer, timing.applyFrame)) timing.requiredPeers |= static_cast<uint8_t>(1U << (peer - 1));
		if (timing.applyFrame < m_Stats.nextFrame) {
			if (error) *error = "hold would contradict an accepted frame";
			return false;
		}
		m_TimingDecisions[timing.revision] = {timing, 0, true, nowMs};
		QueueTiming(timing);
		FlushTimingOutgoing();
		ApplyTiming(timing);
		// A hold the park deferred has not taken the seat, and the caller must not treat it as one: it would ask
		// again on the next frame and spin on a decision that cannot land until the park closes.
		if (std::any_of(m_DeferredParkTimings.begin(), m_DeferredParkTimings.end(),
		        [&](const auto& deferred) { return deferred.revision == timing.revision; })) {
			if (error) *error = "the capture park deferred this hold";
			return false;
		}
		std::vector<uint64_t> pending;
		for (const auto& [revision, decision]: m_TimingDecisions) if (!decision.committed) pending.push_back(revision);
		for (uint64_t revision: pending) CommitTiming(revision);
		return true;
	}

	bool NetLockstepCoordinator::ProposeWorldAdmission(NetPeerId transport, uint32_t incarnation, const NetGameWorldTransition& transition, std::string* error) {
		NET_PLANE_CHECK();
		const uint8_t peer = transition.peerId;
		if (!IsPersistentWorldRound() || !IsRunning() || m_RoundId == 0 || m_Config.localPeerId != GetHostPeerId() || peer == 0 || peer > m_Config.peerCount ||
		    peer == GetHostPeerId() || transport == c_InvalidNetPeerId || incarnation == 0 || transition.activationFrame < FutureTimingFrame()) {
			if (error) *error = "world admission is not ahead of the agreed input horizon";
			return false;
		}
		for (const auto& [revision, decision]: m_TimingDecisions)
			if (decision.proposal.action == NetTimingAction::WorldAdmission && decision.proposal.peerId == peer && decision.proposal.applyFrame == transition.activationFrame) return true;
		NetLockstepTiming timing;
		timing.action = NetTimingAction::WorldAdmission;
		timing.senderPeerId = GetHostPeerId(); timing.peerId = peer;
		timing.sessionId = m_Config.sessionId; timing.roundId = m_RoundId;
		timing.revision = m_NextTimingRevision++; timing.authorityGeneration = m_Config.migrationGeneration;
		timing.applyFrame = transition.activationFrame; timing.nextFrame = m_Stats.nextFrame;
		timing.delayFrames = InputDelayAt(peer, timing.applyFrame);
		timing.seatIncarnations[peer - 1] = incarnation;
		timing.worldTransition = transition;
		timing.neutralThroughFrame = timing.applyFrame + timing.delayFrames;
		timing.requiredPeers = static_cast<uint8_t>(1U << (GetHostPeerId() - 1));
		for (uint8_t survivor: m_RemotePeerIds) if (survivor != peer && m_RemoteStartsReceived.contains(survivor) && !IsPeerGoneAtFrame(survivor, timing.applyFrame))
			timing.requiredPeers |= static_cast<uint8_t>(1U << (survivor - 1));
		m_RemoteTransports[peer] = transport;
		m_TimingDecisions[timing.revision] = {timing, static_cast<uint8_t>(1U << (GetHostPeerId() - 1)), false, m_TimingNowMs};
		QueueTiming(timing);
		CommitTiming(timing.revision);
		return true;
	}

	bool NetLockstepCoordinator::SchedulePeerReclaim(uint8_t peerId, NetPeerId transport, uint32_t incarnation, uint64_t frame, std::string* error) {
		NET_PLANE_CHECK();
		if (!IsRunning() || !UsesBoundedWait() || m_Config.localPeerId != GetHostPeerId() || !HasHeldAISeat(peerId) ||
		    peerId == 0 || peerId > 4 || transport == c_InvalidNetPeerId || incarnation <= m_Config.peerIncarnations[peerId] ||
		    frame <= std::max(m_Stats.nextFrame, SentInputThrough()) || m_NextTimingRevision == UINT64_MAX) {
			if (error) *error = "private reclaim requires a held incarnation and a future activation";
			return false;
		}
		NetLockstepTiming timing;
		timing.senderPeerId = GetHostPeerId(); timing.peerId = peerId;
		timing.action = NetTimingAction::Reclaim; timing.phase = NetTimingPhase::ReclaimAtFrame;
		timing.sessionId = m_Config.sessionId; timing.roundId = m_RoundId; timing.authorityGeneration = m_Config.migrationGeneration;
		timing.revision = m_NextTimingRevision++; timing.applyFrame = timing.cutoffFrame = frame; timing.nextFrame = m_Stats.nextFrame;
		timing.neutralThroughFrame = frame + InputDelayAt(peerId, frame);
		for (uint64_t produced = frame > NetLockstepCodec::c_MaxInputDelayFrames ? frame - NetLockstepCodec::c_MaxInputDelayFrames : 0; produced <= frame; ++produced)
			timing.neutralThroughFrame = std::max(timing.neutralThroughFrame, produced + InputDelayAt(GetHostPeerId(), produced));
		timing.delayFrames = InputDelayAt(peerId, frame); timing.heldPeers = static_cast<uint8_t>(1U << (peerId - 1));
		timing.requiredPeers = static_cast<uint8_t>(1U << (GetHostPeerId() - 1)); timing.seatIncarnations[peerId - 1] = incarnation;
		m_RemoteTransports[peerId] = transport;
		m_TimingDecisions[timing.revision] = {timing, timing.requiredPeers, true, m_TimingNowMs};
		QueueTiming(timing); FlushTimingOutgoing(); ApplyTiming(timing);
		for (const auto& [revision, decision]: m_TimingDecisions) {
			if (decision.proposal.action != NetTimingAction::Delay || decision.proposal.applyFrame < frame) continue;
			QueueTiming(decision.proposal, peerId);
			if (decision.committed) { auto committed = decision.proposal; committed.phase = NetTimingPhase::Commit; QueueTiming(committed, peerId); }
		}
		FlushTimingOutgoing();
		return true;
	}

	bool NetLockstepCoordinator::IsSeatHoldGap(uint8_t peerId, uint64_t frame) const {
		NET_PLANE_CHECK();
		const auto held = m_AiHeldSeats.find(peerId);
		if (held == m_AiHeldSeats.end() || frame < held->second) return false;
		uint16_t delay = InputDelayAt(GetHostPeerId(), held->second);
		if (m_Playback) {
			delay = NetMatchConfigUtil::PeerInputDelay(m_OpeningMatchConfig, GetHostPeerId());
			if (const auto changes = m_DelayChanges.find(GetHostPeerId()); changes != m_DelayChanges.end()) {
				const auto at = changes->second.upper_bound(held->second);
				if (at != changes->second.begin()) delay = std::prev(at)->second;
			}
		}
		return frame - held->second <= delay;
	}

	void NetLockstepCoordinator::AdoptReturnsBefore(NetLockstepConfig& config, const std::vector<NetLockstepTiming>& reclaims, uint64_t firstFrame) {
		std::map<uint8_t, const NetLockstepTiming*> newest;
		for (const NetLockstepTiming& reclaim: reclaims) {
			if (reclaim.phase != NetTimingPhase::ReclaimAtFrame || reclaim.peerId == 0 || reclaim.peerId > config.peerCount || reclaim.peerId > 4 ||
			    reclaim.peerId == config.localPeerId || reclaim.applyFrame > firstFrame || reclaim.applyFrame <= config.seatStateThroughFrame) continue;
			const NetLockstepTiming*& latest = newest[reclaim.peerId];
			if (!latest || latest->revision < reclaim.revision) latest = &reclaim;
		}
		for (const auto& [peer, reclaim]: newest) {
			const uint32_t incarnation = reclaim->seatIncarnations[peer - 1];
			if (const auto known = config.peerIncarnations.find(peer); known != config.peerIncarnations.end() && known->second >= incarnation) continue;
			// The seat is back before we are, so the host hands us its start with the rest of the round.
			config.initialSeatHolds.erase(peer);
			config.initialPeerLeaves.erase(peer);
			config.peerIncarnations[peer] = incarnation;
			config.initialSeatReclaims[peer] = {peer, reclaim->authorityGeneration, reclaim->revision, incarnation, reclaim->applyFrame, reclaim->delayFrames,
			    reclaim->neutralThroughFrame, reclaim->worldTransition};
		}
	}

	bool NetLockstepCoordinator::IsSeatReclaimGap(uint8_t peerId, uint64_t frame) const {
		NET_PLANE_CHECK();
		if (const auto retired = m_RetiredReclaimGaps.find(peerId); retired != m_RetiredReclaimGaps.end() && frame >= retired->second.first && frame <= retired->second.second)
			return true;
		const auto found = m_ReclaimTransactions.find(peerId);
		if (found == m_ReclaimTransactions.end()) return false;
		const auto& reclaim = found->second;
		return frame >= reclaim.activationFrame && frame <= std::max(reclaim.neutralThroughFrame, reclaim.activationFrame + reclaim.delayFrames);
	}

	bool NetLockstepCoordinator::IsSeatUnderAI(uint8_t peerId, uint64_t frame) const {
		NET_PLANE_CHECK();
		const auto seat = m_AiHeldSeats.find(peerId);
		return seat != m_AiHeldSeats.end() && frame >= seat->second &&
		    (!m_ReclaimTransactions.contains(peerId) || frame < m_ReclaimTransactions.at(peerId).activationFrame);
	}

	bool NetLockstepCoordinator::IsReturningSeatBeforeItsFirstInput(uint8_t peerId) const {
		const auto reclaim = m_ReclaimTransactions.find(peerId);
		if (reclaim == m_ReclaimTransactions.end()) return false;
		const auto stats = m_Stats.peers.find(peerId);
		// Nothing the seat sent is consumable past the window it was admitted on. A frame that only
		// reached our decoder is not an answer: until its new start lands, its input sits buffered.
		return stats == m_Stats.peers.end() || stats->second.acceptedThroughFrame <=
		    std::max(reclaim->second.neutralThroughFrame, reclaim->second.activationFrame + reclaim->second.delayFrames);
	}

	uint32_t NetLockstepCoordinator::RejoinDelayFrames(uint8_t peerId, const NetInputDelayEstimator& estimate) const {
		NET_PLANE_CHECK();
		const uint32_t linkFrames = estimate.RequiredFrames(m_Config.simTickMs, m_Config.matchConfig.inputDelayFrames);
		const auto stats = m_Stats.peers.find(peerId);
		if (!std::isfinite(m_Config.simTickMs) || m_Config.simTickMs <= 0 || stats == m_Stats.peers.end()) return linkFrames;
		// A sender already in the round clears one trip: its clock and the round's advanced together. A
		// returning seat starts its clock a trip after the round passed its activation frame and pays its
		// own restart before its first tick, so its window has to clear the whole trip and that restart.
		// Only the restart the peer published is used: our own parks move while the match runs, and a
		// window that chased them would never settle.
		const uint32_t restartFrames = static_cast<uint32_t>(std::ceil(static_cast<double>(stats->second.startParkMs) / m_Config.simTickMs));
		return linkFrames > UINT32_MAX - restartFrames ? UINT32_MAX : linkFrames + restartFrames;
	}

	bool NetLockstepCoordinator::PreparePeerRejoin(uint8_t peerId, uint32_t rttMs, uint64_t nowMs, std::string* error) {
		NET_PLANE_CHECK();
		if (!UsesBoundedWait() || !m_AiHeldSeats.contains(peerId)) return true;
		if (m_ReleasedAiSeats.contains(peerId)) {
			if (error) *error = "Your seat was released; you can join the match again as a new player.";
			return false;
		}
		if (!m_LastDeliveredFrame || *m_LastDeliveredFrame < m_AiHeldSeats.at(peerId)) {
			if (error) *error = "Rejoining: waiting for the agreed AI handoff frame";
			return false;
		}
		auto& estimate = m_DelayEstimators[peerId];
		estimate.Observe(nowMs, rttMs);
		const uint32_t needed = RejoinDelayFrames(peerId, estimate);
		const uint16_t current = InputDelayAt(peerId, m_LastDeliveredFrame.value_or(m_Config.startFrame));
		if (needed <= current) return true;
		if (needed > NetLockstepCodec::c_MaxInputDelayFrames || m_Config.matchConfig.delayPolicy == NetMatchDelayPolicy::Fixed) {
			if (error) *error = "Your connection needs " + std::to_string(needed) + " delay frames; the match currently allows " + std::to_string(current) + ". Your seat remains under AI control.";
			return false;
		}
		ProposeInputDelay(peerId, static_cast<uint16_t>(needed), FutureTimingFrame());
		if (error) *error = "Rejoining: waiting for the agreed input delay to take effect";
		return false;
	}

	std::vector<uint8_t> NetLockstepCoordinator::ResumePeerIds() const {
		NET_PLANE_CHECK();
		std::vector<uint8_t> peers;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer)
			if (peer == m_Config.localPeerId || !IsPeerGoneAtFrame(peer, GetResumeFrame()) || HeldSeatResolution(peer) == NetLockstepHoldResolution::Reclaimed)
				peers.push_back(peer);
		return peers;
	}

	bool NetLockstepCoordinator::DeclareOverdueInputs(uint64_t frame, uint64_t nowMs, uint64_t firstMissingMs, const std::vector<uint8_t>& missing) {
		if (!UsesBoundedWait() || m_Playback || m_Config.localPeerId != GetHostPeerId() || missing.empty()) return false;
		// Past this host's last tick nothing simulates the frame, so no survivor waits on the seat's input.
		if (m_GoodbyeDrain || frame > m_FinalFrame) return false;
		// Autosave is an agreed event: all peers are in the same capture park, so its silence is
		// not evidence that one seat stopped producing input.
		if (IsSynchronizedCapturePark(frame)) return false;
		const uint64_t boundMs = static_cast<uint64_t>(std::max(1.0, std::floor(m_Config.slowPlayerBoundTicks * m_Config.simTickMs)));
		uint64_t noticeMs = 2;
		for (uint8_t survivor: m_RemotePeerIds) {
			if (std::find(missing.begin(), missing.end(), survivor) != missing.end() || IsPeerGoneAtFrame(survivor, frame)) continue;
			const auto estimate = m_DelayEstimators.find(survivor);
			const auto& stats = m_Stats.peers[survivor];
			noticeMs = std::max(noticeMs, uint64_t(2) + std::max(stats.pingMs, estimate == m_DelayEstimators.end() ? 0U : estimate->second.P95Ms()) + stats.jitterMs);
		}
		m_Stats.holdNoticeBudgetMs = noticeMs;
		m_Stats.holdDeadlineFeasible = m_Stats.holdDeadlineFeasible && noticeMs <= boundMs;
		// A park handshake is in flight: every boundary authored now is deferred to its final end, so declaring
		// one would propose a seat hold that cannot take effect and the caller would ask again next tick.
		if (m_CaptureParkAwaitingReports) return false;
		// The commit horizon runs a delay window ahead of the simulation, so an idle horizon is what a full
		// pipeline looks like: committed frames nobody has consumed yet are the round's runway. Judge a seat
		// only once that runway can no longer carry the decision's notice - until then nothing is waiting.
		// The runway is the shortest any survivor has: one ahead of this host runs dry first, and it is the one waiting.
		uint64_t runwayFrames = m_ReadyFrames.size();
		for (uint8_t survivor: m_RemotePeerIds) {
			if (std::find(missing.begin(), missing.end(), survivor) != missing.end() || IsPeerGoneAtFrame(survivor, frame) || IsSeatUnderAI(survivor, frame)) continue;
			// The input for a frame is produced while simulating the frame a delay earlier, so the next frame it simulates follows that.
			const uint64_t produced = m_Stats.peers[survivor].highestTargetFrame, delay = InputDelayAt(survivor, frame);
			if (produced >= delay) runwayFrames = std::min<uint64_t>(runwayFrames, frame > produced - delay + 1 ? frame - (produced - delay + 1) : 0);
		}
		const uint64_t runwayMs = static_cast<uint64_t>(std::llround(runwayFrames * m_Config.simTickMs));
		if (runwayMs > noticeMs) return false;
		// Declaring early lets the decision land by the bound. When the notice alone costs more than the
		// bound - a survivor on a long link - it cannot, and the survivors wait for the decision instead;
		// the seat is still only declared after the bound of missing input, never the instant one is late.
		const uint64_t declarationDeadline = noticeMs < boundMs ? boundMs - noticeMs : boundMs;
		if (nowMs >= firstMissingMs && nowMs - firstMissingMs >= declarationDeadline) {
			m_Stats.lastHoldDeclarationMs = nowMs - firstMissingMs;
			bool held = false;
			for (uint8_t peer: missing) {
				const auto& peerStats = m_Stats.peers[peer];
				// A park commits its frames with no input from anyone, so a seat owes nothing until the frames after it fall
				// due: its first post-park inputs are due a delay after the park's last frame ran here, never at the release.
				if (const uint16_t owed = InputDelayAt(peer, frame); m_ParkFrameSimulated != UINT64_MAX && m_ParkFrameSimulated == m_SynchronizedCaptureEndFrame &&
				    frame > m_ParkFrameSimulated && frame <= m_ParkFrameSimulated + std::max<uint16_t>(1, owed)) {
					const uint64_t dueMs = m_ParkFrameSimulatedMs + static_cast<uint64_t>(std::llround(owed * m_Config.simTickMs));
					if (dueMs > firstMissingMs && (nowMs < dueMs || nowMs - dueMs < declarationDeadline)) continue;
				}
				// A seat produces this frame's input when it simulates the frame one delay earlier, which it could not
				// do before we committed that frame and it crossed the seat's link: until then it waits on us.
				const uint64_t producedFrom = frame - std::min<uint64_t>(frame, std::max<uint16_t>(1, InputDelayAt(peer, frame)));
				if (const auto given = m_CommittedAtMs.find(producedFrom); given != m_CommittedAtMs.end()) {
					const uint64_t answerableMs = given->second + peerStats.pingMs + peerStats.jitterMs;
					if (answerableMs > firstMissingMs && (nowMs < answerableMs || nowMs - answerableMs < declarationDeadline)) continue;
				}
				// Nor can it produce at or past a timing decision's frame before our commit of it has crossed its link.
				uint64_t gateCommittedMs = 0;
				for (const auto& [applyFrame, committedMs]: m_DecisionCommittedAtMs) if (applyFrame <= frame) gateCommittedMs = std::max(gateCommittedMs, committedMs);
				if (gateCommittedMs != 0) {
					const uint64_t answerableMs = gateCommittedMs + peerStats.pingMs + peerStats.jitterMs + static_cast<uint64_t>(std::ceil(m_Config.simTickMs));
					if (answerableMs > firstMissingMs && (nowMs < answerableMs || nowMs - answerableMs < declarationDeadline)) continue;
				}
				// A sender's first frames of the round are its pipeline filling: the round starts skewed by
				// the start message's own trip and each machine's startup work, and the sender's delay
				// window is the budget that fill was agreed to take. Its first second of play is judged by
				// that ramp; after it the sender is judged like any other.
				// A sender still feeding the round every tick is not stalled, it is behind: the round absorbs the
				// skew by waiting, but only while its stream is within the bound of this frame, so it never paces
				// the others past the bound. The bound catches a stream that STOPPED or strayed.
				const bool feeding = peerStats.lastProgressMs >= firstMissingMs && nowMs - peerStats.lastProgressMs < declarationDeadline &&
				    peerStats.highestTargetFrame + m_Config.slowPlayerBoundTicks >= frame && peerStats.highestTargetFrame <= frame + m_Config.slowPlayerBoundTicks;
				if (!m_PeerAdmissions.contains(peer) && (!m_PeersPlayedThisRound.contains(peer) ||
				    frame <= EffectiveStartOf(peer) + std::max<uint64_t>(m_Config.slowPlayerBoundTicks, c_StartupSettleTicks))) {
					// Our own longest park is the start work this machine did; a peer that has not produced
					// yet is doing the same, so it is allowed as much before its silence means anything.
					const uint64_t park = std::max(peerStats.startParkMs, m_Stats.longestOwnParkMs);
					const uint64_t ramp = static_cast<uint64_t>(std::llround(InputDelayAt(peer, frame) * m_Config.simTickMs)) +
					    peerStats.pingMs + peerStats.jitterMs + park;
					if (nowMs - firstMissingMs < declarationDeadline + ramp || feeding) continue;
					std::cout << "[net-lockstep] bound judged peer " << static_cast<int>(peer) << " at frame " << frame
					          << " starting: since_missing=" << (nowMs - firstMissingMs) << "ms deadline=" << declarationDeadline
					          << "ms ramp=" << ramp << "ms own_park=" << m_Stats.longestOwnParkMs
					          << "ms peer_park=" << peerStats.startParkMs << "ms played=" << m_PeersPlayedThisRound.contains(peer) << std::endl;
				} else if (IsReturningSeatBeforeItsFirstInput(peer)) {
					// The round reaches the first frame a returning seat owes one window after the
					// activation, but that seat only hears of the activation a trip later and pays its
					// restart before its first tick, and its new start has to reach us before anything it sends
					// can be read at all: the answer needs that handshake's trip on top. The window
					// buys the frames; this buys the trip, once, and only until its stream is flowing. Only that
					// peer's own published restart counts here: our park is our machine's work, not its.
					// A seat that returns on a new connection has no samples on it yet and may have published no
					// restart: ping, jitter and park all read zero and the allowance would be none at all. The
					// window the round agreed for that peer was sized from its link, so it is the floor.
					const uint64_t windowMs = static_cast<uint64_t>(std::max<long long>(0, std::llround(InputDelayAt(peer, frame) * m_Config.simTickMs)));
					// A link this round has never measured borrows the slowest one it has: the seat's answer still
					// has to cross a real network, and zero is the one reading it cannot have.
					uint32_t linkMs = peerStats.pingMs, linkJitterMs = peerStats.jitterMs;
					if (linkMs == 0) {
						for (const auto& [other, otherStats]: m_Stats.peers) {
							if (other == m_Config.localPeerId || otherStats.pingMs <= linkMs) continue;
							linkMs = otherStats.pingMs;
							linkJitterMs = std::max(linkJitterMs, otherStats.jitterMs);
						}
					}
					// The window the seat was admitted on was sized for its restart; what its machine costs past that
					// window is its own, so the survivors lend it the slow-player bound and no more.
					const uint64_t restartMs = std::min<uint64_t>(peerStats.startParkMs, boundMs);
					// A seat that caught up in place, or whose catch-up reached its reclaim frame, started its round before that frame and has
					// no restart to pay: its window covers its link like any seat's, so the survivors lend it nothing more.
					const uint64_t ramp = peerStats.returnerCaughtUpMs != 0 || peerStats.returnsInPlace ? 0 :
					    std::max<uint64_t>(windowMs, 2 * static_cast<uint64_t>(linkMs) + linkJitterMs + restartMs);
					// The clock starts at the admission, never at a dryness that began while the seat was away.
					const uint64_t clockFrom = std::max({firstMissingMs, peerStats.reclaimAdmittedMs, peerStats.returnerCaughtUpMs});
					if (nowMs < clockFrom) continue;
					const uint64_t since = nowMs - clockFrom;
					if (since < declarationDeadline + ramp) continue;
					std::cout << "[net-lockstep] bound judged returning peer " << static_cast<int>(peer) << " at frame " << frame
					          << ": since_missing=" << since << "ms deadline=" << declarationDeadline
					          << "ms ramp=" << ramp << "ms ping=" << peerStats.pingMs << "ms link=" << linkMs << "ms jitter=" << linkJitterMs
					          << "ms own_park=" << m_Stats.longestOwnParkMs << "ms peer_park=" << restartMs
					          << "ms heard_through=" << peerStats.highestTargetFrame << " clock=" << NetLockstepSharedClockMs() << std::endl;
				} else if (feeding) {
					continue;
				}
				std::string holdError;
				if (ProposePeerHold(peer, nowMs, &holdError)) held = true;
				else std::cout << "[net-lockstep] hold refused peer=" << static_cast<int>(peer) << ": " << holdError << std::endl;
			}
			return held;
		}
		return false;
	}

	bool NetLockstepCoordinator::NoteFrameWait(uint64_t frame, uint64_t nowMs, bool waitingForDecision) {
		NET_PLANE_CHECK();
		if (!UsesBoundedWait()) return false;
		if (frame < m_Stats.nextFrame) return true;
		if (m_ConsumerWaitingFrame != frame) {
			m_ConsumerWaitingFrame = frame;
			m_ConsumerWaitStartMs = nowMs;
			m_ConsumerWaitCounted = false;
		}
		const uint64_t elapsed = nowMs >= m_ConsumerWaitStartMs ? nowMs - m_ConsumerWaitStartMs : 0;
		const bool first = !m_ConsumerWaitCounted && elapsed > 0;
		if (first) { ++m_Stats.blockingFrameWaits; m_ConsumerWaitCounted = true; }
		m_Stats.longestStallMs = std::max(m_Stats.longestStallMs, elapsed);
		const auto remote = m_RemoteFrames.find(frame);
		std::vector<uint8_t> missing;
		for (uint8_t peer: m_RemotePeerIds) {
			bool awaitingAck = false;
			if (waitingForDecision && m_Config.localPeerId == GetHostPeerId()) {
				for (const auto& [revision, decision]: m_TimingDecisions) {
					const uint8_t bit = static_cast<uint8_t>(1U << (peer - 1));
					awaitingAck |= !decision.committed && decision.proposal.applyFrame <= frame &&
					    (decision.proposal.requiredPeers & bit) != 0 && (decision.acknowledgedPeers & bit) == 0;
				}
			}
			if (!awaitingAck && (!IsRemoteRequiredForFrame(peer, frame) || (remote != m_RemoteFrames.end() && remote->second.contains(peer)))) continue;
			missing.push_back(peer);
			auto& stats = m_Stats.peers[peer];
			if (first) { ++stats.waits; ++stats.waitsSinceReclaim; }
			stats.longestWaitMs = std::max(stats.longestWaitMs, elapsed);
			stats.longestWaitMsSinceReclaim = std::max(stats.longestWaitMsSinceReclaim, elapsed);
		}
		if (!missing.empty()) m_Stats.lastMissingPeers = DescribeMissingPeers();
		// A wait past the bound names what it waits on, once: a stall is a defect to be found, not a number.
		if (elapsed >= 100 && m_DescribedWaitFrame != frame) {
			m_DescribedWaitFrame = frame;
			std::cout << "[net-frame-wait] frame=" << frame << " waiting_ms=" << elapsed << " next=" << m_Stats.nextFrame << " missing=" << DescribeMissingPeers()
			          << " blocked=" << m_AdvanceBlock << " local_sent_through=" << (m_LastQueuedTargetFrame == UINT64_MAX ? -1 : static_cast<int64_t>(m_LastQueuedTargetFrame))
			          << " decision=" << TimingDecisionPendingAt(frame) << " " << DescribePendingTimingDecisions(frame) << std::endl;
		}
		const uint64_t firstMissing = m_FirstMissingFrame == frame ? std::min(m_FirstMissingMs, m_ConsumerWaitStartMs) : m_ConsumerWaitStartMs;
		if (DeclareOverdueInputs(frame, nowMs, firstMissing, missing)) AdvanceReadyFrames(nowMs);
		return m_Stats.nextFrame > frame;
	}

	void NetLockstepCoordinator::FinishFrameWait(uint64_t nowMs) {
		NET_PLANE_CHECK();
		if (m_ConsumerWaitingFrame && nowMs >= m_ConsumerWaitStartMs) {
			m_Stats.longestStallMs = std::max(m_Stats.longestStallMs, nowMs - m_ConsumerWaitStartMs);
			if (nowMs > m_ConsumerWaitStartMs) {
				std::cout << "[net-frame-wait] frame=" << *m_ConsumerWaitingFrame << " wait_ms=" << nowMs - m_ConsumerWaitStartMs;
				if (nowMs - m_ConsumerWaitStartMs >= 100) std::cout << " blocked=" << m_AdvanceBlock << " duplicates=" << m_WaitDuplicates;
				std::cout << std::endl;
			}
		}
		m_ConsumerWaitingFrame.reset();
		m_WaitDuplicates = 0;
	}

	void NetLockstepCoordinator::NoteLocalTickCost(uint64_t producedFrame, double computeMs) {
		NET_PLANE_CHECK();
		if (m_Playback || m_Config.simTickMs <= 0 || !std::isfinite(computeMs) || computeMs < 0) return;
		if (!m_Stats.measuredMissingFrameBase && producedFrame >= m_Config.startFrame + 300) {
			m_Stats.measuredMissingFrameBase = m_Stats.missingFrameStalls;
			m_Stats.measuredBlockingWaitBase = m_Stats.blockingFrameWaits;
		}
		const bool overrun = computeMs > m_Config.simTickMs;
		if (overrun) ++m_Stats.localTickOverruns;
		m_Stats.localComputeDebtMs = std::max(0.0, m_Stats.localComputeDebtMs + computeMs - m_Config.simTickMs);
	}

	void NetLockstepCoordinator::NoteLocalInputProduced(uint64_t producedFrame, uint64_t nowUs, uint64_t networkWaitUs) {
		NET_PLANE_CHECK();
		if (m_Playback || m_Config.simTickMs <= 0) return;
		if (!m_ProductionBaseFrame || producedFrame < *m_ProductionBaseFrame || nowUs < m_ProductionBaseUs || networkWaitUs < m_ProductionWaitBaseUs) {
			m_ProductionBaseFrame = producedFrame;
			m_ProductionBaseUs = nowUs;
			m_ProductionWaitBaseUs = networkWaitUs;
		}
		const uint64_t elapsed = nowUs - m_ProductionBaseUs, waited = networkWaitUs - m_ProductionWaitBaseUs;
		const double localElapsedMs = (elapsed > waited ? elapsed - waited : 0) / 1000.0;
		const double deadlineMs = (producedFrame - *m_ProductionBaseFrame + std::max<uint16_t>(1, InputDelayAt(m_Config.localPeerId, producedFrame))) * m_Config.simTickMs;
		m_Stats.localProductionLateMs = std::max(0.0, localElapsedMs - deadlineMs);
		const bool late = m_Stats.localProductionLateMs > 0;
		if (late) { ++m_Stats.localLateInputs; ++m_Stats.consecutiveLateInputs; }
		else m_Stats.consecutiveLateInputs = 0;
		m_Stats.localMachineSlow = m_Stats.consecutiveLateInputs >= 8 && m_Stats.localComputeDebtMs >= m_Config.simTickMs;
	}

	void NetLockstepCoordinator::CommitTiming(uint64_t revision) {
		auto found = m_TimingDecisions.find(revision);
		if (found == m_TimingDecisions.end() || found->second.committed || m_Config.localPeerId != GetHostPeerId()) return;
		auto& decision = found->second;
		if ((decision.acknowledgedPeers & decision.proposal.requiredPeers) != decision.proposal.requiredPeers) return;
		NetLockstepTiming commit = decision.proposal;
		commit.phase = NetTimingPhase::Commit;
		QueueTiming(commit);
		decision.committed = true;
		// A peer produces nothing at or past this frame until the commit reaches it; its lateness there is ours.
		if (UsesBoundedWait()) {
			auto& committedMs = m_DecisionCommittedAtMs[commit.applyFrame];
			committedMs = std::max(committedMs, m_TimingNowMs);
			while (m_DecisionCommittedAtMs.size() > 64) m_DecisionCommittedAtMs.erase(m_DecisionCommittedAtMs.begin());
		}
		FlushTimingOutgoing();
		ApplyTiming(commit);
		if (commit.action == NetTimingAction::Hold) {
			std::vector<uint64_t> waiting;
			for (const auto& [other, pending]: m_TimingDecisions) if (!pending.committed) waiting.push_back(other);
			for (uint64_t other: waiting) CommitTiming(other);
		}
	}

	void NetLockstepCoordinator::TakeReturnBeforeFirstFrame(const NetLockstepTiming& reclaim) {
		const uint8_t peer = reclaim.peerId;
		if (!IsKnownRemotePeer(peer)) { m_RemotePeerIds.push_back(peer); std::sort(m_RemotePeerIds.begin(), m_RemotePeerIds.end()); }
		m_ReclaimTransactions[peer] = {peer, reclaim.authorityGeneration, reclaim.revision, reclaim.seatIncarnations[peer - 1], reclaim.applyFrame,
		    reclaim.delayFrames, reclaim.neutralThroughFrame, reclaim.worldTransition};
		m_Config.peerIncarnations[peer] = reclaim.seatIncarnations[peer - 1];
		m_PeerEffectiveStart[peer] = std::max(m_Config.startFrame, reclaim.applyFrame + reclaim.delayFrames);
		m_PeerAdmissions[peer] = reclaim.applyFrame < m_Config.startFrame ? PeerAdmission{m_Config.startFrame, PeerInputDelay(peer)} :
		    PeerAdmission{reclaim.applyFrame, reclaim.delayFrames};
		// The seat state this round started from may still hold the seat; the return ends that hold here, as the frame that
		// carried it did on every peer that simulated it, or our first frame would hand the returned seat to the AI again.
		m_AiHeldSeats.erase(peer); m_HoldTransactions.erase(peer); m_ReleasedAiSeats.erase(peer);
		m_PeerLeaveFrames.erase(peer); m_PeerFrameWaivers.erase(peer);
		m_DroppedSeats.erase(peer); m_LeftSeatsHeld.erase(peer); m_DroppedAtMs.erase(peer);
		m_DroppedSeatResolutions[peer] = NetLockstepHoldResolution::Reclaimed;
		// The host may have answered our start before this return: the decision stands in for the start it will not repeat.
		m_RemoteStartsReceived.insert(peer);
	}

	void NetLockstepCoordinator::HandleTiming(const NetLockstepTiming& timing, uint64_t nowMs, NetPeerId fromTransport) {
		// A hold or a return that lands by a joining round's first frame is taken at once, even before the round runs: the starts
		// the handshake waits for depend on it, and nothing later in the round revisits a frame behind its first.
		if (m_Config.joinsRunningRound && timing.applyFrame <= m_Config.startFrame &&
		    ((timing.phase == NetTimingPhase::ReclaimAtFrame && timing.peerId != m_Config.localPeerId) ||
		     (timing.phase == NetTimingPhase::HoldAtFrame && (timing.heldPeers & (1U << (m_Config.localPeerId - 1))) == 0))) {
			if (timing.senderPeerId != GetHostPeerId() || LockstepPeerOfTransport(fromTransport) != GetHostPeerId() || timing.sessionId != m_Config.sessionId ||
			    (m_RoundId != 0 && timing.roundId != m_RoundId) || timing.authorityGeneration != m_Config.migrationGeneration || timing.peerId == 0 ||
			    timing.peerId > 4 || timing.peerId > m_Config.peerCount) return;
			// The seat state this round started from already applied every transition up to this frame.
			if (timing.applyFrame <= m_Config.seatStateThroughFrame) return;
			if (timing.phase == NetTimingPhase::ReclaimAtFrame) {
				const auto prior = m_ReclaimTransactions.find(timing.peerId);
				const uint32_t known = m_Config.peerIncarnations[timing.peerId];
				if ((prior != m_ReclaimTransactions.end() && prior->second.eventSequence >= timing.revision) ||
				    (timing.peerId == GetHostPeerId() ? timing.seatIncarnations[timing.peerId - 1] < known : timing.seatIncarnations[timing.peerId - 1] <= known)) return;
				TakeReturnBeforeFirstFrame(timing);
				return;
			}
			if (!UsesBoundedWait()) return;
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer)
				if ((timing.heldPeers & (1U << (peer - 1))) != 0 && timing.seatIncarnations[peer - 1] < m_Config.peerIncarnations[peer]) return;
			if (!m_TimingDecisions.try_emplace(timing.revision, TimingDecision{timing, 0, true, nowMs}).second) return;
			m_TimingNowMs = nowMs;
			ApplyTiming(timing);
			// A seat held when we arrive owes this round no start.
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer)
				if ((timing.heldPeers & (1U << (peer - 1))) != 0 && IsKnownRemotePeer(peer)) m_RemoteStartsReceived.insert(peer);
			return;
		}
		if (m_State == NetLockstepState::WaitingForStart && timing.senderPeerId == GetHostPeerId() &&
		    timing.sessionId == m_Config.sessionId && (m_RoundId == 0 || timing.roundId == m_RoundId) && SenderOwnsTransport(timing.senderPeerId, fromTransport)) {
			if (m_PreStartTiming.size() >= 256) { Fail(NetLockstepStopReason::ProtocolError, m_Config.startFrame, "pre-start timing backlog overflow"); return; }
			m_PreStartTiming.emplace_back(timing, fromTransport);
			// A joiner's own admission names the frame and delay its start must carry, or the host reads that start as a straggler.
			if (m_Config.joinsRunningRound && timing.action == NetTimingAction::WorldAdmission && timing.peerId == m_Config.localPeerId &&
			    (timing.phase == NetTimingPhase::Propose || timing.phase == NetTimingPhase::Commit)) {
				const PeerAdmission terms{timing.applyFrame, timing.delayFrames};
				const auto adopted = m_PeerAdmissions.find(m_Config.localPeerId);
				if (adopted == m_PeerAdmissions.end() || adopted->second.frame != terms.frame || adopted->second.delay != terms.delay) {
					m_PeerAdmissions[m_Config.localPeerId] = terms;
					std::cout << "[lockstep] took this seat's admission before the start: frame=" << terms.frame << " delay=" << terms.delay << std::endl;
					(void)SendStart(nullptr);
				}
			}
			return;
		}
		if (!IsRunning() || timing.sessionId != m_Config.sessionId || timing.roundId != m_RoundId || timing.authorityGeneration != m_Config.migrationGeneration ||
		    timing.peerId > m_Config.peerCount || !SenderOwnsTransport(timing.senderPeerId, fromTransport)) return;
		const bool authority = timing.senderPeerId == GetHostPeerId() && LockstepPeerOfTransport(fromTransport) == GetHostPeerId();
		// A decision the host re-stamped names the proposal it replaces: the peer drops that one here, so it can
		// never hold two pending proposals for the same timing or match a commit against the withdrawn frame.
		if (authority && timing.supersededRevision != 0) m_TimingDecisions.erase(timing.supersededRevision);
		if (timing.action == NetTimingAction::CapturePark) {
			if (timing.phase == NetTimingPhase::Status) {
				if (m_Config.localPeerId != GetHostPeerId() || timing.peerId != timing.senderPeerId) return;
				// Each park closes on its own reports; a report for a park that is over says nothing about this one.
				if (timing.applyFrame != m_SynchronizedCaptureStartFrame) return;
				m_CaptureParkReportsMs[timing.peerId] = std::max(m_CaptureParkReportsMs[timing.peerId], timing.pingMs);
				PublishCapturePark(timing.applyFrame);
			} else if (timing.phase == NetTimingPhase::Commit && authority) {
				ApplyCapturePark(timing);
			} else if (m_Config.localPeerId != GetHostPeerId()) {
				std::cout << "[net-lockstep] capture park packet ignored phase=" << static_cast<int>(timing.phase)
				          << " authority=" << authority << std::endl;
			}
			return;
		}
		if (authority && timing.action == NetTimingAction::WorldAdmission && (timing.phase == NetTimingPhase::Propose || timing.phase == NetTimingPhase::Commit)) {
			const auto applied = m_ReclaimTransactions.find(timing.peerId);
			const NetGameSeatReclaim expected{timing.peerId, timing.authorityGeneration, timing.revision, timing.seatIncarnations[timing.peerId - 1],
			    timing.applyFrame, timing.delayFrames, timing.neutralThroughFrame, timing.worldTransition};
			if (applied != m_ReclaimTransactions.end() && applied->second == expected) return;
		}
		if (timing.phase == NetTimingPhase::ReclaimAtFrame) {
			if (!authority || timing.peerId > 4 || timing.applyFrame < m_Stats.nextFrame) return;
			const auto prior = m_ReclaimTransactions.find(timing.peerId);
			if (prior != m_ReclaimTransactions.end() && prior->second.eventSequence >= timing.revision) return;
			// A returning player comes back as a new incarnation; the host's own seat comes back on the connection it never left.
			const uint32_t known = m_Config.peerIncarnations[timing.peerId];
			if (timing.peerId == GetHostPeerId() ? timing.seatIncarnations[timing.peerId - 1] < known : timing.seatIncarnations[timing.peerId - 1] <= known) return;
			m_TimingDecisions[timing.revision] = {timing, timing.requiredPeers, true, nowMs};
			ApplyTiming(timing);
			return;
		}
		if (timing.phase == NetTimingPhase::HoldAppliedAck) {
			if (m_Config.localPeerId != GetHostPeerId()) return;
			const auto found = m_TimingDecisions.find(timing.revision);
			if (found == m_TimingDecisions.end() || found->second.proposal.phase != NetTimingPhase::HoldAtFrame) return;
			NetLockstepTiming expected = found->second.proposal;
			expected.senderPeerId = timing.senderPeerId; expected.phase = NetTimingPhase::HoldAppliedAck; expected.nextFrame = timing.nextFrame;
			if (expected == timing && timing.nextFrame > timing.applyFrame)
				found->second.acknowledgedPeers |= static_cast<uint8_t>(1U << (timing.senderPeerId - 1));
			return;
		}
		if (timing.phase == NetTimingPhase::HoldAtFrame) {
			if (!authority || !UsesBoundedWait()) return;
			const bool ownHold = (timing.heldPeers & (1U << (m_Config.localPeerId - 1))) != 0;
			bool repeated = true;
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if ((timing.heldPeers & (1U << (peer - 1))) != 0) {
				const auto current = m_Config.peerIncarnations.find(peer);
				if (current != m_Config.peerIncarnations.end() && timing.seatIncarnations[peer - 1] < current->second) return;
				const NetGameSeatHold transaction{peer, timing.authorityGeneration, timing.revision, timing.seatIncarnations[peer - 1], timing.cutoffFrame};
				repeated = repeated && m_HoldTransactions.contains(peer) && m_HoldTransactions.at(peer) == transaction;
			}
			if (repeated) return;
			if ((!ownHold && timing.applyFrame < m_Stats.nextFrame) || timing.applyFrame > m_Stats.nextFrame + NetLockstepCodec::c_MaxFutureFrameSkew) {
				std::ostringstream clause;
				clause << "hold contradicts the survivor's accepted horizon: " << (timing.applyFrame < m_Stats.nextFrame ? "hold behind the accepted horizon" : "hold past the future skew")
				       << " held=" << static_cast<int>(timing.heldPeers) << " hold_frame=" << timing.applyFrame << " accepted_through=" << (m_Stats.nextFrame > 0 ? m_Stats.nextFrame - 1 : 0)
				       << " announcer=" << static_cast<int>(timing.senderPeerId) << " revision=" << timing.revision << " start=" << m_Config.startFrame;
				Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, clause.str());
				return;
			}
			auto [found, inserted] = m_TimingDecisions.try_emplace(timing.revision, TimingDecision{timing, 0, true, nowMs});
			if (!inserted) {
				if (found->second.proposal != timing) Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "conflicting hold transaction");
				return;
			}
			ApplyTiming(timing);
			return;
		}
		if (timing.action == NetTimingAction::Hold && timing.phase != NetTimingPhase::Status) return;
		if (timing.phase == NetTimingPhase::Status) {
			if (!authority && (m_Config.localPeerId != GetHostPeerId() || timing.peerId != timing.senderPeerId)) return;
			auto& stats = m_Stats.peers[timing.peerId];
			if (timing.nextFrame > m_Stats.nextFrame + NetLockstepCodec::c_MaxFutureFrameSkew) return;
			stats.reportedNextFrame = std::max(stats.reportedNextFrame, timing.nextFrame);
			if (authority) { stats.pingMs = timing.pingMs; stats.jitterMs = timing.jitterMs; stats.delayFrames = timing.delayFrames; }
			return;
		}
		if (timing.phase == NetTimingPhase::Acknowledge) {
			if (m_Config.localPeerId != GetHostPeerId()) return;
			auto found = m_TimingDecisions.find(timing.revision);
			if (found == m_TimingDecisions.end()) return;
			NetLockstepTiming expected = found->second.proposal;
			expected.senderPeerId = timing.senderPeerId;
			expected.phase = NetTimingPhase::Acknowledge;
			expected.nextFrame = timing.nextFrame;
			if (expected != timing || timing.nextFrame > timing.applyFrame) {
				Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "timing acknowledgement crossed its boundary");
				return;
			}
			found->second.acknowledgedPeers |= static_cast<uint8_t>(1U << (timing.senderPeerId - 1));
			CommitTiming(timing.revision);
			return;
		}
		if (!authority) return;
		// A seat joining a running round replayed every frame before its first: a delay decided for one of them belongs to that
		// tail, so it is taken for the frames after it and never acknowledged.
		const bool tailDelay = m_Config.joinsRunningRound && timing.action == NetTimingAction::Delay && timing.applyFrame < m_Config.startFrame;
		if (timing.phase == NetTimingPhase::Propose) {
			const bool ownHold = timing.action == NetTimingAction::Hold && (timing.heldPeers & (1U << (m_Config.localPeerId - 1))) != 0;
			if ((timing.action == NetTimingAction::Hold && !UsesBoundedWait()) || (!ownHold && !tailDelay && timing.applyFrame < m_Stats.nextFrame) ||
			    (timing.applyFrame > m_Stats.nextFrame && timing.applyFrame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew) || m_TimingDecisions.size() >= 16) {
				Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "timing proposal missed its boundary");
				return;
			}
			auto [found, inserted] = m_TimingDecisions.try_emplace(timing.revision, TimingDecision{timing});
			if (!inserted && found->second.proposal != timing) {
				Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "conflicting timing proposal");
				return;
			}
			NetLockstepTiming ack = timing;
			ack.senderPeerId = m_Config.localPeerId;
			ack.phase = NetTimingPhase::Acknowledge;
			ack.nextFrame = m_Stats.nextFrame;
			if (!tailDelay && (timing.requiredPeers & (1U << (m_Config.localPeerId - 1))) != 0) QueueTiming(ack, GetHostPeerId());
		} else if (timing.phase == NetTimingPhase::Commit) {
			auto found = m_TimingDecisions.find(timing.revision);
			NetLockstepTiming proposed = timing;
			proposed.phase = NetTimingPhase::Propose;
			// The tail's proposal can predate the link this round listens on; its commit alone is the host's decision.
			if (found == m_TimingDecisions.end() && tailDelay) found = m_TimingDecisions.try_emplace(timing.revision, TimingDecision{proposed}).first;
			if (found == m_TimingDecisions.end() || proposed != found->second.proposal) {
				Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "timing commit has no matching proposal");
				return;
			}
			if (!found->second.committed) { found->second.committed = true; ApplyTiming(timing); }
		}
		(void)nowMs;
	}

	bool NetLockstepCoordinator::DecisionSettled(const TimingDecision& decision) const {
		const bool applied = decision.proposal.phase != NetTimingPhase::HoldAtFrame ||
		    (m_Config.localPeerId == GetHostPeerId() ? (decision.acknowledgedPeers & decision.proposal.requiredPeers) == decision.proposal.requiredPeers :
		     (decision.acknowledgedPeers & (1U << (m_Config.localPeerId - 1))) != 0);
		return decision.committed && applied && decision.proposal.applyFrame < GetResumeFrame();
	}

	std::optional<uint16_t> NetLockstepCoordinator::SlackLimitedDecrease(uint8_t peerId, uint16_t proposed, uint16_t current, uint64_t nowMs) {
		// A lower delay makes the sender's next inputs due that many ticks sooner. When our sim is already waiting on
		// them the frames between the two delays are never produced in time, so the change would hold the seat.
		// Only arrivals made under the delay now in force count, over a whole window of them: a change still pending,
		// or one whose inputs have not been measured for a window, leaves no slack to spend.
		uint64_t firstFrameUnderCurrent = 0;
		if (const auto changes = m_DelayChanges.find(peerId); changes != m_DelayChanges.end() && !changes->second.empty()) {
			const auto& [applyFrame, delay] = *changes->second.rbegin();
			if (delay != current) return std::nullopt;
			firstFrameUnderCurrent = applyFrame + delay;
		}
		const auto found = m_ArrivalLeads.find(peerId);
		if (found == m_ArrivalLeads.end()) return std::nullopt;
		const auto& leads = found->second;
		const auto first = std::find_if(leads.begin(), leads.end(), [&](const ArrivalLead& arrival) { return arrival.frame >= firstFrameUnderCurrent; });
		if (first == leads.end() || first->ms > nowMs || nowMs - first->ms < NetInputDelayEstimator::c_WindowMs) return std::nullopt;
		uint64_t spare = UINT64_MAX;
		for (auto arrival = first; arrival != leads.end(); ++arrival)
			if (nowMs - arrival->ms <= NetInputDelayEstimator::c_WindowMs && arrival->frame >= firstFrameUnderCurrent) spare = std::min(spare, arrival->lead);
		if (spare == UINT64_MAX) return std::nullopt;
		// The seat keeps the slow-player bound's worth of that lead, so a spike the bound absorbed before the change still is.
		const uint64_t keep = std::max<uint64_t>(1, m_Config.slowPlayerBoundTicks);
		const uint64_t needed = spare >= current + keep ? 0 : static_cast<uint64_t>(current) + keep - spare;
		const uint64_t delay = std::max<uint64_t>(proposed, needed);
		return delay < current ? std::optional<uint16_t>{static_cast<uint16_t>(delay)} : std::nullopt;
	}

	std::optional<uint16_t> NetLockstepCoordinator::MarginKeepingIncrease(uint8_t peerId, uint16_t current, uint32_t required, uint64_t nowMs) const {
		// A seat whose inputs arrive with less than the slow-player bound's worth of lead is one load spike away from a
		// hold. Its delay rises by what the lead lacks, before any wait: only arrivals made under the delay now in force
		// count, over a whole short window of them, so a change still pending or not yet measured leaves nothing to raise.
		uint64_t firstFrameUnderCurrent = 0;
		if (const auto changes = m_DelayChanges.find(peerId); changes != m_DelayChanges.end() && !changes->second.empty()) {
			const auto& [applyFrame, delay] = *changes->second.rbegin();
			if (delay != current) return std::nullopt;
			firstFrameUnderCurrent = applyFrame + delay;
		}
		const auto found = m_ArrivalLeads.find(peerId);
		if (found == m_ArrivalLeads.end()) return std::nullopt;
		const auto& leads = found->second;
		const auto first = std::find_if(leads.begin(), leads.end(), [&](const ArrivalLead& arrival) { return arrival.frame >= firstFrameUnderCurrent; });
		if (first == leads.end() || first->ms > nowMs || nowMs - first->ms < c_MarginWindowMs) return std::nullopt;
		uint64_t least = UINT64_MAX;
		for (auto arrival = first; arrival != leads.end(); ++arrival)
			if (nowMs - arrival->ms <= c_MarginWindowMs) least = std::min(least, arrival->lead);
		const uint64_t keep = std::max<uint64_t>(1, m_Config.slowPlayerBoundTicks);
		if (least == UINT64_MAX || least >= keep) return std::nullopt;
		// The rise stays within the bound above what the link's round trip needs: a machine that cannot keep pace gains
		// nothing from a longer delay, and the bound is what answers it.
		const uint64_t ceiling = std::min<uint64_t>(static_cast<uint64_t>(required) + keep, NetLockstepCodec::c_MaxInputDelayFrames);
		const uint64_t delay = std::min<uint64_t>(static_cast<uint64_t>(current) + keep - least, ceiling);
		return delay > current ? std::optional<uint16_t>{static_cast<uint16_t>(delay)} : std::nullopt;
	}

	void NetLockstepCoordinator::TickTiming(uint64_t nowMs) {
		if (!IsRunning() || m_Playback) return;
		const bool host = m_Config.localPeerId == GetHostPeerId();
		if (m_LastTimingSampleMs == UINT64_MAX || nowMs - m_LastTimingSampleMs >= NetInputDelayEstimator::c_SampleMs) {
			m_LastTimingSampleMs = nowMs;
			for (const auto& [peer, transport]: m_RemoteTransports) {
				auto& estimator = m_DelayEstimators[peer];
				const uint32_t ping = m_Transport->GetPeerPingMs(transport);
				estimator.Observe(nowMs, ping);
				auto& stats = m_Stats.peers[peer];
				// A connection with no samples on it yet reports nothing, which is not a reading of an instant
				// link: a seat that comes back on a new transport keeps what its link last measured.
				if (ping > 0 || stats.pingMs == 0) stats.pingMs = ping;
				if (const uint32_t jitter = estimator.JitterMs(); jitter > 0 || stats.jitterMs == 0) stats.jitterMs = jitter;
				stats.delayFrames = InputDelayAt(peer, m_Stats.nextFrame);
				if (host && m_Config.adaptiveInputDelay) {
					std::optional<uint16_t> delay = estimator.Change(nowMs, stats.delayFrames, m_Config.simTickMs, m_Config.matchConfig.inputDelayFrames);
					if (delay && *delay < stats.delayFrames) delay = SlackLimitedDecrease(peer, *delay, stats.delayFrames, nowMs);
					const uint32_t required = estimator.RequiredFrames(m_Config.simTickMs, m_Config.matchConfig.inputDelayFrames);
					if (const auto kept = MarginKeepingIncrease(peer, stats.delayFrames, required, nowMs); kept && (!delay || *kept > *delay)) delay = kept;
					if (delay) ProposeInputDelay(peer, *delay, FutureTimingFrame());
				}
			}
		}
		if ((m_Config.adaptiveInputDelay || UsesBoundedWait()) && (m_LastTimingStatusMs == UINT64_MAX || nowMs - m_LastTimingStatusMs >= 500)) {
			m_LastTimingStatusMs = nowMs;
			const auto status = [&](uint8_t peer) {
				NetLockstepTiming timing;
				timing.senderPeerId = m_Config.localPeerId;
				timing.peerId = peer;
				timing.phase = NetTimingPhase::Status;
				timing.authorityGeneration = m_Config.migrationGeneration;
				timing.sessionId = m_Config.sessionId;
				timing.roundId = m_RoundId;
				timing.nextFrame = peer == m_Config.localPeerId ? m_Stats.nextFrame : m_Stats.peers[peer].reportedNextFrame;
				timing.delayFrames = InputDelayAt(peer, m_Stats.nextFrame);
				timing.pingMs = m_Stats.peers[peer].pingMs;
				timing.jitterMs = m_Stats.peers[peer].jitterMs;
				QueueTiming(timing);
			};
			status(m_Config.localPeerId);
			if (host) for (uint8_t peer: m_RemotePeerIds) status(peer);
		}
		std::vector<uint64_t> pending;
		if (host && UsesBoundedWait() && m_ConsumerWaitingFrame) {
			std::set<uint8_t> unresponsive;
			const auto boundMs = static_cast<uint64_t>(std::max<long long>(1, std::llround(m_Config.slowPlayerBoundTicks * m_Config.simTickMs)));
			for (const auto& [revision, decision]: m_TimingDecisions) {
				if (decision.committed || decision.proposal.applyFrame > *m_ConsumerWaitingFrame) continue;
				for (uint8_t peer: m_RemotePeerIds) {
					const uint8_t bit = static_cast<uint8_t>(1U << (peer - 1));
					const auto& stats = m_Stats.peers[peer];
					const uint64_t budget = boundMs + stats.pingMs + stats.jitterMs;
					// A peer still filling its pipeline owes its delay window before it counts as silent, and a
					// peer still starting cannot acknowledge anything: its own start work is part of the ramp. A
					// reclaimed seat refills the same way until its activation and its delay window have passed.
					const auto reclaim = m_ReclaimTransactions.find(peer);
					const bool refilling = reclaim != m_ReclaimTransactions.end() &&
					    *m_ConsumerWaitingFrame <= reclaim->second.activationFrame + InputDelayAt(peer, *m_ConsumerWaitingFrame);
					const uint64_t ramp = !refilling && (m_PeersPlayedThisRound.contains(peer) || m_PeerAdmissions.contains(peer)) ? 0 :
					    static_cast<uint64_t>(std::llround(InputDelayAt(peer, *m_ConsumerWaitingFrame) * m_Config.simTickMs)) +
					        std::max(stats.startParkMs, m_Stats.longestOwnParkMs);
					if ((decision.proposal.requiredPeers & bit) != 0 && (decision.acknowledgedPeers & bit) == 0 &&
					    nowMs >= decision.proposedAtMs && nowMs - decision.proposedAtMs >= budget + ramp) {
						std::cout << "[net-lockstep] timing ack overdue from peer " << static_cast<int>(peer)
						          << " at frame " << *m_ConsumerWaitingFrame << ": waited=" << (nowMs - decision.proposedAtMs)
						          << "ms budget=" << budget << "ms ramp=" << ramp << "ms own_park=" << m_Stats.longestOwnParkMs
						          << "ms peer_park=" << stats.startParkMs << std::endl;
						unresponsive.insert(peer);
					}
				}
			}
			for (uint8_t peer: unresponsive) ProposePeerHold(peer, nowMs);
		}
		for (const auto& [revision, decision]: m_TimingDecisions) if (!decision.committed) pending.push_back(revision);
		for (uint64_t revision: pending) CommitTiming(revision);
		FlushTimingOutgoing();
		// A held seat that comes back sets its round up from a tail that can stop short of the holds and returns decided since:
		// they stay here while a seat is held, for the answer to its start.
		const bool heldSeatMayReturn = host && !m_AiHeldSeats.empty();
		std::erase_if(m_TimingDecisions, [&](const auto& entry) {
			const auto& decision = entry.second;
			if (heldSeatMayReturn && (decision.proposal.phase == NetTimingPhase::HoldAtFrame || decision.proposal.phase == NetTimingPhase::ReclaimAtFrame)) return false;
			return DecisionSettled(decision);
		});
		const uint64_t oldest = m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew ? m_Stats.nextFrame - NetLockstepCodec::c_MaxFutureFrameSkew : 0;
		for (auto& [peer, changes]: m_DelayChanges)
			while (changes.size() > 1 && std::next(changes.begin())->first <= oldest) changes.erase(changes.begin());
	}

	void NetLockstepCoordinator::FlushTimingOutgoing() {
		for (auto it = m_TimingOutgoing.begin(); it != m_TimingOutgoing.end();) {
			const auto transport = m_RemoteTransports.find(it->first);
			if (transport == m_RemoteTransports.end()) { it = m_TimingOutgoing.erase(it); continue; }
			auto& queue = it->second;
			while (!queue.empty()) {
				std::vector<uint8_t> bytes;
				if (!NetLockstepCodec::Encode({queue.front()}, bytes) || !m_Transport->Send(transport->second, NetTransportLane::ControlReliable, bytes)) break;
				queue.pop_front();
			}
			if (queue.empty()) it = m_TimingOutgoing.erase(it); else ++it;
		}
	}

	bool NetLockstepCoordinator::PrimeResyncFrames(const std::vector<std::vector<NetGameCommand>>& batches, std::string* error) {
		NET_PLANE_CHECK();
		if (!IsRunning() || !m_Config.resumeFromSnapshot || m_ResyncPrimed || batches.size() != m_Config.inputDelayFrames) {
			if (error) *error = "invalid resync priming state or batch count";
			return false;
		}
		for (size_t index = 0; index < batches.size(); ++index) {
			NetLockstepFrame frame;
			frame.senderPeerId = m_Config.localPeerId; frame.roundId = m_RoundId; frame.targetFrame = m_Config.startFrame + index; frame.commands = batches[index];
			std::vector<uint8_t> encoded;
			NetLockstepError validation;
			if (std::any_of(frame.commands.begin(), frame.commands.end(), [&](const auto& command) { return command.senderPeerId != m_Config.localPeerId; }) || !NetLockstepCodec::Encode({frame}, encoded, &validation)) {
				if (error) *error = "invalid recovered commands: " + validation.message;
				return false;
			}
		}
		for (size_t index = 0; index < batches.size(); ++index) if (!QueueInputAtTarget(m_Config.startFrame + index, {}, batches[index], error, {})) return false;
		m_ResyncPrimed = true;
		m_ResumeAdmissionPending = false;
		return true;
	}

	bool NetLockstepCoordinator::QueueInputAtTarget(uint64_t targetFrame, const std::vector<ControllerFrame>& frames, const std::vector<NetGameCommand>& commands, std::string* error, const std::vector<NetSoundObservation>& observations, const std::vector<NetValueObservation>& valueObservations) {
		if (m_State != NetLockstepState::Running) {
			// Carry the stop reason so the caller can route it (a resync request must not read as a
			// generic failure).
			if (error) *error = m_Stats.timeoutReason.empty() ? "lockstep coordinator is not running" : m_Stats.timeoutReason;
			return false;
		}
		if (targetFrame < EffectiveStartOf(m_Config.localPeerId)) {
			if (error) *error = "local input precedes the sender's admission delay";
			return false;
		}
		static const auto holdBeforeTarget = TestFrameFromEnvironment("CC_TEST_LOCKSTEP_HOLD_BEFORE_TARGET");
		if (holdBeforeTarget && targetFrame == *holdBeforeTarget) {
			// The crash fixture waits for the host to receive the preceding frame, then kills this
			// process. No later input can enter the transport while that barrier is being observed.
			std::cout << "[lockstep-test] hold_before_target peer=" << static_cast<int>(m_Config.localPeerId)
			          << " produced=" << (targetFrame >= m_Config.inputDelayFrames ? targetFrame - m_Config.inputDelayFrames : 0) << " target=" << targetFrame << std::endl;
			std::this_thread::sleep_for(std::chrono::seconds(30));
			if (error) *error = "the lockstep crash fixture did not terminate its held peer";
			return false;
		}
		// A park already committed a canonical empty frame at or below this watermark on every peer, so this
		// tick's sample was dropped by the park, not lost to an error - and the next park has already moved the
		// live window on by the time the simulation reaches the frames the last one covered.
		if (targetFrame < m_Stats.nextFrame && targetFrame <= m_HighestParkEndFrame) {
			// The input goes with the park, but a command is an event: it rides this peer's next input instead.
			if (!commands.empty()) {
				auto& carried = m_ParkCarriedCommands[targetFrame];
				carried.insert(carried.end(), commands.begin(), commands.end());
			}
			return true;
		}
		if (targetFrame < m_Stats.nextFrame || targetFrame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew ||
		    m_LocalFrames.find(targetFrame) != m_LocalFrames.end() || m_LocalInputHistory.contains(targetFrame) ||
		    std::any_of(m_RecoveryOutgoing.begin(), m_RecoveryOutgoing.end(), [&](const auto& pending) { return pending.frame.senderPeerId == m_Config.localPeerId && pending.frame.targetFrame == targetFrame; })) {
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
		// Commands an older park emptied ride this input; a park frame itself carries its commands like any frame.
		std::map<uint64_t, std::vector<NetGameCommand>> carried = m_ParkCarriedCommands;
		{
			// Oldest first and within the packet's limits: only the newest binding survives, and one this tick carries itself supersedes it.
			const auto isBinding = [](const NetGameCommand& command) { return std::holds_alternative<NetGamePlayerBindings>(command.payload); };
			const bool freshBinding = std::any_of(commands.begin(), commands.end(), isBinding);
			const size_t own = static_cast<size_t>(std::count_if(commands.begin(), commands.end(), [&](const NetGameCommand& command) { return !isBinding(command); }));
			size_t room = NetLockstepCodec::c_MaxCommandsPerPacket - std::min<size_t>(NetLockstepCodec::c_MaxCommandsPerPacket, own);
			std::optional<NetGameCommand> binding;
			for (auto it = carried.begin(); it != carried.end();) {
				auto& pending = it->second;
				size_t taken = 0;
				for (; taken < pending.size(); ++taken) {
					if (isBinding(pending[taken])) {
						if (!freshBinding) binding = pending[taken];
						continue;
					}
					if (room == 0) break;
					packet.commands.push_back(pending[taken]);
					--room;
				}
				pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(taken));
				if (!pending.empty()) break;
				it = carried.erase(it);
			}
			if (binding) packet.commands.push_back(*binding);
			packet.commands.insert(packet.commands.end(), commands.begin(), commands.end());
		}
		for (NetGameCommand& command : packet.commands) {
			command.senderPeerId = m_Config.localPeerId;
		}
		packet.roundId = m_RoundId;
		// What the last frame could not hold goes first, minus anything this frame reads afresh: a newer
		// reading for the same sound would only overwrite it in the same commit.
		std::vector<NetSoundObservation> nextPendingObservations;
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
		}
		packet.observations.insert(packet.observations.end(), observations.begin(), observations.end());
		for (NetSoundObservation& observation : packet.observations) {
			observation.senderPeerId = m_Config.localPeerId;
		}
		std::vector<NetValueObservation> nextPendingValueObservations;
		packet.valueObservations.reserve(m_PendingValueObservations.size() + valueObservations.size());
		packet.valueObservations.insert(packet.valueObservations.end(), m_PendingValueObservations.begin(), m_PendingValueObservations.end());
		packet.valueObservations.insert(packet.valueObservations.end(), valueObservations.begin(), valueObservations.end());
		for (NetValueObservation& observation : packet.valueObservations) {
			observation.senderPeerId = m_Config.localPeerId;
		}
		size_t heldBack = 0;
		if (packet.observations.size() > NetLockstepCodec::c_MaxObservationsPerPacket) {
			heldBack = packet.observations.size() - NetLockstepCodec::c_MaxObservationsPerPacket;
			nextPendingObservations.assign(packet.observations.begin() + NetLockstepCodec::c_MaxObservationsPerPacket, packet.observations.end());
			packet.observations.resize(NetLockstepCodec::c_MaxObservationsPerPacket);
		}
		size_t valueHeldBack = 0;
		if (packet.valueObservations.size() > NetLockstepCodec::c_MaxObservationsPerPacket) {
			valueHeldBack = packet.valueObservations.size() - NetLockstepCodec::c_MaxObservationsPerPacket;
			nextPendingValueObservations.assign(packet.valueObservations.begin() + NetLockstepCodec::c_MaxObservationsPerPacket, packet.valueObservations.end());
			packet.valueObservations.resize(NetLockstepCodec::c_MaxObservationsPerPacket);
		}
		const bool recovery = !m_RecoveryOutgoing.empty();
		size_t observationsEncoded = packet.observations.size();
		size_t valueObservationsEncoded = packet.valueObservations.size();
		if (!recovery) {
			// Every sender spells its keys out again from the epoch, so a member admitted there reads
			// this frame with the empty table it starts with; the window stops at the epoch too.
			ApplyObservationEpoch(m_Config.localPeerId, packet.targetFrame);
			AttachFrameWindow(packet);
		}
		if (recovery) {
			if (!QueueRecoveredInput(packet, error)) return false;
		} else if (!SendPacket({packet}, m_Config.frameLane, error, &m_ObservationEncodeTables.Exactly(m_Config.localPeerId), &observationsEncoded, 0, &valueObservationsEncoded, &ObservationBlocksOf(m_Config.localPeerId, targetFrame))) {
			if (packet.priorWindow.empty()) {
				return false;
			}
			packet.priorWindow.clear();
			observationsEncoded = packet.observations.size();
			valueObservationsEncoded = packet.valueObservations.size();
			if (!SendPacket({packet}, m_Config.frameLane, error, &m_ObservationEncodeTables.Exactly(m_Config.localPeerId), &observationsEncoded, 0, &valueObservationsEncoded, &ObservationBlocksOf(m_Config.localPeerId, targetFrame))) {
				return false;
			}
		}
		if (!recovery) ++m_OwnFramesSent;
		m_PendingObservations = std::move(nextPendingObservations);
		m_PendingValueObservations = std::move(nextPendingValueObservations);
		// Every peer commits what the packet carried, so the leftovers ride the next frame with their own
		// keys and land one frame later on all of them alike.
		if (observationsEncoded < packet.observations.size()) {
			heldBack += packet.observations.size() - observationsEncoded;
			m_PendingObservations.insert(m_PendingObservations.begin(), packet.observations.begin() + static_cast<std::ptrdiff_t>(observationsEncoded), packet.observations.end());
			packet.observations.resize(observationsEncoded);
		}
		if (valueObservationsEncoded < packet.valueObservations.size()) {
			valueHeldBack += packet.valueObservations.size() - valueObservationsEncoded;
			m_PendingValueObservations.insert(m_PendingValueObservations.begin(), packet.valueObservations.begin() + static_cast<std::ptrdiff_t>(valueObservationsEncoded), packet.valueObservations.end());
			packet.valueObservations.resize(valueObservationsEncoded);
		}
		m_Stats.observationsCarried += heldBack;
		m_Stats.valueObservationsCarried += valueHeldBack;
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
		if (m_PendingValueObservations.size() > NetLockstepCodec::c_MaxCarriedObservations) {
			const size_t dropped = m_PendingValueObservations.size() - NetLockstepCodec::c_MaxCarriedObservations;
			m_DroppedValueObservations.insert(m_DroppedValueObservations.end(), m_PendingValueObservations.begin(), m_PendingValueObservations.begin() + static_cast<std::ptrdiff_t>(dropped));
			m_PendingValueObservations.erase(m_PendingValueObservations.begin(), m_PendingValueObservations.begin() + static_cast<std::ptrdiff_t>(dropped));
			if (m_Stats.valueObservationsDropped == 0) {
				std::cout << "[lockstep] more new value writes than the frame can carry; dropping the oldest held writes" << std::endl;
			}
			m_Stats.valueObservationsDropped += dropped;
		}
		m_ParkCarriedCommands = std::move(carried);
		if (recovery) return true;
		RememberLocalInput(packet);
		m_LocalFrames[targetFrame] = frames;
		if (!packet.commands.empty()) {
			m_LocalCommands[targetFrame] = packet.commands;
		}
		if (!packet.observations.empty()) {
			m_LocalObservations[targetFrame] = packet.observations;
		}
		if (!packet.valueObservations.empty()) {
			m_LocalValueObservations[targetFrame] = packet.valueObservations;
		}
		++m_Stats.framePacketsSent;
		m_Stats.localControllerFramesSent += frames.size();
		if (m_LastQueuedTargetFrame == std::numeric_limits<uint64_t>::max() || targetFrame > m_LastQueuedTargetFrame) {
			m_LastQueuedTargetFrame = targetFrame;
		}
		return true;
	}

	std::vector<NetSoundObservation> NetLockstepCoordinator::TakeDroppedObservations() {
		NET_PLANE_CHECK();
		return std::exchange(m_DroppedObservations, {});
	}

	std::vector<NetValueObservation> NetLockstepCoordinator::TakeDroppedValueObservations() {
		NET_PLANE_CHECK();
		return std::exchange(m_DroppedValueObservations, {});
	}

	bool NetLockstepCoordinator::SubmitLocalChecksum(uint64_t frame, const std::array<uint8_t, 32>& hash, std::string* error, const std::map<uint8_t, uint64_t>& appliedCommands) {
		NET_PLANE_CHECK();
		if (m_State != NetLockstepState::Running) {
			return true;
		}
		m_LocalChecksums[frame] = hash;
		++m_Stats.checksumSubmissions;
		NetLockstepChecksum packet;
		packet.senderPeerId = m_Config.localPeerId;
		packet.frame = frame;
		packet.hash = hash;
		packet.roundId = m_RoundId;
		if (m_Config.localPeerId == GetHostPeerId()) {
			packet.appliedCommands = appliedCommands;
			m_AuthoritativeCommandAcks = appliedCommands;
		}
		if (!SendPacket({packet}, NetTransportLane::ControlReliable, error)) {
			return false;
		}
		++m_Stats.checksumSends;
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
		if (checksum.senderPeerId == GetHostPeerId()) {
			for (const auto& [peer, sequence]: checksum.appliedCommands) {
				m_AuthoritativeCommandAcks[peer] = std::max(m_AuthoritativeCommandAcks[peer], sequence);
			}
		}
		RelayToOtherRemotes({checksum}, checksum.senderPeerId);
		CompareChecksums(checksum.frame);
	}

	void NetLockstepCoordinator::RememberLocalInput(const NetLockstepFrame& frame) {
		NetLockstepFrame& kept = m_LocalInputHistory[frame.targetFrame];
		kept = frame;
		// The history holds the tick itself; the copies it rode out with belong to the packet, not to it.
		kept.priorWindow.clear();
		const uint64_t last = m_LocalInputHistory.rbegin()->first;
		while (!m_LocalInputHistory.empty() && last - m_LocalInputHistory.begin()->first > NetLockstepCodec::c_MaxFutureFrameSkew) {
			m_LocalInputHistory.erase(m_LocalInputHistory.begin());
		}
		if (m_LastQueuedTargetFrame == UINT64_MAX || frame.targetFrame > m_LastQueuedTargetFrame) m_LastQueuedTargetFrame = frame.targetFrame;
	}

	void NetLockstepCoordinator::AdvertiseFrameWindow() {
		if (!m_Transport || ConfiguredWindowTicks() <= 1) {
			return;
		}
		if (m_RelayHost && !FrameWindowAllRemotesAdvertised()) {
			return;
		}
		NetLockstepAck ack;
		ack.senderPeerId = m_Config.localPeerId;
		ack.highestContiguousFrame = m_Stats.nextFrame;
		ack.receivedMask = NetLockstepCodec::c_FrameWindowCapabilityMask;
		std::string ignored;
		(void)SendPacket({ack}, NetTransportLane::ControlReliable, &ignored);
	}

	void NetLockstepCoordinator::HandleAck(const NetLockstepAck& ack, NetPeerId fromTransport) {
		if (!IsKnownRemotePeer(ack.senderPeerId) || !SenderOwnsTransport(ack.senderPeerId, fromTransport)) {
			return;
		}
		if (ack.receivedMask == NetLockstepCodec::c_InputAcceptedMask) {
			// Kept as a wire-compatible no-op; local input commits without a per-frame round trip.
			return;
		}
		if (ack.receivedMask & NetLockstepCodec::c_FrameWindowCapabilityMask) {
			m_RemoteFrameWindow.insert(ack.senderPeerId);
			if (m_RelayHost) {
				AdvertiseFrameWindow();
			}
		}
		if ((ack.receivedMask & NetLockstepCodec::c_FrameResendRequestMask) != 0 && IsRunning()) {
			const uint8_t named = static_cast<uint8_t>(ack.receivedMask & 0xFFU);
			if (named == m_Config.localPeerId) (void)ResendOwnFramesFrom(ack.senderPeerId, ack.highestContiguousFrame);
			else if (m_RelayHost && named != ack.senderPeerId && IsKnownRemotePeer(named)) (void)ResendRelayedFramesFrom(ack.senderPeerId, named, ack.highestContiguousFrame);
		}
	}

	void NetLockstepCoordinator::RequestMissingFrames(uint8_t senderPeerId, uint64_t frame, uint64_t nowMs, uint64_t waitedMs) {
		if (m_Config.frameLane == NetTransportLane::ControlReliable || m_Playback || !m_Transport) return;
		// A client hears every other client through the host, which re-serves what it relayed.
		const uint8_t via = m_RemoteTransports.contains(senderPeerId) ? senderPeerId : !m_RelayHost && m_RemoteTransports.contains(GetHostPeerId()) ? GetHostPeerId() : 0;
		if (via == 0) return;
		// A tick is lost once the sender has sent past it, or once the link it comes over has had its round trip: one
		// still in flight on a long link arrives by itself. The same tick is asked for again only a round trip later.
		const auto& link = m_Stats.peers[via];
		const uint64_t tickMs = static_cast<uint64_t>(std::max(1.0, std::ceil(m_Config.simTickMs)));
		uint64_t spacingMs = std::max<uint64_t>(tickMs, static_cast<uint64_t>(link.pingMs) + link.jitterMs);
		// A tick still inside the lateness this sender's ticks usually land with is in flight, not lost: a relayed one
		// crosses two links and waits a relay pass, so neither the sender having sent past it nor one tick of waiting says more.
		uint64_t arrivalMs = 0;
		if (const auto lateness = m_ArrivalLateness.find(senderPeerId); lateness != m_ArrivalLateness.end() && !lateness->second.empty()) {
			std::vector<uint32_t> sorted(lateness->second.begin(), lateness->second.end());
			std::sort(sorted.begin(), sorted.end());
			arrivalMs = sorted[std::min(sorted.size() - 1, sorted.size() * 95 / 100)] + tickMs;
		}
		spacingMs = std::max(spacingMs, arrivalMs);
		if (waitedMs < arrivalMs || (m_Stats.peers[senderPeerId].highestTargetFrame <= frame && waitedMs < spacingMs)) return;
		auto& [askedFrame, askedAtMs] = m_ResendRequests[senderPeerId];
		if (askedFrame == frame && nowMs >= askedAtMs && nowMs - askedAtMs < spacingMs) return;
		if (askedFrame != frame)
			std::cout << "[lockstep-recv] asked peer " << static_cast<int>(via) << " to resend frame=" << frame << " of peer " << static_cast<int>(senderPeerId) << " after " << waitedMs
			          << " ms; highest heard=" << m_Stats.peers[senderPeerId].highestTargetFrame << std::endl;
		askedFrame = frame;
		askedAtMs = nowMs;
		NetLockstepAck request;
		request.senderPeerId = m_Config.localPeerId;
		request.highestContiguousFrame = frame;
		request.receivedMask = NetLockstepCodec::c_FrameResendRequestMask | senderPeerId;
		std::string ignored;
		if (SendPacket({request}, NetTransportLane::ControlReliable, &ignored, nullptr, nullptr, via)) ++m_Stats.frameResendRequests;
	}

	size_t NetLockstepCoordinator::ResendRelayedFramesFrom(uint8_t requesterPeerId, uint8_t senderPeerId, uint64_t fromFrame) {
		if (!m_RelayHost || m_Config.frameLane == NetTransportLane::ControlReliable || !m_RemoteTransports.contains(requesterPeerId)) return 0;
		const auto kept = m_RelayedTickFrames.find(senderPeerId);
		if (kept == m_RelayedTickFrames.end()) return 0;
		NetLockstepObservationBlocks& blocks = m_ObservationBlocks[senderPeerId];
		size_t resent = 0;
		uint64_t last = fromFrame;
		const auto firstKept = kept->second.lower_bound(fromFrame);
		if (firstKept == kept->second.end() || firstKept->first != fromFrame)
			std::cout << "[lockstep] holds no relayed tick " << fromFrame << " of peer " << static_cast<int>(senderPeerId) << " for peer " << static_cast<int>(requesterPeerId)
			          << "; first kept=" << (firstKept == kept->second.end() ? -1 : static_cast<int64_t>(firstKept->first)) << " heard through=" << m_Stats.peers[senderPeerId].highestTargetFrame << std::endl;
		for (auto it = firstKept; it != kept->second.end() && it->first < fromFrame + NetLockstepCodec::c_MaxWindowTicks; ++it) {
			// Repeated only with the bytes it was first forwarded with, so the requester binds what the others bound.
			if (!blocks.contains(it->first)) break;
			std::string error;
			if (!SendPacket({it->second}, NetTransportLane::ControlReliable, &error, &m_ObservationEncodeTables.Exactly(senderPeerId), nullptr, requesterPeerId, nullptr, &blocks)) break;
			last = it->first;
			++resent;
		}
		m_Stats.framesResent += static_cast<uint32_t>(resent);
		if (resent > 0)
			std::cout << "[lockstep] resent " << resent << " relayed ticks " << fromFrame << ".." << last << " of peer " << static_cast<int>(senderPeerId)
			          << " to peer " << static_cast<int>(requesterPeerId) << " on the reliable lane" << std::endl;
		return resent;
	}

	size_t NetLockstepCoordinator::SendReturnerTheRoundFrom(uint8_t peerId, uint64_t fromFrame) {
		NET_PLANE_CHECK();
		if (!m_RelayHost || m_Config.localPeerId != GetHostPeerId() || !m_RemoteTransports.contains(peerId)) return 0;
		// The frames already sent for the reclaim frame on went only to the links bound then; the returner asks for none of them
		// until it misses them, a round trip too late for its first input.
		size_t sent = ResendOwnFramesFrom(peerId, fromFrame);
		for (uint8_t sender: m_RemotePeerIds) if (sender != peerId) sent += ResendRelayedFramesFrom(peerId, sender, fromFrame);
		return sent;
	}

	size_t NetLockstepCoordinator::ResendOwnFramesFrom(uint8_t requesterPeerId, uint64_t fromFrame) {
		if (m_Config.frameLane == NetTransportLane::ControlReliable || !m_RemoteTransports.contains(requesterPeerId) ||
		    m_LastQueuedTargetFrame == UINT64_MAX || fromFrame > m_LastQueuedTargetFrame) return 0;
		NetLockstepObservationBlocks& blocks = ObservationBlocksOf(m_Config.localPeerId, m_LastQueuedTargetFrame);
		size_t resent = 0;
		// A request names the oldest tick the peer waits on; the ticks after it went the same way, so they go too.
		for (uint64_t target = fromFrame; target <= m_LastQueuedTargetFrame && target < fromFrame + NetLockstepCodec::c_MaxWindowTicks; ++target) {
			NetLockstepFrame own;
			if (!FindLocalInput(target, own)) continue;
			// A tick is repeated only with the bytes it first went out with; re-encoding it against a table that has
			// moved on would bind keys the requester never saw.
			if (!blocks.contains(target)) break;
			own.priorWindow.clear();
			std::string error;
			if (!SendPacket({own}, NetTransportLane::ControlReliable, &error, &m_ObservationEncodeTables.Exactly(m_Config.localPeerId), nullptr, requesterPeerId, nullptr, &blocks)) break;
			++resent;
		}
		m_Stats.framesResent += static_cast<uint32_t>(resent);
		if (resent > 0)
			std::cout << "[lockstep] resent " << resent << " ticks " << fromFrame << ".." << m_LastQueuedTargetFrame << " to peer "
			          << static_cast<int>(requesterPeerId) << " on the reliable lane" << std::endl;
		return resent;
	}

	void NetLockstepCoordinator::AcknowledgeAcceptedInput(uint8_t peerId, uint64_t frame) {
		(void)peerId;
		(void)frame;
	}

	uint8_t NetLockstepCoordinator::ConfiguredWindowTicks() const {
		uint32_t ticks = m_Config.frameRedundancyTicks;
		if (ticks == 0) {
			ticks = 4;
		}
		// On the unreliable lane the window is the only repair: it reaches back one round trip of the worst link and its
		// jitter, and never fewer than eight ticks.
		if (m_Config.frameLane != NetTransportLane::ControlReliable) {
			uint32_t linkMs = 0;
			for (const auto& [peer, stats]: m_Stats.peers) {
				if (peer == m_Config.localPeerId) continue;
				uint32_t ping = stats.pingMs;
				if (const auto estimate = m_DelayEstimators.find(peer); estimate != m_DelayEstimators.end()) ping = std::max(ping, estimate->second.P95Ms());
				linkMs = std::max(linkMs, ping + stats.jitterMs);
			}
			const uint32_t cover = std::isfinite(m_Config.simTickMs) && m_Config.simTickMs > 0 ? static_cast<uint32_t>(std::ceil(linkMs / m_Config.simTickMs)) + 1 : 0;
			ticks = std::max({ticks, uint32_t(8), cover});
		}
		return static_cast<uint8_t>(std::min<uint32_t>(ticks, NetLockstepCodec::c_MaxWindowTicks));
	}

	bool NetLockstepCoordinator::FrameWindowAllRemotesAdvertised() const {
		if (m_RemotePeerIds.empty()) {
			return false;
		}
		for (uint8_t peerId : m_RemotePeerIds) {
			if (!m_RemoteFrameWindow.contains(peerId)) {
				return false;
			}
		}
		return true;
	}

	bool NetLockstepCoordinator::FrameWindowAgreedFor(uint8_t peerId) const {
		// Every peer on the unreliable lane reads windows: it is how that lane is repaired.
		return ConfiguredWindowTicks() > 1 && (m_Config.frameLane != NetTransportLane::ControlReliable || m_RemoteFrameWindow.contains(peerId));
	}

	bool NetLockstepCoordinator::FrameWindowAgreed() const {
		NET_PLANE_CHECK();
		return ConfiguredWindowTicks() > 1 && FrameWindowAllRemotesAdvertised();
	}

	void NetLockstepCoordinator::AttachFrameWindow(NetLockstepFrame& packet) const {
		packet.priorWindow.clear();
		if (ConfiguredWindowTicks() <= 1) {
			return;
		}
		bool anyAdvertised = m_Config.frameLane != NetTransportLane::ControlReliable;
		for (uint8_t peerId : m_RemotePeerIds) {
			if (m_RemoteFrameWindow.contains(peerId)) {
				anyAdvertised = true;
				break;
			}
		}
		if (!anyAdvertised) {
			return;
		}
		const uint8_t window = ConfiguredWindowTicks();
		std::vector<NetLockstepFrame> older;
		for (uint8_t back = 1; back < window; ++back) {
			if (packet.targetFrame < back) {
				break;
			}
			NetLockstepFrame copy;
			if (!FindLocalInput(packet.targetFrame - back, copy)) {
				break;
			}
			copy.priorWindow.clear();
			older.push_back(std::move(copy));
		}
		if (older.empty()) {
			return;
		}
		std::reverse(older.begin(), older.end());
		packet.priorWindow = std::move(older);
	}

	uint64_t NetLockstepCoordinator::ObservationBindingsSpelled(uint8_t senderPeerId) const {
		NET_PLANE_CHECK();
		const auto found = m_ObservationEncodeTables.bySender.find(senderPeerId);
		return found == m_ObservationEncodeTables.bySender.end() ? 0 : found->second.BindingCount();
	}

	NetLockstepObservationBlocks& NetLockstepCoordinator::ObservationBlocksOf(uint8_t senderPeerId, uint64_t newestTargetFrame) {
		NetLockstepObservationBlocks& blocks = m_ObservationBlocks[senderPeerId];
		// Only the ticks a window can still name are worth keeping.
		const uint64_t keepFrom = newestTargetFrame > NetLockstepCodec::c_MaxWindowTicks ? newestTargetFrame - NetLockstepCodec::c_MaxWindowTicks : 0;
		blocks.erase(blocks.begin(), blocks.lower_bound(keepFrom));
		return blocks;
	}

	void NetLockstepCoordinator::AcceptRemoteTick(const NetLockstepFrame& frame, uint64_t nowMs, bool windowCopy) {
		if (IsPeerGoneAtFrame(frame.senderPeerId, frame.targetFrame)) return;
		NetLockstepPeerStats& peerStats = m_Stats.peers[frame.senderPeerId];
		if (frame.targetFrame > peerStats.highestTargetFrame) { peerStats.highestTargetFrame = frame.targetFrame; peerStats.lastProgressMs = nowMs; }
		if (frame.targetFrame < EffectiveStartOf(frame.senderPeerId)) {
			// A member admitted mid-round reads the window copies of the ticks before its own start.
			// They are ticks it never owed, not a broken build; only a sender's own new tick can be one.
			if (windowCopy || m_ReclaimTransactions.contains(frame.senderPeerId) ||
			    (IsPersistentWorldRound() && m_Config.joinsRunningRound && frame.targetFrame < m_Config.startFrame)) {
				++m_Stats.windowCopiesSkipped;
				++peerStats.windowCopiesSkipped;
				return;
			}
			Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "lockstep frame targets the sender's delay window");
			return;
		}
		if (frame.targetFrame < m_Stats.nextFrame) {
			AcknowledgeAcceptedInput(frame.senderPeerId, frame.targetFrame);
			if (windowCopy) {
				++m_Stats.windowCopiesSkipped;
				++peerStats.windowCopiesSkipped;
			} else {
				++m_Stats.duplicateFrames;
				++peerStats.duplicateFrames;
				if (m_ConsumerWaitingFrame) ++m_WaitDuplicates;
			}
			return;
		}
		if (frame.targetFrame > m_Stats.nextFrame + NetLockstepCodec::c_MaxFutureFrameSkew) {
			++m_Stats.futureFrameDrops;
			++peerStats.futureFrameDrops;
			std::cout << "[lockstep] dropped a frame targeting " << frame.targetFrame << " far past the committed frame " << m_Stats.nextFrame << std::endl;
			return;
		}
		auto& peerFrames = m_RemoteFrames[frame.targetFrame];
		if (peerFrames.find(frame.senderPeerId) != peerFrames.end()) {
			AcknowledgeAcceptedInput(frame.senderPeerId, frame.targetFrame);
			if (windowCopy) {
				++m_Stats.windowCopiesSkipped;
				++peerStats.windowCopiesSkipped;
			} else {
				++m_Stats.duplicateFrames;
				++peerStats.duplicateFrames;
				if (m_ConsumerWaitingFrame) ++m_WaitDuplicates;
			}
			return;
		}
		if (frame.targetFrame > m_Stats.nextFrame) {
			++m_Stats.outOfOrderFrames;
			++peerStats.outOfOrderFrames;
		}
		m_Stats.remoteControllerFramesReceived += frame.frames.size();
		peerStats.controllerFramesReceived += frame.frames.size();
		if (windowCopy) {
			++m_Stats.windowCopiesApplied;
			++peerStats.windowCopiesApplied;
		}
		peerFrames[frame.senderPeerId] = frame.frames;
		peerStats.acceptedThroughFrame = std::max(peerStats.acceptedThroughFrame, frame.targetFrame);
		// A returning seat's first frame after its reclaim is when its stream is back; named once per return, on the shared clock.
		if (const auto back = m_ReclaimTransactions.find(frame.senderPeerId); back != m_ReclaimTransactions.end() && frame.targetFrame >= back->second.activationFrame &&
		    m_ReturnerFirstFrameNamed.insert(frame.senderPeerId).second)
			std::cout << "[lockstep] first frame of returning peer " << static_cast<int>(frame.senderPeerId) << " accepted: target=" << frame.targetFrame << " next=" << m_Stats.nextFrame
			          << " reclaim=" << back->second.activationFrame << " window=" << windowCopy << " clock=" << NetLockstepSharedClockMs() << std::endl;
		if (!windowCopy && m_Config.adaptiveInputDelay && m_Config.localPeerId == GetHostPeerId()) {
			const uint64_t simNext = m_LastDeliveredFrame ? *m_LastDeliveredFrame + 1 : m_Config.startFrame;
			auto& leads = m_ArrivalLeads[frame.senderPeerId];
			leads.push_back({nowMs, frame.targetFrame, frame.targetFrame > simNext ? frame.targetFrame - simNext : 0});
			while (!leads.empty() && nowMs - leads.front().ms > 2 * NetInputDelayEstimator::c_WindowMs) leads.pop_front();
		}
		static const auto observeTarget = TestFrameFromEnvironment("CC_TEST_LOCKSTEP_OBSERVE_TARGET");
		if (observeTarget && frame.targetFrame == *observeTarget) {
			std::cout << "[lockstep-test] received peer=" << static_cast<int>(frame.senderPeerId)
			          << " target=" << frame.targetFrame << std::endl;
		}
		// A sender's ramp ends with its first frame; the wait it was allowed while filling its pipeline
		// is not lateness to charge against the next one, so the missing-frame deadline starts again.
		if (m_PeersPlayedThisRound.insert(frame.senderPeerId).second) m_FirstMissingFrame.reset();
		if (!frame.commands.empty()) {
			m_RemoteCommands[frame.targetFrame][frame.senderPeerId] = frame.commands;
		}
		if (!frame.observations.empty()) {
			m_RemoteObservations[frame.targetFrame][frame.senderPeerId] = frame.observations;
		}
		if (!frame.valueObservations.empty()) {
			m_RemoteValueObservations[frame.targetFrame][frame.senderPeerId] = frame.valueObservations;
		}
		AcknowledgeAcceptedInput(frame.senderPeerId, frame.targetFrame);
		AdvanceReadyFrames(nowMs);
	}

	bool NetLockstepCoordinator::FindLocalInput(uint64_t targetFrame, NetLockstepFrame& out) const {
		if (const auto history = m_LocalInputHistory.find(targetFrame); history != m_LocalInputHistory.end()) { out = history->second; return true; }
		for (const auto& pending: m_RecoveryOutgoing) {
			if (pending.frame.senderPeerId == m_Config.localPeerId && pending.frame.targetFrame == targetFrame) { out = pending.frame; return true; }
		}
		if (const auto frames = m_LocalFrames.find(targetFrame); frames != m_LocalFrames.end()) {
			out = {};
			out.senderPeerId = m_Config.localPeerId; out.roundId = m_RoundId; out.targetFrame = targetFrame; out.frames = frames->second;
			if (const auto commands = m_LocalCommands.find(targetFrame); commands != m_LocalCommands.end()) out.commands = commands->second;
			if (const auto observations = m_LocalObservations.find(targetFrame); observations != m_LocalObservations.end()) out.observations = observations->second;
			if (const auto values = m_LocalValueObservations.find(targetFrame); values != m_LocalValueObservations.end()) out.valueObservations = values->second;
			return true;
		}
		for (const auto& ready: m_ReadyFrames) {
			if (ready.frame != targetFrame || !ready.hasLocalInput) continue;
			out = {};
			out.senderPeerId = m_Config.localPeerId; out.roundId = m_RoundId; out.targetFrame = targetFrame;
			out.frames = ready.localFrames; out.commands = ready.localCommands; out.observations = ready.localObservations;
			out.valueObservations = ready.localValueObservations;
			return true;
		}
		return false;
	}

	bool NetLockstepCoordinator::RetainRecoveryInput(const NetLockstepFrame& frame, std::vector<uint8_t> bytes, bool commitLocal, std::string* error) {
		for (const auto& pending: m_RecoveryOutgoing) {
			if (pending.frame.senderPeerId == frame.senderPeerId && pending.frame.targetFrame == frame.targetFrame) {
				if (pending.bytes == bytes) return true;
				if (error) *error = "conflicting recovery input retry";
				return false;
			}
		}
		if (m_RecoveryOutgoing.size() >= (NetLockstepCodec::c_MaxFutureFrameSkew + 1) * NetLockstepCodec::c_MaxPeerCount) {
			if (error) *error = "recovery input backlog is full";
			return false;
		}
		RecoveryOutgoing pending;
		pending.frame = frame;
		pending.bytes = std::move(bytes);
		pending.commitLocal = commitLocal;
		for (const auto& [peer, transport]: m_RemoteTransports) {
			if (peer != frame.senderPeerId && !m_PeerLeaveFrames.contains(peer)) pending.nextOffsets.emplace(peer, 0);
		}
		m_RecoveryOutgoing.push_back(std::move(pending));
		UpdateRelayBacklogBytes();
		if (frame.senderPeerId == m_Config.localPeerId) RememberLocalInput(frame);
		return true;
	}

	bool NetLockstepCoordinator::QueueRecoveredInput(const NetLockstepFrame& frame, std::string* error) {
		NET_PLANE_CHECK();
		if (!IsRunning() || m_RoundId == 0 || frame.roundId != m_RoundId || frame.senderPeerId != m_Config.localPeerId) {
			if (error) *error = "invalid recovery input state or sender";
			return false;
		}
		std::vector<uint8_t> bytes;
		NetLockstepError validation;
		NetLockstepFrame decoded;
		if (!NetLockstepCodec::EncodeRecoveryInput(frame, bytes, &validation) || !NetLockstepCodec::DecodeRecoveryInput(bytes, decoded, &validation)) {
			if (error) *error = "invalid recovery input: " + validation.message;
			return false;
		}
		NetLockstepFrame previous;
		if (FindLocalInput(frame.targetFrame, previous)) {
			std::vector<uint8_t> prior;
			if (NetLockstepCodec::EncodeRecoveryInput(previous, prior) && prior == bytes) return true;
			if (error) *error = "conflicting recovery input retry";
			return false;
		}
		if (frame.targetFrame < m_Stats.nextFrame || frame.targetFrame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew ||
		    frame.targetFrame < EffectiveStartOf(m_Config.localPeerId)) {
			if (error) *error = "recovery input target is outside the pending window";
			return false;
		}
		if (!RetainRecoveryInput(frame, std::move(bytes), true, error)) return false;
		FlushRecoveryInputs();
		return true;
	}

	bool NetLockstepCoordinator::PrimeResyncInputs(const std::vector<NetLockstepFrame>& batches, std::string* error) {
		NET_PLANE_CHECK();
		if (!IsRunning() || !m_Config.resumeFromSnapshot || m_RoundId == 0 || batches.size() != m_Config.inputDelayFrames) {
			if (error) *error = "invalid resync input priming state or batch count";
			return false;
		}
		std::vector<std::vector<uint8_t>> encoded(batches.size());
		for (size_t index = 0; index < batches.size(); ++index) {
			const auto& frame = batches[index];
			NetLockstepError validation;
			NetLockstepFrame decoded;
			if (frame.senderPeerId != m_Config.localPeerId || frame.roundId != m_RoundId || m_Config.startFrame > UINT64_MAX - index ||
			    frame.targetFrame != m_Config.startFrame + index || !NetLockstepCodec::EncodeRecoveryInput(frame, encoded[index], &validation) ||
			    !NetLockstepCodec::DecodeRecoveryInput(encoded[index], decoded, &validation)) {
				if (error) *error = "invalid resync input batch: " + validation.message;
				return false;
			}
		}
		if (m_ResyncPrimed) {
			if (encoded == m_ResyncPrimeInputs) return true;
			if (error) *error = "conflicting resync input priming retry";
			return false;
		}
		std::vector<bool> installed(batches.size(), false);
		const auto agreedStart = m_PeerEffectiveStart.find(m_Config.localPeerId);
		for (size_t index = 0; index < batches.size(); ++index) {
			const auto& frame = batches[index];
			// The agreed first frame can sit past the restored start: every peer commits nothing below it, so that
			// input is dropped here exactly as production drops it.
			if (agreedStart != m_PeerEffectiveStart.end() && frame.targetFrame < agreedStart->second) {
				installed[index] = true;
				continue;
			}
			if (frame.targetFrame < m_Stats.nextFrame || frame.targetFrame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew) {
				if (error) *error = "resync input batch targets existing or applied input";
				return false;
			}
			NetLockstepFrame prior;
			if (FindLocalInput(frame.targetFrame, prior)) {
				std::vector<uint8_t> priorBytes;
				if (!NetLockstepCodec::EncodeRecoveryInput(prior, priorBytes) || priorBytes != encoded[index]) {
					if (error) *error = "conflicting installed resync input";
					return false;
				}
				installed[index] = true;
			}
		}
		if (m_RecoveryOutgoing.size() + std::count(installed.begin(), installed.end(), false) > (NetLockstepCodec::c_MaxFutureFrameSkew + 1) * NetLockstepCodec::c_MaxPeerCount) {
			if (error) *error = "recovery input backlog is full";
			return false;
		}
		for (size_t index = 0; index < batches.size(); ++index) {
			if (!installed[index] && !RetainRecoveryInput(batches[index], encoded[index], true, error)) return false;
		}
		m_ResyncPrimeInputs = std::move(encoded);
		m_ResyncPrimed = true;
		m_ResumeAdmissionPending = false;
		FlushRecoveryInputs();
		return true;
	}

	bool NetLockstepCoordinator::InstallResyncInputs(const std::vector<NetLockstepFrame>& authoritativeInputs, std::string* error) {
		NET_PLANE_CHECK();
		if (!m_Config.resumeFromSnapshot || m_ResyncPrimed || m_RoundId == 0 || HasCommittedAFrame() ||
		    (m_State != NetLockstepState::Running && m_State != NetLockstepState::WaitingForStart) ||
		    !m_LocalInputHistory.empty() || !m_RecoveryOutgoing.empty() || !m_InstalledResyncTargets.empty()) {
			if (error) *error = "resync input installation requires an untouched restore";
			return false;
		}
		if (authoritativeInputs.size() > (NetLockstepCodec::c_MaxFutureFrameSkew + 1) * NetLockstepCodec::c_MaxPeerCount) {
			if (error) *error = "too many authoritative recovery inputs";
			return false;
		}
		std::set<std::pair<uint64_t, uint8_t>> targets;
		for (const auto& input: authoritativeInputs) {
			std::vector<uint8_t> bytes;
			NetLockstepFrame decoded;
			NetLockstepError validation;
			if ((input.senderPeerId != m_Config.localPeerId && !IsKnownRemotePeer(input.senderPeerId)) ||
				input.roundId != m_RoundId || input.targetFrame < m_Config.startFrame || input.targetFrame - m_Config.startFrame > NetLockstepCodec::c_MaxFutureFrameSkew ||
			    !targets.emplace(input.targetFrame, input.senderPeerId).second || !NetLockstepCodec::EncodeRecoveryInput(input, bytes, &validation) ||
			    !NetLockstepCodec::DecodeRecoveryInput(bytes, decoded, &validation)) {
				if (error) *error = "invalid authoritative recovery input: " + validation.message;
				return false;
			}
			for (const auto& command: input.commands) {
				const auto* delay = std::get_if<NetGameInputDelay>(&command.payload);
				const auto* hold = std::get_if<NetGameSeatHold>(&command.payload);
				if ((delay || hold) && (input.senderPeerId != GetHostPeerId() || (delay ? delay->peerId : hold->peerId) > m_Config.peerCount)) {
					if (error) *error = "recovered timing event has invalid authority";
					return false;
				}
			}
			if (const auto existing = m_RemoteFrames.find(input.targetFrame); existing != m_RemoteFrames.end() && existing->second.contains(input.senderPeerId)) {
				NetLockstepFrame prior;
				prior.senderPeerId = input.senderPeerId; prior.roundId = m_RoundId; prior.targetFrame = input.targetFrame;
				prior.frames = existing->second.at(input.senderPeerId);
				if (const auto commands = m_RemoteCommands.find(input.targetFrame); commands != m_RemoteCommands.end() && commands->second.contains(input.senderPeerId)) prior.commands = commands->second.at(input.senderPeerId);
				if (const auto observations = m_RemoteObservations.find(input.targetFrame); observations != m_RemoteObservations.end() && observations->second.contains(input.senderPeerId)) prior.observations = observations->second.at(input.senderPeerId);
				if (const auto values = m_RemoteValueObservations.find(input.targetFrame); values != m_RemoteValueObservations.end() && values->second.contains(input.senderPeerId)) prior.valueObservations = values->second.at(input.senderPeerId);
				std::vector<uint8_t> priorBytes;
				if (!NetLockstepCodec::EncodeRecoveryInput(prior, priorBytes) || priorBytes != bytes) {
					if (error) *error = "conflicting authoritative recovery input";
					return false;
				}
			}
		}
		for (const auto& input: authoritativeInputs) {
			for (const auto& command: input.commands) {
				if (const auto* delay = std::get_if<NetGameInputDelay>(&command.payload)) m_DelayChanges[delay->peerId][input.targetFrame] = delay->frames;
				if (const auto* hold = std::get_if<NetGameSeatHold>(&command.payload)) {
					m_AiHeldSeats[hold->peerId] = input.targetFrame;
					m_HoldTransactions[hold->peerId] = *hold;
					if (hold->peerId != GetHostPeerId()) {
						m_PeerLeaveFrames[hold->peerId] = input.targetFrame;
						m_DroppedSeats.insert(hold->peerId);
					}
					m_DroppedSeatResolutions[hold->peerId] = NetLockstepHoldResolution::Substituted;
				}
			}
			if (input.senderPeerId == m_Config.localPeerId) {
				RememberLocalInput(input);
				m_LocalFrames[input.targetFrame] = input.frames;
				if (!input.commands.empty()) m_LocalCommands[input.targetFrame] = input.commands;
				if (!input.observations.empty()) m_LocalObservations[input.targetFrame] = input.observations;
				if (!input.valueObservations.empty()) m_LocalValueObservations[input.targetFrame] = input.valueObservations;
			} else {
				m_RemoteFrames[input.targetFrame][input.senderPeerId] = input.frames;
				if (!input.commands.empty()) m_RemoteCommands[input.targetFrame][input.senderPeerId] = input.commands;
				if (!input.observations.empty()) m_RemoteObservations[input.targetFrame][input.senderPeerId] = input.observations;
				if (!input.valueObservations.empty()) m_RemoteValueObservations[input.targetFrame][input.senderPeerId] = input.valueObservations;
			}
		}
		m_InstalledResyncTargets = std::move(targets);
		return true;
	}

	void NetLockstepCoordinator::FlushRecoveryInputs(uint64_t nowMs) {
		if (!m_Transport || (m_State != NetLockstepState::Running && m_State != NetLockstepState::WaitingForStart)) return;
		size_t budget = 64;
		while (!m_RecoveryOutgoing.empty() && budget > 0) {
			auto& pending = m_RecoveryOutgoing.front();
			bool complete = true;
			for (auto& [peer, offset]: pending.nextOffsets) {
				const auto transport = m_RemoteTransports.find(peer);
				if (transport == m_RemoteTransports.end() || m_PeerLeaveFrames.contains(peer)) {
					offset = pending.bytes.size();
					continue;
				}
				while (offset < pending.bytes.size() && budget > 0) {
					if (m_RelayBacklog.contains(peer)) break;
					const size_t count = std::min(NetLockstepCodec::c_MaxRecoveryChunkBytes, pending.bytes.size() - offset);
					NetLockstepRecoveryChunk chunk;
					chunk.senderPeerId = pending.frame.senderPeerId;
					chunk.sessionId = m_Config.sessionId;
					chunk.roundId = pending.frame.roundId;
					chunk.targetFrame = pending.frame.targetFrame;
					chunk.totalBytes = static_cast<uint32_t>(pending.bytes.size());
					chunk.offset = static_cast<uint32_t>(offset);
					chunk.bytes.assign(pending.bytes.begin() + offset, pending.bytes.begin() + offset + count);
					std::vector<uint8_t> wire;
					NetLockstepError validation;
					if (!NetLockstepCodec::Encode({chunk}, wire, &validation)) {
						Fail(NetLockstepStopReason::InternalError, pending.frame.targetFrame, validation.message);
						return;
					}
					std::string sendError;
					bool congested = false;
					--budget;
					if (!m_Transport->Send(transport->second, NetTransportLane::ControlReliable, wire, &sendError, &congested)) {
						if (m_RelayHost) {
							NoteRelayError(peer, sendError);
							NoteRelayRefusal(peer, congested);
							if (nowMs != UINT64_MAX) {
								const uint64_t since = m_RecoveryBlockedSinceMs.emplace(peer, nowMs).first->second;
								if (m_Config.timeoutMs > 0 && nowMs >= since && nowMs - since >= PeerSilenceLeaveMs()) {
									const uint64_t held = nowMs - since;
									if (congested) {
										if (m_HeldForCongestion.insert(peer).second) ++m_Stats.relayCongestionHolds;
										m_Stats.longestCongestionHoldMs = std::max(m_Stats.longestCongestionHoldMs, held);
										m_Stats.peers[peer].longestCongestionHoldMs = std::max(m_Stats.peers[peer].longestCongestionHoldMs, held);
									}
									if (!congested || held >= CongestionHoldLeaveMs()) m_UnreachablePeers.insert(peer);
								}
							}
						}
						break;
					}
					if (m_RelayHost) {
						CountRelaySent(m_Stats.peers[peer], wire.size());
						m_RecoveryBlockedSinceMs.erase(peer);
						if (!m_RelayBacklog.contains(peer)) { m_CongestedPeers.erase(peer); m_HeldForCongestion.erase(peer); }
					}
					offset += count;
				}
				if (offset < pending.bytes.size()) complete = false;
			}
			if (!complete) { UpdateRelayBacklogBytes(); return; }
			if (pending.commitLocal) {
				m_LocalFrames[pending.frame.targetFrame] = pending.frame.frames;
				if (!pending.frame.commands.empty()) m_LocalCommands[pending.frame.targetFrame] = pending.frame.commands;
				if (!pending.frame.observations.empty()) m_LocalObservations[pending.frame.targetFrame] = pending.frame.observations;
				if (!pending.frame.valueObservations.empty()) m_LocalValueObservations[pending.frame.targetFrame] = pending.frame.valueObservations;
				m_Stats.localControllerFramesSent += pending.frame.frames.size();
			}
			if (pending.frame.senderPeerId == m_Config.localPeerId) ++m_Stats.framePacketsSent;
			m_RecoveryOutgoing.pop_front();
		}
		UpdateRelayBacklogBytes();
	}

	std::vector<NetLockstepFrame> NetLockstepCoordinator::CaptureLocalInputHistory() const {
		NET_PLANE_CHECK();
		std::vector<NetLockstepFrame> result;
		result.reserve(m_LocalInputHistory.size());
		for (const auto& [target, frame]: m_LocalInputHistory) result.push_back(frame);
		return result;
	}

	std::vector<NetLockstepFrame> NetLockstepCoordinator::CapturePendingInputs(uint64_t afterFrame) const {
		NET_PLANE_CHECK();
		std::map<std::pair<uint64_t, uint8_t>, NetLockstepFrame> inputs;
		const auto entry = [&](uint64_t target, uint8_t peer) -> NetLockstepFrame& {
			auto& input = inputs[{target, peer}];
			input.senderPeerId = peer;
			input.targetFrame = target;
			input.roundId = m_RoundId;
			return input;
		};
		for (const auto& [target, frames]: m_LocalFrames) if (target > afterFrame) entry(target, m_Config.localPeerId).frames = frames;
		for (const auto& [target, commands]: m_LocalCommands) if (target > afterFrame) entry(target, m_Config.localPeerId).commands = commands;
		for (const auto& [target, observations]: m_LocalObservations) if (target > afterFrame) entry(target, m_Config.localPeerId).observations = observations;
		for (const auto& [target, observations]: m_LocalValueObservations) if (target > afterFrame) entry(target, m_Config.localPeerId).valueObservations = observations;
		for (const auto& [target, peers]: m_RemoteFrames) if (target > afterFrame) for (const auto& [peer, frames]: peers) entry(target, peer).frames = frames;
		for (const auto& [target, peers]: m_RemoteCommands) if (target > afterFrame) for (const auto& [peer, commands]: peers) entry(target, peer).commands = commands;
		for (const auto& [target, peers]: m_RemoteObservations) if (target > afterFrame) for (const auto& [peer, observations]: peers) entry(target, peer).observations = observations;
		for (const auto& [target, peers]: m_RemoteValueObservations) if (target > afterFrame) for (const auto& [peer, observations]: peers) entry(target, peer).valueObservations = observations;
		for (const auto& ready: m_ReadyFrames) {
			if (ready.frame <= afterFrame) continue;
			if (ready.hasLocalInput) {
				auto& input = entry(ready.frame, m_Config.localPeerId);
				input.frames = ready.localFrames;
				input.commands = ready.localCommands;
				input.observations = ready.localObservations;
				input.valueObservations = ready.localValueObservations;
			}
			size_t offset = 0;
			for (const auto& [peer, count]: ready.remoteFrameCounts) {
				auto& input = entry(ready.frame, peer);
				input.frames.assign(ready.remoteFrames.begin() + offset, ready.remoteFrames.begin() + offset + count);
				offset += count;
			}
			for (const auto& command: ready.remoteCommands) entry(ready.frame, command.senderPeerId).commands.push_back(command);
			for (const auto& observation: ready.remoteObservations) entry(ready.frame, observation.senderPeerId).observations.push_back(observation);
			for (const auto& observation: ready.remoteValueObservations) entry(ready.frame, observation.senderPeerId).valueObservations.push_back(observation);
		}
		for (const auto& [peer, held]: m_PreStartFrames) {
			for (const auto& input: held) if (input.targetFrame > afterFrame) inputs.try_emplace(std::make_pair(input.targetFrame, peer), input);
		}
		for (const auto& pending: m_RecoveryOutgoing) {
			if (pending.frame.targetFrame > afterFrame) inputs.try_emplace(std::make_pair(pending.frame.targetFrame, pending.frame.senderPeerId), pending.frame);
		}
		std::vector<NetLockstepFrame> result;
		result.reserve(inputs.size());
		for (auto& [key, frame]: inputs) result.push_back(std::move(frame));
		return result;
	}

	std::vector<NetResyncPendingCommand> NetLockstepCoordinator::CapturePendingCommands(uint64_t afterFrame) const {
		NET_PLANE_CHECK();
		std::vector<NetResyncPendingCommand> result;
		const auto collect = [&](uint64_t frame, const std::vector<NetGameCommand>& commands) {
			if (frame <= afterFrame) return;
			for (const auto& command: commands) {
				if (command.sequence != 0 && !std::holds_alternative<NetGamePlayerBindings>(command.payload)) result.push_back({frame, command});
			}
		};
		for (const auto& input: CapturePendingInputs(afterFrame)) collect(input.targetFrame, input.commands);
		return result;
	}

	std::vector<NetResyncPendingCommand> NetLockstepCoordinator::CapturePendingPlayerBindings(uint64_t afterFrame) const {
		NET_PLANE_CHECK();
		std::vector<NetResyncPendingCommand> result;
		const auto collect = [&](uint64_t frame, const std::vector<NetGameCommand>& commands) {
			if (frame <= afterFrame) return;
			for (const auto& command: commands) if (std::holds_alternative<NetGamePlayerBindings>(command.payload)) result.push_back({frame, command});
		};
		for (const auto& input: CapturePendingInputs(afterFrame)) collect(input.targetFrame, input.commands);
		return result;
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
			++m_Stats.checksumCompares;
			if (localIt->second != hash) {
				++m_Stats.checksumMismatches;
				std::cout << "[lockstep] desync at frame " << frame << " against " << DescribePeer(peerId)
				          << " (submitted " << m_Stats.checksumSubmissions << ", sent " << m_Stats.checksumSends
				          << ", compared " << m_Stats.checksumCompares << ")" << std::endl;
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

	void NetLockstepCoordinator::ShiftDeadlinesPastOurOwnPark(uint64_t nowMs) {
		NET_PLANE_CHECK();
		const uint64_t previous = m_LastTickMs;
		m_LastTickMs = nowMs;
		// The wait loop ticks us every millisecond, so a gap of several sim ticks is OUR park - a
		// checkpoint capture, an activity restart - and not a peer's silence. Time nobody was listening
		// through is not lateness: every running deadline moves with it instead of being spent.
		const uint64_t park = static_cast<uint64_t>(std::max(50.0, 4 * m_Config.simTickMs));
		if (previous == 0 || nowMs <= previous || nowMs - previous < park) {
			return;
		}
		const uint64_t gap = nowMs - previous;
		const auto shift = [&](uint64_t& stamp) { if (stamp != 0) stamp = std::min(nowMs, stamp + gap); };
		shift(m_FirstMissingMs);
		shift(m_ConsumerWaitStartMs);
		shift(m_WaitStartMs);
		shift(m_AuthorityLastHeardMs);
		for (auto& [peer, heard]: m_PeerLastHeardMs) shift(heard);
		for (auto& [peer, stats]: m_Stats.peers) { shift(stats.lastHeardMs); shift(stats.lastProgressMs); }
		for (auto& [revision, decision]: m_TimingDecisions) shift(decision.proposedAtMs);
		++m_Stats.ownParksExcluded;
		m_Stats.longestOwnParkMs = std::max(m_Stats.longestOwnParkMs, gap);
	}

	uint8_t NetLockstepCoordinator::AgreedSeatDeviceClass(int seat) const {
		NET_PLANE_CHECK();
		if (!m_AgreedStartRecord || seat < 0) return 0;
		// Seats are the roster's human slots in order, the same numbering every peer maps its players by.
		int humanSlot = 0;
		for (const NetMatchPlayerSlot& slot: m_Config.matchConfig.players) {
			if (slot.cpu) continue;
			if (humanSlot++ == seat) return slot.peerId >= 1 && slot.peerId <= m_AgreedStartRecord->peerDeviceClasses.size() ? m_AgreedStartRecord->peerDeviceClasses[slot.peerId - 1] : 0;
		}
		return 0;
	}

	void NetLockstepCoordinator::NoteLocalStartPark(uint32_t restartMs) {
		NET_PLANE_CHECK();
		if (m_LocalStartupPublished && restartMs == m_LocalStartParkMs) {
			return;
		}
		// A machine that restarts instantly measured its startup too: the publication is the fact,
		// never the number, or a 0 ms restart parks the round for good.
		m_LocalStartupPublished = true;
		// The round's own restart runs before our first Tick, so the tick-gap detector never sees it.
		m_LocalStartParkMs = restartMs;
		m_Stats.longestOwnParkMs = std::max<uint64_t>(m_Stats.longestOwnParkMs, restartMs);
		// The host's own publication is the first answer-budget anchor.  A Tick that happens before the
		// activity has measured its restart must not spend the round's deadline while the host is still
		// waiting for its own startup fact.
		// TickStartupWait stamps the publication with the coordinator's tick clock.  NoteLocalStartPark
		// is called by the sim thread and has no clock argument; using the process clock here would make
		// a rematch's custom/session clock run backwards and consume (or extend) the answer budget.
		// Only the handshake carries it out: a start re-sent after the round is running is judged against a
		// config that may have moved since, and a round must never be re-defined to publish a measurement.
		if (m_State == NetLockstepState::WaitingForStart && !m_RemotePeerIds.empty()) {
			std::string ignored;
			(void)SendStart(&ignored);
		}
	}

	void NetLockstepCoordinator::BeginSynchronizedCapture(uint64_t completedFrame) {
		NET_PLANE_CHECK();
		if (m_Playback || completedFrame == UINT64_MAX || m_Config.simTickMs <= 0) return;
		// Every peer measures its own capture, but the window is one host fact.  A client that opened a window
		// from its own budget would empty frames the host commits with real input, so it only measures here and
		// takes the window from the host's publication.
		if (m_Config.localPeerId != GetHostPeerId()) {
			m_CaptureParkReportsMs[m_Config.localPeerId] = static_cast<uint32_t>(std::min<double>(UINT32_MAX, m_SynchronizedCaptureBudgetMs));
			return;
		}
		// A park still waiting for a report closes on the reports it has before the next one opens: its peers wait
		// for its final, and a host that moved on would never send one.
		if (m_SynchronizedCaptureStartFrame != UINT64_MAX && !m_CaptureParkFinalized && IsRunning()) {
			m_CaptureParkDeadlineMs = m_TimingNowMs != 0 ? m_TimingNowMs : NetLockstepNowMs();
			PublishCapturePark(m_SynchronizedCaptureStartFrame);
		}
		// The park cannot reach back over frames a peer has already accepted, and a peer accepts more while this
		// window is in flight: the reported horizons are one trip old and the commit costs another, so the
		// opening frame clears both.  A park that opened on the host's own horizon emptied a frame the client
		// had already committed with real input, which is one tick of divergence per park.
		uint64_t start = std::max(completedFrame + 1, m_Stats.nextFrame);
		double linkMs = 0.0;
		bool live = false;
		for (uint8_t peer: m_RemotePeerIds) {
			if (IsPeerGoneAtFrame(peer, start)) continue;
			const auto& stats = m_Stats.peers[peer];
			start = std::max(start, stats.reportedNextFrame);
			linkMs = std::max(linkMs, static_cast<double>(stats.pingMs) + stats.jitterMs);
			live = true;
		}
		if (live) start += static_cast<uint64_t>(std::ceil(2.0 * linkMs / m_Config.simTickMs)) + 1;
		m_SynchronizedCaptureStartFrame = start;
		// The last park's slowest capture joins the history its successors are sized from.
		if (!m_CaptureParkReportsMs.empty()) {
			uint32_t slowest = 0;
			for (const auto& [peer, captureMs]: m_CaptureParkReportsMs) slowest = std::max(slowest, captureMs);
			m_ParkCaptureHistoryMs.push_back(slowest);
			while (m_ParkCaptureHistoryMs.size() > 3) m_ParkCaptureHistoryMs.pop_front();
		}
		// The window is bounded by what a capture costs, never by a round trip: every frame in it still carries its
		// input, so it only keeps the bound from judging a seat while the capture runs. It is one host fact, final
		// when published; a capture that runs past it is judged by the bound like any late input.
		const double captureMs = SteadyCaptureCostMs();
		const uint64_t ticks = static_cast<uint64_t>(std::max(1.0, std::ceil(captureMs / m_Config.simTickMs)));
		m_SynchronizedCaptureEndFrame = m_SynchronizedCaptureStartFrame + ticks - 1;
		m_CaptureParkDeadlineMs = 0;
		m_CaptureParkFinalized = false;
		m_CaptureParkAwaitingReports = false;
		m_CaptureParkPublishedEndFrame = UINT64_MAX;
		// Each park closes on its own reports: a previous park's duration is not evidence about this one.
		m_CaptureParkReportsMs.clear();
		std::cout << "[net-lockstep] capture park begin frame=" << m_SynchronizedCaptureStartFrame
		          << " end=" << m_SynchronizedCaptureEndFrame << " completed=" << completedFrame << " capture_estimate_ms=" << captureMs << std::endl;
		if (!IsRunning() || m_NextTimingRevision == UINT64_MAX) return;
		NetLockstepTiming timing;
		timing.senderPeerId = GetHostPeerId();
		timing.peerId = GetHostPeerId();
		timing.action = NetTimingAction::CapturePark;
		timing.phase = NetTimingPhase::Commit;
		timing.sessionId = m_Config.sessionId;
		timing.roundId = m_RoundId;
		timing.revision = m_NextTimingRevision++;
		timing.authorityGeneration = m_Config.migrationGeneration;
		timing.applyFrame = m_SynchronizedCaptureStartFrame;
		timing.nextFrame = timing.cutoffFrame = m_SynchronizedCaptureEndFrame;
		timing.delayFrames = static_cast<uint16_t>(std::min<uint64_t>(NetLockstepCodec::c_MaxInputDelayFrames, ticks));
		timing.pingMs = static_cast<uint32_t>(std::clamp(std::ceil(captureMs), 1.0, static_cast<double>(UINT32_MAX)));
		timing.requiredPeers = static_cast<uint8_t>(1U << (GetHostPeerId() - 1));
		for (uint8_t peer: m_RemotePeerIds)
			if (!IsPeerGoneAtFrame(peer, m_SynchronizedCaptureStartFrame)) timing.requiredPeers |= static_cast<uint8_t>(1U << (peer - 1));
		m_CaptureParkRevision = timing.revision;
		m_CaptureParkPublishedEndFrame = timing.nextFrame;
		ApplyCapturePark(timing);
		std::string ignored;
		(void)SendPacket({timing}, NetTransportLane::ControlReliable, &ignored);
	}

	double NetLockstepCoordinator::SteadyCaptureCostMs() const {
		// The middle of the last three parks' slowest captures: one cold capture never sizes a window alone.
		// Before any park was measured the window is the slow-player bound, the wait the round already accepts.
		if (m_ParkCaptureHistoryMs.empty()) return std::max(m_Config.simTickMs, std::floor(m_Config.slowPlayerBoundTicks * m_Config.simTickMs));
		std::vector<uint32_t> sorted(m_ParkCaptureHistoryMs.begin(), m_ParkCaptureHistoryMs.end());
		std::sort(sorted.begin(), sorted.end());
		return static_cast<double>(sorted[sorted.size() / 2]);
	}

	void NetLockstepCoordinator::CompleteSynchronizedCapture(uint64_t completedFrame, double captureMs) {
		NET_PLANE_CHECK();
		if (m_Playback || completedFrame == UINT64_MAX || !std::isfinite(captureMs) || captureMs < 0 || m_Config.simTickMs <= 0) return;
		m_SynchronizedCaptureBudgetMs = std::max(m_SynchronizedCaptureBudgetMs, captureMs);
		m_CaptureParkReportsMs[m_Config.localPeerId] = static_cast<uint32_t>(std::min<double>(UINT32_MAX, std::ceil(captureMs)));
		if (m_Playback || !IsRunning()) return;
		// The host's own measurement sizes the next park; the published window never moves.
		if (m_Config.localPeerId == GetHostPeerId()) return;
		m_PendingCaptureReportMs = static_cast<uint32_t>(std::min<double>(UINT32_MAX, std::ceil(captureMs)));
		m_PendingCaptureTick = completedFrame;
		SendCaptureParkReport();
	}

	void NetLockstepCoordinator::SendCaptureParkReport(uint64_t nowMs) {
		// A client reports against the park the host named.  Until that publication arrives it has no frame to
		// key the report to, so the measurement waits and goes out with the window.
		if (m_Playback || m_Config.localPeerId == GetHostPeerId() || !IsRunning() || m_SynchronizedCaptureStartFrame == UINT64_MAX) {
			return;
		}
		// A capture belongs to the park opened after it: the window this peer is in may still be the previous
		// park's, and a report spent there leaves the capture's own park without one.
		const bool fresh = m_PendingCaptureReportMs != 0 && m_SynchronizedCaptureStartFrame > m_PendingCaptureTick &&
		                   m_SynchronizedCaptureStartFrame != m_ReportedCaptureParkStart;
		// The park waits on this report: until the host's final answers it, it goes out again rather than leaving
		// every peer at the window's end on one lost or refused send.
		constexpr uint64_t c_CaptureReportResendMs = 250;
		const bool resend = !fresh && m_ReportedCaptureParkMs != 0 && m_ReportedCaptureParkStart == m_SynchronizedCaptureStartFrame &&
		                    m_CaptureParkAwaitingReports && nowMs >= m_CaptureReportSentMs + c_CaptureReportResendMs;
		if (!fresh && !resend) return;
		NetLockstepTiming report;
		report.senderPeerId = m_Config.localPeerId;
		report.peerId = m_Config.localPeerId;
		report.action = NetTimingAction::CapturePark;
		report.phase = NetTimingPhase::Status;
		report.sessionId = m_Config.sessionId;
		report.roundId = m_RoundId;
		report.authorityGeneration = m_Config.migrationGeneration;
		report.applyFrame = m_SynchronizedCaptureStartFrame;
		report.nextFrame = m_SynchronizedCaptureEndFrame;
		report.pingMs = fresh ? m_PendingCaptureReportMs : m_ReportedCaptureParkMs;
		std::string ignored;
		if (!SendPacket({report}, NetTransportLane::ControlReliable, &ignored, nullptr, nullptr, GetHostPeerId())) return;
		m_CaptureReportSentMs = nowMs != 0 ? nowMs : m_TimingNowMs;
		if (!fresh) {
			if (!m_CaptureReportResent) std::cout << "[net-lockstep] capture park report resent frame=" << m_SynchronizedCaptureStartFrame << " capture_ms=" << report.pingMs << std::endl;
			m_CaptureReportResent = true;
			return;
		}
		m_CaptureReportResent = false;
		m_ReportedCaptureParkStart = m_SynchronizedCaptureStartFrame;
		m_ReportedCaptureParkMs = m_PendingCaptureReportMs;
		m_PendingCaptureReportMs = 0;
		m_PendingCaptureTick = UINT64_MAX;
	}

	void NetLockstepCoordinator::PublishCapturePark(uint64_t startFrame) {
		if (m_Playback || m_Config.localPeerId != GetHostPeerId() || !IsRunning() || startFrame == UINT64_MAX || m_Config.simTickMs <= 0 || m_CaptureParkFinalized) return;
		const uint64_t nowMs = m_TimingNowMs != 0 ? m_TimingNowMs : NetLockstepNowMs();
		const bool budgetExpired = m_CaptureParkDeadlineMs != 0 && nowMs >= m_CaptureParkDeadlineMs;
		uint32_t slowestMs = 0;
		for (const auto& [peer, captureMs]: m_CaptureParkReportsMs) {
			if (peer == m_Config.localPeerId || !IsPeerGoneAtFrame(peer, startFrame)) slowestMs = std::max(slowestMs, captureMs);
		}
		const uint64_t ticks = static_cast<uint64_t>(std::max(1.0, std::ceil(static_cast<double>(std::max<uint32_t>(1, slowestMs)) / m_Config.simTickMs)));
		bool allReports = true;
		for (uint8_t peer: m_RemotePeerIds)
			if (!IsPeerGoneAtFrame(peer, startFrame) && !m_CaptureParkReportsMs.contains(peer)) allReports = false;
		// A missing capture report is bounded by the same answer budget as startup and input
		// acknowledgements.  Close on the slowest report we do have; the ordinary post-park
		// missing-input bound judges the silent seat.
		allReports = allReports || budgetExpired;
		const uint64_t endFrame = startFrame + ticks;
		// Republish whenever the window this host holds has grown past the one the peers were given, or they
		// would reach an end the round has already moved.
		if (!allReports && m_CaptureParkRevision != 0 && endFrame <= m_SynchronizedCaptureEndFrame &&
		    m_CaptureParkPublishedEndFrame != UINT64_MAX && m_SynchronizedCaptureEndFrame <= m_CaptureParkPublishedEndFrame) return;
		if (m_NextTimingRevision == UINT64_MAX) return;
		NetLockstepTiming timing;
		timing.senderPeerId = GetHostPeerId();
		timing.peerId = GetHostPeerId();
		timing.action = NetTimingAction::CapturePark;
		timing.phase = NetTimingPhase::Commit;
		timing.sessionId = m_Config.sessionId;
		timing.roundId = m_RoundId;
		timing.revision = m_NextTimingRevision++;
		timing.authorityGeneration = m_Config.migrationGeneration;
		timing.applyFrame = startFrame;
		// The end only grows: peers have already emptied every frame of the window they were published.
		timing.nextFrame = std::min(startFrame + CaptureParkCapTicks(),
		    allReports ? std::max(endFrame, m_SynchronizedCaptureEndFrame) : m_SynchronizedCaptureEndFrame);
		timing.cutoffFrame = timing.nextFrame;
		timing.delayFrames = static_cast<uint16_t>(std::min<uint64_t>(NetLockstepCodec::c_MaxInputDelayFrames, ticks));
		timing.pingMs = allReports ? std::max<uint32_t>(1, slowestMs) : 0;
		timing.requiredPeers = static_cast<uint8_t>(1U << (GetHostPeerId() - 1));
		for (uint8_t peer: m_RemotePeerIds)
			if (!IsPeerGoneAtFrame(peer, startFrame)) timing.requiredPeers |= static_cast<uint8_t>(1U << (peer - 1));
		m_CaptureParkRevision = timing.revision;
		m_CaptureParkPublishedEndFrame = timing.nextFrame;
		if (allReports) {
			m_CaptureParkAwaitingReports = false;
			m_CaptureParkFinalized = true;
		}
		ApplyCapturePark(timing);
		if (!allReports) m_CaptureParkAwaitingReports = true;
		std::string ignored;
		(void)SendPacket({timing}, NetTransportLane::ControlReliable, &ignored);
	}

	void NetLockstepCoordinator::ApplyCapturePark(const NetLockstepTiming& timing) {
		if (timing.action != NetTimingAction::CapturePark || timing.applyFrame == UINT64_MAX || timing.nextFrame < timing.applyFrame) return;
		// A park is named by the frame it opens at, so the final commit for the park this peer is IN is never
		// dropped for revision ordering: the next park's provisional may have arrived first, and without its
		// final this one never releases.
		if (timing.revision < m_CaptureParkRevision && timing.applyFrame != m_SynchronizedCaptureStartFrame) return;
		// A park is named by the frame it opens at, so the next park's provisional opens a new window while a
		// late provisional for the park we already closed stays stale.
		const bool newPark = m_SynchronizedCaptureStartFrame == UINT64_MAX || timing.applyFrame != m_SynchronizedCaptureStartFrame;
		if (m_CaptureParkFinalized && timing.pingMs == 0 && !newPark) return;
		if (newPark) {
			m_SynchronizedCaptureStartFrame = timing.applyFrame;
			m_SynchronizedCaptureEndFrame = timing.applyFrame;
			m_CaptureParkFinalized = false;
		}
		// The end only grows: this peer has already emptied every frame of the window it was published.
		m_SynchronizedCaptureEndFrame = std::max(m_SynchronizedCaptureEndFrame, timing.nextFrame);
		if (timing.pingMs == 0) {
			m_CaptureParkAwaitingReports = true;
			std::cout << "[net-lockstep] capture park window peer=" << static_cast<int>(m_Config.localPeerId)
			          << " frame=" << m_SynchronizedCaptureStartFrame << " end=" << m_SynchronizedCaptureEndFrame << std::endl;
			SendCaptureParkReport();
		} else {
			m_CaptureParkAwaitingReports = false;
			m_CaptureParkFinalized = true;
			// The window is final before its capture ends: the measurement still goes to the host for the next park.
			SendCaptureParkReport();
			m_SynchronizedCaptureBudgetMs = std::max(m_SynchronizedCaptureBudgetMs, static_cast<double>(timing.pingMs));
			std::cout << "[net-lockstep] capture park released frame=" << m_SynchronizedCaptureStartFrame
			          << " end=" << m_SynchronizedCaptureEndFrame << " capture_ms=" << timing.pingMs << std::endl;
		}
		m_HighestParkEndFrame = std::max(m_HighestParkEndFrame, m_SynchronizedCaptureEndFrame);
		m_CaptureParkRevision = std::max(m_CaptureParkRevision, timing.revision);
	}

	uint64_t NetLockstepCoordinator::CaptureParkCapTicks() const {
		// A park is never longer than the capture it covers plus the answer budget it closes on: the window may
		// be extended while a report is late, never without end.
		if (m_Config.simTickMs <= 0) return 1;
		const double budgetMs = m_SynchronizedCaptureBudgetMs + (m_Config.timeoutMs == 0 ? c_DefaultCaptureParkBudgetMs : m_Config.timeoutMs);
		return static_cast<uint64_t>(std::max(1.0, std::ceil(budgetMs / m_Config.simTickMs)));
	}

	bool NetLockstepCoordinator::CaptureParkMayReach(uint64_t frame) const {
		NET_PLANE_CHECK();
		if (m_SynchronizedCaptureStartFrame == UINT64_MAX || frame < m_SynchronizedCaptureStartFrame) return false;
		return frame <= (m_CaptureParkFinalized ? m_SynchronizedCaptureEndFrame : std::max(m_SynchronizedCaptureEndFrame, m_SynchronizedCaptureStartFrame + CaptureParkCapTicks()));
	}

	void NetLockstepCoordinator::RetryLateStartReclaims() {
		if (m_LateStartReclaims.empty() || m_Config.localPeerId != GetHostPeerId()) return;
		for (auto it = m_LateStartReclaims.begin(); it != m_LateStartReclaims.end();) {
			const uint8_t peer = it->first;
			if (!HasHeldAISeat(peer) || m_ReclaimTransactions.contains(peer)) { it = m_LateStartReclaims.erase(it); continue; }
			const uint32_t incarnation = m_Config.peerIncarnations.contains(peer) ? m_Config.peerIncarnations.at(peer) : 0;
			if (SchedulePeerReclaim(peer, it->second, incarnation + 1, FutureTimingFrame())) {
				std::cout << "[net-match] late startup reclaim admitted peer " << static_cast<int>(peer) << std::endl;
				it = m_LateStartReclaims.erase(it);
			} else ++it;
		}
	}

	void NetLockstepCoordinator::FlushDeferredParkTimings() {
		if (m_ApplyingDeferredParkTiming || IsSynchronizedCapturePark(m_Stats.nextFrame) || m_DeferredParkTimings.empty()) return;
		// The host's final end is what re-stamps a deferred decision.  Releasing one at a local end would apply
		// it at the original frame here and at end + 1 on the host: the same decision at two frames.
		if (m_SynchronizedCaptureStartFrame != UINT64_MAX && !m_CaptureParkFinalized) return;
		m_ApplyingDeferredParkTiming = true;
		auto deferred = std::move(m_DeferredParkTimings);
		m_DeferredParkTimings.clear();
		const bool host = m_Config.localPeerId == GetHostPeerId();
		const bool parked = m_CaptureParkFinalized && m_SynchronizedCaptureStartFrame != UINT64_MAX;
		std::set<uint64_t> reproposed;
		for (auto timing: deferred) {
			if (!parked || timing.applyFrame > m_SynchronizedCaptureEndFrame) {
				ApplyTiming(timing);
				continue;
			}
			// A client never re-authors a boundary: the host withdraws the old revision and sends a fresh
			// proposal and commit, and the client applies that pair like any other.
			if (!host) continue;
			// One decision, one re-proposal, whichever of its phases the park happened to retain.
			if (!reproposed.insert(timing.revision).second) continue;
			NetLockstepTiming proposal = timing;
			const auto retained = m_TimingDecisions.find(timing.revision);
			if (retained != m_TimingDecisions.end()) proposal = retained->second.proposal;
			else if (timing.phase == NetTimingPhase::Commit) continue;
			if (m_NextTimingRevision == UINT64_MAX) { m_DeferredParkTimings.push_back(timing); continue; }
			const uint64_t superseded = proposal.revision;
			const uint64_t oldApply = proposal.applyFrame;
			proposal.applyFrame = m_SynchronizedCaptureEndFrame + 1;
			if (proposal.cutoffFrame == oldApply) proposal.cutoffFrame = proposal.applyFrame;
			if (proposal.action == NetTimingAction::Reclaim || proposal.action == NetTimingAction::WorldAdmission)
				proposal.neutralThroughFrame = std::max(proposal.neutralThroughFrame, proposal.applyFrame + proposal.delayFrames);
			proposal.revision = m_NextTimingRevision++;
			proposal.supersededRevision = superseded;
			m_TimingDecisions.erase(superseded);
			// A phase that carries its own authority lands at once; a proposal waits for the acknowledgement it
			// asks for and only then commits, so no peer ever sees a commit whose proposal it never had.
			const bool authored = proposal.phase == NetTimingPhase::HoldAtFrame || proposal.phase == NetTimingPhase::ReclaimAtFrame;
			m_TimingDecisions[proposal.revision] = {proposal,
			    authored ? proposal.requiredPeers : static_cast<uint8_t>(1U << (GetHostPeerId() - 1)), authored, m_TimingNowMs};
			QueueTiming(proposal);
			if (authored) ApplyTiming(proposal);
		}
		FlushTimingOutgoing();
		m_ApplyingDeferredParkTiming = false;
	}

	bool NetLockstepCoordinator::IsSynchronizedCapturePark(uint64_t frame) const {
		NET_PLANE_CHECK();
		return m_SynchronizedCaptureStartFrame != UINT64_MAX && frame >= m_SynchronizedCaptureStartFrame && frame <= m_SynchronizedCaptureEndFrame;
	}

	bool NetLockstepCoordinator::SendAgreedStart(uint8_t onlyPeerId) {
		if (!m_AgreedStartRecord || !m_Transport || m_Config.localPeerId != GetHostPeerId()) return false;
		std::string error;
		if (!SendPacket({*m_AgreedStartRecord}, NetTransportLane::ControlReliable, &error, nullptr, nullptr, onlyPeerId)) {
			if (!error.empty()) std::cout << "[net-match] agreed start send failed: " << error << std::endl;
			return false;
		}
		++m_Stats.startPacketsSent;
		return true;
	}

	void NetLockstepCoordinator::ApplyAgreedStart(const NetLockstepStart& start, uint64_t nowMs) {
		// A seat joining the running round starts at its own admission, past the round's first boundary. Taking that
		// record - its round tag above all - would turn the host's own start into a mismatch instead of a straggler.
		if (m_Config.joinsRunningRound && start.agreedEffectiveStartFrame < m_Config.startFrame) return;
		if (m_AgreedStartApplied) {
			if (m_AgreedStartRecord && *m_AgreedStartRecord != start) {
				Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "conflicting agreed start boundary");
			}
			return;
		}
		if (m_State != NetLockstepState::WaitingForStart && (m_State != NetLockstepState::Running || HasCommittedAFrame())) {
			// A seat that rejoined past this boundary hears the host's record again when it republishes its start.
			// The record names a frame the round is already past, so it is a retransmission to ignore rather than
			// a boundary that contradicts anything this peer has committed.
			if (start.agreedEffectiveStartFrame < m_Config.startFrame || start.agreedEffectiveStartFrame < m_Stats.nextFrame) return;
			Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "agreed start boundary arrived after the round started");
			return;
		}
		if (start.sessionId != m_Config.sessionId || (m_RoundId != 0 && start.roundId != m_RoundId) ||
		    start.peerCount != m_Config.peerCount || start.agreedEffectiveStartFrame < start.agreedFirstFrame) {
			Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "agreed start boundary does not match the round");
			return;
		}
		m_AgreedStartApplied = true;
		m_AgreedStartRecord = start;
		m_RoundId = start.roundId;
		m_ObservationDecodeTables.roundId = m_RoundId;
		m_PeerEffectiveStart.clear();
		uint64_t firstCommitFrame = UINT64_MAX;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			const uint64_t effective = start.peerEffectiveStartFrames[peer - 1];
			m_PeerEffectiveStart[peer] = effective;
			m_Config.peerInputDelayFrames[peer] = start.peerInputDelays[peer - 1];
			if (m_Config.matchConfig.peerInputDelayFrames.size() < m_Config.peerCount)
				m_Config.matchConfig.peerInputDelayFrames.resize(m_Config.peerCount, m_Config.inputDelayFrames);
			m_Config.matchConfig.peerInputDelayFrames[peer - 1] = start.peerInputDelays[peer - 1];
			if (peer == m_Config.localPeerId) m_Config.inputDelayFrames = start.peerInputDelays[peer - 1];
			firstCommitFrame = std::min(firstCommitFrame, effective);
			if ((start.publishedPeerMask & (uint32_t{1} << (peer - 1))) != 0) {
				m_PeerStartupPublished.insert(peer);
				m_Stats.peers[peer].startParkMs = start.peerStartupParks[peer - 1];
			}
		}
		m_Stats.effectiveStartFrame = start.agreedEffectiveStartFrame != 0 ? start.agreedEffectiveStartFrame : firstCommitFrame;
		// A seat that rejoined at a later activation frame is already past this boundary: the record is the
		// round's start fact, never a rewind of a horizon the seat has moved beyond.
		const bool boundaryPassed = m_Config.startFrame > m_Stats.effectiveStartFrame || m_Stats.nextFrame > m_Stats.effectiveStartFrame;
		if (!boundaryPassed) m_Stats.nextFrame = m_Stats.effectiveStartFrame;
		// The host may have received a publication before this packet while a client is still draining its
		// relay lane. Install a harmless synthetic start for those published seats so their pre-start frames
		// are released by the same host fact instead of waiting for a second local formation.
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			if (peer == m_Config.localPeerId || (start.publishedPeerMask & (uint32_t{1} << (peer - 1))) == 0) continue;
			if (m_RemoteStartsReceived.insert(peer).second) {
				NetLockstepStart synthetic;
				synthetic.sessionId = m_Config.sessionId;
				synthetic.startFrame = m_Config.startFrame;
				synthetic.inputDelayFrames = PeerInputDelay(peer);
				synthetic.controllerFrameVersion = ControllerFrame::c_Version;
				synthetic.controllerFrameEncodedSize = static_cast<uint16_t>(ControllerFrame::c_EncodedSize);
				synthetic.localPeerId = peer;
				synthetic.peerCount = m_Config.peerCount;
				synthetic.scenario = m_Config.scenario;
				synthetic.ownershipPolicy = m_Config.ownershipPolicy;
				synthetic.roundId = m_RoundId;
				synthetic.activityRestartMs = start.peerStartupParks[peer - 1];
				synthetic.startupPublished = true;
				m_RemoteStarts[peer] = synthetic;
			}
		}
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			// The startup holds belong to the boundary; a seat that joined past it takes its seat state from
			// the reclaim it was admitted with, not from a resolution the round already made.
			if (boundaryPassed || (start.heldPeerMask & (uint32_t{1} << (peer - 1))) == 0) continue;
			m_AiHeldSeats[peer] = start.agreedFirstFrame;
			m_PeerLeaveFrames[peer] = start.agreedFirstFrame;
			m_DroppedSeats.insert(peer);
			m_DroppedAtMs[peer] = nowMs;
			m_DroppedSeatResolutions[peer] = NetLockstepHoldResolution::Substituted;
			m_StartupHeldSeatStamps.insert(peer);
			++m_Stats.peers[peer].holds;
			if (peer == m_Config.localPeerId) m_LocalSeatHeld = true;
			std::cout << "[net-match] hold peer=" << static_cast<int>(peer) << " frame=" << start.agreedFirstFrame << " AI in control" << std::endl;
		}
		RefreshLeftSeatHolds();
		std::cout << "[net-match] agreed first frame=" << start.agreedFirstFrame
		          << " effective_start=" << m_Stats.effectiveStartFrame
		          << " startup_deadline_ms=" << start.agreedDeadlineMs << std::endl;
		m_State = NetLockstepState::Running;
		auto timing = std::move(m_PreStartTiming); m_PreStartTiming.clear();
		for (const auto& [decision, source]: timing) HandleTiming(decision, nowMs, source);
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer)
			if (peer != m_Config.localPeerId && (start.publishedPeerMask & (uint32_t{1} << (peer - 1))) != 0)
				FlushPreStart(peer, nowMs);
		m_WaitingFrame = std::numeric_limits<uint64_t>::max();
	}

	void NetLockstepCoordinator::FormAgreedFirstFrame(uint64_t nowMs) {
		if (m_AgreedStartApplied || m_Config.localPeerId != GetHostPeerId()) return;
		const bool expired = StartupWaitExpired(nowMs);
		uint32_t slowestStartupMs = 0;
		uint32_t publishedPeerMask = 0;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			const bool published = peer == m_Config.localPeerId ? m_LocalStartupPublished : m_PeerStartupPublished.contains(peer);
			if (published) {
				publishedPeerMask |= uint32_t{1} << (peer - 1);
				const uint32_t park = peer == m_Config.localPeerId ? m_LocalStartParkMs : m_Stats.peers[peer].startParkMs;
				slowestStartupMs = std::max(slowestStartupMs, park);
			}
		}
		const uint64_t startupFrames = m_Config.simTickMs > 0
			? static_cast<uint64_t>(std::ceil(static_cast<double>(slowestStartupMs) / m_Config.simTickMs)) : 0;
		NetLockstepStart record;
		record.sessionId = m_Config.sessionId;
		record.startFrame = m_Config.startFrame;
		record.inputDelayFrames = m_Config.inputDelayFrames;
		record.controllerFrameVersion = ControllerFrame::c_Version;
		record.controllerFrameEncodedSize = static_cast<uint16_t>(ControllerFrame::c_EncodedSize);
		record.localPeerId = m_Config.localPeerId;
		record.peerCount = m_Config.peerCount;
		record.scenario = m_Config.scenario;
		record.ownershipPolicy = m_Config.ownershipPolicy;
		record.roundId = m_RoundId;
		record.resumeFromSnapshot = m_Config.resumeFromSnapshot;
		record.activityRestartMs = m_LocalStartParkMs;
		record.startupPublished = m_LocalStartupPublished;
		record.deviceClass = m_LocalDeviceClass;
		record.agreedStartRecord = true;
		record.agreedFirstFrame = m_Config.startFrame + startupFrames;
		record.publishedPeerMask = publishedPeerMask;
		// A wall-clock deadline is local state, not a deterministic boundary.  The host's frame record
		// carries the agreed frames and masks only; each peer keeps the answer budget in its handshake state.
		record.agreedDeadlineMs = 0;
		uint32_t heldPeerMask = 0;
		uint64_t firstCommitFrame = UINT64_MAX;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
			const auto delayIt = m_Config.peerInputDelayFrames.find(peer);
			const uint16_t delay = delayIt != m_Config.peerInputDelayFrames.end() ? delayIt->second : m_Config.inputDelayFrames;
			record.peerEffectiveStartFrames[peer - 1] = record.agreedFirstFrame + (m_Config.resumeFromSnapshot ? 0 : delay);
			record.peerStartupParks[peer - 1] = peer == m_Config.localPeerId ? m_LocalStartParkMs : m_Stats.peers[peer].startParkMs;
			record.peerInputDelays[peer - 1] = delay;
			record.peerDeviceClasses[peer - 1] = peer == m_Config.localPeerId ? m_LocalDeviceClass : m_PeerDeviceClasses[peer - 1];
			firstCommitFrame = std::min(firstCommitFrame, record.peerEffectiveStartFrames[peer - 1]);
			if ((expired || m_StartupLinksLost.contains(peer)) && m_Config.substituteSlowPeers && peer != m_Config.localPeerId &&
			    std::find(m_RemotePeerIds.begin(), m_RemotePeerIds.end(), peer) != m_RemotePeerIds.end() &&
			    !IsPeerGoneAtFrame(peer, record.agreedFirstFrame) &&
			    (publishedPeerMask & (uint32_t{1} << (peer - 1))) == 0)
				heldPeerMask |= uint32_t{1} << (peer - 1);
		}
		record.heldPeerMask = heldPeerMask;
		record.agreedEffectiveStartFrame = firstCommitFrame == UINT64_MAX ? record.agreedFirstFrame : firstCommitFrame;
		m_AgreedStartRecord = record;
		ApplyAgreedStart(record, nowMs);
		(void)SendAgreedStart();
	}

	bool NetLockstepCoordinator::StartupWaitExpired(uint64_t nowMs) const {
		// A publication that never arrives cannot park the round: a slow loader gets the startup answer budget past
		// the host's own startup and no more; the AI takes its seat at the agreed first frame on every peer.
		if (!m_StartWaitAnnounced || m_Config.timeoutMs == 0) return false;
		const uint64_t budgetMs = m_Config.substituteSlowPeers ? std::min<uint64_t>(m_Config.timeoutMs, c_StartupAnswerBudgetMs) : m_Config.timeoutMs;
		return nowMs >= m_StartWaitSinceMs && nowMs - m_StartWaitSinceMs >= budgetMs;
	}

	void NetLockstepCoordinator::TickStartupWait(uint64_t nowMs) {
		if (m_State != NetLockstepState::WaitingForStart || !m_RequirePublishedStart || m_AgreedStartApplied) {
			return;
		}
		if (m_Config.localPeerId != GetHostPeerId()) return;
		// The answer budget starts at the host's published startup, not at the first scheduler tick.
		if (!m_LocalStartupPublished) return;
		if (!m_StartWaitAnnounced) {
			m_StartWaitAnnounced = true;
			m_StartWaitSinceMs = nowMs;
		}
		bool allPublished = m_LocalStartupPublished;
		for (uint8_t peer: m_RemotePeerIds) {
			// A clean leave can arrive while the seat is still waiting to publish its startup.  Its
			// announced leave is already the host-authored boundary for that seat; it must not hold the
			// surviving seats behind the startup publication budget.
			if (IsPeerGoneAtFrame(peer, m_Config.startFrame)) continue;
			// A seat whose link died before it published never will; the start holds it instead of waiting out the budget.
			allPublished = allPublished && (m_PeerStartupPublished.contains(peer) || (m_Config.substituteSlowPeers && m_StartupLinksLost.contains(peer)));
		}
		if (allPublished || StartupWaitExpired(nowMs)) FormAgreedFirstFrame(nowMs);
	}

	void NetLockstepCoordinator::Tick(uint64_t nowMs) {
		NET_PLANE_CHECK();
		if (!m_Transport || m_State == NetLockstepState::Idle) {
			return;
		}
		if (!m_PlaneTicking) m_SimTickedMs.store(nowMs, std::memory_order_release);
		TickStartupWait(nowMs);
		ShiftDeadlinesPastOurOwnPark(nowMs);
		m_TimingNowMs = nowMs;
		if (!m_PlaneTicking) TickMigrationRollCallLinks(nowMs);
		if (IsMigrating()) {
			if (!m_PlaneTicking) TickHostMigration(nowMs);
			return;
		}
		RefreshLeftSeatHolds();
		HandleTransportEvents(nowMs);
		if (IsMigrating()) {
			TickHostMigration(nowMs);
			return;
		}
		for (auto& [peer, pending]: m_MigrationOutbox)
			while (!pending.empty() && m_Transport->Send(peer, NetTransportLane::ControlReliable, pending.front()))
				pending.pop_front();
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
		FlushSeatSnapshot();
		FlushRecoveryInputs(nowMs);
		DropUnreachablePeers(nowMs);
		AdjudicateSilentPeers(nowMs);
		RetryLateStartReclaims();
		SendCaptureParkReport(nowMs);
		if (m_Config.localPeerId == GetHostPeerId() && m_CaptureParkAwaitingReports && m_SynchronizedCaptureStartFrame != UINT64_MAX) {
			// A park still waiting for a report must never let the round reach the published end: past it the
			// survivors stop committing.  The window is extended ahead of the horizon until the park closes.
			if (m_CaptureParkDeadlineMs != 0 && nowMs >= m_CaptureParkDeadlineMs) PublishCapturePark(m_SynchronizedCaptureStartFrame);
		}
		TickTiming(nowMs);
		FlushDeferredParkTimings();
		AdvanceReadyFrames(nowMs);
		for (auto it = m_EvictAfterReclaim.begin(); it != m_EvictAfterReclaim.end();) {
			const auto reclaim = m_ReclaimTransactions.find(it->first);
			if (reclaim != m_ReclaimTransactions.end() && m_LastDeliveredFrame.value_or(0) < reclaim->second.activationFrame) { ++it; continue; }
			const auto [peer, message] = *it;
			it = m_EvictAfterReclaim.erase(it);
			EvictRemovedPeer(peer, message, nowMs);
		}
		EndRoundIfNobodyIsComingBack();
		ReclaimOwnSeat(nowMs);
	}

	void NetLockstepCoordinator::Complete(const std::string& message) {
		NET_PLANE_CHECK();
		if (m_State == NetLockstepState::Failed || m_State == NetLockstepState::Stopped || m_State == NetLockstepState::Idle) {
			return;
		}
		if (IsMigrating()) m_OwnEndDuringMigration = NetLockstepStop{m_Config.localPeerId, NetLockstepStopReason::Complete, 0, message};
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
		NET_PLANE_CHECK();
		if (m_State == NetLockstepState::Failed || m_State == NetLockstepState::Stopped || m_State == NetLockstepState::Idle) {
			return;
		}
		// The relay host is the star's hub — without it the survivors cannot exchange frames.
		if (m_RelayHost && m_RemotePeerIds.size() > 1 && m_Config.matchConfig.successorOrder.empty()) {
			Complete(message);
			return;
		}
		if (IsMigrating()) m_OwnEndDuringMigration = NetLockstepStop{m_Config.localPeerId, NetLockstepStopReason::PeerLeft, 0, message};
		if (m_Transport) {
			NetLockstepStop stop;
			stop.senderPeerId = m_Config.localPeerId;
			stop.reason = NetLockstepStopReason::PeerLeft;
			// The first frame WITHOUT our data: peers advance freely from here; zero when we never produced.
			stop.frame = m_LastQueuedTargetFrame == std::numeric_limits<uint64_t>::max() ? 0 : m_LastQueuedTargetFrame + 1;
			for (const auto& pending: m_RecoveryOutgoing) {
				if (pending.commitLocal) stop.frame = std::min(stop.frame, pending.frame.targetFrame);
			}
			stop.message = message;
			std::string ignored;
			(void)SendPacket({stop}, NetTransportLane::ControlReliable, &ignored);
		}
		m_State = NetLockstepState::Stopped;
		m_Stats.timeoutReason = std::string(NetLockstepCodec::StopReasonName(NetLockstepStopReason::PeerLeft)) + ":" + message;
	}

	void NetLockstepCoordinator::RequestResync(const std::string& message, bool immediate) {
		NET_PLANE_CHECK();
		if (m_State == NetLockstepState::Failed || m_State == NetLockstepState::Stopped || m_State == NetLockstepState::Idle) {
			return;
		}
		if (m_DeferStops && !immediate) {
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
		if (m_Config.localPeerId != GetHostPeerId()) {
			// A client requests the stop while continuing to supply the host's current tick.
			std::string ignored;
			(void)SendPacket({*m_PendingRecoveryStop}, NetTransportLane::ControlReliable, &ignored);
		}
	}

	bool NetLockstepCoordinator::FinishSimulationTick(uint64_t completedTick) {
		NET_PLANE_CHECK();
		if (IsRunning() && !m_Playback) for (auto& [revision, decision]: m_TimingDecisions) {
			const uint8_t local = static_cast<uint8_t>(1U << (m_Config.localPeerId - 1));
			if (decision.proposal.phase != NetTimingPhase::HoldAtFrame || completedTick < decision.proposal.applyFrame || (decision.acknowledgedPeers & local) != 0) continue;
			decision.acknowledgedPeers |= local;
			if (m_Config.localPeerId != GetHostPeerId()) {
				NetLockstepTiming ack = decision.proposal;
				ack.senderPeerId = m_Config.localPeerId; ack.phase = NetTimingPhase::HoldAppliedAck; ack.nextFrame = completedTick + 1;
				QueueTiming(ack, GetHostPeerId());
			}
		}
		FlushTimingOutgoing();
		// A tick the sim applied counts even once the round has failed: the heal resumes from it.
		if (IsRunning() || IsFailed()) m_LastCompletedSimulationTick = completedTick;
		if (IsRunning() && !m_Playback && IsSynchronizedCapturePark(completedTick)) {
			m_ParkFrameSimulated = completedTick;
			m_ParkFrameSimulatedMs = m_TimingNowMs != 0 ? m_TimingNowMs : NetLockstepNowMs();
		}
		if (!m_DeferStops) return false;
		if (!IsRunning()) return false;
		if (m_MigrationResult.snapshotProviderPeerId == m_Config.localPeerId && completedTick == m_MigrationResult.boundary + 1) {
			Fail(NetLockstepStopReason::ResyncRequested, completedTick + 1, "successor snapshot provider reached the boundary");
			return true;
		}
		if (m_PendingRecoveryStop && m_Config.localPeerId == GetHostPeerId()) {
			for (const auto& [peer, frame]: m_AiHeldSeats) if (frame > completedTick) return false;
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

	bool NetLockstepCoordinator::WaivePendingPeersWhileWaiting(uint64_t waitingTick) {
		NET_PLANE_CHECK();
		if (!m_DeferStops || !IsRunning() || !m_RelayHost || m_Config.localPeerId != GetHostPeerId() ||
		    !m_PendingRecoveryStop || m_Stats.nextFrame != waitingTick) {
			return false;
		}
		const uint64_t nowMs = NetLockstepNowMs();
		const auto remoteIt = m_RemoteFrames.find(waitingTick);
		bool waived = false;
		for (uint8_t peerId: m_RemotePeerIds) {
			if (!IsRemoteRequiredForFrame(peerId, waitingTick)) {
				continue;
			}
			if (remoteIt != m_RemoteFrames.end() && remoteIt->second.find(peerId) != remoteIt->second.end()) {
				continue;
			}
			// Only a superseded incarnation: a live peer that is merely slow still owes this frame.
			const auto transportIt = m_RemoteTransports.find(peerId);
			if (transportIt != m_RemoteTransports.end() && !SeatStateOf(peerId, transportIt->second).fencedTransport) {
				continue;
			}
			waived = WaiveRemoteFrames(peerId, waitingTick, nowMs, true) || waived;
		}
		if (waived) {
			AdvanceReadyFrames(nowMs);
		}
		return waived;
	}

	bool NetLockstepCoordinator::IsFrameWaiver(const NetLockstepStop& stop) {
		return stop.reason == NetLockstepStopReason::PeerDropped && stop.message.starts_with(c_FrameWaiverPrefix);
	}

	// The seat keeps everything a drop would take: the holder is on another connection and the resync
	// brings it into the next round. All the round gives up is this tick's input from the dead handle.
	bool NetLockstepCoordinator::WaiveRemoteFrames(uint8_t peerId, uint64_t fromFrame, uint64_t nowMs, bool announce) {
		const auto existing = m_PeerFrameWaivers.find(peerId);
		if (existing != m_PeerFrameWaivers.end() && existing->second <= fromFrame) {
			return false;
		}
		m_PeerFrameWaivers[peerId] = fromFrame;
		++m_Stats.peerFramesWaived;
		std::cout << "[net-match] " << DescribePeer(peerId) << "'s fenced connection is not owed frame " << fromFrame << std::endl;
		if (announce && m_RelayHost && m_Transport) {
			NetLockstepStop notice;
			notice.senderPeerId = peerId;
			notice.reason = NetLockstepStopReason::PeerDropped;
			notice.frame = fromFrame;
			notice.message = std::string(c_FrameWaiverPrefix) + std::to_string(peerId);
			RelayToOtherRemotes({notice}, peerId);
		}
		(void)nowMs;
		return true;
	}

	bool NetLockstepCoordinator::PopReadyFrame(NetLockstepReadyFrame& outFrame) {
		NET_PLANE_CHECK();
		if (NeedsMigrationSnapshot())
			return false;
		if (m_ReadyFrames.empty()) {
			return false;
		}
		outFrame = std::move(m_ReadyFrames.front());
		m_ReadyFrames.pop_front();
		m_LastDeliveredFrame = outFrame.frame;
		// Who drives the seats the AI holds must read the same on every peer at every frame; a change is named with its frame.
		if (const uint8_t authority = AiAuthorityAt(outFrame.frame); authority != m_NamedAiAuthority) {
			std::cout << "[net-lockstep] AI authority frame=" << outFrame.frame << " peer=" << static_cast<int>(authority) << " was=" << static_cast<int>(m_NamedAiAuthority) << std::endl;
			m_NamedAiAuthority = authority;
		}
		if (m_Playback || IsMigrationCatchUp() || m_Config.resumeFromSnapshot) {
			for (const auto* commands: {&outFrame.localCommands, &outFrame.remoteCommands}) for (const auto& command: *commands) {
				if (const auto* delay = std::get_if<NetGameInputDelay>(&command.payload)) {
					if (command.senderPeerId != GetHostPeerId() || delay->peerId == 0 || delay->peerId > m_Config.peerCount) {
						Fail(NetLockstepStopReason::ProtocolError, outFrame.frame, "recorded delay has invalid authority");
						return false;
					}
					m_DelayChanges[delay->peerId][outFrame.frame] = delay->frames;
				}
			}
		}
		bool changedDelay = false;
		for (const auto& [peer, changes]: m_DelayChanges) changedDelay = changedDelay || changes.contains(outFrame.frame);
		if (changedDelay) {
			if (m_Config.matchConfig.configRevision == UINT64_MAX) { Fail(NetLockstepStopReason::ProtocolError, outFrame.frame, "live configuration revision exhausted"); return false; }
			m_Config.matchConfig.peerInputDelayFrames.resize(m_Config.peerCount);
			for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) {
				uint16_t delay = m_Playback ? NetMatchConfigUtil::PeerInputDelay(m_OpeningMatchConfig, peer) : InputDelayAt(peer, outFrame.frame);
				if (m_Playback) if (const auto changes = m_DelayChanges.find(peer); changes != m_DelayChanges.end()) {
					auto at = changes->second.upper_bound(outFrame.frame);
					if (at != changes->second.begin()) delay = std::prev(at)->second;
				}
				m_Config.matchConfig.peerInputDelayFrames[peer - 1] = delay;
			}
			++m_Config.matchConfig.configRevision;
			if (m_Config.publishLiveConfig) m_Config.publishLiveConfig(m_Config.matchConfig);
		}
		if (m_Playback || IsMigrationCatchUp() || m_Config.resumeFromSnapshot) {
			for (const auto* commands: {&outFrame.localCommands, &outFrame.remoteCommands}) {
				for (const NetGameCommand& command: *commands) {
					if (const auto* hold = std::get_if<NetGameSeatHold>(&command.payload)) {
						if (command.senderPeerId != GetHostPeerId() || hold->peerId == 0 || hold->peerId > m_Config.peerCount) {
							Fail(NetLockstepStopReason::ProtocolError, outFrame.frame, "recorded seat hold has invalid authority");
							return false;
						}
						m_AiHeldSeats[hold->peerId] = outFrame.frame;
						m_HoldTransactions[hold->peerId] = *hold;
						if (hold->peerId != GetHostPeerId()) m_PeerLeaveFrames[hold->peerId] = outFrame.frame;
						m_DroppedSeatResolutions[hold->peerId] = NetLockstepHoldResolution::Substituted;
					}
				}
			}
		}
		for (const auto* commands: {&outFrame.localCommands, &outFrame.remoteCommands}) for (const auto& command: *commands) {
			if (const auto* reclaim = std::get_if<NetGameSeatReclaim>(&command.payload)) {
				if (command.senderPeerId != GetHostPeerId() || reclaim->peerId == 0 || reclaim->peerId > m_Config.peerCount || reclaim->activationFrame != outFrame.frame) {
					Fail(NetLockstepStopReason::ProtocolError, outFrame.frame, "recorded reclaim has invalid authority or frame"); return false;
				}
				if (const auto held = m_ReclaimTransactions.find(reclaim->peerId); held == m_ReclaimTransactions.end() || !(held->second == *reclaim))
					std::cout << "[net-lockstep] return of peer " << static_cast<int>(reclaim->peerId) << " committed at " << outFrame.frame << " delay=" << reclaim->delayFrames
					          << " neutral_through=" << reclaim->neutralThroughFrame << " revision=" << reclaim->eventSequence << " differs from the one this peer held" << std::endl;
				m_ReclaimTransactions[reclaim->peerId] = *reclaim;
				m_Config.peerIncarnations[reclaim->peerId] = reclaim->seatIncarnation;
				m_AiHeldSeats.erase(reclaim->peerId); m_ReleasedAiSeats.erase(reclaim->peerId); m_HoldTransactions.erase(reclaim->peerId);
				m_PeerLeaveFrames.erase(reclaim->peerId); m_PeerFrameWaivers.erase(reclaim->peerId);
				m_DroppedSeats.erase(reclaim->peerId); m_LeftSeatsHeld.erase(reclaim->peerId); m_DroppedAtMs.erase(reclaim->peerId);
				m_DroppedSeatResolutions[reclaim->peerId] = NetLockstepHoldResolution::Reclaimed;
				m_PeerEffectiveStart[reclaim->peerId] = outFrame.frame + reclaim->delayFrames;
				m_PeerAdmissions[reclaim->peerId] = {outFrame.frame, reclaim->delayFrames};
				outFrame.reclaimedPeerIds.push_back(reclaim->peerId);
				++m_Stats.peers[reclaim->peerId].rejoins;
				NoteSeatReclaimed(reclaim->peerId);
			}
		}
		for (const auto& [peer, frame]: m_AiHeldSeats) {
			if (frame != outFrame.frame) continue;
			if (m_Playback || m_DroppedAtMs.contains(peer)) ++m_Stats.peers[peer].substitutions;
			if (std::find(outFrame.aiHeldPeerIds.begin(), outFrame.aiHeldPeerIds.end(), peer) == outFrame.aiHeldPeerIds.end()) outFrame.aiHeldPeerIds.push_back(peer);
		}
		if (IsMigrationCatchUp()) {
			if (outFrame.committedPeerLeaves.contains(m_Config.localPeerId) && outFrame.committedPeerLeaves.at(m_Config.localPeerId) <= outFrame.frame) {
				m_MigrationNeedsResync = true;
				return false;
			}
			m_PeerLeaveFrames = outFrame.committedPeerLeaves;
			m_PeerFrameWaivers = outFrame.committedFrameWaivers;
			m_DroppedSeats.clear();
			m_LeftSeatsHeld.clear();
			m_DroppedSeatResolutions.clear();
			for (const auto& [peer, frame]: m_AiHeldSeats) {
				m_DroppedSeats.insert(peer);
				m_LeftSeatsHeld.insert(peer);
				m_DroppedSeatResolutions[peer] = NetLockstepHoldResolution::Substituted;
			}
		} else {
			outFrame.committedPeerLeaves = m_PeerLeaveFrames;
			outFrame.committedFrameWaivers = m_PeerFrameWaivers;
		}
		for (const auto& [peerId, leaveFrame]: m_PeerLeaveFrames) {
			if (leaveFrame <= outFrame.frame && std::find(outFrame.departedPeerIds.begin(), outFrame.departedPeerIds.end(), peerId) == outFrame.departedPeerIds.end())
				outFrame.departedPeerIds.push_back(peerId);
		}
		std::sort(outFrame.departedPeerIds.begin(), outFrame.departedPeerIds.end());
		RetainMigrationFrame(outFrame);
		return true;
	}

	bool NetLockstepCoordinator::PeekLocalFrames(uint64_t frame, std::vector<ControllerFrame>& outFrames) const {
		NET_PLANE_CHECK();
		const auto found = m_LocalFrames.find(frame);
		if (found == m_LocalFrames.end()) {
			return false;
		}
		outFrames = found->second;
		return true;
	}

	void NetLockstepCoordinator::RememberAppliedFrameInputs(const NetLockstepReadyFrame& ready) {
		NET_PLANE_CHECK();
		if (!m_LastDeliveredFrame || ready.frame != *m_LastDeliveredFrame) return;
		m_ReadyHistory[ready.frame] = ready;
		RetainMigrationFrame(ready);
	}

	bool NetLockstepCoordinator::PeekReadyFrame(uint64_t frame, NetLockstepReadyFrame& outFrame) const {
		NET_PLANE_CHECK();
		for (const NetLockstepReadyFrame& ready: m_ReadyFrames) {
			if (ready.frame == frame) {
				outFrame = ready;
				return true;
			}
		}
		const auto found = m_ReadyHistory.find(frame);
		if (found == m_ReadyHistory.end()) {
			return false;
		}
		outFrame = found->second;
		return true;
	}

	bool NetLockstepCoordinator::PeekQueuedCommands(uint64_t frame, uint8_t peerId, std::vector<NetGameCommand>& outCommands) const {
		NET_PLANE_CHECK();
		outCommands.clear();
		if (peerId == m_Config.localPeerId) {
			const auto found = m_LocalCommands.find(frame);
			if (found == m_LocalCommands.end()) {
				return false;
			}
			outCommands = found->second;
			return true;
		}
		const auto frameIt = m_RemoteCommands.find(frame);
		if (frameIt == m_RemoteCommands.end()) {
			return false;
		}
		const auto peerIt = frameIt->second.find(peerId);
		if (peerIt == frameIt->second.end()) {
			return false;
		}
		outCommands = peerIt->second;
		return true;
	}

	uint8_t NetLockstepCoordinator::ResolveActorOwnerBeforeLeaves(int64_t actorUniqueID, int actorTeam, bool cpuControlled) const {
		NET_PLANE_CHECK();
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
		NET_PLANE_CHECK();
		uint8_t ownerPeerId = ResolveActorOwnerBeforeLeaves(actorUniqueID, actorTeam, cpuControlled);
		if (m_Config.peerCount == 0 || m_Config.localPeerId == 0 || m_Config.matchConfig.players.empty()) {
			return ownerPeerId;
		}
		const uint8_t team = actorTeam < 0 ? 0 : static_cast<uint8_t>(actorTeam);
		if (m_LastDeliveredFrame && IsSeatUnderAI(ownerPeerId, *m_LastDeliveredFrame)) return AiAuthorityAt(*m_LastDeliveredFrame);
		// A leaver's team falls to its next surviving human peer, so the units play on. The lockstep gate synchronizes leave
		// knowledge, so every peer re-resolves identically - except for a leave heard before its frame: the leaver's own
		// inputs drive its units until that frame is committed, so the round re-resolves them there.
		const auto leave = m_PeerLeaveFrames.find(ownerPeerId);
		const bool heardAhead = leave != m_PeerLeaveFrames.end() && m_LeavesHeardAhead.contains(ownerPeerId) &&
		                        (!m_LastDeliveredFrame || *m_LastDeliveredFrame < leave->second);
		if (UsesBoundedWait() || m_Playback ? m_LastDeliveredFrame && IsPeerGoneAtFrame(ownerPeerId, *m_LastDeliveredFrame)
		                                 : leave != m_PeerLeaveFrames.end() && !heardAhead) {
			const uint8_t survivor = FirstAliveHumanPeerForTeam(team, std::numeric_limits<uint64_t>::max());
			// The host produces AI controllers for a departed team while the round continues.
			ownerPeerId = survivor != 0 ? survivor : (IsRunning() || IsHoldingSeatForReclaim() ? GetHostPeerId() : survivor);
		}
		return AiProducerOf(ownerPeerId);
	}

	uint8_t NetLockstepCoordinator::AiAuthorityAt(uint64_t frame) const {
		NET_PLANE_CHECK();
		const uint8_t host = GetHostPeerId();
		if (!IsSeatUnderAI(host, frame)) return host;
		const auto playing = [&](uint8_t peer) { return peer != host && peer != 0 && peer <= m_Config.peerCount && !IsPeerGoneAtFrame(peer, frame) && !IsSeatUnderAI(peer, frame); };
		for (uint8_t peer: m_Config.matchConfig.successorOrder) if (playing(peer)) return peer;
		for (uint8_t peer = 1; peer <= m_Config.peerCount; ++peer) if (playing(peer)) return peer;
		return host;
	}

	uint8_t NetLockstepCoordinator::AiProducerOf(uint8_t ownerPeerId) const {
		NET_PLANE_CHECK();
		return ownerPeerId == GetHostPeerId() && m_LastDeliveredFrame ? AiAuthorityAt(*m_LastDeliveredFrame) : ownerPeerId;
	}

	bool NetLockstepCoordinator::IsOwnHostSeatHeld() const {
		NET_PLANE_CHECK();
		const uint8_t local = m_Config.localPeerId;
		return local == GetHostPeerId() && m_AiHeldSeats.contains(local);
	}

	bool NetLockstepCoordinator::JudgeOwnSeat(uint64_t frame, uint64_t nowMs) {
		const uint8_t local = m_Config.localPeerId;
		if (!UsesBoundedWait() || local != GetHostPeerId() || m_GoodbyeDrain || frame > m_FinalFrame || IsOwnHostSeatHeld() || m_RemotePeerIds.empty()) return false;
		// The host's start work, a park every peer is in, and a capture it announced excuse its ticks exactly as they excuse a client's.
		if (frame <= EffectiveStartOf(local) + std::max<uint64_t>(m_Config.slowPlayerBoundTicks, c_StartupSettleTicks) || IsSynchronizedCapturePark(frame) ||
		    m_CaptureParkAwaitingReports || TimingDecisionPendingAt(frame) || HostBusyWithAnnouncedCapture(frame)) {
			m_OwnMissingFrame.reset();
			return false;
		}
		// Only a seat the others are waiting on is late: the clock runs from the moment every other seat's input for the frame is in.
		const auto remote = m_RemoteFrames.find(frame);
		for (uint8_t peer: m_RemotePeerIds) {
			if (IsRemoteRequiredForFrame(peer, frame) && (remote == m_RemoteFrames.end() || !remote->second.contains(peer))) {
				m_OwnMissingFrame.reset();
				return false;
			}
		}
		if (m_OwnMissingFrame != frame) {
			m_OwnMissingFrame = frame;
			m_OwnMissingSinceMs = nowMs;
		}
		const uint64_t boundMs = static_cast<uint64_t>(std::max(1.0, std::floor(m_Config.slowPlayerBoundTicks * m_Config.simTickMs)));
		if (nowMs < m_OwnMissingSinceMs || nowMs - m_OwnMissingSinceMs < boundMs) return false;
		const uint64_t simTickedMs = m_SimTickedMs.load(std::memory_order_acquire);
		std::cout << "[net-lockstep] own seat late at frame " << frame << ": missing_ms=" << (nowMs - m_OwnMissingSinceMs) << " sim_quiet_ms="
		          << (simTickedMs != 0 && nowMs >= simTickedMs ? nowMs - simTickedMs : 0) << " sent_through=" << SentInputThrough() << " plane=" << m_PlaneTicking << std::endl;
		std::string holdError;
		if (!ProposePeerHold(local, nowMs, &holdError)) {
			std::cout << "[net-lockstep] hold refused peer=" << static_cast<int>(local) << ": " << holdError << std::endl;
			return false;
		}
		m_OwnMissingFrame.reset();
		return true;
	}

	void NetLockstepCoordinator::ReclaimOwnSeat(uint64_t nowMs) {
		const uint8_t local = m_Config.localPeerId;
		if (m_PlaneTicking || !IsRunning() || !IsOwnHostSeatHeld() || m_ReclaimTransactions.contains(local) || !m_LastCompletedSimulationTick || m_NextTimingRevision == UINT64_MAX) return;
		for (const auto& [revision, pending]: m_TimingDecisions) if (!pending.committed) return;
		// Back once the simulation has replayed what the AI played and is producing for frames not yet committed.
		const uint16_t delay = InputDelayAt(local, m_Stats.nextFrame);
		if (*m_LastCompletedSimulationTick + delay + 1 < m_Stats.nextFrame) return;
		NetLockstepTiming timing;
		timing.senderPeerId = local; timing.peerId = local;
		timing.action = NetTimingAction::Reclaim;
		timing.phase = NetTimingPhase::ReclaimAtFrame;
		timing.sessionId = m_Config.sessionId; timing.roundId = m_RoundId;
		timing.authorityGeneration = m_Config.migrationGeneration;
		timing.revision = m_NextTimingRevision++;
		uint64_t applyFrame = std::max(m_Stats.nextFrame, SentInputThrough() + 1) + 1;
		for (uint8_t peer: m_RemotePeerIds) if (!IsPeerGoneAtFrame(peer, applyFrame)) applyFrame = std::max(applyFrame, m_Stats.peers[peer].reportedNextFrame + 1);
		timing.applyFrame = timing.cutoffFrame = applyFrame;
		timing.delayFrames = delay;
		timing.neutralThroughFrame = applyFrame + delay;
		timing.nextFrame = m_Stats.nextFrame;
		timing.heldPeers = static_cast<uint8_t>(1U << (local - 1));
		const auto incarnation = m_Config.peerIncarnations.find(local);
		timing.seatIncarnations[local - 1] = incarnation == m_Config.peerIncarnations.end() ? 1 : incarnation->second;
		for (uint8_t peer: m_RemotePeerIds) if (!IsPeerGoneAtFrame(peer, applyFrame)) timing.requiredPeers |= static_cast<uint8_t>(1U << (peer - 1));
		std::cout << "[net-lockstep] own seat back at frame " << applyFrame << " delay=" << delay << " applied_through=" << *m_LastCompletedSimulationTick
		          << " next_frame=" << m_Stats.nextFrame << " held_from=" << m_AiHeldSeats.at(local) << std::endl;
		m_TimingDecisions[timing.revision] = {timing, 0, true, nowMs};
		QueueTiming(timing);
		FlushTimingOutgoing();
		ApplyTiming(timing);
	}

	bool NetLockstepCoordinator::IsLocalActor(int64_t actorUniqueID, int actorTeam, bool cpuControlled) const {
		NET_PLANE_CHECK();
		if (m_Config.peerCount == 0 || m_Config.localPeerId == 0) {
			return true;
		}
		return ResolveActorOwner(actorUniqueID, actorTeam, cpuControlled) == m_Config.localPeerId;
	}

	uint8_t NetLockstepCoordinator::ResolveTeamCommandAuthority(int team) const {
		NET_PLANE_CHECK();
		if (team < 0) {
			return 0;
		}
		if (m_Config.matchConfig.successorOrder.empty())
			return NetActorOwnership::ResolveTeamCommandAuthority(m_Config.matchConfig, static_cast<uint8_t>(team));
		const uint8_t human = FirstAliveHumanPeerForTeam(static_cast<uint8_t>(team), GetResumeFrame());
		return human != 0 ? human : GetHostPeerId();
	}

	bool NetLockstepCoordinator::IsActorOwnerGone(int64_t actorUniqueID, int actorTeam, bool cpuControlled, uint64_t frame) const {
		NET_PLANE_CHECK();
		if (m_PeerLeaveFrames.empty() || m_Config.matchConfig.players.empty()) {
			return false;
		}
		const uint8_t team = actorTeam < 0 ? 0 : static_cast<uint8_t>(actorTeam);
		if (!IsPeerGoneAtFrame(NetActorOwnership::ResolveOwnerPeer(m_Config.matchConfig, {actorUniqueID, team, cpuControlled}), frame)) {
			return false;
		}
		return FirstAliveHumanPeerForTeam(team, frame) == 0 && !IsRunning() && !IsHoldingSeatForReclaim();
	}

	bool NetLockstepCoordinator::IsPeerGoneAtFrame(uint8_t peerId, uint64_t frame) const {
		NET_PLANE_CHECK();
		if (peerId == m_Config.localPeerId && !m_Playback && !m_LocalSeatHeld && !m_AiHeldSeats.contains(peerId)) {
			return false;
		}
		if (const auto reclaim = m_ReclaimTransactions.find(peerId); reclaim != m_ReclaimTransactions.end() && frame >= reclaim->second.activationFrame) return false;
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
		NET_PLANE_CHECK();
		for (const NetMatchPlayerSlot& slot: m_Config.matchConfig.players) {
			if (slot.peerId == peerId && !slot.displayName.empty()) {
				return slot.displayName;
			}
		}
		return "peer " + std::to_string(peerId);
	}

	std::string NetLockstepCoordinator::DescribeMissingPeers() const {
		NET_PLANE_CHECK();
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

	bool NetLockstepCoordinator::CommitsRemoteInput(uint8_t peerId, uint64_t frame) const {
		// A seat the AI holds at the frame contributes nothing, whatever of its input landed anyway.
		if (IsSeatReclaimGap(peerId, frame) || IsSeatUnderAI(peerId, frame)) return false;
		// A returning seat contributes from the start it was admitted on, never whatever it sent earlier and happened to land in time:
		// every peer commits the same set of senders at the frame.
		const auto reclaim = m_ReclaimTransactions.find(peerId);
		return reclaim == m_ReclaimTransactions.end() || frame < reclaim->second.activationFrame || frame >= EffectiveStartOf(peerId);
	}

	bool NetLockstepCoordinator::IsRemoteRequiredForFrame(uint8_t peerId, uint64_t frame) const {
		// Ramp-in: a sender contributes nothing before its own first delayed frame.
		if (frame < EffectiveStartOf(peerId) || IsSeatReclaimGap(peerId, frame)) {
			return false;
		}
		const auto waiverIt = m_PeerFrameWaivers.find(peerId);
		if (waiverIt != m_PeerFrameWaivers.end() && frame >= waiverIt->second) {
			return false;
		}
		const auto leaveIt = m_PeerLeaveFrames.find(peerId);
		return leaveIt == m_PeerLeaveFrames.end() || frame < leaveIt->second ||
		    (m_ReclaimTransactions.contains(peerId) && frame >= m_ReclaimTransactions.at(peerId).activationFrame);
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
		NET_PLANE_CHECK();
		std::ostringstream out;
		out << "{";
		out << "\"state\":\"" << StateName(m_State) << "\",";
		out << "\"session_id\":" << m_Stats.sessionId << ",";
		out << "\"local_peer_id\":" << static_cast<int>(m_Stats.localPeerId) << ",";
		out << "\"host_peer_id\":" << static_cast<int>(GetHostPeerId()) << ",";
		out << "\"migration_generation\":" << m_MigrationGeneration << ",";
		out << "\"migration_history_bytes\":" << m_MigrationHistoryBytes << ",";
		out << "\"migration_boundary\":" << m_MigrationResult.boundary << ",";
		out << "\"migration_phase\":" << static_cast<int>(m_MigrationPhase) << ",";
		out << "\"remote_peer_id\":" << static_cast<int>(m_Stats.remotePeerId) << ",";
		out << "\"configured_start_frame\":" << m_Stats.configuredStartFrame << ",";
		out << "\"effective_start_frame\":" << m_Stats.effectiveStartFrame << ",";
		out << "\"input_delay_frames\":" << m_Stats.inputDelayFrames << ",";
		out << "\"peer_input_delays\":{";
		for (uint8_t peerId = 1; peerId <= m_Config.peerCount; ++peerId) {
			out << (peerId == 1 ? "" : ",") << "\"" << static_cast<int>(peerId) << "\":" << InputDelayAt(peerId, m_LastDeliveredFrame.value_or(m_Config.startFrame));
		}
		out << "},";
		out << "\"next_frame\":" << m_Stats.nextFrame << ",";
		out << "\"completed_simulation_tick\":" << (m_LastCompletedSimulationTick ? std::to_string(*m_LastCompletedSimulationTick) : "null") << ",";
		out << "\"ready_frames_pending\":" << m_ReadyFrames.size() << ",";
		out << "\"start_packets_sent\":" << m_Stats.startPacketsSent << ",";
		out << "\"start_packets_received\":" << m_Stats.startPacketsReceived << ",";
		out << "\"frame_packets_sent\":" << m_Stats.framePacketsSent << ",";
		out << "\"frame_packets_received\":" << m_Stats.framePacketsReceived << ",";
		out << "\"ignored_session_packets\":" << m_Stats.ignoredSessionPackets << ",";
		out << "\"stale_round_packets\":" << m_Stats.staleRoundPackets << ",";
		out << "\"start_retransmits\":" << m_Stats.startRetransmits << ",";
		out << "\"start_answers\":" << m_Stats.startAnswers << ",";
		out << "\"round_readoptions\":" << m_Stats.roundReadoptions << ",";
		uint32_t publishedStartMask = m_LocalStartupPublished ? (uint32_t{1} << (m_Config.localPeerId - 1)) : 0;
		for (uint8_t peer: m_PeerStartupPublished) if (peer > 0 && peer <= 32) publishedStartMask |= uint32_t{1} << (peer - 1);
		out << "\"local_startup_published\":" << (m_LocalStartupPublished ? "true" : "false") << ",";
		out << "\"published_start_mask\":" << publishedStartMask << ",";
		out << "\"agreed_start_applied\":" << (m_AgreedStartApplied ? "true" : "false") << ",";
		out << "\"start_wait_announced\":" << (m_StartWaitAnnounced ? "true" : "false") << ",";
		out << "\"start_wait_since_ms\":" << m_StartWaitSinceMs << ",";
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
		out << "\"frame_binding_gap_drops\":" << m_Stats.frameBindingGapDrops << ",";
		out << "\"future_frame_drops\":" << m_Stats.futureFrameDrops << ",";
		out << "\"missing_frame_stalls\":" << m_Stats.missingFrameStalls << ",";
		out << "\"blocking_frame_waits\":" << m_Stats.blockingFrameWaits << ",";
		out << "\"hold_notice_budget_ms\":" << m_Stats.holdNoticeBudgetMs << ",";
		out << "\"last_hold_declaration_ms\":" << m_Stats.lastHoldDeclarationMs << ",";
		out << "\"own_parks_excluded\":" << m_Stats.ownParksExcluded << ",";
		out << "\"park_frames\":" << m_Stats.parkFramesCommitted << ",\"park_frames_with_input\":" << m_Stats.parkFramesWithInput << ",";
		out << "\"frame_resend_requests\":" << m_Stats.frameResendRequests << ",\"frames_resent\":" << m_Stats.framesResent << ",";
		out << "\"hold_deadline_feasible\":" << (m_Stats.holdDeadlineFeasible ? "true" : "false") << ",";
		out << "\"steady_missing_frame_stalls\":" << (m_Stats.measuredMissingFrameBase ? std::to_string(m_Stats.missingFrameStalls - *m_Stats.measuredMissingFrameBase) : "null") << ",";
		out << "\"steady_blocking_frame_waits\":" << (m_Stats.measuredBlockingWaitBase ? std::to_string(m_Stats.blockingFrameWaits - *m_Stats.measuredBlockingWaitBase) : "null") << ",";
		out << "\"delay_changes_proposed\":" << m_Stats.delayChangesProposed << ",";
		out << "\"delay_changes_committed\":" << m_Stats.delayChangesCommitted << ",";
		out << "\"delay_padding_frames\":" << m_Stats.delayPaddingFrames << ",";
		out << "\"delay_deferred_samples\":" << m_Stats.delayDeferredSamples << ",";
		out << "\"local_tick_overruns\":" << m_Stats.localTickOverruns << ",";
		out << "\"local_late_inputs\":" << m_Stats.localLateInputs << ",";
		out << "\"local_consecutive_late_inputs\":" << m_Stats.consecutiveLateInputs << ",";
		out << "\"local_compute_debt_ms\":" << m_Stats.localComputeDebtMs << ",";
		out << "\"local_production_late_ms\":" << m_Stats.localProductionLateMs << ",";
		out << "\"local_machine_slow\":" << (m_Stats.localMachineSlow ? "true" : "false") << ",";
		out << "\"slow_player_bound_ticks\":" << m_Config.slowPlayerBoundTicks << ",";
		out << "\"sim_tick_ms\":" << m_Config.simTickMs << ",";
		out << "\"longest_stall_ms\":" << m_Stats.longestStallMs << ",";
		out << "\"last_missing_peers\":\"" << EscapeJson(m_Stats.lastMissingPeers) << "\",";
		out << "\"relay_packets_sent\":" << m_Stats.relayPacketsSent << ",";
		out << "\"relay_send_failures\":" << m_Stats.relaySendFailures << ",";
		out << "\"relay_resends\":" << m_Stats.relayResends << ",";
		out << "\"relay_congested_refusals\":" << m_Stats.relayCongestedRefusals << ",";
		out << "\"relay_congestion_holds\":" << m_Stats.relayCongestionHolds << ",";
		out << "\"relay_longest_congestion_hold_ms\":" << m_Stats.longestCongestionHoldMs << ",";
		out << "\"relay_congestion_hold_leave_ms\":" << CongestionHoldLeaveMs() << ",";
		out << "\"relay_backlog_overflows\":" << m_Stats.relayBacklogOverflows << ",";
		out << "\"relay_backlog_peers\":" << m_RelayBacklog.size() << ",";
		out << "\"relay_bytes_sent\":" << m_Stats.relayBytesSent << ",";
		out << "\"largest_relay_packet_bytes\":" << m_Stats.largestRelayPacketBytes << ",";
		out << "\"relay_backlog_bytes\":" << m_Stats.relayBacklogBytes << ",";
		out << "\"observations_carried\":" << m_Stats.observationsCarried << ",";
		out << "\"observations_dropped\":" << m_Stats.observationsDropped << ",";
		out << "\"value_observations_carried\":" << m_Stats.valueObservationsCarried << ",";
		out << "\"value_observations_dropped\":" << m_Stats.valueObservationsDropped << ",";
		out << "\"unresolved_observation_packets\":" << m_Stats.unresolvedObservationPackets << ",";
		out << "\"relay_observation_overflows\":" << m_Stats.relayObservationOverflows << ",";
		out << "\"last_relay_error\":\"" << EscapeJson(m_Stats.lastRelayError) << "\",";
		out << "\"peer_silence_leave_ms\":" << PeerSilenceLeaveMs() << ",";
		out << "\"peers_dropped_silent\":" << m_Stats.peersDroppedSilent << ",";
		out << "\"stops_from_left_peers\":" << m_Stats.stopsFromLeftPeers << ",";
		out << "\"stops_adjudicated_as_leaves\":" << m_Stats.stopsAdjudicatedAsLeaves << ",";
		out << "\"peer_frames_waived\":" << m_Stats.peerFramesWaived << ",";
		out << "\"connections_closed_on_eviction\":" << m_Stats.connectionsClosedOnEviction << ",";
		out << "\"peers_left\":" << m_PeerLeaveFrames.size() << ",";
		out << "\"peer_leave_frames\":{";
		for (auto it = m_PeerLeaveFrames.begin(); it != m_PeerLeaveFrames.end(); ++it) {
			out << (it == m_PeerLeaveFrames.begin() ? "" : ",") << "\"" << static_cast<int>(it->first) << "\":" << it->second;
		}
		out << "},\"ai_held_peer_ids\":[";
		for (auto it = m_AiHeldSeats.begin(); it != m_AiHeldSeats.end(); ++it) {
			out << (it == m_AiHeldSeats.begin() ? "" : ",") << static_cast<int>(it->first);
		}
		out << "],\"reclaim_activation_frames\":{";
		for (auto it = m_ReclaimTransactions.begin(); it != m_ReclaimTransactions.end(); ++it) {
			out << (it == m_ReclaimTransactions.begin() ? "" : ",") << "\"" << static_cast<int>(it->first) << "\":" << it->second.activationFrame;
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
			    << ",\"relay_backlog_overflows\":" << peer.relayBacklogOverflows
			    << ",\"longest_congestion_hold_ms\":" << peer.longestCongestionHoldMs
			    << ",\"last_relay_error\":\"" << EscapeJson(peer.lastRelayError) << "\""
			    << ",\"relay_bytes_sent\":" << peer.relayBytesSent
			    << ",\"largest_relay_packet_bytes\":" << peer.largestRelayPacketBytes
			    << ",\"relay_backlog_packets\":" << RelayBacklogPackets(it->first)
			    << ",\"highest_target_frame\":" << peer.highestTargetFrame
			    << ",\"accepted_through_frame\":" << peer.acceptedThroughFrame
			    << ",\"ping_ms\":" << peer.pingMs << ",\"jitter_ms\":" << peer.jitterMs
			    << ",\"delay_frames\":" << InputDelayAt(it->first, m_LastDeliveredFrame.value_or(m_Config.startFrame))
			    << ",\"waits\":" << peer.waits << ",\"longest_wait_ms\":" << peer.longestWaitMs
			    << ",\"holds\":" << peer.holds << ",\"substitutions\":" << peer.substitutions << ",\"rejoins\":" << peer.rejoins
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

	bool NetLockstepCoordinator::SendPacket(const NetLockstepPacket& packet, NetTransportLane lane, std::string* error, NetSoundObservationDictionary* dictionary, size_t* outObservationsEncoded, uint8_t onlyPeerId, size_t* outValueObservationsEncoded, NetLockstepObservationBlocks* blocks) {
		std::vector<uint8_t> windowBytes;
		std::vector<uint8_t> classicBytes;
		NetLockstepError encodeError;
		const NetLockstepFrame* frame = std::get_if<NetLockstepFrame>(&packet.payload);
		bool windowed = frame && !frame->priorWindow.empty() && blocks;
		NetLockstepFrame classic;
		if (windowed) {
			classic = *frame;
			classic.priorWindow.clear();
		}
		// Only the classic packet spends the dictionary, so a peer without the capability commits what the advertised peers commit.
		if (!NetLockstepCodec::Encode(windowed ? NetLockstepPacket{classic} : packet, classicBytes, &encodeError, dictionary, outObservationsEncoded, outValueObservationsEncoded, blocks)) {
			if (error) *error = encodeError.message;
			return false;
		}
		// The redundancy is what a peer can do without; the tick is not.
		if (windowed && !NetLockstepCodec::Encode(packet, windowBytes, &encodeError, nullptr, nullptr, nullptr, blocks)) {
			windowBytes.clear();
			windowed = false;
		}
		auto bytesFor = [&](uint8_t peerId) -> const std::vector<uint8_t>& {
			return windowed && FrameWindowAgreedFor(peerId) ? windowBytes : classicBytes;
		};
		// Send to every remote peer's transport (a set of one in the 2-peer case), or to just the one asked for.
		for (const auto& [peerId, transportId]: m_RemoteTransports) {
			if (onlyPeerId != 0 && peerId != onlyPeerId) {
				continue;
			}
			const std::vector<uint8_t>& bytes = bytesFor(peerId);
			// Only a frame has a reserved byte there; another payload's byte 1 means something else.
			if (frame && bytes.size() > NetLockstepCodec::c_HeaderBytes + 1) {
				m_Stats.peers[peerId].lastFrameReserved = bytes[NetLockstepCodec::c_HeaderBytes + 1];
			}
			const NetTransportLane destinationLane = frame && lane == m_Config.frameLane ? LaneTo(peerId, packet, lane) : lane;
			// Behind an undrained backlog, or this peer's stream would arrive out of order. An unreliable frame has no order to keep.
			const bool backlogged = lane == m_Config.frameLane && destinationLane == NetTransportLane::ControlReliable && [&] {
				const auto backlogIt = m_RelayBacklog.find(peerId);
				return (backlogIt != m_RelayBacklog.end() && !backlogIt->second.empty()) || m_TimingOutgoing.contains(peerId);
			}();
			if (frame && destinationLane == NetTransportLane::InputUnreliable && TestBlipDropsFrameSend(frame->targetFrame)) {
				continue;
			}
			std::string sendError;
			bool congested = false;
			if (!backlogged && m_Transport->Send(transportId, destinationLane, bytes, &sendError, &congested)) {
				continue;
			}
			// A refused unreliable frame is what the window is for: the next packet repeats it.
			if (!backlogged && destinationLane == NetTransportLane::InputUnreliable) {
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
				NoteRelayError(peerId, sendError);
				NoteRelayRefusal(peerId, congested);
			}
			if (lane == m_Config.frameLane && destinationLane == NetTransportLane::ControlReliable) {
				QueueRelayBacklog(peerId, bytes);
			}
		}
		return true;
	}

	// Test lever: CC_TEST_LOCKSTEP_BLIP_FRAME / _MS drop this peer's unreliable frame sends for a wall-clock window from that frame on, once.
	bool NetLockstepCoordinator::TestBlipDropsFrameSend(uint64_t targetFrame) {
		static const auto blipFrame = TestFrameFromEnvironment("CC_TEST_LOCKSTEP_BLIP_FRAME");
		static const auto blipMs = TestFrameFromEnvironment("CC_TEST_LOCKSTEP_BLIP_MS");
		static uint64_t untilMs = 0;
		static uint64_t drops = 0;
		if (!blipFrame || !blipMs || m_Playback) return false;
		const uint64_t nowMs = NetLockstepNowMs();
		if (untilMs == 0 && targetFrame >= *blipFrame) {
			untilMs = nowMs + *blipMs;
			std::cout << "[lockstep-test] blip from frame=" << targetFrame << " ms=" << *blipMs << std::endl;
		}
		if (untilMs == 0 || untilMs == UINT64_MAX) return false;
		if (nowMs < untilMs) {
			++drops;
			return true;
		}
		std::cout << "[lockstep-test] blip ended at frame=" << targetFrame << " dropped_sends=" << drops << std::endl;
		untilMs = UINT64_MAX;
		return false;
	}

	bool NetLockstepCoordinator::IsKnownRemotePeer(uint8_t peerId) const {
		return std::find(m_RemotePeerIds.begin(), m_RemotePeerIds.end(), peerId) != m_RemotePeerIds.end();
	}

	bool NetLockstepCoordinator::AdmitWorldMember(uint8_t peerId, NetPeerId transportPeerId, uint64_t firstRequiredFrame, std::string* error) {
		NET_PLANE_CHECK();
		if (!IsPersistentWorldRound()) {
			if (error) *error = "only a persistent world admits members into a running round";
			return false;
		}
		if (m_State != NetLockstepState::Running) {
			if (error) *error = "the world round is not running";
			return false;
		}
		if (peerId == 0 || peerId == m_Config.localPeerId || peerId > m_Config.peerCount || transportPeerId == c_InvalidNetPeerId) {
			if (error) *error = "the world member's identity is invalid";
			return false;
		}
		// A slot whose holder cleanly left is free again: the same lockstep id is re-admitted under the
		// membership's next generation, which is a fresh admission, not the old holder returning.
		const bool alreadyListed = IsKnownRemotePeer(peerId);
		if (alreadyListed && m_PeerLeaveFrames.find(peerId) == m_PeerLeaveFrames.end()) {
			if (error) *error = "peer " + std::to_string(static_cast<int>(peerId)) + " is already a member of this world";
			return false;
		}
		if (firstRequiredFrame <= m_Stats.nextFrame) {
			if (error) *error = "a world member's first required frame must be announced ahead of the committed frame";
			return false;
		}
		const uint16_t delay = InputDelayAt(peerId, firstRequiredFrame);
		if (firstRequiredFrame > UINT64_MAX - delay) { if (error) *error = "world admission input target overflow"; return false; }
		// A fresh member is not a returning seat: it enters the required set at its announced frame and
		// never through the dropped-seat hold, so nothing about it can pause the world.
		m_PeerLeaveFrames.erase(peerId);
		m_PeerFrameWaivers.erase(peerId);
		m_DroppedSeats.erase(peerId);
		m_AiHeldSeats.erase(peerId);
		// A released seat that admits a member is that member's: a later hold of it is reclaimable.
		m_ReleasedAiSeats.erase(peerId);
		m_DroppedSeatResolutions.erase(peerId);
		// The member's round starts at E, so its own first produced target is E plus its input delay.
		// The frames before that are the ones this admission replays; the round must not wait on the
		// member for frames it was never in a position to produce.
		m_PeerAdmissions[peerId] = {firstRequiredFrame, delay};
		m_PeerEffectiveStart[peerId] = firstRequiredFrame + delay;
		m_RemoteStartsReceived.erase(peerId);
		m_RemoteStarts.erase(peerId);
		m_PeersPlayedThisRound.erase(peerId);
		m_LastStartAnswerMs.erase(peerId);
		m_RemoteTransports[peerId] = transportPeerId;
		if (!alreadyListed) {
			m_RemotePeerIds.push_back(peerId);
			std::sort(m_RemotePeerIds.begin(), m_RemotePeerIds.end());
		}
		RefreshLeftSeatHolds();
		// The epoch was set when this activation was announced, ahead of every frame the round has
		// sent, so the live stream has already restarted at it for the members that were here.
		// Everything already on the wire for its first required frames went out before it was a peer.
		NetLockstepStart admitted;
		admitted.sessionId = m_Config.sessionId; admitted.roundId = m_RoundId;
		admitted.startFrame = firstRequiredFrame; admitted.inputDelayFrames = delay;
		admitted.localPeerId = peerId; admitted.peerCount = m_Config.peerCount;
		admitted.controllerFrameVersion = ControllerFrame::c_Version;
		admitted.controllerFrameEncodedSize = static_cast<uint16_t>(ControllerFrame::c_EncodedSize);
		admitted.scenario = m_Config.scenario; admitted.ownershipPolicy = m_Config.ownershipPolicy;
		for (uint8_t survivor: m_RemotePeerIds) if (survivor != peerId && !IsPeerGoneAtFrame(survivor, firstRequiredFrame))
			SendPacket({admitted}, NetTransportLane::ControlReliable, nullptr, nullptr, nullptr, survivor);
		m_LastAdmissionReplayFrames = ReplaySentFramesTo(peerId, firstRequiredFrame);
		return true;
	}

	void NetLockstepCoordinator::SetObservationEpoch(uint64_t frame) {
		NET_PLANE_CHECK();
		if (frame == 0) {
			return;
		}
		// Two joiners in flight announce two restarts: a second announcement may never drop the
		// first, or the member admitted there reads frames spelled against a table it never had.
		m_ObservationEpochs.insert(frame);
		PruneObservationEpochs();
	}

	void NetLockstepCoordinator::MoveObservationEpoch(uint64_t from, uint64_t to) {
		NET_PLANE_CHECK();
		// A re-announce is the same activation at a later frame, so its old restart goes with it.
		if (from != 0) {
			m_ObservationEpochs.erase(from);
		}
		SetObservationEpoch(to);
	}

	void NetLockstepCoordinator::PruneObservationEpochs() {
		if (m_ObservationEpochApplied.empty() || m_ObservationEpochs.size() < 2) {
			return;
		}
		// An epoch every sender has already reset at can never be due again: senders encode in
		// target order, and a sender that is behind takes the newest epoch at or below its frame.
		uint64_t slowest = UINT64_MAX;
		for (const auto& [peerId, applied]: m_ObservationEpochApplied) {
			(void)peerId;
			slowest = std::min(slowest, applied);
		}
		if (slowest == UINT64_MAX) {
			return;
		}
		m_ObservationEpochs.erase(m_ObservationEpochs.begin(), m_ObservationEpochs.lower_bound(slowest));
	}

	void NetLockstepCoordinator::ApplyObservationEpoch(uint8_t senderPeerId, uint64_t targetFrame) {
		// The newest restart this frame is at or past; a sender behind two of them takes the newer.
		const auto above = m_ObservationEpochs.upper_bound(targetFrame);
		if (above == m_ObservationEpochs.begin()) {
			return;
		}
		const uint64_t due = *std::prev(above);
		const auto applied = m_ObservationEpochApplied.find(senderPeerId);
		if (applied != m_ObservationEpochApplied.end() && applied->second >= due) {
			return;
		}
		// An emptied encode table says so on the wire - the block's binding count reads 0 where the
		// receiver holds more - and every receiver resets that sender's table before it binds again.
		m_ObservationEncodeTables.Exactly(senderPeerId).Reset();
		// A window may not carry a copy from before the epoch: its kept block was spelled against
		// the table this reset emptied. The encoder leaves a tick off when its block is gone.
		NetLockstepObservationBlocks& blocks = m_ObservationBlocks[senderPeerId];
		blocks.erase(blocks.begin(), blocks.lower_bound(due));
		m_ObservationEpochApplied[senderPeerId] = due;
	}

	bool NetLockstepCoordinator::BuildPendingRemoteFrame(uint64_t targetFrame, uint8_t senderPeerId, NetLockstepFrame& out) const {
		const auto frameIt = m_RemoteFrames.find(targetFrame);
		if (frameIt == m_RemoteFrames.end()) {
			return false;
		}
		const auto senderIt = frameIt->second.find(senderPeerId);
		if (senderIt == frameIt->second.end()) {
			return false;
		}
		out = NetLockstepFrame{};
		out.senderPeerId = senderPeerId;
		out.targetFrame = targetFrame;
		out.roundId = m_RoundId;
		out.frames = senderIt->second;
		if (const auto commands = m_RemoteCommands.find(targetFrame); commands != m_RemoteCommands.end()) {
			if (const auto found = commands->second.find(senderPeerId); found != commands->second.end()) out.commands = found->second;
		}
		if (const auto observations = m_RemoteObservations.find(targetFrame); observations != m_RemoteObservations.end()) {
			if (const auto found = observations->second.find(senderPeerId); found != observations->second.end()) out.observations = found->second;
		}
		if (const auto values = m_RemoteValueObservations.find(targetFrame); values != m_RemoteValueObservations.end()) {
			if (const auto found = values->second.find(senderPeerId); found != values->second.end()) out.valueObservations = found->second;
		}
		return true;
	}

	size_t NetLockstepCoordinator::ReplaySentFramesTo(uint8_t peerId, uint64_t fromFrame) {
		const auto transportIt = m_RemoteTransports.find(peerId);
		if (transportIt == m_RemoteTransports.end() || m_LastQueuedTargetFrame == std::numeric_limits<uint64_t>::max() ||
		    fromFrame > m_LastQueuedTargetFrame) {
			return 0;
		}
		size_t replayed = 0;
		// The live tables belong to the members already here: they read the restart the announced
		// epoch put in the live stream and nothing else may move under them. The replay spells its
		// own stream out against a table that starts empty, exactly as the admitted member's does,
		// so when it ends the member holds what the live table holds and the frames after it decode.
		std::map<uint8_t, NetSoundObservationDictionary> scratch;
		for (uint64_t target = fromFrame; target <= m_LastQueuedTargetFrame; ++target) {
			// This peer's own frame first, then the members' in peer id order: the same order every
			// receiver already read them in, so the member's tables bind the same keys in the same way.
			NetLockstepFrame own;
			if (FindLocalInput(target, own)) {
				own.priorWindow.clear();
				std::string error;
				if (SendPacket({own}, NetTransportLane::ControlReliable, &error, &scratch[m_Config.localPeerId], nullptr, peerId)) ++replayed;
			}
			for (uint8_t sender: m_RemotePeerIds) {
				if (sender == peerId) {
					continue;
				}
				NetLockstepFrame held;
				if (!BuildPendingRemoteFrame(target, sender, held)) {
					continue;
				}
				std::string error;
				if (SendPacket({held}, NetTransportLane::ControlReliable, &error, &scratch[sender], nullptr, peerId)) ++replayed;
			}
		}
		if (m_Config.frameLane != NetTransportLane::ControlReliable) m_ReliableFramesThrough[peerId] = m_LastQueuedTargetFrame + NetLockstepCodec::c_MaxWindowTicks;
		std::cout << "[lockstep] replayed " << replayed << " frames for targets " << fromFrame << ".."
		          << m_LastQueuedTargetFrame << " to the member admitted as peer " << static_cast<int>(peerId) << std::endl;
		return replayed;
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
		NET_PLANE_CHECK();
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
		std::vector<uint8_t> windowBytes;
		std::vector<uint8_t> classicBytes;
		size_t observationsEncoded = 0;
		size_t valueObservationsEncoded = 0;
		const NetLockstepFrame* relayed = std::get_if<NetLockstepFrame>(&packet.payload);
		if (relayed) {
			// The forwarded sender spells its keys out again from the epoch, on the frame that says so.
			ApplyObservationEpoch(fromPeerId, relayed->targetFrame);
		}
		NetLockstepObservationBlocks* blocks = relayed ? &ObservationBlocksOf(fromPeerId, relayed->targetFrame) : nullptr;
		bool windowed = relayed && !relayed->priorWindow.empty() && blocks;
		NetLockstepFrame classic;
		if (windowed) {
			classic = *relayed;
			classic.priorWindow.clear();
		}
		// The forward spends the table once, on the tick that just arrived; the copies of the older ticks
		// are the blocks this host already forwarded for them.
		if (!NetLockstepCodec::Encode(windowed ? NetLockstepPacket{classic} : packet, classicBytes, nullptr, &m_ObservationEncodeTables.Exactly(fromPeerId), &observationsEncoded, &valueObservationsEncoded, blocks)) {
			return;
		}
		// A window this host cannot build costs the forward its redundancy, never the tick.
		if (windowed && !NetLockstepCodec::Encode(packet, windowBytes, nullptr, nullptr, nullptr, nullptr, blocks)) {
			windowBytes.clear();
			windowed = false;
		}
		// The table this re-encodes from is the one that just decoded the packet, so every key is already
		// a slot and the forward is never longer than what arrived. If it ever were, the peers behind the
		// relay would commit a smaller table than the host and the desync check would find it.
		if (const NetLockstepFrame* frame = std::get_if<NetLockstepFrame>(&packet.payload); frame &&
		    (observationsEncoded < frame->observations.size() || valueObservationsEncoded < frame->valueObservations.size())) {
			++m_Stats.relayObservationOverflows;
			std::cout << "[lockstep] relay of peer " << static_cast<int>(fromPeerId) << "'s frame " << frame->targetFrame
			          << " carried " << observationsEncoded << " of " << frame->observations.size() << " sound observations"
			          << " and " << valueObservationsEncoded << " of " << frame->valueObservations.size() << " value observations" << std::endl;
		}
		for (const auto& [peerId, transportId]: m_RemoteTransports) {
			if (peerId == fromPeerId) {
				continue;
			}
			const std::vector<uint8_t>& bytes = windowed && FrameWindowAgreedFor(peerId) ? windowBytes : classicBytes;
			// Only a frame has a reserved byte there; another payload's byte 1 means something else.
			if (relayed && bytes.size() > NetLockstepCodec::c_HeaderBytes + 1) {
				m_Stats.peers[peerId].lastFrameReserved = bytes[NetLockstepCodec::c_HeaderBytes + 1];
			}
			const NetTransportLane lane = relayed ? LaneTo(peerId, packet, m_Config.frameLane) : NetTransportLane::ControlReliable;
			// Behind an undrained backlog, or this peer's stream would arrive out of order. An unreliable frame has no order to keep.
			const auto backlogIt = m_RelayBacklog.find(peerId);
			if (lane == NetTransportLane::ControlReliable && ((backlogIt != m_RelayBacklog.end() && !backlogIt->second.empty()) || m_TimingOutgoing.contains(peerId))) {
				QueueRelayBacklog(peerId, bytes);
				continue;
			}
			NetLockstepPeerStats& peerStats = m_Stats.peers[peerId];
			std::string sendError;
			bool congested = false;
			if (m_Transport->Send(transportId, lane, bytes, &sendError, &congested)) {
				CountRelaySent(peerStats, bytes.size());
				m_CongestedPeers.erase(peerId);
				continue;
			}
			// A refused unreliable forward is what the window is for: the next one repeats it.
			if (lane == NetTransportLane::InputUnreliable) {
				continue;
			}
			// A refused forward was never queued, and on a reliable lane the receiver cannot ask for
			// it again - it would wait on that frame until its own grace ran out. Hold it for retry.
			++m_Stats.relaySendFailures;
			++peerStats.relaySendFailures;
			NoteRelayError(peerId, sendError);
			NoteRelayRefusal(peerId, congested);
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
		for (const auto& pending: m_RecoveryOutgoing) {
			for (const auto& [peer, offset]: pending.nextOffsets) {
				const size_t remaining = pending.bytes.size() - offset;
				bytes += remaining + ((remaining + NetLockstepCodec::c_MaxRecoveryChunkBytes - 1) / NetLockstepCodec::c_MaxRecoveryChunkBytes) * (NetLockstepCodec::c_HeaderBytes + 36);
			}
		}
		return bytes;
	}

	uint32_t NetLockstepCoordinator::RelayBacklogPackets(uint8_t peerId) const {
		const auto backlog = m_RelayBacklog.find(peerId);
		uint32_t count = backlog == m_RelayBacklog.end() ? 0 : static_cast<uint32_t>(backlog->second.size());
		for (const auto& pending: m_RecoveryOutgoing) {
			if (const auto offset = pending.nextOffsets.find(peerId); offset != pending.nextOffsets.end()) {
				count += static_cast<uint32_t>((pending.bytes.size() - offset->second + NetLockstepCodec::c_MaxRecoveryChunkBytes - 1) / NetLockstepCodec::c_MaxRecoveryChunkBytes);
			}
		}
		return count;
	}

	void NetLockstepCoordinator::NoteRelayError(uint8_t peerId, const std::string& error) {
		m_Stats.lastRelayError = error;
		m_Stats.peers[peerId].lastRelayError = error;
	}

	// Lockstep ids are reused - a replacement takes the dropped peer's slot - so nothing about the old
	// occupant's congestion may outlive it, or the new one's first hold goes uncounted.
	void NetLockstepCoordinator::ForgetCongestion(uint8_t peerId) {
		m_RecoveryBlockedSinceMs.erase(peerId);
		m_CongestedPeers.erase(peerId);
		m_HeldForCongestion.erase(peerId);
		m_RelayBacklogSinceMs.erase(peerId);
	}

	void NetLockstepCoordinator::NoteRelayRefusal(uint8_t peerId, bool congested) {
		if (congested) {
			++m_Stats.relayCongestedRefusals;
			m_CongestedPeers.insert(peerId);
		} else {
			m_CongestedPeers.erase(peerId);
		}
	}

	void NetLockstepCoordinator::QueueRelayBacklog(uint8_t peerId, const std::vector<uint8_t>& bytes) {
		std::deque<std::vector<uint8_t>>& backlog = m_RelayBacklog[peerId];
		// The frame lane is reliable and in order, so a forward we drop here is a hole this peer can
		// never ask for again: its stream is finished whatever the link does next, and a healed link
		// would only feed a seat that can no longer catch up. Take the seat while the round is whole.
		if (backlog.size() >= NetLockstepCodec::c_MaxFutureFrameSkew) {
			++m_Stats.relayBacklogOverflows;
			NetLockstepPeerStats& peerStats = m_Stats.peers[peerId];
			if (++peerStats.relayBacklogOverflows == 1) {
				NoteRelayError(peerId, "relay backlog full at " + std::to_string(backlog.size()) + " forwards: " +
				                           (peerStats.lastRelayError.empty() ? m_Stats.lastRelayError : peerStats.lastRelayError));
			}
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
				ForgetCongestion(peerId);
				it = m_RelayBacklog.erase(it);
				continue;
			}
			while (!it->second.empty() && !m_TimingOutgoing.contains(peerId)) {
				std::string sendError;
				bool congested = false;
				if (!m_Transport->Send(transportIt->second, NetTransportLane::ControlReliable, it->second.front(), &sendError, &congested)) {
					NoteRelayError(peerId, sendError);
					NoteRelayRefusal(peerId, congested);
					break;
				}
				m_CongestedPeers.erase(peerId);
				m_HeldForCongestion.erase(peerId);
				++m_Stats.relayResends;
				++m_Stats.peers[peerId].relayResends;
				CountRelaySent(m_Stats.peers[peerId], it->second.front().size());
				it->second.pop_front();
			}
			if (it->second.empty()) {
				ForgetCongestion(peerId);
				it = m_RelayBacklog.erase(it);
				continue;
			}
			const uint64_t since = m_RelayBacklogSinceMs.emplace(peerId, nowMs).first->second;
			if (m_Config.timeoutMs > 0 && nowMs >= since && nowMs - since >= PeerSilenceLeaveMs()) {
				// A queue of ours that will not drain is backpressure, not a lost player, so the peer
				// keeps its seat while there is still time for the queue to come back. Past the hold
				// bound there is not: a seat we cannot feed has to go before the round's own grace ends
				// the match for everybody.
				const uint64_t heldMs = nowMs - since;
				if (m_CongestedPeers.find(peerId) != m_CongestedPeers.end()) {
					if (m_HeldForCongestion.insert(peerId).second) {
						++m_Stats.relayCongestionHolds;
						std::cout << "[net-match] holding " << DescribePeer(peerId) << ": our send queue has not drained in "
						          << heldMs << "ms (" << m_Stats.peers[peerId].lastRelayError << ")" << std::endl;
					}
					m_Stats.longestCongestionHoldMs = std::max(m_Stats.longestCongestionHoldMs, heldMs);
					NetLockstepPeerStats& peerStats = m_Stats.peers[peerId];
					peerStats.longestCongestionHoldMs = std::max(peerStats.longestCongestionHoldMs, heldMs);
					if (heldMs >= CongestionHoldLeaveMs()) {
						NoteRelayError(peerId, "our send queue has not drained in " + std::to_string(heldMs) + "ms: " + peerStats.lastRelayError);
						m_UnreachablePeers.insert(peerId);
					}
				} else {
					m_UnreachablePeers.insert(peerId);
				}
			}
			++it;
		}
		UpdateRelayBacklogBytes();
	}

	void NetLockstepCoordinator::HandleEvent(const NetTransportEvent& event, uint64_t nowMs) {
		// The plane handles only the round's own frames, acks, decisions and checksums; everything that reaches the session or the
		// transport's lifecycle waits for the simulation thread, in order.
		if (m_PlaneTicking && (event.type != NetTransportEventType::PacketReceived || NetHostMigrationCodec::LooksLikePacket(event.bytes) ||
		                       m_State == NetLockstepState::Failed || m_State == NetLockstepState::Stopped || m_PlaneHeldTransports.contains(event.peerId))) {
			m_PlaneDeferredEvents.push_back(event);
			m_PlaneHeldTransports.insert(event.peerId);
			return;
		}
		if (event.type == NetTransportEventType::PacketReceived && NetHostMigrationCodec::LooksLikePacket(event.bytes)) {
			HandleMigrationEvent(event, nowMs);
			return;
		}
		if (m_State == NetLockstepState::Failed || m_State == NetLockstepState::Stopped) {
			// An ended round still owns the wire through the host's goodbye drain: a seat knocking to come back
			// is the session's traffic, and the round that ended must not be what swallows its handshake.
			if (m_State == NetLockstepState::Stopped && m_SessionEventSink && m_Config.localPeerId == GetHostPeerId() && !UsesTransportPeer(event.peerId) &&
			    (event.type == NetTransportEventType::PeerConnected || event.type == NetTransportEventType::PeerDisconnected ||
			     event.type == NetTransportEventType::PacketReceived))
				m_SessionEventSink(event);
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
				if (lockstepPeer == GetHostPeerId() && event.reason.find("slow player:") != std::string::npos) {
					if (m_SessionEventSink) m_SessionEventSink(event);
					m_LocalSeatHeld = true;
					m_PeerLeaveFrames[m_Config.localPeerId] = m_Stats.nextFrame;
					m_Stats.timeoutReason = "PeerHeld:Your seat is held by the AI. Rejoin when your connection and machine can keep up.";
					m_State = NetLockstepState::Stopped;
					break;
				}
				if (lockstepPeer == GetHostPeerId() && BeginHostMigration(nowMs))
					break;
				// The session tracks the same lifecycles for the eventual rematch/rejoin bookkeeping.
				if (m_SessionEventSink) {
					m_SessionEventSink(event);
				}
				// A cleanly-left peer's socket closing behind its notice is expected.
				if (lockstepPeer != 0 && m_PeerLeaveFrames.find(lockstepPeer) != m_PeerLeaveFrames.end()) {
					m_RemoteTransports.erase(lockstepPeer);
					// The capability belonged to that peer, not to the id a refilled seat may reuse.
					m_RemoteFrameWindow.erase(lockstepPeer);
					break;
				}
				// A superseded incarnation's socket finally closing says nothing about the seat: its live
				// holder is another transport, which the resync brings into the round. The dead handle
				// leaves the round with it, or the next send names a peer the transport has forgotten.
				if (m_RelayHost && lockstepPeer != 0 && SeatStateOf(lockstepPeer, event.peerId).fencedTransport) {
					m_RemoteTransports.erase(lockstepPeer);
					m_RemoteFrameWindow.erase(lockstepPeer);
					++m_Stats.ignoredAdmissionFaults;
					break;
				}
				// The old drop's close can land on the refilled seat; this round already listed them.
				if (m_RelayHost && lockstepPeer != 0 && IgnoreStaleRefillLeave(lockstepPeer, nowMs)) {
					++m_Stats.ignoredAdmissionFaults;
					break;
				}
				// The relay host adjudicates a client drop as a leave at the first frame it has no data
				// for, so the survivors keep playing; a host drop still ends the match. The relayed
				// frames precede this notice on the reliable lane, so no survivor learns of the leave
				// before it holds everything the leave references.
				if (m_RelayHost && lockstepPeer != 0 && m_State == NetLockstepState::Running) {
					ApplyPeerLeave(lockstepPeer, FirstFrameWithout(lockstepPeer), "connection lost", nowMs, false);
					break;
				}
				// Before the agreed start the bounded wait holds a seat whose link died, as its answer budget would.
				if (m_RelayHost && lockstepPeer != 0 && m_State == NetLockstepState::WaitingForStart && UsesBoundedWait() && m_RequirePublishedStart &&
				    m_Config.localPeerId == GetHostPeerId() && !m_AgreedStartApplied) {
					m_StartupLinksLost.insert(lockstepPeer);
					m_RemoteTransports.erase(lockstepPeer);
					m_RemoteFrameWindow.erase(lockstepPeer);
					std::cout << "[net-lockstep] " << DescribePeer(lockstepPeer) << " lost its link before the agreed start; the start holds its seat" << std::endl;
					TickStartupWait(nowMs);
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
				if (!m_RelayHost && !UsesTransportPeer(event.peerId)) {
					++m_Stats.ignoredAdmissionFaults;
					NameDroppedPacket(event, "a connection this client's round does not use");
					return;
				}
				// Whatever the host sent proves it alive, a packet this peer cannot read included.
				if (!m_RelayHost && LockstepPeerOfTransport(event.peerId) == GetHostPeerId()) NoteAuthorityHeard(nowMs);
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
							NameDroppedPacket(event, "observation slots on a connection bound to no seat", decoded.error.message);
							return;
						}
						if (m_Stats.unresolvedObservationPackets == 0) {
							std::cout << "[lockstep] dropped a frame over its sound observations: " << decoded.error.message << std::endl;
						}
						NameDroppedPacket(event, "sound observations this peer cannot resolve", decoded.error.message);
						++m_Stats.unresolvedObservationPackets;
						return;
					}
					// A chat line that fails decode is still chat: the session counts it malformed and
					// drops it, and no peer loses its connection over presentation traffic.
					uint16_t peekedType = 0;
					const bool chatTyped = NetProtocol::PeekMessageType(event.bytes.data(), event.bytes.size(), peekedType) &&
					                       peekedType == static_cast<uint16_t>(NetMessageType::Chat);
					const auto sessionPacket = decoded.error.code == NetLockstepErrorCode::BadMagic ? NetProtocol::Decode(event.bytes) : NetDecodeResult{};
					if (m_PlaneTicking && decoded.error.code == NetLockstepErrorCode::BadMagic) {
						m_PlaneDeferredEvents.push_back(event);
						return;
					}
					if (decoded.error.code == NetLockstepErrorCode::BadMagic && (sessionPacket.ok || chatTyped)) {
						// A loading authority keeps its authenticated connection alive through session heartbeats.
						if (sessionPacket.ok && std::holds_alternative<NetHeartbeat>(sessionPacket.message.payload) &&
						    !m_RelayHost && LockstepPeerOfTransport(event.peerId) == GetHostPeerId()) NoteAuthorityHeard(nowMs);
						// Session-protocol traffic mid-match is a reconnect handshake; hand it over.
						if (m_SessionEventSink) {
							m_SessionEventSink(event);
						} else {
							++m_Stats.ignoredSessionPackets;
						}
						return;
					}
					if (decoded.error.code == NetLockstepErrorCode::BadMagic && NetLobbyProtocol::Decode(event.bytes).ok) {
						if (m_SessionEventSink) m_SessionEventSink(event);
						else ++m_Stats.ignoredSessionPackets;
						return;
					}
					// A frame on the unreliable lane whose bindings ran past every window is one packet lost, not a broken peer.
					if (event.lane == NetTransportLane::InputUnreliable && decoded.error.code == NetLockstepErrorCode::ObservationBindingGap) {
						if (m_Stats.frameBindingGapDrops++ == 0) std::cout << "[lockstep] dropped an unreliable frame packet: " << decoded.error.message << std::endl;
						NameDroppedPacket(event, "an unreliable frame past its bindings", decoded.error.message);
						return;
					}
					// Malformed admission traffic cannot stop a round it never joined.
					if (UsesTransportPeer(event.peerId)) {
						Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, decoded.error.message);
					} else {
						++m_Stats.ignoredAdmissionFaults;
					}
					return;
				}
				if ((std::holds_alternative<NetLockstepRecoveryChunk>(decoded.packet.payload) || std::holds_alternative<NetLockstepTiming>(decoded.packet.payload)) && event.lane != NetTransportLane::ControlReliable) {
					if (UsesTransportPeer(event.peerId)) Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "recovery input requires the reliable control lane");
					return;
				}
				// A start or a stop may end or reshape the round; the simulation thread takes it.
				if (m_PlaneTicking && (std::holds_alternative<NetLockstepStart>(decoded.packet.payload) || std::holds_alternative<NetLockstepStop>(decoded.packet.payload))) {
					m_PlaneDeferredEvents.push_back(event);
					m_PlaneHeldTransports.insert(event.peerId);
					return;
				}
				m_PacketLane = event.lane;
				HandlePacket(decoded.packet, nowMs, event.peerId);
				m_PacketLane = NetTransportLane::ControlReliable;
				break;
			}
		}
	}

	void NetLockstepCoordinator::NameDroppedPacket(const NetTransportEvent& event, const char* reason, const std::string& detail) {
		if (!m_DropReasonsNamed.insert({event.peerId, reason}).second) return;
		std::cout << "[lockstep] dropped a packet from connection " << event.peerId << " (peer " << static_cast<int>(LockstepPeerOfTransport(event.peerId)) << ", "
		          << (event.lane == NetTransportLane::ControlReliable ? "reliable" : "unreliable") << "): " << reason << (detail.empty() ? "" : ": " + detail)
		          << " next=" << m_Stats.nextFrame << std::endl;
	}

	void NetLockstepCoordinator::HandlePacket(const NetLockstepPacket& packet, uint64_t nowMs, NetPeerId fromTransport) {
		if (!m_RelayHost && LockstepPeerOfTransport(fromTransport) == GetHostPeerId())
			NoteAuthorityHeard(nowMs);
		// Any traffic proves the sender is alive, whatever the packet turns out to say; the host's
		// drop adjudication runs off this and nothing else.
		const uint8_t sender = std::visit(Overloaded{
			[](const NetLockstepStart& start) { return start.localPeerId; },
			[](const NetLockstepFrame& frame) { return frame.senderPeerId; },
			[](const NetLockstepAck& ack) { return ack.senderPeerId; },
			[](const NetLockstepStop& stop) { return stop.senderPeerId; },
			[](const NetLockstepChecksum& checksum) { return checksum.senderPeerId; },
			[](const NetLockstepSeatSnapshot& snapshot) { return snapshot.senderPeerId; },
			[](const NetLockstepRecoveryChunk& chunk) { return chunk.senderPeerId; },
			[](const NetLockstepTiming& timing) { return timing.senderPeerId; },
		}, packet.payload);
		if (IsKnownRemotePeer(sender) && SenderOwnsTransport(sender, fromTransport)) {
			m_PeerLastHeardMs[sender] = nowMs;
			m_Stats.peers[sender].lastHeardMs = nowMs;
		}
		std::visit(Overloaded{
			[&](const NetLockstepStart& start) { HandleStart(start, nowMs, fromTransport); },
			[&](const NetLockstepFrame& frame) { HandleFrame(frame, nowMs, fromTransport); },
			[&](const NetLockstepAck& ack) { HandleAck(ack, fromTransport); },
			[&](const NetLockstepStop& stop) { HandleStop(stop, nowMs, fromTransport); },
			[&](const NetLockstepChecksum& checksum) { HandleChecksum(checksum, fromTransport); },
			[&](const NetLockstepSeatSnapshot& snapshot) { HandleSeatSnapshot(snapshot, fromTransport); },
			[&](const NetLockstepRecoveryChunk& chunk) { HandleRecoveryChunk(chunk, nowMs, fromTransport); },
			[&](const NetLockstepTiming& timing) { HandleTiming(timing, nowMs, fromTransport); },
		}, packet.payload);
	}

	bool NetLockstepCoordinator::SenderOwnsTransport(uint8_t claimedPeerId, NetPeerId fromTransport) const {
		// Clients receive every remote's traffic through the host relay, so the transport is always the
		// host's; only the relay host, which receives each remote directly, can bind claim to connection.
		if (!m_RelayHost) {
			return true;
		}
		const auto it = m_RemoteTransports.find(claimedPeerId);
		// Departed seats remain known logical peers, but their removed transport binding grants no
		// authority. Rejoining peers acquire a new binding only after the session admits them.
		return it != m_RemoteTransports.end() && it->second == fromTransport;
	}

	void NetLockstepCoordinator::HandleStart(const NetLockstepStart& start, uint64_t nowMs, NetPeerId fromTransport) {
		++m_Stats.startPacketsReceived;
		// Our own start, forwarded back to us: a relay that still holds a stale transport for this seat sends
		// it where it came from. It carries nothing we do not know and it is not a broken build.
		if (start.localPeerId == m_Config.localPeerId) {
			++m_Stats.staleRoundPackets;
			std::cout << "[lockstep] ignored our own start returned to us for peer " << static_cast<int>(start.localPeerId) << std::endl;
			return;
		}
		// The host's boundary is a distinct start record. It is the only start that may change the
		// effective frame; a late ordinary publication below only updates that seat's measured park.
		if (start.agreedStartRecord) {
			const bool authority = start.localPeerId == GetHostPeerId() && IsRoundAuthority(start.localPeerId, fromTransport);
			if (!authority || start.sessionId != m_Config.sessionId || (m_RoundId != 0 && start.roundId != m_RoundId)) return;
			ApplyAgreedStart(start, nowMs);
			return;
		}
		const auto hostTransport = m_RemoteTransports.find(GetHostPeerId());
		const bool worldRosterMessage = IsPersistentWorldRound() && !m_RelayHost && hostTransport != m_RemoteTransports.end() &&
		    fromTransport == hostTransport->second && start.localPeerId != GetHostPeerId() && start.localPeerId != m_Config.localPeerId &&
		    start.localPeerId > 0 && start.localPeerId <= m_Config.peerCount;
		if (!SenderOwnsTransport(start.localPeerId, fromTransport)) {
			const auto bound = m_RemoteTransports.find(start.localPeerId);
			std::cout << "[lockstep] dropped a start claiming peer " << static_cast<int>(start.localPeerId) << " from the wrong transport: from=" << fromTransport
			          << " bound=" << (bound == m_RemoteTransports.end() ? std::string("none") : std::to_string(bound->second)) << " frame=" << start.startFrame << std::endl;
			return;
		}
		// A returning seat's start is the first thing its round sends; when it lands says whether its frames can be in time.
		if (m_RelayHost && m_ReclaimTransactions.contains(start.localPeerId) && !m_RemoteStartsReceived.contains(start.localPeerId))
			std::cout << "[lockstep] start of returning peer " << static_cast<int>(start.localPeerId) << " landed: frame=" << start.startFrame << " next=" << m_Stats.nextFrame
			          << " reclaim=" << m_ReclaimTransactions.at(start.localPeerId).activationFrame << " clock=" << NetLockstepSharedClockMs() << std::endl;
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
		if (!followTheAuthority && start.roundId != 0 && ((m_RoundId != 0 && start.roundId != m_RoundId) || (m_RoundId == 0 && start.startFrame != m_Config.startFrame && !worldRosterMessage))) {
			++m_Stats.staleRoundPackets;
			return;
		}
		// A seat joining the running round starts at its own admission; the start the host began the round with names
		// a frame before it, and once this seat has its round that start is a straggler, never a mismatch.
		if (m_Config.joinsRunningRound && start.localPeerId == GetHostPeerId() && start.startFrame < m_Config.startFrame &&
		    m_RoundId != 0 && start.roundId == m_RoundId) {
			++m_Stats.staleRoundPackets;
			return;
		}
		if (const auto admission = m_PeerAdmissions.find(start.localPeerId); admission != m_PeerAdmissions.end() && start.startFrame < admission->second.frame) {
			++m_Stats.staleRoundPackets;
			return;
		}
		const bool introducedByHost = worldRosterMessage && (!IsKnownRemotePeer(start.localPeerId) || IsPeerGoneAtFrame(start.localPeerId, start.startFrame) ||
		    (m_Config.joinsRunningRound && m_State == NetLockstepState::WaitingForStart && !m_RemoteStartsReceived.contains(start.localPeerId)));
		if (introducedByHost) {
			if (start.startFrame > UINT64_MAX - start.inputDelayFrames ||
			    std::max(m_Config.startFrame, start.startFrame + start.inputDelayFrames) < m_Stats.nextFrame) {
				Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "world admission precedes committed input"); return;
			}
			m_PeerAdmissions[start.localPeerId] = {start.startFrame, start.inputDelayFrames};
			m_PeerEffectiveStart[start.localPeerId] = std::max(m_Config.startFrame, start.startFrame + start.inputDelayFrames);
			m_PeerLeaveFrames.erase(start.localPeerId);
			m_RemoteStartsReceived.erase(start.localPeerId);
			m_RemoteStarts.erase(start.localPeerId);
			if (!IsKnownRemotePeer(start.localPeerId)) { m_RemotePeerIds.push_back(start.localPeerId); std::sort(m_RemotePeerIds.begin(), m_RemotePeerIds.end()); }
		}
		if (!StartMatchesConfig(start)) {
			// A start we would only have taken by following its round belongs to another round after all,
			// and another round's start is ignored here - it was never this round's handshake to fail on.
			if (followTheAuthority) {
				++m_Stats.staleRoundPackets;
				std::cout << "[lockstep] ignored a start of round " << start.roundId << " that disagrees with this round's setup" << std::endl;
				return;
			}
			// A returning seat's start sent before it took the host's admission terms is its straggler, never the round's end:
			// the host authored the admission, and the seat's next start carries it.
			if (const auto admission = m_PeerAdmissions.find(start.localPeerId); admission != m_PeerAdmissions.end() && m_Config.localPeerId == GetHostPeerId() &&
			    start.localPeerId != m_Config.localPeerId && start.inputDelayFrames != admission->second.delay) {
				NetLockstepStart delayed = start;
				delayed.inputDelayFrames = admission->second.delay;
				if (StartMatchesConfig(delayed)) {
					++m_Stats.staleRoundPackets;
					std::cout << "[lockstep] ignored peer " << static_cast<int>(start.localPeerId) << "'s start sent before its admission's delay: " << start.inputDelayFrames
					          << " for " << admission->second.delay << std::endl;
					return;
				}
			}
			Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "lockstep start mismatch (" + DescribeStartMismatch(start) + ")");
			return;
		}
		if (m_RoundId == 0 && start.roundId != 0) {
			m_RoundId = start.roundId;
			m_ObservationDecodeTables.roundId = m_RoundId;
		} else if (followTheAuthority) {
			ReadoptRound(start.roundId, nowMs);
		}
		if (m_PeerAdmissions.contains(start.localPeerId) && m_RelayHost) {
			// The seat set its round up from the tail it replayed while no decision reached it: the holds and returns decided for
			// frames up to its first go to it ahead of the starts that assume them.
			const uint8_t returning = static_cast<uint8_t>(1U << (start.localPeerId - 1));
			for (const auto& [revision, decision]: m_TimingDecisions) {
				const NetLockstepTiming& transition = decision.proposal;
				if ((transition.phase != NetTimingPhase::HoldAtFrame && transition.phase != NetTimingPhase::ReclaimAtFrame) ||
				    transition.applyFrame > start.startFrame || (transition.heldPeers & returning) != 0) continue;
				// The settled ones are kept for a held seat's return; a new member's snapshot already carries them.
				if (!m_AiHeldSeats.contains(start.localPeerId) && DecisionSettled(decision)) continue;
				SendPacket({transition}, NetTransportLane::ControlReliable, nullptr, nullptr, nullptr, start.localPeerId);
			}
			auto members = m_RemotePeerIds;
			members.push_back(m_Config.localPeerId);
			for (uint8_t peer: members) {
				if (peer == start.localPeerId || IsPeerGoneAtFrame(peer, start.startFrame)) continue;
				NetLockstepStart member = start; member.localPeerId = peer; member.inputDelayFrames = InputDelayAt(peer, start.startFrame);
				member.roundId = m_RoundId;
				if (IsPersistentWorldRound() && peer != m_Config.localPeerId) {
					const auto admission = m_PeerAdmissions.find(peer);
					member.startFrame = admission == m_PeerAdmissions.end() ? m_Config.startFrame : admission->second.frame;
					member.inputDelayFrames = admission == m_PeerAdmissions.end() ? PeerInputDelay(peer) : admission->second.delay;
				}
				SendPacket({member}, NetTransportLane::ControlReliable, nullptr, nullptr, nullptr, start.localPeerId);
			}
			ReplaySentFramesTo(start.localPeerId, start.startFrame);
			for (const auto& [revision, decision]: m_TimingDecisions) {
				if (decision.proposal.applyFrame < start.startFrame) continue;
				auto proposal = decision.proposal;
				SendPacket({proposal}, NetTransportLane::ControlReliable, nullptr, nullptr, nullptr, start.localPeerId);
				if ((proposal.action == NetTimingAction::Delay || proposal.action == NetTimingAction::WorldAdmission) && decision.committed) {
					proposal.phase = NetTimingPhase::Commit;
					SendPacket({proposal}, NetTransportLane::ControlReliable, nullptr, nullptr, nullptr, start.localPeerId);
				}
			}
		}
		// A peer republishes its start once it has measured its own restart, so the allowance we give it
		// before the bound judges it is the start work ITS machine did, not ours.
		const bool publishedStartup = start.startupPublished || start.activityRestartMs > 0;
		if (publishedStartup) {
			m_Stats.peers[start.localPeerId].startParkMs = start.activityRestartMs;
		}
		if (start.localPeerId >= 1 && start.localPeerId <= m_PeerDeviceClasses.size()) m_PeerDeviceClasses[start.localPeerId - 1] = start.deviceClass;
		// A seat held at the agreed startup boundary is a late, valid return once its new startup
		// publication arrives.  Route it through the same future admission window as a reconnect;
		// leaving the AI marker in place until that frame gives every peer the same neutral ramp.
		if (m_Config.localPeerId == GetHostPeerId() && m_AiHeldSeats.contains(start.localPeerId) && !m_ReclaimTransactions.contains(start.localPeerId) &&
		    start.localPeerId != m_Config.localPeerId && m_State == NetLockstepState::Running) {
			m_TimingNowMs = nowMs;
			const uint32_t currentIncarnation = m_Config.peerIncarnations.contains(start.localPeerId)
				? m_Config.peerIncarnations.at(start.localPeerId) : 0;
			std::string reclaimError;
			const uint64_t admissionFrame = FutureTimingFrame();
			if (!SchedulePeerReclaim(start.localPeerId, fromTransport, currentIncarnation + 1, admissionFrame, &reclaimError)) {
				// The conditions a reclaim needs - a running round and an activation past the sent horizon - arrive
				// a few frames later, so the start is remembered and retried until the seat is admitted.
				m_LateStartReclaims[start.localPeerId] = fromTransport;
				std::cout << "[net-match] late startup reclaim deferred for peer " << static_cast<int>(start.localPeerId)
				          << ": " << reclaimError << std::endl;
			}
		}
		const bool firstFromThisPeer = m_RemoteStartsReceived.insert(start.localPeerId).second;
		// The peer published its startup: the flag its start carries, or a measurement from a build
		// that had no flag. A repeat that answers our own start publishes nothing.
		if (publishedStartup) {
			const bool firstPublication = m_PeerStartupPublished.insert(start.localPeerId).second;
			if (firstPublication && m_Config.localPeerId == GetHostPeerId() && m_LocalStartupPublished && !m_AgreedStartApplied) {
				// A late but valid startup publication gets the full answer budget.  The host's own
				// publication remains the floor; the last publication is the deadline anchor.
				m_StartWaitAnnounced = true;
				m_StartWaitSinceMs = std::max(m_StartWaitSinceMs, nowMs);
			}
		}
		if (firstFromThisPeer) {
			m_RemoteStarts[start.localPeerId] = start;
			RelayToOtherRemotes({start}, start.localPeerId);
		} else {
			AnswerRepeatedStart(start.localPeerId, nowMs);
		}
		if (m_State == NetLockstepState::WaitingForStart && AllRemoteStartsReceived() && !m_AgreedStartApplied) {
			if (!m_RequirePublishedStart) {
				m_State = NetLockstepState::Running;
				auto timing = std::move(m_PreStartTiming); m_PreStartTiming.clear();
				for (const auto& [decision, source]: timing) HandleTiming(decision, nowMs, source);
				m_WaitingFrame = std::numeric_limits<uint64_t>::max();
			}
		}
		if (firstFromThisPeer) {
			FlushPreStart(start.localPeerId, nowMs);
			AdvertiseFrameWindow();
			if (m_AgreedStartRecord && m_Config.localPeerId == GetHostPeerId()) (void)SendAgreedStart(start.localPeerId);
		}
	}

	void NetLockstepCoordinator::HandleRecoveryChunk(const NetLockstepRecoveryChunk& chunk, uint64_t nowMs, NetPeerId fromTransport) {
		if (!IsKnownRemotePeer(chunk.senderPeerId) || !SenderOwnsTransport(chunk.senderPeerId, fromTransport)) return;
		if (chunk.sessionId != m_Config.sessionId || chunk.roundId != m_RoundId || m_RoundId == 0) {
			++m_Stats.staleRoundPackets;
			++m_Stats.peers[chunk.senderPeerId].staleRoundPackets;
			return;
		}
		if (const auto pending = m_RecoveryIncoming.find(chunk.senderPeerId); pending != m_RecoveryIncoming.end() && pending->second.targetFrame < m_Stats.nextFrame) m_RecoveryIncoming.erase(pending);
		if (chunk.targetFrame < m_Stats.nextFrame) return;
		if (chunk.targetFrame - m_Stats.nextFrame > NetLockstepCodec::c_MaxFutureFrameSkew) {
			++m_Stats.futureFrameDrops;
			++m_Stats.peers[chunk.senderPeerId].futureFrameDrops;
			return;
		}
		if (chunk.targetFrame < EffectiveStartOf(chunk.senderPeerId)) {
			Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "recovery input targets the sender's delay window");
			return;
		}
		auto found = m_RecoveryIncoming.find(chunk.senderPeerId);
		if (found == m_RecoveryIncoming.end()) {
			if (chunk.offset != 0) {
				Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "recovery input is missing its first chunk");
				return;
			}
			RecoveryIncoming incoming;
			incoming.targetFrame = chunk.targetFrame;
			incoming.totalBytes = chunk.totalBytes;
			incoming.bytes.reserve(chunk.totalBytes);
			found = m_RecoveryIncoming.emplace(chunk.senderPeerId, std::move(incoming)).first;
		}
		auto& incoming = found->second;
		if (incoming.targetFrame != chunk.targetFrame || incoming.totalBytes != chunk.totalBytes || chunk.offset > incoming.bytes.size()) {
			Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "conflicting or out-of-order recovery chunk");
			return;
		}
		if (chunk.offset < incoming.bytes.size()) {
			if (chunk.bytes.size() > incoming.bytes.size() - chunk.offset || !std::equal(chunk.bytes.begin(), chunk.bytes.end(), incoming.bytes.begin() + chunk.offset)) {
				Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "conflicting recovery chunk retry");
			}
			return;
		}
		incoming.bytes.insert(incoming.bytes.end(), chunk.bytes.begin(), chunk.bytes.end());
		if (incoming.bytes.size() != incoming.totalBytes) return;
		NetLockstepFrame frame;
		NetLockstepError validation;
		if (!NetLockstepCodec::DecodeRecoveryInput(incoming.bytes, frame, &validation) || frame.senderPeerId != chunk.senderPeerId ||
		    frame.roundId != chunk.roundId || frame.targetFrame != chunk.targetFrame) {
			Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "invalid assembled recovery input: " + validation.message);
			return;
		}
		m_RecoveryIncoming.erase(found);
		HandleFrame(frame, nowMs, fromTransport, true, true);
	}

	void NetLockstepCoordinator::HandleFrame(const NetLockstepFrame& frame, uint64_t nowMs, NetPeerId fromTransport, bool relay, bool recovered) {
		++m_Stats.framePacketsReceived;
		NetLockstepPeerStats& peerStats = m_Stats.peers[frame.senderPeerId];
		++peerStats.framePacketsReceived;
		// A tick this peer waited on past a resend names how it finally came, so every long wait is explained by its arrival.
		struct WaitedArrival {
			std::function<void()> report;
			~WaitedArrival() { if (report) report(); }
		} waitedArrival;
		const uint64_t waitedFrame = m_MissingSinceFrame;
		const uint64_t waitedSinceMs = m_MissingSinceMs;
		const auto holdsWaited = [this, waitedFrame, sender = frame.senderPeerId] {
			const auto held = m_RemoteFrames.find(waitedFrame);
			return waitedFrame < m_Stats.nextFrame || (held != m_RemoteFrames.end() && held->second.contains(sender));
		};
		const bool carriesWaited = frame.targetFrame == waitedFrame ||
		    std::any_of(frame.priorWindow.begin(), frame.priorWindow.end(), [&](const NetLockstepFrame& copy) { return copy.targetFrame == waitedFrame; });
		if (waitedFrame != UINT64_MAX && waitedSinceMs != 0 && nowMs >= waitedSinceMs + 100 && carriesWaited && !holdsWaited()) {
			const NetTransportLane lane = m_PacketLane;
			waitedArrival.report = [this, nowMs, waitedFrame, waitedSinceMs, lane, relay, recovered, holdsWaited, sender = frame.senderPeerId, newest = frame.targetFrame] {
				std::cout << "[lockstep-recv] waited frame=" << waitedFrame << " of peer " << static_cast<int>(sender) << " came after " << nowMs - waitedSinceMs
				          << " ms lane=" << (lane == NetTransportLane::ControlReliable ? "reliable" : "unreliable") << " relay=" << relay << " recovered=" << recovered
				          << " as=" << (newest == waitedFrame ? "newest" : "window") << " taken=" << holdsWaited() << " gone=" << IsPeerGoneAtFrame(sender, waitedFrame)
				          << " required=" << IsRemoteRequiredForFrame(sender, waitedFrame) << " next=" << m_Stats.nextFrame << std::endl;
			};
		}
		if (!SenderOwnsTransport(frame.senderPeerId, fromTransport)) {
			const auto bound = m_RemoteTransports.find(frame.senderPeerId);
			std::cout << "[lockstep] dropped a frame claiming peer " << static_cast<int>(frame.senderPeerId) << " from the wrong transport: target=" << frame.targetFrame
			          << " from=" << fromTransport << " bound=" << (bound == m_RemoteTransports.end() ? std::string("none") : std::to_string(bound->second)) << std::endl;
			return;
		}
		if (std::any_of(frame.commands.begin(), frame.commands.end(), [](const auto& command) { return std::holds_alternative<NetGameSeatHold>(command.payload) || std::holds_alternative<NetGameInputDelay>(command.payload) || std::holds_alternative<NetGameSeatReclaim>(command.payload); }) &&
		    !(recovered && m_Config.resumeFromSnapshot && m_InstalledResyncTargets.contains({frame.targetFrame, frame.senderPeerId}))) {
			if (m_Config.localPeerId == GetHostPeerId()) ApplyPeerLeave(frame.senderPeerId, FirstFrameWithout(frame.senderPeerId), "unacknowledged seat hold", nowMs, false, true);
			else Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "seat holds must use the acknowledged decision lane");
			return;
		}
		if (frame.roundId != 0 && m_RoundId != 0 && frame.roundId != m_RoundId) {
			++m_Stats.staleRoundPackets;
			++peerStats.staleRoundPackets;
			return;
		}
		const auto authority = m_RemoteTransports.find(GetHostPeerId());
		const bool awaitingWorldRoster = IsPersistentWorldRound() && m_Config.joinsRunningRound && !m_RelayHost &&
		    m_State == NetLockstepState::WaitingForStart && authority != m_RemoteTransports.end() && authority->second == fromTransport &&
		    frame.senderPeerId > 0 && frame.senderPeerId <= m_Config.peerCount && frame.senderPeerId != m_Config.localPeerId;
		if (!IsKnownRemotePeer(frame.senderPeerId) && !awaitingWorldRoster) {
			Fail(NetLockstepStopReason::ProtocolError, m_Stats.nextFrame, "lockstep frame sender mismatch: peer " + std::to_string(frame.senderPeerId) + " is not a remote");
			return;
		}
		if (frame.targetFrame > peerStats.highestTargetFrame) { peerStats.highestTargetFrame = frame.targetFrame; peerStats.lastProgressMs = nowMs; }
		// How late this sender's tick lands after we first missed it is the window a resend request waits out.
		if (frame.targetFrame == m_MissingSinceFrame && m_MissingSinceMs != 0 && nowMs >= m_MissingSinceMs) {
			const auto held = m_RemoteFrames.find(frame.targetFrame);
			if (held == m_RemoteFrames.end() || !held->second.contains(frame.senderPeerId)) {
				auto& lateness = m_ArrivalLateness[frame.senderPeerId];
				lateness.push_back(static_cast<uint32_t>(std::min<uint64_t>(nowMs - m_MissingSinceMs, UINT32_MAX)));
				while (lateness.size() > c_ArrivalLatenessSamples) lateness.pop_front();
			}
		}
		// Forward every frame this peer's slot table has taken in, before any rule of ours drops it: what
		// the peers behind the relay decode has to be the same sequence, or their tables fall behind and a
		// later slot reference means nothing to them. They apply the same rules to it that we do.
		if (relay) {
			if (recovered && m_RelayHost) {
				std::vector<uint8_t> bytes;
				std::string error;
				if (!NetLockstepCodec::EncodeRecoveryInput(frame, bytes) || !RetainRecoveryInput(frame, std::move(bytes), false, &error)) {
					Fail(NetLockstepStopReason::ProtocolError, frame.targetFrame, "cannot retain recovered input relay: " + error);
					return;
				}
			} else if (!recovered) {
				if (m_Config.frameLane == NetTransportLane::ControlReliable) RelayToOtherRemotes({frame}, frame.senderPeerId);
				else RelayArrivedTicks(frame);
			}
		}
		// After a round restart a peer's first frames can outrun its start; hold them until it lands.
		if (m_RemoteStartsReceived.find(frame.senderPeerId) == m_RemoteStartsReceived.end()) {
			if (m_PreStartFrames[frame.senderPeerId].empty())
				std::cout << "[lockstep] holds frames of peer " << static_cast<int>(frame.senderPeerId) << " until its start lands: first target=" << frame.targetFrame
				          << " next=" << m_Stats.nextFrame << std::endl;
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
		for (const NetLockstepFrame& older : frame.priorWindow) {
			// A copy read past this peer's table is a tick it already holds; committing its frames without
			// the observations that rode the original would leave the peers with different readings.
			if (older.observationsReadPast) {
				++m_Stats.windowCopiesSkipped;
				++peerStats.windowCopiesSkipped;
				continue;
			}
			NetLockstepFrame copy = older;
			copy.priorWindow.clear();
			copy.roundId = frame.roundId;
			copy.senderPeerId = frame.senderPeerId;
			AcceptRemoteTick(copy, nowMs, true);
			if (m_State == NetLockstepState::Failed) {
				return;
			}
		}
		// A tick read past arrived after this peer already held it; its readings were not read, so it is not taken again.
		if (frame.observationsReadPast) {
			++m_Stats.windowCopiesSkipped;
			++peerStats.windowCopiesSkipped;
			return;
		}
		NetLockstepFrame newest = frame;
		newest.priorWindow.clear();
		AcceptRemoteTick(newest, nowMs, false);
	}

	void NetLockstepCoordinator::RelayArrivedTicks(const NetLockstepFrame& frame) {
		if (!m_RelayHost) return;
		std::vector<NetLockstepFrame> ticks(frame.priorWindow.begin(), frame.priorWindow.end());
		NetLockstepFrame newest = frame;
		newest.priorWindow.clear();
		ticks.push_back(std::move(newest));
		auto& relayed = m_RelayedTicks[frame.senderPeerId];
		const size_t window = ConfiguredWindowTicks();
		const uint64_t keepFrom = m_Stats.nextFrame > 2 * NetLockstepCodec::c_MaxWindowTicks ? m_Stats.nextFrame - 2 * NetLockstepCodec::c_MaxWindowTicks : 0;
		for (size_t index = 0; index < ticks.size(); ++index) {
			const NetLockstepFrame& tick = ticks[index];
			// A tick already sent on, one older than this host still tracks, or one it drops past the seat's boundary is none
			// of the others' business.
			if (tick.observationsReadPast || tick.targetFrame < keepFrom || relayed.contains(tick.targetFrame) ||
			    IsPeerGoneAtFrame(frame.senderPeerId, tick.targetFrame)) continue;
			NetLockstepFrame packet = tick;
			packet.senderPeerId = frame.senderPeerId;
			packet.roundId = frame.roundId;
			for (size_t older = index + 1 > window ? index + 1 - window : 0; older < index; ++older) {
				NetLockstepFrame copy = ticks[older];
				copy.priorWindow.clear();
				packet.priorWindow.push_back(std::move(copy));
			}
			RelayToOtherRemotes({packet}, frame.senderPeerId);
			relayed.insert(tick.targetFrame);
			NetLockstepFrame whole = std::move(packet);
			whole.priorWindow.clear();
			m_RelayedTickFrames[frame.senderPeerId][tick.targetFrame] = std::move(whole);
		}
		relayed.erase(relayed.begin(), relayed.lower_bound(keepFrom));
		auto& keptTicks = m_RelayedTickFrames[frame.senderPeerId];
		keptTicks.erase(keptTicks.begin(), keptTicks.lower_bound(keepFrom));
	}

	NetTransportLane NetLockstepCoordinator::LaneTo(uint8_t peerId, const NetLockstepPacket& packet, NetTransportLane lane) const {
		const NetLockstepFrame* frame = std::get_if<NetLockstepFrame>(&packet.payload);
		if (!frame) return NetTransportLane::ControlReliable;
		// A member admitted mid-round reads the replay on the reliable lane, and the frames after it there too until its
		// tables hold what the replay spelled out.
		const auto through = m_ReliableFramesThrough.find(peerId);
		return through != m_ReliableFramesThrough.end() && frame->targetFrame <= through->second ? NetTransportLane::ControlReliable : lane;
	}

	void NetLockstepCoordinator::HandleStop(const NetLockstepStop& stop, uint64_t nowMs, NetPeerId fromTransport) {
		if (NeedsMigrationSnapshot() && stop.senderPeerId == m_MigrationResult.snapshotProviderPeerId && SenderOwnsTransport(stop.senderPeerId, fromTransport) &&
		    stop.reason == NetLockstepStopReason::ResyncRequested && stop.frame == m_MigrationResult.boundary + 2) {
			Fail(stop.reason, stop.frame, "successor receives the boundary snapshot");
			return;
		}
		// A host that announces its leave hands the match to the survivors; with no other survivor there is no one to hand it to,
		// and the leave ends this seat's match as the host's own decision.
		const bool otherSurvivor = std::any_of(m_RemotePeerIds.begin(), m_RemotePeerIds.end(), [&](uint8_t peer) {
			return peer != GetHostPeerId() && !IsPeerGoneAtFrame(peer, m_Stats.nextFrame) && !m_AiHeldSeats.contains(peer);
		});
		if (stop.senderPeerId == GetHostPeerId() && IsRoundAuthority(stop.senderPeerId, fromTransport) && stop.reason == NetLockstepStopReason::PeerLeft && otherSurvivor &&
		    BeginHostMigration(nowMs))
			return;
		if (IsHoldResolutionReason(stop.reason)) {
			if (m_RelayHost) {
				return;
			}
			if (!IsRoundAuthority(GetHostPeerId(), fromTransport) || !IsKnownRemotePeer(stop.senderPeerId)) {
				return;
			}
			ApplyHoldResolution(stop.senderPeerId, HoldResolutionOf(stop.reason), nowMs, false);
			return;
		}
		// The host's waiver, not a leave: every survivor stops requiring the fenced incarnation's frames
		// at the same frame, and nothing else about the seat moves. Only the round authority issues it.
		if (IsFrameWaiver(stop)) {
			if (m_RelayHost || !IsRoundAuthority(GetHostPeerId(), fromTransport) || !IsKnownRemotePeer(stop.senderPeerId)) {
				return;
			}
			if (WaiveRemoteFrames(stop.senderPeerId, stop.frame, nowMs, false)) {
				AdvanceReadyFrames(nowMs);
			}
			return;
		}
		if (!SenderOwnsTransport(stop.senderPeerId, fromTransport)) {
			std::cout << "[lockstep] dropped a stop claiming peer " << static_cast<int>(stop.senderPeerId) << " from the wrong transport" << std::endl;
			return;
		}
		if (!IsKnownRemotePeer(stop.senderPeerId)) return;
		// A seat the round has already dropped cannot end it: a link that fails one way leaves the evicted
		// peer able to send, and its own grace runs out on a round it is no longer in.
		const auto leftIt = m_PeerLeaveFrames.find(stop.senderPeerId);
		// A seat the bound held before its announced leave arrived is released: a clean leaver's return is a new join.
		if (leftIt != m_PeerLeaveFrames.end() && stop.reason == NetLockstepStopReason::PeerLeft && m_RelayHost && UsesBoundedWait() && HasHeldAISeat(stop.senderPeerId)) {
			std::cout << "[net-lockstep] " << DescribePeer(stop.senderPeerId) << " announced its leave while held at frame " << leftIt->second
			          << ": the seat is released" << std::endl;
			ReleaseHeldSeat(stop.senderPeerId, nowMs, true);
			return;
		}
		if (leftIt != m_PeerLeaveFrames.end()) {
			std::cout << "[lockstep] ignored a " << NetLockstepCodec::StopReasonName(stop.reason) << " from peer "
			          << static_cast<int>(stop.senderPeerId) << ", which left at frame " << leftIt->second << std::endl;
			++m_Stats.stopsFromLeftPeers;
			return;
		}
		// A member reaching its own planned end leaves; only the host's end closes the round. A bounded-wait
		// host hands that seat to the AI like any leave and plays on.
		if (stop.reason == NetLockstepStopReason::Complete && stop.senderPeerId != GetHostPeerId() &&
		    (IsPersistentWorldRound() || (UsesBoundedWait() && m_RelayHost))) {
			NetLockstepStop leave = stop;
			leave.reason = NetLockstepStopReason::PeerLeft;
			HandleStop(leave, nowMs, fromTransport);
			return;
		}
		if (m_DeferStops && stop.reason == NetLockstepStopReason::Complete &&
		    (!m_LastCompletedSimulationTick || *m_LastCompletedSimulationTick + 1 < stop.frame)) {
			if (!m_PendingCompleteStop || stop.frame < m_PendingCompleteStop->frame) m_PendingCompleteStop = stop;
			return;
		}
		if (m_DeferStops && stop.senderPeerId != GetHostPeerId() &&
		    (stop.reason == NetLockstepStopReason::Desync || stop.reason == NetLockstepStopReason::ResyncRequested)) {
			ScheduleRecoveryStop(stop.reason, stop.frame, stop.message);
			return;
		}
		if (stop.reason == NetLockstepStopReason::PeerRemoved && (m_RelayHost || LockstepPeerOfTransport(fromTransport) != GetHostPeerId())) return;
		if (stop.reason == NetLockstepStopReason::PeerLeft || stop.reason == NetLockstepStopReason::PeerDropped || stop.reason == NetLockstepStopReason::PeerRemoved) {
			if (IsKnownRemotePeer(stop.senderPeerId)) {
				if (m_Config.resumeFromSnapshot && m_RemoteTransports.contains(stop.senderPeerId) &&
				    stop.frame <= m_Config.startFrame) {
					++m_Stats.ignoredAdmissionFaults;
					return;
				}
				ApplyPeerLeave(stop.senderPeerId, stop.frame, stop.message, nowMs, stop.reason != NetLockstepStopReason::PeerDropped, false, false, stop.reason == NetLockstepStopReason::PeerRemoved);
			}
			return;
		}
		if (m_RelayHost && stop.senderPeerId != GetHostPeerId() &&
		    (stop.reason == NetLockstepStopReason::ProtocolError || stop.reason == NetLockstepStopReason::InternalError ||
		     stop.reason == NetLockstepStopReason::MissingFrameTimeout || stop.reason == NetLockstepStopReason::PeerDisconnected)) {
			++m_Stats.stopsAdjudicatedAsLeaves;
			ApplyPeerLeave(stop.senderPeerId, FirstFrameWithout(stop.senderPeerId),
			               std::string(NetLockstepCodec::StopReasonName(stop.reason)) + ": " + stop.message, nowMs, true);
			return;
		}
		m_Stats.timeoutReason = std::string(NetLockstepCodec::StopReasonName(stop.reason)) + ":" + stop.message;
		m_State = stop.reason == NetLockstepStopReason::Complete ? NetLockstepState::Stopped : NetLockstepState::Failed;
	}

	bool NetLockstepCoordinator::PublishSeatSnapshot(std::vector<NetSeatPresenceEntry> seats, uint64_t observedAtMs) {
		NET_PLANE_CHECK();
		if (!m_RelayHost || m_Config.localPeerId != GetHostPeerId() || !IsRunning())
			return false;
		if (m_SeatSnapshot && observedAtMs < m_SeatSnapshot->observedAtMs) return false;
		const bool unchanged = m_SeatSnapshot && m_SeatSnapshot->seats == seats;
		const bool countingHold = std::any_of(seats.begin(), seats.end(), [](const auto& seat) { return seat.holdActive; });
		if (unchanged && (!countingHold || observedAtMs < m_SeatSnapshot->observedAtMs || observedAtMs - m_SeatSnapshot->observedAtMs < 1000)) return true;
		if (m_SeatSnapshot && m_SeatSnapshot->revision == UINT64_MAX) return false;
		NetLockstepSeatSnapshot snapshot;
		snapshot.senderPeerId = m_Config.localPeerId;
		snapshot.sessionId = m_Config.sessionId;
		snapshot.epoch = m_Config.seatPresenceEpoch;
		snapshot.roundId = m_RoundId;
		snapshot.revision = m_SeatSnapshot ? m_SeatSnapshot->revision + 1 : 1;
		snapshot.observedAtMs = observedAtMs;
		snapshot.seats = std::move(seats);
		if (!AdvancesSeatSnapshot(m_SeatSnapshot, snapshot, m_Config.peerCount, m_Config.matchConfig)) return false;
		std::vector<uint8_t> checked;
		if (!NetLockstepCodec::Encode({snapshot}, checked)) return false;
		m_SeatSnapshot = std::move(snapshot);
		m_SeatSnapshotUnread = true;
		for (const auto& [peerId, transportId]: m_RemoteTransports) m_PendingSeatSnapshotPeers.insert(peerId);
		FlushSeatSnapshot();
		return true;
	}

	void NetLockstepCoordinator::FlushSeatSnapshot() {
		if (!IsRunning() || !m_RelayHost || !m_Transport || !m_SeatSnapshot || m_PendingSeatSnapshotPeers.empty()) return;
		std::vector<uint8_t> bytes;
		if (!NetLockstepCodec::Encode({*m_SeatSnapshot}, bytes)) return;
		for (auto pending = m_PendingSeatSnapshotPeers.begin(); pending != m_PendingSeatSnapshotPeers.end();) {
			const auto transport = m_RemoteTransports.find(*pending);
			if (transport == m_RemoteTransports.end()) {
				pending = m_PendingSeatSnapshotPeers.erase(pending);
				continue;
			}
			// Coalesce UI updates while this peer's simulation stream is backlogged.
			const auto backlog = m_RelayBacklog.find(*pending);
			std::string error;
			if ((backlog == m_RelayBacklog.end() || backlog->second.empty()) &&
			    m_Transport->Send(transport->second, NetTransportLane::ControlReliable, bytes, &error)) {
				pending = m_PendingSeatSnapshotPeers.erase(pending);
			} else {
				++pending;
			}
		}
	}

	void NetLockstepCoordinator::HandleSeatSnapshot(const NetLockstepSeatSnapshot& snapshot, NetPeerId fromTransport) {
		if (m_RelayHost || snapshot.senderPeerId != GetHostPeerId() ||
		    !IsRoundAuthority(snapshot.senderPeerId, fromTransport) ||
		    snapshot.sessionId != m_Config.sessionId || snapshot.epoch != m_Config.seatPresenceEpoch ||
		    snapshot.roundId == 0 || snapshot.roundId != m_RoundId ||
		    !AdvancesSeatSnapshot(m_SeatSnapshot, snapshot, m_Config.peerCount, m_Config.matchConfig)) return;
		m_SeatSnapshot = snapshot;
		m_SeatSnapshotUnread = true;
	}

	std::optional<NetLockstepSeatSnapshot> NetLockstepCoordinator::TakeSeatSnapshot() {
		NET_PLANE_CHECK();
		if (!m_SeatSnapshotUnread) return std::nullopt;
		m_SeatSnapshotUnread = false;
		return m_SeatSnapshot;
	}

	void NetLockstepCoordinator::SetSeatStateSource(NetLockstepSeatState (*source)(void*, uint8_t, NetPeerId), void* context) {
		NET_PLANE_CHECK();
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
		for (uint8_t peerId: m_DroppedSeats) {
			m_LeftSeatsHeld.insert(peerId);
		}
	}

	bool NetLockstepCoordinator::IsSeatHeldForReclaimAtFrame(uint64_t) const {
		NET_PLANE_CHECK();
		return !UsesBoundedWait() && m_State == NetLockstepState::Running && !m_DroppedSeats.empty();
	}

	NetLockstepHoldResolution NetLockstepCoordinator::HeldSeatResolution(uint8_t peerId) const {
		NET_PLANE_CHECK();
		const auto it = m_DroppedSeatResolutions.find(peerId);
		return it != m_DroppedSeatResolutions.end() ? it->second : NetLockstepHoldResolution::None;
	}

	bool NetLockstepCoordinator::IsHoldResolutionReason(NetLockstepStopReason reason) {
		return reason == NetLockstepStopReason::Reclaimed || reason == NetLockstepStopReason::Substituted ||
		       reason == NetLockstepStopReason::Expired;
	}

	NetLockstepStopReason NetLockstepCoordinator::StopReasonOf(NetLockstepHoldResolution resolution) {
		switch (resolution) {
			case NetLockstepHoldResolution::Reclaimed: return NetLockstepStopReason::Reclaimed;
			case NetLockstepHoldResolution::Substituted: return NetLockstepStopReason::Substituted;
			case NetLockstepHoldResolution::Expired: return NetLockstepStopReason::Expired;
			case NetLockstepHoldResolution::None: break;
		}
		return NetLockstepStopReason::PeerDropped;
	}

	NetLockstepHoldResolution NetLockstepCoordinator::HoldResolutionOf(NetLockstepStopReason reason) {
		switch (reason) {
			case NetLockstepStopReason::Reclaimed: return NetLockstepHoldResolution::Reclaimed;
			case NetLockstepStopReason::Substituted: return NetLockstepHoldResolution::Substituted;
			case NetLockstepStopReason::Expired: return NetLockstepHoldResolution::Expired;
			default: break;
		}
		return NetLockstepHoldResolution::None;
	}

	uint64_t NetLockstepCoordinator::HoldPauseRemainingMs(uint64_t nowMs) const {
		NET_PLANE_CHECK();
		if (m_DroppedSeats.empty()) {
			return 0;
		}
		uint64_t remaining = 0;
		for (uint8_t peerId: m_DroppedSeats) {
			const auto droppedAt = m_DroppedAtMs.find(peerId);
			const uint64_t started = droppedAt != m_DroppedAtMs.end() ? droppedAt->second : nowMs;
			const uint64_t elapsed = nowMs >= started ? nowMs - started : 0;
			const uint64_t left = elapsed < c_HoldPauseMs ? c_HoldPauseMs - elapsed : 0;
			if (remaining == 0 || left > remaining) {
				remaining = left;
			}
		}
		return remaining;
	}

	std::string NetLockstepCoordinator::DescribeHeldPause(uint32_t& secondsLeft, uint64_t nowMs) const {
		NET_PLANE_CHECK();
		secondsLeft = 0;
		if (m_DroppedSeats.empty()) {
			return {};
		}
		secondsLeft = static_cast<uint32_t>((HoldPauseRemainingMs(nowMs) + 999) / 1000);
		return DescribePeer(*m_DroppedSeats.begin());
	}

	void NetLockstepCoordinator::MaybeSendHoldHeartbeats(uint64_t nowMs) {
		if (m_LastHoldHeartbeatMs != 0 && nowMs >= m_LastHoldHeartbeatMs && nowMs - m_LastHoldHeartbeatMs < c_HoldHeartbeatMs) {
			return;
		}
		m_LastHoldHeartbeatMs = nowMs;
		if (!m_Transport) {
			return;
		}
		NetLockstepAck ack;
		ack.senderPeerId = m_Config.localPeerId;
		ack.highestContiguousFrame = m_Stats.nextFrame;
		ack.receivedMask = 0;
		if (ConfiguredWindowTicks() > 1 && (!m_RelayHost || FrameWindowAllRemotesAdvertised())) {
			ack.receivedMask = NetLockstepCodec::c_FrameWindowCapabilityMask;
		}
		std::string ignored;
		(void)SendPacket({ack}, NetTransportLane::ControlReliable, &ignored);
	}

	void NetLockstepCoordinator::ResolveHeldSeat(uint8_t peerId, NetLockstepHoldResolution resolution, uint64_t nowMs) {
		NET_PLANE_CHECK();
		if (!m_RelayHost) {
			return;
		}
		ApplyHoldResolution(peerId, resolution, nowMs, true);
	}

	void NetLockstepCoordinator::EvictRemovedPeer(uint8_t peerId, const std::string& message, uint64_t nowMs) {
		NET_PLANE_CHECK();
		if (!m_RelayHost || peerId == 0 || peerId == m_Config.localPeerId) {
			return;
		}
		// A round that has ended or is relaunching is not rewritten: the next round forms without the removed seat.
		if (!IsRunning()) {
			return;
		}
		if (m_AiHeldSeats.contains(peerId)) {
			if (m_ReleasedAiSeats.contains(peerId)) return;
			// A return some peer may already have committed stands; the removal meets the seat once it is back.
			if (const auto reclaim = m_ReclaimTransactions.find(peerId); reclaim != m_ReclaimTransactions.end() && reclaim->second.activationFrame <= FutureTimingFrame()) {
				m_EvictAfterReclaim[peerId] = message;
				return;
			}
			ReleaseHeldSeat(peerId, nowMs, true);
			return;
		}
		if (m_DroppedSeats.find(peerId) != m_DroppedSeats.end()) {
			ApplyHoldResolution(peerId, NetLockstepHoldResolution::Expired, nowMs, true);
			return;
		}
		if (m_PeerLeaveFrames.find(peerId) != m_PeerLeaveFrames.end()) {
			return;
		}
		ApplyPeerLeave(peerId, m_Stats.nextFrame, message, nowMs, true, false, false, true);
	}

	void NetLockstepCoordinator::ApplyHoldResolution(uint8_t peerId, NetLockstepHoldResolution resolution, uint64_t nowMs, bool relay) {
		if (UsesBoundedWait() && m_AiHeldSeats.contains(peerId)) {
			// A seat whose returner is already agreed has nothing left to release: the return stands.
			if (resolution == NetLockstepHoldResolution::Expired && !m_ReclaimTransactions.contains(peerId)) ReleaseHeldSeat(peerId, nowMs, relay);
			return;
		}
		if (resolution == NetLockstepHoldResolution::None || m_DroppedSeats.find(peerId) == m_DroppedSeats.end()) {
			return;
		}
		if (HeldSeatResolution(peerId) != NetLockstepHoldResolution::None && !m_AiHeldSeats.contains(peerId)) {
			return;
		}
		m_DroppedSeatResolutions[peerId] = resolution;
		if (resolution == NetLockstepHoldResolution::Reclaimed) ++m_Stats.peers[peerId].rejoins;
		m_DroppedSeats.erase(peerId);
		RefreshLeftSeatHolds();
		const auto leaveIt = m_PeerLeaveFrames.find(peerId);
		const uint64_t heldFrame = leaveIt != m_PeerLeaveFrames.end() ? leaveIt->second : m_Stats.nextFrame;
		if (relay && m_RelayHost && m_Transport) {
			NetLockstepStop notice;
			notice.senderPeerId = peerId;
			notice.reason = StopReasonOf(resolution);
			notice.frame = heldFrame;
			notice.message = std::string(NetLockstepCodec::StopReasonName(notice.reason));
			std::string ignored;
			(void)SendPacket({notice}, NetTransportLane::ControlReliable, &ignored);
		}
		if (resolution == NetLockstepHoldResolution::Expired) {
			if (!IsPersistentWorldRound() && m_AiHeldSeats.empty() && LeftPeersNotRefilling() >= m_RemotePeerIds.size() && !AnyLeftSeatHeld() && !ReclaimResyncPending()) {
				m_Stats.timeoutReason = std::string(NetLockstepCodec::StopReasonName(NetLockstepStopReason::PeerLeft)) + ":" + m_LastLeaveMessage;
				m_State = NetLockstepState::Stopped;
			}
			return;
		}
		if (IsPersistentWorldRound()) {
			return;
		}
		std::cout << "[net-match] rejoin: " << DescribePeer(peerId) << " reconnected - resyncing the match" << std::endl;
		// Deferred: the tick in flight commits first, or the heal snapshots half a tick under the label
		// of the one before it.
		RequestResync(resolution == NetLockstepHoldResolution::Reclaimed ? "seat reclaimed" : "seat substituted");
		(void)nowMs;
	}

	void NetLockstepCoordinator::ReleaseHeldSeat(uint8_t peerId, uint64_t nowMs, bool relay) {
		if (!m_AiHeldSeats.contains(peerId) || !m_ReleasedAiSeats.insert(peerId).second) return;
		const bool host = relay && m_RelayHost && m_Config.localPeerId == GetHostPeerId();
		// An agreed return is withdrawn by holding the seat again at its current incarnation. The caller made sure its
		// activation is still past every peer's horizon, so no peer can have committed it.
		if (const auto reclaim = m_ReclaimTransactions.find(peerId); host && reclaim != m_ReclaimTransactions.end() && m_NextTimingRevision != UINT64_MAX) {
			NetLockstepTiming timing;
			timing.senderPeerId = GetHostPeerId(); timing.peerId = peerId;
			timing.action = NetTimingAction::Hold; timing.phase = NetTimingPhase::HoldAtFrame;
			timing.sessionId = m_Config.sessionId; timing.roundId = m_RoundId; timing.authorityGeneration = m_Config.migrationGeneration;
			timing.revision = m_NextTimingRevision++;
			timing.applyFrame = timing.cutoffFrame = FutureTimingFrame();
			timing.heldPeers = static_cast<uint8_t>(1U << (peerId - 1));
			const auto incarnation = m_Config.peerIncarnations.find(peerId);
			timing.seatIncarnations[peerId - 1] = incarnation == m_Config.peerIncarnations.end() ? 1 : incarnation->second;
			timing.nextFrame = m_Stats.nextFrame;
			timing.requiredPeers = static_cast<uint8_t>(1U << (m_Config.localPeerId - 1));
			for (uint8_t peer: m_RemotePeerIds)
				if (peer != peerId && !IsPeerGoneAtFrame(peer, timing.applyFrame)) timing.requiredPeers |= static_cast<uint8_t>(1U << (peer - 1));
			m_TimingDecisions[timing.revision] = {timing, 0, true, nowMs};
			QueueTiming(timing);
			FlushTimingOutgoing();
			ApplyTiming(timing);
			std::cout << "[net-match] reclaim withdrawn peer=" << static_cast<int>(peerId) << " frame=" << timing.applyFrame << std::endl;
		}
		m_DroppedSeats.erase(peerId);
		m_DroppedSeatResolutions[peerId] = NetLockstepHoldResolution::Expired;
		m_LateStartReclaims.erase(peerId);
		m_EvictAfterReclaim.erase(peerId);
		RefreshLeftSeatHolds();
		std::cout << "[net-match] seat released peer=" << static_cast<int>(peerId) << " - the AI keeps its units" << std::endl;
		if (host && m_Transport) {
			NetLockstepStop notice;
			notice.senderPeerId = peerId;
			notice.reason = StopReasonOf(NetLockstepHoldResolution::Expired);
			const auto leaveIt = m_PeerLeaveFrames.find(peerId);
			notice.frame = leaveIt != m_PeerLeaveFrames.end() ? leaveIt->second : m_Stats.nextFrame;
			notice.message = std::string(NetLockstepCodec::StopReasonName(notice.reason));
			std::string ignored;
			(void)SendPacket({notice}, NetTransportLane::ControlReliable, &ignored);
		}
	}

	bool NetLockstepCoordinator::AnyLeftSeatHeld() const {
		return !m_LeftSeatsHeld.empty();
	}

	bool NetLockstepCoordinator::IsSeatHeldForReclaim(uint8_t peerId) const {
		NET_PLANE_CHECK();
		return m_LeftSeatsHeld.find(peerId) != m_LeftSeatsHeld.end();
	}

	bool NetLockstepCoordinator::IgnoreStaleRefillLeave(uint8_t peerId, uint64_t) const {
		return m_Config.resumeFromSnapshot && m_RemoteTransports.contains(peerId) &&
		       m_PeerLeaveFrames.find(peerId) == m_PeerLeaveFrames.end() &&
		       m_PeerLastHeardMs.find(peerId) == m_PeerLastHeardMs.end();
	}

	bool NetLockstepCoordinator::SeatIsRefilling(uint8_t peerId) const {
		const NetLockstepHoldResolution resolution = HeldSeatResolution(peerId);
		if (m_AiHeldSeats.contains(peerId)) return resolution == NetLockstepHoldResolution::Reclaimed;
		return resolution == NetLockstepHoldResolution::Reclaimed || resolution == NetLockstepHoldResolution::Substituted;
	}

	bool NetLockstepCoordinator::AnySeatRefilling() const {
		for (const auto& [peerId, resolution]: m_DroppedSeatResolutions) {
			(void)peerId;
			if (resolution == NetLockstepHoldResolution::Reclaimed || resolution == NetLockstepHoldResolution::Substituted) {
				return true;
			}
		}
		return false;
	}

	size_t NetLockstepCoordinator::LeftPeersNotRefilling() const {
		size_t left = 0;
		for (const auto& [peerId, frame]: m_PeerLeaveFrames) {
			(void)frame;
			if (!SeatIsRefilling(peerId) && std::find(m_RemotePeerIds.begin(), m_RemotePeerIds.end(), peerId) != m_RemotePeerIds.end()) {
				++left;
			}
		}
		return left;
	}

	bool NetLockstepCoordinator::IsHoldingSeatForReclaim() const {
		NET_PLANE_CHECK();
		return m_State == NetLockstepState::Running && !m_RemotePeerIds.empty() &&
		       std::all_of(m_RemotePeerIds.begin(), m_RemotePeerIds.end(), [&](uint8_t peer) { return m_PeerLeaveFrames.contains(peer); }) && (AnyLeftSeatHeld() || AnySeatRefilling());
	}

	// The round is already ending through the pending resync, one boundary from now; ending it as a
	// last-player leave first would throw the reclaim away.
	bool NetLockstepCoordinator::ReclaimResyncPending() const {
		return m_PendingRecoveryStop && m_PendingRecoveryStop->reason == NetLockstepStopReason::ResyncRequested;
	}

	void NetLockstepCoordinator::EndRoundIfNobodyIsComingBack() {
		// A bounded-wait round never ends because its last remote human went: the AI holds the seats and the host plays on.
		if (m_State != NetLockstepState::Running || IsPersistentWorldRound() || UsesBoundedWait() || !m_AiHeldSeats.empty() || m_RemotePeerIds.empty() ||
		    LeftPeersNotRefilling() < m_RemotePeerIds.size() || AnyLeftSeatHeld() || ReclaimResyncPending()) {
			return;
		}
		// Nobody left to play with.
		m_Stats.timeoutReason = std::string(NetLockstepCodec::StopReasonName(NetLockstepStopReason::PeerLeft)) + ":" + m_LastLeaveMessage;
		m_State = NetLockstepState::Stopped;
	}

	// A leave is deterministic by construction: no survivor can advance to the leaver's first missing
	// frame without processing this, so every peer drops the requirement at the same tick.
	void NetLockstepCoordinator::ApplyPeerLeave(uint8_t peerId, uint64_t firstFrameWithout, const std::string& message, uint64_t nowMs, bool announced, bool closeTransport, bool agreedBoundary, bool removed) {
		// Past the round's last tick no seat is held: a leave there is only a leave.
		if (UsesBoundedWait() && !agreedBoundary && !removed && m_Config.localPeerId == GetHostPeerId() && !m_GoodbyeDrain && firstFrameWithout <= m_FinalFrame) {
			std::cout << "[net-lockstep] a leave becomes a hold for peer " << static_cast<int>(peerId)
			          << " at frame " << firstFrameWithout << ": " << message << std::endl;
			// A clean leaver's seat is released once the AI has it: a return is a new join, never a reclaim.
			if (announced) m_ReleaseWhenHeld.insert(peerId);
			std::string holdError;
			if (!ProposePeerHold(peerId, nowMs, &holdError) && holdError != "the capture park deferred this hold") m_ReleaseWhenHeld.erase(peerId);
			return;
		}
		if (!m_PeerLeaveFrames.emplace(peerId, firstFrameWithout).second) {
			return;
		}
		if (!announced) {
			m_DroppedSeats.insert(peerId);
			m_DroppedAtMs[peerId] = nowMs;
		}
		// The leaver produced inputs for the frames up to this one: its own inputs drive its units there.
		if (firstFrameWithout > m_Stats.nextFrame) m_LeavesHeardAhead.insert(peerId);
		RefreshLeftSeatHolds();
		std::cout << "[net-match] " << DescribePeer(peerId) << " left the match at frame " << firstFrameWithout << " (" << message << ")" << std::endl;
		NetLockstepStop notice;
		notice.senderPeerId = peerId;
		notice.reason = removed ? NetLockstepStopReason::PeerRemoved : announced ? NetLockstepStopReason::PeerLeft : NetLockstepStopReason::PeerDropped;
		notice.frame = firstFrameWithout;
		notice.message = message;
		if (!agreedBoundary) {
			RelayToOtherRemotes({notice}, peerId);
		}
		const auto transportIt = m_RemoteTransports.find(peerId);
		const NetPeerId transportId = transportIt != m_RemoteTransports.end() ? transportIt->second : c_InvalidNetPeerId;
		m_RemoteTransports.erase(peerId);
		m_RemoteFrameWindow.erase(peerId);
		// A seat the round took keeps nothing. Its packets still cost receive work, and on a link that
		// fails one way they keep the connection's own timeout alive; a returner comes back on a new
		// connection through the reconnect path, so this is not the way back. The transport flushes what
		// is still queued before it closes.
		if (closeTransport && m_RelayHost && m_Transport && transportId != c_InvalidNetPeerId) {
			++m_Stats.connectionsClosedOnEviction;
			m_Transport->Disconnect(transportId, "seat taken: " + message);
		}
		ForgetCongestion(peerId);
		const auto discardHeldInputs = [&](auto& inputs) {
			for (auto it = inputs.lower_bound(firstFrameWithout); it != inputs.end();) {
				it->second.erase(peerId);
				if (it->second.empty()) it = inputs.erase(it); else ++it;
			}
		};
		if (UsesBoundedWait() || removed) {
			discardHeldInputs(m_RemoteFrames);
			discardHeldInputs(m_RemoteCommands);
			discardHeldInputs(m_RemoteObservations);
			discardHeldInputs(m_RemoteValueObservations);
		}
		for (auto it = m_RemoteCommands.begin(); !UsesBoundedWait() && it != m_RemoteCommands.end();) {
			const auto accepted = m_RemoteFrames.find(it->first);
			if (accepted == m_RemoteFrames.end() || !accepted->second.contains(peerId)) it->second.erase(peerId);
			if (it->second.empty()) it = m_RemoteCommands.erase(it); else ++it;
		}
		m_LastLeaveMessage = message;
		if (agreedBoundary) {
			return;
		}
		// The relay host is the star's hub: with it gone no survivor can reach another, and its own team
		// would keep resolving to a peer that produces nothing for it. The round ends for every survivor.
		if (peerId == GetHostPeerId() && peerId != m_Config.localPeerId) {
			m_Stats.timeoutReason = std::string(NetLockstepCodec::StopReasonName(NetLockstepStopReason::PeerLeft)) + ":the host left the match: " + message;
			m_State = NetLockstepState::Stopped;
			return;
		}
		// A dropped seat pauses commits until the host resolves it; an announced leave still ends a last-player match at once.
		// A world outlives its players: the last member leaving frees its slot and the world ticks on.
		if (!IsPersistentWorldRound() && !UsesBoundedWait() && LeftPeersNotRefilling() >= m_RemotePeerIds.size() && (announced || !AnyLeftSeatHeld()) && !ReclaimResyncPending()) {
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
			// A peer we are not managing to send to has nothing to answer: its silence is our queue's
			// doing, not evidence that it went away.
			if (m_CongestedPeers.find(peerId) != m_CongestedPeers.end()) {
				continue;
			}
			const auto heardIt = m_PeerLastHeardMs.find(peerId);
			if (heardIt == m_PeerLastHeardMs.end()) {
				continue;
			}
			// A returning seat sent nothing while it was away: its silence runs from its admission, and its ramp judges it before that.
			const uint64_t heardMs = std::max(heardIt->second, m_Stats.peers[peerId].reclaimAdmittedMs);
			if (nowMs < heardMs || nowMs - heardMs < budget) {
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
			ApplyPeerLeave(peerId, FirstFrameWithout(peerId), "no frames for " + std::to_string(budget) + "ms", nowMs, false, true);
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
			const std::string& reason = m_Stats.peers[peerId].lastRelayError;
			ApplyPeerLeave(peerId, FirstFrameWithout(peerId), "unreachable: " + (reason.empty() ? m_Stats.lastRelayError : reason), nowMs, false, true);
		}
	}

	void NetLockstepCoordinator::AdvanceReadyFrames(uint64_t nowMs) {
		if (IsMigrating() || m_State != NetLockstepState::Running || (m_Config.resumeFromSnapshot && (m_ResumeAdmissionPending || !m_ResyncPrimed)) || m_AwaitingReplayedSeatState) {
			m_AdvanceBlock = IsMigrating() ? "migrating" : m_State != NetLockstepState::Running ? "not-running" : m_AwaitingReplayedSeatState ? "replayed-seat-state" : "resume-admission";
			return;
		}
		if (AnyDroppedSeatHeld() && !UsesBoundedWait()) {
			m_AdvanceBlock = "dropped-seat-pause";
			MaybeSendHoldHeartbeats(nowMs);
			return;
		}
		while (true) {
			const bool parkFrame = IsSynchronizedCapturePark(m_Stats.nextFrame);
			if (!parkFrame && m_CaptureParkAwaitingReports && m_SynchronizedCaptureStartFrame != UINT64_MAX &&
			    m_Stats.nextFrame > m_SynchronizedCaptureEndFrame) { m_AdvanceBlock = "capture-reports"; break; }
			if (!parkFrame) FlushDeferredParkTimings();
			// A park frame carries every peer's input like any other frame: the park only keeps the bound from
			// judging a seat while the capture runs, so no player's input is ever dropped for it.
			if (TimingDecisionPendingAt(m_Stats.nextFrame)) { m_AdvanceBlock = "timing-decision"; break; }
			// The LOCAL peer ramps in like any sender: its first queued input targets its own delay,
			// so earlier committed frames legitimately carry no local entry.
			auto localIt = m_LocalFrames.find(m_Stats.nextFrame);
			if (localIt == m_LocalFrames.end() && (m_Playback || (m_Stats.nextFrame >= EffectiveStartOf(m_Config.localPeerId) &&
			    !IsSeatReclaimGap(m_Config.localPeerId, m_Stats.nextFrame) && !IsSeatUnderAI(m_Config.localPeerId, m_Stats.nextFrame)))) {
				if (!m_Playback && JudgeOwnSeat(m_Stats.nextFrame, nowMs)) continue;
				m_AdvanceBlock = "own-input";
				break;
			}
			m_OwnMissingFrame.reset();
			// Local input commits optimistically after the restored input batch has been admitted.
			// Advance only when every REQUIRED remote's frame is in — a cleanly-left peer stops being
			// required past its announced last frame.
			auto remoteIt = m_RemoteFrames.find(m_Stats.nextFrame);
			bool allRequiredIn = true;
			size_t requiredRemotes = 0;
			for (uint8_t peerId: m_RemotePeerIds) {
				if (!IsRemoteRequiredForFrame(peerId, m_Stats.nextFrame)) {
					continue;
				}
				++requiredRemotes;
				if (remoteIt == m_RemoteFrames.end() || remoteIt->second.find(peerId) == remoteIt->second.end()) {
					allRequiredIn = false;
					break;
				}
			}
			if (!allRequiredIn) {
				if (m_MissingSinceFrame != m_Stats.nextFrame) { m_MissingSinceFrame = m_Stats.nextFrame; m_MissingSinceMs = nowMs; }
				if (nowMs >= m_MissingSinceMs && static_cast<double>(nowMs - m_MissingSinceMs) >= m_Config.simTickMs) {
					for (uint8_t peer: m_RemotePeerIds) if (IsRemoteRequiredForFrame(peer, m_Stats.nextFrame) &&
					    (remoteIt == m_RemoteFrames.end() || !remoteIt->second.contains(peer))) RequestMissingFrames(peer, m_Stats.nextFrame, nowMs, nowMs - m_MissingSinceMs);
				}
				if (UsesBoundedWait() && m_Config.localPeerId == GetHostPeerId()) {
					if (m_FirstMissingFrame != m_Stats.nextFrame) { m_FirstMissingFrame = m_Stats.nextFrame; m_FirstMissingMs = nowMs; }
					std::vector<uint8_t> missing;
					for (uint8_t peer: m_RemotePeerIds) if (IsRemoteRequiredForFrame(peer, m_Stats.nextFrame) &&
					    (remoteIt == m_RemoteFrames.end() || !remoteIt->second.contains(peer))) missing.push_back(peer);
					if (DeclareOverdueInputs(m_Stats.nextFrame, nowMs, m_FirstMissingMs, missing)) continue;
				}
				m_AdvanceBlock = "remote-input";
				break;
			}
			// With its own seat off and no other seat required, the host's own simulation paces the round, exactly as its input would.
			if (!m_Playback && requiredRemotes == 0 && localIt == m_LocalFrames.end() && m_Config.localPeerId == GetHostPeerId() &&
			    (IsSeatUnderAI(m_Config.localPeerId, m_Stats.nextFrame) || IsSeatReclaimGap(m_Config.localPeerId, m_Stats.nextFrame)) &&
			    (!m_LastCompletedSimulationTick || m_Stats.nextFrame > *m_LastCompletedSimulationTick + InputDelayAt(m_Config.localPeerId, m_Stats.nextFrame) + 1)) {
				m_AdvanceBlock = "host-paced";
				break;
			}
			// A host whose own seat the AI holds still commits every frame first: it sends the others an empty frame of its own for each
			// frame it commits, which they wait on and drop as a held seat's input, so no peer ever commits past the host's decisions.
			if (!m_Playback && m_Config.localPeerId == GetHostPeerId() && IsSeatUnderAI(m_Config.localPeerId, m_Stats.nextFrame)) {
				// Once its return is agreed, the frames before its new start are ones the others no longer wait on the host for.
				if (localIt == m_LocalFrames.end() && m_Stats.nextFrame >= EffectiveStartOf(m_Config.localPeerId)) {
					std::string markerError;
					if (!QueueInputAtTarget(m_Stats.nextFrame, {}, {}, &markerError, {})) {
						std::cout << "[net-lockstep] held host could not send its commit of frame " << m_Stats.nextFrame << ": " << markerError << std::endl;
						m_AdvanceBlock = "held-host-marker";
						break;
					}
				}
				m_LocalFrames.erase(m_Stats.nextFrame);
				m_LocalCommands.erase(m_Stats.nextFrame);
				m_LocalObservations.erase(m_Stats.nextFrame);
				m_LocalValueObservations.erase(m_Stats.nextFrame);
				localIt = m_LocalFrames.end();
			}
			if (parkFrame && !m_Playback) {
				// Measured, not assumed: the frame is counted with input only when every seat it requires is in it.
				++m_Stats.parkFramesCommitted;
				const bool localIn = localIt != m_LocalFrames.end() || m_Stats.nextFrame < EffectiveStartOf(m_Config.localPeerId) ||
				    IsSeatReclaimGap(m_Config.localPeerId, m_Stats.nextFrame) || IsSeatUnderAI(m_Config.localPeerId, m_Stats.nextFrame);
				const size_t remotesIn = remoteIt == m_RemoteFrames.end() ? 0 : remoteIt->second.size();
				if (localIn && remotesIn >= requiredRemotes) ++m_Stats.parkFramesWithInput;
				if (m_Stats.nextFrame == m_SynchronizedCaptureEndFrame)
					std::cout << "[net-lockstep] capture park committed frame=" << m_SynchronizedCaptureStartFrame << " end=" << m_SynchronizedCaptureEndFrame
					          << " frames=" << m_Stats.parkFramesCommitted << " with_input=" << m_Stats.parkFramesWithInput << std::endl;
			}
			m_FirstMissingFrame.reset();
			NetLockstepReadyFrame ready;
			ready.frame = m_Stats.nextFrame;
			if (localIt != m_LocalFrames.end()) {
				ready.hasLocalInput = true;
				if (m_Playback || !IsSeatReclaimGap(m_Config.localPeerId, ready.frame)) ready.localFrames = std::move(localIt->second);
			}
			// Merge every remote peer's frames in ascending peerId order (std::map iteration) so every
			// peer builds the byte-identical apply set. This is the one N-peer determinism-sensitive spot.
			if (remoteIt != m_RemoteFrames.end()) {
				for (auto& [peerId, frames]: remoteIt->second) {
					if (!m_Playback && !CommitsRemoteInput(peerId, ready.frame)) continue;
					ready.remoteFrameCounts.emplace(peerId, frames.size());
					++m_Stats.peers[peerId].framesContributed;
					ready.remoteFrames.insert(ready.remoteFrames.end(), std::make_move_iterator(frames.begin()), std::make_move_iterator(frames.end()));
				}
			}
			if (auto localCmdIt = m_LocalCommands.find(ready.frame); localCmdIt != m_LocalCommands.end()) {
				if (m_Playback || !IsSeatReclaimGap(m_Config.localPeerId, ready.frame)) ready.localCommands = std::move(localCmdIt->second);
				m_LocalCommands.erase(localCmdIt);
			}
			if (auto remoteCmdIt = m_RemoteCommands.find(ready.frame); remoteCmdIt != m_RemoteCommands.end()) {
				for (auto& [peerId, cmds]: remoteCmdIt->second) {
					if (!m_Playback && !CommitsRemoteInput(peerId, ready.frame)) continue;
					ready.remoteCommands.insert(ready.remoteCommands.end(), std::make_move_iterator(cmds.begin()), std::make_move_iterator(cmds.end()));
				}
				m_RemoteCommands.erase(remoteCmdIt);
			}
			if (auto localObsIt = m_LocalObservations.find(ready.frame); localObsIt != m_LocalObservations.end()) {
				if (m_Playback || !IsSeatReclaimGap(m_Config.localPeerId, ready.frame)) ready.localObservations = std::move(localObsIt->second);
				m_LocalObservations.erase(localObsIt);
			}
			if (auto remoteObsIt = m_RemoteObservations.find(ready.frame); remoteObsIt != m_RemoteObservations.end()) {
				for (auto& [peerId, observations]: remoteObsIt->second) {
					if (!m_Playback && !CommitsRemoteInput(peerId, ready.frame)) continue;
					ready.remoteObservations.insert(ready.remoteObservations.end(), std::make_move_iterator(observations.begin()), std::make_move_iterator(observations.end()));
				}
				m_RemoteObservations.erase(remoteObsIt);
			}
			if (auto localValueIt = m_LocalValueObservations.find(ready.frame); localValueIt != m_LocalValueObservations.end()) {
				if (m_Playback || !IsSeatReclaimGap(m_Config.localPeerId, ready.frame)) ready.localValueObservations = std::move(localValueIt->second);
				m_LocalValueObservations.erase(localValueIt);
			}
			if (auto remoteValueIt = m_RemoteValueObservations.find(ready.frame); remoteValueIt != m_RemoteValueObservations.end()) {
				for (auto& [peerId, observations]: remoteValueIt->second) {
					if (!m_Playback && !CommitsRemoteInput(peerId, ready.frame)) continue;
					ready.remoteValueObservations.insert(ready.remoteValueObservations.end(), std::make_move_iterator(observations.begin()), std::make_move_iterator(observations.end()));
				}
				m_RemoteValueObservations.erase(remoteValueIt);
			}
			m_Stats.remoteControllerFramesAccepted += ready.remoteFrames.size();
			if (localIt != m_LocalFrames.end()) {
				m_LocalFrames.erase(localIt);
			}
			if (remoteIt != m_RemoteFrames.end()) m_RemoteFrames.erase(remoteIt);
			if (!m_Playback) for (const auto& [peer, changes]: m_DelayChanges) {
				if (const auto delay = changes.find(ready.frame); delay != changes.end()) {
					auto& commands = m_Config.localPeerId == GetHostPeerId() ? ready.localCommands : ready.remoteCommands;
					const NetGameCommand event{GetHostPeerId(), NetGameInputDelay{peer, delay->second}};
					if (std::find(commands.begin(), commands.end(), event) == commands.end()) commands.push_back(event);
				}
			}
			for (const auto& [peer, frame]: m_AiHeldSeats) {
				if (frame != ready.frame || m_Playback) continue;
				ready.aiHeldPeerIds.push_back(peer);
				auto& commands = m_Config.localPeerId == GetHostPeerId() ? ready.localCommands : ready.remoteCommands;
				const NetGameCommand event{GetHostPeerId(), m_HoldTransactions.contains(peer) ? m_HoldTransactions.at(peer) : NetGameSeatHold{peer}};
				if (std::find(commands.begin(), commands.end(), event) == commands.end()) commands.push_back(event);
			}
			if (!m_Playback && !m_StartupHeldSeatStamps.empty() && ready.frame >= m_AgreedStartRecord.value_or(NetLockstepStart{}).agreedFirstFrame) {
				for (uint8_t peer: m_StartupHeldSeatStamps) {
					ready.aiHeldPeerIds.push_back(peer);
					auto& commands = m_Config.localPeerId == GetHostPeerId() ? ready.localCommands : ready.remoteCommands;
					const NetGameCommand event{GetHostPeerId(), m_HoldTransactions.contains(peer) ? m_HoldTransactions.at(peer) : NetGameSeatHold{peer}};
					if (std::find(commands.begin(), commands.end(), event) == commands.end()) commands.push_back(event);
				}
				m_StartupHeldSeatStamps.clear();
			}
			if (!m_Playback) for (const auto& [peer, reclaim]: m_ReclaimTransactions) if (reclaim.activationFrame == ready.frame) {
				auto& commands = m_Config.localPeerId == GetHostPeerId() ? ready.localCommands : ready.remoteCommands;
				commands.push_back(NetGameCommand{GetHostPeerId(), reclaim});
				if (reclaim.worldTransition) commands.push_back(NetGameCommand{GetHostPeerId(), *reclaim.worldTransition});
			}
			m_ReadyHistory[ready.frame] = ready;
			while (m_ReadyHistory.size() > 180) {
				m_ReadyHistory.erase(m_ReadyHistory.begin());
			}
			m_ReadyFrames.push_back(std::move(ready));
			m_HostAcceptedLocalFrames.erase(m_Stats.nextFrame);
			if (UsesBoundedWait() && m_Config.localPeerId == GetHostPeerId()) {
				m_CommittedAtMs[m_Stats.nextFrame] = nowMs;
				while (m_CommittedAtMs.size() > 512) m_CommittedAtMs.erase(m_CommittedAtMs.begin());
			}
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
		// A waiting host keeps talking every tick: the clients of a round that elects a successor read a silent host as a gone one, so a
		// host that has sent no frame of its own for a tick says it is alive instead.
		const bool saysAlive = m_Config.localPeerId == GetHostPeerId() && m_RelayHost && m_Transport && !m_RemoteTransports.empty() &&
		    !m_Config.matchConfig.successorOrder.empty() && m_Config.migrationTransportFactory;
		if (saysAlive && (m_OwnFramesSent != m_LivenessFramesSeen || nowMs < m_LivenessQuietSinceMs)) { m_LivenessFramesSeen = m_OwnFramesSent; m_LivenessQuietSinceMs = nowMs; }
		if (saysAlive && static_cast<double>(nowMs - m_LivenessQuietSinceMs) >= m_Config.simTickMs &&
		    (nowMs < m_LastLivenessMs || static_cast<double>(nowMs - m_LastLivenessMs) >= m_Config.simTickMs)) {
			m_LastLivenessMs = nowMs;
			NetLockstepAck alive;
			alive.senderPeerId = m_Config.localPeerId;
			alive.highestContiguousFrame = m_Stats.nextFrame;
			if (ConfiguredWindowTicks() > 1 && FrameWindowAllRemotesAdvertised()) alive.receivedMask = NetLockstepCodec::c_FrameWindowCapabilityMask;
			std::string ignored;
			(void)SendPacket({alive}, m_Config.frameLane, &ignored);
		}
		const bool pending = hasLocal || hasRemote || hasFutureLocal || hasFutureRemote || !m_RecoveryOutgoing.empty();
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
		if (!UsesBoundedWait() && nowMs >= m_WaitStartMs && nowMs - m_WaitStartMs > m_Stats.longestStallMs) {
			m_Stats.longestStallMs = nowMs - m_WaitStartMs;
			m_Stats.lastMissingPeers = DescribeMissingPeers();
		}
		const uint64_t lastAuthorityTraffic = std::max(m_WaitStartMs, m_AuthorityLastHeardMs);
		const uint64_t silenceBoundMs = static_cast<uint64_t>(std::max(1.0, std::floor(m_Config.slowPlayerBoundTicks * m_Config.simTickMs)));
		// One reading and one threshold: the slow-player bound plus the jitter of the host's link, as its link reports it and as the gaps
		// between its packets show it. A live host talks every tick whatever its simulation is doing, so only its link can go quiet.
		uint64_t jitterMs = 0;
		if (const auto host = m_Stats.peers.find(GetHostPeerId()); host != m_Stats.peers.end()) jitterMs = host->second.jitterMs;
		if (const auto estimate = m_DelayEstimators.find(GetHostPeerId()); estimate != m_DelayEstimators.end()) jitterMs = std::max<uint64_t>(jitterMs, estimate->second.JitterMs());
		// A host that has stalled this long for its own work once this round will again: the longest gap it left is its jitter too.
		jitterMs = std::max<uint64_t>(jitterMs, m_AuthorityLongestGapMs);
		for (const uint32_t gap: m_AuthorityGaps) jitterMs = std::max<uint64_t>(jitterMs, gap);
		const uint64_t hostSilenceMs = std::min<uint64_t>(m_Config.timeoutMs, silenceBoundMs + jitterMs + static_cast<uint64_t>(std::ceil(m_Config.simTickMs)));
		// A host still in its start work (no frame from it yet this round) or in a capture park it announced is busy, not gone:
		// only its link's close or the round's timeout ends that wait.
		const bool hostBusy = !m_PeersPlayedThisRound.contains(GetHostPeerId()) || HostBusyWithAnnouncedCapture(m_Stats.nextFrame) ||
		    (m_SynchronizedCaptureStartFrame != UINT64_MAX && m_Stats.nextFrame >= m_SynchronizedCaptureStartFrame && m_Stats.nextFrame <= m_SynchronizedCaptureEndFrame + 1);
		// Past this peer's last tick the host has nothing left to send: its quiet there is the round's end, not a death.
		if (!hostBusy && hostSilenceMs > 0 && m_Stats.nextFrame <= m_FinalFrame && nowMs >= lastAuthorityTraffic && nowMs - lastAuthorityTraffic >= hostSilenceMs &&
		    BeginHostMigration(nowMs)) return;
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
